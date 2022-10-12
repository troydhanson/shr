#include <stdio.h>
#include <unistd.h>
#include "shr.h"

char *ring =  __FILE__ ".ring";

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

 p = &od;
 ad.counter = 1;
 sc = shr_appdata(s, &p, &ad, &sz);
 if (sc < 0) goto done;
 printf("app_data: was %d\n", od.counter);
 printf("app_data: now %d\n", ad.counter);

 ad.counter = 2;
 sc = shr_appdata(s, &p, &ad, &sz);
 if (sc < 0) goto done;
 printf("app_data: was %d\n", od.counter);
 printf("app_data: now %d\n", ad.counter);

 rc = 0;

done:
 if (s) shr_close(s);
 unlink(ring);
 return rc;
}
