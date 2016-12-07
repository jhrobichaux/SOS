/* -*- C -*-
 *
 * Copyright 2011 Sandia Corporation. Under the terms of Contract
 * DE-AC04-94AL85000 with Sandia Corporation, the U.S.  Government
 * retains certain rights in this software.
 * 
 * Copyright (c) 2015 Intel Corporation. All rights reserved.
 * This software is available to you under the BSD license.
 *
 * This file is part of the Sandia OpenSHMEM software package. For license
 * information, see the LICENSE file in the top level directory of the
 * distribution.
 *
 */

#include "config.h"

#include <stdlib.h>
#include <sys/time.h>
#include <sys/param.h>
#include <stdio.h>
#include <limits.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <regex.h>

#define SHMEM_INTERNAL_INCLUDE
#include "shmem.h"
#include "shmemx.h"
#include "shmem_internal.h"
#include "shmem_collectives.h"
#include "shmem_comm.h"
#include "runtime.h"


#ifdef __APPLE__
#include <mach-o/getsect.h>
#else
extern char data_start;
extern char end;
#endif

void *shmem_internal_heap_base = NULL;
long shmem_internal_heap_length = 0;
void *shmem_internal_data_base = NULL;
long shmem_internal_data_length = 0;
int shmem_internal_heap_use_huge_pages = 0;
long shmem_internal_heap_huge_page_size = 0;

int shmem_internal_my_pe = -1;
int shmem_internal_num_pes = -1;
int shmem_internal_initialized = 0;
int shmem_internal_finalized = 0;
int shmem_internal_initialized_with_start_pes = 0;
int shmem_internal_global_exit_called = 0;

int shmem_internal_thread_level;
int shmem_internal_debug = 0;
 
#ifdef ENABLE_HETEROGENEOUS_MEM
extern char **environ;
#define MAXSTRING 257
shmem_partition_t symheap_partition[SHM_INTERNAL_MAX_PARTITIONS] = {{ 0 }};
int shmem_internal_defined_partitions = 0;
#endif


#ifdef ENABLE_THREADS
shmem_internal_mutex_t shmem_internal_mutex_alloc;
#endif

#ifdef USE_ON_NODE_COMMS
char *shmem_internal_location_array = NULL;
#endif

#ifdef MAXHOSTNAMELEN
static char shmem_internal_my_hostname[MAXHOSTNAMELEN];
#else
static char shmem_internal_my_hostname[HOST_NAME_MAX];
#endif


static void
shmem_internal_shutdown(int barrier_requested)
{
    if (!shmem_internal_initialized ||
        shmem_internal_finalized) {
        return;
    }
    shmem_internal_finalized = 1;

    if (barrier_requested)
        shmem_internal_barrier_all();

    shmem_transport_fini();

#ifdef USE_XPMEM
    shmem_transport_xpmem_fini();
#endif
#ifdef USE_CMA
    shmem_transport_cma_fini();
#endif

    SHMEM_MUTEX_DESTROY(shmem_internal_mutex_alloc);

    shmem_internal_symmetric_fini();
    shmem_runtime_fini();
}


static void
shmem_internal_shutdown_atexit(void)
{
    if ( shmem_internal_initialized && !shmem_internal_finalized &&
         !shmem_internal_initialized_with_start_pes && !shmem_internal_global_exit_called &&
         shmem_internal_my_pe == 0) {
        fprintf(stderr, "Warning: shutting down without a call to shmem_finalize()\n");
    }

    shmem_internal_shutdown(0);
}


void
shmem_internal_start_pes(int npes)
{
    int tl_provided;

    shmem_internal_initialized_with_start_pes = 1;
    shmem_internal_init(SHMEMX_THREAD_SINGLE, &tl_provided);
}


void
shmem_internal_init(int tl_requested, int *tl_provided)
{
    int ret;
    int radix = -1, crossover = -1;
    long heap_size, eager_size;
    int heap_use_malloc = 0;

    int runtime_initialized   = 0;
    int transport_initialized = 0;
#ifdef USE_XPMEM
    int xpmem_initialized     = 0;
#endif
#ifdef USE_CMA
    int cma_initialized       = 0;
#endif

    ret = shmem_runtime_init();
    if (0 != ret) {
        fprintf(stderr, "ERROR: runtime init failed: %d\n", ret);
        goto cleanup;
    }
    runtime_initialized = 1;
    shmem_internal_my_pe = shmem_runtime_get_rank();
    shmem_internal_num_pes = shmem_runtime_get_size();

    /* Ensure that the vendor string will not cause an overflow in user code */
    if (sizeof(SHMEM_VENDOR_STRING) > SHMEM_MAX_NAME_LEN) {
        fprintf(stderr,
                "[%03d] ERROR: SHMEM_VENDOR_STRING length (%zu) exceeds SHMEM_MAX_NAME_LEN (%d)\n",
                shmem_internal_my_pe, sizeof(SHMEM_VENDOR_STRING), SHMEM_MAX_NAME_LEN);
        goto cleanup;
    }

    /* Process environment variables */
    radix = shmem_util_getenv_long("COLL_RADIX", 0, 4);
    crossover = shmem_util_getenv_long("COLL_CROSSOVER", 0, 4);

    eager_size = shmem_util_getenv_long("BOUNCE_SIZE", 1, 2048);
    heap_use_malloc = shmem_util_getenv_long("SYMMETRIC_HEAP_USE_MALLOC", 0, 0);
    shmem_internal_debug = (NULL != shmem_util_getenv_str("DEBUG")) ? 1 : 0;

#ifndef USE_HETEROGENEOUS_MEM
    heap_size = shmem_util_getenv_long("SYMMETRIC_SIZE", 1, 512 * 1024 * 1024);
#else
    /* Parse Envs for symmetric partitions */ 
    ret = shmem_internal_parse_partition_env();
    if (0 != ret) {
    	fprintf(stderr,
    	                "[%03d] ERROR: Processing environment variables for symmetric heap failed: %d\n",
    	                shmem_internal_my_pe, ret);
    	goto cleanup;
    }
#endif

    /* Find symmetric data */
#ifdef __APPLE__
    shmem_internal_data_base = (void*) get_etext();
    shmem_internal_data_length = get_end() - get_etext();
#else
    shmem_internal_data_base = &data_start;
    shmem_internal_data_length = (unsigned long) &end  - (unsigned long) &data_start;
#endif

#ifdef USE_ON_NODE_COMMS
    shmem_internal_location_array = malloc(sizeof(char) * shmem_internal_num_pes);
    if (NULL == shmem_internal_location_array) goto cleanup;

    memset(shmem_internal_location_array, -1, shmem_internal_num_pes);
#endif

    /* create symmetric heap */
    ret = shmem_internal_symmetric_init(heap_size, heap_use_malloc);
    if (0 != ret) {
        fprintf(stderr,
                "[%03d] ERROR: symmetric heap initialization failed: %d\n",
                shmem_internal_my_pe, ret);
        goto cleanup;
    }

    /* Initialize transport devices */
    ret = shmem_transport_init(eager_size);
    if (0 != ret) {
        fprintf(stderr,
                "[%03d] ERROR: Transport init failed\n",
                shmem_internal_my_pe);
        goto cleanup;
    }
    transport_initialized = 1;
#ifdef USE_XPMEM
    ret = shmem_transport_xpmem_init(eager_size);
    if (0 != ret) {
        fprintf(stderr,
                "[%03d] ERROR: XPMEM init failed\n",
                shmem_internal_my_pe);
        goto cleanup;
    }
    xpmem_initialized = 1;
#endif
#ifdef USE_CMA
    shmem_transport_cma_put_max = shmem_util_getenv_long("CMA_PUT_MAX", 1, 8*1024);
    shmem_transport_cma_get_max = shmem_util_getenv_long("CMA_GET_MAX", 1, 16*1024);

    ret = shmem_transport_cma_init(eager_size);
    if (0 != ret) {
        fprintf(stderr,
                "[%03d] ERROR: CMA init failed\n",
                shmem_internal_my_pe);
        goto cleanup;
    }
    cma_initialized = 1;
#endif

    /* exchange information */
    ret = shmem_runtime_exchange();
    if (0 != ret) {
        fprintf(stderr, "[%03d] ERROR: runtime exchange failed: %d\n", 
                shmem_internal_my_pe, ret);
        goto cleanup;
    }

    /* finish transport initialization after information sharing. */
    ret = shmem_transport_startup();
    if (0 != ret) {
        fprintf(stderr,
                "[%03d] ERROR: Transport startup failed\n",
                shmem_internal_my_pe);
        goto cleanup;
    }

#ifdef USE_XPMEM
    ret = shmem_transport_xpmem_startup();
    if (0 != ret) {
        fprintf(stderr,
                "[%03d] ERROR: XPMEM startup failed\n",
                shmem_internal_my_pe);
        goto cleanup;
    }
#endif
#ifdef USE_CMA
    ret = shmem_transport_cma_startup();
    if (0 != ret) {
        fprintf(stderr,
                "[%03d] ERROR: CMA startup failed\n",
                shmem_internal_my_pe);
        goto cleanup;
    }
#endif

    ret = shmem_internal_collectives_init(crossover, radix);
    if (ret != 0) {
        fprintf(stderr,
                "[%03d] ERROR: initialization of collectives failed: %d\n",
                shmem_internal_my_pe, ret);
        goto cleanup;
    }

    atexit(shmem_internal_shutdown_atexit);
    shmem_internal_initialized = 1;

    /* set up threading */
    SHMEM_MUTEX_INIT(shmem_internal_mutex_alloc);
#ifdef ENABLE_THREADS
    shmem_internal_thread_level = tl_requested;
    *tl_provided = tl_requested;
#else
    shmem_internal_thread_level = SHMEMX_THREAD_SINGLE;
    *tl_provided = SHMEMX_THREAD_SINGLE;
#endif

    /* get hostname for shmem_getnodename */
    if (gethostname(shmem_internal_my_hostname,
                    sizeof(shmem_internal_my_hostname))) {
        snprintf(shmem_internal_my_hostname,
                    sizeof(shmem_internal_my_hostname),
                    "ERR: gethostname '%s'?", strerror(errno));
    }

    /* last minute printing of information */
    if (0 == shmem_internal_my_pe) {
        if (NULL != shmem_util_getenv_str("VERSION")) {
            printf(PACKAGE_STRING "\n");
            fflush(NULL);
        }

        if (NULL != shmem_util_getenv_str("INFO")) {
            printf(PACKAGE_STRING "\n\n");
            printf("SMA_VERSION             %s\n",
                   (NULL != shmem_util_getenv_str("VERSION")) ? "Set" : "Not set");
            printf("\tIf set, print library version at startup\n");
            printf("SMA_INFO                %s\n",
                   (NULL != shmem_util_getenv_str("INFO")) ? "Set" : "Not set");
            printf("\tIf set, print this help message at startup\n");
            printf("SMA_SYMMETRIC_SIZE      %ld\n", heap_size);
            printf("\tSymmetric heap size\n");
            printf("SMA_SYMMETRIC_HEAP_USE_MALLOC %s\n",
                   (0 != heap_use_malloc) ? "Set" : "Not set");
            printf("\tIf set, allocate the symmetric heap using malloc\n");
            if (heap_use_malloc == 0) {
                printf("SMA_SYMMETRIC_HEAP_USE_HUGE_PAGES %s\n",
                        shmem_internal_heap_use_huge_pages ? "Yes" : "No");
                if (shmem_internal_heap_use_huge_pages) {
                    printf("SMA_SYMMETRIC_HEAP_PAGE_SIZE %ld \n",
                           shmem_internal_heap_huge_page_size);
                }
                printf("\tSymmetric heap use large pages\n");
            }
            printf("SMA_COLL_CROSSOVER      %d\n", crossover);
            printf("\tCross-over between linear and tree collectives\n");
            printf("SMA_COLL_RADIX          %d\n", radix);
            printf("\tRadix for tree-based collectives\n");
            printf("SMA_BOUNCE_SIZE         %ld\n", eager_size);
            printf("\tMaximum message size to bounce buffer\n");
            printf("SMA_BARRIER_ALGORITHM   %s\n", coll_type_str[shmem_internal_barrier_type]);
            printf("\tAlgorithm for barrier.  Options are auto, linear, tree, dissem\n");
            printf("SMA_BCAST_ALGORITHM     %s\n", coll_type_str[shmem_internal_bcast_type]);
            printf("\tAlgorithm for broadcast.  Options are auto, linear, tree\n");
            printf("SMA_REDUCE_ALGORITHM    %s\n", coll_type_str[shmem_internal_reduce_type]);
            printf("\tAlgorithm for reductions.  Options are auto, linear, tree, recdbl\n");
            printf("SMA_COLLECT_ALGORITHM   %s\n", coll_type_str[shmem_internal_collect_type]);
            printf("\tAlgorithm for collect.  Options are auto, linear\n");
            printf("SMA_FCOLLECT_ALGORITHM  %s\n", coll_type_str[shmem_internal_fcollect_type]);
            printf("\tAlgorithm for fcollect.  Options are auto, linear, ring, recdbl\n");
#ifdef USE_CMA
            printf("SMA_CMA_PUT_MAX         %zu\n", shmem_transport_cma_put_max);
            printf("SMA_CMA_GET_MAX         %zu\n", shmem_transport_cma_get_max);
#endif /* USE_CMA */
            printf("SMA_DEBUG               %s\n", (NULL != shmem_util_getenv_str("DEBUG")) ? "On" : "Off");
            printf("\tEnable debugging messages.\n");

            shmem_transport_print_info();
            printf("\n");
            fflush(NULL);
        }
    }

    /* finish up */
    shmem_runtime_barrier();
    return;

 cleanup:
    if (transport_initialized) {
        shmem_transport_fini();
    }

#ifdef USE_XPMEM
    if (xpmem_initialized) {
        shmem_transport_xpmem_fini();
    }
#endif
#ifdef USE_CMA
    if (cma_initialized) {
        shmem_transport_cma_fini();
    }
#endif
    if (NULL != shmem_internal_data_base) {
        shmem_internal_symmetric_fini();
    }
    if (runtime_initialized) {
        shmem_runtime_fini();
    }
    abort();
}


char *
shmem_internal_nodename(void)
{
    return shmem_internal_my_hostname;
}


void shmem_internal_finalize(void)
{
    shmem_internal_shutdown(1);
}

void
shmem_internal_global_exit(int status)
{
    char str[256];

    snprintf(str, 256, "PE %d called shmem_global_exit with status %d", shmem_internal_my_pe, status);

    shmem_internal_global_exit_called = 1;
    shmem_runtime_abort(status, str);
}

#ifdef ENABLE_HETEROGENEOUS_MEM
#define S_MAXSTR 257
#define S_MAXREGEXMATCH 8
/* Stuff below for partitions */
/* atol() + optional scaled suffix recognition: 1K, 2M, 3G, 1T */
static long
init_atol_scaled(char *s)
{
    long val;
    char *e;
    errno = 0;

    val = strtol(s,&e,0);
    if(errno != 0 || e == s) {
        shmem_runtime_abort(1, "env var conversion");
    }
    if (e == NULL || *e =='\0')
        return val;

    if (*e == 'K')
        val *= 1024L;
    else if (*e == 'M')
        val *= 1024L*1024L;
    else if (*e == 'G')
        val *= 1024L*1024L*1024L;
    else if (*e == 'T')
        val *= 1024L*1024L*1024L*1024L;

    return val;
}

static int runregex(regex_t *compiled_regex, int max_match, char *str_input, char str_array[][S_MAXSTR])
{
    int i, matches, m;
    regmatch_t group[S_MAXREGEXMATCH];

    if (max_match > S_MAXREGEXMATCH)
        return -2;

    m = 0;
    matches = regexec(compiled_regex, str_input, max_match, group, 0);
    //          printf ("mymatch %d\n", mymatch);
    if (matches == 0)
    {
       for (i=0; i < max_match; i++)
       {
           if (group[i].rm_so == -1)
               break;
           int start = (int) group[i].rm_so;
           int finish = (int) group[i].rm_eo;
           int numchars = finish-start;
           strncpy(str_array[i], str_input + group[i].rm_so, numchars);
           str_array[i][numchars] = '\0';

//           printf("%d %d %s\n", i, numchars,  str_array[i]);
//           printf("start : %d finish %d\n", start, finish);
           m++;
        }
     }
     return m;
}

static int get_partition_option(shmem_partition_t *sp, char *rhs, char *e)
{
	char *s1, *s2, *saveptr2;
	
	s1 = strtok_r(rhs, "=", &saveptr2);
	s2 = strtok_r(NULL, "=", &saveptr2);
	
	if (strncmp(s1,"size",5) == 0)
	{
		sp->size = init_atol_scaled(s2);
	} else if (strncmp(s1,"kind", 5) == 0)
	{
		if(strncmp(s2,"D",1) == 0)
		{
			sp->kind = KIND_DEFAULT;
		} else if(strncmp(s2,"F",1) == 0)
		{
			sp->kind = KIND_FASTMEM;
		} else {
			fprintf(stderr,"ERROR: incorrect kind value for environment variable: %s\n", e);
			return -1;
		}
	} else if (strncmp(s1,"policy", 7) == 0)
	{
		if(strncmp(s2,"M",1) == 0)
		{
			sp->policy = POLICY_DEFAULT;
		} else if(strncmp(s2,"I",1) == 0)
		{
			sp->policy = POLICY_INTERLEAVED;
		}  else if(strncmp(s2,"P",1) == 0)
		{
			sp->policy = POLICY_POLICY1;
		} else {
			fprintf(stderr,"ERROR: incorrect policy value for environment variable: %s\n", e);
			return -1;
		}
	} else if (strncmp(s1,"pgsize", 7) == 0)
	{
		sp->pgsize = init_atol_scaled(s2);
	} else {
		fprintf(stderr,"ERROR: Unrecognized symmetric partition option for environment variable: %s\n", e);
		return -1;
	}
	return 0;
}

/* Sort partition array in ascending order. */
static void sort_partitions(shmem_partition_t A[], int n )
{
	int i, j;
	shmem_partition_t tmp_part;
	
	for (i=0; i < (n-1); i++)
	{
		for (j=0; j < (n-i-1); j++)
		{
			if (A[j].id > A[j+1].id)
			{
				memcpy(&tmp_part, &A[j], sizeof(shmem_partition_t));
				memcpy(&A[j], &A[j+1], sizeof(shmem_partition_t));
				memcpy(&A[j+1], &tmp_part, sizeof(shmem_partition_t));
			}
		}
	}
}

int shmem_internal_parse_partition_env(void)
{
    char **env;
    int i,j, num, id, idcount, rc;
    char rhs[MAXSTRING];
    char *str1, *p, *saveptr1;
    size_t slen;
    
    char sym_partition_rexpr[] = "^(SHMEM|SMA)_SYMMETRIC_PARTITION([0-9]+)=(.*)$";
    char sym_size_rexpr[] = "^(SHMEM|SMA)_SYMMETRIC_SIZE=(.*)$";
    regex_t sym_partition_recomp;
    regex_t sym_size_recomp;
    char sym_partition_tmpstr[4][MAXSTRING];
    char sym_size_tmpstr[3][MAXSTRING];
    int sym_partition_match, sym_size_match;
    int sym_size_set = 0;
    long heap_use_malloc = 0;
    
    unsigned long default_heap_size = 512 * 1024 * 1024;
    unsigned long default_page_size = 4 * 1024;
    
    rc = 0;
    /* Huge page envs*/
    /* huge page support only on Linux for now, default is to use 2MB large pages */
#ifdef __linux__
    heap_use_malloc = shmem_util_getenv_long("SYMMETRIC_HEAP_USE_MALLOC", 0, 0);
    if (heap_use_malloc == 0) {
        shmem_internal_heap_use_huge_pages=
             (shmem_util_getenv_str("SYMMETRIC_HEAP_USE_HUGE_PAGES") != NULL) ? 1 : 0;
        shmem_internal_heap_huge_page_size = shmem_util_getenv_long("SYMMETRIC_HEAP_PAGE_SIZE",
                                                                    1,
                                                                    2 * 1024 * 1024);
    }
#endif

	/* 
	 * group	description
	 * 1		SHMEM || SMA
	 * 2		Partition id
	 * 3		Right hand side
	 */
    regcomp(&sym_partition_recomp, sym_partition_rexpr, REG_EXTENDED);
    
	/* 
	 * group	description
	 * 1		SHMEM || SMA
	 * 2		Right hand side
	 */
    regcomp(&sym_size_recomp, sym_size_rexpr, REG_EXTENDED);
    
    shmem_internal_defined_partitions = 0;
    env = environ;
    for (i=1; *env != NULL; )
    {
        sym_partition_match = runregex(&sym_partition_recomp, 4, *env, sym_partition_tmpstr);
        sym_size_match = runregex(&sym_size_recomp, 3, *env, sym_size_tmpstr);
        
        if (sym_partition_match != 0) /* match SYMMETRIC_PARTITION */
        {
        	if (i > SHM_INTERNAL_MAX_PARTITIONS-1)
        	{
        		fprintf(stderr,"ERROR: Max partitions reached\n");
        		rc |= 1;
        		break;
        	}
        	
        	num = atoi(sym_partition_tmpstr[2]);
        	if (num >= SHM_INTERNAL_MAX_PARTITION_ID || num < 1)
        	{
        		fprintf(stderr,"ERROR: Partition ID (%d) is not in range of 1-127\n", num);
        		rc |= 1;
        	}
        	symheap_partition[i].id = num;
        	
        	/* Initialize defaults */
        	symheap_partition[i].pgsize = 4 * 1024;
        	symheap_partition[i].kind = KIND_DEFAULT;
        	symheap_partition[i].policy = POLICY_DEFAULT;
        	
        	slen = strnlen(sym_partition_tmpstr[3],MAXSTRING);
        	strncpy(rhs,sym_partition_tmpstr[3],slen);
        	
        	/* Loop over tokens delimeted by ':' */
        	p = strtok_r(rhs,":",&saveptr1);
        	while (p != NULL)
        	{
        		rc |= get_partition_option(&symheap_partition[i], p, *env);
        		p = strtok_r(NULL,":",&saveptr1);
        	}
        	if (shmem_internal_debug > 0)
        	{
                fprintf(stdout,"Debug: shmem_internal_parse_partition_env \n");
                fprintf(stdout,"Debug: SHMEM_SYMMETRIC_PARTIION = %s\n", *env);
                shmem_partition_print_info(&symheap_partition[i]);       		
        	}
        	shmem_internal_defined_partitions = i;
        	i++;	
#if 0
            printf("mymatch %d\n", mymatch);
            for (j=0; j< mymatch; j++)
            {
                printf("%d : %s\n", j, sym_partition_tmpstr[j]);
            }
            printf("\n");
#endif
        } else if (sym_size_match != 0)   /* SYMMETRIC_SIZE */
        {
        	sym_size_set = 1;
        	default_heap_size = init_atol_scaled(sym_size_tmpstr[2]);
        }
        env++;
        
    }
    
    /* Default partition is partition 1*/
    if (shmem_internal_defined_partitions == 0)
    {
    	symheap_partition[1].id = 1;
    	symheap_partition[1].size = default_heap_size;
    	symheap_partition[1].pgsize = default_page_size;
    	symheap_partition[1].kind = KIND_DEFAULT;
    	symheap_partition[1].policy = POLICY_DEFAULT;
    	
    	if (shmem_internal_heap_use_huge_pages != 0)
    		symheap_partition[1].pgsize = shmem_internal_heap_huge_page_size;
    	
    	shmem_internal_defined_partitions = 1;
    } else if (sym_size_set != 0)  
    {
    	fprintf(stderr,"ERROR: Cannot set both SYMMETRIC_SIZE and SYMMETRIC_PARTITIONS\n");
    	rc |= 1;
    }
    
    sort_partitions(&symheap_partition[1], shmem_internal_defined_partitions);
    
    if (symheap_partition[1].id != 1) /* There must be a partition id 1 */
    {
    	fprintf(stderr,"ERROR: Partition #1 must be defined.\n");
    	rc |= 1;
    }
    
    /* Check for multiple partitions defined with same  id  */
    for (i=1; i<shmem_internal_defined_partitions+1; i++)
    {
    	id = symheap_partition[i].id;
    	idcount = 0;
    	for (j=i+1; j <shmem_internal_defined_partitions+1; j++)
    	{
    		if (id == symheap_partition[j].id) 
    			idcount++;
    	}
    	if (idcount > 0)
    	{
    		fprintf(stderr,"ERROR: Multiple partitions defined for partition id %d.\n", id);
    		rc |= 1;
    		break;
    	}
    }
    
   	if (shmem_internal_debug > 0)
   	{
   		fprintf(stdout,"Debug: shmem_internal_defined_partitions : %d\n", shmem_internal_defined_partitions);
   		for (i=1;i<shmem_internal_defined_partitions+1; i++)
   		{
   			fprintf(stdout,"Debug: Partition # : %d\n", i);
   			shmem_partition_print_info(&symheap_partition[i]);   
   		}
   	}
    
    regfree(&sym_partition_recomp);
    regfree(&sym_size_recomp);
    
    return rc;
}
#endif /* ENABLE_HETEROGENEOUS_MEM */
