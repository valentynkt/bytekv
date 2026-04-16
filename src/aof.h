#ifndef AOF_H
#define AOF_H

/* Housekeeping entry for the AOF subsystem.
   Called once per event-loop tick from before_sleep.
   No-op unless AOF is enabled, policy is pertick, buffer is dirty,
   and the tick counter aligns with aof_check_hz. */
void aof_cron(void);

#endif
