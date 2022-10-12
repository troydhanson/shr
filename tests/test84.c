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
int ring_sz = 6;
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
  char buf[4];
  ssize_t nr;

  unlink(ring);
  sc = shr_init(ring, ring_sz, SHR_DROP|SHR_MAXMSGS_2, nmsg);
  if (sc < 0) {
    fprintf(stderr, "shr_init: error\n");
    goto done;
  }

  s = shr_open(ring, SHR_WRONLY);
  r = shr_open(ring, SHR_RDONLY);

  nr = shr_write(s, "a", 1);
  assert(nr == 1);

  nr = shr_write(s, "b", 1);
  assert(nr == 1);

  nr = shr_write(s, "cccc", 4);
  assert(nr == 4);
  stats(s);

  nr = shr_read(r, buf, sizeof(buf));
  assert(nr == 1);
  printf("read %.*s\n", (int)nr, buf);

  nr = shr_write(s, "d", 1); /* space exists; replaces consumed a  */
  assert(nr == 1);
  stats(s);

  nr = shr_write(s, "e", 1); /* causes drop of unread b */
  assert(nr == 1);
  stats(s);

done:
  if (s) shr_close(s);
  if (r) shr_close(r);
  unlink(ring);
  return rc;
}

