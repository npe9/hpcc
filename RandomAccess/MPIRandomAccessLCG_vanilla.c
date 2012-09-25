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
#include "verification.h"

#define CHUNK    MAX_TOTAL_PENDING_UPDATES
#define CHUNKBIG (32*CHUNK)

#ifdef RA_SANDIA_NOPT
void
HPCC_AnyNodesMPIRandomAccessUpdate_LCG(u64Int logTableSize,
                              u64Int TableSize,
                              s64Int LocalTableSize,
                              u64Int MinLocalTableSize,
                              u64Int GlobalStartMyProc,
                              u64Int Top,
                              int logNumProcs,
                              int NumProcs,
                              int Remainder,
                              int MyProc,
                              s64Int ProcNumUpdates,
                              MPI_Datatype INT64_DT,
                              MPI_Status *finish_statuses,
                              MPI_Request *finish_req) {
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

  ran = HPCC_starts_LCG(4*GlobalStartMyProc);

  offsets = (u64Int *) malloc((NumProcs+1)*sizeof(u64Int));
  MPI_Allgather(&GlobalStartMyProc,1,INT64_DT,offsets,1,INT64_DT,
                MPI_COMM_WORLD);
  offsets[NumProcs] = TableSize;

  niterate = 4 * TableSize / NumProcs / CHUNK + 1;
  nglobalm1 = 64 - logTableSize;

  /* actual update loop: this is only section that should be timed */

  for (iterate = 0; iterate < niterate; iterate++) {
    for (i = 0; i < CHUNK; i++) {
      ran = LCG_MUL64 * ran + LCG_ADD64;
      data[i] = ran;
    }
    ndata = CHUNK;

    npartition = NumProcs;
    proclo = 0;
    while (npartition > 1) {
      nlower = npartition/2;
      nupper = npartition - nlower;
      procmid = proclo + nlower;
      indexmid = offsets[procmid];

      nkeep = nsend = 0;
      if (MyProc < procmid) {
        for (i = 0; i < ndata; i++) {
          if ((data[i] >> nglobalm1) >= indexmid) send[nsend++] = data[i];
          else data[nkeep++] = data[i];
        }
      } else {
        for (i = 0; i < ndata; i++) {
          if ((data[i] >> nglobalm1) < indexmid) send[nsend++] = data[i];
          else data[nkeep++] = data[i];
        }
      }

      if (nlower == nupper) {
        if (MyProc < procmid) ipartner = MyProc + nlower;
        else ipartner = MyProc - nlower;
        MPI_Sendrecv(send,nsend,INT64_DT,ipartner,0,&data[nkeep],
                     CHUNKBIG,INT64_DT,ipartner,0,MPI_COMM_WORLD,&status);
        MPI_Get_count(&status,INT64_DT,&nrecv);
        ndata = nkeep + nrecv;
      } else {
        if (MyProc < procmid) {
          nfrac = (nlower - (MyProc-proclo)) * nsend / nupper;
          ipartner = MyProc + nlower;
          MPI_Sendrecv(send,nfrac,INT64_DT,ipartner,0,&data[nkeep],
                       CHUNKBIG,INT64_DT,ipartner,0,MPI_COMM_WORLD,&status);
          MPI_Get_count(&status,INT64_DT,&nrecv);
          nkeep += nrecv;
          MPI_Sendrecv(&send[nfrac],nsend-nfrac,INT64_DT,ipartner+1,0,
                       &data[nkeep],CHUNKBIG,INT64_DT,
                       ipartner+1,0,MPI_COMM_WORLD,&status);
          MPI_Get_count(&status,INT64_DT,&nrecv);
          ndata = nkeep + nrecv;
        } else if (MyProc > procmid && MyProc < procmid+nlower) {
          nfrac = (MyProc - procmid) * nsend / nlower;
          ipartner = MyProc - nlower;
          MPI_Sendrecv(&send[nfrac],nsend-nfrac,INT64_DT,ipartner,0,
                       &data[nkeep],CHUNKBIG,INT64_DT,
                       ipartner,0,MPI_COMM_WORLD,&status);
          MPI_Get_count(&status,INT64_DT,&nrecv);
          nkeep += nrecv;
          MPI_Sendrecv(send,nfrac,INT64_DT,ipartner-1,0,&data[nkeep],
                       CHUNKBIG,INT64_DT,ipartner-1,0,MPI_COMM_WORLD,&status);
          MPI_Get_count(&status,INT64_DT,&nrecv);
          ndata = nkeep + nrecv;
        } else {
          if (MyProc == procmid) ipartner = MyProc - nlower;
          else ipartner = MyProc - nupper;
          MPI_Sendrecv(send,nsend,INT64_DT,ipartner,0,&data[nkeep],
                       CHUNKBIG,INT64_DT,ipartner,0,MPI_COMM_WORLD,&status);
          MPI_Get_count(&status,INT64_DT,&nrecv);
          ndata = nkeep + nrecv;
        }
      }

      if (MyProc < procmid) npartition = nlower;
      else {
        proclo = procmid;
        npartition = nupper;
      }
    }

    for (i = 0; i < ndata; i++) {
      datum = data[i];
      index = (datum >> nglobalm1) - GlobalStartMyProc;
      HPCC_Table[index] ^= datum;
    }
  }

  /* clean up: should not really be part of this timed routine */

  free(data);
  free(send);
  free(offsets);
}

void
HPCC_Power2NodesMPIRandomAccessUpdate_LCG(u64Int logTableSize,
                                 u64Int TableSize,
                                 s64Int LocalTableSize,
                                 u64Int MinLocalTableSize,
                                 u64Int GlobalStartMyProc,
                                 u64Int Top,
                                 int logNumProcs,
                                 int NumProcs,
                                 int Remainder,
                                 int MyProc,
                                 s64Int ProcNumUpdates,
                                 MPI_Datatype INT64_DT,
                                 MPI_Status *finish_statuses,
                                 MPI_Request *finish_req) {
  int i, j, logTableLocal,ipartner;
  int ndata, nkeep, nsend, nrecv;
  s64Int iterate, niterate;
  u64Int ran,datum,procmask, nglobalm1, nlocalm1, index;
  u64Int *data,*send;
  MPI_Status status;

  /* setup: should not really be part of this timed routine */

  data = (u64Int *) malloc(CHUNKBIG*sizeof(u64Int));
  send = (u64Int *) malloc(CHUNKBIG*sizeof(u64Int));

  ran = HPCC_starts_LCG(4*GlobalStartMyProc);

  niterate = ProcNumUpdates / CHUNK;
  logTableLocal = logTableSize - logNumProcs;
  nlocalm1 = (u64Int)(LocalTableSize - 1);
  nglobalm1 = 64 - logTableSize;

  /* actual update loop: this is only section that should be timed */

  for (iterate = 0; iterate < niterate; iterate++) {
    for (i = 0; i < CHUNK; i++) {
      ran = LCG_MUL64 * ran + LCG_ADD64;
      data[i] = ran;
    }
    ndata = CHUNK;

    for (j = 0; j < logNumProcs; j++) {
      nkeep = nsend = 0;
      ipartner = (1 << j) ^ MyProc;
      procmask = ((u64Int) 1) << (nglobalm1 + logTableLocal + j);
      if (ipartner > MyProc) {
        for (i = 0; i < ndata; i++) {
          if (data[i] & procmask) send[nsend++] = data[i];
          else data[nkeep++] = data[i];
        }
      } else {
        for (i = 0; i < ndata; i++) {
          if (data[i] & procmask) data[nkeep++] = data[i];
          else send[nsend++] = data[i];
        }
      }

      MPI_Sendrecv(send,nsend,INT64_DT,ipartner,0,
                   &data[nkeep],CHUNKBIG,INT64_DT,
                   ipartner,0,MPI_COMM_WORLD,&status);
      MPI_Get_count(&status,INT64_DT,&nrecv);
      ndata = nkeep + nrecv;
    }

    for (i = 0; i < ndata; i++) {
      datum = data[i];
      index = datum >> nglobalm1;
      HPCC_Table[index & nlocalm1] ^= datum;
    }
  }

  /* clean up: should not really be part of this timed routine */

  free(data);
  free(send);
}
#endif
