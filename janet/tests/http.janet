(import ../http)
(import ../websocket :as ws)
(import ./harness :as t)

(defn- request-head [method path headers]
  (string method " " path " HTTP/1.1\r\n" headers "\r\n"))

(defn- rejected [head &opt limit]
  (var reads 0)
  (def status
    (try
      (do (http/read-request {:read (fn [&] (++ reads) nil)} (buffer head) 8770 limit) nil)
      ([err] (if (dictionary? err) (err :http-status) (error err)))))
  (t/is= 0 reads "invalid headers must be rejected before reading any body")
  status)

(t/test "only the bound localhost authorities are accepted"
  (each host ["127.0.0.1:8770" "localhost:8770" "LOCALHOST:8770"]
    (t/ok (http/local-host? host 8770)))
  (each host [nil "" "attacker.example:8770" "127.0.0.1" "localhost:8771"
              "localhost:8770.attacker.example" "user@localhost:8770" "127.0.0.2:8770"]
    (t/ok (not (http/local-host? host 8770))))
  (each path ["/" "/session" "/terminal?k=token"]
    (t/is= "403 Forbidden"
      (rejected (request-head "GET" path "Host: attacker.example:8770\r\n")))))

(t/test "HTTP headers are bounded with and without a terminator"
  (t/is= "431 Request Header Fields Too Large"
    (rejected (string "GET / HTTP/1.1\r\nX: " (string/repeat "x" http/max-header))))
  (t/is= "431 Request Header Fields Too Large"
    (rejected (request-head "GET" "/" (string "X: " (string/repeat "x" http/max-header) "\r\n")))))

(t/test "invalid and ambiguous body lengths are rejected before buffering"
  (each value ["-1" "+1" "1.5" "1e6" "0x10" "NaN" "" "1, 1"]
    (t/is= "400 Bad Request"
      (rejected (request-head "POST" "/config"
        (string "Host: localhost:8770\r\nContent-Length: " value "\r\n")))))
  (each headers ["Content-Length: 1\r\nContent-Length: 1\r\n"
                 "Content-Length: 0\r\nTransfer-Encoding: chunked\r\n"
                 "Transfer-Encoding: chunked\r\n"
                 "Host: attacker.example:8770\r\n"]
    (t/is= "400 Bad Request"
      (rejected (request-head "POST" "/config" (string "Host: localhost:8770\r\n" headers)))))
  (t/is= "403 Forbidden" (rejected (request-head "GET" "/session" "")))
  (t/is= "413 Content Too Large"
    (rejected (request-head "POST" "/config" "Host: localhost:8770\r\nContent-Length: 1048577\r\n")))
  (t/is= "413 Content Too Large"
    (rejected (request-head "POST" "/config" "Host: localhost:8770\r\nContent-Length: 999999999999999999999999999999999999\r\n")))
  (t/is= "413 Content Too Large"
    (rejected (request-head "GET" "/terminal" "Host: localhost:8770\r\nContent-Length: 1\r\n")))
  (t/is= "413 Content Too Large"
    (rejected (request-head "POST" "/errors?k=token" "Host: localhost:8770\r\nContent-Length: 16385\r\n")
      (fn [path] (t/is= "/errors?k=token" path) 16384))))

(t/test "an HTTP body at the limit is preserved along with the next request"
  (def body (string/repeat "x" http/max-body))
  (def next (request-head "GET" "/session" "Host: localhost:8770\r\n"))
  (def carry (buffer (request-head "POST" "/config"
    (string "Host: localhost:8770\r\nContent-Length: " (length body) "\r\n")) body next))
  (def request (http/read-request nil carry 8770))
  (t/is= body (request :body))
  (t/is= next (string carry))
  (t/is= "/session" ((http/read-request nil carry 8770) :path))
  (t/is= "" (string carry)))

(t/test "EOF cannot turn a partial body into a valid request"
  (def carry (buffer (request-head "POST" "/config" "Host: localhost:8770\r\nContent-Length: 5\r\n") "ab"))
  (t/is= "400 Bad Request"
    (try (do (http/read-request {:read (fn [&] nil)} carry 8770) nil)
      ([err] (err :http-status)))))

(t/test "coalesced WebSocket data does not count toward the HTTP header limit"
  (def handshake (request-head "GET" "/terminal?k=token"
    (string "Host: localhost:8770\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
            "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n")))
  (def packet (buffer (ws/frame 1 (string/repeat "x" 65536))))
  (put packet 1 (bor (packet 1) 128))
  (def frame (string (string/slice packet 0 10) "\0\0\0\0" (string/slice packet 10)))
  (each initial [true false]
    (var reads 0)
    (def carry (if initial (buffer handshake frame) @""))
    (def request (http/read-request
      {:read (fn [&] (++ reads) (string handshake frame))} carry 8770))
    (t/is= (if initial 0 1) reads)
    (t/ok (ws/upgrade? request))
    (t/is= "" (request :body))
    (t/is= frame (string carry))
    (t/is= (string/repeat "x" 65536) (get (ws/decode-frame carry) 3))))

(t/test "a plain filename in web/ is served"
  (t/is= "term.js" (http/static-file "/term.js"))
  (t/is= "app.js" (http/static-file "/app.js"))
  (t/is= "style.css" (http/static-file "/style.css"))
  (t/is= "index.html" (http/static-file "/index.html")))

(t/test "nested static assets are served relative to the static roots"
  (t/is= "terminal/term.js" (http/static-file "/terminal/term.js"))
  (t/is= "graph/camera.js" (http/static-file "/graph/camera.js"))
  (t/is= "fonts/Parkinsans-Regular.ttf" (http/static-file "/fonts/Parkinsans-Regular.ttf")))

(t/test "static paths cannot traverse directories or contain empty segments"

  (t/is= nil (http/static-file "/../visualize_config"))
  (t/is= nil (http/static-file "/../../etc/passwd"))
  (t/is= nil (http/static-file "/terminal/../../src.server/core.janet"))
  (t/is= nil (http/static-file "/terminal/../app.js"))
  (t/is= nil (http/static-file "/terminal/./term.js"))
  (t/is= nil (http/static-file "/terminal//term.js"))
  (t/is= nil (http/static-file "/terminal/"))
  (t/is= nil (http/static-file "terminal/term.js"))
  (t/is= nil (http/static-file "/..\\windows"))
  (t/is= nil (http/static-file "/")))

(t/test "a dotfile is not servable"
  (t/is= nil (http/static-file "/.gitignore"))
  (t/is= nil (http/static-file "/.env"))
  (t/is= nil (http/static-file "/terminal/.env"))
  (t/is= nil (http/static-file "/.git/config")))

(t/test "odd characters are refused rather than interpreted"

  (t/is= nil (http/static-file "/term%2Ejs"))
  (t/is= nil (http/static-file "/terminal/%2e%2e/app.js"))
  (t/is= nil (http/static-file "/terminal%2fterm.js"))
  (t/is= nil (http/static-file "/term.js?k=1"))
  (t/is= nil (http/static-file "/term js"))
  (t/is= nil (http/static-file "/term;js"))
  (t/is= nil (http/static-file "/$(whoami)")))

(t/test "javascript is labelled so a browser will execute it"

  (t/ok (string/has-prefix? "text/javascript" (http/content-type "term.js")))
  (t/ok (string/has-prefix? "text/javascript" (http/content-type "app.mjs")))
  (t/ok (string/has-prefix? "text/css" (http/content-type "style.css")))
  (t/ok (string/has-prefix? "text/html" (http/content-type "index.html")))
  (t/ok (string/has-prefix? "text/plain" (http/content-type "notes.txt"))
        "an unknown extension falls back rather than guessing"))

(t/test "image responses preserve binary data and sandbox active formats"
  (each [extension mime] [["PNG" "image/png"] ["jpg" "image/jpeg"] ["JPEG" "image/jpeg"]
                          ["gif" "image/gif"] ["webp" "image/webp"] ["avif" "image/avif"]
                          ["svg" "image/svg+xml"] ["bmp" "image/bmp"] ["ico" "image/x-icon"]
                          ["tif" "image/tiff"] ["tiff" "image/tiff"] ["heic" "image/heic"] ["heif" "image/heif"]]
    (t/is= mime (http/content-type (string "image name." extension))))
  (var response "")
  (def payload "\x00\xff\x89PNG\r\n")
  (http/respond {:write (fn [_ bytes] (set response bytes))} "200 OK" "image/png" payload)
  (t/ok (string/has-suffix? payload response))
  (t/ok (string/find (string "Content-Length: " (length payload) "\r\n") response))
  (http/respond {:write (fn [_ bytes] (set response bytes))} "200 OK" "image/svg+xml" "<svg/>")
  (t/ok (string/find "Content-Security-Policy: sandbox; default-src 'none'" response))
  (t/ok (string/find "X-Content-Type-Options: nosniff" response)))

(t/test "a connection serves many requests, and close still means close"

  (def handler (fn [req] ["200 OK" "text/plain" (string "echo:" (req :path))]))
  (def [server port accept-loop] (http/serve 8941 5 handler))
  (ev/go accept-loop)
  (def conn (net/connect "127.0.0.1" (string port)))
  (defn ask [path & extra]
    (:write conn (string "GET " path " HTTP/1.1\r\nHost: 127.0.0.1:" port "\r\n"
                         (string/join extra "") "\r\n"))
    (var reply @"")
    (var tries 0)
    (while (and (< tries 40) (not (string/find (string "echo:" path) (string reply))))
      (++ tries)
      (when-let [chunk (:read conn 4096 nil 1)]
        (buffer/push-string reply chunk)))
    (string reply))
  (def first-reply (ask "/one"))
  (t/ok (string/find "echo:/one" first-reply) "the first request is answered")
  (t/ok (string/find "keep-alive" first-reply) "and the connection is offered onward")
  (t/ok (string/find "echo:/two" (ask "/two"))
        "a second request on the SAME connection is answered")
  (def parting (ask "/three" "Connection: close\r\n"))
  (t/ok (string/find "echo:/three" parting) "a request asking to close is answered")
  (t/ok (string/find "Connection: close" parting) "and told the connection ends")
  (:close conn)
  (:close server))

(t/test "two servers walk to two ports, despite SO_REUSEPORT"

  (def handler (fn [_] ["200 OK" "text/plain" "a"]))
  (def [one port-one loop-one] (http/serve 8931 5 handler))
  (def [two port-two loop-two] (http/serve 8931 5 handler))
  (t/ok (not= port-one port-two)
        "the second server must not share the first one's port")
  (t/is= (inc port-one) port-two "it lands on the very next port")
  (:close one)
  (:close two))

(t/test "abandoned replies close normally and other clients remain usable"
  (def entered (ev/chan 1))
  (def release (ev/chan 1))
  (def events (ev/chan 8))
  (def [server port accept-loop]
    (http/serve 8941 5
      (fn [request]
        (if (= (request :path) "/abandon")
          (do (ev/give entered true) (ev/take release)
              ["200 OK" "text/plain" (string/repeat "x" (* 4 1024 1024))])
          ["200 OK" "text/plain" "still serving"]))))
  (def accepting (ev/go accept-loop nil events))
  (defer (do (:close server) (ev/take events))
    (defn connect [path]
      (def conn (net/connect "127.0.0.1" (string port)))
      (:write conn (request-head "GET" path
        (string "Host: localhost:" port "\r\nConnection: close\r\n")))
      conn)
    (def abandoned (connect "/abandon"))
    (ev/take entered)
    (:close abandoned)
    (ev/give release true)
    (def [signal completed] (ev/take events))
    (t/is= :ok signal)
    (t/ok (not= accepting completed))
    (def conn (connect "/"))
    (defer (:close conn)
      (def reply @"")
      (forever
        (def chunk (:read conn 4096 nil 2))
        (unless chunk (break))
        (buffer/push-string reply chunk))
      (t/ok (string/has-prefix? "HTTP/1.1 200 OK" reply))
      (t/ok (string/find "still serving" reply)))
    (t/is= :ok (first (ev/take events)))))

(t/test "HTTP rejections close the connection without dispatching a queued request"
  (var calls 0)
  (def [server port accept-loop]
    (http/serve 8941 5
      (fn [_] (++ calls) ["200 OK" "text/plain" "accepted"])
      (fn [_] 16)))
  (ev/go accept-loop)
  (defer (:close server)
    (def host (string "Host: localhost:" port "\r\n"))
    (def queued (request-head "GET" "/session" host))
    (each [status head]
      [["403 Forbidden" (request-head "GET" "/session" "Host: attacker.example\r\n")]
       ["413 Content Too Large" (request-head "POST" "/errors" (string host "Content-Length: 17\r\n"))]
       ["400 Bad Request" (request-head "POST" "/config" (string host "Content-Length: -1\r\n"))]
       ["431 Request Header Fields Too Large" (request-head "GET" "/" (string host "X: " (string/repeat "x" http/max-header) "\r\n"))]]
      (def conn (net/connect "127.0.0.1" (string port)))
      (defer (:close conn)
        (:write conn (string head queued))
        (def reply @"")
        (forever
          (def chunk (try (:read conn 4096 nil 2)
            ([err] (if (= (string err) "Connection reset by peer") nil (error err)))))
          (unless chunk (break))
          (buffer/push-string reply chunk))
        (t/ok (string/has-prefix? (string "HTTP/1.1 " status) reply) (string "expected " status ", received " (string/format "%q" reply)))
        (t/ok (string/find "Connection: close" reply) (string "expected close for " status))
        (t/is= nil (string/find "accepted" reply))))
    (t/is= 0 calls)))

(t/test "closing a listener ends its accept task without a failed or phantom connection"
  (for iteration 0 6
    (def events (ev/chan 8))
    (def [server port accept-loop] (http/serve 8941 5 |["200 OK" "text/plain" "ok"]))
    (def accepting (ev/go accept-loop nil events))
    (ev/sleep 0)
    (when (even? iteration)
      (def conn (net/connect "127.0.0.1" (string port)))
      (:write conn "GET / HTTP/1.1\r\n")
      (:close conn)
      (def [signal client-task] (ev/take events))
      (t/is= :ok signal "an incomplete request ends normally at EOF")
      (t/ok (not= accepting client-task)))
    (:close server)
    (def [signal completed] (ev/take events))
    (t/is= :ok signal "closing the listener completes its accept loop")
    (t/is= accepting completed)))

