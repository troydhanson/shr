#include <sys/signalfd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/mount.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <arpa/inet.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <limits.h>
#include <signal.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <netdb.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include "shr.h"

/* 
 * shr tool
 *
 */

/* we put a couple of buffers out here 
 * to make them go into bss, whereas if
 * they're in the struct below they'd be
 * in the initialized data area and the
 * resulting binary gets much larger */
#define BUFLEN 1000000
#define NUMIOV (1024 * 1024)
char buf_bss[BUFLEN];
struct iovec iov_bss[NUMIOV];

/* subscriber safeguard */
#define MAX_FRAME (1024*1024)

struct {
  char *prog;
  int verbose;
  char *ring;
  enum {mode_status,
        mode_create,
        mode_mount,
        mode_write,
        mode_write_hex,
        mode_write_file,
        mode_read,
        mode_read_hex,
        mode_pub,
        mode_sub,
        mode_tee} mode;
  struct shr *shr;
  int signal_fd;
  int epoll_fd;
  size_t size;
  size_t max_msgs;
  int flags;
  int fd;
  int block;
  char *hex_arg;
  char *wfile;
  char *wfile_buf;
  size_t wfile_len;
  char *mountpoint;
  char *fs_type;
  enum {mnt_mount,
        mnt_make_subdirs,
        mnt_unmount,
        mnt_query} mount_mode;
  char parent[PATH_MAX];
  int nr_hugepages;
  /* pub/sub */
  char *addr_spec;
  struct sockaddr_in addr;
  int listen_fd;
  int pub_fd;
  int sub_fd;
  int pub_len_prefix;
  size_t sub_buf_used;
  struct iovec *sub_iov;
  char tmp[100];
  char *buf;
  /* tee */
  struct shr *o1;
  struct shr *o2;
} cfg = {
  .fs_type = "ramfs",
  .listen_fd = -1,
  .signal_fd = -1,
  .epoll_fd = -1,
  .pub_fd = -1,
  .sub_fd = -1,
  .fd = -1,
  .buf = buf_bss,
  .sub_iov = iov_bss,
  .pub_len_prefix = 1,
};

/* signals that we'll accept via signalfd in epoll */
int sigs[] = {SIGHUP,SIGTERM,SIGINT,SIGQUIT,SIGALRM};

void usage() {
    fprintf(stderr,"usage: %s COMMAND [options] RING\n"
                 "\n"
                 "commands\n"
                 "--------\n"
                 " status          get counters\n"
                 " create          create ring(s)\n"
                 " read            read frames- raw\n"
                 " readhex         read frames- hex/ascii\n"
                 " write           frames from stdin lines\n"
                 " writehex HEX    frame from hex argument\n"
                 " writefile FILE  frame from file content\n"
                 " tee A B C       tee ring A to rings B C\n"
                 " mount DIR       mount/unmount ram disk\n"
                 " pub [ip:]port   publish ring over TCP\n"
                 " sub host:port   subscribe to ring pub\n"
                 "\n"
                 "read options\n"
                 "------------\n"
                 "  -b            wait for data when exhausted\n"
                 "\n"
                 "create options\n"
                 "--------------\n"
                 "  -s size        size with kmgt suffix\n"
                 "  -A file        copy file into app-data\n"
                 "  -N maxmsgs     set max number of messages\n"
                 "  -m dfksl       flags (combinable, default: 0)\n"
                 "      d          drop unread frames when full\n"
                 "      f          farm of independent readers\n"
                 "      k          keep ring as-is if it exists\n"
                 "      l          lock into memory when opened\n"
                 "      s          sync after each i/o\n"
                 "\n"
                 "status options\n"
                 "--------------\n"
                 "  -A file        copy app-data out to file\n"
                 "\n"
                 "pub options\n"
                 "--------------\n"
                 "  -P             omit frame length prefixes\n"
                 "\n"
                 "mount options\n"
                 "-------------\n"
                 "  -d ...         create subdirs DIR SUBDIR ...\n"
                 "  -s size        size with kmgt suffix [tmpfs]\n"
                 "  -q             query presence and utilization\n"
                 "  -u             unmount\n"
                 "  -t TYPE        ramdisk type\n"
                 "     tmpfs       swappable, size-limited\n"
                 "     ramfs       unswappable, size-unlimited\n"
                 "     hugetlbfs   unswappable, hugepage-backed\n"
                 "  -H num         set /proc/sys/vm/nr_hugepages\n"
                 "                 num = size-needed/huge-pagesize\n"
                 "\n"
                 "\n", cfg.prog);
  exit(-1);
}

void hexdump(char *buf, size_t len) {
  size_t i,n=0;
  unsigned char c;
  while(n < len) {
    fprintf(stdout,"%08x ", (int)n);
    for(i=0; i < 16; i++) {
      c = (n+i < len) ? buf[n+i] : 0;
      if (n+i < len) fprintf(stdout,"%.2x ", c);
      else fprintf(stdout, "   ");
    }
    for(i=0; i < 16; i++) {
      c = (n+i < len) ? buf[n+i] : ' ';
      if (c < 0x20 || c > 0x7e) c = '.';
      fprintf(stdout,"%c",c);
    }
    fprintf(stdout,"\n");
    n += 16;
  }
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

int mod_epoll(int events, int fd) {
  struct epoll_event ev;
  int rc = -1, sc;

  memset(&ev, 0, sizeof(ev));
  ev.data.fd = fd;
  ev.events = events;

  sc = epoll_ctl(cfg.epoll_fd, EPOLL_CTL_MOD, fd, &ev);
  if (sc < 0) {
    fprintf(stderr, "epoll_ctl: %s\n", strerror(errno));
    goto done;
  }

  rc = 0;

 done:
  return rc;
}

int new_epoll(int events, int fd) {
  int rc;
  struct epoll_event ev;
  memset(&ev,0,sizeof(ev));
  ev.events = events;
  ev.data.fd= fd;
  rc = epoll_ctl(cfg.epoll_fd, EPOLL_CTL_ADD, fd, &ev);
  if (rc == -1) {
    fprintf(stderr,"epoll_ctl: %s\n", strerror(errno));
  }
  return rc;
}

/*
 * parse_spec
 *
 * parse [<ip|hostname>]:<port>, populate sockaddr_in
 *
 * if there was no IP/hostname, ip is set to INADDR_ANY
 * port is required, or the function fails
 *
 * returns 
 *  0 success
 * -1 error
 *
 */
int parse_spec(char *spec, struct sockaddr_in *sa) {
  char *colon=NULL, *p, *h;
  struct hostent *e;
  int rc = -1, port;
  uint32_t s_addr;

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

  if (cfg.verbose) {
    fprintf(stderr, "%s -> IP %s port %d\n",
      spec, inet_ntoa(sa->sin_addr), port);
  }

  rc = 0;

 done:
  if (colon) *colon = ':';
  return rc;
}

#define TMPFS_MAGIC  0x01021994
#define RAMFS_MAGIC  0x858458F6
#define HUGEFS_MAGIC 0x958458F6
/*
 * test if the directory exists, prior to mounting a tmpfs/ramfs on it.
 * also report if its already a ramdisk mount to prevent stacking.
 *
 * returns
 *   0 suitable directory (exists, not already a ramdisk mount)
 *  -1 error
 *  -2 directory is already a ramdisk mountpoint
 *
 */
int suitable_mountpoint(char *dir, struct stat *sb, struct statfs *sf) {
  int sc, is_mountpoint, is_tmpfs, is_ramfs, is_hugefs;
  size_t dlen = strlen(dir);
  struct stat psb;

  if (dlen+4 > PATH_MAX) {
    fprintf(stderr, "path too long\n");
    return -1;
  }

  /* does mount point exist? */
  sc = stat(dir, sb);
  if (sc < 0) {
    fprintf(stderr, "no mount point %s: %s\n", dir, strerror(errno));
    return -1;
  }

  /* has to be a directory */
  if (S_ISDIR(sb->st_mode) == 0) {
    fprintf(stderr, "mount point %s: not a directory\n", dir);
    return -1;
  }

  /* is dir already a mountpoint? */
  memcpy(cfg.parent,dir,dlen+1);
  strcat(cfg.parent,"/..");
  sc = stat(cfg.parent, &psb);
  if (sc < 0) {
    fprintf(stderr, "stat %s: %s\n", cfg.parent, strerror(errno));
    return -1;
  }
  is_mountpoint = (psb.st_dev == sb->st_dev) ? 0 : 1;

  /* is dir already a tmpfs/ramfs? */
  sc = statfs(dir, sf);
  if (sc < 0) {
    fprintf(stderr, "statfs %s: %s\n", dir, strerror(errno));
    return -1;
  }
  is_tmpfs  = (sf->f_type == TMPFS_MAGIC);
  is_ramfs  = (sf->f_type == RAMFS_MAGIC);
  is_hugefs = (sf->f_type == HUGEFS_MAGIC);

  if (is_mountpoint && (is_tmpfs || is_ramfs || is_hugefs)) {
    return -2;
  }

  return 0;
}

#define KB 1024L
#define MB (1024*1024L)
#define GB (1024*1024*1024L)
int query_ramdisk(char *ramdisk) {
  struct stat sb;
  struct statfs sf;

  if (suitable_mountpoint(ramdisk, &sb, &sf) != -2) {
    printf("%s: not a ramdisk\n", ramdisk);
    return -1;
  }

  if (sf.f_type == RAMFS_MAGIC) {
    printf("%s: ramfs (unbounded size)\n", ramdisk);
    return 0;
  }

  if (sf.f_type == HUGEFS_MAGIC) {
    printf("%s: hugetlbfs (unbounded size)\n", ramdisk);
    return 0;
  }

  char szb[100];
  long bytes = sf.f_bsize*sf.f_blocks;
  if (bytes < KB) snprintf(szb, sizeof(szb), "%ld bytes", bytes);
  else if (bytes < MB) snprintf(szb, sizeof(szb), "%ld kb", bytes/KB);
  else if (bytes < GB) snprintf(szb, sizeof(szb), "%ld mb", bytes/MB);
  else                 snprintf(szb, sizeof(szb), "%ld gb", bytes/GB);
  int used_pct = 100 - (sf.f_bfree * 100.0 / sf.f_blocks);
  printf("%s: tmpfs of size %s (%d%% used)\n", ramdisk, szb, used_pct);
  return 0;
}

/* mmap file, placing its size in len and returning address or NULL on error.
 * caller should munmap the buffer eventually.
 */
char *map(char *file, size_t *len) {
  int fd = -1, rc = -1, sc;
  char *buf = NULL;
  struct stat s;

  fd = open(file, O_RDONLY);
  if (fd < 0) {
    fprintf(stderr,"open: %s\n", strerror(errno));
    goto done;
  }

  sc = fstat(fd, &s);
  if (sc < 0) {
    fprintf(stderr,"fstat: %s\n", strerror(errno));
    goto done;
  }

  if (s.st_size == 0) {
    fprintf(stderr,"error: mmap zero size file\n");
    goto done;
  }

  buf = mmap(0, s.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  if (buf == MAP_FAILED) {
    fprintf(stderr, "mmap: %s\n", strerror(errno));
    buf = NULL;
    goto done;
  }

  rc = 0;
  *len = s.st_size;

 done:
  if (fd != -1) close(fd);
  if (rc && buf) { munmap(buf, s.st_size); buf = NULL; }
  return buf;
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

  assert( cfg.mode == mode_sub );

  eob = cfg.buf + cfg.sub_buf_used;
  c = cfg.buf;
  while(1) {
    if (c + sizeof(uint32_t) > eob) break;
    memcpy(&blen, c, sizeof(uint32_t));
    if (blen > MAX_FRAME) {
      fprintf(stderr,"discarding overlong frame\n");
      goto done;
    }
    body = c + sizeof(uint32_t);
    if (body + blen > eob) break;
    cfg.sub_iov[ iov_used ].iov_base = body;
    cfg.sub_iov[ iov_used ].iov_len  = blen;
    iov_used++;
    if (iov_used == NUMIOV) break;
    c += sizeof(uint32_t) + blen;
  }

  if (iov_used == 0) {
    rc = 0;
    goto done;
  }

  nr = shr_writev(cfg.shr, cfg.sub_iov, iov_used);
  if (nr < 0) {
    fprintf(stderr,"shr_writev: error (%zd)\n", nr);
    goto done;
  }

  /* if buffer ends with partial frame, save it */
  if (c < eob) memmove(cfg.buf, c, eob - c);
  cfg.sub_buf_used = eob - c;

  rc = 0;

 done:
  return rc;
}

/*
 * do_subscriber
 *
 *
 */
int do_subscriber(void) {
  int rc = -1, sc;
  size_t avail;
  ssize_t nr;
  char *b;

  assert( cfg.mode == mode_sub );
  assert( cfg.sub_fd != -1 );

  /* the buffer must have some free space because
   * any time we read data we process it right here,
   * leaving at most a tiny fragment of a partial
   * frame to prepend the next read */
  assert(cfg.sub_buf_used < BUFLEN);
  avail = BUFLEN - cfg.sub_buf_used;
  b = cfg.buf + cfg.sub_buf_used;

  nr = read(cfg.sub_fd, b, avail);
  if (nr <= 0) {
    fprintf(stderr, "read: %s\n", nr ? strerror(errno) : "eof");
    goto done;
  }

  assert(nr > 0);
  cfg.sub_buf_used += nr;
  if (decode_frames() < 0) goto done;

  rc = 0;

 done:
  return rc;
}

int close_client(void) {
  int rc = -1, sc;

  close(cfg.pub_fd);
  cfg.pub_fd = -1;

  /* stop monitoring ring */
  sc = mod_epoll(0, cfg.fd);
  if (sc < 0) goto done;

  rc = 0;

 done:
  return rc;
}

/*
 * handle_client
 *
 * in pub mode, client sent data or closed
 *
 */
int handle_client(void) {
  int rc = -1, sc;
  char buf[100], *b;
  ssize_t nr;

  assert( cfg.mode == mode_pub );
  assert( cfg.pub_fd != -1 );

  nr = recv(cfg.pub_fd, buf, sizeof(buf), MSG_DONTWAIT);
  if (nr < 0) {
    fprintf(stderr, "recv: %s\n", strerror(errno) );
    close_client();
    return -1;
  }

  if (nr == 0) {
    fprintf(stderr, "client disconnected\n");
    close_client();
    return 0;
  }

  assert(nr > 0);
  return 0;
}

/*
 * accept_client
 *
 * in pub mode, accept client connection
 *
 */
int accept_client(void) {
  struct sockaddr_in remote;
  socklen_t sz = sizeof(remote);
  int rc = -1, sc;

  assert( cfg.mode == mode_pub );

  sc = accept(cfg.listen_fd, (struct sockaddr*)&remote, &sz);
  if (sc < 0) {
    fprintf(stderr, "accept: %s\n", strerror(errno));
    goto done;
  }

  /* one client at a time */
  if (cfg.pub_fd != -1) {
    fprintf(stderr, "refusing secondary client connection\n");
    close(sc);
    rc = 0;
    goto done;
  }

  cfg.pub_fd = sc;
  sc = new_epoll(EPOLLIN, cfg.pub_fd);
  fprintf(stderr, "connection from %s\n", inet_ntoa(remote.sin_addr));

  /* start epolling ring */
  sc = mod_epoll(EPOLLIN, cfg.fd);
  if (sc < 0) goto done;

  rc = 0;

 done:
  return rc;
}

int handle_signal(void) {
  struct signalfd_siginfo info;
  ssize_t nr;
  int rc=-1;
  
  nr = read(cfg.signal_fd, &info, sizeof(info));
  if (nr != sizeof(info)) {
    fprintf(stderr,"failed to read signal fd buffer\n");
    goto done;
  }

  switch(info.ssi_signo) {
    case SIGALRM: 
      /* periodic work */
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

/*
 * handle_io
 *
 * called when ring is readable
 */
int handle_io(void) {
  int rc = -1, sc, fl;
  ssize_t nr, np;
  uint32_t len32;
  size_t len;
  char *out;

  nr = shr_read(cfg.shr, cfg.buf, BUFLEN);
  if (nr < 0) {
    fprintf(stderr, "shr_read: error (%zd)\n", nr);
    goto done;
  }

  /* spurious wakeup, allowed */
  if ((nr == 0) && cfg.block) {
    rc = 0;
    goto done;
  }

  switch (cfg.mode) {

    case mode_read:
    case mode_read_hex:

      /* non blocking end-of-data */
      if ((cfg.block == 0) && (nr == 0)) {
        fprintf(stderr, "shr_read: end-of-data (0)\n");
        goto done;
      }
      /* data to print */
      assert(nr > 0);
      if (cfg.mode == mode_read_hex) hexdump(cfg.buf,nr);
      else printf("%.*s\n", (int)nr, cfg.buf);
      fflush(stdout);
      break;

    case mode_pub:
      assert(cfg.pub_fd != -1);
      if (nr == 0) {
        rc = 0;
        goto done;
      }

      /* write frame length prefix */
      if (cfg.pub_len_prefix) {
        if (nr > UINT32_MAX) {
          fprintf(stderr, "frame too long for pub\n");
          goto done;
        }
        len32 = (uint32_t)nr;
        np = write(cfg.pub_fd, &len32, sizeof(len32));
        if (np < 0) {
          fprintf(stderr, "write: %s\n", strerror(errno));
          if (close_client() < 0) goto done;
          break;
        }
        assert(np == sizeof(len32));
      }

      /* write frame proper */
      out = cfg.buf;
      len = nr;
      do {
        nr = write(cfg.pub_fd, out, len);
        if (nr < 0) {
          fprintf(stderr, "write: %s\n", strerror(errno));
          if (close_client() < 0) goto done;
          break;
        }
        len -= nr;
        out += nr;
      } while (len > 0);
      break;

    case mode_tee:
      assert(cfg.o1 && cfg.o2);
      if (nr == 0) {
        rc = 0;
        goto done;
      }
      if (shr_write(cfg.o1, cfg.buf, nr) < 0) {
        fprintf(stderr, "shr_write: error\n");
        goto done;
      }
      if (shr_write(cfg.o2, cfg.buf, nr) < 0) {
        fprintf(stderr, "shr_write: error\n");
        goto done;
      }
      break;

    default:
      assert(0);
      goto done;
      break;
  }

  rc = 0;

 done:
  return rc;
}

/*
 * setup_subscriber
 *
 *
 */
int setup_subscriber(void) {
  int rc = -1, sc;
  ssize_t nr;

  cfg.sub_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (cfg.sub_fd == -1) {
    fprintf(stderr, "socket: %s\n", strerror(errno));
    goto done;
  }

  sc = parse_spec(cfg.addr_spec, &cfg.addr);
  if (sc < 0) goto done;

  /* require host for subscriber */
  if (cfg.addr.sin_addr.s_addr == 0) usage();

  sc = connect(cfg.sub_fd, (struct sockaddr*)&cfg.addr, sizeof(cfg.addr));
  if (sc < 0) {
    fprintf(stderr, "connect: %s\n", strerror(errno));
    goto done;
  }

  rc = 0;

 done:
  return rc;
}

/*
 * setup_listener
 *
 * bring up listening socket for publishing
 *
 */
int setup_listener(void) {
  int rc = -1, sc, one=1;

  cfg.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (cfg.listen_fd == -1) {
    fprintf(stderr, "socket: %s\n", strerror(errno));
    goto done;
  }

  sc = parse_spec(cfg.addr_spec, &cfg.addr);
  if (sc < 0) goto done;

  sc = setsockopt(cfg.listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  if (sc < 0) {
    fprintf(stderr, "setsockopt: %s\n", strerror(errno));
    goto done;
  }

  sc = bind(cfg.listen_fd, (struct sockaddr*)&cfg.addr, sizeof(cfg.addr));
  if (sc < 0) {
    fprintf(stderr, "bind: %s\n", strerror(errno));
    goto done;
  }

  sc = listen(cfg.listen_fd, 1);
  if (sc < 0) {
    fprintf(stderr, "listen: %s\n", strerror(errno));
    goto done;
  }

  rc = 0;

 done:
  return rc;
}

int main(int argc, char *argv[]) {
  int n, one_shot=0, opt, rc=-1, sc, mode, fd, epoll_mode, tmo, ec;
  char unit, *c, *app_data, *cmd, line[1000], opts[100],
    *ring1=NULL, *ring2=NULL, *data, *sub;
  struct epoll_event ev;
  struct shr_stat stat;
  size_t app_len = 0;
  cfg.prog = argv[0];
  struct statfs sf;
  struct stat sb;
  ssize_t nr;

  if (argc < 3) usage();
 
  cmd = argv[1];
  if      (!strcmp(cmd, "status"))    cfg.mode = mode_status;
  else if (!strcmp(cmd, "create"))    cfg.mode = mode_create;
  else if (!strcmp(cmd, "mount"))     cfg.mode = mode_mount;
  else if (!strcmp(cmd, "read"))      cfg.mode = mode_read;
  else if (!strcmp(cmd, "readhex"))   cfg.mode = mode_read_hex;
  else if (!strcmp(cmd, "write"))     cfg.mode = mode_write;
  else if (!strcmp(cmd, "writefile")) cfg.mode = mode_write_file;
  else if (!strcmp(cmd, "writehex"))  cfg.mode = mode_write_hex;
  else if (!strcmp(cmd, "pub"))       cfg.mode = mode_pub;
  else if (!strcmp(cmd, "sub"))       cfg.mode = mode_sub;
  else if (!strcmp(cmd, "tee"))       cfg.mode = mode_tee;
  else /* "help" or anything else */  usage();

  argv++;
  argc--;

  if (cfg.mode == mode_write_hex) {
      if (argc < 3) usage();
      cfg.hex_arg = strdup(argv[1]);
      argv++;
      argc--;
  }

  if (cfg.mode == mode_write_file) {
      if (argc < 3) usage();
      cfg.wfile = strdup(argv[1]);
      argv++;
      argc--;
  }

  if ((cfg.mode == mode_pub) || (cfg.mode == mode_sub)) {
      if (argc < 3) usage();
      cfg.addr_spec = strdup(argv[1]);
      argv++;
      argc--;
  }

  while ( (opt = getopt(argc,argv,"vbs:m:A:N:t:uqdH:P")) > 0) {
    switch(opt) {
      default : usage(); break;
      case 'v': cfg.verbose++; break;
      case 'b': cfg.block = 1; break;
      case 'P': cfg.pub_len_prefix = 0; break;
      case 't': cfg.fs_type = strdup(optarg); break;
      case 'd': cfg.mount_mode = mnt_make_subdirs; break;
      case 'u': cfg.mount_mode = mnt_unmount; break;
      case 'q': cfg.mount_mode = mnt_query; break;
      case 'H': cfg.nr_hugepages = atoi(optarg);
      case 'N': cfg.flags |= SHR_MAXMSGS_2;
                cfg.max_msgs = atoi(optarg);
                break;
      case 's':  /* ring size */
         sc = sscanf(optarg, "%ld%c", &cfg.size, &unit);
         if (sc == 0) usage();
         if (sc == 2) {
            switch (unit) {
              case 't': case 'T': cfg.size *= 1024; /* fall through */
              case 'g': case 'G': cfg.size *= 1024; /* fall through */
              case 'm': case 'M': cfg.size *= 1024; /* fall through */
              case 'k': case 'K': cfg.size *= 1024; break;
              default: usage(); break;
            }
         }
         break;
      case 'm': /* ring mode */
         c = optarg;
         while((*c) != '\0') {
           switch (*c) {
             case 'd': cfg.flags |= SHR_DROP; break;
             case 'k': cfg.flags |= SHR_KEEPEXIST; break;
             case 'f': cfg.flags |= SHR_FARM; break;
             case 's': cfg.flags |= SHR_SYNC; break;
             case 'l': cfg.flags |= SHR_MLOCK; break;
             default: usage(); break;
           }
           c++;
         }
         break;
      case 'A': cfg.wfile = strdup(optarg); break;
    }
  }

  if (optind >= argc) usage();
  cfg.ring = argv[optind];
  
  switch(cfg.mode) {

    case mode_mount:
      cfg.mountpoint = argv[optind++];
      one_shot=1;
      switch (cfg.mount_mode) {
        case mnt_mount:
        case mnt_make_subdirs:
          if (suitable_mountpoint(cfg.mountpoint, &sb, &sf) == -2) {
            printf("%s: already a ramdisk\n", cfg.mountpoint);
            goto done;
          }
          snprintf(opts, sizeof(opts), "size=%zu", cfg.size);
          data = cfg.size ? opts : NULL;
          sc = mount("shr", cfg.mountpoint, cfg.fs_type, MS_NOATIME, data);
          if (sc < 0) {
            fprintf(stderr, "mount %s: %s\n", cfg.mountpoint, strerror(errno));
            goto done;
          }
          sc = chdir(cfg.mountpoint);
          if (sc < 0) {
            fprintf(stderr, "chdir %s: %s\n", cfg.mountpoint, strerror(errno));
            goto done;
          }
          while ((cfg.mount_mode == mnt_make_subdirs) && (optind < argc)) {
            sub = argv[optind++];
            sc = mkdir(sub, 0777);
            if (sc < 0) {
              fprintf(stderr, "mkdir %s: %s\n", sub, strerror(errno));
              goto done;
            }
          }
          if (cfg.nr_hugepages) {
            snprintf(cfg.tmp, sizeof(cfg.tmp), "%d\n", cfg.nr_hugepages);
            char *nrfile = "/proc/sys/vm/nr_hugepages";
            fd = open(nrfile, O_WRONLY);
            if (fd == -1) {
              fprintf(stderr, "open %s: %s\n", nrfile, strerror(errno));
              goto done;
            }
            nr = write(fd, cfg.tmp, strlen(cfg.tmp));
            if (nr < 0) {
              fprintf(stderr, "write %s: %s\n", nrfile, strerror(errno));
              goto done;
            }
            close(fd);
          }
          break;
        case mnt_unmount:
          sc = umount(cfg.mountpoint);
          if (sc < 0) {
            fprintf(stderr, "umount: %s\n", strerror(errno));
            goto done;
          }
          break;
        case mnt_query:
          query_ramdisk(cfg.mountpoint);
          break;
        default:
          fprintf(stderr, "unimplemented mode\n");
          goto done;
          break;
      }
      break;

    case mode_create:
      one_shot=1;
      if (cfg.size == 0) usage();
      if (cfg.wfile) { 
        cfg.flags |= SHR_APPDATA;
        cfg.wfile_buf = map(cfg.wfile, &cfg.wfile_len);
        if (cfg.wfile_buf == NULL) goto done;
        while (optind < argc) {
          rc = shr_init(argv[optind++], cfg.size, cfg.flags, 
                cfg.wfile_buf, cfg.wfile_len, cfg.max_msgs);
          if (rc < 0) goto done;
        }
      } else {
        while (optind < argc) {
          rc = shr_init(argv[optind++], cfg.size, cfg.flags, cfg.max_msgs);
          if (rc < 0) goto done;
        }
      }
      break;

    case mode_status:
      one_shot=1;
      cfg.shr = shr_open(cfg.ring, SHR_RDONLY);
      if (cfg.shr == NULL) goto done;
      rc = shr_stat(cfg.shr, &stat, NULL);
      if (rc < 0) goto done;
      printf(" bytes-written %ld\n"
             " bytes-read %ld\n"
             " bytes-dropped %ld\n"
             " messages-written %ld\n"
             " messages-read %ld\n"
             " messages-dropped %ld\n"
             " ring-size %ld\n"
             " bytes-ready %ld\n"
             " max-messages %ld\n"
             " messages-ready %ld\n",
         stat.bw, stat.br, stat.bd, stat.mw, stat.mr, stat.md, stat.bn,
         stat.bu, stat.mm, stat.mu);

      printf(" attributes ");
      if (stat.flags == 0)          printf("none");
      if (stat.flags & SHR_DROP)    printf("drop ");
      if (stat.flags & SHR_APPDATA) printf("appdata ");
      if (stat.flags & SHR_FARM)    printf("farm ");
      if (stat.flags & SHR_MLOCK)   printf("mlock ");
      if (stat.flags & SHR_SYNC)    printf("sync ");
      printf("\n");

      app_data = NULL;
      rc = shr_appdata(cfg.shr, (void**)&app_data, NULL, &app_len);
      printf(" app-data %zu\n", app_len);
      if (rc == 0) {
        assert(app_data && app_len);
        if (cfg.verbose) printf("%.*s\n", (int)app_len, app_data);
        if (cfg.wfile) {
          int fd = open(cfg.wfile, O_WRONLY|O_CREAT|O_TRUNC, 0644);
          if (fd < 0) {
            fprintf(stderr, "open %s: %s\n", cfg.wfile, strerror(errno));
          } else { 
            nr = write(fd, app_data, app_len);
            if (nr < 0) {
              fprintf(stderr, "write: %s\n", strerror(errno));
              goto done;
            }
            close(fd);
          }
        }
        free(app_data);
      }
      break;

    case mode_pub:
      sc = setup_listener();
      if (sc < 0) goto done; /* FALLTHRU */
    case mode_read:          /* FALLTHRU */
    case mode_read_hex:      /* FALLTHRU */
      mode = SHR_RDONLY | SHR_NONBLOCK;
      cfg.shr = shr_open(cfg.ring, mode);
      if (cfg.shr == NULL) goto done;
      cfg.fd = shr_get_selectable_fd(cfg.shr);
      if (cfg.fd < 0) goto done;
      break;

    case mode_write:
      one_shot=1;
      cfg.shr = shr_open(cfg.ring, SHR_WRONLY);
      if (cfg.shr == NULL) goto done;
      while (fgets(line, sizeof(line), stdin)) {
        nr = strlen(line);
        if ((nr > 0) && (line[nr-1] == '\n')) line[nr--] = '\0';
        if (shr_write(cfg.shr, line, nr) < 0) {
          fprintf(stderr, "shr_write: error\n");
          goto done;
        }
      }
      break;

    case mode_write_hex:
      one_shot=1;
      cfg.shr = shr_open(cfg.ring, SHR_WRONLY);
      if (cfg.shr == NULL) goto done;
      nr = unhex(cfg.hex_arg);
      if (nr <= 0) goto done;
      if (shr_write(cfg.shr, cfg.hex_arg, nr) < 0) {
        fprintf(stderr, "shr_write: error\n");
        goto done;
      }
      break;

    case mode_write_file:
      one_shot=1;
      cfg.shr = shr_open(cfg.ring, SHR_WRONLY);
      if (cfg.shr == NULL) goto done;
      cfg.wfile_buf = map(cfg.wfile, &cfg.wfile_len);
      if (cfg.wfile_buf == NULL) goto done;
      if (shr_write(cfg.shr, cfg.wfile_buf, cfg.wfile_len) < 0) {
        fprintf(stderr, "shr_write: error\n");
        goto done;
      }
      break;

    case mode_tee:
      optind++;
      if (optind + 2 > argc) usage();
      ring1 = argv[optind++];
      ring2 = argv[optind++];
      cfg.shr = shr_open(cfg.ring, SHR_RDONLY|SHR_NONBLOCK );
      if (cfg.shr == NULL) goto done;
      cfg.fd = shr_get_selectable_fd(cfg.shr);
      if (cfg.fd < 0) goto done;
      if ((ring1 == NULL) || (ring2 == NULL)) goto done;
      cfg.o1 = shr_open(ring1, SHR_WRONLY);
      cfg.o2 = shr_open(ring2, SHR_WRONLY);
      if ((cfg.o1 == NULL) || (cfg.o2 == NULL)) goto done;
      break;

    case mode_sub:
      cfg.shr = shr_open(cfg.ring, SHR_WRONLY);
      if (cfg.shr == NULL) goto done;
      sc = setup_subscriber();
      if (sc < 0) goto done;
      break;

    default: 
      assert(0);
      break;
  }

  if (one_shot) {
    rc = 0;
    goto done;
  }

  /* block signals. we accept signals in signalfd */
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
  sc = new_epoll(EPOLLIN, cfg.signal_fd);
  if (sc < 0) goto done;

  /* most modes poll the ring immediately */
  epoll_mode = (cfg.mode == mode_pub) ? 0 : EPOLLIN;

  switch (cfg.mode) {
    case mode_tee:
    case mode_read:
    case mode_read_hex:
      sc = new_epoll(EPOLLIN, cfg.fd);
      if (sc < 0) goto done;
      break;
    case mode_pub:
      sc = new_epoll(EPOLLIN, cfg.listen_fd);
      if (sc < 0) goto done;
      sc = new_epoll(0, cfg.fd); /* change flags upon connect */
      if (sc < 0) goto done;
      break;
    case mode_sub:
      sc = new_epoll(EPOLLIN, cfg.sub_fd);
      if (sc < 0) goto done;
      break;
    default:
      assert(0);
      break;
  }

  /* kick off timer */
  alarm(1);

  /* induce an immediate timeout in non-blocking read mode */
  tmo = ((cfg.block == 0) && 
         ((cfg.mode == mode_read) || (cfg.mode == mode_read_hex))) ? 0 : -1;

  do { 
    ec = epoll_wait(cfg.epoll_fd, &ev, 1, tmo);
    if      (ec < 0)  fprintf(stderr, "epoll: %s\n", strerror(errno));
    else if (ec == 0) /* print no-data */ { if (handle_io()     < 0) goto done;}
    else if (ev.data.fd == cfg.fd)        { if (handle_io()     < 0) goto done;}
    else if (ev.data.fd == cfg.signal_fd) { if (handle_signal() < 0) goto done;}
    else if (ev.data.fd == cfg.listen_fd) { if (accept_client() < 0) goto done;}
    else if (ev.data.fd == cfg.pub_fd)    { if (handle_client() < 0) goto done;}
    else if (ev.data.fd == cfg.sub_fd)    { if (do_subscriber() < 0) goto done;}
    else { assert(0); }
  } while (ec >= 0);

  rc = 0;
 
 done:
  if (cfg.signal_fd != -1) close(cfg.signal_fd);
  if (cfg.listen_fd != -1) close(cfg.listen_fd);
  if (cfg.pub_fd != -1) close(cfg.pub_fd);
  if (cfg.sub_fd != -1) close(cfg.sub_fd);
  if (cfg.epoll_fd != -1) close(cfg.epoll_fd);
  if (cfg.shr) shr_close(cfg.shr);
  /* don't close cfg.fd - it's done in shr_close */
  if (cfg.o1) shr_close(cfg.o1);
  if (cfg.o2) shr_close(cfg.o2);
  if (cfg.hex_arg) free(cfg.hex_arg);
  // don't free(cfg.fs_type);, default is static string
  if (cfg.wfile_buf) munmap(cfg.wfile_buf, cfg.wfile_len);
  if (cfg.wfile) free(cfg.wfile);
  if (cfg.addr_spec) free(cfg.addr_spec);
  return rc;
}

