AC_INIT([shr], 1.0)
AC_CONFIG_MACRO_DIRS([m4])
dnl need AC_GNU_SOURCE for SCM_CREDENTIALS and recvmmsg
AC_GNU_SOURCE
AM_INIT_AUTOMAKE([foreign subdir-objects])
m4_ifdef([AM_SILENT_RULES],
    [AM_SILENT_RULES([yes])
    ])
LT_INIT([shared])
AC_PROG_CC


have_recvmmsg=n
AC_CHECK_DECL([recvmmsg],[have_recvmmsg=y],[have_recvmmsg=n],[
#include<sys/socket.h>])
if test "x${have_recvmmsg}" != xy
then
  AC_MSG_ERROR([
  -----------------------------------------------------
  libshr requires support for recvmmsg() which
  was introduced in Linux 2.6.33.
  -----------------------------------------------------
  ])
fi

# is ncurses installed
AC_CHECK_LIB(ncurses,initscr,
  AM_CONDITIONAL(HAVE_NCURSES,true),
  AM_CONDITIONAL(HAVE_NCURSES,false))

AC_CONFIG_FILES([Makefile
   src/Makefile
   lib/Makefile
   util/Makefile
   ])
AC_OUTPUT
