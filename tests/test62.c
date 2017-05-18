#include <stdio.h>
#include <unistd.h>
#include "shr.h"

char *ring = __FILE__ ".ring";

const char app[] = "abcdefghijklmnopqrstuvwxyz";

int main() {
 struct shr *s=NULL;
 int rc = -1;

 unlink(ring);
 if (shr_init(ring, 8, SHR_APPDATA, app, sizeof(app)) < 0) goto done;

 char *buf=NULL;
 size_t len=0;

 s = shr_open(ring, SHR_RDONLY | SHR_GET_APPDATA, &buf, &len);
 if (s == NULL) goto done;

 if (buf) printf("%.*s\n", (int)len, buf);

 rc = 0;

done:
 if (s) shr_close(s);
 return rc;
}
