#include <sys/types.h>
#ifndef _BLOCK_WAKE_H_
#define _BLOCK_WAKE_H_
#if defined __cplusplus
extern "C" {
#endif

/* CFLAG -DBW_SILENT silences error messages */
#ifndef BW_SILENT
#include <stdio.h>
#define bw_log(...) do { fprintf(stderr, __VA_ARGS__); } while(0)
#define bw_trace(w, ...) do { if ((w) & BW_TRACE) fprintf(stderr, __VA_ARGS__); } while(0)
#else
#define bw_log(...) do { } while(0)
#define bw_trace(w, ...)  do { } while(0)
#endif

/* bw_handle
 *
 * this structure must be free of pointers, as
 * we require it be usable when mapped into the
 * address space of N unrelated processes.
 */
#define BW_NAMELEN 8
#define BW_WAITMAX 64
struct bw_handle {
  int seqno;
  struct {
    pid_t pid;
    char name[BW_NAMELEN];
  } wr[BW_WAITMAX];
};

struct bw_t;
typedef struct bw_t bw_t;
typedef struct bw_handle bw_handle;

#define BW_WAKE   (1U << 1)
#define BW_WAIT   (1U << 2)
#define BW_TRACE  (1U << 3)
#define BW_POLLFD (1U << 4) /* bw_ctl flag */

/* API */
bw_t * bw_open(int flags, bw_handle *h, ...); /* CALL WITH HANDLE UNDER LOCK */
int bw_wake(bw_t *w);                         /* CALL WITH HANDLE UNDER LOCK */
void bw_close(bw_t *w);                       /* CALL WITH HANDLE UNDER LOCK */
int bw_force(bw_t *w, int ready);             /* CALL WITH HANDLE UNDER LOCK */
int bw_ready_ul(bw_t *w);                     /* call WITHOUT lock on handle */
int bw_wait_ul(bw_t *w);                      /* call WITHOUT lock on handle */
int bw_ctl(bw_t *w, int flag, ...);           /* call WITHOUT lock on handle */

#if defined __cplusplus
}
#endif
#endif
