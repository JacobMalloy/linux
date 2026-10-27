.. SPDX-License-Identifier: GPL-2.0

=============================
Per-process young page bitmap
=============================

``/proc/pid/young_bitmap`` tests and clears the accessed ("young") bit of
the page table entries of a single process, indexed by virtual address.
It is an alternative to :doc:`idle_page_tracking` for users that work with
one process's address space: there is no need to translate virtual addresses
to PFNs through ``/proc/pid/pagemap``, and no reverse-mapping walk to find
every page table entry that maps a page.

The file is available when ``CONFIG_PROC_PAGE_MONITOR`` is enabled. Since both
reading and writing modify the process's page tables, opening it requires
permission to ptrace-attach to the process.

Format
======

The file position is a virtual page number, that is, the virtual address
divided by the page size. The data is a bitmap with one bit per page: bit
``n % 8`` of byte ``n / 8`` describes the page at virtual address
``(pos + n) * PAGE_SIZE``. A transfer of ``count`` bytes therefore covers
``count * 8`` pages, and ``read(2)``/``write(2)`` advance the file position
by ``count * 8``. The position does not need to be aligned. On a
little-endian machine the bitmap can also be accessed as an array of 64-bit
words, with page ``n`` in bit ``n % 64`` of word ``n / 64``.

Reading
=======

Reading tests and clears the accessed bit of every page in the range. A bit
reads as 1 if the page is mapped by a present page table entry that had its
accessed bit set, or if a secondary MMU (for example KVM) reported the page
as young; the accessed bit is then cleared in both. A bit reads as 0
otherwise, including for unmapped addresses, swapped-out pages and
``VM_PFNMAP`` mappings. Each read therefore reports the pages accessed since
the previous read of the same range. A read starting beyond the end of the
process's address space returns 0 (end of file).

Clearing without reading
========================

Writing a bitmap in the same format clears the accessed bit of every page
whose bit is set, without reporting anything; pages whose bit is 0 are not
touched.

Common behaviour
================

Only the page table entry of this process is cleared: other processes mapping
the same page keep their accessed bits. When an accessed bit is cleared the
page is marked young for reclaim (as idle page tracking does), so that
reclaim does not consider the page cold because of the cleared bit.

Limitations:

* A PMD-mapped transparent huge page has a single accessed bit. It reads as
  1 or 0 for all its pages, and reading or selecting any of its pages clears
  it for all of them.
* Hugetlb pages are reported but never cleared.
* On architectures that group page table entries (for example arm64
  contiguous PTEs) clearing one entry may clear the whole group.
* Reclaim and MGLRU also clear accessed bits, so a 0 may mean that the page
  was accessed and then aged by the kernel.

TLB flushing
============

Clearing an accessed bit does not, by itself, remove the translation from the
TLB, and while the translation stays cached the CPU may access the page
without setting the accessed bit again. ``/proc/pid/young_bitmap_flush``
controls whether a read or write of ``young_bitmap`` also flushes the TLB:

``0`` (default)
	The TLB is left alone. This is cheap, but accesses may be missed until
	the TLB entries are evicted.

``1``
	If the read or write cleared any accessed bit, the process's TLB
	entries are flushed on all CPUs once it is done, and secondary MMUs are
	asked to flush as well. Subsequent accesses are guaranteed to set the
	accessed bit.

The setting belongs to the process's address space, so it is shared by all
threads and is not inherited across ``fork(2)``. Reading the file returns the
current value.

Example
=======

::

	int fd = open("/proc/1234/young_bitmap", O_RDONLY);
	uint8_t bits[64];			/* 512 pages */
	off_t pos = (uintptr_t)addr / getpagesize();

	pread(fd, bits, sizeof(bits), pos);	/* reset */
	sleep(1);
	pread(fd, bits, sizeof(bits), pos);	/* pages accessed since */
