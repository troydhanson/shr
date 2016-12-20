#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include "shr.h"


char msg[] = "abc";
#define ring_sz (sizeof(msg)*10)
#define adim(a) (sizeof(a)/sizeof(*a))

char *ring = __FILE__ ".ring";

void delay() { usleep(50000); }

#define do_open   'o'
#define do_close  'c'
#define do_write  'w'
#define do_writec 'W'
#define do_read   'r'
#define do_readv  'v'
#define do_unlink 'u'
#define do_select 's'
#define do_get_fd 'g'
#define do_empty  'e'
#define do_fill   'f'

void r(int fd) {
  shr *s = NULL;
  char op, c;
  int rc, selectable_fd=-1;

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
      case do_readv:
        printf("r: readv\n");
        struct iovec io[10], *ov;
        char buf[100];
        int niov = 0;
        rc = shr_readv(s, buf, sizeof(buf), io, &niov);
        printf("r: rc %d\n", rc);
        printf("r: niov %d\n", niov);
        if (rc > 0) {
          ov = io;
          while(niov--) {
            printf("[%.*s]\n", (int)(ov->iov_len), (char*)(ov->iov_base));
            ov++;
          }
        }
        break;
      case do_read:
        do {
          printf("r: read\n");
          rc = shr_read(s, &c, sizeof(c)); // byte read
          if (rc > 0) printf("r: [%c]\n", c);
          if (rc == 0) printf("r: wouldblock\n");
        } while (rc > 0);
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
  int rc;

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
      case do_writec:
        printf("w: writec\n");
	      struct iovec iovc;
	      iovc.iov_base = msg;
	      iovc.iov_len = 1;
        rc = shr_writev(s, &iovc, 1);
        if (rc != 1) printf("w: rc %d\n", rc);
        break;
      case do_write:
        printf("w: write\n");
	      struct iovec iov;
	      iov.iov_base = msg;
	      iov.iov_len = 3;
        rc = shr_writev(s, &iov, 1);
        if (rc != 3) printf("w: rc %d\n", rc);
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

#define issue(fd,op) do {                           \
  char cmd = op;                                    \
  if (write((fd), &cmd, sizeof(cmd)) < 0) {         \
    fprintf(stderr,"write: %s\n", strerror(errno)); \
    goto done;                                      \
  }                                                 \
} while(0)

int main() {
  int rc = 0;
  pid_t rpid,wpid;

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

  issue(W, do_open); delay();
  issue(R, do_open); delay();

  issue(W, do_write); delay(); /* abc */
  issue(W, do_write); delay(); /* abcabc */
  issue(W, do_write); delay(); /* abcabcabc */

  issue(R, do_readv); delay();

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
