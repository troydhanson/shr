AC_INIT([shr], 1.0)
AM_INIT_AUTOMAKE([foreign subdir-objects])
m4_ifdef([AM_SILENT_RULES],
    [AM_SILENT_RULES([yes])
    ])
AC_PROG_CC
AC_PROG_RANLIB

dnl need AC_GNU_SOURCE for open(2) to get O_PATH
AC_GNU_SOURCE

# is ncurses installed
AC_CHECK_LIB(ncurses,initscr,
  AM_CONDITIONAL(HAVE_NCURSES,true),
  AM_CONDITIONAL(HAVE_NCURSES,false))

have_o_path=n
AC_CHECK_DECL([O_PATH],[have_o_path=y],[have_o_path=n],[
#include<fcntl.h>])
if test "x${have_o_path}" != xy
then
  AC_MSG_ERROR([
  -----------------------------------------------------
  libshr requires support for open(dir, O_PATH) which
  was introduced in Linux 2.6.39.
  -----------------------------------------------------
  ])
fi


AC_CONFIG_FILES([Makefile
   src/Makefile
   util/Makefile
   ])
AC_OUTPUT
