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
with page table isolation (``pti=off``) or broadcast TLB invalidation
(INVLPGB). This is an experimental feature.

How it works
============

The process's own page table is view 0. Every other view starts out empty
and is filled one entry at a time from the page faults of its threads, by
copying the translation from view 0 (and having view 0 handle the fault
first if it lacks one). A view never holds a translation that view 0 does
not: every TLB flush of a range of the process's address space first
clears that range from all views. A view therefore behaves like a TLB that
the hardware walks, and is coherent with view 0 for the same reasons the
TLB is: the kernel never reuses a page before flushing the TLB entries
that point to it.

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
  Creates a view and returns its id.

``prctl(PR_PT_VIEW, PR_PT_VIEW_SET, id, 0, 0)``
  Moves the calling thread onto view ``id`` (0 for the process's own page
  table).

``prctl(PR_PT_VIEW, PR_PT_VIEW_GET, 0, 0, 0)``
  Returns the view of the calling thread.

New threads start on the view of the thread that created them.

/proc/pid/pt_views/
-------------------

``create``
  Writing anything creates a view; reading returns the id of the newest
  view. This lets a tool set up views for an unmodified program.

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
  page faults disabled (``atomic_fills``, ``atomic_misses``), and TLB
  flushes that cleared entries of the view (``zap_calls``, ``zapped``).

``<n>/check``
  Reading walks view ``n`` and returns the number of entries that do not
  match view 0, which must be 0 whenever none of the process's page tables
  is being changed. For testing.

Opening ``young_bitmap``, writing to ``tasks`` or ``create`` requires
permission to ptrace-attach to the process; reading the other files
requires read permission.

Example
=======

To find the heap pages that a JVM's garbage collector threads touched,
without changing the JVM::

	echo 1 > /proc/$pid/pt_views/create
	for t in /proc/$pid/task/*; do
		case $(cat $t/comm) in
		"GC Thread"*|"G1 "*|Z*) echo ${t##*/} > /proc/$pid/pt_views/1/tasks;;
		esac
	done
	echo 1 > /proc/$pid/young_bitmap_flush

then read ``/proc/$pid/pt_views/0/young_bitmap`` and
``/proc/$pid/pt_views/1/young_bitmap`` over the heap periodically.
