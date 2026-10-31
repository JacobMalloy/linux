/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PT_VIEWS_H
#define _LINUX_PT_VIEWS_H

/*
 * Page table views: extra page tables for groups of threads of a process.
 *
 * A view is a second set of page tables for the user half of an mm.  It
 * starts out empty and is filled one PTE at a time from page faults taken
 * by threads running on it, copying the translation from the mm's own page
 * tables (view 0).  It never holds a translation that view 0 does not, and
 * it is invalidated whenever the TLB is: every TLB flush of a range of the
 * mm also clears that range from all views.  A view thus behaves like a TLB
 * that is walked by hardware, and stays coherent for the same reason the
 * TLB does.
 *
 * The point is the accessed bits: the hardware sets them in the page tables
 * it walks, so the bits in a view record the accesses of its threads only.
 * Clearing a view entry keeps its accessed bit in a non-present marker so
 * it is not lost before it is read.
 *
 * Views live as long as the mm.  Threads move onto a view with prctl() or
 * through /proc/<pid>/pt_views/<n>/tasks.
 */

#include <linux/types.h>
#include <linux/atomic.h>
#include <linux/mutex.h>
#include <linux/mm_types.h>
#include <linux/sched.h>

struct mm_struct;
struct task_struct;
struct vm_area_struct;
struct pt_regs;

#define MM_PT_VIEWS_MAX		8	/* including view 0, the mm's own */

#ifdef CONFIG_MM_PT_VIEWS

struct mm_pt_view {
	struct mm_struct *mm;
	pgd_t *pgd;
	int id;
	/* Statistics, see /proc/<pid>/pt_views/<n>/stats */
	atomic_long_t fills;		/* PTEs copied in from view 0 */
	atomic_long_t primary_faults;	/* faults view 0 had to handle first */
	atomic_long_t atomic_fills;	/* fills with page faults disabled */
	atomic_long_t atomic_misses;	/* ... that could not be done */
	atomic_long_t zap_calls;	/* TLB flushes that cleared this view */
	atomic_long_t zapped;		/* present PTEs cleared by them */
};

struct mm_pt_views {
	struct mutex lock;		/* serializes creation */
	struct mm_pt_view *view[MM_PT_VIEWS_MAX];	/* [0] is unused */
};

static inline struct mm_pt_view *pt_view_current(void)
{
	return current->pt_view;
}

static inline bool mm_has_pt_views(struct mm_struct *mm)
{
	return READ_ONCE(mm->pt_views) != NULL;
}

/* The page table @tsk runs on when it uses @mm */
static inline pgd_t *pt_view_pgd(struct mm_struct *mm, struct task_struct *tsk)
{
	struct mm_pt_view *view = tsk ? READ_ONCE(tsk->pt_view) : NULL;

	if (view && view->mm == mm)
		return view->pgd;
	return mm->pgd;
}

struct mm_pt_view *mm_pt_view_get(struct mm_struct *mm, int id);
int mm_pt_view_create(struct mm_struct *mm, int id);
int pt_view_set_current(int id);
int pt_view_move_task(struct task_struct *tsk, struct mm_struct *mm, int id);
void mm_pt_views_free(struct mm_struct *mm);

void __pt_views_zap(struct mm_struct *mm, unsigned long start,
		    unsigned long end);
static inline void pt_views_zap(struct mm_struct *mm, unsigned long start,
				unsigned long end)
{
	if (unlikely(mm_has_pt_views(mm)))
		__pt_views_zap(mm, start, end);
}

vm_fault_t pt_view_handle_fault(struct vm_area_struct *vma,
				unsigned long address, unsigned int flags,
				struct pt_regs *regs);
bool pt_view_fault_atomic(struct mm_struct *mm, unsigned long address,
			  bool write);
void pt_view_fixup_fault(struct mm_struct *mm, unsigned long address,
			 bool write);

/*
 * Report and clear the accessed bits of view @id for the pages in
 * [start, end), setting bit (addr - start) / PAGE_SIZE of @bitmap for each
 * young page (or, if @select, clearing only the pages whose bit is set).
 * Returns whether any bit was cleared.  Called with the mmap lock held.
 */
bool pt_view_young(struct mm_pt_view *view, unsigned long start,
		   unsigned long end, unsigned long *bitmap, bool select);
/* Flush the hardware TLBs of @mm without clearing its views */
void flush_tlb_mm_keep_views(struct mm_struct *mm);
/* Number of view entries that disagree with view 0, for testing */
long pt_view_check(struct mm_pt_view *view);

#else /* !CONFIG_MM_PT_VIEWS */

#define flush_tlb_mm_keep_views(mm)	flush_tlb_mm(mm)

static inline struct mm_pt_view *pt_view_current(void) { return NULL; }
static inline bool mm_has_pt_views(struct mm_struct *mm) { return false; }
static inline void pt_views_zap(struct mm_struct *mm, unsigned long start,
				unsigned long end) { }
static inline void mm_pt_views_free(struct mm_struct *mm) { }
static inline void pt_view_fixup_fault(struct mm_struct *mm,
				       unsigned long address, bool write) { }
static inline bool pt_view_fault_atomic(struct mm_struct *mm,
					unsigned long address, bool write)
{
	return false;
}
static inline vm_fault_t pt_view_handle_fault(struct vm_area_struct *vma,
		unsigned long address, unsigned int flags, struct pt_regs *regs)
{
	return 0;
}

#endif /* CONFIG_MM_PT_VIEWS */

#endif /* _LINUX_PT_VIEWS_H */
