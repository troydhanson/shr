/* define _GNU_SOURCE to get SCM_CREDENTIALS *
 * or include AC_GNU_SOURCE in configure.ac */
#ifndef _GNU_SOURCE 
#define _GNU_SOURCE 
#endif

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/epoll.h>
#include <sys/un.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/un.h>
#include <errno.h>
#include <fcntl.h>
#include "bw.h"

struct bw_t {
  int flags;
  bw_handle volatile * h;

  /* wake mode */
  int seqno;
  int fd[BW_WAITMAX];
  char name[BW_WAITMAX][BW_NAMELEN];
  char zero;

  /* wait mode */
  int slotno;
  int listenfd;
  int selffd;
  int epollfd;
  char discard[8];
  struct mmsghdr *drain;

  /* for tracing */
  struct iovec iov;
  struct msghdr hdr;
  union {
    struct cmsghdr cmh;
    char   control[CMSG_SPACE(sizeof(struct ucred))];
  } control_un;
};

/* prototypes */
int has_pid_socket(pid_t pid, char *name);

/*
 * asc2sock
 *
 * helper function that converts the friendly form
 * of the abstract socket name to sockaddr_un form
 *
 * friendly     @abcde\0
 * sockaddr_un \0abcde
 *
 */
static void asc2sock(char *name, struct sockaddr_un *addr, socklen_t *len) {
  memset(addr, 0, sizeof(*addr));
  addr->sun_family = AF_UNIX;
  memcpy(addr->sun_path+1, name+1, BW_NAMELEN-1);
  /* sa_family_t + leading nul + name w/out @ or nul */
  *len = sizeof(sa_family_t) + 1 + strlen(name+1);
}

/*
 * sock2asc
 *
 * helper function that converts sockaddr_un form
 * of the abstract socket name to the friendly form
 *
 * sockaddr_un \0abcde
 * friendly     @abcde\0
 *
 */
static void sock2asc(char *name, struct sockaddr_un *addr, socklen_t addrlen) {
  memset(name, 0, BW_NAMELEN);
  int len = addrlen - sizeof(sa_family_t) - 1;
  assert(len + 2 <= BW_NAMELEN);
  name[0] = '@';
  memcpy(name + 1, addr->sun_path + 1, len);
  name[len+1] = '\0';
}

/*
 * zero any pid occupying a slot if it no longer exists.
 * this may happen if a pid exits uncleanly
 *
 * returns:
 *   0
 *
 */
static int prune_handle(bw_handle *h) {
  int n, sc;

  for(n = 0; n < BW_WAITMAX; n++) {
    if (h->wr[n].pid == 0) continue;
    sc = has_pid_socket(h->wr[n].pid, h->wr[n].name); 
    if (sc > 0) continue; /* slot occupied, leave alone */
    if (sc < 0) continue; /* cannot confirm, leave alone */
    assert(sc == 0);
    h->wr[n].pid = 0;
    h->seqno++;
  }

  return 0;
}

/*
 * connect wake-mode caller to one waiting socket
 *
 * the unix domain socket name is like @abcde in 
 * /proc/net/unix and in our structures. here we
 * connect using its abstract name (@ becomes \0)
 *
 * special case: failure to connect to an extinct 
 * socket is a success; we confirm and purge slot
 *
 * returns:
 *   0 on success
 *  -1 on error
 *
 */
static int open_socket(bw_t *w, int n) {
  bw_handle *h = (bw_handle *)w->h;
  struct sockaddr_un addr;
  int sc, fd=-1, errtmp;
  socklen_t len;

  assert(w->flags & BW_WAKE);
  if (w->flags & BW_TRACE) {
    bw_log("opening %s\n", w->name[n]);
  }

  fd = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (fd < 0) {
    bw_log("socket: %s\n", strerror(errno));
    return -1;
  }

  asc2sock(w->name[n], &addr, &len);
  sc = connect(fd, (struct sockaddr*)&addr, len);
  if (sc == 0) {
    w->fd[n] = fd;
    return 0;
  }

  /* connect failed. save errno, close descriptor */
  errtmp = errno;
  close(fd);

  /* release slot if socket owner exited; success */
  sc = has_pid_socket(h->wr[n].pid, w->name[n]); 
  if (sc == 0) {
    h->wr[n].pid = 0;
    h->seqno++;
    return 0;
  }

  /* any other reason for connect failure is error */
  bw_log("connect: %s\n", strerror(errtmp));
  return -1;
}

/*
 * ensure wake-mode caller has open socket to all 
 * active slots in handle, and only these sockets
 */
static int bw_sync(bw_t *w) {
  bw_handle *h = (bw_handle *)w->h;
  int rc = -1, n;

  assert(w->flags & BW_WAKE);
  if (w->seqno == h->seqno) {
    rc = 0;
    goto done;
  }

  for(n = 0; n < BW_WAITMAX; n++) {

   /* has owner released slot and we need to close socket,
    * or, slot is active but name changed since we opened? */
    if ((w->fd[n] != -1) && ((h->wr[n].pid == 0) || 
      memcmp(h->wr[n].name, w->name[n], BW_NAMELEN))) {
      close(w->fd[n]);
      w->fd[n] = -1;
    }

    /* is slot active and we need to open socket to it? */
    if ((h->wr[n].pid != 0) && (w->fd[n] == -1)) {
      memcpy(w->name[n], h->wr[n].name, BW_NAMELEN);
      if (open_socket(w,n) < 0) goto done;
    }
  }

  w->seqno = h->seqno;
  rc = 0;

 done:
  return rc;
}

static int make_socket(bw_t *w, int n) {
  int rc=-1, fd=-1, sc, one=1;
  struct sockaddr_un addr;
  char *name;

  fd = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (fd < 0) {
    bw_log("socket: %s\n", strerror(errno));
    goto done;
  }

  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  socklen_t want_autobind = sizeof(sa_family_t);
  sc = bind(fd, (struct sockaddr*)&addr, want_autobind);
  if (sc < 0) {
    bw_log("bind: %s\n", strerror(errno));
    goto done;
  }

  sc = (w->flags & BW_TRACE) ?
       setsockopt(fd, SOL_SOCKET, SO_PASSCRED, &one, sizeof(one)) : 0;
  if (sc < 0) {
    bw_log("setsockopt: %s\n", strerror(errno));
    goto done;
  }

  socklen_t addrlen = sizeof(struct sockaddr_un);
  sc = getsockname(fd, (struct sockaddr *)&addr, &addrlen);
  if (sc < 0) {
    bw_log("getsockname: %s\n", strerror(errno));
    goto done;
  }

  name = (char*)(w->h->wr[ n ].name);
  sock2asc(name, &addr, addrlen);

  rc = 0;

 done:
  if ((rc < 0) && (fd != -1)) { close(fd); fd = -1; }
  return fd;
}

/* acquire a slot in handle */
static int bw_reserve(bw_t *w) {
  int n, rc = -1;

  assert(w->flags & BW_WAIT);

  /* find open slot */
  for (n = 0; n < BW_WAITMAX; n++) {
    if (w->h->wr[n].pid == 0) break;
  }

  if (n == BW_WAITMAX) {
    bw_log("slots exhausted\n");
    goto done;
  }

  w->listenfd = make_socket(w,n);
  if (w->listenfd < 0) goto done;
  w->h->wr[ n ].pid = getpid();
  w->h->seqno++;
  w->slotno = n;

  if (w->flags & BW_TRACE) {
    bw_log("slot %d: inserted %s\n", n, w->h->wr[n].name);
  }

  rc = 0;

 done:
  return rc;
}

/*
 * connect_self
 *
 * connect a datagram socket to our own listening socket.
 * we do this to have a way to queue our own wakeup when
 * returning to the caller after bw_force(w,1) is called
 *
 */
static int connect_self(bw_t *w) {
  bw_handle *h = (bw_handle*)w->h;
  struct sockaddr_un addr;
  socklen_t len;
  int rc = -1, sc, fd=-1;
  char *name;

  assert(w->flags & BW_WAIT);
  assert(w->selffd == -1);

  fd = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (fd < 0) {
    bw_log("socket: %s\n", strerror(errno));
    goto done;
  }

  name = h->wr[ w->slotno ].name;
  asc2sock(name, &addr, &len);

  sc = connect(fd, (struct sockaddr*)&addr, len);
  if (sc < 0) {
    bw_log("connect: %s\n", strerror(errno));
    goto done;
  }

  w->selffd = fd;
  rc = 0;

 done:
  if (rc && (fd != -1)) close(fd);
  return rc;
}

/* API */

/*
 * bw_open
 *
 * open a handle for either wait or wake 
 *
 * call WITH handle under lock
 *
 * flags:
 *   BW_WAKE  - wake mode
 *   BW_WAIT  - wait mode (requires int *FD)
 *
 * returns bw_t * or NULL on error
 *
 */
bw_t * bw_open(int flags, bw_handle *h, ...) {
  int n, sc, rc = -1, *fd;
  struct epoll_event ev;
  bw_t *w = NULL;

  va_list ap;
  va_start(ap, h);

  if (((flags & BW_WAKE) ^ (flags & BW_WAIT)) == 0) {
    bw_log("bw_open: invalid mode\n");
    goto done;
  }

  w = calloc(1, sizeof(bw_t));
  if (w == NULL) {
    bw_log("out of memory\n");
    goto done;
  }

  if (flags & BW_WAIT)  w->flags |= BW_WAIT;
  if (flags & BW_WAKE)  w->flags |= BW_WAKE;
  if (flags & BW_TRACE) w->flags |= BW_TRACE;
  for(n=0; n < BW_WAITMAX; n++) w->fd[n] = -1;
  w->h = h;
  w->seqno = -1;
  w->slotno = -1;
  w->listenfd = -1;
  w->selffd = -1;
  w->epollfd = -1;

  sc = prune_handle(h);
  if (sc < 0) goto done;

  if (flags & BW_WAKE) {
    sc = bw_sync(w);
    if (sc < 0) goto done;
    w->iov.iov_base = &w->zero;
    w->iov.iov_len = 1;
    w->hdr.msg_iov = &w->iov;
    w->hdr.msg_iovlen = 1;
  }

  if (flags & BW_WAIT) {
    sc = bw_reserve(w);
    if (sc < 0) goto done;
    fd = (int *)va_arg(ap, void*);
    assert(fd);
    *fd = w->listenfd;
    if (connect_self(w) < 0) goto done;

    /* for blocking via epoll */
    w->epollfd = epoll_create(1);
    if (w->epollfd == -1) {
      bw_log("epoll_create: %s", strerror(errno));
      goto done;
    }

    /* set up listenfd for epoll */
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.fd = w->listenfd;
    sc = epoll_ctl(w->epollfd, EPOLL_CTL_ADD, w->listenfd, &ev);
    if (sc < 0) {
      bw_log("epoll_ctl: %s\n", strerror(errno));
      goto done;
    }

    /* for tracing - receiving peer creds */
    w->control_un.cmh.cmsg_len = CMSG_LEN(sizeof(struct ucred));
    w->control_un.cmh.cmsg_level = SOL_SOCKET;
    w->control_un.cmh.cmsg_type = SCM_CREDENTIALS;
    w->iov.iov_base = &w->discard;
    w->iov.iov_len = sizeof(w->discard);
    w->hdr.msg_control = w->control_un.control;
    w->hdr.msg_controllen = sizeof(w->control_un.control);
    w->hdr.msg_iov = &w->iov;
    w->hdr.msg_iovlen = 1;
  }

  rc = 0;

 done:
  if (rc && w) {
    if (w->slotno != -1)
      w->h->wr[ w->slotno ].pid = 0;
    if (w->listenfd != -1) close(w->listenfd);
    if (w->selffd != -1) close(w->selffd);
    if (w->epollfd != -1) close(w->epollfd);
    free(w);
    w = NULL;
  }
  va_end(ap);
  return w;
}

/*
 * bw_close
 *
 * close socket and release slot in handle (in wait mode)
 * close open connnections to waiting ends (in wake mode)
 *
 * call WITH handle under lock
 *
 */
void bw_close(bw_t *w) {
  int n;

  /* close open descriptors to sockets */
  if (w->flags & BW_WAKE) {
    for(n=0; n < BW_WAITMAX; n++) {
      if (w->fd[n] != -1) close(w->fd[n]);
    }
  }

  /* release our slot in handle */
  if (w->flags & BW_WAIT) {
    assert(w->listenfd != -1);
    close(w->listenfd);

    assert(w->selffd != -1);
    close(w->selffd);

    assert(w->epollfd != -1);
    close(w->epollfd);

    assert(w->slotno != -1);
    w->h->wr[ w->slotno ].pid = 0;
    w->h->seqno++;

    if (w->drain) free(w->drain);
  }

  free(w);
}

static void trace_senders(bw_t *w) {
 struct cmsghdr *cmsg;
 struct ucred *ucredp;

 assert(w->flags & BW_TRACE);

 for (cmsg = CMSG_FIRSTHDR(&w->hdr); cmsg != NULL;
      cmsg = CMSG_NXTHDR(&w->hdr,cmsg)) {

   if (cmsg->cmsg_len   != CMSG_LEN(sizeof(struct ucred))) continue;
   if (cmsg->cmsg_level != SOL_SOCKET) continue;
   if (cmsg->cmsg_type  != SCM_CREDENTIALS) continue;

   ucredp = (struct ucred *) CMSG_DATA(cmsg);

   bw_log("wakeup sent by pid=%d, uid=%d, gid=%d\n",
    (int)ucredp->pid, (int)ucredp->uid, (int)ucredp->gid);
 }
}

/*
 * bw_ready_ul
 *
 * invoked by the wait-mode caller to advise the library that
 * the fd (previously obtained from bw_open) is now readable.
 *
 * this function reads the pending data to clear its readiness
 * and implements tracing
 *
 * call WITHOUT handle under lock
 *
 * returns 0 on success
 *        -1 on error
 *
 */
int bw_ready_ul(bw_t *w) {
  int rc = -1;
  ssize_t nr;

  assert(w->flags & BW_WAIT);

  if (w->flags & BW_TRACE) {
    bw_log("wakeup\n");
  }

  nr = recvmsg(w->listenfd, &w->hdr, MSG_DONTWAIT);
  if (nr < 0) {
    if ((errno != EWOULDBLOCK) && (errno != EAGAIN)) {
      bw_log("recvmsg: %s\n", strerror(errno));
      goto done;
    }
  }

  if (w->flags & BW_TRACE) trace_senders(w);

  rc = 0;

 done:
  return rc;
}

/*
 * bw_wait_ul
 *
 * invoked by a wait-mode caller to induce a blocking wait.
 * afterward, bw_ready_ul is called internally to clear the
 * ready condition.
 *
 * a caller needing to monitor additional descriptors while
 * blocked in bw_wait_ul can use bw_ctl (see BW_POLLFD flag).
 * for example this can be used with a timerfd or signalfd
 * to cause bw_wait_ul to return -2 if one becomes readable.
 *
 * call WITHOUT handle under lock
 *
 * returns 0 on success
 *        -1 on error
 *        -2 (not used)
 *        -3 other descriptor ready (see bw_ctl BW_POLLFD)
 *
 */
int bw_wait_ul(bw_t *w) {
  struct epoll_event ev;
  int rc = -1, sc;

  assert(w);
  assert(w->flags & BW_WAIT);
  assert(w->epollfd != -1);

  if (w->flags & BW_TRACE) {
    bw_log("waiting\n");
  }

  /* wait for listenfd to become readable.
   * other descriptors may also be in the
   * epoll set if bw_ctl(BW_POLLFD) used */
  sc = epoll_wait(w->epollfd, &ev, 1, -1);
  if (sc < 0) {
    bw_log("epoll_wait: %s\n", strerror(errno));
    goto done;
  }

  assert(sc == 1);

  /* "other" descriptor is ready */
  if (ev.data.fd != w->listenfd) {
    rc = -3;
    goto done;
  }

  /* listenfd is ready */
  assert(ev.data.fd == w->listenfd);
  sc = bw_ready_ul(w);
  if (sc < 0) goto done;

  rc = 0;

 done:
  return rc;
}

/*
 * send wake up byte one waiting socket
 * if it errors out for anything but ewouldblock/eagain
 * disconnect from that socket and zero its slot
 */
static void wake_one(bw_t *w, int n) {
  ssize_t nr;

  if (w->flags & BW_TRACE) {
    bw_log("waking %s\n", w->name[n]);
  }

  nr = sendmsg(w->fd[n], &w->hdr, MSG_DONTWAIT);
  if (nr >= 0) return;
  if ((errno == EWOULDBLOCK) || (errno == EAGAIN)) return;

  if (w->flags & BW_TRACE) {
    bw_log("purge %s: %s\n", w->name[n], strerror(errno));
  }
  close(w->fd[n]);
  w->fd[n] = -1;
  w->h->seqno++;
  w->h->wr[n].pid = 0;
}

/*
 * bw_wake
 *
 * invoked by the wake-mode caller to issue wake up
 *
 * call WITH handle under lock
 *
 * returns 0 on success
 *        -1 on error
 *
 */
int bw_wake(bw_t *w) {
  int n, rc = -1;

  assert(w->flags & BW_WAKE);
  if (bw_sync(w) < 0) goto done;

  for(n = 0; n < BW_WAITMAX; n++) {
    if (w->fd[n] == -1) continue;
    wake_one(w,n);
  }

  rc = 0;

 done:
  return rc;
}

/*
 * used within bw_force to drain pending wakeups
 */
#define DRAIN_MSGS 1024
static int drain_msgs(bw_t *w) {
  int rc = -1, nr, i;

  /* one time setup. allocate lazily - only if called.
   * set all the iov to the same buffer because we are
   * trying to discard the data so overwrite is fine */
  if (w->drain == NULL) {
    w->drain = calloc(DRAIN_MSGS, sizeof(struct mmsghdr));
    if (w->drain == NULL) {
      bw_log("out of memory");
      goto done;
    }
    for(i=0; i < DRAIN_MSGS; i++) {
      w->drain[i].msg_hdr.msg_iov = &w->iov;
      w->drain[i].msg_hdr.msg_iovlen = 1;
    }
  }

  do {
    nr = recvmmsg(w->listenfd, w->drain, DRAIN_MSGS, MSG_DONTWAIT, NULL);
    if ((nr < 0) && (errno != EWOULDBLOCK) && (errno != EAGAIN)) {
      bw_log("recvmmsg: %s\n", strerror(errno));
      goto done;
    }

    if ((nr > 0) && (w->flags & BW_TRACE)) {
      bw_log("discarded %d wakeups\n", nr);
    }
  } while (nr > 0);

  rc = 0;

 done:
  return rc;
}

/*
 * bw_force
 *
 * used by a wait-mode caller to forcibly clear
 * or set the readability of its own listen fd.
 *
 * the use case is when the shared resource for
 * which this library is managing waits/wakeups
 * undergoes multiple bw_wake events, and then 
 * an awakened process consumes the underlying
 * resource all at once, so it wants to clear 
 * the remaining wakeups.
 *
 * or, if it knows it consumed only some of the 
 * available underlying resource, it can ensure
 * it gets awakened immediately again.
 *
 */
#define DRAIN_MSGS 1024
int bw_force(bw_t *w, int want_ready) {
  int rc = -1, have_ready, sc;
  char c = '*';
  ssize_t nr;

  assert(w->flags & BW_WAIT);

  sc = ioctl(w->listenfd, FIONREAD, &have_ready);
  if (sc < 0) {
    bw_log("ioctl: %s\n", strerror(errno));
    goto done;
  }

  if ((want_ready == 0) && (have_ready > 0)) {
    sc = drain_msgs(w);
    if (sc < 0) goto done;
  }

  if ((want_ready != 0) && (have_ready == 0)) {
    nr = send(w->selffd, &c, 1, MSG_DONTWAIT);
    if (nr < 0) {
      bw_log("send: %s\n", strerror(errno));
      goto done;
    }
  }

  rc = 0;

 done:
  return rc;
}

/*
 * bw_ctl
 *
 * special purpose miscellaneous api
 * flag determines behavior
 *
 *  flag          arguments  meaning
 *  -----------   ---------  ----------------------------------------------
 *  BW_POLLFD     int fd     add fd to epoll set when blocked in bw_wait_ul
 *                           if it becomes readable, bw_wait_ul returns -2
 *
 * call WITHOUT handle under lock
 *
 * returns
 *  0 on success
 * -1 on error
 */
int bw_ctl(bw_t *w, int flag, ...) {
  struct epoll_event ev;
  int rc = -1, fd, sc;

  va_list ap;
  va_start(ap, flag);

  switch(flag) {

    case BW_POLLFD:
      assert(w->flags & BW_WAIT);
      assert(w->epollfd != -1);
      fd = (int)va_arg(ap, int);
      memset(&ev, 0, sizeof(ev));
      ev.events = EPOLLIN;
      ev.data.fd = fd;
      sc = epoll_ctl(w->epollfd, EPOLL_CTL_ADD, fd, &ev);
      if (sc < 0) {
        bw_log("epoll_ctl: %s\n", strerror(errno));
        goto done;
      }
      break;

    default:
      bw_log("bw_ctl: unknown flag %d\n", flag);
      goto done;
      break;
  }

  rc = 0;

 done:
  va_end(ap);
  return rc;
}

