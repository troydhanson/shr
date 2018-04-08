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
 * shr utility
 */

/* read buffer; outside of inited struct below to reduce exe sz*/
#define BATCH_FRAMES 10000
#define BATCH_MB     10
#define BATCH_BYTES  (BATCH_MB * 1024 * 1024)
char read_buffer[BATCH_BYTES];
struct iovec read_iov[BATCH_FRAMES];

struct {
  char *prog;
  enum {mode_pub } mode;
  int verbose;
  int epoll_fd;     /* epoll descriptor */
  int signal_fd;    /* to receive signals */
  int ring_fd;      /* ring readability fd */
  int listen_fd;    /* listening tcp socket */
  int client_fd;    /* connected tcp socket */
  in_addr_t addr;   /* IP address to listen on */
  int port;         /* TCP port to listen on */
  char *file;       /* ring file name */
  struct shr *ring; /* open ring handle */
  char *buf;        /* buf for shr_readv */
  struct iovec *iov;/* iov for shr_readv */
  char *stats_file;
  size_t ompp;      /* messages-out per stats period */
} cfg = {
  .addr = INADDR_ANY, /* by default, listen on all local IP's */
  .port = 1919,       /* arbitrary */
  .buf = read_buffer,
  .iov = read_iov,
  .epoll_fd = -1,
  .signal_fd = -1,
  .ring_fd = -1,
  .listen_fd = -1,
  .client_fd = -1,
};

/* signals that we'll accept via signalfd in epoll */
int sigs[] = {SIGHUP,SIGTERM,SIGINT,SIGQUIT,SIGALRM};

void usage() {
  fprintf(stderr,"usage: %s [options] <ring>\n", cfg.prog);
  fprintf(stderr,"options:\n"
                 "               -p <port>  (TCP port to listen on)\n"
                 "               -v         (verbose)\n"
                 "               -h         (this help)\n"
                 "               -S <file>  (stats file to write)\n"
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

void dump_stats() {
  int fd=-1, rc;
  char stats[100];

  if (cfg.stats_file == NULL) return;
  fd = open(cfg.stats_file, O_WRONLY|O_TRUNC|O_CREAT, 0664);
  if (fd == -1) {
    fprintf(stderr,"open %s: %s\n", cfg.stats_file, strerror(errno));
    goto done;
  }

  double msgs_per_sec = cfg.ompp/10.0;
  snprintf(stats,sizeof(stats),"%.2f msgs/s\n", msgs_per_sec);
  if (write(fd,stats,strlen(stats)) < 0) {
    fprintf(stderr,"write %s: %s\n", cfg.stats_file, strerror(errno));
    goto done;
  }

 done:
  if (fd != -1) close(fd);
  cfg.ompp = 0; // reset
}

/* work we do at 1hz  */
int periodic_work(void) {
  int rc = -1;

  static int counter=0;
  if ((++counter % 10) == 0) dump_stats();

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
  char *b;
  size_t n, iovcnt;

  switch(cfg.mode) {
    case mode_pub:
        iovcnt = BATCH_FRAMES;
        rv = shr_readv(cfg.ring, cfg.buf, BATCH_BYTES, cfg.iov, &iovcnt);
        if (rv < 0) fprintf(stderr, "shr_readv: error\n");
        if (rv > 0) {
          if (cfg.verbose) {
            fprintf(stderr,"shr_readv: %zu frames\n", iovcnt);
          }
          /* send data to client. this is a blocking, fully draining loop */
          b = cfg.buf;
          n = rv;
          while (n) {
            /* we check for client presence b/c this loop can close it */
            wc = (cfg.client_fd == -1) ? n : write(cfg.client_fd, b, n);
            if (wc < 0) {
              fprintf(stderr,"write: %s\n", strerror(errno));
              close(cfg.client_fd);
              cfg.client_fd = -1;
              del_epoll(cfg.ring_fd);
              n = 0;
            } else {
              assert(wc > 0);
              n -= wc;
              b += wc;
            }
          }
          cfg.ompp += iovcnt;
        }
      break;
    default:
      assert(0);
      break;
  }

  rc = 0;

 done:
  return rc;
}

int setup_listener() {
  int rc = -1, one=1;

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd == -1) {
    fprintf(stderr,"socket: %s\n", strerror(errno));
    goto done;
  }

  /**********************************************************
   * internet socket address structure: our address and port
   *********************************************************/
  struct sockaddr_in sin;
  sin.sin_family = AF_INET;
  sin.sin_addr.s_addr = cfg.addr;
  sin.sin_port = htons(cfg.port);

  /**********************************************************
   * bind socket to address and port 
   *********************************************************/
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  if (bind(fd, (struct sockaddr*)&sin, sizeof(sin)) == -1) {
    fprintf(stderr,"bind: %s\n", strerror(errno));
    goto done;
  }

  /**********************************************************
   * put socket into listening state
   *********************************************************/
  if (listen(fd,1) == -1) {
    fprintf(stderr,"listen: %s\n", strerror(errno));
    goto done;
  }

  if (add_epoll(EPOLLIN, fd)) goto done;
  cfg.listen_fd = fd;
  rc=0;

 done:
  if ((rc < 0) && (fd != -1)) close(fd);
  return rc;
}

/* accept a new client connection to the listening socket */
int accept_client() {
  int fd=-1, rc=-1;
  struct sockaddr_in in;
  socklen_t sz = sizeof(in);

  fd = accept(cfg.listen_fd,(struct sockaddr*)&in, &sz);
  if (fd == -1) {
    fprintf(stderr,"accept: %s\n", strerror(errno)); 
    goto done;
  }

  if (cfg.verbose && (sizeof(in)==sz)) {
    fprintf(stderr,"connection fd %d from %s:%d\n", fd,
    inet_ntoa(in.sin_addr), (int)ntohs(in.sin_port));
  }

  if (cfg.client_fd != -1) { /* already have a client? */
    fprintf(stderr,"refusing client\n");
    close(fd);
    rc = 0;
    goto done;
  }

  cfg.client_fd = fd;

  /* epoll on both the ring and the client */
  if (add_epoll(EPOLLIN, cfg.ring_fd)   < 0) goto done;
  if (add_epoll(EPOLLIN, cfg.client_fd) < 0) goto done;

  rc = 0;

 done:
  return rc;
}

/* discard input from client; close on EOF */
int drain_client() {
  int rc = -1;
  char buf[1024];
  assert(cfg.client_fd != -1);

  rc = read(cfg.client_fd, buf, sizeof(buf));
  if(rc > 0) { 
    if (cfg.verbose) fprintf(stderr,"client: %d bytes\n", rc);
  } else {
    fprintf(stderr,"client: %s\n", rc ? strerror(errno) : "closed");
    del_epoll(cfg.ring_fd); /* only epoll ring when client present */
    close(cfg.client_fd);   /* close removes epoll */
    cfg.client_fd = -1;
  }

  rc = 0;
  return rc;
}

int main(int argc, char *argv[]) {
  int opt, rc=-1, n, ec;
  struct epoll_event ev;
  cfg.prog = argv[0];
  char unit, *c, buf[100];
  struct shr_stat stat;
  ssize_t nr;

  while ( (opt = getopt(argc,argv,"vhp:S:")) > 0) {
    switch(opt) {
      case 'v': cfg.verbose++; break;
      case 'h': default: usage(); break;
      case 'p': cfg.port = atoi(optarg); break;
      case 'S': cfg.stats_file=strdup(optarg); break;
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

  /* add descriptors of interest */
  if (add_epoll(EPOLLIN, cfg.signal_fd)) goto done;

  switch(cfg.mode) {

    case mode_pub:
      cfg.ring = shr_open(cfg.file, SHR_RDONLY|SHR_NONBLOCK);
      if (cfg.ring == NULL) goto done;
      cfg.ring_fd = shr_get_selectable_fd(cfg.ring);
      if (cfg.ring_fd < 0) goto done;
      if (setup_listener() < 0) goto done;
      // epoll on ring_fd once client connects
      break;

    default: 
      assert(0);
      break;
  }

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
    else if (ev.data.fd == cfg.listen_fd) { if (accept_client() < 0) goto done; }
    else if (ev.data.fd == cfg.client_fd) { if (drain_client() < 0) goto done; }
    else                                  { assert(0); goto done; }
  }
  
  rc = 0;
 
 done:
  if (cfg.ring) shr_close(cfg.ring);
  if (cfg.signal_fd != -1) close(cfg.signal_fd);
  if (cfg.epoll_fd != -1) close(cfg.epoll_fd);
  if (cfg.listen_fd != -1) close(cfg.listen_fd);
  if (cfg.client_fd != -1) close(cfg.client_fd);
  if (cfg.stats_file) free(cfg.stats_file);
  // in mode_pub, cfg.ring_fd is internal to ring
  return 0;
}
