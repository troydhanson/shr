#include <stdio.h>
#include <unistd.h>
#include "shr.h"

char *data = "abcdefghi";
char *ring =  __FILE__ ".ring";

char out[10];

int main() {
  setlinebuf(stdout);
 struct shr *s=NULL,*t=NULL;
 ssize_t nr;
 int rc = -1;

 unlink(ring);
 if (shr_init(ring, 14, 0) < 0) goto done;

 s = shr_open(ring, SHR_RDONLY);
 if (s == NULL) goto done;

 t = shr_open(ring, SHR_WRONLY);
 if (t == NULL) goto done;

 printf("writing ...");
 nr = shr_write(t, &data[0], 3);
 if (nr < 0) goto done;
 printf("ok\n");

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
