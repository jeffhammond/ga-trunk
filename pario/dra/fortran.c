/**************************** fortran  DRA interface **************************/

#include "global.h"
#include "drap.h"
#include "dra.h"

#if defined(__STDC__) || defined(__cplusplus)
# define _ARGS_(s) s
#else
# define _ARGS_(s) ()
#endif

extern void f2cstring   _ARGS_((char*, Integer, char*, Integer));
extern void c2fstring   _ARGS_((char*, char*, Integer));

#undef _ARGS_

#ifdef CRAY_T3D
#      include <fortran.h>
#endif


static char cname[DRA_MAX_NAME+1], cfilename[DRA_MAX_FNAME+1];


#ifdef CRAY_T3D
Integer dra_create_(type, dim1, dim2, name, filename, mode, reqdim1, reqdim2,d_a)
        Integer *d_a;                      /*input:DRA handle*/
        Integer *type;                     /*input*/
        Integer *dim1;                     /*input*/
        Integer *dim2;                     /*input*/
        Integer *reqdim1;                  /*input: dim1 of typical request*/
        Integer *reqdim2;                  /*input: dim2 of typical request*/
        Integer *mode;                     /*input*/
        _fcd    name;                      /*input*/
        _fcd    filename;                  /*input*/
#else
Integer dra_create_(type, dim1, dim2, name, filename, mode, reqdim1, reqdim2,d_a,
                   nlen, flen)
        Integer *d_a;                      /*input:DRA handle*/
        Integer *type;                     /*input*/
        Integer *dim1;                     /*input*/
        Integer *dim2;                     /*input*/
        Integer *reqdim1;                  /*input: dim1 of typical request*/
        Integer *reqdim2;                  /*input: dim2 of typical request*/
        Integer *mode;                     /*input*/
        char    *name;                     /*input*/
        char    *filename;                 /*input*/

        int     nlen;
        int     flen;

#endif
{
#ifdef CRAY_T3D
      f2cstring(_fcdtocp(name), _fcdlen(name), cname, DRA_MAX_NAME);
      f2cstring(_fcdtocp(filename), _fcdlen(filename), cfilename, DRA_MAX_FNAME);
#else
      f2cstring(name, nlen, cname, DRA_MAX_NAME);
      f2cstring(filename, flen, cfilename, DRA_MAX_FNAME);
#endif
return dra_create(type, dim1, dim2,cname,cfilename, mode, reqdim1, reqdim2,d_a);
}



#ifdef CRAY_T3D
Integer dra_open_(filename, mode, d_a)
        _fcd  filename;                  /*input*/
        Integer *mode;                   /*input*/
        Integer *d_a;                    /*input*/ 
#else
Integer dra_open_(filename, mode, d_a, flen)
        char *filename;                  /*input*/
        Integer *mode;                   /*input*/
        Integer *d_a;                    /*input*/
        int     flen;
#endif
{
#ifdef CRAY_T3D
      f2cstring(_fcdtocp(filename), _fcdlen(filename), cfilename, DRA_MAX_FNAME);
#else
      f2cstring(filename, flen, cfilename, DRA_MAX_FNAME);
#endif
      return dra_open(cfilename, mode, d_a);
}



#ifdef CRAY_T3D
Integer dra_inquire_(d_a, type, dim1, dim2, name, filename)
        Integer *d_a;                      /*input:DRA handle*/
        Integer *type;                     /*output*/
        Integer *dim1;                     /*output*/
        Integer *dim2;                     /*output*/
        _fcd    name;                      /*output*/
        _fcd    filename;        
#else
Integer dra_inquire_(d_a, type, dim1, dim2, name, filename, nlen, flen)
        Integer *d_a;                      /*input:DRA handle*/
        Integer *type;                     /*output*/
        Integer *dim1;                     /*output*/
        Integer *dim2;                     /*output*/
        char    *name;                     /*output*/
        char    *filename;

        int     nlen;
        int     flen;
#endif
{
Integer stat = dra_inquire(d_a, type, dim1, dim2, cname, cfilename);
#ifdef CRAY_T3D
   c2fstring(cname, _fcdtocp(name), _fcdlen(name));
   c2fstring(cfilename, _fcdtocp(filename), _fcdlen(filename));
#else
   c2fstring(cname, name, nlen);
   c2fstring(cfilename, filename, flen);
#endif
   return stat;
}

