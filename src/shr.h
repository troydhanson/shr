#ifndef _SHARED_RING_H_
#define _SHARED_RING_H_

#include <sys/time.h> /* struct timeval (for stats) */
#include <sys/uio.h>  /* struct iovec (for readv/writev) */

#if defined __cplusplus
extern "C" {
#endif

/* -DSHR_SILENT silences error logging to stderr 
 * from shr/bw libs; error codes still returned */
#ifndef SHR_SILENT
#include <stdio.h>
#define shr_log(...) do { fprintf(stderr, __VA_ARGS__); } while(0)
#else
#define shr_log(...) do { } while(0)
#define BW_SILENT
#endif

/* opaque data structure */
struct shr;
typedef struct shr shr;

/* stats structure */
struct shr_stat {

  /* stats for the current period. 
   * caller resets these at will.
   */
  struct timeval start; /* start of the stats period (last reset) */
  size_t bw, br;        /* bytes written to/read from ring in period */
  size_t mw, mr;        /* messages written to/read from ring in period */
  size_t md, bd;        /* in drop mode: messages dropped/bytes dropped */

  /* this set of numbers describes the ring,
   * in terms of its size and unread content.
   * a reset has no bearing on these numbers. 
   */
  size_t bn;            /* ring size in bytes */
  size_t bu;            /* current unread bytes (ready to read) in ring */
  size_t mu;            /* current unread messages (ready to read) in ring */
  size_t mm;            /* max number of messages ring can hold */

  /* cache state in SHR_BUFFERED mode.
   * reflects the calling client only.
   */
  size_t cn;            /* cache size in bytes */
  size_t cm;            /* messages in cache */
  size_t cb;            /* bytes in cache */

  /* ring attributes */
  unsigned flags;
};

int shr_init(char *file, size_t sz, unsigned flags, ...);
shr *shr_open(char *file, unsigned flags, ...);
int shr_get_selectable_fd(shr *s);
ssize_t shr_read(shr *s, char *buf, size_t len);
ssize_t shr_write(shr *s, char *buf, size_t len);
ssize_t shr_readv(shr *s, char *buf, size_t len, struct iovec *iov, size_t *iovcnt);
ssize_t shr_writev(shr *s, struct iovec *iov, size_t iovcnt);
ssize_t shr_flush(struct shr *s, int wait);
void shr_close(shr *s);
int shr_appdata(shr *s, void **get, void *set, size_t *sz);
int shr_stat(shr *s, struct shr_stat *stat, struct timeval *reset);
size_t shr_farm_stat(shr *s, int reset);
int shr_ctl(shr *s, int flag, ...);

/* flags */

#define SHR_KEEPEXIST    (1U << 0)  /* shr_init */
#define SHR_DROP         (1U << 1)  /* shr_init */
#define SHR_APPDATA_1    (1U << 2)  /* shr_init */
#define SHR_FARM         (1U << 3)  /* shr_init */
#define SHR_MAXMSGS_2    (1U << 4)  /* shr_init */
#define SHR_SYNC         (1U << 5)  /* shr_init */
#define SHR_MLOCK        (1U << 6)  /* shr_init */
#define SHR_OPEN_FENCE   (1U << 12) /* barrier between init and open flags */
#define SHR_RDONLY       (1U << 13) /* shr_open */
#define SHR_WRONLY       (1U << 14) /* shr_open */
#define SHR_NONBLOCK     (1U << 15) /* shr_open */
#define SHR_BUFFERED     (1U << 16) /* shr_open */
#define SHR_POLLFD       (1U << 17) /* shr_ctl */

#define SHR_APPDATA SHR_APPDATA_1 /* shr_init alias */
#define SHR_MESSAGES     (0)      /* shr_init obsolete / always enabled */
#define SHR_OVERWRITE    (0)      /* shr_init obsolete / default enabled */

#if defined __cplusplus
}
#endif
#endif
