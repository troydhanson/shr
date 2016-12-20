#include <stdio.h>
#include <unistd.h>
#include "shr.h"

/* this test shows that partial writes are rejected; always full or error */

char *ring = __FILE__ ".ring";
char *data = "abcdefghi";

char out[10];

int main() {
 struct shr *s=NULL,*t=NULL;
 ssize_t nr;
 int rc = -1;

 unlink(ring);
 if (shr_init(ring, 4, 0) < 0) goto done; /* only 4 capacity */

 s = shr_open(ring, SHR_RDONLY);
 if (s == NULL) goto done;

 t = shr_open(ring, SHR_WRONLY|SHR_NONBLOCK);
 if (t == NULL) goto done;

 printf("writing ...");
 nr = shr_write(t, &data[0], 3);    /* write 3 ok */
 printf("%s\n", (nr < 0) ? "fail" : ((nr == 0) ? "would block" : "ok"));

 printf("writing ...");
 nr = shr_write(t, &data[3], 3); /* fails- can't write 3 more */
 printf("%s\n", (nr < 0) ? "fail" : ((nr == 0) ? "would block" : "ok"));

 printf("writing ...");
 nr = shr_write(t, &data[3], 2); /* fails- can't write 2 more */
 printf("%s\n", (nr < 0) ? "fail" : ((nr == 0) ? "would block" : "ok"));

 printf("writing ...");
 nr = shr_write(t, &data[3], 1); /* ok - can write 1 more */
 printf("%s\n", (nr < 0) ? "fail" : ((nr == 0) ? "would block" : "ok"));

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
 return rc;
}
