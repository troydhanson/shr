#include <sys/signalfd.h>
#include <sys/signal.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include "bw.h"

/*
 * test tool for waitlib
 *
 */

struct {
  char *prog;
  int verbose;
  int force_ready;
  int force_clear;
  int delay_after_open;
  int sigalrm_sec;
  int wait_use_api;
  char *file;
} CF = {
};

void usage() {
  fprintf(stderr,"usage: %s [options] -r file [make|wake|wait]\n", CF.prog);
  fprintf(stderr,"  -v         (verbose, repeatable)\n"
                 "  -d <sec>   (delay after open before wait or wake)\n"
                 "  -f | -c    (force or clear fd readability after wakeup)\n"
                 "  -a <sec>   (schedule sigalrm after n second wait)\n"
                 "  -w         (wait using bw_wait_ul rather than select)\n"
                 );
  exit(-1);
}

char *map(char *file, size_t *len, int *fd) {
  int rc = -1, sc;
  char *buf = NULL;
  struct stat s;

  *fd = open(file, O_RDWR);
  if (*fd < 0) {
    fprintf(stderr,"open: %s\n", strerror(errno));
    goto done;
  }

  sc = fstat(*fd, &s);
  if (sc < 0) {
    fprintf(stderr,"fstat: %s\n", strerror(errno));
    goto done;
  }

  if (s.st_size == 0) {
    fprintf(stderr,"error: mmap zero size file\n");
    goto done;
  }

  buf = mmap(0, s.st_size, PROT_READ|PROT_WRITE, MAP_SHARED, *fd, 0);
  if (buf == MAP_FAILED) {
    fprintf(stderr, "mmap: %s\n", strerror(errno));
    buf = NULL;
    goto done;
  }

  rc = 0;
  *len = s.st_size;

 done:
  if (rc && buf) { munmap(buf, s.st_size); buf = NULL; }
  return buf;
}

static int lock(int fd) {
  int rc = -1;
  const struct flock f = { .l_type = F_WRLCK, .l_whence = SEEK_SET, };

  if (fcntl(fd, F_SETLKW, &f) < 0) {
    fprintf(stderr, "fcntl (lock acquisiton failed): %s\n", strerror(errno));
    goto done;
  }

  rc = 0;

 done:
  return rc;
}

static int unlock(int fd) {
  int rc = -1;
  const struct flock f = { .l_type = F_UNLCK, .l_whence = SEEK_SET, };

  if (fcntl(fd, F_SETLK, &f) < 0) {
    fprintf(stderr, "fcntl (lock release failed): %s\n", strerror(errno));
    goto done;
  }

  rc = 0;

 done:
  return rc;
}

int handle_signal(int signal_fd) {
  struct signalfd_siginfo info;
  ssize_t nr;
  char *s;

  nr = read(signal_fd, &info, sizeof(info));
  if (nr != sizeof(info)) {
    fprintf(stderr,"failed to read signal fd buffer\n");
    return -1;
  }

	s = strsignal(info.ssi_signo);
	fprintf(stderr,"got signal %d (%s)\n", info.ssi_signo, s);
  return -1;
}

static int await_readiness(int fd, int sig_fd, int block) {
  int sc, rc = -1, max;

  fd_set fds;
  FD_ZERO(&fds);
  FD_SET(fd, &fds);
  FD_SET(sig_fd, &fds);

  max = (fd > sig_fd) ? fd : sig_fd;

  struct timeval tv = {.tv_sec = 0, .tv_usec = 0};

  sc = select(max+1, &fds, NULL, NULL, block ? NULL : &tv);
  if (sc < 0) {
    fprintf(stderr, "select: %s\n", strerror(errno));
    goto done;
  }

  fprintf(stderr, "bw-tool: fd %s\n", sc ? "ready" : "clear");
  if (sc && (FD_ISSET(sig_fd, &fds))) {
    handle_signal(sig_fd);
    goto done;
  }
  rc = 0;

 done:
  return rc;
}


int main(int argc, char *argv[]) {
  int opt, rc=-1, fd=-1, poll_fd=-1, signal_fd = -1, sc;
  struct bw_t *bw=NULL;
  char *buf=NULL;
  unsigned n;
  size_t len;
  
  CF.prog = argv[0];
  signal(SIGALRM, SIG_IGN);

  while ( (opt = getopt(argc,argv,"vhr:d:fca:w")) > 0) {
    switch(opt) {
      case 'v': CF.verbose++; break;
      case 'r': CF.file = strdup(optarg); break;
      case 'd': CF.delay_after_open = atoi(optarg); break;
      case 'a': CF.sigalrm_sec = atoi(optarg); break;
      case 'f': CF.force_ready=1; break;
      case 'w': CF.wait_use_api=1; break;
      case 'c': CF.force_clear=1; break;
      case 'h': default: usage(); break;
    }
  }

  if (CF.file == NULL) usage();
  if (optind >= argc) usage();

	/* block all signals. we take signals via signalfd */
	sigset_t all;
	sigfillset(&all);
	sigprocmask(SIG_SETMASK,&all,NULL);

	/* a few signals we'll accept via our signalfd */
	int sigs[] = {SIGHUP,SIGTERM,SIGINT,SIGQUIT,SIGALRM};
	sigset_t sw;
	sigemptyset(&sw);
	for(n=0; n < sizeof(sigs)/sizeof(*sigs); n++) sigaddset(&sw, sigs[n]);

	/* create the signalfd for receiving signals */
	signal_fd = signalfd(-1, &sw, 0);
	if (signal_fd == -1) {
		fprintf(stderr,"signalfd: %s\n", strerror(errno));
		goto done;
	}


  /**********************************************/
  if (strcmp(argv[optind], "make") == 0) {
    fd = open(CF.file, O_WRONLY|O_CREAT, 0644);
    if (fd < 0) {
      fprintf(stderr, "open: %s\n", strerror(errno));
      goto done;
    }
    if (ftruncate(fd, sizeof(bw_handle)) < 0) {
      fprintf(stderr, "fruncate: %s\n", strerror(errno));
      goto done;
    }
  }

  /**********************************************/
  if (strcmp(argv[optind], "wake") == 0) {
    buf = map(CF.file, &len, &fd);
    if (buf == NULL) goto done;
    if (lock(fd) < 0) goto done;
    bw = bw_open(BW_WAKE|BW_TRACE, (bw_handle*)buf);
    if (bw == NULL) goto done;
    if (unlock(fd) < 0) goto done;
    sleep(CF.delay_after_open);
    if (lock(fd) < 0) goto done;
    if (bw_wake(bw)) goto done;
    bw_close(bw);
    if (unlock(fd) < 0) goto done;
  }

  /**********************************************/
  if (strcmp(argv[optind], "wait") == 0) {
    buf = map(CF.file, &len, &fd);
    if (buf == NULL) goto done;
    if (lock(fd) < 0) goto done;
    bw = bw_open(BW_WAIT|BW_TRACE, (bw_handle*)buf, &poll_fd);
    if (bw == NULL) goto done;
    if (unlock(fd) < 0) goto done;
    sleep(CF.delay_after_open);
    if (bw_ctl(bw, BW_POLLFD, signal_fd) < 0) goto done;
    if (CF.sigalrm_sec) alarm(CF.sigalrm_sec);
    if (CF.wait_use_api) {
      sc = bw_wait_ul(bw);
      if (sc ==  0) fprintf(stderr, "bw_wait_ul: awoken\n");
      if (sc == -1) fprintf(stderr, "bw_wait_ul: error\n");
      if (sc == -2) {
        fprintf(stderr, "bw_wait_ul: signal while blocked\n");
        handle_signal(signal_fd);
        if (lock(fd) < 0) goto done;
        bw_close(bw);
        if (unlock(fd) < 0) goto done;
        goto done;
      }
    } else {
      if (await_readiness(poll_fd,signal_fd,1) < 0) goto done;
      if (bw_ready_ul(bw) < 0) goto done;
    }
    if (lock(fd) < 0) goto done;
    if (CF.force_ready) bw_force(bw,1);
    if (CF.force_clear) bw_force(bw,0);
    if (await_readiness(poll_fd,signal_fd,0) < 0) goto done;
    bw_close(bw);
    if (unlock(fd) < 0) goto done;
  }

  rc = 0;
 
 done:
  if (CF.file) free(CF.file);
  if (fd != -1) close(fd);
  /* don't close(poll_fd) - bw_close does */
  if (signal_fd != -1) close(signal_fd);
  if (buf) munmap(buf,len);
  if (rc) printf("error exit\n");
  return rc;
}
