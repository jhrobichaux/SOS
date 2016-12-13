/* -*- C -*-
 *
 * Copyright 2011 Sandia Corporation. Under the terms of Contract
 * DE-AC04-94AL85000 with Sandia Corporation, the U.S.  Government
 * retains certain rights in this software.
 * 
 * This file is part of the Sandia OpenSHMEM software package. For license
 * information, see the LICENSE file in the top level directory of the
 * distribution.
 *
 */

#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <sys/mman.h>
#include <fcntl.h>
#ifdef __linux__
#include <mntent.h>
#include <sys/vfs.h>
#endif

#define SHMEM_INTERNAL_INCLUDE
#include "shmem.h"
#include "shmem_internal.h"
#include "shmem_comm.h"
#include "shmem_collectives.h"

#include "dlmalloc.h"

#ifdef ENABLE_PROFILING
#include "pshmem.h"

#pragma weak shmem_malloc = pshmem_malloc
#define shmem_malloc pshmem_malloc

#pragma weak shmem_align = pshmem_align
#define shmem_align pshmem_align

#pragma weak shmem_realloc = pshmem_realloc
#define shmem_realloc pshmem_realloc

#pragma weak shmem_free = pshmem_free
#define shmem_free pshmem_free

#pragma weak shmalloc = pshmalloc
#define shmalloc pshmalloc

#pragma weak shmemalign = pshmemalign
#define shmemalign pshmemalign

#pragma weak shrealloc = pshrealloc
#define shrealloc pshrealloc

#pragma weak shfree = pshfree
#define shfree pshfree

#endif /* ENABLE_PROFILING */

#ifndef FLOOR
#define FLOOR(a,b)      ((uint64_t)(a) - ( ((uint64_t)(a)) % (uint64_t)(b)))
#endif
#ifndef CEILING
#define CEILING(a,b)    ((uint64_t)(a) <= 0LL ? 0 : (FLOOR((a)-1,b) + (b)))
#endif
/* PAD x based on alignment factor a */
#ifndef PADBYES
#define PADBYTES(x,a)    ((x) + (a - ((x) % a)) % a)
#endif

#define ONEGIG (1024UL*1024UL*1024UL)

static char *shmem_internal_heap_curr = NULL;
static int shmem_internal_use_malloc = 0;


/*
 * scan /proc/mounts for a huge page file system with the
 * requested page size - on most Linux systems there will
 * only be a single 2MB pagesize present.
 * On success return 0, else -1.
 */

static int find_hugepage_dir(size_t page_size, char **directory)
{
    int ret = -1;
#ifdef __linux__
    struct statfs pg_size;
    struct mntent *mntent;
    FILE *fd;
    char *path;

    if (!directory || !page_size) {
        return ret;
    }

    fd = setmntent ("/proc/mounts", "r");
    if (fd == NULL) {
        return ret;
    }

    while ((mntent = getmntent(fd)) != NULL) {

        if (strcmp (mntent->mnt_type, "hugetlbfs") != 0) {
            continue;
        }

        path = mntent->mnt_dir;
        if (statfs(path, &pg_size) == 0) {
            if (pg_size.f_bsize == page_size) {
                *directory = strdup(path);
                ret = 0;
                break;
            }
        }
    }

    endmntent(fd);
#endif

    return ret;
}

/* shmalloc and friends are defined to not be thread safe, so this is
   fine.  If they change that definition, this is no longer fine and
   needs to be made thread safe. */
void*
shmem_internal_get_next(intptr_t incr)
{
    char *orig = shmem_internal_heap_curr;

    shmem_internal_heap_curr += incr;
    if (shmem_internal_heap_curr < (char*) shmem_internal_heap_base) {
        fprintf(stderr, "[%03d] WARNING: symmetric heap pointer pushed below start\n",
                shmem_internal_my_pe);
        shmem_internal_heap_curr = (char*) shmem_internal_heap_base;
    } else if (shmem_internal_heap_curr - (char*) shmem_internal_heap_base >
               shmem_internal_heap_length) {
        fprintf(stderr, "[%03d] WARNING: top of symmetric heap found\n",
                shmem_internal_my_pe);
        shmem_internal_heap_curr = orig;
        orig = (void*) -1;
    }

    return orig;
}

/* alloc VM space starting @ '_end' + 1GB */

static void *mmap_alloc(size_t bytes)
{
    char *file_name;
    int fd = 0;
    char *directory = NULL;
    const char basename[] = "hugepagefile.SOS";
    int size;
    void *requested_base = 
        (void*) (((unsigned long) shmem_internal_data_base + 
                  shmem_internal_data_length + 2 * ONEGIG) & ~(ONEGIG - 1));
    void *ret;

    if (shmem_internal_heap_use_huge_pages) {

        /*
         * check what /proc/mounts has for explicit huge page support
         */

        if(find_hugepage_dir(shmem_internal_heap_huge_page_size, &directory) == 0) {

            size = snprintf(NULL, 0, "%s/%s.%d", directory, basename, getpid());
            if (size < 0) {

                RAISE_WARN_STR("snprint returned error, cannot use huge pages");

            } else {

                file_name = malloc(size + 1);
                if (file_name) {
                    sprintf(file_name, "%s/%s.%d", directory, basename, getpid());
                    fd = open(file_name, O_CREAT | O_RDWR, 0755);
                    if (fd < 0) {
                        RAISE_WARN_STR("file open failed, cannot use huge pages");
                        fd = 0;
                    } else {
                        /* have to round up
                           by the pagesize being used */
                        bytes = CEILING(bytes, shmem_internal_heap_huge_page_size);
                    }
                }
            }
        }
    }

    ret = mmap(requested_base,
               bytes,
               PROT_READ | PROT_WRITE,
               MAP_ANON | MAP_PRIVATE,
               fd,
               0);
    if (ret == MAP_FAILED) {
        RAISE_WARN_STR("mmap for symmetric heap failed");
        return NULL;
    }
    if (fd) {
        unlink(file_name);
        close(fd);
    }
    return ret;
}

#ifdef ENABLE_HETEROGENEOUS_MEM
static inline uint8_t *align_address_upwards(uint8_t *ptr, uintptr_t align)
{
    uintptr_t addr  = (uintptr_t)ptr;
    if (addr % align != 0)
        addr += align - addr % align;
    return (uint8_t *)addr;
}

static int set_mmap_opts(shmem_partition_t *p)
{
	size_t size, bytes;
	size_t default_pgsize;
	int fd = 0;
	int id;
    char *directory = NULL;
    char *file_name = NULL;
    const char basename[] = "hugepagefile.SOS";

	p->meminfo.mmap.prot = PROT_READ | PROT_WRITE;
	p->meminfo.mmap.flags = MAP_ANONYMOUS | MAP_PRIVATE;
	p->meminfo.mmap.offset = 0;

	p->size_allocated = p->size  + SHM_INTERNAL_MSPACE_EXTRA;;

	default_pgsize = (size_t)sysconf(_SC_PAGESIZE);
	id = p->id;
	bytes =  (size_t)  PADBYTES(p->size_allocated, p->pgsize);
	/* Check it partition page size is same as default page size*/
    if (p->pgsize != default_pgsize ) {
        /*
         * check what /proc/mounts has for explicit huge page support
         */

        if(find_hugepage_dir(p->pgsize, &directory) == 0) {

            size = snprintf(NULL, 0, "%s/%s.%d.%d", directory, basename, id, getpid());
            if (size < 0) {

                RAISE_WARN_STR("snprint returned error, cannot use huge pages");

            } else {

                file_name = malloc(size + 1);
                if (file_name) {
                    sprintf(file_name, "%s/%s.%d.%d", directory, basename, id, getpid());
                    fd = open(file_name, O_CREAT | O_RDWR, 0755);
                    if (fd < 0) {
                        RAISE_WARN_STR("file open failed, cannot use huge pages for partition");
                        fd = 0;
                        bytes =  (size_t)  PADBYTES(p->size_allocated, default_pgsize);
                    } else {
                        /* have to round up
                           by the pagesize being used */
                    	bytes =  (size_t)  PADBYTES(p->size_allocated, p->pgsize);

                    }
                }
            }
        }
    }

    p->size_allocated = bytes;
    p->meminfo.mmap.fd = fd;
    p->meminfo.mmap.file_name = file_name;
}

static void cleanup_mmap_opts(shmem_partition_t *p)
{
	if (p->meminfo.mmap.fd)
	{
		unlink(p->meminfo.mmap.file_name);
		close(p->meminfo.mmap.fd);
		free(p->meminfo.mmap.file_name);
	}
}

static void set_mbind_opts(shmem_partition_t *p)
{
	/* Keep simple for now */
	p->meminfo.mbind.mode = -1;
	p->meminfo.mbind.nodemask = NULL;
}

static void cleanup_mbind_opts(shmem_partition_t *p)
{
	if (p->meminfo.mbind.nodemask != NULL)
		free(p->meminfo.mbind.nodemask);
}

static void set_madvise_opts(shmem_partition_t *p)
{
	p->meminfo.madvise.advice = -1;
}

static void cleanup_madvise_opts(shmem_partition_t *p)
{

}

static int check_partition_pgsizes()
{
}

static void * partitions_mmap_alloc()
{
	int rc = 0;
	int i;
	mspace m;
	size_t bytes, sumbytes;
	shmem_partition_t *p_desc;
	void *base, *ret, *offset;
    void *requested_base =
        (void*) (((unsigned long) shmem_internal_data_base +
                  shmem_internal_data_length + 2 * ONEGIG) & ~(ONEGIG - 1));

	/* For now assume special case that all partitions have same kind/pagesize, use one big malloc */
	sumbytes = 0;
	for (i=0; i <= shmem_internal_defined_partitions; i++)
	{
		p_desc = &symheap_partition[i];
		p_desc->size_allocated = p_desc->size + SHM_INTERNAL_MSPACE_EXTRA;

		// Make sure partition is multiple of a page size. Just being safe
		bytes =  (size_t)  PADBYTES(p_desc->size_allocated, p_desc->pgsize);
		p_desc->size_allocated = (unsigned long) bytes;

		set_mmap_opts(p_desc);
		set_mbind_opts(p_desc);
		set_madvise_opts(p_desc);

		sumbytes += bytes;
	}
	shmem_internal_heap_length = sumbytes;

	p_desc = &symheap_partition[0];
	base = (void *) align_address_upwards((uint8_t *) requested_base,(uintptr_t) p_desc->pgsize);
    ret = mmap(base,
               sumbytes,
               p_desc->meminfo.mmap.prot,
			   p_desc->meminfo.mmap.flags,
			   p_desc->meminfo.mmap.fd,
			   p_desc->meminfo.mmap.offset);
    if (ret == MAP_FAILED) {
        RAISE_WARN_STR("mmap for symmetric heap failed");
        return NULL;
    }
    offset = ret;

	for (i=0; i <= shmem_internal_defined_partitions; i++)
	{
		p_desc = &symheap_partition[i];
		p_desc->start_address = offset;
		m = create_mspace_with_base(p_desc->start_address, p_desc->size_allocated, 0);
		p_desc->msp = (void *) m;
	    cleanup_mmap_opts(p_desc);
	    cleanup_mbind_opts(p_desc);
	    cleanup_madvise_opts(p_desc);

		offset += p_desc->size_allocated;
	}

#ifdef GENERAL_CASE
    for (i=0; i <= shmem_internal_defined_partitions; i++)
    {
		set_mmap_opts(p_desc);
		set_mbind_opts(p_desc);
		set_madvise_opts(p_desc);

		base = (void *) align_address_upwards((uint8_t *) requested_base, p_desc->pgsize);
	    ret = mmap(base,
	               p_desc->size_allocated,
	               p_desc->meminfo.mmap.prot,
				   p_desc->meminfo.mmap.flags,
				   p_desc->meminfo.mmap.fd,
				   p_desc->meminfo.mmap.offset);
	    if (ret == MAP_FAILED) {
	        RAISE_WARN_STR("mmap for symmetric heap failed");
	        return NULL;
	    }
	    p_desc->start_address = ret;

	    // tbd  Apply mbind/numa options
	    // tbd Apply madvise options
	    cleanup_mmap_opts(p_desc);
	    cleanup_mbind_opts(p_desc);
	    cleanup_madvise_opts(p_desc);

		p_desc->msp = create_mspace_with_base(p_desc->start_address, p_desc->size_allocated, 0);
		/* for now assume mspace is created successfully */

		requested_base = ret + p_desc->size_allocated;
    }
#endif

	return ret;
}

/* Malloc partitions for symmetric paritions */
static void * partitions_malloc_alloc()
{
	int rc = 0;
	int i;
	size_t bytes, sumbytes;
	void *ret, *offset;
	shmem_partition_t *p_desc;

	/* For now assume special case that all partitions have same kind/pagesize, use one big malloc */
	sumbytes = 0;
	for (i=0; i <= shmem_internal_defined_partitions; i++)
	{
		p_desc = &symheap_partition[i];
		p_desc->size_allocated = p_desc->size + SHM_INTERNAL_MSPACE_EXTRA;
		// Make sure partition is multiple of a page size. Just being safe
		bytes =  (size_t)  PADBYTES(p_desc->size_allocated, p_desc->pgsize);
		p_desc->size_allocated = bytes;
		sumbytes += bytes;
	}

	shmem_internal_heap_length = sumbytes;

	ret = offset = malloc(sumbytes);
	if (ret == NULL)
	{
		fprintf(stderr, "[%03d] WARNING: Malloc of %lu bytes for  symmetric partition for partition %d failed\n",
					shmem_internal_my_pe, sumbytes, 1 );
		return NULL;
	}


	for (i=0; i <= shmem_internal_defined_partitions; i++)
	{
		symheap_partition[i].start_address = offset;
		offset += symheap_partition[i].size_allocated;
		symheap_partition[i].msp = create_mspace_with_base(symheap_partition[i].start_address, symheap_partition[i].size_allocated, 0);
	}

#ifdef GENERAL_CASE
	for (i=0; i <= shmem_internal_defined_partitions; i++)
	{
		p_desc = &symheap_partition[i];
		p_desc->size_allocated = p_desc->size + SHM_INTERNAL_MSPACE_EXTRA;
		// Make sure partition is multiple of a page size. Just being safe
		bytes =  (size_t)  PADBYTES(p_desc->size_allocated, p_desc->pgsize);
		p_desc->size_allocated = bytes;

		p_desc->start_address = (void *) malloc(bytes);
		if (ret == NULL)
		{
			fprintf(stderr, "[%03d] WARNING: Malloc of %lu bytes for  symmetric partition for partition %d failed\n",
						shmem_internal_my_pe, sumbytes, p_desc->id );
			rc |= 1;
			return NULL;
		}
		p_desc->msp = create_mspace_with_base(p_desc->start_address, p_desc->size_allocated, 0);
		/* for now assume mspace is created successfully */
	}
#endif

	return ret;
}

static void partitions_unmap()
{
	int i;
	size_t bytes, sumbytes;
	void *base;
	shmem_partition_t *p_desc;

	for (i=0; i <= shmem_internal_defined_partitions; i++)
	{
		p_desc = &symheap_partition[i];
		sumbytes += p_desc->size_allocated;

		destroy_mspace(p_desc->msp);
		p_desc->size_allocated = 0UL;
		p_desc->start_address = NULL;
	}
	base = (void *) symheap_partition[0].start_address;
	if (base != NULL)
		munmap(base, sumbytes);

#ifdef GENERAL_CASE
	for (i=0; i <= shmem_internal_defined_partitions; i++)
	{
		p_desc = &symheap_partition[i];
		if (p_desc->start_address != NULL)
		{
			destroy_mspace(p_desc->msp);
			munmap((void *)p_desc->start_address , (size_t) p_desc->size_allocated);
			p_desc->size_allocated = 0UL;
			p_desc->start_address = NULL;
		}
	}
#endif
}

static void partitions_free()
{
	int i;
	size_t bytes, sumbytes;
	void *base;
	shmem_partition_t *p_desc;

	base = (void *) symheap_partition[0].start_address;
	for (i=0; i <= shmem_internal_defined_partitions; i++)
	{
		p_desc = &symheap_partition[i];
		sumbytes += p_desc->size_allocated;

		destroy_mspace(p_desc->msp);
		p_desc->size_allocated = 0UL;
		p_desc->start_address = NULL;
	}

	if (base != NULL)
		munmap(base, sumbytes);

#ifdef GENERAL_CASE
	for (i=0; i <= shmem_internal_defined_partitions; i++)
	{
		p_desc = &symheap_partition[i];
		if (p_desc->start_address != NULL)
		{
			destroy_mspace(p_desc->msp);
			free((void *)p_desc->start_address);
			p_desc->size_allocated = 0UL;
			p_desc->start_address = NULL;
		}
	}
#endif /* GENERAL_CASE */
}

int shmem_internal_get_partition_index_from_addr(void * ptr)
{
	int i;
	int rc = -1;
	uintptr_t addr_strt, addr_end;

	uintptr_t addr  = (uintptr_t) ptr;
	for (i=0; i<=shmem_internal_defined_partitions;i++)
	{
		addr_strt = (uintptr_t) symheap_partition[i].start_address;
		addr_end = (uintptr_t) (symheap_partition[i].start_address + symheap_partition[i].size_allocated -
				symheap_partition[i].pgsize/sizeof(uintptr_t));
		if ((addr >= addr_strt) && (addr <=addr_end))
		{
			rc = i;
			break;
		}
	}
	return rc;
}

int shmem_internal_get_partition_index_from_id(int partition_id)
{
	int i;
	int rc=-1;
	for (i=0; i<=shmem_internal_defined_partitions;i++)
	{
		if (symheap_partition[i].id == partition_id)
		{
			rc = i;
			break;
		}
	}
	return rc;
}
#endif /* ENABLE_HETEROGENEOUS_MEM */


int
shmem_internal_symmetric_init(size_t requested_length, int use_malloc)
{
    shmem_internal_use_malloc = use_malloc;

    /* add library overhead such that the max can be shmalloc()'ed */
    shmem_internal_heap_length = requested_length + (1024*1024);

    if (0 == shmem_internal_use_malloc) {
#ifndef ENABLE_HETEROGENEOUS_MEM
        shmem_internal_heap_base =
            shmem_internal_heap_curr =
            mmap_alloc(shmem_internal_heap_length);
#else
        shmem_internal_heap_base =
            shmem_internal_heap_curr = partitions_mmap_alloc();
#endif
    } else {
#ifndef ENABLE_HETEROGENEOUS_MEM
        shmem_internal_heap_base =
            shmem_internal_heap_curr =
            malloc(shmem_internal_heap_length);
#else
        shmem_internal_heap_base =
            shmem_internal_heap_curr = partitions_malloc_alloc();
#endif
    }

   	if (shmem_internal_debug > 0)
   	{
   		fprintf(stdout,"Debug: In routine shmem_internal_symmetric_init\n");
   		fprintf(stdout,"Debug: shmem_internal_defined_partitions : %d\n", shmem_internal_defined_partitions);
   		for (int i=0;i<shmem_internal_defined_partitions+1; i++)
   		{
   			fprintf(stdout,"Debug: Partition # : %d\n", i);
   			shmem_partition_print_info(&symheap_partition[i]);
   			shmem_partition_print_meminfo(&(symheap_partition[i].meminfo));
   		}
   	}

    return (NULL == shmem_internal_heap_base) ? -1 : 0;
}

int
shmem_internal_symmetric_fini(void)
{
    if (NULL != shmem_internal_heap_base) {
        if (0 == shmem_internal_use_malloc) {
#ifndef ENABLE_HETEROGENEOUS_MEM
            munmap( (void*)shmem_internal_heap_base, (size_t)shmem_internal_heap_length );
#else
            partitions_unmap();
#endif
        } else {
#ifndef ENABLE_HETEROGENEOUS_MEM
            free(shmem_internal_heap_base);
#else
            partitions_free();
#endif
        }
        shmem_internal_heap_length = 0;
        shmem_internal_heap_base = shmem_internal_heap_curr = NULL;
    }

    return 0;
}

#ifdef ENABLE_HETEROGENEOUS_MEM

#endif

void*
shmem_internal_shmalloc(size_t size)
{
    void *ret;

    SHMEM_MUTEX_LOCK(shmem_internal_mutex_alloc);
#ifndef ENABLE_HETEROGENEOUS_MEM
    ret = dlmalloc(size);
#else
    ret = mspace_malloc(symheap_partition[0].msp, size);
#endif
    SHMEM_MUTEX_UNLOCK(shmem_internal_mutex_alloc);

    return ret;
}

void *
shmem_malloc(size_t size)
{
    void *ret;

    SHMEM_ERR_CHECK_INITIALIZED();

    SHMEM_MUTEX_LOCK(shmem_internal_mutex_alloc);
#ifndef ENABLE_HETEROGENEOUS_MEM
    ret = dlmalloc(size);
#else
    ret = mspace_malloc(symheap_partition[1].msp, size);
#endif
    SHMEM_MUTEX_UNLOCK(shmem_internal_mutex_alloc);

    shmem_internal_barrier_all();

    return ret;
}

void
shmem_free(void *ptr)
{
    SHMEM_ERR_CHECK_INITIALIZED();

    SHMEM_MUTEX_LOCK(shmem_internal_mutex_alloc);
#ifndef ENABLE_HETEROGENEOUS_MEM
    dlfree(ptr);
#else
    int index = shmem_internal_get_partition_index_from_addr(ptr);
    mspace_free(symheap_partition[index].msp, ptr);
#endif
    SHMEM_MUTEX_UNLOCK(shmem_internal_mutex_alloc);

    shmem_internal_barrier_all();
}


void *
shmem_realloc(void *ptr, size_t size)
{
    void *ret;

    SHMEM_ERR_CHECK_INITIALIZED();

    SHMEM_MUTEX_LOCK(shmem_internal_mutex_alloc);
#ifndef ENABLE_HETEROGENEOUS_MEM
    ret = dlrealloc(ptr, size);
#else
    if (ptr == NULL) /* Assume allocating to default partition */
    {
    	ret = mspace_malloc(symheap_partition[1].msp, size);
    } else {
    	int index = shmem_internal_get_partition_index_from_addr(ptr);
    	ret = mspace_realloc(symheap_partition[index].msp, ptr, size);
    }
#endif
    SHMEM_MUTEX_UNLOCK(shmem_internal_mutex_alloc);

    shmem_internal_barrier_all();

    return ret;
}


void *
shmem_align(size_t alignment, size_t size)
{
    void *ret;

    SHMEM_ERR_CHECK_INITIALIZED();

    SHMEM_MUTEX_LOCK(shmem_internal_mutex_alloc);
#ifndef ENABLE_HETEROGENEOUS_MEM
    ret = dlmemalign(alignment, size);
#else
    ret = mspace_memalign(symheap_partition[1].msp, alignment, size);
#endif
    SHMEM_MUTEX_UNLOCK(shmem_internal_mutex_alloc);

    shmem_internal_barrier_all();

    return ret;
}

void* shmemx_kind_malloc(size_t size, int partition_id)
{
	void *ret;
	int index;

	SHMEM_ERR_CHECK_INITIALIZED();
    SHMEM_MUTEX_LOCK(shmem_internal_mutex_alloc);
#ifndef ENABLE_HETEROGENEOUS_MEM
	ret = dlmalloc(size);
#else
	index = shmem_internal_get_partition_index_from_id(partition_id);
    ret = mspace_malloc(symheap_partition[index].msp, size);
#endif
    SHMEM_MUTEX_UNLOCK(shmem_internal_mutex_alloc);
    return ret;
}

void* shmemx_kind_align(size_t alignment, size_t size, int partition_id)
{
	void *ret;
	int index;

	SHMEM_ERR_CHECK_INITIALIZED();
    SHMEM_MUTEX_LOCK(shmem_internal_mutex_alloc);
#ifndef ENABLE_HETEROGENEOUS_MEM
    ret = dlmemalign(alignment, size);
#else
	index = shmem_internal_get_partition_index_from_id(partition_id);
    ret = mspace_memalign(symheap_partition[index].msp, alignment, size);
#endif
    SHMEM_MUTEX_UNLOCK(shmem_internal_mutex_alloc);
    return ret;
}

/* The following functions were renamed in OpenSHMEM 1.2 and the old names were
 * deprecated.  Note that if PROFILING_ENABLED, the profiling macros will cause
 * these functions to directly call the corresponding pshmem_* routine which
 * should make the up-call invsible to a profiling tool.
 */

void * shmalloc(size_t size)
{
    return shmem_malloc(size);
}


void shfree(void *ptr)
{
    shmem_free(ptr);
}


void * shrealloc(void *ptr, size_t size)
{
    return shmem_realloc(ptr, size);
}


void * shmemalign(size_t alignment, size_t size)
{
    return shmem_align(alignment, size);
}

/* For symmetric partitions */
