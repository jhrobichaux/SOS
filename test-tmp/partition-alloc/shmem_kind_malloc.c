/* :vim:sw=4:ts=4: */
/*
 *  usage: shmalloc [-p] [nWords] [loops] [incWords-per-loop]
 *    where: -p == power-of-two allocation bump per loop
 *      [nWords] # of longs to shmem_malloc()\n"
 *      [loops(1)]  # of loops\n"
 *      [incWords(2)] nWords += incWords per loop\n");
 * Loop:
 *  PE* shmem_malloc(nWords)
 *   set *DataType = 1
 *  PE* shmem_malloc(nWords)
 *   set *DataType = 2
 *  PE* shmem_malloc(nWords)
 *   set *DataType = 3
 *
 *  for(1...3) allocated ranges
 *    verify
 *    shmem_free()
 * end-loop
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include <shmem.h>
#include <shmemx.h>

#define DFLT_NWORDS 32
#define DFLT_INCR 1025
#define DFLT_LOOPS 50

#define DataType long


static DataType **source;
static DataType **target;
static DataType **result;

static int source_sz;
static int target_sz;
static int result_sz;

static char *pgm;

void usage (void);
int getSize (char *);

void
usage (void)
{
    if (shmem_my_pe() == 0 ) {
        fprintf (stderr,
            "Usage: %s [-p]  [nWords(%d)] [loops(%d)] [incWords(%d)]\n",
            pgm, DFLT_NWORDS, DFLT_LOOPS, DFLT_INCR);
        fprintf (stderr,
            "  -p  == (2**0 ... 2**22) shmem_malloc(), other args ignored\n"
            "  -v == Verbose output\n"
            "  [nWords] # of longs to shmem_malloc()\n"
            "  [loops]  # of loops\n"
            "  [incWords] nWords += incWords per loop\n");
    }
    exit (1);
}

int
getSize (char *str)
{
    int size;
    char mod[32];

    switch (sscanf (str, "%d%1[mMkK]", &size, mod))
    {
    case 1:
        return (size);

    case 2:
        switch (*mod)
        {
        case 'm':
        case 'M':
            return (size << 20);

        case 'k':
        case 'K':
            return (size << 10);

        default:
            return (size);
        }

    default:
        return (-1);
    }
}

int
main(int argc, char **argv)
{
    int me, nProcs, c, l;
    int nWords, loops, incWords;
    int Verbose = 0, power2 = 0, modulo = 5;
    int npartitions = 7;
    int p, partid;
    unsigned int pmask;
    DataType *dp;

    pgm = strrchr(argv[0],'/');
    if ( pgm )
        pgm++;
    else
        pgm = argv[0];

    shmem_init();
    me = shmem_my_pe();
    nProcs = shmem_n_pes();

    while ((c = getopt (argc, argv, "hpv")) != -1)
        switch (c)
        {
        case 'p':
            power2++;
            break;
        case 'v':
            Verbose++;
            break;
        case 'h':
        default:
            usage();
            break;
        }

    if (optind == argc)
        nWords = DFLT_NWORDS;
    else if ((nWords = getSize (argv[optind++])) <= 0)
        usage ();

    if (optind == argc)
            loops = DFLT_LOOPS;
    else if ((loops = getSize (argv[optind++])) < 0)
        usage ();

    if (optind == argc)
        incWords = DFLT_INCR;
    else if ((incWords = getSize (argv[optind++])) < 0)
        usage ();

    if (power2) {
        nWords = 1;
        modulo = 1;
        loops = 21;
    }


    if (Verbose && me == 0) {
        if (power2) {
            printf("%s: nWords(1) << 1 per loop.\n", pgm);
        }
        else
            printf("%s: nWords(%d) loops(%d) nWords-incr-per-loop(%d)\n",
                pgm, nWords, loops, incWords);
    }

    source = (DataType **) malloc(npartitions  * sizeof(DataType *));
    result = (DataType **) malloc(npartitions  * sizeof(DataType *));
    target = (DataType **) malloc(npartitions  * sizeof(DataType *));

    for(l=0; l < loops; l++) {

    	result_sz = (nProcs-1) * (nWords * sizeof(DataType));
    	for(p=0; p < npartitions; p++)
    	{
    		partid = p+1;  pmask = partid << 4;

    		result[p] = (DataType *)shmemx_kind_malloc(result_sz, partid);
    		if (! result[p])
    		{
    			perror ("Failed result memory allocation");
    			exit (1);
    		}
    		for(dp=result[p]; dp < &result[p][(result_sz/sizeof(DataType))];)
    			*dp++ = (pmask | 1);
    	}

    	for(p=0; p < npartitions; p++)
    	{
    		partid = p+1;  pmask = partid << 4;
    		target_sz = nWords * sizeof(DataType);
    		if (!(target[p] = (DataType *)shmemx_kind_malloc(target_sz, partid)))
    		{
    			perror ("Failed target memory allocation");
    			exit (1);
    		}
    		for(dp=target[p]; dp < &target[p][(target_sz / sizeof(DataType))];)
    			*dp++ = (pmask | 2);
    	}

    	for(p=0; p < npartitions; p++)
    	{
    		partid = p+1;  pmask = partid << 4;
    		source_sz = 2 * nWords * sizeof(DataType);
    		if (!(source[p] = (DataType *)shmemx_kind_malloc(source_sz,partid)))
    		{
    			perror ("Failed source memory allocation");
    			exit (1);
    		}
    		for(dp=source[p]; dp < &source[p][(source_sz / sizeof(DataType))];)
    			*dp++ = (pmask | 3);
    	}


#if 0
        printf("[%d] source %p target %p result %p\n",
            me, (void*)source,(void*)target,(void*)result);
        shmem_barrier_all(); 
#endif

        shmem_barrier_all(); /* sync sender and receiver */

        for(p=0; p < npartitions; p++)
        {
        	partid = p+1;  pmask = partid << 4;
        	for(dp=source[p]; dp < &source[p][(source_sz / sizeof(DataType))]; dp++)
        		if (*dp != (pmask | 3)) {
        			printf("source not consistent @ 3?\n");
        			break;
        		}
        	shmem_free(source[p]);
        }

        for(p=0; p < npartitions; p++)
        {
        	partid = p+1;  pmask = partid << 4;
        	for(dp=target[p]; dp < &target[p][(target_sz / sizeof(DataType))]; dp++)
        		if (*dp != (pmask | 2) ) {
        			printf("target not consistent @ 2?\n");
        			break;
        		}
        	shmem_free(target[p]);
        }

        for(p=0; p < npartitions; p++)
        {
        	partid = p+1;  pmask = partid << 4;
        	for(dp=result[p]; dp < &result[p][(result_sz / sizeof(DataType))]; dp++)
        		if (*dp != (pmask | 1) ) {
        			printf("result not consistent @ 1?\n");
        			break;
        		}
        	shmem_free(result[p]);
        }

        if (loops > 1) {
            if (Verbose && me == 0) {
                if (l == 0 || (l % modulo == 0))
                    printf("End loop %3d nWords(%d)\n",(l+1),nWords);
            }
            if (power2)
                nWords <<= 1;
            else
                nWords += incWords; // watch for double inc.
        }
    }

    free(source);
    free(result);
    free(target);
    shmem_finalize();

    return 0;
}
