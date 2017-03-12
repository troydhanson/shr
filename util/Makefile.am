srcdir = @srcdir@

bin_PROGRAMS = shr-pub shr-tool 
noinst_PROGRAMS = shr-read-template

shr_pub_SOURCES = shr-pub.c
shr_pub_CPPFLAGS = -I$(srcdir)/../src
shr_pub_LDADD = -L../src -lshr

shr_tool_SOURCES = shr-tool.c
shr_tool_CPPFLAGS = -I$(srcdir)/../src
shr_tool_LDADD = -L../src -lshr

shr_read_template_SOURCES = shr-read-template.c
shr_read_template_CPPFLAGS = -I$(srcdir)/../src
shr_read_template_LDADD = -L../src -lshr

