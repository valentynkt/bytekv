#include "aof.h"
#include "config.h"
#include "crc64.h"
#include "db.h"
#include "server.h"
#include "util.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#if defined(__linux__)
#include <endian.h>
#elif defined(__APPLE__)
#include <sys/endian.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* Serialization                                                       */
static ssize_t aof_serialize(uint8_t *buf, size_t buf_sz, aof_opcode_t op,
                             const char *key, size_t key_len, const char *value,
                             size_t val_len, int64_t expire_at) {
  int off = AOF_HEADER_SIZE;
  buf[off] = op;
  off += 1;

  uint16_t klen_net = htons((uint16_t)key_len);
  memcpy(buf + off, &klen_net, sizeof(klen_net));
  off += sizeof(klen_net);

  uint32_t vlen_net = htonl((uint32_t)val_len);
  memcpy(buf + off, &vlen_net, sizeof(vlen_net));
  off += sizeof(vlen_net);

  uint64_t expire_net = htobe64((uint64_t)expire_at);
  memcpy(buf + off, &expire_net, sizeof(expire_net));
  off += sizeof(expire_net);

  if (off + key_len + val_len > buf_sz) {
    return -1;
  }
  memcpy(buf + off, key, key_len);
  off += key_len;
  memcpy(buf + off, value, val_len);
  off += val_len;

  uint32_t body_len = off - AOF_HEADER_SIZE;
  uint32_t body_len_net = htonl(body_len);
  memcpy(buf + AOF_CRC_SIZE, &body_len_net, sizeof(body_len_net));

  uint64_t checksum = htobe64(crc64(0, buf + AOF_CRC_SIZE, off - AOF_CRC_SIZE));
  memcpy(buf, &checksum, sizeof(checksum));
  return off;
}

/* Write path                                                          */

/* Live append: append to the on-disk AOF (kernel page cache). This is
   always done while AOF is enabled, regardless of policy. */
static int aof_emit_live(const uint8_t *buf, size_t n) {
  return write_all(server.aof_fd, (const char *)buf, n) == -1 ? -1 : 0;
}

/* Tear down an in-flight rewrite (diff overflow / realloc failure).
   Signals the child, frees the diff buffer, moves state to ABORTING so the
   cron path will discard temp.aof on reap. The live AOF is untouched. */
static void aof_abort_rewrite(const char *reason) {
  fprintf(stderr,
          "[bytekv] aof rewrite: aborting (%s); live AOF unaffected\n",
          reason);
  kill(server.aof_rewrite_pid, SIGTERM);
  free(server.aof_rewrite_buf);
  server.aof_rewrite_buf = NULL;
  server.aof_rewrite_len = 0;
  server.aof_rewrite_state = AOF_RW_ABORTING;
}

/* Diff append: stash a copy of the record into the in-memory diff buffer
   so finalize() can append it after the child's snapshot. Bounded by
   aof_rewrite_buf_max_size — on overflow we abort the rewrite. */
static void aof_emit_diff(const uint8_t *buf, size_t n) {
  size_t needed = server.aof_rewrite_len + n;
  size_t max_cap = (size_t)server.config.aof_rewrite_buf_max_size;

  if (needed > server.aof_rewrite_cap) {
    if (needed > max_cap) {
      char msg[64];
      snprintf(msg, sizeof(msg), "diff buf would exceed %zu bytes", max_cap);
      aof_abort_rewrite(msg);
      return;
    }
    size_t new_cap = server.aof_rewrite_cap * 2;
    if (new_cap < needed)
      new_cap = needed;
    if (new_cap > max_cap)
      new_cap = max_cap;
    char *new_buf = realloc(server.aof_rewrite_buf, new_cap);
    if (!new_buf) {
      aof_abort_rewrite("realloc failed");
      return;
    }
    server.aof_rewrite_buf = new_buf;
    server.aof_rewrite_cap = new_cap;
  }
  memcpy(server.aof_rewrite_buf + server.aof_rewrite_len, buf, n);
  server.aof_rewrite_len += n;
}

/* ALWAYS policy: fsync the live AOF after each record. Other policies defer
   fsync to aof_cron (PERTICK) or to the OS (NO). */
static int aof_maybe_fsync_always(void) {
  if (server.config.aof_policy != AOF_POLICY_ALWAYS)
    return 0;
  if (durable_flush(server.aof_fd) == -1)
    return -1;
  server.aof_buf_dirty = false;
  return 0;
}

int aof_append(aof_opcode_t op, const char *key, size_t key_len,
               const char *value, size_t val_len, int64_t expire_at) {
  if (!server.config.aof_enabled)
    return EXIT_SUCCESS;

  uint8_t buf[AOF_RECORD_MAX];
  ssize_t record_size = aof_serialize(buf, sizeof(buf), op, key, key_len, value,
                                      val_len, expire_at);
  if (record_size == -1)
    return EXIT_FAILURE;

  if (aof_emit_live(buf, record_size) == -1)
    return EXIT_FAILURE;

  if (server.aof_rewrite_state == AOF_RW_ACTIVE)
    aof_emit_diff(buf, record_size);

  server.aof_buf_dirty = true;
  return aof_maybe_fsync_always() == -1 ? EXIT_FAILURE : EXIT_SUCCESS;
}

/* Recovery (read path)                                                */

static int aof_replay_record(const uint8_t *buf, uint32_t body_len) {
  int off = 0;
  int opcode = buf[off];
  off += 1;

  uint16_t key_len_net;
  memcpy(&key_len_net, buf + off, sizeof(uint16_t));
  uint16_t key_len = ntohs(key_len_net);
  off += sizeof(uint16_t);

  uint32_t val_len_net;
  memcpy(&val_len_net, buf + off, sizeof(uint32_t));
  uint32_t val_len = ntohl(val_len_net);
  off += sizeof(uint32_t);

  uint64_t expire_at_net;
  memcpy(&expire_at_net, buf + off, sizeof(uint64_t));
  int64_t expire_at = (int64_t)be64toh(expire_at_net);
  off += sizeof(uint64_t);

  if (expire_at > 0 && expire_at < realtime_ms())
    return EXIT_SUCCESS;

  size_t expected_body_len = AOF_BODY_FIXED + key_len + val_len;
  if (key_len + val_len > MSG_MAX || expected_body_len != body_len)
    return EXIT_FAILURE;

  char key[MSG_MAX + 1];
  memcpy(key, buf + off, key_len);
  key[key_len] = '\0';
  off += key_len;

  char val[MSG_MAX + 1];
  memcpy(val, buf + off, val_len);
  val[val_len] = '\0';
  off += val_len;

  int result = EXIT_FAILURE;
  switch (opcode) {
  case AOF_OP_SET:
    result = db_set(server.db, key, val);
    break;
  case AOF_OP_DEL:
    result = db_del(server.db, key);
    break;
  case AOF_OP_SETEX:
    result = db_setex(server.db, key, val, expire_at);
    break;
  case AOF_OP_EXPIRE:
    result = db_key_expire(server.db, key, expire_at);
    break;
  case AOF_OP_PERSIST:
    result = db_persist(server.db, key);
    break;
  default:
    fprintf(stderr, "[bytekv] aof: unknown opcode %d\n", opcode);
    break;
  }
  return result;
}

/* Copy bytes [from, from + len) of src_fd into a new quarantine file named
   "<path>.corrupt-<timestamp>". Best-effort: errors are logged but do not
   propagate, since this is a forensic capture, not a correctness operation. */
static void aof_quarantine_tail(const char *path, int src_fd, off_t from,
                                off_t len) {
  char quarantine[512];
  snprintf(quarantine, sizeof(quarantine), "%s.corrupt-%lld", path,
           (long long)realtime_ms());
  int qfd = open(quarantine, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (qfd == -1) {
    fprintf(stderr,
            "[bytekv] aof: WARNING could not open quarantine '%s': %s\n",
            quarantine, strerror(errno));
    return;
  }
  if (lseek(src_fd, from, SEEK_SET) == (off_t)-1) {
    perror("aof quarantine: lseek");
    close(qfd);
    return;
  }
  char buf[8192];
  off_t remaining = len;
  while (remaining > 0) {
    size_t chunk = remaining > (off_t)sizeof(buf) ? sizeof(buf) : remaining;
    ssize_t n = read(src_fd, buf, chunk);
    if (n <= 0)
      break;
    if (write_all(qfd, buf, n) == -1) {
      perror("aof quarantine: write");
      break;
    }
    remaining -= n;
  }
  close(qfd);
  fprintf(stderr, "[bytekv] aof: quarantined corrupt tail to %s\n", quarantine);
}

int aof_load(const char *path) {
  struct stat st;
  if (stat(path, &st) == -1 || st.st_size == 0)
    return 0;

  int fd = open(path, O_RDWR);
  if (fd == -1) {
    perror("aof load: open");
    return -1;
  }

  off_t total_size = st.st_size;
  uint8_t record[AOF_RECORD_MAX];
  off_t valid_end = 0;
  size_t records = 0;
  const char *break_reason = NULL;

  while (true) {
    ssize_t nread = read_exact(fd, (char *)record, AOF_HEADER_SIZE);
    if (nread < 0) {
      break_reason = "header read error";
      break;
    }
    if (nread == 0)
      break; /* clean EOF on a record boundary */
    if (nread < AOF_HEADER_SIZE) {
      break_reason = "truncated header";
      break;
    }

    uint32_t body_len_net;
    memcpy(&body_len_net, record + AOF_CRC_SIZE, sizeof(body_len_net));
    uint32_t body_len = ntohl(body_len_net);

    nread = read_exact(fd, (char *)record + AOF_HEADER_SIZE, body_len);
    if (nread < 0 || (uint32_t)nread < body_len) {
      break_reason = "truncated body";
      break;
    }

    uint64_t crc_stored_net;
    memcpy(&crc_stored_net, record, AOF_CRC_SIZE);
    uint64_t crc_stored = be64toh(crc_stored_net);
    uint64_t crc_computed =
        crc64(0, (const unsigned char *)record + AOF_CRC_SIZE,
              body_len + AOF_BODYLEN_SIZE);

    if (crc_stored != crc_computed) {
      break_reason = "CRC mismatch";
      break;
    }
    int replay_status = aof_replay_record(record + AOF_HEADER_SIZE, body_len);
    if (replay_status == EXIT_FAILURE) {
      break_reason = "replay failed";
      break;
    }
    records += 1;
    valid_end += AOF_HEADER_SIZE + body_len;
  }

  off_t bytes_lost = total_size - valid_end;
  printf("[bytekv] aof: replayed %zu records (%lld bytes)\n", records,
         (long long)valid_end);

  if (bytes_lost > 0) {
    fprintf(stderr,
            "[bytekv] aof: stopping replay at offset %lld (%s); "
            "%lld trailing bytes will be truncated\n",
            (long long)valid_end, break_reason ? break_reason : "unknown",
            (long long)bytes_lost);
    aof_quarantine_tail(path, fd, valid_end, bytes_lost);
  }

  if (ftruncate(fd, valid_end) == -1)
    perror("aof load: ftruncate");
  close(fd);
  return 0;
}

/* ------------------------------------------------------------------ */
/* Periodic flush (pertick policy)                                     */
/* ------------------------------------------------------------------ */

void aof_cron(void) {
  if (!server.config.aof_enabled)
    return;
  if (server.config.aof_policy != AOF_POLICY_PERTICK)
    return;
  if (!server.aof_buf_dirty)
    return;
  static int64_t last_fsync_ms = 0;
  int interval_ms = server.config.aof_check_hz * 1000 / server.config.hz;
  if (!run_every_ms(&last_fsync_ms, server.now_ms, interval_ms))
    return;

  if (durable_flush(server.aof_fd) == -1) {
    perror("aof flush");
    return;
  }
  server.aof_buf_dirty = false;
}

static void aof_rewrite_child(void) {
  int fd = open(server.aof_temp_filename, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd == -1)
    _exit(1);

  uint8_t buf[AOF_RECORD_MAX];
  ht_iter_t *iter = ht_iter_create(server.db->keyspace);
  ht_entry_t *entry;

  while ((entry = ht_iter_next(iter)) != NULL) {
    int64_t *expire = ht_get_i64(server.db->expires, entry->key);
    if (expire && *expire < realtime_ms())
      continue;

    aof_opcode_t op = expire ? AOF_OP_SETEX : AOF_OP_SET;
    int64_t expire_at = expire ? *expire : 0;

    ssize_t record_size =
        aof_serialize(buf, sizeof(buf), op, entry->key, strlen(entry->key),
                      entry->val.str, strlen(entry->val.str), expire_at);
    if (record_size == -1)
      _exit(1);
    if (write_all(fd, (const char *)buf, record_size) == -1)
      _exit(1);
  }

  free(iter);
  if (durable_flush(fd) == -1)
    _exit(1);
  close(fd);
  _exit(0);
}

int aof_rewrite_start(void) {
  /* Allocate the diff buffer BEFORE fork so the parent never lands in a
     state where the child is running but rewrite_buf is NULL. A NULL
     rewrite_buf with pid > 0 would crash aof_append on its first memcpy. */
  char *buf = malloc(server.aof_rewrite_cap);
  if (!buf) {
    fprintf(stderr, "[bytekv] aof rewrite: malloc(%zu) failed, skipping\n",
            server.aof_rewrite_cap);
    return -1;
  }

  pid_t pid = fork();
  if (pid < 0) {
    perror("aof rewrite: fork");
    free(buf);
    return -1;
  }
  if (pid == 0) {
    /* child never returns from this call */
    aof_rewrite_child();
    _exit(1); /* defensive: unreachable */
  }
  /* parent */
  server.aof_rewrite_pid = pid;
  server.aof_rewrite_state = AOF_RW_ACTIVE;
  server.aof_rewrite_len = 0;
  server.aof_rewrite_buf = buf;
  return 0;
}

static void aof_rewrite_cleanup(void) {
  free(server.aof_rewrite_buf);
  server.aof_rewrite_buf = NULL;
  server.aof_rewrite_len = 0;
  server.aof_rewrite_pid = -1;
  server.aof_rewrite_state = AOF_RW_IDLE;
}

static void aof_rewrite_finalize(void) {
  int temp_fd = open(server.aof_temp_filename, O_WRONLY | O_APPEND | O_CLOEXEC);
  if (temp_fd == -1) {
    perror("aof rewrite finalize: open");
    goto fail;
  }

  if (write_all(temp_fd, server.aof_rewrite_buf, server.aof_rewrite_len) ==
      -1) {
    perror("aof rewrite finalize: write");
    close(temp_fd);
    goto fail;
  }

  if (durable_flush(temp_fd) == -1) {
    perror("aof rewrite finalize: fsync");
    close(temp_fd);
    goto fail;
  }
  close(temp_fd);

  /* Open the new fd BEFORE rename. The fd binds to the inode, so the rename
     (which only manipulates the directory entry) leaves the fd pointing to
     the same file. This guarantees we always hold a valid writable fd —
     no window where new aof_append calls would fail with EBADF. */
  int new_aof_fd = open_aof(server.aof_temp_filename);
  if (new_aof_fd == -1) {
    perror("aof rewrite finalize: reopen");
    goto fail;
  }

  if (rename(server.aof_temp_filename, server.config.aof_filename) == -1) {
    perror("aof rewrite finalize: rename");
    close(new_aof_fd);
    goto fail;
  }

  int old_aof_fd = server.aof_fd;
  server.aof_fd = new_aof_fd;
  close(old_aof_fd);

  struct stat st;
  if (stat(server.config.aof_filename, &st) == 0)
    server.aof_rewrite_last_size = st.st_size;

  aof_rewrite_cleanup();
  printf("[bytekv] aof rewrite complete\n");
  return;

fail:
  fprintf(stderr, "[bytekv] aof rewrite failed, keeping old AOF\n");
  unlink(server.aof_temp_filename);
  aof_rewrite_cleanup();
}

void aof_rewrite_cron(void) {
  if (!server.config.aof_enabled)
    return;

  /* check once per second, regardless of event-loop tick rate */
  static int64_t last_check_ms = 0;
  if (!run_every_ms(&last_check_ms, server.now_ms, 1000))
    return;

  /* no child running, maybe start one */
  if (server.aof_rewrite_state == AOF_RW_IDLE) {
    struct stat st;
    if (stat(server.config.aof_filename, &st) != 0)
      return;
    if (st.st_size < server.config.aof_rewrite_min_size)
      return;
    off_t threshold = server.aof_rewrite_last_size +
                      server.aof_rewrite_last_size *
                          server.config.aof_rewrite_growth / 100;
    if (st.st_size < threshold)
      return;
    aof_rewrite_start();
    return;
  }

  /* child running, check if done */
  int status;
  pid_t result = waitpid(server.aof_rewrite_pid, &status, WNOHANG);
  if (result == 0)
    return; /* still working */
  if (result == -1) {
    perror("aof rewrite: waitpid");
    aof_rewrite_cleanup();
    return;
  }

  /* child finished. If we aborted mid-flight (diff overflow), discard the
     temp file regardless of how the child terminated — the diff is gone so
     finalize would produce a file missing every write since the abort. */
  if (server.aof_rewrite_state == AOF_RW_ABORTING) {
    fprintf(stderr,
            "[bytekv] aof rewrite: discarding temp.aof after abort\n");
    unlink(server.aof_temp_filename);
    aof_rewrite_cleanup();
    return;
  }

  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
    aof_rewrite_finalize();
  } else {
    fprintf(stderr, "[bytekv] aof rewrite: child failed (status %d)\n", status);
    unlink(server.aof_temp_filename);
    aof_rewrite_cleanup();
  }
}
