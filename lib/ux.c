#include <dirent.h>
#include <string.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include "bw.h"

/* internal prototypes */
static int get_socket_inodes(pid_t, size_t *, size_t, size_t *);
static int has_socket_name(size_t *, size_t, char *);

/*
 *  has_pid_socket
 *
 *  test if pid exists and has open the given domain socket 
 *  depends on the /proc filesystem
 *
 *  returns:
 *    0 (no)
 *    1 (yes)
 *   <0 (indeterminate)
 */

#define MAX_INODES 1000
int has_pid_socket(pid_t pid, char *name) {
  size_t inodes[MAX_INODES], n_inodes=0;
  int sc;

  /* test existence of pid - no signal is sent */
  sc = kill(pid, 0);
  if ((sc < 0) && (errno == ESRCH)) return 0;

  sc = get_socket_inodes(pid, inodes, MAX_INODES, &n_inodes);
  if (sc < 0) return sc;

  sc = has_socket_name(inodes, n_inodes, name);
  return sc;
}

/*
 * get_socket_inodes
 *
 * populate inodes[] belonging to pid's open sockets
 *
 * returns:
 *   0 success (*n_inodes and *inodes is populated)
 *  -1 error
 *
 * works by parsing /proc/<pid>/fd where fd are symlinks:
 *   3 -> socket:[39882]
 * the number in square brackets is the inode number. for
 * domain sockets it correlates to name in /proc/net/unix 
 *
 */
static int get_socket_inodes(pid_t pid, size_t *inodes, 
                             size_t max_inodes, size_t *n_inodes) {
  char target[100], *c, path[100];
  struct dirent *dent;
  int rc = -1, sc;
  DIR *d = NULL;
  size_t i;

  sc = snprintf(path, sizeof(path), "/proc/%u/fd", pid);
  if (sc >= (int)sizeof(path)) {
    bw_log("buffer exhausted\n");
    goto done;
  }

  d = opendir(path);
  if (d == NULL) {
    if (errno != EACCES) {
      bw_log("opendir %s: %s\n", path, strerror(errno));
    }
    goto done;
  }

  while ( (dent = readdir(d)) != NULL) {

    if (dent->d_type != DT_LNK) continue;

    sc = snprintf(path, sizeof(path), "/proc/%u/fd/%s", pid, dent->d_name);
    if (sc >= (int)sizeof(path)) {
      bw_log("buffer exhausted\n");
      goto done;
    }

    sc = readlink(path, target, sizeof(target));
    if (sc < 0) {
      bw_log("readlink: %s\n", strerror(errno));
      goto done;
    }

    if (sc == sizeof(target)) continue; /* truncation; too long to be socket */
    if (sc < 8) continue;
    if (memcmp(target, "socket:[", 8)) continue;
    sc -= 8;

    /* parse the inode number from socket:[12345] */
    for (i=0, c = &target[8]; sc > 0; sc--, c++) {
      if (*c == ']') break;
      if ((*c < '0') || (*c > '9')) {
        bw_log("readlink: parsing error\n");
        goto done;
      }
      i = (i * 10) + (*c - '0');
    }

    if (*n_inodes == max_inodes) {
      bw_log("too many socket inodes\n");
      goto done;
    }

    inodes[ *n_inodes ] = i;
    (*n_inodes)++;
  }

  closedir(d);
  rc = 0;

 done:
  return rc;
}

/* read a file of unknown size, such as special files in /proc. 
 * place its size into len, returning buffer or NULL on error.
 * caller should free the buffer eventually.
 */
static char *slurp_special(char *file, size_t *len) {
  char *buf=NULL, *b, *tmp;
  int fd = -1, rc = -1, eof=0;
  size_t sz, br=0, l;
  ssize_t nr;

  /* initial guess at a sufficient buffer size */
  sz = 10000;

  fd = open(file, O_RDONLY);
  if (fd < 0) {
    bw_log("open: %s\n", strerror(errno));
    goto done;
  }

  while(!eof) {

    tmp = realloc(buf, sz);
    if (tmp == NULL) {
      bw_log("out of memory\n");
      goto done;
    }

    buf = tmp;
    b = buf + br;
    l = sz - br;

    do {
      nr = read(fd, b, l);
      if (nr < 0) {
        bw_log("read: %s\n", strerror(errno));
        goto done;
      }

      b += nr;
      l -= nr;
      br += nr;

      /* out of space? double buffer size */
      if (l == 0) { 
        sz *= 2;
        break;
      }

      if (nr == 0) eof = 1;

    } while (nr > 0);
  }

  *len = br;
  rc = 0;

 done:
  if (fd != -1) close(fd);
  if (rc && buf) { free(buf); buf = NULL; }
  return buf;
}

/* 
 * find start and length of column N (one-based)
 * in input buffer buf of length buflen
 *
 * columns must be space delimited
 * returns NULL if column not found

 * the final column may end in newline or eob  
 *
 */
static char *get_col(int col, size_t *len, char *buf, size_t buflen) {
  char *b, *start=NULL, *eob;
  int num;

  eob = buf + buflen;

  b = buf;
  num = 0;  /* column number */
  *len = 0; /* column length */

  while (b < eob) {

    if ((*b == ' ') && (num == col)) break; /* end of sought column */
    if (*b == '\n') break;                  /* end of line */

    if  (*b == ' ') *len = 0;               /* skip over whitespace */
    if ((*b != ' ') && (*len == 0)) {       /* record start of column */
      num++;
      start = b;
    }
    if  (*b != ' ') (*len)++;               /* increment column length */
    b++;
  }

  if ((*len) && (num == col)) return start;
  return NULL;
}

/*
 * does pid have open a domain socket of given name whose inode is in the list?
 *
 * returns:
 *   0: no
 *   1: yes
 *  -1: error
 *
 * /proc/net/unix has lines like:
 * 
 * Num       RefCount Protocol Flags    Type St Inode Path
 * 0000000000000000: 00000002 00000000 00010000 0001 01 13474 /run/uuidd/request
 * 0000000000000000: 00000002 00000000 00010000 0001 01 14066 @0000a
 * 
 * 
 */
static int has_socket_name(size_t *inodes, size_t n_inodes, char *sockname) {
  char *buf = NULL, *b, *inode, *name, *line;
  size_t len, sz=0, inum, n, namelen;
  int rc = -1;

  buf = slurp_special("/proc/net/unix", &sz);
  if (buf == NULL) goto done;
  namelen = strlen(sockname);

  /* find initial newline; discard header row */
  b = buf;
  while ((b < buf+sz) && (*b != '\n')) b++;
  line = b+1;

  while (line < buf+sz) {

    /* get inode string */
    inode = get_col(7, &len, line, sz-(line-buf));
    if (inode == NULL) goto done;

    /* convert to number */
    inum = 0;
    b = inode;
    while(len--) {
      if ((*b < '0') || (*b > '9')) goto done;
      inum = (inum*10) + (*b - '0');
      b++;
    }

    /* name is after inode */
    name = get_col(8, &len, line, sz-(line-buf));
    if (name == NULL) goto done;

    /* does name match? */
    if ((len == namelen) && (memcmp(name, sockname, len) == 0)) {
      
      /* name matches. is inode in list? */
      for(n=0; n < n_inodes; n++) {
        if (inum != inodes[n]) continue;

        /* match found. */
        rc = 1;
        goto done;
      }
    }

    /* advance to next line */
    b = name+len;
    while ((b < buf+sz) && (*b != '\n')) b++;
    line = b+1;
  }

  rc = 0;

 done:
  if (buf) free(buf);
  return rc;
}


