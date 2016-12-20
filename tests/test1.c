#include <stdio.h>
#include <unistd.h>
#include "shr.h"

char *ring = __FILE__ ".ring";

int main() {
 int rc = -1;

 unlink(ring);
 if (shr_init(ring, 1024, 0) < 0) goto done;

 rc = 0;

done:
 return rc;
}
