#include <linux/perf_event.h>
#include <linux/pm_runtime.h>

#include "i915_drv.h"
#include "intel_ringbuffer.h"

#define FREQUENCY 200
#define PERIOD max_t(u64, 10000, NSEC_PER_SEC / FREQUENCY)

#define RING_MASK 0xffffffff
#define RING_MAX 32

#define INSTDONE_ENABLE 0x8

static bool gpu_active(struct drm_i915_private *i915)
{
	struct intel_engine_cs *engine;
	int i;

	if (!pm_runtime_active(&i915->dev->pdev->dev))
		return false;

	for_each_engine(engine, i915, i) {
		if (engine->last_request == NULL)
			continue;

		if (!i915_request_complete(engine->last_request))
			return true;
	}

	return false;
}

static void engines_sample(struct drm_i915_private *dev_priv)
{
	struct intel_engine_cs *engine;
	int i;

	if ((dev_priv->pmu.enable & RING_MASK) == 0)
		return;

	if (!gpu_active(dev_priv))
		return;

	if (dev_priv->info.gen >= 6)
		gen6_gt_force_wake_get(dev_priv, FORCEWAKE_ALL);

	for_each_engine(engine, dev_priv, i) {
		u32 head, tail, ctrl;

		if ((dev_priv->pmu.enable & (0x7 << (4*i))) == 0)
			continue;

		if (engine->last_request == NULL)
			continue;

		head = I915_READ_NOTRACE(RING_HEAD((engine)->mmio_base));
		tail = I915_READ_NOTRACE(RING_TAIL((engine)->mmio_base));
		ctrl = I915_READ_NOTRACE(RING_CTL((engine)->mmio_base));

		if ((head ^ tail) & HEAD_ADDR)
			engine->pmu_sample[I915_SAMPLE_BUSY] += PERIOD;

		if (ctrl & ((dev_priv->info.gen == 2) ? RING_WAIT_I8XX : RING_WAIT))
			engine->pmu_sample[I915_SAMPLE_WAIT] += PERIOD;

		if (ctrl & RING_WAIT_SEMAPHORE)
			engine->pmu_sample[I915_SAMPLE_SEMA] += PERIOD;
	}

	if (dev_priv->pmu.enable & INSTDONE_ENABLE) {
		u64 instdone;

		if (dev_priv->info.gen < 4) {
			instdone = I915_READ_NOTRACE(INSTDONE);
		} else if (dev_priv->info.gen < 7) {
			instdone  = I915_READ_NOTRACE(INSTDONE_I965);
			instdone |= (u64)I915_READ_NOTRACE(INSTDONE1) << 32;
		} else {
			instdone  = I915_READ_NOTRACE(GEN7_INSTDONE_1);
			instdone |= (u64)(I915_READ_NOTRACE(GEN7_SC_INSTDONE) & 0xff) << 32;
			instdone |= (u64)(I915_READ_NOTRACE(GEN7_SAMPLER_INSTDONE) & 0xff) << 40;
			instdone |= (u64)(I915_READ_NOTRACE(GEN7_ROW_INSTDONE) & 0xff) << 48;
		}

		for (instdone = ~instdone & dev_priv->pmu.instdone, i = 0;
		     instdone;
		     instdone >>= 1, i++) {
			if ((instdone & 1) == 0)
				continue;

			dev_priv->pmu.sample[__I915_SAMPLE_INSTDONE_0 + i] += PERIOD;
		}
	}

	if (dev_priv->info.gen >= 6)
		gen6_gt_force_wake_put(dev_priv, FORCEWAKE_ALL);
}

static void frequency_sample(struct drm_i915_private *dev_priv)
{
	if (dev_priv->pmu.enable & ((u64)1 << I915_PERF_ACTUAL_FREQUENCY)) {
		u64 val;

		if (gpu_active(dev_priv)) {
			if (dev_priv->info.is_valleyview) {
				mutex_lock(&dev_priv->rps.hw_lock);
				val = vlv_punit_read(dev_priv, PUNIT_REG_GPU_FREQ_STS);
				mutex_unlock(&dev_priv->rps.hw_lock);
				val = vlv_gpu_freq(dev_priv, (val >> 8) & 0xff);
			} else {
				val = I915_READ_NOTRACE(GEN6_RPSTAT1);
				if (dev_priv->info.is_haswell)
					val = (val & HSW_CAGF_MASK) >> HSW_CAGF_SHIFT;
				else
					val = (val & GEN6_CAGF_MASK) >> GEN6_CAGF_SHIFT;
				val *= GT_FREQUENCY_MULTIPLIER;
			}
		} else {
			val = dev_priv->rps.cur_freq; /* minor white lie to save power */
			if (dev_priv->info.is_valleyview)
				val = vlv_gpu_freq(dev_priv, val);
			else
				val *= GT_FREQUENCY_MULTIPLIER;
		}

		dev_priv->pmu.sample[__I915_SAMPLE_FREQ_ACT] += val * PERIOD;
	}

	if (dev_priv->pmu.enable & ((u64)1 << I915_PERF_REQUESTED_FREQUENCY)) {
		u64 val = dev_priv->rps.cur_freq;
		if (dev_priv->info.is_valleyview)
			val = vlv_gpu_freq(dev_priv, val);
		else
			val *= GT_FREQUENCY_MULTIPLIER;
		dev_priv->pmu.sample[__I915_SAMPLE_FREQ_REQ] += val * PERIOD;
	}
}

static enum hrtimer_restart i915_sample(struct hrtimer *hrtimer)
{
	struct drm_i915_private *i915 =
		container_of(hrtimer, struct drm_i915_private, pmu.timer);

	if (i915->pmu.enable == 0)
		return HRTIMER_NORESTART;

	engines_sample(i915);
	frequency_sample(i915);

	hrtimer_forward_now(hrtimer, ns_to_ktime(PERIOD));
	return HRTIMER_RESTART;
}

static void i915_perf_event_destroy(struct perf_event *event)
{
	WARN_ON(event->parent);
}

static int engine_event_init(struct perf_event *event)
{
	struct drm_i915_private *i915 =
		container_of(event->pmu, typeof(*i915), pmu.base);
	int engine = event->attr.config >> 2;
	int sample = event->attr.config & 3;

	switch (sample) {
	case I915_SAMPLE_BUSY:
	case I915_SAMPLE_WAIT:
		break;
	case I915_SAMPLE_SEMA:
		if (i915->info.gen < 6)
			return -ENODEV;
		break;
	default:
		return -ENOENT;
	}

	if (engine >= I915_NUM_ENGINES)
		return -ENOENT;

	if (!intel_engine_initialized(&i915->engine[engine]))
		return -ENODEV;

	return 0;
}

static enum hrtimer_restart hrtimer_sample(struct hrtimer *hrtimer)
{
	struct pt_regs *regs;
	struct perf_sample_data data;
	struct perf_event *event;
	u64 period;

	event = container_of(hrtimer, struct perf_event, hw.hrtimer);
	if (event->state != PERF_EVENT_STATE_ACTIVE)
		return HRTIMER_NORESTART;

	event->pmu->read(event);

	perf_sample_data_init(&data, 0, event->hw.last_period);
	regs = get_irq_regs();

	perf_event_overflow(event, &data, NULL);

	period = max_t(u64, 10000, event->hw.sample_period);
	hrtimer_forward_now(hrtimer, ns_to_ktime(period));
	return HRTIMER_RESTART;
}

static void init_hrtimer(struct perf_event *event)
{
	struct hw_perf_event *hwc = &event->hw;

	printk(KERN_ERR "%s %d, is-sampling-event? %d\n", __func__, (int)event->attr.config, is_sampling_event(event));

	if (!is_sampling_event(event))
		return;

	hrtimer_init(&hwc->hrtimer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	hwc->hrtimer.function = hrtimer_sample;

	if (event->attr.freq) {
		long freq = event->attr.sample_freq;

		event->attr.sample_period = NSEC_PER_SEC / freq;
		hwc->sample_period = event->attr.sample_period;
		local64_set(&hwc->period_left, hwc->sample_period);
		hwc->last_period = hwc->sample_period;
		event->attr.freq = 0;
	}
}

static int i915_perf_event_init(struct perf_event *event)
{
	struct drm_i915_private *i915 =
		container_of(event->pmu, typeof(*i915), pmu.base);
	int ret;

	/* XXX ideally only want pid == -1 && cpu == -1 */

	if (event->attr.type != event->pmu->type)
		return -ENOENT;

	if (has_branch_stack(event))
		return -EOPNOTSUPP;

	ret = 0;
	if (event->attr.config < RING_MAX) {
		ret = engine_event_init(event);
	} else switch (event->attr.config) {
	case I915_PERF_ACTUAL_FREQUENCY:
	case I915_PERF_REQUESTED_FREQUENCY:
	case I915_PERF_ENERGY:
	case I915_PERF_RC6_RESIDENCY:
	case I915_PERF_RC6p_RESIDENCY:
	case I915_PERF_RC6pp_RESIDENCY:
		if (i915->info.gen < 6)
			ret = -ENODEV;
		break;
	case I915_PERF_STATISTIC_0...I915_PERF_STATISTIC_8:
		if (i915->info.gen < 4)
			ret = -ENODEV;
		break;
	case I915_PERF_INSTDONE_0...I915_PERF_INSTDONE_63:
		if (i915->info.gen < 4 &&
		    event->attr.config - I915_PERF_INSTDONE_0 >= 32) {
			ret = -ENODEV;
			break;
		}
	}
	if (ret)
		return ret;

	if (!event->parent) {
		event->destroy = i915_perf_event_destroy;
	}

	init_hrtimer(event);

	return 0;
}

static inline bool is_instdone_event(struct perf_event *event)
{
	return (event->attr.config >= I915_PERF_INSTDONE_0 &&
		event->attr.config <= I915_PERF_INSTDONE_63);
}

static void i915_perf_timer_start(struct perf_event *event)
{
	struct hw_perf_event *hwc = &event->hw;
	s64 period;

	if (!is_sampling_event(event))
		return;

	period = local64_read(&hwc->period_left);
	if (period) {
		if (period < 0)
			period = 10000;

		local64_set(&hwc->period_left, 0);
	} else {
		period = max_t(u64, 10000, hwc->sample_period);
	}

	__hrtimer_start_range_ns(&hwc->hrtimer,
				 ns_to_ktime(period), 0,
				 HRTIMER_MODE_REL_PINNED, 0);
}

static void i915_perf_timer_cancel(struct perf_event *event)
{
	struct hw_perf_event *hwc = &event->hw;

	if (!is_sampling_event(event))
		return;

	local64_set(&hwc->period_left,
		    ktime_to_ns(hrtimer_get_remaining(&hwc->hrtimer)));
	hrtimer_cancel(&hwc->hrtimer);
}

static void i915_perf_enable(struct perf_event *event)
{
	struct drm_i915_private *i915 =
		container_of(event->pmu, typeof(*i915), pmu.base);
	u64 mask;

	if (i915->pmu.enable == 0)
		__hrtimer_start_range_ns(&i915->pmu.timer,
					 ns_to_ktime(PERIOD), 0,
					 HRTIMER_MODE_REL_PINNED, 0);

	if (is_instdone_event(event)) {
		i915->pmu.instdone |= (u64)1 << (event->attr.config - I915_PERF_INSTDONE_0);
		mask = INSTDONE_ENABLE;
	} else
		mask = (u64)1 << event->attr.config;

	i915->pmu.enable |= mask;

	i915_perf_timer_start(event);
}

static void i915_perf_disable(struct perf_event *event)
{
	struct drm_i915_private *i915 =
		container_of(event->pmu, typeof(*i915), pmu.base);
	u64 mask;

	if (is_instdone_event(event)) {
		i915->pmu.instdone &= ~((u64)1 << (event->attr.config - I915_PERF_INSTDONE_0));
		mask = i915->pmu.instdone == 0 ? INSTDONE_ENABLE : 0;
	} else
		mask = (u64)1 << event->attr.config;

	i915->pmu.enable &= ~mask;

	i915_perf_timer_cancel(event);
}

static int i915_perf_event_add(struct perf_event *event, int flags)
{
	struct hw_perf_event *hwc = &event->hw;

	printk(KERN_ERR "%s %d\n", __func__, (int)event->attr.config);

	if (flags & PERF_EF_START)
		i915_perf_enable(event);

	hwc->state = !(flags & PERF_EF_START);

	return 0;
}

static void i915_perf_event_del(struct perf_event *event, int flags)
{
	printk(KERN_ERR "%s %d\n", __func__, (int)event->attr.config);

	i915_perf_disable(event);
}

static void i915_perf_event_start(struct perf_event *event, int flags)
{
	printk(KERN_ERR "%s %d\n", __func__, (int)event->attr.config);

	i915_perf_enable(event);
}

static void i915_perf_event_stop(struct perf_event *event, int flags)
{
	printk(KERN_ERR "%s %d\n", __func__, (int)event->attr.config);

	i915_perf_disable(event);
}

static u64 read_energy_uJ(struct drm_i915_private *dev_priv)
{
	u64 power;
	u32 units;

	if (dev_priv->info.gen < 6)
		return 0;

	rdmsrl(MSR_RAPL_POWER_UNIT, power);
	power = (power & 0x1f00) >> 8;
	units = 1000000 / (1 << power); /* convert to uJ */
	power = I915_READ_NOTRACE(MCH_SECP_NRG_STTS);
	power *= units;

	return power;
}

static inline u64 calc_residency(struct drm_i915_private *dev_priv, const u32 reg)
{
	if (dev_priv->info.gen >= 6) {
		u64 val, units = 128, div = 100000;
		if (dev_priv->info.is_valleyview) {
			u32 clock;

			clock = I915_READ_NOTRACE(VLV_CLK_CTL2) >> CLK_CTL2_CZCOUNT_30NS_SHIFT;
			if (clock) {
				units = DIV_ROUND_UP(30 * 1000, clock);
				if (I915_READ_NOTRACE(VLV_COUNTER_CONTROL) & VLV_COUNT_RANGE_HIGH)
					units <<= 8;
			} else
				units = 0;

			div *= 1000;
		}
		val = I915_READ_NOTRACE(reg);
		val *= units;
		return DIV_ROUND_UP_ULL(val, div);
	} else
		return 0;
}

static inline u64 read_statistic(struct drm_i915_private *dev_priv,
				 const int statistic)
{
	const u32 reg = 0x2310 + 8 *statistic;
	u32 high, low;

	do {
		high = I915_READ_NOTRACE(reg + 4);
		low = I915_READ_NOTRACE(reg);
	} while (high != I915_READ_NOTRACE(reg + 4));

	return (u64)high << 32 | low;
}

static u64 count_interrupts(struct drm_i915_private *i915)
{
	/* open-coded kstat_irqs() */
	struct irq_desc *desc = irq_to_desc(i915->dev->pdev->irq);
	u64 sum = 0;
	int cpu;

	if (!desc || !desc->kstat_irqs)
		return 0;

	for_each_possible_cpu(cpu)
		sum += *per_cpu_ptr(desc->kstat_irqs, cpu);

	return sum;
}

static void i915_perf_event_read(struct perf_event *event)
{
	struct drm_i915_private *i915 =
		container_of(event->pmu, typeof(*i915), pmu.base);
	u64 val = 0;

	if (event->attr.config < 32) {
		int engine = event->attr.config >> 2;
		int sample = event->attr.config & 3;
		val = i915->engine[engine].pmu_sample[sample];
	} else switch (event->attr.config) {
	case I915_PERF_ACTUAL_FREQUENCY:
		val = i915->pmu.sample[__I915_SAMPLE_FREQ_ACT];
		break;
	case I915_PERF_REQUESTED_FREQUENCY:
		val = i915->pmu.sample[__I915_SAMPLE_FREQ_REQ];
		break;
	case I915_PERF_ENERGY:
		val = read_energy_uJ(i915);
		break;
	case I915_PERF_INTERRUPTS:
		val = count_interrupts(i915);
		break;

	case I915_PERF_RC6_RESIDENCY:
		if (!pm_runtime_active(&i915->dev->pdev->dev))
			return;

		val = calc_residency(i915, i915->info.is_valleyview ? VLV_GT_RENDER_RC6 : GEN6_GT_GFX_RC6);
		break;

	case I915_PERF_RC6p_RESIDENCY:
		if (!pm_runtime_active(&i915->dev->pdev->dev))
			return;

		if (!i915->info.is_valleyview)
			val = calc_residency(i915, GEN6_GT_GFX_RC6p);
		break;

	case I915_PERF_RC6pp_RESIDENCY:
		if (!pm_runtime_active(&i915->dev->pdev->dev))
			return;

		if (!i915->info.is_valleyview)
			val = calc_residency(i915, GEN6_GT_GFX_RC6pp);
		break;

	case I915_PERF_STATISTIC_0...I915_PERF_STATISTIC_8:
		if (!pm_runtime_active(&i915->dev->pdev->dev))
			return;

		val = read_statistic(i915, event->attr.config - I915_PERF_STATISTIC_0);
		break;

	case I915_PERF_INSTDONE_0...I915_PERF_INSTDONE_63:
		val = i915->pmu.sample[event->attr.config - I915_PERF_INSTDONE_0 + __I915_SAMPLE_INSTDONE_0];
		break;
	}

	local64_set(&event->count, val);
}

static int i915_perf_event_event_idx(struct perf_event *event)
{
	return 0;
}

void i915_perf_register(struct drm_device *dev)
{
	struct drm_i915_private *i915 = to_i915(dev);

	i915->pmu.base.task_ctx_nr	= perf_sw_context;
	i915->pmu.base.event_init	= i915_perf_event_init;
	i915->pmu.base.add		= i915_perf_event_add;
	i915->pmu.base.del		= i915_perf_event_del;
	i915->pmu.base.start		= i915_perf_event_start;
	i915->pmu.base.stop		= i915_perf_event_stop;
	i915->pmu.base.read		= i915_perf_event_read;
	i915->pmu.base.event_idx	= i915_perf_event_event_idx;

	hrtimer_init(&i915->pmu.timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	i915->pmu.timer.function = i915_sample;
	i915->pmu.enable = 0;

	if (perf_pmu_register(&i915->pmu.base, "i915", -1))
		i915->pmu.base.event_init = NULL;
}

void i915_perf_unregister(struct drm_device *dev)
{
	struct drm_i915_private *i915 = to_i915(dev);

	if (i915->pmu.base.event_init == NULL)
		return;

	perf_pmu_unregister(&i915->pmu.base);
	i915->pmu.base.event_init = NULL;
}
