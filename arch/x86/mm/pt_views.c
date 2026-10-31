// SPDX-License-Identifier: GPL-2.0
/*
 * Page table views, see include/linux/pt_views.h.
 *
 * Coherence: a view entry is a copy of a view 0 translation, made under the
 * view's PTE lock while reading view 0 locklessly (like GUP-fast).  Whoever
 * changes or removes a view 0 translation flushes the TLB for it before the
 * old page can be reused, and every flush first clears the range from all
 * views under the same PTE locks.  So a copy made before the change is
 * cleared by that flush, and a copy made after it sees the new translation:
 * views are exactly as coherent as the TLB.  Permission upgrades are not
 * flushed; like the CPU, a view refetches the entry on the resulting fault.
 *
 * Dirty bits: the kernel reads them from view 0, so a view entry is only
 * writable if the view 0 entry is writable and already dirty.  A write
 * fault on a clean entry sets the dirty bit in view 0 first, atomically, as
 * the CPU would.  Accessed bits are never copied either way: the point of
 * a view is that its accessed bits are its own.
 *
 * View page tables are only freed with the mm, so walking them needs no
 * lock but the PTE lock of the table being changed.
 */
#include <linux/mm.h>
#include <linux/pt_views.h>
#include <linux/sched/mm.h>
#include <linux/slab.h>
#include <linux/task_work.h>
#include <asm/mmu_context.h>
#include <asm/pgalloc.h>
#include <asm/tlbflush.h>

/*
 * View PTE locks are taken while view 0 page table locks are held (by the
 * TLB flush that clears views), so they need their own lockdep class.
 */
static struct lock_class_key pt_view_ptl_key;

struct mm_pt_view *mm_pt_view_get(struct mm_struct *mm, int id)
{
	struct mm_pt_views *views = smp_load_acquire(&mm->pt_views);

	if (!views || id <= 0 || id >= MM_PT_VIEWS_MAX)
		return NULL;
	return smp_load_acquire(&views->view[id]);
}

int mm_pt_view_create(struct mm_struct *mm)
{
	struct mm_pt_views *views, *new;
	struct mm_pt_view *view;
	int id, ret;

	/*
	 * PTI would need a user copy of every view PGD, and broadcast TLB
	 * flushing keeps ASIDs valid across CR3 loads, which views rely on to
	 * not share TLB entries.
	 */
	if (boot_cpu_has(X86_FEATURE_PTI) ||
	    cpu_feature_enabled(X86_FEATURE_INVLPGB))
		return -EOPNOTSUPP;

	views = smp_load_acquire(&mm->pt_views);
	if (!views) {
		new = kzalloc(sizeof(*new), GFP_KERNEL_ACCOUNT);
		if (!new)
			return -ENOMEM;
		mutex_init(&new->lock);
		views = cmpxchg_release(&mm->pt_views, NULL, new);
		if (views)
			kfree(new);
		else
			views = new;
	}

	mutex_lock(&views->lock);
	for (id = 1; id < MM_PT_VIEWS_MAX; id++)
		if (!views->view[id])
			break;
	ret = -ENOSPC;
	if (id == MM_PT_VIEWS_MAX)
		goto unlock;

	ret = -ENOMEM;
	view = kzalloc(sizeof(*view), GFP_KERNEL_ACCOUNT);
	if (!view)
		goto unlock;
	view->mm = mm;
	view->id = id;
	view->pgd = pgd_alloc_view(mm);
	if (!view->pgd) {
		kfree(view);
		goto unlock;
	}
	smp_store_release(&views->view[id], view);
	ret = id;
unlock:
	mutex_unlock(&views->lock);
	return ret;
}

/* Move current onto @view (NULL for view 0) and load its page table */
static void pt_view_switch(struct mm_pt_view *view)
{
	WRITE_ONCE(current->pt_view, view);
	switch_mm(current->mm, current->mm, current);
}

int pt_view_set_current(int id)
{
	struct mm_struct *mm = current->mm;
	struct mm_pt_view *view = NULL;

	if (!mm)
		return -EINVAL;
	if (id) {
		view = mm_pt_view_get(mm, id);
		if (!view)
			return -EINVAL;
	}
	pt_view_switch(view);
	return 0;
}

struct pt_view_move {
	struct callback_head work;
	struct mm_struct *mm;
	struct mm_pt_view *view;
};

static void pt_view_move_fn(struct callback_head *work)
{
	struct pt_view_move *move = container_of(work, struct pt_view_move, work);

	/* The view belongs to the mm the thread had when the move was asked */
	if (current->mm == move->mm)
		pt_view_switch(move->view);
	kfree(move);
}

/* Move @tsk, a thread using @mm, onto view @id the next time it runs */
int pt_view_move_task(struct task_struct *tsk, struct mm_struct *mm, int id)
{
	struct mm_pt_view *view = NULL;
	struct pt_view_move *move;

	if (id) {
		view = mm_pt_view_get(mm, id);
		if (!view)
			return -EINVAL;
	}
	if (tsk == current) {
		if (current->mm != mm)
			return -EINVAL;
		pt_view_switch(view);
		return 0;
	}
	if (tsk->flags & PF_KTHREAD)
		return -EINVAL;

	move = kmalloc(sizeof(*move), GFP_KERNEL);
	if (!move)
		return -ENOMEM;
	move->mm = mm;
	move->view = view;
	init_task_work(&move->work, pt_view_move_fn);
	if (task_work_add(tsk, &move->work, TWA_RESUME)) {
		kfree(move);
		return -ESRCH;
	}
	return 0;
}

/* Install a new, empty PTE table in the view PMD entry @pmd */
static int pt_view_pte_alloc(struct mm_struct *mm, pmd_t *pmd)
{
	pgtable_t new = pte_alloc_one(mm);
	spinlock_t *ptl;

	if (!new)
		return -ENOMEM;
	if (IS_ENABLED(CONFIG_SPLIT_PTE_PTLOCKS))
		lockdep_set_class(ptlock_ptr(page_ptdesc(new)),
				  &pt_view_ptl_key);

	ptl = pmd_lock(mm, pmd);
	if (likely(pmd_none(*pmd))) {
		mm_inc_nr_ptes(mm);
		smp_wmb(); /* See comment in pmd_install() */
		pmd_populate(mm, pmd, new);
		new = NULL;
	}
	spin_unlock(ptl);
	if (new)
		pte_free(mm, new);
	return 0;
}

/*
 * Return the view PTE for @addr, mapped and locked, or NULL if it has no
 * page table and @alloc is false, or an ERR_PTR if allocation failed.
 */
static pte_t *pt_view_pte_lock(struct mm_pt_view *view, unsigned long addr,
			       bool alloc, spinlock_t **ptlp)
{
	struct mm_struct *mm = view->mm;
	pgd_t *pgd = pgd_offset_pgd(view->pgd, addr);
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;

	if (alloc) {
		p4d = p4d_alloc(mm, pgd, addr);
		if (!p4d)
			return ERR_PTR(-ENOMEM);
		pud = pud_alloc(mm, p4d, addr);
		if (!pud)
			return ERR_PTR(-ENOMEM);
		pmd = pmd_alloc(mm, pud, addr);
		if (!pmd)
			return ERR_PTR(-ENOMEM);
		if (pmd_none(pmdp_get(pmd)) && pt_view_pte_alloc(mm, pmd))
			return ERR_PTR(-ENOMEM);
	} else {
		if (pgd_none(pgdp_get(pgd)))
			return NULL;
		p4d = p4d_offset(pgd, addr);
		if (p4d_none(p4dp_get(p4d)))
			return NULL;
		pud = pud_offset(p4d, addr);
		if (pud_none(pudp_get(pud)))
			return NULL;
		pmd = pmd_offset(pud, addr);
		if (pmd_none(pmdp_get(pmd)))
			return NULL;
	}
	return pte_offset_map_lock(mm, pmd, addr, ptlp);
}

#define PT_VIEW_CHANGED	(~0UL)	/* view 0 entry changed under us */

/* What a lookup is for */
#define PTV_WRITE	0x1	/* a write: make sure view 0 is dirty */
#define PTV_EXEC	0x2	/* an instruction fetch */
#define PTV_OLD		0x4	/* clear view 0's accessed bit */
#define PTV_RAW		0x8	/* just return view 0's entry, if present */

/*
 * Check the access against the view 0 entry at @entryp, whose value was
 * @val, and return the value to copy, 0 if view 0 must handle a fault
 * first, or PT_VIEW_CHANGED if the entry changed and must be looked up
 * again (it may not even be a leaf any more).  For a write to a clean
 * entry, set its dirty bit as the CPU would.
 */
static unsigned long pt_view_access(unsigned long *entryp, unsigned long val,
				    unsigned int how)
{
	if (!(val & _PAGE_PRESENT) || !(val & _PAGE_USER))
		return 0;
	if (how & PTV_RAW)
		return val;
	if ((how & PTV_EXEC) && (val & _PAGE_NX))
		return 0;
	if ((how & PTV_WRITE) && !(val & _PAGE_DIRTY)) {
		if (!(val & _PAGE_RW))
			return 0;
		if (!try_cmpxchg(entryp, &val, val | _PAGE_DIRTY))
			return PT_VIEW_CHANGED;
		val |= _PAGE_DIRTY;
	}
	/* (A read-only dirty entry is a shadow stack entry: copy it as is) */
	if ((how & PTV_OLD) && (val & _PAGE_ACCESSED))
		clear_bit(_PAGE_BIT_ACCESSED, entryp);
	return val;
}

/* The 4K PTE for @addr within a huge entry with flags @flags and @pfn */
static pte_t pt_view_huge_pte(unsigned long pfn, unsigned long flags,
			      unsigned long addr, unsigned long mask)
{
	pfn += (addr & ~mask) >> PAGE_SHIFT;
	return pfn_pte(pfn, pgprot_large_2_4k(__pgprot(flags & ~_PAGE_PSE)));
}

/*
 * Look up @addr in view 0 and return the PTE a view should copy, or a zero
 * PTE if view 0 must handle a fault first.  View 0 page tables can be
 * freed under us, so this runs with interrupts off like GUP-fast.
 */
static pte_t __pt_view_lookup(struct mm_struct *mm, unsigned long addr,
			      unsigned int how)
{
	pgd_t *pgdp, pgd;
	p4d_t *p4dp, p4d;
	pud_t *pudp, pud;
	pmd_t *pmdp, pmd;
	pte_t *ptep, pte;
	unsigned long val;

	lockdep_assert_irqs_disabled();

	pgdp = pgd_offset(mm, addr);
	pgd = pgdp_get(pgdp);
	if (!(pgd_flags(pgd) & _PAGE_PRESENT))
		return __pte(0);
	p4dp = p4d_offset_lockless(pgdp, pgd, addr);
	p4d = p4dp_get(p4dp);
	if (!(p4d_flags(p4d) & _PAGE_PRESENT))
		return __pte(0);
	pudp = pud_offset_lockless(p4dp, p4d, addr);
	pud = pudp_get(pudp);
	if (!(pud_flags(pud) & _PAGE_PRESENT))
		return __pte(0);
	if (pud_leaf(pud)) {
		val = pt_view_access((unsigned long *)pudp, pud_val(pud), how);
		if (!val || val == PT_VIEW_CHANGED || (how & PTV_RAW))
			return __pte(val);
		return pt_view_huge_pte(pud_pfn(__pud(val)),
					pud_flags(__pud(val)), addr, PUD_MASK);
	}
	pmdp = pmd_offset_lockless(pudp, pud, addr);
	pmd = pmdp_get_lockless(pmdp);
	if (!(pmd_flags(pmd) & _PAGE_PRESENT))
		return __pte(0);
	if (pmd_leaf(pmd)) {
		val = pt_view_access((unsigned long *)pmdp, pmd_val(pmd), how);
		if (!val || val == PT_VIEW_CHANGED || (how & PTV_RAW))
			return __pte(val);
		return pt_view_huge_pte(pmd_pfn(__pmd(val)),
					pmd_flags(__pmd(val)), addr, PMD_MASK);
	}
	ptep = pte_offset_map(pmdp, addr);
	if (!ptep)
		return __pte(0);
	pte = ptep_get(ptep);
	val = pt_view_access((unsigned long *)ptep, pte_val(pte), how);
	pte_unmap(ptep);
	return __pte(val);
}

/*
 * Look up @addr in view 0 for an access described by @how: return the PTE
 * a view should copy, or a zero PTE if view 0 must handle a fault first.
 * With PTV_RAW, return view 0's entry (PTE, PMD or PUD) if it is present.
 */
static pte_t pt_view_lookup(struct mm_struct *mm, unsigned long addr,
			    unsigned int how)
{
	unsigned long flags;
	pte_t pte;

	local_irq_save(flags);
	do {
		pte = __pt_view_lookup(mm, addr, how);
	} while (pte_val(pte) == PT_VIEW_CHANGED);
	local_irq_restore(flags);
	return pte;
}

/* The view entry for view 0 entry @src: never accessed, writable only if dirty */
static pteval_t pt_view_entry(pte_t src)
{
	pteval_t val = pte_val(src) & ~_PAGE_ACCESSED;

	if ((val & _PAGE_RW) && !(val & _PAGE_DIRTY))
		val &= ~_PAGE_RW;
	return val;
}

/*
 * Copy the view 0 translation of @addr into @view for an access described
 * by @how.  Returns 0 if done, 1 if view 0 must handle a fault first,
 * -EAGAIN if @atomic and the view has no page table there yet, or -ENOMEM.
 */
static int pt_view_fill(struct mm_pt_view *view, unsigned long addr,
			unsigned int how, bool atomic)
{
	spinlock_t *ptl;
	pte_t *ptep, src, old;

	addr &= PAGE_MASK;
	ptep = pt_view_pte_lock(view, addr, !atomic, &ptl);
	if (IS_ERR(ptep))
		return PTR_ERR(ptep);
	if (!ptep)
		return -EAGAIN;

	/* Under the view PTE lock, see the comment at the top */
	src = pt_view_lookup(view->mm, addr, how);
	if (!pte_val(src)) {
		pte_unmap_unlock(ptep, ptl);
		return 1;
	}

	/* Keep the accessed bit of what was there, present or not */
	old = native_ptep_get_and_clear(ptep);
	native_set_pte(ptep, __pte(pt_view_entry(src) |
				   (pte_val(old) & _PAGE_ACCESSED)));
	pte_unmap_unlock(ptep, ptl);
	atomic_long_inc(&view->fills);
	return 0;
}

vm_fault_t pt_view_handle_fault(struct vm_area_struct *vma,
				unsigned long address, unsigned int flags,
				struct pt_regs *regs)
{
	struct mm_pt_view *view = current->pt_view;
	unsigned int how = 0;
	vm_fault_t fault;
	int tries, ret;
	pte_t before;

	if (flags & FAULT_FLAG_WRITE)
		how |= PTV_WRITE;
	if (flags & FAULT_FLAG_INSTRUCTION)
		how |= PTV_EXEC;
	for (tries = 0; ; tries++) {
		ret = pt_view_fill(view, address, how, false);
		if (ret == 0)
			break;
		if (ret < 0)
			return VM_FAULT_OOM;
		/* Let the CPU retry rather than loop here forever */
		if (tries == 2)
			break;

		/*
		 * Have view 0 handle the fault.  New entries are born young
		 * on x86, which would count this view's access in view 0:
		 * clear the bit again when copying, unless it was set before.
		 */
		before = pt_view_lookup(vma->vm_mm, address, PTV_RAW);
		if (!(pte_val(before) & _PAGE_ACCESSED))
			how |= PTV_OLD;
		atomic_long_inc(&view->primary_faults);
		fault = handle_mm_fault(vma, address, flags, regs);
		/*
		 * Anything but a plain success may have dropped the mmap lock
		 * or left nothing to copy: let the caller deal with it.
		 */
		if (fault & ~VM_FAULT_MAJOR)
			return fault;
	}
	return 0;
}

/*
 * Fill for a fault with page faults disabled, e.g. a futex operation or
 * copy_from_user_inatomic().  Without this, the caller would fault the page
 * in through view 0 (with GUP) and retry forever.  Only works where the view
 * already has a page table, as nothing may sleep.
 */
bool pt_view_fault_atomic(struct mm_struct *mm, unsigned long address,
			  bool write)
{
	struct mm_pt_view *view = current->pt_view;
	int ret;

	if (in_interrupt() || !view || view->mm != mm)
		return false;
	ret = pt_view_fill(view, address, write ? PTV_WRITE : 0, true);
	if (ret)
		atomic_long_inc(&view->atomic_misses);
	else
		atomic_long_inc(&view->atomic_fills);
	return ret == 0;
}

/*
 * After a fault was handled in view 0 on behalf of current without going
 * through the page fault handler (fixup_user_fault(), GUP), copy the result
 * into current's view so that the access the caller retries succeeds.
 */
void pt_view_fixup_fault(struct mm_struct *mm, unsigned long address,
			 bool write)
{
	struct mm_pt_view *view = current->pt_view;

	if (view && view->mm == mm)
		pt_view_fill(view, address, write ? PTV_WRITE : 0, false);
}

/*
 * Walk the PTE tables of @view over [addr, end), calling @fn with each PTE
 * table locked.  @fn returns a count, and the sum is returned.
 */
typedef long (*pt_view_pte_fn)(struct mm_pt_view *view, pte_t *pte,
			       unsigned long addr, unsigned long end,
			       void *arg);

static long pt_view_walk_pmd(struct mm_pt_view *view, pud_t *pud,
			     unsigned long addr, unsigned long end,
			     pt_view_pte_fn fn, void *arg)
{
	pmd_t *pmd = pmd_offset(pud, addr);
	unsigned long next;
	spinlock_t *ptl;
	pte_t *pte;
	long n = 0;

	do {
		next = pmd_addr_end(addr, end);
		if (pmd_none(pmdp_get(pmd)))
			continue;
		pte = pte_offset_map_lock(view->mm, pmd, addr, &ptl);
		if (!pte)
			continue;
		n += fn(view, pte, addr, next, arg);
		pte_unmap_unlock(pte, ptl);
	} while (pmd++, addr = next, addr != end);
	return n;
}

static long pt_view_walk(struct mm_pt_view *view, unsigned long addr,
			 unsigned long end, pt_view_pte_fn fn, void *arg)
{
	pgd_t *pgd = pgd_offset_pgd(view->pgd, addr);
	unsigned long pgd_next, p4d_next, next;
	p4d_t *p4d;
	pud_t *pud;
	long n = 0;

	do {
		pgd_next = pgd_addr_end(addr, end);
		if (pgd_none(pgdp_get(pgd)))
			continue;
		p4d = p4d_offset(pgd, addr);
		do {
			p4d_next = p4d_addr_end(addr, pgd_next);
			if (p4d_none(p4dp_get(p4d)))
				continue;
			pud = pud_offset(p4d, addr);
			do {
				next = pud_addr_end(addr, p4d_next);
				if (pud_none(pudp_get(pud)))
					continue;
				n += pt_view_walk_pmd(view, pud, addr, next,
						      fn, arg);
			} while (pud++, addr = next, addr != p4d_next);
		} while (p4d++, addr = p4d_next, addr != pgd_next);
	} while (pgd++, addr = pgd_next, addr != end);
	return n;
}

/* Clamp a TLB flush range to the user address space; false if empty */
static bool pt_view_range(unsigned long *start, unsigned long *end)
{
	*start &= PAGE_MASK;
	if (*end > TASK_SIZE_MAX || *end < *start)
		*end = TASK_SIZE_MAX;
	*end = PAGE_ALIGN(*end);
	return *start < *end;
}

static long pt_view_zap_ptes(struct mm_pt_view *view, pte_t *pte,
			     unsigned long addr, unsigned long end, void *arg)
{
	long n = 0;
	pte_t old;

	for (; addr != end; pte++, addr += PAGE_SIZE) {
		if (!(pte_flags(ptep_get(pte)) & _PAGE_PRESENT))
			continue;
		/* Leave a marker with the accessed bit, see pt_view_young() */
		old = native_ptep_get_and_clear(pte);
		if (pte_val(old) & _PAGE_ACCESSED)
			native_set_pte(pte, __pte(_PAGE_ACCESSED));
		n++;
	}
	return n;
}

/*
 * Called for every TLB flush of @mm, before the hardware flush, with
 * whatever locks the caller holds for changing view 0.
 */
void __pt_views_zap(struct mm_struct *mm, unsigned long start,
		    unsigned long end)
{
	struct mm_pt_views *views = smp_load_acquire(&mm->pt_views);
	struct mm_pt_view *view;
	int id;

	if (!views || !pt_view_range(&start, &end))
		return;
	for (id = 1; id < MM_PT_VIEWS_MAX; id++) {
		view = smp_load_acquire(&views->view[id]);
		if (!view)
			continue;
		atomic_long_inc(&view->zap_calls);
		atomic_long_add(pt_view_walk(view, start, end,
					     pt_view_zap_ptes, NULL),
				&view->zapped);
	}
}

struct pt_view_young_arg {
	unsigned long start;
	unsigned long *bitmap;
	bool select;
};

static long pt_view_young_ptes(struct mm_pt_view *view, pte_t *pte,
			       unsigned long addr, unsigned long end, void *arg)
{
	struct pt_view_young_arg *ya = arg;
	unsigned long idx;
	long cleared = 0;
	pteval_t val;
	bool young;

	for (; addr != end; pte++, addr += PAGE_SIZE) {
		idx = (addr - ya->start) >> PAGE_SHIFT;
		if (ya->select && !test_bit_le(idx, ya->bitmap))
			continue;
		val = pte_val(ptep_get(pte));
		if (!(val & _PAGE_ACCESSED))
			continue;
		if (val & _PAGE_PRESENT) {
			young = test_and_clear_bit(_PAGE_BIT_ACCESSED,
						   (unsigned long *)&pte->pte);
		} else {
			/* A marker left by pt_view_zap_ptes() */
			native_set_pte(pte, __pte(0));
			young = true;
		}
		if (young) {
			cleared++;
			if (!ya->select)
				__set_bit_le(idx, ya->bitmap);
		}
	}
	return cleared;
}

bool pt_view_young(struct mm_pt_view *view, unsigned long start,
		   unsigned long end, unsigned long *bitmap, bool select)
{
	struct pt_view_young_arg ya = {
		.start = start, .bitmap = bitmap, .select = select,
	};

	if (end > TASK_SIZE_MAX)
		end = TASK_SIZE_MAX;
	if (start >= end)
		return false;
	return pt_view_walk(view, start, end, pt_view_young_ptes, &ya) > 0;
}

static long pt_view_check_ptes(struct mm_pt_view *view, pte_t *pte,
			       unsigned long addr, unsigned long end, void *arg)
{
	pte_t src, cur;
	long bad = 0;

	for (; addr != end; pte++, addr += PAGE_SIZE) {
		cur = ptep_get(pte);
		if (!(pte_flags(cur) & _PAGE_PRESENT))
			continue;
		src = pt_view_lookup(view->mm, addr, 0);
		if (pte_val(src) && pte_pfn(src) == pte_pfn(cur) &&
		    (!(pte_flags(cur) & _PAGE_RW) ||
		     (pte_flags(src) & (_PAGE_RW | _PAGE_DIRTY)) ==
		     (_PAGE_RW | _PAGE_DIRTY)))
			continue;
		if (bad++ < 4)
			pr_warn("pt_view %d: stale entry at %#lx: view %#lx, view 0 %#lx\n",
				view->id, addr, (unsigned long)pte_val(cur),
				(unsigned long)pte_val(src));
	}
	return bad;
}

long pt_view_check(struct mm_pt_view *view)
{
	return pt_view_walk(view, 0, TASK_SIZE_MAX, pt_view_check_ptes, NULL);
}

/* Free the user half of a view's page tables; no CPU may be using it */
static void pt_view_free_tables(struct mm_pt_view *view)
{
	struct mm_struct *mm = view->mm;
	unsigned long nr_pgd = pgd_index(TASK_SIZE_MAX - 1) + 1;
	unsigned long i, j, k, l, nr_p4d;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;

	nr_p4d = pgtable_l5_enabled() ? PTRS_PER_P4D : 1;
	for (i = 0; i < nr_pgd; i++) {
		if (pgd_none(view->pgd[i]))
			continue;
		p4d = p4d_offset(&view->pgd[i], 0);
		for (j = 0; j < nr_p4d; j++) {
			if (p4d_none(p4d[j]))
				continue;
			pud = pud_offset(&p4d[j], 0);
			for (k = 0; k < PTRS_PER_PUD; k++) {
				if (pud_none(pud[k]))
					continue;
				pmd = pmd_offset(&pud[k], 0);
				for (l = 0; l < PTRS_PER_PMD; l++) {
					if (pmd_none(pmd[l]))
						continue;
					pte_free(mm, pmd_pgtable(pmd[l]));
					mm_dec_nr_ptes(mm);
				}
				pmd_free(mm, pmd);
				mm_dec_nr_pmds(mm);
			}
			pud_free(mm, pud);
			mm_dec_nr_puds(mm);
		}
		if (pgtable_l5_enabled())
			p4d_free(mm, p4d);
	}
}

/* Called from __mmdrop(), once no CPU can have any view loaded */
void mm_pt_views_free(struct mm_struct *mm)
{
	struct mm_pt_views *views = mm->pt_views;
	struct mm_pt_view *view;
	int id;

	if (!views)
		return;
	for (id = 1; id < MM_PT_VIEWS_MAX; id++) {
		view = views->view[id];
		if (!view)
			continue;
		pt_view_free_tables(view);
		pgd_free_view(mm, view->pgd);
		kfree(view);
	}
	kfree(views);
	mm->pt_views = NULL;
}
