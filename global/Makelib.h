# Makelib.h, 01.26.94
#

       CORE_LIBS = -lglobal -lma

ifndef LIBMPI
   LIBMPI = -lmpi
endif

ifneq ($(MSG_COMMS),MPI)
  ifdef USE_MPI
       LIBCOM = -ltcgmsg-mpi
  else
       LIBCOM = -ltcgmsg
  endif
endif


#................................ SUN ..........................................
ifeq ($(TARGET),SOLARIS)
       EXTRA_LIBS = /usr/ucblib/libucb.a -lsocket -lrpcsvc -lnsl
endif
#................................ KSR ......................................
#
ifeq ($(TARGET),KSR)
#
# KSR-2 running OSF 1.2.0.7
#
# These are pointers to much faster (optimized for KSR) version of TCGMSG 
# (does not come with the GA distribution package)
#
#    LIBCOM = $(SRC)/tcgmsg/ipcv4.0/libtcgmsg.a

       BLAS  = -lksrblas
 EXTRA_LIBS += -lrpc -para
endif
#................................ HPUX  .....................................
ifeq ($(TARGET),HPUX)
       EXTRA_LIBS = -lm 
endif
#................................ Intel .....................................
ifeq ($(INTEL),YES)
#
# all Intel machines
#
#................................ PARAGON ...................................
ifeq ($(TARGET),PARAGON)
       CLIB = -lm
#
       EXTRA_LIBS = -nx 
else
       EXTRA_LIBS = -node 
endif
       BLAS  = -lkmath
endif
#................................   SGI ....................................
ifeq ($(TARGET),SGITFP)
endif
ifeq ($(TARGET),SGI)
       BLAS = -lblas
endif
ifeq ($(TARGET),SGI64)
       BLAS = -lblas
endif
#.................................. SP1 ....................................
ifeq ($(TARGET),SP1)
       BLAS = -lblas
endif
#...........................................................................
ifeq ($(TARGET),IBM)
       BLAS = -lblas
endif
#...........................................................................

#LIBS += $(BLAS) -llinalg $(BLAS)


ifdef USE_MPI
   ifdef MPI_LIB
         LIBCOM += -L$(MPI_LIB) $(LIBMPI)
   else
         LIBCOM +=  $(LIBMPI)
   endif
endif

ifdef IWAY
  LIBCOM += -lserver 
endif

LIBCOM += $(EXTRA_LIBS)

ifdef USE_SCALAPACK
  LINALG = $(SCALAPACK)
endif

LINALG += $(BLAS) -llinalg $(BLAS)
