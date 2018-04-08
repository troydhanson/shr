#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "shr.h"

char *ring = "/dev/shm/" __FILE__ ".ring";

int main() {
  setlinebuf(stdout);
 struct shr *s = NULL;
 int rc = -1, sc;
 ssize_t nr;
 struct shr_stat st;

 unlink(ring);

 sc = shr_init(ring, 5, 0);
 if (sc < 0) goto done;

 s = shr_open(ring, SHR_WRONLY | SHR_BUFFERED);
 if (s == NULL) goto done;

 nr = shr_write(s, "hello", 5);
 if (nr < 0) goto done;

 sc = shr_stat(s, &st, NULL);
 if (sc < 0) goto done;
 printf("ring size: %zu, bytes: %zu, messages: %zu\n", st.bn, st.bu, st.mu);
 printf("cache size: %zu, bytes: %zu, messages: %zu\n", st.cn, st.cb, st.cm);

 /* test if shr_close flushes the cache */
 shr_close(s);
 s = NULL;
 s = shr_open(ring, SHR_WRONLY | SHR_BUFFERED);
 if (s == NULL) goto done;
 sc = shr_stat(s, &st, NULL);
 if (sc < 0) goto done;
 printf("ring size: %zu, bytes: %zu, messages: %zu\n", st.bn, st.bu, st.mu);
 printf("cache size: %zu, bytes: %zu, messages: %zu\n", st.cn, st.cb, st.cm);

 rc = 0;

done:
 if (s) shr_close(s);
 unlink(ring);
 return rc;
}
