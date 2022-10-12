#include <stdio.h>
#include <unistd.h>
#include "shr.h"

char *ring =  __FILE__ ".ring";
char *data = "abcdefghi";

int main() {
  setlinebuf(stdout);
 struct shr *s=NULL;
 int rc = -1, i;
 struct iovec io[3];
 for(i = 0; i < 3; i++) {
   io[i].iov_base = data; 
   io[i].iov_len = 3;
 }

 unlink(ring);
 if (shr_init(ring, 2*3, 0) < 0) goto done;

 s = shr_open(ring, SHR_WRONLY|SHR_NONBLOCK);
 if (s == NULL) goto done;

 printf("writing 2 iovec...");
 if (shr_writev(s, io, 2) < 0) goto done;
 printf("ok\n");

 /* this should fail */
 printf("writing 1 iovec...");
 int nr = shr_writev(s, io, 1);
 if (nr < 0) goto done;
 if (nr == 0) printf("non-blocking shr_write: would block\n");
 else printf("ok\n");

 rc = 0;

done:
 printf("end\n");
 if (s) shr_close(s);
 unlink(ring);
 return rc;
}
