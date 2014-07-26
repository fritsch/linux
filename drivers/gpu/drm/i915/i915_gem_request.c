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
 */

#include <drm/drmP.h>
#include "i915_drv.h"
#include <drm/i915_drm.h>
#include "i915_trace.h"
#include "intel_drv.h"

static bool check_reset(struct i915_gem_request *rq)
{
	unsigned reset = atomic_read(&rq->i915->gpu_error.reset_counter);
	return likely(reset == rq->reset_counter);
}

void
i915_request_add_vma(struct i915_gem_request *rq,
		     struct i915_vma *vma,
		     unsigned fenced)
{
	struct drm_i915_gem_object *obj = vma->obj;
	u32 old_read = obj->base.read_domains;
	u32 old_write = obj->base.write_domain;

	lockdep_assert_held(&rq->i915->dev->struct_mutex);
	BUG_ON(!drm_mm_node_allocated(&vma->node));

	obj->base.write_domain = obj->base.pending_write_domain;
	if (obj->base.write_domain == 0)
		obj->base.pending_read_domains |= obj->base.read_domains;
	obj->base.read_domains = obj->base.pending_read_domains;

	obj->base.pending_read_domains = 0;
	obj->base.pending_write_domain = 0;

	trace_i915_gem_object_change_domain(obj, old_read, old_write);
	list_move_tail(&vma->exec_list, &rq->vmas);

	if (obj->base.read_domains) {
		vma->exec_read = 1;
		vma->exec_fence = fenced;
		vma->exec_write = !!(obj->base.write_domain & I915_GEM_GPU_DOMAINS);

		if (vma->exec_write) {
			rq->pending_flush |= I915_FLUSH_CACHES;
			intel_fb_obj_invalidate(obj, rq);
		}
	} else
		vma->exec_read = 0;

	/* update for the implicit flush after the rq */
	obj->base.write_domain &= ~I915_GEM_GPU_DOMAINS;
}

int
i915_request_emit_flush(struct i915_gem_request *rq,
			unsigned flags)
{
	struct intel_engine_cs *engine = rq->engine;
	int ret;

	lockdep_assert_held(&rq->i915->dev->struct_mutex);

	if ((flags & rq->pending_flush) == 0)
		return 0;

	trace_i915_gem_request_emit_flush(rq);
	ret = engine->emit_flush(rq, rq->pending_flush);
	if (ret)
		return ret;

	rq->pending_flush = 0;
	return 0;
}

int
__i915_request_emit_breadcrumb(struct i915_gem_request *rq, int id)
{
	struct intel_engine_cs *engine = rq->engine;
	u32 seqno;
	int ret;

	lockdep_assert_held(&rq->i915->dev->struct_mutex);

	if (rq->breadcrumb[id])
		return 0;

	if (rq->outstanding) {
		ret = i915_request_emit_flush(rq, I915_COMMAND_BARRIER);
		if (ret)
			return ret;

		trace_i915_gem_request_emit_breadcrumb(rq);
		if (id == engine->id)
			ret = engine->emit_breadcrumb(rq);
		else
			ret = engine->semaphore.signal(rq, id);
		if (ret)
			return ret;

		seqno = rq->seqno;
	} else if (engine->breadcrumb[id] == 0 ||
		   __i915_seqno_passed(rq->seqno, engine->breadcrumb[id])) {
		struct i915_gem_request *tmp;

		tmp = intel_engine_alloc_request(engine,
						 rq->ring->last_context);
		if (IS_ERR(tmp))
			return PTR_ERR(tmp);

		/* Masquerade as a continuation of the earlier request */
		tmp->reset_counter = rq->reset_counter;

		ret = __i915_request_emit_breadcrumb(tmp, id);
		if (ret == 0 && id != engine->id) {
			/* semaphores are unstable across a wrap */
			if (tmp->seqno < engine->breadcrumb[id])
				ret = i915_request_wait(tmp);
		}
		if (ret == 0)
			ret = i915_request_commit(tmp);

		i915_request_put(tmp);
		if (ret)
			return ret;

		seqno = tmp->seqno;
	} else
		seqno = engine->breadcrumb[id];

	rq->breadcrumb[id] = seqno;
	return 0;
}

int
i915_request_emit_batchbuffer(struct i915_gem_request *rq,
			      struct i915_vma *batch,
			      uint64_t start, uint32_t len,
			      unsigned flags)
{
	struct intel_engine_cs *engine = rq->engine;
	int ret;

	lockdep_assert_held(&rq->i915->dev->struct_mutex);

	trace_i915_gem_request_emit_batch(rq);
	rq->batch = batch;

	ret = engine->emit_batchbuffer(rq, start, len, flags);
	if (ret)
		return ret;

	/* We track the associated batch vma for debugging and error capture.
	 * Whilst this request exists, the batch obj will be on the active_list,
	 * and so will hold the active reference. Only when this request is
	 * retired will the the batch be moved onto the inactive_list and lose
	 * its active reference. Hence we do not need to explicitly hold
	 * another reference here.
	 */
	rq->pending_flush |= I915_COMMAND_BARRIER;
	return 0;
}

/* Track the batches submitted by clients for throttling */
static void
add_to_client(struct i915_gem_request *rq)
{
	struct drm_i915_file_private *file_priv = rq->ctx->file_priv;

	if (file_priv) {
		spin_lock(&file_priv->mm.lock);
		list_add_tail(&rq->client_list,
			      &file_priv->mm.request_list);
		rq->file_priv = file_priv;
		spin_unlock(&file_priv->mm.lock);
	}
}

static void
remove_from_client(struct i915_gem_request *rq)
{
	struct drm_i915_file_private *file_priv = rq->file_priv;

	if (!file_priv)
		return;

	spin_lock(&file_priv->mm.lock);
	if (rq->file_priv) {
		list_del(&rq->client_list);
		rq->file_priv = NULL;
	}
	spin_unlock(&file_priv->mm.lock);
}

/* Activity tracking on the object so that we can serialise CPU access to
 * the object's memory with the GPU.
 */
static void
add_to_obj(struct i915_gem_request *rq, struct i915_vma *vma)
{
	struct drm_i915_gem_object *obj = vma->obj;
	struct intel_engine_cs *engine = rq->engine;

	if (!vma->exec_read)
		return;

	if (vma->last_read[engine->id].request == NULL && vma->active++ == 0)
		i915_vma_get(vma);

	i915_request_put(vma->last_read[engine->id].request);
	vma->last_read[engine->id].request = i915_request_get(rq);

	list_move_tail(&vma->last_read[engine->id].engine_link,
		       &engine->vma_list);

	/* Add a reference if we're newly entering the active list. */
	if (obj->last_read[engine->id].request == NULL && obj->active++ == 0)
		drm_gem_object_reference(&obj->base);

	if (vma->exec_write) {
		obj->dirty = 1;
		i915_request_put(obj->last_write.request);
		obj->last_write.request = i915_request_get(rq);
		list_move_tail(&obj->last_write.engine_list,
			       &engine->write_list);

		if (obj->active > 1) {
			int i;

			for (i = 0; i < I915_NUM_ENGINES; i++) {
				if (obj->last_read[i].request == NULL)
					continue;

				list_del_init(&obj->last_read[i].engine_list);
				i915_request_put(obj->last_read[i].request);
				obj->last_read[i].request = NULL;
			}

			obj->active = 1;
		}
	}

	if (vma->exec_fence & VMA_IS_FENCED) {
		i915_request_put(obj->last_fence.request);
		obj->last_fence.request = i915_request_get(rq);
		list_move_tail(&obj->last_fence.engine_list,
			       &engine->fence_list);
		if (vma->exec_fence & VMA_HAS_FENCE)
			list_move_tail(&rq->i915->fence_regs[obj->fence_reg].lru_list,
					&rq->i915->mm.fence_list);
	}

	i915_request_put(obj->last_read[engine->id].request);
	obj->last_read[engine->id].request = i915_request_get(rq);
	list_move_tail(&obj->last_read[engine->id].engine_list,
		       &engine->read_list);

	list_move_tail(&vma->mm_list, &vma->vm->active_list);
	BUG_ON(obj->active == 0);
}

static void vma_free(struct i915_vma *vma)
{
	list_del_init(&vma->exec_list);
	__i915_vma_unreserve(vma);
	drm_gem_object_unreference(&vma->obj->base);
	i915_vma_put(vma);
}

static bool leave_breadcrumb(struct i915_gem_request *rq)
{
	if (rq->breadcrumb[rq->engine->id])
		return false;

	/* Auto-report HEAD every 4k to make sure that we can always wait on
	 * some available ring space in the future. This also caps the
	 * latency of future waits for missed breadcrumbs.
	 */
	if (__intel_ring_space(rq->ring->tail, rq->ring->breadcrumb_tail,
			       rq->ring->size, 0) >= PAGE_SIZE)
		return true;

	return false;
}

static bool simulated_hang(struct intel_engine_cs *engine)
{
	return test_and_clear_bit(engine->id,
				  &engine->i915->gpu_error.stop_rings);
}

int i915_request_commit(struct i915_gem_request *rq)
{
	int ret, n;

	lockdep_assert_held(&rq->i915->dev->struct_mutex);

	if (!rq->outstanding)
		return 0;

	if (rq->head == rq->ring->tail) {
		rq->completed = true;
		goto done;
	}

	if (simulated_hang(rq->engine))
		i915_handle_error(rq->i915->dev, true, "Simulated hang");

	if (!check_reset(rq))
		return rq->i915->mm.interruptible ? -EAGAIN : -EIO;

	if (leave_breadcrumb(rq)) {
		ret = i915_request_emit_breadcrumb(rq);
		if (ret)
			return ret;
	}

	/* TAIL must be aligned to a qword */
	if ((rq->ring->tail / sizeof (uint32_t)) & 1) {
		intel_ring_emit(rq->ring, MI_NOOP);
		intel_ring_advance(rq->ring);
	}
	rq->tail = rq->ring->tail;
	rq->emitted_jiffies = jiffies;

	intel_runtime_pm_get(rq->i915);

	trace_i915_gem_request_commit(rq);
	ret = rq->engine->add_request(rq);
	if (ret) {
		intel_runtime_pm_put(rq->i915);
		return ret;
	}

	i915_request_get(rq);

	rq->outstanding = false;
	if (rq->breadcrumb[rq->engine->id]) {
		list_add_tail(&rq->breadcrumb_link, &rq->ring->breadcrumbs);
		rq->ring->breadcrumb_tail = rq->tail;
	}

	memcpy(rq->engine->semaphore.sync,
	       rq->semaphore,
	       sizeof(rq->semaphore));
	for (n = 0; n < ARRAY_SIZE(rq->breadcrumb); n++)
		if (rq->breadcrumb[n])
			rq->engine->breadcrumb[n] = rq->breadcrumb[n];

	rq->ring->pending_flush = rq->pending_flush;

	if (rq->batch) {
		add_to_client(rq);
		while (!list_empty(&rq->vmas)) {
			struct i915_vma *vma =
				list_first_entry(&rq->vmas,
						 typeof(*vma),
						 exec_list);

			add_to_obj(rq, vma);
			vma_free(vma);
		}
		rq->batch->vm->dirty = false;
	}

	i915_request_switch_context__commit(rq);

	rq->engine->last_request = rq;
done:
	rq->ring->last_context = rq->ctx;
	return 0;
}

static void fake_irq(unsigned long data)
{
	wake_up_process((struct task_struct *)data);
}

static bool missed_irq(struct i915_gem_request *rq)
{
	return test_bit(rq->engine->id, &rq->i915->gpu_error.missed_irq_rings);
}

bool __i915_request_complete__wa(struct i915_gem_request *rq)
{
	struct drm_i915_private *dev_priv = rq->i915;
	unsigned head, tail;

	if (i915_request_complete(rq))
		return true;

	/* With execlists, we rely on interrupts to track request completion */
	if (rq->engine->execlists_enabled)
		return false;

	/* As we may not emit a breadcrumb with every request, we
	 * often have unflushed requests. In the event of an emergency,
	 * just assume that if the RING_HEAD has reached the tail, then
	 * the request is complete. However, note that the RING_HEAD
	 * advances before the instruction completes, so this is quite lax,
	 * and should only be used carefully.
	 *
	 * As we treat this as only an advisory completion, we forgo
	 * marking the request as actually complete.
	 */
	head = __intel_ring_space(I915_READ_HEAD(rq->engine) & HEAD_ADDR,
				  rq->ring->tail, rq->ring->size, 0);
	tail = __intel_ring_space(rq->tail,
				  rq->ring->tail, rq->ring->size, 0);
	return head >= tail;
}

/**
 * __i915_request_wait - wait until execution of request has finished
 * @request: the request to wait upon
 * @interruptible: do an interruptible wait (normally yes)
 * @timeout_ns: in - how long to wait (NULL forever); out - how much time remaining
 *
 * Returns 0 if the request was completed within the alloted time. Else returns the
 * errno with remaining time filled in timeout argument.
 */
int __i915_request_wait(struct i915_gem_request *rq,
			bool interruptible,
			s64 *timeout_ns,
			struct drm_i915_file_private *file_priv)
{
	const bool irq_test_in_progress =
		ACCESS_ONCE(rq->i915->gpu_error.test_irq_rings) & intel_engine_flag(rq->engine);
	DEFINE_WAIT(wait);
	unsigned long timeout_expire;
	unsigned long before, now;
	int ret = 0;

	WARN(!intel_irqs_enabled(rq->i915), "IRQs disabled");

	if (i915_request_complete(rq))
		return 0;

	timeout_expire = timeout_ns ? jiffies + nsecs_to_jiffies((u64)*timeout_ns) : 0;

	if (rq->engine->id == RCS && INTEL_INFO(rq->i915)->gen >= 6)
		gen6_rps_boost(rq->i915, file_priv);

	if (!irq_test_in_progress) {
		if (WARN_ON(!intel_irqs_enabled(rq->i915)))
			return -ENODEV;

		rq->engine->irq_get(rq->engine);
	}

	/* Record current time in case interrupted by signal, or wedged */
	trace_i915_gem_request_wait_begin(rq);
	before = jiffies;
	for (;;) {
		struct timer_list timer;

		prepare_to_wait(&rq->engine->irq_queue, &wait,
				interruptible ? TASK_INTERRUPTIBLE : TASK_UNINTERRUPTIBLE);

		if (!check_reset(rq))
			break;

		rq->engine->irq_barrier(rq->engine);

		if (i915_request_complete(rq))
			break;

		if (timeout_ns && time_after_eq(jiffies, timeout_expire)) {
			ret = -ETIME;
			break;
		}

		if (interruptible && signal_pending(current)) {
			ret = -ERESTARTSYS;
			break;
		}

		/* Paranoid kick of hangcheck so that we never wait forever */
		i915_queue_hangcheck(rq->i915->dev);

		timer.function = NULL;
		if (timeout_ns || missed_irq(rq)) {
			unsigned long expire;

			setup_timer_on_stack(&timer, fake_irq, (unsigned long)current);
			expire = missed_irq(rq) ? jiffies + 1 : timeout_expire;
			mod_timer(&timer, expire);
		}

		io_schedule();

		if (timer.function) {
			del_singleshot_timer_sync(&timer);
			destroy_timer_on_stack(&timer);
		}
	}
	now = jiffies;
	trace_i915_gem_request_wait_end(rq);

	if (!irq_test_in_progress)
		rq->engine->irq_put(rq->engine);

	finish_wait(&rq->engine->irq_queue, &wait);

	if (timeout_ns) {
		s64 tres = *timeout_ns - jiffies_to_nsecs(now - before);
		*timeout_ns = tres <= 0 ? 0 : tres;
	}

	return ret;
}

struct i915_gem_request *
i915_request_get_breadcrumb(struct i915_gem_request *rq)
{
	struct list_head *list;
	u32 seqno;
	int ret;

	/* Writes are only coherent from the cpu (in the general case) when
	 * the interrupt following the write to memory is complete. That is
	 * when the breadcrumb after the write request is complete.
	 *
	 * Reads are only complete when then command streamer barrier is
	 * passed.
	 *
	 * In both cases, the CPU needs to wait upon the subsequent breadcrumb,
	 * which ensures that all pending flushes have been emitted and are
	 * complete, before reporting that the request is finished and
	 * the CPU's view of memory is coherent with the GPU.
	 */

	ret = i915_request_emit_breadcrumb(rq);
	if (ret)
		return ERR_PTR(ret);

	ret = i915_request_commit(rq);
	if (ret)
		return ERR_PTR(ret);

	if (!list_empty(&rq->breadcrumb_link))
		return i915_request_get(rq);

	seqno = rq->breadcrumb[rq->engine->id];
	list = &rq->ring->breadcrumbs;
	list_for_each_entry_reverse(rq, list, breadcrumb_link) {
		if (rq->seqno == seqno)
			return i915_request_get(rq);
	}

	return ERR_PTR(-EIO);
}

int
i915_request_wait(struct i915_gem_request *rq)
{
	int ret;

	lockdep_assert_held(&rq->i915->dev->struct_mutex);

	rq = i915_request_get_breadcrumb(rq);
	if (IS_ERR(rq))
		return PTR_ERR(rq);

	ret = __i915_request_wait(rq, rq->i915->mm.interruptible,
				  NULL, NULL);
	i915_request_put(rq);

	return ret;
}

void
i915_request_retire(struct i915_gem_request *rq)
{
	lockdep_assert_held(&rq->i915->dev->struct_mutex);

	if (!rq->completed) {
		trace_i915_gem_request_complete(rq);
		rq->completed = true;
	}
	trace_i915_gem_request_retire(rq);

	/* We know the GPU must have read the request to have
	 * sent us the seqno + interrupt, we can use the position
	 * of tail of the request to update the last known position
	 * of the GPU head.
	 */
	if (!list_empty(&rq->breadcrumb_link))
		rq->ring->retired_head = rq->tail;

	rq->batch = NULL;

	/* We need to protect against simultaneous hangcheck/capture */
	spin_lock(&rq->engine->lock);
	if (rq->engine->last_request == rq)
		rq->engine->last_request = NULL;
	list_del(&rq->engine_list);
	spin_unlock(&rq->engine->lock);

	list_del(&rq->breadcrumb_link);
	remove_from_client(rq);

	intel_runtime_pm_put(rq->i915);
	i915_request_put(rq);
}

void
__i915_request_free(struct kref *kref)
{
	struct i915_gem_request *rq = container_of(kref, struct i915_gem_request, kref);

	lockdep_assert_held(&rq->i915->dev->struct_mutex);

	if (rq->outstanding) {
		/* Rollback this partial transaction as we never committed
		 * the request to the hardware queue.
		 */
		rq->ring->tail = rq->head;
		rq->ring->space = intel_ring_space(rq->ring);
	}

	while (!list_empty(&rq->vmas))
		vma_free(list_first_entry(&rq->vmas,
					  struct i915_vma,
					  exec_list));

	i915_request_switch_context__undo(rq);
	i915_gem_context_unreference(rq->ctx);
	kfree(rq);
}
