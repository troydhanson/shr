srcdir = @srcdir@

lib_LTLIBRARIES =libshr.la
include_HEADERS = shr.h
libshr_la_SOURCES = shr.c
libshr_la_CFLAGS = -Wall -Wextra 
libshr_la_CPPFLAGS = -I$(srcdir)/../lib
libshr_la_LIBADD = ../lib/libbw.la
libshr_la_LDFLAGS = --version-info 0:0:0
