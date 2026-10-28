(import ./json)

(def max-message 262144)

(defn- i32 [n]
  (def x (% n 4294967296))
  (cond (> x 2147483647) (- x 4294967296)
        (< x -2147483648) (+ x 4294967296)
        x))

(defn- rotate [n bits] (bor (blshift n bits) (brushift (if (neg? n) (+ n 4294967296) n) (- 32 bits))))

(defn sha1 [text]
  (def bytes (buffer text))
  (def bits (* 8 (length bytes)))
  (buffer/push-byte bytes 128)
  (while (not= (% (length bytes) 64) 56) (buffer/push-byte bytes 0))
  (for i 0 8 (buffer/push-byte bytes (% (math/floor (/ bits (math/pow 256 (- 7 i)))) 256)))
  (def h @[1732584193 -271733879 -1732584194 271733878 -1009589776])
  (for block 0 (/ (length bytes) 64)
    (def offset (* block 64))
    (def w (array/new-filled 80 0))
    (for i 0 16
      (var n 0)
      (for j 0 4 (set n (+ (* n 256) (get bytes (+ offset (* i 4) j)))))
      (put w i (i32 n)))
    (for i 16 80 (put w i (rotate (bxor (w (- i 3)) (w (- i 8)) (w (- i 14)) (w (- i 16))) 1)))
    (var a (h 0)) (var b (h 1)) (var c (h 2)) (var d (h 3)) (var e (h 4))
    (for i 0 80
      (def [f k]
        (cond (< i 20) [(bor (band b c) (band (bnot b) d)) 1518500249]
              (< i 40) [(bxor b c d) 1859775393]
              (< i 60) [(bor (band b c) (band b d) (band c d)) -1894007588]
              [(bxor b c d) -899497514]))
      (def next (i32 (+ (rotate a 5) f e k (w i))))
      (set e d) (set d c) (set c (rotate b 30)) (set b a) (set a next))
    (eachp [i n] [a b c d e] (put h i (i32 (+ (h i) n)))))
  (def out @"")
  (each n h (each shift [24 16 8 0] (buffer/push-byte out (% (math/floor (/ (if (neg? n) (+ n 4294967296) n) (math/pow 2 shift))) 256))))
  (string out))

(defn accept-key [key] (json/base64 (sha1 (string key "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))))

(defn upgrade? [request]
  (def h (request :headers))
  (and (= (request :method) "GET")
       (= "websocket" (string/ascii-lower (get h "upgrade" "")))
       (some |(= "upgrade" (string/trim $)) (string/split "," (string/ascii-lower (get h "connection" ""))))
       (= "13" (get h "sec-websocket-version"))
       (peg/match ~(* (repeat 22 (set "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/")) "==" -1)
                  (get h "sec-websocket-key" ""))))

(defn frame [opcode payload]
  (def n (length payload))
  (def out @"")
  (buffer/push-byte out (bor 128 opcode))
  (cond (< n 126) (buffer/push-byte out n)
        (< n 65536) (buffer/push-byte out 126 (brushift n 8) (band n 255))
        (do (buffer/push-byte out 127 0 0 0 0)
            (each shift [24 16 8 0] (buffer/push-byte out (band 255 (brushift n shift))))))
  (buffer/push-string out payload)
  (string out))

(defn decode-frame [bytes]
  (when (< (length bytes) 2) (break nil))
  (def first (bytes 0))
  (def second (bytes 1))
  (def opcode (band first 15))
  (def final (not (zero? (band first 128))))
  (when (or (pos? (band first 112)) (zero? (band second 128))
            (not (index-of opcode [0 1 2 8 9 10])))
    (error "invalid websocket frame"))
  (var n (band second 127))
  (var head 2)
  (when (= n 126)
    (when (< (length bytes) 4) (break nil))
    (set n (+ (* 256 (bytes 2)) (bytes 3)))
    (when (< n 126) (error "non-minimal websocket length"))
    (set head 4))
  (when (= (band second 127) 127)
    (when (< (length bytes) 10) (break nil))
    (set n 0)
    (for i 2 10
      (set n (+ (* n 256) (bytes i)))
      (when (> n max-message) (error "websocket message too large")))
    (when (< n 65536) (error "non-minimal websocket length"))
    (set head 10))
  (when (or (> n max-message) (and (>= opcode 8) (or (not final) (> n 125))))
    (error "invalid websocket length"))
  (def end (+ head 4 n))
  (when (< (length bytes) end) (break nil))
  (def payload (buffer/new-filled n 0))
  (for i 0 n (put payload i (bxor (bytes (+ head 4 i)) (bytes (+ head (% i 4))))))
  [end opcode final (string payload)])

(defn utf8? [text]
  (truthy? (peg/match ~(* (any (+ (range "\x00\x7f")
    (* (range "\xc2\xdf") (range "\x80\xbf"))
    (* "\xe0" (range "\xa0\xbf") (range "\x80\xbf"))
    (* (+ (range "\xe1\xec") (range "\xee\xef")) (repeat 2 (range "\x80\xbf")))
    (* "\xed" (range "\x80\x9f") (range "\x80\xbf"))
    (* "\xf0" (range "\x90\xbf") (repeat 2 (range "\x80\xbf")))
    (* (range "\xf1\xf3") (repeat 3 (range "\x80\xbf")))
    (* "\xf4" (range "\x80\x8f") (repeat 2 (range "\x80\xbf"))))) -1) text)))

(defn serve [connection carry request on-open]
  (:write connection (string "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: "
                             (accept-key ((request :headers) "sec-websocket-key")) "\r\n\r\n") 5)
  (var alive true)
  (var queued 0)
  (def outgoing (ev/chan 64))
  (defn close []
    (when alive
      (set alive false)
      (ev/chan-close outgoing)
      (try (:close connection) ([_] nil))))
  (defn send [payload &opt opcode]
    (default opcode 2)
    (unless alive (error "websocket closed"))
    (def bytes (frame opcode payload))
    (while (and alive (pos? queued) (> (+ queued (length bytes)) 1048576))
      (ev/sleep 0.001))
    (unless alive (error "websocket closed"))
    (+= queued (length bytes))
    (ev/give outgoing bytes))
  (ev/go (fn []
    (try
      (while alive
        (when-let [bytes (ev/take outgoing)]
          (:write connection bytes 10)
          (-= queued (length bytes))))
      ([_] (close)))))
  (def callbacks (on-open send))
  (var fragment nil)
  (def text @"")
  (defer (do (close) ((callbacks :close)))
    (try
      (while alive
        (if-let [[end opcode final payload] (decode-frame carry)]
          (do
            (def rest (buffer/slice carry end))
            (buffer/clear carry)
            (buffer/push-string carry rest)
            (case opcode
              8 (do
                  (when (or (= (length payload) 1)
                            (and (> (length payload) 2) (not (utf8? (string/slice payload 2)))))
                    (error "invalid websocket close"))
                  (when (>= (length payload) 2)
                    (def code (+ (* 256 (payload 0)) (payload 1)))
                    (unless (or (and (>= code 1000) (<= code 1014) (not (index-of code [1004 1005 1006])))
                                (and (>= code 3000) (<= code 4999)))
                      (error "invalid websocket close code")))
                  (send payload 8)
                  (while (and alive (pos? queued)) (ev/sleep 0.001))
                  (close))
              9 (send payload 10)
              10 nil
              (do
                (when (= opcode 2) (error "binary requests are unsupported"))
                (if (= opcode 1)
                  (do (when fragment (error "unfinished websocket message")) (set fragment true))
                  (unless fragment (error "unexpected websocket continuation")))
                (when (> (+ (length text) (length payload)) max-message) (error "websocket message too large"))
                (buffer/push-string text payload)
                (when final
                  (unless (utf8? text) (error "invalid websocket UTF-8"))
                  ((callbacks :message) (string text))
                  (buffer/clear text)
                  (set fragment nil)))))
          (if-let [chunk (:read connection 65536 nil 75)]
            (buffer/push-string carry chunk)
            (close))))
      ([_]
        (when alive
          (try (do (send (string/from-bytes 3 234) 8)
                   (while (and alive (pos? queued)) (ev/sleep 0.001))) ([_] nil)))))))
