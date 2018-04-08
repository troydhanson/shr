Back to the [shr Github page](http://github.com/troydhanson/shr).
Back to [my other projects](http://troydhanson.github.io/).

# `shr`: a multi-process ring buffer in C

The `shr` library is written in C for Linux only. It is MIT licensed.

This is a ring buffer in C for Linux.  The elements of the ring are messages.
A message is an arbitrary binary buffer with a length (given as a `size_t`, 
usually an 8-byte quantity). Each read or write is a full message, with its
boundary preserved. A write never accepts part of a message, nor does a read
ever return part of a message. It supports batched I/O (`shr_writev` and
`shr_readv`), or one by one (`shr_write` and `shr_read`).

This ring buffer is designed for use by two or more processes. The processes
can be concurrent, or they can run at separate times.  In other words, the ring
persists independently of the processes that use it. When concurrent processes
pass data through the ring, the data sharing occurs in memory. The ring is a
file that's mapped into memory when processes open it.  A POSIX file lock
protects the ring so that only one process can read or write it at any moment.

When the ring is full, newly-arriving data overwrites old, already-read data.
This may cause a blocking writer to wait for space to become available.
Space is made available as readers read data, making it eligible for overwrite.
However, if the ring was created in `SHR_DROP` mode, writes proceed regardless;
the ring discards old data, read or unread, to make room for new.

A writer and reader can also be opened in non-blocking mode.

* a non-blocking read returns 0 immediately if no new data is available.
* a non-blocking write returns 0 immediately if it requires more than the 
  available space (except in `SHR_DROP` mode, where the write proceeds by
  dropping unread data)

A reader can use select/poll/epoll to monitor availability of data in the ring.
To do so, it calls `shr_get_selectable_fd` to get a file descriptor to poll.
The descriptor becomes readable if data is available in the ring. When it is,
the reader calls `shr_read` or `shr_readv` to read it. To get the selectable
descriptor, the reader must be opened in non-blocking mode.

Multiple readers and writers can operate on one ring. It's limited to 64 of 
each (`BW_WAITMAX`). All contend for the ring lock so concurrency is limited.
Readers normally consume the data they read; multiple readers therefore read
distinct messages from the ring.  The opposite behavior is set using the
`SHR_FARM` ring creation flag; this means a farm of independent readers all
see each message. `SHR_FARM` also sets `SHR_DROP` on the ring; see `shr_init`.

Each process that opens a ring maps it into memory.  Data flow occurs via these
pages of shared memory. As described in "The Linux Programming Interface," by
Michael Kerrisk, p.1027:

> Since all processes with a shared mapping of the same file region share the
> same physical pages of memory, [this] is a method of (fast) IPC.

Yet, because the ring is a file, it must be created on a filesystem. Place the
ring file in a RAM filesystem unless persistence is needed.  Then, the periodic
flush of the memory pages to their backing store becomes a no-op.  It is vastly
preferable to place the ring in a RAM filesystem for this reason. But if disk
persistence is needed, the ring can be created in a regular filesystem.

Three RAM filesystems suitable for use with shr are tmpfs, ramfs and hugetlbfs.
You can use `shr-tool` to mount these or do it using the `mount` command.
however shr-tool will protect against accidentally stacking a RAM filesystem on
top of another one, and can make subdirectories inside the new mount for you.

Very briefly, 

 * ramfs is a RAM filesystem that consumes pages of physical memory
   without enforcing size limits.  It is unswappable. It is well behaved
   with shr, because shr rings have their full size allocated at creation.

 * Tmpfs is basically ramfs with the addition of swappability and size limits.
   It provides no particular benefit for over ramfs, shr. The swappability 
   is usually undesirable, therefore, it is recommended to create the ring using
   `SHR_MLOCK` so it remains locked into memory when any process uses it. 
    This also solves an issue where tmpfs when over 50% full seem to degrade.

 * hugetlbfs is an unswappable RAM filesystem using huge pages (2MB or larger).
   It gives the best performance, because TLB cache is used more effectively.
   However huge pages are a limited in quantity.  When `shr-tool` is used to
   mount a hugetlbfs, it has an option to try to raise the system huge page
   reservation.  Ulrich Drepper writes in https://lwn.net/Articles/253361/

> Still, huge pages are the way to go in situations where performance is a premium,
> resources are plenty, and cumbersome setup is not a big deterrent.

## Build & Install

You should have gcc, autoconf, automake, and libtool installed first.

    cd shr
    ./autogen.sh
    ./configure
    make
    sudo make install
    sudo ldconfig

Afterward, libshr.so can be found in `/usr/local/lib`, the shr.h header in
`/usr/local/include` and shr-tool in `/usr/local/bin`.

## API

### Create 

Create a ring of a given size. 

    int shr_init(char *file, size_t sz, int flags, ...);

The ring should be created once, ahead of time. Do not call `shr_init` in each
process that uses the ring. Typically a setup program creates the ring before
any of the programs that need it. (You can also create the ring on the command
line using the `shr-tool` executable from the `util/` directory; run it with
`-h` to view its usage).  The ring file is created a bit larger than the size
given, for the bookkeeping areas.

A bitwise OR made from the following flags can be passed as the third argument:

    SHR_KEEPEXIST
    SHR_DROP
    SHR_FARM
    SHR_SYNC
    SHR_APPDATA_1
    SHR_MAXMSGS_2
    SHR_MLOCK

The first mode flag controls what happens if the ring file already exists.
By default is gets overwritten; `SHR_KEEPEXIST` instead keeps the ring file
as-is. This flag is safe even if the ring might be open by another process.

In `SHR_DROP` mode, a write to a full ring automatically overwrites old ring
data *even if it is unread*. In the absence of this flag, such a write blocks.

With `SHR_FARM`, the ring is considered input to a farm of independent readers.
In the absence of this flag, readers consume the messages they read meaning
that each reader sees distinct messages. In `SHR_FARM` all readers see the 
same messages.  Each reader starts at the oldest message in the ring.  If a
reader exits and restarts, it starts reading back at the beginning again.
`SHR_FARM` automatically sets `SHR_DROP`. This means that writes on the ring
always succeed, even if data unread by some readers has to be overwritten.
A farm reader can use `shr_farm_stat` to see how many messages it has lost
since opening the ring.

When `SHR_APPDATA_1` is set, the caller should pass a `char*` and `size_t` as
trailing arguments to `shr_init`, specifying a buffer and its length to copy
into the ring buffer's "application data" area. This is opaque data stored
separately from the ring data, as a convenience to the caller. It may contain
any data. For example, the caller could store meta-data about the ring in it.
The caller can read it back or rewrite it later using `shr_appdata()`.

If `SHR_MAXMSGS_2` is set in the flags, the caller should add a `size_t`
argument specifying the maximum number of messages the ring should hold.
This forms a secondary limit (the first being the byte capacity of the ring)
on the ring content. If left unspecified it defaults to the ring byte size
divided by 100. The `_2` in the flag name indicates the order its arguments
should appear- in this case, after `SHR_APPDATA_1` arguments if present.

Use `SHR_SYNC` to induce an `msync` after each ring I/O operation. It is 
only intended for greater robustness in the event of power failure on a
disk-backed shr ring. On a RAM-backed ring, it has no benefit.

Use `SHR_MLOCK` to cause processes that open the ring to lock it into memory.
This makes it unswappable, for as long as any process has it open.

### Open

A process has to open the ring before it can read or write data to it.

    shr *shr_open(char *file, int flags);

This returns an opaque ring handle. The flags is 0 or a bitwise combination of:

    SHR_RDONLY
    SHR_WRONLY
    SHR_NONBLOCK
    SHR_BUFFERED

A reader uses `SHR_RDONLY` and a writer uses `SHR_WRONLY`. These are mutually
exclusive.

The `SHR_NONBLOCK` mode causes subsequent `shr_read` or `shr_write` operations
to return immediately rather than block if they would need to wait for data or
space.

The `SHR_BUFFERED` flag is designed for use with writers only. If this flag
is set, writes will be buffered internally until the internal buffer fills or
until `shr_flush()` is called. It is designed to reduce acquisition of the 
lock, so that batched output is performed.  (The buffer is normally 10% of the
ring size or 10,000 messages; these values are clamped and may change).

The shr handle is not designed for use across a fork.

### Write data

A process that has opened the ring for writing can put data in this way:

    ssize_t shr_write(shr *s, char *buf, size_t len);

The buffer and length are considered to comprise a single message, whose
boundaries should be preserved when read later.

There is also `shr_writev`, an `iovec`-based bulk write function.

    ssize_t shr_writev(shr *s, struct iovec *iov, size_t iovcnt);

If there is sufficient space in the ring, it copies all the messages
(one per struct iovec) into the ring; otherwise it waits for space,
or returns 0 immediately in non-blocking mode. (Or, it drops unread
data from the ring, if the ring was created in `SHR_DROP` mode). It
is "all or nothing"- it writes all the messages, or none of them.

See shr.c for return values.

### Read data

A process that has opened the ring for reading can read a message this way:

    ssize_t shr_read(shr *s, char *buf, size_t len);

There is also `shr_readv`, an `iovec`-based bulk read function.

    ssize_t shr_readv(shr *s, char *buf, size_t len, struct iovec *iov, 
      size_t *iovcnt);

This function uses buf to hold message data, and populates the struct iovec
array so each one points to one message.  The caller provides the uninitialized
array of struct iovec, and this function fills them in. The iovcnt is an IN/OUT
parameter: on input it's the number of struct iov provided, and on output it's
how many this function filled in.
 
See shr.c for return values.

### Select/poll for data

A process that has opened the ring for reading in non-blocking mode can use:

    int shr_get_selectable_fd(shr *s);

to get a file descriptor compatible with select or poll.  The caller can
monitor the descriptor for readability in its own event loop. Whenever the
descriptor becomes readable, the reader should call `shr_read` or `shr_readv`.
The caller should not read on the selectable descriptor itself. After `shr_read`
the descriptor is made readable again if unread data remains in the ring, or
cleared if the ring data has all been read. It is okay for the caller to read
only some of the available messages; the descriptor then remains ready as long
as unread data remains.

When the descriptor becomes readable, the next `shr_read` or `shr_readv` may
return zero indicating no data was available. This is a spurious wakeup. The
shr library allows for the possibility of these. For example, if one process
writes a message to the ring while two processes wait, both readers wake up-
but only one may consume the message. The other reader gets a zero return.
To handle spurious wakeups, a reader needs to be open in `SHR_NONBLOCK` mode.
This prevents blocking in `shr_read` after a spurious wakeup.

This function returns -1 on error.

### Signals while waiting

If an application is blocked waiting for ring data, or blocked waiting for
free space to write data into the ring, and a signal occurs, what happens?

If an application leaves default signal handlers and mask in place, it may
simply terminate if a signal occurs while blocked in `shr_read`/`shr_write`.
For example, SIGHUP's default handler terminates the process.  Or, the signal
may be ignored. Or, if the application installed a handler, the internal read
may return an error (EINTR), or it may be restarted if `SA_RESTART` was used.

These complications can be avoided by using signalfd and sigprocmask.
First, the application uses sigprocmask to block all signals. Then it creates
a signalfd to be notified about signals of interest. The signalfd's descriptor
can be given to `shr_ctl`:

    int shr_ctl(shr *s, SHR_POLLFD, fd);

to cause `shr_write` or `shr_read`, and their writev and readv versions, to
return -3 if the signalfd becomes readable inside a blocking `shr_read`/write.

In this case, the application can read the signalfd as usual to get the signal
at an opportune time in its event loop.

### Close

To close the ring, use:

    void shr_close(shr *s);

### Metrics

A set of numbers describing the ring in terms of total space, used space, and I/O counters,
can be obtained using this function. See the definition of `shr_stat` in `shr.h`.

    int shr_stat(shr *s, struct shr_stat *stat, struct timeval *reset);

This function can, optionally, reset the counters and start a new metrics
period, by passing a non-NULL pointer in the final argument.

You can also view the metrics on the command line using `shr-tool` from the `util/` 
directory.

