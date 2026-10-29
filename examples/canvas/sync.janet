# Each native host owns one signed namespace and follows explicitly selected peers.
# Received updates remain in their native replica stores; they are never republished
# as this host's authoritative state.
(import ./build/canvas_store :as store)
(import ../../janet-utils/json)

(def endpoint "147.182.181.182:7777")
(defn key? [value]
  (and (string? value) (peg/match ~(* (repeat 64 (+ (range "09") (range "af"))) -1) value)))
(defn mkdir [path] (unless (os/stat path) (os/mkdir path)))
(defn read-json [path] (when (os/stat path) (json/decode (slurp path))))
(defn write-json [path value]
  (with [f (file/open (string path ".next") :w)] (file/write f (json/encode value)))
  (os/rename (string path ".next") path))

(defn open [directory address identity-alias peer-keys]
  (def parts (string/split ":" address))
  (unless (= 2 (length parts)) (error "Use --sync numeric-IPv4:port (or off)."))
  (def port (scan-number (parts 1)))
  (unless (and port (<= 1 port 65535)) (error "Invalid sync port."))
  (def config-path (string directory "/native-host.json"))
  (def previous (read-json config-path))
  (when (and previous identity-alias (not= identity-alias (previous "identity")))
    (error "This data directory belongs to a different identity. Use a separate --data directory."))
  (def alias (or identity-alias (get previous "identity")
    (string "turtle-canvas-" (string/join (map |(string/format "%02x" $) (os/cryptorand 16))))))
  (def identity (store/identity alias))
  (def public-key (store/public-key identity))
  (when (and previous (not= public-key (previous "publicKey"))) (error "Native identity changed."))
  (def keys (distinct (filter |(not= $ public-key) (or peer-keys (get previous "peers") []))))
  (unless (and (<= (length keys) 32) (every? (map key? keys))) (error "Peers must be up to 32 public keys."))
  (write-json config-path {"identity" alias "publicKey" public-key "peers" keys "endpoint" address})
  (def owner-path (string directory "/owner"))
  (mkdir owner-path)
  (def owner (store/open owner-path public-key))
  (def replicas @[])
  (def network (store/network (parts 0) port))
  (try (do
    (store/attach network owner identity)
    (mkdir (string directory "/replicas"))
    (each key keys
      (def path (string directory "/replicas/" key))
      (mkdir path)
      (def db (store/open path key))
      (array/push replicas {:key key :db db})
      (store/attach network db))
    {:owner owner :identity identity :key public-key :replicas replicas :network network
     :endpoint address :peers keys})
    ([err]
      (store/network-close network)
      (each replica replicas (store/close (replica :db)))
      (store/close owner)
      (error err))))

(defn close [session]
  (store/network-close (session :network))
  (each replica (session :replicas) (store/close (replica :db)))
  (store/close (session :owner)))

(defn poll [session]
  (def state (store/pump (session :network)))
  (def changed @[])
  (each replica (session :replicas)
    (when (store/changed? (replica :db))
      (array/push changed {:key (replica :key) :records (store/records (replica :db))})))
  {:state state :changed changed})

(defn migrate [directory session]
  (def legacy-path (string directory "/store"))
  (def marker (string directory "/migrated-native.json"))
  (when (and (os/stat (string legacy-path "/data.mdb")) (not (os/stat marker)))
    (def existing @{})
    (each [_ text] (store/records (session :owner))
      (put existing ((json/decode text) "id") true))
    (def legacy (store/open legacy-path))
    (defer (store/close legacy)
      (each [_ text] (store/records legacy)
        (def record (table ;(kvs (json/decode text))))
        (unless (get existing (record "id"))
          (put record "writer" (session :key))
          (store/put (session :owner) 0 (json/encode record)))))
    (write-json marker {"publicKey" (session :key)})))
