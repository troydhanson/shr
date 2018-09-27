/*
 * shr-socket
 *
 * a pair of shr rings must be created before using this tool.
 *
 * one ring is the outgoing-message ring, and the other is the
 * incoming-message ring. this tool opens a socket to a remote
 * server. the server should accept, and generate, messages
 * having u32 length prefixes. the rings are an abstraction
 * for passing messages, and receiving messages, from the server.
 * when a local program writes to the outgoing-message ring,
 * this program transmits the frame to the server. likewise,
 * when the remote server sends a message, this program writes
 * it to the incoming-message ring.
 *
 * this program is run in the background to maintain this flow.
 *
 */

#include <sys/signalfd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <string.h>
#include <signal.h>
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include "shr.h"

#define MAX_FRAME (10*1024*1024)
#define BUFLEN (MAX_FRAME*2)
#define NUMIOV (1024 * 1024)
char rx_buf_bss[BUFLEN];
char tx_buf_bss[BUFLEN];
struct iovec rx_iov_bss[NUMIOV];
struct iovec tx_iov_bss[NUMIOV];

struct {
  char *prog;
  int verbose;
  char *server;
  char *rxname;
  char *txname;
  struct sockaddr_in addr;
  struct shr *rx;
  struct shr *tx;
  int socket_fd;
  int signal_fd;
  int txring_fd;
  int epoll_fd;
  int hex;
  int omit_length_prefix;
  int unlink_on_eof;
  size_t rx_buf_used;
  struct iovec *rx_iov;
  char *rx_buf;
  struct iovec *tx_iov;
  char *tx_buf;
  char *eof_msg;
  size_t eof_msg_len;
} cfg = {
  .socket_fd = -1,
  .signal_fd = -1,
  .txring_fd = -1,
  .epoll_fd = -1,
  .rx_buf = rx_buf_bss,
  .rx_iov = rx_iov_bss,
  .tx_buf = tx_buf_bss,
  .tx_iov = tx_iov_bss,
};

void usage() {
  fprintf(stderr,"usage: %s [options]\n", cfg.prog);
  fprintf(stderr,"\n");
  fprintf(stderr,"options:\n");
  fprintf(stderr," -s host:port   server address\n");
  fprintf(stderr," -i ring        incoming message ring\n");
  fprintf(stderr," -o ring        outgoing message ring\n");
  fprintf(stderr," -L             omit length prefix\n");
  fprintf(stderr," -D <hex>       ring message on exit\n");
  fprintf(stderr," -U             unlink rings on exit\n");
  fprintf(stderr," -v             verbose\n");
  fprintf(stderr," -x             hex dump\n");
  fprintf(stderr,"\n");
  fprintf(stderr,"-D mode writes a synthetic message to the incoming\n");
  fprintf(stderr,"   message ring on shutdown, from hex e.g. deadbeef\n");
  fprintf(stderr,"\n");
  exit(-1);
}

void hexdump(char *buf, size_t len) {
  size_t i,n=0;
  unsigned char c;
  while(n < len) {
    fprintf(stderr,"%08x ", (int)n);
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

int new_epoll(int events, int fd) {
  struct epoll_event ev;
  int sc, rc = -1;

  memset(&ev, 0, sizeof(ev));
  ev.events = events;
  ev.data.fd = fd;

  sc = epoll_ctl( cfg.epoll_fd, EPOLL_CTL_ADD, fd, &ev);
  if (sc < 0) {
    fprintf(stderr, "epoll_ctl: %s\n", strerror(errno));
    goto done;
  }

  rc = 0;

 done:
  return rc;
}


/*
+ * parse_hostport
+ *
+ * parse [<ip|hostname>]:<port>, populate sockaddr_in
+ *
+ * if there was no IP/hostname, ip is set to INADDR_ANY
+ * port is required, or the function fails
+ *
+ * returns 
+ *  0 success
+ * -1 error
+ *
+ */
int parse_hostport(char *spec, struct sockaddr_in *sa) {
  char *colon=NULL, *p, *h;
  struct hostent *e;
  int rc = -1, port;

  memset(sa, 0, sizeof(*sa));

  colon = strchr(spec, ':');
  h = colon ? spec : NULL;
  p = colon ? colon+1 : spec;

  if (colon) *colon = '\0';
  e = h ? gethostbyname(h) : NULL;
  if (h && (!e || !e->h_length)) {
    fprintf(stderr, "%s: %s\n", h, hstrerror(h_errno));
    goto done;
  }

  port = atoi(p);
  if ((port <= 0) || (port > 65535)) {
    fprintf(stderr, "%s: not a port number\n", p);
    goto done;
  }

  sa->sin_family      = AF_INET;
  sa->sin_port        = htons(port);
  sa->sin_addr.s_addr = htonl(INADDR_ANY);
  if (h) memcpy(&sa->sin_addr.s_addr, e->h_addr, e->h_length);

  fprintf(stderr, "%s -> IP %s port %d\n",
    spec, inet_ntoa(sa->sin_addr), port);

  rc = 0;

 done:
  if (colon) *colon = ':';
  return rc;
}

int do_connect(void) {
  int sc, rc = -1;

  /* make socket */
  cfg.socket_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (cfg.socket_fd == -1) {
    fprintf(stderr, "socket: %s\n", strerror(errno));
    goto done;
  }

  /* perform connect */
  sc = connect(cfg.socket_fd, (struct sockaddr*)&cfg.addr, sizeof(cfg.addr));
  if (sc < 0) {
    fprintf(stderr, "connect: %s\n", strerror(errno));
    goto done;
  }

  rc = 0;

 done:
  return rc;
}

int handle_signal(void) {
  struct signalfd_siginfo si;
  int rc = -1;
  ssize_t nr;

  nr = read(cfg.signal_fd, &si, sizeof(si));
  if (nr < 0) {
    fprintf(stderr, "read: %s\n", strerror(errno));
    goto done;
  }

  assert(nr == sizeof(si));

  /* induce program shutdown */
  fprintf(stderr, "received signal %d\n", si.ssi_signo);
  rc = -1;

 done:
  return rc;
}

/*
 * given a buffer of N frames 
 * with a possible partial final frame
 * find message boundaries and write to ring
 * saving the last frame prefix if partial
 */
int decode_frames(void) {
  char *c, *body, *eob;
  size_t iov_used=0;
  uint32_t blen;
  int rc = -1;
  ssize_t nr;

  eob = cfg.rx_buf + cfg.rx_buf_used;
  c = cfg.rx_buf;
  while(1) {
    if (c + sizeof(uint32_t) > eob) break;
    memcpy(&blen, c, sizeof(uint32_t));
    if (blen > MAX_FRAME) {
      fprintf(stderr,"discarding overlong frame\n");
      goto done;
    }
    body = c + sizeof(uint32_t);
    if (body + blen > eob) break;
    cfg.rx_iov[ iov_used ].iov_base = body;
    cfg.rx_iov[ iov_used ].iov_len  = blen;
    iov_used++;
    if (cfg.hex) hexdump(body, blen);
    if (iov_used == NUMIOV) break;
    c += sizeof(uint32_t) + blen;
  }

  if (iov_used == 0) {
    rc = 0;
    goto done;
  }

  nr = shr_writev(cfg.rx, cfg.rx_iov, iov_used);
  if (nr < 0) {
    fprintf(stderr,"shr_writev: error (%zd)\n", nr);
    goto done;
  }

  /* if buffer ends with partial frame, save it */
  if (c < eob) memmove(cfg.rx_buf, c, eob - c);
  cfg.rx_buf_used = eob - c;

  rc = 0;

 done:
  return rc;
}

int handle_socket(void) {
  int rc = -1;
  size_t avail;
  ssize_t nr;
  char *b;

  assert(cfg.rx_buf_used < BUFLEN);
  avail = BUFLEN - cfg.rx_buf_used;
  b = cfg.rx_buf + cfg.rx_buf_used;

  nr = read(cfg.socket_fd, b, avail);
  if (nr <= 0) {
    fprintf(stderr, "read: %s\n", nr ? strerror(errno) : "eof");
    goto done;
  }

  assert(nr > 0);
  cfg.rx_buf_used += nr;
  if (decode_frames() < 0) goto done;

  rc = 0;

 done:
  return rc;
}

int handle_txring(void) {
  size_t niov, n, l;
  ssize_t nr, sr;
  uint32_t u32;
  int rc=-1;
  char *b;

  niov = NUMIOV;
  nr = shr_readv(cfg.tx, cfg.tx_buf, BUFLEN, cfg.rx_iov, &niov);
  if (nr < 0) {  /* error */
    fprintf(stderr, "shr_readv: error %ld\n", nr);
    goto done;
  }
  if (nr == 0) { /* spurious wakeup */
    rc = 0;
    goto done;
  }

  assert(nr > 0);
  assert(niov > 0);

  /* do draining write on each iov */
  for(n = 0; n < niov; n++) {

    b = cfg.rx_iov[ n ].iov_base;
    l = cfg.rx_iov[ n ].iov_len;

    if (cfg.omit_length_prefix)
      goto body;

    if (l > UINT32_MAX) {
      fprintf(stderr, "overlong frame length %zu\n", l);
      goto done;
    }

    /* send length prefix */
    u32 = (uint32_t)l;
    sr = write(cfg.socket_fd, &u32, sizeof(u32));
    if (sr < 0) {
      fprintf(stderr, "write: %s\n", strerror(errno));
      goto done;
    }
    assert(sr == sizeof(u32));

  body:
    /* send frame body */
    if (cfg.hex) hexdump(b, l);
    do {
      sr = write(cfg.socket_fd, b, l);
      if (sr < 0) {
        fprintf(stderr, "write: %s\n", strerror(errno));
        goto done;
      }
      l -= sr;
      b += sr;
    } while(l);

  }

  rc = 0;
 
 done:
  return rc;
}

/* unhexer, overwrites input space;
 * returns number of bytes or -1 */
int unhex(char *h) {
  char b;
  int rc = -1;
  unsigned u;
  size_t i, len = strlen(h);

  if (len == 0) goto done;
  if (len &  1) goto done; /* odd number of digits */
  for(i=0; i < len; i += 2) {
    if (sscanf( &h[i], "%2x", &u) < 1) goto done;
    assert(u <= 255);
    b = (unsigned char)u;
    h[i/2] = b;
  }

  rc = 0;

 done:
  if (rc < 0) {
    fprintf(stderr, "hex conversion failed\n");
    return -1;
  }

  return len/2;
}

int main(int argc, char *argv[]) {
  struct epoll_event ev;
  int opt, rc=-1, sc;
  ssize_t nr;
  
  cfg.prog = argv[0];

  while ( (opt = getopt(argc,argv,"vxhs:i:o:LD:U")) > 0) {
    switch(opt) {
      case 'v': cfg.verbose++; break;
      case 'x': cfg.hex=1; break;
      case 'L': cfg.omit_length_prefix=1; break;
      case 'U': cfg.unlink_on_eof=1; break;
      case 's': cfg.server = strdup(optarg); break;
      case 'i': cfg.rxname = strdup(optarg); break;
      case 'o': cfg.txname = strdup(optarg); break;
      case 'D': cfg.eof_msg = strdup(optarg);
                cfg.eof_msg_len = unhex(cfg.eof_msg);
                if (cfg.eof_msg_len < 0) usage();
                break;
      case 'h': default: usage(); break;
    }
  }

  if (cfg.server == NULL) usage();
  if (cfg.rxname == NULL) usage();
  if (cfg.txname == NULL) usage();

  sc = parse_hostport(cfg.server, &cfg.addr);
  if (sc < 0) goto done;

  /* external app writes to tx ring; we transmit from it */
  cfg.tx = shr_open(cfg.txname, SHR_RDONLY|SHR_NONBLOCK);
  if (cfg.tx == NULL) goto done;
  cfg.txring_fd = shr_get_selectable_fd(cfg.tx);
  assert(cfg.txring_fd != -1);

  /* remote sends messages; we deframe and write to rx ring */
  cfg.rx = shr_open(cfg.rxname, SHR_WRONLY);
  if (cfg.rx == NULL) goto done;

  sc = do_connect();
  if (sc < 0) goto done;

  /* block all signals. take signals synchronously via signalfd */
  sigset_t all;
  sigfillset(&all);
  sigprocmask(SIG_SETMASK,&all,NULL);

  /* a few signals we'll accept via our signalfd */
  sigset_t sw;
  sigemptyset(&sw);
  sigaddset(&sw, SIGINT);
  sigaddset(&sw, SIGTERM);

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
  if (new_epoll(EPOLLIN, cfg.signal_fd))   goto done;
  if (new_epoll(EPOLLIN, cfg.socket_fd))   goto done;
  if (new_epoll(EPOLLIN, cfg.txring_fd))   goto done;

  while (1) {

    sc = epoll_wait(cfg.epoll_fd, &ev, 1, -1);
    if (sc < 0) {
      fprintf(stderr, "epoll_wait: %s\n", strerror(errno));
      goto done;
    }

    if (ev.data.fd == cfg.signal_fd) {
      sc = handle_signal();
      if (sc < 0) goto done;
    }
    else if (ev.data.fd == cfg.socket_fd) {
      sc = handle_socket();
      if (sc < 0) goto done;
    }
    else if (ev.data.fd == cfg.txring_fd) {
      sc = handle_txring();
      if (sc < 0) goto done;
    }
    else {
      assert(0);
      goto done;
    }
  }

  rc = 0;
 
 done:
  if (cfg.eof_msg && cfg.rx) {
    nr = shr_write(cfg.rx, cfg.eof_msg, cfg.eof_msg_len);
    if (nr < 0) fprintf(stderr, "shr_write: error %zd\n", nr);
  }
  if (cfg.unlink_on_eof && cfg.rxname) unlink(cfg.rxname);
  if (cfg.unlink_on_eof && cfg.txname) unlink(cfg.txname);
  if (cfg.socket_fd != -1) close(cfg.socket_fd);
  if (cfg.signal_fd != -1) close(cfg.signal_fd);
  if (cfg.epoll_fd != -1) close(cfg.epoll_fd);
  /* cfg.txring_fd is internal to shr */
  if (cfg.rxname) free(cfg.rxname);
  if (cfg.txname) free(cfg.txname);
  if (cfg.server) free(cfg.server);
  if (cfg.eof_msg) free(cfg.eof_msg);
  if (cfg.rx) shr_close(cfg.rx);
  if (cfg.tx) shr_close(cfg.tx);
  return rc;
}

