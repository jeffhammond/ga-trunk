/* $Id: shmem.c,v 1.28 2000-06-13 18:41:07 d3h325 Exp $ */
/* System V shared memory allocation and managment
 *
 * Interface:
 * ~~~~~~~~~
 *  char *Create_Shared_Region(long *idlist, long size, long *offset)
 *       . to be called by just one process. 
 *       . calls shmalloc, a modified by Robert Harrison version of malloc-like
 *         memory allocator from K&R. shmalloc in turn calls allocate() that
 *         does shmget() and shmat(). 
 *       . idlist might be just a pointer to integer or a true array in the
 *         MULTIPLE_REGIONS versions (calling routine has to take cere of it) 
 *  char *Attach_Shared_Region(long *idlist, long size, long offset)
 *       . called by any other process to attach to existing shmem region or
 *         if attached just calculate the address based on the offset
 *       . has to be called after shmem region was created
 *  void  Free_Shmem_Ptr(long id, long size, char* addr)
 *       . called ONLY by the process that created shmem region (id) to return
 *         pointer to shmalloc (shmem is not destroyed)
 *  void  Delete_All_Regions()
 *       . destroys all shared memory regions
 *       . can be called by any process assuming that all processes attached
 *         to alllocated shmem regions 
 *       . needs to by called by cleanup procedure(s)
 *
 * Jarek Nieplocha, 06.13.94
 * 
 */

#ifdef SYSV

 
#define DEBUG_ 0
#define DEBUG1 0

/* For debugging purposes at the beginning of the shared memory region
 * creator process can write a stamp which then is read by attaching processes
 * NOTE: on clusters we cannot use it anymore since ARMCI node master
 * uses it since Nov 99 to write the value of address it attached at
 * This feature is used in the ARMCI memlock table.
 */
#define STAMP 0

extern void armci_die();
extern int armci_me, armci_master;

#include "shmem.h"
#include "shmalloc.h"

#if defined(SUN) || defined(SOLARIS)
#define MULTIPLE_REGIONS
#endif

#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/param.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef   ALLOC_MUNMAP
#include <sys/mman.h>
#include <unistd.h>
static  size_t pagesize=0;
static  int logpagesize=0;
/* allow only that big shared memory segment (in MB) */
#define MAX_ALLOC_MUNMAP 128
#endif

#if defined(SUN)
  extern char *shmat();
#endif

#define SHM_UNIT (1024)


/* Need to determine the max shmem segment size. There are 2 alternatives:
 * 1. use predefined SHMMAX if available or set some reasonable values, or
 * 2. trial-and-error search for a max value (default)
 */
/* on SP cannot get return status from a child process needed to test SHMMAX */
#if defined(LAPI) || defined(IBM) || defined(GM)
#define NO_SHMMAX_SEARCH
#endif

#ifdef NO_SHMMAX_SEARCH
/* Limits for the largest shmem segment are in Kilobytes to avoid passing
 * Gigavalues to shmalloc
 * the limit for the KSR is lower than SHMMAX in sys/param.h because
 * shmat would fail -- SHMMAX cannot be trusted (a bug)
 */
#define _SHMMAX need to know the SHMMAX limit for this machine

#if defined(SUN)||defined(SOLARIS)
#  undef _SHMMAX
#  define _SHMMAX (1024)  /* memory in KB */
#elif defined(SGI64) || defined(AIX) || defined(CONVEX)
#  undef _SHMMAX
#  define _SHMMAX ((unsigned long)512*1024)
#elif defined(SGI) && !defined(SGI64)
#  undef _SHMMAX
#  define _SHMMAX ((unsigned long)128*1024)
#elif defined(KSR)
#  undef _SHMMAX
#  define _SHMMAX ((unsigned long)512*1024)
#elif defined(HPUX)
#  undef _SHMMAX
#  define _SHMMAX ((unsigned long)64*1024)
#elif defined(__FreeBSD__)
#  undef _SHMMAX
#  define _SHMMAX ((unsigned long)3*1024)
#elif defined(LINUX) 
#  if !defined(SHMMAX) /* Red Hat does not define SHMMAX */
#     undef _SHMMAX
#     if defined(__sparc__) || defined(__powerpc__) 
#       define _SHMMAX ((unsigned long)16*1024)
#     elif defined(__alpha__)
#       define _SHMMAX ((unsigned long)4*1024)
#     else
        /* Intel */
#       define _SHMMAX ((unsigned long)32*1024)
#     endif
#  endif
#elif defined(SHMMAX)
#  undef _SHMMAX
#  define _SHMMAX (((unsigned long)SHMMAX)>>10)
#endif
   static  unsigned long MinShmem = _SHMMAX;  
   static  unsigned long MaxShmem = MAX_REGIONS*_SHMMAX;
#else

   static  unsigned long MinShmem;  
   static  unsigned long MaxShmem;

#endif

#ifdef LINUX
#define CLEANUP_CMD(command) sprintf(command,"/usr/bin/ipcrm shm %d",id);
#elif  defined(SOLARIS) 
#define CLEANUP_CMD(command) sprintf(command,"/bin/ipcrm -m %d",id);
#elif  defined(SGI) 
#define CLEANUP_CMD(command) sprintf(command,"/usr/sbin/ipcrm -m %d",id);
#else
#define CLEANUP_CMD(command) sprintf(command,"/usr/bin/ipcrm -m %d",id);
#endif


#ifdef   ALLOC_MUNMAP
#ifdef QUADRICS
#include <elan/elan.h>
#include <elan3/elan3.h>
/*
extern void* elan_base;
extern void   *elan3_allocMain(void*, int, int);
*/
#define ALGN_MALLOC(s,a) elan_allocMain(elan_base->state, (a), (s))
#else 
#define ALGN_MALLOC(s,a) malloc((s))
#endif
static char* alloc_munmap(size_t size)
{
char *tmp;
unsigned long iptr;
    tmp = ALGN_MALLOC(size+pagesize-1, pagesize);
    if(tmp){
        iptr = (unsigned long)tmp;
        iptr >>= logpagesize; iptr <<= logpagesize;
        if(DEBUG_)
           printf("%d:unmap ptr=%d->%d size=%d\n",armci_me, tmp,iptr, (int)size);
        tmp = (char*)iptr;
        if(munmap(tmp, size) == -1) armci_die("munmap failed",0);
    }else armci_die("alloc_munmap: malloc failed",(int)size);
    return tmp;
}
#endif



int armci_test_allocate(long size)
{
   char *ptr;
   long id = (long)shmget(IPC_PRIVATE, (size_t) size, (int) (IPC_CREAT |00600));
   if (id <0L) return 0;

   /* attach to segment */
   ptr =  shmat((int) id, (char *) NULL, 0);

   /* delete segment id */
   if(shmctl( (int) id, IPC_RMID,(struct shmid_ds *)NULL))
      fprintf(stderr,"failed to remove shm id=%ld\n",id);

   /* test pointer */
   if (((long)ptr) == -1L) return 0;
   else return 1;
}


/* parameters that define range and granularity of search for shm segment size
 * UBOUND is chosen to be < 2GB to avoid overflowing on 32-bit systems
 * smaller PAGE gives more accurate results but with more search steps
 * LBOUND  is set to minimum amount for our purposes
 * change UBOUND=512MB if you need larger arrays than 512 MB
 */
#define PAGE (16*65536L)
#define LBOUND  1048576L
#define UBOUND 512*LBOUND

/*\ determine the max shmem segment size using bisection
\*/
int armci_shmem_test()
{
long x;
int  i,rc;
long upper_bound=UBOUND;
long lower_bound=0;

     x = UBOUND;
     for(i=1;;i++){
        long step;
        rc = armci_test_allocate(x);
        if(DEBUG_) 
           printf("%d:test %d size=%ld bytes status=%d\n",armci_me,i,x,rc);
        if(rc){
          lower_bound = x;
          step = (upper_bound -x)>>1;
          if(step < PAGE) break;
          x += step;
        }else{
          upper_bound = x;
          step = (x-lower_bound)>>1;
          if(step<PAGE) break;
          x -= step;
        }
        /* round it up to a full base-2 MB */
        x += 1048576L -1L;
        x >>=20;
        x <<=20; 
      }

      if(!lower_bound){
          /* try if can get LBOUND - necessary if search starts from UBOUND */
          lower_bound=LBOUND;
          rc = armci_test_allocate(lower_bound);
          if(!rc) return(0);
      }

      if(DEBUG_) printf("%ld bytes segment size, %d calls \n",lower_bound,i);
      return (int)( lower_bound>>20); /* return shmmax in mb */
}


void armci_shmem_init()
{

#ifdef ALLOC_MUNMAP
   /* determine log2(pagesize) needed for address alignment */
   int tp=512;
   logpagesize = 9;
   pagesize = getpagesize();
   if(tp>pagesize)armci_die("armci_shmem_init:pagesize",pagesize);

   while(tp<pagesize){
        tp <<= 1;
        logpagesize++;
   }
   if(tp!=pagesize)armci_die("armci_shmem_init:pagesize pow 2",pagesize);
   if(DEBUG_)printf("page size =%d log=%d\n",pagesize,logpagesize);

#endif

   if(armci_me == armci_master){
#ifndef NO_SHMMAX_SEARCH
        int x;
#ifdef LAPI
        x = armci_shmem_test();
#else
        x =armci_child_shmem_init();
#endif

        if(x<1)
          armci_die("no usable amount (%d bytes) of shared memory available\n",
          (int)LBOUND);

#       if defined(ALLOC_MUNMAP)
           /* need to cap down for special memory allocator */
           if(x>MAX_ALLOC_MUNMAP) x=MAX_ALLOC_MUNMAP;
#       endif

        if(DEBUG_) printf("GOT %d mbytes max segment size \n",x);fflush(stdout);
        MinShmem = (long)(x<<10); /* make sure it is in kb: mb <<10 */ 
        MaxShmem = MAX_REGIONS*MinShmem;
#ifdef REPORT_SHMMAX
       printf("%d using x=%d SHMMAX=%dKB\n", armci_me,x, MinShmem);
       fflush(stdout);
       sleep(1);
#endif
#else

      /* nothing to do here - limits were given */
#endif
    }
}


/*\ application can reset the upper limit (bytes) for memory allocation
\*/
void armci_set_shmem_limit(unsigned long shmemlimit)
{
     unsigned long kbytes;
     kbytes = (shmemlimit + SHM_UNIT -1)/SHM_UNIT;
     if(MaxShmem > kbytes) MaxShmem = kbytes;
     if(MinShmem > kbytes) MinShmem = kbytes;
}


static void shmem_errmsg(long size)
{
    printf("******************* ARMCI INFO ************************\n");
    printf("The application attempted to allocate a shared memory segment ");
    printf("of %ld bytes in size. This might be in addition to segments ",size);
    printf("that were allocated succesfully previously. ");
    printf("The current system configuration does not allow enough ");
    printf("shared memory to be allocated to the application.\n");
    printf("This is most often caused by:\n1) system parameter SHMMAX ");
    printf("(largest shared memory segment) being too small or\n");
    printf("2) insufficient swap space.\n");
    printf("Please ask your system administrator to verify if SHMMAX ");
    printf("matches the amount of memory needed by your application and ");
    printf("the system has sufficient amount of swap space. ");
    printf("Most UNIX systems can be easily reconfigured ");
    printf("to allow larger shared memory segments,\n");
    printf("see http://www.emsl.pnl.gov/docs/global/support.html\n");
    printf("In some cases, the problem might be caused by insufficient swap space.\n");
    printf("*******************************************************\n");
}


static struct shm_region_list{
   char     *addr;
   long     id;
   int      attached;
}region_list[MAX_REGIONS];
static long alloc_regions=0;
static long occup_blocks=0;

/* Terminology
 *   region - actual piece of shared memory allocated from OS
 *   block  - a part of allocated shmem that is given to the requesting process
 */




#if defined(MULTIPLE_REGIONS)
/********************************* MULTIPLE_REGIONS *******************/
/* allocate contiguous shmem -- glue pieces together -- works on SUN 
 * SUN max shmem segment is only 1MB so we might need several to    
 * satisfy a request
 */


/* SHM_OP is an operator to calculate shmem address to attach 
 * might be + or - depending on the system 
 */
#if defined(DECOSF) || defined(LINUX)
#define SHM_OP +
#else
#define SHM_OP -
#endif


static long prev_alloc_regions=0;

/*\
 *   assembles the list of shmem id for the block 
\*/
find_regions(addrp,   idlist, first)
    char *addrp;
    long *idlist, *first;
{
long reg, nreg, freg=-1, min_reg, max_reg;

       /* find the region where addrp belongs */
       for(reg = 0; reg < alloc_regions-1; reg++){
          if(region_list[reg].addr < region_list[reg+1].addr){
             min_reg = reg; max_reg = reg+1;
          }else{
             min_reg = reg+1; max_reg = reg;
          }
          if(region_list[min_reg].addr <= addrp  && 
             region_list[max_reg].addr > addrp){
             freg = min_reg;
             break;
          }
       }
       /* if not found yet, it must be the last region */
       if(freg < 0) freg=alloc_regions-1;

       if( alloc_regions == prev_alloc_regions){
           /* no new regions were allocated this time - just get the id */
           idlist[0] = 1;
           idlist[1] = region_list[freg].id;
       }else{
           /* get ids of the allocated regions */
           idlist[0] = alloc_regions - prev_alloc_regions;
           if(idlist[0] < 0)armci_die(" find_regions error ",0);
           for(reg =prev_alloc_regions,nreg=1; reg <alloc_regions;reg++,nreg++){
               idlist[nreg] = region_list[reg].id;
           }
           prev_alloc_regions = alloc_regions;
       }
       *first = freg;
       return 1;
}


/*\
\*/
char *Create_Shared_Region(idlist, size, offset)
     long size, *idlist, *offset;
{
char *temp,  *shmalloc();
void shmalloc_request();
long reg;
  
  if(alloc_regions>=MAX_REGIONS)
       armci_die("Create_Shared_Region: to many regions ",0L);

  /*initialization */
  if(!alloc_regions){
      for(reg=0;reg<MAX_REGIONS;reg++){
        region_list[reg].addr=(char*)0;
        region_list[reg].attached=0;
        region_list[reg].id=0;
      }
      shmalloc_request((unsigned)MinShmem, (unsigned)MaxShmem);
      idlist[SHMIDLEN-2]=MinShmem;
  }

  temp = shmalloc((unsigned long)size);
  if(temp == (char*)0 )
     armci_die("Create_Shared_Region: shmalloc failed ",0L);
    
  if(!find_regions(temp, idlist,&reg))
     armci_die("Create_Shared_Region: allocation inconsistent ",0L);

  /* stamp at the beginning address to be tested by other processes  */
  if(STAMP) *(int*)temp = alloc_regions-1;

/*  *offset = (long) (temp - region_list[reg].addr);*/
  *offset = (long) (temp - region_list[0].addr);
  occup_blocks ++;

  if(DEBUG_) fprintf(stderr, ">Create_Shared_Region: reg=%d id= %d  off=%d  addr=%d addr+off=%d s=%d stamp=%d num ids=%d\n",reg,region_list[reg].id, *offset, region_list[reg].addr, temp, size, *(int*)temp,idlist[0]);

  return temp;
}


char *Attach_Shared_Region(idlist, size, offset)
     long *idlist, offset, size;
{
int ir, reg,  found, first;
char *temp = (char*)0, *pref_addr=(char*)0;

  if(alloc_regions>=MAX_REGIONS)
       armci_die("Attach_Shared_Region: too many regions ",0L);

  /* first time needs to initialize region_list structure */
  if(!alloc_regions){
      for(reg=0;reg<MAX_REGIONS;reg++){
        region_list[reg].addr=(char*)0;
        region_list[reg].attached=0;
        region_list[reg].id=0;
      }
      MinShmem= idlist[SHMIDLEN-2];
  }

 /* 
  * Now, process the idlist list:
  *    . for every shemem ID make sure that it is attached
  *    . calulate shmem address by adding offset to the address for 1st region
  *    . idlist[0] has the number of shmem regions to process
  *    . idlist is assumed to be ordered -- first region comes first etc.
  */
  pref_addr = (char*)0;   /* first time let the OS choose address */
  for (ir = 0; ir< idlist[0]; ir++){
      /* search region_list for the current shmem id */
      for(found =0, reg=0; reg < MAX_REGIONS;reg++)
         if(found=(region_list[reg].id == idlist[1+ir])) break;

      if(!found){
         /* shmem id is not on the list */ 
         reg = alloc_regions;
         region_list[reg].id =idlist[1+ir];

      }

      /* attach if not attached yet */
      if(!region_list[reg].attached){
        /* make sure the next shmem region will be adjacent to previous one */

         if(temp) pref_addr= temp SHM_OP (MinShmem*SHM_UNIT);
#ifdef   ALLOC_MUNMAP
         else
              pref_addr = alloc_munmap((size_t) (MinShmem*SHM_UNIT));
#        endif

         if(DEBUG_)
            fprintf(stderr,"%d:trying id=%d pref=%ld tmp=%ld u=%d\n",armci_me,
                 idlist[1+ir],pref_addr,temp,MinShmem);

         if ( (int) (temp = (char*)shmat((int)idlist[1+ir], pref_addr, 0))==-1){
           fprintf(stderr,"%d:shmat err:id=%d pref=%ld off=%d\n",
                   armci_me, idlist[1+ir],pref_addr,offset);
           shmem_errmsg(size);
           armci_die("AttachSharedRegion:failed to attach",(long)idlist[1+ir]);
         }

         region_list[reg].addr = temp; 
         region_list[reg].attached = 1;
         alloc_regions++;

         if(DEBUG_){
           printf("%d: Attach_Shared_Region: id=%d pref=%ld got addr=%ld\n",
                           armci_me, idlist[1+ir], pref_addr, temp);
           fflush(stdout);
         }
      }
      /* now we have this region attached and ready to go */

      if(!ir)first = reg;  /* store the first region */
  }

  reg = first; /* first region on the list */ 

  if(DEBUG_) fprintf(stderr, ">Attach_Shared_Region: reg=%d id= %d  off=%d  addr=%d addr+off=%d \n",reg,region_list[reg].id, offset, region_list[reg].addr, region_list[reg].addr+ offset);

  if(STAMP)
  /* check stamp to make sure that we are attached in the right place */
  if(*((int*)(region_list[reg].addr+ offset))!= alloc_regions-1){
      fprintf(stderr, "attach: region=%d  ",alloc_regions);
      armci_die("Attach_Shared_Region: wrong stamp value !", 
                *((int*)(region_list[reg].addr+ offset)));
  }
  occup_blocks++;

/*  return (region_list[reg].addr + offset);*/
  return (region_list[0].addr + offset);
}


/*\ allocates shmem, to be called by shmalloc that is called by process that
 *  creates shmem region
\*/
char *allocate(size)
     long size;
{
#define min(a,b) ((a)>(b)? (b): (a))
char *temp = (char*)0, *pref_addr=(char*)0, *ftemp;
int id, newreg, i;
long sz;

    if(DEBUG1){
       printf("%d:Shmem allocate size %ld bytes\n",armci_me,size); 
       fflush(stdout);
    }

    newreg = (size+(SHM_UNIT*MinShmem)-1)/(SHM_UNIT*MinShmem);
    if( (alloc_regions + newreg)> MAX_REGIONS)
       armci_die("allocate: to many regions already allocated ",(long)newreg);

    prev_alloc_regions = alloc_regions; 

    if(DEBUG_)fprintf(stderr, "in allocate size=%d\n",size);

#ifdef  ALLOC_MUNMAP
    pref_addr = alloc_munmap((size_t) size);
#else
    pref_addr = (char*)0;   /* first time let the OS choose address */
#endif

    /* allocate shmem in as many segments as neccesary */
    for(i =0; i< newreg; i++){ 
       sz =(i==newreg-1)?size -i*MinShmem*SHM_UNIT: min(size,SHM_UNIT*MinShmem);

       if ( (int)(id = shmget(IPC_PRIVATE, (int) sz,
                     (int) (IPC_CREAT | 00600))) < 0 ){
          fprintf(stderr,"%d:id=%d size=%d MAX=%d\n",armci_me,id,(int)sz,MinShmem);
          alloc_regions++;
          shmem_errmsg(size);
          armci_die("allocate: failed to create shared region ",(long)id);
       }

       /* make sure the next shmem region will be adjacent to previous one */
       if(temp) pref_addr= temp SHM_OP (MinShmem*SHM_UNIT);

       if(DEBUG_)printf(" calling shmat: id=%d adr=%d sz=%d\n",id,pref_addr,sz);

       if ( (int)(temp = (char*)shmat((int) id, pref_addr, 0)) == -1){
          char command[64];
          CLEANUP_CMD(command);
          if(system(command) == -1) 
            fprintf(stderr,"Please clean shared memory id=%d: see man ipcrm\n",id);
          if(pref_addr){
             printf("ARMCI shared memory allocator was unable to obtain from ");
             printf("the operating system multiple segments adjacent to ");
             printf("each other in order to combine them into a one large ");
             printf("segment together\n");
             shmem_errmsg(size);
             armci_die("allocate: failed to attach to shared region",  0L);
         }
       }

       region_list[alloc_regions].addr = temp;
       region_list[alloc_regions].id = id;
       region_list[alloc_regions].attached=1;

       if(DEBUG_) fprintf(stderr," allocate:attach: id=%d addr=%d \n",id, temp);
       alloc_regions++;
       if(i==0)ftemp = temp;
    }
    return (min(ftemp,temp));
}
    
/************************** END of MULTIPLE_REGIONS *******************/

#else /* Now, the machines where shm segments are not glued together */ 


int last_allocated=-1;
/*\
\*/
char *Create_Shared_Region(id, size, offset)
     long size, *id, *offset;
{
char *temp,  *shmalloc();
void shmalloc_request();
int  reg, nreg;
  
  if(alloc_regions>=MAX_REGIONS)
       armci_die("Create_Shared_Region: to many regions ",0L);

  /*initialization */
  if(!alloc_regions){
      for(reg=0;reg<MAX_REGIONS;reg++){
        region_list[reg].addr=(char*)0;
        region_list[reg].attached=0;
        region_list[reg].id=0;
      }
      if(DEBUG_)
           printf("%d:allocation unit: %dK, max shmem:%dK\n",armci_me,MinShmem,MaxShmem);
      shmalloc_request((unsigned)MinShmem, (unsigned)MaxShmem);
      id[SHMIDLEN-2]=MinShmem;
  }


    temp = shmalloc((unsigned)size);
    if(temp == (char*)0 )
       armci_die("Create_Shared_Region: shmalloc failed ",0L);
    
    /* find the region */
    if(last_allocated!=-1){
       reg=last_allocated;
       last_allocated = -1;
    } else{
       for(reg=0,nreg=0;nreg<alloc_regions; nreg++){
          if(region_list[nreg].addr > temp )break;
          reg = nreg;
       }
    }

    if(STAMP) *((int*)temp) = alloc_regions-1;

    *offset = (long) (temp - region_list[reg].addr);
    *id = region_list[reg].id;
    occup_blocks++;
  
/*
  if(DEBUG_) fprintf(stderr,"Create_Shared_Region: reg=%d id= %d  off=%d  addr=%d    addr+off=%d s=%d\n",reg,*id, *offset, region_list[reg].addr, temp, size);
*/

    return temp;
}


char *Attach_Shared_Region(id, size, offset)
     long *id, offset, size;
{
int reg, found;
static char *temp;

  if(alloc_regions>=MAX_REGIONS)
       armci_die("Attach_Shared_Region: to many regions ",0L);

  if(!*id)
      armci_die("Attach_Shared_Region: shmem ID=0 ",*id);

  /* first time needs to initialize region_list structure */
  if(!alloc_regions){
      for(reg=0;reg<MAX_REGIONS;reg++){
        region_list[reg].addr=(char*)0;
        region_list[reg].attached=0;
        region_list[reg].id=0;
      }
      MinShmem= id[SHMIDLEN-2];
      if(DEBUG_)
         printf("%d:allocation unit: %dK\n",armci_me,MinShmem);
  }

  /* search region_list for the current shmem id */
  for(found = 0, reg=0; reg < MAX_REGIONS;reg++)
     if(found=(region_list[reg].id == *id))break;

  if(!found){
     reg = alloc_regions;
     region_list[reg].id =*id;
     alloc_regions++;
  }

  /* attach if not attached yet */
  if(!region_list[reg].attached){
#   ifdef ALLOC_MUNMAP
       char *pref_addr = alloc_munmap((size_t) (MinShmem*SHM_UNIT));
#   else
       char *pref_addr = (char*)0;
#   endif
    if ( (long) (temp = shmat((int) *id, pref_addr, 0)) == -1L){
       fprintf(stderr,"%d:attach error:id=%ld off=%ld seg=%d\n",armci_me,*id,offset,MinShmem);
       shmem_errmsg(MinShmem*1024);
       armci_die("Attach_Shared_Region:failed to attach to segment id=",*id);
    }
    region_list[reg].addr = temp; 
    region_list[reg].attached = 1;
  }

/*
  if(DEBUG_) fprintf(stderr, "attach: reg=%d id= %d  off=%d  addr=%d addr+off=%d   f=%d\n",reg,*id,offset, region_list[reg].addr,region_list[reg].addr+ offset,  found);
*/

  if(STAMP)
  /* check stamp to make sure that we are attached in the right place */
  if(*((int*)(region_list[reg].addr+ offset))!= alloc_regions-1)
      armci_die("Attach_Shared_Region: wrong stamp value !", 
                *((int*)(region_list[reg].addr+ offset)));
  occup_blocks++;
  return (region_list[reg].addr+ offset);
}


/*\ allocates shmem, to be called by shmalloc that is called by process that
 *  creates shmem region
\*/
char *allocate(size)
     long size;
{
char * temp;
long id;
#ifdef ALLOC_MUNMAP
       char *pref_addr = alloc_munmap((size_t) (MinShmem*SHM_UNIT));
#else
       char *pref_addr = (char*)0;
#endif

    if(DEBUG1){
       printf("%d:Shmem allocate size %ld bytes\n",armci_me,size); 
       fflush(stdout);
    }

    if( alloc_regions >= MAX_REGIONS)
       armci_die("Create_Shared_Region: to many regions already allocated ",0L);

    last_allocated = alloc_regions;
    if ( (id = (long)shmget(IPC_PRIVATE, (int) size,
                     (int) (IPC_CREAT | 00600))) < 0L ){
       fprintf(stderr,"id=%ld size=%d\n",id, (int) size);
       shmem_errmsg(size);
       armci_die("allocate: failed to create shared region ",(long)id);
    }

    if ( (long) (temp = shmat((int)id, pref_addr, 0)) == -1L){
       armci_die("allocate: failed to attach to shared region",  temp);
    }

    if(DEBUG_){
      printf("%d:allocate:id=%ld adr=%ld size=%ld\n",armci_me,id,temp,size);
      fflush(stdout);
    }
    region_list[alloc_regions].addr = temp;
    region_list[alloc_regions].id = id;
    region_list[alloc_regions].attached=1;
    alloc_regions++;
    return (temp);
}
    
#endif

/* common code for the two versions */


/*\ only process that created shared region returns the pointer to shmalloc 
\*/
void Free_Shmem_Ptr( id, size, addr)
     long id, size;
     char *addr;
{
  void shfree();
  shfree(addr);
}


void Delete_All_Regions()
{
int reg;
int code=0;
extern int armci_me;

  for(reg = 0; reg < MAX_REGIONS; reg++){
    if(region_list[reg].addr != (char*)0){
      code += shmctl((int)region_list[reg].id,IPC_RMID,(struct shmid_ds *)NULL);
      region_list[reg].addr = (char*)0;
      region_list[reg].attached = 0;
      if(DEBUG_)
         fprintf(stderr,"%d Delete_All_Regions id=%d code=%d\n",armci_me, 
                (int)region_list[reg].id, code);
    }
  }
}


/* the rest is obsolete */

long Detach_Shared_Region( id, size, addr)
     long id, size;
     char *addr;
{
int reg;
long code;
void shfree();

  shfree(addr);
  occup_blocks--;
  code = 0;

  if(!occup_blocks)
    for(reg=0;reg<MAX_REGIONS;reg++){
       if(region_list[reg].attached) code += shmdt(region_list[reg].addr);
  }
  return (code);
}



long Delete_Shared_Region(id)
     long id;
{
long code;

 code = 0;
 occup_blocks--;

 /* don't actually detach until all blocks are gone */
/* if(!occup_blocks)code = Delete_All_Regions(); */
 return (code);
}

#else
 what are doing here ?
#endif
