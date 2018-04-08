srcdir = @srcdir@

# build as convenience libs

noinst_LTLIBRARIES = libbw.la

libbw_la_CFLAGS = -Wall -Wextra
libbw_la_CPPFLAGS = -I$(srcdir)/lib
libbw_la_SOURCES = bw.c bw.h ux.c

# this is a testing tool
noinst_PROGRAMS = bw-tool
bw_tool_SOURCES = bw-tool.c bw.c bw.h ux.c
