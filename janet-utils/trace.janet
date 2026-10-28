(def enabled (= "1" (os/getenv "VISUALIZE_TRACE")))
(def- capacity 2048)
(def- samples @[])
(var- cursor 0)

(defn now [] (os/clock :monotonic))

(defn record [kind ms]
  (when enabled
    (def sample {:kind kind :ms ms :at (* 1000 (now))})
    (if (< (length samples) capacity)
      (array/push samples sample)
      (put samples cursor sample))
    (set cursor (% (inc cursor) capacity))
    (when-let [timing (dyn :latency)]
      (put timing kind (+ (get timing kind 0) ms)))))

(defmacro measure [kind & body]
  (def started (gensym))
  ~(if ,enabled
     (let [,started (,now)]
       (defer (,record ,kind (* 1000 (- (,now) ,started))) ,;body))
     (do ,;body)))

(defn snapshot []
  {:enabled enabled
   :samples (if (< (length samples) capacity)
              (array/slice samples)
              (array/concat @[] (array/slice samples cursor)
                               (array/slice samples 0 cursor)))})

(defn heartbeat []
  (when enabled
    (ev/go
      (fn []
        (forever
          (def before (now))
          (ev/sleep 0.005)
          (def late (- (* 1000 (- (now) before)) 5))
          (when (> late 2) (record "loop-lag" late)))))))
