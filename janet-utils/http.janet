(import ./trace)

(def max-header 16384)
(def max-body 1048576)

(defn- reject [status message]
  (error {:http-status status :message message}))

(defn local-host? [host port]
  (and (string? host)
       (index-of (string/ascii-lower host)
                 [(string "127.0.0.1:" port) (string "localhost:" port)])))

(defn- header-end

  [buf]
  (if-let [at (string/find "\r\n\r\n" buf)]
    (+ at 4)

    (if-let [at (string/find "\n\n" buf)] (+ at 2))))

(defn- parse-headers

  [text]
  (def headers @{})
  (each line (string/split "\n" text)
    (def clean (string/trim line))
    (unless (empty? clean)
      (def colon (string/find ":" clean))
      (unless (and colon (pos? colon)) (reject "400 Bad Request" "invalid header"))
      (def name (string/ascii-lower (string/slice clean 0 colon)))
      (when (or (not= name (string/trim name))
                (and (index-of name ["host" "content-length" "transfer-encoding"])
                     (get headers name)))
        (reject "400 Bad Request" "ambiguous header"))
      (put headers name (string/trim (string/slice clean (+ colon 1))))))
  headers)

(defn read-request

  [conn carry &opt port body-limit]
  (var split (header-end carry))

  (while (not split)
    (when (>= (length carry) max-header)
      (reject "431 Request Header Fields Too Large" "headers too large"))
    (def chunk (:read conn 4096 nil 75))
    (if-not chunk (break))
    (buffer/push-string carry chunk)
    (set split (header-end carry)))
  (when split
    (when (> split max-header)
      (reject "431 Request Header Fields Too Large" "headers too large"))
    (def head (string/slice carry 0 split))
    (def lines (string/split "\n" head))
    (def request (string/split " " (string/trim (or (first lines) ""))))
    (when (>= (length request) 2)
      (def headers (parse-headers (string/join (drop 1 lines) "\n")))
      (when (and port (not (local-host? (headers "host") port)))
        (reject "403 Forbidden" "unexpected host"))
      (when (headers "transfer-encoding")
        (reject "400 Bad Request" "transfer encoding is unsupported"))
      (def size (or (headers "content-length") "0"))
      (unless (peg/match ~(* (some (range "09")) -1) size)
        (reject "400 Bad Request" "invalid content length"))
      (def wanted (scan-number size))
      (def limit (if (index-of (request 0) ["GET" "HEAD"])
                  0 (if body-limit (body-limit (request 1)) max-body)))
      (when (or (not wanted) (> wanted limit))
        (reject "413 Content Too Large" "request body too large"))

      (while (< (- (length carry) split) wanted)
        (def chunk (:read conn 4096 nil 75))
        (if-not chunk (reject "400 Bad Request" "incomplete request body"))
        (buffer/push-string carry chunk))
      (def have (min wanted (- (length carry) split)))
      (def body (string/slice carry split (+ split have)))

      (def rest (string/slice carry (+ split have)))
      (buffer/clear carry)
      (buffer/push-string carry rest)
      {:method (request 0)
       :path (request 1)
       :headers headers

       :origin (headers "origin")

       :close (= "close" (string/ascii-lower (or (headers "connection") "")))
       :body body})))

(defn respond

  [conn status content-type body &opt keep timing]
  (def payload (if (bytes? body) body (string body)))
  (:write conn (string "HTTP/1.1 " status "\r\n"
                       "Content-Type: " content-type "\r\n"
                       "Content-Length: " (length payload) "\r\n"

                       "Cache-Control: no-store\r\n"
                       (if (string/has-prefix? "image/" content-type)
                         "Content-Security-Policy: sandbox; default-src 'none'; style-src 'unsafe-inline'\r\nX-Content-Type-Options: nosniff\r\n"
                         "")
                       (or timing "")
                       (if keep
                         "Connection: keep-alive\r\n"
                         "Connection: close\r\n")
                       "\r\n"
                       payload)))

(defn- answered?

  [port]
  (if-let [probe (try (net/connect "127.0.0.1" (string port)) ([_] nil))]
    (do (try (:close probe) ([_] nil)) true)
    false))

(defn serve

  [port tries handler &opt body-limit]
  (var server nil)
  (var bound nil)
  (for candidate port (+ port tries)
    (unless server
      (unless (answered? candidate)
        (when-let [attempt (try (net/server "127.0.0.1" (string candidate)) ([_] nil))]
          (set server attempt)
          (set bound candidate)))))
  (unless server
    (errorf "no free port in %d-%d" port (+ port tries -1)))

  [server bound
   (fn []
     (net/accept-loop server
         (fn [conn]
           (defer (:close conn)
             (def carry @"")
             (var serving true)
             (var phase :read)
             (try

               (while serving

                 (setdyn :serving nil)
                 (set phase :read)
                 (def request (read-request conn carry bound body-limit))
                 (if-not request
                   (set serving false)
                   (do
                     (set phase :handler)
                     (when trace/enabled (setdyn :latency @{}))
                     (def response (trace/measure "handler" (handler request)))
                     (if (and (dictionary? response) (response :upgrade))
                       (do
                         (set serving false)
                         ((response :upgrade) conn carry))
                       (do
                     (def [status content-type body] response)
                     (def timing
                       (when trace/enabled
                         (string "Server-Timing: "
                           (string/join
                             (map (fn [key] (string key ";dur=" ((dyn :latency) key)))
                                  (keys (dyn :latency))) ", ") "\r\n")))
                     (setdyn :serving (request :path))
                     (set phase :write)
                     (trace/measure "http-write"
                       (respond conn status content-type body (not (request :close)) timing))
                     (when (request :close) (set serving false)))))))
               ([err fib]
                 (cond
                   (and (index-of phase [:read :write])
                        (index-of (string err) ["Broken pipe" "Connection reset by peer" "stream hup"])) nil
                   (and (dictionary? err) (err :http-status))
                   (try (respond conn (err :http-status) "text/plain" (err :message)) ([_] nil))
                   (or (= (string err) "Bad file descriptor")

                         (and (= (string err) "timeout") (nil? (dyn :serving))))

                   (unless (= (string err) "timeout")
                     (eprintf "dropped one reply (%s): connection fd went stale"
                              (or (dyn :serving) "?")))
                   (do

                     (def where (or (dyn :serving) "before-respond"))
                     (eprintf "request failed (%s): %s" where (string err))
                     (debug/stacktrace fib err "  ")
                     (try
                       (respond conn "500 Internal Server Error" "text/plain"
                                (string err))
                       ([_] nil))))))))))])

(defn static-file

  [path]
  (def name (string/slice path 1))
  (when (and (string/has-prefix? "/" path)
             (not (find (fn [part]
                          (or (string/has-prefix? "." part)
                              (not (peg/match ~(* (some (+ (range "az") (range "AZ") (range "09")
                                                          "." "-" "_")) -1)
                                              part))))
                        (string/split "/" name))))
    name))

(def content-types
  {".html" "text/html; charset=utf-8"
   ".js" "text/javascript; charset=utf-8"
   ".mjs" "text/javascript; charset=utf-8"
   ".css" "text/css; charset=utf-8"
   ".svg" "image/svg+xml"
   ".png" "image/png"
   ".jpg" "image/jpeg"
   ".jpeg" "image/jpeg"
   ".gif" "image/gif"
   ".webp" "image/webp"
   ".avif" "image/avif"
   ".bmp" "image/bmp"
   ".ico" "image/x-icon"
   ".tif" "image/tiff"
   ".tiff" "image/tiff"
   ".heic" "image/heic"
   ".heif" "image/heif"
   ".json" "application/json"

   ".ttf" "font/ttf"

   ".wasm" "application/wasm"})

(defn content-type

  [name]
  (def name (string/ascii-lower name))
  (or (some (fn [suffix] (when (string/has-suffix? suffix name)
                           (content-types suffix)))
            (keys content-types))
      "text/plain; charset=utf-8"))
