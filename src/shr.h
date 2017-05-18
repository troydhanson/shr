#ifndef _SHARED_RING_H_
#define _SHARED_RING_H_

#include <sys/time.h> /* struct timeval (for stats) */
#include <sys/uio.h>  /* struct iovec (for readv/writev) */

#if defined __cplusplus
extern "C" {
#endif

/* opaque data structure */
struct shr;
typedef struct shr shr;

/* stats structure */
struct shr_stat {

  /* this set of stats reflects the current period. when the caller resets the
   * the period (by calling shr_start with a reset value) these stats get zeroed. 
   */
  struct timeval start; /* start of the stats period (last reset) */
  size_t bw, br;        /* cumulative bytes written to/read from ring in period */
  size_t mw, mr;        /* cumulative messages written to/read from ring in period */
  size_t md, bd;        /* in lru drop mode: messages dropped/bytes dropped */

  /* this set of numbers reflect the ring state at the moment shr_stat is called,
   * in other words, resetting the current period has no bearing on these numbers. 
   */
  size_t bn;            /* ring size in bytes */
  size_t bu;            /* current unread bytes (ready to read) in ring */
  size_t mu;            /* current unread messages (ready to read) in ring */
};

int shr_init(char *file, size_t sz, unsigned flags, ...);
shr *shr_open(char *file, unsigned flags, ...);
int shr_get_selectable_fd(shr *s);
ssize_t shr_read(shr *s, char *buf, size_t len);
ssize_t shr_write(shr *s, char *buf, size_t len);
ssize_t shr_readv(shr *s, char *buf, size_t len, struct iovec *iov, int *iovcnt);
ssize_t shr_writev(shr *s, struct iovec *iov, int iovcnt);
void shr_close(shr *s);
int shr_stat(shr *s, struct shr_stat *stat, struct timeval *reset);

/* flags */

#define SHR_OVERWRITE    (1U << 0)  /* shr_init */
#define SHR_KEEPEXIST    (1U << 1)  /* shr_init */
#define SHR_MESSAGES     (1U << 2)  /* shr_init */
#define SHR_DROP         (1U << 3)  /* shr_init */
#define SHR_APPDATA      (1U << 4)  /* shr_init */
#define SHR_OPEN_FENCE   (1U << 10) /* barrier between init and open flags */
#define SHR_GET_APPDATA  (1U << 11) /* shr_open */
#define SHR_RDONLY       (1U << 12) /* shr_open */
#define SHR_WRONLY       (1U << 13) /* shr_open */
#define SHR_NONBLOCK     (1U << 14) /* shr_open */

#if defined __cplusplus
}
#endif
#endif
