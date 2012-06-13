#
#
# AC_PROG_DATASPACES
#
# Test for DATASPACES installation
# and set $DATASPACES to the correct value.
#
#
dnl @synopsis AC_PROG_DATASPACES
dnl
dnl This macro test if DATASPACES is installed. If DATASPACES
dnl is installed, it set $DATASPACES to the right value
dnl
dnl @version 1.0
dnl @author Norbert Podhorszki, pnorbert@ornl.gov
dnl
AC_DEFUN([AC_PROG_DATASPACES],[

AM_CONDITIONAL(HAVE_DATASPACES,true)

AC_ARG_WITH(dataspaces,
        [AS_HELP_STRING([--with-dataspaces=DIR],
           [Build the DATASPACES transport method. Point to the DATASPACES installation.])],
        [DATASPACES_LDFLAGS="-L$withval/lib";
         DATASPACES_CPPFLAGS="-I$withval/include";],
        [with_dataspaces=check])

dnl If --without-dataspaces was given set HAVE_DATASPACES to false and do nothing more
if test "x$with_dataspaces" == "xno"; then

   AM_CONDITIONAL(HAVE_DATASPACES,false)

else

    dnl allow args --with-dataspaces incdir and --with-dataspaces-libdir
    AC_ARG_WITH(dataspaces-incdir,
                [  --with-dataspaces-incdir=<location of dataspaces includes>],
                [DATASPACES_INCDIR=$withval
                 with_dataspaces=detailed])
    
    AC_ARG_WITH(dataspaces-libdir,
                [  --with-dataspaces-libdir=<location of dataspaces library>],
                [DATASPACES_LIBDIR=$withval
                 with_dataspaces=detailed])
    
    
    dnl If we know DATASPACES_DIR, then we can know DATASPACES_INCDIR.
    dnl We don't overwrite DATASPACES_INCDIR.
    if test -n "${DATASPACES_DIR}" -a -z "${DATASPACES_INCDIR}"; then
            DATASPACES_INCDIR="${DATASPACES_DIR}/include";
    dnl We may have DATASPACES denoting the dir (e.g. on ewok BUT on franklin it contains all flags)
    elif test -n "${DATASPACES}" -a -d "${DATASPACES}"; then
            DATASPACES_INCDIR="${DATASPACES}/include"
    fi

    dnl If we know DATASPACES_DIR, then we can know DATASPACES_LIBDIR.
    dnl We don't overwrite DATASPACES_LIBDIR.
    if test -n "${DATASPACES_DIR}" -a -z "${DATASPACES_LIBDIR}"; then
            DATASPACES_LIBDIR="${DATASPACES_DIR}/lib";
    dnl We may have DATASPACES denoting the dir (e.g. on ewok BUT on franklin it contains all flags)
    elif test -n "${DATASPACES}" -a -d "${DATASPACES}"; then
            DATASPACES_LIBDIR="${DATASPACES}/lib"
    fi

    dnl Add "-I" to DATASPACES_INCDIR.
    if test -n "${DATASPACES_INCDIR}"; then
            DATASPACES_CPPFLAGS="-I${DATASPACES_INCDIR}"
    else
            ac_dataspaces_ok=no
    fi

    dnl Add "-L" to DATASPACES_LIBDIR.
    if test -n "${DATASPACES_LIBDIR}"; then
            DATASPACES_LDFLAGS="-L${DATASPACES_LIBDIR}"
    else
            ac_dataspaces_ok=no
    fi

    save_CPPFLAGS="$CPPFLAGS"
    save_LIBS="$LIBS"
    save_LDFLAGS="$LDFLAGS"
#    LIBS="$LIBS -ldart2 -lspaces"
    LIBS="$LIBS -ldart -ldataspaces"
    LDFLAGS="$LDFLAGS $DATASPACES_LDFLAGS"
    CPPFLAGS="$CPPFLAGS $DATASPACES_CPPFLAGS"
    
    if test -z "${HAVE_DATASPACES_TRUE}"; then
#            AC_CHECK_HEADERS(dataspaces.h,
            AC_CHECK_HEADERS(dart_interface.h,
                    ,
                    [AM_CONDITIONAL(HAVE_DATASPACES,false)])
    fi
    
    # Check for the Mini-XML library and headers
#    AC_TRY_COMPILE([#include "dataspaces.h"],
#            [int err; err = dataspaces_init(1,1);],
#            [DATASPACES_LIBS="-ldart2 -lspaces"],
#            [AM_CONDITIONAL(HAVE_DATASPACES,false)])
    AC_TRY_COMPILE([#include "dart_interface.h"],
            [int err; err = dart_init(1,1);],
            [DATASPACES_LIBS="-ldart -ldataspaces -lrdmacm -libverbs"],
            [AM_CONDITIONAL(HAVE_DATASPACES,false)])
    
    LIBS="$save_LIBS"
    LDFLAGS="$save_LDFLAGS"
    CPPFLAGS="$save_CPPFLAGS"
    
    AC_SUBST(DATASPACES_LIBS)
    AC_SUBST(DATASPACES_LDFLAGS)
    AC_SUBST(DATASPACES_CPPFLAGS)
    
    # Finally, execute ACTION-IF-FOUND/ACTION-IF-NOT-FOUND:
    if test -z "${HAVE_DATASPACES_TRUE}"; then
            ifelse([$1],,[AC_DEFINE(HAVE_DATASPACES,1,[Define if you have the DATASPACES.])],[$1])
            :
    else
            $2
            :
    fi

fi
])dnl AC_DATASPACES