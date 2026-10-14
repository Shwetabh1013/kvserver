# kvserver

A multithreaded, in-memory key-value server written in C11 with POSIX threads and BSD sockets. Clients connect over TCP and send simple text commands (`SET`, `GET`, `DEL`, ...), similar in spirit to a tiny Redis.

## Architecture

```
               +-------------------+
 TCP clients ->|  accept() thread  |  main thread
               +---------+---------+
                         | push client fd
                         v
               +-------------------+
               | bounded fd queue  |  mutex + 2 condition variables
               |  (producer/consumer)
               +---------+---------+
                         | pop
        +----------------+----------------+
        v                v                v
   worker 1          worker 2   ...   worker N     fixed thread pool
        |                |                |
        +----------------+----------------+
                         v
               +-------------------+
               | lock-striped hash |  64 rwlocks guard 65,536 buckets
               |      table        |
               +-------------------+
```

- **Thread pool + bounded queue.** The main thread only accepts connections; a fixed pool of worker threads serves them. The queue blocks the producer when full (back-pressure) and blocks workers when empty, using `pthread_cond_wait` in a `while` loop to handle spurious wakeups.
- **Lock striping.** Instead of one global lock, buckets are guarded by 64 reader-writer locks (`bucket % 64`). Reads on the same stripe run in parallel, and writes to different stripes never contend.
- **Short critical sections.** Values are allocated before taking the lock and freed after releasing it.
- **Robust stream parsing.** TCP is a byte stream, so the server buffers partial reads, handles several pipelined commands in one `recv()`, accepts `\r\n` line endings and rejects lines over 4 KB.
- **Graceful shutdown.** `SIGINT`/`SIGTERM` are blocked in workers and handled only by the main thread. The handler sets an atomic flag, `accept()` returns `EINTR`, the queue is closed, workers notice via a 1 s `SO_RCVTIMEO`, and every thread is joined and all memory freed.
- **`SIGPIPE` ignored** so a client disconnecting mid-reply cannot kill the server.

## Build and run

Requires a C11 compiler and POSIX threads (Linux or macOS).

```sh
make                       # builds build/kvserver and build/bench
./build/kvserver -p 7070 -t 8
```

Try it with netcat:

```sh
$ nc localhost 7070
SET city Jaipur
+OK
GET city
$6 Jaipur
COUNT
:1
QUIT
+BYE
```

## Protocol

| Command | Reply |
| --- | --- |
| `PING` | `+PONG` |
| `SET <key> <value>` | `+OK` (value may contain spaces) |
| `GET <key>` | `$<len> <value>` or `$-1` if missing |
| `DEL <key>` | `:1` if removed, `:0` if absent |
| `COUNT` | `:<number of keys>` |
| `QUIT` | `+BYE`, then the connection closes |
| invalid input | `-ERR <message>` |

`GET` replies carry the value's length so a value such as `-1` is never ambiguous.

## Testing

```sh
make test    # unit tests (C) + integration tests against the real binary (Python)
make asan    # same tests under AddressSanitizer + UndefinedBehaviorSanitizer
make tsan    # same tests under ThreadSanitizer (data-race detection)
```

- **Unit tests** cover set/get/overwrite/delete, 1,000 keys forced into a single collision chain, and 8 threads doing 20,000 concurrent operations each with a consistency check on the final count.
- **Integration tests** cover every command, error handling, pipelined commands, a command split across two TCP segments, data shared across connections, and clean shutdown on `SIGINT`.

## Benchmark

`bench` spawns client threads, each holding one TCP connection and alternating `SET`/`GET`. It verifies every reply and reports throughput and p50/p95/p99 latency.

```sh
make bench
# or: ./build/bench 127.0.0.1 7070 <clients> <requests_per_client>
```

Results on a 2-vCPU Linux VM, 8 worker threads, client and server on the same machine:

| Clients | Requests | Errors | Throughput | p50 | p95 | p99 |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | 20,000 | 0 | 20,262 req/s | 43.9 µs | 73.5 µs | 108.3 µs |
| 8 | 160,000 | 0 | 153,078 req/s | 36.0 µs | 112.3 µs | 173.9 µs |
| 32 | 640,000 | 0 | 158,132 req/s | 38.6 µs | 123.6 µs | 178.5 µs |

Throughput plateaus between 8 and 32 clients because each worker serves one connection at a time, so with 8 workers the extra connections wait in the queue.

## Limitations and next steps

- **One connection per worker.** Idle connections occupy a thread. The next step is an event loop (`epoll` on Linux, `kqueue` on macOS) so a few threads can serve thousands of connections.
- **Fixed bucket count.** The table does not resize; a resize would need all stripe locks or incremental rehashing.
- **In-memory only.** No persistence; an append-only log replayed at startup would add durability.

## Layout

```
src/server.c        accept loop, worker pool, protocol, signal handling
src/hashtable.[ch]  lock-striped hash table
src/queue.[ch]      bounded blocking fd queue
bench/bench.c       multithreaded load generator
tests/              C unit tests and Python integration tests
```
