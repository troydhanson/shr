#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include "shr.h"

/* this test is only valid on platforms where sizeof(size_t) == 8 */

char msg[]  = "abcdef";

int nmsg = 2;
#define ring_sz (((sizeof(msg) + sizeof(size_t)) * nmsg) + 1)

char *ring = __FILE__ ".ring";

void delay() { usleep(50000); }

#define do_open   'o'
#define do_stat   't'
#define do_close  'c'
#define do_write  'w'
#define do_read   'r'
#define do_unlink 'u'
#define do_select 's'
#define do_get_fd 'g'
#define do_empty  'e'
#define do_fill   'f'

void print_elapsed(char *name, struct timeval *start, struct timeval *end, int nmsg) {
  unsigned long usec_start, usec_end, usec_elapsed;
  usec_end = end->tv_sec * 1000000 + end->tv_usec;
  usec_start = start->tv_sec * 1000000 + start->tv_usec;
  usec_elapsed = usec_end - usec_start;
  double msgs_per_sec = usec_elapsed  ? (nmsg * 1000000.0 / usec_elapsed) : 0;
  fprintf(stderr,"%s: %f msgs/sec\n", name, msgs_per_sec);
}

__attribute__ ((__unused__)) static void hexdump(char *buf, size_t len) {
  size_t i,n=0;
  char c;
  while(n < len) {
    for(i=0; i < 16; i++) {
      c = (n+i < len) ? buf[n+i] : 0;
      if (n+i < len) fprintf(stderr,"%.2x ", c);
      else fprintf(stderr, "   ");
    }
    for(i=0; i < 16; i++) {
      c = (n+i < len) ? buf[n+i] : ' ';
      if (c < 0x20 || c > 0x7e) c = '.';
      fprintf(stderr,"%c",c);
    }
    fprintf(stderr,"\n");
    n += 16;
  }
}

void r(int fd) {
  shr *s = NULL;
  char op, buf[sizeof(msg)];
  int rc, selectable_fd=-1, n;
  struct timeval tv_start, tv_end;

  printf("r: ready\n");

  for(;;) {
    rc = read(fd, &op, sizeof(op));
    if (rc < 0) {
      fprintf(stderr,"r read: %s\n", strerror(errno));
      goto done;
    }
    if (rc == 0) {
      printf("r: eof\n");
      goto done;
    }
    assert(rc == sizeof(op));
    switch(op) {
      case do_open:
        s = shr_open(ring, SHR_RDONLY|SHR_NONBLOCK);
        if (s == NULL) goto done;
        printf("r: open\n");
        break;
      case do_get_fd:
        printf("r: get selectable fd\n");
        selectable_fd = shr_get_selectable_fd(s);
        if (selectable_fd < 0) goto done;
        break;
      case do_select:
        printf("r: select\n");
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(selectable_fd, &fds);
        struct timeval tv = {.tv_sec = 1, .tv_usec =0};
        rc = select(selectable_fd+1, &fds, NULL, NULL, &tv);
        if (rc < 0) printf("r: select %s\n", strerror(errno));
        else if (rc == 0) printf("r: timeout\n");
        else if (rc == 1) printf("r: ready\n");
        else assert(0);
        break;
      case do_read:
        printf("r: read\n");
        rc = shr_read(s, buf, sizeof(buf)); // msg read
        printf("r: rc = %d\n", rc);
        //hexdump(buf,rc);
        if (rc > 0) printf("r: [%.*s]\n", rc, buf);
        if (rc == 0) printf("r: wouldblock\n");
        break;
      case do_empty:
        // printf("r: empty\n");
        gettimeofday(&tv_start,NULL);
        for(n=0; n < nmsg; n++) {
          rc = shr_read(s, buf, sizeof(buf));
          //fprintf(stderr,"r\n");
          if (rc != sizeof(buf)) printf("r: %d != %d\n", (int)rc, (int)sizeof(buf));
        }
        gettimeofday(&tv_end,NULL);
        print_elapsed("r", &tv_start,&tv_end,nmsg);
        break;
      case do_close:
        assert(s);
        shr_close(s);
        printf("r: close\n");
        break;
      default:
        assert(0);
        break;
    }
  }
 done:
  exit(0);
}

void w(int fd) {
  shr *s = NULL;
  char op;
  int rc, n;
  struct timeval tv_start, tv_end;
  struct shr_stat stat;

  printf("w: ready\n");

  for(;;) {
    rc = read(fd, &op, sizeof(op));
    if (rc < 0) {
      fprintf(stderr,"w read: %s\n", strerror(errno));
      goto done;
    }
    if (rc == 0) {
      printf("w: eof\n");
      goto done;
    }
    assert(rc == sizeof(op));
    switch(op) {
      case do_open:
        s = shr_open(ring, SHR_WRONLY);
        if (s == NULL) goto done;
        printf("w: open\n");
        break;
      case do_write:
        printf("w: write\n");
        rc = shr_write(s, msg, sizeof(msg));
        if (rc != sizeof(msg)) printf("w: rc %d\n", rc);
        break;
      case do_fill:
        gettimeofday(&tv_start,NULL);
        // printf("w: fill\n");
        for(n=0; n < nmsg; n++) {
          rc = shr_write(s, msg, sizeof(msg));
          //fprintf(stderr,"w\n");
          if (rc != sizeof(msg)) printf("w: rc %d\n", rc);
        }
        gettimeofday(&tv_end,NULL);
        print_elapsed("w", &tv_start,&tv_end,nmsg);
        break;
      case do_unlink:
        printf("w: unlink\n");
        unlink(ring);
        break;
      case do_stat:
        //if (gettimeofday(&tv_start, NULL) != 0) goto done;
        rc = shr_stat(s, &stat, NULL);
        printf("w: stat: %d\n", rc);
        printf("w: bw %ld, br %ld, mw %ld, mr %ld, md %ld, bd %ld, bn %ld, bu %ld mu %ld\n",
              stat.bw, stat.br, stat.mw, stat.mr, stat.md, stat.bd, stat.bn, stat.bu, stat.mu);
        break;
      case do_close:
        assert(s);
        shr_close(s);
        printf("w: close\n");
        break;
      default:
        assert(0);
        break;
    }
  }
 done:
  exit(0);
}

#define issue(fd,op) do {                           \
  char cmd = op;                                    \
  if (write((fd), &cmd, sizeof(cmd)) < 0) {         \
    fprintf(stderr,"write: %s\n", strerror(errno)); \
    goto done;                                      \
  }                                                 \
  delay();                                          \
} while(0)

int main() {
  int rc = 0;
  pid_t rpid,wpid;
  assert(sizeof(size_t) == 8); // this TEST only (shr does not make this assumption)

  setbuf(stdout,NULL);
  unlink(ring);

  int pipe_to_r[2];
  int pipe_to_w[2];

  shr_init(ring, ring_sz, SHR_MESSAGES|SHR_LRU_DROP);

  if (pipe(pipe_to_r) < 0) goto done;

  rpid = fork();
  if (rpid < 0) goto done;
  if (rpid == 0) { /* child */
    close(pipe_to_r[1]);
    r(pipe_to_r[0]);
    assert(0); /* not reached */
  }

  /* parent */
  close(pipe_to_r[0]);

  delay();

  if (pipe(pipe_to_w) < 0) goto done;
  wpid = fork();
  if (wpid < 0) goto done;
  if (wpid == 0) { /* child */
    close(pipe_to_r[1]);
    close(pipe_to_w[1]);
    w(pipe_to_w[0]);
    assert(0); /* not reached */
  }
  /* parent */
  close(pipe_to_w[0]);

  int R = pipe_to_r[1];
  int W = pipe_to_w[1];

  issue(W, do_open); delay();
  issue(R, do_open); delay();

  issue(W, do_stat); delay();
                      /* 0000000000111111111122222222223 */ // indexes
                      /* 0123456789012345678901234567890 */ // indexes
                      /* _______________________________ */ // contents _ unused
  issue(W, do_write); /* MSG_SIZEabcdef0________________ */
  issue(W, do_stat); delay();
                      /* ^                               */
  issue(W, do_write); /* MSG_SIZEabcdef0MSG_SIZEabcdef0_ */
  issue(W, do_stat); delay();
                      /* ^                               */
  issue(W, do_write); /* SG_SIZEabcdef0_MSG_SIZEabcdef0M */ // DROP oldest msg
                      /*                ^                */
  issue(W, do_stat); delay();

  issue(R, do_read);  /*                        abcdef0^ */
  issue(W, do_stat); delay();
  issue(R, do_read);  /*        abcdef0 .                */
  issue(W, do_stat); delay();
  issue(R, do_read);  /* wouldblock                      */

  issue(W, do_stat); delay();
  issue(W, do_close); delay();
  issue(R, do_close); delay();

  close(W); delay();
  close(R);

  waitpid(wpid,NULL,0);
  waitpid(rpid,NULL,0);

 rc = 0;

done:
 unlink(ring);
 printf("end\n");
 return rc;
}
