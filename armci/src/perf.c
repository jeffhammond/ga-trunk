/* 
 *    Author: Jialin Ju, PNNL
 */


#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <math.h>
#include <mpi.h>
#include "armci.h"

#define SIZE 550
#define MAXPROC 8
#define CHUNK_NUM 28

#ifndef ABS
#define ABS(a) ((a)>0? (a): -(a))
#endif

int CHECK_RESULT=0;

int chunk[CHUNK_NUM] = {1,3,4,6,9,12,16,20,24,30,40,48,52,64,78,91,104,
                        128,142,171,210,256,300,353,400,440,476,512};

char check_type[15];
int nproc, me;
int warn_accuracy=0;

void fill_array(double *arr, int count, int which);
void check_result(double *src_buf, double *dst_buf, int *stride, int *count,
                  int stride_levels);
void acc_array(double scale, double *array1, double *array2, int *stride,
               int *count, int stride_levels);


static double _tt0=0.0;
/*\ quick fix for inacurate timer
\*/
double Timer()
{
#define DELTA 0.00001
  double t=MPI_Wtime();
  if(t<=_tt0 + DELTA) _tt0 += DELTA;
  else _tt0 = t;
  return _tt0;
}

#define TIMER MPI_Wtime


double time_get(double *src_buf, double *dst_buf, int chunk, int loop,
                int proc, int levels)
{
    int i, bal = 0;
    
    int stride[2];
    int count[2];
    int stride_levels = levels;
    double *tmp_buf, *tmp_buf_ptr;
    
    double start_time, stop_time, total_time = 0;

    stride[0] = SIZE * sizeof(double);
    count[0] = chunk * sizeof(double); count[1] = chunk;

    if(CHECK_RESULT) {
        tmp_buf = (double *)malloc(SIZE * SIZE * sizeof(double));
        assert(tmp_buf != NULL);

        fill_array(tmp_buf, SIZE*SIZE, proc);
        tmp_buf_ptr = tmp_buf;
    }
    
    start_time = TIMER();
    for(i=0; i<loop; i++) {
        
        ARMCI_GetS(src_buf, stride, dst_buf, stride, count, stride_levels,
                   proc);

        if(CHECK_RESULT) {
            sprintf(check_type, "ARMCI_GetS:");
            check_result(tmp_buf_ptr, dst_buf, stride, count, stride_levels);
        }
        
        /* prepare next src and dst ptrs: avoid cache locality */
        if(bal == 0) {
            src_buf += chunk * (loop - i - 1);
            dst_buf += chunk * (loop - i - 1);
            if(CHECK_RESULT) tmp_buf_ptr += chunk * (loop - i - 1);
            bal = 1;
        } else {
            src_buf -= chunk * (loop - i - 1);
            dst_buf -= chunk * (loop - i - 1);
            if(CHECK_RESULT) tmp_buf_ptr -= chunk * (loop - i - 1);
            bal = 0;
        }
    }
    stop_time = TIMER();
    total_time = (stop_time - start_time);

    if(CHECK_RESULT) free(tmp_buf);

    if(total_time == 0.0){
       total_time=0.00001; /* workaround for inaccurate timers */
       warn_accuracy++;
    }
    return(total_time/loop);
}

double time_put(double *src_buf, double *dst_buf, int chunk, int loop,
                int proc, int levels)
{
    int i, bal = 0;

    int stride[2];
    int count[2];
    int stride_levels = levels;
    double *tmp_buf;

    double start_time, stop_time, total_time = 0;

    stride[0] = SIZE * sizeof(double);
    count[0] = chunk * sizeof(double); count[1] = chunk;

    if(CHECK_RESULT) {
        tmp_buf = (double *)malloc(SIZE * SIZE * sizeof(double));
        assert(tmp_buf != NULL);
    }
    
    start_time = TIMER();
    for(i=0; i<loop; i++) {

        ARMCI_PutS(src_buf, stride, dst_buf, stride,
                   count, stride_levels, proc);

        if(CHECK_RESULT) {
            ARMCI_GetS(dst_buf, stride, tmp_buf, stride, count,
                       stride_levels, proc);

            sprintf(check_type, "ARMCI_PutS:");
            check_result(tmp_buf, src_buf, stride, count, stride_levels);
        }
        
        /* prepare next src and dst ptrs: avoid cache locality */
        if(bal == 0) {
            src_buf += chunk * (loop - i - 1);
            dst_buf += chunk * (loop - i - 1);
            bal = 1;
        } else {
            src_buf -= chunk * (loop - i - 1);
            dst_buf -= chunk * (loop - i - 1);
            bal = 0;
        }
    }
    stop_time = TIMER();
    total_time = (stop_time - start_time);

    if(CHECK_RESULT) free(tmp_buf);
    
    if(total_time == 0.0){ 
       total_time=0.00001; /* workaround for inaccurate timers */
       warn_accuracy++;
    }
    return(total_time/loop);
}

double time_acc(double *src_buf, double *dst_buf, int chunk, int loop,
                int proc, int levels)
{
    int i, bal = 0;

    int stride[2];
    int count[2];
    int stride_levels = levels;
    double *before_buf, *after_buf;
    
    double start_time, stop_time, total_time = 0;

    stride[0] = SIZE * sizeof(double);
    count[0] = chunk * sizeof(double); count[1] = chunk;

    if(CHECK_RESULT) {
        before_buf = (double *)malloc(SIZE * SIZE * sizeof(double));
        assert(before_buf != NULL);
        after_buf = (double *)malloc(SIZE * SIZE * sizeof(double));
        assert(after_buf != NULL);
    }
    
    start_time = TIMER();
    for(i=0; i<loop; i++) {
        double scale = (double)i;

        if(CHECK_RESULT) {
            ARMCI_GetS(dst_buf, stride, before_buf, stride, count,
                       stride_levels, proc);

            acc_array(scale, before_buf, src_buf, stride, count,stride_levels);
        }

        ARMCI_AccS(ARMCI_ACC_DBL, &scale, src_buf, stride, dst_buf, stride,
                   count, stride_levels, proc);

        if(CHECK_RESULT) {
            ARMCI_GetS(dst_buf, stride, after_buf, stride, count,
                       stride_levels, proc);
            
            sprintf(check_type, "ARMCI_AccS:");
            check_result(after_buf, before_buf, stride, count, stride_levels);
        }
        
        /* prepare next src and dst ptrs: avoid cache locality */
        if(bal == 0) {
            src_buf += chunk * (loop - i - 1);
            dst_buf += chunk * (loop - i - 1);
            bal = 1;
        } else {
            src_buf -= chunk * (loop - i - 1);
            dst_buf -= chunk * (loop - i - 1);
            bal = 0;
        }
    }
    stop_time = TIMER();
    total_time = (stop_time - start_time);

    if(CHECK_RESULT) { free(before_buf); free(after_buf); }
    
    if(total_time == 0.0){ 
       total_time=0.00001; /* workaround for inaccurate timers */
       warn_accuracy++;
    }
    return(total_time/loop);
}

void test_1D()
{
    int i, j;
    int src, dst;
    int ierr;
    double *buf;
    void *ptr[MAXPROC], *get_ptr[MAXPROC];

    /* find who I am and the dst process */
    src = me;
    
    /* memory allocation */
    if(me == 0) {
        buf = (double *)malloc(SIZE * SIZE * sizeof(double));
        assert(buf != NULL);
    }
    
    ierr = ARMCI_Malloc(ptr, (SIZE * SIZE * sizeof(double)));
    assert(ierr == 0); assert(ptr[me]);
    ierr = ARMCI_Malloc(get_ptr, (SIZE * SIZE * sizeof(double)));
    assert(ierr == 0); assert(get_ptr[me]);

    /* ARMCI - initialize the data window */
    fill_array(ptr[me], SIZE*SIZE, me);
    fill_array(get_ptr[me], SIZE*SIZE, me);
    
    MPI_Barrier(MPI_COMM_WORLD);
    
    /* only the proc 0 doest the work */
    if(me == 0) {
        printf("\n\t\t\tRemote 1-D Array Section\n");
        if(!CHECK_RESULT){
          printf("  section               get                 put");
          printf("                 acc\n");
          printf("bytes   loop       sec      MB/s       sec      MB/s");
          printf("       sec      MB/s\n");
          printf("------- ------  --------  --------  --------  --------");
          printf("  --------  --------\n");
          fflush(stdout);
        }
        
        for(i=0; i<CHUNK_NUM; i++) {
            int loop;
            int bytes = chunk[i] * chunk[i] * sizeof(double);
            
            double t_get = 0, t_put = 0, t_acc = 0;
            double latency_get, latency_put, latency_acc;
            double bandwidth_get, bandwidth_put, bandwidth_acc;
            
            loop = (SIZE * SIZE) / (chunk[i] * chunk[i]);
            loop = (int)sqrt((double)loop);
            
            for(dst=1; dst<nproc; dst++) {
                /* strided get */
                fill_array(buf, SIZE*SIZE, me*10);
                t_get += time_get((double *)(get_ptr[dst]), (double *)buf,
                                  chunk[i]*chunk[i], loop, dst, 0);
                
                /* strided put */
                fill_array(buf, SIZE*SIZE, me*10);
                t_put += time_put((double *)buf, (double *)(ptr[dst]),
                                  chunk[i]*chunk[i], loop, dst, 0);
                
                /* strided acc */
                fill_array(buf, SIZE*SIZE, me*10);
                t_acc += time_acc((double *)buf, (double *)(ptr[dst]),
                                  chunk[i]*chunk[i], loop, dst, 0);
            }
            
            latency_get = t_get/(nproc - 1);
            latency_put = t_put/(nproc - 1);
            latency_acc = t_acc/(nproc - 1);
            
            bandwidth_get = (bytes * (nproc - 1) * 1e-6)/t_get;
            bandwidth_put = (bytes * (nproc - 1) * 1e-6)/t_put;
            bandwidth_acc = (bytes * (nproc - 1) * 1e-6)/t_acc;

            /* print */
            if(!CHECK_RESULT)printf("%d\t%d\t%.2e  %.2e  %.2e  %.2e  %.2e  %.2e\n",
                   bytes, loop, latency_get, bandwidth_get,
                   latency_put, bandwidth_put, latency_acc, bandwidth_acc);
        }
    }
    else sleep(10);
    
    MPI_Barrier(MPI_COMM_WORLD);
    
    /* cleanup */
    ARMCI_Free(get_ptr[me]);
    ARMCI_Free(ptr[me]);
    
    if(me == 0) free(buf);
}

void test_2D()
{
    int i, j;
    int src, dst;
    int ierr;
    double *buf;
    void *ptr[MAXPROC], *get_ptr[MAXPROC];

    /* find who I am and the dst process */
    src = me;
    
    /* memory allocation */
    if(me == 0) {
        buf = (double *)malloc(SIZE * SIZE * sizeof(double));
        assert(buf != NULL);
    }
    
    ierr = ARMCI_Malloc(ptr, (SIZE * SIZE * sizeof(double)));
    assert(ierr == 0); assert(ptr[me]);
    ierr = ARMCI_Malloc(get_ptr, (SIZE * SIZE * sizeof(double)));
    assert(ierr == 0); assert(get_ptr[me]);
    
    /* ARMCI - initialize the data window */
    fill_array(ptr[me], SIZE*SIZE, me);
    fill_array(get_ptr[me], SIZE*SIZE, me);

    MPI_Barrier(MPI_COMM_WORLD);
    
    /* only the proc 0 doest the work */
    /* print the title */
    if(me == 0) {
        printf("\n\t\t\tRemote 2-D Array Square Section\n");
        if(!CHECK_RESULT){
           printf("  section               get                 put");
           printf("                 acc\n");
           printf("bytes   loop       sec      MB/s       sec      MB/s");
           printf("       sec      MB/s\n");
           printf("------- ------  --------  --------  --------  --------");
           printf("  --------  --------\n");
           fflush(stdout);
        }
        
        for(i=0; i<CHUNK_NUM; i++) {
            int loop;
            int bytes = chunk[i] * chunk[i] * sizeof(double);

            double t_get = 0, t_put = 0, t_acc = 0;
            double latency_get, latency_put, latency_acc;
            double bandwidth_get, bandwidth_put, bandwidth_acc;
            
            loop = SIZE / chunk[i];

            for(dst=1; dst<nproc; dst++) {
                /* strided get */
                fill_array(buf, SIZE*SIZE, me*10);
                t_get += time_get((double *)(get_ptr[dst]), (double *)buf,
                                 chunk[i], loop, dst, 1);
 
                /* strided put */
                fill_array(buf, SIZE*SIZE, me*10);
                t_put += time_put((double *)buf, (double *)(ptr[dst]),
                                 chunk[i], loop, dst, 1);
                
                /* strided acc */
                fill_array(buf, SIZE*SIZE, me*10);
                t_acc += time_acc((double *)buf, (double *)(ptr[dst]),
                                 chunk[i], loop, dst, 1);
            }
            
            latency_get = t_get/(nproc - 1);
            latency_put = t_put/(nproc - 1);
            latency_acc = t_acc/(nproc - 1);
            
            bandwidth_get = (bytes * (nproc - 1) * 1e-6)/t_get;
            bandwidth_put = (bytes * (nproc - 1) * 1e-6)/t_put;
            bandwidth_acc = (bytes * (nproc - 1) * 1e-6)/t_acc;

            /* print */
            if(!CHECK_RESULT)printf("%d\t%d\t%.2e  %.2e  %.2e  %.2e  %.2e  %.2e\n",
                       bytes, loop, latency_get, bandwidth_get,
                       latency_put, bandwidth_put, latency_acc, bandwidth_acc);
        }
    }
    else sleep(10);
    
    /* cleanup */
    MPI_Barrier(MPI_COMM_WORLD);
    ARMCI_Free(get_ptr[me]);
    ARMCI_Free(ptr[me]);

    if(me==0)free(buf);
}

    
main(int argc, char **argv)
{
    
    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &me);
    MPI_Comm_size(MPI_COMM_WORLD, &nproc);

    if(nproc < 2) {
        if(me == 0)
            fprintf(stderr,
                    "USAGE: 2 <= processes < %d\n", nproc);
        MPI_Barrier(MPI_COMM_WORLD);
        MPI_Finalize();
        exit(0);
    }
    
    /* initialize ARMCI */
    ARMCI_Init();

    MPI_Barrier(MPI_COMM_WORLD);
    
    /* test 1 dimension array */
    test_1D();
    
    /* test 2 dimension array */
    test_2D();

    MPI_Barrier(MPI_COMM_WORLD);
    if(me == 0){
       if(warn_accuracy) 
          printf("\n\nWARNING: Your MPI timer does not have sufficient accuracy for this test (%d)\n",warn_accuracy);
       printf("\n\n------------ Now we test the same data transfer for correctness ----------\n");
       fflush(stdout);
    }

    CHECK_RESULT=1;
    MPI_Barrier(MPI_COMM_WORLD);
    test_1D();
    if(me == 0) printf("OK\n");
    MPI_Barrier(MPI_COMM_WORLD);
    test_2D();
    if(me == 0) printf("OK\n\n\nTests Completed.\n");
    MPI_Barrier(MPI_COMM_WORLD);

    /* done */
    ARMCI_Finalize();
    MPI_Finalize();
}    

void fill_array(double *arr, int count, int which)
{
    int i;

    for(i=0; i<count; i++) arr[i] = i * 8.23 + which * 2.89;
}

void check_result(double *src_buf, double *dst_buf, int *stride, int *count,
                  int stride_levels)
{
    int i, j, size;
    long idx;
    int n1dim;  /* number of 1 dim block */
    int bvalue[ARMCI_MAX_STRIDE_LEVEL], bunit[ARMCI_MAX_STRIDE_LEVEL];

    /* number of n-element of the first dimension */
    n1dim = 1;
    for(i=1; i<=stride_levels; i++)
        n1dim *= count[i];

    /* calculate the destination indices */
    bvalue[0] = 0; bvalue[1] = 0; bunit[0] = 1; bunit[1] = 1;
    for(i=2; i<=stride_levels; i++) {
        bvalue[i] = 0;
        bunit[i] = bunit[i-1] * count[i-1];
    }

    for(i=0; i<n1dim; i++) {
        idx = 0;
        for(j=1; j<=stride_levels; j++) {
            idx += bvalue[j] * stride[j-1];
            if((i+1) % bunit[j] == 0) bvalue[j]++;
            if(bvalue[j] > (count[j]-1)) bvalue[j] = 0;
        }
        
        size = count[0] / sizeof(double);
        for(j=0; j<size; j++)
            if(ABS(((double *)((char *)src_buf+idx))[j] - 
               ((double *)((char *)dst_buf+idx))[j]) > 0.000001 ){
                fprintf(stdout,"Error: %s comparison failed: (%d) (%f : %f)\n",
                        check_type, j, ((double *)((char *)src_buf+idx))[j],
                        ((double *)((char *)dst_buf+idx))[j]);
                ARMCI_Error("failed",0);
            }
    }
}

/* array1 = array1 + array2 * scale */
void acc_array(double scale, double *array1, double *array2, int *stride,
               int *count, int stride_levels)
{
        int i, j, size;
    long idx;
    int n1dim;  /* number of 1 dim block */
    int bvalue[ARMCI_MAX_STRIDE_LEVEL], bunit[ARMCI_MAX_STRIDE_LEVEL];

    /* number of n-element of the first dimension */
    n1dim = 1;
    for(i=1; i<=stride_levels; i++)
        n1dim *= count[i];

    /* calculate the destination indices */
    bvalue[0] = 0; bvalue[1] = 0; bunit[0] = 1; bunit[1] = 1;
    for(i=2; i<=stride_levels; i++) {
        bvalue[i] = 0;
        bunit[i] = bunit[i-1] * count[i-1];
    }

    for(i=0; i<n1dim; i++) {
        idx = 0;
        for(j=1; j<=stride_levels; j++) {
            idx += bvalue[j] * stride[j-1];
            if((i+1) % bunit[j] == 0) bvalue[j]++;
            if(bvalue[j] > (count[j]-1)) bvalue[j] = 0;
        }

        size = count[0] / sizeof(double);
        for(j=0; j<size; j++)
            ((double *)((char *)array1+idx))[j] +=
                ((double *)((char *)array2+idx))[j] * scale;

    }
}
