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

char *ring = "/dev/shm/" __FILE__ ".ring";

/* make an enum and a char*[] for the ops */
#define adim(x) (sizeof(x)/sizeof(*x))
#define OPS o(do_none) o(do_open) o(do_fill) o(do_read_half) o(do_close) o(do_exit)
#define o(x) #x,
char *op_s[] = { OPS };
#undef o
#define o(x) x,
typedef enum { OPS } ops;

#define NMSG 10
char msg[] = "000000000000000000000000000000000000";
const int ring_sz = (sizeof(msg)+0)*NMSG;
struct iovec iov[NMSG];
char msg_all[sizeof(msg)*NMSG];

struct {
  char *prog;
  time_t start;
  int verbose;
  int speed;
  int unlink;
} CF = {
  .speed = 1,
  .unlink = 1,
};

/* the event sequence we want executed */
struct events {
  int when;
  int who;
  ops op;
} ev[] = {
    {1, R, do_open},
    {2, W, do_open},

    {3, W, do_fill},      /* w 10 */
    {4, R, do_read_half}, /* r  5 (seq 0-4) */
    {5, W, do_fill},      /* w 10 (Drop 5) */
    {6, R, do_read_half}, /* r  5 (seq 10-14) */
    {7, R, do_read_half}, /* r  5 (seq 15-19) */
    {8, R, do_close},

   { 9, R, do_open},      /* start over again */
   {10, R, do_read_half}, /* r 0 (non block no data) */
   {11, R, do_read_half}, /* r 0 (non block no data) */

   {12, R, do_close},
   {13, W, do_close},
   {14, W, do_exit},
   {15, R, do_exit},
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
  char msg_one[sizeof(msg)];
  struct shr *s = NULL;
  unsigned i, n;
  ssize_t nr;
  unsigned seq = 0;

  for(i=0; i < adim(ev); i++) {

    if ( ev[i].who != me ) continue;

    sleep_til( ev[i].when * CF.speed );
    printf("%s: %s\n", (me == R) ? "r" : "w", op_s[ ev[i].op ]);

    switch( ev[i].op ) {
      case do_open:
        s = shr_open(ring, (me == R) ? SHR_RDONLY|SHR_NONBLOCK : SHR_WRONLY);
        if (s == NULL) goto done;
        break;
      case do_close:
        shr_close(s);
        break;
      case do_exit:
        goto done;
        break;
      case do_read_half:
        for(n=0; n < NMSG/2; n++) {
          nr = shr_read(s, msg_one, sizeof(msg_one));
          if (nr != sizeof(msg)) {
            printf("shr_read: %d\n", (int)nr);
            break;
          }
          printf("%s\n", msg_one);
        }
        break;
      case do_fill:
        for(n=0; n < NMSG; n++) {
          snprintf(msg_one, sizeof(msg_one), "%u", seq++);
          nr = shr_write(s, msg_one, sizeof(msg_one));
        }
        printf("w: wrote %d messages (to seq %u)\n", NMSG, seq);
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
  fprintf(stderr,"-v verbose\n");
  fprintf(stderr,"-s <slowdown> (factor to slow test [def: 1])\n");
  fprintf(stderr,"-u            (don't unlink ring after test)\n");
  exit(-1);
}

int main(int argc, char *argv[]) {
  setlinebuf(stdout);
  int rc = -1, opt;
  pid_t rpid,wpid;

  while ( (opt = getopt(argc,argv,"vhs:u")) > 0) {
    switch(opt) {
      case 'v': CF.verbose++; break;
      case 's': CF.speed = atoi(optarg); break;
      case 'u': CF.unlink = 0; break;
      case 'h': default: usage(); break;
    }
  }

  time(&CF.start);
  unlink(ring);
  shr_init(ring, ring_sz, SHR_DROP|SHR_MAXMSGS_2, NMSG);

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
  if (CF.unlink) unlink(ring);
  printf("end\n");
  return rc;
}

