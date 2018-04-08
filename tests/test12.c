#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "shr.h"

char *ring = "/dev/shm/" __FILE__ ".ring";

struct app_data {
  int counter;
} ad, od;

int main() {
  setlinebuf(stdout);
 struct shr *s = NULL;
 int rc = -1, sc;
 void *p;
 size_t sz = sizeof(ad);

 unlink(ring);

 sc = shr_init(ring, 8, SHR_APPDATA, &ad, sizeof(ad));
 if (sc < 0) goto done;

 s = shr_open(ring, SHR_RDONLY);
 if (s == NULL) goto done;

 ad.counter = 10;
 sc = shr_appdata(s, NULL, &ad, &sz);  /* no get, set to 10 */
 if (sc < 0) goto done;

 p = &od;
 sc = shr_appdata(s, &p, NULL, &sz);   /* get 10, no set */
 if (sc < 0) goto done;
 printf("app_data: now %d\n", ad.counter);

 ad.counter = 20;
 sc = shr_appdata(s, &p, &ad, &sz);  /* get 10, set to 20 */
 if (sc < 0) goto done;
 printf("app_data: was %d\n", od.counter);
 printf("app_data: now %d\n", ad.counter);

 p = NULL;
 sz = 0;
 sc = shr_appdata(s, &p, NULL, &sz); /* get 20 in mallocd struct */
 if (sc < 0) goto done;
 if (p == NULL) goto done;
 if (sizeof(struct app_data) != sz) goto done;
 printf("app_data: now %d\n", ((struct app_data*)p)->counter);
 free(p);

 rc = 0;

done:
 if (s) shr_close(s);
 unlink(ring);
 return rc;
}
