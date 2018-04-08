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
#define F 2

char *ring = "/dev/shm/" __FILE__ ".ring";

/* make an enum and a char*[] for the ops */
#define adim(x) (sizeof(x)/sizeof(*x))
#define OPS o(do_none) o(do_open) o(do_fill) o(do_read_half) o(do_read_one) \
 o(do_write_one) o(do_write_one_2b) o(do_write_one_3b) o(do_write_one_4b) o(do_stat) o(do_farm_stat) o(do_close) o(do_exit)
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
  int who;
  ops op;
} ev[] = {
    { W, do_open},
    { R, do_open},

    { W, do_write_one},     /* write A */
    { W, do_write_one_2b},  /* write BB */
    { W, do_write_one_3b},  /* write CCC */
    { W, do_write_one_4b},  /* write DDDD */

    { R, do_stat},          /* mu 4 */
 
    { R, do_read_one},      /* read A */
    { R, do_stat},          /* mu 3 */

    { R, do_read_one},      /* read BB */
    { R, do_read_one},      /* read CCC */
    { R, do_read_one},      /* read DDDD */
    { R, do_read_one},      /* read-no-data */

    { R, do_stat},          /* mu 0 */
 
    { W, do_close},
    { F, do_close},
    { R, do_close},

    { W, do_exit},
    { F, do_exit},
    { R, do_exit},
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
  unsigned seq = 'a';
  unsigned i, n;
  ssize_t nr;
  size_t d;
  int sc;
  struct shr_stat stat;

  for(i=0; i < adim(ev); i++) {

    if ( ev[i].who != me ) continue;

    sleep_til( i * CF.speed );
    if (me == R) printf("r: ");
    if (me == F) printf("f: ");
    if (me == W) printf("w: ");
    printf("%s\n", op_s[ ev[i].op ]);

    switch( ev[i].op ) {
      case do_open:
        s = shr_open(ring, 
          ((me == R) || (me == F))    ?
          (SHR_RDONLY | SHR_NONBLOCK) : 
          SHR_WRONLY);
        if (s == NULL) goto done;
        break;
      case do_close:
        shr_close(s);
        break;
      case do_exit:
        goto done;
        break;
      case do_stat:
        sc = shr_stat(s, &stat, NULL);
        printf("w: stat: %d\n", sc);
        printf("w: bw %ld, br %ld, mw %ld, mr %ld, md %ld, bd %ld, bn %ld, bu %ld, mu %ld\n",
              stat.bw, stat.br, stat.mw, stat.mr, stat.md, stat.bd, stat.bn, stat.bu, stat.mu);
        break;
      case do_farm_stat:
        d = shr_farm_stat(s, 0);
        printf("drops: %zu\n", d);
        break;
      case do_read_one:
        nr = shr_read(s, msg_one, sizeof(msg_one));
        if (nr <= 0) 
          printf("shr_read: %s\n", nr ? "error" : "no data");
        else
          printf("%.*s\n", (int)nr, msg_one);
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
      case do_write_one:
        msg_one[0] = seq;
        nr = shr_write(s, msg_one, 1);
        printf("w: wrote seq %c\n", seq);
        seq++;
        break;
      case do_write_one_2b:
        msg_one[0] = seq;
        msg_one[1] = seq;
        nr = shr_write(s, msg_one, 2);
        printf("w: wrote seq %c%c\n", seq, seq);
        seq++;
        break;
      case do_write_one_3b:
        msg_one[0] = seq;
        msg_one[1] = seq;
        msg_one[2] = seq;
        nr = shr_write(s, msg_one, 3);
        printf("w: wrote seq %c%c%c\n", seq, seq, seq);
        seq++;
        break;
      case do_write_one_4b:
        msg_one[0] = seq;
        msg_one[1] = seq;
        msg_one[2] = seq;
        msg_one[3] = seq;
        nr = shr_write(s, msg_one, 4);
        printf("w: wrote seq %c%c%c%c\n", seq, seq, seq, seq);
        seq++;
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
  if (me == R) printf("r: ");
  if (me == F) printf("f: ");
  if (me == W) printf("w: ");
  printf("exiting\n");
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
  pid_t rpid,wpid,fpid;

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

  fpid = fork();
  if (fpid < 0) goto done;
  if (fpid == 0) execute(F);
  assert(fpid > 0);

  waitpid(wpid,NULL,0);
  waitpid(rpid,NULL,0);
  waitpid(fpid,NULL,0);

done:
  if (CF.unlink) unlink(ring);
  printf("end\n");
  return rc;
}

