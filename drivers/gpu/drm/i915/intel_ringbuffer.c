/*
 * Copyright © 2008-2010 Intel Corporation
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
 *    Eric Anholt <eric@anholt.net>
 *    Zou Nan hai <nanhai.zou@intel.com>
 *    Xiang Hai hao<haihao.xiang@intel.com>
 *
 */

#include <drm/drmP.h>
#include "i915_drv.h"
#include <drm/i915_drm.h>
#include "i915_trace.h"
#include "intel_drv.h"

/* Just userspace ABI convention to limit the wa batch bo to a resonable size */
#define I830_BATCH_LIMIT (256*1024)
#define I830_TLB_ENTRIES (2)
#define I830_WA_SIZE max(I830_TLB_ENTRIES*4096, I830_BATCH_LIMIT)

static int
gen2_emit_flush(struct i915_gem_request *rq, u32 flags)
{
	struct intel_ringbuffer *ring;
	u32 cmd;

	cmd = MI_FLUSH;
	if ((flags & (I915_FLUSH_CACHES | I915_INVALIDATE_CACHES)) == 0)
		cmd |= MI_NO_WRITE_FLUSH;

	if (flags & I915_INVALIDATE_CACHES)
		cmd |= MI_READ_FLUSH;

	ring = intel_ring_begin(rq, 1);
	if (IS_ERR(ring))
		return PTR_ERR(ring);

	intel_ring_emit(ring, cmd);
	intel_ring_advance(ring);

	return 0;
}

static int
gen4_emit_flush(struct i915_gem_request *rq, u32 flags)
{
	struct intel_ringbuffer *ring;
	u32 cmd;

	/*
	 * read/write caches:
	 *
	 * I915_GEM_DOMAIN_RENDER is always invalidated, but is
	 * only flushed if MI_NO_WRITE_FLUSH is unset.  On 965, it is
	 * also flushed at 2d versus 3d pipeline switches.
	 *
	 * read-only caches:
	 *
	 * I915_GEM_DOMAIN_SAMPLER is flushed on pre-965 if
	 * MI_READ_FLUSH is set, and is always flushed on 965.
	 *
	 * I915_GEM_DOMAIN_COMMAND may not exist?
	 *
	 * I915_GEM_DOMAIN_INSTRUCTION, which exists on 965, is
	 * invalidated when MI_EXE_FLUSH is set.
	 *
	 * I915_GEM_DOMAIN_VERTEX, which exists on 965, is
	 * invalidated with every MI_FLUSH.
	 *
	 * TLBs:
	 *
	 * On 965, TLBs associated with I915_GEM_DOMAIN_COMMAND
	 * and I915_GEM_DOMAIN_CPU in are invalidated at PTE write and
	 * I915_GEM_DOMAIN_RENDER and I915_GEM_DOMAIN_SAMPLER
	 * are flushed at any MI_FLUSH.
	 */

	cmd = MI_FLUSH;
	if ((flags & (I915_FLUSH_CACHES | I915_INVALIDATE_CACHES)) == 0)
		cmd |= MI_NO_WRITE_FLUSH;
	if (flags & I915_INVALIDATE_CACHES) {
		cmd |= MI_EXE_FLUSH;
		if (IS_G4X(rq->i915) || IS_GEN5(rq->i915))
			cmd |= MI_INVALIDATE_ISP;
	}

	ring = intel_ring_begin(rq, 1);
	if (IS_ERR(ring))
		return PTR_ERR(ring);

	intel_ring_emit(ring, cmd);
	intel_ring_advance(ring);

	return 0;
}

/**
 * Emits a PIPE_CONTROL with a non-zero post-sync operation, for
 * implementing two workarounds on gen6.  From section 1.4.7.1
 * "PIPE_CONTROL" of the Sandy Bridge PRM volume 2 part 1:
 *
 * [DevSNB-C+{W/A}] Before any depth stall flush (including those
 * produced by non-pipelined state commands), software needs to first
 * send a PIPE_CONTROL with no bits set except Post-Sync Operation !=
 * 0.
 *
 * [Dev-SNB{W/A}]: Before a PIPE_CONTROL with Write Cache Flush Enable
 * =1, a PIPE_CONTROL with any non-zero post-sync-op is required.
 *
 * And the workaround for these two requires this workaround first:
 *
 * [Dev-SNB{W/A}]: Pipe-control with CS-stall bit set must be sent
 * BEFORE the pipe-control with a post-sync op and no write-cache
 * flushes.
 *
 * And this last workaround is tricky because of the requirements on
 * that bit.  From section 1.4.7.2.3 "Stall" of the Sandy Bridge PRM
 * volume 2 part 1:
 *
 *     "1 of the following must also be set:
 *      - Render Target Cache Flush Enable ([12] of DW1)
 *      - Depth Cache Flush Enable ([0] of DW1)
 *      - Stall at Pixel Scoreboard ([1] of DW1)
 *      - Depth Stall ([13] of DW1)
 *      - Post-Sync Operation ([13] of DW1)
 *      - Notify Enable ([8] of DW1)"
 *
 * The cache flushes require the workaround flush that triggered this
 * one, so we can't use it.  Depth stall would trigger the same.
 * Post-sync nonzero is what triggered this second workaround, so we
 * can't use that one either.  Notify enable is IRQs, which aren't
 * really our business.  That leaves only stall at scoreboard.
 */
static int
gen6_emit_post_sync_nonzero_flush(struct i915_gem_request *rq)
{
	const u32 scratch = rq->engine->scratch.gtt_offset + 2*CACHELINE_BYTES;
	struct intel_ringbuffer *ring;

	ring = intel_ring_begin(rq, 8);
	if (IS_ERR(ring))
		return PTR_ERR(ring);

	intel_ring_emit(ring, GFX_OP_PIPE_CONTROL(4));
	intel_ring_emit(ring, PIPE_CONTROL_CS_STALL |
			PIPE_CONTROL_STALL_AT_SCOREBOARD);
	intel_ring_emit(ring, scratch | PIPE_CONTROL_GLOBAL_GTT);
	intel_ring_emit(ring, 0);

	intel_ring_emit(ring, GFX_OP_PIPE_CONTROL(4));
	intel_ring_emit(ring, PIPE_CONTROL_QW_WRITE);
	intel_ring_emit(ring, scratch | PIPE_CONTROL_GLOBAL_GTT);
	intel_ring_emit(ring, 0);

	intel_ring_advance(ring);
	return 0;
}

static int
gen6_render_emit_flush(struct i915_gem_request *rq, u32 flags)
{
	const u32 scratch = rq->engine->scratch.gtt_offset + 2*CACHELINE_BYTES;
	struct intel_ringbuffer *ring;
	u32 cmd = 0;
	int ret;

	if (flags & I915_FLUSH_CACHES) {
		/* Force SNB workarounds for PIPE_CONTROL flushes */
		ret = gen6_emit_post_sync_nonzero_flush(rq);
		if (ret)
			return ret;

		cmd |= PIPE_CONTROL_RENDER_TARGET_CACHE_FLUSH;
		cmd |= PIPE_CONTROL_DEPTH_CACHE_FLUSH;
	}
	if (flags & I915_INVALIDATE_CACHES) {
		cmd |= PIPE_CONTROL_TLB_INVALIDATE;
		cmd |= PIPE_CONTROL_INSTRUCTION_CACHE_INVALIDATE;
		cmd |= PIPE_CONTROL_TEXTURE_CACHE_INVALIDATE;
		cmd |= PIPE_CONTROL_VF_CACHE_INVALIDATE;
		cmd |= PIPE_CONTROL_CONST_CACHE_INVALIDATE;
		cmd |= PIPE_CONTROL_STATE_CACHE_INVALIDATE;
		/*
		 * TLB invalidate requires a post-sync write.
		 */
		cmd |= PIPE_CONTROL_QW_WRITE | PIPE_CONTROL_CS_STALL;
	}
	if (flags & I915_COMMAND_BARRIER)
		/*
		 * Ensure that any following seqno writes only happen
		 * when the render cache is indeed flushed.
		 */
		cmd |= PIPE_CONTROL_CS_STALL;

	if (cmd) {
		ring = intel_ring_begin(rq, 4);
		if (IS_ERR(ring))
			return PTR_ERR(ring);

		intel_ring_emit(ring, GFX_OP_PIPE_CONTROL(4));
		intel_ring_emit(ring, cmd);
		intel_ring_emit(ring, scratch | PIPE_CONTROL_GLOBAL_GTT);
		intel_ring_emit(ring, 0);
		intel_ring_advance(ring);
	}

	return 0;
}

static int
gen7_render_ring_cs_stall_wa(struct i915_gem_request *rq)
{
	struct intel_ringbuffer *ring;

	ring = intel_ring_begin(rq, 4);
	if (IS_ERR(ring))
		return PTR_ERR(ring);

	intel_ring_emit(ring, GFX_OP_PIPE_CONTROL(4));
	intel_ring_emit(ring, PIPE_CONTROL_CS_STALL |
			      PIPE_CONTROL_STALL_AT_SCOREBOARD);
	intel_ring_emit(ring, 0);
	intel_ring_emit(ring, 0);
	intel_ring_advance(ring);

	return 0;
}

static int gen7_ring_fbc_flush(struct i915_gem_request *rq, u32 value)
{
	struct intel_ringbuffer *ring;

	ring = intel_ring_begin(rq, 6);
	if (IS_ERR(ring))
		return PTR_ERR(rq);

	/* WaFbcNukeOn3DBlt:ivb/hsw */
	intel_ring_emit(ring, MI_LOAD_REGISTER_IMM(1));
	intel_ring_emit(ring, MSG_FBC_REND_STATE);
	intel_ring_emit(ring, value);
	intel_ring_emit(ring, MI_STORE_REGISTER_MEM(1) | MI_SRM_LRM_GLOBAL_GTT);
	intel_ring_emit(ring, MSG_FBC_REND_STATE);
	intel_ring_emit(ring, rq->engine->scratch.gtt_offset + 256);
	intel_ring_advance(ring);

	return 0;
}

static int
gen7_render_emit_flush(struct i915_gem_request *rq, u32 flags)
{
	const u32 scratch_addr = rq->engine->scratch.gtt_offset + 2 * CACHELINE_BYTES;
	struct intel_ringbuffer *ring;
	u32 cmd = 0;
	int ret;

	/*
	 * Ensure that any following seqno writes only happen when the render
	 * cache is indeed flushed.
	 *
	 * Workaround: 4th PIPE_CONTROL command (except the ones with only
	 * read-cache invalidate bits set) must have the CS_STALL bit set. We
	 * don't try to be clever and just set it unconditionally.
	 */
	cmd |= PIPE_CONTROL_CS_STALL;

	/* Just flush everything.  Experiments have shown that reducing the
	 * number of bits based on the write domains has little performance
	 * impact.
	 */
	if (flags & I915_FLUSH_CACHES) {
		cmd |= PIPE_CONTROL_RENDER_TARGET_CACHE_FLUSH;
		cmd |= PIPE_CONTROL_DEPTH_CACHE_FLUSH;
	}
	if (flags & I915_INVALIDATE_CACHES) {
		cmd |= PIPE_CONTROL_TLB_INVALIDATE;
		cmd |= PIPE_CONTROL_INSTRUCTION_CACHE_INVALIDATE;
		cmd |= PIPE_CONTROL_TEXTURE_CACHE_INVALIDATE;
		cmd |= PIPE_CONTROL_VF_CACHE_INVALIDATE;
		cmd |= PIPE_CONTROL_CONST_CACHE_INVALIDATE;
		cmd |= PIPE_CONTROL_STATE_CACHE_INVALIDATE;
		/*
		 * TLB invalidate requires a post-sync write.
		 */
		cmd |= PIPE_CONTROL_QW_WRITE;
		cmd |= PIPE_CONTROL_GLOBAL_GTT_IVB;

		/* Workaround: we must issue a pipe_control with CS-stall bit
		 * set before a pipe_control command that has the state cache
		 * invalidate bit set. */
		ret = gen7_render_ring_cs_stall_wa(rq);
		if (ret)
			return ret;
	}

	ring = intel_ring_begin(rq, 4);
	if (IS_ERR(ring))
		return PTR_ERR(ring);

	intel_ring_emit(ring, GFX_OP_PIPE_CONTROL(4));
	intel_ring_emit(ring, cmd);
	intel_ring_emit(ring, scratch_addr);
	intel_ring_emit(ring, 0);
	intel_ring_advance(ring);

	if (flags & I915_KICK_FBC) {
		ret = gen7_ring_fbc_flush(rq, FBC_REND_NUKE);
		if (ret)
			return ret;
	}

	return 0;
}

static int
gen8_emit_pipe_control(struct i915_gem_request *rq,
		       u32 cmd, u32 scratch_addr)
{
	struct intel_ringbuffer *ring;

	if (cmd == 0)
		return 0;

	ring = intel_ring_begin(rq, 6);
	if (IS_ERR(rq))
		return PTR_ERR(rq);

	intel_ring_emit(ring, GFX_OP_PIPE_CONTROL(6));
	intel_ring_emit(ring, cmd);
	intel_ring_emit(ring, scratch_addr);
	intel_ring_emit(ring, 0);
	intel_ring_emit(ring, 0);
	intel_ring_emit(ring, 0);
	intel_ring_advance(ring);

	return 0;
}

static int
gen8_render_emit_flush(struct i915_gem_request *rq,
		       u32 flags)
{
	const u32 scratch_addr = rq->engine->scratch.gtt_offset + 2 * CACHELINE_BYTES;
	u32 cmd = 0;
	int ret;

	if (flags & I915_FLUSH_CACHES) {
		cmd |= PIPE_CONTROL_RENDER_TARGET_CACHE_FLUSH;
		cmd |= PIPE_CONTROL_DEPTH_CACHE_FLUSH;
	}
	if (flags & I915_INVALIDATE_CACHES) {
		cmd |= PIPE_CONTROL_TLB_INVALIDATE;
		cmd |= PIPE_CONTROL_INSTRUCTION_CACHE_INVALIDATE;
		cmd |= PIPE_CONTROL_TEXTURE_CACHE_INVALIDATE;
		cmd |= PIPE_CONTROL_VF_CACHE_INVALIDATE;
		cmd |= PIPE_CONTROL_CONST_CACHE_INVALIDATE;
		cmd |= PIPE_CONTROL_STATE_CACHE_INVALIDATE;
		cmd |= PIPE_CONTROL_QW_WRITE;
		cmd |= PIPE_CONTROL_GLOBAL_GTT_IVB;

		/* WaCsStallBeforeStateCacheInvalidate:bdw,chv */
		ret = gen8_emit_pipe_control(rq,
					     PIPE_CONTROL_CS_STALL |
					     PIPE_CONTROL_STALL_AT_SCOREBOARD,
					     0);
		if (ret)
			return ret;
	}

	if (flags & I915_COMMAND_BARRIER)
		cmd |= PIPE_CONTROL_CS_STALL;


	ret = gen8_emit_pipe_control(rq, cmd, scratch_addr);
	if (ret)
		return ret;

	if (flags & I915_KICK_FBC) {
		ret = gen7_ring_fbc_flush(rq, FBC_REND_NUKE);
		if (ret)
			return ret;
	}

	return 0;
}

static void ring_write_tail(struct intel_engine_cs *engine,
			    u32 value)
{
	struct drm_i915_private *dev_priv = engine->i915;
	I915_WRITE_TAIL(engine, value);
}

u64 intel_engine_get_active_head(struct intel_engine_cs *engine)
{
	struct drm_i915_private *dev_priv = engine->i915;
	u64 acthd;

	if (INTEL_INFO(dev_priv)->gen >= 8)
		acthd = I915_READ64_2x32(RING_ACTHD(engine->mmio_base),
					 RING_ACTHD_UDW(engine->mmio_base));
	else if (INTEL_INFO(dev_priv)->gen >= 4)
		acthd = I915_READ(RING_ACTHD(engine->mmio_base));
	else
		acthd = I915_READ(ACTHD);

	return acthd;
}

static bool engine_stop(struct intel_engine_cs *engine)
{
	struct drm_i915_private *dev_priv = engine->i915;

	if (!IS_GEN2(dev_priv)) {
		I915_WRITE_MODE(engine, _MASKED_BIT_ENABLE(STOP_RING));
		if (wait_for((I915_READ_MODE(engine) & MODE_IDLE) != 0, 1000)) {
			DRM_ERROR("%s : timed out trying to stop ring\n", engine->name);
			/* Sometimes we observe that the idle flag is not
			 * set even though the ring is empty. So double
			 * check before giving up.
			 */
			if (I915_READ_HEAD(engine) != I915_READ_TAIL(engine))
				return false;
		}
	}

	I915_WRITE_CTL(engine, 0);
	I915_WRITE_HEAD(engine, 0);
	engine->write_tail(engine, 0);

	if (!IS_GEN2(dev_priv)) {
		(void)I915_READ_CTL(engine);
		I915_WRITE_MODE(engine, _MASKED_BIT_DISABLE(STOP_RING));
	}

	return (I915_READ_HEAD(engine) & HEAD_ADDR) == 0;
}

static int engine_suspend(struct intel_engine_cs *engine)
{
	return engine_stop(engine) ? 0 : -EIO;
}

static int enable_status_page(struct intel_engine_cs *engine)
{
	struct drm_i915_private *dev_priv = engine->i915;
	u32 mmio, addr;
	int ret = 0;

	if (!I915_NEED_GFX_HWS(dev_priv)) {
		addr = dev_priv->status_page_dmah->busaddr;
		if (INTEL_INFO(dev_priv)->gen >= 4)
			addr |= (dev_priv->status_page_dmah->busaddr >> 28) & 0xf0;
		mmio = HWS_PGA;
	} else {
		addr = engine->status_page.gfx_addr;
		/* The ring status page addresses are no longer next to the rest of
		 * the ring registers as of gen7.
		 */
		if (IS_GEN7(dev_priv)) {
			switch (engine->id) {
			default:
			case RCS:
				mmio = RENDER_HWS_PGA_GEN7;
				break;
			case BCS:
				mmio = BLT_HWS_PGA_GEN7;
				break;
				/*
				 * VCS2 actually doesn't exist on Gen7. Only shut up
				 * gcc switch check warning
				 */
			case VCS2:
			case VCS:
				mmio = BSD_HWS_PGA_GEN7;
				break;
			case VECS:
				mmio = VEBOX_HWS_PGA_GEN7;
				break;
			}
		} else if (IS_GEN6(dev_priv)) {
			mmio = RING_HWS_PGA_GEN6(engine->mmio_base);
		} else {
			/* XXX: gen8 returns to sanity */
			mmio = RING_HWS_PGA(engine->mmio_base);
		}
	}

	if (IS_GEN5(dev_priv) || IS_G4X(dev_priv)) {
		if (wait_for((I915_READ(mmio) & 1) == 0, 1000))
			DRM_ERROR("%s: wait for Translation-in-Progress to complete for HWS timed out\n",
				  engine->name);
	}

	I915_WRITE(mmio, addr);
	POSTING_READ(mmio);

	/*
	 * Flush the TLB for this page
	 *
	 * FIXME: These two bits have disappeared on gen8, so a question
	 * arises: do we still need this and if so how should we go about
	 * invalidating the TLB?
	 */
	if (INTEL_INFO(dev_priv)->gen >= 6 && INTEL_INFO(dev_priv)->gen < 8) {
		u32 reg = RING_INSTPM(engine->mmio_base);

		/* ring should be idle before issuing a sync flush*/
		WARN_ON((I915_READ_MODE(engine) & MODE_IDLE) == 0);

		I915_WRITE(reg,
			   _MASKED_BIT_ENABLE(INSTPM_TLB_INVALIDATE |
					      INSTPM_SYNC_FLUSH));
		if (wait_for((I915_READ(reg) & INSTPM_SYNC_FLUSH) == 0,
			     1000)) {
			DRM_ERROR("%s: wait for SyncFlush to complete for TLB invalidation timed out\n",
				  engine->name);
			ret = -EIO;
		}
	}

	return ret;
}

static struct intel_ringbuffer *
engine_get_ring(struct intel_engine_cs *engine,
		struct intel_context *ctx)
{
	struct drm_i915_private *dev_priv = engine->i915;
	struct intel_ringbuffer *ring;
	int ret = 0;

	ring = engine->legacy_ring;
	if (ring)
		return ring;

	ring = intel_engine_alloc_ring(engine, ctx, 32 * PAGE_SIZE);
	if (IS_ERR(ring)) {
		DRM_ERROR("Failed to allocate ringbuffer for %s: %ld\n", engine->name, PTR_ERR(ring));
		return ERR_CAST(ring);
	}

	gen6_gt_force_wake_get(dev_priv, FORCEWAKE_ALL);
	if (!engine_stop(engine)) {
		/* G45 ring initialization often fails to reset head to zero */
		DRM_DEBUG_KMS("%s head not reset to zero "
			      "ctl %08x head %08x tail %08x start %08x\n",
			      engine->name,
			      I915_READ_CTL(engine),
			      I915_READ_HEAD(engine),
			      I915_READ_TAIL(engine),
			      I915_READ_START(engine));
		if (!engine_stop(engine)) {
			DRM_ERROR("failed to set %s head to zero "
				  "ctl %08x head %08x tail %08x start %08x\n",
				  engine->name,
				  I915_READ_CTL(engine),
				  I915_READ_HEAD(engine),
				  I915_READ_TAIL(engine),
				  I915_READ_START(engine));
			ret = -EIO;
		}
	}
	gen6_gt_force_wake_put(dev_priv, FORCEWAKE_ALL);

	if (ret == 0) {
		engine->legacy_ring = ring;
	} else {
		intel_ring_free(ring);
		ring = ERR_PTR(ret);
	}

	return ring;
}

static int engine_resume(struct intel_engine_cs *engine)
{
	struct drm_i915_private *dev_priv = engine->i915;
	struct intel_ringbuffer *ring = engine->legacy_ring;
	int retry = 3, ret;

	if (WARN_ON(ring == NULL))
		return -ENODEV;

	gen6_gt_force_wake_get(dev_priv, FORCEWAKE_ALL);

	ret = enable_status_page(engine);

reset:
	/* Enforce ordering by reading HEAD register back */
	engine->write_tail(engine, ring->tail);
	I915_WRITE_HEAD(engine, ring->head);
	(void)I915_READ_HEAD(engine);

	/* Initialize the ring. This must happen _after_ we've cleared the ring
	 * registers with the above sequence (the readback of the HEAD registers
	 * also enforces ordering), otherwise the hw might lose the new ring
	 * register values. */
	I915_WRITE_START(engine, i915_gem_obj_ggtt_offset(ring->obj));

	/* WaClearRingBufHeadRegAtInit:ctg,elk */
	if (I915_READ_HEAD(engine) != ring->head)
		DRM_DEBUG("%s initialization failed [head=%08x], fudging\n",
			  engine->name, I915_READ_HEAD(engine));
	I915_WRITE_HEAD(engine, ring->head);
	(void)I915_READ_HEAD(engine);

	I915_WRITE_CTL(engine,
		       ((ring->size - PAGE_SIZE) & RING_NR_PAGES)
		       | RING_VALID);

	if (wait_for((I915_READ_CTL(engine) & RING_VALID) != 0, 50)) {
		if (retry-- && engine_stop(engine))
			goto reset;
	}

	if ((I915_READ_CTL(engine) & RING_VALID) == 0 ||
	    I915_READ_START(engine) != i915_gem_obj_ggtt_offset(ring->obj)) {
		DRM_ERROR("%s initialization failed "
			  "ctl %08x (valid? %d) head %08x [expected %08x], tail %08x [expected %08x], start %08x [expected %08lx]\n",
			  engine->name,
			  I915_READ_CTL(engine), I915_READ_CTL(engine) & RING_VALID,
			  I915_READ_HEAD(engine), ring->head,
			  I915_READ_TAIL(engine), ring->tail,
			  I915_READ_START(engine), (unsigned long)i915_gem_obj_ggtt_offset(ring->obj));
		ret = -EIO;
	}

	gen6_gt_force_wake_put(dev_priv, FORCEWAKE_ALL);
	return ret;
}

static void engine_put_ring(struct intel_ringbuffer *ring,
			    struct intel_context *ctx)
{
	if (ring->last_context == ctx) {
		struct i915_gem_request *rq;
		int ret = -EINVAL;

		rq = intel_engine_alloc_request(ring->engine,
						ring->engine->default_context);
		if (!IS_ERR(rq)) {
			ret = i915_request_commit(rq);
			i915_request_put(rq);
		}
		if (WARN_ON(ret))
			ring->last_context = ring->engine->default_context;
	}
}

static int engine_add_request(struct i915_gem_request *rq)
{
	rq->engine->write_tail(rq->engine, rq->tail);
	list_add_tail(&rq->engine_list, &rq->engine->requests);
	return 0;
}

static bool engine_rq_is_complete(struct i915_gem_request *rq)
{
	return __i915_seqno_passed(intel_engine_get_seqno(rq->engine),
				   rq->seqno);
}

static int
init_broken_cs_tlb_wa(struct intel_engine_cs *engine)
{
	struct drm_i915_gem_object *obj;
	int ret;

	obj = i915_gem_object_create_stolen(engine->i915->dev, I830_WA_SIZE);
	if (obj == NULL)
		obj = i915_gem_alloc_object(engine->i915->dev, I830_WA_SIZE);
	if (obj == NULL) {
		DRM_ERROR("Failed to allocate batch bo\n");
		return -ENOMEM;
	}

	ret = i915_gem_obj_ggtt_pin(obj, 0, 0);
	if (ret != 0) {
		drm_gem_object_unreference(&obj->base);
		DRM_ERROR("Failed to ping batch bo\n");
		return ret;
	}

	engine->scratch.obj = obj;
	engine->scratch.gtt_offset = i915_gem_obj_ggtt_offset(obj);
	return 0;
}

static int
init_pipe_control(struct intel_engine_cs *engine)
{
	int ret;

	engine->scratch.obj = i915_gem_alloc_object(engine->i915->dev, 4096);
	if (engine->scratch.obj == NULL) {
		ret = -ENOMEM;
		goto err;
	}

	ret = i915_gem_object_set_cache_level(engine->scratch.obj,
					      I915_CACHE_LLC);
	if (ret)
		goto err_unref;

	ret = i915_gem_obj_ggtt_pin(engine->scratch.obj, 4096, 0);
	if (ret)
		goto err_unref;

	engine->scratch.gtt_offset =
		i915_gem_obj_ggtt_offset(engine->scratch.obj);
	DRM_DEBUG_DRIVER("%s pipe control offset: 0x%08x\n",
			 engine->name, engine->scratch.gtt_offset);
	return 0;

err_unref:
	drm_gem_object_unreference(&engine->scratch.obj->base);
	engine->scratch.obj = NULL;
err:
	DRM_ERROR("Failed to allocate seqno page [%d]\n", ret);
	return ret;
}

static int
emit_lri(struct i915_gem_request *rq,
	 int num_registers,
	 ...)
{
	struct intel_ringbuffer *ring;
	va_list ap;

	BUG_ON(num_registers > 60);

	ring = intel_ring_begin(rq, 2*num_registers + 1);
	if (IS_ERR(ring))
		return PTR_ERR(ring);

	intel_ring_emit(ring, MI_LOAD_REGISTER_IMM(num_registers));
	va_start(ap, num_registers);
	while (num_registers--) {
		intel_ring_emit(ring, va_arg(ap, u32));
		intel_ring_emit(ring, va_arg(ap, u32));
	}
	va_end(ap);
	intel_ring_advance(ring);

	return 0;
}

static int bdw_render_init_context(struct i915_gem_request *rq)
{
	int ret;

	ret = emit_lri(rq, 6,

	/* FIXME: Unclear whether we really need this on production bdw. */
	GEN8_ROW_CHICKEN,
	/* WaDisablePartialInstShootdown:bdw */
	_MASKED_BIT_ENABLE(PARTIAL_INSTRUCTION_SHOOTDOWN_DISABLE) |
	/* WaDisableThreadStallDopClockGating:bdw (pre-production) */
	_MASKED_BIT_ENABLE(STALL_DOP_GATING_DISABLE),

	GEN7_ROW_CHICKEN2,
	/* WaDisableDopClockGating:bdw */
	_MASKED_BIT_ENABLE(DOP_CLOCK_GATING_DISABLE),

	HALF_SLICE_CHICKEN3,
	_MASKED_BIT_ENABLE(GEN8_SAMPLER_POWER_BYPASS_DIS),

	/* Use Force Non-Coherent whenever executing a 3D context. This is a
	 * workaround for for a possible hang in the unlikely event a TLB
	 * invalidation occurs during a PSD flush.
	 */
	HDC_CHICKEN0,
	_MASKED_BIT_ENABLE(HDC_FORCE_NON_COHERENT) |
	/* WaDisableFenceDestinationToSLM:bdw (GT3 pre-production) */
	_MASKED_BIT_ENABLE(IS_BDW_GT3(rq->i915) ? HDC_FENCE_DEST_SLM_DISABLE : 0),

	CACHE_MODE_1,
	/* Wa4x4STCOptimizationDisable:bdw */
	_MASKED_BIT_ENABLE(GEN8_4x4_STC_OPTIMIZATION_DISABLE),

	/*
	 * BSpec recommends 8x4 when MSAA is used,
	 * however in practice 16x4 seems fastest.
	 *
	 * Note that PS/WM thread counts depend on the WIZ hashing
	 * disable bit, which we don't touch here, but it's good
	 * to keep in mind (see 3DSTATE_PS and 3DSTATE_WM).
	 */
	GEN7_GT_MODE,
	GEN6_WIZ_HASHING_MASK | GEN6_WIZ_HASHING_16x4);
	if (ret)
		return ret;

	return i915_gem_render_state_init(rq);
}

static int chv_render_init_context(struct i915_gem_request *rq)
{
	int ret;

	ret = emit_lri(rq, 2,

	GEN8_ROW_CHICKEN,
	/* WaDisablePartialInstShootdown:chv */
	_MASKED_BIT_ENABLE(PARTIAL_INSTRUCTION_SHOOTDOWN_DISABLE) |
	/* WaDisableThreadStallDopClockGating:chv */
	_MASKED_BIT_ENABLE(STALL_DOP_GATING_DISABLE),

	/* Use Force Non-Coherent whenever executing a 3D context. This is a
	 * workaround for a possible hang in the unlikely event a TLB
	 * invalidation occurs during a PSD flush.
	 */
	HDC_CHICKEN0,
	/* WaForceEnableNonCoherent:chv */
	HDC_FORCE_NON_COHERENT |
	/* WaHdcDisableFetchWhenMasked:chv */
	HDC_DONOT_FETCH_MEM_WHEN_MASKED);

	if (ret)
		return ret;

	return i915_gem_render_state_init(rq);
}

static int render_resume(struct intel_engine_cs *engine)
{
	struct drm_i915_private *dev_priv = engine->i915;
	int ret;

	ret = engine_resume(engine);
	if (ret)
		return ret;

	/* WaTimedSingleVertexDispatch:cl,bw,ctg,elk,ilk,snb */
	if (INTEL_INFO(dev_priv)->gen >= 4 && INTEL_INFO(dev_priv)->gen < 7)
		I915_WRITE(MI_MODE, _MASKED_BIT_ENABLE(VS_TIMER_DISPATCH));

	/* We need to disable the AsyncFlip performance optimisations in order
	 * to use MI_WAIT_FOR_EVENT within the CS. It should already be
	 * programmed to '1' on all products.
	 *
	 * WaDisableAsyncFlipPerfMode:snb,ivb,hsw,vlv,bdw,chv
	 */
	if (INTEL_INFO(dev_priv)->gen >= 6 && INTEL_INFO(dev_priv)->gen < 9)
		I915_WRITE(MI_MODE, _MASKED_BIT_ENABLE(ASYNC_FLIP_PERF_DISABLE));

	/* Required for the hardware to program scanline values for waiting */
	/* WaEnableFlushTlbInvalidationMode:snb */
	if (INTEL_INFO(dev_priv)->gen == 6)
		I915_WRITE(GFX_MODE,
			   _MASKED_BIT_ENABLE(GFX_TLB_INVALIDATE_EXPLICIT));

	/* WaBCSVCSTlbInvalidationMode:ivb,vlv,hsw */
	if (IS_GEN7(dev_priv))
		I915_WRITE(GFX_MODE_GEN7,
			   _MASKED_BIT_ENABLE(GFX_TLB_INVALIDATE_EXPLICIT) |
			   _MASKED_BIT_ENABLE(GFX_REPLAY_MODE));

	if (IS_GEN6(dev_priv)) {
		/* From the Sandybridge PRM, volume 1 part 3, page 24:
		 * "If this bit is set, STCunit will have LRA as replacement
		 *  policy. [...] This bit must be reset.  LRA replacement
		 *  policy is not supported."
		 */
		I915_WRITE(CACHE_MODE_0,
			   _MASKED_BIT_DISABLE(CM0_STC_EVICT_DISABLE_LRA_SNB));
	}

	if (INTEL_INFO(dev_priv)->gen >= 6)
		I915_WRITE(INSTPM, _MASKED_BIT_ENABLE(INSTPM_FORCE_ORDERING));

	return 0;
}

static void cleanup_status_page(struct intel_engine_cs *engine)
{
	struct drm_i915_gem_object *obj;

	obj = engine->status_page.obj;
	if (obj == NULL)
		return;

	kunmap(sg_page(obj->pages->sgl));
	i915_gem_object_ggtt_unpin(obj);
	drm_gem_object_unreference(&obj->base);
	engine->status_page.obj = NULL;
}

static void engine_cleanup(struct intel_engine_cs *engine)
{
	if (engine->legacy_ring)
		intel_ring_free(engine->legacy_ring);

	cleanup_status_page(engine);
	i915_cmd_parser_fini_engine(engine);
}

static void render_cleanup(struct intel_engine_cs *engine)
{
	struct drm_i915_private *dev_priv = engine->i915;

	engine_cleanup(engine);

	if (dev_priv->semaphore_obj) {
		i915_gem_object_ggtt_unpin(dev_priv->semaphore_obj);
		drm_gem_object_unreference(&dev_priv->semaphore_obj->base);
		dev_priv->semaphore_obj = NULL;
	}

	if (engine->scratch.obj) {
		i915_gem_object_ggtt_unpin(engine->scratch.obj);
		drm_gem_object_unreference(&engine->scratch.obj->base);
		engine->scratch.obj = NULL;
	}
}

static int
gen8_rcs_emit_signal(struct i915_gem_request *rq, int id)
{
	u64 offset = GEN8_SEMAPHORE_OFFSET(rq->i915, rq->engine->id, id);
	struct intel_ringbuffer *ring;

	ring = intel_ring_begin(rq, 8);
	if (IS_ERR(ring))
		return PTR_ERR(ring);

	intel_ring_emit(ring, GFX_OP_PIPE_CONTROL(6));
	intel_ring_emit(ring,
			PIPE_CONTROL_GLOBAL_GTT_IVB |
			PIPE_CONTROL_QW_WRITE |
			PIPE_CONTROL_FLUSH_ENABLE);
	intel_ring_emit(ring, lower_32_bits(offset));
	intel_ring_emit(ring, upper_32_bits(offset));
	intel_ring_emit(ring, rq->seqno);
	intel_ring_emit(ring, 0);
	intel_ring_emit(ring,
			MI_SEMAPHORE_SIGNAL |
			MI_SEMAPHORE_TARGET(id));
	intel_ring_emit(ring, 0);
	intel_ring_advance(ring);

	return 0;
}

static int
gen8_xcs_emit_signal(struct i915_gem_request *rq, int id)
{
	u64 offset = GEN8_SEMAPHORE_OFFSET(rq->i915, rq->engine->id, id);
	struct intel_ringbuffer *ring;

	ring = intel_ring_begin(rq, 6);
	if (IS_ERR(ring))
		return PTR_ERR(ring);

	intel_ring_emit(ring,
			MI_FLUSH_DW |
			MI_FLUSH_DW_OP_STOREDW |
			(4 - 2));
	intel_ring_emit(ring,
			lower_32_bits(offset) |
			MI_FLUSH_DW_USE_GTT);
	intel_ring_emit(ring, upper_32_bits(offset));
	intel_ring_emit(ring, rq->seqno);
	intel_ring_emit(ring,
			MI_SEMAPHORE_SIGNAL |
			MI_SEMAPHORE_TARGET(id));
	intel_ring_emit(ring, 0);
	intel_ring_advance(ring);

	return 0;
}

static int
gen6_emit_signal(struct i915_gem_request *rq, int id)
{
	struct intel_ringbuffer *ring;

	ring = intel_ring_begin(rq, 3);
	if (IS_ERR(ring))
		return PTR_ERR(ring);

	intel_ring_emit(ring, MI_LOAD_REGISTER_IMM(1));
	intel_ring_emit(ring, rq->engine->semaphore.mbox.signal[id]);
	intel_ring_emit(ring, rq->seqno);
	intel_ring_advance(ring);

	return 0;
}

/**
 * intel_ring_sync - sync the waiter to the signaller on seqno
 *
 * @waiter - ring that is waiting
 * @signaller - ring which has, or will signal
 * @seqno - seqno which the waiter will block on
 */

static int
gen8_emit_wait(struct i915_gem_request *waiter,
	       struct i915_gem_request *signaller)
{
	u64 offset = GEN8_SEMAPHORE_OFFSET(waiter->i915, signaller->engine->id, waiter->engine->id);
	struct intel_ringbuffer *ring;

	ring = intel_ring_begin(waiter, 4);
	if (IS_ERR(ring))
		return PTR_ERR(ring);

	intel_ring_emit(ring,
			MI_SEMAPHORE_WAIT |
			MI_SEMAPHORE_GLOBAL_GTT |
			MI_SEMAPHORE_POLL |
			MI_SEMAPHORE_SAD_GTE_SDD);
	intel_ring_emit(ring, signaller->breadcrumb[waiter->engine->id]);
	intel_ring_emit(ring, lower_32_bits(offset));
	intel_ring_emit(ring, upper_32_bits(offset));
	intel_ring_advance(ring);
	return 0;
}

static int
gen6_emit_wait(struct i915_gem_request *waiter,
	       struct i915_gem_request *signaller)
{
	u32 dw1 = MI_SEMAPHORE_MBOX |
		  MI_SEMAPHORE_COMPARE |
		  MI_SEMAPHORE_REGISTER;
	u32 wait_mbox = signaller->engine->semaphore.mbox.wait[waiter->engine->id];
	struct intel_ringbuffer *ring;

	WARN_ON(wait_mbox == MI_SEMAPHORE_SYNC_INVALID);

	ring = intel_ring_begin(waiter, 3);
	if (IS_ERR(ring))
		return PTR_ERR(ring);

	intel_ring_emit(ring, dw1 | wait_mbox);
	/* Throughout all of the GEM code, seqno passed implies our current
	 * seqno is >= the last seqno executed. However for hardware the
	 * comparison is strictly greater than.
	 */
	intel_ring_emit(ring, signaller->breadcrumb[waiter->engine->id] - 1);
	intel_ring_emit(ring, 0);
	intel_ring_advance(ring);

	return 0;
}

static void
gen5_irq_get(struct intel_engine_cs *engine)
{
	struct drm_i915_private *i915 = engine->i915;
	unsigned long flags;

	spin_lock_irqsave(&i915->irq_lock, flags);
	if (engine->irq_refcount++ == 0)
		gen5_enable_gt_irq(i915, engine->irq_enable_mask);
	spin_unlock_irqrestore(&i915->irq_lock, flags);
}

static void
gen5_irq_put(struct intel_engine_cs *engine)
{
	struct drm_i915_private *i915 = engine->i915;
	unsigned long flags;

	spin_lock_irqsave(&i915->irq_lock, flags);
	if (--engine->irq_refcount == 0)
		gen5_disable_gt_irq(i915, engine->irq_enable_mask);
	spin_unlock_irqrestore(&i915->irq_lock, flags);
}

static void
i9xx_irq_get(struct intel_engine_cs *engine)
{
	struct drm_i915_private *dev_priv = engine->i915;
	unsigned long flags;

	spin_lock_irqsave(&dev_priv->irq_lock, flags);
	if (engine->irq_refcount++ == 0) {
		dev_priv->irq_mask &= ~engine->irq_enable_mask;
		I915_WRITE(IMR, dev_priv->irq_mask);
		POSTING_READ(IMR);
	}
	spin_unlock_irqrestore(&dev_priv->irq_lock, flags);
}

static void
i9xx_irq_put(struct intel_engine_cs *engine)
{
	struct drm_i915_private *dev_priv = engine->i915;
	unsigned long flags;

	spin_lock_irqsave(&dev_priv->irq_lock, flags);
	if (--engine->irq_refcount == 0) {
		dev_priv->irq_mask |= engine->irq_enable_mask;
		I915_WRITE(IMR, dev_priv->irq_mask);
		POSTING_READ(IMR);
	}
	spin_unlock_irqrestore(&dev_priv->irq_lock, flags);
}

static void
i8xx_irq_get(struct intel_engine_cs *engine)
{
	struct drm_i915_private *dev_priv = engine->i915;
	unsigned long flags;

	spin_lock_irqsave(&dev_priv->irq_lock, flags);
	if (engine->irq_refcount++ == 0) {
		dev_priv->irq_mask &= ~engine->irq_enable_mask;
		I915_WRITE16(IMR, dev_priv->irq_mask);
		POSTING_READ16(IMR);
	}
	spin_unlock_irqrestore(&dev_priv->irq_lock, flags);
}

static void
i8xx_irq_put(struct intel_engine_cs *engine)
{
	struct drm_i915_private *dev_priv = engine->i915;
	unsigned long flags;

	spin_lock_irqsave(&dev_priv->irq_lock, flags);
	if (--engine->irq_refcount == 0) {
		dev_priv->irq_mask |= engine->irq_enable_mask;
		I915_WRITE16(IMR, dev_priv->irq_mask);
		POSTING_READ16(IMR);
	}
	spin_unlock_irqrestore(&dev_priv->irq_lock, flags);
}

static int
bsd_emit_flush(struct i915_gem_request *rq,
	       u32 flags)
{
	struct intel_ringbuffer *ring;

	ring = intel_ring_begin(rq, 1);
	if (IS_ERR(ring))
		return PTR_ERR(ring);

	intel_ring_emit(ring, MI_FLUSH);
	intel_ring_advance(ring);
	return 0;
}

static int
emit_breadcrumb(struct i915_gem_request *rq)
{
	struct intel_ringbuffer *ring;

	ring = intel_ring_begin(rq, 4);
	if (IS_ERR(ring))
		return PTR_ERR(ring);

	intel_ring_emit(ring, MI_STORE_DWORD_INDEX);
	intel_ring_emit(ring, I915_GEM_HWS_INDEX << MI_STORE_DWORD_INDEX_SHIFT);
	intel_ring_emit(ring, rq->seqno);
	intel_ring_emit(ring, MI_USER_INTERRUPT);
	intel_ring_advance(ring);

	return 0;
}

static void
gen6_irq_get(struct intel_engine_cs *engine)
{
	struct drm_i915_private *dev_priv = engine->i915;
	unsigned long flags;

	spin_lock_irqsave(&dev_priv->irq_lock, flags);
	if (engine->irq_refcount++ == 0) {
		I915_WRITE_IMR(engine,
			       ~(engine->irq_enable_mask |
				 engine->irq_keep_mask));
		gen5_enable_gt_irq(dev_priv, engine->irq_enable_mask);
	}
	spin_unlock_irqrestore(&dev_priv->irq_lock, flags);

	/* Keep the device awake to save expensive CPU cycles when
	 * reading the registers.
	 */
	gen6_gt_force_wake_get(dev_priv, engine->power_domains);
}

static void
gen6_irq_barrier(struct intel_engine_cs *engine)
{
	/* w/a for lax serialisation of GPU writes with IRQs */
	struct drm_i915_private *dev_priv = engine->i915;
	(void)I915_READ(RING_ACTHD(engine->mmio_base));
}

static void
gen6_irq_put(struct intel_engine_cs *engine)
{
	struct drm_i915_private *dev_priv = engine->i915;
	unsigned long flags;

	gen6_gt_force_wake_put(dev_priv, engine->power_domains);

	spin_lock_irqsave(&dev_priv->irq_lock, flags);
	if (--engine->irq_refcount == 0) {
		I915_WRITE_IMR(engine, ~engine->irq_keep_mask);
		gen5_disable_gt_irq(dev_priv, engine->irq_enable_mask);
	}
	spin_unlock_irqrestore(&dev_priv->irq_lock, flags);
}

static void
hsw_vebox_irq_get(struct intel_engine_cs *engine)
{
	struct drm_i915_private *dev_priv = engine->i915;
	unsigned long flags;

	spin_lock_irqsave(&dev_priv->irq_lock, flags);
	if (engine->irq_refcount++ == 0) {
		I915_WRITE_IMR(engine,
			       ~(engine->irq_enable_mask |
				 engine->irq_keep_mask));
		gen6_enable_pm_irq(dev_priv, engine->irq_enable_mask);
	}
	spin_unlock_irqrestore(&dev_priv->irq_lock, flags);

	gen6_gt_force_wake_get(dev_priv, engine->power_domains);
}

static void
hsw_vebox_irq_put(struct intel_engine_cs *engine)
{
	struct drm_i915_private *dev_priv = engine->i915;
	unsigned long flags;

	gen6_gt_force_wake_put(dev_priv, engine->power_domains);

	spin_lock_irqsave(&dev_priv->irq_lock, flags);
	if (--engine->irq_refcount == 0) {
		I915_WRITE_IMR(engine, ~engine->irq_keep_mask);
		gen6_disable_pm_irq(dev_priv, engine->irq_enable_mask);
	}
	spin_unlock_irqrestore(&dev_priv->irq_lock, flags);
}

static void
gen8_irq_get(struct intel_engine_cs *engine)
{
	struct drm_i915_private *dev_priv = engine->i915;
	unsigned long flags;

	spin_lock_irqsave(&dev_priv->irq_lock, flags);
	if (engine->irq_refcount++ == 0) {
		I915_WRITE_IMR(engine,
			       ~(engine->irq_enable_mask |
				 engine->irq_keep_mask));
		POSTING_READ(RING_IMR(engine->mmio_base));
	}
	spin_unlock_irqrestore(&dev_priv->irq_lock, flags);
}

static void
gen8_irq_put(struct intel_engine_cs *engine)
{
	struct drm_i915_private *dev_priv = engine->i915;
	unsigned long flags;

	spin_lock_irqsave(&dev_priv->irq_lock, flags);
	if (--engine->irq_refcount == 0) {
		I915_WRITE_IMR(engine, ~engine->irq_keep_mask);
		POSTING_READ(RING_IMR(engine->mmio_base));
	}
	spin_unlock_irqrestore(&dev_priv->irq_lock, flags);
}

static int
i965_emit_batchbuffer(struct i915_gem_request *rq,
		      u64 offset, u32 length,
		      unsigned flags)
{
	struct intel_ringbuffer *ring;

	ring = intel_ring_begin(rq, 2);
	if (IS_ERR(ring))
		return PTR_ERR(ring);

	intel_ring_emit(ring,
			MI_BATCH_BUFFER_START |
			MI_BATCH_GTT |
			(flags & I915_DISPATCH_SECURE ? 0 : MI_BATCH_NON_SECURE_I965));
	intel_ring_emit(ring, offset);
	intel_ring_advance(ring);

	return 0;
}

static int
i830_emit_batchbuffer(struct i915_gem_request *rq,
		      u64 offset, u32 len,
		      unsigned flags)
{
	u32 cs_offset = rq->engine->scratch.gtt_offset;
	struct intel_ringbuffer *ring;

	if (rq->batch->vm->dirty) {
		ring = intel_ring_begin(rq, 5);
		if (IS_ERR(ring))
			return PTR_ERR(ring);

		/* Evict the invalid PTE TLBs */
		intel_ring_emit(ring, COLOR_BLT_CMD | BLT_WRITE_RGBA);
		intel_ring_emit(ring, BLT_DEPTH_32 | BLT_ROP_COLOR_COPY | 4096);
		intel_ring_emit(ring, I830_TLB_ENTRIES << 16 | 4);
		intel_ring_emit(ring, cs_offset);
		intel_ring_emit(ring, 0xdeadbeef);
		intel_ring_advance(ring);
	}

	if ((flags & I915_DISPATCH_PINNED) == 0) {
		if (len > I830_BATCH_LIMIT)
			return -ENOSPC;

		ring = intel_ring_begin(rq, 6 + 1);
		if (IS_ERR(ring))
			return PTR_ERR(ring);

		/* Blit the batch (which has now all relocs applied) to the
		 * stable batch scratch bo area (so that the CS never
		 * stumbles over its tlb invalidation bug) ...
		 */
		intel_ring_emit(ring, SRC_COPY_BLT_CMD | BLT_WRITE_RGBA);
		intel_ring_emit(ring, BLT_DEPTH_32 | BLT_ROP_SRC_COPY | 4096);
		intel_ring_emit(ring, DIV_ROUND_UP(len, 4096) << 16 | 4096);
		intel_ring_emit(ring, cs_offset);
		intel_ring_emit(ring, 4096);
		intel_ring_emit(ring, offset);

		intel_ring_emit(ring, MI_FLUSH);
		intel_ring_advance(ring);

		/* ... and execute it. */
		offset = cs_offset;
	}

	ring = intel_ring_begin(rq, 3);
	if (IS_ERR(ring))
		return PTR_ERR(ring);

	intel_ring_emit(ring, MI_BATCH_BUFFER);
	intel_ring_emit(ring, offset | (flags & I915_DISPATCH_SECURE ? 0 : MI_BATCH_NON_SECURE));
	intel_ring_emit(ring, offset + len - 8);
	intel_ring_advance(ring);

	return 0;
}

static int
i915_emit_batchbuffer(struct i915_gem_request *rq,
		      u64 offset, u32 len,
		      unsigned flags)
{
	struct intel_ringbuffer *ring;

	ring = intel_ring_begin(rq, 2);
	if (IS_ERR(ring))
		return PTR_ERR(ring);

	intel_ring_emit(ring, MI_BATCH_BUFFER_START | MI_BATCH_GTT);
	intel_ring_emit(ring, offset | (flags & I915_DISPATCH_SECURE ? 0 : MI_BATCH_NON_SECURE));
	intel_ring_advance(ring);

	return 0;
}

static int setup_status_page(struct intel_engine_cs *engine)
{
	struct drm_i915_gem_object *obj;
	unsigned flags;
	int ret;

	obj = i915_gem_alloc_object(engine->i915->dev, 4096);
	if (obj == NULL) {
		DRM_ERROR("Failed to allocate status page\n");
		return -ENOMEM;
	}

	ret = i915_gem_object_set_cache_level(obj, I915_CACHE_LLC);
	if (ret)
		goto err_unref;

	flags = 0;
	if (!HAS_LLC(engine->i915))
		/* On g33, we cannot place HWS above 256MiB, so
		 * restrict its pinning to the low mappable arena.
		 * Though this restriction is not documented for
		 * gen4, gen5, or byt, they also behave similarly
		 * and hang if the HWS is placed at the top of the
		 * GTT. To generalise, it appears that all !llc
		 * platforms have issues with us placing the HWS
		 * above the mappable region (even though we never
		 * actualy map it).
		 */
		flags |= PIN_MAPPABLE;
	ret = i915_gem_obj_ggtt_pin(obj, 4096, flags);
	if (ret) {
err_unref:
		drm_gem_object_unreference(&obj->base);
		return ret;
	}

	engine->status_page.obj = obj;

	engine->status_page.gfx_addr = i915_gem_obj_ggtt_offset(obj);
	engine->status_page.page_addr = kmap(sg_page(obj->pages->sgl));
	memset(engine->status_page.page_addr, 0, PAGE_SIZE);

	DRM_DEBUG_DRIVER("%s hws offset: 0x%08x\n",
			engine->name, engine->status_page.gfx_addr);
	return 0;
}

static int setup_phys_status_page(struct intel_engine_cs *engine)
{
	struct drm_i915_private *i915 = engine->i915;

	i915->status_page_dmah =
		drm_pci_alloc(i915->dev, PAGE_SIZE, PAGE_SIZE);
	if (!i915->status_page_dmah)
		return -ENOMEM;

	engine->status_page.page_addr = i915->status_page_dmah->vaddr;
	memset(engine->status_page.page_addr, 0, PAGE_SIZE);

	return 0;
}

void intel_ring_free(struct intel_ringbuffer *ring)
{
	if (ring->obj) {
		iounmap(ring->virtual_start);
		i915_gem_object_ggtt_unpin(ring->obj);
		drm_gem_object_unreference(&ring->obj->base);
	}

	list_del(&ring->engine_list);
	kfree(ring);
}

struct intel_ringbuffer *
intel_engine_alloc_ring(struct intel_engine_cs *engine,
			struct intel_context *ctx,
			int size)
{
	struct drm_i915_private *i915 = engine->i915;
	struct intel_ringbuffer *ring;
	struct drm_i915_gem_object *obj;
	int ret;

	DRM_DEBUG("creating ringbuffer for %s, size %d\n", engine->name, size);

	if (WARN_ON(!is_power_of_2(size)))
		return ERR_PTR(-EINVAL);

	ring = kzalloc(sizeof(*ring), GFP_KERNEL);
	if (ring == NULL)
		return ERR_PTR(-ENOMEM);

	ring->engine = engine;
	ring->ctx = ctx;

	obj = i915_gem_object_create_stolen(i915->dev, size);
	if (obj == NULL)
		obj = i915_gem_alloc_object(i915->dev, size);
	if (obj == NULL)
		return ERR_PTR(-ENOMEM);

	/* mark ring buffers as read-only from GPU side by default */
	obj->gt_ro = 1;

	ret = i915_gem_obj_ggtt_pin(obj, PAGE_SIZE, PIN_MAPPABLE);
	if (ret) {
		DRM_ERROR("failed pin ringbuffer into GGTT\n");
		goto err_unref;
	}

	ret = i915_gem_object_set_to_gtt_domain(obj, true);
	if (ret) {
		DRM_ERROR("failed mark ringbuffer for GTT writes\n");
		goto err_unpin;
	}

	ring->virtual_start =
		ioremap_wc(i915->gtt.mappable_base + i915_gem_obj_ggtt_offset(obj),
			   size);
	if (ring->virtual_start == NULL) {
		DRM_ERROR("failed to map ringbuffer through GTT\n");
		ret = -EINVAL;
		goto err_unpin;
	}

	ring->obj = obj;
	ring->size = size;

	/* Workaround an erratum on the i830 which causes a hang if
	 * the TAIL pointer points to within the last 2 cachelines
	 * of the buffer.
	 */
	ring->effective_size = size;
	if (IS_I830(i915) || IS_845G(i915))
		ring->effective_size -= 2 * CACHELINE_BYTES;

	ring->space = intel_ring_space(ring);
	ring->retired_head = -1;

	INIT_LIST_HEAD(&ring->requests);
	INIT_LIST_HEAD(&ring->breadcrumbs);
	list_add_tail(&ring->engine_list, &engine->rings);

	return ring;

err_unpin:
	i915_gem_object_ggtt_unpin(obj);
err_unref:
	drm_gem_object_unreference(&obj->base);
	return ERR_PTR(ret);
}

static void
nop_irq_barrier(struct intel_engine_cs *engine)
{
}

static int intel_engine_init(struct intel_engine_cs *engine,
			     struct drm_i915_private *i915)
{
	int ret;

	engine->i915 = i915;

	INIT_LIST_HEAD(&engine->rings);
	INIT_LIST_HEAD(&engine->vma_list);
	INIT_LIST_HEAD(&engine->read_list);
	INIT_LIST_HEAD(&engine->write_list);
	INIT_LIST_HEAD(&engine->requests);
	INIT_LIST_HEAD(&engine->pending);
	INIT_LIST_HEAD(&engine->submitted);

	spin_lock_init(&engine->lock);
	spin_lock_init(&engine->irqlock);

	engine->suspend = engine_suspend;
	engine->resume = engine_resume;
	engine->cleanup = engine_cleanup;

	engine->irq_barrier = nop_irq_barrier;
	engine->emit_breadcrumb = emit_breadcrumb;

	engine->power_domains = FORCEWAKE_ALL;
	engine->get_ring = engine_get_ring;
	engine->put_ring = engine_put_ring;

	engine->semaphore.wait = NULL;

	engine->add_request = engine_add_request;
	engine->write_tail = ring_write_tail;
	engine->is_complete = engine_rq_is_complete;

	init_waitqueue_head(&engine->irq_queue);

	if (I915_NEED_GFX_HWS(i915)) {
		ret = setup_status_page(engine);
	} else {
		BUG_ON(engine->id != RCS);
		ret = setup_phys_status_page(engine);
	}
	if (ret)
		return ret;

	ret = i915_cmd_parser_init_engine(engine);
	if (ret)
		return ret;

	return 0;
}

static void gen6_bsd_ring_write_tail(struct intel_engine_cs *engine,
				     u32 value)
{
	struct drm_i915_private *dev_priv = engine->i915;

       /* Every tail move must follow the sequence below */

	/* Disable notification that the ring is IDLE. The GT
	 * will then assume that it is busy and bring it out of rc6.
	 */
	I915_WRITE(GEN6_BSD_SLEEP_PSMI_CONTROL,
		   _MASKED_BIT_ENABLE(GEN6_BSD_SLEEP_MSG_DISABLE));

	/* Clear the context id. Here be magic! */
	I915_WRITE64(GEN6_BSD_RNCID, 0x0);

	/* Wait for the ring not to be idle, i.e. for it to wake up. */
	if (wait_for((I915_READ(GEN6_BSD_SLEEP_PSMI_CONTROL) &
		      GEN6_BSD_SLEEP_INDICATOR) == 0,
		     50))
		DRM_ERROR("timed out waiting for the BSD ring to wake up\n");

	/* Now that the ring is fully powered up, update the tail */
	I915_WRITE_TAIL(engine, value);
	POSTING_READ(RING_TAIL(engine->mmio_base));

	/* Let the ring send IDLE messages to the GT again,
	 * and so let it sleep to conserve power when idle.
	 */
	I915_WRITE(GEN6_BSD_SLEEP_PSMI_CONTROL,
		   _MASKED_BIT_DISABLE(GEN6_BSD_SLEEP_MSG_DISABLE));
}

static int gen6_bsd_emit_flush(struct i915_gem_request *rq,
			       u32 flags)
{
	struct intel_ringbuffer *ring;
	uint32_t cmd;

	cmd = 3;
	if (INTEL_INFO(rq->i915)->gen >= 8)
		cmd += 1;

	ring = intel_ring_begin(rq, cmd);
	if (IS_ERR(ring))
		return PTR_ERR(ring);

	/*
	 * Bspec vol 1c.5 - video engine command streamer:
	 * "If ENABLED, all TLBs will be invalidated once the flush
	 * operation is complete. This bit is only valid when the
	 * Post-Sync Operation field is a value of 1h or 3h."
	 */
	cmd = MI_FLUSH_DW | (cmd - 2);
	if (flags & I915_INVALIDATE_CACHES)
		cmd |= (MI_INVALIDATE_TLB |
			MI_INVALIDATE_BSD |
			MI_FLUSH_DW_STORE_INDEX |
			MI_FLUSH_DW_OP_STOREDW);
	intel_ring_emit(ring, cmd);
	intel_ring_emit(ring, I915_GEM_HWS_SCRATCH_ADDR | MI_FLUSH_DW_USE_GTT);
	if (INTEL_INFO(rq->i915)->gen >= 8)
		intel_ring_emit(ring, 0); /* upper addr */
	intel_ring_emit(ring, 0); /* value */
	intel_ring_advance(ring);
	return 0;
}

static int
gen8_emit_batchbuffer(struct i915_gem_request *rq,
		      u64 offset, u32 len,
		      unsigned flags)
{
	bool ppgtt = USES_PPGTT(ring->dev) && !(flags & I915_DISPATCH_SECURE);
	struct intel_ringbuffer *ring;

	ring = intel_ring_begin(rq, 3);
	if (IS_ERR(ring))
		return PTR_ERR(ring);

	/* FIXME(BDW): Address space and security selectors. */
	intel_ring_emit(ring, MI_BATCH_BUFFER_START_GEN8 | (ppgtt<<8));
	intel_ring_emit(ring, lower_32_bits(offset));
	intel_ring_emit(ring, upper_32_bits(offset));
	intel_ring_advance(ring);

	return 0;
}

static int
hsw_emit_batchbuffer(struct i915_gem_request *rq,
		     u64 offset, u32 len,
		     unsigned flags)
{
	struct intel_ringbuffer *ring;

	ring = intel_ring_begin(rq, 2);
	if (IS_ERR(ring))
		return PTR_ERR(ring);

	intel_ring_emit(ring,
			MI_BATCH_BUFFER_START |
			(flags & I915_DISPATCH_SECURE ?
			 0 : MI_BATCH_PPGTT_HSW | MI_BATCH_NON_SECURE_HSW));
	/* bit0-7 is the length on GEN6+ */
	intel_ring_emit(ring, offset);
	intel_ring_advance(ring);

	return 0;
}

static int
gen6_emit_batchbuffer(struct i915_gem_request *rq,
		      u64 offset, u32 len,
		      unsigned flags)
{
	struct intel_ringbuffer *ring;

	ring = intel_ring_begin(rq, 2);
	if (IS_ERR(ring))
		return PTR_ERR(ring);

	intel_ring_emit(ring,
			MI_BATCH_BUFFER_START |
			(flags & I915_DISPATCH_SECURE ? 0 : MI_BATCH_NON_SECURE_I965));
	/* bit0-7 is the length on GEN6+ */
	intel_ring_emit(ring, offset);
	intel_ring_advance(ring);

	return 0;
}

/* Blitter support (SandyBridge+) */

static int gen6_blt_emit_flush(struct i915_gem_request *rq,
			       u32 flags)
{
	struct intel_ringbuffer *ring;
	uint32_t cmd;

	cmd = 3;
	if (INTEL_INFO(rq->i915)->gen >= 8)
		cmd += 1;

	ring = intel_ring_begin(rq, cmd);
	if (IS_ERR(ring))
		return PTR_ERR(ring);

	/*
	 * Bspec vol 1c.3 - blitter engine command streamer:
	 * "If ENABLED, all TLBs will be invalidated once the flush
	 * operation is complete. This bit is only valid when the
	 * Post-Sync Operation field is a value of 1h or 3h."
	 */
	cmd = MI_FLUSH_DW | (cmd - 2);
	if (flags & I915_INVALIDATE_CACHES)
		cmd |= (MI_INVALIDATE_TLB |
			MI_FLUSH_DW_STORE_INDEX |
			MI_FLUSH_DW_OP_STOREDW);
	intel_ring_emit(ring, cmd);
	intel_ring_emit(ring, I915_GEM_HWS_SCRATCH_ADDR | MI_FLUSH_DW_USE_GTT);
	if (INTEL_INFO(rq->i915)->gen >= 8)
		intel_ring_emit(ring, 0); /* upper addr */
	intel_ring_emit(ring, 0); /* value */
	intel_ring_advance(ring);

	if (flags & I915_KICK_FBC) {
		if (IS_GEN7(rq->i915))
			return gen7_ring_fbc_flush(rq, FBC_REND_CACHE_CLEAN);
		if (IS_BROADWELL(rq->i915))
			rq->i915->fbc.need_sw_cache_clean = true; /* XXX */
	}

	return 0;
}

static void gen8_engine_init_semaphore(struct intel_engine_cs *engine)
{
	if (engine->i915->semaphore_obj == NULL)
		return;

	engine->semaphore.wait = gen8_emit_wait;
	engine->semaphore.signal =
		engine->id == RCS ? gen8_rcs_emit_signal : gen8_xcs_emit_signal;
}

static bool semaphores_enabled(struct drm_i915_private *dev_priv)
{
	if (INTEL_INFO(dev_priv)->gen < 6)
		return false;

	if (i915.semaphores >= 0)
		return i915.semaphores;

	/* Until we get further testing... */
	if (IS_GEN8(dev_priv))
		return false;

#ifdef CONFIG_INTEL_IOMMU
	/* Enable semaphores on SNB when IO remapping is off */
	if (INTEL_INFO(dev_priv)->gen == 6 && intel_iommu_gfx_mapped)
		return false;
#endif

	return true;
}

int intel_init_render_engine(struct drm_i915_private *dev_priv)
{
	struct intel_engine_cs *engine = &dev_priv->engine[RCS];
	struct drm_i915_gem_object *obj;
	int ret;

	ret = intel_engine_init(engine, dev_priv);
	if (ret)
		return ret;

	engine->name = "render ring";
	engine->id = RCS;
	engine->mmio_base = RENDER_RING_BASE;
	if (IS_VALLEYVIEW(dev_priv))
		engine->power_domains = FORCEWAKE_RENDER;

	engine->init_context = i915_gem_render_state_init;

	if (HAS_L3_DPF(dev_priv)) {
		if (INTEL_INFO(dev_priv)->gen >= 8)
			engine->irq_keep_mask |= GT_RENDER_L3_PARITY_ERROR_INTERRUPT;
		else
			engine->irq_keep_mask |= GT_PARITY_ERROR(dev_priv);
	}

	if (INTEL_INFO(dev_priv)->gen >= 8) {
		if (semaphores_enabled(dev_priv)) {
			obj = i915_gem_alloc_object(dev_priv->dev, 4096);
			if (obj == NULL) {
				DRM_ERROR("Failed to allocate semaphore bo. Disabling semaphores\n");
				i915.semaphores = 0;
			} else {
				i915_gem_object_set_cache_level(obj, I915_CACHE_LLC);
				ret = i915_gem_obj_ggtt_pin(obj, 0, PIN_NONBLOCK);
				if (ret != 0) {
					drm_gem_object_unreference(&obj->base);
					DRM_ERROR("Failed to pin semaphore bo. Disabling semaphores\n");
					i915.semaphores = 0;
				} else
					dev_priv->semaphore_obj = obj;
			}
		}
		if (IS_CHERRYVIEW(dev_priv))
			engine->init_context = chv_render_init_context;
		else
			engine->init_context = bdw_render_init_context;
		engine->emit_flush = gen8_render_emit_flush;
		engine->irq_get = gen8_irq_get;
		engine->irq_put = gen8_irq_put;
		engine->irq_enable_mask = GT_RENDER_USER_INTERRUPT;
		gen8_engine_init_semaphore(engine);
	} else if (INTEL_INFO(dev_priv)->gen >= 6) {
		engine->emit_flush = gen7_render_emit_flush;
		if (INTEL_INFO(dev_priv)->gen == 6)
			engine->emit_flush = gen6_render_emit_flush;
		engine->irq_get = gen6_irq_get;
		engine->irq_barrier = gen6_irq_barrier;
		engine->irq_put = gen6_irq_put;
		engine->irq_enable_mask = GT_RENDER_USER_INTERRUPT;
		if (semaphores_enabled(dev_priv)) {
			engine->semaphore.wait = gen6_emit_wait;
			engine->semaphore.signal = gen6_emit_signal;
			/*
			 * The current semaphore is only applied on pre-gen8
			 * platform.  And there is no VCS2 ring on the pre-gen8
			 * platform. So the semaphore between RCS and VCS2 is
			 * initialized as INVALID.  Gen8 will initialize the
			 * sema between VCS2 and RCS later.
			 */
			engine->semaphore.mbox.wait[RCS] = MI_SEMAPHORE_SYNC_INVALID;
			engine->semaphore.mbox.wait[VCS] = MI_SEMAPHORE_SYNC_RV;
			engine->semaphore.mbox.wait[BCS] = MI_SEMAPHORE_SYNC_RB;
			engine->semaphore.mbox.wait[VECS] = MI_SEMAPHORE_SYNC_RVE;
			engine->semaphore.mbox.wait[VCS2] = MI_SEMAPHORE_SYNC_INVALID;
			engine->semaphore.mbox.signal[RCS] = GEN6_NOSYNC;
			engine->semaphore.mbox.signal[VCS] = GEN6_VRSYNC;
			engine->semaphore.mbox.signal[BCS] = GEN6_BRSYNC;
			engine->semaphore.mbox.signal[VECS] = GEN6_VERSYNC;
			engine->semaphore.mbox.signal[VCS2] = GEN6_NOSYNC;
		}
	} else if (IS_GEN5(dev_priv)) {
		engine->emit_flush = gen4_emit_flush;
		engine->irq_get = gen5_irq_get;
		engine->irq_put = gen5_irq_put;
		engine->irq_enable_mask =
			GT_RENDER_USER_INTERRUPT |
			GT_RENDER_PIPECTL_NOTIFY_INTERRUPT;
	} else {
		if (INTEL_INFO(dev_priv)->gen < 4)
			engine->emit_flush = gen2_emit_flush;
		else
			engine->emit_flush = gen4_emit_flush;
		if (IS_GEN2(dev_priv)) {
			engine->irq_get = i8xx_irq_get;
			engine->irq_put = i8xx_irq_put;
		} else {
			engine->irq_get = i9xx_irq_get;
			engine->irq_put = i9xx_irq_put;
		}
		engine->irq_enable_mask = I915_USER_INTERRUPT;
	}

	if (IS_GEN8(dev_priv))
		engine->emit_batchbuffer = gen8_emit_batchbuffer;
	else if (IS_HASWELL(dev_priv))
		engine->emit_batchbuffer = hsw_emit_batchbuffer;
	else if (INTEL_INFO(dev_priv)->gen >= 6)
		engine->emit_batchbuffer = gen6_emit_batchbuffer;
	else if (INTEL_INFO(dev_priv)->gen >= 4)
		engine->emit_batchbuffer = i965_emit_batchbuffer;
	else if (IS_I830(dev_priv) || IS_845G(dev_priv))
		engine->emit_batchbuffer = i830_emit_batchbuffer;
	else
		engine->emit_batchbuffer = i915_emit_batchbuffer;

	engine->resume = render_resume;
	engine->cleanup = render_cleanup;

	if (HAS_BROKEN_CS_TLB(dev_priv))
		/* Workaround batchbuffer to combat CS tlb bug. */
		ret = init_broken_cs_tlb_wa(engine);
	else if (INTEL_INFO(dev_priv)->gen >= 6)
		ret = init_pipe_control(engine);
	if (ret)
		return ret;

	return intel_engine_enable_execlists(engine);
}

int intel_init_bsd_engine(struct drm_i915_private *dev_priv)
{
	struct intel_engine_cs *engine = &dev_priv->engine[VCS];
	int ret;

	ret = intel_engine_init(engine, dev_priv);
	if (ret)
		return ret;

	engine->name = "bsd ring";
	engine->id = VCS;
	if (IS_VALLEYVIEW(dev_priv))
		engine->power_domains = FORCEWAKE_MEDIA;

	if (INTEL_INFO(dev_priv)->gen >= 6) {
		engine->mmio_base = GEN6_BSD_RING_BASE;

		/* gen6 bsd needs a special wa for tail updates */
		if (IS_GEN6(dev_priv))
			engine->write_tail = gen6_bsd_ring_write_tail;
		engine->emit_flush = gen6_bsd_emit_flush;
		if (INTEL_INFO(dev_priv)->gen >= 8) {
			engine->irq_enable_mask =
				GT_RENDER_USER_INTERRUPT << GEN8_VCS1_IRQ_SHIFT;
			engine->irq_get = gen8_irq_get;
			engine->irq_put = gen8_irq_put;
			engine->emit_batchbuffer = gen8_emit_batchbuffer;
			gen8_engine_init_semaphore(engine);
		} else {
			engine->irq_enable_mask = GT_BSD_USER_INTERRUPT;
			engine->irq_get = gen6_irq_get;
			engine->irq_barrier = gen6_irq_barrier;
			engine->irq_put = gen6_irq_put;
			engine->emit_batchbuffer = gen6_emit_batchbuffer;
			if (semaphores_enabled(dev_priv)) {
				engine->semaphore.wait = gen6_emit_wait;
				engine->semaphore.signal = gen6_emit_signal;
				engine->semaphore.mbox.wait[RCS] = MI_SEMAPHORE_SYNC_VR;
				engine->semaphore.mbox.wait[VCS] = MI_SEMAPHORE_SYNC_INVALID;
				engine->semaphore.mbox.wait[BCS] = MI_SEMAPHORE_SYNC_VB;
				engine->semaphore.mbox.wait[VECS] = MI_SEMAPHORE_SYNC_VVE;
				engine->semaphore.mbox.wait[VCS2] = MI_SEMAPHORE_SYNC_INVALID;
				engine->semaphore.mbox.signal[RCS] = GEN6_RVSYNC;
				engine->semaphore.mbox.signal[VCS] = GEN6_NOSYNC;
				engine->semaphore.mbox.signal[BCS] = GEN6_BVSYNC;
				engine->semaphore.mbox.signal[VECS] = GEN6_VEVSYNC;
				engine->semaphore.mbox.signal[VCS2] = GEN6_NOSYNC;
			}
		}
	} else {
		engine->mmio_base = BSD_RING_BASE;

		engine->emit_flush = bsd_emit_flush;
		if (IS_GEN5(dev_priv)) {
			engine->irq_enable_mask = ILK_BSD_USER_INTERRUPT;
			engine->irq_get = gen5_irq_get;
			engine->irq_put = gen5_irq_put;
		} else {
			engine->irq_enable_mask = I915_BSD_USER_INTERRUPT;
			engine->irq_get = i9xx_irq_get;
			engine->irq_put = i9xx_irq_put;
		}
		engine->emit_batchbuffer = i965_emit_batchbuffer;
	}

	return intel_engine_enable_execlists(engine);
}

/**
 * Initialize the second BSD ring for Broadwell GT3.
 * It is noted that this only exists on Broadwell GT3.
 */
int intel_init_bsd2_engine(struct drm_i915_private *dev_priv)
{
	struct intel_engine_cs *engine = &dev_priv->engine[VCS2];
	int ret;

	if ((INTEL_INFO(dev_priv)->gen != 8)) {
		DRM_ERROR("No dual-BSD ring on non-BDW machine\n");
		return -EINVAL;
	}

	ret = intel_engine_init(engine, dev_priv);
	if (ret)
		return ret;

	engine->name = "bsd2 ring";
	engine->id = VCS2;
	engine->mmio_base = GEN8_BSD2_RING_BASE;
	if (IS_VALLEYVIEW(dev_priv))
		engine->power_domains = FORCEWAKE_MEDIA;

	engine->emit_flush = gen6_bsd_emit_flush;
	engine->emit_batchbuffer = gen8_emit_batchbuffer;
	engine->irq_enable_mask =
			GT_RENDER_USER_INTERRUPT << GEN8_VCS2_IRQ_SHIFT;
	engine->irq_get = gen8_irq_get;
	engine->irq_put = gen8_irq_put;
	gen8_engine_init_semaphore(engine);

	return intel_engine_enable_execlists(engine);
}

int intel_init_blt_engine(struct drm_i915_private *dev_priv)
{
	struct intel_engine_cs *engine = &dev_priv->engine[BCS];
	int ret;

	ret = intel_engine_init(engine, dev_priv);
	if (ret)
		return ret;

	engine->name = "blitter ring";
	engine->id = BCS;
	engine->mmio_base = BLT_RING_BASE;
	if (IS_VALLEYVIEW(dev_priv))
		engine->power_domains = FORCEWAKE_MEDIA;
	else if (IS_GEN9(dev_priv))
		engine->power_domains = FORCEWAKE_BLITTER;

	engine->emit_flush = gen6_blt_emit_flush;
	if (INTEL_INFO(dev_priv)->gen >= 8) {
		engine->irq_enable_mask =
			GT_RENDER_USER_INTERRUPT << GEN8_BCS_IRQ_SHIFT;
		engine->irq_get = gen8_irq_get;
		engine->irq_put = gen8_irq_put;
		engine->emit_batchbuffer = gen8_emit_batchbuffer;
		gen8_engine_init_semaphore(engine);
	} else {
		engine->irq_enable_mask = GT_BLT_USER_INTERRUPT;
		engine->irq_get = gen6_irq_get;
		engine->irq_barrier = gen6_irq_barrier;
		engine->irq_put = gen6_irq_put;
		engine->emit_batchbuffer = gen6_emit_batchbuffer;
		if (semaphores_enabled(dev_priv)) {
			engine->semaphore.signal = gen6_emit_signal;
			engine->semaphore.wait = gen6_emit_wait;
			/*
			 * The current semaphore is only applied on pre-gen8
			 * platform.  And there is no VCS2 ring on the pre-gen8
			 * platform. So the semaphore between BCS and VCS2 is
			 * initialized as INVALID.  Gen8 will initialize the
			 * sema between BCS and VCS2 later.
			 */
			engine->semaphore.mbox.wait[RCS] = MI_SEMAPHORE_SYNC_BR;
			engine->semaphore.mbox.wait[VCS] = MI_SEMAPHORE_SYNC_BV;
			engine->semaphore.mbox.wait[BCS] = MI_SEMAPHORE_SYNC_INVALID;
			engine->semaphore.mbox.wait[VECS] = MI_SEMAPHORE_SYNC_BVE;
			engine->semaphore.mbox.wait[VCS2] = MI_SEMAPHORE_SYNC_INVALID;
			engine->semaphore.mbox.signal[RCS] = GEN6_RBSYNC;
			engine->semaphore.mbox.signal[VCS] = GEN6_VBSYNC;
			engine->semaphore.mbox.signal[BCS] = GEN6_NOSYNC;
			engine->semaphore.mbox.signal[VECS] = GEN6_VEBSYNC;
			engine->semaphore.mbox.signal[VCS2] = GEN6_NOSYNC;
		}
	}

	return intel_engine_enable_execlists(engine);
}

int intel_init_vebox_engine(struct drm_i915_private *dev_priv)
{
	struct intel_engine_cs *engine = &dev_priv->engine[VECS];
	int ret;

	ret = intel_engine_init(engine, dev_priv);
	if (ret)
		return ret;

	engine->name = "video enhancement ring";
	engine->id = VECS;
	engine->mmio_base = VEBOX_RING_BASE;
	if (IS_VALLEYVIEW(dev_priv))
		engine->power_domains = FORCEWAKE_MEDIA;

	engine->emit_flush = gen6_blt_emit_flush;

	if (INTEL_INFO(dev_priv)->gen >= 8) {
		engine->irq_enable_mask =
			GT_RENDER_USER_INTERRUPT << GEN8_VECS_IRQ_SHIFT;
		engine->irq_get = gen8_irq_get;
		engine->irq_put = gen8_irq_put;
		engine->emit_batchbuffer = gen8_emit_batchbuffer;
		gen8_engine_init_semaphore(engine);
	} else {
		engine->irq_enable_mask = PM_VEBOX_USER_INTERRUPT;
		engine->irq_get = hsw_vebox_irq_get;
		engine->irq_barrier = gen6_irq_barrier;
		engine->irq_put = hsw_vebox_irq_put;
		engine->emit_batchbuffer = gen6_emit_batchbuffer;
		if (semaphores_enabled(dev_priv)) {
			engine->semaphore.wait = gen6_emit_wait;
			engine->semaphore.signal = gen6_emit_signal;
			engine->semaphore.mbox.wait[RCS] = MI_SEMAPHORE_SYNC_VER;
			engine->semaphore.mbox.wait[VCS] = MI_SEMAPHORE_SYNC_VEV;
			engine->semaphore.mbox.wait[BCS] = MI_SEMAPHORE_SYNC_VEB;
			engine->semaphore.mbox.wait[VECS] = MI_SEMAPHORE_SYNC_INVALID;
			engine->semaphore.mbox.wait[VCS2] = MI_SEMAPHORE_SYNC_INVALID;
			engine->semaphore.mbox.signal[RCS] = GEN6_RVESYNC;
			engine->semaphore.mbox.signal[VCS] = GEN6_VVESYNC;
			engine->semaphore.mbox.signal[BCS] = GEN6_BVESYNC;
			engine->semaphore.mbox.signal[VECS] = GEN6_NOSYNC;
			engine->semaphore.mbox.signal[VCS2] = GEN6_NOSYNC;
		}
	}

	return intel_engine_enable_execlists(engine);
}

int
intel_engine_flush(struct intel_engine_cs *engine,
		   struct intel_context *ctx)
{
	struct i915_gem_request *rq;
	int ret;

	rq = intel_engine_alloc_request(engine, ctx);
	if (IS_ERR(rq))
		return PTR_ERR(rq);

	ret = i915_request_emit_breadcrumb(rq);
	if (ret == 0)
		ret = i915_request_commit(rq);
	i915_request_put(rq);

	return ret;
}

int intel_engine_sync(struct intel_engine_cs *engine)
{
	/* Wait upon the last request to be completed */
	if (engine->last_request == NULL)
		return 0;

	return i915_request_wait(engine->last_request);
}

static u32
next_seqno(struct drm_i915_private *i915)
{
	/* reserve 0 for non-seqno */
	if (++i915->next_seqno == 0)
		++i915->next_seqno;
	return i915->next_seqno;
}

struct i915_gem_request *
intel_engine_alloc_request(struct intel_engine_cs *engine,
			   struct intel_context *ctx)
{
	struct intel_ringbuffer *ring;
	struct i915_gem_request *rq;
	int ret, n;

	ring = ctx->ring[engine->id].ring;
	if (ring == NULL) {
		ring = engine->get_ring(engine, ctx);
		if (IS_ERR(ring))
			return ERR_CAST(ring);

		ctx->ring[engine->id].ring = ring;
	}

	rq = kzalloc(sizeof(*rq), GFP_KERNEL);
	if (rq == NULL)
		return ERR_PTR(-ENOMEM);

	kref_init(&rq->kref);
	INIT_LIST_HEAD(&rq->vmas);
	INIT_LIST_HEAD(&rq->breadcrumb_link);

	rq->i915 = engine->i915;
	rq->ring = ring;
	rq->engine = engine;

	rq->reset_counter = atomic_read(&rq->i915->gpu_error.reset_counter);
	if (rq->reset_counter & (I915_RESET_IN_PROGRESS_FLAG | I915_WEDGED)) {
		ret = rq->reset_counter & I915_WEDGED ? -EIO : -EAGAIN;
		goto err;
	}

	rq->seqno = next_seqno(rq->i915);
	memcpy(rq->semaphore, engine->semaphore.sync, sizeof(rq->semaphore));
	for (n = 0; n < ARRAY_SIZE(rq->semaphore); n++)
		if (__i915_seqno_passed(rq->semaphore[n], rq->seqno))
			rq->semaphore[n] = 0;
	rq->head = ring->tail;
	rq->outstanding = true;
	rq->pending_flush = ring->pending_flush;

	rq->ctx = ctx;
	i915_gem_context_reference(rq->ctx);

	ret = i915_request_switch_context(rq);
	if (ret)
		goto err_ctx;

	return rq;

err_ctx:
	i915_gem_context_unreference(ctx);
err:
	kfree(rq);
	return ERR_PTR(ret);
}

struct i915_gem_request *
intel_engine_seqno_to_request(struct intel_engine_cs *engine,
			      u32 seqno)
{
	struct i915_gem_request *rq;

	list_for_each_entry(rq, &engine->requests, engine_list) {
		if (rq->seqno == seqno)
			return rq;

		if (__i915_seqno_passed(rq->seqno, seqno))
			break;
	}

	return NULL;
}

void intel_engine_cleanup(struct intel_engine_cs *engine)
{
	WARN_ON(engine->last_request);

	if (engine->cleanup)
		engine->cleanup(engine);
}

static void intel_engine_clear_rings(struct intel_engine_cs *engine)
{
	struct intel_ringbuffer *ring;

	list_for_each_entry(ring, &engine->rings, engine_list) {
		if (ring->retired_head != -1) {
			ring->head = ring->retired_head;
			ring->retired_head = -1;

			ring->space = intel_ring_space(ring);
		}

		if (ring->last_context != NULL) {
			struct drm_i915_gem_object *obj;

			obj = ring->last_context->ring[engine->id].state;
			if (obj)
				i915_gem_object_ggtt_unpin(obj);

			ring->last_context = NULL;
		}

		ring->pending_flush = 0;
	}
}

int intel_engine_suspend(struct intel_engine_cs *engine)
{
	struct drm_i915_private *dev_priv = engine->i915;
	int ret = 0;

	if (WARN_ON(!intel_engine_initialized(engine)))
		return 0;

	I915_WRITE_IMR(engine, ~0);

	if (engine->suspend)
		ret = engine->suspend(engine);

	intel_engine_clear_rings(engine);

	return ret;
}

int intel_engine_resume(struct intel_engine_cs *engine)
{
	struct drm_i915_private *dev_priv = engine->i915;
	int ret = 0;

	if (WARN_ON(!intel_engine_initialized(engine)))
		return 0;

	if (engine->resume)
		ret = engine->resume(engine);

	I915_WRITE_IMR(engine, ~engine->irq_keep_mask);
	return ret;
}

int intel_engine_retire(struct intel_engine_cs *engine,
			u32 seqno)
{
	int count;

	if (engine->retire)
		engine->retire(engine, seqno);

	count = 0;
	while (!list_empty(&engine->requests)) {
		struct i915_gem_request *rq;

		rq = list_first_entry(&engine->requests,
				      struct i915_gem_request,
				      engine_list);

		if (!__i915_seqno_passed(seqno, rq->seqno))
			break;

		i915_request_retire(rq);
		count++;
	}

	if (unlikely(engine->trace_irq_seqno &&
		     __i915_seqno_passed(seqno, engine->trace_irq_seqno))) {
		engine->irq_put(engine);
		engine->trace_irq_seqno = 0;
	}

	return count;
}

static struct i915_gem_request *
find_active_batch(struct list_head *list)
{
	struct i915_gem_request *rq, *last = NULL;

	list_for_each_entry(rq, list, engine_list) {
		if (rq->batch == NULL)
			continue;

		if (!__i915_request_complete__wa(rq))
			return rq;

		last = rq;
	}

	return last;
}

static bool context_is_banned(const struct intel_context *ctx,
			      unsigned long now)
{
	const struct i915_ctx_hang_stats *hs = &ctx->hang_stats;

	if (hs->banned)
		return true;

	if (hs->ban_period_seconds == 0)
		return false;

	if (now - hs->guilty_ts <= hs->ban_period_seconds) {
		if (!i915_gem_context_is_default(ctx)) {
			DRM_DEBUG("context hanging too fast, banning!\n");
			return true;
		} else if (i915_stop_ring_allow_ban(ctx->i915)) {
			if (i915_stop_ring_allow_warn(ctx->i915))
				DRM_ERROR("gpu hanging too fast, banning!\n");
			return true;
		}
	}

	return false;
}

static void
intel_engine_hangstats(struct intel_engine_cs *engine)
{
	struct i915_ctx_hang_stats *hs;
	struct i915_gem_request *rq;

	rq = find_active_batch(&engine->requests);
	if (rq == NULL)
		return;

	hs = &rq->ctx->hang_stats;
	if (engine->hangcheck.score >= HANGCHECK_SCORE_RING_HUNG) {
		unsigned long now = get_seconds();
		hs->banned = context_is_banned(rq->ctx, now);
		hs->guilty_ts = now;
		hs->batch_active++;
	} else
		hs->batch_pending++;

	list_for_each_entry_continue(rq, &engine->requests, engine_list) {
		if (rq->batch == NULL)
			continue;

		if (__i915_request_complete__wa(rq))
			continue;

		rq->ctx->hang_stats.batch_pending++;
	}
}

void intel_engine_reset(struct intel_engine_cs *engine)
{
	if (WARN_ON(!intel_engine_initialized(engine)))
		return;

	if (engine->reset)
		engine->reset(engine);

	memset(&engine->hangcheck, 0, sizeof(engine->hangcheck));
	intel_engine_hangstats(engine);

	intel_engine_retire(engine, engine->i915->next_seqno);
	intel_engine_clear_rings(engine);
}

static int ring_wait(struct intel_ringbuffer *ring, int n)
{
	int ret;

	trace_intel_ringbuffer_wait(ring, n);

	do {
		struct i915_gem_request *rq;

		i915_gem_retire_requests__engine(ring->engine);
		if (ring->retired_head != -1) {
			ring->head = ring->retired_head;
			ring->retired_head = -1;

			ring->space = intel_ring_space(ring);
			if (ring->space >= n)
				return 0;
		}

		list_for_each_entry(rq, &ring->breadcrumbs, breadcrumb_link)
			if (__intel_ring_space(rq->tail, ring->tail,
					       ring->size, I915_RING_RSVD) >= n)
				break;

		if (WARN_ON(&rq->breadcrumb_link == &ring->breadcrumbs))
			return -EDEADLK;

		ret = i915_request_wait(rq);
	} while (ret == 0);

	return ret;
}

static int ring_wrap(struct intel_ringbuffer *ring, int bytes)
{
	uint32_t __iomem *virt;
	int rem;

	rem = ring->size - ring->tail;
	if (unlikely(ring->space < rem)) {
		rem = ring_wait(ring, rem);
		if (rem)
			return rem;
	}

	trace_intel_ringbuffer_wrap(ring, rem);

	virt = ring->virtual_start + ring->tail;
	rem = ring->size - ring->tail;

	ring->space -= rem;
	ring->tail = 0;

	rem /= 4;
	while (rem--)
		iowrite32(MI_NOOP, virt++);

	return 0;
}

static int __intel_ring_prepare(struct intel_ringbuffer *ring,
				int bytes)
{
	int ret;

	trace_intel_ringbuffer_begin(ring, bytes);

	if (unlikely(ring->tail + bytes > ring->effective_size)) {
		ret = ring_wrap(ring, bytes);
		if (unlikely(ret))
			return ret;
	}

	if (unlikely(ring->space < bytes)) {
		ret = ring_wait(ring, bytes);
		if (unlikely(ret))
			return ret;
	}

	return 0;
}

struct intel_ringbuffer *
intel_ring_begin(struct i915_gem_request *rq,
		 int num_dwords)
{
	struct intel_ringbuffer *ring = rq->ring;
	int ret;

	/* TAIL updates must be aligned to a qword, so make sure we
	 * reserve space for any implicit padding required for this
	 * command.
	 */
	ret = __intel_ring_prepare(ring,
				   ALIGN(num_dwords, 2) * sizeof(uint32_t));
	if (ret)
		return ERR_PTR(ret);

	ring->space -= num_dwords * sizeof(uint32_t);

	return ring;
}

/* Align the ring tail to a cacheline boundary */
int intel_ring_cacheline_align(struct i915_gem_request *rq)
{
	struct intel_ringbuffer *ring;
	int tail, num_dwords;

	do {
		tail = rq->ring->tail;
		num_dwords = (tail & (CACHELINE_BYTES - 1)) / sizeof(uint32_t);
		if (num_dwords == 0)
			return 0;

		num_dwords = CACHELINE_BYTES / sizeof(uint32_t) - num_dwords;
		ring = intel_ring_begin(rq, num_dwords);
		if (IS_ERR(ring))
			return PTR_ERR(ring);
	} while (tail != rq->ring->tail);

	while (num_dwords--)
		intel_ring_emit(ring, MI_NOOP);

	intel_ring_advance(ring);

	return 0;
}

struct i915_gem_request *
intel_engine_find_active_batch(struct intel_engine_cs *engine)
{
	struct i915_gem_request *rq;
	unsigned long flags;

	spin_lock_irqsave(&engine->irqlock, flags);
	rq = find_active_batch(&engine->submitted);
	spin_unlock_irqrestore(&engine->irqlock, flags);
	if (rq)
		return rq;

	return find_active_batch(&engine->requests);
}
