srcdir = @srcdir@

bin_PROGRAMS = shr-tool shr-socket


shr_tool_SOURCES = shr-tool.c
shr_tool_CPPFLAGS = -I$(srcdir)/../src
shr_tool_LDADD = -L../src -lshr

shr_socket_SOURCES = shr-socket.c
shr_socket_CPPFLAGS = -I$(srcdir)/../src
shr_socket_LDADD = -L../src -lshr

#
# noinst_PROGRAMS = shr-read-template
# shr_read_template_SOURCES = shr-read-template.c
# shr_read_template_CPPFLAGS = -I$(srcdir)/../src
# shr_read_template_LDADD = -L../src -lshr

# bin_PROGRAMS += shr-pub 
# shr_pub_SOURCES = shr-pub.c
# shr_pub_CPPFLAGS = -I$(srcdir)/../src
# shr_pub_LDADD = -L../src -lshr
