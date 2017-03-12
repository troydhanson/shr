# `shr`: a multi-process ring buffer in C

Originally part of [fluxcap](https://github.com/troydhanson/fluxcap), the shr
library is now in its own repository since it is a generic buffering mechanism.

This is a ring buffer in C for Linux.  The elements of the ring can be bytes or
messages- an arbitrary binary buffer with a length.  In message mode, each read
or write is a full message, with its boundaries preserved (no partial messages).

It is designed for use by two or more communicating processes. The processes
can be concurrent, or they can run at separate times.  In other words, the ring
persists independently of the processes that use it. When concurrent processes
pass data through the ring, the data sharing occurs in memory.

When the ring is full, the arrival of new data overwrites old data in the ring
if that data has been read (or, even if it has not been read, in `SHR_DROP`
mode; otherwise, a write blocks until sufficient old data has been read).

In non-blocking mode, reads returns immediately if no data is available; and
writes return immediately if inadequate space exists in the ring. (The write 
would succeed in `SHR_DROP` mode by reclaiming space from unread data).
Otherwise the a non-blocking read or write returns 0 immediately if it would
normally have to block.

The ring itself is a memory-mapped file. Usually, it is placed in a `tmpfs`
filesystem like `/dev/shm` so it resides in RAM without a disk backing.  Since
the ring is a file, it has file persistence. A POSIX file lock protects the
ring so that only one process can put data in, or read data from it, at any
moment.

When one or more processes have the ring open, they map it into their memory.
So a reader and writer pass data through shared memory. The operating system
does lazy background flushing of the memory pages to the backing store, which
is why placing it in a `tmpfs` is preferable, unless you want disk persistence.

A reader can select or poll on data availability in the ring. The reader calls
`shr_get_selectable_fd` to get a file descriptor on which to poll. Internally,
it is tied to a fifo. The fifo is readable if there is data available, and is
not readable when the ring data is fully consumed. (The data itself does not
pass through the fifo. It is just to indicate I/O readiness in the ring).

Typically a ring is used with one reader and one writer. While having multiple
readers and multiple writers is ok, it's limited to `MAX_RWPROC` (16) of each.
All contend for the same lock so concurrency is limited.  Lastly, readers each
consume the data they read; the next read (in any reader) gets the next data.

The source is in `shr.h` and `shr.c`.

Compatibility: `shr` is written in C for Linux only. Whether it runs on other
POSIX platforms depends on whether they support opening a fifo in read/write
mode- see `fifo(7)`. 

Performance: no claims are made. On a laptop, rates on the order of a million
reads and writes per second seemed typical. Once a reader or writer has the
lock, it can transfer data at the memory bandwidth since I/O is `memcpy`.

## API

### Create 

Create a ring of a given size. 

    int shr_init(char *file, size_t sz, int flags, ...);

The ring should be created once, ahead of time. Do not call `shr_init` in each
process that uses the ring. Typically a setup program creates the ring before
any of the programs that need it. (You can also create the ring on the command
line using the `shr-tool` executable from the `util/` directory; run it with
`-h` to view its usage).

A bitwise OR made from the following flags can be passed as the third argument:

    SHR_OVERWRITE
    SHR_KEEPEXIST
    SHR_MESSAGES
    SHR_DROP

The first two mode flags control what happens if the ring file already exists.
`SHR_OVERWRITE` is destructive mode; it removes then recreates the file empty.
`SHR_KEEPEXIST` preserves the ring as-is. (It is even safe on a ring that is
open and in-use).  If neither flag is specified, `shr_init` returns an error
(a negative number) if the ring already exists. 

Message mode is enacted by setting `SHR_MESSAGES`. In this mode, read and
write operate on one message at a time. Otherwise, the ring elements are bytes.

In `SHR_DROP` mode, a write to a full ring automatically overwrites old ring
data *even if it is unread*. (Unread data is normally retained until it's read,
by blocking a writer if need be).

The file is actually created a tiny bit larger than the size given, for the
bookkeeping region at the front of the file.

### Open

A process has to open the ring before it can read or write data to it.

    shr *shr_open(char *file, int flags);

This returns an opaque ring handle. The flags is 0 or a bitwise combination of:

    SHR_RDONLY
    SHR_WRONLY
    SHR_NONBLOCK

A reader uses `SHR_RDONLY` and a writer uses `SHR_WRONLY`. These are mutually
exclusive.

The `SHR_NONBLOCK` mode causes subsequent `shr_read` or `shr_write` operations
to return immediately rather than block if they would need to wait for data or
space.

### Write data

A process (that has the ring open for writing) can put data in this way:

    ssize_t shr_write(shr *s, char *buf, size_t len);

If the ring was initialized in message mode, then the buffer and length are considered
to comprise a single message, whose boundaries should be preserved when eventually read.
In contrast, in the default (byte) mode, the data is considered a byte stream.

Return value:

     > 0 (number of bytes copied into ring, always the full buffer)
     0   (insufficient space in ring, in non-blocking mode)
    -1   (error, such as the buffer exceeds the total ring capacity)
    -2   (signal arrived while blocked waiting for ring)

Also see `shr_writev` in `shr.c` for the `iovec`-based write function.


### Read data

To read data from the ring, a process that has opened the ring for reading, can do:

    ssize_t shr_read(shr *s, char *buf, size_t len);

Return value:

     > 0 (number of bytes read from the ring)
     0   (ring empty, in non-blocking mode)
    -1   (error)
    -2   (signal arrived while blocked waiting for ring)
    -3   (buffer can't hold message; SHR_MESSAGES mode)

Also see `shr_readv` in `shr.c` for the `iovec`-based read function.

### Select/poll for data

A reader that has opened the ring in `SHR_RDONLY | SHR_NONBLOCK` mode can call

    int shr_get_selectable_fd(shr *s);

to get a file descriptor compatible with select or poll.  If the descriptor
becomes readable, the reader should call `shr_read`.  (It is not necessary
to read from the descriptor itself; this is taken care of internally).
Afterward, the descriptor is made readable again if unread data remains in
the ring.  It is OK to read only some of the available data, and rely on the
poll to re-trigger.

When the descriptor becomes readable, it is possible to have the subsequent
`shr_read` return zero indicating no data was available. (For example if one
process writes a byte to the ring, while two processes await data, both readers
wake up but only one gets the byte. The other reader gets a zero return from
`shr_read`). Therefore, to use `shr_get_selectable_fd` the reader needs to be
in `SHR_NONBLOCK` mode (so it gets a zero return instead of blocking in this
case).

This function returns -1 on error.

### Close

To close the ring, use:

    void shr_close(shr *s);

### Metrics

A set of numbers describing the ring in terms of total space, used space, and I/O counters,
can be obtained using this function. See the definition of `shr_stat` in `shr.h`.

    int shr_stat(shr *s, struct shr_stat *stat, struct timeval *reset);

This function can, optionally, reset the stats and start a new period, by
passing a non-NULL pointer in the final argument.

You can also view the metrics on the command line using `shr-tool` from the `util/` 
directory.
