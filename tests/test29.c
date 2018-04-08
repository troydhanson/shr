#include <sys/signalfd.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include "shr.h"

char *ring = "/dev/shm/" __FILE__ ".ring";

int main() {
  setlinebuf(stdout);
 int rc = -1, sc, signal_fd = -1;
 struct shr *s=NULL;
 unsigned int n;
 char buf[100];
 ssize_t nr;

 unlink(ring);

 printf("init\n");
 if (shr_init(ring, sizeof(buf)+0, 0) < 0) goto done;

 s = shr_open(ring, SHR_WRONLY);
 if (s == NULL) goto done;

 /* signals that we'll accept via signalfd in epoll */
 int sigs[] = {SIGHUP,SIGTERM,SIGINT,SIGQUIT,SIGALRM};

 /* block all signals. we take signals synchronously via signalfd */
 sigset_t all;
 sigfillset(&all);
 sigprocmask(SIG_SETMASK,&all,NULL);

 /* a few signals we'll accept via our signalfd */
 sigset_t sw;
 sigemptyset(&sw);
 for(n=0; n < sizeof(sigs)/sizeof(*sigs); n++) sigaddset(&sw, sigs[n]);

 /* create the signalfd for receiving signals */
 signal_fd = signalfd(-1, &sw, 0);
 if (signal_fd == -1) {
   fprintf(stderr,"signalfd: %s\n", strerror(errno));
   goto done;
 }

 /* tell shr to monitor this fd while blocked */
 sc = shr_ctl(s, SHR_POLLFD, signal_fd);
 if (sc < 0) goto done;

 /* schedule SIGARLM for 10s from now */
 alarm(10);

 /* initial write should succeed immediately */
 memset(buf, 0, sizeof(buf));
 nr = shr_write(s, buf, sizeof(buf));
 printf("shr_write: %zd\n", nr);

 /* start a blocking write. signal should cause nr == -3 */
 nr = shr_write(s, buf, sizeof(buf));
 printf("shr_write: %zd\n", nr);

 printf("ok\n");
 rc = 0;

done:
 if (signal_fd) close(signal_fd);
 if (s) shr_close(s);
 unlink(ring);
 return rc;
}
