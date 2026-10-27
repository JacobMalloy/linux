// SPDX-License-Identifier: GPL-2.0
/*
 * Test /proc/pid/young_bitmap and /proc/pid/young_bitmap_flush.
 *
 * young_bitmap is indexed by virtual page number and holds one bit per
 * page.  Reading reports whether each page's PTE had its accessed bit set
 * and clears it; writing a bitmap clears the selected pages only.  Both
 * affect this process's PTEs only.
 */
#undef NDEBUG
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define NPAGES		256
#define HOLE_START	64
#define HOLE_END	128

static long page_size;

static int test_bit(const uint8_t *map, unsigned long n)
{
	return (map[n / 8] >> (n % 8)) & 1;
}

static void set_bit(uint8_t *map, unsigned long n)
{
	map[n / 8] |= 1 << (n % 8);
}

static off_t vpn(const void *p)
{
	return (uintptr_t)p / page_size;
}

static void read_bitmap(int fd, const void *addr, uint8_t *map, size_t bytes)
{
	ssize_t rv = pread(fd, map, bytes, vpn(addr));

	assert(rv == (ssize_t)bytes);
}

static void clear_bitmap(int fd, const void *addr, const uint8_t *map,
			 size_t bytes)
{
	ssize_t rv = pwrite(fd, map, bytes, vpn(addr));

	assert(rv == (ssize_t)bytes);
}

static void touch(volatile char *p)
{
	*p = *p + 1;
}

static int mapped(unsigned long i)
{
	return i < HOLE_START || i >= HOLE_END;
}

/* Touch every @stride'th mapped page. */
static void touch_all(char *base, unsigned long stride)
{
	unsigned long i;

	for (i = 0; i < NPAGES; i += stride)
		if (mapped(i))
			touch(base + i * page_size);
}

int main(void)
{
	uint8_t map[NPAGES / 8], sel[NPAGES / 8];
	char buf[8];
	char *base;
	int fd, ffd;
	unsigned long i;
	ssize_t rv;

	page_size = sysconf(_SC_PAGESIZE);

	fd = open("/proc/self/young_bitmap", O_RDWR);
	if (fd == -1 && errno == ENOENT)
		return 4;	/* KSFT_SKIP */
	assert(fd >= 0);
	ffd = open("/proc/self/young_bitmap_flush", O_RDWR);
	assert(ffd >= 0);

	/* flags: default 0, 1 = flush TLB, 2 = mark folios young */
	rv = pread(ffd, buf, sizeof(buf), 0);
	assert(rv == 2 && !memcmp(buf, "0\n", 2));
	assert(pwrite(ffd, "4", 1, 0) == -1 && errno == EINVAL);
	assert(pwrite(ffd, "3", 1, 0) == 1);
	rv = pread(ffd, buf, sizeof(buf), 0);
	assert(rv == 2 && !memcmp(buf, "3\n", 2));
	assert(pwrite(ffd, "1", 1, 0) == 1);
	rv = pread(ffd, buf, sizeof(buf), 0);
	assert(rv == 2 && !memcmp(buf, "1\n", 2));

	base = mmap(NULL, NPAGES * page_size, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	assert(base != MAP_FAILED);
	madvise(base, NPAGES * page_size, MADV_NOHUGEPAGE);
	assert(!munmap(base + HOLE_START * page_size,
		       (HOLE_END - HOLE_START) * page_size));

	/* Every touched page reads as accessed, the hole reads as 0. */
	touch_all(base, 1);
	read_bitmap(fd, base, map, sizeof(map));
	for (i = 0; i < NPAGES; i++)
		assert(test_bit(map, i) == mapped(i));

	/* The read cleared the accessed bits. */
	read_bitmap(fd, base, map, sizeof(map));
	for (i = 0; i < NPAGES; i++)
		assert(!test_bit(map, i));

	/* With flushing enabled, new accesses are always seen. */
	touch_all(base, 2);
	read_bitmap(fd, base, map, sizeof(map));
	for (i = 0; i < NPAGES; i++)
		assert(test_bit(map, i) == (mapped(i) && !(i % 2)));
	read_bitmap(fd, base, map, sizeof(map));
	for (i = 0; i < NPAGES; i++)
		assert(!test_bit(map, i));

	/* Writing clears only the selected pages, without reading. */
	touch_all(base, 1);
	memset(sel, 0, sizeof(sel));
	for (i = 0; i < 32; i++)
		set_bit(sel, i);
	clear_bitmap(fd, base, sel, sizeof(sel));
	read_bitmap(fd, base, map, sizeof(map));
	for (i = 0; i < NPAGES; i++)
		assert(test_bit(map, i) == (i >= 32 && mapped(i)));

	/* Unaligned position: one byte covers pages 34..41. */
	touch_all(base, 2);
	read_bitmap(fd, base + 34 * page_size, map, 1);
	for (i = 0; i < 8; i++)
		assert(test_bit(map, i) == !((34 + i) % 2));
	/* Only pages 34..41 were read and cleared. */
	read_bitmap(fd, base, map, sizeof(map));
	for (i = 0; i < NPAGES; i++)
		assert(test_bit(map, i) ==
		       (mapped(i) && !(i % 2) && (i < 34 || i > 41)));

	/* Sequential read() advances the position by 8 pages per byte. */
	assert(lseek(fd, vpn(base), SEEK_SET) == vpn(base));
	assert(read(fd, map, 2) == 2);
	assert(lseek(fd, 0, SEEK_CUR) == vpn(base) + 16);

	/* Without flushing, reading still clears. */
	assert(pwrite(ffd, "0", 1, 0) == 1);
	touch_all(base, 1);
	read_bitmap(fd, base, map, sizeof(map));
	for (i = 0; i < NPAGES; i++)
		assert(test_bit(map, i) == mapped(i));
	read_bitmap(fd, base, map, sizeof(map));
	for (i = 0; i < NPAGES; i++)
		assert(!test_bit(map, i));
	assert(pwrite(ffd, "1", 1, 0) == 1);

	/* Only this process's PTE is cleared, not other mappings of the page. */
	{
		char path[64], c;
		int p2c[2], c2p[2], cfd;
		char *shared;
		pid_t pid;

		shared = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
			      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
		assert(shared != MAP_FAILED);
		assert(!pipe(p2c) && !pipe(c2p));

		pid = fork();
		assert(pid >= 0);
		if (pid == 0) {
			touch(shared);
			assert(write(c2p[1], "x", 1) == 1);
			assert(read(p2c[0], &c, 1) == 1);
			_exit(0);
		}
		touch(shared);
		assert(read(c2p[0], &c, 1) == 1);

		read_bitmap(fd, shared, map, 1);
		assert(test_bit(map, 0));
		read_bitmap(fd, shared, map, 1);
		assert(!test_bit(map, 0));

		snprintf(path, sizeof(path), "/proc/%d/young_bitmap", pid);
		cfd = open(path, O_RDONLY);
		assert(cfd >= 0);
		read_bitmap(cfd, shared, map, 1);
		assert(test_bit(map, 0));
		close(cfd);

		assert(write(p2c[1], "x", 1) == 1);
		assert(waitpid(pid, NULL, 0) == pid);
	}

	/*
	 * Page tables whose PMD was not used since the last read are skipped:
	 * touch one page in each of a few 2 MiB regions and check that exactly
	 * those are reported, with and without marking folios young.
	 */
	{
		const unsigned long big = 8 * 512;	/* 16 MiB of 4 KiB pages */
		uint8_t *bmap = malloc(big / 8);
		const char *flags[] = { "1", "3" };
		char *area;
		int f;

		assert(bmap);
		area = mmap(NULL, big * page_size, PROT_READ | PROT_WRITE,
			    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		assert(area != MAP_FAILED);
		madvise(area, big * page_size, MADV_NOHUGEPAGE);
		for (i = 0; i < big; i++)
			touch(area + i * page_size);

		for (f = 0; f < 2; f++) {
			assert(pwrite(ffd, flags[f], 1, 0) == 1);
			read_bitmap(fd, area, bmap, big / 8);	/* reset */
			read_bitmap(fd, area, bmap, big / 8);
			for (i = 0; i < big; i++)
				assert(!test_bit(bmap, i));

			for (i = 700; i < big; i += 1500)
				touch(area + i * page_size);
			read_bitmap(fd, area, bmap, big / 8);
			for (i = 0; i < big; i++)
				assert(test_bit(bmap, i) == (i >= 700 && (i - 700) % 1500 == 0));
		}
		/*
		 * A read of part of a PMD must not hide young PTEs from a
		 * later read of the rest: read the area in 64-page pieces.
		 */
		for (i = 0; i < big; i += 300)
			touch(area + i * page_size);
		for (i = 0; i < big; i += 64)
			read_bitmap(fd, area + i * page_size, bmap + i / 8, 8);
		for (i = 0; i < big; i++)
			assert(test_bit(bmap, i) == !(i % 300));

		assert(pwrite(ffd, "1", 1, 0) == 1);
		munmap(area, big * page_size);
		free(bmap);
	}

	/* Positions past the address space read as EOF. */
	assert(pread(fd, map, 1, (off_t)(UINTPTR_MAX / page_size)) == 0);

	return 0;
}
