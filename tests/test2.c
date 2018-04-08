#include <stdio.h>
#include <unistd.h>
#include "shr.h"

char *ring = "/dev/shm/" __FILE__ ".ring";

int main() {
  setlinebuf(stdout);
 struct shr *s=NULL;
 int rc = -1;

 unlink(ring);
 if (shr_init(ring, 1024, 0) < 0) goto done;

 s = shr_open(ring, SHR_RDONLY);
 if (s == NULL) goto done;

 rc = 0;

done:
 if (s) shr_close(s);
 unlink(ring);
 return rc;
}
