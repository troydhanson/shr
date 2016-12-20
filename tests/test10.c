#include <stdio.h>
#include <unistd.h>
#include "shr.h"

char *ring = __FILE__ ".ring";
char *data = "abcdefghi";

char out[10];

int main() {
 struct shr *s=NULL,*t=NULL;
 int rc = -1;
 ssize_t nr;

 unlink(ring);
 if (shr_init(ring, 6, 0) < 0) goto done;

 s = shr_open(ring, SHR_RDONLY|SHR_NONBLOCK);
 if (s == NULL) goto done;

 t = shr_open(ring, SHR_WRONLY);
 if (t == NULL) goto done;

 printf("writing ...");
 if ( (nr = shr_write(t, &data[0], 3)) < 0) goto done;
 printf("wrote %ld bytes\n", (long)nr);
 printf("ok\n");

 printf("writing ...");
 if ( (nr = shr_write(t, &data[3], 3)) < 0) goto done;
 printf("wrote %ld bytes\n", (long)nr);
 printf("ok\n");

 printf("reading ...");
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
 unlink(ring);
 printf("end\n");
 if (s) shr_close(s);
 if (t) shr_close(t);
 return rc;
}
