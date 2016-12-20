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
};

void usage() {
  fprintf(stderr,"usage: %s [options] <ring>\n", CF.prog);
  fprintf(stderr,"options:\n"
                 "         -c [-f <mode>] [-s <size>]  (create ring)\n"
                 "         -q                          (status ring) [default]\n"
                 "         -w -s <count>               (do <count> writes @ 1/sec)\n"
                 "         -r -b [0|1]                 (read ring, -b 0=nonblock)\n"
                 "\n"
                 "  <size> is allowed to have k/m/g/t suffix\n"
                 "  <mode> is a legal combination of okml\n"
                 "         o = overwrite, clear and resize ring, if it already exists\n"
                 "         k = keep existing ring size/content, if it already exists\n"
                 "         m = message mode; each i/o is a full message, not a stream\n"
                 "         l = lru-overwrite; reclaim oldest elements when ring fills\n"
                 "\n");
  exit(-1);
}

int main(int argc, char *argv[]) {
  int opt, rc=-1, sc;
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
         c = optarg;
         while((*c) != '\0') {
           switch (*c) {
             case 'o': CF.flags |= SHR_OVERWRITE; break;
             case 'k': CF.flags |= SHR_KEEPEXIST; break;
             case 'm': CF.flags |= SHR_MESSAGES; break;
             case 'l': CF.flags |= SHR_LRU_DROP; break;
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
      rc = shr_init(CF.ring, CF.size, CF.flags);
      if (rc < 0) goto done;
      break;

    case mode_status:
      if (CF.size || CF.flags) usage();
      CF.shr = shr_open(CF.ring, SHR_RDONLY);
      if (CF.shr == NULL) goto done;
      rc = shr_stat(CF.shr, &stat, NULL);
      if (rc < 0) goto done;
      printf("w: bw %ld, br %ld, mw %ld, mr %ld, md %ld, bd %ld, bn %ld, bu %ld mu %ld\n",
         stat.bw, stat.br, stat.mw, stat.mr, stat.md, stat.bd, stat.bn, stat.bu, stat.mu);
      break;

    case mode_read:
      CF.shr = shr_open(CF.ring, SHR_RDONLY | ((CF.block == 0) ? SHR_NONBLOCK : 0));
      if (CF.shr == NULL) goto done;
      while(1) {
        nr = shr_read(CF.shr, buf, sizeof(buf));
        if (nr <= 0) {
          fprintf(stderr, "shr_read error: %ld\n", nr);
          goto done;
        }
        if (nr) printf("%ld bytes: [%.*s]\n", nr, (int)nr, buf);
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
        if ((nr > 0) && (tstr[nr-1] == '\n')) tstr[nr-1] = '\0';
        if (shr_write(CF.shr, tstr, strlen(tstr)) < 0) goto done;
        sleep(1);
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
