/* $Id: memlock.c,v 1.10 2000-06-14 18:38:41 edo Exp $ */
#include "armcip.h"
#include "locks.h"
#include "copy.h"
#include "memlock.h"
#include <stdio.h>

#define DEBUG_ 0
#define INVALID_VAL -9999999

#ifdef DATA_SERVER
#  define CORRECT_PTR 
#endif
size_t armci_mem_offset=0;

/* We start by  using table: assign address of local variable set to 1
 * On shmem systems, this addres is overwritten by a shared memory location
 * when memlock array is allocated in armci_init 
 * Therefore, any process within shmem node can reset armci_use_memlock_table
 * to "not used" when offset changes. Since the variable is in shmem, everybody
 * on that SMP node will see the change and use the same locking functions
 */ 
int init_use_memlock_table=1;
int *armci_use_memlock_table=&init_use_memlock_table;

static int locked_slot=INVALID_VAL;

volatile double armci_dummy_work=0.;
void **memlock_table_array;

/* constants for cache line alignment */
#ifdef SOLARIS
#  define CALGN 32
#  define LOG_CALGN 5
#else
#  define CALGN 64
#  define LOG_CALGN 6
#endif

#define ALIGN_ADDRESS(x) (char*)((((unsigned long)x) >> LOG_CALGN) << LOG_CALGN) 
#ifdef CRAY_T3E
#pragma _CRI cache_align table
#endif
static memlock_t table[MAX_SLOTS];



/*\ simple locking scheme that ignores addresses
\*/
void armci_lockmem_(void *pstart, void *pend, int proc)
{
#ifdef QUADRICS
    int lock = proc;
#else
    int lock = proc-armci_master;
#endif

    NATIVE_LOCK(lock);
#   ifdef LAPI
    {
       extern int kevin_ok;
       kevin_ok=0;
    }
#   endif
}

void armci_unlockmem_(int proc)
{
#ifdef QUADRICS
    int lock = proc;
#else
    int lock = proc-armci_master;
#endif
    NATIVE_UNLOCK(lock);
#   ifdef LAPI
    {
       extern int kevin_ok;
       kevin_ok=1;
    }
#   endif
}



/*\ idle for a time proportional to factor 
\*/
void armci_waitsome(int factor)
{
int i=factor*100000;

   if(factor <= 1) armci_dummy_work =0.;
   if(factor < 1) return;
   while(--i){
      armci_dummy_work = armci_dummy_work + 1./(double)i;  
   }
}
   

/*\ acquire exclusive LOCK to MEMORY area <pstart,pend> owned by process "proc"
 *   . only one area can be locked at a time by the calling process
 *   . must unlock it with armci_unlockmem
\*/
void armci_lockmem(void *start, void *end, int proc)
{
     register void* pstart, *pend;
     register  int slot, avail=0;
     int turn=0, conflict=0;
     memlock_t *memlock_table;
     int lock;

#ifdef CORRECT_PTR
     if(! *armci_use_memlock_table){
       /* if offset invalid, use dumb locking scheme ignoring addresses */
       armci_lockmem_(start, end, proc); 
       return;
     }

     /* when processes are attached to a shmem region at different addresses,
      * addresses written to memlock table must be adjusted to the node master
      */
     if(armci_mem_offset){
        start = armci_mem_offset + (char*)start;
        end   = armci_mem_offset + (char*)end;
     }
#endif

     if(DEBUG_){
       printf("%d: calling armci_lockmem for %d range %lx -%lx\n",
              armci_me, proc, start,end);
       fflush(stdout);
     }
     memlock_table = (memlock_t*)memlock_table_array[proc];

     lock = proc%NUM_LOCKS;

#ifdef ALIGN_ADDRESS
     /* align address range on cache line boundary to avoid false sharing */
     pstart = ALIGN_ADDRESS(start);
     pend = CALGN -1 + ALIGN_ADDRESS(end);
#else
     pstart=start;
     pend =end;
#endif

     while(1){

        NATIVE_LOCK(lock);

        armci_get(memlock_table, table, sizeof(table), proc);
/*        armci_copy(memlock_table, table, sizeof(table));*/
        
        /* inspect the table */
        conflict = 0; avail =-1;
        for(slot = 0; slot < MAX_SLOTS; slot ++){

            /* nonzero starting address means the slot is occupied */ 
            if(table[slot].start == NULL){

              /* remember a free slot to store address range */
              avail = slot;  

            }else{
           
              /*check for conflict: overlap between stored and current range*/
              if(  (pstart >= table[slot].start && pstart <= table[slot].end)
                 || (pend >= table[slot].start && pend <= table[slot].end) ){

                  conflict = 1;
                  break;

              }
              /*
              printf("%d: locking %ld-%ld (%d) conflict\n",
                  armci_me,  */
            }
       }
        
       if(avail != -1 && !conflict)break;

       NATIVE_UNLOCK(lock);
       armci_waitsome( ++turn );

     }

     /* we got the memory lock: enter address into the table */
     table[avail].start = pstart;
     table[avail].end = pend;
     armci_put(table+avail,memlock_table+avail,sizeof(memlock_t),proc);

     FENCE_NODE(proc);

     NATIVE_UNLOCK(lock);
     locked_slot = avail;

}
        

/*\ release lock to the memory area locked by previous call to armci_lockemem
\*/
void armci_unlockmem(int proc)
{
     void *null[2] = {NULL,NULL};
     memlock_t *memlock_table;

#ifdef CORRECT_PTR
     if(!armci_use_memlock_table){
       /* if offset invalid, use dumb locking scheme ignoring addresses */
       armci_unlockmem_(proc);               
       return;
     }
#endif

#ifdef DEBUG
     if(locked_slot == INVALID_VAL) armci_die("armci_unlock: empty",0);
     if(locked_slot >= MAX_SLOTS || locked_slot <0) 
        armci_die("armci_unlock: corrupted slot?",locked_slot);
#endif

     memlock_table = (memlock_t*)memlock_table_array[proc];
     armci_put(null,&memlock_table[locked_slot].start,2*sizeof(void*),proc);

}



/*\ based on address for set by master, determine correction for
 *  memory addresses set in memlock table
 *  if the correction/offset ever changes stop using memlock table locking
\*/ 
void armci_set_mem_offset(void *ptr)
{
   extern size_t armci_mem_offset;
   size_t off;
   static int first_time=1;
   volatile void *ref_ptr;

   /* do not care if memlock not used */
   if(! *armci_use_memlock_table) return;

   if(!ptr) armci_die("armci_set_mem_offset : null ptr",0);
   ref_ptr = *(void**)ptr;
   off = (char*)ref_ptr - (char*)ptr;

   if(first_time){

      armci_mem_offset =off;
      first_time =0;
      if(DEBUG_){
        printf("%d memlock offset=%ld ref=%lx ptr=%lx\n",armci_me,
                  armci_mem_offset, ref_ptr, ptr); fflush(stdout);
      }

   }else{
      if(armci_mem_offset != off){
         *armci_use_memlock_table =0;
         printf("%d: WARNING:armci_set_mem_offset: offset changed %ld to %ld\n",
                 armci_me, armci_mem_offset, off); fflush(stdout);
      }
   }
}
