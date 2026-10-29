(import ../../janet-utils/http)
(import ../../janet-utils/websocket :as ws)
(import ../../janet-utils/json)
(import ./build/canvas_store :as store)
(import ./room)
(import ./sync)

(defn now [] (math/floor (* 1000 (os/time))))
(defn random-key [] (string/join (map |(string/format "%02x" $) (os/cryptorand 32))))
(defn mkdir [path] (unless (os/stat path) (os/mkdir path)))
(defn inactive [record]
  (def next (table ;(kvs record)))
  (put next "active" false)
  (put next "online" false)
  (update next "version" inc)
  next)

(defn run [args storage listen accept-loop]
  (var host "127.0.0.1")
  (var port 8787)
  (var directory "build/data")
  (var sync-address (if (storage :native) sync/endpoint "off"))
  (var identity-alias nil)
  (var peer-keys nil)
  (var offline false)
  (when (or (index-of "--help" args) (not (zero? (% (length args) 2))))
    (print "Usage: ./run [--port 8787] [--data directory] [--sync IPv4:port|off] [--identity alias] [--peers key,key] [--offline true|false] [--host 127.0.0.1|0.0.0.0]\n       ./run --print-key identity-alias")
    (os/exit (if (index-of "--help" args) 0 1)))
  (for group 0 (/ (length args) 2)
    (def i (* group 2))
    (case (args i)
      "--host" (set host (args (+ i 1)))
      "--port" (set port (scan-number (args (+ i 1))))
      "--data" (set directory (args (+ i 1)))
      "--sync" (set sync-address (args (+ i 1)))
      "--identity" (set identity-alias (args (+ i 1)))
      "--peers" (set peer-keys (filter |(not (empty? $)) (string/split "," (args (+ i 1)))))
      "--offline" (do
        (unless (index-of (args (+ i 1)) ["true" "false"]) (error "--offline expects true or false"))
        (set offline (= "true" (args (+ i 1)))))
      (error (string "Unknown option: " (args i)))))
  (unless (and (number? port) (= port (math/floor port)) (<= 1 port 65535)) (error "Invalid port."))
  (mkdir directory)
  (def key-path (string directory "/invite-key"))
  (def key (if (os/stat key-path) (string/trim (slurp key-path)) (random-key)))
  (unless (os/stat key-path)
    (with [file (file/open key-path :w)] (file/write file key))
    (os/chmod key-path 8r600))
  (unless (peg/match ~(* (repeat 64 (+ (range "09") (range "af"))) -1) key)
    (error "Invalid invite-key file."))
  (def db-path (string directory "/store"))
  (mkdir db-path)
  (def session (unless (= sync-address "off") (sync/open directory sync-address identity-alias peer-keys)))
  (def db (if session (session :owner) ((storage :open) db-path)))
  (var running true)
  (defer (do (set running false) (if session (sync/close session) ((storage :close) db)))
    (when session
      (when offline (store/online (session :network) false))
      (sync/migrate directory session))
    (def entity-ids @{})
    (def loaded @[])
    (each [entity text] ((storage :records) db)
      (def record (json/decode text))
      (put entity-ids (record "id") entity)
      (array/push loaded record))
    (def board (room/restore loaded))
    (def own-key (when session (session :key)))
    (defn import-record [source text]
      (def record (json/decode text))
      (def old (get (board :records) (record "id")))
      (when (or (not old) (> (record "version") (old "version")))
        (room/accept board (record "author") record (get record "seenAt" 0) (fn [_]) source true)))
    (when session
      (each replica (session :replicas)
        (each [_ text] (store/records (replica :db))
          (import-record (replica :key) text))))
    (defn persist [record]
      (put entity-ids (record "id")
        ((storage :put) db (get entity-ids (record "id") 0) (json/encode record))))
    # Presence is a persisted record too. Restart marks previous cursors inactive.
    (each record (values (board :records))
      (when (and (= "cursor" (record "kind")) (record "online") (= own-key (record "writer")))
        (room/accept board (record "author") (inactive record) (now) persist own-key)))
    (def peers @{})
    (def cursor-peers @{})
    (var next-peer 0)
    (defn drop-peer [peer]
      (unless (peer :closed)
        (put peer :closed true)
        (ev/chan-close (peer :out))
        (try (:close (peer :conn)) ([_] nil))))
    (defn enqueue [peer text]
      (unless (peer :closed)
        (if (or (ev/full (peer :out)) (> (+ (peer :bytes) (length text)) 2097152))
          (drop-peer peer)
          (do
            (update peer :bytes + (length text))
            (ev/give (peer :out) text)))))
    (defn publish [record]
      (def text (json/encode {"type" "record" "record" record}))
      (each peer (values peers) (enqueue peer text)))
    (var sync-state {"enabled" (truthy? session) "connected" false "publicKey" own-key
                    "endpoint" sync-address "peers" (if session (session :peers) [])})
    (when session
      (ev/go (fn []
        (var previous-status "")
        (var last-status 0)
        (while running
          (try (do
            (def result (sync/poll session))
            (each changed (result :changed)
              (each [_ text] (changed :records)
                (when-let [record (import-record (changed :key) text)] (publish record))))
            (def state (result :state))
            (set sync-state {"enabled" true "connected" (state :connected) "settled" (state :settled)
              "error" (state :error) "publicKey" own-key "endpoint" sync-address "peers" (session :peers)
              "sent" (state :sent) "received" (state :received)})
            (def status (json/encode {"type" "sync" "state" sync-state}))
            (when (and (not= status previous-status) (> (- (now) last-status) 1000))
              (set previous-status status) (set last-status (now))
              (each peer (values peers) (enqueue peer status))))
            ([err] (eprintf "Native sync: %s" err)))
          (ev/sleep 0.016)))))
    (defn write-record [actor record]
      (def saved (room/accept board actor record (now) persist own-key))
      (publish saved)
      saved)
    (defn connect [conn carry request]
      (ws/serve conn carry request
        (fn [send]
          (++ next-peer)
          (def peer @{:id next-peer :conn conn :out (ev/chan 128) :bytes 0 :closed false :actor nil :cursor nil})
          (ev/go (fn [] (ev/sleep 10) (unless (peer :actor) (drop-peer peer))))
          {:message
           (fn [text]
             (try (do
               (def message (json/decode text))
               (unless (dictionary? message) (error "Expected a record message."))
               (if-not (peer :actor)
                 (do
                   (unless (and (= "hello" (message "type")) (= key (message "key"))
                                (room/identifier? (message "actor")) (room/identifier? (message "cursor")))
                     (error "This invite link is not valid."))
                   (when (>= (length peers) 64) (error "This example supports 64 connected people."))
                   (put peer :actor (message "actor"))
                   (put peer :cursor (message "cursor"))
                   (put peers (peer :id) peer)
                   (put cursor-peers (peer :cursor) peer)
                   # Snapshot is immutable; any subsequent changes queue behind it.
                   (def snapshot (array ;(values (board :records))))
                   (ev/go (fn []
                     (try (do
                       (send (json/encode {"type" "snapshot-start"}) 1)
                       (for group 0 (math/ceil (/ (length snapshot) 16))
                         (def offset (* group 16))
                         (send (json/encode {"type" "snapshot" "records"
                           (array/slice snapshot offset (min (+ offset 16) (length snapshot)))}) 1))
                       (send (json/encode {"type" "ready" "time" (now) "sync" sync-state}) 1)
                       (while (not (peer :closed))
                         (when-let [out (ev/take (peer :out))]
                           (send out 1)
                           (update peer :bytes - (length out)))))
                       ([_] (drop-peer peer))))))
                 (case (message "type")
                   "put"
                   (do
                     (def record (message "record"))
                     (when (and (dictionary? record) (= "cursor" (record "kind"))
                                (not= (peer :cursor) (record "id")))
                       (error "Unexpected cursor ID."))
                     (write-record (peer :actor) record))
                   (error "Unknown message."))))
               ([err]
                 (if (peer :actor)
                   (enqueue peer (json/encode {"type" "error" "message" (string err)}))
                   (do (try (send (json/encode {"type" "error" "fatal" true "message" (string err)}) 1) ([_] nil))
                       (ev/go (fn [] (ev/sleep 0.1) (drop-peer peer))))))))
           :close
           (fn []
             (drop-peer peer)
             (put peers (peer :id) nil)
             (when (= peer (get cursor-peers (peer :cursor)))
               (put cursor-peers (peer :cursor) nil)
               (when-let [cursor (get (board :records) (peer :cursor))]
                 (try (write-record (peer :actor) (inactive cursor))
                   ([err] (eprintf "Could not save disconnected cursor: %s" err))))))})))
    (def server (listen host (string port)))
    (print "\nCommon ground — native Datafriend + Janet")
    (printf "Open http://127.0.0.1:%d/#key=%s" port key)
    (when session
      (printf "Littlebear: %s · Native identity: %s" sync-address own-key)
      (printf "Following %d native hosts. Add this public key to their --peers list." (length (session :peers))))
    (unless (= host "127.0.0.1")
      (printf "For other devices, replace 127.0.0.1 with this computer's LAN address (port %d)." port))
    (printf "Records: %d · Data: %s\n" (length (board :records)) directory)
    (file/flush stdout)
    (defer (:close server)
      (accept-loop server
        (fn [conn]
          (defer (:close conn)
            (try (do
              (def carry @"")
              (def request (http/read-request conn carry nil (fn [_] 0)))
              (when request
                (def path (request :path))
                (if (= path "/sync")
                  (if (and (ws/upgrade? request)
                           (= (request :origin) (string "http://" ((request :headers) "host"))))
                    (connect conn carry request)
                    (http/respond conn "403 Forbidden" "text/plain" "Use the invite link from this host."))
                  (if (and (= "GET" (request :method))
                           (index-of path ["/" "/app.mjs" "/model.mjs" "/style.css" "/health"]))
                    (if (= path "/health")
                      (http/respond conn "200 OK" "application/json"
                        (json/encode {"ok" true "sync" sync-state "records" (length (board :records))}))
                      (let [name (if (= path "/") "index.html" (string/slice path 1))]
                        (http/respond conn "200 OK" (http/content-type name) (slurp (string "public/" name)) false
                          "Referrer-Policy: no-referrer\r\nX-Content-Type-Options: nosniff\r\nContent-Security-Policy: default-src 'self'; connect-src 'self'; style-src 'self'; img-src 'self' blob: data:; frame-ancestors 'none'; base-uri 'none'\r\n")))
                    (http/respond conn "404 Not Found" "text/plain" "Not found")))))
              ([err]
                (try (http/respond conn "400 Bad Request" "text/plain" "Invalid request") ([_] nil))
                (eprintf "Connection ended: %s" err)))))))))

(defn main [script & args]
  (if (and (= 2 (length args)) (= "--print-key" (args 0)))
    (print (store/public-key (store/identity (args 1))))
    (run args {:open store/open :put store/put :records store/records :close store/close :native true}
         net/server net/accept-loop)))
