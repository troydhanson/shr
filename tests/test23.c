#include <stdio.h>
#include <sys/wait.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include "shr.h"

char *ring = __FILE__ ".ring";

#define do_open   'o'
#define do_close  'c'
#define do_write  'w'
#define do_read   'r'
#define do_unlink 'u'
#define do_select 's'
#define do_get_fd 'g'

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
        s = shr_open(ring, SHR_RDONLY|SHR_NONBLOCK|SHR_SELECTFD);
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
  char op, msg[] = "squirrel";
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
      case do_write:
        printf("w: write\n");
        rc = shr_write(s, msg, sizeof(msg));
        /* if (rc != sizeof(msg)) printf("w: rc %d\n", rc); */
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

void delay() { usleep(50000); }

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

  setbuf(stdout,NULL);
  unlink(ring);

  int pipe_to_r[2];
  int pipe_to_w[2];

  shr_init(ring, 10, 0);

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

  issue(R, do_open);
  issue(W, do_open);

  issue(R, do_read);   /* nonblocking, so 0 */

  issue(R, do_get_fd);
  issue(R, do_select); /* r blocks in select */
  issue(W, do_write);  /* r wakeup */
  issue(R, do_read);   /* r reads in loop til wouldblock   */

  issue(W, do_unlink);
  issue(W, do_close);
  issue(R, do_close);

  close(W); delay();
  close(R); delay();

  waitpid(rpid,NULL,0);
  waitpid(wpid,NULL,0);

 rc = 0;

done:
 unlink(ring);
 printf("end\n");
 return rc;
}
