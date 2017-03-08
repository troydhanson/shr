srcdir = @srcdir@

bin_PROGRAMS = shr-pub 
noinst_PROGRAMS = shrtool shr-template

shr_pub_SOURCES = shr-pub.c
shr_pub_CPPFLAGS = -I$(srcdir)/..
shr_pub_LDADD = -L.. -lshr

shrtool_SOURCES = shrtool.c
shrtool_CPPFLAGS = -I$(srcdir)/..
shrtool_LDADD = -L.. -lshr

shr_template_SOURCES = shr-template.c
shr_template_CPPFLAGS = -I$(srcdir)/..
shr_template_LDADD = -L.. -lshr

