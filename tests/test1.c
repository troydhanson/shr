#include <stdio.h>
#include <unistd.h>
#include "shr.h"

char *ring = "/dev/shm/" __FILE__ ".ring";

int main() {
  setlinebuf(stdout);
 int rc = -1;

 unlink(ring);
 if (shr_init(ring, 1024, 0) < 0) goto done;

 rc = 0;

done:
 unlink(ring);
 return rc;
}
