// SPDX-License-Identifier: GPL-2.0
/*
 * Tests for page table views (CONFIG_MM_PT_VIEWS): PR_PT_VIEW and
 * /proc/<pid>/pt_views/.
 *
 * A view is a cache of the process's page tables that threads on it run
 * on.  The tests check that the accessed bits of each view record only its
 * own threads' accesses, and mostly that a view never keeps a translation
 * that the process's page tables (view 0) no longer have: after munmap,
 * mremap, mprotect, fork, MADV_DONTNEED/FREE/PAGEOUT, migration, KSM, THP
 * split and collapse, truncation, and so on.  A stale entry shows up as a
 * view thread seeing old data, or being able to write where it should
 * fault, and in the "check" file, which counts view entries that disagree
 * with view 0.
 *
 * Each test runs in a child process of its own.  Some need root and
 * hardware or setup that the QEMU init in this directory provides (zram
 * swap, hugetlb pages, two NUMA nodes) and are skipped otherwise.
 *
 * usage: proc-pt-views [test name...]
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <linux/userfaultfd.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <time.h>
#include <unistd.h>

#ifndef PR_PT_VIEW
#define PR_PT_VIEW		0x50545657
#define PR_PT_VIEW_CREATE	0
#define PR_PT_VIEW_SET		1
#define PR_PT_VIEW_GET		2
#endif
#define MM_PT_VIEWS_MAX		8

#ifndef MADV_PAGEOUT
#define MADV_PAGEOUT	21
#endif
#ifndef MADV_COLLAPSE
#define MADV_COLLAPSE	25
#endif
#ifndef MPOL_MF_MOVE
#define MPOL_MF_MOVE	(1 << 1)
#endif

#define PS		4096UL
#define HPS		(2UL << 20)
#define PASS		0
#define FAIL		1
#define SKIP		4

static int failures;
static bool verbose;
static bool no_views;	/* run "view" threads on view 0, to test the tests */

#define CHECK(cond, ...) do {						\
	if (!(cond)) {							\
		printf("    FAIL %s:%d: %s: ", __func__, __LINE__, #cond);	\
		printf(__VA_ARGS__);					\
		printf("\n");						\
		fflush(stdout);						\
		failures++;						\
	}								\
} while (0)

static void die(const char *what)
{
	perror(what);
	exit(FAIL);
}

static void note(const char *fmt, ...)
{
	va_list ap;

	printf("    ");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	fflush(stdout);
}

/* ---- views ---- */

static int view_create(void)
{
	int id;

	if (no_views)
		return 0;
	id = prctl(PR_PT_VIEW, PR_PT_VIEW_CREATE, 0, 0, 0);

	if (id < 0)
		die("PR_PT_VIEW_CREATE");
	return id;
}

static int view_get(void)
{
	return prctl(PR_PT_VIEW, PR_PT_VIEW_GET, 0, 0, 0);
}

static void view_set(int id)
{
	if (prctl(PR_PT_VIEW, PR_PT_VIEW_SET, id, 0, 0))
		die("PR_PT_VIEW_SET");
}

static char *view_path(char *buf, int id, const char *file)
{
	sprintf(buf, "/proc/self/pt_views/%d/%s", id, file);
	return buf;
}

/* Read the young bitmap of view @id for @npages pages at @addr */
static void read_young(int id, void *addr, size_t npages, uint8_t *bm)
{
	char path[64];
	int fd = open(view_path(path, id, "young_bitmap"), O_RDWR);
	ssize_t n;

	if (fd < 0)
		die(path);
	memset(bm, 0, (npages + 7) / 8);
	n = pread(fd, bm, (npages + 7) / 8, (uintptr_t)addr / PS);
	if (n != (ssize_t)((npages + 7) / 8))
		die("pread young_bitmap");
	close(fd);
}

static bool bit(const uint8_t *bm, size_t i)
{
	return bm[i / 8] >> (i % 8) & 1;
}

static long read_long(const char *path)
{
	char buf[64] = "";
	int fd = open(path, O_RDONLY);
	ssize_t n;

	if (fd < 0)
		return -1;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	return n > 0 ? atol(buf) : -1;
}

static void write_str(const char *path, const char *s)
{
	int fd = open(path, O_WRONLY);

	if (fd < 0 || write(fd, s, strlen(s)) != (ssize_t)strlen(s))
		die(path);
	close(fd);
}

static bool try_write_str(const char *path, const char *s)
{
	int fd = open(path, O_WRONLY);
	bool ok;

	if (fd < 0)
		return false;
	ok = write(fd, s, strlen(s)) == (ssize_t)strlen(s);
	close(fd);
	return ok;
}

/* Number of stale entries in view @id, which must be 0 when quiescent */
static long view_check(int id)
{
	char path[64];

	if (no_views)
		return 0;
	return read_long(view_path(path, id, "check"));
}

#define CHECK_VIEW(id) do {						\
	long _bad = view_check(id);					\
	CHECK(_bad == 0, "view %d has %ld stale entries", id, _bad);	\
} while (0)

static long view_stat(int id, const char *name)
{
	char path[64], line[128];
	long val = -1;
	FILE *f = fopen(view_path(path, id, "stats"), "r");

	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f))
		if (!strncmp(line, name, strlen(name)) &&
		    line[strlen(name)] == ' ')
			val = atol(line + strlen(name) + 1);
	fclose(f);
	return val;
}

/* ---- running code on a view ---- */

struct vjob {
	int view;
	void (*fn)(void *);
	void *arg;
};

static void *vthread(void *p)
{
	struct vjob *job = p;

	if (job->view)
		view_set(job->view);
	job->fn(job->arg);
	return NULL;
}

/* Run fn(arg) in a new thread on view @view and wait for it */
static void in_view(int view, void (*fn)(void *), void *arg)
{
	struct vjob job = { view, fn, arg };
	pthread_t t;

	if (pthread_create(&t, NULL, vthread, &job))
		die("pthread_create");
	pthread_join(t, NULL);
}

/* Faults expected by a test: probe_*() return the signal, or 0 */
static __thread sigjmp_buf probe_jmp;
static __thread volatile int probing;

static void probe_handler(int sig, siginfo_t *si, void *uc)
{
	ucontext_t *ctx = uc;
	char msg[160];
	int len;

	if (probing)
		siglongjmp(probe_jmp, sig);
	len = snprintf(msg, sizeof(msg),
		       "    unexpected signal %d code %d at %p ip %#llx sp %#llx tid %d view %d\n",
		       sig, si->si_code, si->si_addr,
		       (unsigned long long)ctx->uc_mcontext.gregs[REG_RIP],
		       (unsigned long long)ctx->uc_mcontext.gregs[REG_RSP],
		       gettid(), view_get());
	if (write(1, msg, len) < 0)
		;
	if (verbose) {
		int fd = open("/proc/self/maps", O_RDONLY);
		char m[4096];
		ssize_t n;

		while (fd >= 0 && (n = read(fd, m, sizeof(m))) > 0)
			if (write(1, m, n) < 0)
				break;
	}
	signal(sig, SIG_DFL);
	raise(sig);
}

static int probe_read(const volatile uint64_t *p, uint64_t *val)
{
	int sig;

	probing = 1;
	sig = sigsetjmp(probe_jmp, 1);
	if (!sig)
		*val = *p;
	probing = 0;
	return sig;
}

static int probe_write(volatile uint64_t *p, uint64_t val)
{
	int sig;

	probing = 1;
	sig = sigsetjmp(probe_jmp, 1);
	if (!sig)
		*p = val;
	probing = 0;
	return sig;
}

/* ---- memory helpers ---- */

static void *map_anon(size_t len, int prot)
{
	void *p = mmap(NULL, len, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if (p == MAP_FAILED)
		die("mmap");
	return p;
}

/* An anonymous mapping of @len aligned to @align */
static char *map_aligned(size_t len, size_t align)
{
	char *p = map_anon(len + align, PROT_READ | PROT_WRITE);
	char *a = (char *)(((uintptr_t)p + align - 1) & ~(align - 1));

	if (a > p)
		munmap(p, a - p);
	munmap(a + len, p + align - a);
	return a;
}

/*
 * Unmap a range for good: replace it with an inaccessible mapping instead
 * of leaving a hole, which glibc could fill with a thread stack before the
 * test is done with the address.  The old pages go through the same unmap
 * path as munmap().
 */
static void unmap_guard(void *addr, size_t len)
{
	if (mmap(addr, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
		 -1, 0) != addr)
		die("mmap guard");
}

static inline uint64_t *word(void *base, size_t page)
{
	return (uint64_t *)((char *)base + page * PS);
}

/* Write @tag + page index into the first word of each page */
static void fill(void *base, size_t npages, uint64_t tag)
{
	for (size_t i = 0; i < npages; i++)
		*word(base, i) = tag + i;
}

/* ---- arguments for code run on views ---- */

struct range {
	char *base;
	size_t npages;
	size_t step;		/* touch every step'th page */
	uint64_t tag;		/* for fill/verify */
	int bad;		/* pages that did not match */
	int sig;		/* first signal from a probe */
	int nsig;		/* number of probes that faulted */
};

static void do_read(void *p)
{
	struct range *r = p;
	uint64_t v;

	for (size_t i = 0; i < r->npages; i += r->step ? r->step : 1) {
		int sig = probe_read(word(r->base, i), &v);

		if (sig) {
			if (!r->nsig++)
				r->sig = sig;
		}
	}
}

static void do_verify(void *p)
{
	struct range *r = p;
	uint64_t v;

	for (size_t i = 0; i < r->npages; i += r->step ? r->step : 1) {
		int sig = probe_read(word(r->base, i), &v);

		if (sig) {
			if (!r->nsig++)
				r->sig = sig;
		} else if (v != r->tag + i) {
			/* Tag 0 checks for zero pages: no noise for those */
			if (r->bad++ < 3 && r->tag)
				note("page %zu: %#lx, expected %#lx", i,
				     (unsigned long)v,
				     (unsigned long)(r->tag + i));
		}
	}
}

static void do_fill(void *p)
{
	struct range *r = p;

	for (size_t i = 0; i < r->npages; i += r->step ? r->step : 1) {
		int sig = probe_write(word(r->base, i), r->tag + i);

		if (sig && !r->nsig++)
			r->sig = sig;
	}
}

static struct range R(void *base, size_t npages, uint64_t tag)
{
	return (struct range){ .base = base, .npages = npages, .tag = tag };
}

/* Verify @npages pages at @base hold tag + i, from view @view */
static int verify_in(int view, void *base, size_t npages, uint64_t tag)
{
	struct range r = R(base, npages, tag);

	in_view(view, do_verify, &r);
	return r.bad + r.nsig;
}

static int read_sig_in(int view, void *base, size_t npages)
{
	struct range r = R(base, npages, 0);

	in_view(view, do_read, &r);
	return r.nsig == (int)npages ? r.sig : -r.nsig;
}

static void flush_on(void)
{
	write_str("/proc/self/young_bitmap_flush", "1");
}

/* ---- tests ---- */

static int t_api(void)
{
	char path[64], buf[256];
	int id, n, fd;

	CHECK(view_get() == 0, "main thread on view %d", view_get());
	CHECK(prctl(PR_PT_VIEW, PR_PT_VIEW_SET, 1, 0, 0) == -1 &&
	      errno == EINVAL, "set to a missing view");
	CHECK(prctl(PR_PT_VIEW, 99, 0, 0, 0) == -1 && errno == EINVAL,
	      "bad op");
	id = view_create();
	CHECK(id == 1, "first view is %d", id);
	CHECK(access("/proc/self/pt_views/0/young_bitmap", F_OK) == 0,
	      "no view 0 dir");
	CHECK(access(view_path(path, id, "young_bitmap"), F_OK) == 0,
	      "no view %d dir", id);
	CHECK(access("/proc/self/pt_views/2", F_OK) == -1, "view 2 exists");
	CHECK(access("/proc/self/pt_views/01", F_OK) == -1, "01 exists");

	view_set(id);
	CHECK(view_get() == id, "on view %d", view_get());
	fd = open(view_path(path, id, "tasks"), O_RDONLY);
	n = fd < 0 ? -1 : read(fd, buf, sizeof(buf) - 1);
	buf[n > 0 ? n : 0] = 0;
	CHECK(n > 0 && atoi(buf) == gettid(), "tasks: '%s'", buf);
	close(fd);
	view_set(0);
	CHECK(view_get() == 0, "back on view %d", view_get());

	for (n = 2; n < MM_PT_VIEWS_MAX; n++)
		CHECK(view_create() == n, "view %d", n);
	CHECK(prctl(PR_PT_VIEW, PR_PT_VIEW_CREATE, 0, 0, 0) == -1 &&
	      errno == ENOSPC, "more than %d views", MM_PT_VIEWS_MAX - 1);
	return PASS;
}

/* Moving another thread through /proc/<pid>/pt_views/<n>/tasks */
static volatile int spin_stop;
static volatile int spin_view;
static volatile pid_t spin_tid;

static void *spinner(void *p)
{
	spin_tid = gettid();
	while (!spin_stop) {
		/* No system calls: the move must happen on interrupt return */
		for (volatile int i = 0; i < 100000; i++)
			;
		spin_view = view_get();
	}
	return NULL;
}

/* Views created from outside, with mkdir /proc/<pid>/pt_views/<n> */
static int t_create(void)
{
	char path[64], names[256] = "";
	struct dirent *de;
	DIR *dir;
	pid_t pid;
	int status;

	CHECK(access("/proc/self/pt_views/1", F_OK) && errno == ENOENT,
	      "no views yet");
	CHECK(!mkdir("/proc/self/pt_views/1", 0700), "mkdir 1: %m");
	CHECK(view_create() == 2, "prctl takes the next free id");
	CHECK(mkdir("/proc/self/pt_views/1", 0700) && errno == EEXIST,
	      "mkdir of an existing view");
	CHECK(mkdir("/proc/self/pt_views/0", 0700) && errno == EEXIST,
	      "mkdir 0");
	CHECK(mkdir("/proc/self/pt_views/8", 0700) && errno == EINVAL,
	      "mkdir past the last view");
	CHECK(mkdir("/proc/self/pt_views/x", 0700) && errno == EINVAL,
	      "mkdir of a name that is not a view id");
	CHECK(mkdir("/proc/self/pt_views/01", 0700) && errno == EINVAL,
	      "mkdir 01");
	CHECK(rmdir("/proc/self/pt_views/1") && errno == EPERM,
	      "rmdir: views are not destroyed");
	/*
	 * From another process: into a child, since with Yama ptrace_scope=1
	 * only an ancestor may attach
	 */
	{
		int fds[2];
		char c;

		if (pipe(fds))
			die("pipe");
		pid = fork();
		if (!pid) {
			close(fds[1]);
			/* Wait for the parent, then report whether view 1 exists */
			if (read(fds[0], &c, 1) < 0)
				_exit(2);
			_exit(access("/proc/self/pt_views/1", F_OK) ? 1 : 0);
		}
		close(fds[0]);
		snprintf(path, sizeof(path), "/proc/%d/pt_views/1", pid);
		CHECK(!mkdir(path, 0700), "mkdir in a child: %m");
		if (write(fds[1], "x", 1) != 1)
			die("write");
		close(fds[1]);
		waitpid(pid, &status, 0);
		CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
		      "child does not see the view created for it");
	}
	/* Leave a gap for prctl to fill */
	CHECK(!mkdir("/proc/self/pt_views/5", 0700), "mkdir 5: %m");
	CHECK(view_create() == 3, "prctl fills the gap");
	dir = opendir("/proc/self/pt_views");
	while (dir && (de = readdir(dir))) {
		strcat(names, " ");
		strcat(names, de->d_name);
	}
	if (dir)
		closedir(dir);
	CHECK(!strcmp(names, " . .. 0 1 2 3 5"), "listing:%s", names);

	/* Move a thread onto the new view from outside */
	{
		int id = 5;

		spin_view = -1;
		spin_stop = 0;
		pthread_t t;
		char buf[16];

		pthread_create(&t, NULL, spinner, NULL);
		while (spin_view < 0)
			usleep(1000);
		snprintf(buf, sizeof(buf), "%d", spin_tid);
		write_str(view_path(path, id, "tasks"), buf);
		for (int tries = 0; tries < 500 && spin_view != id; tries++)
			usleep(10000);
		CHECK(spin_view == id, "spinner on view %d", spin_view);
		spin_stop = 1;
		pthread_join(t, NULL);
	}
	return PASS;
}

/* Accessed bits of each view record only its own threads' accesses */
static int t_attribution(void)
{
	size_t n = 512;
	char *buf = map_anon(n * PS, PROT_READ | PROT_WRITE);
	uint8_t b0[64], b1[64];
	struct range r = R(buf, n, 0);
	int id = view_create(), wrong0 = 0, wrong1 = 0;

	/* View 0 has one accessed bit per THP; views are always per 4K */
	madvise(buf, n * PS, MADV_NOHUGEPAGE);
	flush_on();
	fill(buf, n, 0x1000);
	in_view(id, do_read, &r);		/* populate the view */
	read_young(0, buf, n, b0);
	read_young(id, buf, n, b1);

	/* view 1 reads every 3rd page, view 0 every 5th */
	r.step = 3;
	in_view(id, do_read, &r);
	for (size_t i = 0; i < n; i += 5)
		(void)*(volatile uint64_t *)word(buf, i);

	read_young(0, buf, n, b0);
	read_young(id, buf, n, b1);
	for (size_t i = 0; i < n; i++) {
		wrong0 += bit(b0, i) != (i % 5 == 0);
		wrong1 += bit(b1, i) != (i % 3 == 0);
	}
	CHECK(wrong0 == 0, "%d pages wrong in view 0", wrong0);
	CHECK(wrong1 == 0, "%d pages wrong in view %d", wrong1, id);

	/* Reading clears */
	read_young(id, buf, n, b1);
	for (size_t i = 0, set = 0; i < n; i++)
		set += bit(b1, i), wrong1 = set;
	CHECK(wrong1 == 0, "%d pages still young after a read", wrong1);

	/* Writes from a view are seen by view 0 and back */
	r.tag = 0x2000;
	r.step = 7;
	in_view(id, do_fill, &r);
	for (size_t i = 0; i < n; i += 7)
		CHECK(*word(buf, i) == 0x2000 + i, "page %zu", i);
	fill(buf, n, 0x3000);
	CHECK(verify_in(id, buf, n, 0x3000) == 0, "view sees view 0 writes");
	CHECK_VIEW(id);
	return PASS;
}

/* A view thread touching pages first leaves view 0's accessed bits clear */
static int t_first_touch(void)
{
	size_t n = 256;
	char *buf = map_anon(n * PS, PROT_READ | PROT_WRITE);
	struct range r = R(buf, n, 0x4000);
	int id = view_create(), young0 = 0, young1 = 0;
	uint8_t b0[32], b1[32];

	madvise(buf, n * PS, MADV_NOHUGEPAGE);
	flush_on();
	in_view(id, do_fill, &r);
	read_young(0, buf, n, b0);
	read_young(id, buf, n, b1);
	for (size_t i = 0; i < n; i++)
		young0 += bit(b0, i), young1 += bit(b1, i);
	CHECK(young1 == (int)n, "view %d: %d young", id, young1);
	/* do_anonymous_page() maps pages old on x86, but report rather than fail */
	if (young0)
		note("view 0 has %d of %zu pages young after view-only faults",
		     young0, n);
	for (size_t i = 0; i < n; i++)
		CHECK(*word(buf, i) == 0x4000 + i, "page %zu", i);
	CHECK(view_stat(id, "primary_faults") >= (long)n,
	      "primary_faults %ld", view_stat(id, "primary_faults"));
	CHECK_VIEW(id);
	return PASS;
}

static uint64_t pagemap(void *addr)
{
	uint64_t ent = 0;
	int fd = open("/proc/self/pagemap", O_RDONLY);

	if (fd < 0 || pread(fd, &ent, 8, (uintptr_t)addr / PS * 8) != 8)
		die("pagemap");
	close(fd);
	return ent;
}

#define PM_SOFT_DIRTY	(1ULL << 55)
#define PM_SWAP		(1ULL << 62)
#define PM_PRESENT	(1ULL << 63)

/* Writes from a view go through view 0's write fault handling */
static int t_soft_dirty(void)
{
	size_t n = 32;
	char *buf = map_anon(n * PS, PROT_READ | PROT_WRITE);
	struct range r = R(buf, n, 0x5000);
	int id = view_create(), clean = 0;

	/* zero pages first, then written pages */
	for (size_t i = 0; i < n; i++)
		(void)*(volatile uint64_t *)word(buf, i);
	in_view(id, do_read, &r);
	write_str("/proc/self/clear_refs", "4");
	in_view(id, do_fill, &r);
	for (size_t i = 0; i < n; i++)
		clean += !(pagemap(word(buf, i)) & PM_SOFT_DIRTY);
	CHECK(clean == 0, "%d zero pages not soft-dirty after view writes", clean);
	CHECK(verify_in(0, buf, n, 0x5000) == 0, "data");

	write_str("/proc/self/clear_refs", "4");
	r.tag = 0x5100;
	in_view(id, do_fill, &r);
	clean = 0;
	for (size_t i = 0; i < n; i++)
		clean += !(pagemap(word(buf, i)) & PM_SOFT_DIRTY);
	CHECK(clean == 0, "%d pages not soft-dirty after view writes", clean);
	CHECK(verify_in(0, buf, n, 0x5100) == 0, "data");
	CHECK_VIEW(id);
	return PASS;
}

static int t_munmap(void)
{
	size_t n = 64;
	char *buf = map_anon(n * PS, PROT_READ | PROT_WRITE);
	int id = view_create(), sig;

	fill(buf, n, 0x6000);
	CHECK(verify_in(id, buf, n, 0x6000) == 0, "initial");
	/* Replace with fresh zero pages */
	if (mmap(buf, n * PS, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != buf)
		die("mmap fixed");
	/* Zero pages fail verification against tag + i except page 0 */
	CHECK(verify_in(id, buf, n, 0) == (int)n - 1,
	      "view sees old data after MAP_FIXED");
	fill(buf, n, 0x6100);
	CHECK(verify_in(id, buf, n, 0x6100) == 0, "after refill");
	unmap_guard(buf, n * PS);
	sig = read_sig_in(id, buf, n);
	CHECK(sig == SIGSEGV, "read after munmap: %d", sig);
	CHECK_VIEW(id);
	return PASS;
}

static int t_mprotect(void)
{
	size_t n = 16;
	char *buf = map_anon(n * PS, PROT_READ | PROT_WRITE);
	struct range r = R(buf, n, 0x7000);
	int id = view_create();

	fill(buf, n, 0x6fff);
	in_view(id, do_fill, &r);		/* writable view entries */
	CHECK(r.nsig == 0, "write failed");
	if (mprotect(buf, n * PS, PROT_READ))
		die("mprotect");
	r.tag = 0x7100;
	r.nsig = 0;
	in_view(id, do_fill, &r);
	CHECK(r.nsig == (int)n && r.sig == SIGSEGV,
	      "%d of %zu writes faulted after PROT_READ", r.nsig, n);
	CHECK(verify_in(id, buf, n, 0x7000) == 0, "data after failed writes");
	if (mprotect(buf, n * PS, PROT_NONE))
		die("mprotect");
	CHECK(read_sig_in(id, buf, n) == SIGSEGV, "read after PROT_NONE");
	if (mprotect(buf, n * PS, PROT_READ | PROT_WRITE))
		die("mprotect");
	r.nsig = 0;
	in_view(id, do_fill, &r);
	CHECK(r.nsig == 0, "write after PROT_READ|PROT_WRITE");
	CHECK(verify_in(0, buf, n, 0x7100) == 0, "data");
	CHECK_VIEW(id);
	return PASS;
}

static int t_mremap(void)
{
	size_t n = 64, big = 2 * HPS / PS;
	char *buf = map_anon(n * PS, PROT_READ | PROT_WRITE);
	char *dst = map_anon(n * PS, PROT_READ | PROT_WRITE);
	struct range r = R(buf, n, 0x8000);
	int id = view_create();
	char *a, *b, *c;

	/* dst stays mapped until MREMAP_FIXED replaces it */
	fill(buf, n, 0x8000);
	in_view(id, do_fill, &r);
	CHECK(verify_in(id, buf, n, 0x8000) == 0, "initial");
	a = mremap(buf, n * PS, n * PS, MREMAP_MAYMOVE | MREMAP_FIXED, dst);
	if (a != dst)
		die("mremap");
	unmap_guard(buf, n * PS);
	CHECK(verify_in(id, dst, n, 0x8000) == 0, "moved");
	CHECK(read_sig_in(id, buf, n) == SIGSEGV, "old address");
	CHECK_VIEW(id);

	/* Shrink, then grow in place, with nothing else in between */
	a = mremap(dst, n * PS, n / 2 * PS, 0);
	CHECK(a == dst, "shrink");
	a = mremap(dst, n / 2 * PS, n * PS, 0);
	CHECK(a == dst, "grow");
	if (a == dst) {
		CHECK(verify_in(id, dst, n / 2, 0x8000) == 0, "kept part");
		CHECK(verify_in(id, dst + n / 2 * PS, n / 2, 0) == (int)n / 2 - 1,
		      "grown part not zero");
	}
	CHECK_VIEW(id);

	/* Whole PTE tables: move_normal_pmd() */
	a = map_aligned(2 * HPS, HPS);
	b = map_aligned(2 * HPS, HPS);
	madvise(a, 2 * HPS, MADV_NOHUGEPAGE);
	fill(a, big, 0x9000);
	r = R(a, big, 0x9000);
	in_view(id, do_verify, &r);
	CHECK(r.bad == 0 && r.nsig == 0, "PMD-sized initial");
	c = mremap(a, 2 * HPS, 2 * HPS, MREMAP_MAYMOVE | MREMAP_FIXED, b);
	if (c != b)
		die("mremap pmd");
	unmap_guard(a, 2 * HPS);
	CHECK(verify_in(id, b, big, 0x9000) == 0, "PMD-sized moved");
	CHECK(read_sig_in(id, a, big) == SIGSEGV, "PMD-sized old address");
	r = R(b, big, 0x9100);
	in_view(id, do_fill, &r);
	CHECK(verify_in(0, b, big, 0x9100) == 0, "PMD-sized write after move");
	CHECK_VIEW(id);
	return PASS;
}

/* fork() write-protects the parent's pages: views must lose write access */
static int t_fork(void)
{
	size_t n = 32;
	char *buf = map_anon(n * PS, PROT_READ | PROT_WRITE);
	struct range r = R(buf, n, 0xa000);
	int id = view_create(), go[2], done[2], status;
	char c;
	pid_t pid;

	in_view(id, do_fill, &r);		/* writable view entries */
	if (pipe(go) || pipe(done))
		die("pipe");
	pid = fork();
	if (pid < 0)
		die("fork");
	if (!pid) {
		int bad;

		if (read(go[0], &c, 1) != 1)
			_exit(2);
		bad = verify_in(0, buf, n, 0xa000);
		fill(buf, n, 0xc000);
		_exit(bad ? 1 : 0);
	}
	r.tag = 0xb000;
	in_view(id, do_fill, &r);		/* must COW */
	if (write(go[1], "x", 1) != 1)
		die("write");
	waitpid(pid, &status, 0);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
	      "child saw the parent's view writes (status %#x)", status);
	CHECK(verify_in(id, buf, n, 0xb000) == 0, "parent view data");
	CHECK(verify_in(0, buf, n, 0xb000) == 0, "parent data");
	CHECK_VIEW(id);
	return PASS;
}

static int t_dontneed(void)
{
	size_t n = 64;
	char *buf = map_anon(n * PS, PROT_READ | PROT_WRITE);
	int id = view_create();

	fill(buf, n, 0xd000);
	CHECK(verify_in(id, buf, n, 0xd000) == 0, "initial");
	madvise(buf, n * PS, MADV_DONTNEED);
	CHECK(verify_in(id, buf, n, 0) == (int)n - 1, "data after DONTNEED");
	CHECK_VIEW(id);
	return PASS;
}

/*
 * A write from a view to a MADV_FREE'd page must mark it dirty in view 0, or
 * reclaim discards it.
 */
static int t_madv_free(void)
{
	size_t n = 32;
	char *buf = map_anon(2 * n * PS, PROT_READ | PROT_WRITE);
	char *ctl = buf + n * PS;
	struct range r = R(buf, n, 0xe100);
	int id = view_create(), lost = 0, kept = 0;

	fill(buf, 2 * n, 0xe000);
	in_view(id, do_read, &r);
	if (madvise(buf, 2 * n * PS, MADV_FREE))
		return SKIP;
	in_view(id, do_fill, &r);
	if (madvise(buf, 2 * n * PS, MADV_PAGEOUT))
		return SKIP;
	for (size_t i = 0; i < n; i++)
		kept += *word(ctl, i) != 0;
	if (kept == (int)n) {
		note("MADV_PAGEOUT discarded nothing, inconclusive");
		return SKIP;
	}
	for (size_t i = 0; i < n; i++)
		lost += *word(buf, i) != 0xe100 + i;
	CHECK(lost == 0, "%d view writes lost to reclaim (%d control pages kept)",
	      lost, kept);
	CHECK_VIEW(id);
	return PASS;
}

static bool have_swap(void)
{
	FILE *f = fopen("/proc/swaps", "r");
	char line[256];
	int lines = 0;

	if (!f)
		return false;
	while (fgets(line, sizeof(line), f))
		lines++;
	fclose(f);
	return lines > 1;
}

static int t_swap(void)
{
	size_t n = 64;
	char *buf = map_anon(n * PS, PROT_READ | PROT_WRITE);
	struct range r = R(buf, n, 0xf000);
	int id = view_create(), swapped = 0;

	if (!have_swap())
		return SKIP;
	in_view(id, do_fill, &r);
	madvise(buf, n * PS, MADV_PAGEOUT);
	for (size_t i = 0; i < n; i++)
		swapped += !!(pagemap(word(buf, i)) & PM_SWAP);
	note("%d of %zu pages swapped out", swapped, n);
	CHECK(verify_in(id, buf, n, 0xf000) == 0, "after swap-in by view");
	/* Writes to swapped-in pages must survive the next swap-out */
	r.tag = 0xf100;
	in_view(id, do_fill, &r);
	madvise(buf, n * PS, MADV_PAGEOUT);
	CHECK(verify_in(0, buf, n, 0xf100) == 0, "view writes after swap");
	CHECK(verify_in(id, buf, n, 0xf100) == 0, "view reads after swap");
	CHECK_VIEW(id);
	return swapped ? PASS : SKIP;
}

static size_t anon_huge_kb(void *addr)
{
	FILE *f = fopen("/proc/self/smaps", "r");
	char line[256];
	unsigned long start, end;
	size_t kb = 0;
	bool in = false;

	if (!f)
		return 0;
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "%lx-%lx ", &start, &end) == 2)
			in = (uintptr_t)addr >= start && (uintptr_t)addr < end;
		else if (in && sscanf(line, "AnonHugePages: %zu kB", &kb) == 1)
			break;
	}
	fclose(f);
	return in ? kb : 0;
}

static int t_thp(void)
{
	size_t n = 2 * HPS / PS;
	char *buf = map_aligned(2 * HPS, HPS);
	uint8_t b0[128], b1[128];
	struct range r = R(buf, n, 0);
	int id = view_create(), wrong = 0, young0 = 0;

	madvise(buf, 2 * HPS, MADV_HUGEPAGE);
	fill(buf, n, 0x10000);
	if (anon_huge_kb(buf) < 2048) {
		note("no THP");
		return SKIP;
	}
	flush_on();
	in_view(id, do_read, &r);
	read_young(0, buf, n, b0);
	read_young(id, buf, n, b1);

	/* A view maps THPs with 4K entries, so its bits are per 4K page */
	r.step = 3;
	in_view(id, do_read, &r);
	(void)*(volatile uint64_t *)buf;
	read_young(0, buf, n, b0);
	read_young(id, buf, n, b1);
	for (size_t i = 0; i < n; i++) {
		wrong += bit(b1, i) != (i % 3 == 0);
		young0 += bit(b0, i);
	}
	CHECK(wrong == 0, "%d pages wrong in the view", wrong);
	CHECK(young0 == 512, "view 0: %d young, expected one THP", young0);

	/* Split by mprotect of one page */
	if (mprotect(buf + 5 * PS, PS, PROT_READ))
		die("mprotect");
	r = R(buf, n, 0x11000);
	in_view(id, do_fill, &r);
	CHECK(r.nsig == 1, "%d writes faulted, expected 1", r.nsig);
	mprotect(buf + 5 * PS, PS, PROT_READ | PROT_WRITE);
	*word(buf, 5) = 0x11000 + 5;
	CHECK(verify_in(0, buf, n, 0x11000) == 0, "data after split");

	/* Zap part of the second THP */
	madvise(buf + HPS + 16 * PS, 16 * PS, MADV_DONTNEED);
	r = R(buf + HPS + 16 * PS, 16, 0);
	in_view(id, do_verify, &r);
	CHECK(r.bad == 15, "zapped part of a THP not zero (%d)", r.bad);
	CHECK_VIEW(id);

	/* Collapse back */
	fill(buf, n, 0x12000);
	if (!madvise(buf, 2 * HPS, MADV_COLLAPSE)) {
		CHECK(anon_huge_kb(buf) >= 2048, "not collapsed");
		CHECK(verify_in(id, buf, n, 0x12000) == 0, "after collapse");
		r = R(buf, n, 0x13000);
		in_view(id, do_fill, &r);
		CHECK(verify_in(0, buf, n, 0x13000) == 0, "writes after collapse");
	} else {
		note("MADV_COLLAPSE: %s", strerror(errno));
	}
	CHECK_VIEW(id);
	return PASS;
}

static int t_hugetlb(void)
{
	size_t n = 2 * HPS / PS;
	char *buf = mmap(NULL, 2 * HPS, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
	uint8_t b1[128];
	struct range r;
	int id, wrong = 0;

	if (buf == MAP_FAILED)
		return SKIP;
	id = view_create();
	flush_on();
	fill(buf, n, 0x14000);
	CHECK(verify_in(id, buf, n, 0x14000) == 0, "initial");
	read_young(id, buf, n, b1);
	r = R(buf, n, 0x15000);
	r.step = 4;
	in_view(id, do_fill, &r);
	read_young(id, buf, n, b1);
	for (size_t i = 0; i < n; i++)
		wrong += bit(b1, i) != (i % 4 == 0);
	CHECK(wrong == 0, "%d pages wrong in the view", wrong);
	for (size_t i = 0; i < n; i += 4)
		CHECK(*word(buf, i) == 0x15000 + i, "page %zu", i);
	if (mmap(buf, 2 * HPS, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_FIXED,
		 -1, 0) != buf)
		die("mmap hugetlb fixed");
	CHECK(verify_in(id, buf, n, 0) == (int)n - 1, "old data after remap");
	CHECK_VIEW(id);
	unmap_guard(buf, 2 * HPS);
	CHECK(read_sig_in(id, buf, 4) == SIGSEGV, "after munmap");
	return PASS;
}

static int t_migrate(void)
{
	size_t n = 32;
	char *buf = map_anon(n * PS, PROT_READ | PROT_WRITE);
	struct range r = R(buf, n, 0x16000);
	void *pages[32];
	int nodes[32], status[32], id = view_create(), moved = 0;

	fill(buf, n, 0x15fff);
	in_view(id, do_fill, &r);		/* writable view entries */
	for (size_t i = 0; i < n; i++)
		pages[i] = word(buf, i), status[i] = -1;
	if (syscall(SYS_move_pages, 0, n, pages, NULL, status, 0))
		return SKIP;
	for (size_t i = 0; i < n; i++)
		nodes[i] = !status[i];
	if (syscall(SYS_move_pages, 0, n, pages, nodes, status, MPOL_MF_MOVE)) {
		note("move_pages: %s", strerror(errno));
		return SKIP;
	}
	for (size_t i = 0; i < n; i++)
		moved += status[i] == nodes[i];
	note("%d of %zu pages moved", moved, n);
	r.tag = 0x17000;
	in_view(id, do_fill, &r);
	CHECK(verify_in(0, buf, n, 0x17000) == 0, "view writes after migration");
	CHECK_VIEW(id);
	return moved ? PASS : SKIP;
}

static int t_ksm(void)
{
	size_t n = 64;
	char *buf = map_anon(n * PS, PROT_READ | PROT_WRITE);
	struct range r = R(buf, n, 0);
	int id = view_create(), changed = 0;
	long sharing = 0;
	/* Restored at exit, for runs on a real system */
	static const char *const knobs[] = { "run", "sleep_millisecs", "pages_to_scan" };
	char saved[3][32];

	for (int k = 0; k < 3; k++) {
		char path[64];

		snprintf(path, sizeof(path), "/sys/kernel/mm/ksm/%s", knobs[k]);
		snprintf(saved[k], sizeof(saved[k]), "%ld", read_long(path));
	}
	if (!try_write_str("/sys/kernel/mm/ksm/run", "1"))
		return SKIP;
	try_write_str("/sys/kernel/mm/ksm/sleep_millisecs", "10");
	try_write_str("/sys/kernel/mm/ksm/pages_to_scan", "1000");
	for (size_t i = 0; i < n; i++)
		memset(word(buf, i), 0x5a, PS);
	in_view(id, do_read, &r);
	r.tag = 0x5a5a5a5a5a5a5a5a;
	if (madvise(buf, n * PS, MADV_MERGEABLE))
		return SKIP;
	/* Writable view entries, then wait for KSM to merge the pages */
	for (int tries = 0; tries < 200 && sharing < (long)n / 2; tries++) {
		usleep(50000);
		sharing = read_long("/sys/kernel/mm/ksm/pages_sharing");
	}
	for (int k = 2; k >= 0; k--) {
		char path[64];

		snprintf(path, sizeof(path), "/sys/kernel/mm/ksm/%s", knobs[k]);
		try_write_str(path, saved[k]);
	}
	note("pages_sharing %ld", sharing);
	if (sharing <= 0)
		return SKIP;
	/* Writing one merged page from the view must not change the others */
	r = R(buf, 1, 0x18000);
	in_view(id, do_fill, &r);
	for (size_t i = 1; i < n; i++)
		changed += *word(buf, i) != 0x5a5a5a5a5a5a5a5a;
	CHECK(changed == 0, "%d merged pages changed by one view write", changed);
	CHECK(*word(buf, 0) == 0x18000, "written page");
	CHECK_VIEW(id);
	return PASS;
}

struct uffd_arg {
	int uffd;
	char *base;
	size_t npages;
	int events, wp_events;
	volatile int stop;
};

static void *uffd_handler(void *p)
{
	struct uffd_arg *u = p;
	static char page[PS] __attribute__((aligned(PS)));
	struct uffd_msg msg;
	struct pollfd pfd = { .fd = u->uffd, .events = POLLIN };

	while (!u->stop) {
		if (poll(&pfd, 1, 100) <= 0)
			continue;
		if (read(u->uffd, &msg, sizeof(msg)) != sizeof(msg))
			continue;
		if (msg.event != UFFD_EVENT_PAGEFAULT)
			continue;
		unsigned long addr = msg.arg.pagefault.address & ~(PS - 1);

		if (msg.arg.pagefault.flags & UFFD_PAGEFAULT_FLAG_WP) {
			struct uffdio_writeprotect wp = {
				.range = { addr, PS }, .mode = 0,
			};

			u->wp_events++;
			ioctl(u->uffd, UFFDIO_WRITEPROTECT, &wp);
		} else {
			struct uffdio_copy copy = {
				.dst = addr, .src = (uintptr_t)page, .len = PS,
			};

			*(uint64_t *)page = 0x19000 + (addr - (uintptr_t)u->base) / PS;
			u->events++;
			ioctl(u->uffd, UFFDIO_COPY, &copy);
		}
	}
	return NULL;
}

static int t_uffd(void)
{
	size_t n = 32;
	char *buf = map_anon(n * PS, PROT_READ | PROT_WRITE);
	struct uffdio_api api = { .api = UFFD_API,
				  .features = UFFD_FEATURE_PAGEFAULT_FLAG_WP };
	struct uffdio_register reg = { .range = { (uintptr_t)buf, n * PS },
				       .mode = UFFDIO_REGISTER_MODE_MISSING |
					       UFFDIO_REGISTER_MODE_WP };
	struct uffd_arg u = { .base = buf, .npages = n };
	struct range r = R(buf, n, 0x19000);
	int id = view_create();
	pthread_t t;

	u.uffd = syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK);
	if (u.uffd < 0 || ioctl(u.uffd, UFFDIO_API, &api))
		return SKIP;
	if (ioctl(u.uffd, UFFDIO_REGISTER, &reg)) {
		reg.mode = UFFDIO_REGISTER_MODE_MISSING;
		if (ioctl(u.uffd, UFFDIO_REGISTER, &reg))
			return SKIP;
	}
	pthread_create(&t, NULL, uffd_handler, &u);
	in_view(id, do_verify, &r);
	CHECK(r.bad == 0 && r.nsig == 0, "missing faults: %d bad", r.bad);
	CHECK(u.events == (int)n, "%d missing events", u.events);

	if (reg.mode & UFFDIO_REGISTER_MODE_WP) {
		struct uffdio_writeprotect wp = {
			.range = { (uintptr_t)buf, n * PS },
			.mode = UFFDIO_WRITEPROTECT_MODE_WP,
		};

		r = R(buf, n, 0x1a000);
		in_view(id, do_fill, &r);	/* writable view entries */
		u.wp_events = 0;
		if (ioctl(u.uffd, UFFDIO_WRITEPROTECT, &wp))
			die("UFFDIO_WRITEPROTECT");
		r.tag = 0x1b000;
		in_view(id, do_fill, &r);
		CHECK(u.wp_events == (int)n, "%d wp events for %zu writes",
		      u.wp_events, n);
		CHECK(verify_in(0, buf, n, 0x1b000) == 0, "data");
	}
	u.stop = 1;
	pthread_join(t, NULL);
	CHECK_VIEW(id);
	return PASS;
}

static int t_memfd(void)
{
	size_t n = 16;
	int fd = memfd_create("pt-views", 0), id = view_create();
	char *buf;

	if (fd < 0 || ftruncate(fd, n * PS))
		die("memfd");
	buf = mmap(NULL, n * PS, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (buf == MAP_FAILED)
		die("mmap memfd");
	fill(buf, n, 0x1c000);
	CHECK(verify_in(id, buf, n, 0x1c000) == 0, "initial");
	if (ftruncate(fd, 4 * PS))
		die("ftruncate");
	CHECK(read_sig_in(id, buf + 8 * PS, 8) == SIGBUS, "beyond EOF");
	CHECK(verify_in(id, buf, 4, 0x1c000) == 0, "before EOF");
	if (fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, PS, PS))
		die("punch hole");
	{
		struct range r = R(buf + PS, 1, 0);

		in_view(id, do_read, &r);
		CHECK(*word(buf, 1) == 0, "punched page");
		r = R(buf + PS, 1, 0);
		in_view(id, do_verify, &r);
		CHECK(r.bad == 0, "view sees punched page data");
	}
	/* A second process writes, the view sees it */
	if (!fork()) {
		char *p = mmap(NULL, 4 * PS, PROT_READ | PROT_WRITE, MAP_SHARED,
			       fd, 0);

		fill(p, 4, 0x1d000);
		_exit(0);
	}
	wait(NULL);
	CHECK(verify_in(id, buf, 4, 0x1d000) == 0, "other process's writes");
	CHECK_VIEW(id);
	return PASS;
}

/*
 * Writes through a view to a file on a disk filesystem must survive
 * writeback: writeback write-protects the page (page_mkclean()), so the
 * view's next write has to fault and dirty the page again.  The directory
 * is $PTV_FS_DIR, default /mnt; skipped on tmpfs, which has no writeback.
 */
struct wb_arg {
	char *base;
	size_t npages;
	volatile int stop;
	uint64_t *last;		/* last value written to each page */
	unsigned long writes;
};

static void do_wb_writer(void *p)
{
	struct wb_arg *a = p;
	uint64_t v = 1;

	while (!a->stop) {
		for (size_t i = 0; i < a->npages; i++) {
			*word(a->base, i) = (v << 16) | i;
			a->last[i] = (v << 16) | i;
		}
		v++;
		a->writes++;
	}
}

/* Count pages that do not hold a->last[], in a->writes */
static void do_wb_compare(void *p)
{
	struct wb_arg *a = p;

	a->writes = 0;
	for (size_t i = 0; i < a->npages; i++)
		a->writes += *word(a->base, i) != a->last[i];
}

static int file_bad(int fd, size_t npages, const uint64_t *want)
{
	int bad = 0;

	for (size_t i = 0; i < npages; i++) {
		uint64_t v = 0;

		if (pread(fd, &v, sizeof(v), i * PS) != sizeof(v))
			die("pread");
		if (v != want[i] && bad++ < 3)
			note("page %zu on disk: %#lx, expected %#lx", i,
			     (unsigned long)v, (unsigned long)want[i]);
	}
	return bad;
}

/* Write back and evict @fd's pages so that reads come from the disk */
static void wb_evict(int fd)
{
	if (fsync(fd))
		die("fsync");
	if (!try_write_str("/proc/sys/vm/drop_caches", "1"))
		posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
}

static int t_writeback(void)
{
	const char *dir = getenv("PTV_FS_DIR") ?: "/mnt";
	size_t n = 64;
	char path[256];
	uint64_t want[64];
	struct statfs sf;
	struct wb_arg a = { .npages = n, .last = want };
	pthread_t t;
	int fd, id = view_create(), round;
	long zapped;
	char *buf;

	if (statfs(dir, &sf) || sf.f_type == 0x01021994 /* tmpfs */ ||
	    sf.f_type == 0x858458f6 /* ramfs */) {
		note("no disk filesystem at %s", dir);
		return SKIP;
	}
	snprintf(path, sizeof(path), "%s/pt-views-wb.%d", dir, getpid());
	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (fd < 0 || ftruncate(fd, n * PS))
		die("open file");
	unlink(path);
	buf = mmap(NULL, n * PS, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (buf == MAP_FAILED)
		die("mmap file");

	/* Write, write back, write again from the view: each round reaches disk */
	zapped = view_stat(id, "zapped");
	for (round = 0; round < 20; round++) {
		struct range r = R(buf, n, (uint64_t)(round + 1) << 32);

		in_view(id, do_fill, &r);
		CHECK(r.nsig == 0, "round %d: view write faulted", round);
		if (msync(buf, n * PS, MS_SYNC))
			die("msync");
		for (size_t i = 0; i < n; i++)
			want[i] = r.tag + i;
		CHECK(file_bad(fd, n, want) == 0, "round %d after msync", round);
	}
	/* page_mkclean() write-protected the pages, clearing the view's entries */
	CHECK(no_views || view_stat(id, "zapped") - zapped >= 19 * (long)n,
	      "writeback zapped %ld view entries",
	      view_stat(id, "zapped") - zapped);

	/* Same, with the pages evicted and faulted back between rounds */
	for (round = 0; round < 5; round++) {
		struct range r = R(buf, n, (uint64_t)(round + 0x40) << 32);

		in_view(id, do_fill, &r);
		munmap(buf, n * PS);
		wb_evict(fd);
		for (size_t i = 0; i < n; i++)
			want[i] = r.tag + i;
		CHECK(file_bad(fd, n, want) == 0, "evict round %d", round);
		buf = mmap(buf, n * PS, PROT_READ | PROT_WRITE,
			   MAP_SHARED | MAP_FIXED, fd, 0);
		if (buf == MAP_FAILED)
			die("mmap file again");
		CHECK(verify_in(id, buf, n, r.tag) == 0, "view reads back round %d",
		      round);
	}

	/* A view thread writes nonstop while view 0 writes the file back */
	a.base = buf;
	if (pthread_create(&t, NULL, vthread,
			   &(struct vjob){ id, do_wb_writer, &a }))
		die("pthread_create");
	for (round = 0; round < 200; round++) {
		if (round & 1)
			sync_file_range(fd, 0, 0, SYNC_FILE_RANGE_WAIT_BEFORE |
					SYNC_FILE_RANGE_WRITE |
					SYNC_FILE_RANGE_WAIT_AFTER);
		else if (msync(buf, n * PS, MS_SYNC))
			die("msync");
	}
	a.stop = 1;
	pthread_join(t, NULL);
	/* The writer's last values were written after the last sync */
	munmap(buf, n * PS);
	wb_evict(fd);
	CHECK(file_bad(fd, n, want) == 0, "concurrent writeback, %lu passes",
	      a.writes);
	if (verbose)
		note("%lu writer passes over %zu pages", a.writes, n);

	/* A private file mapping: view writes copy, the file is unchanged */
	buf = mmap(NULL, n * PS, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	if (buf == MAP_FAILED)
		die("mmap private");
	a.base = buf;
	in_view(id, do_wb_compare, &a);
	CHECK(a.writes == 0, "view reads %lu wrong pages of the private mapping",
	      a.writes);
	{
		struct range r = R(buf, n, 0x7e0000);

		in_view(id, do_fill, &r);
		CHECK(verify_in(0, buf, n, 0x7e0000) == 0, "copies seen by view 0");
	}
	if (msync(buf, n * PS, MS_SYNC))
		die("msync");
	CHECK(file_bad(fd, n, want) == 0, "private writes reached the file");
	CHECK_VIEW(id);
	close(fd);
	return PASS;
}

/* Futex operations fault with page faults disabled */
struct futex_arg {
	char *base;
	size_t npages;
	pthread_mutex_t *locks;
	int errors;
};

static void do_futex(void *p)
{
	struct futex_arg *f = p;

	for (size_t i = 0; i < f->npages; i++) {
		uint32_t *a = (uint32_t *)word(f->base, i);
		uint32_t *b = a + 16;

		/* FUTEX_WAKE_OP writes *b with page faults disabled */
		if (syscall(SYS_futex, a, FUTEX_WAKE_OP, 1, 0, b,
			    FUTEX_OP(FUTEX_OP_ADD, 1, FUTEX_OP_CMP_EQ, 0)) < 0)
			f->errors++;
	}
}

static void *pi_locker(void *p)
{
	struct futex_arg *f = p;

	for (size_t i = 0; i < f->npages; i++) {
		pthread_mutex_lock(&f->locks[i]);
		pthread_mutex_unlock(&f->locks[i]);
	}
	return NULL;
}

static void do_pi(void *p)
{
	struct futex_arg *f = p;
	pthread_t t;

	for (size_t i = 0; i < f->npages; i++)
		pthread_mutex_lock(&f->locks[i]);
	pthread_create(&t, NULL, pi_locker, f);	/* also on this view */
	usleep(1000);
	for (size_t i = 0; i < f->npages; i++)
		pthread_mutex_unlock(&f->locks[i]);
	pthread_join(t, NULL);
}

static int t_futex(void)
{
	size_t n = 64;
	char *buf = map_anon(n * PS, PROT_READ | PROT_WRITE);
	struct futex_arg f = { .base = buf, .npages = n };
	pthread_mutexattr_t attr;
	int id = view_create();

	alarm(30);
	fill(buf, n, 0);		/* present in view 0, not in the view */
	for (size_t i = 0; i < n; i++)
		((uint32_t *)word(buf, i))[16] = 0;
	in_view(id, do_futex, &f);
	CHECK(f.errors == 0, "%d FUTEX_WAKE_OP errors", f.errors);
	for (size_t i = 0; i < n; i++)
		CHECK(((uint32_t *)word(buf, i))[16] == 1, "page %zu", i);
	note("atomic fills %ld, misses %ld", view_stat(id, "atomic_fills"),
	     view_stat(id, "atomic_misses"));

	/* PI mutexes in pages that the view does not have yet */
	munmap(buf, n * PS);
	buf = map_anon(n * PS, PROT_READ | PROT_WRITE);
	f.locks = (pthread_mutex_t *)buf;
	f.npages = n * PS / sizeof(pthread_mutex_t) / 64;
	pthread_mutexattr_init(&attr);
	pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT);
	for (size_t i = 0; i < f.npages; i++)
		pthread_mutex_init(&f.locks[i], &attr);
	in_view(id, do_pi, &f);
	CHECK_VIEW(id);
	return PASS;
}

/* Kernel accesses to user memory on behalf of a view thread */
struct sys_arg {
	char *base;
	size_t npages;
	int bad;
};

static void do_syscalls(void *p)
{
	struct sys_arg *s = p;
	int fds[2];

	if (pipe(fds))
		die("pipe");
	for (size_t i = 0; i < s->npages; i++) {
		uint64_t v = 0x1e000 + i;

		/* copy_from_user() from a view-only page, copy_to_user() into one */
		*word(s->base, i) = v;
		if (write(fds[1], word(s->base, i), 8) != 8 ||
		    read(fds[0], word(s->base, s->npages + i), 8) != 8 ||
		    *word(s->base, s->npages + i) != v)
			s->bad++;
	}
	close(fds[0]);
	close(fds[1]);
}

static void altstack_handler(int sig)
{
	(void)sig;
}

static void do_altstack(void *p)
{
	struct sys_arg *s = p;
	stack_t ss = { .ss_sp = s->base, .ss_size = s->npages * PS };
	struct sigaction sa = { .sa_handler = altstack_handler,
				.sa_flags = SA_ONSTACK };

	/* The kernel writes the signal frame into memory new to this view */
	if (sigaltstack(&ss, NULL) || sigaction(SIGUSR1, &sa, NULL))
		die("sigaltstack");
	if (raise(SIGUSR1))
		s->bad++;
	ss.ss_flags = SS_DISABLE;
	sigaltstack(&ss, NULL);
}

static int t_syscalls(void)
{
	size_t n = 32;
	char *buf = map_anon(2 * n * PS, PROT_READ | PROT_WRITE);
	struct sys_arg s = { buf, n, 0 };
	int id = view_create();
	struct iovec local, remote;
	uint64_t v = 0x1f000;

	in_view(id, do_syscalls, &s);
	CHECK(s.bad == 0, "%d bad copies", s.bad);
	s = (struct sys_arg){ map_anon(16 * PS, PROT_READ | PROT_WRITE), 16, 0 };
	in_view(id, do_altstack, &s);
	CHECK(s.bad == 0, "signal on alternate stack");

	/* process_vm_writev() writes through view 0, the view sees it */
	local = (struct iovec){ &v, 8 };
	remote = (struct iovec){ buf, 8 };
	CHECK(process_vm_writev(getpid(), &local, 1, &remote, 1, 0) == 8,
	      "process_vm_writev: %s", strerror(errno));
	{
		struct range r = R(buf, 1, 0x1f000);

		in_view(id, do_verify, &r);
		CHECK(r.bad == 0, "view after process_vm_writev");
	}
	CHECK_VIEW(id);
	return PASS;
}

static void do_exec(void *p)
{
	int status;
	pid_t pid;

	(void)p;
	pid = fork();
	if (!pid) {
		execl("/proc/self/exe", "proc-pt-views", "--print-view", NULL);
		_exit(3);
	}
	waitpid(pid, &status, 0);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
	      "fork+exec from a view: status %#x", status);
	pid = vfork();
	if (!pid) {
		execl("/proc/self/exe", "proc-pt-views", "--print-view", NULL);
		_exit(3);
	}
	waitpid(pid, &status, 0);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
	      "vfork+exec from a view: status %#x", status);
}

static void do_check_view(void *p)
{
	int *id = p;

	CHECK(view_get() == *id, "thread on view %d", view_get());
}

static void *view_child_thread(void *p)
{
	/* Threads start on their creator's view */
	return (void *)(long)view_get();
}

static void do_child_thread(void *p)
{
	pthread_t t;
	void *ret;

	pthread_create(&t, NULL, view_child_thread, NULL);
	pthread_join(t, &ret);
	*(int *)p = (int)(long)ret;
}

static int t_exec(void)
{
	int id = view_create(), child = -1;

	in_view(id, do_check_view, &id);
	in_view(id, do_exec, NULL);
	in_view(id, do_child_thread, &child);
	CHECK(child == id, "new thread on view %d", child);
	return PASS;
}


static int t_move(void)
{
	int id = view_create(), tries;
	char path[64], buf[32];
	pthread_t t;
	pid_t tid;

	spin_view = -1;
	pthread_create(&t, NULL, spinner, NULL);
	while (spin_view < 0)
		usleep(1000);
	tid = spin_tid;
	snprintf(buf, sizeof(buf), "%d", tid);
	write_str(view_path(path, id, "tasks"), buf);
	for (tries = 0; tries < 500 && spin_view != id; tries++)
		usleep(10000);
	CHECK(spin_view == id, "spinner on view %d", spin_view);
	write_str(view_path(path, 0, "tasks"), buf);
	for (tries = 0; tries < 500 && spin_view != 0; tries++)
		usleep(10000);
	CHECK(spin_view == 0, "spinner back on view %d", spin_view);
	CHECK(!try_write_str(view_path(path, id, "tasks"), "1"),
	      "moved a task of another process");
	spin_stop = 1;
	pthread_join(t, NULL);
	return PASS;
}

/* Accessed bits survive the view entry being cleared */
static int t_young_markers(void)
{
	size_t n = 16;
	char *buf = map_anon(n * PS, PROT_READ | PROT_WRITE);
	struct range r = R(buf, n, 0);
	int id = view_create(), young = 0;
	uint8_t b[8];
	char path[64];
	int fd;

	flush_on();
	fill(buf, n, 0);
	in_view(id, do_read, &r);
	madvise(buf, n * PS, MADV_DONTNEED);	/* clears the view */
	read_young(id, buf, n, b);
	for (size_t i = 0; i < n; i++)
		young += bit(b, i);
	CHECK(young == (int)n, "%d young after the view was cleared", young);
	read_young(id, buf, n, b);
	young = 0;
	for (size_t i = 0; i < n; i++)
		young += bit(b, i);
	CHECK(young == 0, "%d young after a second read", young);

	/* Select-mode writes clear only the selected pages */
	in_view(id, do_read, &r);
	memset(b, 0, sizeof(b));
	b[0] = 0x0f;
	fd = open(view_path(path, id, "young_bitmap"), O_RDWR);
	CHECK(pwrite(fd, b, 2, (uintptr_t)buf / PS) == 2, "select write");
	close(fd);
	read_young(id, buf, n, b);
	young = 0;
	for (size_t i = 0; i < n; i++)
		young += bit(b, i) != (i >= 4);
	CHECK(young == 0, "select write cleared the wrong pages (%#x %#x)",
	      b[0], b[1]);

	/* Beyond the address space: EOF */
	fd = open(view_path(path, id, "young_bitmap"), O_RDONLY);
	CHECK(pread(fd, b, 8, 1ULL << 50) == 0, "read beyond task size");
	close(fd);
	CHECK_VIEW(id);
	return PASS;
}

/*
 * Stress: view threads read and write chunks of a region that other
 * threads concurrently remap, mprotect, zap and swap out, all under one
 * read-write lock per chunk.  Holding the lock, the data must match the
 * chunk's current generation; a stale view entry shows old data.
 */
#define S_CHUNKS	32
#define S_CHUNK_PAGES	16

static char *s_base;
static pthread_rwlock_t s_lock[S_CHUNKS];
static uint64_t s_gen[S_CHUNKS];
static volatile int s_stop;
static int s_bad, s_ops;

static void s_fill(int c)
{
	for (int i = 0; i < S_CHUNK_PAGES; i++)
		*word(s_base, c * S_CHUNK_PAGES + i) = s_gen[c] << 16 | i;
}

static void do_stress_worker(void *p)
{
	unsigned int seed = (uintptr_t)p;
	int bad = 0, ops = 0;

	while (!s_stop) {
		int c = rand_r(&seed) % S_CHUNKS, i = rand_r(&seed) % S_CHUNK_PAGES;
		uint64_t *w = word(s_base, c * S_CHUNK_PAGES + i);

		if (rand_r(&seed) & 1) {
			pthread_rwlock_rdlock(&s_lock[c]);
			if (*w != (s_gen[c] << 16 | i) && bad++ < 3)
				printf("    stale: chunk %d page %d %#lx gen %lu\n",
				       c, i, (unsigned long)*w,
				       (unsigned long)s_gen[c]);
			pthread_rwlock_unlock(&s_lock[c]);
		} else {
			pthread_rwlock_wrlock(&s_lock[c]);
			s_gen[c]++;
			s_fill(c);
			pthread_rwlock_unlock(&s_lock[c]);
		}
		ops++;
	}
	__atomic_add_fetch(&s_bad, bad, __ATOMIC_RELAXED);
	__atomic_add_fetch(&s_ops, ops, __ATOMIC_RELAXED);
}

static void *stress_worker(void *p)
{
	int view = (uintptr_t)p & 0xff;

	if (view)
		view_set(view);
	do_stress_worker(p);
	return NULL;
}

static void *stress_mutator(void *p)
{
	unsigned int seed = (uintptr_t)p;
	size_t len = S_CHUNK_PAGES * PS;

	while (!s_stop) {
		int c = rand_r(&seed) % S_CHUNKS;
		char *a = s_base + c * len;

		pthread_rwlock_wrlock(&s_lock[c]);
		switch (rand_r(&seed) % 6) {
		case 0:
			mmap(a, len, PROT_READ | PROT_WRITE,
			     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
			break;
		case 1:
			madvise(a, len, MADV_DONTNEED);
			break;
		case 2:
			mprotect(a, len, PROT_READ);
			mprotect(a, len, PROT_READ | PROT_WRITE);
			break;
		case 3:
			madvise(a, len, MADV_PAGEOUT);
			break;
		case 4: {
			/* Move the chunk away and back */
			char *tmp = mremap(a, len, len, MREMAP_MAYMOVE);

			if (tmp != MAP_FAILED && tmp != a)
				mremap(tmp, len, len,
				       MREMAP_MAYMOVE | MREMAP_FIXED, a);
			break;
		}
		case 5:
			if (!fork())
				_exit(0);
			break;
		}
		s_gen[c]++;
		s_fill(c);
		pthread_rwlock_unlock(&s_lock[c]);
		while (waitpid(-1, NULL, WNOHANG) > 0)
			;
	}
	return NULL;
}

static int t_stress(void)
{
	int v1 = view_create(), v2 = view_create();
	pthread_t t[7];
	int nt = 0;

	s_base = map_anon(S_CHUNKS * S_CHUNK_PAGES * PS, PROT_READ | PROT_WRITE);
	for (int c = 0; c < S_CHUNKS; c++) {
		pthread_rwlock_init(&s_lock[c], NULL);
		s_fill(c);
	}
	for (int i = 0; i < 2; i++) {
		pthread_create(&t[nt++], NULL, stress_worker,
			       (void *)(uintptr_t)(v1 | (i + 1) << 8));
		pthread_create(&t[nt++], NULL, stress_worker,
			       (void *)(uintptr_t)(v2 | (i + 3) << 8));
	}
	pthread_create(&t[nt++], NULL, stress_worker, (void *)(uintptr_t)(5 << 8));
	pthread_create(&t[nt++], NULL, stress_mutator, (void *)1);
	pthread_create(&t[nt++], NULL, stress_mutator, (void *)2);
	sleep(getenv("PT_VIEWS_STRESS_SECS") ?
	      atoi(getenv("PT_VIEWS_STRESS_SECS")) : 8);
	s_stop = 1;
	for (int i = 0; i < nt; i++)
		pthread_join(t[i], NULL);
	while (waitpid(-1, NULL, WNOHANG) > 0)
		;
	note("%d operations, view %d: %ld fills %ld zapped, view %d: %ld fills",
	     s_ops, v1, view_stat(v1, "fills"), view_stat(v1, "zapped"), v2,
	     view_stat(v2, "fills"));
	CHECK(s_bad == 0, "%d stale reads", s_bad);
	CHECK_VIEW(v1);
	CHECK_VIEW(v2);
	return PASS;
}

/* Threads on many views and view 0 all accessing the same memory */
static int t_many_views(void)
{
	size_t n = 64;
	char *buf = map_anon(n * PS, PROT_READ | PROT_WRITE);
	int ids[MM_PT_VIEWS_MAX], wrong = 0;
	uint8_t b[8];

	flush_on();
	fill(buf, n, 0x20000);
	for (int v = 1; v < MM_PT_VIEWS_MAX; v++) {
		ids[v] = view_create();
		CHECK(verify_in(ids[v], buf, n, 0x20000) == 0, "view %d", v);
	}
	for (int v = 0; v < MM_PT_VIEWS_MAX; v++)
		read_young(v, buf, n, b);
	/* View v reads page v only */
	for (int v = 1; v < MM_PT_VIEWS_MAX; v++) {
		struct range r = R(buf + v * PS, 1, 0x20000 + v);

		in_view(ids[v], do_verify, &r);
	}
	for (int v = 0; v < MM_PT_VIEWS_MAX; v++) {
		read_young(v, buf, n, b);
		for (size_t i = 0; i < n; i++)
			wrong += bit(b, i) != (v && (int)i == v);
	}
	CHECK(wrong == 0, "%d bits wrong", wrong);
	unmap_guard(buf, n * PS);
	for (int v = 1; v < MM_PT_VIEWS_MAX; v++)
		CHECK(read_sig_in(ids[v], buf, n) == SIGSEGV, "view %d", v);
	for (int v = 1; v < MM_PT_VIEWS_MAX; v++)
		CHECK_VIEW(ids[v]);
	return PASS;
}

/* Processes ending while threads on views are busy */
static void *busy_toucher(void *p)
{
	char *buf = p;

	if ((uintptr_t)buf & 1) {
		buf--;
	} else {
		view_set(1);
	}
	for (unsigned long i = 0;; i++) {
		buf[(i * 4099 * 64) % (256 * PS)]++;
		if (!(i & 0xffff))
			madvise(buf + (i % 256) * PS, 16 * PS, MADV_DONTNEED);
	}
	return NULL;
}

static pid_t busy_child(int how)
{
	pid_t pid = fork();

	if (!pid) {
		char *buf = map_anon(256 * PS, PROT_READ | PROT_WRITE);
		pthread_t t;

		view_create();
		for (int i = 0; i < 3; i++)
			pthread_create(&t, NULL, busy_toucher, buf);
		pthread_create(&t, NULL, busy_toucher, buf + 1);  /* view 0 */
		usleep(200000);
		if (how == 1)
			exit(0);
		if (how == 2)
			execl("/proc/self/exe", "proc-pt-views", "--print-view",
			      NULL);
		pause();
		_exit(3);
	}
	return pid;
}

static int t_exit(void)
{
	int status;
	pid_t pid;

	for (int round = 0; round < 3; round++) {
		pid = busy_child(1);
		waitpid(pid, &status, 0);
		CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
		      "exit: status %#x", status);
		pid = busy_child(0);
		usleep(250000);
		kill(pid, SIGKILL);
		waitpid(pid, &status, 0);
		CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
		      "kill: status %#x", status);
		pid = busy_child(2);
		waitpid(pid, &status, 0);
		CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
		      "exec with threads on views: status %#x", status);
	}
	return PASS;
}

/* Protection keys apply to view threads without any page table change */
static int pkey_sig;

static void do_pkey_write(void *p)
{
	struct range *r = p;
	int pkey = r->step;

	r->step = 1;
	pkey_set(pkey, PKEY_DISABLE_WRITE);
	do_fill(r);
	pkey_sig = r->sig;
	r->nsig = 0;
	pkey_set(pkey, 0);
	r->tag += 0x100;
	do_fill(r);
}

static int t_pkeys(void)
{
	size_t n = 16;
	char *buf = map_anon(n * PS, PROT_READ | PROT_WRITE);
	int id = view_create(), pkey = pkey_alloc(0, 0);
	struct range r = R(buf, n, 0x21000);

	if (pkey < 0)
		return SKIP;
	in_view(id, do_fill, &r);		/* writable view entries */
	if (pkey_mprotect(buf, n * PS, PROT_READ | PROT_WRITE, pkey))
		die("pkey_mprotect");
	r.tag = 0x22000;
	r.step = pkey;
	r.nsig = 0;
	in_view(id, do_pkey_write, &r);
	CHECK(pkey_sig == SIGSEGV, "write with the key disabled: signal %d",
	      pkey_sig);
	CHECK(r.nsig == 0, "%d writes faulted with the key enabled", r.nsig);
	CHECK(verify_in(0, buf, n, 0x22100) == 0, "data");
	CHECK_VIEW(id);
	return PASS;
}

/* A memory cgroup limit forces reclaim of a view thread's memory */
static bool cgroup_enter(const char *name, const char *max, const char *swap)
{
	char path[128], pid[16];

	snprintf(path, sizeof(path), "/sys/fs/cgroup/%s", name);
	if (mkdir(path, 0755) && errno != EEXIST)
		return false;
	snprintf(path, sizeof(path), "/sys/fs/cgroup/%s/memory.max", name);
	if (!try_write_str(path, max))
		return false;
	snprintf(path, sizeof(path), "/sys/fs/cgroup/%s/memory.swap.max", name);
	try_write_str(path, swap);
	snprintf(path, sizeof(path), "/sys/fs/cgroup/%s/cgroup.procs", name);
	snprintf(pid, sizeof(pid), "%d", getpid());
	return try_write_str(path, pid);
}

static int t_memcg(void)
{
	size_t n = 48 << 20 >> 12;		/* 48 MiB in a 16 MiB cgroup */
	char *buf;
	struct range r;
	int id;

	if (!have_swap() || !cgroup_enter("pt-views-memcg", "16M", "max"))
		return SKIP;
	buf = map_anon(n * PS, PROT_READ | PROT_WRITE);
	madvise(buf, n * PS, MADV_NOHUGEPAGE);
	id = view_create();
	r = R(buf, n, 0x23000);
	in_view(id, do_fill, &r);
	CHECK(verify_in(id, buf, n, 0x23000) == 0, "view after reclaim");
	CHECK(verify_in(0, buf, n, 0x23000) == 0, "view 0 after reclaim");
	r.tag = 0x24000;
	in_view(id, do_fill, &r);
	CHECK(verify_in(0, buf, n, 0x24000) == 0, "view 0 after view rewrite");
	note("view %d: %ld fills, %ld zapped", id, view_stat(id, "fills"),
	     view_stat(id, "zapped"));
	CHECK_VIEW(id);
	return PASS;
}

static void do_hog(void *p)
{
	for (;;) {
		char *b = mmap(NULL, 16 << 20, PROT_READ | PROT_WRITE,
			       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

		if (b == MAP_FAILED)
			return;
		memset(b, 1, 16 << 20);
	}
}

static int t_oom(void)
{
	int status;
	pid_t pid = fork();

	if (!pid) {
		if (!cgroup_enter("pt-views-oom", "32M", "0"))
			_exit(SKIP);
		view_create();
		in_view(1, do_hog, NULL);
		_exit(0);
	}
	waitpid(pid, &status, 0);
	if (WIFEXITED(status) && WEXITSTATUS(status) == SKIP)
		return SKIP;
	CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
	      "hog not OOM-killed: status %#x", status);
	return PASS;
}

static const struct {
	const char *name;
	int (*fn)(void);
} tests[] = {
	{ "api", t_api },
	{ "create", t_create },
	{ "attribution", t_attribution },
	{ "first_touch", t_first_touch },
	{ "soft_dirty", t_soft_dirty },
	{ "munmap", t_munmap },
	{ "mprotect", t_mprotect },
	{ "mremap", t_mremap },
	{ "fork", t_fork },
	{ "dontneed", t_dontneed },
	{ "madv_free", t_madv_free },
	{ "swap", t_swap },
	{ "thp", t_thp },
	{ "hugetlb", t_hugetlb },
	{ "migrate", t_migrate },
	{ "ksm", t_ksm },
	{ "uffd", t_uffd },
	{ "memfd", t_memfd },
	{ "writeback", t_writeback },
	{ "futex", t_futex },
	{ "syscalls", t_syscalls },
	{ "exec", t_exec },
	{ "move", t_move },
	{ "young_markers", t_young_markers },
	{ "many_views", t_many_views },
	{ "exit", t_exit },
	{ "pkeys", t_pkeys },
	{ "memcg", t_memcg },
	{ "oom", t_oom },
	{ "stress", t_stress },
};

int main(int argc, char **argv)
{
	struct sigaction sa = { .sa_sigaction = probe_handler,
				.sa_flags = SA_SIGINFO | SA_NODEFER };
	int pass = 0, fail = 0, skip = 0;

	if (argc == 2 && !strcmp(argv[1], "--print-view")) {
		/* exec resets the view */
		return view_get() == 0 ? 0 : 1;
	}
	if (!(argc > 1 && !strcmp(argv[1], "--no-views")) &&
	    prctl(PR_PT_VIEW, PR_PT_VIEW_GET, 0, 0, 0) < 0) {
		printf("PT_VIEWS: not supported (%s), skipping\n", strerror(errno));
		return SKIP;
	}
	/* Views can be built in but unusable, e.g. with page table isolation */
	if (!(argc > 1 && !strcmp(argv[1], "--no-views"))) {
		pid_t pid = fork();
		int status;

		if (!pid)
			_exit(prctl(PR_PT_VIEW, PR_PT_VIEW_CREATE, 0, 0, 0) < 0 ?
			      errno : 0);
		waitpid(pid, &status, 0);
		if (WIFEXITED(status) && WEXITSTATUS(status) == EOPNOTSUPP) {
			printf("PT_VIEWS: views unavailable (%s; pti=off?), skipping\n",
			       strerror(EOPNOTSUPP));
			return SKIP;
		}
	}
	setvbuf(stdout, NULL, _IOLBF, 0);

	while (argc > 1 && argv[1][0] == '-') {
		if (!strcmp(argv[1], "-v"))
			verbose = true;
		else if (!strcmp(argv[1], "--no-views"))
			no_views = true;
		argv++;
		argc--;
	}
	for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
		bool run = argc < 2;
		int status, ret;
		pid_t pid;

		for (int a = 1; a < argc; a++)
			run |= !strcmp(argv[a], tests[i].name);
		if (!run)
			continue;
		pid = fork();
		if (!pid) {
			sigaction(SIGSEGV, &sa, NULL);
			sigaction(SIGBUS, &sa, NULL);
			alarm(120);
			ret = tests[i].fn();
			fflush(stdout);
			_exit(failures ? FAIL : ret);
		}
		waitpid(pid, &status, 0);
		/* Empty once the test's processes are gone */
		rmdir("/sys/fs/cgroup/pt-views-memcg");
		rmdir("/sys/fs/cgroup/pt-views-oom");
		ret = WIFEXITED(status) ? WEXITSTATUS(status) : FAIL;
		if (WIFSIGNALED(status))
			printf("    killed by signal %d\n", WTERMSIG(status));
		printf("%s %s\n", ret == PASS ? "ok" : ret == SKIP ? "skip" :
		       "not ok", tests[i].name);
		pass += ret == PASS;
		skip += ret == SKIP;
		fail += ret != PASS && ret != SKIP;
	}
	printf("PT_VIEWS: %d passed, %d failed, %d skipped\n", pass, fail, skip);
	return fail ? 1 : 0;
}
