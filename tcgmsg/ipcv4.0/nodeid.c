/* $Header: /tmp/hpctools/ga/tcgmsg/ipcv4.0/nodeid.c,v 1.3 1995-02-24 02:14:06 d3h325 Exp $ */

#include "sndrcv.h"
#include "sndrcvP.h"

long NODEID_()
/*
  return logical node no. of current process
*/
{
  return SR_proc_id;
}
