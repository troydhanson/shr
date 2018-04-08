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

 /* the thing that remained in the cache -- "world"
  * will be lost on shr_flush because the ring is in 
  * non blocking mode, and nothing liberated space */
 nr = shr_flush(s,0);
 printf("shr_flush: %zu\n", nr);

 rc = 0;

done:
 if (s) shr_close(s);
 unlink(ring);
 return rc;
}
