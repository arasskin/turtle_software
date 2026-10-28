(import ../json)
(import ./harness :as t)

(t/test "base64 encodes empty, padded and binary payloads"
  (each [plain encoded] [["" ""] ["f" "Zg=="] ["fo" "Zm8="]
                         ["foo" "Zm9v"] ["foob" "Zm9vYg=="]
                         ["fooba" "Zm9vYmE="] ["foobar" "Zm9vYmFy"]
                         ["\x00\xff" "AP8="]]
    (t/is= encoded (json/base64 plain))
    (t/is= encoded (json/base64 (buffer plain)))))

(t/test "scalars encode as JSON, not as Janet"
  (t/is= "null" (json/encode nil))
  (t/is= "true" (json/encode true))
  (t/is= "false" (json/encode false))
  (t/is= "42" (json/encode 42) "a whole number keeps no decimal point")
  (t/is= `"hi"` (json/encode "hi")))

(t/test "a table keyed by NUMBER keeps its values"

  (t/is= `{"1":"bad colour","3":"unknown verb"}`
         (json/encode @{1 "bad colour" 3 "unknown verb"})))

(t/test "a string cannot break out of the <script> block it is embedded in"

  (def encoded (json/encode "</script><img src=x onerror=alert(1)>"))
  (t/ok (not (string/find "</script>" encoded)))
  (t/ok (not (string/find "<" encoded)) "no raw < survives, escaped as \\u003c")
  (t/ok (not (string/find ">" encoded)))

  (t/is= "</script><img src=x onerror=alert(1)>" (json/decode encoded)))

(t/test "control characters and quotes are escaped"
  (t/is= `"a\nb"` (json/encode "a\nb"))
  (t/is= `"say \"hi\""` (json/encode `say "hi"`))
  (t/is= `"back\\slash"` (json/encode "back\\slash")))

(t/test "decoding reads what the page sends"
  (def got (json/decode `{"action":"run","index":3,"lines":["hide ~.A",""]}`))
  (t/is= "run" (got "action"))
  (t/is= 3 (got "index"))
  (t/is= ["hide ~.A" ""] (got "lines")))

(t/test "decoding handles the escapes encoding produces"
  (t/is= "</script>" (json/decode `"</script>"`)
         "what encode wrote, decode reads back")
  (t/is= "a\nb" (json/decode `"a\nb"`))
  (t/is= ["x"] (json/decode `["x"]`))
  (t/is= {} (json/decode "{}"))
  (t/is= [] (json/decode "[]")))

(t/test "a round trip through both is the identity"
  (def original {"lines" ["only ~" "box ~.A red"]
                 "problems" {"0" "went wrong"}
                 "error" ""
                 "svg" "<svg><path d=\"M0,0\"/></svg>"})
  (t/is= original (json/decode (json/encode original))))

(t/test "escaping is byte-exact for every byte and every pair"

  (defn reference [text]
    (def out @"\"")
    (each byte text
      (cond
        (= byte (chr "\"")) (buffer/push-string out "\\\"")
        (= byte (chr "\\")) (buffer/push-string out "\\\\")
        (= byte (chr "\n")) (buffer/push-string out "\\n")
        (= byte (chr "\r")) (buffer/push-string out "\\r")
        (= byte (chr "\t")) (buffer/push-string out "\\t")
        (= byte (chr "<")) (buffer/push-string out "\\u003c")
        (= byte (chr ">")) (buffer/push-string out "\\u003e")
        (= byte (chr "&")) (buffer/push-string out "\\u0026")
        (< byte 0x20) (buffer/push-string out (string/format "\\u%04x" byte))
        (buffer/push-byte out byte)))
    (buffer/push-string out "\"")
    (string out))

  (var singles 0)
  (for b 0 256
    (def s (string/from-bytes b))
    (unless (= (reference s) (json/encode s)) (++ singles)))
  (t/is= 0 singles "all 256 bytes encode identically")

  (var pairs 0)
  (for a 0 256
    (for b 0 256
      (def s (string/from-bytes a b))
      (unless (= (reference s) (json/encode s)) (++ pairs))))
  (t/is= 0 pairs "and all 65536 adjacent pairs, so no run boundary is wrong")

  (def hairy (string "plain " (string/from-bytes 0x1b) "[0m <script>&\"\\ "
                     (string/from-bytes 0) "end"))
  (t/is= hairy (json/decode (json/encode hairy))))

(t/test "MCP JSON rejects malformed input and decodes surrogate pairs"
  (t/is= "🚀" (json/decode `"\ud83d\ude80"`))
  (each invalid [`"unfinished` `"\q"` `"\u123"` `"\ud83d"` `"\ude80"`
                 `"\ud83d\u1234"` `{"a":1} trailing` `01` `+1` `1.` `[1,]` `{a:1}`]
    (t/ok (try (do (json/decode invalid) false) ([_] true)) invalid)))
