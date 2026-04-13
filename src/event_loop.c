#include "event_loop.h"
#include "util.h"
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/event.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#define MAX_EVENTS 64
#define ACTIVE_EXPIRE_TRESHOLD_PERS 25
#define ACTIVE_EXPIRE_NUM_ITEMS 10
#define ACTIVE_EXPIRE_MS_TIMEOUT 5000
void active_expire(event_loop_t *el) {
  // I have to budget my time, have some confid in milliseconds of how much
  // active expire should be limited to, also have the config for number of
  // items to pick (20?) and treshold 25%?
  printf("[LOG] Active Expiry Started\n");
  int64_t start = now_ms();
  int64_t current = start;
  while (current - start < ACTIVE_EXPIRE_MS_TIMEOUT) {
    current = now_ms();
  }
}
void before_sleep(event_loop_t *el) {
  // active expiry
  printf("[LOG] Before Sleep Started\n. kq: %d", el->kq);

  if (el->active_expire) {
    active_expire(el);
  }
  // AOF flushing
}
int el_init(event_loop_t *el) {
  memset(el->read_handlers, 0, sizeof(el->read_handlers));
  memset(el->write_handlers, 0, sizeof(el->write_handlers));
  el->before_sleep_proc = before_sleep;
  el->active_expire = true;

  el->kq = kqueue();
  if (el->kq == -1)
    return -1;

  return 0;
}

int el_add(event_loop_t *el, int fd, el_handler_fn handler) {
  if (fd < 0 || fd >= MAX_FDS)
    return -1;

  struct kevent ev;
  EV_SET(&ev, fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
  if (kevent(el->kq, &ev, 1, NULL, 0, NULL) == -1)
    return -1;

  el->read_handlers[fd] = handler;
  return 0;
}

int el_add_write(event_loop_t *el, int fd, el_handler_fn handler) {
  if (fd < 0 || fd >= MAX_FDS)
    return -1;

  struct kevent ev;
  EV_SET(&ev, fd, EVFILT_WRITE, EV_ADD, 0, 0, NULL);
  if (kevent(el->kq, &ev, 1, NULL, 0, NULL) == -1)
    return -1;

  el->write_handlers[fd] = handler;
  return 0;
}

void el_remove(event_loop_t *el, int fd) {
  if (fd < 0 || fd >= MAX_FDS)
    return;

  struct kevent ev;
  EV_SET(&ev, fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
  kevent(el->kq, &ev, 1, NULL, 0, NULL);
  EV_SET(&ev, fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
  kevent(el->kq, &ev, 1, NULL, 0, NULL);

  el->read_handlers[fd] = NULL;
  el->write_handlers[fd] = NULL;
}

void el_remove_write(event_loop_t *el, int fd) {
  if (fd < 0 || fd >= MAX_FDS)
    return;

  struct kevent ev;
  EV_SET(&ev, fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
  kevent(el->kq, &ev, 1, NULL, 0, NULL);
  el->write_handlers[fd] = NULL;
}

int el_run(event_loop_t *el) {
  struct kevent events[MAX_EVENTS];

  while (!el->stop) {
    el->before_sleep_proc(el);
    // We need to add the timeout here. Which and how? Currently we have it
    // hardcoded, but maybe we should have some server tick rates, or any like
    // that, so we could strike in exact ticks moment when we now there are a
    // gap of processing, like mini application layer scheduler.
    struct timespec timeout;
    timeout.tv_sec = 5;
    timeout.tv_nsec = 10;

    int n = kevent(el->kq, NULL, 0, events, MAX_EVENTS, &timeout);
    if (n == -1) {
      if (errno == EINTR) {
        continue;
      }
      perror("kevent");
      return -1;
    }

    for (int i = 0; i < n; i++) {
      int fd = (int)events[i].ident;
      short filter = events[i].filter;

      if (filter == EVFILT_READ && el->read_handlers[fd])
        el->read_handlers[fd](el, fd);
      else if (filter == EVFILT_WRITE && el->write_handlers[fd])
        el->write_handlers[fd](el, fd);
    }
  }
  return 0;
}

void el_cleanup(event_loop_t *el) { close(el->kq); }
