#ifndef DF_FLUSH_H
#define DF_FLUSH_H
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Only the sync callback runs on this worker. Its context and fd must outlive
 * stop. Store transactions, indexes and live results stay on the caller. */
typedef struct df_flush {
  pthread_mutex_t mutex;
  pthread_cond_t wake;
  pthread_t worker;
  int (*sync)(void *);
  void *context;
  uint64_t dirty, synced, attempts, interval_ns;
  struct timespec deadline;
  int error;
  bool stop, urgent, started;
} df_flush;
int df_flush_init(df_flush *, int (*)(void *), void *, uint64_t interval_ms);
void df_flush_written(df_flush *);
int df_flush_check(df_flush *);
int df_flush_barrier(df_flush *);
int df_flush_stop(df_flush *);
void df_flush_destroy(df_flush *);
uint64_t df_flush_attempts(df_flush *);
#ifdef __cplusplus
}
#endif
#endif
