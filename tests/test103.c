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
  struct shr *r=NULL;
  int rc = -1, sc;
  char buf[5];
  ssize_t nr;

  unlink(ring);
  sc = shr_init(ring, ring_sz, SHR_DROP|SHR_MAXMSGS_2, nmsg);
  if (sc < 0) {
    fprintf(stderr, "shr_init: error\n");
    goto done;
  }

  s = shr_open(ring, SHR_WRONLY);
  r = shr_open(ring, SHR_RDONLY);

  nr = shr_write(s, "aa", 2);
  assert(nr == 2);
  nr = shr_write(s, "bb", 2);
  assert(nr == 2);
  nr = shr_write(s, "ccc", 3);
  assert(nr == 3);
  stats(s);

  nr = shr_write(s, "dd", 2); /* ddbbccc */
  assert(nr == 2);
  nr = shr_write(s, "ee", 2); /* ddeeccc */
  assert(nr == 2);
  nr = shr_write(s, "ff", 2); /* ddeeffc */
  assert(nr == 2);
  nr = shr_write(s, "g", 1);  /* ddeeffg */
  assert(nr == 1);
  stats(s);

  nr = shr_read(r, buf, sizeof(buf));
  assert(nr == 2);
  printf("read %.*s\n", (int)nr, buf);

  nr = shr_read(r, buf, sizeof(buf));
  assert(nr == 2);
  printf("read %.*s\n", (int)nr, buf);

  nr = shr_read(r, buf, sizeof(buf));
  assert(nr == 2);
  printf("read %.*s\n", (int)nr, buf);

  nr = shr_read(r, buf, sizeof(buf));
  assert(nr == 1);
  printf("read %.*s\n", (int)nr, buf);


done:
  if (s) shr_close(s);
  if (r) shr_close(r);
  unlink(ring);
  return rc;
}

