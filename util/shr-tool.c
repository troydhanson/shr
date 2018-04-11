#include <sys/mount.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <limits.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
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
        mode_mount,
        mode_write,
        mode_write_hex,
        mode_write_file,
        mode_read,
        mode_read_hex,
        mode_tee} mode;
  struct shr *shr;
  size_t size;
  size_t max_msgs;
  int flags;
  int block;
  char *hex_arg;
  char *wfile;
  char *wfile_buf;
  size_t wfile_len;
  char *mountpoint;
  char *fs_type;
  enum {mnt_mount,
        mnt_make_subdirs,
        mnt_unmount,
        mnt_query} mount_mode;
  char parent[PATH_MAX];
  int nr_hugepages;
  char tmp[100];
} CF = {
  .fs_type = "ramfs",
};

void usage() {
  fprintf(stderr,"usage: %s <command> [options] ...\n"
                 "\n"
                 "command summary\n"
                 "---------------\n"
                 "status          get counters\n"
                 "create          create ring(s)\n"
                 "read            read frames- ascii\n"
                 "readhex         read frames- hex/ascii\n"
                 "write           frames from stdin lines\n"
                 "writehex HEX    frame from hex argument\n"
                 "writefile FILE  frame from file content\n"
                 "tee A B C       tee ring A to rings B C\n"
                 "mount DIR       mount/unmount ram disk\n"
                 "\n"
                 "read/readhex [options] RING\n"
                 "---------------------------\n"
                 "  -b            wait for data when exhausted\n"
                 "\n"
                 "create [options] RING ...\n"
                 "-------------------------\n"
                 "  -s size        size with kmgt suffix\n"
                 "  -A file        copy file into app-data\n"
                 "  -N maxmsgs     set max number of messages\n"
                 "  -m dfkshl      flags (combinable, default: 0)\n"
                 "      d          drop unread frames when full\n"
                 "      f          farm of independent readers\n"
                 "      k          keep ring as-is if it exists\n"
                 "      l          lock into memory when opened\n"
                 "      s          sync after each i/o\n"
                 "\n"
                 "status [options] RING\n"
                 "---------------------\n"
                 "  -A file        copy app-data to file\n"
                 "\n"
                 "mount [options] DIR\n"
                 "-------------------\n"
                 "  -d             create subdirs e.g. DIR SUB ...\n"
                 "  -s size        size with kmgt suffix [tmpfs]\n"
                 "  -q             query presence and utilization\n"
                 "  -u             unmount\n"
                 "  -t TYPE        ramdisk type\n"
                 "     tmpfs       swappable, size-limited\n"
                 "     ramfs       unswappable, size-unlimited\n"
                 "     hugetlbfs   unswappable, hugepage-backed\n"
                 "  -H num         set /proc/sys/vm/nr_hugepages\n"
                 "                 num = size-needed/huge-pagesize\n"
                 "\n"
                 "\n", CF.prog);
  exit(-1);
}

void hexdump(char *buf, size_t len) {
  size_t i,n=0;
  unsigned char c;
  while(n < len) {
    fprintf(stdout,"%08x ", (int)n);
    for(i=0; i < 16; i++) {
      c = (n+i < len) ? buf[n+i] : 0;
      if (n+i < len) fprintf(stdout,"%.2x ", c);
      else fprintf(stdout, "   ");
    }
    for(i=0; i < 16; i++) {
      c = (n+i < len) ? buf[n+i] : ' ';
      if (c < 0x20 || c > 0x7e) c = '.';
      fprintf(stdout,"%c",c);
    }
    fprintf(stdout,"\n");
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

#define TMPFS_MAGIC  0x01021994
#define RAMFS_MAGIC  0x858458F6
#define HUGEFS_MAGIC 0x958458F6
/*
 * test if the directory exists, prior to mounting a tmpfs/ramfs on it.
 * also report if its already a ramdisk mount to prevent stacking.
 *
 * returns
 *   0 suitable directory (exists, not already a ramdisk mount)
 *  -1 error
 *  -2 directory is already a ramdisk mountpoint
 *
 */
int suitable_mountpoint(char *dir, struct stat *sb, struct statfs *sf) {
  int sc, is_mountpoint, is_tmpfs, is_ramfs, is_hugefs;
  size_t dlen = strlen(dir);
  struct stat psb;

  if (dlen+4 > PATH_MAX) {
    fprintf(stderr, "path too long\n");
    return -1;
  }

  /* does mount point exist? */
  sc = stat(dir, sb);
  if (sc < 0) {
    fprintf(stderr, "no mount point %s: %s\n", dir, strerror(errno));
    return -1;
  }

  /* has to be a directory */
  if (S_ISDIR(sb->st_mode) == 0) {
    fprintf(stderr, "mount point %s: not a directory\n", dir);
    return -1;
  }

  /* is dir already a mountpoint? */
  memcpy(CF.parent,dir,dlen+1);
  strcat(CF.parent,"/..");
  sc = stat(CF.parent, &psb);
  if (sc < 0) {
    fprintf(stderr, "stat %s: %s\n", CF.parent, strerror(errno));
    return -1;
  }
  is_mountpoint = (psb.st_dev == sb->st_dev) ? 0 : 1;

  /* is dir already a tmpfs/ramfs? */
  sc = statfs(dir, sf);
  if (sc < 0) {
    fprintf(stderr, "statfs %s: %s\n", dir, strerror(errno));
    return -1;
  }
  is_tmpfs  = (sf->f_type == TMPFS_MAGIC);
  is_ramfs  = (sf->f_type == RAMFS_MAGIC);
  is_hugefs = (sf->f_type == HUGEFS_MAGIC);

  if (is_mountpoint && (is_tmpfs || is_ramfs || is_hugefs)) {
    return -2;
  }

  return 0;
}

#define KB 1024L
#define MB (1024*1024L)
#define GB (1024*1024*1024L)
int query_ramdisk(char *ramdisk) {
  struct stat sb;
  struct statfs sf;

  if (suitable_mountpoint(ramdisk, &sb, &sf) != -2) {
    printf("%s: not a ramdisk\n", ramdisk);
    return -1;
  }

  if (sf.f_type == RAMFS_MAGIC) {
    printf("%s: ramfs (unbounded size)\n", ramdisk);
    return 0;
  }

  if (sf.f_type == HUGEFS_MAGIC) {
    printf("%s: hugetlbfs (unbounded size)\n", ramdisk);
    return 0;
  }

  char szb[100];
  long bytes = sf.f_bsize*sf.f_blocks;
  if (bytes < KB) snprintf(szb, sizeof(szb), "%ld bytes", bytes);
  else if (bytes < MB) snprintf(szb, sizeof(szb), "%ld kb", bytes/KB);
  else if (bytes < GB) snprintf(szb, sizeof(szb), "%ld mb", bytes/MB);
  else                 snprintf(szb, sizeof(szb), "%ld gb", bytes/GB);
  int used_pct = 100 - (sf.f_bfree * 100.0 / sf.f_blocks);
  printf("%s: tmpfs of size %s (%d%% used)\n", ramdisk, szb, used_pct);
  return 0;
}

/* mmap file, placing its size in len and returning address or NULL on error.
 * caller should munmap the buffer eventually.
 */
char *map(char *file, size_t *len) {
  int fd = -1, rc = -1, sc;
  char *buf = NULL;
  struct stat s;

  fd = open(file, O_RDONLY);
  if (fd < 0) {
    fprintf(stderr,"open: %s\n", strerror(errno));
    goto done;
  }

  sc = fstat(fd, &s);
  if (sc < 0) {
    fprintf(stderr,"fstat: %s\n", strerror(errno));
    goto done;
  }

  if (s.st_size == 0) {
    fprintf(stderr,"error: mmap zero size file\n");
    goto done;
  }

  buf = mmap(0, s.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  if (buf == MAP_FAILED) {
    fprintf(stderr, "mmap: %s\n", strerror(errno));
    buf = NULL;
    goto done;
  }

  rc = 0;
  *len = s.st_size;

 done:
  if (fd != -1) close(fd);
  if (rc && buf) { munmap(buf, s.st_size); buf = NULL; }
  return buf;
}

char buf[1000000]; /* scratch space */
int main(int argc, char *argv[]) {
  char unit, *c, *app_data, *cmd, line[1000], opts[100],
    *ring1=NULL, *ring2=NULL, *data, *sub;
  struct shr *o1=NULL, *o2=NULL;
  int opt, rc=-1, sc, mode, fd;
  struct shr_stat stat;
  size_t app_len = 0;
  CF.prog = argv[0];
  struct statfs sf;
  struct stat sb;
  ssize_t nr;

  if (argc < 3) usage();
 
  cmd = argv[1];
  if      (!strcmp(cmd, "status"))    CF.mode = mode_status;
  else if (!strcmp(cmd, "create"))    CF.mode = mode_create;
  else if (!strcmp(cmd, "mount"))     CF.mode = mode_mount;
  else if (!strcmp(cmd, "read"))      CF.mode = mode_read;
  else if (!strcmp(cmd, "readhex"))   CF.mode = mode_read_hex;
  else if (!strcmp(cmd, "write"))     CF.mode = mode_write;
  else if (!strcmp(cmd, "writefile")) CF.mode = mode_write_file;
  else if (!strcmp(cmd, "writehex"))  CF.mode = mode_write_hex;
  else if (!strcmp(cmd, "tee"))       CF.mode = mode_tee;
  else /* "help" or anything else */  usage();

  argv++;
  argc--;

  if (CF.mode == mode_write_hex) {
      if (argc < 3) usage();
      CF.hex_arg = strdup(argv[1]);
      argv++;
      argc--;
  }

  if (CF.mode == mode_write_file) {
      if (argc < 3) usage();
      CF.wfile = strdup(argv[1]);
      argv++;
      argc--;
  }

  while ( (opt = getopt(argc,argv,"vbs:m:A:N:t:uqdH:")) > 0) {
    switch(opt) {
      default : usage(); break;
      case 'v': CF.verbose++; break;
      case 'b': CF.block = 1; break;
      case 't': CF.fs_type = strdup(optarg); break;
      case 'd': CF.mount_mode = mnt_make_subdirs; break;
      case 'u': CF.mount_mode = mnt_unmount; break;
      case 'q': CF.mount_mode = mnt_query; break;
      case 'H': CF.nr_hugepages = atoi(optarg);
      case 'N': CF.flags |= SHR_MAXMSGS_2;
                CF.max_msgs = atoi(optarg);
                break;
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
         c = optarg;
         while((*c) != '\0') {
           switch (*c) {
             case 'd': CF.flags |= SHR_DROP; break;
             case 'k': CF.flags |= SHR_KEEPEXIST; break;
             case 'f': CF.flags |= SHR_FARM; break;
             case 's': CF.flags |= SHR_SYNC; break;
             case 'l': CF.flags |= SHR_MLOCK; break;
             default: usage(); break;
           }
           c++;
         }
         break;
      case 'A': CF.wfile = strdup(optarg); break;
    }
  }

  if (optind >= argc) usage();
  CF.ring = argv[optind];
  
  switch(CF.mode) {

    case mode_mount:
      CF.mountpoint = argv[optind++];
      switch (CF.mount_mode) {
        case mnt_mount:
        case mnt_make_subdirs:
          if (suitable_mountpoint(CF.mountpoint, &sb, &sf) == -2) {
            printf("%s: already a ramdisk\n", CF.mountpoint);
            goto done;
          }
          snprintf(opts, sizeof(opts), "size=%zu", CF.size);
          data = CF.size ? opts : NULL;
          sc = mount("shr", CF.mountpoint, CF.fs_type, MS_NOATIME, data);
          if (sc < 0) {
            fprintf(stderr, "mount %s: %s\n", CF.mountpoint, strerror(errno));
            goto done;
          }
          sc = chdir(CF.mountpoint);
          if (sc < 0) {
            fprintf(stderr, "chdir %s: %s\n", CF.mountpoint, strerror(errno));
            goto done;
          }
          while ((CF.mount_mode == mnt_make_subdirs) && (optind < argc)) {
            sub = argv[optind++];
            sc = mkdir(sub, 0777);
            if (sc < 0) {
              fprintf(stderr, "mkdir %s: %s\n", sub, strerror(errno));
              goto done;
            }
          }
          if (CF.nr_hugepages) {
            snprintf(CF.tmp, sizeof(CF.tmp), "%d\n", CF.nr_hugepages);
            char *nrfile = "/proc/sys/vm/nr_hugepages";
            fd = open(nrfile, O_WRONLY);
            if (fd == -1) {
              fprintf(stderr, "open %s: %s\n", nrfile, strerror(errno));
              goto done;
            }
            nr = write(fd, CF.tmp, strlen(CF.tmp));
            if (nr < 0) {
              fprintf(stderr, "write %s: %s\n", nrfile, strerror(errno));
              goto done;
            }
            close(fd);
          }
          break;
        case mnt_unmount:
          sc = umount(CF.mountpoint);
          if (sc < 0) {
            fprintf(stderr, "umount: %s\n", strerror(errno));
            goto done;
          }
          break;
        case mnt_query:
          query_ramdisk(CF.mountpoint);
          break;
        default:
          fprintf(stderr, "unimplemented mode\n");
          goto done;
          break;
      }
      break;

    case mode_create:
      if (CF.size == 0) usage();
      if (CF.wfile) { 
        CF.flags |= SHR_APPDATA;
        CF.wfile_buf = map(CF.wfile, &CF.wfile_len);
        if (CF.wfile_buf == NULL) goto done;
        while (optind < argc) {
          rc = shr_init(argv[optind++], CF.size, CF.flags, 
                CF.wfile_buf, CF.wfile_len, CF.max_msgs);
          if (rc < 0) goto done;
        }
      } else {
        while (optind < argc) {
          rc = shr_init(argv[optind++], CF.size, CF.flags, CF.max_msgs);
          if (rc < 0) goto done;
        }
      }
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
             " max-messages %ld\n"
             " messages-ready %ld\n",
         stat.bw, stat.br, stat.bd, stat.mw, stat.mr, stat.md, stat.bn,
         stat.bu, stat.mm, stat.mu);

      printf(" attributes ");
      if (stat.flags == 0)          printf("none");
      if (stat.flags & SHR_DROP)    printf("drop ");
      if (stat.flags & SHR_APPDATA) printf("appdata ");
      if (stat.flags & SHR_FARM)    printf("farm ");
      if (stat.flags & SHR_MLOCK)   printf("mlock ");
      printf("\n");

      app_data = NULL;
      rc = shr_appdata(CF.shr, (void**)&app_data, NULL, &app_len);
      printf(" app-data %zu\n", app_len);
      if (rc == 0) {
        assert(app_data && app_len);
        if (CF.verbose) printf("%.*s\n", (int)app_len, app_data);
        if (CF.wfile) {
          int fd = open(CF.wfile, O_WRONLY|O_CREAT|O_TRUNC, 0644);
          if (fd < 0) {
            fprintf(stderr, "open %s: %s\n", CF.wfile, strerror(errno));
          } else { 
            nr = write(fd, app_data, app_len);
            if (nr < 0) {
              fprintf(stderr, "write: %s\n", strerror(errno));
              goto done;
            }
            close(fd);
          }
        }
        free(app_data);
      }
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

    case mode_write_file:
      CF.shr = shr_open(CF.ring, SHR_WRONLY);
      if (CF.shr == NULL) goto done;
      CF.wfile_buf = map(CF.wfile, &CF.wfile_len);
      if (CF.wfile_buf == NULL) goto done;
      if (shr_write(CF.shr, CF.wfile_buf, CF.wfile_len) < 0) {
        fprintf(stderr, "shr_write: error\n");
        goto done;
      }
      break;

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
  // don't free(CF.fs_type);, default is static string
  if (CF.wfile_buf) munmap(CF.wfile_buf, CF.wfile_len);
  if (CF.wfile) free(CF.wfile);
  return rc;
}

