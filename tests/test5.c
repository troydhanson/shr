#include <stdio.h>
#include <unistd.h>
#include "shr.h"

char *ring =  __FILE__ ".ring";
char *data = "abcdefghi";

char out[10];

int main() {
  setlinebuf(stdout);
 struct shr *s=NULL,*t=NULL;
 int rc = -1;

 unlink(ring);
 if (shr_init(ring, 14, 0) < 0) goto done;

 s = shr_open(ring, SHR_RDONLY|SHR_NONBLOCK);
 if (s == NULL) goto done;

 t = shr_open(ring, SHR_WRONLY);
 if (t == NULL) goto done;

 printf("writing ...");
 if (shr_write(t, &data[0], 3) < 0) goto done;
 printf("ok\n");

 printf("reading ...");
 ssize_t nr;
 nr = shr_read(s, out, sizeof(out));
 if (nr < 0) goto done;
 printf("read %ld bytes\n", (long)nr);
 if (nr > 0) printf("%.*s\n", (int)nr, out);

 printf("reading ...");
 nr = shr_read(s, out, sizeof(out));
 if (nr < 0) goto done;
 printf("read %ld bytes\n", (long)nr);
 if (nr > 0) printf("%.*s\n", (int)nr, out);

 rc = 0;

done:
 printf("end\n");
 if (s) shr_close(s);
 if (t) shr_close(t);
 unlink(ring);
 return rc;
}
