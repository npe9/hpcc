/* pzc2b.f -- translated by f2c (version 20030320).
   You must link the resulting object file with the libraries:
	-lf2c -lm   (in that order)
*/

#include "hpccfft.h"
#include "wrapmpifftw.h"

/*     FFTE: A FAST FOURIER TRANSFORM PACKAGE */

/*     (C) COPYRIGHT SOFTWARE, 2000-2004, ALL RIGHTS RESERVED */
/*                BY */
/*         DAISUKE TAKAHASHI */
/*         INSTITURE OF INFORMATION SCIENCES AND ELECTRONICS */
/*         UNIVERSITY OF TSUKUBA */
/*         1-1-1 TENNODAI, TSUKUBA, IBARAKI 305-8573, JAPAN */
/*         E-MAIL: daisuke@is.tsukuba.ac.jp */


/*     CYCLIC TO BLOCK REALIGNMENT ROUTINE (MPI VERSION) */

/*     FORTRAN77 SOURCE PROGRAM */

/*     CALL PZC2B(A,B,C,NN,NPU) */

/*     A(NN) IS COMPLEX INPUT VECTOR (COMPLEX*16) */
/* !HPF$ DISTRIBUTE A(CYCLIC) */
/*     B(NN) IS COMPLEX OUTPUT VECTOR (COMPLEX*16) */
/* !HPF$ DISTRIBUTE B(BLOCK) */
/*     C(NN) IS WORK VECTOR (COMPLEX*16) */

/*     NPU IS THE NUMBER OF PROCESSORS (INTEGER*4) */

/*     WRITTEN BY DAISUKE TAKAHASHI */

int
pzc2b_(doublecomplex *a, doublecomplex *b, doublecomplex 
       *c__, integer *nn, integer *npu, hpcc_fftw_mpi_plan p) {
    integer nn2;
    extern int ztrans_(doublecomplex *, doublecomplex *, integer *, integer *),
      pztrans_(doublecomplex *, doublecomplex *, integer *, integer *, hpcc_fftw_mpi_plan);

    nn2 = *nn / *npu;
    pztrans_( a, c__, nn, npu, p );
    ztrans_( c__, b, &nn2, npu );
    return 0;
} /* pzc2b_ */

