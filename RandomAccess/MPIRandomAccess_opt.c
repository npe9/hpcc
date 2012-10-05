/* -*- mode: C; tab-width: 2; indent-tabs-mode: nil; -*- */

/*
 * This code has been contributed by the DARPA HPCS program.  Contact
 * David Koester <dkoester@mitre.org> or Bob Lucas <rflucas@isi.edu>
 * if you have questions.
 *
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
 * Select the memory size to be the power of two such that 2^n <= 1/2 of the
 * total memory.  Each CPU operates on its own address stream, and the single
 * table may be distributed among nodes. The distribution of memory to nodes
 * is left to the implementer.  A uniform data distribution may help balance
 * the workload, while non-uniform data distributions may simplify the
 * calculations that identify processor location by eliminating the requirement
 * for integer divides. A small (less than 1%) percentage of missed updates
 * are permitted.
 *
 * When implementing a benchmark that measures GUPS on a distributed memory
 * multiprocessor system, it may be required to define constraints as to how
 * far in the random address stream each node is permitted to "look ahead".
 * Likewise, it may be required to define a constraint as to the number of
 * update messages that can be stored before processing to permit multi-level
 * parallelism for those systems that support such a paradigm.  The limits on
 * "look ahead" and "stored updates" are being implemented to assure that the
 * benchmark meets the intent to profile memory architecture and not induce
 * significant artificial data locality. For the purpose of measuring GUPS,
 * we will stipulate that each process is permitted to look ahead no more than
 * 1024 random address stream samples with the same number of update messages
 * stored before processing.
 *
 * The supplied MPI-1 code generates the input stream {A} on all processors
 * and the global table has been distributed as uniformly as possible to
 * balance the workload and minimize any Amdahl fraction.  This code does not
 * exploit "look-ahead".  Addresses are sent to the appropriate processor
 * where the table entry resides as soon as each address is calculated.
 * Updates are performed as addresses are received.  Each message is limited
 * to a single 64 bit long integer containing element ai from {A}.
 * Local offsets for T[ ] are extracted by the destination processor.
 *
 * If the number of processors is equal to a power of two, then the global
 * table can be distributed equally over the processors.  In addition, the
 * processor number can be determined from that portion of the input stream
 * that identifies the address into the global table by masking off log2(p)
 * bits in the address.
 *
 * If the number of processors is not equal to a power of two, then the global
 * table cannot be equally distributed between processors.  In the MPI-1
 * implementation provided, there has been an attempt to minimize the differences
 * in workloads and the largest difference in elements of T[ ] is one.  The
 * number of values in the input stream generated by each processor will be
 * related to the number of global table entries on each processor.
 *
 * The MPI-1 version of RandomAccess treats the potential instance where the
 * number of processors is a power of two as a special case, because of the
 * significant simplifications possible because processor location and local
 * offset can be determined by applying masks to the input stream values.
 * The non power of two case uses an integer division to determine the processor
 * location.  The integer division will be more costly in terms of machine
 * cycles to perform than the bit masking operations
 *
 * For additional information on the GUPS metric, the HPCchallenge RandomAccess
 * Benchmark,and the rules to run RandomAccess or modify it to optimize
 * performance -- see http://icl.cs.utk.edu/hpcc/
 *
 */

/* Jan 2005
 *
 * This code has been modified to allow local bucket sorting of updates.
 * The total maximum number of updates in the local buckets of a process
 * is currently defined in "RandomAccess.h" as MAX_TOTAL_PENDING_UPDATES.
 * When the total maximum number of updates is reached, the process selects
 * the bucket (or destination process) with the largest number of
 * updates and sends out all the updates in that bucket. See buckets.c
 * for details about the buckets' implementation.
 *
 * This code also supports posting multiple MPI receive descriptors (based
 * on a contribution by David Addison).
 *
 * In addition, this implementation provides an option for limiting
 * the execution time of the benchmark to a specified time bound
 * (see time_bound.c). The time bound is currently defined in
 * time_bound.h, but it should be a benchmark parameter. By default
 * the benchmark will execute the recommended number of updates,
 * that is, four times the global table size.
 */

#include <hpcc.h>

#include "RandomAccess.h"
#include "buckets.h"
#include "time_bound.h"

#define CHUNK       MAX_TOTAL_PENDING_UPDATES
#define CHUNKBIG    (32*CHUNK)
#define RCHUNK      (16384)
#define PITER       8
#define MAXLOGPROCS 20

#ifdef RA_SANDIA_OPT2
void
AnyNodesMPIRandomAccessUpdate(HPCC_RandomAccess_tabparams_t tparams) {
  int i, ipartner,npartition,proclo,nlower,nupper,procmid;
  int ndata,nkeep,nsend,nrecv,nfrac;
  s64Int iterate, niterate;
  u64Int ran,datum,nglobalm1,indexmid, index;
  u64Int *data,*send, *offsets;
  MPI_Status status;

  /* setup: should not really be part of this timed routine
     NOTE: niterate must be computed from global TableSize * 4
           not from ProcNumUpdates since that can be different on each proc
           round niterate up by 1 to do slightly more than required updates */

  data = (u64Int *) malloc(CHUNKBIG*sizeof(u64Int));
  send = (u64Int *) malloc(CHUNKBIG*sizeof(u64Int));

  ran = HPCC_starts(4*tparams.GlobalStartMyProc);

  offsets = (u64Int *) malloc((tparams.NumProcs+1)*sizeof(u64Int));
  MPI_Allgather(&tparams.GlobalStartMyProc,1,tparams.dtype64,offsets,1,tparams.dtype64,
                MPI_COMM_WORLD);
  offsets[tparams.NumProcs] = tparams.TableSize;

  niterate = 4 * tparams.TableSize / tparams.NumProcs / CHUNK + 1;
  nglobalm1 = tparams.TableSize - 1;

  /* actual update loop: this is only section that should be timed */

  for (iterate = 0; iterate < niterate; iterate++) {
    for (i = 0; i < CHUNK; i++) {
      ran = (ran << 1) ^ ((s64Int) ran < ZERO64B ? POLY : ZERO64B);
      data[i] = ran;
    }
    ndata = CHUNK;

    npartition = tparams.NumProcs;
    proclo = 0;
    while (npartition > 1) {
      nlower = npartition/2;
      nupper = npartition - nlower;
      procmid = proclo + nlower;
      indexmid = offsets[procmid];

      nkeep = nsend = 0;
      if (tparams.MyProc < procmid) {
        for (i = 0; i < ndata; i++) {
          if ((data[i] & nglobalm1) >= indexmid) send[nsend++] = data[i];
          else data[nkeep++] = data[i];
        }
      } else {
        for (i = 0; i < ndata; i++) {
          if ((data[i] & nglobalm1) < indexmid) send[nsend++] = data[i];
          else data[nkeep++] = data[i];
        }
      }

      if (nlower == nupper) {
        if (tparams.MyProc < procmid) ipartner = tparams.MyProc + nlower;
        else ipartner = tparams.MyProc - nlower;
        MPI_Sendrecv(send,nsend,tparams.dtype64,ipartner,0,&data[nkeep],
                     CHUNKBIG,tparams.dtype64,ipartner,0,MPI_COMM_WORLD,&status);
        MPI_Get_count(&status,tparams.dtype64,&nrecv);
        ndata = nkeep + nrecv;
      } else {
        if (tparams.MyProc < procmid) {
          nfrac = (nlower - (tparams.MyProc-proclo)) * nsend / nupper;
          ipartner = tparams.MyProc + nlower;
          MPI_Sendrecv(send,nfrac,tparams.dtype64,ipartner,0,&data[nkeep],
                       CHUNKBIG,tparams.dtype64,ipartner,0,MPI_COMM_WORLD,&status);
          MPI_Get_count(&status,tparams.dtype64,&nrecv);
          nkeep += nrecv;
          MPI_Sendrecv(&send[nfrac],nsend-nfrac,tparams.dtype64,ipartner+1,0,
                       &data[nkeep],CHUNKBIG,tparams.dtype64,
                       ipartner+1,0,MPI_COMM_WORLD,&status);
          MPI_Get_count(&status,tparams.dtype64,&nrecv);
          ndata = nkeep + nrecv;
        } else if (tparams.MyProc > procmid && tparams.MyProc < procmid+nlower) {
          nfrac = (tparams.MyProc - procmid) * nsend / nlower;
          ipartner = tparams.MyProc - nlower;
          MPI_Sendrecv(&send[nfrac],nsend-nfrac,tparams.dtype64,ipartner,0,
                       &data[nkeep],CHUNKBIG,tparams.dtype64,
                       ipartner,0,MPI_COMM_WORLD,&status);
          MPI_Get_count(&status,tparams.dtype64,&nrecv);
          nkeep += nrecv;
          MPI_Sendrecv(send,nfrac,tparams.dtype64,ipartner-1,0,&data[nkeep],
                       CHUNKBIG,tparams.dtype64,ipartner-1,0,MPI_COMM_WORLD,&status);
          MPI_Get_count(&status,tparams.dtype64,&nrecv);
          ndata = nkeep + nrecv;
        } else {
          if (tparams.MyProc == procmid) ipartner = tparams.MyProc - nlower;
          else ipartner = tparams.MyProc - nupper;
          MPI_Sendrecv(send,nsend,tparams.dtype64,ipartner,0,&data[nkeep],
                       CHUNKBIG,tparams.dtype64,ipartner,0,MPI_COMM_WORLD,&status);
          MPI_Get_count(&status,tparams.dtype64,&nrecv);
          ndata = nkeep + nrecv;
        }
      }

      if (tparams.MyProc < procmid) npartition = nlower;
      else {
        proclo = procmid;
        npartition = nupper;
      }
    }

    for (i = 0; i < ndata; i++) {
      datum = data[i];
      index = (datum & nglobalm1) - tparams.GlobalStartMyProc;
      HPCC_Table[index] ^= datum;
    }
  }

  /* clean up: should not really be part of this timed routine */

  free(data);
  free(send);
  free(offsets);
}

/* This sort is manually unrolled to make sure the compiler can see
 * the parallelism -KDU
 */

static
void sort_data(u64Int *source, u64Int *nomatch, u64Int *match, int number,
               int *nnomatch, int *nmatch, int mask_shift)
{
  int i,dindex,myselect[8],counts[2];
  int div_num = number / 8;
  int loop_total = div_num * 8;
  u64Int procmask = ((u64Int) 1) << mask_shift;
  u64Int *buffers[2];

  buffers[0] = nomatch;
  counts[0] = *nnomatch;
  buffers[1] = match;
  counts[1] = *nmatch;

  for (i = 0; i < div_num; i++) {
    dindex = i*8;
    myselect[0] = (source[dindex] & procmask) >> mask_shift;
    myselect[1] = (source[dindex+1] & procmask) >> mask_shift;
    myselect[2] = (source[dindex+2] & procmask) >> mask_shift;
    myselect[3] = (source[dindex+3] & procmask) >> mask_shift;
    myselect[4] = (source[dindex+4] & procmask) >> mask_shift;
    myselect[5] = (source[dindex+5] & procmask) >> mask_shift;
    myselect[6] = (source[dindex+6] & procmask) >> mask_shift;
    myselect[7] = (source[dindex+7] & procmask) >> mask_shift;
    buffers[myselect[0]][counts[myselect[0]]++] = source[dindex];
    buffers[myselect[1]][counts[myselect[1]]++] = source[dindex+1];
    buffers[myselect[2]][counts[myselect[2]]++] = source[dindex+2];
    buffers[myselect[3]][counts[myselect[3]]++] = source[dindex+3];
    buffers[myselect[4]][counts[myselect[4]]++] = source[dindex+4];
    buffers[myselect[5]][counts[myselect[5]]++] = source[dindex+5];
    buffers[myselect[6]][counts[myselect[6]]++] = source[dindex+6];
    buffers[myselect[7]][counts[myselect[7]]++] = source[dindex+7];
  }

  for (i = loop_total; i < number; i++) {
    u64Int mydata = source[i];
    if (mydata & procmask) buffers[1][counts[1]++] = mydata;
    else buffers[0][counts[0]++] = mydata;
  }

  *nnomatch = counts[0];
  *nmatch = counts[1];
}

/* Manual unrolling is a significant win if -Msafeptr is used -KDU */

static
void update_table(u64Int *data, u64Int *table, int number, u64Int nlocalm1) {
  int i,dindex;
  int div_num = number / 8;
  int loop_total = div_num * 8;
  u64Int index,index0,index1,index2,index3,index4,index5,index6,index7;
  u64Int ltable0,ltable1,ltable2,ltable3,ltable4,ltable5,ltable6,ltable7;

  for (i = 0; i < div_num; i++) {
    dindex = i*8;

    index0 = data[dindex  ] & nlocalm1;
    index1 = data[dindex+1] & nlocalm1;
    index2 = data[dindex+2] & nlocalm1;
    index3 = data[dindex+3] & nlocalm1;
    index4 = data[dindex+4] & nlocalm1;
    index5 = data[dindex+5] & nlocalm1;
    index6 = data[dindex+6] & nlocalm1;
    index7 = data[dindex+7] & nlocalm1;
    ltable0 = table[index0];
    ltable1 = table[index1];
    ltable2 = table[index2];
    ltable3 = table[index3];
    ltable4 = table[index4];
    ltable5 = table[index5];
    ltable6 = table[index6];
    ltable7 = table[index7];

    table[index0] = ltable0 ^ data[dindex];
    table[index1] = ltable1 ^ data[dindex+1];
    table[index2] = ltable2 ^ data[dindex+2];
    table[index3] = ltable3 ^ data[dindex+3];
    table[index4] = ltable4 ^ data[dindex+4];
    table[index5] = ltable5 ^ data[dindex+5];
    table[index6] = ltable6 ^ data[dindex+6];
    table[index7] = ltable7 ^ data[dindex+7];
  }

  for (i = loop_total; i < number; i++) {
    u64Int datum = data[i];
    index = datum & nlocalm1;
    table[index] ^= datum;
  }
}

void
Power2NodesMPIRandomAccessUpdate(HPCC_RandomAccess_tabparams_t tparams) {
  int i, j, k, logTableLocal, ipartner;
  int ndata, nkeep, nsend, nrecv, nkept;
  s64Int iterate, niterate, iter_mod;
  u64Int ran, procmask, nlocalm1;
  u64Int *data,*send,*send1,*send2;
  u64Int *recv[PITER][MAXLOGPROCS];
  MPI_Status status;
  MPI_Request request[PITER][MAXLOGPROCS];
  MPI_Request srequest;

  /* setup: should not really be part of this timed routine */

  data = (u64Int *) malloc(CHUNKBIG*sizeof(u64Int));
  send1 = (u64Int *) malloc(CHUNKBIG*sizeof(u64Int));
  send2 = (u64Int *) malloc(CHUNKBIG*sizeof(u64Int));
  send = send1;

  for (j = 0; j < PITER; j++)
    for (i = 0; i < tparams.logNumProcs; i++)
      recv[j][i] = (u64Int *) malloc(sizeof(u64Int)*RCHUNK);

  ran = HPCC_starts(4*tparams.GlobalStartMyProc);

  niterate = tparams.ProcNumUpdates / CHUNK;
  logTableLocal = tparams.logTableSize - tparams.logNumProcs;
  nlocalm1 = (u64Int)(tparams.LocalTableSize - 1);

  /* actual update loop: this is only section that should be timed */

  for (iterate = 0; iterate < niterate; iterate++) {
    iter_mod = iterate % PITER;
    for (i = 0; i < CHUNK; i++) {
      ran = (ran << 1) ^ ((s64Int) ran < ZERO64B ? POLY : ZERO64B);
      data[i] = ran;
    }
    nkept = CHUNK;
    nrecv = 0;

    if (iter_mod == 0)
      for (k = 0; k < PITER; k++)
        for (j = 0; j < tparams.logNumProcs; j++) {
          ipartner = (1 << j) ^ tparams.MyProc;
          MPI_Irecv(recv[k][j],RCHUNK,tparams.dtype64,ipartner,0,MPI_COMM_WORLD,
                    &request[k][j]);
        }

    for (j = 0; j < tparams.logNumProcs; j++) {
      nkeep = nsend = 0;
      send = (send == send1) ? send2 : send1;
      ipartner = (1 << j) ^ tparams.MyProc;
      procmask = ((u64Int) 1) << (logTableLocal + j);
      if (ipartner > tparams.MyProc) {
      	sort_data(data,data,send,nkept,&nkeep,&nsend,logTableLocal+j);
        if (j > 0) {
          MPI_Wait(&request[iter_mod][j-1],&status);
          MPI_Get_count(&status,tparams.dtype64,&nrecv);
      	  sort_data(recv[iter_mod][j-1],data,send,nrecv,&nkeep,
                    &nsend,logTableLocal+j);
        }
      } else {
        sort_data(data,send,data,nkept,&nsend,&nkeep,logTableLocal+j);
        if (j > 0) {
          MPI_Wait(&request[iter_mod][j-1],&status);
          MPI_Get_count(&status,tparams.dtype64,&nrecv);
          sort_data(recv[iter_mod][j-1],send,data,nrecv,&nsend,
                    &nkeep,logTableLocal+j);
        }
      }
      if (j > 0) MPI_Wait(&srequest,&status);
      MPI_Isend(send,nsend,tparams.dtype64,ipartner,0,MPI_COMM_WORLD,&srequest);
      if (j == (tparams.logNumProcs - 1)) update_table(data,HPCC_Table,nkeep,nlocalm1);
      nkept = nkeep;
    }

    if (tparams.logNumProcs == 0) update_table(data,HPCC_Table,nkept,nlocalm1);
    else {
      MPI_Wait(&request[iter_mod][j-1],&status);
      MPI_Get_count(&status,tparams.dtype64,&nrecv);
      update_table(recv[iter_mod][j-1],HPCC_Table,nrecv,nlocalm1);
      MPI_Wait(&srequest,&status);
    }

    ndata = nkept + nrecv;
  }

  /* clean up: should not really be part of this timed routine */

  for (j = 0; j < PITER; j++)
    for (i = 0; i < tparams.logNumProcs; i++) free(recv[j][i]);

  free(data);
  free(send1);
  free(send2);
}
#endif
