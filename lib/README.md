Blocking / waking library

Processes can use this library to convey resource availability.

See rationale for why this library uses UNIX domain sockets in the
abstract namespace as its implementation.

1. Embed `bw_handle` in to data structure containing shared resource
2. Lock the shared resource in which `bw_handle` is embedded, before 
   calling `bw` functions, except those with ul ("unlocked") suffix.
3. Any number of processes can open the handle for waking others up.
4. Up to `BW_WAITMAX` processes can open the handle open for waiting.
5. A wakeup wakes up ALL the waiting processes
6. A waiting process can epoll to wait, on the descriptor from `bw_open`.
   Alternatively it can use the `bw_wait_ul` API call.
7. An awakened process can schedule immediate reawakening using `bw_force`.
8. An awakened process can discard remaining, pending wakeups using `bw_force`.
9. Trace can be enabled in `bw_open` flags. This logs "who wakes who" up.
10. Blocking in `bw_wait_ul` can monitor additional descriptors; see `bw_ctl`

