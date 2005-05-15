/* -*- mode: C; tab-width: 2; indent-tabs-mode: nil; fill-column: 79; coding: iso-latin-1-unix -*- */
/*
  hpcc.c
*/

#include <hpcc.h>

#include <ctype.h>
#include <time.h>

static int
ReadInts(char *buf, int n, int *val) {
  int i, j;

  for (j = i = 0; i < n; i++) {
    if (sscanf( buf + j, "%d", val + i ) != 1) {
      i--;
      break;
    }
    for (; buf[j] && isdigit(buf[j]); j++)
      ; /* EMPTY */
    for (; buf[j] && ! isdigit(buf[j]); j++)
      ; /* EMPTY */
    if (! buf[j]) {
      i--;
      break;
    }
  }

  return i + 1;
}

static int
HPCC_InitHPL(HPCC_Params *p) {
  HPL_pdinfo( &p->test, &p->ns, p->nval, &p->nbs, p->nbval, &p->porder, &p->npqs, p->pval,
              p->qval, &p->npfs, p->pfaval, &p->nbms, p->nbmval, &p->ndvs, p->ndvval, &p->nrfs,
              p->rfaval, &p->ntps, p->topval, &p->ndhs, p->ndhval, &p->fswap, &p->tswap,
              &p->L1notran, &p->Unotran, &p->equil, &p->align );

  if (p->test.thrsh <= 0.0) p->Failure = 1;

  return 0;
}

static int
iiamax(int n, int *x, int incx) {
  int i, v, mx, idx = 0;

  idx = 0;
  mx = (x[0] < 0 ? -x[0] : x[0]);
  for (i = 0; i < n; i++, x += incx) {
    v = (x[i] < 0 ? -x[i] : x[i]);
    if (mx < v) {mx = v; idx = i;}
  }

  return idx;
}

static void
icopy(int n, int *src, int sinc, int *dst, int dinc) {
  int i;

  for (i = n; i; i--) {
    *dst = *src;
    dst += dinc;
    src += sinc;
  }
}

int
HPCC_InputFileInit(HPCC_Params *params) {
  int myRank, commSize;
  int i, j, n, ioErr, lastConfigLine = 32, line, rv, maxHPLn;
  char buf[82]; int nbuf = 82;
  FILE *f, *outputFile;
  MPI_Comm comm = MPI_COMM_WORLD;

  MPI_Comm_size( comm, &commSize );
  MPI_Comm_rank( comm, &myRank );

  if (0 == myRank) {
    f = fopen( params->inFname, "r" );
    if (! f) {
      ioErr = 1;
      goto ioEnd;
    }

    /* skip irrelevant lines in config file */
            for (line = 0; line < lastConfigLine; line++)
              if (! fgets( buf, nbuf, f )) break;

            if (line < lastConfigLine) { /* if didn't read all the required lines */
      ioErr = 1;
      goto ioEnd;
    }

    /* Get values of N for PTRANS */
    line++;
    fgets( buf, nbuf, f );
    rv = sscanf( buf, "%d", &n );
    if (rv != 1 || n < 0) { /* parse error or negative value*/
      n = 0;
      BEGIN_IO(myRank, params->outFname, outputFile);
      FPRINTF( myRank, outputFile, "Error in line %d of the input file.", line );
      END_IO(  myRank, outputFile );
    }
    n = MIN( n, HPL_MAX_PARAM );

    line++;
    fgets( buf, nbuf, f );
    ReadInts( buf, n, params->PTRANSnval );

    /* find the largest matrix for HPL */
    maxHPLn = params->nval[iiamax( params->ns, params->nval, 1 )];

    for (j = i = 0; i < n; i++) {
      /* if memory for PTRANS is at least 90% of what's used for HPL */
      if (params->PTRANSnval[i] >= 0.9486 * maxHPLn * 0.5) {
        params->PTRANSnval[j] = params->PTRANSnval[i];
        j++;
      }
    }
    n = j; /* only this many entries use enough memory */

    /* copy matrix sizes from HPL, divide by 2 so both PTRANS matrices (plus "work" arrays) occupy
       as much as HPL's one */
    for (i = 0; i < params->ns; i++)
      params->PTRANSnval[i + n] = params->nval[i] / 2;
    params->PTRANSns = n + params->ns;

    /* Get values of block sizes */
    line++;
    fgets( buf, nbuf, f );
    rv = sscanf( buf, "%d", &n );
    if (rv != 1 || n < 0) { /* parse error or negative value*/
      n = 0;
      BEGIN_IO(myRank, params->outFname, outputFile);
      FPRINTF( myRank, outputFile, "Error in line %d of the input file.", line );
      END_IO(  myRank, outputFile );
    }
    n = MIN( n, HPL_MAX_PARAM );

    line++;
    fgets( buf, nbuf, f );
    ReadInts( buf, n, params->PTRANSnbval );

    icopy( params->nbs, params->nbval, 1, params->PTRANSnbval + n, 1 );
    params->PTRANSnbs = n + params->nbs;

    ioErr = 0;
    ioEnd:
    if (f) fclose( f );
  }

  MPI_Bcast( &ioErr, 1, MPI_INT, 0, comm );
  if (ioErr) return ioErr;

  /* broadcast what's been read on node 0 */
  MPI_Bcast( &params->PTRANSns, 1, MPI_INT, 0, comm );
  if (params->PTRANSns > 0)
    MPI_Bcast( &params->PTRANSnval, params->PTRANSns, MPI_INT, 0, comm );
  MPI_Bcast( &params->PTRANSnbs, 1, MPI_INT, 0, comm );
  if (params->PTRANSnbs > 0)
    MPI_Bcast( &params->PTRANSnbval, params->PTRANSnbs, MPI_INT, 0, comm );

  /* copy what HPL has */
  params->PTRANSnpqs = params->npqs;
  icopy( params->npqs, params->qval, 1, params->PTRANSqval, 1 );
  icopy( params->npqs, params->pval, 1, params->PTRANSpval, 1 );

  return 0;
}

int
HPCC_Init(HPCC_Params *params) {
  int myRank, commSize;
  int i, nMax, procCur, procMax;
  double totalMem;
  char inFname[] = "hpccinf.txt", outFname[] = "hpccoutf.txt";
  FILE *outputFile;
  MPI_Comm comm = MPI_COMM_WORLD;
  time_t currentTime;
  char hostname[MPI_MAX_PROCESSOR_NAME + 1]; int hostnameLen;

  i = MPI_Get_processor_name( hostname, &hostnameLen );
  if (i) hostname[0] = 0;
  else hostname[MAX(hostnameLen, MPI_MAX_PROCESSOR_NAME)] = 0;
  time( &currentTime );

  MPI_Comm_size( comm, &commSize );
  MPI_Comm_rank( comm, &myRank );

  strcpy( params->inFname, inFname );
  strcpy( params->outFname, outFname );

  BEGIN_IO(myRank, params->outFname, outputFile);
  FPRINTF( myRank, outputFile,
            "########################################################################%s", "" );
  FPRINTF3( myRank, outputFile,
            "This is the DARPA/DOE HPC Challange Benchmark version %d.%d.%d October 2003",
            HPCC_VERSION_MAJOR, HPCC_VERSION_MINOR, HPCC_VERSION_MICRO );
  FPRINTF( myRank, outputFile, "Produced by Jack Dongarra and Piotr Luszczek%s", "" );
  FPRINTF( myRank, outputFile, "Innovative Computing Laboratory%s", "" );
  FPRINTF( myRank, outputFile, "University of Tennessee Knoxville and Oak Ridge National Laboratory%s", "" );
  FPRINTF( myRank, outputFile, "%s", "" );
  FPRINTF( myRank, outputFile, "See the source files for authors of specific codes. %s", "" );
  FPRINTF( myRank, outputFile, "Compiled on " __DATE__ " at %s", __TIME__ );
  FPRINTF2(myRank, outputFile, "Current time (%ld) is %s",(long)currentTime,ctime(&currentTime));
  FPRINTF( myRank, outputFile, "Hostname: '%s'", hostname );
  FPRINTF( myRank, outputFile,
            "########################################################################%s", "" );
  END_IO(  myRank, outputFile );

  params->Failure = 0;

  HPCC_InitHPL( params ); /* HPL calls exit() if there is a problem */
  HPCC_InputFileInit( params );

  params->RunHPL = 0;
  params->RunStarDGEMM = 0;
  params->RunSingleDGEMM = 0;
  params->RunPTRANS = 0;
  params->RunStarStream = 0;
  params->RunSingleStream = 0;
  params->RunMPIRandomAccess = 0;
  params->RunStarRandomAccess = 0;
  params->RunSingleRandomAccess = 0;
  params->RunLatencyBandwidth = 0;
  params->RunMPIFFT = 0;
  params->RunHPL = params->RunStarDGEMM = params->RunSingleDGEMM =
  params->RunPTRANS = params->RunStarStream = params->RunSingleStream =
  params->RunMPIRandomAccess = params->RunStarRandomAccess = params->RunSingleRandomAccess = 
  params->RunMPIFFT = params->RunStarFFT = params->RunSingleFFT = 
  params->RunLatencyBandwidth = 1;

  params->MPIGUPs = params->StarGUPs = params->SingleGUPs =
  params->StarDGEMMGflops = params->SingleDGEMMGflops = -1.0;
  params->StarStreamCopyGBs = params->StarStreamScaleGBs = params->StarStreamAddGBs =
  params->StarStreamTriadGBs = params->SingleStreamCopyGBs = params->SingleStreamScaleGBs =
  params->SingleStreamAddGBs = params->SingleStreamTriadGBs =
  params->SingleFFTGflops = params->StarFFTGflops = params->MPIFFTGflops = params->MPIFFT_maxErr =
  params->MaxPingPongLatency = params-> RandomlyOrderedRingLatency = params-> MinPingPongBandwidth =
  params->NaturallyOrderedRingBandwidth = params->RandomlyOrderedRingBandwidth =
  params->MinPingPongLatency = params->AvgPingPongLatency = params->MaxPingPongBandwidth =
  params->AvgPingPongBandwidth = params->NaturallyOrderedRingLatency = -1.0;

  params->HPLrdata.Gflops = -1000.0;
  params->HPLrdata.time = params->HPLrdata.eps = params->HPLrdata.RnormI = params->HPLrdata.Anorm1 = params->HPLrdata.AnormI = params->HPLrdata.Xnorm1 = params->HPLrdata.XnormI = -1.0;
  params->HPLrdata.N = params->HPLrdata.NB = params->HPLrdata.nprow = params->HPLrdata.npcol = params->HPLrdata.depth = params->HPLrdata.nbdiv = params->HPLrdata.nbmin = -1;
  params->HPLrdata.cpfact = params->HPLrdata.crfact = params->HPLrdata.ctop = params->HPLrdata.order = '-';

  params->PTRANSrdata.GBs = params->PTRANSrdata.time = params->PTRANSrdata.residual = -1.0;
  params->PTRANSrdata.n = params->PTRANSrdata.nb = params->PTRANSrdata.nprow =
  params->PTRANSrdata.npcol = -1;

  params->RandomAccess_N =
  params->MPIRandomAccess_Errors =
  params->MPIRandomAccess_ErrorsFraction =
  params->MPIRandomAccess_N =
  params->MPIRandomAccess_time = params->MPIRandomAccess_CheckTime =
  params->MPIFFT_N = -1.0;

  params->DGEMM_N =
  params->FFT_N =
  params->StreamVectorSize = -1;
  params->StreamThreads = 1;

  procMax = 0;
  for (i = 0; i < params->npqs; i++) {
    procCur = params->pval[i] * params->qval[i];
    if (procMax < procCur) procMax = procCur;
  }
  params->HPLMaxProc = procMax;

  nMax = params->nval[iiamax( params->ns, params->nval, 1 )];

  /* totalMem = (nMax*nMax) * sizeof(double) */
  totalMem = nMax;
  totalMem *= nMax;
  totalMem *= sizeof(double);
  params->HPLMaxProcMem = totalMem / procMax;

  for (i = 0; i < MPIFFT_TIMING_COUNT; i++)
    params->MPIFFTtimingsForward[i] = 0.0;

  return 0;
}

int
HPCC_Finalize(HPCC_Params *params) {
  int myRank, commSize;
  int i;
  FILE *outputFile;
  MPI_Comm comm = MPI_COMM_WORLD;
  time_t currentTime;

  time( &currentTime );

  MPI_Comm_rank( comm, &myRank );
  MPI_Comm_size( comm, &commSize );

  BEGIN_IO(myRank, params->outFname, outputFile);
  FPRINTF( myRank, outputFile, "Begin of Summary section.%s", "" );
  FPRINTF( myRank, outputFile, "VersionMajor=%d", HPCC_VERSION_MAJOR );
  FPRINTF( myRank, outputFile, "VersionMinor=%d", HPCC_VERSION_MINOR );
  FPRINTF( myRank, outputFile, "VersionMicro=%d", HPCC_VERSION_MICRO );
  FPRINTF( myRank, outputFile, "VersionRelease=%c", HPCC_VERSION_RELEASE );
  FPRINTF( myRank, outputFile, "LANG=%s", "C" );
  FPRINTF( myRank, outputFile, "Success=%d", params->Failure ? 0 : 1 );
  FPRINTF( myRank, outputFile, "sizeof_char=%d", (int)sizeof(char) );
  FPRINTF( myRank, outputFile, "sizeof_short=%d", (int)sizeof(short) );
  FPRINTF( myRank, outputFile, "sizeof_int=%d", (int)sizeof(int) );
  FPRINTF( myRank, outputFile, "sizeof_long=%d", (int)sizeof(long) );
  FPRINTF( myRank, outputFile, "sizeof_void_ptr=%d", (int)sizeof(void*) );
  FPRINTF( myRank, outputFile, "sizeof_size_t=%d", (int)sizeof(size_t) );
  FPRINTF( myRank, outputFile, "sizeof_float=%d", (int)sizeof(float) );
  FPRINTF( myRank, outputFile, "sizeof_double=%d", (int)sizeof(double) );
  FPRINTF( myRank, outputFile, "CommWorldProcs=%d", commSize );
  FPRINTF( myRank, outputFile, "MPI_Wtick=%e", MPI_Wtick() );
  FPRINTF( myRank, outputFile, "HPL_Tflops=%g", params->HPLrdata.Gflops * 1e-3 );
  FPRINTF( myRank, outputFile, "HPL_time=%g", params->HPLrdata.time );
  FPRINTF( myRank, outputFile, "HPL_eps=%g", params->HPLrdata.eps );
  FPRINTF( myRank, outputFile, "HPL_RnormI=%g", params->HPLrdata.RnormI );
  FPRINTF( myRank, outputFile, "HPL_Anorm1=%g", params->HPLrdata.Anorm1 );
  FPRINTF( myRank, outputFile, "HPL_AnormI=%g", params->HPLrdata.AnormI );
  FPRINTF( myRank, outputFile, "HPL_Xnorm1=%g", params->HPLrdata.Xnorm1 );
  FPRINTF( myRank, outputFile, "HPL_XnormI=%g", params->HPLrdata.XnormI );
  FPRINTF( myRank, outputFile, "HPL_N=%d", params->HPLrdata.N );
  FPRINTF( myRank, outputFile, "HPL_NB=%d", params->HPLrdata.NB );
  FPRINTF( myRank, outputFile, "HPL_nprow=%d", params->HPLrdata.nprow );
  FPRINTF( myRank, outputFile, "HPL_npcol=%d", params->HPLrdata.npcol );
  FPRINTF( myRank, outputFile, "HPL_depth=%d", params->HPLrdata.depth );
  FPRINTF( myRank, outputFile, "HPL_nbdiv=%d", params->HPLrdata.nbdiv );
  FPRINTF( myRank, outputFile, "HPL_nbmin=%d", params->HPLrdata.nbmin );
  FPRINTF( myRank, outputFile, "HPL_cpfact=%c", params->HPLrdata.cpfact );
  FPRINTF( myRank, outputFile, "HPL_crfact=%c", params->HPLrdata.crfact );
  FPRINTF( myRank, outputFile, "HPL_ctop=%c", params->HPLrdata.ctop );
  FPRINTF( myRank, outputFile, "HPL_order=%c", params->HPLrdata.order );
  FPRINTF( myRank, outputFile, "HPLMaxProcs=%d", params->HPLMaxProc );
  FPRINTF( myRank, outputFile, "DGEMM_N=%d", params->DGEMM_N );
  FPRINTF( myRank, outputFile, "StarDGEMM_Gflops=%g",   params->StarDGEMMGflops );
  FPRINTF( myRank, outputFile, "SingleDGEMM_Gflops=%g", params->SingleDGEMMGflops );
  FPRINTF( myRank, outputFile, "PTRANS_GBs=%g", params->PTRANSrdata.GBs );
  FPRINTF( myRank, outputFile, "PTRANS_time=%g", params->PTRANSrdata.time );
  FPRINTF( myRank, outputFile, "PTRANS_residual=%g", params->PTRANSrdata.residual );
  FPRINTF( myRank, outputFile, "PTRANS_n=%d", params->PTRANSrdata.n );
  FPRINTF( myRank, outputFile, "PTRANS_nb=%d", params->PTRANSrdata.nb );
  FPRINTF( myRank, outputFile, "PTRANS_nprow=%d", params->PTRANSrdata.nprow );
  FPRINTF( myRank, outputFile, "PTRANS_npcol=%d", params->PTRANSrdata.npcol );
  FPRINTF( myRank, outputFile, "MPIRandomAccess_N=%g", params->MPIRandomAccess_N );
  FPRINTF( myRank, outputFile, "MPIRandomAccess_time=%g", params->MPIRandomAccess_time );
  FPRINTF( myRank, outputFile, "MPIRandomAccess_CheckTime=%g", params->MPIRandomAccess_CheckTime );
  FPRINTF( myRank, outputFile, "MPIRandomAccess_Errors=%g", params->MPIRandomAccess_Errors );
  FPRINTF( myRank, outputFile, "MPIRandomAccess_ErrorsFraction=%g", params->MPIRandomAccess_ErrorsFraction );
  FPRINTF( myRank, outputFile, "MPIRandomAccess_GUPs=%g", params->MPIGUPs );
  FPRINTF( myRank, outputFile, "RandomAccess_N=%g", params->RandomAccess_N );
  FPRINTF( myRank, outputFile, "StarRandomAccess_GUPs=%g", params->StarGUPs );
  FPRINTF( myRank, outputFile, "SingleRandomAccess_GUPs=%g", params->SingleGUPs );
  FPRINTF( myRank, outputFile, "STREAM_VectorSize=%d", params->StreamVectorSize );
  FPRINTF( myRank, outputFile, "STREAM_Threads=%d", params->StreamThreads );
  FPRINTF( myRank, outputFile, "StarSTREAM_Copy=%g", params->StarStreamCopyGBs );
  FPRINTF( myRank, outputFile, "StarSTREAM_Scale=%g", params->StarStreamScaleGBs );
  FPRINTF( myRank, outputFile, "StarSTREAM_Add=%g", params->StarStreamAddGBs );
  FPRINTF( myRank, outputFile, "StarSTREAM_Triad=%g", params->StarStreamTriadGBs );
  FPRINTF( myRank, outputFile, "SingleSTREAM_Copy=%g", params->SingleStreamCopyGBs );
  FPRINTF( myRank, outputFile, "SingleSTREAM_Scale=%g", params->SingleStreamScaleGBs );
  FPRINTF( myRank, outputFile, "SingleSTREAM_Add=%g", params->SingleStreamAddGBs );
  FPRINTF( myRank, outputFile, "SingleSTREAM_Triad=%g", params->SingleStreamTriadGBs );
  FPRINTF( myRank, outputFile, "FFT_N=%d", params->FFT_N );
  FPRINTF( myRank, outputFile, "StarFFT_Gflops=%g",   params->StarFFTGflops );
  FPRINTF( myRank, outputFile, "SingleFFT_Gflops=%g", params->SingleFFTGflops );
  FPRINTF( myRank, outputFile, "MPIFFT_N=%g", params->MPIFFT_N );
  FPRINTF( myRank, outputFile, "MPIFFT_Gflops=%g", params->MPIFFTGflops );
  FPRINTF( myRank, outputFile, "MPIFFT_maxErr=%g", params->MPIFFT_maxErr );
  FPRINTF( myRank, outputFile, "MaxPingPongLatency_usec=%g", params->MaxPingPongLatency );
  FPRINTF( myRank, outputFile, "RandomlyOrderedRingLatency_usec=%g", params->RandomlyOrderedRingLatency );
  FPRINTF( myRank, outputFile, "MinPingPongBandwidth_GBytes=%g", params->MinPingPongBandwidth );
  FPRINTF( myRank, outputFile, "NaturallyOrderedRingBandwidth_GBytes=%g", params->NaturallyOrderedRingBandwidth );
  FPRINTF( myRank, outputFile, "RandomlyOrderedRingBandwidth_GBytes=%g", params->RandomlyOrderedRingBandwidth );
  FPRINTF( myRank, outputFile, "MinPingPongLatency_usec=%g", params->MinPingPongLatency );
  FPRINTF( myRank, outputFile, "AvgPingPongLatency_usec=%g", params->AvgPingPongLatency );
  FPRINTF( myRank, outputFile, "MaxPingPongBandwidth_GBytes=%g", params->MaxPingPongBandwidth );
  FPRINTF( myRank, outputFile, "AvgPingPongBandwidth_GBytes=%g", params->AvgPingPongBandwidth );
  FPRINTF( myRank, outputFile, "NaturallyOrderedRingLatency_usec=%g", params->NaturallyOrderedRingLatency );
  for (i = 0; i < MPIFFT_TIMING_COUNT - 1; i++)
    FPRINTF2(myRank,outputFile, "MPIFFT_time%d=%g", i, params->MPIFFTtimingsForward[i+1] - params->MPIFFTtimingsForward[i] );
  FPRINTF( myRank, outputFile, "End of Summary section.%s", "" );
  FPRINTF( myRank, outputFile,
            "########################################################################%s", "" );
  FPRINTF( myRank, outputFile, "End of HPC Challange tests.%s", "" );
  FPRINTF2(myRank, outputFile, "Current time (%ld) is %s",(long)currentTime,ctime(&currentTime));
  FPRINTF( myRank, outputFile,
            "########################################################################%s", "" );
  END_IO(  myRank, outputFile );

  return 0;
}

int
MinStoreBits(unsigned long x) {
  int i;

  for (i = 0; x; i++, x >>= 1)
    ; /* EMPTY */

  return i;
}

#ifdef XERBLA_MISSING

#ifdef Add_
#define F77xerbla xerbla_
#endif
#ifdef NoChange
#define F77xerbla xerbla
#endif
#ifdef UpCase
#define F77xerbla XERBLA
#endif
#ifdef f77IsF2C
#define F77xerbla xerbla_
#endif

void
F77xerbla(char *srname, F77_INTEGER *info, long srname_len) {
  /*
  int i; char Cname[7];
  for (i = 0; i < 6; i++) Cname[i] = srname[i];
  Cname[6] = 0;
  printf("xerbla(%d)\n", *info);
  */
  printf("xerbla()\n");
  fflush(stdout);
}
#endif
