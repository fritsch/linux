/*
 * Copyright © 2014 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Ben Widawsky <ben@bwidawsk.net>
 *    Michel Thierry <michel.thierry@intel.com>
 *    Thomas Daniel <thomas.daniel@intel.com>
 *    Oscar Mateo <oscar.mateo@intel.com>
 *
 */

/**
 * DOC: Logical Rings, Logical Ring Contexts and Execlists
 *
 * Motivation:
 * GEN8 brings an expansion of the HW contexts: "Logical Ring Contexts".
 * These expanded contexts enable a number of new abilities, especially
 * "Execlists" (also implemented in this file).
 *
 * One of the main differences with the legacy HW contexts is that logical
 * ring contexts incorporate many more things to the context's state, like
 * PDPs or ringbuffer control registers:
 *
 * The reason why PDPs are included in the context is straightforward: as
 * PPGTTs (per-process GTTs) are actually per-context, having the PDPs
 * contained there mean you don't need to do a ppgtt->switch_mm yourself,
 * instead, the GPU will do it for you on the context switch.
 *
 * But, what about the ringbuffer control registers (head, tail, etc..)?
 * shouldn't we just need a set of those per engine command streamer? This is
 * where the name "Logical Rings" starts to make sense: by virtualizing the
 * rings, the engine cs shifts to a new "ring buffer" with every context
 * switch. When you want to submit a workload to the GPU you: A) choose your
 * context, B) find its appropriate virtualized ring, C) write commands to it
 * and then, finally, D) tell the GPU to switch to that context.
 *
 * Instead of the legacy MI_SET_CONTEXT, the way you tell the GPU to switch
 * to a contexts is via a context execution list, ergo "Execlists".
 *
 * LRC implementation:
 * Regarding the creation of contexts, we have:
 *
 * - One global default context.
 * - One local default context for each opened fd.
 * - One local extra context for each context create ioctl call.
 *
 * Now that ringbuffers belong per-context (and not per-engine, like before)
 * and that contexts are uniquely tied to a given engine (and not reusable,
 * like before) we need:
 *
 * - One ringbuffer per-engine inside each context.
 * - One backing object per-engine inside each context.
 *
 * The global default context starts its life with these new objects fully
 * allocated and populated. The local default context for each opened fd is
 * more complex, because we don't know at creation time which engine is going
 * to use them. To handle this, we have implemented a deferred creation of LR
 * contexts:
 *
 * The local context starts its life as a hollow or blank holder, that only
 * gets populated for a given engine once we receive an execbuffer. If later
 * on we receive another execbuffer ioctl for the same context but a different
 * engine, we allocate/populate a new ringbuffer and context backing object and
 * so on.
 *
 * Finally, regarding local contexts created using the ioctl call: as they are
 * only allowed with the render ring, we can allocate & populate them right
 * away (no need to defer anything, at least for now).
 *
 * Execlists implementation:
 * Execlists are the new method by which, on gen8+ hardware, workloads are
 * submitted for execution (as opposed to the legacy, ringbuffer-based, method).
 * This method works as follows:
 *
 * When a request is committed, its commands (the BB start and any leading or
 * trailing commands, like the seqno breadcrumbs) are placed in the ringbuffer
 * for the appropriate context. The tail pointer in the hardware context is not
 * updated at this time, but instead, kept by the driver in the ringbuffer
 * structure. A structure representing this request is added to a request queue
 * for the appropriate engine: this structure contains a copy of the context's
 * tail after the request was written to the ring buffer and a pointer to the
 * context itself.
 *
 * If the engine's request queue was empty before the request was added, the
 * queue is processed immediately. Otherwise the queue will be processed during
 * a context switch interrupt. In any case, elements on the queue will get sent
 * (in pairs) to the GPU's ExecLists Submit Port (ELSP, for short) with a
 * globally unique 20-bits submission ID.
 *
 * When execution of a request completes, the GPU updates the context status
 * buffer with a context complete event and generates a context switch interrupt.
 * During the interrupt handling, the driver examines the events in the buffer:
 * for each context complete event, if the announced ID matches that on the head
 * of the request queue, then that request is retired and removed from the queue.
 *
 * After processing, if any requests were retired and the queue is not empty
 * then a new execution list can be submitted. The two requests at the front of
 * the queue are next to be submitted but since a context may not occur twice in
 * an execution list, if subsequent requests have the same ID as the first then
 * the two requests must be combined. This is done simply by discarding requests
 * at the head of the queue until either only one requests is left (in which case
 * we use a NULL second context) or the first two requests have unique IDs.
 *
 * By always executing the first two requests in the queue the driver ensures
 * that the GPU is kept as busy as possible. In the case where a single context
 * completes but a second context is still executing, the request for this second
 * context will be at the head of the queue when we remove the first one. This
 * request will then be resubmitted along with a new request for a different context,
 * which will cause the hardware to continue executing the second request and queue
 * the new request (the GPU detects the condition of a context getting preempted
 * with the same context and optimizes the context switch flow by not doing
 * preemption, but just sampling the new tail pointer).
 *
 */

#include <drm/drmP.h>
#include <drm/i915_drm.h>
#include "i915_drv.h"

#define GEN9_LR_CONTEXT_RENDER_SIZE (22 * PAGE_SIZE)
#define GEN8_LR_CONTEXT_RENDER_SIZE (20 * PAGE_SIZE)
#define GEN8_LR_CONTEXT_OTHER_SIZE (2 * PAGE_SIZE)

#define GEN8_LR_CONTEXT_ALIGN 4096

#define RING_EXECLIST_QFULL		(1 << 0x2)
#define RING_EXECLIST1_VALID		(1 << 0x3)
#define RING_EXECLIST0_VALID		(1 << 0x4)
#define RING_EXECLIST_ACTIVE_STATUS	(3 << 0xE)
#define RING_EXECLIST1_ACTIVE		(1 << 0x11)
#define RING_EXECLIST0_ACTIVE		(1 << 0x12)

#define GEN8_CTX_STATUS_IDLE_ACTIVE	(1 << 0)
#define GEN8_CTX_STATUS_PREEMPTED	(1 << 1)
#define GEN8_CTX_STATUS_ELEMENT_SWITCH	(1 << 2)
#define GEN8_CTX_STATUS_ACTIVE_IDLE	(1 << 3)
#define GEN8_CTX_STATUS_COMPLETE	(1 << 4)
#define GEN8_CTX_STATUS_LITE_RESTORE	(1 << 15)

#define CTX_LRI_HEADER_0		0x01
#define CTX_CONTEXT_CONTROL		0x02
#define CTX_RING_HEAD			0x04
#define CTX_RING_TAIL			0x06
#define CTX_RING_BUFFER_START		0x08
#define CTX_RING_BUFFER_CONTROL		0x0a
#define CTX_BB_HEAD_U			0x0c
#define CTX_BB_HEAD_L			0x0e
#define CTX_BB_STATE			0x10
#define CTX_SECOND_BB_HEAD_U		0x12
#define CTX_SECOND_BB_HEAD_L		0x14
#define CTX_SECOND_BB_STATE		0x16
#define CTX_BB_PER_CTX_PTR		0x18
#define CTX_RCS_INDIRECT_CTX		0x1a
#define CTX_RCS_INDIRECT_CTX_OFFSET	0x1c
#define CTX_LRI_HEADER_1		0x21
#define CTX_CTX_TIMESTAMP		0x22
#define CTX_PDP3_UDW			0x24
#define CTX_PDP3_LDW			0x26
#define CTX_PDP2_UDW			0x28
#define CTX_PDP2_LDW			0x2a
#define CTX_PDP1_UDW			0x2c
#define CTX_PDP1_LDW			0x2e
#define CTX_PDP0_UDW			0x30
#define CTX_PDP0_LDW			0x32
#define CTX_LRI_HEADER_2		0x41
#define CTX_R_PWR_CLK_STATE		0x42
#define CTX_GPGPU_CSR_BASE_ADDRESS	0x44

#define GEN8_CTX_VALID (1<<0)
#define GEN8_CTX_FORCE_PD_RESTORE (1<<1)
#define GEN8_CTX_FORCE_RESTORE (1<<2)
#define GEN8_CTX_L3LLC_COHERENT (1<<5)
#define GEN8_CTX_PRIVILEGE (1<<8)
enum {
	ADVANCED_CONTEXT = 0,
	LEGACY_CONTEXT,
	ADVANCED_AD_CONTEXT,
	LEGACY_64B_CONTEXT
};
#define GEN8_CTX_MODE_SHIFT 3
enum {
	FAULT_AND_HANG = 0,
	FAULT_AND_HALT, /* Debug only */
	FAULT_AND_STREAM,
	FAULT_AND_CONTINUE /* Unsupported */
};
#define GEN8_CTX_ID_SHIFT 32

static uint64_t execlists_ctx_descriptor(struct drm_i915_gem_object *ctx_obj,
					 u32 ctx_id)
{
	uint64_t desc, lrca;

	lrca = i915_gem_obj_ggtt_offset(ctx_obj);
	WARN_ON(lrca & 0xFFFFFFFF00000FFFULL);

	desc = GEN8_CTX_VALID;
	desc |= LEGACY_CONTEXT << GEN8_CTX_MODE_SHIFT;
	desc |= GEN8_CTX_L3LLC_COHERENT;
	desc |= GEN8_CTX_PRIVILEGE;
	desc |= lrca;
	desc |= (u64)ctx_id << GEN8_CTX_ID_SHIFT;

	/* TODO: WaDisableLiteRestore when we start using semaphore
	 * signalling between Command Streamers */
	/* desc |= GEN8_CTX_FORCE_RESTORE; */

	return desc;
}

static u32 execlists_ctx_write_tail(struct drm_i915_gem_object *obj, u32 tail, u32 tag)
{
	uint32_t *reg_state;

	reg_state = kmap_atomic(i915_gem_object_get_page(obj, 1));
	reg_state[CTX_RING_TAIL+1] = tail;
	kunmap_atomic(reg_state);

	return execlists_ctx_descriptor(obj, tag);
}

static void execlists_submit_pair(struct intel_engine_cs *engine,
				  struct i915_gem_request *rq[2])
{
	struct drm_i915_private *dev_priv = engine->i915;
	uint64_t tmp;
	uint32_t desc[4];

	/* XXX: You must always write both descriptors in the order below. */

	tmp = execlists_ctx_write_tail(rq[0]->ctx->ring[engine->id].state,
				       rq[0]->tail, rq[0]->tag);
	desc[3] = upper_32_bits(tmp);
	desc[2] = lower_32_bits(tmp);

	if (rq[1])
		tmp = execlists_ctx_write_tail(rq[1]->ctx->ring[engine->id].state,
					       rq[1]->tail, rq[1]->tag);
	else
		tmp = 0;
	desc[1] = upper_32_bits(tmp);
	desc[0] = lower_32_bits(tmp);

	gen6_gt_force_wake_get(dev_priv, engine->power_domains);
	I915_WRITE(RING_ELSP(engine), desc[1]);
	I915_WRITE(RING_ELSP(engine), desc[0]);
	I915_WRITE(RING_ELSP(engine), desc[3]);
	/* The context is automatically loaded after the following */
	I915_WRITE(RING_ELSP(engine), desc[2]);

	/* ELSP is a wo register, so use another nearby reg for posting instead */
	POSTING_READ(RING_EXECLIST_STATUS(engine));
	gen6_gt_force_wake_put(dev_priv, engine->power_domains);
}

static u16 next_tag(struct intel_engine_cs *engine)
{
	/* status tags are limited to 20b, so we use a u16 for convenience */
	if (++engine->next_tag == 0)
		++engine->next_tag;
	WARN_ON((s16)(engine->next_tag - engine->tag) < 0);
	return engine->next_tag;
}

static void execlists_submit(struct intel_engine_cs *engine)
{
	struct i915_gem_request *rq[2] = {};
	int i = 0;

	assert_spin_locked(&engine->irqlock);

	/* Try to read in pairs */
	while (!list_empty(&engine->pending)) {
		struct i915_gem_request *next;

		next = list_first_entry(&engine->pending,
					typeof(*next),
					engine_link);

		if (rq[i] == NULL) {
new_slot:
			next->tag = next_tag(engine);
			rq[i] = next;
		} else if (rq[i]->ctx == next->ctx) {
			/* Same ctx: ignore first request, as second request
			 * will update tail past first request's workload */
			next->tag = rq[i]->tag;
			rq[i] = next;
		} else {
			if (++i == ARRAY_SIZE(rq))
				break;

			goto new_slot;
		}

		/* Move to requests is staged via the submitted list
		 * so that we can keep the main request list out of
		 * the spinlock coverage.
		 */
		list_move_tail(&next->engine_link, &engine->submitted);
	}

	execlists_submit_pair(engine, rq);

	engine->execlists_submitted++;
	if (rq[1])
		engine->execlists_submitted++;
}

/**
 * intel_execlists_handle_ctx_events() - handle Context Switch interrupts
 * @ring: Engine Command Streamer to handle.
 *
 * Check the unread Context Status Buffers and manage the submission of new
 * contexts to the ELSP accordingly.
 */
void intel_execlists_irq_handler(struct intel_engine_cs *engine)
{
	struct drm_i915_private *dev_priv = engine->i915;
	unsigned long flags;
	u8 read_pointer;
	u8 write_pointer;

	read_pointer = engine->next_context_status_buffer;
	write_pointer = I915_READ(RING_CONTEXT_STATUS_PTR(engine)) & 0x07;
	if (read_pointer > write_pointer)
		write_pointer += 6;

	spin_lock_irqsave(&engine->irqlock, flags);

	while (read_pointer++ < write_pointer) {
		u32 reg = (RING_CONTEXT_STATUS_BUF(engine) +
			   (read_pointer % 6) * 8);
		u32 status = I915_READ(reg);

		if (status & GEN8_CTX_STATUS_PREEMPTED) {
			if (status & GEN8_CTX_STATUS_LITE_RESTORE)
				WARN(1, "Lite Restored request removed from queue\n");
			else
				WARN(1, "Preemption without Lite Restore\n");
		}

		if (status & (GEN8_CTX_STATUS_ACTIVE_IDLE | GEN8_CTX_STATUS_ELEMENT_SWITCH)) {
			engine->tag = I915_READ(reg + 4);
			engine->execlists_submitted--;
		}
	}

	if (engine->execlists_submitted < 2)
		execlists_submit(engine);

	spin_unlock_irqrestore(&engine->irqlock, flags);

	engine->next_context_status_buffer = write_pointer % 6;
	I915_WRITE(RING_CONTEXT_STATUS_PTR(engine),
		   ((u32)engine->next_context_status_buffer & 0x07) << 8);
}

static int
populate_lr_context(struct intel_context *ctx,
		    struct drm_i915_gem_object *ctx_obj,
		    struct intel_engine_cs *engine)
{
	struct intel_ringbuffer *ring = ctx->ring[engine->id].ring;
	struct i915_hw_ppgtt *ppgtt;
	uint32_t *reg_state;
	int ret;

	ret = i915_gem_object_set_to_cpu_domain(ctx_obj, true);
	if (ret) {
		DRM_DEBUG_DRIVER("Could not set to CPU domain\n");
		return ret;
	}

	ret = i915_gem_object_get_pages(ctx_obj);
	if (ret) {
		DRM_DEBUG_DRIVER("Could not get object pages\n");
		return ret;
	}

	/* The second page of the context object contains some fields which must
	 * be set up prior to the first execution. */
	reg_state = kmap_atomic(i915_gem_object_get_page(ctx_obj, 1));

	/* A context is actually a big batch buffer with several MI_LOAD_REGISTER_IMM
	 * commands followed by (reg, value) pairs. The values we are setting here are
	 * only for the first context restore: on a subsequent save, the GPU will
	 * recreate this batchbuffer with new values (including all the missing
	 * MI_LOAD_REGISTER_IMM commands that we are not initializing here). */
	if (engine->id == RCS)
		reg_state[CTX_LRI_HEADER_0] = MI_LOAD_REGISTER_IMM(14);
	else
		reg_state[CTX_LRI_HEADER_0] = MI_LOAD_REGISTER_IMM(11);
	reg_state[CTX_LRI_HEADER_0] |= MI_LRI_FORCE_POSTED;

	reg_state[CTX_CONTEXT_CONTROL] = RING_CONTEXT_CONTROL(engine);
	reg_state[CTX_CONTEXT_CONTROL+1] =
			_MASKED_BIT_ENABLE((1<<3) | MI_RESTORE_INHIBIT);

	reg_state[CTX_RING_HEAD] = RING_HEAD(engine->mmio_base);
	reg_state[CTX_RING_HEAD+1] = 0;
	reg_state[CTX_RING_TAIL] = RING_TAIL(engine->mmio_base);
	reg_state[CTX_RING_TAIL+1] = 0;
	reg_state[CTX_RING_BUFFER_START] = RING_START(engine->mmio_base);
	reg_state[CTX_RING_BUFFER_START+1] = i915_gem_obj_ggtt_offset(ring->obj);
	reg_state[CTX_RING_BUFFER_CONTROL] = RING_CTL(engine->mmio_base);
	reg_state[CTX_RING_BUFFER_CONTROL+1] =
			((ring->size - PAGE_SIZE) & RING_NR_PAGES) | RING_VALID;

	reg_state[CTX_BB_HEAD_U] = engine->mmio_base + 0x168;
	reg_state[CTX_BB_HEAD_U+1] = 0;
	reg_state[CTX_BB_HEAD_L] = engine->mmio_base + 0x140;
	reg_state[CTX_BB_HEAD_L+1] = 0;
	reg_state[CTX_BB_STATE] = engine->mmio_base + 0x110;
	reg_state[CTX_BB_STATE+1] = (1<<5);

	reg_state[CTX_SECOND_BB_HEAD_U] = engine->mmio_base + 0x11c;
	reg_state[CTX_SECOND_BB_HEAD_U+1] = 0;
	reg_state[CTX_SECOND_BB_HEAD_L] = engine->mmio_base + 0x114;
	reg_state[CTX_SECOND_BB_HEAD_L+1] = 0;
	reg_state[CTX_SECOND_BB_STATE] = engine->mmio_base + 0x118;
	reg_state[CTX_SECOND_BB_STATE+1] = 0;

	if (engine->id == RCS) {
		/* TODO: according to BSpec, the register state context
		 * for CHV does not have these. OTOH, these registers do
		 * exist in CHV. I'm waiting for a clarification */
		reg_state[CTX_BB_PER_CTX_PTR] = engine->mmio_base + 0x1c0;
		reg_state[CTX_BB_PER_CTX_PTR+1] = 0;
		reg_state[CTX_RCS_INDIRECT_CTX] = engine->mmio_base + 0x1c4;
		reg_state[CTX_RCS_INDIRECT_CTX+1] = 0;
		reg_state[CTX_RCS_INDIRECT_CTX_OFFSET] = engine->mmio_base + 0x1c8;
		reg_state[CTX_RCS_INDIRECT_CTX_OFFSET+1] = 0;
	}

	reg_state[CTX_LRI_HEADER_1] = MI_LOAD_REGISTER_IMM(9);
	reg_state[CTX_LRI_HEADER_1] |= MI_LRI_FORCE_POSTED;
	reg_state[CTX_CTX_TIMESTAMP] = engine->mmio_base + 0x3a8;
	reg_state[CTX_CTX_TIMESTAMP+1] = 0;

	reg_state[CTX_PDP3_UDW] = GEN8_RING_PDP_UDW(engine, 3);
	reg_state[CTX_PDP3_LDW] = GEN8_RING_PDP_LDW(engine, 3);
	reg_state[CTX_PDP2_UDW] = GEN8_RING_PDP_UDW(engine, 2);
	reg_state[CTX_PDP2_LDW] = GEN8_RING_PDP_LDW(engine, 2);
	reg_state[CTX_PDP1_UDW] = GEN8_RING_PDP_UDW(engine, 1);
	reg_state[CTX_PDP1_LDW] = GEN8_RING_PDP_LDW(engine, 1);
	reg_state[CTX_PDP0_UDW] = GEN8_RING_PDP_UDW(engine, 0);
	reg_state[CTX_PDP0_LDW] = GEN8_RING_PDP_LDW(engine, 0);

	ppgtt = ctx->ppgtt ?: engine->i915->mm.aliasing_ppgtt;
	reg_state[CTX_PDP3_UDW+1] = upper_32_bits(ppgtt->pd_dma_addr[3]);
	reg_state[CTX_PDP3_LDW+1] = lower_32_bits(ppgtt->pd_dma_addr[3]);
	reg_state[CTX_PDP2_UDW+1] = upper_32_bits(ppgtt->pd_dma_addr[2]);
	reg_state[CTX_PDP2_LDW+1] = lower_32_bits(ppgtt->pd_dma_addr[2]);
	reg_state[CTX_PDP1_UDW+1] = upper_32_bits(ppgtt->pd_dma_addr[1]);
	reg_state[CTX_PDP1_LDW+1] = lower_32_bits(ppgtt->pd_dma_addr[1]);
	reg_state[CTX_PDP0_UDW+1] = upper_32_bits(ppgtt->pd_dma_addr[0]);
	reg_state[CTX_PDP0_LDW+1] = lower_32_bits(ppgtt->pd_dma_addr[0]);

	if (engine->id == RCS) {
		reg_state[CTX_LRI_HEADER_2] = MI_LOAD_REGISTER_IMM(1);
		reg_state[CTX_R_PWR_CLK_STATE] = 0x20c8;
		reg_state[CTX_R_PWR_CLK_STATE+1] = 0;
	}

	kunmap_atomic(reg_state);

	return 0;
}

static uint32_t get_lr_context_size(struct intel_engine_cs *engine)
{
	int ret = 0;

	WARN_ON(INTEL_INFO(engine->i915)->gen < 8);

	switch (engine->id) {
	case RCS:
		if (INTEL_INFO(ring->dev)->gen >= 9)
			ret = GEN9_LR_CONTEXT_RENDER_SIZE;
		else
			ret = GEN8_LR_CONTEXT_RENDER_SIZE;
		break;
		break;
	case VCS:
	case BCS:
	case VECS:
	case VCS2:
		ret = GEN8_LR_CONTEXT_OTHER_SIZE;
		break;
	}

	return ret;
}

static struct intel_ringbuffer *
execlists_get_ring(struct intel_engine_cs *engine,
		   struct intel_context *ctx)
{
	struct drm_i915_gem_object *ctx_obj;
	struct intel_ringbuffer *ring;
	uint32_t context_size;
	int ret;

	ring = intel_engine_alloc_ring(engine, ctx, 32 * PAGE_SIZE);
	if (IS_ERR(ring)) {
		DRM_ERROR("Failed to allocate ringbuffer %s: %ld\n",
			  engine->name, PTR_ERR(ring));
		return ERR_CAST(ring);
	}

	context_size = round_up(get_lr_context_size(engine), 4096);

	ctx_obj = i915_gem_alloc_context_obj(engine->i915->dev, context_size);
	if (IS_ERR(ctx_obj)) {
		ret = PTR_ERR(ctx_obj);
		DRM_DEBUG_DRIVER("Alloc LRC backing obj failed: %d\n", ret);
		return ERR_CAST(ctx_obj);
	}

	ret = i915_gem_object_ggtt_pin(ctx_obj, GEN8_LR_CONTEXT_ALIGN, 0);
	if (ret) {
		DRM_DEBUG_DRIVER("Pin LRC backing obj failed: %d\n", ret);
		goto err_unref;
	}

	ret = populate_lr_context(ctx, ctx_obj, engine);
	if (ret) {
		DRM_DEBUG_DRIVER("Failed to populate LRC: %d\n", ret);
		goto err_unpin;
	}

	ctx->ring[engine->id].state = ctx_obj;

	if (ctx == engine->default_context) {
		struct drm_i915_private *dev_priv = engine->i915;
		u32 reg;

		/* The status page is offset 0 from the context object in LRCs. */
		engine->status_page.gfx_addr = i915_gem_obj_ggtt_offset(ctx_obj);
		engine->status_page.page_addr = kmap(sg_page(ctx_obj->pages->sgl));
		if (engine->status_page.page_addr == NULL) {
			ret = -ENOMEM;
			goto err_unpin;
		}

		engine->status_page.obj = ctx_obj;

		reg = RING_HWS_PGA(engine->mmio_base);
		I915_WRITE(reg, engine->status_page.gfx_addr);
		POSTING_READ(reg);
	}

	return 0;

err_unpin:
	i915_gem_object_ggtt_unpin(ctx_obj);
err_unref:
	drm_gem_object_unreference(&ctx_obj->base);
	return ERR_PTR(ret);
}

static void execlists_put_ring(struct intel_ringbuffer *ring,
			       struct intel_context *ctx)
{
	intel_ring_free(ring);
}

static int execlists_add_request(struct i915_gem_request *rq)
{
	unsigned long flags;

	spin_lock_irqsave(&rq->engine->irqlock, flags);

	list_add_tail(&rq->engine_link, &rq->engine->pending);
	if (rq->engine->execlists_submitted < 2)
		execlists_submit(rq->engine);

	spin_unlock_irqrestore(&rq->engine->irqlock, flags);

	return 0;
}

static bool execlists_rq_is_complete(struct i915_gem_request *rq)
{
	return (s16)(rq->engine->tag - rq->tag) >= 0;
}

static int execlists_suspend(struct intel_engine_cs *engine)
{
	struct drm_i915_private *dev_priv = engine->i915;
	unsigned long flags;

	/* disable submitting more requests until resume */
	spin_lock_irqsave(&engine->irqlock, flags);
	engine->execlists_submitted = ~0;
	spin_unlock_irqrestore(&engine->irqlock, flags);

	I915_WRITE(RING_MODE_GEN7(engine),
		   _MASKED_BIT_ENABLE(GFX_REPLAY_MODE) |
		   _MASKED_BIT_DISABLE(GFX_RUN_LIST_ENABLE));
	POSTING_READ(RING_MODE_GEN7(engine));
	DRM_DEBUG_DRIVER("Execlists disabled for %s\n", engine->name);

	return 0;
}

static int execlists_resume(struct intel_engine_cs *engine)
{
	struct drm_i915_private *dev_priv = engine->i915;
	unsigned long flags;

	/* XXX */
	I915_WRITE(RING_HWSTAM(engine->mmio_base), 0xffffffff);
	I915_WRITE(INSTPM, _MASKED_BIT_ENABLE(INSTPM_FORCE_ORDERING));

	/* We need to disable the AsyncFlip performance optimisations in order
	 * to use MI_WAIT_FOR_EVENT within the CS. It should already be
	 * programmed to '1' on all products.
	 *
	 * WaDisableAsyncFlipPerfMode:bdw
	 */
	I915_WRITE(MI_MODE, _MASKED_BIT_ENABLE(ASYNC_FLIP_PERF_DISABLE));

	I915_WRITE(RING_MODE_GEN7(engine),
		   _MASKED_BIT_DISABLE(GFX_REPLAY_MODE) |
		   _MASKED_BIT_ENABLE(GFX_RUN_LIST_ENABLE));
	POSTING_READ(RING_MODE_GEN7(engine));
	DRM_DEBUG_DRIVER("Execlists enabled for %s\n", engine->name);

	spin_lock_irqsave(&engine->irqlock, flags);
	engine->execlists_submitted = 0;
	execlists_submit(engine);
	spin_unlock_irqrestore(&engine->irqlock, flags);

	return 0;
}

static void execlists_retire(struct intel_engine_cs *engine,
			     u32 seqno)
{
	unsigned long flags;

	spin_lock_irqsave(&engine->irqlock, flags);
	list_splice_tail_init(&engine->submitted, &engine->requests);
	spin_unlock_irqrestore(&engine->irqlock, flags);
}

static void execlists_reset(struct intel_engine_cs *engine)
{
	unsigned long flags;

	spin_lock_irqsave(&engine->irqlock, flags);
	list_splice_tail_init(&engine->pending, &engine->submitted);
	list_splice_tail_init(&engine->submitted, &engine->requests);
	spin_unlock_irqrestore(&engine->irqlock, flags);
}

static bool enable_execlists(struct drm_i915_private *dev_priv)
{
	if (!HAS_LOGICAL_RING_CONTEXTS(dev_priv) ||
	    !USES_PPGTT(dev_priv))
		return false;

	return i915_module.enable_execlists;
}

static const int gen8_irq_shift[] = {
	[RCS] = GEN8_RCS_IRQ_SHIFT,
	[VCS] = GEN8_VCS1_IRQ_SHIFT,
	[BCS] = GEN8_BCS_IRQ_SHIFT,
	[VECS] = GEN8_VECS_IRQ_SHIFT,
	[VCS2] = GEN8_VCS2_IRQ_SHIFT,
};

int intel_engine_enable_execlists(struct intel_engine_cs *engine)
{
	if (!enable_execlists(engine->i915))
		return 0;

	if (WARN_ON(!IS_GEN8(engine->i915)))
		return 0;

	engine->irq_keep_mask |=
		GT_CONTEXT_SWITCH_INTERRUPT << gen8_irq_shift[engine->id];

	engine->get_ring = execlists_get_ring;
	engine->put_ring = execlists_put_ring;
	engine->add_request = execlists_add_request;
	engine->is_complete = execlists_rq_is_complete;

	/* Disable semaphores until further notice */
	engine->semaphore.wait = NULL;

	engine->suspend = execlists_suspend;
	engine->resume = execlists_resume;
	engine->reset = execlists_reset;
	engine->retire = execlists_retire;

	/* start suspended */
	engine->execlists_enabled = true;
	engine->execlists_submitted = ~0;

	return 0;
}
