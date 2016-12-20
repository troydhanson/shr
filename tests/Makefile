PROGS=test1 test2 test3 test4 test5 test6 test7 test8 test9 test10 test11 test12 \
      test13 test14 test15 test16 test17 test18 test19 test20 test21 test22 test23 \
      test24 test25 test26 test27 test28 test29 test30 test31 test32 test33 test34 \
      test35 test36 test37 test38 test39 test40 test41 test42 test43 test44 test45 \
      test46 test47 test48 test49 test50 test51 test52 test53 test54 test55 test56 \
      test57 test58 test59
OBJS=$(patsubst %,%.o,$(PROGS))

CFLAGS = -I..
CFLAGS += -g -O0
CFLAGS += -Wall -Wextra
LDFLAGS=

TEST_TARGET=run_tests
TESTS=./do_tests

all: $(OBJS) $(PROGS) $(TEST_TARGET) 

# static pattern rule: multiple targets 

$(OBJS) ../shr.o: %.o: %.c
	$(CC) -o $@ -c $(CFLAGS) $< 

$(PROGS): %: %.o ../shr.o
	$(CC) -o $@ $(CFLAGS) $< ../shr.o $(LDFLAGS)


run_tests: $(PROGS)
	perl $(TESTS)

.PHONY: clean

clean:	
	rm -f $(PROGS) $(OBJS) *.o test*.out *.ring
