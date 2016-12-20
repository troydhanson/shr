#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include "shr.h"

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

/* this ring sz forces the second message length prefix to wrap */
#define ring_sz (sizeof(msg) + sizeof(size_t) +1)

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
#define do_undersized_read 'U'

void print_elapsed(char *name, struct timeval *start, struct timeval *end, int nmsg) {
  unsigned long usec_start, usec_end, usec_elapsed;
  usec_end = end->tv_sec * 1000000 + end->tv_usec;
  usec_start = start->tv_sec * 1000000 + start->tv_usec;
  usec_elapsed = usec_end - usec_start;
  double msgs_per_sec = usec_elapsed  ? (nmsg * 1000000.0 / usec_elapsed) : 0;
  fprintf(stderr,"%s: %f msgs/sec\n", name, msgs_per_sec);
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
        //printf("r: select\n");
        gettimeofday(&tv_start,NULL);
        n = 0;
        while (n < nmsg) {
          fd_set fds;
          FD_ZERO(&fds);
          FD_SET(selectable_fd, &fds);
          struct timeval tv = {.tv_sec = 1, .tv_usec =0};
          rc = select(selectable_fd+1, &fds, NULL, NULL, &tv);
          if (rc < 0) printf("r: select %s\n", strerror(errno));
          else if (rc == 0) printf("r: timeout\n");
          else if (rc == 1) {
            //printf("r: ready. draining...\n");
            do {
              rc = shr_read(s, buf, sizeof(buf));
              //printf("r: msg\n");
              n++;
            } while (rc > 0);
          }
          else assert(0);
        }
        gettimeofday(&tv_end,NULL);
        print_elapsed("r", &tv_start,&tv_end,nmsg);
        break;
      case do_undersized_read:
        printf("r: undersized read\n");
        rc = shr_read(s, buf, sizeof(buf)-1);
        printf("r: %d\n", rc);
        break;
      case do_read:
        printf("r: read\n");
        rc = shr_read(s, buf, sizeof(buf));
        printf("r: %d\n", rc);
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
  pid_t rpid,wpid,pid;

  setbuf(stdout,NULL);
  unlink(ring);

  int pipe_to_r[2];
  int pipe_to_w[2];

  shr_init(ring, ring_sz, SHR_MESSAGES);

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

  issue(W, do_write); delay();
  issue(R, do_read); delay();

  issue(W, do_write); delay(); /* msg length prefix wraps */
  issue(R, do_read); delay();

  issue(W, do_unlink); delay();
  issue(W, do_close); delay();
  issue(R, do_close); delay();

  close(W); delay();
  close(R);

  int status;
  pid = waitpid(wpid,&status,0);
  //printf("waitpid: %d\n",(int)pid);
  if (pid < 0) printf("waitpid: %s\n", strerror(errno));
  if (WIFSIGNALED(status)) printf("w signal exit\n");

  pid = waitpid(rpid,&status,0);
  //printf("waitpid: %d\n",(int)pid);
  if (pid < 0) printf("waitpid: %s\n", strerror(errno));
  if (WIFSIGNALED(status)) printf("r signal exit\n");

 rc = 0;

done:
 unlink(ring);
 printf("end\n");
 return rc;
}
