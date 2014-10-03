#ifndef _INTEL_RINGBUFFER_H_
#define _INTEL_RINGBUFFER_H_

#include <linux/hashtable.h>

#define I915_CMD_HASH_ORDER 9

/* Early gen2 devices have a cacheline of just 32 bytes, using 64 is overkill,
 * but keeps the logic simple. Indeed, the whole purpose of this macro is just
 * to give some inclination as to some of the magic values used in the various
 * workarounds!
 */
#define CACHELINE_BYTES 64

/*
 * Gen2 BSpec "1. Programming Environment" / 1.4.4.6 "Ring Buffer Use"
 * Gen3 BSpec "vol1c Memory Interface Functions" / 2.3.4.5 "Ring Buffer Use"
 * Gen4+ BSpec "vol1c Memory Interface and Command Stream" / 5.3.4.5 "Ring Buffer Use"
 *
 * "If the Ring Buffer Head Pointer and the Tail Pointer are on the same
 * cacheline, the Head Pointer must not be greater than the Tail
 * Pointer."
 *
 * To also accommodate errata on 830/845 which makes the last pair of
 * cachelines in the ringbuffer unavailable, reduce the available space
 * further.
 */
#define I915_RING_RSVD (2*CACHELINE_BYTES)

struct intel_hw_status_page {
	u32		*page_addr;
	unsigned int	gfx_addr;
	struct		drm_i915_gem_object *obj;
};

#define I915_READ_TAIL(engine) I915_READ(RING_TAIL((engine)->mmio_base))
#define I915_WRITE_TAIL(engine, val) I915_WRITE(RING_TAIL((engine)->mmio_base), val)

#define I915_READ_START(engine) I915_READ(RING_START((engine)->mmio_base))
#define I915_WRITE_START(engine, val) I915_WRITE(RING_START((engine)->mmio_base), val)

#define I915_READ_HEAD(engine)  I915_READ(RING_HEAD((engine)->mmio_base))
#define I915_WRITE_HEAD(engine, val) I915_WRITE(RING_HEAD((engine)->mmio_base), val)

#define I915_READ_CTL(engine) I915_READ(RING_CTL((engine)->mmio_base))
#define I915_WRITE_CTL(engine, val) I915_WRITE(RING_CTL((engine)->mmio_base), val)

#define I915_READ_IMR(engine) I915_READ(RING_IMR((engine)->mmio_base))
#define I915_WRITE_IMR(engine, val) I915_WRITE(RING_IMR((engine)->mmio_base), val)

#define I915_READ_MODE(engine) I915_READ(RING_MI_MODE((engine)->mmio_base))
#define I915_WRITE_MODE(engine, val) I915_WRITE(RING_MI_MODE((engine)->mmio_base), val)

/* seqno size is actually only a uint32, but since we plan to use MI_FLUSH_DW to
 * do the writes, and that must have qw aligned offsets, simply pretend it's 8b.
 */
#define i915_semaphore_seqno_size sizeof(uint64_t)
#define GEN8_SEMAPHORE_OFFSET(__dp, __from, __to)			     \
	(i915_gem_obj_ggtt_offset((__dp)->semaphore_obj) + \
	 ((__from) * I915_NUM_ENGINES + (__to)) * i915_semaphore_seqno_size)

enum intel_engine_hangcheck_action {
	HANGCHECK_IDLE = 0,
	HANGCHECK_IDLE_WAITERS,
	HANGCHECK_WAIT,
	HANGCHECK_ACTIVE,
	HANGCHECK_ACTIVE_LOOP,
	HANGCHECK_KICK,
	HANGCHECK_HUNG,
};

#define HANGCHECK_SCORE_RING_HUNG 31

struct intel_engine_hangcheck {
	u64 acthd;
	u64 max_acthd;
	u32 seqno;
	u32 interrupts;
	int score;
	enum intel_engine_hangcheck_action action;
	int deadlock;
};

struct i915_gem_request;
struct intel_context;
struct intel_engine_cs;

struct intel_ringbuffer {
	struct intel_context *last_context;

	struct intel_engine_cs *engine;
	struct intel_context *ctx;
	struct list_head engine_link;

	struct drm_i915_gem_object *obj;
	void __iomem *virtual_start;

	/**
	 * List of breadcrumbs associated with GPU requests currently
	 * outstanding.
	 */
	struct list_head requests;
	struct list_head breadcrumbs;

	int head;
	int tail;
	int space;

	int size;
	int effective_size;

	/** We track the position of the requests in the ring buffer, and
	 * when each is retired we increment retired_head as the GPU
	 * must have finished processing the request and so we know we
	 * can advance the ringbuffer up to that position.
	 *
	 * retired_head is set to -1 after the value is consumed so
	 * we can detect new retirements.
	 */
	int retired_head;
	int breadcrumb_tail;

	unsigned pending_flush:4;
};

struct intel_engine_cs {
	struct drm_i915_private *i915;
	const char *name;
	enum intel_ring_id {
		RCS = 0x0,
		VCS,
		BCS,
		VECS,
		VCS2
	} id;
#define I915_NUM_ENGINES 5
#define I915_NUM_ENGINE_BITS 4
#define LAST_USER_RING (VECS + 1)
	u32 mmio_base;
	u32 power_domains;

	/* protects requests against hangcheck */
	spinlock_t lock;
	/* protects exlists: pending + submitted */
	spinlock_t irqlock;

	atomic_t interrupts;
	u32 breadcrumb[I915_NUM_ENGINES];
	u16 tag, next_tag;

	struct list_head rings;
	struct list_head requests;
	struct list_head pending, submitted;
	struct i915_gem_request *last_request;

	struct intel_hw_status_page status_page;

	struct intel_ringbuffer *legacy_ring;

	unsigned irq_refcount; /* protected by i915->irq_lock */
	u32		irq_enable_mask; /* bitmask to enable ring interrupt */
	u32             irq_keep_mask; /* never mask these interrupts */
	u32		trace_irq_seqno;
	void		(*irq_get)(struct intel_engine_cs *engine);
	void		(*irq_barrier)(struct intel_engine_cs *engine);
	void		(*irq_put)(struct intel_engine_cs *engine);

	struct intel_ringbuffer *
			(*get_ring)(struct intel_engine_cs *engine,
				    struct intel_context *ctx);
	void		(*put_ring)(struct intel_ringbuffer *ring,
				    struct intel_context *ctx);

	void		(*retire)(struct intel_engine_cs *engine,
				  u32 seqno);
	void		(*reset)(struct intel_engine_cs *engine);
	int		(*suspend)(struct intel_engine_cs *engine);
	int		(*resume)(struct intel_engine_cs *engine);
	void		(*cleanup)(struct intel_engine_cs *engine);

	int		(*init_context)(struct i915_gem_request *rq);

	int __must_check (*emit_flush)(struct i915_gem_request *rq,
				       u32 domains);
#define I915_COMMAND_BARRIER 0x1
#define I915_FLUSH_CACHES 0x2
#define I915_INVALIDATE_CACHES 0x4
#define I915_KICK_FBC 0x8
	int __must_check (*emit_batchbuffer)(struct i915_gem_request *rq,
					     u64 offset, u32 length,
					     unsigned flags);
#define I915_DISPATCH_SECURE 0x1
#define I915_DISPATCH_PINNED 0x2
	int __must_check (*emit_breadcrumb)(struct i915_gem_request *rq);

	int __must_check (*add_request)(struct i915_gem_request *rq);
	void		(*write_tail)(struct intel_engine_cs *engine,
				      u32 value);

	bool (*is_complete)(struct i915_gem_request *rq);


	/* GEN8 signal/wait table - never trust comments!
	 *	  signal to	signal to    signal to   signal to      signal to
	 *	    RCS		   VCS          BCS        VECS		 VCS2
	 *      --------------------------------------------------------------------
	 *  RCS | NOP (0x00) | VCS (0x08) | BCS (0x10) | VECS (0x18) | VCS2 (0x20) |
	 *	|-------------------------------------------------------------------
	 *  VCS | RCS (0x28) | NOP (0x30) | BCS (0x38) | VECS (0x40) | VCS2 (0x48) |
	 *	|-------------------------------------------------------------------
	 *  BCS | RCS (0x50) | VCS (0x58) | NOP (0x60) | VECS (0x68) | VCS2 (0x70) |
	 *	|-------------------------------------------------------------------
	 * VECS | RCS (0x78) | VCS (0x80) | BCS (0x88) |  NOP (0x90) | VCS2 (0x98) |
	 *	|-------------------------------------------------------------------
	 * VCS2 | RCS (0xa0) | VCS (0xa8) | BCS (0xb0) | VECS (0xb8) | NOP  (0xc0) |
	 *	|-------------------------------------------------------------------
	 *
	 * Generalization:
	 *  f(x, y) := (x->id * NUM_RINGS * seqno_size) + (seqno_size * y->id)
	 *  ie. transpose of g(x, y)
	 *
	 *	 sync from	sync from    sync from    sync from	sync from
	 *	    RCS		   VCS          BCS        VECS		 VCS2
	 *      --------------------------------------------------------------------
	 *  RCS | NOP (0x00) | VCS (0x28) | BCS (0x50) | VECS (0x78) | VCS2 (0xa0) |
	 *	|-------------------------------------------------------------------
	 *  VCS | RCS (0x08) | NOP (0x30) | BCS (0x58) | VECS (0x80) | VCS2 (0xa8) |
	 *	|-------------------------------------------------------------------
	 *  BCS | RCS (0x10) | VCS (0x38) | NOP (0x60) | VECS (0x88) | VCS2 (0xb0) |
	 *	|-------------------------------------------------------------------
	 * VECS | RCS (0x18) | VCS (0x40) | BCS (0x68) |  NOP (0x90) | VCS2 (0xb8) |
	 *	|-------------------------------------------------------------------
	 * VCS2 | RCS (0x20) | VCS (0x48) | BCS (0x70) | VECS (0x98) |  NOP (0xc0) |
	 *	|-------------------------------------------------------------------
	 *
	 * Generalization:
	 *  g(x, y) := (y->id * NUM_RINGS * seqno_size) + (seqno_size * x->id)
	 *  ie. transpose of f(x, y)
	 */
	struct {
		struct {
			/* our mbox written by others */
			u32		wait[I915_NUM_ENGINES];
			/* mboxes this ring signals to */
			u32		signal[I915_NUM_ENGINES];
		} mbox;

		int	(*wait)(struct i915_gem_request *waiter,
				struct i915_gem_request *signaller);
		int	(*signal)(struct i915_gem_request *rq, int id);

		u32 sync[I915_NUM_ENGINES];
	} semaphore;

	/* Execlists */
	bool execlists_enabled;
	u32 execlists_submitted;
	u8 next_context_status_buffer;

	/**
	 * List of objects currently involved in rendering from the
	 * ringbuffer.
	 *
	 * Includes buffers having the contents of their GPU caches
	 * flushed, not necessarily primitives.  last_rendering_seqno
	 * represents when the rendering involved will be completed.
	 *
	 * A reference is held on the buffer while on this list.
	 */
	struct list_head vma_list, read_list, write_list, fence_list;

	u64 pmu_sample[3];

	wait_queue_head_t irq_queue;

	struct intel_context *default_context;

	struct intel_engine_hangcheck hangcheck;

	struct {
		struct drm_i915_gem_object *obj;
		u32 gtt_offset;
	} scratch;

	bool needs_cmd_parser;

	/*
	 * Table of commands the command parser needs to know about
	 * for this ring.
	 */
	DECLARE_HASHTABLE(cmd_hash, I915_CMD_HASH_ORDER);

	/*
	 * Table of registers allowed in commands that read/write registers.
	 */
	const u32 *reg_table;
	int reg_count;

	/*
	 * Table of registers allowed in commands that read/write registers, but
	 * only from the DRM master.
	 */
	const u32 *master_reg_table;
	int master_reg_count;

	/*
	 * Returns the bitmask for the length field of the specified command.
	 * Return 0 for an unrecognized/invalid command.
	 *
	 * If the command parser finds an entry for a command in the ring's
	 * cmd_tables, it gets the command's length based on the table entry.
	 * If not, it calls this function to determine the per-ring length field
	 * encoding for the command (i.e. certain opcode ranges use certain bits
	 * to encode the command length in the header).
	 */
	u32 (*get_cmd_length_mask)(u32 cmd_header);
};

static inline bool
intel_engine_initialized(struct intel_engine_cs *engine)
{
	return engine->default_context;
}

static inline unsigned
intel_engine_flag(struct intel_engine_cs *engine)
{
	return 1 << engine->id;
}

static inline u32
intel_read_status_page(struct intel_engine_cs *engine,
		       int reg)
{
	/* Ensure that the compiler doesn't optimize away the load. */
	barrier();
	return engine->status_page.page_addr[reg];
}

static inline void
intel_write_status_page(struct intel_engine_cs *engine,
			int reg, u32 value)
{
	engine->status_page.page_addr[reg] = value;
}

/**
 * Reads a dword out of the status page, which is written to from the command
 * queue by automatic updates, MI_REPORT_HEAD, MI_STORE_DATA_INDEX, or
 * MI_STORE_DATA_IMM.
 *
 * The following dwords have a reserved meaning:
 * 0x00: ISR copy, updated when an ISR bit not set in the HWSTAM changes.
 * 0x04: ring 0 head pointer
 * 0x05: ring 1 head pointer (915-class)
 * 0x06: ring 2 head pointer (915-class)
 * 0x10-0x1b: Context status DWords (GM45)
 * 0x1f: Last written status offset. (GM45)
 *
 * The area from dword 0x20 to 0x3ff is available for driver usage.
 */
#define I915_GEM_HWS_INDEX		0x20
#define I915_GEM_HWS_SCRATCH_INDEX	0x30
#define I915_GEM_HWS_SCRATCH_ADDR (I915_GEM_HWS_SCRATCH_INDEX << MI_STORE_DWORD_INDEX_SHIFT)

static inline u32
intel_engine_get_seqno(struct intel_engine_cs *engine)
{
	return intel_read_status_page(engine, I915_GEM_HWS_INDEX);
}

struct intel_ringbuffer *
intel_engine_alloc_ring(struct intel_engine_cs *engine,
			struct intel_context *ctx,
			int size);
void intel_ring_free(struct intel_ringbuffer *ring);

struct intel_ringbuffer *__must_check
intel_ring_begin(struct i915_gem_request *rq, int n);
int __must_check intel_ring_cacheline_align(struct i915_gem_request *rq);
static inline void intel_ring_emit(struct intel_ringbuffer *ring,
				   u32 data)
{
	iowrite32(data, ring->virtual_start + ring->tail);
	ring->tail += 4;
}
static inline void intel_ring_advance(struct intel_ringbuffer *ring)
{
	ring->tail &= ring->size - 1;
}

static inline int __intel_ring_space(int head, int tail, int size, int rsvd)
{
	int space = head - (tail + 8);
	if (space < 0)
		space += size;
	return space - rsvd;
}

static inline int intel_ring_space(struct intel_ringbuffer *ring)
{
	return __intel_ring_space(ring->head, ring->tail,
				  ring->size, I915_RING_RSVD);
}


struct i915_gem_request * __must_check __attribute__((nonnull))
intel_engine_alloc_request(struct intel_engine_cs *engine,
			   struct intel_context *ctx);

struct i915_gem_request *
intel_engine_find_active_batch(struct intel_engine_cs *engine);

struct i915_gem_request *
intel_engine_seqno_to_request(struct intel_engine_cs *engine,
			      u32 seqno);

int intel_init_render_engine(struct drm_i915_private *i915);
int intel_init_bsd_engine(struct drm_i915_private *i915);
int intel_init_bsd2_engine(struct drm_i915_private *i915);
int intel_init_blt_engine(struct drm_i915_private *i915);
int intel_init_vebox_engine(struct drm_i915_private *i915);

int __must_check intel_engine_sync(struct intel_engine_cs *engine);
int __must_check intel_engine_flush(struct intel_engine_cs *engine,
				    struct intel_context *ctx);

int intel_engine_retire(struct intel_engine_cs *engine, u32 seqno);
void intel_engine_reset(struct intel_engine_cs *engine);
int intel_engine_suspend(struct intel_engine_cs *engine);
int intel_engine_resume(struct intel_engine_cs *engine);
void intel_engine_cleanup(struct intel_engine_cs *engine);


u64 intel_engine_get_active_head(struct intel_engine_cs *engine);

static inline void i915_trace_irq_get(struct intel_engine_cs *engine, u32 seqno)
{
	if (engine->trace_irq_seqno == 0)
		engine->irq_get(engine);

	engine->trace_irq_seqno = seqno;
}

#endif /* _INTEL_RINGBUFFER_H_ */
