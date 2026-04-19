#include "config.h"
#include "server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OFFSET(field) offsetof(server_config_t, field)

/* Order must match aof_policy_t values in config.h */
static const char *aof_policy_values[] = {"no", "pertick", "always", NULL};

static config_entry_t config_table[] = {
    {"port", CONFIG_TYPE_INT, OFFSET(port), 1, 65535, NULL},
    {"tcp-backlog", CONFIG_TYPE_INT, OFFSET(tcp_backlog), 1, 65535, NULL},
    {"hz", CONFIG_TYPE_INT, OFFSET(hz), 1, 500, NULL},
    {"active-expire-enabled", CONFIG_TYPE_BOOL, OFFSET(active_expire_enabled),
     0, 1, NULL},
    {"active-expire-percent", CONFIG_TYPE_INT, OFFSET(active_expire_percent), 1,
     100, NULL},
    {"active-expire-keys-per-round", CONFIG_TYPE_INT,
     OFFSET(active_expire_keys_per_round), 1, 1000, NULL},
    {"client-timeout", CONFIG_TYPE_INT, OFFSET(client_timeout_s), 0, 86400,
     NULL},
    {"client-timeout-check-hz", CONFIG_TYPE_INT,
     OFFSET(client_timeout_check_hz), 1, 100, NULL},
    {"aof-enabled", CONFIG_TYPE_BOOL, OFFSET(aof_enabled), 0, 1, NULL},
    {"aof-check-hz", CONFIG_TYPE_INT, OFFSET(aof_check_hz), 1, 100, NULL},
    {"aof-filename", CONFIG_TYPE_STRING, OFFSET(aof_filename), 0, 0, NULL},
    {"appendfsync", CONFIG_TYPE_ENUM, OFFSET(aof_policy), 0, 0,
     aof_policy_values},
    {NULL, 0, 0, 0, 0, NULL},
};

void init_server_config(void) {
  server.config.port = CONFIG_DEFAULT_PORT;
  server.config.tcp_backlog = CONFIG_DEFAULT_TCP_BACKLOG;
  server.config.hz = CONFIG_DEFAULT_HZ;
  server.config.active_expire_enabled = CONFIG_DEFAULT_ACTIVE_EXPIRE_ENABLED;
  server.config.active_expire_percent = CONFIG_DEFAULT_ACTIVE_EXPIRE_PERCENT;
  server.config.active_expire_keys_per_round =
      CONFIG_DEFAULT_ACTIVE_EXPIRE_KEYS_PER_ROUND;
  server.config.client_timeout_s = CONFIG_DEFAULT_CLIENT_TIMEOUT_S;
  server.config.client_timeout_check_hz =
      CONFIG_DEFAULT_CLIENT_TIMEOUT_CHECK_HZ;
  server.config.aof_check_hz = CONFIG_DEFAULT_AOF_CHECK_HZ;
  server.config.aof_enabled = CONFIG_DEFAULT_AOF_ENABLED;
  server.config.aof_policy = CONFIG_DEFAULT_AOF_POLICY;
  server.config.aof_filename = strdup(CONFIG_DEFAULT_AOF_FILENAME);
}

static config_entry_t *find_config(const char *name) {
  for (int i = 0; config_table[i].name != NULL; i++) {
    if (strcmp(config_table[i].name, name) == 0)
      return &config_table[i];
  }
  return NULL;
}

static int apply_config(config_entry_t *entry, const char *value) {
  void *target = (char *)&server.config + entry->offset;

  switch (entry->type) {
  case CONFIG_TYPE_INT: {
    char *end;
    long val = strtol(value, &end, 10);
    if (*end != '\0') {
      fprintf(stderr, "config error: '%s' expects integer, got '%s'\n",
              entry->name, value);
      return -1;
    }
    if (val < entry->min || val > entry->max) {
      fprintf(stderr, "config error: '%s' must be %d-%d, got %ld\n",
              entry->name, entry->min, entry->max, val);
      return -1;
    }
    *(int *)target = (int)val;
    break;
  }
  case CONFIG_TYPE_BOOL: {
    if (strcmp(value, "yes") == 0)
      *(bool *)target = true;
    else if (strcmp(value, "no") == 0)
      *(bool *)target = false;
    else {
      fprintf(stderr, "config error: '%s' expects yes/no, got '%s'\n",
              entry->name, value);
      return -1;
    }
    break;
  }
  case CONFIG_TYPE_STRING: {
    char *copy = strdup(value);
    if (!copy) {
      fprintf(stderr, "config error: out of memory for '%s'\n", entry->name);
      return -1;
    }
    free(*(char **)target);
    *(char **)target = copy;
    break;
  }
  case CONFIG_TYPE_ENUM: {
    for (int i = 0; entry->enum_values[i] != NULL; i++) {
      if (strcmp(entry->enum_values[i], value) == 0) {
        *(int *)target = i;
        return 0;
      }
    }
    fprintf(stderr, "config error: '%s' invalid value '%s', expected one of:",
            entry->name, value);
    for (int i = 0; entry->enum_values[i] != NULL; i++) {
      fprintf(stderr, " %s", entry->enum_values[i]);
    }
    fprintf(stderr, "\n");
    return -1;
  }
  }
  return 0;
}

int load_config(const char *filename) {
  FILE *fp = fopen(filename, "r");
  if (!fp) {
    fprintf(stderr, "cannot open config file: %s\n", filename);
    return -1;
  }

  char line[1024];
  int lineno = 0;
  int errors = 0;

  while (fgets(line, sizeof(line), fp)) {
    lineno++;

    /* strip newline */
    size_t len = strlen(line);
    if (len > 0 && line[len - 1] == '\n')
      line[len - 1] = '\0';

    /* skip empty lines and comments */
    if (line[0] == '\0' || line[0] == '#')
      continue;

    char key[256], value[256];
    if (sscanf(line, "%255s %255s", key, value) != 2) {
      fprintf(stderr, "config error: malformed line %d: %s\n", lineno, line);
      errors++;
      continue;
    }

    config_entry_t *entry = find_config(key);
    if (!entry) {
      fprintf(stderr, "config error: unknown key '%s' at line %d\n", key,
              lineno);
      errors++;
      continue;
    }

    if (apply_config(entry, value) == -1) {
      fprintf(stderr, "  at line %d: %s\n", lineno, line);
      errors++;
    }
  }

  fclose(fp);

  if (errors > 0) {
    fprintf(stderr, "config: %d error(s) in %s\n", errors, filename);
    return -1;
  }

  printf("[bytekv] loaded config from %s\n", filename);
  return 0;
}
