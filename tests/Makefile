SRCS=$(wildcard test*.c)
PROGS=$(patsubst %.c,%,  $(SRCS))
OBJS =$(patsubst %.c,%.o,$(SRCS))

CFLAGS = -I../src -I../lib
CFLAGS += -Wall -Wextra
#CFLAGS += -g -O0
CFLAGS += -O2

STATIC_OBJS=shr.o bw.o ux.o

all: $(STATIC_OBJS) $(PROGS) tests

# rather than using libtool which turns our 
# tests into shell scripts, to dynamically
# link the pre-installed ../src/libshr.so,
# this Makefile builds the ../src/ objects
# itself and links them right in statically
$(STATIC_OBJS):
	$(CC) -c $(CFLAGS) ../src/shr.c
	$(CC) -c $(CFLAGS) ../lib/bw.c
	$(CC) -c $(CFLAGS) ../lib/ux.c

perf perf-farm: $(STATIC_OBJS)
	$(CC) -o $@ $(CFLAGS) $@.c $(STATIC_OBJS)

# static pattern rule: multiple targets 
$(OBJS): %.o: %.c
	$(CC) -c $(CFLAGS) $< 

$(PROGS): %: %.o $(STATIC_OBJS)
	$(CC) -o $@ $(CFLAGS) $< $(STATIC_OBJS) $(LDFLAGS)

.PHONY: clean tests

tests:
	perl ./do_tests

clean:	
	rm -f $(PROGS) *.out *.ring *.o
