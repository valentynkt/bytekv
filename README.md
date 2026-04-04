# bytekv

Networked in-memory key-value server written in C from scratch. Single-threaded, event-loop-driven, no dependencies. About 1,100 lines total.

Built in Neovim with zero AI assistance, because in 2026 the most contrarian thing an engineer can do is write code themselves.

## Demo

```
bytekv> SET mood rebellious
+OK
bytekv> SETEX hype 5 "ai-will-replace-programmers"
+OK
bytekv> GET hype
+"ai-will-replace-programmers"
  ... 5 seconds later ...
bytekv> GET hype
-ERR key not found
```

**Commands:** `GET`, `SET`, `DEL`, `SETEX`, `EXPIRE`, `TTL`, `PERSIST`, `KEYS`, `INFO`

## Architecture

```
Client (Python CLI / netcat)
        | TCP :9999
        v
+---- Networking Layer -------------------------+
|  kqueue event loop, non-blocking I/O          |
|  per-client read/write buffers                |
|  length-prefixed binary framing (4B header)   |
+-------------------+---------------------------+
                    |
            command_execute()
                    |
+-------------------v---------------------------+
|  Storage Layer                                |
|  hash table (djb2, separate chaining)         |
|  dual keyspace: values + expiry timestamps    |
|  lazy expiration, monotonic clock             |
+-----------------------------------------------+
```

Networking knows nothing about storage. Storage knows nothing about TCP. The command dispatcher is the only seam.

## What's inside

| File           | Lines | What it does                                                        |
| -------------- | ----: | ------------------------------------------------------------------- |
| `event_loop.c` |   109 | kqueue event loop                                                   |
| `server.c`     |   272 | connections, buffers, framing, fast-path write optimization         |
| `command.c`    |   298 | command table, tokenizer, arity checking, dispatch                  |
| `ht.c`         |   211 | hash table from scratch (djb2, resize at 0.75, tagged union values) |
| `db.c`         |    80 | dual hash table storage, lazy expiration on read                    |
| `util.c`       |    78 | socket setup, `CLOCK_MONOTONIC` time                                |
| `main.c`       |     5 | calls `run_networking()`                                            |

## Build & Run

```bash
make
./bytekv

# in another terminal
python3 client.py
```

Compiles with `-Wall -Wextra -Werror -fsanitize=undefined`.

## What's left

- [ ] AOF persistence (append-only file, `fdatasync`, mmap, CRC32 for torn-write detection)
- [ ] AOF compaction (rewrite live keys, atomic `rename()` swap)
- [ ] Active expiration sweep

---

_gcc, man pages, Neovim, stubbornness._
