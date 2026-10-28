(import ../websocket :as ws)
(import ../json)
(import ./harness :as t)

(defn- masked [payload &opt opcode final]
  (default opcode 1)
  (default final true)
  (def out (buffer (ws/frame opcode payload)))
  (unless final (put out 0 (band (out 0) 127)))
  (def n (band (out 1) 127))
  (def head (case n 126 4 127 10 2))
  (put out 1 (bor (out 1) 128))
  (def prefix (buffer/slice out 0 head))
  (def mask [17 34 51 68])
  (buffer/push-byte prefix ;mask)
  (eachp [i byte] payload (buffer/push-byte prefix (bxor byte (mask (% i 4)))))
  (string prefix))

(t/test "websocket handshake uses the RFC accept key"
  (t/is= "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=" (ws/accept-key "dGhlIHNhbXBsZSBub25jZQ=="))
  (t/is= "qZk+NkcGgWq6PiVxeFDCbJzQ2J0=" (json/base64 (ws/sha1 "abc")))
  (t/ok (ws/upgrade? {:method "GET" :headers {"upgrade" "WebSocket" "connection" "keep-alive, Upgrade"
                       "sec-websocket-version" "13" "sec-websocket-key" "dGhlIHNhbXBsZSBub25jZQ=="}}))
  (t/ok (not (ws/upgrade? {:method "GET" :headers {}}))))

(t/test "masked frames retain exact payloads and consume one frame"
  (each size [0 1 125 126 127 128 65535 65536]
    (def text (string/repeat "x" size))
    (def packet (masked text))
    (t/is= [(length packet) 1 true text] (ws/decode-frame (string packet packet)))
    (t/is= nil (ws/decode-frame (string/slice packet 0 -2))))
  (t/is= [9 9 true "abc"] (ws/decode-frame (masked "abc" 9)))
  (t/is= [7 1 false "x"] (ws/decode-frame (masked "x" 1 false)))
  (t/is= [7 0 true "y"] (ws/decode-frame (masked "y" 0))))

(t/test "invalid masking, lengths, control frames, and UTF-8 are rejected"
  (each packet [(ws/frame 1 "unmasked") (masked "bad" 9 false)
                (masked (string/repeat "x" 126) 9)
                (string/from-bytes 129 255 0 0 0 0 0 16 0 0)]
    (t/ok (try (do (ws/decode-frame packet) false) ([_] true))))
  (each text ["" "plain" "é" "😀"] (t/ok (ws/utf8? text)))
  (each text ["\xc0\xaf" "\xed\xa0\x80" "\xf4\x90\x80\x80" "\x80" "\xe2\x82"]
    (t/ok (not (ws/utf8? text)))))

(t/test "large reconnect snapshots drain before subsequent output"
  (def incoming (ev/chan 1))
  (def writes @[])
  (def large (string/repeat "x" 2131805))
  (def followup "next snapshot")
  (var closed false)
  (def connection
    {:write (fn [_ bytes &opt timeout]
              (unless (string/has-prefix? "HTTP/" bytes)
                (ev/sleep 0.01)
                (array/push writes bytes)
                (when (= 2 (length writes)) (ev/give incoming (masked "" 8)))))
     :read (fn [&] (ev/take incoming))
     :close (fn [&] (set closed true))})
  (ws/serve connection @"" {:headers {"sec-websocket-key" "dGhlIHNhbXBsZSBub25jZQ=="}}
    (fn [send]
      (send large)
      (send followup)
      {:message (fn [&]) :close (fn [&])}))
  (t/is= (ws/frame 2 large) (get writes 0))
  (t/is= (ws/frame 2 followup) (get writes 1))
  (t/ok closed))
