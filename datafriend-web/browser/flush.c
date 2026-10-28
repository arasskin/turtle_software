/* Browser stores are buffered and serialized in one JS context. No disk thread. */
#include "flush.h"
#include <errno.h>
int df_flush_init(df_flush *f, int (*sync)(void *), void *ctx, uint64_t ms) {
  (void)f; (void)sync; (void)ctx; (void)ms; return ENOTSUP;
}
void df_flush_written(df_flush *f) { (void)f; }
int df_flush_check(df_flush *f) { (void)f; return 0; }
int df_flush_barrier(df_flush *f) { (void)f; return 0; }
int df_flush_stop(df_flush *f) { (void)f; return 0; }
void df_flush_destroy(df_flush *f) { (void)f; }
uint64_t df_flush_attempts(df_flush *f) { (void)f; return 0; }
