#include "aof.h"
#include "config.h"
#include "crc64.h"
#include "db.h"
#include "server.h"
#include "util.h"
#include <arpa/inet.h>
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

int aof_append(aof_opcode_t op, const char *key, size_t key_len,
               const char *value, size_t val_len, int64_t expire_at) {
  if (!server.config.aof_enabled)
    return EXIT_SUCCESS;

  uint8_t buf[AOF_RECORD_MAX];
  ssize_t record_size = aof_serialize(buf, sizeof(buf), op, key, key_len, value,
                                      val_len, expire_at);
  if (record_size == -1)
    return EXIT_FAILURE;
  ssize_t nwritten = write_all(server.aof_fd, (const char *)buf, record_size);
  if (nwritten == -1)
    return EXIT_FAILURE;

  server.aof_buf_dirty = true;
  if (server.config.aof_policy == AOF_POLICY_ALWAYS) {
    if (durable_flush(server.aof_fd) == -1)
      return EXIT_FAILURE;
    server.aof_buf_dirty = false;
  }
  return EXIT_SUCCESS;
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

int aof_load(const char *path) {
  struct stat st;
  if (stat(path, &st) == -1 || st.st_size == 0)
    return 0;

  int fd = open(path, O_RDWR);
  if (fd == -1) {
    perror("aof load: open");
    return -1;
  }

  uint8_t record[AOF_RECORD_MAX];
  off_t valid_end = 0;
  size_t records = 0;

  while (true) {
    ssize_t nread = read_exact(fd, (char *)record, AOF_HEADER_SIZE);
    if (nread <= 0)
      break;

    uint32_t body_len_net;
    memcpy(&body_len_net, record + AOF_CRC_SIZE, sizeof(body_len_net));
    uint32_t body_len = ntohl(body_len_net);

    nread = read_exact(fd, (char *)record + AOF_HEADER_SIZE, body_len);
    if (nread <= 0)
      break;

    uint64_t crc_stored_net;
    memcpy(&crc_stored_net, record, AOF_CRC_SIZE);
    uint64_t crc_stored = be64toh(crc_stored_net);
    uint64_t crc_computed =
        crc64(0, (const unsigned char *)record + AOF_CRC_SIZE,
              body_len + AOF_BODYLEN_SIZE);

    if (crc_stored != crc_computed) {
      fprintf(stderr, "[bytekv] aof: CRC mismatch, stopping replay\n");
      break;
    }
    int replay_status = aof_replay_record(record + AOF_HEADER_SIZE, body_len);
    if (replay_status == EXIT_FAILURE) {
      fprintf(stderr, "[bytekv] aof: replay failed\n");
      break;
    }
    records += 1;
    valid_end += AOF_HEADER_SIZE + body_len;
  }

  printf("[bytekv] aof: replayed %zu records\n", records);
  ftruncate(fd, valid_end);
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
  if ((server.cronloops + 1) % server.config.aof_check_hz != 0)
    return;

  if (durable_flush(server.aof_fd) == -1) {
    perror("aof flush");
    return;
  }
  server.aof_buf_dirty = false;
}
