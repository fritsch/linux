/*
 * Copyright © 2011-2012 Intel Corporation
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
 *
 */

/*
 * This file implements HW context support. On gen5+ a HW context consists of an
 * opaque GPU object which is referenced at times of context saves and restores.
 * With RC6 enabled, the context is also referenced as the GPU enters and exists
 * from RC6 (GPU has it's own internal power context, except on gen5). Though
 * something like a context does exist for the media ring, the code only
 * supports contexts for the render ring.
 *
 * In software, there is a distinction between contexts created by the user,
 * and the default HW context. The default HW context is used by GPU clients
 * that do not request setup of their own hardware context. The default
 * context's state is never restored to help prevent programming errors. This
 * would happen if a client ran and piggy-backed off another clients GPU state.
 * The default context only exists to give the GPU some offset to load as the
 * current to invoke a save of the context we actually care about. In fact, the
 * code could likely be constructed, albeit in a more complicated fashion, to
 * never use the default context, though that limits the driver's ability to
 * swap out, and/or destroy other contexts.
 *
 * All other contexts are created as a request by the GPU client. These contexts
 * store GPU state, and thus allow GPU clients to not re-emit state (and
 * potentially query certain state) at any time. The kernel driver makes
 * certain that the appropriate commands are inserted.
 *
 * The context life cycle is semi-complicated in that context BOs may live
 * longer than the context itself because of the way the hardware, and object
 * tracking works. Below is a very crude representation of the state machine
 * describing the context life.
 *                                         refcount     pincount     active
 * S0: initial state                          0            0           0
 * S1: context created                        1            0           0
 * S2: context is currently running           2            1           X
 * S3: GPU referenced, but not current        2            0           1
 * S4: context is current, but destroyed      1            1           0
 * S5: like S3, but destroyed                 1            0           1
 *
 * The most common (but not all) transitions:
 * S0->S1: client creates a context
 * S1->S2: client submits execbuf with context
 * S2->S3: other clients submits execbuf with context
 * S3->S1: context object was retired
 * S3->S2: clients submits another execbuf
 * S2->S4: context destroy called with current context
 * S3->S5->S0: destroy path
 * S4->S5->S0: destroy path on current context
 *
 * There are two confusing terms used above:
 *  The "current context" means the context which is currently running on the
 *  GPU. The GPU has loaded its state already and has stored away the gtt
 *  offset of the BO. The GPU is not actively referencing the data at this
 *  offset, but it will on the next context switch. The only way to avoid this
 *  is to do a GPU reset.
 *
 *  An "active context' is one which was previously the "current context" and is
 *  on the active list waiting for the next context switch to occur. Until this
 *  happens, the object must remain at the same gtt offset. It is therefore
 *  possible to destroy a context, but it is still active.
 *
 */

#include <drm/drmP.h>
#include <drm/i915_drm.h>
#include "i915_drv.h"
#include "i915_trace.h"

/* This is a HW constraint. The value below is the largest known requirement
 * I've seen in a spec to date, and that was a workaround for a non-shipping
 * part. It should be safe to decrease this, but it's more future proof as is.
 */
#define GEN6_CONTEXT_ALIGN (64<<10)
#define GEN7_CONTEXT_ALIGN 4096

static size_t get_context_alignment(struct drm_i915_private *i915)
{
	if (IS_GEN6(i915))
		return GEN6_CONTEXT_ALIGN;

	return GEN7_CONTEXT_ALIGN;
}

static int get_context_size(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	int ret;
	u32 reg;

	switch (INTEL_INFO(dev)->gen) {
	case 5:
		ret = ILK_CXT_TOTAL_SIZE;
		break;
	case 6:
		reg = I915_READ(CXT_SIZE);
		ret = GEN6_CXT_TOTAL_SIZE(reg) * 64;
		break;
	case 7:
		reg = I915_READ(GEN7_CXT_SIZE);
		if (IS_HASWELL(dev))
			ret = HSW_CXT_TOTAL_SIZE;
		else
			ret = GEN7_CXT_TOTAL_SIZE(reg) * 64;
		break;
	case 8:
		ret = GEN8_CXT_TOTAL_SIZE;
		break;
	default:
		BUG();
	}

	return ret;
}

void i915_gem_context_free(struct kref *ctx_ref)
{
	struct intel_context *ctx =
		container_of(ctx_ref, typeof(*ctx), ref);
	struct drm_i915_private *dev_priv = ctx->i915;
	int i;

	i915_vm_put(&ctx->ppgtt->base);

	trace_i915_context_free(ctx);
	for (i = 0; i < I915_NUM_ENGINES; i++) {
		if (intel_engine_initialized(&dev_priv->engine[i]) &&
		    ctx->ring[i].ring != NULL)
			dev_priv->engine[i].put_ring(ctx->ring[i].ring, ctx);

		if (ctx->ring[i].state != NULL)
			drm_gem_object_unreference(&ctx->ring[i].state->base);
	}

	list_del(&ctx->link);
	kfree(ctx);
}

struct drm_i915_gem_object *
i915_gem_alloc_context_obj(struct drm_device *dev, size_t size)
{
	struct drm_i915_gem_object *obj;
	int ret;

	obj = i915_gem_alloc_object(dev, size);
	if (obj == NULL)
		return ERR_PTR(-ENOMEM);

	/*
	 * Try to make the context utilize L3 as well as LLC.
	 *
	 * On VLV we don't have L3 controls in the PTEs so we
	 * shouldn't touch the cache level, especially as that
	 * would make the object snooped which might have a
	 * negative performance impact.
	 */
	if (INTEL_INFO(dev)->gen >= 7 && !IS_VALLEYVIEW(dev)) {
		ret = i915_gem_object_set_cache_level(obj, I915_CACHE_L3_LLC);
		/* Failure shouldn't ever happen this early */
		if (WARN_ON(ret)) {
			drm_gem_object_unreference(&obj->base);
			return ERR_PTR(ret);
		}
	}

	return obj;
}

static struct intel_context *
i915_gem_create_context(struct drm_device *dev,
			struct drm_i915_file_private *file_priv)
{
	struct drm_i915_private *dev_priv = to_i915(dev);
	struct intel_context *ctx;
	int ret;

	BUG_ON(!mutex_is_locked(&dev->struct_mutex));

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (ctx == NULL)
		return ERR_PTR(-ENOMEM);

	kref_init(&ctx->ref);
	list_add_tail(&ctx->link, &dev_priv->context_list);
	ctx->i915 = dev_priv;

	if (dev_priv->hw_context_size) {
		struct drm_i915_gem_object *obj =
			i915_gem_alloc_context_obj(dev, dev_priv->hw_context_size);
		if (IS_ERR(obj)) {
			ret = PTR_ERR(obj);
			goto err;
		}
		ctx->ring[RCS].state = obj;
	}

	/* Default context will never have a file_priv */
	if (file_priv != NULL) {
		ret = idr_alloc(&file_priv->context_idr, ctx,
				DEFAULT_CONTEXT_HANDLE, 0, GFP_KERNEL);
		if (ret < 0)
			goto err;
	} else
		ret = DEFAULT_CONTEXT_HANDLE;

	ctx->file_priv = file_priv;
	ctx->user_handle = ret;
	/* NB: Mark all slices as needing a remap so that when the context first
	 * loads it will restore whatever remap state already exists. If there
	 * is no remap info, it will be a NOP. */
	ctx->remap_slice = (1 << NUM_L3_SLICES(dev)) - 1;

	ctx->hang_stats.ban_period_seconds = DRM_I915_CTX_BAN_PERIOD;

	if (USES_FULL_PPGTT(dev)) {
		struct i915_hw_ppgtt *ppgtt = i915_ppgtt_create(dev, file_priv);

		if (IS_ERR_OR_NULL(ppgtt)) {
			DRM_DEBUG_DRIVER("PPGTT setup failed (%ld)\n",
					 PTR_ERR(ppgtt));
			ret = PTR_ERR(ppgtt);
			goto err;
		}

		ctx->ppgtt = ppgtt;
	}

	trace_i915_context_create(ctx);

	return ctx;

err:
	i915_gem_context_unreference(ctx);
	return ERR_PTR(ret);
}

int i915_gem_context_init(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct intel_context *ctx;
	int i, ret;

	if (RCS_ENGINE(dev_priv)->execlists_enabled) {
		/* NB: intentionally left blank. We will allocate our own
		 * backing objects as we need them, thank you very much */
		dev_priv->hw_context_size = 0;
	} else if (HAS_HW_CONTEXTS(dev)) {
		dev_priv->hw_context_size = round_up(get_context_size(dev), 4096);
		if (dev_priv->hw_context_size > (1<<20)) {
			DRM_DEBUG_DRIVER("Disabling HW Contexts; invalid size %d\n",
					 dev_priv->hw_context_size);
			dev_priv->hw_context_size = 0;
		}
	}

	/**
	 * The default context needs to exist per ring that uses contexts.
	 * It stores the context state of the GPU for applications that don't
	 * utilize HW contexts or per-process VM, as well as an idle case.
	 */
	ctx = i915_gem_create_context(dev, NULL);
	if (IS_ERR(ctx)) {
		DRM_ERROR("Failed to create default global context (error %ld)\n",
			  PTR_ERR(ctx));
		return PTR_ERR(ctx);
	}

	if (dev_priv->hw_context_size) {
		/* We may need to do things with the shrinker which
		 * require us to immediately switch back to the default
		 * context. This can cause a problem as pinning the
		 * default context also requires GTT space which may not
		 * be available. To avoid this we always pin the default
		 * context.
		 */
		ret = i915_gem_obj_ggtt_pin(ctx->ring[RCS].state,
					    get_context_alignment(dev_priv), 0);
		if (ret) {
			DRM_ERROR("Failed to pin global default context\n");
			i915_gem_context_unreference(ctx);
			return ret;
		}
	}

	for (i = 0; i < I915_NUM_ENGINES; i++) {
		struct intel_engine_cs *engine = &dev_priv->engine[i];

		if (engine->i915 == NULL)
			continue;

		engine->default_context = ctx;
		i915_gem_context_reference(ctx);
	}

	dev_priv->default_context = ctx;

	DRM_DEBUG_DRIVER("%s context support initialized\n",
			 RCS_ENGINE(dev_priv)->execlists_enabled ? "LR" :
			 dev_priv->hw_context_size ? "HW" : "fake");
	return 0;
}

void i915_gem_context_fini(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct intel_engine_cs *engine;
	int i;

	if (dev_priv->hw_context_size)
		/* The only known way to stop the gpu from accessing the hw context is
		 * to reset it. Do this as the very last operation to avoid confusing
		 * other code, leading to spurious errors. */
		intel_gpu_reset(dev);

	for_each_engine(engine, dev_priv, i) {
		i915_gem_context_unreference(engine->default_context);
		engine->default_context = NULL;
	}

	if (dev_priv->default_context) {
		if (dev_priv->hw_context_size)
			i915_gem_object_ggtt_unpin(dev_priv->default_context->ring[RCS].state);
		i915_gem_context_unreference(dev_priv->default_context);
		dev_priv->default_context = NULL;
	}
}

int i915_gem_context_enable(struct drm_i915_private *dev_priv)
{
	struct intel_engine_cs *engine;
	int ret, i;

	for_each_engine(engine, dev_priv, i) {
		struct intel_context *ctx = engine->default_context;
		struct i915_gem_request *rq;

		if (HAS_L3_DPF(dev_priv))
			ctx->remap_slice = (1 << NUM_L3_SLICES(dev_priv)) - 1;

		rq = intel_engine_alloc_request(engine, ctx);
		if (IS_ERR(rq)) {
			ret = PTR_ERR(rq);
			goto err;
		}

		ret = 0;
		/*
		 * Workarounds applied in this fn are part of register state context,
		 * they need to be re-initialized followed by gpu reset, suspend/resume,
		 * module reload.
		 */
		if (engine->init_context)
			ret = engine->init_context(rq);
		if (ret == 0)
			ret = i915_request_commit(rq);
		i915_request_put(rq);
		if (ret) {
err:
			DRM_ERROR("failed to enabled contexts (%s): %d\n", engine->name, ret);
			return ret;
		}
	}

	return 0;
}

static int context_idr_cleanup(int id, void *p, void *data)
{
	struct intel_context *ctx = p;

	ctx->file_priv = NULL;
	i915_gem_context_unreference(ctx);

	return 0;
}

int i915_gem_context_open(struct drm_device *dev, struct drm_file *file)
{
	struct drm_i915_file_private *file_priv = file->driver_priv;
	struct intel_context *ctx;

	idr_init(&file_priv->context_idr);

	mutex_lock(&dev->struct_mutex);
	ctx = i915_gem_create_context(dev, file_priv);
	mutex_unlock(&dev->struct_mutex);

	if (IS_ERR(ctx)) {
		idr_destroy(&file_priv->context_idr);
		return PTR_ERR(ctx);
	}

	return 0;
}

void i915_gem_context_close(struct drm_device *dev, struct drm_file *file)
{
	struct drm_i915_file_private *file_priv = file->driver_priv;

	idr_for_each(&file_priv->context_idr, context_idr_cleanup, NULL);
	idr_destroy(&file_priv->context_idr);
}

struct intel_context *
i915_gem_context_get(struct drm_i915_file_private *file_priv, u32 id)
{
	struct intel_context *ctx;

	ctx = (struct intel_context *)idr_find(&file_priv->context_idr, id);
	if (!ctx)
		return ERR_PTR(-ENOENT);

	return ctx;
}

static inline int
mi_set_context(struct i915_gem_request *rq,
	       struct intel_engine_context *new_context,
	       u32 flags)
{
	struct intel_ringbuffer *ring;
	int len;

	/* w/a: If Flush TLB Invalidation Mode is enabled, driver must do a TLB
	 * invalidation prior to MI_SET_CONTEXT. On GEN6 we don't set the value
	 * explicitly, so we rely on the value at engine init, stored in
	 * itlb_before_ctx_switch.
	 */
	if (IS_GEN6(rq->i915))
		rq->pending_flush |= I915_INVALIDATE_CACHES;

	len = 3;
	switch (INTEL_INFO(rq->i915)->gen) {
	case 8:
	case 7:
	case 5: len += 2;
		break;
	}

	ring = intel_ring_begin(rq, len);
	if (IS_ERR(ring))
		return PTR_ERR(ring);

	switch (INTEL_INFO(rq->i915)->gen) {
	case 8:
	case 7:
		/* WaProgramMiArbOnOffAroundMiSetContext:ivb,vlv,hsw,bdw,chv */
		intel_ring_emit(ring, MI_ARB_ON_OFF | MI_ARB_DISABLE);
		break;
	case 5:
		intel_ring_emit(ring, MI_SUSPEND_FLUSH | MI_SUSPEND_FLUSH_EN);
		break;
	}

	intel_ring_emit(ring, MI_SET_CONTEXT);
	intel_ring_emit(ring,
			i915_gem_obj_ggtt_offset(new_context->state) |
			MI_MM_SPACE_GTT |
			flags);
	/*
	 * w/a: MI_SET_CONTEXT must always be followed by MI_NOOP
	 * WaMiSetContext_Hang:snb,ivb,vlv
	 */
	intel_ring_emit(ring, MI_NOOP);

	switch (INTEL_INFO(rq->i915)->gen) {
	case 8:
	case 7:
		intel_ring_emit(ring, MI_ARB_ON_OFF | MI_ARB_ENABLE);
		break;
	case 5:
		intel_ring_emit(ring, MI_SUSPEND_FLUSH);
		break;
	}

	intel_ring_advance(ring);

	rq->pending_flush &= ~I915_COMMAND_BARRIER;
	return 0;
}

static int l3_remap(struct i915_gem_request *rq, int slice)
{
	const u32 reg_base = GEN7_L3LOG_BASE + (slice * 0x200);
	const u32 *remap_info;
	struct intel_ringbuffer *ring;
	int i;

	remap_info = rq->i915->l3_parity.remap_info[slice];
	if (remap_info == NULL)
		return 0;

	ring = intel_ring_begin(rq, GEN7_L3LOG_SIZE / 4 * 3);
	if (IS_ERR(ring))
		return PTR_ERR(ring);

	/*
	 * Note: We do not worry about the concurrent register cacheline hang
	 * here because no other code should access these registers other than
	 * at initialization time.
	 */
	for (i = 0; i < GEN7_L3LOG_SIZE; i += 4) {
		intel_ring_emit(ring, MI_LOAD_REGISTER_IMM(1));
		intel_ring_emit(ring, reg_base + i);
		intel_ring_emit(ring, remap_info[i/4]);
	}

	intel_ring_advance(ring);
	return 0;
}

/**
 * i915_request_switch_context() - perform a GPU context switch.
 * @rq: request and ring/ctx for which we'll execute the context switch
 *
 * The context life cycle is simple. The context refcount is incremented and
 * decremented by 1 and create and destroy. If the context is in use by the GPU,
 * it will have a refoucnt > 1. This allows us to destroy the context abstract
 * object while letting the normal object tracking destroy the backing BO.
 */
int i915_request_switch_context(struct i915_gem_request *rq)
{
	struct intel_context *to = rq->ctx;
	struct intel_engine_context *ctx = &to->ring[rq->engine->id];
	struct intel_context *from;
	int ret, i;

	lockdep_assert_held(&rq->i915->dev->struct_mutex);

	if (rq->ring->last_context == to && !to->remap_slice)
		return 0;

	if (ctx->state != NULL) {
		/* Trying to pin first makes error handling easier. */
		ret = i915_gem_obj_ggtt_pin(ctx->state,
					    get_context_alignment(rq->i915), 0);
		if (ret)
			return ret;

		/*
		 * Clear this page out of any CPU caches for coherent swap-in/out. Note
		 * that thanks to write = false in this call and us not setting any gpu
		 * write domains when putting a context object onto the active list
		 * (when switching away from it), this won't block.
		 *
		 * XXX: We need a real interface to do this instead of trickery.
		 */
		ret = i915_gem_object_set_to_gtt_domain(ctx->state, false);
		if (ret)
			goto unpin_out;
	}

	/* With execlists enabled, the ring, vm and logical state are
	 * interwined and we do not need to explicitly load the mm or
	 * logical state as it is loaded along with the LRCA.
	 *
	 * But we still want to pin the state (for global usage tracking)
	 * whilst in use and reload the l3 mapping if it has changed.
	 */
	if (!rq->engine->execlists_enabled) {
		if (ctx->state != NULL) {
			u32 flags;

			flags = 0;
			if (!ctx->initialized || i915_gem_context_is_default(to))
				flags |= MI_RESTORE_INHIBIT;

			/* These flags are for resource streamer on HSW+ */
			if (!IS_HASWELL(rq->i915) && INTEL_INFO(rq->i915)->gen < 8) {
				if (ctx->initialized)
					flags |= MI_RESTORE_EXT_STATE_EN;
				flags |= MI_SAVE_EXT_STATE_EN;
			}

			trace_i915_gem_ring_switch_context(rq->engine, to, flags);
			ret = mi_set_context(rq, ctx, flags);
			if (ret)
				goto unpin_out;

			rq->pending_flush &= ~I915_COMMAND_BARRIER;
		}

		if (to->ppgtt) {
			trace_switch_mm(ring, to);
			ret = to->ppgtt->switch_mm(rq, to->ppgtt);
			if (ret)
				goto unpin_out;
		}
	}

	for (i = 0; i < MAX_L3_SLICES; i++) {
		if (!(to->remap_slice & (1 << i)))
			continue;

		/* If it failed, try again next round */
		if (l3_remap(rq, i) == 0)
			rq->remap_l3 |= 1 << i;
	}

	/*
	 * Pin can switch back to the default context if we end up calling into
	 * evict_everything - as a last ditch gtt defrag effort that also
	 * switches to the default context. Hence we need to reload from here.
	 */
	from = rq->ring->last_context;

	/* The backing object for the context is done after switching to the
	 * *next* context. Therefore we cannot retire the previous context until
	 * the next context has already started running. In fact, the below code
	 * is a bit suboptimal because the retiring can occur simply after the
	 * MI_SET_CONTEXT instead of when the next seqno has completed.
	 */
	if (from && from->ring[rq->engine->id].state) {
		struct drm_i915_gem_object *from_obj = from->ring[rq->engine->id].state;

		from_obj->base.pending_read_domains = I915_GEM_DOMAIN_INSTRUCTION;
		/* obj is kept alive until the next request by its active ref */
		drm_gem_object_reference(&from_obj->base);
		i915_request_add_vma(rq, i915_gem_obj_get_ggtt(from_obj), 0);

		/* As long as MI_SET_CONTEXT is serializing, ie. it flushes the
		 * whole damn pipeline, we don't need to explicitly mark the
		 * object dirty. The only exception is that the context must be
		 * correct in case the object gets swapped out. Ideally we'd be
		 * able to defer doing this until we know the object would be
		 * swapped, but there is no way to do that yet.
		 */
		from_obj->dirty = 1;
	}

	rq->has_ctx_switch = true;
	return 0;

unpin_out:
	if (ctx->state)
		i915_gem_object_ggtt_unpin(ctx->state);
	return ret;
}

/**
 * i915_request_switch_context__commit() - commit the context sitch
 * @rq: request for which we have executed the context switch
 */
void i915_request_switch_context__commit(struct i915_gem_request *rq)
{
	struct intel_context *ctx;

	lockdep_assert_held(&rq->i915->dev->struct_mutex);

	if (!rq->has_ctx_switch)
		return;

	ctx = rq->ring->last_context;
	if (ctx && ctx->ring[rq->engine->id].state)
		i915_gem_object_ggtt_unpin(ctx->ring[rq->engine->id].state);

	ctx = rq->ctx;
	ctx->remap_slice &= ~rq->remap_l3;
	ctx->ring[rq->engine->id].initialized = true;

	rq->has_ctx_switch = false;
}

/**
 * i915_request_switch_context__undo() - unwind the context sitch
 * @rq: request for which we have executed the context switch
 */
void i915_request_switch_context__undo(struct i915_gem_request *rq)
{
	lockdep_assert_held(&rq->i915->dev->struct_mutex);

	if (!rq->has_ctx_switch)
		return;

	if (rq->ctx->ring[rq->engine->id].state)
		i915_gem_object_ggtt_unpin(rq->ctx->ring[rq->engine->id].state);
}

static bool contexts_enabled(struct drm_i915_private *dev_priv)
{
	if (RCS_ENGINE(dev_priv)->execlists_enabled)
		return true;

	return dev_priv->hw_context_size;
}

int i915_gem_context_create_ioctl(struct drm_device *dev, void *data,
				  struct drm_file *file)
{
	struct drm_i915_gem_context_create *args = data;
	struct drm_i915_file_private *file_priv = file->driver_priv;
	struct intel_context *ctx;
	int ret;

	if (!contexts_enabled(to_i915(dev)))
		return -ENODEV;

	ret = i915_mutex_lock_interruptible(dev);
	if (ret)
		return ret;

	ctx = i915_gem_create_context(dev, file_priv);
	mutex_unlock(&dev->struct_mutex);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	args->ctx_id = ctx->user_handle;
	DRM_DEBUG_DRIVER("HW context %d created\n", args->ctx_id);

	return 0;
}

int i915_gem_context_destroy_ioctl(struct drm_device *dev, void *data,
				   struct drm_file *file)
{
	struct drm_i915_gem_context_destroy *args = data;
	struct drm_i915_file_private *file_priv = file->driver_priv;
	struct intel_context *ctx;
	int ret;

	if (args->ctx_id == DEFAULT_CONTEXT_HANDLE)
		return -ENOENT;

	ret = i915_mutex_lock_interruptible(dev);
	if (ret)
		return ret;

	ctx = i915_gem_context_get(file_priv, args->ctx_id);
	if (IS_ERR(ctx)) {
		mutex_unlock(&dev->struct_mutex);
		return PTR_ERR(ctx);
	}

	idr_remove(&ctx->file_priv->context_idr, ctx->user_handle);
	i915_gem_context_unreference(ctx);
	mutex_unlock(&dev->struct_mutex);

	DRM_DEBUG_DRIVER("HW context %d destroyed\n", args->ctx_id);
	return 0;
}

int i915_gem_context_getparam_ioctl(struct drm_device *dev, void *data,
				    struct drm_file *file)
{
	struct drm_i915_file_private *file_priv = file->driver_priv;
	struct drm_i915_gem_context_param *args = data;
	struct intel_context *ctx;
	int ret;

	ret = i915_mutex_lock_interruptible(dev);
	if (ret)
		return ret;

	ctx = i915_gem_context_get(file_priv, args->ctx_id);
	if (IS_ERR(ctx)) {
		mutex_unlock(&dev->struct_mutex);
		return PTR_ERR(ctx);
	}

	args->size = 0;
	switch (args->param) {
	case I915_CONTEXT_PARAM_BAN_PERIOD:
		args->value = ctx->hang_stats.ban_period_seconds;
		break;
	default:
		ret = -EINVAL;
		break;
	}
	mutex_unlock(&dev->struct_mutex);

	return ret;
}

int i915_gem_context_setparam_ioctl(struct drm_device *dev, void *data,
				    struct drm_file *file)
{
	struct drm_i915_file_private *file_priv = file->driver_priv;
	struct drm_i915_gem_context_param *args = data;
	struct intel_context *ctx;
	int ret;

	ret = i915_mutex_lock_interruptible(dev);
	if (ret)
		return ret;

	ctx = i915_gem_context_get(file_priv, args->ctx_id);
	if (IS_ERR(ctx)) {
		mutex_unlock(&dev->struct_mutex);
		return PTR_ERR(ctx);
	}

	switch (args->param) {
	case I915_CONTEXT_PARAM_BAN_PERIOD:
		if (args->size)
			ret = -EINVAL;
		else if (args->value < ctx->hang_stats.ban_period_seconds &&
			 !capable(CAP_SYS_ADMIN))
			ret = -EPERM;
		else
			ctx->hang_stats.ban_period_seconds = args->value;
		break;
	default:
		ret = -EINVAL;
		break;
	}
	mutex_unlock(&dev->struct_mutex);

	return ret;
}
