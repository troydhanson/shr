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
#define OPS o(do_none) o(do_open) o(do_write) o(do_writev) \
            o(do_read) o(do_readv) o(do_close) o(do_exit)
#define o(x) #x,
char *op_s[] = { OPS };
#undef o
#define o(x) x,
typedef enum { OPS } ops;

#define NMSG 1000000
char msg[] = "1234567890abcdefghijklmnopqrstuvwxyz";
const int ring_sz = (sizeof(msg)+0)*NMSG;
struct iovec iov[NMSG];
char msg_all[sizeof(msg)*NMSG];

struct {
  char *prog;
  time_t start;
  int verbose;
  int speed;
  char s[100];
} CF = {
  .speed = 1,
};

/* the event sequence we want executed */
struct events {
  int when;
  int who;
  ops op;
} ev[] = {
    {0, W, do_open},
    {1, R, do_open},

    {2, W, do_write},
    {3, R, do_read},

    {4, W, do_writev},
    {5, R, do_readv},

    {6, W, do_close},
    {7, R, do_close},

    {8, W, do_exit},
    {9, R, do_exit},
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

/* generate a string of rate info. use result immediately as its overwritten */
char *rate(ssize_t byte, int nmsg, struct timeval *beg, struct timeval *end) {
  unsigned long beg_us, end_us, elp_us;
  assert(end->tv_sec >= beg->tv_sec);
  double mmsgs_s, mbyte_s;

  beg_us = beg->tv_sec * 1000000 + beg->tv_usec;
  end_us = end->tv_sec * 1000000 + end->tv_usec;
  elp_us = end_us - beg_us;

  /* convert msgs-per-microsecond to millions-of-messages-per-second */
  mmsgs_s = elp_us ? (nmsg / elp_us) : 0;

  /* convert bytes-per-microsecond to megabytes-per-second */
  mbyte_s = elp_us ? (byte * 1000000.0 / (1024.0 * 1024.0) / elp_us) : 0;

  if (CF.verbose) {
    printf("%.2f mmsgs/s (%d messages in %lu usec)\n", mmsgs_s, nmsg, elp_us);
    printf("%.2f mb/s\n", mbyte_s);
  }

  snprintf(CF.s, sizeof(CF.s), "%u million msgs/sec", (int)mmsgs_s);
  return CF.s;
}

/* run the event sequence 
 * runs in child process. never returns 
 */
void execute(int me) {
  struct timeval a, b;
  char msg_one[sizeof(msg)];
  struct shr *s = NULL;
  unsigned i, n;
  size_t nmsg;
  ssize_t nr;

  for(i=0; i < adim(ev); i++) {

    if ( ev[i].who != me ) continue;

    sleep_til( ev[i].when * CF.speed );
    // printf("%s: %s\n", (me == R) ? "r" : "w", op_s[ ev[i].op ]);

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
      case do_read:
        gettimeofday(&a, NULL);
        for(n=0; n < NMSG; n++) {
          nr = shr_read(s, msg_one, sizeof(msg_one));
          if (nr != sizeof(msg)) printf("shr_write: %zu\n", nr);
        }
        gettimeofday(&b, NULL);
        printf("shr_read:   %s\n", rate(nr*NMSG,NMSG,&a,&b));
        break;
      case do_write:
        gettimeofday(&a, NULL);
        for(n=0; n < NMSG; n++) {
          nr = shr_write(s, msg, sizeof(msg));
          if (nr != sizeof(msg)) printf("shr_write: %zu\n", nr);
        }
        gettimeofday(&b, NULL);
        printf("shr_write:  %s\n", rate(nr*NMSG,NMSG,&a,&b));
        break;
      case do_readv:
        nmsg = NMSG;
        gettimeofday(&a, NULL);
        nr = shr_readv(s, msg_all, sizeof(msg_all), iov, &nmsg);
        gettimeofday(&b, NULL);
        if ((nr != sizeof(msg_all)) || (nmsg != NMSG) ) {
          printf("shr_read: %d/%d bytes\n", (int)nr, (int)sizeof(msg_all));
          printf("shr_read: %d/%d iov\n", (int)nmsg, NMSG);
          break;
        }
        printf("shr_readv:  %s\n", rate(nr,NMSG,&a,&b));
        break;
      case do_writev:
        for(n=0; n < NMSG; n++) {
          iov[n].iov_len = sizeof(msg_one);
          iov[n].iov_base = msg_one;
        }
        gettimeofday(&a, NULL);
        nr = shr_writev(s, iov, NMSG);
        gettimeofday(&b, NULL);
        if (nr != NMSG*sizeof(msg)) {
          printf("shr_writev: %d/%d bytes\n", (int)nr, (int)(NMSG*sizeof(msg)));
        }
        printf("shr_writev: %s\n", rate(nr,NMSG,&a,&b));
        break;
      default:
        fprintf(stderr,"op not implemented\n");
        assert(0);
        break;
    }
  }

 done:
  //printf("%s: exiting\n", (me == R) ? "r" : "w");
  exit(0);
}

void usage() {
  fprintf(stderr,"usage: %s [-v] [-s <slowdown>]\n", CF.prog);
  fprintf(stderr,"-s <slowdown> (factor to slow test [def: 1])\n");
  fprintf(stderr,"-v verbose\n");
  exit(-1);
}

int main(int argc, char *argv[]) {
  int rc = -1, opt;
  pid_t rpid,wpid;
  uid_t uid;

  while ( (opt = getopt(argc,argv,"vhs:")) > 0) {
    switch(opt) {
      case 'v': CF.verbose++; break;
      case 's': CF.speed = atoi(optarg); break;
      case 'h': default: usage(); break;
    }
  }

  uid = geteuid();
  if (uid != 0)
    fprintf(stderr, "warning: run as root to raise memory locking limits\n");

  time(&CF.start);
  shr_init(ring, ring_sz, SHR_MLOCK|SHR_MAXMSGS_2, NMSG);

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
  //printf("end\n");
  return rc;
}

