#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <sys/signalfd.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include "shr.h"

/* 
 *
 */

#define BATCH_FRAMES 10000
#define BATCH_MB     10
#define BATCH_BYTES  (BATCH_MB * 1024 * 1024)
char read_buffer[BATCH_BYTES];
struct iovec read_iov[BATCH_FRAMES];

struct {
  char *prog;
  int verbose;
  int epoll_fd;     /* epoll descriptor */
  int signal_fd;    /* to receive signals */
  int ring_fd;      /* ring readability fd */
  char *file;       /* ring file name */
  struct shr *ring; /* open ring handle */
  char *buf;        /* buf for shr_readv */
  struct iovec *iov;/* iov for shr_readv */
} cfg = {
  .buf = read_buffer,
  .iov = read_iov,
  .epoll_fd = -1,
  .signal_fd = -1,
  .ring_fd = -1,
};

/* signals that we'll accept via signalfd in epoll */
int sigs[] = {SIGHUP,SIGTERM,SIGINT,SIGQUIT,SIGALRM};

void usage() {
  fprintf(stderr,"usage: %s [options] <ring>\n", cfg.prog);
  fprintf(stderr,"options:\n"
                 "               -v         (verbose)\n"
                 "               -h         (this help)\n"
                 "\n");
  exit(-1);
}

int add_epoll(int events, int fd) {
  int rc;
  struct epoll_event ev;
  memset(&ev,0,sizeof(ev)); // placate valgrind
  ev.events = events;
  ev.data.fd= fd;
  if (cfg.verbose) fprintf(stderr,"adding fd %d to epoll\n", fd);
  rc = epoll_ctl(cfg.epoll_fd, EPOLL_CTL_ADD, fd, &ev);
  if (rc == -1) {
    fprintf(stderr,"epoll_ctl: %s\n", strerror(errno));
  }
  return rc;
}

int del_epoll(int fd) {
  int rc;
  struct epoll_event ev;
  rc = epoll_ctl(cfg.epoll_fd, EPOLL_CTL_DEL, fd, &ev);
  if (rc == -1) {
    fprintf(stderr,"epoll_ctl: %s\n", strerror(errno));
  }
  return rc;
}

/* work we do at 1hz  */
int periodic_work(void) {
  int rc = -1;

  rc = 0;

 done:
  return rc;
}

int handle_signal(void) {
  int rc=-1;
  struct signalfd_siginfo info;
  
  if (read(cfg.signal_fd, &info, sizeof(info)) != sizeof(info)) {
    fprintf(stderr,"failed to read signal fd buffer\n");
    goto done;
  }

  switch(info.ssi_signo) {
    case SIGALRM: 
      if (periodic_work() < 0) goto done;
      alarm(1); 
      break;
    default: 
      fprintf(stderr,"got signal %d\n", info.ssi_signo);  
      goto done;
      break;
  }

 rc = 0;

 done:
  return rc;
}

int handle_io(void) {
  int rc = -1;
  ssize_t rv, wc;
  size_t iovcnt;

  iovcnt = BATCH_FRAMES;
  rv = shr_readv(cfg.ring, cfg.buf, BATCH_BYTES, cfg.iov, &iovcnt);
  if (rv < 0) fprintf(stderr, "shr_readv: error\n");
  if (rv > 0) {
    if (cfg.verbose) fprintf(stderr,"shr_readv: %zu frames\n", iovcnt);
    /* TODO process data */
  }

  rc = 0;

 done:
  return rc;
}

int main(int argc, char *argv[]) {
  int opt, rc=-1, n, ec;
  struct epoll_event ev;
  cfg.prog = argv[0];
  char unit, *c, buf[100];

  while ( (opt = getopt(argc,argv,"vh")) > 0) {
    switch(opt) {
      case 'v': cfg.verbose++; break;
      case 'h': default: usage(); break;
    }
  }

  if (optind < argc) cfg.file = argv[optind++];
  if (cfg.file == NULL) usage();
  
  /* block all signals. we accept signals via signal_fd */
  sigset_t all;
  sigfillset(&all);
  sigprocmask(SIG_SETMASK,&all,NULL);

  /* a few signals we'll accept via our signalfd */
  sigset_t sw;
  sigemptyset(&sw);
  for(n=0; n < sizeof(sigs)/sizeof(*sigs); n++) sigaddset(&sw, sigs[n]);

  /* create the signalfd for receiving signals */
  cfg.signal_fd = signalfd(-1, &sw, 0);
  if (cfg.signal_fd == -1) {
    fprintf(stderr,"signalfd: %s\n", strerror(errno));
    goto done;
  }

  /* set up the epoll instance */
  cfg.epoll_fd = epoll_create(1); 
  if (cfg.epoll_fd == -1) {
    fprintf(stderr,"epoll: %s\n", strerror(errno));
    goto done;
  }

  /* open the ring */
  cfg.ring = shr_open(cfg.file, SHR_RDONLY|SHR_NONBLOCK);
  if (cfg.ring == NULL) goto done;
  cfg.ring_fd = shr_get_selectable_fd(cfg.ring);
  if (cfg.ring_fd < 0) goto done;

  /* add descriptors of interest to epoll */
  if (add_epoll(EPOLLIN, cfg.signal_fd)) goto done;
  if (add_epoll(EPOLLIN, cfg.ring_fd)) goto done;

  alarm(1);

  while (1) {
    ec = epoll_wait(cfg.epoll_fd, &ev, 1, -1);
    if (ec < 0) { 
      fprintf(stderr, "epoll: %s\n", strerror(errno));
      goto done;
    }

    if (ec == 0)                          { assert(0); goto done; }
    else if (ev.data.fd == cfg.signal_fd) { if (handle_signal()  < 0) goto done; }
    else if (ev.data.fd == cfg.ring_fd)   { if (handle_io() < 0) goto done; }
    else                                  { assert(0); goto done; }
  }
  
  rc = 0;
 
 done:
  if (cfg.ring) shr_close(cfg.ring);
  if (cfg.signal_fd != -1) close(cfg.signal_fd);
  if (cfg.epoll_fd != -1) close(cfg.epoll_fd);
  // cfg.ring_fd is internal to ring
  return 0;
}
