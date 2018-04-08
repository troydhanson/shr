/* _GNU_SOURCE defined via AC_GNU_SOURCE in configure.ac */
#ifndef _GNU_SOURCE 
#define _GNU_SOURCE 
#endif
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <unistd.h>
#include <limits.h>
#include <stdlib.h>
#include <stdarg.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include "shr.h"
#include "bw.h"

#define CREAT_MODE 0644
#define MIN_RING_SZ (sizeof(shr_ctrl) + 1)
#define MIN(a,b) (((a) < (b)) ? (a) : (b))

struct msg {
  size_t pos;
  size_t len;
};

/* shr_ctrl is the control region of the shared/multiprocess ring.
 * this struct is mapped to the beginning of the mmap'd ring file.
 * the volatile offsets constantly change, under posix file lock,
 * as other processes copy data in or out of the ring. 
 */
static char magic[] = "libshr4";
typedef struct {
  char magic[sizeof(magic)];
  unsigned volatile gflags;
  size_t volatile n;        /* allocd size */
  size_t volatile i;        /* offset from r->d for next write      */
  size_t volatile u;        /* current number of unread bytes       */
  size_t volatile m;        /* current number of unread messages    */
  size_t volatile mp;       /* msgs present in ring, unread + read  */
  size_t volatile r;        /* slot number in mv for next read      */
  size_t volatile e;        /* slot number in mv of eldest message  */
  size_t volatile q;        /* sequence number of eldest message    */
  struct shr_stat stat;     /* i/o stats                            */
  size_t mm;                /* max number of messages (mv slots)    */
  size_t mv_len;            /* message vector len, located after d  */
  size_t pad_len;           /* padding after data to align mv       */
  size_t app_len;           /* len of app region after mv - opaque  */
  bw_handle w2r;            /* implements reader blocking           */
  bw_handle r2w;            /* implements writer blocking           */
  char d[];                 /* ring data; C99 flexible array member */
} shr_ctrl;

struct cache {
  char *buf;                /* cache space */
  size_t sz;                /* size of buf */
  size_t n;                 /* bytes used */
  struct iovec *iov;        /* cache iov */
  size_t vt;                /* iov total */
  size_t vm;                /* iov used */
};

/* handle returned from shr_open,
 * opaque to the caller  */
struct shr {
  int ring_fd;    /* descriptor of mapped ring file   */
  int wait_fd;    /* descriptor to block on if needed */
  struct stat s;  /* stat of mapped ring file         */
  unsigned flags; /* flags reflecting shr_open mode   */
  size_t q;       /* next unread msg seqno (farm mode)*/
  size_t md;      /* msgs dropped by this farm-reader */
  bw_t *w2r;      /* block/wake line writer-to-reader */
  bw_t *r2w;      /* block/wake line reader-to-writer */
  struct cache c; /* when ring is opened SHR_BUFFERED */
  size_t n;       /* copy of r->n to utilize w/o lock */
  size_t mm;      /* copy of r->mm to utilize w/o lock */
  union {
    char *buf;    /* ring file mmap'd location        */
    shr_ctrl * r; /* ring file control region         */
  };
};


/* get the lock on the ring file. we use a file lock for any read or write, 
 * since even the reader adjusts the position offsets in the ring buffer. note,
 * we use a blocking wait (F_SETLKW) for the lock. this should be obtainable in
 * quasi bounded time because a peer (reader or writer using this same library)
 * should also only lock/manipulate/release in rapid succession.
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
 * returns
 *  0 on success
 * -1 on error
 */
static int lock(int fd) {
  int rc = -1, sc;

  const struct flock f = {
    .l_type = F_WRLCK,
    .l_whence = SEEK_SET
  };

  sc = fcntl(fd, F_SETLKW, &f);
  if (sc < 0) {
    shr_log("fcntl lock: %s\n", strerror(errno));
    goto done;
  }

  rc = 0;
  
 done:
  return rc;
}

static int unlock(int fd) {
  int rc = -1, sc;

  const struct flock f = {
    .l_type = F_UNLCK,
    .l_whence = SEEK_SET
  };

  sc = fcntl(fd, F_SETLK, &f);
  if (sc < 0) {
    shr_log("fcntl unlock: %s\n", strerror(errno));
    goto done;
  }

  rc = 0;

 done:
  return rc;
}

/*
 * shr_sync
 *
 * sync the mapped ring buffer to backing store.
 *
 * called with ring under lock
 *
 * this should never be necessary, strictly speaking.
 * furthermore, it should be a no-op on a tmpfs,
 * which is the normal backing for a shr ring.
 *
 * The Linux Programming Interface by Michael Kerrisk
 * (TLPI) states on p. 1032:
 *
 *   Linux provides a so-called unified virtual
 *   memory system. This means that, where possible,
 *   memory mappings and blocks of the buffer cache
 *   share the same pages of physical memory. Thus,
 *   the views of a file obtained via a mapping and
 *   via I/O system calls (read(), write(), and so
 *   on) are always consistent, and the only use of
 *   msync() is to force the contents of a mapped
 *   region to be flushed to disk.
 *
 */
static int shr_sync(shr *s) {
  shr_ctrl *r = s->r;
  int rc;

  rc = (r->gflags & SHR_SYNC) ?
       msync(s->buf, s->s.st_size, MS_SYNC) : 0;

  if (rc < 0)
    shr_log("msync: %s\n", strerror(errno));

  return rc;
}

/*
 * shr_init creates a ring file
 *
 * flags:
 *    SHR_FARM         - farm of parallel readers; SHR_DROP implied
 *    SHR_DROP         - ring overwrites unread messages when full
 *    SHR_APPDATA_1    - store caller buffer in ring (buf,len args)
 *    SHR_MAXMSGS_2    - ring holds given number of msgs (size_t arg)
 *    SHR_KEEPEXIST    - if ring exists already, leave as-is
 *
 * returns 
 *   0 on success
 *  -1 on error
 *
 */
int shr_init(char *file, size_t data_sz, unsigned flags, ...) {
  size_t appsize=0, sz=0, mv_bytes, max_msgs=0, pad, m;
  int rc = -1, fd = -1, exists, sc;
  char *appdata=NULL, *buf=NULL;

  va_list ap;
  va_start(ap, flags);

  if ((flags >= SHR_OPEN_FENCE) || (data_sz == 0)) {
    shr_log("shr_init: invalid flags\n");
    goto done;
  }

  if (flags & SHR_APPDATA_1) {
    appdata = va_arg(ap, char*);
    appsize = va_arg(ap, size_t);
  }

  /* either the ring's data_sz or the max messages (mm)
   * become the limiting factor in how much data the ring
   * can store. only the data_sz is a required parameter.
   * guestimate a max number of messages unless told. */
  if (flags & SHR_MAXMSGS_2)
    max_msgs = va_arg(ap, size_t);
  if (max_msgs == 0)
    max_msgs = (100 + data_sz / 100);
  mv_bytes = max_msgs * sizeof(struct msg);

  exists = (access(file, F_OK) == 0) ? 1 : 0;
  if (exists && (flags & SHR_KEEPEXIST)) {
    rc = 0;
    goto done;
  } 
  
  sc = exists ? unlink(file) : 0;
  if (sc < 0) {
    shr_log("unlink %s: %s\n", file, strerror(errno));
    goto done;
  }

  fd = open(file, O_RDWR|O_CREAT|O_EXCL, CREAT_MODE);
  if (fd < 0) {
    shr_log("open %s: %s\n", file, strerror(errno));
    goto done;
  }

  /* calculate padding after data to align mv */
  m = data_sz % sizeof(void*);
  pad = m ? (sizeof(void*) - m) : 0;
  assert(pad < sizeof(void*));
  sz = sizeof(shr_ctrl) + data_sz + pad;
  assert((sz % sizeof(void*)) == 0);

  if (lock(fd) < 0) /* close() below releases lock */
    goto done;

  /* set the ring file size. ftruncate is unimplemented
   * on hugetlbfs, so we permit EINVAL; on that fs, the
   * mmap itself suffices to set ring's length in pages */
  sz = sizeof(shr_ctrl) + data_sz + pad + mv_bytes + appsize;
  sc = ftruncate(fd, sz);
  if ((sc < 0) && (errno != EINVAL)) {
    shr_log("ftruncate %s: %s\n", file, strerror(errno));
    goto done;
  }

  buf = mmap(0, sz, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
  if (buf == MAP_FAILED) {
    shr_log("mmap %s: %s\n", file, strerror(errno));
    buf = NULL;
    goto done;
  }

  shr_ctrl *r = (shr_ctrl *)buf; 
  memset(r, 0, sizeof(*r));
  memcpy(r->magic, magic, sizeof(magic));
  r->mm = max_msgs;
  r->pad_len = pad;
  r->mv_len = mv_bytes;
  r->app_len = appsize;
  r->n = data_sz;
  r->gflags = 0;
  if (flags & SHR_SYNC)      r->gflags |=  SHR_SYNC;
  if (flags & SHR_DROP)      r->gflags |=  SHR_DROP;
  if (flags & SHR_FARM)      r->gflags |= (SHR_FARM | SHR_DROP);
  if (flags & SHR_MLOCK)     r->gflags |=  SHR_MLOCK;
  if (flags & SHR_APPDATA) {
    memcpy(r->d + r->n + r->pad_len + r->mv_len, appdata, appsize);
  }

  rc = 0;

 done:
  if (rc && (fd != -1)) unlink(file);
  if (fd != -1) close(fd);
  if (buf) munmap(buf, sz);
  va_end(ap);
  return rc;
}


/* 
 * shr_stat
 *
 * retrieve statistics about the ring. if reset is non-NULL, the 
 * struct timeval it points to is written into the internal stats
 * structure, counters are zeroed, beginning a new stats period.
 *
 * returns 
 *  0 on success (and fills in *stat)
 * -1 on failure
 *
 */
int shr_stat(shr *s, struct shr_stat *stat, struct timeval *reset) {
  int rc = -1;

  if (lock(s->ring_fd) < 0) goto done;

  /* copy stats */
  *stat = s->r->stat;

  /* ring state */
  stat->bn = s->r->n;
  stat->bu = s->r->u;
  stat->mu = s->r->m;
  stat->mm = s->r->mm;

  /* cache state */
  stat->cn = s->c.sz;
  stat->cm = s->c.vm;
  stat->cb = s->c.n;

  /* ring attributes */
  stat->flags = s->r->gflags;

  if (reset) {
    memset(&s->r->stat, 0, sizeof(s->r->stat));
    s->r->stat.start = *reset; /* struct copy */
  }

  if (shr_sync(s) < 0) goto done;

  rc = 0;

 done:
  unlock(s->ring_fd);
  return rc;
}

/*
 * shr_farm_stat
 *
 * for a SHR_FARM mode ring, and an SHR_RDONLY reader,
 * return how many messages this reader has missed
 * (dropped by the ring before this reader read them).
 * If reset is non-zero, it zeroes the drops counter.
 *
 * the drop counter is updated when shr_read/shr_readv
 * is called.
 */
size_t shr_farm_stat(shr *s, int reset) {
  size_t d;

  assert(s->flags & SHR_RDONLY);

  d = s->md;
  if (reset) s->md = 0;

  return d;
}

/*
 * validate_ring
 *
 * check basic conditions for ring: magic, internal state.
 *
 * NOTE: the size of the ring is allowed to exceed the
 * precisely expected byte size that the rings requires.
 * this supports hugetlbfs-backed rings, whose size gets
 * rounded up to a huge tlb page size.
 *
 * called with ring under lock
 *
 */
static int validate_ring(struct shr *s) {
  int rc = -1;
  shr_ctrl *r = s->r;
  ssize_t sz, exp_sz;

  if (s->s.st_size < (off_t)MIN_RING_SZ)       { rc = -2; goto done; }
  if (memcmp(s->r->magic,magic,sizeof(magic))) { rc = -3; goto done; }

  exp_sz = sizeof(shr_ctrl) + r->n + r->pad_len + r->mv_len + r->app_len;
  sz = s->s.st_size;

  if (sz <  exp_sz) {rc = -4; goto done; } /* undersize (see note) */
  if (r->u >  r->n) {rc = -5; goto done; } /* used > size */
  if (r->i >= r->n) {rc = -6; goto done; } /* input position >= size */
  if (r->r >= r->mm){rc = -7; goto done; } /* output slot# >= #slots */

  rc = 0;

 done:
  return rc;
}

/*
 * shr_get_selectable_fd
 *
 * returns a file descriptor that can be used with select/poll/epoll 
 *
 * succeeds only if the ring was opened with SHR_RDONLY|SHR_NONBLOCK
 * the caller must be non-blocking because spurious wakeups can occur 
 * e.g. two writes + two wakeups -> one coalesced read + extra wakeup
 * thus, an shr_read arising from a spurious wakeup needs to not block
 * 
 * readers only: writers can't poll externally for space availability
 */
int shr_get_selectable_fd(shr *s) {
  if ((s->flags & SHR_RDONLY) &&
      (s->flags & SHR_NONBLOCK)) return s->wait_fd;
  return -1;
}

/* helper to open the bw library handles during shr_open 
*
 * called with the ring under lock 
 *
 * regarding need_r2w: 
 *   it's always ok for writer to open the r2w handle.
 *   but, we prefer not to open it, unless we need it.
 *   if open, a reader is obligated to send wakeups
 *   when they free space in the ring, to wake writers 
 *   blocked in shr_write. but, in cases where the ring
 *   is in SHR_DROP mode, or the writer is non blocking,
 *   writes succeed or fail without needing to block. 
 *   for these writers we leave the r2w handle closed. 
 *   this is just for performance. if a non blocking 
 *   writer later calls shr_flush(s,1) to do a blocking
 *   flush, it opens the r2w handle at that time. 
 *
 */
static int open_blockwake(struct shr *s, int flags) {
  int rc = -1, sc, need_r2w;

  if (flags & SHR_RDONLY) {
    s->w2r = bw_open(BW_WAIT, &s->r->w2r, &s->wait_fd);
    s->r2w = bw_open(BW_WAKE, &s->r->r2w);
    if ((s->w2r == NULL) || (s->r2w == NULL)) goto done;
    /* set initial readability of fd */
    sc = bw_force(s->w2r, s->r->u ? 1 : 0);
    if (sc < 0) goto done;
  }
  
  if (flags & SHR_WRONLY) {
    s->w2r = bw_open(BW_WAKE, &s->r->w2r);
    if (s->w2r == NULL) goto done;
    /* does writer need free-space wakeups? */
    need_r2w =  (((s->flags & SHR_NONBLOCK) == 0) &&
                 ((s->r->gflags & SHR_DROP) == 0)) ?  1 : 0;
    if (need_r2w) {
      s->r2w = bw_open(BW_WAIT, &s->r->r2w, &s->wait_fd);
      if (s->r2w == NULL) goto done;
    }
  }

  rc = 0;

 done:
  if (rc && s->r2w) { bw_close(s->r2w); s->r2w = NULL; }
  if (rc && s->w2r) { bw_close(s->w2r); s->w2r = NULL; }
  return rc;
}

static int validate_flags(int flags) {

  if (((flags & SHR_RDONLY) ^ (flags & SHR_WRONLY)) == 0)
    return -1; 

  if (flags & (SHR_OPEN_FENCE-1))
    return -1;

  return 0;
}

/*
 * init_cache
 *
 * the cache is used to reduce lock acquisition on the ring.
 * it is supported only for the writer side at this time.
 * 
 * called with s under lock
 */
static int init_cache(struct shr *s, int flags) {
  int rc = -1;

  if ((flags & SHR_WRONLY) == 0) return 0;
  if ((flags & SHR_BUFFERED) == 0) return 0;

 /* cache size is 10% of ring size (clamped) and 10k iov */
  s->c.sz = s->r->n / 10;
  if (s->c.sz > 1024*1024*1024) s->c.sz = 1024*1024*1024;
  if (s->c.sz < 1024)           s->c.sz = s->r->n;
  s->c.vt = MIN(10000, s->r->mm);

  s->c.buf = malloc( s->c.sz );
  if (s->c.buf == NULL) {
    shr_log("out of memory\n");
    goto done;
  }

  s->c.iov = calloc( s->c.vt,  sizeof(struct iovec));
  if (s->c.iov == NULL) {
    shr_log("out of memory\n");
    goto done;
  }

  rc = 0;

 done:
  if (rc < 0) {
    if (s->c.buf) free(s->c.buf);
    if (s->c.iov) free(s->c.iov);
    s->c.buf = NULL;
    s->c.iov = NULL;
  }
  return rc;
}

/*
 * shr_open opens a ring 
 *
 * the ring is opened for reading only OR writing only
 * and must exist already
 *
 * flags:
 *    SHR_RDONLY      - open for reading 
 *    SHR_WRONLY      - open for writing
 *    SHR_BUFFERED    - buffer writes
 *    SHR_NONBLOCK    - reads/writes fail immediately
 *                      when data/space unavailable
 *
 * returns:
 *  struct shr * on success (opaque to caller)
 *  NULL on error
 *
 */
struct shr *shr_open(char *file, unsigned flags, ...) {
  int rc = -1, sc, prot;
  struct shr *s = NULL;

  va_list ap;
  va_start(ap, flags);

  if (validate_flags(flags) < 0) {
    shr_log("shr_open: invalid flags\n");
    goto done;
  }

  s = calloc(1, sizeof(struct shr));
  if (s == NULL) {
    shr_log("out of memory\n");
    goto done;
  }
  s->ring_fd = -1;
  s->wait_fd = -1;
  s->flags = flags;

  s->ring_fd = open(file, O_RDWR);
  if (s->ring_fd == -1) {
    shr_log("open %s: %s\n", file, strerror(errno));
    goto done;
  }

  if (fstat(s->ring_fd, &s->s) == -1) {
    shr_log("stat %s: %s\n", file, strerror(errno));
    goto done;
  }

  /* read-write, since even readers write to ctl region */
  prot = PROT_READ|PROT_WRITE;

  s->buf = mmap(0, s->s.st_size, prot, MAP_SHARED, s->ring_fd, 0);
  if (s->buf == MAP_FAILED) {
    shr_log("mmap %s: %s\n", file, strerror(errno));
    s->buf = NULL;
    goto done;
  }

  if (lock(s->ring_fd) < 0) goto done;
  sc = validate_ring(s);
  if (sc < 0) {
    shr_log("validate_ring failed: %s (%d)\n", file, sc);
    goto done;
  }

  s->q = s->r->q;
  s->n = s->r->n;
  s->mm = s->r->mm;

  /* prefault and lock pages in memory if requested */
  sc = (s->r->gflags & SHR_MLOCK) ? mlock(s->buf, s->s.st_size) : 0;
  if (sc < 0) {
    shr_log("mlock: %s\n", strerror(errno));
    goto done;
  }

  if (open_blockwake(s, flags) < 0) goto done;
  if (init_cache(s, flags) < 0) goto done;
  if (shr_sync(s) < 0) goto done;
  rc = 0;

 done:
  if (s && (s->ring_fd != -1)) unlock(s->ring_fd);
  if (s && rc) {
    if (s->ring_fd != -1) close(s->ring_fd);
    if (s->buf) munmap(s->buf, s->s.st_size);
    free(s);
    s = NULL;
  }
  va_end(ap);
  return s;
}

/* 
 * reclaim unread messages from the ring (SHR_DROP mode).
 * The oldest portion of ring data is sacrificed.
 *
 * called under lock 
 *
 * preserves message boundaries by moving the read position 
 * to the now-eldest message after dropping to satisfy need
 */
static void reclaim(struct shr *s, size_t need, size_t niov) {
  size_t ab, am, i, e, z;
  shr_ctrl *r = s->r;
  struct msg *mv;

  ab = r->n - r->u;  /* available bytes */
  am = r->mm - r->m; /* available messages */

  mv = (struct msg*)(r->d + r->n + r->pad_len);

  assert(r->gflags & SHR_DROP);
  assert(r->mm >= niov);
  assert(r->m <= r->mp);
  assert(r->m <= r->mm);
  assert(r->mp);

  i = 0;
  z = 0;
  e = r->e;

  /* drop messages to free enough space */
  while (need > ab+z) {
    z += mv[ e ].len;
    i++;
    e++;
    if (e == s->mm) e = 0;
  }

  /* drop messages to free enough slots */
  while (niov > am+i) {
    z += mv[ e ].len;
    i++;
    e++;
    if (e == s->mm) e = 0;
  }

  r->u -= z;
  r->stat.bd += z;
  r->stat.md += i;
  r->mp -= i;
  r->m -= i;
  r->r = ( r->r + i ) % r->mm;
  r->e = ( r->e + i ) % r->mm;
  r->q += i;

  assert(r->n - r->u >= need);
  assert(r->mm - r->m >= niov);
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
 * Each read returns exactly one message.
 * Block if ring empty, or return immediately in SHR_NONBLOCK mode.
 *
 * returns:
 *   > 0 (number of bytes read from the ring)
 *   0   (ring empty, in non-blocking mode)
 *  -1   (error)
 *  -2   (buffer can't hold message)
 *   
 */
ssize_t shr_read(struct shr *s, char *buf, size_t len) {
  struct iovec io = {.iov_base = buf, .iov_len = len };
  size_t one = 1;
  return shr_readv(s, buf, len, &io, &one);
}


/*
 * next_msg_info
 *
 * without advancing afterward, return the buffer
 * position and length of the next available message.
 * actually, return two buffers and two lengths. why?
 * because the message may wrap around the end of ring 
 *
 * called with ring under lock
 *
 * returns
 *    0  (no message ready) 
 *    1  message is ready
 */
static int next_msg_info(shr *s, char **m1, size_t *l1,
                                 char **m2, size_t *l2 ) {
  int msg_ready, msg_wraps;
  size_t slot, len, pos;
  shr_ctrl *r = s->r;
  struct msg *mv;

  mv = (struct msg*)(r->d + r->n + r->pad_len);

  /* if farm reader's "next read" sequence number has
   * passed out of ring, advance to eldest available */
  if ((r->gflags & SHR_FARM) && (s->q < r->q)) {
    s->md += (r->q - s->q); 
    s->q = r->q;
  }

  if (r->gflags & SHR_FARM)
    msg_ready = (s->q < r->q + r->mp) ? 1 : 0;
  else
    msg_ready = r->m ? 1 : 0;

  if (msg_ready == 0) return 0;

  /* what slot in mv points to the next message? */
  slot = (r->gflags & SHR_FARM) ?
         (s->q % r->mm) :
         r->r;

  pos = mv[ slot ].pos;
  len = mv[ slot ].len;
  msg_wraps = (pos + len > r->n) ? 1 : 0;

  *m1 = &r->d[ pos ];
  *l1 = msg_wraps ? (r->n - pos) : len;
  *m2 = msg_wraps ? r->d : NULL;
  *l2 = msg_wraps ? (len - *l1) : 0;
  return 1;
}

/*
 * read multiple messages from ring
 *
 * also see shr_read
 *
 * the function copies ring data into buf, and populates the struct iovec
 * array so each one points to a message in buf.  the caller provides the
 * uninitialized array of iov and this function fills them in. niov is an
 * IN/OUT parameter; on input it's the number of structures in iov, and on
 * output it's how many this function filled in.
 *
 * returns:
 *   > 0 (number of bytes read from the ring)
 *   0   (no data in ring, in non-blocking mode)
 *  -1   (error)
 *  -2   (buffer can't hold message)
 *  -3   (caller descriptor became ready while blocked; see bw_ctl BW_POLLFD)
 *
 */
ssize_t
shr_readv(shr *s, char *buf, size_t len, struct iovec *iov, size_t *niov) {
  int sc, rc = -1, msg_ready;
  size_t mc = 0, l1, l2;
  shr_ctrl *r = s->r;
  char *m1, *m2;
  ssize_t nr=0;

  if (len == 0) goto done;
  if (*niov == 0) goto done;
  if (len > SSIZE_MAX) len = SSIZE_MAX;

  /* test or await data availability */
  while (1) {

    sc = lock(s->ring_fd);
    if (sc < 0) goto done;

    msg_ready = next_msg_info(s, &m1, &l1, &m2, &l2);
    if (msg_ready) break;

    if (s->flags & SHR_NONBLOCK) {
      bw_force(s->w2r, 0);
      rc = 0;
      goto done;
    }

    /* blocking wait. awake/retry */
    unlock(s->ring_fd);
    sc = bw_wait_ul(s->w2r);
    if (sc) {
      rc = sc; /* see bw_ctl BW_POLLFD */
      goto done;
    }
  }

  /* reached when data is available */
  while (msg_ready) {
    if (*niov == 0)  break; /* caller iov exhausted */
    if (l1+l2 > len) break; /* caller buf exhausted */
    iov[mc].iov_base = buf;
    iov[mc].iov_len = l1+l2;
    memcpy(buf, m1, l1);
    if (l2)
      memcpy(buf + l1, m2, l2);
    buf += (l1+l2);
    len -= (l1+l2);
    nr +=  (l1+l2);
    (*niov)--;
    mc++;

    /* advance read position */
    if (r->gflags & SHR_FARM) s->q++;
    else {
      r->r = (r->r + 1) % r->mm;
      r->u -= (l1+l2);
      r->m--;
    }

    msg_ready = next_msg_info(s, &m1, &l1, &m2, &l2);
  }

  r->stat.br += nr;
  r->stat.mr += mc;
  if (nr > 0) bw_wake(s->r2w);
  rc = (mc > 0) ? 0 : -2;
  bw_force(s->w2r, msg_ready);
  if (shr_sync(s) < 0) goto done;

 done:
  unlock(s->ring_fd);
  *niov = mc;
  return (rc == 0) ? (ssize_t)nr : rc;
}

/*
 * advance_eldest
 *
 * accomodate 'len' bytes of overwrite 
 * (from r->i). if it will overwrite
 * eldest message(s) then advance eldest
 *
 * called with ring under lock
 *
 */
static void advance_eldest(shr *s, size_t len) {
  shr_ctrl *r = s->r;
  struct msg *mv;
  size_t ep, el, eoe, eow;
  int wsie, weie;

  mv = (struct msg*)(r->d + r->n + r->pad_len);

  while(r->mp) {
    ep = mv[ r->e ].pos;
    el = mv[ r->e ].len;

    eoe = (ep + el) % r->n;
    eow = (r->i + len) % r->n;

    wsie = ((r->i >= ep) && (r->i < eoe)) ? 1 : 0;
    weie = ((eow > ep) && (eow <= eoe)) ? 1: 0;

    if ((wsie == 0) && (weie == 0)) break;

    r->e = (r->e + 1) % r->mm;
    r->mp--;
    r->q++;
  }
}

/*
 * write sequential io buffers into ring
 *
 * if there is sufficient space in the ring - copy the whole iovec in.
 * if there is insufficient free space in the ring- wait for space, or
 * return 0 immediately in non-blocking mode. only writes all or nothing.
 * each iovec element becomes one message.
 *
 * returns:
 *   > 0 (number of bytes copied into ring, always the full iovec)
 *   0   (insufficient space in ring, in non-blocking mode)
 *  -1   (error, such as the iovec exceeds the total ring capacity)
 *  -2   (not used)
 *  -3   (caller descriptor became ready while blocked; see bw_ctl BW_POLLFD)
 *
 */
ssize_t shr_writev(shr *s, struct iovec *iov, size_t niov) {
  size_t bsz, len=0, i, l1, l2;
  int rc = -1, sc, msg_wraps;
  shr_ctrl *r = s->r;
  struct msg *mv;
  ssize_t nr;
  char *buf;

  for(i=0; i < niov; i++) {
    len += iov[i].iov_len;
    if (len == 0) goto done;
    if (len > SSIZE_MAX) goto done;
    if (len > s->n) goto done;
    if (niov > s->mm) goto done;
  }
  if (len == 0) goto done;

  while (s->flags & SHR_BUFFERED) {

    /* sink writev into cache if it fits.
     * cache only as much can be flushed 
     * at once to an empty ring */
    if ((len  <= (s->c.sz - s->c.n))  &&
        (niov <= (s->c.vt - s->c.vm))) {
      for(i=0; i < niov; i++) {
        memcpy(s->c.buf + s->c.n, iov[i].iov_base, iov[i].iov_len);
        s->c.iov[ s->c.vm ].iov_base = s->c.buf + s->c.n;
        s->c.iov[ s->c.vm ].iov_len = iov[i].iov_len;
        s->c.n += iov[i].iov_len;
        s->c.vm++;
      }
      return len;
    }

    /* didn't fit. flush cache, then either
     * cache current write, or conduct now */
    if (s->c.n == 0) break;
    s->flags &= ~SHR_BUFFERED;
    nr = shr_writev(s, s->c.iov, s->c.vm);
    s->flags |=  SHR_BUFFERED;
    if (nr <= 0) return nr;
    s->c.n = 0;
    s->c.vm = 0;
  }

  while (1) {
    if (lock(s->ring_fd) < 0) goto done;

    /* if ring has enough free space, break */
    if ((r->n - r->u >= len) && 
        (r->mm - r->m >= niov)) break;

    if (r->gflags & SHR_DROP) {
      reclaim(s, len, niov);
      break;
    }

    if (s->flags & SHR_NONBLOCK) {
      rc = 0;
      len = 0;
      goto done;
    }

    unlock(s->ring_fd);
    sc = bw_wait_ul(s->r2w);
    if (sc) return sc;
  }

  assert(r->n - r->u >= len);
  assert(r->m + niov <= r->mm);
  assert(r->mp <= r->mm);
  mv = (struct msg*)(r->d + r->n + r->pad_len);

  /* at this point, sufficient free space in the
   * ring and enough mv slots are available.
   * if we're about to overwrite the eldest read 
   * message(s) we need to advance eldest too. */
  advance_eldest(s, len);

  for(i=0; i < niov; i++) {

    buf = iov[i].iov_base;
    bsz = iov[i].iov_len;
    assert(bsz > 0);

    mv[ ( r->e + r->mp ) % r->mm].pos = r->i;
    mv[ ( r->e + r->mp ) % r->mm].len = bsz;

    msg_wraps = (r->i + bsz > r->n) ? 1 : 0;
    l1 = msg_wraps ? (r->n - r->i) : bsz;
    l2 = msg_wraps ? (bsz - l1) : 0;

    memcpy(r->d + r->i, buf, l1);
    if (l2) memcpy(r->d, buf + l1, l2);
    r->i = (r->i + bsz) % r->n;
    r->u += bsz;
    r->mp++;
    r->m++;
  }

  sc = bw_wake(s->w2r);
  if (sc) goto done;
  r->stat.bw += len;
  r->stat.mw += niov;
  if (shr_sync(s) < 0) goto done;
  rc = 0;

 done:
  unlock(s->ring_fd);
  return (rc == 0) ? (ssize_t)len : -1;
}

/*
 * get and/or set the "app data" under lock
 * 
 * TO Get the data only: 
 *   pass set as NULL.
 *   if *get is NULL then malloc'd buffer is 
 *   placed into *get of size *sz, and 
 *   caller must eventually free it.
 *   otherwise, if *get is non-NULL and *sz
 *   is sufficiently large, then the data
 *   is stored into *get and *sz is set to
 *   the actual content length.
 *
 * TO Set the data only:
 *   pass get as NULL.
 *   pass set as the buffer of size *sz
 *
 * TO both get and then set the data at once:
 *   pass *get and set as non-NULL 
 *   and *sz to their length
 *
 * returns:
 *  0 success
 * -1 error (zero len data, size mismatch, or lock error)
 */
int shr_appdata(shr *s, void **get, void *set, size_t *sz) {
  shr_ctrl *r = s->r;
  int rc = -1;
  char *ad;

  if (lock(s->ring_fd) < 0) goto done;
  if (r->app_len == 0) goto done;

  /* appdata is stored after ring data and after mv list */
  ad = r->d + r->n + r->pad_len + r->mv_len;

  if ((get != NULL) && (*get == NULL) && (set == NULL)) {
    *sz = r->app_len;
    *get = malloc(*sz);
    if (*get == NULL) {
      shr_log("out of memory\n");
      goto done;
    }
  }

  if ((get != NULL) && (*get != NULL) && (set == NULL)) {
    if (*sz < r->app_len) goto done;
    memcpy(*get, ad, *sz);
    *sz = r->app_len;
  }

  if ((get == NULL) && (set != NULL)) {
    if (*sz != r->app_len) goto done;
    memcpy(ad, set, *sz);
  }

  if ((get != NULL) && (*get != NULL) && (set != NULL)) {
    if (*sz != r->app_len) goto done;
    memcpy(*get, ad, *sz);
    memcpy(ad, set, *sz);
  }

  if (shr_sync(s) < 0) goto done;
  rc = 0;

 done:
  unlock(s->ring_fd);
  return rc;
}

/*
 * shr_flush
 *
 * flush write buffer for an SHR_BUFFERED ring
 *
 * NOTE
 *  for a ring in NON-BLOCKING mode, this may return 0
 *  if the ring is too full to accept the cached data!
 *
 * wait parameter:
 *  if non-zero, causes a blocking flush. this only matters
 *  if the ring is open for non-blocking writes.
 *
 * returns:
 *
 *  >= 0 bytes written to ring (see NOTE!)
 *   < 0 error
 */
ssize_t shr_flush(struct shr *s, int wait) {
  int toggled = 0;
  ssize_t nr = -1;

  assert(s->flags & SHR_WRONLY);

  if (s->c.n == 0) {
    nr = 0;
    goto done;
  }

  /* non-blocking caller wants blocking flush? */
  if ((s->flags & SHR_NONBLOCK) && wait) {

    s->flags &= ~SHR_NONBLOCK;
    toggled = 1;

    /* to block, a writer needs the r2w handle */
    if (s->r2w == NULL) {
      if (lock(s->ring_fd) < 0) goto done;
      s->r2w = bw_open(BW_WAIT, &s->r->r2w, &s->wait_fd);
      unlock(s->ring_fd);
      if (s->r2w == NULL) goto done;
    }
  }

  s->flags &= ~SHR_BUFFERED;
  nr = shr_writev(s, s->c.iov, s->c.vm);
  s->flags |=  SHR_BUFFERED;
  if (nr < 0) goto done;
  if (nr > 0) {
    s->c.n = 0;
    s->c.vm = 0;
  }

 done:
  if (toggled) s->flags |= SHR_NONBLOCK;
  return nr;
}

/*
 * shr_close
 *
 * NOTE: 
 *
 *  a writer in SHR_NONBLOCK|SHR_BUFFERED mode
 *  can do a blocking flush by shr_flush(s,1);
 *  before shr_close to flush any cached data.
 *  otherwise a non-blocking buffered writer
 *  gets only a non-blocking final flush.
 *
 */
void shr_close(struct shr *s) {
  
  /* flush cache */
  if (s->c.n) shr_flush(s,0);

  /* release bw handles under lock.
   * don't close s->wait_fd- bw does! */
  if (lock(s->ring_fd) < 0) goto end;
  if (s->w2r) bw_close(s->w2r);
  if (s->r2w) bw_close(s->r2w);
  unlock(s->ring_fd);

 end:
  /* free the cache if any */
  if (s->c.buf) free(s->c.buf);
  if (s->c.iov) free(s->c.iov);
  /* unmap the ring buffer */
  assert(s->buf);
  munmap(s->buf, s->s.st_size);
  close(s->ring_fd);
  free(s);
}

/*
 * shr_ctl
 *
 * special purpose miscellaneous api
 * flag determines behavior
 *
 *  flag          arguments  meaning
 *  -----------   ---------  ----------------------------------------------
 *  SHR_POLLFD    int fd     add fd to epoll set when blocked internally in
 *                           shr_read/write, cause it to return -3 if ready
 *
 * returns
 *  0 on success
 * -1 on error
 */
int shr_ctl(shr *s, int flag, ...) {
  int rc = -1, fd, sc;

  va_list ap;
  va_start(ap, flag);

  switch(flag) {

    case SHR_POLLFD:
      fd = (int)va_arg(ap, int);

      /* add the fd to be monitored to the "wait" mode bw depending on r vs w */
      sc = 0;
      if ((s->flags & SHR_WRONLY) && s->r2w) sc = bw_ctl(s->r2w, BW_POLLFD, fd);
      if  (s->flags & SHR_RDONLY)            sc = bw_ctl(s->w2r, BW_POLLFD, fd);
      if (sc < 0) {
        shr_log("bw_ctl: error\n");
        goto done;
      }
      break;

    default:
      shr_log("shr_ctl: unknown flag %d\n", flag);
      goto done;
      break;
  }

  rc = 0;

 done:
  va_end(ap);
  return rc;
}

