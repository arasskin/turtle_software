#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "flush.h"
#include <string.h>
#include <errno.h>
static struct timespec deadline(df_flush *f) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  uint64_t ns = (uint64_t)t.tv_nsec + f->interval_ns;
  t.tv_sec += (time_t)(ns / 1000000000);
  t.tv_nsec = (long)(ns % 1000000000);
  return t;
}
static int wait_deadline(df_flush *f) {
#ifdef __APPLE__
  struct timespec now, left;
  clock_gettime(CLOCK_MONOTONIC, &now);
  left.tv_sec = f->deadline.tv_sec - now.tv_sec;
  left.tv_nsec = f->deadline.tv_nsec - now.tv_nsec;
  if (left.tv_nsec < 0) {
    --left.tv_sec;
    left.tv_nsec += 1000000000;
  }
  if (left.tv_sec < 0)
    return ETIMEDOUT;
  return pthread_cond_timedwait_relative_np(&f->wake, &f->mutex, &left);
#else
  return pthread_cond_timedwait(&f->wake, &f->mutex, &f->deadline);
#endif
}
static void *run(void *context) {
  df_flush *f = context;
  pthread_mutex_lock(&f->mutex);
  for (;;) {
    while (!f->stop && f->dirty == f->synced)
      pthread_cond_wait(&f->wake, &f->mutex);
    if (f->dirty == f->synced && f->stop)
      break;
    while (!f->stop && !f->urgent) {
      int rc = wait_deadline(f);
      if (rc == ETIMEDOUT)
        break;
      if (rc) {
        f->error = rc;
        f->stop = true;
        break;
      }
    }
    uint64_t target = f->dirty;
    f->urgent = false;
    pthread_mutex_unlock(&f->mutex);
    int error = f->sync(f->context);
    pthread_mutex_lock(&f->mutex);
    ++f->attempts;
    f->error = error;
    if (!error)
      f->synced = target;
    f->deadline = deadline(f);
    pthread_cond_broadcast(&f->wake);
    if (f->stop && (error || f->dirty == f->synced))
      break;
  }
  pthread_mutex_unlock(&f->mutex);
  return NULL;
}
int df_flush_init(df_flush *f, int (*sync)(void *), void *context, uint64_t interval_ms) {
  memset(f, 0, sizeof(*f));
  if (!sync || interval_ms > UINT64_MAX / 1000000)
    return EINVAL;
  f->sync = sync;
  f->context = context;
  f->interval_ns = interval_ms * 1000000;
  int rc = pthread_mutex_init(&f->mutex, NULL);
  if (rc)
    return rc;
  pthread_condattr_t attr;
  rc = pthread_condattr_init(&attr);
  if (rc)
    goto mutex;
#ifndef __APPLE__
  rc = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
  if (rc) {
    pthread_condattr_destroy(&attr);
    goto mutex;
  }
#endif
  rc = pthread_cond_init(&f->wake, &attr);
  pthread_condattr_destroy(&attr);
  if (rc)
    goto mutex;
  rc = pthread_create(&f->worker, NULL, run, f);
  if (rc) {
    pthread_cond_destroy(&f->wake);
    goto mutex;
  }
  f->started = true;
  return 0;
mutex:
  pthread_mutex_destroy(&f->mutex);
  return rc;
}
void df_flush_written(df_flush *f) {
  pthread_mutex_lock(&f->mutex);
  if (f->dirty == f->synced)
    f->deadline = deadline(f);
  ++f->dirty;
  pthread_cond_broadcast(&f->wake);
  pthread_mutex_unlock(&f->mutex);
}
int df_flush_check(df_flush *f) {
  pthread_mutex_lock(&f->mutex);
  int rc = f->error;
  pthread_mutex_unlock(&f->mutex);
  return rc;
}
int df_flush_barrier(df_flush *f) {
  pthread_mutex_lock(&f->mutex);
  uint64_t target = f->dirty, previous = f->attempts;
  if (f->synced >= target) {
    pthread_mutex_unlock(&f->mutex);
    return 0;
  }
  f->urgent = true;
  pthread_cond_broadcast(&f->wake);
  while (f->synced < target && !(f->attempts > previous && f->error))
    pthread_cond_wait(&f->wake, &f->mutex);
  int rc = f->error;
  pthread_mutex_unlock(&f->mutex);
  return rc;
}
int df_flush_stop(df_flush *f) {
  pthread_mutex_lock(&f->mutex);
  f->stop = true;
  pthread_cond_broadcast(&f->wake);
  pthread_mutex_unlock(&f->mutex);
  if (f->started) {
    pthread_join(f->worker, NULL);
    f->started = false;
  }
  return f->error;
}
void df_flush_destroy(df_flush *f) {
  df_flush_stop(f);
  pthread_cond_destroy(&f->wake);
  pthread_mutex_destroy(&f->mutex);
}
uint64_t df_flush_attempts(df_flush *f) {
  pthread_mutex_lock(&f->mutex);
  uint64_t n = f->attempts;
  pthread_mutex_unlock(&f->mutex);
  return n;
}
