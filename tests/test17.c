#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "shr.h"

char *ring =  __FILE__ ".ring";

int main() {
  setlinebuf(stdout);
 struct shr *s = NULL, *z=NULL;
 int rc = -1, sc;
 ssize_t nr;
 struct shr_stat st;

 unlink(ring);

 sc = shr_init(ring, 5, 0);
 if (sc < 0) goto done;

 s = shr_open(ring, SHR_WRONLY | SHR_BUFFERED | SHR_NONBLOCK);
 if (s == NULL) goto done;

 nr = shr_write(s, "hello", 5);
 if (nr < 0) goto done;
 sc = shr_stat(s, &st, NULL);
 if (sc < 0) goto done;
 printf("ring size: %zu, bytes: %zu, messages: %zu\n", st.bn, st.bu, st.mu);
 printf("cache size: %zu, bytes: %zu, messages: %zu\n", st.cn, st.cb, st.cm);

 nr = shr_write(s, "world", 5); /* now this is in the cache */
 if (nr < 0) goto done;
 sc = shr_stat(s, &st, NULL);
 if (sc < 0) goto done;
 printf("ring size: %zu, bytes: %zu, messages: %zu\n", st.bn, st.bu, st.mu);
 printf("cache size: %zu, bytes: %zu, messages: %zu\n", st.cn, st.cb, st.cm);

 nr = shr_write(s, "there", 5); /* this fails non-blocking write lacks space */
 if (nr < 0) goto done;
 if (nr == 0) printf("non-blocking write: would block\n");
 sc = shr_stat(s, &st, NULL);
 if (sc < 0) goto done;
 printf("ring size: %zu, bytes: %zu, messages: %zu\n", st.bn, st.bu, st.mu);
 printf("cache size: %zu, bytes: %zu, messages: %zu\n", st.cn, st.cb, st.cm);

 /* liberate space in the ring */
 char buf[5];
 z = shr_open(ring, SHR_RDONLY);
 if (z == NULL) goto done;
 nr = shr_read(z, buf, sizeof(buf));
 if (nr < 0) goto done;
 if (nr != 5) printf("read: %zu bytes\n", nr);

 nr = shr_flush(s,0); /* puts "world" in the ring; "there" never was cached */
 printf("shr_flush: %zu\n", nr);

 rc = 0;

done:
 if (s) shr_close(s);
 if (z) shr_close(z);
 unlink(ring);
 return rc;
}
