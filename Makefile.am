srcdir = @srcdir@

noinst_LIBRARIES =libshr.a
libshr_a_SOURCES = shr.c shr.h
libshr_a_CFLAGS = -Wall -Wextra 

noinst_PROGRAMS = shrtool
shrtool_SOURCES = shrtool.c
shrtool_CPPFLAGS = -I$(srcdir)
shrtool_LDADD = -L. -lshr
