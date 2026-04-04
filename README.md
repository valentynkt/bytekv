# bytekv

**A networked in-memory key-value server, written in C, from scratch, in Neovim, with zero AI assistance.**

Because apparently in 2026, the most rebellious thing a software engineer can do is write code themselves.

---

Everyone around me was talking about the latest AI tools, prompt engineering tricks, which copilot generates the best boilerplate. I got so tired of it that I swung the pendulum the other way: picked the least AI-friendly language I could think of, opened Neovim, turned off all the clever plugins, and started calling `socket()` by hand.

No copilot. No "generate a hash table for me." No "explain this segfault." Just man pages, `lldb`, a lot of `printf` debugging, and the quiet satisfaction of watching bytes flow through a TCP connection I wired up myself.

This is a learning project. It's not production-ready and it's not trying to be. What it _is_ is about 1,100 lines of hand-written C where I understand every single one.

## What It Does

bytekv is a Redis-inspired key-value server that handles multiple concurrent clients over TCP using a single-threaded event loop. You connect, you store keys, you get them back. Keys can expire.

```
$ python3 client.py
bytekv client — connected to localhost:9999
Type commands (SET key val, GET key, ...). Ctrl-C to quit.

bytekv> SET mood rebellious
+OK
bytekv> GET mood
+rebellious
bytekv> SETEX hype 5 "ai-will-replace-programmers"
+OK
bytekv> GET hype
+"ai-will-replace-programmers"

  ... wait 5 seconds ...

bytekv> GET hype
-ERR key not found
```

Some things expire. Like hype.

### Supported Commands

| Command   | Example              | What It Does                                 |
| --------- | -------------------- | -------------------------------------------- |
| `SET`     | `SET key value`      | Store a value                                |
| `GET`     | `GET key`            | Retrieve it                                  |
| `DEL`     | `DEL key`            | Delete it                                    |
| `SETEX`   | `SETEX key 60 value` | Store with TTL (seconds)                     |
| `EXPIRE`  | `EXPIRE key 30`      | Add/update TTL on existing key               |
| `TTL`     | `TTL key`            | Seconds remaining (-1 = no TTL, -2 = gone)   |
| `PERSIST` | `PERSIST key`        | Remove TTL, keep the key                     |
| `KEYS`    | `KEYS`               | List all keys                                |
| `INFO`    | `INFO`               | Server stats (uptime, key count, table size) |

## Architecture

Two layers. Networking knows nothing about storage. Storage knows nothing about TCP. A command dispatcher sits between them and is the only thing that touches both.

```
Client (Python CLI / netcat)
        | TCP (port 9999)
        v
+------------- Networking Layer -------------------+
|                                                   |
|  kqueue event loop                                |
|    -> non-blocking accept/read/write              |
|    -> per-client read & write buffers             |
|    -> length-prefixed binary framing              |
|    -> memmove() compaction on partial reads       |
|                                                   |
+-------------------+-------------------------------+
                    |
            command_execute()
                    |
+-------------------v-------------------------------+
|                                                   |
|  Storage Layer                                    |
|    -> hash table (djb2, separate chaining, 0.75)  |
|    -> dual keyspace: values + expiry timestamps   |
|    -> lazy expiration on read                     |
|    -> monotonic clock for TTL math                |
|                                                   |
+---------------------------------------------------+
```

### How Bytes Actually Flow

1. Client sends: `[4-byte big-endian length][payload]`
2. `kqueue` fires `EVFILT_READ` on the client's fd
3. `on_read()` appends to per-client buffer, calls `process_buffer()`
4. `process_buffer()` extracts complete frames, handles partials and pipelining
5. Each frame hits `command_execute()`: tokenize, lookup command table, dispatch
6. Response gets framed and goes through `queue_write()`:
   - **Fast path:** try `write()` immediately, if it all goes out, skip kqueue entirely
   - **Slow path:** data remains, register `EVFILT_WRITE`, let the event loop drain it

That fast-path write optimization I stole from Redis. You read `networking.c` after building your own version and go "oh, that's clever," and you can actually explain _why_ it's clever because you felt the problem first.

## What I Built By Hand

### Event Loop (`event_loop.c`, 109 lines)

A kqueue-based event loop. The thing underneath Tokio, libuv, and nginx, stripped to its bones. Handler tables indexed by fd. `kevent()` blocks until something happens.

The most important thing I learned here: an event loop is not multithreading. It's one thread that never blocks on any single client. Obvious once you see it, but it took building one to really click.

### Hash Table (`ht.c`, 211 lines)

djb2 hash function, separate chaining for collisions, auto-resize at 0.75 load factor. Tagged union values (string or int64) so the same table handles both the keyspace and expiry metadata.

Every string is `strdup()`'d on insert and `free()`'d on delete. Ownership is unambiguous. In Rust the type system enforces this. In C you just have to not mess up.

```c
/* djb2 — five lines that decide where your data lives */
static size_t ht_hash(const char *key)
{
    size_t hash = 5381;
    int c;
    while ((c = *key++))
        hash = ((hash << 5) + hash) + c;
    return hash;
}
```

### Server & Per-Client Buffers (`server.c`, 272 lines)

This is where I learned that TCP is a byte stream, not a message stream. A single `read()` might give you half a message, or three-and-a-half messages. You accumulate, extract complete frames, `memmove()` the leftovers, and try again. Every network server does this. You don't really _get it_ until you're staring at a buffer wondering where message boundaries went.

Each client gets its own read and write buffers. The write buffer exists because non-blocking `write()` might not send everything, and you can't just drop the rest.

### Command Dispatch (`command.c`, 298 lines)

A command table with arity checking (positive = exact count, negative = minimum). `strsep()` tokenizer. Case-insensitive lookup. Response buffer helpers with format-string safety via `__attribute__((format(printf, 2, 3)))`.

Deliberately simple: tokenize, look up, dispatch, format response. No fancy parsing, no AST. Just splitting on whitespace and calling function pointers.

### TTL & Expiration (`db.c`, 80 lines)

Two hash tables: one for values, one for expiry timestamps. `CLOCK_MONOTONIC` for time because wall clocks lie (NTP adjustments, timezone changes, monotonic clocks don't care).

Lazy expiration: every `GET` checks if the key has expired and silently deletes it. You ask for a key, it's dead, you get "not found." The key cleans up after itself.

## Build & Run

```bash
# Build (requires gcc with C17 support)
make

# Run
./bytekv

# In another terminal, interactive mode
python3 client.py

# Or one-shot
python3 client.py SET hello world
python3 client.py GET hello

# Or raw, if you're into that
echo -ne '\x00\x00\x00\x09SET hi yo' | nc localhost 9999
```

Compiles with `-Wall -Wextra -Werror -fsanitize=undefined`. I'd rather have the compiler yell at me than chase undefined behavior at 2 AM.

## The Commit History Tells a Story

```
a5dfa1d  Networking Phase 1                          <- "hello, socket()"
cc9bb66  Tokenization of command using strsep        <- strings are hard
cf48348  Arity checking, + lookup command             <- "wrong number of arguments"
d4bae60  adding dict create method                    <- the hash table is born
cfacaaf  dictSet                                      <- it can store things!
209796b  Add Dict/Hashing operations                  <- it can do everything!
7eae089  redis like optimization (fast-path write)    <- read Redis, had an "oh" moment
5f5ae5a  Add keys command and dict iterator           <- now it can show what it knows
d36b059  Refactoring of response buffer + INFO        <- cleanup pass
ac6f3bc  Refactoring: SERVER in BSS                   <- globals done right
980f375  Adding force shutdown                        <- double Ctrl-C = rage quit
88c451b  Handle Shutdown                              <- graceful first, rage second
d48885f  Refactoring: ht app agnostic, db layer       <- the SRP revelation
e557535  SETEX + union-tagged values                   <- one table, two types
5deaa2b  Expiration, TTL, Monotonic Clock             <- time is surprisingly tricky
3ef8c13  CHORE: refactoring, fixing bugs              <- housekeeping
```

Some of these commits were wrong at first. That's the point.

## What I Learned

TCP will humble you. I thought I understood networking from using HTTP libraries. Then I called `read()` and got 3 bytes of a 200-byte message and realized the abstraction had been doing a LOT of heavy lifting.

Memory ownership in C is a social contract. No borrow checker, no garbage collector. Just you, `malloc`, `free`, and the question "who is responsible for this pointer?" Get it wrong and you leak memory or use-after-free. Get it right and it's honestly beautiful.

I didn't expect event loops to be this simple. `kqueue()` is literally "hey kernel, wake me up when something happens on these file descriptors." That's Tokio. That's Node.js. That's nginx. Seeing it at the syscall level made all the async/await abstractions I'd used for years suddenly make sense.

The biggest thing though: build first, read Redis second. Every time I built something ugly and then opened the Redis source, I understood their decisions because I'd already hit the same walls. Reading code without that context is just reading words. After struggling with the same problem, it's a conversation.

## What's Left

- [ ] AOF Persistence (append-only file with `fdatasync`, mmap for reads, torn-write detection with CRC32)
- [ ] AOF Compaction (iterate live keys, write to temp file, atomic `rename()` swap)
- [ ] Active expiration sweep (right now only lazy, so keys nobody reads just sit there forever)

## Project Structure

```
bytekv/
  src/
    main.c           5 lines   (yes, really. it calls run_networking())
    event_loop.c    109 lines   kqueue event loop
    server.c        272 lines   connections, buffers, framing
    command.c       298 lines   command table, tokenizer, dispatch
    db.c             80 lines   dual hash table storage, lazy expiration
    ht.c            211 lines   hash table from scratch
    util.c           78 lines   socket setup, monotonic clock
  client.py          70 lines   Python test client
  Makefile           27 lines   the whole build system
  docs/
    CONTEXT.md                  deep design decisions and learning methodology
```

About 1,100 lines of C. No dependencies. No frameworks. No AI.

## Why C, Why Now

I'm a backend engineer with 5+ years of experience who got tired of working at the wrong altitude. Frameworks abstract away the parts I wanted to understand. C doesn't abstract anything. You call `socket()` and the kernel gives you a file descriptor. You call `malloc()` and you get a pointer to some bytes. There's nowhere to hide.

The industry keeps telling engineers to go higher: more abstraction, more AI, more "don't worry about how it works." I went lower instead. Not because low-level is inherently better, but because understanding what's underneath makes you better at everything above it.

Also, debugging a segfault with `lldb` at midnight is genuinely fun and I will not apologize for that.

---

_Built with gcc, man pages, Neovim, and stubbornness._
