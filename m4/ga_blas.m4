# GA_F77_BLAS_TEST
# ----------------
# Generate Fortran 77 conftest for BLAS.
AC_DEFUN([GA_F77_BLAS_TEST], [AC_LANG_CONFTEST([AC_LANG_PROGRAM([],
[[      implicit none
      INTEGER M, N, K, LDA, LDB, LDC
      COMPLEX CA(20,40), CB(20,30), CC(40,30), Calpha, Cbeta
      DOUBLE COMPLEX ZA(20,40), ZB(20,30), ZC(40,30), Zalpha, Zbeta
      REAL SA(20,40), SB(20,30), SC(40,30), Salpha, Sbeta
      DOUBLE PRECISION DA(20,40), DB(20,30), DC(40,30), Dalpha, Dbeta
      external CGEMM
      external ZGEMM
      external SGEMM
      external DGEMM
      M = 10
      N = 20
      K = 15
      LDA = 20
      LDB = 20
      LDC = 40
      Calpha = 2.0
      Cbeta = 2.0
      Zalpha = 2.0
      Zbeta = 2.0
      Salpha = 2.0
      Sbeta = 2.0
      Dalpha = 2.0
      Dbeta = 2.0
      CALL CGEMM ('T','N',M,N,K,Calpha,CA,LDA,CB,LDB,Cbeta,CC,LDC)
      CALL ZGEMM ('T','N',M,N,K,Zalpha,ZA,LDA,ZB,LDB,Zbeta,ZC,LDC)
      CALL SGEMM ('T','N',M,N,K,Salpha,SA,LDA,SB,LDB,Sbeta,SC,LDC)
      CALL DGEMM ('T','N',M,N,K,Dalpha,DA,LDA,DB,LDB,Dbeta,DC,LDC)]])])
])

# GA_C_BLAS_TEST
# --------------
# Generate C conftest for BLAS.
AC_DEFUN([GA_C_BLAS_TEST], [AC_LANG_CONFTEST([AC_LANG_PROGRAM(
[#ifdef __cplusplus
extern "C" {
#endif
char cgemm ();
char dgemm ();
char sgemm ();
char zgemm ();
#ifdef __cplusplus
}
#endif
],
[[char cresult =  cgemm ();
char dresult =  dgemm ();
char sresult =  sgemm ();
char zresult =  zgemm ();
]])])
])


# GA_BLAS([ACTION-IF-FOUND[, ACTION-IF-NOT-FOUND]])
# -------------------------------------------------
# Originally from http://www.nongnu.org/autoconf-archive/ga_blas.html
# Modified to support many options to --with flag, updated to use AS_* macros,
# and different defaults for ACTIONs.
# Apparently certain compilers on BGP define sgemm and dgemm, so we must
# test for a different BLAS routine. cgemm seems okay.
AC_DEFUN([GA_BLAS],
[AC_REQUIRE([AC_F77_LIBRARY_LDFLAGS])
AC_ARG_WITH([blas],
    [AS_HELP_STRING([--with-blas[[=ARG]]], [use external BLAS library])])

ga_blas_ok=no
AS_IF([test "x$with_blas" = xno], [ga_blas_ok=skip])

# Parse --with-blas argument.
GA_ARG_PARSE([with_blas], [BLAS_LIBS], [BLAS_LDFLAGS], [BLAS_CPPFLAGS])

# Get fortran linker names of BLAS functions to check for.
AC_F77_FUNC(cgemm)
AC_F77_FUNC(dgemm)
AC_F77_FUNC(sgemm)
AC_F77_FUNC(zgemm)

ga_save_LIBS="$LIBS"
ga_save_LDFLAGS="$LDFLAGS";     LDFLAGS="$BLAS_LDFLAGS $LDFLAGS"
ga_save_CPPFLAGS="$CPPFLAGS";   CPPFLAGS="$BLAS_CPPFLAGS $CPPFLAGS"

AC_MSG_NOTICE([Attempting to locate BLAS library])

# First, check environment/command-line variables.
# If failed, erase BLAS_LIBS but maintain BLAS_LDFLAGS and BLAS_CPPFLAGS.
AS_IF([test $ga_blas_ok = no],
    [ga_save_LIBS="$LIBS"; LIBS="$BLAS_LIBS $LIBS"
     AS_IF([test "x$enable_f77" = xno],
        [AC_MSG_CHECKING([for C BLAS with user-supplied flags])
         AC_LANG_PUSH([C])
         GA_C_BLAS_TEST()
         AC_LINK_IFELSE([], [ga_blas_ok=yes], [BLAS_LIBS=])
         AC_LANG_POP([C])],
        [AC_MSG_CHECKING([for Fortran 77 BLAS with user-supplied flags])
         AC_LANG_PUSH([Fortran 77])
         GA_F77_BLAS_TEST()
         AC_LINK_IFELSE([], [ga_blas_ok=yes], [BLAS_LIBS=])
         AC_LANG_POP([Fortran 77])])
     AC_MSG_RESULT([$ga_blas_ok])
     LIBS="$ga_save_LIBS"])

# BLAS in ATLAS library? (http://math-atlas.sourceforge.net/)
AS_IF([test $ga_blas_ok = no],
    [AS_IF([test "x$enable_f77" = xno],
        [AC_MSG_CHECKING([for C BLAS in ATLAS])
         # add -lcblas if needed but missing from BLAS_LIBS and LIBS
         AS_CASE([$LIBS:$BLAS_LIBS],
                 [*cblas*:*],       [],
                 [*:*cblas*],       [],
                                    [BLAS_LIBS="$BLAS_LIBS -lcblas"])],
        [AC_MSG_CHECKING([for Fortran 77 BLAS in ATLAS])
         # add -lf77blas if needed but missing from BLAS_LIBS and LIBS
         AS_CASE([$LIBS:$BLAS_LIBS],
                 [*f77blas*:*],         [],
                 [*:*f77blas*],         [],
                                        [BLAS_LIBS="$BLAS_LIBS -lf77blas"])])
     # add -latlas if needed but missing from BLAS_LIBS and LIBS
     AS_CASE([$LIBS:$BLAS_LIBS],
             [*atlas*:*],       [],
             [*:*atlas*],       [],
                                [BLAS_LIBS="$BLAS_LIBS -latlas"])
     LIBS="$BLAS_LIBS $LIBS"
     AS_IF([test "x$enable_f77" = xno],
        [AC_LANG_PUSH([C])
         GA_C_BLAS_TEST()
         AC_LINK_IFELSE([], [ga_blas_ok=yes], [BLAS_LIBS=])
         AC_LANG_POP([C])],
        [AC_LANG_PUSH([Fortran 77])
         GA_F77_BLAS_TEST()
         AC_LINK_IFELSE([], [ga_blas_ok=yes], [BLAS_LIBS=])
         AC_LANG_POP([Fortran 77])])
     LIBS="$ga_save_LIBS"
     AC_MSG_RESULT([$ga_blas_ok])])

# BLAS in AMD Core Math Library? (ACML)
AS_IF([test $ga_blas_ok = no],
    [AC_MSG_CHECKING([for BLAS in AMD Core Math Library])
     AS_CASE([$LIBS:$BLAS_LIBS],
        [*acml*:*],         [],
        [*:*acml*],         [],
                            [BLAS_LIBS="$BLAS_LIBS -lacml"])
     LIBS="$BLAS_LIBS $LIBS"
     AC_LANG_PUSH([Fortran 77])
     GA_F77_BLAS_TEST()
     AC_LINK_IFELSE([], [ga_blas_ok=yes], [BLAS_LIBS=])
     AC_LANG_POP([Fortran 77])
     LIBS="$ga_save_LIBS"
     AC_MSG_RESULT([$ga_blas_ok])])

# BLAS in Intel MKL library?
AS_IF([test $ga_blas_ok = no],
    [AC_MSG_CHECKING([for BLAS in Intel MKL])
     AS_CASE([$LIBS:$BLAS_LIBS],
        [*mkl*:*],      [],
        [*:*mkl*],      [],
                        [BLAS_LIBS="$BLAS_LIBS -lmkl"])
     LIBS="$BLAS_LIBS $LIBS"
     AC_LANG_PUSH([Fortran 77])
     GA_F77_BLAS_TEST()
     AC_LINK_IFELSE([], [ga_blas_ok=yes], [BLAS_LIBS=])
     AC_LANG_POP([Fortran 77])
     LIBS="$ga_save_LIBS"
     AC_MSG_RESULT([$ga_blas_ok])])

# BLAS in PhiPACK libraries? (requires generic BLAS lib, too)
#AS_IF([test $ga_blas_ok = no],
#    [AC_MSG_NOTICE([  BLAS in PhiPACK libraries?])
#     AC_CHECK_LIB([blas], [$sgemm],
#        [AC_CHECK_LIB([dgemm], [$dgemm],
#            [AC_CHECK_LIB([sgemm], [$sgemm],
#                [ga_blas_ok=yes; BLAS_LIBS="-lsgemm -ldgemm -lblas"],
#                [],
#                [-lblas])],
#            [],
#            [-lblas])])])

# BLAS in Apple vecLib library?
AS_IF([test $ga_blas_ok = no],
    [AC_MSG_CHECKING([for BLAS in Apple vecLib library])
     AS_CASE([$LIBS:$BLAS_LIBS],
        [*vecLib*:*],   [],
        [*:*vecLib*],   [],
        [BLAS_LIBS="$BLAS_LIBS -framework vecLib"])
     LIBS="$BLAS_LIBS $LIBS"
     AC_LANG_PUSH([C])
     GA_C_BLAS_TEST()
     AC_LINK_IFELSE([], [ga_blas_ok=yes], [BLAS_LIBS=])
     AC_LANG_POP([C])
     LIBS="$ga_save_LIBS"
     AC_MSG_RESULT([$ga_blas_ok])])

# BLAS in Alpha CXML library?
AS_IF([test $ga_blas_ok = no],
    [AC_MSG_CHECKING([for BLAS in Alpha CXML library])
     AS_CASE([$LIBS:$BLAS_LIBS],
        [*cxml*:*], [],
        [*:*cxml*], [],
                    [BLAS_LIBS="$BLAS_LIBS -lcxml"])
     LIBS="$BLAS_LIBS $LIBS"
     AC_LANG_PUSH([Fortran 77])
     GA_F77_BLAS_TEST()
     AC_LINK_IFELSE([], [ga_blas_ok=yes],
        [AS_CASE([$LIBS:$BLAS_LIBS],
            [*cpml*:*], [],
            [*:*cpml*], [],
                        [BLAS_LIBS="$BLAS_LIBS -lcpml"; LIBS="$LIBS -lcpml"])
         AC_LINK_IFELSE([], [ga_blas_ok=yes], [BLAS_LIBS=])])
     AC_LANG_POP([Fortran 77])
     LIBS="$ga_save_LIBS"
     AC_MSG_RESULT([$ga_blas_ok])])

# BLAS in Alpha DXML library? (now called CXML, see above)
#AS_IF([test $ga_blas_ok = no],
#    [AC_MSG_NOTICE([  BLAS in Alpha DXML library?])
#     AC_CHECK_LIB(dxml, $sgemm, [ga_blas_ok=yes; BLAS_LIBS="-ldxml"])])

################### Assume Fortran 77 hereafter
AC_LANG_PUSH([Fortran 77])

# BLAS in Sun Performance library?
AS_IF([test $ga_blas_ok = no],
    [AS_IF([test "x$GCC" != xyes],
        [AC_MSG_NOTICE([  BLAS in Sun Performance library?])
         AC_CHECK_LIB([sunmath], [acosp],
            [AC_CHECK_LIB([sunperf], [sgemm],
                [ga_blas_ok=yes; BLAS_LIBS="-xlic_lib=sunperf -lsunmath"],
                [],
                [-lsunmath])])])])

# BLAS in SCSL library?  (SGI/Cray Scientific Library)
AS_IF([test $ga_blas_ok = no],
    [AC_MSG_NOTICE([  BLAS in SGI/Cray Scientific Library?])
     AC_CHECK_LIB([scs], [sgemm], [ga_blas_ok=yes; BLAS_LIBS="-lscs"])])

# BLAS in SGIMATH library?
AS_IF([test $ga_blas_ok = no],
    [AC_MSG_NOTICE([  BLAS in SGIMATH library?])
     AC_CHECK_LIB([complib.sgimath], [sgemm],
        [ga_blas_ok=yes; BLAS_LIBS="-lcomplib.sgimath"])])

# BLAS in IBM ESSL library? (requires generic BLAS lib, too)
AS_IF([test $ga_blas_ok = no],
    [AC_MSG_NOTICE([  BLAS in IBM ESSL library?])
     AC_CHECK_LIB([blas], [sgemm],
        [AC_CHECK_LIB([essl], [sgemm],
            [ga_blas_ok=yes; BLAS_LIBS="-lessl -lblas"],
            [],
            [-lblas $FLIBS])])])

# Generic BLAS library?
AS_IF([test $ga_blas_ok = no],
    [AC_MSG_NOTICE([  BLAS generic library?])
     AC_CHECK_LIB([blas], [sgemm], [ga_blas_ok=yes; BLAS_LIBS="-lblas"])])

AC_LANG_POP([Fortran 77])

CPPFLAGS="$ga_save_CPPFLAGS"
LDFLAGS="$ga_save_LDFLAGS"

AC_SUBST([BLAS_LIBS])
AC_SUBST([BLAS_LDFLAGS])
AC_SUBST([BLAS_CPPFLAGS])

# Finally, execute ACTION-IF-FOUND/ACTION-IF-NOT-FOUND:
AS_IF([test $ga_blas_ok = yes],
    [AC_DEFINE([HAVE_BLAS], [1], [Define if you have a BLAS library.])
     $1],
    [AS_IF([test "x$enable_f77" = xyes],
        [AC_MSG_WARN([BLAS library not found, using internal BLAS])],
        [AC_MSG_WARN([BLAS library not found and Fortran is disabled])
         AC_MSG_WARN([BLAS use is disabled])])
     $2])
AM_CONDITIONAL([HAVE_BLAS], [test $ga_blas_ok = yes])
])dnl GA_BLAS
