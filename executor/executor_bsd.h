// Copyright 2017 syzkaller project authors. All rights reserved.
// Use of this source code is governed by Apache 2 LICENSE that can be found in the LICENSE file.

#include <fcntl.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static void os_init(int argc, char** argv, void* data, size_t data_size)
{
#if GOOS_openbsd
	// W^X not allowed by default on OpenBSD.
	int prot = PROT_READ | PROT_WRITE;
#else
	int prot = PROT_READ | PROT_WRITE | PROT_EXEC;
#endif

	if (mmap(data, data_size, prot, MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0) != data)
		fail("mmap of data segment failed");

	// Makes sure the file descriptor limit is sufficient to map control pipes.
	struct rlimit rlim;
	rlim.rlim_cur = rlim.rlim_max = kMaxFd;
	setrlimit(RLIMIT_NOFILE, &rlim);
}

static long execute_syscall(const call_t* c, long a[kMaxArgs])
{
	if (c->call)
		return c->call(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8]);
	return __syscall(c->sys_nr, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8]);
}

#if GOOS_freebsd

// TODO(mptre): once kcov is merged to FreeBSD[1], just include sys/kcov.h for
// both FreeBSD and OpenBSD.
//
// [1] https://reviews.freebsd.org/D14599

#define KIOENABLE _IOWINT('c', 2) // Enable coverage recording
#define KIODISABLE _IO('c', 3) // Disable coverage recording
#define KIOSETBUFSIZE _IOWINT('c', 4) // Set the buffer size

#define KCOV_MODE_NONE -1
#define KCOV_MODE_TRACE_PC 0
#define KCOV_MODE_TRACE_CMP 1

#elif GOOS_openbsd

// TODO(mptre): temporary defined until trace-cmp is fully supported
#define KCOV_MODE_TRACE_CMP 2

#include <sys/kcov.h>

#endif

#if GOOS_freebsd || GOOS_openbsd

static void cover_open(cover_t* cov)
{
	int fd = open("/dev/kcov", O_RDWR);
	if (fd == -1)
		fail("open of /dev/kcov failed");
	if (dup2(fd, cov->fd) < 0)
		fail("failed to dup2(%d, %d) cover fd", fd, cov->fd);
	close(fd);

#if GOOS_freebsd
	// On FreeBSD provide the size in bytes, not in number of entries.
	if (ioctl(cov->fd, KIOSETBUFSIZE, kCoverSize * sizeof(uint64_t)))
		fail("ioctl init trace write failed");
#elif GOOS_openbsd
	unsigned long cover_size = kCoverSize;
	if (ioctl(cov->fd, KIOSETBUFSIZE, &cover_size))
		fail("ioctl init trace write failed");
#endif

#if GOOS_freebsd
	// FreeBSD only supports kcov on 64-bit platforms and always uses
	// entries of type uint64_t.
	size_t mmap_alloc_size = kCoverSize * sizeof(uint64_t);
#else
	size_t mmap_alloc_size = kCoverSize * (is_kernel_64_bit ? 8 : 4);
#endif
	void* mmap_ptr = mmap(NULL, mmap_alloc_size, PROT_READ | PROT_WRITE,
			      MAP_SHARED, cov->fd, 0);
	if (mmap_ptr == MAP_FAILED)
		fail("cover mmap failed");
	cov->data = (char*)mmap_ptr;
	cov->data_end = cov->data + mmap_alloc_size;
}

static void cover_enable(cover_t* cov, bool collect_comps)
{
	int kcov_mode = collect_comps ? KCOV_MODE_TRACE_CMP : KCOV_MODE_TRACE_PC;
#if GOOS_freebsd
	// FreeBSD uses an int as the third argument.
	if (ioctl(cov->fd, KIOENABLE, kcov_mode))
		exitf("cover enable write trace failed, mode=%d", kcov_mode);
#elif GOOS_openbsd
	// OpenBSD uses an pointer to an int as the third argument.
	if (ioctl(cov->fd, KIOENABLE, &kcov_mode))
		exitf("cover enable write trace failed, mode=%d", kcov_mode);
#endif
}

static void cover_reset(cover_t* cov)
{
	*(uint64*)cov->data = 0;
}

static void cover_collect(cover_t* cov)
{
	cov->size = *(uint64*)cov->data;
}

static bool cover_check(uint32 pc)
{
	return true;
}

static bool cover_check(uint64 pc)
{
	return true;
}
#else
#include "nocover.h"
#endif
