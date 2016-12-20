#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include "shr.h"

/* this test is special. it doubles as a performance test */

/* test message is 36*10 + 1 bytes. 361 bytes just for realism */
char msg[]  =      "1234567890abcdefghijklmnopqrstuvwxyz"
                   "!@#$%^&*()ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                   "1234567890abcdefghijklmnopqrstuvwxyz"
                   "!@#$%^&*()ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                   "1234567890abcdefghijklmnopqrstuvwxyz"
                   "!@#$%^&*()ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                   "1234567890abcdefghijklmnopqrstuvwxyz"
                   "!@#$%^&*()ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                   "1234567890abcdefghijklmnopqrstuvwxyz"
                   "!@#$%^&*()ABCDEFGHIJKLMNOPQRSTUVWXYZ";

int nmsg = 1000000;
#define ring_sz (sizeof(msg) * nmsg)

char *ring = __FILE__ ".ring";

void delay() { usleep(50000); }

#define do_open   'o'
#define do_close  'c'
#define do_write  'w'
#define do_read   'r'
#define do_unlink 'u'
#define do_select 's'
#define do_get_fd 'g'
#define do_empty  'e'
#define do_fill   'f'

void r(int fd) {
  shr *s = NULL;
  char op, c, buf[sizeof(msg)];
  int rc, selectable_fd=-1, n;

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
        s = shr_open(ring, SHR_RDONLY);
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
        do {
          printf("r: read\n");
          rc = shr_read(s, &c, sizeof(c)); // byte read
          if (rc > 0) printf("r: [%c]\n", c);
          if (rc == 0) printf("r: wouldblock\n");
        } while (rc > 0);
        break;
      case do_empty:
        printf("r: empty\n");
        for(n=0; n < nmsg; n++) {
          rc = shr_read(s, buf, sizeof(buf));
          if (rc != sizeof(buf)) printf("r: %d != %d\n", (int)rc, (int)sizeof(buf));
        }
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
  struct iovec *io;

  printf("w: ready\n");
  io = calloc(nmsg, sizeof(*io));
  if (io == NULL) {
      fprintf(stderr,"w oom\n");
      goto done;
  }

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
        printf("w: fill\n");
        for(n=0; n < nmsg; n++) {
          io[n].iov_len = sizeof(msg);
          io[n].iov_base = msg;
        }
        rc = shr_writev(s, io, nmsg);
        if (rc != (int)(nmsg*sizeof(msg))) printf("w: rc %d\n", rc);
        break;
      case do_unlink:
        printf("w: unlink\n");
        unlink(ring);
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

void print_elapsed(struct timeval *start, struct timeval *end, int nmsg) {
  unsigned long usec_start, usec_end, usec_elapsed;
  usec_end = end->tv_sec * 1000000 + end->tv_usec;
  usec_start = start->tv_sec * 1000000 + start->tv_usec;
  usec_elapsed = usec_end - usec_start;
  double msgs_per_sec = usec_elapsed  ? (nmsg * 1000000.0 / usec_elapsed) : 0;
  fprintf(stderr,"%f msgs/sec\n", msgs_per_sec);
}

#define issue(fd,op) do {                           \
  char cmd = op;                                    \
  if (write((fd), &cmd, sizeof(cmd)) < 0) {         \
    fprintf(stderr,"write: %s\n", strerror(errno)); \
    goto done;                                      \
  }                                                 \
} while(0)

int main() {
  int rc = 0;
  pid_t rpid,wpid,pid;
  struct timeval tv_start, tv_end;
  int status;

  setbuf(stdout,NULL);
  unlink(ring);

  int pipe_to_r[2];
  int pipe_to_w[2];

  shr_init(ring, ring_sz, 0);

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

  delay();

  printf("%ld msgs total %ld bytes\n", (long)nmsg, (long)nmsg*sizeof(msg));

  printf("writing\n");
  gettimeofday(&tv_start,NULL);
  issue(W, do_open);
  issue(W, do_fill);
  issue(W, do_close);
  close(W);
  pid = waitpid(wpid,&status,0);
  if (pid < 0) printf("waitpid: %s\n", strerror(errno));
  if (WIFSIGNALED(status)) printf("w signal exit\n");
  gettimeofday(&tv_end,NULL);
  print_elapsed(&tv_start,&tv_end,nmsg);
  
  printf("reading\n");
  gettimeofday(&tv_start,NULL);
  issue(R, do_open);
  issue(R, do_empty);
  issue(R, do_close);
  close(R);
  pid = waitpid(rpid,&status,0);
  if (pid < 0) printf("waitpid: %s\n", strerror(errno));
  if (WIFSIGNALED(status)) printf("r signal exit\n");
  gettimeofday(&tv_end,NULL);
  print_elapsed(&tv_start,&tv_end,nmsg);

 rc = 0;

done:
 unlink(ring);
 printf("end\n");
 return rc;
}
