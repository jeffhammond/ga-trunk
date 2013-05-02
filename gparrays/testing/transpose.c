#include <math.h>
#include <stdio.h>

#include "macdecls.h"
#include "ga.h"
#include "gp.h"
#define DEBUG 1
#define USE_HYPRE 1
#define IMAX 100
#define JMAX 100
#define KMAX 100
#define LMAX IMAX*JMAX*KMAX
#define MAXVEC 5000000
#define EPSLN 1.0d-10
#define CHECK_BOUND 1

#define bb_a(ib) bb_v(bb_i + (ib))
#define cc_a(ib) cc_v(cc_i + (ib))

#define Integer int
#define MAX_FACTOR 10000

#if USE_HYPRE
#include "HYPRE.h"
#include "HYPRE_struct_mv.h"
#include "mpi.h"
#endif

void grid_factor(int p, int xdim, int ydim, int zdim, int *idx, int *idy, int *idz) {
  int i, j; 
  int ip, ifac, pmax, prime[MAX_FACTOR];
  int fac[MAX_FACTOR];
  int ix, iy, iz, ichk;

  i = 1;
/*
     factor p completely
     first, find all prime numbers, besides 1, less than or equal to 
     the square root of p
*/
  ip = (int)(sqrt((double)p))+1;
  pmax = 0;
  for (i=2; i<=ip; i++) {
    ichk = 1;
    for (j=0; j<pmax; j++) {
      if (i%prime[j] == 0) {
        ichk = 0;
        break;
      }
    }
    if (ichk) {
      pmax = pmax + 1;
      if (pmax > MAX_FACTOR) printf("Overflow in grid_factor\n");
      prime[pmax-1] = i;
    }
  }
/*
     find all prime factors of p
*/
  ip = p;
  ifac = 0;
  for (i=0; i<pmax; i++) {
    while(ip%prime[i] == 0) {
      ifac = ifac + 1;
      fac[ifac-1] = prime[i];
      ip = ip/prime[i];
    }
  }
/*
     determine three factors of p of approximately the
     same size
*/
  *idx = 1;
  *idy = 1;
  *idz = 1;
  for (i = ifac-1; i >= 0; i--) {
    ix = xdim/(*idx);
    iy = ydim/(*idy);
    iz = zdim/(*idz);
    if (ix >= iy && ix >= iz && ix > 1) {
      *idx = fac[i]*(*idx);
    } else if (iy >= ix && iy >= iz && iy > 1) {
      *idy = fac[i]*(*idy);
    } else if (iz >= ix && iz >= iy && iz > 1) {
      *idz = fac[i]*(*idz);
    } else {
      printf("Too many processors in grid factoring routine\n");
    }
  }
}

/*
   Short subroutine for multiplying sparse matrix block with vector segment
*/
void loc_matmul(double *a_mat, int *jvec, int *ivec,
                double *bvec, double *cvec, int nrows) {
  double tempc;
  int i, j, jj, jmin,jmax;
  for (i=0; i<nrows; i++) {
    jmin = ivec[i];
    jmax = ivec[i+1]-1;
    tempc = 0.0;
    for (j=jmin; j<jmax; j++) {
      jj = jvec[j];
      tempc = tempc + a_mat[j]*bvec[jj];
    }
    cvec[i] = cvec[i] + tempc;
  }
}
/*
    Random number generator from Numerical Recipes
*/
#define  MBIG 1000000000
#define  MSEED 161803398
#define  MZ 0
#define  FAC 1.e-9
double ran3(int *idum) {
  static int iff = 0;
  if (*idum < 0 || iff == 0) {
    iff = 1;
    srand(abs(*idum));
    *idum = 1;
  }
  return ((double)rand())/((double)RAND_MAX);
}

/*
    create a random sparse matrix in compressed row form corresponding to a
    7-point stencil for a grid on a lattice of dimension idim X jdim X kdim grid
    points
*/
void create_laplace_mat(int idim, int jdim, int kdim, int pdi, int pdj, int pdk,
                        int *gp_block, int *g_j, int *g_i, int **imapc) {
/*
    idim: i-dimension of grid
    jdim: j-dimension of grid
    kdim: k-dimension of grid
    pdi: i-dimension of processor grid
    pdj: j-dimension of processor grid
    pdk: k-dimension of processor grid
!    g_data: global array of values
!    g_j: global array containing j indices (using local indices)
!    g_i: global array containing starting location of each row in g_j
!         (using local indices)
    gp_block: global pointer array containing non-zero sparse sub-blocks of
              matrix
    g_j: global array containing j indices of sub-blocks
    g_i: global array containing starting location of each row in g_j
    tsize: total number of non-zero elements in matrix
    imapc: map array for vectors
*/
  int ltotal_procs, ltsize;
  int *lproclist, *lproc_inv,  *lvoffset, *lnsize, *loffset, *licnt, *limapc;
  int *nnz_list;
  int nnz, offset, b_nnz;
  int nprocs, me, imin, imax, jcnt;
  int *jmin, *jmax;
  int ix, iy, iz, idx;
  double x, dr;
  double *rval, *gp_rval;
  int isize, idbg;
  int *jval, *gp_jval, *ival, *gp_ival, *ivalt;
  int i, j, k, itmp, one, tlo, thi, ld;
  int idum, ntot, indx, nghbrs[7], ncnt;
  int ixn[7],iyn[7],izn[7], procid[7];
  int status;
  int lo[3], hi[3], ip, jp, kp, ldi, ldj, jdx, joff;
  int il, jl, kl, ldmi, ldpi, ldmj, ldpj;
  int *xld, *yld, *zld, *tmapc;
  int *ecnt, *total_distr;
  int total_max, toffset;
  int *iparams, *blk_ptr;
  FILE *fp, *fopen();

  me = GA_Nodeid();
  nprocs = GA_Nnodes();
  idum = -(12345+me);
  x = ran3(&idum);
  one = 1;

  if (me == 0) {
    printf("\n Dimension of grid: \n\n");
    printf(" I Dimension: %d\n",idim);
    printf(" J Dimension: %d\n",jdim);
    printf(" K Dimension: %d\n\n",kdim);
  }
/*
   Find position of processor in processor grid and calulate minimum
   and maximum values of indices
*/
  i = me;
  ip = i%pdi;
  i = (i-ip)/pdi;
  jp = i%pdj;
  kp = (i-jp)/pdj;
 
  lo[0] = (int)((((double)idim)*((double)ip))/((double)pdi));
  if (ip < pdi-1) {
    hi[0] = (int)((((double)idim)*((double)(ip+1)))/((double)pdi))-1;
  } else {
    hi[0] = idim - 1;
  } 

  lo[1] = (int)((((double)jdim)*((double)jp))/((double)pdj));
  if (jp < pdj-1) {
    hi[1] = (int)((((double)jdim)*((double)(jp+1)))/((double)pdj))-1;
  } else {
    hi[1] = jdim - 1;
  } 

  lo[2] = (int)((((double)kdim)*((double)kp))/((double)pdk));
  if (kp < pdk-1) {
    hi[2] = (int)((((double)kdim)*((double)(kp+1)))/((double)pdk))-1;
  } else {
    hi[2] = kdim - 1;
  } 
 
  ldi = hi[0]-lo[0]+1;
  ldj = hi[1]-lo[1]+1;
 
  /* Evaluate xld, yld, zld. These contain the number of elements in each
     division along the x, y, z axes */
  xld = (int*)malloc(pdi*sizeof(int));
  for (i=0; i<pdi; i++) {
    if (i<pdi-1) {
      xld[i] = (int)((((double)idim)*((double)(i+1)))/((double)pdi));
    } else {
      xld[i] = idim;
    }
    xld[i] = xld[i] - (int)((((double)idim)*((double)(i)))/((double)pdi));
  }

  yld = (int*)malloc(pdj*sizeof(int));
  for (i=0; i<pdj; i++) {
    if (i<pdj-1) {
      yld[i] = (int)((((double)jdim)*((double)(i+1)))/((double)pdj));
    } else {
      yld[i] = jdim;
    }
    yld[i] = yld[i] - (int)((((double)jdim)*((double)(i)))/((double)pdj));
  }

  zld = (int*)malloc(pdk*sizeof(int));
  for (i=0; i<pdk; i++) {
    if (i<pdk-1) {
      zld[i] = (int)((((double)kdim)*((double)(i+1)))/((double)pdk));
    } else {
      zld[i] = jdim;
    }
    zld[i] = zld[i] - (int)((((double)kdim)*((double)(i)))/((double)pdk));
  }

/* Determine number of rows per processor
   lnsize[i]: number of rows associated with process i
   loffset[i]: global offset to location of first row associated
               with process i */

  lnsize = (int*)malloc(nprocs*sizeof(int));
  loffset = (int*)malloc(nprocs*sizeof(int));
  for (i=0; i<nprocs; i++) {
    lnsize[i] = 0;
    loffset[i] = 0;
  }
  lnsize[me] = (hi[0]-lo[0]+1)*(hi[1]-lo[1]+1)*(hi[2]-lo[2]+1);
  NGA_Igop(lnsize,nprocs,"+");
  loffset[0] = 0;
  for (i=1; i<nprocs; i++) {
    loffset[i] = loffset[i-1] + lnsize[i-1];
  }
 
  ntot = idim*jdim*kdim;
  NGA_Sync();
/*
    scan over rows of lattice
    imin: minimum global index of rows associated with this process (me)
    imax: maximum global index of rows associated with this process (me)
*/
  imin = loffset[me];
  imax = loffset[me]+lnsize[me]-1;
  free(loffset);
/*
    find out how many other processors couple to this row of blocks
    ecnt[i]: the number of columns on processor i that are coupled to this
    process
*/
  ecnt = (int*)malloc(nprocs*sizeof(int));
  for (i=0; i<nprocs; i++) {
    ecnt[i] = 0;
  }

  for (i=imin; i<=imax; i++) {
/*
    compute local indices of grid point corresponding to row i
*/
    indx = i - imin;
    ix = indx%ldi;
    indx = (indx - ix)/ldi;
    iy = indx%ldj;
    iz = (indx - iy)/ldj;
    ix = ix + lo[0];
    iy = iy + lo[1];
    iz = iz + lo[2];
 
    ecnt[me] = ecnt[me] + 1;
    if (ix+1 <= idim-1) {
      if (ix+1 > hi[0]) {
        jdx = kp*pdi*pdj + jp*pdi + ip + 1;
        ecnt[jdx] = ecnt[jdx] + 1;
      } else {
        ecnt[me] = ecnt[me] + 1;
      }
    }
    if (ix-1 >= 0) {
      if (ix-1 < lo[0]) {
        jdx = kp*pdi*pdj + jp*pdi + ip - 1;
        ecnt[jdx] = ecnt[jdx] + 1;
      } else {
        ecnt[me] = ecnt[me] + 1;
      }
    }
    if (iy+1 <= jdim-1) {
      if (iy+1 > hi[1]) {
        jdx = kp*pdi*pdj + (jp+1)*pdi + ip;
        ecnt[jdx] = ecnt[jdx] + 1;
      } else {
        ecnt[me] = ecnt[me] + 1;
      }
    }
    if (iy-1 >= 0) {
      if (iy-1 < lo[1]) {
        jdx = kp*pdi*pdj + (jp-1)*pdi + ip;
        ecnt[jdx] = ecnt[jdx] + 1;
      } else {
        ecnt[me] = ecnt[me] + 1;
      }
    }
    if (iz+1 <= kdim-1) {
      if (iz+1 > hi[2]) {
        jdx = (kp+1)*pdi*pdj + jp*pdi + ip;
        ecnt[jdx] = ecnt[jdx] + 1;
      } else {
        ecnt[me] = ecnt[me] + 1;
      }
    }
    if (iz-1 >= 0) {
      if (iz-1 < lo[2]) {
        jdx = (kp-1)*pdi*pdj + jp*pdi + ip;
        ecnt[jdx] = ecnt[jdx] + 1;
      } else {
        ecnt[me] = ecnt[me] + 1;
      }
    }
  }

/* Create list of processors that this processor is coupled to.
   If ecnt[i] is greater than zero then process i is coupled to this process.
   ltotal_procs: the total number of other processor that this process is coupled
                 to. This includes this process (the diagonal term).
   lproclist[i]: the IDs of the processor that this processor is coupled to
   lproc_inv[i]: the location in lproclist of processor i. If processor i is not
                 coupled to this process, the lproc_inv[i] = -1
   ncnt: total number of non-zero elements held by this process
   nnz_list[i]: number of processes coupled to process i by sparse blocks
   nnz: total number of sparse blocks */

  ltotal_procs = 0;
  ncnt = 0;
  for (i=0; i<nprocs; i++) {
    if (ecnt[i] > 0) {
      ltotal_procs += 1;
      ncnt += ecnt[i];
    }
  }

  lproclist = (int*)malloc(ltotal_procs*sizeof(int));
  lproc_inv = (int*)malloc(nprocs*sizeof(int));
  licnt = (int*)malloc(ltotal_procs*sizeof(int));
  for (i=0; i<ltotal_procs; i++) {
    licnt[i] = 0;
  }

  rval = (double*)malloc(ncnt*sizeof(double));
  idbg = ncnt;
  jval = (int*)malloc(ncnt*sizeof(int));
  ival = (int*)malloc((imax-imin+2)*ltotal_procs*sizeof(int));
  ivalt = (int*)malloc((imax-imin+2)*ltotal_procs*sizeof(int));

  for (i=0; i<ncnt; i++) {
    rval[i] = 0.0;
    jval[i] = 0;
  }

  j = (imax-imin+2)*ltotal_procs;
  for (i=0; i<j; i++) {
    ival[i] = 0;
    ivalt[i] = 0;
  }

  nnz_list = (int*)malloc(nprocs*sizeof(int));
  for (i=0; i<nprocs; i++) {
    nnz_list[i] = 0;
  }
  nnz_list[me] = ltotal_procs;
  NGA_Igop(nnz_list, nprocs, "+");
  nnz = 0;
  for (i=0; i<nprocs; i++) {
    nnz += nnz_list[i];
  }

/*  lvoffset[i]: local offset into array ival[i] to get to elements associated
    with block i (i runs from 0 to ltotal_procs-1)
    isize: number of rows (plus 1) that reside on this processor */
  isize = (imax-imin+2);
  for (i=0; i<nprocs; i++) {
    lproc_inv[i] = -1;
  }
  lvoffset = (int*)malloc(ltotal_procs*sizeof(int));
  lvoffset[0] = 0;
  j = 0;
  for (i=0; i<nprocs; i++) {
    if (ecnt[i] > 0) {
      lproclist[j] = i;
      if (j > 0) {
        lvoffset[j] = ecnt[lproclist[j-1]]+lvoffset[j-1];
      }
      lproc_inv[i] = j;
      j++;
    }
  }

/* Create arrays the hold the sparse block representation of the sparse matrix
   gp_block[nnz]: Global Pointer array holding the sparse sub-matrices
   g_j[nnz]: column block indices for the element in gp_block
   g_i[nprocs]: row block indices for the elements in g_j */

  tmapc = (int*)malloc((nprocs+1)*sizeof(int));
  tmapc[0] = 0;
  for (i=1; i<=nprocs; i++) {
    tmapc[i] = tmapc[i-1]+nnz_list[i-1];
  }
  *gp_block = GP_Create_handle();
  GP_Set_dimensions(*gp_block,one,&nnz);
  GP_Set_irreg_distr(*gp_block, tmapc, &nprocs);
  GP_Allocate(*gp_block);

  *g_j = NGA_Create_handle();
  NGA_Set_data(*g_j,one,&nnz,C_INT);
  NGA_Set_irreg_distr(*g_j, tmapc, &nprocs);
  NGA_Allocate(*g_j);

  for (i=0; i<nprocs; i++) {
    tmapc[i] = i;
  }
  *g_i = NGA_Create_handle();
  i = nprocs+1;
  NGA_Set_data(*g_i,one,&i,C_INT);
  NGA_Set_irreg_distr(*g_i, tmapc, &nprocs);
  NGA_Allocate(*g_i);
  free(tmapc);

  jmin = (int*)malloc(nprocs*sizeof(int));
  jmax = (int*)malloc(nprocs*sizeof(int));
  for (i=0; i<nprocs; i++) {
    jmin[i] = 0;
    jmax[i] = 0;
  }
  jmin[me] = imin;
  jmax[me] = imax;
  NGA_Igop(jmin, nprocs, "+");
  NGA_Igop(jmax, nprocs, "+");

/*
   Create the sparse blocks holding actual data. All the elements within each block
   couple this processor to one other processor
   rval[i]: values of matrix elements
   jval[i]: column indices of matrix elements
   ival[i]: index of first elements in rval and jval for the row represented by
            the index i.
   ivalt[i]: temporary array used in the construction of ival[i]
*/
  for (i=imin; i<=imax; i++) {
    /*
    compute local indices of grid point corresponding to row i
     */
    indx = i - imin;
    ix = indx%ldi;
    indx = (indx - ix)/ldi;
    iy = indx%ldj;
    iz = (indx - iy)/ldj;
    ix = ix + lo[0];
    iy = iy + lo[1];
    iz = iz + lo[2];
    /*
    find locations of neighbors in 7-point stencil (if they are on the grid)
     */
    ncnt = 0;
    ixn[ncnt] = ix;
    iyn[ncnt] = iy;
    izn[ncnt] = iz;
    il = ix - lo[0];
    jl = iy - lo[1];
    kl = iz - lo[2];
    idx = kl*ldi*ldj + jl*ldi + il;
    nghbrs[ncnt] = idx;
    procid[ncnt] = me;
    if (ix+1 <= idim - 1) {
      ncnt++;
      ixn[ncnt] = ix + 1;
      iyn[ncnt] = iy;
      izn[ncnt] = iz;
      if (ix+1 > hi[0]) {
        jdx = kp*pdi*pdj + jp*pdi + ip + 1;
        il = 0;
        jl = iy - lo[1];
        kl = iz - lo[2];
        ldpi = xld[ip+1];
      } else {
        jdx = me;
        il = ix - lo[0] + 1;
        jl = iy - lo[1];
        kl = iz - lo[2];
        ldpi = ldi;
      }
      idx = kl*ldpi*ldj + jl*ldpi + il;
      nghbrs[ncnt] = idx;
      procid[ncnt] = jdx;
    }
    if (ix-1 >= 0) {
      ncnt++;
      ixn[ncnt] = ix - 1;
      iyn[ncnt] = iy;
      izn[ncnt] = iz;
      if (ix-1 < lo[0]) {
        jdx = kp*pdi*pdj + jp*pdi + ip - 1;
        il = xld[ip] - 1;
        jl = iy - lo[1];
        kl = iz - lo[2];
        ldmi = xld[ip-1];
      } else {
        jdx = me;
        il = ix - lo[0] - 1;
        jl = iy - lo[1];
        kl = iz - lo[2];
        ldmi = ldi;
      }
      idx = kl*ldmi*ldj + jl*ldmi + il;
      nghbrs[ncnt] = idx;
      procid[ncnt] = jdx;
    }
    if (iy+1 <= jdim-1) {
      ncnt++;
      ixn[ncnt] = ix; 
      iyn[ncnt] = iy + 1;
      izn[ncnt] = iz;
      if (iy+1 > hi[1]) {
        jdx = kp*pdi*pdj + (jp+1)*pdi + ip;
        il = ix - lo[0];
        jl = 0;
        kl = iz - lo[2];
        ldpj = yld[jp+1];
      } else {
        jdx = me;
        il = ix - lo[0];
        jl = iy - lo[1] + 1;
        kl = iz - lo[2];
        ldpj = ldj;
      }
      idx = kl*ldi*ldpj + jl*ldi + il;
      nghbrs[ncnt] = idx;
      procid[ncnt] = jdx;
    }
    if (iy-1 >= 0) {
      ncnt++;
      ixn[ncnt] = ix;
      iyn[ncnt] = iy - 1;
      izn[ncnt] = iz;
      if (iy-1 < lo[1]) {
        jdx = kp*pdi*pdj + (jp-1)*pdi + ip;
        il = ix - lo[0];
        jl = yld[jp] - 1;
        kl = iz - lo[2];
        ldmj = yld[jp-1];
      } else {
        jdx = me;
        il = ix - lo[0];
        jl = iy - lo[1] - 1;
        kl = iz - lo[2];
        ldmj = ldj;
      }
      idx = kl*ldi*ldmj + jl*ldi + il;
      nghbrs[ncnt] = idx;
      procid[ncnt] = jdx;
    }
    if (iz+1 <= kdim-1) {
      ncnt++;
      ixn[ncnt] = ix;
      iyn[ncnt] = iy;
      izn[ncnt] = iz + 1;
      if (iz+1 > hi[2]) {
        jdx = (kp+1)*pdi*pdj + jp*pdi + ip;
        il = ix - lo[0];
        jl = iy - lo[1];
        kl = 0;
      } else {
        jdx = me;
        il = ix - lo[0];
        jl = iy - lo[1];
        kl = iz - lo[2] + 1;
      }
      idx = kl*ldi*ldj + jl*ldi + il;
      nghbrs[ncnt] = idx;
      procid[ncnt] = jdx;
    }
    if (iz-1 >= 0) {
      ncnt++;
      ixn[ncnt] = ix;
      iyn[ncnt] = iy;
      izn[ncnt] = iz - 1;
      if (iz-1 < lo[2]) {
        jdx = (kp-1)*pdi*pdj + jp*pdi + ip;
        il = ix - lo[0];
        jl = iy - lo[1];
        kl = zld[kp] - 1;
      } else {
        jdx = me;
        il = ix - lo[0];
        jl = iy - lo[1];
        kl = iz - lo[2] - 1;
      }
      idx = kl*ldi*ldj + jl*ldi + il;
      nghbrs[ncnt] = idx;
      procid[ncnt] = jdx;
    }
    /*
    sort indices so that neighbors run from lowest to highest local index. This sort
    is not particularly efficient but ncnt is generally small
     */
    ncnt++;
    for (j=0; j<ncnt; j++) {
      for (k=j+1; k<ncnt; k++) {
        if (nghbrs[j] > nghbrs[k]) {
          itmp = nghbrs[j];
          nghbrs[j] = nghbrs[k];
          nghbrs[k] = itmp;
          itmp = ixn[j];
          ixn[j] = ixn[k];
          ixn[k] = itmp;
          itmp = iyn[j];
          iyn[j] = iyn[k];
          iyn[k] = itmp;
          itmp = izn[j];
          izn[j] = izn[k];
          izn[k] = itmp;
          itmp = procid[j];
          procid[j] = procid[k];
          procid[k] = itmp;
        }
      }
    }
    for (k=0; k<ncnt; k++) {
      if (nghbrs[k] < 0 || nghbrs[k] >= ntot) {
        printf("p[%d] Invalid neighbor %d\n",me,nghbrs[k]);
      }
    }

/* set weights corresponding to a finite difference Laplacian on a 7-point
   stencil */

    for (j=0; j<ncnt; j++) {
      jdx = procid[j];
      idx = lproc_inv[jdx];
      if (ix == ixn[j] && iy == iyn[j] && iz == izn[j]) {
        rval[lvoffset[idx]+licnt[idx]] = 6.0;
      } else {
        rval[lvoffset[idx]+licnt[idx]] = -1.0;
      }
      if (lvoffset[idx]+licnt[idx]>=idbg) {
      }
      /* TODO: Check this carefully */
      jval[lvoffset[idx]+licnt[idx]] = nghbrs[j];
      ivalt[idx*isize+i-imin] = ivalt[idx*isize+i-imin]+1;
      licnt[idx]++;
    }
  }

/* finish evaluating ival array */

  for (i=0; i<ltotal_procs; i++) {
    ival[i*isize] = lvoffset[i];
    for (j=1; j<isize; j++) {
      ival[i*isize+j] = ival[i*isize+j-1] + ivalt[i*isize+j-1];
    }
  }
  isize = 0;
  for (i=0; i<ltotal_procs; i++) {
    isize = isize + licnt[i];
  }
  ltsize = isize;
  GA_Igop(&ltsize,one,"+");
  if (isize > MAXVEC)
    GA_Error("ISIZE exceeds MAXVEC in local arrays ",isize);

/* Local portion of sparse matrix has been evaluated and decomposed into blocks
   that match partitioning of right hand side across processors. The following
   data is available at this point:
      1) ltotal_procs: the number of processors that are coupled to this one via
         the sparse matrix
      2) lproclist(ltotal_procs): a list of processor IDs that are coupled to
         this processor
      3) lproc_inv(nprocs): The entry in proc_list that corresponds to a given
         processor. If the entry is -1 then that processor does not couple to
         this processor.
      4) licnt(ltotal_procs): The number of non-zero entries in the sparse matrix
         that couple the process represented by proc_list(j) to this process
      5) lvoffset(ltotal_procs): The offsets for the non-zero data in the arrays
         rval and jval for the blocks that couple this processor to other
         processes in proc_list
      6) offset(nprocs): the offset array for the distributed right hand side
         vector
    These arrays describe how the sparse matrix is layed out both locally and
    across processors. In addition, the actual data for the distributed sparse
    matrix is found in the following arrays:
      1) rval: values of matrix for all blocks on this processor
      2) jval: j-indices of matrix for all blocks on this processor
      3) ival(ltotal_procs*(lnsize(me)+1)): starting index in rval and
         jval for each row in each block */
 
  GA_Sync();

/* Create a sparse array of sparse blocks.
   Each block element is divided into for sections.
   The first section consists of 7 ints and contains the parameters
     imin: minimum i index represented by block
     imin: maximum i index represented by block
     jmin: minimum j index represented by block
     jmin: maximum j index represented by block
     iblock: row index of block
     jblock: column index of block
     nnz: number of non-zero elements in block
   The next section consists of nnz doubles that represent the non-zero values
   in the block. The third section consists of nnz ints and contains the local
   j indices of all values. The final section consists of (imax-imin+2) ints
   and contains the starting index in jval and rval for the each row between
   imin and imax. An extra value is included at the end and is set equal to
   nnz+1. This is included to simplify some coding.
 */

  offset = 0;
  for (i=0; i<me; i++) {
    offset += nnz_list[i];
  }
  NGA_Put(*g_i, &me, &me, &offset, &one);
  if (me==nprocs-1) {
    NGA_Put(*g_i, &nprocs, &nprocs, &nnz, &one);
  }
  for (i = 0; i<ltotal_procs; i++) {
    /* evaluate total size of block */
    b_nnz = ecnt[lproclist[i]];
    isize = 7*sizeof(int) + b_nnz*(sizeof(double)+sizeof(int))
          + (imax-imin+2)*sizeof(int);
    blk_ptr = (int*)GP_Malloc(isize);

    iparams = blk_ptr;
    gp_rval = (double*)(iparams+7);
    gp_jval = (int*)(gp_rval+b_nnz);
    gp_ival = (gp_jval+b_nnz);

    iparams[0] = imin;
    iparams[1] = imax;
    iparams[2] = jmin[lproclist[i]];
    iparams[3] = jmax[lproclist[i]];
    iparams[4] = me;
    iparams[5] = lproclist[i];
    iparams[6] = b_nnz;

    for (j=0; j<b_nnz; j++) {
      toffset = lvoffset[i];
      gp_rval[j] = rval[toffset+j];
      gp_jval[j] = jval[toffset+j];
      /*
      printf("p[%d] block: %d rval[%d]: %f jval[%d]: %d\n",me,i,j,gp_rval[j],j,gp_jval[j]);
      */
    }

    j = (imax-imin+2);
    /*
    printf("p[%d] block: %d imin: %d imax: %d jmin: %d jmax: %d\n",me,i,imin,imax,
          iparams[2],iparams[3]);
    printf("p[%d] ip: %d jp: %d b_nnz: %d nrows: %d\n",me,me,lproclist[i],b_nnz,j);
    */
    toffset = ival[i*j];
    for (k=0; k<j; k++) {
      gp_ival[k] = ival[i*j+k]-toffset;
      /*
      printf("p[%d] block: %d gp_ival[%d]: %d ival[%d]: %d\n",me,i,k,gp_ival[k],
             i*j+k,ival[i*j+k]);
             */
    }

    /* Assign blk_ptr to GP array element */
    /*
    printf("p[%d] offset: %d\n",me,offset);
    */
    GP_Assign_local_element(*gp_block, &offset, (void*)blk_ptr, isize);
    offset++;
  }

  tmapc = (int*)malloc(nprocs*sizeof(int));
  tmapc[0] = 0;
  for (i=1; i<nprocs; i++) {
    tmapc[i] = tmapc[i-1] + lnsize[i-1];
  }
    i = nprocs-1;
  *imapc = tmapc;

  free(rval);
  free(jval);
  free(ival);
  free(ivalt);
  free(xld);
  free(yld);
  free(zld);
  free(lnsize);
  free(lvoffset);
  free(ecnt);
  free(licnt);
  free(lproclist);
  free(lproc_inv);
  free(jmin);
  free(jmax);
  free(nnz_list);
  return;
}

int main(int argc, char **argv) {
  int nmax, nprocs, me, me_plus;
  int g_a_data, g_a_i, g_a_j, isize;
  int g_b, g_c;
  int i, j, jj, k, one, jcnt;
  int chunk, kp1, ld;
  int *p_i, *p_j;
  double *p_data, *p_b, *p_c;
  double t_beg, t_beg2, t_ga_tot, t_get, t_mult, t_cnstrct, t_mpi_in, t_ga_in;
  double t_hypre_strct;
  double prdot, dotga, dothypre, tempc;
  double prtot, gatot, hypretot;
  int status;
  int idim, jdim, kdim, idum, memsize;
  int lsize, ntot;
  int heap=200000, fudge=100, stack=200000, ma_heap;
  double *cbuf, *vector;
  int pdi, pdj, pdk, ip, jp, kp, ncells;
  int lo[3],hi[3];
  int blo[3], bhi[3];
  int ld_a, ld_b, ld_c, ld_i, ld_j, irows, ioff, joff, total_procs;
  int iproc, iblock, btot;
  double *amat, *bvec;
  int *ivec, *jvec;
  int *proclist, *proc_inv, *icnt;
  int *voffset, *offset, *mapc;
  int iloop, lo_bl, hi_bl;
  char *buf, **buf_ptr;
  int *iparams, *jval, *ival;
  double *rval;
  int imin, imax, jmin, jmax, irow, icol, nnz;
  int nrows, kmin, kmax, lmin, lmax, jdx;
  int LOOPNUM = 100;
  void **blk_ptr;
  void *blk;
  int blk_size, tsize;
/*
   Hypre declarations
*/
  int ierr;
#if USE_HYPRE
  HYPRE_StructGrid grid;
  HYPRE_StructStencil stencil;
  HYPRE_StructMatrix matrix;
  HYPRE_StructVector vec_x, vec_y;
  int i4, j4, ndim, nelems, offsets[7][3];
  int stencil_indices[7], hlo[3], hhi[3];
  double weights[7];
  double *values;
  double alpha, beta;
  int *rows, *cols;
#endif
/*
  ***  Intitialize a message passing library
*/
  one = 1;
  ierr = MPI_Init(&argc, &argv);
/*
 ***  Initialize GA
 
      There are 2 choices: ga_initialize or ga_initialize_ltd.
      In the first case, there is no explicit limit on memory usage.
      In the second, user can set limit (per processor) in bytes.
*/
  t_beg = GA_Wtime();
  GA_Initialize();
  GP_Initialize();
  t_ga_in = GA_Wtime() - t_beg;
  GA_Dgop(&t_ga_in,one,"+");

  t_ga_tot = 0.0;
  t_mult = 0.0;
  t_get = 0.0;
  t_hypre_strct = 0.0;
  prtot = 0.0;
  gatot = 0.0;
  hypretot = 0.0;

  me = GA_Nodeid();
  me_plus = me + 1;
  nprocs = GA_Nnodes();
  if (me == 0) {
   printf("Time to initialize GA:                                 %12.4f\n",
          t_ga_in/((double)nprocs));
  }
/*
     we can also use GA_set_memory_limit BEFORE first ga_create call
*/
  ma_heap = heap + fudge;
/*      call GA_set_memory_limit(util_mdtob(ma_heap)) */
 
  if (me == 0) {
    printf("\nNumber of cores used: %d\n\nGA initialized\n\n",nprocs);
  }
/*
 ***  Initialize the MA package
      MA must be initialized before any global array is allocated
*/
  if (!MA_init(MT_DBL, stack, ma_heap)) GA_Error("ma_init failed",-1);
/*
     create a sparse LMAX x LMAX matrix and two vectors of length
     LMAX. The matrix is stored in compressed row format.
     One of the vectors is filled with random data and the other
     is filled with zeros.
*/
  idim = IMAX;
  jdim = JMAX;
  kdim = KMAX;
  ntot = idim*jdim*kdim;
  if (me == 0) {
    printf("\nDimension of matrix: %d\n\n",ntot);
  }
  t_beg = GA_Wtime();
  grid_factor(nprocs,idim,jdim,kdim,&pdi,&pdj,&pdk);
  if (me == 0) {
    printf("\nProcessor grid configuration\n");
    printf("  PDX: %d\n",pdi);
    printf("  PDY: %d\n",pdj);
    printf("  PDZ: %d\n\n",pdk);
  }

  create_laplace_mat(idim,jdim,kdim,pdi,pdj,pdk,&g_a_data,&g_a_j,&g_a_i,&mapc);
  t_cnstrct = GA_Wtime() - t_beg;

  g_b = GA_Create_handle();
  GA_Set_data(g_b,one,&ntot,MT_DBL);
  GA_Set_irreg_distr(g_b,mapc,&nprocs);
  status = GA_Allocate(g_b);
/*
    fill g_b with random values
*/
  NGA_Distribution(g_b,me,blo,bhi);
  NGA_Access(g_b,blo,bhi,&p_b,&ld);
  ld = bhi[0]-blo[0]+1;
  btot = ld;
  vector = (double*)malloc(ld*sizeof(double));
  for (i=0; i<ld; i++) {
    idum  = 0;
    p_b[i] = ran3(&idum);
    vector[i] = p_b[i];
    /*
    printf("p[%d] p_b[%d]: %f\n",me,blo[0]+i,p_b[i]);
    */
  }
  NGA_Release(g_b,blo,bhi);
  GA_Sync();

  g_c = GA_Create_handle();
  NGA_Set_data(g_c,one,&ntot,MT_DBL);
  NGA_Set_irreg_distr(g_c,mapc,&nprocs);
  status = GA_Allocate(g_c);
  NGA_Zero(g_c);
#if USE_HYPRE
/*
    Assemble HYPRE grid and use that to create matrix. Start by creating
    grid partition
*/
  ndim = 3;
  i = me;
  ip = i%pdi;
  i = (i-ip)/pdi;
  jp = i%pdj;
  kp = (i-jp)/pdj;
  lo[0] = (int)(((double)idim)*((double)ip)/((double)pdi));
  if (ip < pdi-1) {
    hi[0] = (int)(((double)idim)*((double)(ip+1))/((double)pdi)) - 1;
  } else {
    hi[0] = idim - 1;
  }
  lo[1] = (int)(((double)jdim)*((double)jp)/((double)pdj));
  if (jp < pdj-1) {
    hi[1] = (int)(((double)jdim)*((double)(jp+1))/((double)pdj)) - 1;
  } else {
    hi[1] = jdim - 1;
  }
  lo[2] = (int)(((double)kdim)*((double)kp)/((double)pdk));
  if (kp < pdk-1) {
    hi[2] = (int)(((double)kdim)*((double)(kp+1))/((double)pdk)) - 1;
  } else {
    hi[2] = kdim - 1;
  }
/*
   Create grid
*/
  hlo[0] = lo[0];
  hlo[1] = lo[1];
  hlo[2] = lo[2];
  hhi[0] = hi[0];
  hhi[1] = hi[1];
  hhi[2] = hi[2];
  ierr = HYPRE_StructGridCreate(MPI_COMM_WORLD, ndim, &grid);
  ierr = HYPRE_StructGridSetExtents(grid, hlo, hhi);
  ierr = HYPRE_StructGridAssemble(grid);
/*
   Create stencil
*/
  offsets[0][0] = 0;
  offsets[0][1] = 0;
  offsets[0][2] = 0;

  offsets[1][0] = 1;
  offsets[1][1] = 0;
  offsets[1][2] = 0;

  offsets[2][0] = 0;
  offsets[2][1] = 1;
  offsets[2][2] = 0;

  offsets[3][0] = 0;
  offsets[3][1] = 0;
  offsets[3][2] = 1;

  offsets[4][0] = -1;
  offsets[4][1] = 0;
  offsets[4][2] = 0;

  offsets[5][0] = 0;
  offsets[5][1] = -1;
  offsets[5][2] = 0;

  offsets[6][0] = 0;
  offsets[6][1] = 0;
  offsets[6][2] = -1;

  nelems = 7;
  ierr = HYPRE_StructStencilCreate(ndim, nelems, &stencil);
  for (i=0; i<nelems; i++) {
    ierr = HYPRE_StructStencilSetElement(stencil, i, offsets[i]);
  }

  ncells = (hi[0]-lo[0]+1)*(hi[1]-lo[1]+1)*(hi[2]-lo[2]+1);
  jcnt = 7*ncells;
  values = (double*)malloc(jcnt*sizeof(double));
  jcnt = 0;
  weights[0] = 6.0;
  weights[1] = -1.0;
  weights[2] = -1.0;
  weights[3] = -1.0;
  weights[4] = -1.0;
  weights[5] = -1.0;
  weights[6] = -1.0;
  for (i=0; i<ncells; i++) {
    for (j=0; j<7; j++) {
      values[jcnt] = weights[j];
      jcnt++;
    }
  }

  ierr = HYPRE_StructMatrixCreate(MPI_COMM_WORLD, grid, stencil, &matrix);
  ierr = HYPRE_StructMatrixInitialize(matrix);
  for (i=0; i<7; i++) {
    stencil_indices[i] = i;
  }
  ierr = HYPRE_StructMatrixSetBoxValues(matrix, hlo, hhi, 7, stencil_indices, values);
  free(values);
/*
   Check all six sides of current box to see if any are boundaries.
   Set values to zero if they are.
*/
  if (hi[0] == idim-1) {
    ncells = (hi[1]-lo[1]+1)*(hi[2]-lo[2]+1);
    hlo[0] = idim-1;
    hhi[0] = idim-1;
    hlo[1] = lo[1];
    hhi[1] = hi[1];
    hlo[2] = lo[2];
    hhi[2] = hi[2];
    values = (double*)malloc(ncells*sizeof(double));
    for (i=0; i<ncells; i++) values[i] = 0.0;
    i4 = 1;
    j4 = 1;
    ierr = HYPRE_StructMatrixSetBoxValues(matrix, hlo, hhi, i4, &j4, values);
    free(values);
  }
  if (hi[1] == jdim-1) {
    ncells = (hi[0]-lo[0]+1)*(hi[2]-lo[2]+1);
    hlo[0] = lo[0];
    hhi[0] = hi[0];
    hlo[1] = jdim-1;
    hhi[1] = jdim-1;
    hlo[2] = lo[2];
    hhi[2] = hi[2];
    values = (double*)malloc(ncells*sizeof(double));
    for (i=0; i<ncells; i++) values[i] = 0.0;
    i4 = 1;
    j4 = 2;
    ierr = HYPRE_StructMatrixSetBoxValues(matrix, hlo, hhi, i4, &j4, values);
    free(values);
  } 
  if (hi[2] == kdim-1) {
    ncells = (hi[0]-lo[0]+1)*(hi[1]-lo[1]+1);
    hlo[0] = lo[0];
    hhi[0] = hi[0];
    hlo[1] = lo[1];
    hhi[1] = hi[1];
    hlo[2] = kdim-1;
    hhi[2] = kdim-1;
    values = (double*)malloc(ncells*sizeof(double));
    for (i=0; i<ncells; i++) values[i] = 0.0;
    i4 = 1;
    j4 = 3;
    ierr = HYPRE_StructMatrixSetBoxValues(matrix, hlo, hhi, i4, &j4, values);
    free(values);
  }
  if (lo[0] == 0) {
    ncells = (hi[1]-lo[1]+1)*(hi[2]-lo[2]+1);
    hlo[0] = 0;
    hhi[0] = 0;
    hlo[1] = lo[1];
    hhi[1] = hi[1];
    hlo[2] = lo[2];
    hhi[2] = hi[2];
    values = (double*)malloc(ncells*sizeof(double));
    for (i=0; i<ncells; i++) values[i] = 0.0;
    i4 = 1;
    j4 = 4;
    ierr = HYPRE_StructMatrixSetBoxValues(matrix, hlo, hhi, i4, &j4, values);
    free(values);
  }
  if (lo[1] == 0) {
    ncells = (hi[0]-lo[0]+1)*(hi[2]-lo[2]+1);
    hlo[0] = lo[0];
    hhi[0] = hi[0];
    hlo[1] = 0;
    hhi[1] = 0;
    hlo[2] = lo[2];
    hhi[2] = hi[2];
    values = (double*)malloc(ncells*sizeof(double));
    for (i=0; i<ncells; i++) values[i] = 0.0;
    i4 = 1;
    j4 = 5;
    ierr = HYPRE_StructMatrixSetBoxValues(matrix, hlo, hhi, i4, &j4, values);
    free(values);
  }
  if (lo[2] == 1) {
    ncells = (hi[1]-lo[1]+1)*(hi[2]-lo[2]+1);
    hlo[0] = lo[0];
    hhi[0] = hi[0];
    hlo[1] = lo[1];
    hhi[1] = hi[1];
    hlo[2] = 0;
    hhi[2] = 0;
    values = (double*)malloc(ncells*sizeof(double));
    for (i=0; i<ncells; i++) values[i] = 0.0;
    i4 = 1;
    j4 = 6;
    ierr = HYPRE_StructMatrixSetBoxValues(matrix, hlo, hhi, i4, &j4, values);
    free(values);
  }
  ierr = HYPRE_StructMatrixAssemble(matrix);
/*
    Create vectors for matrix-vector multiply
*/
  ierr = HYPRE_StructVectorCreate(MPI_COMM_WORLD, grid, &vec_x);
  ierr = HYPRE_StructVectorInitialize(vec_x);
  hlo[0] = lo[0];
  hlo[1] = lo[1];
  hlo[2] = lo[2];
  hhi[0] = hi[0];
  hhi[1] = hi[1];
  hhi[2] = hi[2];
  ierr = HYPRE_StructVectorSetBoxValues(vec_x, hlo, hhi, vector);
  ierr = HYPRE_StructVectorAssemble(vec_x);
  NGA_Distribution(g_a_i,me,blo,bhi);

  if (bhi[1] > ntot-1) {
    bhi[1] = ntot-1;
  }

  btot = (hi[0]-lo[0]+1)*(hi[1]-lo[1]+1)*(hi[2]-lo[2]+1);

  for (i=0; i<btot; i++) vector[i] = 0.0;
  hlo[0] = lo[0];
  hlo[1] = lo[1];
  hlo[2] = lo[2];
  hhi[0] = hi[0];
  hhi[1] = hi[1];
  hhi[2] = hi[2];
  ierr = HYPRE_StructVectorGetBoxValues(vec_x, hlo, hhi, vector);

  for (i=0; i<btot; i++) vector[i] = 0.0;
  ierr = HYPRE_StructVectorCreate(MPI_COMM_WORLD, grid, &vec_y);
  ierr = HYPRE_StructVectorInitialize(vec_y);
  ierr = HYPRE_StructVectorSetBoxValues(vec_y, hlo, hhi, vector);
  ierr = HYPRE_StructVectorAssemble(vec_y);
#endif
/* Multiply sparse matrix. Start by accessing pointers to local portions of
   g_a_data, g_a_j, g_a_i */

  for (iloop=0; iloop<LOOPNUM; iloop++) {
    t_beg2 = GA_Wtime();

    NGA_Distribution(g_c,me,blo,bhi);
    NGA_Access(g_c,blo,bhi,&p_c,&ld_c);
    for (i = 0; i<bhi[0]-blo[0]+1; i++) {
      p_c[i] = 0.0;
    }

/* get number of matrix blocks coupled to this process */
    NGA_Get(g_a_i,&me,&me,&lo_bl,&one);
    NGA_Get(g_a_i,&me_plus,&me_plus,&hi_bl,&one);
    hi_bl--;
    total_procs = hi_bl - lo_bl + 1;
    /*
    printf("p[%d] total_procs: %d\n",me,total_procs);
    */
    blk_ptr = (void**)malloc(sizeof(void*));
/* Loop through matrix blocks */
    ioff = 0;
    for (iblock = 0; iblock<total_procs; iblock++) {
      jdx = lo_bl+iblock;
      GP_Get_size(g_a_data, &jdx, &jdx, &isize);
      blk = (void*)malloc(isize);
      GP_Get(g_a_data, &jdx, &jdx, blk, blk_ptr, &one, &blk_size, &one, &tsize, 0); 
      iparams = (int*)blk_ptr[0];
      rval = (double*)(iparams+7);
      imin = iparams[0];
      imax = iparams[1];
      jmin = iparams[2];
      jmax = iparams[3];
      irow = iparams[4];
      icol = iparams[5];
      nnz = iparams[6];
      jval = (int*)(rval+nnz);
      ival = (int*)(jval+nnz);
      nrows = imax - imin + 1;
      bvec = (double*)malloc((jmax-jmin+1)*sizeof(double));
      j = 0;
      t_beg = GA_Wtime();
      NGA_Get(g_b,&jmin,&jmax,bvec,&j);
      t_get = t_get + GA_Wtime() - t_beg;
      t_beg = GA_Wtime();
      /*
    printf("p[%d] imin: %d imax: %d jmin: %d jmax: %d rval[0]: %f nrows: %d\n",
           me,imin,imax,jmin,jmax,rval[0],nrows);
           */
      for (i=0; i<nrows; i++) {
        kmin = ival[i];
        kmax = ival[i+1]-1;
        tempc = 0.0;
        for (j = kmin; j<=kmax; j++) {
          jj = jval[j];
          /*
          if (jj<0 || jj>=nrows) {
            printf("p[%d]: iblock: %d jj: %d nrows: %d\n",me,iblock,jj,nrows);
          }
          if (j<0 || j>=nnz) {
            printf("p[%d]: iblock: %d jj: %d nnz: %d\n",me,iblock,j,nnz);
          }
          */
          tempc = tempc + rval[j]*bvec[jj];
          /*
          printf("p[%d] kmin: %d kmax: %d rval[%d]: %f bvec[%d]: %f\n",me,kmin,kmax,j,rval[j],jj,bvec[jj]);
          */
        }
        p_c[i] = p_c[i] + tempc;
        /*
          printf("p[%d] p_c[%d]: %f\n",me,i,p_c[i]);
          */
      }
      t_mult = t_mult + GA_Wtime() - t_beg;
      free(bvec);
      free(blk);
    }
    t_ga_tot = t_ga_tot + GA_Wtime() - t_beg2;

    NGA_Distribution(g_c,me,blo,bhi);
    NGA_Release(g_c,blo,bhi);

#if USE_HYPRE
    alpha = 1.0;
    beta = 0.0;
    t_beg = GA_Wtime();
    ierr = HYPRE_StructMatrixMatvec(alpha, matrix, vec_x, beta, vec_y);
    t_hypre_strct = GA_Wtime() - t_beg;
    hlo[0] = lo[0];
    hlo[1] = lo[1];
    hlo[2] = lo[2];
    hhi[0] = hi[0];
    hhi[1] = hi[1];
    hhi[2] = hi[2];
    ierr = HYPRE_StructVectorGetBoxValues(vec_y, hlo, hhi, vector);
    NGA_Distribution(g_c,me,hlo,hhi);
    cbuf = (double*)malloc((hhi[0]-hlo[0]+1)*sizeof(double));
    NGA_Get(g_c,hlo,hhi,cbuf,&one);
    prdot = 0.0;
    dotga = 0.0;
    dothypre = 0.0;
    for (i=0; i<(hhi[0]-hlo[0]+1); i++) {
      /*
      printf("p[%d] ga[%d]: %f hypre[%d]: %f\n",me,lo[0]+i,cbuf[i],
             lo[i]+i,vector[i]);
             */
      dothypre = dothypre + vector[i]*vector[i];
      dotga = dotga + cbuf[i]*cbuf[i];
      prdot = prdot + (vector[i]-cbuf[i])*(vector[i]-cbuf[i]);
    }
    NGA_Dgop(&dotga,1,"+");
    NGA_Dgop(&dothypre,1,"+");
    NGA_Dgop(&prdot,1,"+");
    gatot += sqrt(dotga);
    hypretot += sqrt(dothypre);
    prtot += sqrt(prdot);
    free(cbuf);
#endif
  }
#if USE_HYPRE
  if (me == 0) {
    printf("Magnitude of GA solution:                         %e\n",
        gatot/((double)LOOPNUM));
    printf("Magnitude of HYPRE solution:                      %e\n",
        hypretot/((double)LOOPNUM));
    printf("Difference between GA and HYPRE (Struct) results: %e\n",
        prtot/((double)LOOPNUM));
  }
#endif

  free(vector);
/*
   Clean up arrays
*/
  NGA_Destroy(g_b);
  NGA_Destroy(g_c);
  GP_Destroy(g_a_data);
  NGA_Destroy(g_a_i);
  NGA_Destroy(g_a_j);
#if USE_HYPRE
#if USE_STRUCT
  ierr = HYPRE_StructStencilDestroy(stencil);
  ierr = HYPRE_StructGridDestroy(grid);
  ierr = HYPRE_StructMatrixDestroy(matrix);
  ierr = HYPRE_StructVectorDestroy(vec_x);
  ierr = HYPRE_StructVectorDestroy(vec_y);
#endif
#endif

  NGA_Dgop(&t_cnstrct,1,"+");
  NGA_Dgop(&t_get,1,"+");
  NGA_Dgop(&t_mult,1,"+");
  NGA_Dgop(&t_ga_tot,1,"+");
#if USE_HYPRE
  NGA_Dgop(&t_hypre_strct,1,"+");
#endif
  free(mapc);

  if (me == 0) {
    printf("Time to create sparse matrix:                         %12.4f\n",
      t_cnstrct/((double)nprocs));
    printf("Time to get right hand side vector:                   %12.4f\n",
      t_get/((double)nprocs));
    printf("Time for sparse matrix block multiplication:          %12.4f\n",
      t_mult/((double)nprocs));
    printf("Time for total sparse matrix multiplication:          %12.4f\n",
      t_ga_tot/((double)nprocs));
#if USE_HYPRE
#if USE_STRUCT
    printf("Total time for HYPRE (Struct)  matrix-vector multiply:%12.4f\n",
      t_hypre_strct/((double)nprocs));
#endif
#endif
  }
  if (me==0) {
    printf("Terminating GA library\n");
  }
  NGA_Terminate();
/*
 ***  Tidy up after message-passing library
 */
  ierr = MPI_Finalize();
}
