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

#define W 0
#define R 1

char *ring =  __FILE__ ".ring";

/* make an enum and a char*[] for the ops */
#define adim(x) (sizeof(x)/sizeof(*x))
#define OPS o(do_none) o(do_open) o(do_fill) o(do_close) o(do_exit)
#define o(x) #x,
char *op_s[] = { OPS };
#undef o
#define o(x) x,
typedef enum { OPS } ops;

#define NMSG 10000
char msg[] = "1234567890abcdefghijklmnopqrstuvwxyz";
const int ring_sz = (sizeof(msg)+0)*NMSG;
struct iovec iov[NMSG];

struct {
  char *prog;
  time_t start;
  int verbose;
  int speed;
  struct iovec *io;
} CF = {
  .speed = 1,
  .io = iov,
};

/* the event sequence we want executed */
struct events {
  int when;
  int who;
  ops op;
} ev[] = {
    {1, W, do_open},
    {2, W, do_fill},
    {3, W, do_close},
    {4, W, do_exit},
    {5, R, do_exit},
};

/* sleep til X seconds since start */
void sleep_til( int el ) {
  time_t now;
  time(&now);

  if (now > CF.start + el) {
    fprintf(stderr, "sleep_til: already elapsed\n");
    return;
  }

  sleep((CF.start + el) - now);
}

/* run the event sequence 
 * runs in child process. never returns 
 */
void execute(int me) {
  struct shr *s = NULL;
  unsigned i, n;
  ssize_t nr;

  for(i=0; i < adim(ev); i++) {

    if ( ev[i].who != me ) continue;

    sleep_til( ev[i].when * CF.speed );
    printf("%s: %s\n", (me == R) ? "r" : "w", op_s[ ev[i].op ]);

    switch( ev[i].op ) {
      case do_open:
        s = shr_open(ring, (me == R) ? SHR_RDONLY : SHR_WRONLY);
        if (s == NULL) goto done;
        break;
      case do_close:
        shr_close(s);
        break;
      case do_exit:
        goto done;
        break;
      case do_fill:
        for(n=0; n < NMSG; n++) {
          CF.io[n].iov_len = sizeof(msg);
          CF.io[n].iov_base = msg;
        }
        nr = shr_writev(s, CF.io, NMSG);
        printf("w: wrote %d bytes / %d messages\n", (int)nr, NMSG);
        break;
      default:
        fprintf(stderr,"op not implemented\n");
        assert(0);
        break;
    }
  }

 done:
  printf("%s: exiting\n", (me == R) ? "r" : "w");
  exit(0);
}

void usage() {
  fprintf(stderr,"usage: %s [-v] [-s <slowdown>]\n", CF.prog);
  fprintf(stderr,"-s <slowdown> (factor to slow test [def: 1])\n");
  fprintf(stderr,"-v verbose\n");
  exit(-1);
}

int main(int argc, char *argv[]) {
  setlinebuf(stdout);
  int rc = -1, opt;
  pid_t rpid,wpid;

  while ( (opt = getopt(argc,argv,"vhs:")) > 0) {
    switch(opt) {
      case 'v': CF.verbose++; break;
      case 's': CF.speed = atoi(optarg); break;
      case 'h': default: usage(); break;
    }
  }

  time(&CF.start);
  unlink(ring);
  shr_init(ring, ring_sz, SHR_MAXMSGS_2, NMSG);

  rpid = fork();
  if (rpid < 0) goto done;
  if (rpid == 0) execute(R);
  assert(rpid > 0);

  wpid = fork();
  if (wpid < 0) goto done;
  if (wpid == 0) execute(W);
  assert(wpid > 0);

  waitpid(wpid,NULL,0);
  waitpid(rpid,NULL,0);

done:
  unlink(ring);
  printf("end\n");
  return rc;
}

