/* kcollins - RandomAccess core_single_cpu kernel from HPCC */
/*            with C driver for standalone testing */

/*
 * This code has been contributed by the DARPA HPCS program.  Contact
 * David Koester <dkoester@mitre.org> or Bob Lucas <rflucas@isi.edu>
 * if you have questions.
 *
 * GUPS (Giga UPdates per Second) is a measurement that profiles the memory
 * architecture of a system and is a measure of performance similar to MFLOPS.
 * The HPCS HPCchallenge RandomAccess benchmark is intended to exercise the
 * GUPS capability of a system, much like the LINPACK benchmark is intended to
 * exercise the MFLOPS capability of a computer.  In each case, we would
 * expect these benchmarks to achieve close to the "peak" capability of the
 * memory system. The extent of the similarities between RandomAccess and
 * LINPACK are limited to both benchmarks attempting to calculate a peak system
 * capability.
 *
 * GUPS is calculated by identifying the number of memory locations that can be
 * randomly updated in one second, divided by 1 billion (1e9). The term "randomly"
 * means that there is little relationship between one address to be updated and
 * the next, except that they occur in the space of one half the total system
 * memory.  An update is a read-modify-write operation on a table of 64-bit words.
 * An address is generated, the value at that address read from memory, modified
 * by an integer operation (add, and, or, xor) with a literal value, and that
 * new value is written back to memory.
 *
 * We are interested in knowing the GUPS performance of both entire systems and
 * system subcomponents --- e.g., the GUPS rating of a distributed memory
 * multiprocessor the GUPS rating of an SMP node, and the GUPS rating of a
 * single processor.  While there is typically a scaling of FLOPS with processor
 * count, a similar phenomenon may not always occur for GUPS.
 *
 * For additional information on the GUPS metric, the HPCchallenge RandomAccess
 * Benchmark,and the rules to run RandomAccess or modify it to optimize
 * performance -- see http://icl.cs.utk.edu/hpcc/
 *
 */

/*
 * This file contains the computational core of the single cpu version
 * of GUPS.  The inner loop should easily be vectorized by compilers
 * with such support.
 *
 * This core is used by both the single_cpu and star_single_cpu tests.
 */

/* Number of updates to table (suggested: 4x number of table entries) */

#include <sys/types.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <omp.h>
#include <string.h>
#include "hif.h"

#define POLY 0x0000000000000007UL
#define PERIOD 1317624576693539401L

#define NUPDATE (4 * TableSize)
#define NUM_THREADS 32

uint64_t HPCC_starts(int64_t);

static void
RandomAccessUpdate(uint64_t TableSize, uint64_t *Table) {
    uint64_t i;
    uint64_t *ran;
    int j;

    uint32_t unitCnt = __htc_get_unit_count();
    uint64_t ranSize = unitCnt * NUM_THREADS;
    ran = (uint64_t *)malloc(ranSize*sizeof(uint64_t));
    if (! ran) {
        printf( "Failed to allocate memory for the ran array (%ld).\n", 
                ranSize);
        exit(1);
    }

#pragma omp parallel for
    for (j=0; j<ranSize; j++) {
        ran[j] = HPCC_starts ((NUPDATE/ranSize) * j);
    }

    fprintf(stderr,"ran array has been initialized\n"); fflush(stderr);

    uint32_t updates_per_unit = NUPDATE/unitCnt;
    printf("will use %d units and %d threads per unit, %d total threads\n",unitCnt,NUM_THREADS,unitCnt*NUM_THREADS);
    printf("NUPDATE is %ld updates_per_unit is %ld\n", NUPDATE, updates_per_unit);

#pragma omp parallel num_threads(unitCnt)
    {
        int unit = omp_get_thread_num();
        uint64_t *unitran = ran + (unit * NUM_THREADS);

#pragma omp target device(unit)
        {
#pragma omp parallel num_threads(NUM_THREADS) 
            { 
                uint64_t pran = unitran[omp_get_thread_num()];
#pragma omp for schedule(static, 1) nowait 
                for (i=0; i< updates_per_unit; i++) {
                    pran = (pran << 1) ^ ((int64_t) pran < 0 ? POLY : 0);
                    Table[pran & (TableSize-1)] ^= pran;
                }
            }
        }
    }
}
 

/* Utility routine to start random number generator at Nth step */
uint64_t HPCC_starts(int64_t n)
{
  int i, j;
  uint64_t m2[64];
  uint64_t temp, ran;

  while (n < 0) n += PERIOD;
  while (n > PERIOD) n -= PERIOD;
  if (n == 0) return 0x1;

  temp = 0x1;
  for (i=0; i<64; i++) {
    m2[i] = temp;
    temp = (temp << 1) ^ ((int64_t) temp < 0 ? POLY : 0);
    temp = (temp << 1) ^ ((int64_t) temp < 0 ? POLY : 0);
  }

  for (i=62; i>=0; i--)
    if ((n >> i) & 1)
      break;

  ran = 0x2;
  while (i > 0) {
    temp = 0;
    for (j=0; j<64; j++)
      if ((ran >> j) & 1)
        temp ^= m2[j];
    ran = temp;
    i -= 1;
    if ((n >> i) & 1)
      ran = (ran << 1) ^ ((int64_t) ran < 0 ? POLY : 0);
  }

  return ran;
}

/*kcollins timers*/
#include <sys/time.h>
#include <sys/resource.h>
double RTSEC() {
  struct timeval tp;
  struct timezone tzp;

  gettimeofday(&tp,&tzp);
  return ( (double) tp.tv_sec + (double) tp.tv_usec * 1.e-6 );
}

double CPUSEC() {
  struct rusage ru;

  getrusage(RUSAGE_SELF,&ru);
  return( (double) ru.ru_utime.tv_sec + (double) ru.ru_utime.tv_usec * 1.e-6 );
}

int
main (int argc, char **argv) {

  uint64_t i;
  uint64_t temp;
  double cputime;               /* CPU time to update table */
  double realtime;              /* Real time to update table */
  double GUPs;
  uint64_t *Table;
  uint64_t *cp_Table;
  uint64_t TableSize;

  pers_attach();

  int power = 24;
  if(argc > 1) {
    power = atoi(argv[1]);
  }
  TableSize = 1<<power;

  Table = (uint64_t *)calloc( TableSize, sizeof(uint64_t) );
  if (! Table) {
    printf( "Failed to allocate memory for the update table (%ld).\n", TableSize);
    return 1;
  }
  cp_Table = ((uint64_t *)(pers_cp_malloc(TableSize*sizeof(uint64_t ))));
  if (!cp_Table) {
    printf("Failed to allocate memory for the cp update table (%ld).\n",TableSize);
    return 1;
  }

  /* Print parameters for run */
  printf( "Main table size   = %ld words\n", TableSize);
  printf( "Number of updates = %ld\n", NUPDATE);

  /* Initialize main table */
  for (i=0; i<TableSize; i++) Table[i] = i;

  pers_cp_memcpy(cp_Table, Table, TableSize*sizeof(uint64_t ));

  /* Begin timing here */
  cputime = -CPUSEC();
  realtime = -RTSEC();

  //  RandomAccessUpdate(TableSize,Table);
  RandomAccessUpdate(TableSize,cp_Table);

  /* End timed section */
  cputime += CPUSEC();
  realtime += RTSEC();

  pers_cp_memcpy(Table, cp_Table, TableSize*sizeof(uint64_t ));

  /* make sure no division by zero */
  GUPs = (realtime > 0.0 ? 1.0 / realtime : -1.0);
  GUPs *= 1e-9*NUPDATE;
  /* Print timing results */
  printf( "CPU time used  = %.6f seconds\n", cputime);
  printf( "Real time used = %.6f seconds\n", realtime);
  printf( "%.9f Billion(10^9) Updates    per second [GUP/s]\n", GUPs );

  /* Verification of results (in serial or "safe" mode; optional) */
  temp = 0x1;
  for (i=0; i<NUPDATE; i++) {
    temp = (temp << 1) ^ (((int64_t) temp < 0) ? POLY : 0);
    Table[temp & (TableSize-1)] ^= temp;
  }

  temp = 0;
  for (i=0; i<TableSize; i++)
    if (Table[i] != i)
      temp++;

  printf( "Found %ld errors in %ld locations (%s).\n",
           temp, TableSize, (temp <= 0.01*TableSize) ? "passed" : "failed");

  free( Table );
  pers_detach();

  return 0;
}
