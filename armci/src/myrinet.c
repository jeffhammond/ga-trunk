/* $Id: myrinet.c,v 1.32 2001-09-11 18:17:21 d3h325 Exp $
 * DISCLAIMER
 *
 * This material was prepared as an account of work sponsored by an
 * agency of the United States Government.  Neither the United States
 * Government nor the United States Department of Energy, nor Battelle,
 * nor any of their employees, MAKES ANY WARRANTY, EXPRESS OR IMPLIED, OR
 * ASSUMES ANY LEGAL LIABILITY OR RESPONSIBILITY FOR THE ACCURACY,
 * COMPLETENESS, OR USEFULNESS OF ANY INFORMATION, APPARATUS, PRODUCT,
 * SOFTWARE, OR PROCESS DISCLOSED, OR REPRESENTS THAT ITS USE WOULD NOT
 * INFRINGE PRIVATELY OWNED RIGHTS.
 *
 *
 * ACKNOWLEDGMENT
 *
 * This software and its documentation were produced with United States
 * Government support under Contract Number DE-AC06-76RLO-1830 awarded by
 * the United States Department of Energy.  The United States Government
 * retains a paid-up non-exclusive, irrevocable worldwide license to
 * reproduce, prepare derivative works, perform publicly and display
 * publicly by or for the US Government, including the right to
 * distribute to other US Government contractors.
 *
 * History: 
 * 03/00, Jialin: initial version
 * 9/8/00, Jarek: added armci_gm_server_ready to fix timing problems at startup
 * 10/12/00 jarek: fixed unititialized context variable -hangs
 *                 changed context allocation to static
 * 10/00    jarek: support for overlaping client pinning with memcopy at server
 *
 */


#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <sys/mman.h>

#include "myrinet.h"
#define GM_STRONG_TYPES 0
#include "gm.h"

#include "armcip.h"

#define DEBUG_ 0
#define DEBUG2 0
#define DEBUG_INIT_ 0
#define STATIC_PORTS__

#define FALSE  0
#define TRUE   1
#define TAG_DFLT  0
#define TAG_SHORT 1

/* call back */
#define ARMCI_GM_SENT    1
#define ARMCI_GM_SENDING 3

/* msg ack */
#define ARMCI_GM_CLEAR     0
#define ARMCI_GM_READY     1
#define ARMCI_GM_COMPLETE -2
#define ARMCI_GM_ACK      -3

#define ARMCI_GM_MIN_MESG_SIZE 5
#define SHORT_MSGLEN 68000
#define SND_BUFLEN (MSG_BUFLEN +128)

extern void armci_buf_free(char *ap);
extern void armci_init_buf_alloc(size_t len, void* buffer);

/***************/

/* data structure of computing process */
typedef struct {
    unsigned int node_id;    /* my node id */
    int *node_map;           /* other's node id */
    int *port_map;           /* servers port to send request to */
    struct gm_port *port;    /* my port */
    long *ack;               /* acknowledgment for server */
    long *tmp;
    long **serv_ack_ptr;     /* keep the pointers of server ack buffer */
} armci_gm_proc_t;

/* data structure of server thread */
typedef struct {
    int node_id;            /* my node id */
    int port_id;            /* my port id */
    int *node_map;          /* other's node id */
    int *port_map;          /* other's port id. server only */
    struct gm_port *rcv_port;   /* server receive port */
    struct gm_port *snd_port;   /* server receive port */
    void **dma_buf;         /* dma memory for receive */
    void **dma_buf_short;   /* dma memory for receive */
    long *ack;              /* ack for each computing process */
    long *direct_ack;
    long *proc_buf_ptr;     /* keep the pointers of client MessageSndBuffer */
    long *proc_ack_ptr;
    unsigned long pending_msg_ct;
    unsigned long complete_msg_ct;
} armci_gm_serv_t;

/***************/

extern struct gm_port *gmpi_gm_port; /* the port that mpi currently using */

armci_gm_proc_t __armci_proc_gm_struct;
armci_gm_proc_t *proc_gm = &__armci_proc_gm_struct;

armci_gm_context_t __armci_gm_client_context_struct = {0, ARMCI_GM_CLEAR};
armci_gm_context_t *armci_gm_client_context = &__armci_gm_client_context_struct;

armci_gm_context_t __armci_gm_serv_context_struct = {0, ARMCI_GM_CLEAR};
armci_gm_context_t *armci_gm_serv_context = &__armci_gm_serv_context_struct;

armci_gm_context_t __armci_gm_serv_ack_context_struct = {0, ARMCI_GM_CLEAR};
armci_gm_context_t *armci_gm_serv_ack_context = &__armci_gm_serv_ack_context_struct;

armci_gm_serv_t __armci_serv_gm_struct;
armci_gm_serv_t *serv_gm = &__armci_serv_gm_struct;

char *MessageSndBuffer;
char *MessageRcvBuffer;

int armci_gm_bypass = 0;
static int armci_gm_server_ready = 0;
static int armci_gm_num_send_tokens=0;
static int armci_gm_num_receive_tokens=0;

GM_ENTRY_POINT char * _gm_get_kernel_build_id(struct gm_port *p);

/*********************************************************************
                        UTILITY FUNCTIONS                            
 *********************************************************************/

int __armci_wait_some =20;
double __armci_fake_work=99.0;
extern long check_flag(long*);

/* check memory */
void armci_wait_long_flag_updated(long *buf, int val)
{
    long res;
    long spin =0;

    res = check_flag(buf);
    while(res != (long)val){
       for(spin=0; spin<__armci_wait_some; spin++)__armci_fake_work+=0.001;
       res = check_flag(buf);
    }
    __armci_fake_work =99.0;
}

void armci_wait_long_flag_updated_clear(long *buf, int val)
{
    armci_wait_long_flag_updated(buf,val);
    *buf = ARMCI_GM_CLEAR;
}

/*\ wait until flag is not ARMCI_GM_CLEAR and return its value
\*/
long armci_wait_long_flag_not_clear(long *buf)
{
    long res;
    long spin =0;

    res = check_flag(buf);
    while(res == ARMCI_GM_CLEAR){
       for(spin=0; spin<__armci_wait_some; spin++)__armci_fake_work+=0.001;
       res = check_flag(buf);
    }
/*    *buf = ARMCI_GM_CLEAR; */
    __armci_fake_work =99.0;
    return res;
}


static int pin_in_block;   /* indicate pin memory in one large block or not */
static int pin_in_segment; /* when pining segment by segment, serves as
                            * counter how many segments have been pinned so far
                            */

int armci_pin_contig(void *ptr, int bytes)
{
    gm_status_t status;
    struct gm_port *port;
    if(SERVER_CONTEXT) port = serv_gm->snd_port;
    else port = proc_gm->port;
    status = gm_register_memory(port, (char *)ptr, bytes);
    if(status == GM_SUCCESS) return TRUE;
    printf("%d:  pinning failed %d\n",armci_me, bytes);
    fflush(stdout);

    return FALSE;
}

void armci_unpin_contig(void *ptr, int bytes)
{
    gm_status_t status;
    struct gm_port *port;

    if(SERVER_CONTEXT) port = serv_gm->snd_port;
    else port = proc_gm->port;

    status = gm_deregister_memory(port, (char *)ptr, bytes);
    if(status != GM_SUCCESS)
       armci_die(" unpinning cont memory failed", armci_me);
}


int armci_pin_memory(void *ptr, int stride_arr[], int count[], int strides)
{
    int i, j;
    long idx;
    int n1dim;  /* number of 1 dim block */
    int bvalue[MAX_STRIDE_LEVEL], bunit[MAX_STRIDE_LEVEL];
    gm_status_t status;
    struct gm_port *port;

    if(SERVER_CONTEXT) port = serv_gm->snd_port;
    else port = proc_gm->port;
    
    if(strides ==0){
       if(gm_register_memory(port, (char *)ptr, count[0])==GM_SUCCESS)
          return TRUE;
       else return FALSE;
    }

    if(count[0] == stride_arr[0]){
       int sizes = 1;
       for(i=0; i<strides; i++) sizes *= stride_arr[i];
       sizes *= count[strides];
        
       status = gm_register_memory(port, (char *)ptr, sizes);
       if(status == GM_SUCCESS) { pin_in_block = TRUE; return TRUE; }
    }

    pin_in_block = FALSE;
    pin_in_segment = 0;  /* set counter to zero */
    
    /* if can pin memory in one piece, pin it segment by segment */
    n1dim = 1;
    for(i=1; i<=strides; i++) n1dim *= count[i];

    /* calculate the destination indices */
    bvalue[0] = 0; bvalue[1] = 0; bunit[0] = 1; bunit[1] = 1;
    for(i=2; i<=strides; i++) {
        bvalue[i] = 0; bunit[i] = bunit[i-1] * count[i-1];
    }

    for(i=0; i<n1dim; i++) {
        idx = 0;
        for(j=1; j<=strides; j++) {
            idx += bvalue[j] * stride_arr[j-1];
            if((i+1) % bunit[j] == 0) bvalue[j]++;
            if(bvalue[j] > (count[j]-1)) bvalue[j] = 0;
        }

        status = gm_register_memory(port, (char *)ptr+idx, count[0]);
        if(status != GM_SUCCESS) {
            armci_unpin_memory(ptr, stride_arr, count, strides);
            printf("%d: strided pinning 2 failed %d\n",armci_me, count[0]);
            fflush(stdout);
            return FALSE;
        }
        pin_in_segment++;
    }

    return TRUE;
}


void armci_unpin_memory(void *ptr, int stride_arr[], int count[], int strides)
{
    int i, j, sizes;
    long idx;
    int n1dim;  /* number of 1 dim block */
    int bvalue[MAX_STRIDE_LEVEL], bunit[MAX_STRIDE_LEVEL]; 
    gm_status_t status;
    struct gm_port *port;

    if(SERVER_CONTEXT) port = serv_gm->snd_port;
    else port = proc_gm->port;

    if(strides ==0){
       if(gm_deregister_memory(port, (char *)ptr, count[0])!=GM_SUCCESS)
            armci_die(" unpinning memory failed", armci_me);
    }
   
    if(pin_in_block) {
        sizes = 1;
        for(i=0; i<strides; i++) sizes *= stride_arr[i];
        sizes *= count[strides];
        
#if 1
        status = gm_deregister_memory(port, (char *)ptr, sizes);
        if(status != GM_SUCCESS)
            armci_die(" unpinning memory failed", armci_me);
#else
        status = munlock((char *)ptr, sizes);
        if(!status)  return; 
        else armci_die("unpin failed",sizes);
#endif

    } else {
        
        /* if can unpin memory in one piece, unpin it segment by segment */
        n1dim = 1;
        for(i=1; i<=strides; i++) n1dim *= count[i];
        
        /* calculate the destination indices */
        bvalue[0] = 0; bvalue[1] = 0; bunit[0] = 1; bunit[1] = 1;
        for(i=2; i<=strides; i++) {
            bvalue[i] = 0; bunit[i] = bunit[i-1] * count[i-1];
        }
        
        for(i=0; i<n1dim; i++) {
            idx = 0;
            for(j=1; j<=strides; j++) {
                idx += bvalue[j] * stride_arr[j-1];
                if((i+1) % bunit[j] == 0) bvalue[j]++;
                if(bvalue[j] > (count[j]-1)) bvalue[j] = 0;
            }

            if(pin_in_segment > 0) {
                status = gm_deregister_memory(port, (char *)ptr+idx, count[0]);
                if(status != GM_SUCCESS)
                    armci_die(" unpinning memory failed", armci_me);
                pin_in_segment--;
            }
        }
    }
}

/*********************************************************************
                           COMPUTING PROCESS                            
 *********************************************************************/


/* pre-allocate required memory at start up*/
int armci_gm_client_mem_alloc()
{
char *tmp;

    /* allocate buf keeping the pointers of server ack buf */
    proc_gm->serv_ack_ptr = (long **)calloc(armci_nclus, sizeof(long*));
    if(!proc_gm->serv_ack_ptr) return FALSE;

    /* allocate send buffer */
    MessageSndBuffer = (char *)gm_dma_malloc(proc_gm->port, SND_BUFLEN);
    if(MessageSndBuffer == 0) return FALSE;

    tmp = gm_dma_malloc(proc_gm->port, 2*sizeof(long));
    if(!tmp)return FALSE;
    
    proc_gm->ack = (long*)tmp;
    proc_gm->tmp = (long*)(tmp + sizeof(long)); 

    return TRUE;
}

/* deallocate the preallocated memory used by gm */
int armci_gm_proc_mem_free()
{
    free(proc_gm->serv_ack_ptr);

    gm_dma_free(proc_gm->port, proc_gm->ack);
    gm_dma_free(proc_gm->port, MessageSndBuffer);
        
    return TRUE;
}


/* initialization of client process/thread */
int armci_gm_client_init()
{
    int status,i;
    
    /* allocate gm data structure for computing process */
    proc_gm->node_map = (int *)calloc(armci_nproc, sizeof(int));
    if(!proc_gm->node_map) armci_die("Error allocating proc data structure",0);

    proc_gm->port_map = (int*)calloc(armci_nclus, sizeof(int));
    if(!proc_gm->port_map)armci_die("error allocating serv port map",0);

    /* use existing MPI port to save ports */
    proc_gm->port = gmpi_gm_port;

    /* get my node id */
    status = gm_get_node_id(proc_gm->port, &(proc_gm->node_id));
    if(status != GM_SUCCESS)armci_die("Could not get GM node id",0); 
    if(DEBUG_INIT_) printf("%d: node id is %d\n", armci_me, proc_gm->node_id);

    /* broadcast my node id to other processes */
    proc_gm->node_map[armci_me] = proc_gm->node_id;
    armci_msg_igop(proc_gm->node_map, armci_nproc, "+");

#if 1
    if(armci_me==armci_master){
       for(i=0; i<armci_nproc; i++) serv_gm->node_map[i] = proc_gm->node_map[i];

       /* publish port id of local server thread to other smp nodes */
       proc_gm->port_map[armci_clus_me] = serv_gm->port_id;
       armci_msg_gop_scope(SCOPE_MASTERS,proc_gm->port_map, armci_nclus, 
                           "+", ARMCI_INT);
    }

    /* master makes port ids of server processes available to other tasks */
    armci_msg_bcast_scope(SCOPE_NODE,proc_gm->port_map, armci_nclus*sizeof(int),
                          armci_master);
#endif

    /* allow direct send */
#if 1
    status = gm_allow_remote_memory_access(proc_gm->port);
    if(status != GM_SUCCESS) armci_die("could not enable direct sends",0);
#endif

    /* memory preallocation for computing process */
    if(!armci_gm_client_mem_alloc()) armci_die(" client mem alloc failed ",0); 

    /* query GM for number of tokens available */
    armci_gm_num_receive_tokens = gm_num_receive_tokens(proc_gm->port);
    armci_gm_num_send_tokens = gm_num_send_tokens(proc_gm->port);
    if(DEBUG_ && armci_me==0){
        printf("has %d send %d receive tokens\n",armci_gm_num_send_tokens,
           armci_gm_num_receive_tokens); fflush(stdout);
    }

#ifdef CLIENT_BUF_BYPASS
    /* get the gm version number and set bypass flag: need GM >1.1 */
    if(armci_me == 0) {
        char gm_version[8];
        strncpy(gm_version, _gm_get_kernel_build_id(proc_gm->port), 3);
        /*printf("GM version is %s\n",gm_version);*/
        gm_version[3] = '\0';
        if(strcmp(gm_version, "1.0") == 0) armci_gm_bypass = FALSE;
        else if(strcmp(gm_version, "1.1") == 0) armci_gm_bypass = FALSE;
        else armci_gm_bypass = TRUE;
    }
    armci_msg_brdcst(&armci_gm_bypass, sizeof(int), 0);
#endif
    
    ((armci_gm_context_t*)MessageSndBuffer)->done=ARMCI_GM_CLEAR;
    return TRUE;
}


/* callback func of gm_send_with_callback */
void armci_client_send_callback(struct gm_port *port, void *context,gm_status_t status)
{
    if(status==GM_SUCCESS){
         ((armci_gm_context_t*)context)->done = ARMCI_GM_CLEAR;
    }else ((armci_gm_context_t *)context)->done = ARMCI_GM_FAILED;
}

/* callback func of gm_send_directed */
void armci_client_send_callback_direct(struct gm_port *port, void *context,gm_status_t status)
{
    if(status==GM_SUCCESS)((armci_gm_context_t*)context)->done = ARMCI_GM_CLEAR;
    else ((armci_gm_context_t *)context)->done = ARMCI_GM_FAILED;
}

/* client trigers gm_unknown, so that callback func can be executed */
void armci_client_send_complete(armci_gm_context_t* context)
{
    MPI_Status status;
    int flag;
    
    /* blocking: wait til the send is done by calling the callback */
    while(context->done == ARMCI_GM_SENDING) 
        MPI_Iprobe(armci_me, MPI_ANY_TAG, MPI_COMM_WORLD, &flag, &status);
    if(context->done == ARMCI_GM_FAILED)
       armci_die("armci_client_send_complete: failed code=",context->done);
}


/*\ direct send to server 
\*/
void armci_client_direct_send(int p, void *src_buf, void *dst_buf, int len)
{
    int s           = armci_clus_id(p);
    int serv_mpi_id = armci_clus_info[s].master;

    armci_gm_client_context->done = ARMCI_GM_SENDING;
    gm_directed_send_with_callback(proc_gm->port, src_buf,
               (gm_remote_ptr_t)(gm_up_t)dst_buf, len, GM_LOW_PRIORITY,
                proc_gm->node_map[serv_mpi_id], proc_gm->port_map[s], 
                armci_client_send_callback_direct, armci_gm_client_context);

    /* blocking: wait until send is done by calling the callback */
    armci_client_send_complete(armci_gm_client_context);
}


/* computing process start initial communication with all the servers */
void armci_client_connect_to_servers()
{
    int server_mpi_id, size, i;
 
    /* make sure that server thread is ready */
    if(armci_me == armci_master) while(!armci_gm_server_ready) usleep(10);
    armci_msg_barrier();

    /* make initial conection to the server, not the server in this node */
    for(i=0; i<armci_nclus; i++) {
        if(armci_clus_me != i) {
            server_mpi_id = armci_clus_info[i].master;
            ((long *)(MessageSndBuffer))[0] = ARMCI_GM_CLEAR;
            ((long *)(MessageSndBuffer))[1] = armci_me;
            ((long *)(MessageSndBuffer))[2] = (long)MessageSndBuffer;;
            ((long *)(MessageSndBuffer))[3] = (long)(proc_gm->ack);
            ((long *)(MessageSndBuffer))[4] = ARMCI_GM_CLEAR;

            /* wait til the last sending done, either successful or failed */
            while(armci_gm_client_context->done == ARMCI_GM_SENDING);

            size = gm_min_size_for_length(3*sizeof(long));
            
            armci_gm_client_context->done = ARMCI_GM_SENDING;
            gm_send_with_callback(proc_gm->port,
                    MessageSndBuffer+sizeof(long), size, 3*sizeof(long),
                    GM_LOW_PRIORITY, proc_gm->node_map[server_mpi_id],
                    proc_gm->port_map[i], armci_client_send_callback_direct,armci_gm_client_context);

            /* blocking: wait til the send is done by calling the callback */
            armci_client_send_complete(armci_gm_client_context);

            if(DEBUG_INIT_) fprintf(stderr,"%d:sent 1 msg to server %d at %p\n",
                                    armci_me, server_mpi_id, MessageSndBuffer);

            /* wait til the serv_ack_ptr has been updated */
            armci_wait_long_flag_updated_clear((long *)MessageSndBuffer, ARMCI_GM_COMPLETE);
            armci_wait_long_flag_updated_clear((long *)MessageSndBuffer+4, ARMCI_GM_COMPLETE);
            
            proc_gm->serv_ack_ptr[i] = (long*)((long *)MessageSndBuffer)[1];
            
            /* send back the ack to server */
            ((long *)MessageSndBuffer)[0] = ARMCI_GM_ACK;
            armci_gm_client_context->done = ARMCI_GM_SENDING;

            if(DEBUG_INIT_) {
                printf("%d:rcvd 1 msg from server %d\n",armci_me,server_mpi_id);
                printf("%d: sending back ack to server %d at %p\n",
                        armci_me, server_mpi_id, proc_gm->serv_ack_ptr[i]);
                fflush(stdout);
            }
            
            armci_client_direct_send(server_mpi_id, MessageSndBuffer,
                                     proc_gm->serv_ack_ptr[i], sizeof(long));

            if(DEBUG_INIT_) fprintf(stderr, "%d: connected to server %d\n",
                                    armci_me, server_mpi_id);

        }
    }

    armci_init_buf_alloc(SND_BUFLEN,MessageSndBuffer);
}


/*\ wait until flag is updated indicated that bypass transfer is done
\*/
void armci_wait_for_data_bypass()
{
   armci_wait_long_flag_updated_clear((long *)(proc_gm->ack), ARMCI_GM_COMPLETE);
}

void armci_clear_ack(int proc)
{
 serv_gm->ack[proc]=ARMCI_GM_CLEAR;
}

/*\ wait until ack is set by client to certain value: on failure return 1  
\*/
int armci_wait_client_seq(int proc,int val)
{
long *buf;
long spin =0,res;

    if(proc <0 || proc >= armci_nproc)armci_die("armci_wait_pin_client:p",proc);
    buf = serv_gm->ack +proc;

    res = check_flag(buf);
    while(res < (long)val){
       for(spin=0; spin<__armci_wait_some; spin++)__armci_fake_work+=0.001;
       res = check_flag(buf);
    }
    __armci_fake_work =99.0;
    return 1;
}

/*\ wait until ack is set by client: on failure return 1  
\*/
int armci_wait_pin_client(int proc)
{
long status;

    if(proc <0 || proc >= armci_nproc)armci_die("armci_wait_pin_client:p",proc);

    status = armci_wait_long_flag_not_clear(serv_gm->ack +proc);
    serv_gm->ack[proc]=ARMCI_GM_CLEAR;
    if(status == ARMCI_GM_FAILED) return 1;
    return 0;
}

void armci_client_send_ack(int p, int success)
{
     int cluster = armci_clus_id(p);
     long *pflag = proc_gm->tmp;
     *pflag= (success)? success : ARMCI_GM_FAILED;
     armci_client_direct_send(p, pflag, proc_gm->serv_ack_ptr[cluster], 
                                                         sizeof(long));
}

void armci_client_send_ack_seq(int p, int success)
{
     int cluster = armci_clus_id(p);
     long *pflag = proc_gm->tmp;
     *pflag= (success)? ARMCI_GM_READY : ARMCI_GM_FAILED;
     armci_client_direct_send(p, pflag, proc_gm->serv_ack_ptr[cluster],
                                                         sizeof(long));
}

/*\ send request message to server and wait for completion
 *  assumption: the buffer is pinned and most probably is MessageSndBuffer
\*/
int armci_send_req_msg(int proc, void *vbuf, int len)
{
    char *buf     = (char*)vbuf;
    int size      = gm_min_size_for_length(len);
    int s         = armci_clus_id(proc);
    int serv_mpi_id = armci_clus_info[s].master;
    request_header_t *msginfo = (request_header_t *)vbuf;
    armci_gm_context_t *context = ((armci_gm_context_t *)buf)-1;

    /* set the message tag */
    msginfo->tag.data_ptr = buf + sizeof(request_header_t) - sizeof(long);
    msginfo->tag.ack = ARMCI_GM_CLEAR;

    context->done = ARMCI_GM_SENDING;
    gm_send_with_callback(proc_gm->port, buf, size, len, GM_LOW_PRIORITY,
                          proc_gm->node_map[serv_mpi_id], proc_gm->port_map[s], 
                          armci_client_send_callback, context);

#ifndef MULTIPLE_SEND_BUFS
/*     armci_client_send_complete(context);
*/
#endif

    return 0;
}


/* check if data is available in the buffer
 * assume the buf is pinned and is inside MessageSndBuffer
 * format buf = hdr ack + data + tail ack
 */
char *armci_ReadFromDirect(int proc, request_header_t * msginfo, int len)
{
    char *buf = (char*) msginfo;
    long *tail;

/*
     printf("%d: reading direct ptr=%x\n", armci_me,&(msginfo->tag.ack));fflush(stdout);
*/

    /* check the header ack */
    armci_wait_long_flag_updated_clear(&(msginfo->tag.ack), ARMCI_GM_COMPLETE);

    /* reset header ack */
    msginfo->tag.ack = ARMCI_GM_CLEAR;

    buf += sizeof(request_header_t);
    
    /* check the tail ack */
    tail = (long*)(buf+len);
    ALIGN_PTR_LONG(long, tail);

    armci_wait_long_flag_updated_clear(tail, ARMCI_GM_COMPLETE);

    /* reset tail ack */
    *tail = ARMCI_GM_CLEAR;

    return(buf);
}


/*********************************************************************
                           SERVER SIDE                            
 *********************************************************************/

/* preallocate required memory at the startup */
int armci_gm_serv_mem_alloc()
{
    int i;
    int armci_gm_max_msg_size = gm_min_size_for_length(MSG_BUFLEN);
    int short_msg_size = gm_min_size_for_length(SHORT_MSGLEN);
    
    /********************** get local unregistered memory *******************/
    /* allocate dma buffer for low priority */
    serv_gm->dma_buf = (void **)malloc(armci_gm_max_msg_size * sizeof(void *));
    if(!serv_gm->dma_buf)return FALSE;
    
    serv_gm->dma_buf_short = (void **)malloc(short_msg_size * sizeof(void *));
    if(!serv_gm->dma_buf_short)return FALSE;

    /* allocate buf for keeping the pointers of client MessageSndbuffer */
    serv_gm->proc_buf_ptr = (long *)calloc(armci_nproc, sizeof(long));
    if(!serv_gm->proc_buf_ptr) return FALSE;

    /********************** get registered memory **************************/
    for(i=ARMCI_GM_MIN_MESG_SIZE; i<=armci_gm_max_msg_size; i++) {
        if((armci_me==0) && DEBUG_){
           printf("size %d len=%ld\n",i,gm_max_length_for_size(i));
        }
        serv_gm->dma_buf[i] = (char *)gm_dma_malloc(serv_gm->rcv_port,
                                        gm_max_length_for_size(i));
        if(!serv_gm->dma_buf[i]) return FALSE;
    }

    for(i=ARMCI_GM_MIN_MESG_SIZE; i<=short_msg_size; i++) {
        serv_gm->dma_buf_short[i] = (char *)gm_dma_malloc(serv_gm->rcv_port,
                                        gm_max_length_for_size(i));
        if(!serv_gm->dma_buf_short[i]) return FALSE;
    }

    /* allocate ack buffer for each client process */
    serv_gm->ack = (long *)gm_dma_malloc(serv_gm->rcv_port,
                                             armci_nproc*sizeof(long));
    if(serv_gm->ack == 0) return FALSE;
    for(i=0; i<armci_nproc; i++)serv_gm->ack[i]=ARMCI_GM_CLEAR;

    serv_gm->direct_ack = (long*)gm_dma_malloc(serv_gm->snd_port, sizeof(long));
    if(serv_gm->direct_ack == 0) return FALSE;
    *serv_gm->ack = ARMCI_GM_CLEAR;
    
    /* allocate recv buffer */
    MessageRcvBuffer = (char *)gm_dma_malloc(serv_gm->snd_port, MSG_BUFLEN);
    if(MessageRcvBuffer == 0) return FALSE;

    serv_gm->proc_ack_ptr = (long *)gm_dma_malloc(serv_gm->snd_port,
                                                  armci_nproc*sizeof(long));
    if(serv_gm->proc_ack_ptr == 0) return FALSE;

    return TRUE;
}


/* deallocate the preallocated memory used by gm */
int armci_gm_serv_mem_free()
{
    int i;
    int armci_gm_max_msg_size = gm_min_size_for_length(MSG_BUFLEN);
    int short_msg_size = gm_min_size_for_length(SHORT_MSGLEN);

    free(serv_gm->proc_buf_ptr);
    free(serv_gm->dma_buf);
    free(serv_gm->dma_buf_short);

    gm_dma_free(serv_gm->snd_port, serv_gm->proc_ack_ptr);
    gm_dma_free(serv_gm->rcv_port, serv_gm->ack);
    gm_dma_free(serv_gm->snd_port, serv_gm->direct_ack);
    
    for(i=ARMCI_GM_MIN_MESG_SIZE; i<=armci_gm_max_msg_size; i++) {
        gm_dma_free(serv_gm->rcv_port, serv_gm->dma_buf[i]);
    }
    
    for(i=ARMCI_GM_MIN_MESG_SIZE; i<=short_msg_size; i++) {
        gm_dma_free(serv_gm->rcv_port, serv_gm->dma_buf_short[i]);
    }

    gm_dma_free(serv_gm->snd_port, MessageRcvBuffer);

    return TRUE;
}

/* server side call back func */
void armci_serv_callback(struct gm_port *port, void *context, 
			 gm_status_t status)
{
    if(status==GM_SUCCESS) ((armci_gm_context_t*)context)->done = ARMCI_GM_SENT;
    else ((armci_gm_context_t *)context)->done = ARMCI_GM_FAILED;
}

/* server side call back func */
void armci_serv_callback_nonblocking(struct gm_port *port, void *context, 
                                     gm_status_t status)
{
    if(status == GM_SUCCESS)
        serv_gm->complete_msg_ct++;
    else
        armci_die(" armci_serv_callback_nonblocking: send failed", 0);
}

/* server trigers gm_unknown, so that callback func can be executed */
int armci_serv_send_complete()
{
    gm_recv_event_t *event;

    while(armci_gm_serv_context->done == ARMCI_GM_SENDING) {
        event = gm_blocking_receive_no_spin(serv_gm->snd_port);
        gm_unknown(serv_gm->snd_port, event);
    }
    
    return(armci_gm_serv_context->done);
}


/*\ block until the number of outstanding nonblocking messages <= specified val
\*/
void armci_serv_send_nonblocking_complete(int max_outstanding)
{
    gm_recv_event_t *event;

    while((serv_gm->pending_msg_ct - serv_gm->complete_msg_ct) >
          max_outstanding) {
        event = gm_blocking_receive_no_spin(serv_gm->snd_port);
        gm_unknown(serv_gm->snd_port, event);
    }
}


int armci_serv_ack_complete()
{
    gm_recv_event_t *event;

    while(armci_gm_serv_ack_context->done == ARMCI_GM_SENDING) {
        event = gm_blocking_receive_no_spin(serv_gm->snd_port);
        gm_unknown(serv_gm->snd_port, event);
    }
    
    return(armci_gm_serv_ack_context->done);
}



static int port_id=1,board_id=0;
static int armci_get_free_port(int ports, int boards, struct gm_port **p)
{

   for(; board_id<boards; board_id++){
      for(; port_id<ports; port_id++){
         if(GM_SUCCESS == gm_open(p,board_id,port_id,"try",GM_API_VERSION_1_1))
                          return port_id;
      }
      port_id=0; /*start from 0 on next board */
   }
   return(-1);     
}
          
        
          

/* initialization of server thread */
int armci_gm_server_init() 
{
    int i;
    int status;
    
    unsigned long size_mask;
    unsigned int min_mesg_size, min_mesg_length;
    unsigned int max_mesg_size, max_mesg_length;
 
    /* allocate gm data structure for server */
    serv_gm->node_map = (int *)malloc(armci_nproc * sizeof(int));
    serv_gm->port_map = (int *)calloc(armci_nproc, sizeof(int));
    if((serv_gm->node_map == NULL) || (serv_gm->port_map == NULL)) {
        fprintf(stderr,"%d: Error allocate server data structure.\n",armci_me);
        return FALSE;
    }

    if(DEBUG_) fprintf(stdout,
                 "%d(server):opening gm port %d(rcv)dev=%d and %d(snd)dev=%d\n",
                     armci_me, ARMCI_GM_SERVER_RCV_PORT,ARMCI_GM_SERVER_RCV_DEV,
                             ARMCI_GM_SERVER_SND_PORT, ARMCI_GM_SERVER_SND_DEV);

    serv_gm->rcv_port = NULL; serv_gm->snd_port = NULL;

#ifdef STATIC_PORTS
    /* opening gm port for sending data back to clients */
    status = gm_open(&(serv_gm->snd_port), ARMCI_GM_SERVER_SND_DEV,
                     ARMCI_GM_SERVER_SND_PORT, "gm_pt", GM_API_VERSION_1_1);
    if(status != GM_SUCCESS) 
                 armci_die("did not get rcv port",ARMCI_GM_SERVER_SND_DEV);

    /* opening gm rcv port for requests */
    serv_gm->port_id  = ARMCI_GM_SERVER_RCV_PORT; 
    status = gm_open(&(serv_gm->rcv_port), ARMCI_GM_SERVER_RCV_DEV,
                     ARMCI_GM_SERVER_RCV_PORT, "gm_pt", GM_API_VERSION_1_1);
    if(status != GM_SUCCESS) 
                 armci_die("did not get rcv port",ARMCI_GM_SERVER_RCV_DEV);
#else
    status= armci_get_free_port(12, 2, &(serv_gm->snd_port));  
    if(DEBUG_INIT_)printf("%d server got snd port %d \n",armci_me,status);
    if(status!=-1) status= armci_get_free_port(12, 2, &(serv_gm->rcv_port));  
    if(DEBUG_INIT_)printf("%d server got rcv port %d \n",armci_me,status);
    serv_gm->port_id = status;
#endif

    /* get my node id */
    status = gm_get_node_id(serv_gm->rcv_port, &(serv_gm->node_id));
    if(status != GM_SUCCESS)armci_die("Could not get GM node id",0);
    if(DEBUG_)printf("%d(server): node id is %d\n", armci_me, serv_gm->node_id);

#if 0
    for(i=0; i<armci_nproc; i++)
        serv_gm->node_map[i] = proc_gm->node_map[i];
#endif

    /* allow direct send */
    status = gm_allow_remote_memory_access(serv_gm->rcv_port);
    if(status != GM_SUCCESS) {
        fprintf(stderr,"%d(server): could not enable direct sends\n", armci_me);
        return FALSE;
    }

    /* enable put i.e., dma send */
    status = gm_allow_remote_memory_access(serv_gm->snd_port);
    if(status != GM_SUCCESS) {
        fprintf(stderr,"%d(server): could not enable direct sends\n", armci_me);
        return FALSE;
    }
    
    /* memory preallocation for server */
    if(!armci_gm_serv_mem_alloc()) {
        fprintf(stderr,"%d(server): failed allocating memory\n", armci_me);
        return FALSE;
    }

    /* set message size on server */
    min_mesg_size = ARMCI_GM_MIN_MESG_SIZE;
    max_mesg_size = gm_min_size_for_length(MSG_BUFLEN);
    min_mesg_length = gm_max_length_for_size(min_mesg_size);
    max_mesg_length = MSG_BUFLEN;
    
    if(DEBUG_INIT_) {
        printf("%d: SERVER min_mesg_size = %d, max_mesg_size = %d\n",
               armci_me, min_mesg_size, max_mesg_size);
        printf("%d: SERVER min_mesg_length = %d, max_mesg_length = %d\n",
               armci_me, min_mesg_length, max_mesg_length);
         fflush(stdout);
    }
    
    /* accept only the smallest size messages */
    size_mask = (2 << max_mesg_size) - 1;
    status = gm_set_acceptable_sizes(serv_gm->rcv_port, GM_LOW_PRIORITY,
                                     size_mask);
    if (status != GM_SUCCESS) {
        fprintf(stderr, "%d(server): error setting acceptable sizes", armci_me);
        return FALSE;
    }

    /* provide the buffers initially create a size mask and set */
    for(i=min_mesg_size; i<=max_mesg_size; i++)
        gm_provide_receive_buffer_with_tag(serv_gm->rcv_port,
               serv_gm->dma_buf[i], i, GM_LOW_PRIORITY, TAG_DFLT);

    /* provide the extra set of buffers for short messages */
    for(i=min_mesg_size; i<=gm_min_size_for_length(SHORT_MSGLEN); i++)
        gm_provide_receive_buffer_with_tag(serv_gm->rcv_port,
               serv_gm->dma_buf_short[i], i, GM_LOW_PRIORITY, TAG_SHORT);

    if(DEBUG_ && armci_me==0)printf("provided (%d,%d) buffers, rcv tokens=%d\n",
           max_mesg_size,
           gm_min_size_for_length(SHORT_MSGLEN),
           gm_num_receive_tokens(serv_gm->rcv_port));

    serv_gm->pending_msg_ct = 0; serv_gm->complete_msg_ct = 0; 
    
    return TRUE;
}


/* server start communication with all the compute processes */
void armci_server_initial_connection()
{
    gm_recv_event_t *event;
    unsigned int size, length, tag;
    char *buf;
    int rid;
    int procs_in_clus = armci_clus_info[armci_clus_me].nslave;
    int iexit;

    /* notify client thread that we are ready to take requests */
    armci_gm_server_ready = 1;

    /* receive the initial connection from all computing processes,
     * except those from the same node
     */
    iexit = armci_nproc - procs_in_clus;
    while(iexit) {
        event = gm_blocking_receive_no_spin(serv_gm->rcv_port);
        
        switch (event->recv.type) {
          case GM_RECV_EVENT:
          case GM_PEER_RECV_EVENT:
              iexit--;

              size = gm_ntohc(event->recv.size);
              tag  = gm_ntohc(event->recv.tag);
              length = gm_ntohl(event->recv.length);
              buf = gm_ntohp(event->recv.buffer);              

              /* receiving the remote mpi id and addr of serv_ack_ptr */
              rid = (int)(((long *)buf)[0]);

              if(DEBUG_INIT_)printf("%d(srv):init msg from %d size=%d len=%d\n",
                                     armci_me, rid, size, length);
              
              serv_gm->proc_buf_ptr[rid] = ((long *)buf)[1];
              serv_gm->proc_ack_ptr[rid] = ((long *)buf)[2];
              serv_gm->port_map[rid] = gm_ntohc(event->recv.sender_port_id);

              /* send server ack buffer and MessageRcvBuffer ptr to client */
              serv_gm->ack[rid] = ARMCI_GM_CLEAR;
              ((long *)MessageRcvBuffer)[0] = ARMCI_GM_COMPLETE;
              ((long *)MessageRcvBuffer)[1] = (long)(&(serv_gm->ack[rid]));
              ((long *)MessageRcvBuffer)[2] = (long)(MessageRcvBuffer);
              ((long *)MessageRcvBuffer)[4] = ARMCI_GM_COMPLETE;
              
              armci_gm_serv_context->done = ARMCI_GM_SENDING;
              gm_directed_send_with_callback(serv_gm->snd_port,MessageRcvBuffer,
                   (gm_remote_ptr_t)(gm_up_t)(serv_gm->proc_buf_ptr[rid]),
                   5*sizeof(long), GM_LOW_PRIORITY, serv_gm->node_map[rid],
                   serv_gm->port_map[rid], armci_serv_callback,
                   armci_gm_serv_context);

              /* blocking: wait til the send is complete */
              if(armci_serv_send_complete() == ARMCI_GM_FAILED)
                  armci_die(" Init: server could not send msg to client", rid);

              if(DEBUG_INIT_){
                 printf("%d(serv): sent msg to %d (@%ld),expecting ack at %p\n",
                          armci_me, rid, serv_gm->proc_buf_ptr[rid],
                          &(serv_gm->ack[rid])); fflush(stdout);
              }

              /* wait for the client send back the ack */
              armci_wait_long_flag_updated_clear(&(serv_gm->ack[rid]), ARMCI_GM_ACK);
              serv_gm->ack[rid] = ARMCI_GM_CLEAR;
              
              if(DEBUG_INIT_) {
                printf("%d(server): connected to client %d\n", armci_me, rid);
                printf("%d(server): expecting %d more cons\n", armci_me, iexit);
                fflush(stdout);
              }
              
              gm_provide_receive_buffer_with_tag(serv_gm->rcv_port, buf,
                                                 size, GM_LOW_PRIORITY, tag);
              break;
          default:
              gm_unknown(serv_gm->rcv_port, event);
              break;
        }
    }
}


/* direct send from server to client */
void armci_server_direct_send(int dst, char *src_buf, char *dst_buf, int len,
                              int type)
{
    if(type == ARMCI_GM_BLOCKING) {
        armci_gm_serv_context->done = ARMCI_GM_SENDING;

        gm_directed_send_with_callback(serv_gm->snd_port, src_buf,
               (gm_remote_ptr_t)(gm_up_t)(dst_buf),
               len, GM_LOW_PRIORITY, serv_gm->node_map[dst],
               serv_gm->port_map[dst], armci_serv_callback,
               armci_gm_serv_context);
    }
    else if(type == ARMCI_GM_NONBLOCKING) {
        gm_directed_send_with_callback(serv_gm->snd_port, src_buf,
               (gm_remote_ptr_t)(gm_up_t)(dst_buf),
               len, GM_LOW_PRIORITY, serv_gm->node_map[dst],
               serv_gm->port_map[dst], armci_serv_callback_nonblocking,
               armci_gm_serv_context);
        serv_gm->pending_msg_ct++;
    }
    else {
        gm_directed_send(serv_gm->snd_port, src_buf,
               (gm_remote_ptr_t)(gm_up_t)(dst_buf), len,
               GM_LOW_PRIORITY, serv_gm->node_map[dst], serv_gm->port_map[dst]);
    }
}


/* server direct send to the client
 * assume buf is pinned and using MessageRcvBuffer
 * MessageRcvBuffer: .... + hdr ack + data + tail ack
 *                                         ^
 *                                         buf (= len)
 */
void armci_WriteToDirect(int dst, request_header_t *msginfo, void *buffer)
{
    char *buf = (char*)buffer; 
    char *ptr = buf - sizeof(long);
    long *tail, *utail;
    int bytes;

    /* adjust the dst pointer */
    void *dst_addr = msginfo->tag.data_ptr;
    
    /* set head ack */
    *(long *)ptr = ARMCI_GM_COMPLETE;
   
    /* set tail ack, make sure it is alligned */
    utail= tail = (long*)(buf + msginfo->datalen);
    ALIGN_PTR_LONG(long, tail);
    *tail = ARMCI_GM_COMPLETE;
    bytes = (char*)tail - (char*)utail; /* add allignment */
    bytes+= 2*sizeof(long);

    if(armci_serv_send_complete() == ARMCI_GM_FAILED)
        armci_die(" server last send failed", dst);
    armci_gm_serv_context->done = ARMCI_GM_SENDING;
    

/*    printf("%d: writing direct ptr=%x\n", armci_me,dst_addr);fflush(stdout);
*/
    gm_directed_send_with_callback(serv_gm->snd_port, ptr,
                     (gm_remote_ptr_t)(gm_up_t)(dst_addr),
                     msginfo->datalen+bytes, GM_LOW_PRIORITY,
                     serv_gm->node_map[dst], serv_gm->port_map[dst],
                     armci_serv_callback, armci_gm_serv_context);
}


/* NOT USED server inform the client the send is complete */
void armci_InformClient(int dst, void *buf, long flag)
{
    *(long *)buf = flag;

    armci_gm_serv_ack_context->done = ARMCI_GM_SENDING;
    gm_directed_send_with_callback(serv_gm->snd_port, buf,
       (gm_remote_ptr_t)(gm_up_t)(serv_gm->proc_ack_ptr[dst]),
        sizeof(long), GM_LOW_PRIORITY, serv_gm->node_map[dst],
        serv_gm->port_map[dst], armci_serv_callback, armci_gm_serv_ack_context);
    
    /* blocking: wait til the send is done by calling the callback */
    if(armci_serv_ack_complete() == ARMCI_GM_FAILED)
        armci_die(" failed sending data to client", dst);
}


/*\ sends notification to client that data in direct send was transfered/put
 *  into the client buffer
\*/
void armci_server_send_ack(int client)
{
    long *p_ack = serv_gm->direct_ack;

    armci_gm_serv_ack_context->done = ARMCI_GM_SENDING;
    *p_ack = ARMCI_GM_COMPLETE;

    gm_directed_send_with_callback(serv_gm->snd_port, p_ack,
        (gm_remote_ptr_t)(gm_up_t)(serv_gm->proc_ack_ptr[client]), sizeof(long),
        GM_LOW_PRIORITY, serv_gm->node_map[client], serv_gm->port_map[client], 
        armci_serv_callback, armci_gm_serv_ack_context);

    if(armci_serv_ack_complete() == ARMCI_GM_FAILED)
        armci_die(" failed sending data to client", client);
}


/* the main data server loop: accepting events and pass it to data server
 * code to be porcessed -- this is handler for GM specific requests
 */
void armci_call_data_server()
{
    int iexit = FALSE;
    unsigned int size, length, tag;
    char *buf;
    gm_recv_event_t *event; /* gm event */
    
    if(DEBUG_){
        fprintf(stdout, "%d(server): waiting for request\n",armci_me);
        fflush(stdout);
    }

    /* server main loop; wait for and service requests until QUIT requested */
    while(!iexit) {        
        event = gm_blocking_receive_no_spin(serv_gm->rcv_port);

        if(DEBUG_INIT_) { fprintf(stdout, "%d(server): receive event type %d\n",
                    armci_me, event->recv.type); fflush(stdout);     
        }
        
        switch(event->recv.type) {
          case GM_RECV_EVENT:
          case GM_PEER_RECV_EVENT:
              size = gm_ntohc(event->recv.size);
              tag = gm_ntohc(event->recv.tag);
              length = gm_ntohl(event->recv.length);
              buf = (char *)gm_ntohp(event->recv.buffer);

              armci_data_server(buf);
              
              gm_provide_receive_buffer_with_tag(serv_gm->rcv_port, buf,
                                                 size, GM_LOW_PRIORITY, tag);
    
              break;
          default:
              gm_unknown(serv_gm->rcv_port, event);
              break;
        }
    }
    
    if(DEBUG_) {printf("%d(server): done! closing\n",armci_me); fflush(stdout);}
}


/* cleanup of GM: applies to either server or client */
void armci_transport_cleanup()
{    
    /* deallocate the gm data structure */
    if(SERVER_CONTEXT) {
#if 0        
        if(!armci_gm_serv_mem_free()) 
            armci_die("server memory deallocate memory failed", armci_me);
#endif            
        gm_close(serv_gm->rcv_port);
        gm_close(serv_gm->snd_port);
        free(serv_gm->node_map); free(serv_gm->port_map);
    }
    else {
#if 0
        if(!armci_gm_proc_mem_free()) 
            armci_die("computing process  memory deallocate memory failed",0);
#endif   
        free(proc_gm->node_map);
    }
}
 

void armci_init_connections()
{
    if(armci_me == armci_master) {
        if(!armci_gm_server_init())
            armci_die("GM:server connection initialization failed", 0L);
    }
    if(!armci_gm_client_init())
        armci_die("GM:client connection initialization failed", 0L);

}

#define NBUFS 4
#define NBUFS_MAX 8
static void *bufarr[NBUFS_MAX]={NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL};
static int nbuf_cur=0;

void armci_gm_freebuf_tag(int tag)
{
armci_gm_context_t *context;

  if( tag<0 || tag>=NBUFS) armci_die("armci_gm_freebuf_tag: bad tag",tag);
  if(bufarr[tag]){
     context = (armci_gm_context_t *)bufarr[tag];
     if(tag != context->tag)armci_die("armci_gm_freebuf_tag: tag mismatch",tag);
     armci_client_send_complete(context);
     armci_buf_free(bufarr[tag]);
     bufarr[tag]=(void*)0;
  }else{
      armci_die(" armci_gm_freebuf_tag: NULL ptr",tag );
  }

  if(DEBUG_)    {int i;
      printf("%d:freed tag=%d op=%d bufarr[",armci_me,tag,
             ((request_header_t*)(context+1))->operation);
      for(i=0;i<NBUFS;i++)printf("%p ",bufarr[i]);
      printf("]\n"); fflush(stdout);
  }
}
 
char* armci_gm_getbuf(size_t size)
{
extern char *armci_buf_alloc(size_t bytes);
armci_gm_context_t *context;
char *ptr;
int i, bytes = size + sizeof(armci_gm_context_t);
  
   if(DEBUG_){
      printf("%d: getting buffer size %d\n",armci_me,bytes); fflush(stdout);}

   /* get buffer of specified size but also add context */     
   ptr = armci_buf_alloc(bytes);

   if(!ptr){ /* no memory available wait until all pending requests complete */
      nbuf_cur = (nbuf_cur+1)%NBUFS;
      for(i=0;i<NBUFS;i++){
         if(bufarr[nbuf_cur])
            armci_gm_freebuf_tag(((armci_gm_context_t*)bufarr[nbuf_cur])->tag);
         nbuf_cur = (nbuf_cur+1)%NBUFS;
      }
      nbuf_cur =0; /* start from beginning next time*/

      ptr = armci_buf_alloc(bytes );
      if(!ptr)armci_die("armci_gm_getbuf: did not get memory ",(int)size);

      if(DEBUG_){
         printf("%d: armci_gm_getbuf: completed all reqs\n",armci_me);
         fflush(stdout); }

   }else {

      /* search for first free slot */
      for(i=0;i<NBUFS;i++)
         if(bufarr[nbuf_cur]) {nbuf_cur++; nbuf_cur %=NBUFS; }
         else break;

      if(bufarr[nbuf_cur]){ /* no empty found -> free current one */
            armci_gm_freebuf_tag(((armci_gm_context_t*)bufarr[nbuf_cur])->tag);
      }
   }

   bufarr[nbuf_cur]=ptr;

   /* initialize context properly */
   context = (armci_gm_context_t*)bufarr[nbuf_cur];
   context->tag= nbuf_cur;
   context->done = ARMCI_GM_CLEAR;

   nbuf_cur = (nbuf_cur+1)%NBUFS;
   
   return (char*)(context+1);
}

void armci_gm_freebuf(void *ptr)
{
armci_gm_context_t *context = ((armci_gm_context_t *)ptr)-1;
      armci_gm_freebuf_tag(context->tag);
}


static void armci_pipe_advance_buf(int strides, int count[], 
                                   char **buf, long **ack, int *bytes )
{
int i, extra;

     for(i=0, *bytes=1; i<=strides; i++)*bytes*=count[i]; /*compute chunk size*/
     
     /* allign receive buffer on 64-byte boundary */
     extra = ALIGN64ADD((*buf));
     (*buf) +=extra;                  /*** this where the data is *******/
     if(DEBUG2){ printf("%d: pipe advancing %d %d\n",armci_me, *bytes,extra); fflush(stdout);
     }
     *ack = (long*)((*buf) + *bytes); /*** this is where ACK should be ***/
}


/*\ prepost buffers for receiving data from server (pipeline)
\*/
void armcill_pipe_post_bufs(void *ptr, int stride_arr[], int count[],
                            int strides, void* argvoid)
{
int bytes;
buf_arg_t *arg = (buf_arg_t*)argvoid;
long *ack;

     armci_pipe_advance_buf(strides, count, &arg->buf_posted, &ack, &bytes);

     if(DEBUG2){ printf("%d: posting %d pipe receive %d b=%d (%p,%p) ack=%p\n",
          armci_me,arg->count,arg->proc,bytes,arg->buf, arg->buf_posted,ack);
          fflush(stdout);
     }
     *ack = 0L;                      /*** clear ACK flag ***/

     arg->buf_posted += bytes+sizeof(long);/* advance pointer for next chunk */
     arg->count++;
}


void armcill_pipe_extract_data(void *ptr, int stride_arr[], int count[],
                               int strides, void* argvoid)
{
int bytes;
long *ack;
buf_arg_t *arg = (buf_arg_t*)argvoid;

     armci_pipe_advance_buf(strides, count, &arg->buf_posted, &ack, &bytes);

     if(DEBUG2){ printf("%d:extracting pipe  data from %d %d b=%d %p ack=%p\n",
            armci_me,arg->proc,arg->count,bytes,arg->buf,ack); fflush(stdout);
     }

     armci_wait_long_flag_updated(ack, 1); /********* wait for data ********/

     /* copy data to the user buffer identified by ptr */
     armci_read_strided(ptr, strides, stride_arr, count, arg->buf_posted);
     if(DEBUG2 ){printf("%d(c):extracting: data %p first=%f\n",armci_me,
                arg->buf_posted,((double*)arg->buf_posted)[0]); 
                fflush(stdout);
     }

     arg->buf_posted += bytes+sizeof(long);/* advance pointer for next chunk */
     arg->count++;
}


void armcill_pipe_send_chunk(void *data, int stride_arr[], int count[],
                             int strides, void* argvoid)
{
int bytes, bytes_ack;
buf_arg_t *arg = (buf_arg_t*)argvoid;
long *ack;

     armci_pipe_advance_buf(strides, count, &arg->buf_posted, &ack, &bytes);
     armci_pipe_advance_buf(strides, count, &arg->buf, &ack, &bytes);
     bytes_ack = bytes+sizeof(long);

     if(DEBUG2){ printf("%d:SENDING pipe data %d to %d %p b=%d %p)\n",armci_me,
                 arg->count, arg->proc, arg->buf, bytes, ack); fflush(stdout);
     }

     /* copy data to buffer */
     armci_write_strided(data, strides, stride_arr, count, arg->buf);
     *ack=1;

     armci_server_direct_send(arg->proc, arg->buf, arg->buf_posted, bytes_ack,
                              ARMCI_GM_NONBLOCKING);

     if(DEBUG2){ printf("%d:  out of send %d bytes=%d first=%f\n",armci_me,
               arg->count,bytes,((double*)arg->buf)[0]); fflush(stdout);
     }

#if 0
     /* at any time, we will allow fixed numer of outstanding sends */
     armci_serv_send_nonblocking_complete(8);
#endif

     arg->buf += bytes+sizeof(long);        /* advance pointer for next chunk */
     arg->buf_posted += bytes+sizeof(long); /* advance pointer for next chunk */
     arg->count++;
}

void armci_pipe_send_req(int proc, void *vbuf, int len)
{
 armci_send_req_msg(proc, vbuf, len);
}
