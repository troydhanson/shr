#include <sys/wait.h>
#include <inttypes.h>
#include <sys/time.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include "shr.h"

char *ring =  __FILE__ ".ring";
int ring_sz = 7;
int nmsg = 100;

void stats(struct shr *s) {
  struct shr_stat ss;
  int sc;

  sc = shr_stat(s, &ss, NULL);
  if (sc < 0) {
    fprintf(stderr, "shr_stat: error\n");
    return;
  }

  printf("bw %ld br %ld mw %ld mr %ld md %ld bd %ld bn %ld bu %ld mu %ld\n",
   ss.bw, ss.br, ss.mw, ss.mr, ss.md, ss.bd, ss.bn, ss.bu, ss.mu);
}

int main() {
  struct shr *s=NULL;
  int rc = -1, sc;
  ssize_t nr;

  unlink(ring);
  sc = shr_init(ring, ring_sz, SHR_DROP|SHR_MAXMSGS_2, nmsg);
  if (sc < 0) {
    fprintf(stderr, "shr_init: error\n");
    goto done;
  }

  s = shr_open(ring, SHR_WRONLY);

  nr = shr_write(s, "a", 1);
  assert(nr == 1);

  nr = shr_write(s, "b", 1);
  assert(nr == 1);

  nr = shr_write(s, "cccc", 4);
  assert(nr == 4);
  stats(s);

  nr = shr_write(s, "dddd", 4);
  assert(nr == 4);
  stats(s);

done:
  if (s) shr_close(s);
  unlink(ring);
  return rc;
}

