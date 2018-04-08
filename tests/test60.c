#include <stdio.h>
#include <sys/wait.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include "shr.h"

char *ring = "/dev/shm/" __FILE__ ".ring";

int main() {
  setlinebuf(stdout);
  int rc = 0,fd;
  shr *s;
  ssize_t nr;
  char c;

  setbuf(stdout,NULL);
  unlink(ring);

  shr_init(ring, 14, 0);
  s = shr_open(ring, SHR_WRONLY);
  if (s == NULL) goto done;
  nr = shr_write(s, "abc", 3);
  printf("write %d\n", (int)nr);
  shr_close(s);

  printf("opening for reading\n");
  s = shr_open(ring, SHR_RDONLY|SHR_NONBLOCK);
  if (s == NULL) goto done;
  printf("getting selectable fd\n");
  fd = shr_get_selectable_fd(s);
  if (fd < 0) goto done;

  printf("selecting...\n");
  fd_set fds;
  FD_ZERO(&fds);
  FD_SET(fd, &fds);
  struct timeval tv = {.tv_sec = 0, .tv_usec =10};
  rc = select(fd+1, &fds, NULL, NULL, &tv);
  if (rc < 0) printf("select: %s\n", strerror(errno));
  else if (rc == 0) printf("select: timeout\n");
  else if (rc == 1) printf("select: ready\n");

  do {
    printf("read\n");
    nr = shr_read(s, &c, sizeof(c)); // byte read
    if (nr < 0) printf("r: [%zd] (buf too small)\n", nr);
    if (nr > 0) printf("r: [%c]\n", c);
    if (nr == 0) printf("r: wouldblock\n");

    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    struct timeval tv = {.tv_sec = 0, .tv_usec =10};
    rc = select(fd+1, &fds, NULL, NULL, &tv);
    if (rc < 0) printf("select: %s\n", strerror(errno));
    else if (rc == 0) printf("select: timeout\n");
    else if (rc == 1) printf("select: ready\n");

  } while (nr > 0);

 shr_close(s);
 rc = 0;

done:
 unlink(ring);
 return rc;
}
