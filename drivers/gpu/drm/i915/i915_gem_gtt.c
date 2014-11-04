/*
 * Copyright © 2010 Daniel Vetter
 * Copyright © 2011-2014 Intel Corporation
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

#include <linux/seq_file.h>
#include <drm/drmP.h>
#include <drm/i915_drm.h>
#include "i915_drv.h"
#include "i915_trace.h"
#include "intel_drv.h"

static void bdw_setup_private_ppat(struct drm_i915_private *dev_priv);
static void chv_setup_private_ppat(struct drm_i915_private *dev_priv);

static int sanitize_enable_ppgtt(struct drm_device *dev, int enable_ppgtt)
{
	bool has_aliasing_ppgtt;
	bool has_full_ppgtt;

	has_aliasing_ppgtt = INTEL_INFO(dev)->gen >= 6;
	has_full_ppgtt = INTEL_INFO(dev)->gen >= 7;
	if (IS_GEN8(dev))
		has_full_ppgtt = false; /* XXX why? */

	/*
	 * We don't allow disabling PPGTT for gen9+ as it's a requirement for
	 * execlists, the sole mechanism available to submit work.
	 */
	if (INTEL_INFO(dev)->gen < 9 &&
	    (enable_ppgtt == 0 || !has_aliasing_ppgtt))
		return 0;

	if (enable_ppgtt == 1)
		return 1;

	if (enable_ppgtt == 2 && has_full_ppgtt)
		return 2;

#ifdef CONFIG_INTEL_IOMMU
	/* Disable ppgtt on SNB if VT-d is on. */
	if (INTEL_INFO(dev)->gen == 6 && intel_iommu_gfx_mapped) {
		DRM_INFO("Disabling PPGTT because VT-d is on\n");
		return 0;
	}
#endif

	/* Early VLV doesn't have this */
	if (IS_VALLEYVIEW(dev) && !IS_CHERRYVIEW(dev) &&
	    dev->pdev->revision < 0xb) {
		DRM_DEBUG_DRIVER("disabling PPGTT on pre-B3 step VLV\n");
		return 0;
	}

	return has_aliasing_ppgtt ? 1 : 0;
}

static inline gen8_gtt_pte_t gen8_pte_encode(dma_addr_t addr,
					     enum i915_cache_level level,
					     bool valid)
{
	gen8_gtt_pte_t pte = valid ? _PAGE_PRESENT | _PAGE_RW : 0;
	pte |= addr;

	switch (level) {
	case I915_CACHE_NONE:
		pte |= PPAT_UNCACHED_INDEX;
		break;
	case I915_CACHE_WT:
		pte |= PPAT_DISPLAY_ELLC_INDEX;
		break;
	default:
		pte |= PPAT_CACHED_INDEX;
		break;
	}

	return pte;
}

static inline gen8_ppgtt_pde_t gen8_pde_encode(struct drm_device *dev,
					     dma_addr_t addr,
					     enum i915_cache_level level)
{
	gen8_ppgtt_pde_t pde = _PAGE_PRESENT | _PAGE_RW;
	pde |= addr;
	if (level != I915_CACHE_NONE)
		pde |= PPAT_CACHED_PDE_INDEX;
	else
		pde |= PPAT_UNCACHED_INDEX;
	return pde;
}

static gen6_gtt_pte_t snb_pte_encode(dma_addr_t addr,
				     enum i915_cache_level level,
				     bool valid, u32 unused)
{
	gen6_gtt_pte_t pte = valid ? GEN6_PTE_VALID : 0;
	pte |= GEN6_PTE_ADDR_ENCODE(addr);

	switch (level) {
	case I915_CACHE_L3_LLC:
	case I915_CACHE_LLC:
		pte |= GEN6_PTE_CACHE_LLC;
		break;
	case I915_CACHE_NONE:
		pte |= GEN6_PTE_UNCACHED;
		break;
	default:
		WARN_ON(1);
	}

	return pte;
}

static gen6_gtt_pte_t ivb_pte_encode(dma_addr_t addr,
				     enum i915_cache_level level,
				     bool valid, u32 unused)
{
	gen6_gtt_pte_t pte = valid ? GEN6_PTE_VALID : 0;
	pte |= GEN6_PTE_ADDR_ENCODE(addr);

	switch (level) {
	case I915_CACHE_L3_LLC:
		pte |= GEN7_PTE_CACHE_L3_LLC;
		break;
	case I915_CACHE_LLC:
		pte |= GEN6_PTE_CACHE_LLC;
		break;
	case I915_CACHE_NONE:
		pte |= GEN6_PTE_UNCACHED;
		break;
	default:
		WARN_ON(1);
	}

	return pte;
}

static gen6_gtt_pte_t byt_pte_encode(dma_addr_t addr,
				     enum i915_cache_level level,
				     bool valid, u32 flags)
{
	gen6_gtt_pte_t pte = valid ? GEN6_PTE_VALID : 0;
	pte |= GEN6_PTE_ADDR_ENCODE(addr);

	if (!(flags & PTE_READ_ONLY))
		pte |= BYT_PTE_WRITEABLE;

	if (level != I915_CACHE_NONE)
		pte |= BYT_PTE_SNOOPED_BY_CPU_CACHES;

	return pte;
}

static gen6_gtt_pte_t hsw_pte_encode(dma_addr_t addr,
				     enum i915_cache_level level,
				     bool valid, u32 unused)
{
	gen6_gtt_pte_t pte = valid ? GEN6_PTE_VALID : 0;
	pte |= HSW_PTE_ADDR_ENCODE(addr);

	if (level != I915_CACHE_NONE)
		pte |= HSW_WB_LLC_AGE3;

	return pte;
}

static gen6_gtt_pte_t iris_pte_encode(dma_addr_t addr,
				      enum i915_cache_level level,
				      bool valid, u32 unused)
{
	gen6_gtt_pte_t pte = valid ? GEN6_PTE_VALID : 0;
	pte |= HSW_PTE_ADDR_ENCODE(addr);

	switch (level) {
	case I915_CACHE_NONE:
		break;
	case I915_CACHE_WT:
		pte |= HSW_WT_ELLC_LLC_AGE3;
		break;
	default:
		pte |= HSW_WB_ELLC_LLC_AGE3;
		break;
	}

	return pte;
}

/* Broadwell Page Directory Pointer Descriptors */
static int gen8_write_pdp(struct i915_gem_request *rq, unsigned entry, uint64_t val)
{
	struct intel_ringbuffer *ring;

	BUG_ON(entry >= 4);

	ring = intel_ring_begin(rq, 5);
	if (IS_ERR(ring))
		return PTR_ERR(ring);

	intel_ring_emit(ring, MI_LOAD_REGISTER_IMM(2));
	intel_ring_emit(ring, GEN8_RING_PDP_UDW(rq->engine, entry));
	intel_ring_emit(ring, upper_32_bits(val));
	intel_ring_emit(ring, GEN8_RING_PDP_LDW(rq->engine, entry));
	intel_ring_emit(ring, lower_32_bits(val));
	intel_ring_advance(ring);

	return 0;
}

static int gen8_mm_switch(struct i915_gem_request *rq,
			  struct i915_hw_ppgtt *ppgtt)
{
	int i, ret;

	/* bit of a hack to find the actual last used pd */
	int used_pd = ppgtt->num_pd_entries / GEN8_PDES_PER_PAGE;

	for (i = used_pd - 1; i >= 0; i--) {
		dma_addr_t addr = ppgtt->pd_dma_addr[i];
		ret = gen8_write_pdp(rq, i, addr);
		if (ret)
			return ret;
	}

	return 0;
}

static int gen8_ppgtt_clear_range(struct i915_address_space *vm,
				  uint64_t start,
				  uint64_t length,
				  bool use_scratch)
{
	struct i915_hw_ppgtt *ppgtt =
		container_of(vm, struct i915_hw_ppgtt, base);
	gen8_gtt_pte_t *pt_vaddr, scratch_pte;
	unsigned pdpe = start >> GEN8_PDPE_SHIFT & GEN8_PDPE_MASK;
	unsigned pde = start >> GEN8_PDE_SHIFT & GEN8_PDE_MASK;
	unsigned pte = start >> GEN8_PTE_SHIFT & GEN8_PTE_MASK;
	unsigned num_entries = length >> PAGE_SHIFT;
	unsigned last_pte, i;

	scratch_pte = gen8_pte_encode(ppgtt->base.scratch.addr,
				      I915_CACHE_LLC, use_scratch);

	while (num_entries) {
		struct page *page_table = ppgtt->gen8_pt_pages[pdpe][pde];

		last_pte = pte + num_entries;
		if (last_pte > GEN8_PTES_PER_PAGE)
			last_pte = GEN8_PTES_PER_PAGE;

		pt_vaddr = kmap_atomic(page_table);

		for (i = pte; i < last_pte; i++) {
			pt_vaddr[i] = scratch_pte;
			num_entries--;
		}

		if (!HAS_LLC(ppgtt->base.dev))
			drm_clflush_virt_range(pt_vaddr, PAGE_SIZE);
		kunmap_atomic(pt_vaddr);

		pte = 0;
		if (++pde == GEN8_PDES_PER_PAGE) {
			pdpe++;
			pde = 0;
		}
	}

	return 0;
}

static int gen8_ppgtt_insert_entries(struct i915_address_space *vm,
				     struct sg_table *pages,
				     uint64_t start,
				     enum i915_cache_level cache_level, u32 unused)
{
	struct i915_hw_ppgtt *ppgtt =
		container_of(vm, struct i915_hw_ppgtt, base);
	gen8_gtt_pte_t *pt_vaddr;
	unsigned pdpe = start >> GEN8_PDPE_SHIFT & GEN8_PDPE_MASK;
	unsigned pde = start >> GEN8_PDE_SHIFT & GEN8_PDE_MASK;
	unsigned pte = start >> GEN8_PTE_SHIFT & GEN8_PTE_MASK;
	struct sg_page_iter sg_iter;

	pt_vaddr = NULL;

	for_each_sg_page(pages->sgl, &sg_iter, pages->nents, 0) {
		if (WARN_ON(pdpe >= GEN8_LEGACY_PDPS))
			break;

		if (pt_vaddr == NULL)
			pt_vaddr = kmap_atomic(ppgtt->gen8_pt_pages[pdpe][pde]);

		pt_vaddr[pte] =
			gen8_pte_encode(sg_page_iter_dma_address(&sg_iter),
					cache_level, true);
		if (++pte == GEN8_PTES_PER_PAGE) {
			if (!HAS_LLC(ppgtt->base.dev))
				drm_clflush_virt_range(pt_vaddr, PAGE_SIZE);
			kunmap_atomic(pt_vaddr);
			pt_vaddr = NULL;
			if (++pde == GEN8_PDES_PER_PAGE) {
				pdpe++;
				pde = 0;
			}
			pte = 0;
		}
	}
	if (pt_vaddr) {
		if (!HAS_LLC(ppgtt->base.dev))
			drm_clflush_virt_range(pt_vaddr, PAGE_SIZE);
		kunmap_atomic(pt_vaddr);
	}

	return 0;
}

static void gen8_free_page_tables(struct page **pt_pages)
{
	int i;

	if (pt_pages == NULL)
		return;

	for (i = 0; i < GEN8_PDES_PER_PAGE; i++)
		if (pt_pages[i])
			__free_pages(pt_pages[i], 0);
}

static void gen8_ppgtt_free(const struct i915_hw_ppgtt *ppgtt)
{
	int i;

	for (i = 0; i < ppgtt->num_pd_pages; i++) {
		gen8_free_page_tables(ppgtt->gen8_pt_pages[i]);
		kfree(ppgtt->gen8_pt_pages[i]);
		kfree(ppgtt->gen8_pt_dma_addr[i]);
	}

	__free_pages(ppgtt->pd_pages, get_order(ppgtt->num_pd_pages << PAGE_SHIFT));
}

static void gen8_ppgtt_unmap_pages(struct i915_hw_ppgtt *ppgtt)
{
	struct pci_dev *hwdev = ppgtt->base.dev->pdev;
	int i, j;

	for (i = 0; i < ppgtt->num_pd_pages; i++) {
		/* TODO: In the future we'll support sparse mappings, so this
		 * will have to change. */
		if (!ppgtt->pd_dma_addr[i])
			continue;

		pci_unmap_page(hwdev, ppgtt->pd_dma_addr[i], PAGE_SIZE,
			       PCI_DMA_BIDIRECTIONAL);

		for (j = 0; j < GEN8_PDES_PER_PAGE; j++) {
			dma_addr_t addr = ppgtt->gen8_pt_dma_addr[i][j];
			if (addr)
				pci_unmap_page(hwdev, addr, PAGE_SIZE,
					       PCI_DMA_BIDIRECTIONAL);
		}
	}
}

static void gen8_ppgtt_cleanup(struct i915_address_space *vm)
{
	struct i915_hw_ppgtt *ppgtt =
		container_of(vm, struct i915_hw_ppgtt, base);

	gen8_ppgtt_unmap_pages(ppgtt);
	gen8_ppgtt_free(ppgtt);
}

static struct page **__gen8_alloc_page_tables(void)
{
	struct page **pt_pages;
	int i;

	pt_pages = kcalloc(GEN8_PDES_PER_PAGE, sizeof(struct page *), GFP_KERNEL);
	if (!pt_pages)
		return ERR_PTR(-ENOMEM);

	for (i = 0; i < GEN8_PDES_PER_PAGE; i++) {
		pt_pages[i] = alloc_page(GFP_KERNEL);
		if (!pt_pages[i])
			goto bail;
	}

	return pt_pages;

bail:
	gen8_free_page_tables(pt_pages);
	kfree(pt_pages);
	return ERR_PTR(-ENOMEM);
}

static int gen8_ppgtt_allocate_page_tables(struct i915_hw_ppgtt *ppgtt,
					   const int max_pdp)
{
	struct page **pt_pages[GEN8_LEGACY_PDPS];
	int i, ret;

	for (i = 0; i < max_pdp; i++) {
		pt_pages[i] = __gen8_alloc_page_tables();
		if (IS_ERR(pt_pages[i])) {
			ret = PTR_ERR(pt_pages[i]);
			goto unwind_out;
		}
	}

	/* NB: Avoid touching gen8_pt_pages until last to keep the allocation,
	 * "atomic" - for cleanup purposes.
	 */
	for (i = 0; i < max_pdp; i++)
		ppgtt->gen8_pt_pages[i] = pt_pages[i];

	return 0;

unwind_out:
	while (i--) {
		gen8_free_page_tables(pt_pages[i]);
		kfree(pt_pages[i]);
	}

	return ret;
}

static int gen8_ppgtt_allocate_dma(struct i915_hw_ppgtt *ppgtt)
{
	int i;

	for (i = 0; i < ppgtt->num_pd_pages; i++) {
		ppgtt->gen8_pt_dma_addr[i] = kcalloc(GEN8_PDES_PER_PAGE,
						     sizeof(dma_addr_t),
						     GFP_KERNEL);
		if (!ppgtt->gen8_pt_dma_addr[i])
			return -ENOMEM;
	}

	return 0;
}

static int gen8_ppgtt_allocate_page_directories(struct i915_hw_ppgtt *ppgtt,
						const int max_pdp)
{
	ppgtt->pd_pages = alloc_pages(GFP_KERNEL, get_order(max_pdp << PAGE_SHIFT));
	if (!ppgtt->pd_pages)
		return -ENOMEM;

	ppgtt->num_pd_pages = 1 << get_order(max_pdp << PAGE_SHIFT);
	BUG_ON(ppgtt->num_pd_pages > GEN8_LEGACY_PDPS);

	return 0;
}

static int gen8_ppgtt_alloc(struct i915_hw_ppgtt *ppgtt,
			    const int max_pdp)
{
	int ret;

	ret = gen8_ppgtt_allocate_page_directories(ppgtt, max_pdp);
	if (ret)
		return ret;

	ret = gen8_ppgtt_allocate_page_tables(ppgtt, max_pdp);
	if (ret) {
		__free_pages(ppgtt->pd_pages, get_order(max_pdp << PAGE_SHIFT));
		return ret;
	}

	ppgtt->num_pd_entries = max_pdp * GEN8_PDES_PER_PAGE;

	ret = gen8_ppgtt_allocate_dma(ppgtt);
	if (ret)
		gen8_ppgtt_free(ppgtt);

	return ret;
}

static int gen8_ppgtt_setup_page_directories(struct i915_hw_ppgtt *ppgtt,
					     const int pd)
{
	dma_addr_t pd_addr;
	int ret;

	pd_addr = pci_map_page(ppgtt->base.dev->pdev,
			       &ppgtt->pd_pages[pd], 0,
			       PAGE_SIZE, PCI_DMA_BIDIRECTIONAL);

	ret = pci_dma_mapping_error(ppgtt->base.dev->pdev, pd_addr);
	if (ret)
		return ret;

	ppgtt->pd_dma_addr[pd] = pd_addr;

	return 0;
}

static int gen8_ppgtt_setup_page_tables(struct i915_hw_ppgtt *ppgtt,
					const int pd,
					const int pt)
{
	dma_addr_t pt_addr;
	struct page *p;
	int ret;

	p = ppgtt->gen8_pt_pages[pd][pt];
	pt_addr = pci_map_page(ppgtt->base.dev->pdev,
			       p, 0, PAGE_SIZE, PCI_DMA_BIDIRECTIONAL);
	ret = pci_dma_mapping_error(ppgtt->base.dev->pdev, pt_addr);
	if (ret)
		return ret;

	ppgtt->gen8_pt_dma_addr[pd][pt] = pt_addr;

	return 0;
}

/**
 * GEN8 legacy ppgtt programming is accomplished through a max 4 PDP registers
 * with a net effect resembling a 2-level page table in normal x86 terms. Each
 * PDP represents 1GB of memory 4 * 512 * 512 * 4096 = 4GB legacy 32b address
 * space.
 *
 * FIXME: split allocation into smaller pieces. For now we only ever do this
 * once, but with full PPGTT, the multiple contiguous allocations will be bad.
 * TODO: Do something with the size parameter
 */
static int gen8_ppgtt_init(struct i915_hw_ppgtt *ppgtt, uint64_t size)
{
	const int max_pdp = DIV_ROUND_UP(size, 1 << 30);
	const int min_pt_pages = GEN8_PDES_PER_PAGE * max_pdp;
	int i, j, ret;

	if (size % (1<<30))
		DRM_INFO("Pages will be wasted unless GTT size (%llu) is divisible by 1GB\n", size);

	/* 1. Do all our allocations for page directories and page tables. */
	ret = gen8_ppgtt_alloc(ppgtt, max_pdp);
	if (ret)
		return ret;

	/*
	 * 2. Create DMA mappings for the page directories and page tables.
	 */
	for (i = 0; i < max_pdp; i++) {
		ret = gen8_ppgtt_setup_page_directories(ppgtt, i);
		if (ret)
			goto bail;

		for (j = 0; j < GEN8_PDES_PER_PAGE; j++) {
			ret = gen8_ppgtt_setup_page_tables(ppgtt, i, j);
			if (ret)
				goto bail;
		}
	}

	/*
	 * 3. Map all the page directory entires to point to the page tables
	 * we've allocated.
	 *
	 * For now, the PPGTT helper functions all require that the PDEs are
	 * plugged in correctly. So we do that now/here. For aliasing PPGTT, we
	 * will never need to touch the PDEs again.
	 */
	for (i = 0; i < max_pdp; i++) {
		gen8_ppgtt_pde_t *pd_vaddr;
		pd_vaddr = kmap_atomic(&ppgtt->pd_pages[i]);
		for (j = 0; j < GEN8_PDES_PER_PAGE; j++) {
			dma_addr_t addr = ppgtt->gen8_pt_dma_addr[i][j];
			pd_vaddr[j] = gen8_pde_encode(ppgtt->base.dev, addr,
						      I915_CACHE_LLC);
		}
		if (!HAS_LLC(ppgtt->base.dev))
			drm_clflush_virt_range(pd_vaddr, PAGE_SIZE);
		kunmap_atomic(pd_vaddr);
	}

	ppgtt->switch_mm = gen8_mm_switch;
	ppgtt->base.clear_range = gen8_ppgtt_clear_range;
	ppgtt->base.insert_entries = gen8_ppgtt_insert_entries;
	ppgtt->base.cleanup = gen8_ppgtt_cleanup;
	ppgtt->base.start = 0;
	ppgtt->base.total = ppgtt->num_pd_entries * GEN8_PTES_PER_PAGE * PAGE_SIZE;

	ppgtt->base.clear_range(&ppgtt->base, 0, ppgtt->base.total, true);

	DRM_DEBUG_DRIVER("Allocated %d pages for page directories (%d wasted)\n",
			 ppgtt->num_pd_pages, ppgtt->num_pd_pages - max_pdp);
	DRM_DEBUG_DRIVER("Allocated %d pages for page tables (%lld wasted)\n",
			 ppgtt->num_pd_entries,
			 (ppgtt->num_pd_entries - min_pt_pages) + size % (1<<30));
	return 0;

bail:
	gen8_ppgtt_unmap_pages(ppgtt);
	gen8_ppgtt_free(ppgtt);
	return ret;
}

static void gen6_dump_ppgtt(struct i915_hw_ppgtt *ppgtt, struct seq_file *m)
{
	struct i915_address_space *vm = &ppgtt->base;
	gen6_gtt_pte_t scratch_pte;
	int pte, pde;

	if (ppgtt->state->pages == NULL)
		return;

	scratch_pte = vm->pte_encode(vm->scratch.addr, I915_CACHE_LLC, true, 0);

	for (pde = 0; pde < ppgtt->num_pd_entries; pde++) {
		gen6_gtt_pte_t *pt_vaddr;

		pt_vaddr = kmap_atomic(i915_gem_object_get_page(ppgtt->state, pde));
		for (pte = 0; pte < I915_PPGTT_PT_ENTRIES; pte+=4) {
			unsigned long va =
				(pde * PAGE_SIZE * I915_PPGTT_PT_ENTRIES) +
				(pte * PAGE_SIZE);
			int i;
			bool found = false;
			for (i = 0; i < 4; i++)
				if (pt_vaddr[pte + i] != scratch_pte)
					found = true;
			if (!found)
				continue;

			seq_printf(m, "\t\t0x%lx [%03d,%04d]: =", va, pde, pte);
			for (i = 0; i < 4; i++) {
				if (pt_vaddr[pte + i] != scratch_pte)
					seq_printf(m, " %08x", pt_vaddr[pte + i]);
				else
					seq_puts(m, "  SCRATCH ");
			}
			seq_puts(m, "\n");
		}
		kunmap_atomic(pt_vaddr);
	}
}

static uint32_t get_pd_offset(struct i915_hw_ppgtt *ppgtt)
{
	uint64_t offset = i915_gem_obj_to_ggtt(ppgtt->state)->node.start / PAGE_SIZE;
	return (offset * sizeof(gen6_gtt_pte_t) / 64) << 16;
}

static int gen7_mm_switch(struct i915_gem_request *rq,
			  struct i915_hw_ppgtt *ppgtt)
{
	struct intel_ringbuffer *ring;
	int ret;

	rq->pending_flush = ~0; /* XXX force the flush */
	ret = i915_request_emit_flush(rq, I915_COMMAND_BARRIER);
	if (ret)
		return ret;

	ring = intel_ring_begin(rq, 5);
	if (IS_ERR(ring))
		return PTR_ERR(ring);

	intel_ring_emit(ring, MI_LOAD_REGISTER_IMM(2));
	intel_ring_emit(ring, RING_PP_DIR_DCLV(rq->engine));
	intel_ring_emit(ring, PP_DIR_DCLV_2G);
	intel_ring_emit(ring, RING_PP_DIR_BASE(rq->engine));
	intel_ring_emit(ring, get_pd_offset(ppgtt));
	intel_ring_advance(ring);

	rq->pending_flush |= I915_INVALIDATE_CACHES;

	return 0;
}

static int gen6_mm_switch(struct i915_gem_request *rq,
			  struct i915_hw_ppgtt *ppgtt)
{
	return -ENODEV;
}

static int gen8_ppgtt_enable(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct intel_engine_cs *engine;
	int j;

	for_each_engine(engine, dev_priv, j) {
		I915_WRITE(RING_MODE_GEN7(engine),
				_MASKED_BIT_ENABLE(GFX_PPGTT_ENABLE));

		if (dev_priv->mm.aliasing_ppgtt) {
			struct i915_gem_request *rq;
			int ret;

			rq = i915_request_create(engine->default_context,
						 engine);
			if (IS_ERR(rq))
				return PTR_ERR(rq);

			ret = gen8_mm_switch(rq, dev_priv->mm.aliasing_ppgtt);
			if (ret == 0)
				ret = i915_request_commit(rq);
			i915_request_put(rq);
			if (ret)
				return ret;
		}
	}

	return 0;
}

static int gen7_ppgtt_enable(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct intel_engine_cs *engine;
	uint32_t ecochk, ecobits;
	int i;

	ecobits = I915_READ(GAC_ECO_BITS);
	I915_WRITE(GAC_ECO_BITS, ecobits | ECOBITS_PPGTT_CACHE64B);

	ecochk = I915_READ(GAM_ECOCHK);
	if (IS_HASWELL(dev)) {
		ecochk |= ECOCHK_PPGTT_WB_HSW;
	} else {
		ecochk |= ECOCHK_PPGTT_LLC_IVB;
		ecochk &= ~ECOCHK_PPGTT_GFDT_IVB;
	}
	I915_WRITE(GAM_ECOCHK, ecochk);

	for_each_engine(engine, dev_priv, i) {
		/* GFX_MODE is per-ring on gen7+ */
		I915_WRITE(RING_MODE_GEN7(engine),
			   _MASKED_BIT_ENABLE(GFX_PPGTT_ENABLE));

		if (dev_priv->mm.aliasing_ppgtt) {
			I915_WRITE(RING_PP_DIR_DCLV(engine), PP_DIR_DCLV_2G);
			I915_WRITE(RING_PP_DIR_BASE(engine), get_pd_offset(dev_priv->mm.aliasing_ppgtt));
		}

	}
	POSTING_READ(RING_PP_DIR_DCLV(RCS_ENGINE(dev_priv)));
	return 0;
}

static int gen6_ppgtt_enable(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	uint32_t ecochk, gab_ctl, ecobits;
	struct intel_engine_cs *engine;
	int i;

	ecobits = I915_READ(GAC_ECO_BITS);
	I915_WRITE(GAC_ECO_BITS, ecobits | ECOBITS_SNB_BIT |
		   ECOBITS_PPGTT_CACHE64B);

	gab_ctl = I915_READ(GAB_CTL);
	I915_WRITE(GAB_CTL, gab_ctl | GAB_CTL_CONT_AFTER_PAGEFAULT);

	ecochk = I915_READ(GAM_ECOCHK);
	I915_WRITE(GAM_ECOCHK, ecochk | ECOCHK_SNB_BIT | ECOCHK_PPGTT_CACHE64B);

	I915_WRITE(GFX_MODE, _MASKED_BIT_ENABLE(GFX_PPGTT_ENABLE));

	for_each_engine(engine, dev_priv, i) {
		if (dev_priv->mm.aliasing_ppgtt) {
			I915_WRITE(RING_PP_DIR_DCLV(engine), PP_DIR_DCLV_2G);
			I915_WRITE(RING_PP_DIR_BASE(engine), get_pd_offset(dev_priv->mm.aliasing_ppgtt));
		}

	}
	POSTING_READ(RING_PP_DIR_DCLV(RCS_ENGINE(dev_priv)));
	return 0;
}

/* PPGTT support for Sandybdrige/Gen6 and later */
static inline void *kmap_pt(struct i915_hw_ppgtt *ppgtt, int pt)
{
	return kmap_atomic(i915_gem_object_get_page(ppgtt->state, pt));
}

static int gen6_ppgtt_clear_range(struct i915_address_space *vm,
				  uint64_t start,
				  uint64_t length,
				  bool use_scratch)
{
	struct i915_hw_ppgtt *ppgtt =
		container_of(vm, struct i915_hw_ppgtt, base);
	gen6_gtt_pte_t *pt_vaddr, scratch_pte;
	unsigned first_entry = start >> PAGE_SHIFT;
	unsigned num_entries = length >> PAGE_SHIFT;
	unsigned act_pt = first_entry / I915_PPGTT_PT_ENTRIES;
	unsigned first_pte = first_entry % I915_PPGTT_PT_ENTRIES;
	unsigned last_pte, i;
	int ret;

	ret = i915_gem_object_get_pages(ppgtt->state);
	if (ret)
		return ret;

	scratch_pte = vm->pte_encode(vm->scratch.addr, I915_CACHE_LLC, true, 0);

	while (num_entries) {
		last_pte = first_pte + num_entries;
		if (last_pte > I915_PPGTT_PT_ENTRIES)
			last_pte = I915_PPGTT_PT_ENTRIES;

		pt_vaddr = kmap_pt(ppgtt, act_pt);

		for (i = first_pte; i < last_pte; i++)
			pt_vaddr[i] = scratch_pte;

		kunmap_atomic(pt_vaddr);

		num_entries -= last_pte - first_pte;
		first_pte = 0;
		act_pt++;
	}

	return 0;
}

static int gen6_ppgtt_insert_entries(struct i915_address_space *vm,
				     struct sg_table *pages, uint64_t start,
				     enum i915_cache_level cache_level,
				     u32 flags)
{
	struct i915_hw_ppgtt *ppgtt =
		container_of(vm, struct i915_hw_ppgtt, base);
	gen6_gtt_pte_t *pt_vaddr;
	unsigned first_entry = start >> PAGE_SHIFT;
	unsigned act_pt = first_entry / I915_PPGTT_PT_ENTRIES;
	unsigned act_pte = first_entry % I915_PPGTT_PT_ENTRIES;
	struct sg_page_iter sg_iter;
	int ret;

	ret = i915_gem_object_get_pages(ppgtt->state);
	if (ret)
		return ret;

	pt_vaddr = NULL;
	for_each_sg_page(pages->sgl, &sg_iter, pages->nents, 0) {
		if (pt_vaddr == NULL)
			pt_vaddr = kmap_pt(ppgtt, act_pt);

		pt_vaddr[act_pte] =
			vm->pte_encode(sg_page_iter_dma_address(&sg_iter),
				       cache_level, true, flags);

		if (++act_pte == I915_PPGTT_PT_ENTRIES) {
			kunmap_atomic(pt_vaddr);
			pt_vaddr = NULL;
			act_pt++;
			act_pte = 0;
		}
	}
	if (pt_vaddr)
		kunmap_atomic(pt_vaddr);

	return 0;
}

static void gen6_ppgtt_cleanup(struct i915_address_space *vm)
{
	struct i915_hw_ppgtt *ppgtt =
		container_of(vm, struct i915_hw_ppgtt, base);

	drm_gem_object_unreference(&ppgtt->state->base);
}

static int gen6_ppgtt_alloc(struct i915_hw_ppgtt *ppgtt)
{
	/* PPGTT PDEs reside in the GGTT and consists of 512 entries. The
	 * allocator works in address space sizes, so it's multiplied by page
	 * size. We allocate at the top of the GTT to avoid fragmentation.
	 */
	ppgtt->state = i915_gem_alloc_object(ppgtt->base.dev, GEN6_PD_SIZE);
	if (ppgtt->state == NULL)
		return -ENOMEM;

	ppgtt->state->pde = true;

	ppgtt->num_pd_entries = GEN6_PPGTT_PD_ENTRIES;
	ppgtt->alignment = GEN6_PD_ALIGN;
	return 0;
}

static int gen6_ppgtt_init(struct i915_hw_ppgtt *ppgtt)
{
	struct drm_device *dev = ppgtt->base.dev;
	struct drm_i915_private *dev_priv = dev->dev_private;
	int ret;

	ppgtt->base.pte_encode = dev_priv->gtt.base.pte_encode;
	if (IS_GEN6(dev)) {
		ppgtt->switch_mm = gen6_mm_switch;
	} else if (IS_GEN7(dev)) {
		ppgtt->switch_mm = gen7_mm_switch;
	} else
		BUG();

	ret = gen6_ppgtt_alloc(ppgtt);
	if (ret)
		return ret;

	ppgtt->base.clear_range = gen6_ppgtt_clear_range;
	ppgtt->base.insert_entries = gen6_ppgtt_insert_entries;
	ppgtt->base.cleanup = gen6_ppgtt_cleanup;
	ppgtt->base.start = 0;
	ppgtt->base.total =  ppgtt->num_pd_entries * I915_PPGTT_PT_ENTRIES * PAGE_SIZE;
	ppgtt->debug_dump = gen6_dump_ppgtt;

	return ppgtt->base.clear_range(&ppgtt->base,
				       0, ppgtt->base.total,
				       true);
}

static void i915_ggtt_flush(struct drm_i915_private *dev_priv)
{
	if (INTEL_INFO(dev_priv->dev)->gen < 6) {
		intel_gtt_chipset_flush();
	} else {
		I915_WRITE(GFX_FLSH_CNTL_GEN6, GFX_FLSH_CNTL_EN);
		POSTING_READ(GFX_FLSH_CNTL_GEN6);
	}
}

static int pde_bind_vma(struct i915_vma *vma,
			enum i915_cache_level cache_level,
			u32 flags)
{
	struct sg_table *pages = vma->obj->pages;
	struct drm_i915_private *i915 = to_i915(vma->vm->dev);
	gen6_gtt_pte_t __iomem *pd_addr;
	struct sg_page_iter sg_iter;

	if (WARN_ON(flags != GLOBAL_BIND))
		return -EINVAL;

	if (WARN_ON(vma->node.start & 0x3f))
		return -EINVAL;

	if (vma->bound & GLOBAL_BIND)
		return 0;

	pd_addr = (gen6_gtt_pte_t __iomem*)i915->gtt.gsm;
	pd_addr += vma->node.start >> PAGE_SHIFT;
	for_each_sg_page(pages->sgl, &sg_iter, pages->nents, 0) {
		dma_addr_t addr = (sg_dma_address(sg_iter.sg) +
				   (sg_iter.sg_pgoffset << PAGE_SHIFT));
		writel(GEN6_PDE_ADDR_ENCODE(addr) | GEN6_PDE_VALID, pd_addr++);
	}
	i915_ggtt_flush(i915);

	/* Skips a second call when pinning */
	vma->bound = GLOBAL_BIND;
	return 0;
}

static int pde_unbind_vma(struct i915_vma *vma)
{
	vma->bound = 0;
	return 0;
}

static int __hw_ppgtt_init(struct drm_device *dev, struct i915_hw_ppgtt *ppgtt)
{
	struct drm_i915_private *dev_priv = dev->dev_private;

	ppgtt->base.dev = dev;
	ppgtt->base.scratch = dev_priv->gtt.base.scratch;
	INIT_LIST_HEAD(&ppgtt->base.vma_list);

	if (INTEL_INFO(dev)->gen < 8)
		return gen6_ppgtt_init(ppgtt);
	else if (IS_GEN8(dev) || IS_GEN9(dev))
		return gen8_ppgtt_init(ppgtt, dev_priv->gtt.base.total);
	else
		BUG();
}
int i915_ppgtt_init(struct drm_device *dev, struct i915_hw_ppgtt *ppgtt)
{
	int ret;

	ret = __hw_ppgtt_init(dev, ppgtt);
	if (ret)
		return ret;

	drm_mm_init(&ppgtt->base.mm, ppgtt->base.start, ppgtt->base.total);
	i915_init_vm(to_i915(dev), &ppgtt->base);

	return 0;
}

int i915_ppgtt_init_hw(struct drm_device *dev)
{
	int ret;

	if (!USES_PPGTT(dev))
		return 0;

	/* In the case of execlists, PPGTT is enabled by the context descriptor
	 * and the PDPs are contained within the context itself.  We don't
	 * need to do anything here. */
	if (RCS_ENGINE(dev)->execlists_enabled)
		return 0;

	if (IS_GEN6(dev))
		ret = gen6_ppgtt_enable(dev);
	else if (IS_GEN7(dev))
		ret = gen7_ppgtt_enable(dev);
	else if (INTEL_INFO(dev)->gen >= 8)
		ret = gen8_ppgtt_enable(dev);
	else {
		WARN_ON(1);
		ret = -ENODEV;
	}

	return ret;
}

struct i915_hw_ppgtt *
i915_ppgtt_create(struct drm_device *dev, struct drm_i915_file_private *fpriv)
{
	struct i915_hw_ppgtt *ppgtt;
	int ret;

	ppgtt = kzalloc(sizeof(*ppgtt), GFP_KERNEL);
	if (!ppgtt)
		return ERR_PTR(-ENOMEM);

	ret = i915_ppgtt_init(dev, ppgtt);
	if (ret) {
		kfree(ppgtt);
		return ERR_PTR(ret);
	}

	ppgtt->file_priv = fpriv;

	trace_i915_vm_create(&ppgtt->base);

	return ppgtt;
}

void __i915_vm_free(struct kref *kref)
{
	struct i915_address_space *vm =
		container_of(kref, struct i915_address_space, ref);

	DRM_DEBUG_DRIVER("VM %p freed\n", vm);
	trace_i915_vm_free(vm);

	/* vmas should already be unbound */
	WARN_ON(!list_empty(&vm->vma_list));
	WARN_ON(!list_empty(&vm->active_list));
	WARN_ON(!list_empty(&vm->inactive_list));

	list_del(&vm->global_link);
	drm_mm_takedown(&vm->mm);

	vm->cleanup(vm);
	kfree(vm);
}

static int
ppgtt_bind_vma(struct i915_vma *vma,
	       enum i915_cache_level cache_level,
	       u32 flags)
{
	int ret;

	if (WARN_ON(flags & GLOBAL_BIND))
		return -EINVAL;

	/* Currently applicable only to VLV */
	if (vma->obj->gt_ro)
		flags |= PTE_READ_ONLY;

	if (flags == vma->bound)
		return 0;

	if (flags == REBIND) {
		flags &= ~REBIND;
		flags |= LOCAL_BIND;
	}

	ret = vma->vm->insert_entries(vma->vm,
				      vma->obj->pages, vma->node.start,
				      cache_level, flags);
	if (ret)
		return ret;

	vma->vm->dirty = true;
	vma->bound = flags;
	return 0;
}

static int ppgtt_unbind_vma(struct i915_vma *vma)
{
	vma->vm->clear_range(vma->vm,
			     vma->node.start,
			     vma->obj->base.size,
			     true);
	vma->bound = 0;
	return 0;
}

extern int intel_iommu_gfx_mapped;
/* Certain Gen5 chipsets require require idling the GPU before
 * unmapping anything from the GTT when VT-d is enabled.
 */
static inline bool needs_idle_maps(struct drm_device *dev)
{
#ifdef CONFIG_INTEL_IOMMU
	/* Query intel_iommu to see if we need the workaround. Presumably that
	 * was loaded first.
	 */
	if (IS_GEN5(dev) && IS_MOBILE(dev) && intel_iommu_gfx_mapped)
		return true;
#endif
	return false;
}

static bool do_idling(struct drm_i915_private *dev_priv)
{
	bool ret = dev_priv->mm.interruptible;

	if (unlikely(dev_priv->gtt.do_idle_maps)) {
		dev_priv->mm.interruptible = false;
		if (i915_gpu_idle(dev_priv->dev)) {
			DRM_ERROR("Couldn't idle GPU\n");
			/* Wait a bit, in hopes it avoids the hang */
			udelay(10);
		}
	}

	return ret;
}

static void undo_idling(struct drm_i915_private *dev_priv, bool interruptible)
{
	if (unlikely(dev_priv->gtt.do_idle_maps))
		dev_priv->mm.interruptible = interruptible;
}

void i915_check_and_clear_faults(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct intel_engine_cs *engine;
	int i;

	if (INTEL_INFO(dev)->gen < 6)
		return;

	for_each_engine(engine, dev_priv, i) {
		u32 fault_reg;
		fault_reg = I915_READ(RING_FAULT_REG(engine));
		if (fault_reg & RING_FAULT_VALID) {
			DRM_DEBUG_DRIVER("Unexpected fault\n"
					 "\tAddr: 0x%08lx\n"
					 "\tAddress space: %s\n"
					 "\tSource ID: %d\n"
					 "\tType: %d\n",
					 fault_reg & PAGE_MASK,
					 fault_reg & RING_FAULT_GTTSEL_MASK ? "GGTT" : "PPGTT",
					 RING_FAULT_SRCID(fault_reg),
					 RING_FAULT_FAULT_TYPE(fault_reg));
			I915_WRITE(RING_FAULT_REG(engine),
				   fault_reg & ~RING_FAULT_VALID);
		}
	}
	POSTING_READ(RING_FAULT_REG(RCS_ENGINE(dev_priv)));
}

void i915_gem_suspend_gtt_mappings(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = dev->dev_private;

	/* Don't bother messing with faults pre GEN6 as we have little
	 * documentation supporting that it's a good idea.
	 */
	if (INTEL_INFO(dev)->gen < 6)
		return;

	i915_check_and_clear_faults(dev);

	dev_priv->gtt.base.clear_range(&dev_priv->gtt.base,
				       dev_priv->gtt.base.start,
				       dev_priv->gtt.base.total,
				       true);

	i915_ggtt_flush(dev_priv);
}

void i915_gem_restore_gtt_mappings(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct drm_i915_gem_object *obj;

	i915_check_and_clear_faults(dev);

	/* First fill our portion of the GTT with scratch pages */
	dev_priv->gtt.base.clear_range(&dev_priv->gtt.base,
				       dev_priv->gtt.base.start,
				       dev_priv->gtt.base.total,
				       true);

	list_for_each_entry(obj, &dev_priv->mm.bound_list, global_list) {
		struct i915_vma *vma = i915_gem_obj_to_vma(obj,
							   &dev_priv->gtt.base);
		if (!vma)
			continue;

		i915_gem_clflush_object(obj, obj->pin_display);
		vma->bind_vma(vma, obj->cache_level, REBIND);
	}


	if (INTEL_INFO(dev)->gen >= 8) {
		if (IS_CHERRYVIEW(dev))
			chv_setup_private_ppat(dev_priv);
		else
			bdw_setup_private_ppat(dev_priv);

		return;
	}

	i915_ggtt_flush(dev_priv);
}

int i915_gem_gtt_prepare_object(struct drm_i915_gem_object *obj)
{
	if (obj->has_dma_mapping)
		return 0;

	if (!dma_map_sg(&obj->base.dev->pdev->dev,
			obj->pages->sgl, obj->pages->nents,
			PCI_DMA_BIDIRECTIONAL))
		return -ENOSPC;

	return 0;
}

static inline void gen8_set_pte(void __iomem *addr, gen8_gtt_pte_t pte)
{
#if defined(writeq)
	writeq(pte, addr);
#else
	iowrite32(lower_32_bits(pte), addr);
	iowrite32(upper_32_bits(pte), addr + 4);
#endif
}

static int gen8_ggtt_insert_entries(struct i915_address_space *vm,
				    struct sg_table *st,
				    uint64_t start,
				    enum i915_cache_level level, u32 unused)
{
	struct drm_i915_private *dev_priv = vm->dev->dev_private;
	unsigned first_entry = start >> PAGE_SHIFT;
	gen8_gtt_pte_t __iomem *gtt_entries =
		(gen8_gtt_pte_t __iomem *)dev_priv->gtt.gsm + first_entry;
	int i = 0;
	struct sg_page_iter sg_iter;
	dma_addr_t addr = 0; /* shut up gcc */

	for_each_sg_page(st->sgl, &sg_iter, st->nents, 0) {
		addr = sg_dma_address(sg_iter.sg) +
			(sg_iter.sg_pgoffset << PAGE_SHIFT);
		gen8_set_pte(&gtt_entries[i],
			     gen8_pte_encode(addr, level, true));
		i++;
	}

	/*
	 * XXX: This serves as a posting read to make sure that the PTE has
	 * actually been updated. There is some concern that even though
	 * registers and PTEs are within the same BAR that they are potentially
	 * of NUMA access patterns. Therefore, even with the way we assume
	 * hardware should work, we must keep this posting read for paranoia.
	 */
	if (i != 0)
		WARN_ON(readq(&gtt_entries[i-1])
			!= gen8_pte_encode(addr, level, true));

	/* This next bit makes the above posting read even more important. We
	 * want to flush the TLBs only after we're certain all the PTE updates
	 * have finished.
	 */
	I915_WRITE(GFX_FLSH_CNTL_GEN6, GFX_FLSH_CNTL_EN);
	POSTING_READ(GFX_FLSH_CNTL_GEN6);

	return 0;
}

/*
 * Binds an object into the global gtt with the specified cache level. The object
 * will be accessible to the GPU via commands whose operands reference offsets
 * within the global GTT as well as accessible by the GPU through the GMADR
 * mapped BAR (dev_priv->mm.gtt->gtt).
 */
static int gen6_ggtt_insert_entries(struct i915_address_space *vm,
				    struct sg_table *st,
				    uint64_t start,
				    enum i915_cache_level level, u32 flags)
{
	struct drm_i915_private *dev_priv = vm->dev->dev_private;
	unsigned first_entry = start >> PAGE_SHIFT;
	gen6_gtt_pte_t __iomem *gtt_entries =
		(gen6_gtt_pte_t __iomem *)dev_priv->gtt.gsm + first_entry;
	int i = 0;
	struct sg_page_iter sg_iter;
	dma_addr_t addr = 0;

	for_each_sg_page(st->sgl, &sg_iter, st->nents, 0) {
		addr = sg_page_iter_dma_address(&sg_iter);
		iowrite32(vm->pte_encode(addr, level, true, flags), &gtt_entries[i]);
		i++;
	}

	/* XXX: This serves as a posting read to make sure that the PTE has
	 * actually been updated. There is some concern that even though
	 * registers and PTEs are within the same BAR that they are potentially
	 * of NUMA access patterns. Therefore, even with the way we assume
	 * hardware should work, we must keep this posting read for paranoia.
	 */
	if (i != 0) {
		unsigned long gtt = readl(&gtt_entries[i-1]);
		WARN_ON(gtt != vm->pte_encode(addr, level, true, flags));
	}

	/* This next bit makes the above posting read even more important. We
	 * want to flush the TLBs only after we're certain all the PTE updates
	 * have finished.
	 */
	I915_WRITE(GFX_FLSH_CNTL_GEN6, GFX_FLSH_CNTL_EN);
	POSTING_READ(GFX_FLSH_CNTL_GEN6);

	return 0;
}

static int gen8_ggtt_clear_range(struct i915_address_space *vm,
				 uint64_t start,
				 uint64_t length,
				 bool use_scratch)
{
	struct drm_i915_private *dev_priv = vm->dev->dev_private;
	unsigned first_entry = start >> PAGE_SHIFT;
	unsigned num_entries = length >> PAGE_SHIFT;
	gen8_gtt_pte_t scratch_pte, __iomem *gtt_base =
		(gen8_gtt_pte_t __iomem *) dev_priv->gtt.gsm + first_entry;
	const int max_entries = gtt_total_entries(dev_priv->gtt) - first_entry;
	int i;

	if (WARN(num_entries > max_entries,
		 "First entry = %d; Num entries = %d (max=%d)\n",
		 first_entry, num_entries, max_entries))
		num_entries = max_entries;

	scratch_pte = gen8_pte_encode(vm->scratch.addr,
				      I915_CACHE_LLC,
				      use_scratch);
	for (i = 0; i < num_entries; i++)
		gen8_set_pte(&gtt_base[i], scratch_pte);
	readl(gtt_base);

	return 0;
}

static int gen6_ggtt_clear_range(struct i915_address_space *vm,
				 uint64_t start,
				 uint64_t length,
				 bool use_scratch)
{
	struct drm_i915_private *dev_priv = vm->dev->dev_private;
	unsigned first_entry = start >> PAGE_SHIFT;
	unsigned num_entries = length >> PAGE_SHIFT;
	gen6_gtt_pte_t scratch_pte, __iomem *gtt_base =
		(gen6_gtt_pte_t __iomem *) dev_priv->gtt.gsm + first_entry;
	const int max_entries = gtt_total_entries(dev_priv->gtt) - first_entry;
	int i;

	if (WARN(num_entries > max_entries,
		 "First entry = %d; Num entries = %d (max=%d)\n",
		 first_entry, num_entries, max_entries))
		num_entries = max_entries;

	scratch_pte = vm->pte_encode(vm->scratch.addr, I915_CACHE_LLC, use_scratch, 0);

	for (i = 0; i < num_entries; i++)
		iowrite32(scratch_pte, &gtt_base[i]);
	readl(gtt_base);

	return 0;
}

static void mark_global_obj(struct i915_vma *vma)
{
	struct drm_i915_gem_object *obj = vma->obj;
	bool mappable, fenceable;
	u32 fence_size, fence_alignment;

	fence_size = i915_gem_get_gtt_size(obj->base.dev,
					   obj->base.size,
					   obj->tiling_mode);
	fence_alignment = i915_gem_get_gtt_alignment(obj->base.dev,
						     obj->base.size,
						     obj->tiling_mode,
						     true);

	fenceable = (vma->node.size == fence_size &&
		     (vma->node.start & (fence_alignment - 1)) == 0);

	mappable = (vma->node.start + obj->base.size <=
		    to_i915(obj->base.dev)->gtt.mappable_end);

	obj->map_and_fenceable = mappable && fenceable;
}

static int i915_ggtt_bind_vma(struct i915_vma *vma,
			      enum i915_cache_level cache_level,
			      u32 unused)
{
	const unsigned long entry = vma->node.start >> PAGE_SHIFT;
	unsigned int flags = (cache_level == I915_CACHE_NONE) ?
		AGP_USER_MEMORY : AGP_USER_CACHED_MEMORY;

	if (vma->bound && (flags & REBIND) == 0)
		return 0;

	BUG_ON(!i915_is_ggtt(vma->vm));
	intel_gtt_insert_sg_entries(vma->obj->pages, entry, flags);
	vma->bound = GLOBAL_BIND;
	vma->vm->dirty = true;

	mark_global_obj(vma);

	return 0;
}

static int i915_ggtt_clear_range(struct i915_address_space *vm,
				 uint64_t start,
				 uint64_t length,
				 bool unused)
{
	unsigned first_entry = start >> PAGE_SHIFT;
	unsigned num_entries = length >> PAGE_SHIFT;

	intel_gtt_clear_range(first_entry, num_entries);
	return 0;
}

static int nop_clear_range(struct i915_address_space *vm,
			   uint64_t start,
			   uint64_t length,
			   bool use_scratch)
{
	return 0;
}

static int i915_ggtt_unbind_vma(struct i915_vma *vma)
{
	struct drm_i915_gem_object *obj = vma->obj;
	const unsigned int first = vma->node.start >> PAGE_SHIFT;
	const unsigned int size = obj->base.size >> PAGE_SHIFT;
	int ret;

	BUG_ON(!i915_is_ggtt(vma->vm));
	BUG_ON(vma->bound == 0);

	i915_gem_object_finish_gtt(obj);

	/* release the fence reg _after_ flushing */
	ret = i915_gem_object_put_fence(obj);
	if (WARN_ON(ret)) /* should be idle already */
		return ret;

	obj->map_and_fenceable = false;

	intel_gtt_clear_range(first, size);
	vma->bound = 0;

	return 0;
}

static int ggtt_bind_vma(struct i915_vma *vma,
			 enum i915_cache_level cache_level,
			 u32 flags)
{
	struct drm_device *dev = vma->vm->dev;
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct drm_i915_gem_object *obj = vma->obj;
	int ret;

	if (flags == REBIND) {
		flags = vma->bound;
		vma->bound = 0;
	}

	if (!dev_priv->mm.aliasing_ppgtt) {
		flags |= GLOBAL_BIND;
		flags &= ~LOCAL_BIND;
	}

	/* Currently applicable only to VLV */
	if (obj->gt_ro)
		flags |= PTE_READ_ONLY;

	/* If there is no aliasing PPGTT, or the caller needs a global mapping,
	 * or we have a global mapping already but the cacheability flags have
	 * changed, set the global PTEs.
	 *
	 * If there is an aliasing PPGTT it is anecdotally faster, so use that
	 * instead if none of the above hold true.
	 *
	 * NB: A global mapping should only be needed for special regions like
	 * "gtt mappable", SNB errata, or if specified via special execbuf
	 * flags. At all other times, the GPU will use the aliasing PPGTT.
	 */
	if ((flags & ~vma->bound) & GLOBAL_BIND) {
		ret = vma->vm->insert_entries(vma->vm, obj->pages,
					      vma->node.start,
					      cache_level, flags);
		if (ret)
			return ret;

		vma->bound |= GLOBAL_BIND;
		vma->vm->dirty = true;
		mark_global_obj(vma);
	}

	if ((flags & ~vma->bound) & LOCAL_BIND) {
		struct i915_hw_ppgtt *appgtt = dev_priv->mm.aliasing_ppgtt;

		ret = appgtt->base.insert_entries(&appgtt->base,
						  obj->pages,
						  vma->node.start,
						  cache_level, flags);
		if (ret)
			return ret;

		vma->bound |= LOCAL_BIND;
		vma->vm->dirty = true;
	}

	vma->bound |= flags;
	return 0;
}

static int ggtt_unbind_vma(struct i915_vma *vma)
{
	struct drm_device *dev = vma->vm->dev;
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct drm_i915_gem_object *obj = vma->obj;
	int ret;

	if (vma->bound & GLOBAL_BIND) {
		i915_gem_object_finish_gtt(obj);

		/* release the fence reg _after_ flushing */
		ret = i915_gem_object_put_fence(obj);
		if (WARN_ON(ret)) /* should be idle already */
			return ret;

		obj->map_and_fenceable = false;

		vma->vm->clear_range(vma->vm,
				     vma->node.start,
				     obj->base.size,
				     true);
	}

	if (vma->bound & LOCAL_BIND) {
		struct i915_hw_ppgtt *appgtt = dev_priv->mm.aliasing_ppgtt;
		appgtt->base.clear_range(&appgtt->base,
					 vma->node.start,
					 obj->base.size,
					 true);
	}

	vma->bound = 0;
	return 0;
}

void i915_gem_gtt_finish_object(struct drm_i915_gem_object *obj)
{
	struct drm_device *dev = obj->base.dev;
	struct drm_i915_private *dev_priv = dev->dev_private;
	bool interruptible;

	interruptible = do_idling(dev_priv);

	if (!obj->has_dma_mapping)
		dma_unmap_sg(&dev->pdev->dev,
			     obj->pages->sgl, obj->pages->nents,
			     PCI_DMA_BIDIRECTIONAL);

	undo_idling(dev_priv, interruptible);
}

static void i915_gtt_color_adjust(struct drm_mm_node *node,
				  unsigned long color,
				  unsigned long *start,
				  unsigned long *end)
{
	if (node->color != color)
		*start += 4096;

	if (!list_empty(&node->node_list)) {
		node = list_entry(node->node_list.next,
				  struct drm_mm_node,
				  node_list);
		if (node->allocated && node->color != color)
			*end -= 4096;
	}
}

int i915_gem_setup_global_gtt(struct drm_device *dev,
			      unsigned long start,
			      unsigned long mappable_end,
			      unsigned long end)
{
	/* Let GEM Manage all of the aperture.
	 *
	 * However, leave one page at the end still bound to the scratch page.
	 * There are a number of places where the hardware apparently prefetches
	 * past the end of the object, and we've seen multiple hangs with the
	 * GPU head pointer stuck in a batchbuffer bound at the last page of the
	 * aperture.  One page should be enough to keep any prefetching inside
	 * of the aperture.
	 */
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct i915_address_space *ggtt = &dev_priv->gtt.base;
	struct drm_mm_node *entry;
	struct drm_i915_gem_object *obj;
	unsigned long hole_start, hole_end;
	int ret;

	BUG_ON(mappable_end > end);

	/* Subtract the guard page ... */
	drm_mm_init(&ggtt->mm, start, end - start - PAGE_SIZE);
	if (!HAS_LLC(dev))
		dev_priv->gtt.base.mm.color_adjust = i915_gtt_color_adjust;

	/* Mark any preallocated objects as occupied */
	list_for_each_entry(obj, &dev_priv->mm.bound_list, global_list) {
		struct i915_vma *vma = i915_gem_obj_to_ggtt(obj);

		WARN_ON(drm_mm_node_allocated(&vma->node));

		DRM_DEBUG_KMS("reserving preallocated space: %lx + %zx\n",
			      vma->node.start, obj->base.size);
		ret = drm_mm_reserve_node(&ggtt->mm, &vma->node);
		if (ret) {
			DRM_DEBUG_KMS("Reservation failed: %i\n", ret);
			return ret;
		}
		vma->bound |= GLOBAL_BIND;
		mark_global_obj(vma);
	}

	ggtt->start = start;
	ggtt->total = end - start;

	if (USES_PPGTT(dev) && !USES_FULL_PPGTT(dev)) {
		struct i915_hw_ppgtt *ppgtt;

		ppgtt = kzalloc(sizeof(*ppgtt), GFP_KERNEL);
		if (!ppgtt)
			return -ENOMEM;

		ret = __hw_ppgtt_init(dev, ppgtt);
		if (ret== 0 && ppgtt->state)
			ret = i915_gem_object_ggtt_pin(ppgtt->state,
						       ppgtt->alignment,
						       0);
		if (ret == 0)
			dev_priv->mm.aliasing_ppgtt = ppgtt;
	}

	if (USES_FULL_PPGTT(dev) || dev_priv->mm.aliasing_ppgtt)
		ggtt->clear_range = nop_clear_range;

	/* Clear any non-preallocated blocks */
	drm_mm_for_each_hole(entry, &ggtt->mm, hole_start, hole_end) {
		DRM_DEBUG_KMS("clearing unused GTT space: [%lx, %lx]\n",
			      hole_start, hole_end);
		ggtt->clear_range(ggtt,
				  hole_start, hole_end - hole_start,
				  true);
	}

	/* And finally clear the reserved guard page */
	ggtt->clear_range(ggtt, end - PAGE_SIZE, PAGE_SIZE, true);

	return 0;
}

void i915_gem_init_global_gtt(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	unsigned long gtt_size, mappable_size;

	gtt_size = dev_priv->gtt.base.total;
	mappable_size = dev_priv->gtt.mappable_end;

	i915_gem_setup_global_gtt(dev, 0, mappable_size, gtt_size);
}

void i915_global_gtt_cleanup(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct i915_address_space *vm = &dev_priv->gtt.base;

	lockdep_assert_held(&dev->struct_mutex);

	if (dev_priv->mm.aliasing_ppgtt) {
		struct i915_hw_ppgtt *ppgtt = dev_priv->mm.aliasing_ppgtt;

		ppgtt->base.cleanup(&ppgtt->base);
	}

	if (drm_mm_initialized(&vm->mm)) {
		drm_mm_takedown(&vm->mm);
		list_del(&vm->global_link);
	}

	vm->cleanup(vm);
}

static int setup_scratch_page(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct page *page;
	dma_addr_t dma_addr;

	page = alloc_page(GFP_KERNEL | GFP_DMA32 | __GFP_ZERO);
	if (page == NULL)
		return -ENOMEM;
	set_pages_uc(page, 1);

#ifdef CONFIG_INTEL_IOMMU
	dma_addr = pci_map_page(dev->pdev, page, 0, PAGE_SIZE,
				PCI_DMA_BIDIRECTIONAL);
	if (pci_dma_mapping_error(dev->pdev, dma_addr))
		return -EINVAL;
#else
	dma_addr = page_to_phys(page);
#endif
	dev_priv->gtt.base.scratch.page = page;
	dev_priv->gtt.base.scratch.addr = dma_addr;

	return 0;
}

static void teardown_scratch_page(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct page *page = dev_priv->gtt.base.scratch.page;

	set_pages_wb(page, 1);
	pci_unmap_page(dev->pdev, dev_priv->gtt.base.scratch.addr,
		       PAGE_SIZE, PCI_DMA_BIDIRECTIONAL);
	__free_page(page);
}

static inline unsigned int gen6_get_total_gtt_size(u16 snb_gmch_ctl)
{
	snb_gmch_ctl >>= SNB_GMCH_GGMS_SHIFT;
	snb_gmch_ctl &= SNB_GMCH_GGMS_MASK;
	return snb_gmch_ctl << 20;
}

static inline unsigned int gen8_get_total_gtt_size(u16 bdw_gmch_ctl)
{
	bdw_gmch_ctl >>= BDW_GMCH_GGMS_SHIFT;
	bdw_gmch_ctl &= BDW_GMCH_GGMS_MASK;
	if (bdw_gmch_ctl)
		bdw_gmch_ctl = 1 << bdw_gmch_ctl;

#ifdef CONFIG_X86_32
	/* Limit 32b platforms to a 2GB GGTT: 4 << 20 / pte size * PAGE_SIZE */
	if (bdw_gmch_ctl > 4)
		bdw_gmch_ctl = 4;
#endif

	return bdw_gmch_ctl << 20;
}

static inline unsigned int chv_get_total_gtt_size(u16 gmch_ctrl)
{
	gmch_ctrl >>= SNB_GMCH_GGMS_SHIFT;
	gmch_ctrl &= SNB_GMCH_GGMS_MASK;

	if (gmch_ctrl)
		return 1 << (20 + gmch_ctrl);

	return 0;
}

static inline size_t gen6_get_stolen_size(u16 snb_gmch_ctl)
{
	snb_gmch_ctl >>= SNB_GMCH_GMS_SHIFT;
	snb_gmch_ctl &= SNB_GMCH_GMS_MASK;
	return snb_gmch_ctl << 25; /* 32 MB units */
}

static inline size_t gen8_get_stolen_size(u16 bdw_gmch_ctl)
{
	bdw_gmch_ctl >>= BDW_GMCH_GMS_SHIFT;
	bdw_gmch_ctl &= BDW_GMCH_GMS_MASK;
	return bdw_gmch_ctl << 25; /* 32 MB units */
}

static size_t chv_get_stolen_size(u16 gmch_ctrl)
{
	gmch_ctrl >>= SNB_GMCH_GMS_SHIFT;
	gmch_ctrl &= SNB_GMCH_GMS_MASK;

	/*
	 * 0x0  to 0x10: 32MB increments starting at 0MB
	 * 0x11 to 0x16: 4MB increments starting at 8MB
	 * 0x17 to 0x1d: 4MB increments start at 36MB
	 */
	if (gmch_ctrl < 0x11)
		return gmch_ctrl << 25;
	else if (gmch_ctrl < 0x17)
		return (gmch_ctrl - 0x11 + 2) << 22;
	else
		return (gmch_ctrl - 0x17 + 9) << 22;
}

static size_t gen9_get_stolen_size(u16 gen9_gmch_ctl)
{
	gen9_gmch_ctl >>= BDW_GMCH_GMS_SHIFT;
	gen9_gmch_ctl &= BDW_GMCH_GMS_MASK;

	if (gen9_gmch_ctl < 0xf0)
		return gen9_gmch_ctl << 25; /* 32 MB units */
	else
		/* 4MB increments starting at 0xf0 for 4MB */
		return (gen9_gmch_ctl - 0xf0 + 1) << 22;
}

static int ggtt_probe_common(struct drm_device *dev,
			     size_t gtt_size)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	phys_addr_t gtt_phys_addr;
	int ret;

	/* For Modern GENs the PTEs and register space are split in the BAR */
	gtt_phys_addr = pci_resource_start(dev->pdev, 0) +
		(pci_resource_len(dev->pdev, 0) / 2);

	dev_priv->gtt.gsm = ioremap_wc(gtt_phys_addr, gtt_size);
	if (!dev_priv->gtt.gsm) {
		DRM_ERROR("Failed to map the gtt page table\n");
		return -ENOMEM;
	}

	ret = setup_scratch_page(dev);
	if (ret) {
		DRM_ERROR("Scratch setup failed\n");
		/* iounmap will also get called at remove, but meh */
		iounmap(dev_priv->gtt.gsm);
	}

	return ret;
}

/* The GGTT and PPGTT need a private PPAT setup in order to handle cacheability
 * bits. When using advanced contexts each context stores its own PAT, but
 * writing this data shouldn't be harmful even in those cases. */
static void bdw_setup_private_ppat(struct drm_i915_private *dev_priv)
{
	uint64_t pat;

	pat = GEN8_PPAT(0, GEN8_PPAT_WB | GEN8_PPAT_LLC)     | /* for normal objects, no eLLC */
	      GEN8_PPAT(1, GEN8_PPAT_WC | GEN8_PPAT_LLCELLC) | /* for something pointing to ptes? */
	      GEN8_PPAT(2, GEN8_PPAT_WT | GEN8_PPAT_LLCELLC) | /* for scanout with eLLC */
	      GEN8_PPAT(3, GEN8_PPAT_UC)                     | /* Uncached objects, mostly for scanout */
	      GEN8_PPAT(4, GEN8_PPAT_WB | GEN8_PPAT_LLCELLC | GEN8_PPAT_AGE(0)) |
	      GEN8_PPAT(5, GEN8_PPAT_WB | GEN8_PPAT_LLCELLC | GEN8_PPAT_AGE(1)) |
	      GEN8_PPAT(6, GEN8_PPAT_WB | GEN8_PPAT_LLCELLC | GEN8_PPAT_AGE(2)) |
	      GEN8_PPAT(7, GEN8_PPAT_WB | GEN8_PPAT_LLCELLC | GEN8_PPAT_AGE(3));

	if (!USES_PPGTT(dev_priv->dev))
		/* Spec: "For GGTT, there is NO pat_sel[2:0] from the entry,
		 * so RTL will always use the value corresponding to
		 * pat_sel = 000".
		 * So let's disable cache for GGTT to avoid screen corruptions.
		 * MOCS still can be used though.
		 * - System agent ggtt writes (i.e. cpu gtt mmaps) already work
		 * before this patch, i.e. the same uncached + snooping access
		 * like on gen6/7 seems to be in effect.
		 * - So this just fixes blitter/render access. Again it looks
		 * like it's not just uncached access, but uncached + snooping.
		 * So we can still hold onto all our assumptions wrt cpu
		 * clflushing on LLC machines.
		 */
		pat = GEN8_PPAT(0, GEN8_PPAT_UC);

	/* XXX: spec defines this as 2 distinct registers. It's unclear if a 64b
	 * write would work. */
	I915_WRITE(GEN8_PRIVATE_PAT, pat);
	I915_WRITE(GEN8_PRIVATE_PAT + 4, pat >> 32);
}

static void chv_setup_private_ppat(struct drm_i915_private *dev_priv)
{
	uint64_t pat;

	/*
	 * Map WB on BDW to snooped on CHV.
	 *
	 * Only the snoop bit has meaning for CHV, the rest is
	 * ignored.
	 *
	 * The hardware will never snoop for certain types of accesses:
	 * - CPU GTT (GMADR->GGTT->no snoop->memory)
	 * - PPGTT page tables
	 * - some other special cycles
	 *
	 * As with BDW, we also need to consider the following for GT accesses:
	 * "For GGTT, there is NO pat_sel[2:0] from the entry,
	 * so RTL will always use the value corresponding to
	 * pat_sel = 000".
	 * Which means we must set the snoop bit in PAT entry 0
	 * in order to keep the global status page working.
	 */
	pat = GEN8_PPAT(0, CHV_PPAT_SNOOP) |
	      GEN8_PPAT(1, 0) |
	      GEN8_PPAT(2, 0) |
	      GEN8_PPAT(3, 0) |
	      GEN8_PPAT(4, CHV_PPAT_SNOOP) |
	      GEN8_PPAT(5, CHV_PPAT_SNOOP) |
	      GEN8_PPAT(6, CHV_PPAT_SNOOP) |
	      GEN8_PPAT(7, CHV_PPAT_SNOOP);

	I915_WRITE(GEN8_PRIVATE_PAT, pat);
	I915_WRITE(GEN8_PRIVATE_PAT + 4, pat >> 32);
}

static int gen8_gmch_probe(struct drm_device *dev,
			   size_t *gtt_total,
			   size_t *stolen,
			   phys_addr_t *mappable_base,
			   unsigned long *mappable_end)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	unsigned int gtt_size;
	u16 snb_gmch_ctl;
	int ret;

	/* TODO: We're not aware of mappable constraints on gen8 yet */
	*mappable_base = pci_resource_start(dev->pdev, 2);
	*mappable_end = pci_resource_len(dev->pdev, 2);

	if (!pci_set_dma_mask(dev->pdev, DMA_BIT_MASK(39)))
		pci_set_consistent_dma_mask(dev->pdev, DMA_BIT_MASK(39));

	pci_read_config_word(dev->pdev, SNB_GMCH_CTRL, &snb_gmch_ctl);

	if (INTEL_INFO(dev)->gen >= 9) {
		*stolen = gen9_get_stolen_size(snb_gmch_ctl);
		gtt_size = gen8_get_total_gtt_size(snb_gmch_ctl);
	} else if (IS_CHERRYVIEW(dev)) {
		*stolen = chv_get_stolen_size(snb_gmch_ctl);
		gtt_size = chv_get_total_gtt_size(snb_gmch_ctl);
	} else {
		*stolen = gen8_get_stolen_size(snb_gmch_ctl);
		gtt_size = gen8_get_total_gtt_size(snb_gmch_ctl);
	}

	*gtt_total = (gtt_size / sizeof(gen8_gtt_pte_t)) << PAGE_SHIFT;

	if (IS_CHERRYVIEW(dev))
		chv_setup_private_ppat(dev_priv);
	else
		bdw_setup_private_ppat(dev_priv);

	ret = ggtt_probe_common(dev, gtt_size);

	dev_priv->gtt.base.clear_range = gen8_ggtt_clear_range;
	dev_priv->gtt.base.insert_entries = gen8_ggtt_insert_entries;

	return ret;
}

static int gen6_gmch_probe(struct drm_device *dev,
			   size_t *gtt_total,
			   size_t *stolen,
			   phys_addr_t *mappable_base,
			   unsigned long *mappable_end)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	unsigned int gtt_size;
	u16 snb_gmch_ctl;
	int ret;

	*mappable_base = pci_resource_start(dev->pdev, 2);
	*mappable_end = pci_resource_len(dev->pdev, 2);

	/* 64/512MB is the current min/max we actually know of, but this is just
	 * a coarse sanity check.
	 */
	if ((*mappable_end < (64<<20) || (*mappable_end > (512<<20)))) {
		DRM_ERROR("Unknown GMADR size (%lx)\n",
			  dev_priv->gtt.mappable_end);
		return -ENXIO;
	}

	if (!pci_set_dma_mask(dev->pdev, DMA_BIT_MASK(40)))
		pci_set_consistent_dma_mask(dev->pdev, DMA_BIT_MASK(40));
	pci_read_config_word(dev->pdev, SNB_GMCH_CTRL, &snb_gmch_ctl);

	*stolen = gen6_get_stolen_size(snb_gmch_ctl);

	gtt_size = gen6_get_total_gtt_size(snb_gmch_ctl);
	*gtt_total = (gtt_size / sizeof(gen6_gtt_pte_t)) << PAGE_SHIFT;

	ret = ggtt_probe_common(dev, gtt_size);

	dev_priv->gtt.base.clear_range = gen6_ggtt_clear_range;
	dev_priv->gtt.base.insert_entries = gen6_ggtt_insert_entries;

	return ret;
}

static void gen6_gmch_remove(struct i915_address_space *vm)
{

	struct i915_gtt *gtt = container_of(vm, struct i915_gtt, base);

	iounmap(gtt->gsm);
	teardown_scratch_page(vm->dev);
}

static int i915_gmch_probe(struct drm_device *dev,
			   size_t *gtt_total,
			   size_t *stolen,
			   phys_addr_t *mappable_base,
			   unsigned long *mappable_end)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	int ret;

	ret = intel_gmch_probe(dev_priv->bridge_dev, dev_priv->dev->pdev, NULL);
	if (!ret) {
		DRM_ERROR("failed to set up gmch\n");
		return -EIO;
	}

	intel_gtt_get(gtt_total, stolen, mappable_base, mappable_end);

	dev_priv->gtt.do_idle_maps = needs_idle_maps(dev_priv->dev);
	dev_priv->gtt.base.clear_range = i915_ggtt_clear_range;

	if (unlikely(dev_priv->gtt.do_idle_maps))
		DRM_INFO("applying Ironlake quirks for intel_iommu\n");

	return 0;
}

static void i915_gmch_remove(struct i915_address_space *vm)
{
	intel_gmch_remove();
}

int i915_gem_gtt_init(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct i915_gtt *gtt = &dev_priv->gtt;
	int ret;

	INIT_LIST_HEAD(&gtt->base.vma_list);
	kref_init(&gtt->base.ref);

	if (INTEL_INFO(dev)->gen <= 5) {
		gtt->gtt_probe = i915_gmch_probe;
		gtt->base.cleanup = i915_gmch_remove;
	} else if (INTEL_INFO(dev)->gen < 8) {
		gtt->gtt_probe = gen6_gmch_probe;
		gtt->base.cleanup = gen6_gmch_remove;
		if (IS_HASWELL(dev) && dev_priv->ellc_size)
			gtt->base.pte_encode = iris_pte_encode;
		else if (IS_HASWELL(dev))
			gtt->base.pte_encode = hsw_pte_encode;
		else if (IS_VALLEYVIEW(dev))
			gtt->base.pte_encode = byt_pte_encode;
		else if (INTEL_INFO(dev)->gen >= 7)
			gtt->base.pte_encode = ivb_pte_encode;
		else
			gtt->base.pte_encode = snb_pte_encode;
	} else {
		dev_priv->gtt.gtt_probe = gen8_gmch_probe;
		dev_priv->gtt.base.cleanup = gen6_gmch_remove;
	}

	ret = gtt->gtt_probe(dev, &gtt->base.total, &gtt->stolen_size,
			     &gtt->mappable_base, &gtt->mappable_end);
	if (ret)
		return ret;

	gtt->base.dev = dev;

	/* GMADR is the PCI mmio aperture into the global GTT. */
	DRM_INFO("Memory usable by graphics device = %zdM\n",
		 gtt->base.total >> 20);
	DRM_DEBUG_DRIVER("GMADR size = %ldM\n", gtt->mappable_end >> 20);
	DRM_DEBUG_DRIVER("GTT stolen size = %zdM\n", gtt->stolen_size >> 20);
#ifdef CONFIG_INTEL_IOMMU
	if (intel_iommu_gfx_mapped)
		DRM_INFO("VT-d active for gfx access\n");
#endif
	/*
	 * i915.enable_ppgtt is read-only, so do an early pass to validate the
	 * user's requested state against the hardware/driver capabilities.  We
	 * do this now so that we can print out any log messages once rather
	 * than every time we check intel_enable_ppgtt().
	 */
	i915_module.enable_ppgtt =
	       	sanitize_enable_ppgtt(dev, i915_module.enable_ppgtt);
	DRM_DEBUG_DRIVER("ppgtt mode: %i\n", i915_module.enable_ppgtt);

	return 0;
}

static struct i915_vma *__i915_vma_create(struct drm_i915_gem_object *obj,
					  struct i915_address_space *vm)
{
	struct i915_vma *vma;
	int n;

	vma = kzalloc(sizeof(*vma), GFP_KERNEL);
	if (vma == NULL)
		return ERR_PTR(-ENOMEM);

	kref_init(&vma->kref);
	vma->vm = i915_vm_get(vm);
	vma->obj = obj;
	list_add(&vma->vm_link, &vm->vma_list);

	for (n = 0; n < I915_NUM_ENGINES; n++)
		INIT_LIST_HEAD(&vma->last_read[n].engine_link);

	INIT_LIST_HEAD(&vma->mm_list);
	INIT_LIST_HEAD(&vma->exec_link);

	switch (INTEL_INFO(vm->dev)->gen) {
	case 9:
	case 8:
	case 7:
	case 6:
		if (obj->pde) {
			if (WARN_ON(!i915_is_ggtt(vm)))
				return ERR_PTR(-EINVAL);
			vma->unbind_vma = pde_unbind_vma;
			vma->bind_vma = pde_bind_vma;
		} else if (i915_is_ggtt(vm)) {
			vma->unbind_vma = ggtt_unbind_vma;
			vma->bind_vma = ggtt_bind_vma;
		} else {
			vma->unbind_vma = ppgtt_unbind_vma;
			vma->bind_vma = ppgtt_bind_vma;
		}
		break;
	case 5:
	case 4:
	case 3:
	case 2:
		BUG_ON(!i915_is_ggtt(vm));
		vma->unbind_vma = i915_ggtt_unbind_vma;
		vma->bind_vma = i915_ggtt_bind_vma;
		break;
	default:
		BUG();
	}

	/* Keep GGTT vmas first to make debug easier */
	if (i915_is_ggtt(vm))
		list_add(&vma->obj_link, &obj->vma_list);
	else
		list_add_tail(&vma->obj_link, &obj->vma_list);

	return vma;
}

void __i915_vma_free(struct kref *kref)
{
	struct i915_vma *vma = container_of(kref, typeof(*vma), kref);

	WARN_ON(drm_mm_node_allocated(&vma->node));
	WARN_ON(vma->bound);
	WARN_ON(!list_empty(&vma->mm_list));
	WARN_ON(!list_empty(&vma->exec_link));
	WARN_ON(!list_empty(&vma->obj_link));

	list_del(&vma->vm_link);
	i915_vm_put(vma->vm);
	kfree(vma);
}

struct i915_vma *
i915_gem_obj_get_vma(struct drm_i915_gem_object *obj,
		     struct i915_address_space *vm)
{
	struct i915_vma *vma;

	vma = i915_gem_obj_to_vma(obj, vm);
	if (vma == NULL)
		vma = __i915_vma_create(obj, vm);

	return i915_vma_get(vma);
}
