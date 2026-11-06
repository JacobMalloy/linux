.. SPDX-License-Identifier: GPL-2.0

================
Page table views
================

A page table view is an extra page table for a process's address space
that a group of its threads runs on. Threads on a view see exactly the same
memory as the rest of the process, but the hardware sets accessed bits in
the page table it walks, so the accessed bits of a view record only the
accesses of its own threads. Combined with
:doc:`young_bitmap <young_bitmap>`, this tells the memory use of, say, a
garbage collector's threads apart from that of the application.

Views are available with ``CONFIG_MM_PT_VIEWS``, on x86-64 only, and not
with broadcast TLB invalidation (INVLPGB), or with page table isolation
on 5-level paging. This is an experimental feature.

How it works
============

The process's own page table is view 0. Every other view starts out empty
and is filled one entry at a time from the page faults of its threads, by
copying the translation from view 0 (and having view 0 handle the fault
first if it lacks one). A view never holds a translation that view 0 does
not: whenever an entry of view 0 is cleared or loses a permission, the same
range is cleared from all views, at the same time and under the same locks,
before the TLB flush for the change. A view therefore behaves like a TLB
that the hardware walks, and is coherent with view 0 for the same reasons
the TLB is: the kernel never reuses a page before flushing the TLB entries
that point to it. The cost of clearing views is proportional to what
changed in view 0, also when the process exits.

Some consequences:

- A view entry is writable only if the view 0 entry is writable and
  dirty. A write from a view to a clean page first marks view 0's entry
  dirty, as the processor would, so writeback and reclaim see it.
- View accesses never set view 0's accessed bits, also for new pages that
  a view's fault brings in. Reclaim reads view 0's bits, so it does not see
  views' accesses: a page used only by view threads looks cold to it.
- Transparent huge pages and hugetlb pages are mapped with 4 KiB entries
  in views, so views report accessed bits per 4 KiB page. This also means
  more TLB misses for view threads on such memory.
- When a view entry is cleared, its accessed bit is kept in a non-present
  marker until the next ``young_bitmap`` read, so accesses are not lost to
  unrelated invalidations. Such a marker may be reported for an address
  that has since been unmapped.
- Threads of one process on different views load different page tables,
  so a context switch between them flushes the TLB.
- A view uses page table memory for the parts of the address space its
  threads touched, until the process exits.
- Accesses that the kernel makes through view 0 rather than the current
  page table, such as get_user_pages(), count for view 0.

A process can have up to 7 views besides view 0. Views exist until the
process exits or calls ``execve()``.

Interface
=========

prctl()
-------

``prctl(PR_PT_VIEW, PR_PT_VIEW_CREATE, 0, 0, 0)``
  Creates the lowest-numbered free view and returns its id.

``prctl(PR_PT_VIEW, PR_PT_VIEW_SET, id, 0, 0)``
  Moves the calling thread onto view ``id`` (0 for the process's own page
  table).

``prctl(PR_PT_VIEW, PR_PT_VIEW_GET, 0, 0, 0)``
  Returns the view of the calling thread.

New threads start on the view of the thread that created them.

/proc/pid/pt_views/
-------------------

``mkdir /proc/pid/pt_views/<n>``
  Creates view ``n`` (1 to 7), so that a tool can set up views for an
  unmodified program. Fails with ``EEXIST`` if the view exists and
  ``EINVAL`` if ``n`` is not a view id. Views cannot be removed; they last
  as long as the process's address space.

``<n>/young_bitmap``
  Like ``/proc/pid/young_bitmap``, but for the accessed bits of view ``n``
  only. ``0/young_bitmap`` is ``/proc/pid/young_bitmap``. The flags in
  ``/proc/pid/young_bitmap_flush`` apply to all views; flushing the TLB
  after a read does not clear views, since only accessed bits change.

``<n>/tasks``
  The ids of the threads on view ``n``, one per line. Writing a thread id
  moves that thread of the process onto view ``n``; the move takes effect
  the next time the thread returns to user space.

``<n>/stats``
  Counters for view ``n``: entries copied from view 0 (``fills``), faults
  view 0 had to handle first (``primary_faults``), fills from faults with
  page faults disabled (``atomic_fills``, ``atomic_misses``), changes to
  view 0 that cleared entries of the view and the entries they cleared
  (``zap_calls``, ``zapped``), and, with ``CONFIG_DEBUG_PT_VIEWS``, stale
  entries found at TLB flushes (``flush_stale``, always 0 without it).

``<n>/check``
  Reading walks view ``n`` and returns the number of entries that do not
  match view 0, which must be 0 whenever none of the process's page tables
  is being changed. For testing.

Creating a view, opening ``young_bitmap`` or writing to ``tasks`` requires
permission to ptrace-attach to the process; reading the other files
requires read permission.

Example
=======

To find the heap pages that a JVM's garbage collector threads touched,
without changing the JVM::

	mkdir /proc/$pid/pt_views/1
	for t in /proc/$pid/task/*; do
		case $(cat $t/comm) in
		"GC Thread"*|"G1 "*|Z*) echo ${t##*/} > /proc/$pid/pt_views/1/tasks;;
		esac
	done
	echo 1 > /proc/$pid/young_bitmap_flush

then read ``/proc/$pid/pt_views/0/young_bitmap`` and
``/proc/$pid/pt_views/1/young_bitmap`` over the heap periodically.
