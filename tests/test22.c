#include <stdio.h>
#include <unistd.h>
#include "shr.h"

char *ring = "/dev/shm/" __FILE__ ".ring";

int main() {
  setlinebuf(stdout);
 struct shr *s=NULL;
 int rc = -1;

 unlink(ring);

 printf("init\n");
 if (shr_init(ring, 1024, 0) < 0) goto done;

 printf("re-init, overwrite\n");
 if (shr_init(ring, 1024, 0) < 0) goto done;

 printf("re-init, keep existing\n");
 if (shr_init(ring, 1024, SHR_KEEPEXIST) < 0) goto done;

 printf("ok\n");

 rc = 0;

done:
 if (s) shr_close(s);
 unlink(ring);
 return rc;
}
