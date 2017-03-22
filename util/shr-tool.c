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
 * shr test tool
 *
 */

struct {
  char *prog;
  int verbose;
  char *ring;
  enum {mode_status, mode_create, mode_write, mode_read} mode;
  struct shr *shr;
  size_t size;
  int flags;
  int block;
} CF = {
  .flags = SHR_KEEPEXIST | SHR_MESSAGES | SHR_DROP,
};

void usage() {
  fprintf(stderr,"usage: %s [options] <ring>\n"
                 "\n"
                 "[query mode ]: -q [default]\n"
                 "  show ring metrics: bytes/message in ring, unread, etc\n"
                 "\n"
                 "[read mode  ]: -r [-b 1]\n"
                 "  displays ring data on stdout, hexdump to stderr (-b 1 to block)\n"
                 "\n"
                 "[write mode ]: -w [-s <count>]\n"
                 "  writes test data to ring, at 1x/second, til count is reached \n"
                 "\n"
                 "[create mode]: -c -s <size> [-f <mode>]\n"
                 "  create ring of given size and mode\n"
                 "  <size> in bytes with optional k/m/g/t suffix\n"
                 "  <mode> bits (default: mdk)\n"
                 "         m  message mode  (each read/write comprises a message)\n"
                 "         d  drop mode     (drop unread data when full)\n"
                 "         k  keep existing (if ring exists, leave as-is)\n"
                 "         o  overwrite     (if ring exists, re-create)\n"
                 "\n"
                 "\n", CF.prog);
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

int main(int argc, char *argv[]) {
  int opt, rc=-1, sc, mode;
  CF.prog = argv[0];
  char unit, *c, buf[100];
  struct shr_stat stat;
  ssize_t nr;

  while ( (opt = getopt(argc,argv,"vhcs:qf:wrb:")) > 0) {
    switch(opt) {
      case 'v': CF.verbose++; break;
      case 'h': default: usage(); break;
      case 'b': CF.block = atoi(optarg); break;
      case 'c': CF.mode=mode_create; break;
      case 'q': CF.mode=mode_status; break;
      case 'w': CF.mode=mode_write; break;
      case 'r': CF.mode=mode_read; break;
      case 's':  /* ring size */
         sc = sscanf(optarg, "%ld%c", &CF.size, &unit);
         if (sc == 0) usage();
         if (sc == 2) {
            switch (unit) {
              case 't': case 'T': CF.size *= 1024; /* fall through */
              case 'g': case 'G': CF.size *= 1024; /* fall through */
              case 'm': case 'M': CF.size *= 1024; /* fall through */
              case 'k': case 'K': CF.size *= 1024; break;
              default: usage(); break;
            }
         }
         break;
      case 'f': /* ring mode */
         CF.flags = 0; /* override default */
         c = optarg;
         while((*c) != '\0') {
           switch (*c) {
             case 'm': CF.flags |= SHR_MESSAGES; break;
             case 'd': CF.flags |= SHR_DROP; break;
             case 'k': CF.flags |= SHR_KEEPEXIST; break;
             case 'o': CF.flags |= SHR_OVERWRITE; break;
             default: usage(); break;
           }
           c++;
         }
         break;
    }
  }

  if (optind < argc) CF.ring = argv[optind++];
  if (CF.ring == NULL) usage();
  
  switch(CF.mode) {

    case mode_create:
      if (CF.size == 0) usage();
      rc = shr_init(CF.ring, CF.size, CF.flags);
      if (rc < 0) goto done;
      break;

    case mode_status:
      CF.shr = shr_open(CF.ring, SHR_RDONLY);
      if (CF.shr == NULL) goto done;
      rc = shr_stat(CF.shr, &stat, NULL);
      if (rc < 0) goto done;
      printf(" bytes-written %ld\n"
             " bytes-read %ld\n"
             " bytes-dropped %ld\n"
             " messages-written %ld\n"
             " messages-read %ld\n"
             " messages-dropped %ld\n"
             " ring-size %ld\n"
             " bytes-ready %ld\n"
             " messages-ready %ld\n",
         stat.bw, stat.br, stat.bd, stat.mw, stat.mr, stat.md, stat.bn,
         stat.bu, stat.mu);
      break;

    case mode_read:
      mode = SHR_RDONLY;
      if (CF.block == 0) mode |= SHR_NONBLOCK;
      CF.shr = shr_open(CF.ring, mode);
      if (CF.shr == NULL) goto done;
      while(1) {
        nr = shr_read(CF.shr, buf, sizeof(buf));
        if (nr <= 0) {
          fprintf(stderr, "shr_read: %s (%ld)\n", 
             (nr == 0) ? "end-of-data" : "error", nr); 
          goto done;
        } else {
          fprintf(stderr, "read %ld bytes\n", nr);
          hexdump(buf,nr);                /* hex to stderr */
          printf("%.*s\n", (int)nr, buf); /* ascii to stdout */
          fflush(stdout);
        }
      }
      break;

    case mode_write:
      CF.shr = shr_open(CF.ring, SHR_WRONLY);
      if (CF.shr == NULL) goto done;
      if (CF.size == 0) CF.size++;
      while (CF.size--) {
        time_t now = time(NULL);
        char *tstr = asctime(localtime(&now));
        nr = strlen(tstr);
        if (shr_write(CF.shr, tstr, nr) < 0) goto done;
        if (CF.size) sleep(1);
      }
      break;

    default: 
      assert(0);
      break;
  }

  rc = 0;
 
 done:
  if (CF.shr) shr_close(CF.shr);
  return 0;
}
