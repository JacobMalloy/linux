/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_HUGETLB_H
#define _ASM_X86_HUGETLB_H

#include <asm/page.h>

#ifdef CONFIG_MM_PT_VIEWS
/*
 * Page table views map hugetlb pages with 4K entries, so changes to a
 * hugetlb entry must clear the whole page from them, see pt_views.c.
 */
struct mm_struct;
struct vm_area_struct;
#define __HAVE_ARCH_HUGE_PTEP_GET_AND_CLEAR
pte_t huge_ptep_get_and_clear(struct mm_struct *mm, unsigned long addr,
			      pte_t *ptep, unsigned long sz);
#define __HAVE_ARCH_HUGE_PTEP_CLEAR_FLUSH
pte_t huge_ptep_clear_flush(struct vm_area_struct *vma, unsigned long addr,
			    pte_t *ptep);
#define __HAVE_ARCH_HUGE_PTEP_SET_WRPROTECT
void huge_ptep_set_wrprotect(struct mm_struct *mm, unsigned long addr,
			     pte_t *ptep);
#endif

#include <asm-generic/hugetlb.h>

#define hugepages_supported() boot_cpu_has(X86_FEATURE_PSE)

#endif /* _ASM_X86_HUGETLB_H */
