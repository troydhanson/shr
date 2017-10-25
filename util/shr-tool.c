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
 * shr tool
 *
 */

struct {
  char *prog;
  int verbose;
  char *ring;
  enum {mode_status,
        mode_create,
        mode_write,
        mode_write_hex,
        mode_read,
        mode_read_hex,
        mode_tee} mode;
  struct shr *shr;
  size_t size;
  int flags;
  int block;
  char *hex_arg;
} CF = {
  .flags = SHR_KEEPEXIST | SHR_MESSAGES | SHR_DROP,
};

void usage() {
  fprintf(stderr,"usage: %s <mode-options>\n"
                 "\n"
                 "query mode - show ring metrics (default)\n"
                 "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~\n"
                 "  -q  <ring>\n"
                 "\n"
                 "read mode - display ring data\n"
                 "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~\n"
                 "  -r  <ring>   (display ring data on stdout)\n"
                 "  -rr <ring>   (dump ring data in hex on stderr)\n"
                 "  -b1          (block awaiting incoming data)\n"
                 "\n"
                 "write mode - put data in ring\n"
                 "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~\n"
                 "  -w <ring>       (from lines of stdin until eof)\n"
                 "  -W <hex> <ring> (from given hexascii argument)\n"
                 "\n"
                 "tee mode - tee one ring to two rings\n"
                 "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~\n"
                 "  -t <ring> <out1> <out2>\n"
                 "\n"
                 "create mode -  create ring of given size and mode\n"
                 "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~\n"
                 "  -c -s <size> <ring> [<ring>]\n"
                 "\n"
                 "  <size> with kmgt suffix (kb,mb,gb,tb)\n"
                 "\n"
                 "  -m <mode> (mode bits for ring creation; default: mdk)\n"
                 "     m  messages      (each read/write comprises a message)\n"
                 "     d  auto-drop     (unread data drops when space needed)\n"
                 "     k  keep existing (an existing ring is left untouched)\n"
                 "     o  overwrite     (an existing ring is overwritten)\n"
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

/* unhexer, overwrites input space;
 * returns number of bytes or -1 */
int unhex(char *h) {
  char b;
  int rc = -1;
  unsigned u;
  size_t i, len = strlen(h);

  if (len == 0) goto done;
  if (len &  1) goto done; /* odd number of digits */
  for(i=0; i < len; i += 2) {
    if (sscanf( &h[i], "%2x", &u) < 1) goto done;
    assert(u <= 255);
    b = (unsigned char)u;
    h[i/2] = b;
  }

  rc = 0;

 done:
  if (rc < 0) {
    fprintf(stderr, "hex conversion failed\n");
    return -1;
  }

  return len/2;
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

  while ( (opt = getopt(argc,argv,"vhcs:qm:wW:r+tb:")) > 0) {
    switch(opt) {
      case 'v': CF.verbose++; break;
      case 'h': default: usage(); break;
      case 'b': CF.block = atoi(optarg); break;
      case 'c': CF.mode=mode_create; break;
      case 'q': CF.mode=mode_status; break;
      case 'w': CF.mode=mode_write; break;
      case 'W': CF.mode=mode_write_hex; CF.hex_arg = strdup(optarg); break;
      case 'r': CF.mode = (CF.mode==mode_read) ? mode_read_hex : mode_read; break;
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
      CF.shr = shr_open(CF.ring, SHR_RDONLY );
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
    case mode_read_hex:
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
          if (CF.verbose) fprintf(stderr, "read %ld bytes\n", nr);
          if (CF.mode == mode_read_hex) hexdump(buf,nr);
          else { printf("%.*s\n", (int)nr, buf); fflush(stdout); }
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

    case mode_write_hex:
      CF.shr = shr_open(CF.ring, SHR_WRONLY);
      if (CF.shr == NULL) goto done;
      nr = unhex(CF.hex_arg);
      if (nr <= 0) goto done;
      if (shr_write(CF.shr, CF.hex_arg, nr) < 0) {
        fprintf(stderr, "shr_write: error\n");
        goto done;
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
  if (CF.hex_arg) free(CF.hex_arg);
  return rc;
}
