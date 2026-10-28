(defn base64 [bytes]
  (def alphabet "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/")
  (def out @"")
  (for group 0 (math/ceil (/ (length bytes) 3))
    (def i (* group 3))
    (def n (+ (* (get bytes i) 65536) (* (get bytes (+ i 1) 0) 256) (get bytes (+ i 2) 0)))
    (buffer/push-byte out (alphabet (band 63 (brushift n 18)))
                         (alphabet (band 63 (brushift n 12)))
                         (if (< (+ i 1) (length bytes)) (alphabet (band 63 (brushift n 6))) 61)
                         (if (< (+ i 2) (length bytes)) (alphabet (band 63 n)) 61)))
  (string out))

(def- special
  (let [t (array/new-filled 256 false)]
    (each b [(chr `"`) (chr "\\") (chr "<") (chr ">") (chr "&")] (put t b true))
    (for b 0 0x20 (put t b true))
    (freeze t)))

(def- escaped
  (let [t (array/new-filled 256 nil)]
    (put t (chr `"`) `\"`)
    (put t (chr "\\") "\\\\")
    (put t (chr "\n") "\\n")
    (put t (chr "\r") "\\r")
    (put t (chr "\t") "\\t")
    (put t (chr "<") "\\u003c")
    (put t (chr ">") "\\u003e")
    (put t (chr "&") "\\u0026")
    (for b 0 0x20
      (unless (get t b) (put t b (string/format "\\u%04x" b))))
    (freeze t)))

(defn- escape

  [text]
  (def out @`"`)
  (def n (length text))
  (var start 0)
  (var i 0)
  (while (< i n)
    (if (get special (get text i))
      (do

        (when (> i start) (buffer/push-string out (string/slice text start i)))
        (buffer/push-string out (get escaped (get text i)))
        (++ i)
        (set start i))
      (++ i)))
  (when (> n start) (buffer/push-string out (string/slice text start n)))
  (buffer/push-string out `"`)
  (string out))

(defn encode

  [value]
  (cond
    (nil? value) "null"
    (= value true) "true"
    (= value false) "false"
    (number? value) (if (= value (math/floor value))
                      (string (math/floor value))
                      (string value))
    (or (string? value) (buffer? value)) (escape value)
    (keyword? value) (escape (string value))
    (symbol? value) (escape (string value))
    (indexed? value) (string "[" (string/join (map encode value) ",") "]")
    (dictionary? value)

    (string "{"
            (string/join
              (map (fn [k] (string (escape (string k)) ":" (encode (get value k))))
                   (sorted-by string (keys value)))
              ",")
            "}")
    (escape (string value))))

(defn- skip-space [text at]
  (var i at)
  (while (and (< i (length text))
              (let [c (text i)]
                (or (= c 32) (= c 9) (= c 10) (= c 13))))
    (++ i))
  i)

(defn- read-string-at [text at]

  (unless (= (get text at) (chr "\"")) (error "expected JSON string"))
  (var i (+ at 1))
  (def out @"")
  (while (and (< i (length text)) (not= (text i) (chr "\"")))
    (when (< (text i) 32) (error "unescaped control character in JSON string"))
    (if (= (text i) (chr "\\"))
      (let [next (get text (+ i 1))]
        (cond
          (= next (chr "n")) (do (buffer/push-byte out 10) (+= i 2))
          (= next (chr "t")) (do (buffer/push-byte out 9) (+= i 2))
          (= next (chr "r")) (do (buffer/push-byte out 13) (+= i 2))
          (= next (chr "b")) (do (buffer/push-byte out 8) (+= i 2))
          (= next (chr "f")) (do (buffer/push-byte out 12) (+= i 2))
          (= next (chr "u"))
          (do
            (defn unit [start]
              (def hex (string/slice text start (+ start 4)))
              (unless (peg/match ~(* (repeat 4 (+ (range "09") (range "af") (range "AF"))) -1) hex)
                (error "invalid JSON unicode escape"))
              (scan-number (string "0x" hex)))
            (var code (unit (+ i 2)))
            (+= i 6)
            (when (<= 0xd800 code 0xdbff)
              (unless (= (string/slice text i (+ i 2)) "\\u") (error "missing low surrogate"))
              (def low (unit (+ i 2)))
              (unless (<= 0xdc00 low 0xdfff) (error "invalid low surrogate"))
              (set code (+ 0x10000 (* (- code 0xd800) 1024) (- low 0xdc00)))
              (+= i 6))
            (when (<= 0xdc00 code 0xdfff) (error "unexpected low surrogate"))
            (cond
              (< code 0x80) (buffer/push-byte out code)
              (< code 0x800) (buffer/push-byte out (bor 0xc0 (brshift code 6)) (bor 0x80 (band code 0x3f)))
              (< code 0x10000) (buffer/push-byte out (bor 0xe0 (brshift code 12))
                                (bor 0x80 (band (brshift code 6) 0x3f)) (bor 0x80 (band code 0x3f)))
              (buffer/push-byte out (bor 0xf0 (brshift code 18)) (bor 0x80 (band (brshift code 12) 0x3f))
                               (bor 0x80 (band (brshift code 6) 0x3f)) (bor 0x80 (band code 0x3f)))))
          (index-of next [(chr "\"") (chr "\\") (chr "/")])
          (do (buffer/push-byte out next) (+= i 2))
          (error "invalid JSON string escape")))
      (do (buffer/push-byte out (text i)) (++ i))))
  (unless (= (get text i) (chr "\"")) (error "unterminated JSON string"))
  [(string out) (+ i 1)])

(defn- read-value [text at]
  (def i (skip-space text at))
  (when (>= i (length text)) (error "unexpected end of JSON"))
  (def c (text i))
  (cond
    (= c (chr "\"")) (read-string-at text i)

    (= c (chr "{"))
    (do
      (var j (skip-space text (+ i 1)))
      (def out @{})
      (if (= (get text j) (chr "}"))
        [out (+ j 1)]
        (do
          (var running true)
          (while running
            (set j (skip-space text j))
            (def [key after-key] (read-string-at text j))
            (set j (skip-space text after-key))
            (unless (= (get text j) (chr ":")) (error "expected ':' in object"))
            (def [value after-value] (read-value text (+ j 1)))
            (put out key value)
            (set j (skip-space text after-value))
            (cond
              (= (get text j) (chr ",")) (++ j)
              (= (get text j) (chr "}")) (do (++ j) (set running false))
              (error "expected ',' or '}' in object")))
          [out j])))

    (= c (chr "["))
    (do
      (var j (skip-space text (+ i 1)))
      (def out @[])
      (if (= (get text j) (chr "]"))
        [out (+ j 1)]
        (do
          (var running true)
          (while running
            (def [value after] (read-value text j))
            (array/push out value)
            (set j (skip-space text after))
            (cond
              (= (get text j) (chr ",")) (++ j)
              (= (get text j) (chr "]")) (do (++ j) (set running false))
              (error "expected ',' or ']' in array")))
          [out j])))

    (string/has-prefix? "true" (string/slice text i)) [true (+ i 4)]
    (string/has-prefix? "false" (string/slice text i)) [false (+ i 5)]
    (string/has-prefix? "null" (string/slice text i)) [nil (+ i 4)]

    (do
      (var j i)
      (while (and (< j (length text))
                  (let [d (text j)]
                    (or (and (>= d (chr "0")) (<= d (chr "9")))
                        (= d (chr "-")) (= d (chr "+"))
                        (= d (chr ".")) (= d (chr "e")) (= d (chr "E")))))
        (++ j))
      (def token (string/slice text i j))
      (unless (peg/match ~(* (? "-") (+ "0" (* (range "19") (any (range "09"))))
                             (? (* "." (some (range "09"))))
                             (? (* (+ "e" "E") (? (+ "+" "-")) (some (range "09")))) -1) token)
        (error "invalid JSON number"))
      (def found (scan-number token))
      (if (nil? found)
        (errorf "bad JSON at byte %d" i)
        [found j]))))

(defn decode

  [text]
  (def [value end] (read-value (string text) 0))
  (unless (= (skip-space text end) (length text)) (error "trailing data after JSON value"))
  value)
