#include <stdio.h>
#include <unistd.h>
#include "shr.h"

char *ring = __FILE__ ".ring";

const char app[] = "abcdefghijklmnopqrstuvwxyz";

int main() {
 int rc = -1;

 unlink(ring);
 if (shr_init(ring, 8, SHR_APPDATA, app, sizeof(app)) < 0) goto done;

 rc = 0;

done:
 return rc;
}
