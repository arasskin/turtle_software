(import ../trace)
(import ./harness :as t)

(t/test "measurement preserves results and errors"
  (t/is= 7 (trace/measure "test-return" 7))
  (t/is= "expected" (try (trace/measure "test-error" (error "expected")) ([e] e))))

(t/test "trace storage is opt-in, bounded, and chronological"
  (if trace/enabled
    (do
      (for i 0 2100 (trace/record "test-ring" i))
      (def samples ((trace/snapshot) :samples))
      (t/is= 2048 (length samples))
      (t/is= 52 ((first samples) :ms))
      (t/is= 2099 ((last samples) :ms)))
    (do
      (trace/record "test-disabled" 1)
      (t/is= false ((trace/snapshot) :enabled))
      (t/is= 0 (length ((trace/snapshot) :samples))))))
