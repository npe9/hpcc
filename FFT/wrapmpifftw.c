
#include <stdio.h>
#include <stdlib.h>

#include "hpccfft.h"
#include "wrapmpifftw.h"

hpcc_fftw_mpi_plan
HPCC_fftw_mpi_create_plan(MPI_Comm comm, s64Int_t n, fftw_direction dir, int flags) {
  hpcc_fftw_mpi_plan p;
  fftw_complex *a = NULL, *b = NULL;
  int rank, size;

  MPI_Comm_size( comm, &size );
  MPI_Comm_rank( comm, &rank );

  p = fftw_malloc( sizeof *p );

  p->wx = fftw_malloc( (FFTE_NDA3/2 + FFTE_NP) * (sizeof *p->wx) );
  p->wy = fftw_malloc( (FFTE_NDA3/2 + FFTE_NP) * (sizeof *p->wy) );
  p->wz = fftw_malloc( (FFTE_NDA3/2 + FFTE_NP) * (sizeof *p->wz) );
  p->c = fftw_malloc( ((FFTE_NDA3+FFTE_NP) * (FFTE_NBLK + 1) + FFTE_NP) * (sizeof *p->c) );
  p->work = fftw_malloc( n / size * 3 / 2 * (sizeof *p->work) );

  p->n = n;
  p->comm = comm;
  p->dir = dir;
  p->flags = flags;

  MPI_Type_contiguous( 2, MPI_DOUBLE, &p->cmplx );
  MPI_Type_commit( &p->cmplx );

  if (FFTW_FORWARD == p->dir)
    p->timings = HPCC_fft_timings_forward;
  else
    p->timings = HPCC_fft_timings_backward;

  HPCC_pzfft1d( n, a, b, p->work, rank, size, 0, p );

  return p;
}

void
HPCC_fftw_mpi_destroy_plan(hpcc_fftw_mpi_plan p) {
  if (!p) return;

  MPI_Type_free( &p->cmplx );

  fftw_free( p->work );
  fftw_free( p->c );
  fftw_free( p->wz );
  fftw_free( p->wy );
  fftw_free( p->wx );
  fftw_free( p );
}

void
HPCC_fftw_mpi(hpcc_fftw_mpi_plan p, int n_fields, fftw_complex *local_data, fftw_complex *work){
  int rank, size;
  s64Int_t n;
  int i, ln;

  MPI_Comm_size( p->comm, &size );
  MPI_Comm_rank( p->comm, &rank );

  n = p->n;

  if (FFTW_FORWARD == p->dir)
    HPCC_pzfft1d( n, local_data, work, p->work, rank, size, -1, p );
  else
    HPCC_pzfft1d( n, local_data, work, p->work, rank, size, +1, p );

  ln = n / size;
  for (i = 0; i < ln; ++i) {
    c_assgn( local_data[i], work[i] );
  }
}

void
HPCC_fftw_mpi_local_sizes(hpcc_fftw_mpi_plan p, s64Int_t *local_n, s64Int_t *local_start,
  s64Int_t *local_n_after_transform, s64Int_t *local_start_after_transform, s64Int_t *total_local_size) {
  int rank, size;
  s64Int_t n;
  MPI_Comm_size( p->comm, &size );
  MPI_Comm_rank( p->comm, &rank );
  n = p->n;
  if (local_n) *local_n = n / size;
  if (local_start) *local_start = n / size * rank;
  if (local_n_after_transform) *local_n_after_transform = n / size;
  if (local_start_after_transform) *local_start_after_transform = n / size * rank;
  if (total_local_size) *total_local_size = n / size;
}
