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
  enum {mode_status, mode_create, mode_write, mode_read, mode_tee} mode;
  struct shr *shr;
  size_t size;
  int flags;
  int block;
} CF = {
  .flags = SHR_KEEPEXIST | SHR_MESSAGES | SHR_DROP,
};

void usage() {
  fprintf(stderr,"usage: %s <mode> [options] <ring>\n"
                 "\n"
                 "[query mode ]: -q  <ring>\n"
                 "  show ring metrics: bytes/message in ring, unread, etc\n"
                 "  this is the default mode\n"
                 "\n"
                 "[read mode  ]: -r [-b 1] <ring>\n"
                 "  displays ring data on stdout, hexdump to stderr (-b 1 to block)\n"
                 "\n"
                 "[write mode ]: -w <ring>\n"
                 "  writes data to ring, from lines of stdin until eof\n"
                 "\n"
                 "[tee mode ]: -t <input-ring> <output-ring> <output-ring>\n"
                 "  tee's one ring into two output rings\n"
                 "\n"
                 "[create mode]: -c -s <size> [-m <mode>] <ring> [<ring> ...]\n"
                 "  create ring(s) of given size and mode\n"
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
  char unit, *c, buf[10000], *app_data, line[1000];
  struct shr_stat stat;
  struct shr *o1=NULL, *o2=NULL;
  char *ring1=NULL, *ring2=NULL;
  size_t app_len;
  ssize_t nr;

  while ( (opt = getopt(argc,argv,"vhcs:qm:wrtb:")) > 0) {
    switch(opt) {
      case 'v': CF.verbose++; break;
      case 'h': default: usage(); break;
      case 'b': CF.block = atoi(optarg); break;
      case 'c': CF.mode=mode_create; break;
      case 'q': CF.mode=mode_status; break;
      case 'w': CF.mode=mode_write; break;
      case 'r': CF.mode=mode_read; break;
      case 't': CF.mode=mode_tee; break;
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
      case 'm': /* ring mode */
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

  if (optind < argc) CF.ring = argv[optind];
  if (CF.ring == NULL) usage();
  
  switch(CF.mode) {

    case mode_tee:
      CF.shr = shr_open(CF.ring, SHR_RDONLY | SHR_GET_APPDATA, 
                        &app_data, &app_len);
      if (CF.shr == NULL) goto done;
      if (++optind < argc) ring1 = argv[optind];
      if (++optind < argc) ring2 = argv[optind];
      if ((ring1 == NULL) || (ring2 == NULL)) goto done;
      o1 = shr_open(ring1, SHR_WRONLY);
      o2 = shr_open(ring2, SHR_WRONLY);
      if ((o1 == NULL) || (o2 == NULL)) goto done;
      while(1) {
        nr = shr_read(CF.shr, buf, sizeof(buf));
        if (nr <= 0) {
          fprintf(stderr, "shr_read: %s (%ld)\n", 
             (nr == 0) ? "end-of-data" : "error", nr); 
          goto done;
        } else {
          if (shr_write(o1, buf, nr) < 0) {
            fprintf(stderr, "shr_write: error\n");
            goto done;
          }
          if (shr_write(o2, buf, nr) < 0) {
            fprintf(stderr, "shr_write: error\n");
            goto done;
          }
        }
      }
      break;

    case mode_create:
      if (CF.size == 0) usage();
      while (optind < argc) {
        rc = shr_init(argv[optind++], CF.size, CF.flags);
        if (rc < 0) goto done;
      }
      break;

    case mode_status:
      CF.shr = shr_open(CF.ring, SHR_RDONLY | SHR_GET_APPDATA, 
                        &app_data, &app_len);
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
      if (app_len) printf(" app-data: %lu bytes\n", (unsigned long)app_len);
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
      while (fgets(line, sizeof(line), stdin)) {
        nr = strlen(line);
        if ((nr > 0) && (line[nr-1] == '\n')) line[nr--] = '\0';
        if (shr_write(CF.shr, line, nr) < 0) {
          fprintf(stderr, "shr_write: error\n");
          goto done;
        }
      }
      break;

    default: 
      assert(0);
      break;
  }

  rc = 0;
 
 done:
  if (CF.shr) shr_close(CF.shr);
  if (o1) shr_close(o1);
  if (o2) shr_close(o2);
  return rc;
}
