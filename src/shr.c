/* #define _GNU_SOURCE */ /* now defined via AC_GNU_SOURCE in configure.ac */
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <unistd.h>
#include <limits.h>
#include <stdlib.h>
#include <stdarg.h>
#include <assert.h>
#include <dirent.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <libgen.h>
#include "shr.h"

#define CREAT_MODE 0644
#define MIN_RING_SZ (sizeof(shr_ctrl) + 1)
#define MAX_RWPROC 16
#define MAX_FIFO_NAME 32
#define MIN(a,b) (((a) < (b)) ? (a) : (b))


/* shr_ctrl is the control region of the shared/multiprocess ring.
 * this struct is mapped to the beginning of the mmap'd ring file.
 * the volatile offsets constantly change, under posix file lock,
 * as other processes copy data in or out of the ring. 
 * volatile because these updates happen in our shared memory. 
 */
#define R2W 0
#define W2R 1
static char magic[] = "libshr1";
typedef struct {
  char magic[sizeof(magic)];
  int version;
  int io_seq;       /* advances whenever fifos join or leave ring */
  char fifos[2][MAX_RWPROC][MAX_FIFO_NAME]; /* [0] = R2W, [1]=W2R */
  pid_t pids[2][MAX_RWPROC];                /* pids owning fifos */
  unsigned volatile gflags;
  size_t volatile n;        /* allocd size */
  size_t volatile i;        /* input pos (next write starts here)   */
  size_t volatile u;        /* unread (current num bytes unread)    */
  size_t volatile o;        /* output pos (next read starts here)   */
  size_t volatile m;        /* unread msg count (SHR_MESSAGES mode) */
  struct shr_stat stat;     /* i/o stats       */
  size_t app_data_len;      /* len of app region after d - opaque */
  char d[];                 /* ring data; C99 flexible array member */
} shr_ctrl;

/* the handle is given to each shr_open caller. it is opaque to the caller */
struct shr {
  int ring_fd;
  int tmp_fd;
  int bell_fd;
  int io_seq;               /* advances whenever fifos join or leave ring */
  int rwfd[MAX_RWPROC];     /* fd's to peer fifos */
  char fifo[MAX_FIFO_NAME]; /* our own fifo to block and receive wakeups on */
  struct stat s;
  unsigned flags;
  int index;                 /* index in s->r->pids,fifos (etc) we occupy */
  union {
    char *buf;   /* mmap'd area */
    shr_ctrl * r;
  };
};


/* get the lock on the ring file. we use a file lock for any read or write, 
 * since even the reader adjusts the position offsets in the ring buffer. note,
 * we use a blocking wait (F_SETLKW) for the lock. this should be obtainable in
 * quasi bounded time because a peer (reader or writer using this same library)
 * should also only lock/manipulate/release in rapid succession.
 *
 * if a signal comes in while we await the lock, fcntl can return EINTR. since
 * we are a library, we do not alter the application's signal handling.
 * rather, we propagate the condition up to the application to deal with. 
 *
 * also note, since this is a POSIX file lock, anything that closes the 
 * descriptor (such as killing the application holding the lock) releases it.
 *
 * fcntl based locks can be multiply acquired (without reference counting),
 * meaning it is a no-op to relock an already locked file. it is also ok
 * to unlock an already-unlocked file. See Kerrisk, TLPI p1128 "It is not an
 * error to unlock a region on which we don't currently hold a lock". This 
 * simplifies this library because we can jump to an error clause including
 * an unlock even if we failed to acquire the lock.
 *
 * lastly, on Linux you can see the active locks in /proc/locks
 *
 */
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

/*
 * shr_init creates a ring file
 *
 * succeeds only if the file is created new.  Attempts to resize an existing
 * file or init an existing file, even of the same size, fail.
 *
 * flags:
 *    SHR_OVERWRITE - permits ring to exist already, overwrites it
 *    SHR_KEEPEXIST - permits ring to exist already, leaves size/content
 *    SHR_MESSAGES  - ring i/o occurs in messages instead of byte stream
 *    SHR_DROP      - overwrite old ring data as ring fills, even if unread
 *    SHR_APPDATA   - store supplemental "app data" in ring (buf,len args)
 *
 * returns 0 on success
 *        -1 on error
 *        -2 on already-exists error (keepexist/overwrite not requested)
 * 
 *
 *
 */
int shr_init(char *file, size_t sz, unsigned flags, ...) {
  int rc = -1, fd = -1;
  char *buf = NULL;
  char *app_data = NULL;
  size_t app_sz = 0;

  va_list ap;
  va_start(ap, flags);

  size_t file_sz = sizeof(shr_ctrl) + sz;

  if ((flags >= SHR_OPEN_FENCE) || (sz == 0) || 
     ((flags & SHR_OVERWRITE) && (flags & SHR_KEEPEXIST))) {
    fprintf(stderr,"shr_init: invalid parameters\n");
    goto done;
  }

  /* app data is stored opaquely after the ring */
  if (flags & SHR_APPDATA) {
    app_data = va_arg(ap, char *);
    app_sz   = va_arg(ap, size_t);
    file_sz += app_sz;
  }

  /* if ring exists already, flags determine behavior */
  struct stat st;
  if (stat(file, &st) == 0) { /* exists */
    if (flags & SHR_OVERWRITE) {
      if (unlink(file) < 0) {
        fprintf(stderr, "unlink %s: %s\n", file, strerror(errno));
        goto done;
      }
    } else if (flags & SHR_KEEPEXIST) {
      rc = 0;
      goto done;
    } else {
      fprintf(stderr,"shr_init: %s already exists\n", file);
      rc = -2;
      goto done;
    }
  }

  fd = open(file, O_RDWR|O_CREAT|O_EXCL, CREAT_MODE);
  if (fd == -1) {
    fprintf(stderr,"open %s: %s\n", file, strerror(errno));
    goto done;
  }

  if (lock(fd) < 0) goto done; /* close() below releases lock */

  if (ftruncate(fd, file_sz) < 0) {
    fprintf(stderr,"ftruncate %s: %s\n", file, strerror(errno));
    goto done;
  }

  buf = mmap(0, file_sz, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
  if (buf == MAP_FAILED) {
    fprintf(stderr, "mmap %s: %s\n", file, strerror(errno));
    goto done;
  }

  shr_ctrl *r = (shr_ctrl *)buf; 
  memset(r, 0, sizeof(*r));
  memcpy(r->magic, magic, sizeof(magic));
  r->version = 1;
  r->u = 0;
  r->i = 0;
  r->o = 0;
  r->m = 0;
  r->n = sz;

  r->gflags = 0;
  if (flags & SHR_MESSAGES) r->gflags |= SHR_MESSAGES;
  if (flags & SHR_DROP) r->gflags |= SHR_DROP;
  if (flags & SHR_APPDATA) {
    if (app_sz) memcpy(r->d + r->n, app_data, app_sz);
    r->app_data_len = app_sz;
  }

  rc = 0;

 done:
  if ((rc == -1) && (fd != -1)) unlink(file);
  if (fd != -1) close(fd);
  if (buf && (buf != MAP_FAILED)) munmap(buf, file_sz);
  va_end(ap);
  return rc;
}


/* 
 * shr_stat
 *
 * retrieve statistics about the ring. if reset is non-NULL, the 
 * struct timeval it points to is written into the internal stats
 * structure as the start time of the new stats period, and the 
 * counters are reset as a side effect
 *
 * returns 0 on success (and fills in *stat), -1 on failure
 *
 */
int shr_stat(shr *s, struct shr_stat *stat, struct timeval *reset) {
  int rc = -1;

  if (lock(s->ring_fd) < 0) goto done;
  *stat = s->r->stat; /* struct copy */
  stat->bn = s->r->n; /* see shr.h; these three come from ring state */
  stat->bu = s->r->u;
  stat->mu = s->r->m;

  if (reset) {
    memset(&s->r->stat, 0, sizeof(s->r->stat));
    s->r->stat.start = *reset; /* struct copy */
  }

  rc = 0;

 done:
  unlock(s->ring_fd);
  return rc;
}

static int validate_ring(struct shr *s) {
  int rc = -1;
  shr_ctrl *r = s->r;
  ssize_t sz, exp_sz;

  if (s->s.st_size < (off_t)MIN_RING_SZ)       { rc = -2; goto done; }
  if (memcmp(s->r->magic,magic,sizeof(magic))) { rc = -3; goto done; }

  exp_sz = sizeof(shr_ctrl) + r->n + r->app_data_len; /* expected sz */
  sz = s->s.st_size;                                  /* actual sz */

  if (sz != exp_sz) {rc = -4; goto done; } /* size not expected */
  if (r->u >  r->n) {rc = -5; goto done; } /* used > size */
  if (r->i >= r->n) {rc = -6; goto done; } /* input position >= size */
  if (r->o >= r->n) {rc = -7; goto done; } /* output position >= size */

  rc = 0;

 done:
  return rc;
}

static int make_blocking(int fd) {
  int fl, unused = 0, rc = -1;

  fl = fcntl(fd, F_GETFL, unused);
  if (fl < 0) {
    fprintf(stderr, "fcntl: %s\n", strerror(errno));
    goto done;
  }

  if ((fl & O_NONBLOCK) == 0) {
    rc = 0;
    goto done;
  }

  if (fcntl(fd, F_SETFL, fl & (~O_NONBLOCK)) < 0) {
    fprintf(stderr, "fcntl: %s\n", strerror(errno));
    goto done;
  }

  rc = 0;

 done:
  return rc;
}

static int make_nonblock(int fd) {
  int fl, unused = 0, rc = -1;

  fl = fcntl(fd, F_GETFL, unused);
  if (fl < 0) {
    fprintf(stderr, "fcntl: %s\n", strerror(errno));
    goto done;
  }

  if (fl & O_NONBLOCK) {
    rc = 0;
    goto done;
  }

  if (fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) {
    fprintf(stderr, "fcntl: %s\n", strerror(errno));
    goto done;
  }

  rc = 0;

 done:
  return rc;
}

/* a program that has the ring open for reading, can optionally get a file
 * descriptor (via shr_get_selectable_fd) that is compatible with select/poll,
 * to know when data can be read from the ring. the descriptor is the bell fifo.
 * 
 * after each shr_read we ensure the bell readiness is accurately updated. to
 * do so, we either drain the bell fifo to prevent poll triggering if no data
 * is left in the ring, or we put a byte into the fifo if data remains in the
 * ring, as peers do when they put data in.
 */
static int refresh_bell(shr *s) {
  char b = 0, discard[1024];
  int nr;

  if (make_nonblock(s->bell_fd) < 0) return -1;

  if (s->r->u == 0) { /* no data in ring. drain fifo to prevent poll trigger */
    do {
      nr = read(s->bell_fd, &discard, sizeof(discard));
    } while (nr > 0);

  } else { /*  ring has unread data. put a byte in fifo so poll triggers */
    nr = write(s->bell_fd, &b, sizeof(b));
  }

  if ((nr == -1) && !((errno == EAGAIN) || (errno==EWOULDBLOCK))) {
    fprintf(stderr,"read/write: %s\n", strerror(errno));
    return -1;
  } 

  return 0;
}

/*
 * shr_get_selectable_fd
 *
 * returns a file descriptor that can be used with select/poll/epoll 
 *
 * succeeds only if the ring was opened with SHR_RDONLY|SHR_NONBLOCK
 * because spurious wakeups can occur (e.g. writer puts byte into 
 * ring while two readers await data; both wake up, but one gets it).
 * so the caller must be non-blocking to deal with spurious wakeups
 * 
 */
int shr_get_selectable_fd(shr *s) {
  int rc = -1;

  if ((s->flags & SHR_RDONLY) == 0) goto done;
  if ((s->flags & SHR_NONBLOCK) == 0) goto done;

  rc = 0;

 done:
  return (rc == 0) ? s->bell_fd : -1;
}

/* test pid existence; no signal is actually sent here */
static int stale_pid(pid_t pid) {
  int rc = kill(pid, 0);
  if ((rc < 0) && (errno == ESRCH)) return 1;
  return 0;
}

static int register_fifo(shr *s) {
  int rc = -1, i, slot;
  pid_t pid;

  assert((s->flags & SHR_RDONLY) || (s->flags & SHR_WRONLY));
  slot = (s->flags & SHR_RDONLY) ? W2R : R2W;

  /* claim a slot.  invalidate a claimed slot whose owning pid is absent */
  for(i=0; i < MAX_RWPROC; i++) {
    pid = s->r->pids[slot][i];
    if (pid && stale_pid(pid)) pid = 0;
    if (pid == 0) break;
  }

  if (i == MAX_RWPROC) {
    fprintf(stderr, "IO slots exhausted; increase MAX_RWPROC or tee rings\n");
    goto done;
  }

  /* claim slot */
  assert(i < MAX_RWPROC);
  memcpy(s->r->fifos[slot][i], s->fifo, MAX_FIFO_NAME);
  s->r->pids[slot][i] = getpid();
  s->index = i;
  s->r->io_seq++;

  rc = 0;

 done:
  return rc;
}

/* 
 * rescan_fifos
 *
 * any time the lock is reaquired, the control region in the ring should be
 * rescanned to see if any peers registered their fifos since we last checked.
 * a simple integer is used for this; we cache the last one we saw and compare.
 *
 */
static int rescan_fifos(shr *s) {
  int rc = -1, i, slot;

  if (s->io_seq == s->r->io_seq) { /* already current */
    rc = 0;
    goto done;
  }

  /* fifo registrations have been updated. to make things simple we just
   * close the existing ones, and open them all anew. we could just check
   * for ones we don't yet have an fd open to, that I will leave for later.
   */

  /* close the fifos */
  for(i=0; i < MAX_RWPROC; i++) {
    if (s->rwfd[i] == -1) continue;
    if (close( s->rwfd[i] ) < 0) {
      fprintf(stderr, "close: %s\n", strerror(errno));
    }
    s->rwfd[i] = -1;
  }

  /* open fifos up, first checking their owning PID exists */
  slot = (s->flags & SHR_RDONLY) ? R2W : W2R;
  for(i=0; i < MAX_RWPROC; i++) {

    if (*s->r->fifos[slot][i] == '\0') continue; /* empty slot */

    /* if fifo owner PID is dead; unregister it and try to unlink its fifo.
     * in this case, it's ok if unlink fails - either fifo was already unlinked,
     * or we lack privileges or it's inaccessible- these we can ignore. */
    if (stale_pid(s->r->pids[slot][i])) {
      unlinkat(s->tmp_fd, s->r->fifos[slot][i], 0); /* failure is ok */
      *(s->r->fifos[slot][i]) = '\0';               /* unregister */
      s->r->io_seq++;
      continue;
    }

    /* attempt to open the fifo. note that, failure here is fatally logged.
     * there are situations (like exceeding max number of file descriptors)
     * that are local to this process, so we do NOT unregister fifos here */
    s->rwfd[i] = openat(s->tmp_fd, s->r->fifos[slot][i], O_RDWR);
    if (s->rwfd[i] < 0) {
      fprintf(stderr, "open %s: %s\n", s->r->fifos[slot][i], strerror(errno));
      goto done;
    }

    /* fifo is now open. this fifo is for us to awaken a blocked peer. ensure
     * we don't block in doing so, even if peer is slow to drain fifo or dies */
    if (make_nonblock(s->rwfd[i]) < 0) goto done;
  }

  s->io_seq = s->r->io_seq;
  rc = 0;

 done:
  return rc;
}

/*
 * shr_open opens a ring 
 *
 * the ring is opened for reading only OR writing only, and must exist already
 *
 * flags:
 *    SHR_RDONLY      - open for reading 
 *    SHR_WRONLY      - open for writing
 *    SHR_GET_APPDATA - obtain "app data" pointer (char **buf, size_t *len args)
 *                      note, the app data has no particular memory alignment,
 *                      so, if a caller wants to overlay a struct on it, it
 *                      should copy the app data into a suitably aligned buffer.
 *                      also, the app data memory buffer is considered read-only
 *                      since it points into shared memory that was initialized
 *                      at ring creation. the app data pointer is valid only as
 *                      long as the caller has the ring open- until shr_close.
 *
 * returns opaque struct shr pointer on success, or NULL on error
 *
 */
struct shr *shr_open(char *file, unsigned flags, ...) {
  struct shr *s = NULL;
  int rc = -1, vc, i;
  char **app_data;
  size_t *app_sz;

  va_list ap;
  va_start(ap, flags);

  unsigned disallowed_flags = SHR_OPEN_FENCE-1;

  if (((flags & SHR_RDONLY) && (flags & SHR_WRONLY)) || /* both r AND w    */
      ((flags & (SHR_RDONLY | SHR_WRONLY)) == 0)     || /* neither r NOR w */
      (flags & disallowed_flags)) {                     /* other bad flags */
    fprintf(stderr,"shr_open: invalid mode\n");
    goto done;
  }

  s = malloc( sizeof(struct shr) );
  if (s == NULL) {
    fprintf(stderr, "out of memory\n");
    goto done;
  }
  memset(s, 0, sizeof(*s));
  s->ring_fd = -1;
  s->tmp_fd = -1;
  s->bell_fd = -1;
  s->flags = flags;
  s->index = -1;
  for(i=0; i < MAX_RWPROC; i++) s->rwfd[i] = -1;

  s->ring_fd = open(file, O_RDWR);
  if (s->ring_fd == -1) {
    fprintf(stderr,"open %s: %s\n", file, strerror(errno));
    goto done;
  }

  if (fstat(s->ring_fd, &s->s) == -1) {
    fprintf(stderr,"stat %s: %s\n", file, strerror(errno));
    goto done;
  }

  s->buf = mmap(0, s->s.st_size, PROT_READ|PROT_WRITE, MAP_SHARED, s->ring_fd, 0);
  if (s->buf == MAP_FAILED) {
    fprintf(stderr, "mmap %s: %s\n", file, strerror(errno));
    goto done;
  }

  char *shm = "/dev/shm", *tmp = "/tmp";
  char *dir = (access(shm, F_OK) == 0) ? shm : tmp;

  s->tmp_fd = open(dir, O_DIRECTORY|O_PATH);
  if (s->tmp_fd < 0) {
    fprintf(stderr, "open %s: %s\n", dir, strerror(errno));
    goto done;
  }

  /* name our fifo with pid/tid/sequence number (up to maxtries)
   * to generate a unique fifo name. the purpose is that one caller
   * thread should be able to open multiple shr rings; we need
   * a uniquely named notification fifo for each one. */
  pid_t pid = getpid();
  pid_t tid = syscall(SYS_gettid);
  int seq = 0, max_tries=100;

 again:
  snprintf(s->fifo, sizeof(s->fifo), "fifo.%u.%u.%u", pid, tid, seq);
  if (mkfifoat(s->tmp_fd, s->fifo, CREAT_MODE) < 0) {
    if ((errno == EEXIST) && (seq++ < max_tries)) goto again;
    fprintf(stderr, "mkfifo %s: %s\n", s->fifo, strerror(errno));
    goto done;
  }

  s->bell_fd = openat(s->tmp_fd, s->fifo, O_RDWR); 
  if (s->bell_fd < 0) {
    fprintf(stderr, "open %s: %s\n", s->fifo, strerror(errno));
    goto done;
  }

  if (lock(s->ring_fd) < 0) goto done;

  vc = validate_ring(s);
  if (vc < 0) {
    fprintf(stderr, "validate_ring failed: %s (%d)\n", file, vc);
    goto done;
  }

  if (register_fifo(s) < 0) goto done;
  if (rescan_fifos(s) < 0) goto done;
  if (refresh_bell(s) < 0) goto done;

  if (flags & SHR_GET_APPDATA) {
    app_data = va_arg(ap, char **);
    app_sz   = va_arg(ap, size_t*);
    *app_data = s->r->app_data_len ? (s->r->d + s->r->n) : NULL;
    *app_sz = s->r->app_data_len;
  }

  rc = 0;

 done:
  if (s && (s->ring_fd != -1)) unlock(s->ring_fd);
  if ((rc < 0) && s) {
    if ((s->tmp_fd != -1) && (*s->fifo != '\0')) unlinkat(s->tmp_fd, s->fifo, 0);
    if (s->tmp_fd != -1) close(s->tmp_fd);
    if (s->ring_fd != -1) close(s->ring_fd);
    if (s->bell_fd != -1) close(s->bell_fd);
    if (s->buf && (s->buf != MAP_FAILED)) munmap(s->buf, s->s.st_size);
    free(s);
    s = NULL;
  }
  va_end(ap);
  return s;
}

/* 
 * to ring the bell we write a byte (non-blocking) to a fifo. this notifies
 * the peer. if the fifo is full we consider this to have succeeded, because
 * the point of the fifo is to be readable when data is available; a full fifo
 * satisfies that need.  it is harmless (except for inefficiency) to ring it
 * superfluously; the peer wakes up, checks the condition and reblocks if so.
*/
static int ring_bell(struct shr *s) {
  char b = '1'; /* arbitrary; just a wakeup */
  int rc = -1, i;
  ssize_t n;

  for(i=0; i < MAX_RWPROC; i++) {
    if (s->rwfd[i] == -1) continue;
    n = write(s->rwfd[i], &b, sizeof(b));
    if (n < 0) {
      if ((errno == EWOULDBLOCK) | (errno == EAGAIN)) continue;
      fprintf(stderr, "write: %s\n", strerror(errno));
    }
  }

  rc = 0;

  return rc;
}

/* read the fifo, causing us to block, awaiting notification from the peer.
 * for a reader process, the notification (fifo readability) means that we
 * should check the ring for new data. for a writer process, the notification
 * means that space has become available in the ring. 
 *
 * returns 0 on normal wakeup, 
 *        -1 on error, 
 *        -2 on signal while blocked
 */
static int block(struct shr *s) {
  int rc = -1;
  ssize_t nr;
  char b;

  if (make_blocking(s->bell_fd) < 0) goto done;

  nr = read(s->bell_fd, &b, sizeof(b)); /* block */
  if (nr < 0) {
    if (errno == EINTR) rc = -2;
    else fprintf(stderr, "read: %s\n", strerror(errno));
    goto done;
  }

  rc = 0;

 done:
  return rc;
}

/* helper function; given ring offset o (pointing to a message length prefix)
 * read it, taking into account the possibility that the prefix wraps around */
static size_t get_msg_len(struct shr *s, size_t o) {
  size_t msg_len, hdr = sizeof(size_t);
  assert(o < s->r->n);
  assert(s->r->u >= hdr);
  size_t b = s->r->n - o; /* bytes at o til wrap */
  memcpy(&msg_len, &s->r->d[o], MIN(b, hdr));
  if (b < hdr) memcpy( ((char*)&msg_len) + b, s->r->d, hdr-b);
  return msg_len;
}

/* 
 * this function is called under lock to forcibly reclaim space from the ring,
 * (SHR_DROP mode). The oldest portion of ring data is sacrificed.
 *
 * if this is a ring of messages (SHR_MESSAGES), preserve boundaries by 
 * moving the read position to the nearest message at or after delta bytes.
 */
static void reclaim(struct shr *s, size_t delta) {
  size_t o, reclaimed=0, msg_len;

  if (s->r->gflags & SHR_MESSAGES) {
    for(o = s->r->o; reclaimed < delta; reclaimed += msg_len) {
      msg_len = get_msg_len(s,o) + sizeof(size_t);
      o = (o + msg_len ) % s->r->n;
      s->r->stat.md++; /* msg drops */
      s->r->m--;       /* msgs in ring */
    }
    delta = reclaimed;
  }

  assert(delta <= s->r->u);
  s->r->o = (s->r->o + delta) % s->r->n;
  s->r->u -= delta;
  s->r->stat.bd += delta; /* bytes dropped */
}

/*
 * write data into ring
 *
 * if there is sufficient space in the ring - copy the whole buffer in.
 * if there is insufficient free space in the ring- wait for space, or
 * return 0 immediately in non-blocking mode. only writes all or nothing.
 *
 * returns:
 *   > 0 (number of bytes copied into ring, always the full buffer)
 *   0   (insufficient space in ring, in non-blocking mode)
 *  -1   (error, such as the buffer exceeds the total ring capacity)
 *  -2   (signal arrived while blocked waiting for ring)
 *
 */
ssize_t shr_write(struct shr *s, char *buf, size_t len) {
  struct iovec io = {.iov_base = buf, .iov_len = len };
  return shr_writev(s, &io, 1);
}

/*
 * shr_read
 *
 * Read from the ring. 
 * In SHR_MESSAGES mode, each read returns exactly one message.
 * Block if ring empty, or return immediately in SHR_NONBLOCK mode.
 *
 * returns:
 *   > 0 (number of bytes read from the ring)
 *   0   (ring empty, in non-blocking mode)
 *  -1   (error)
 *  -2   (signal arrived while blocked waiting for ring)
 *  -3   (buffer can't hold message; SHR_MESSAGE mode)
 *   
 */
ssize_t shr_read(struct shr *s, char *buf, size_t len) {
  struct iovec io = {.iov_base = buf, .iov_len = len };
  int one = 1;
  return shr_readv(s, buf, len, &io, &one);
}

/*
 * read multiple messages from ring
 *
 * also see shr_read
 *
 * the function copies ring data into buf, and populates the struct iovec
 * array so each one points to a message in buf (when in message mode); in
 * byte mode, only the first iovec is populated. the caller provides the
 * uninitialized array of iov and this function fills them in. iovcnt is an
 * IN/OUT parameter; on input it's the number of structures in iov, and on
 * output it's how many this function filled in.
 *
 * returns:
 *   > 0 (number of bytes read from the ring)
 *   0   (no data in ring, in non-blocking mode)
 *  -1   (error)
 *  -2   (signal arrived while blocked waiting for ring)
 *  -3   (buffer can't hold message; SHR_MESSAGE mode)
 *
 */
ssize_t shr_readv(shr *s, char *buf, size_t len, struct iovec *iov, int *iovcnt) {
  int rc = -1;
  shr_ctrl *r = s->r;
  size_t nr=0, msg_len, b, mc=0, iovleft;

  iovleft = *iovcnt;
  *iovcnt = 0;

  /* since this function returns signed, cap len */
  if (len > SSIZE_MAX) len = SSIZE_MAX;
  if (len == 0) goto done;
  if (iovleft == 0) goto done;

 again:
  rc = -1;
  if (lock(s->ring_fd) < 0) goto done;
  if (rescan_fifos(s) < 0) goto done;

  if (s->r->u == 0) { /* nothing to read */
    if (s->flags & SHR_NONBLOCK) { rc = 0; goto done; } 
    else goto block;
  }

  /* data IS available in ring */
  assert(s->r->u > 0);

  if ((s->r->gflags & SHR_MESSAGES) == 0) { /* byte mode */
    *iovcnt = 1;
    nr = MIN(len, s->r->u);
    iov[0].iov_len = nr;
    iov[0].iov_base = buf;
    if (s->r->o < s->r->i) { 
      /* readable region is one extent */
      memcpy(buf, &s->r->d[s->r->o], nr);
    } else {
      /* readable region is two parts */
      b = s->r->n - s->r->o;
      memcpy(buf, &s->r->d[s->r->o], MIN(b, nr));
      if (b < nr) memcpy(&buf[b], s->r->d, nr-b);
    }
    s->r->u -= nr;
    s->r->o = (s->r->o + nr) % s->r->n;

    rc = 0;
    goto done;
  }

  /* message mode here */
  assert(s->r->gflags & SHR_MESSAGES);

  /* limiting factor in the amount of data we can read is one of:
   *  (a) buf fills up
   *  (b) iov exhausted
   *  (c) ring exhausted
   */
  while(1) {
    if (s->r->u == 0) break;
    if (iovleft == 0) break;
    msg_len = get_msg_len(s, r->o);
    if (msg_len > len) {
      if (mc == 0) { rc = -3; goto done; }
      break;
    }
    assert(s->r->u >= msg_len + sizeof(msg_len));
    s->r->u -= sizeof(msg_len);
    s->r->o = (s->r->o + sizeof(msg_len)) % s->r->n;
    nr += msg_len;
    iov[mc].iov_base = buf;
    iov[mc].iov_len = msg_len;
    b = (s->r->i <= s->r->o) ? (s->r->n - s->r->o) : (s->r->i - s->r->o);
    memcpy(buf, &s->r->d[s->r->o], MIN(b, msg_len));
    if (msg_len > b) memcpy(&buf[b], s->r->d, msg_len-b);
    buf += msg_len;
    len -= msg_len;
    s->r->u -= msg_len;
    s->r->o = (s->r->o + msg_len) % s->r->n;
    s->r->m--;
    mc++;
    iovleft--;
  }

  *iovcnt = mc;
  rc = 0;

 done:

  refresh_bell(s);

  if (rc == 0) {
    if (nr > 0) ring_bell(s); /* ring peer bells if space freed */
    s->r->stat.br += (nr + (mc * sizeof(msg_len)));
    s->r->stat.mr += mc;
  }

  unlock(s->ring_fd);
  return (rc == 0) ? (ssize_t)nr : rc;
 
 block:
  unlock(s->ring_fd);
  block(s);
  goto again;
}

/*
 * write sequential io buffers into ring
 *
 * if there is sufficient space in the ring - copy the whole iovec in.
 * if there is insufficient free space in the ring- wait for space, or
 * return 0 immediately in non-blocking mode. only writes all or nothing.
 * in message mode (SHR_MESSAGES) each iovec element becomes one message.
 *
 * returns:
 *   > 0 (number of bytes copied into ring, always the full iovec)
 *   0   (insufficient space in ring, in non-blocking mode)
 *  -1   (error, such as the iovec exceeds the total ring capacity)
 *  -2   (signal arrived while blocked waiting for ring)
 *
 */
ssize_t shr_writev(shr *s, struct iovec *iov, int iovcnt) {
  int rc = -1, i;
  shr_ctrl *r = s->r;
  char *buf;
  size_t bsz, a, b, len=0;
  size_t hdr = (s->r->gflags & SHR_MESSAGES) ? (sizeof(bsz) * iovcnt): 0;

  for(i = 0; i < iovcnt; i++) {
    if (iov[i].iov_len == 0) goto done; // disallow zero length messages
    if ((len + iov[i].iov_len) < len) goto done; // overflow
    len += iov[i].iov_len;
  }

  if (len > SSIZE_MAX) goto done;    // this function returns signed, so cap len
  if (len + hdr > s->r->n) goto done;// does buffer exceed total ring capacity
  if (len == 0) goto done;           // zero length writes/messages are an error

 again:
  rc = -1;
  if (lock(s->ring_fd) < 0) goto done;
  if (rescan_fifos(s) < 0) goto done;
  
  a = r->n - r->u;      // total free space in ring
  if (len + hdr > a) {  // if more space is needed...
    if (s->r->gflags & SHR_DROP) reclaim(s, len+hdr - a);
    else if (s->flags & SHR_NONBLOCK) { rc = 0; len = 0; goto done; }
    else goto block;
  }
  assert(r->n - r->u >= len + hdr); // enough space now exists in ring

  for(i=0; i < iovcnt; i++) {

    buf = iov[i].iov_base;
    bsz = iov[i].iov_len;

    a = r->n - r->u;
    if (r->i < r->o) {   // free space is one buffer; has unread data after it
      b = a;             // free "part" starting at r->i is the full free space
    } else if (r->i == r->o) { // the ring is full (u ==n) or empty (u == 0)
      b = a ? (r->n - r->i) : 0; // free part starting at r->i is tail, or 0
    } else {           // ring free space consists of two wrapped parts
      b = r->n - r->i; // free part at r->i abuts ring-end, gets leading input
    }

    if (hdr) {
      char *l = (char*)&bsz;
      memcpy(&r->d[r->i], l, MIN(b, sizeof(bsz)));
      if (sizeof(bsz) > b) memcpy(r->d, l + b, sizeof(bsz)-b);
      r->i = (r->i + sizeof(bsz)) % r->n;
      r->u += sizeof(bsz);

      /* patch up b */
      a = r->n - r->u;
      if (r->i < r->o) {   // free space is one buffer; has unread data after it
        b = a;             // free "part" starting at r->i is the full free space
      } else if (r->i == r->o) { // the ring is full (u ==n) or empty (u == 0)
        assert (0);        // branch should not occur between msg hdr and body
        b = a ? (r->n - r->i) : 0; // free part starting at r->i is tail, or 0
      } else {           // ring free space consists of two wrapped parts
        b = r->n - r->i; // free part at r->i abuts ring-end, gets leading input
      }
    }

    memcpy(&r->d[r->i], buf, MIN(b, bsz));
    if (bsz > b) memcpy(r->d, &buf[b], bsz-b);
    r->i = (r->i + bsz) % r->n;
    r->u += bsz;
  }

  ring_bell(s);
  rc = 0;

 done:
  s->r->stat.bw += ((rc == 0) && len) ? (len + hdr) : 0;
  s->r->stat.mw += ((rc == 0) && len && hdr) ? iovcnt : 0;
  s->r->m += ((rc == 0) && len && hdr) ? iovcnt : 0;
  unlock(s->ring_fd);
  return (rc == 0) ? (ssize_t)len : -1;

 block:
  unlock(s->ring_fd);
  block(s);
  goto again;
}

void shr_close(struct shr *s) {
  int slot, i, mc;

  /* unregister our fifo under lock. 
   * if this were to fail for some reason it is ok. the next process to open
   * the ring would find the fifo unlinked and unregister it for us. */

  slot = (s->flags & SHR_RDONLY) ? W2R : R2W;
  if (lock(s->ring_fd) < 0) goto finish;

  assert(s->index != -1);
  mc = memcmp(s->r->fifos[slot][s->index], s->fifo, MAX_FIFO_NAME);
  assert(mc == 0);
  *s->r->fifos[slot][s->index] = '\0';
  s->r->pids[slot][s->index] = 0;
  s->r->io_seq++;

  unlock(s->ring_fd);

 finish:

  /* close peer notification fd's */
  for(i=0; i < MAX_RWPROC; i++) {
    if (s->rwfd[i] == -1) continue;
    close( s->rwfd[i] );
  }

  /* unlink our own notification fifo */
  assert(s->tmp_fd != -1);
  assert(*s->fifo != '\0');
  unlinkat(s->tmp_fd, s->fifo, 0);

  /* dirfd to /dev/shm or /tmp */
  assert(s->tmp_fd != -1);
  close(s->tmp_fd);

  /* fd open on our own notify fifo */
  assert(s->bell_fd != -1);
  close(s->bell_fd);

  /* ring file */
  assert(s->ring_fd != -1);
  close(s->ring_fd);
  assert(s->buf && (s->buf != MAP_FAILED));
  munmap(s->buf, s->s.st_size);

  free(s);
}
