(import ../room)
(import ../../../janet-utils/json)
(import ../../../janet-utils/tests/harness :as t)

(defn stroke [id actor points &opt version]
  {"kind" "stroke" "id" id "author" actor "version" (or version 1)
   "color" "#608cae" "width" 5 "points" points "complete" false "deleted" false})
(def board (room/create))
(def persisted @[])
(defn persist [r] (array/push persisted r))
(defn put-record [actor r] (room/accept board actor r 1000 persist))
(defn rejects [thunk] (try (do (thunk) false) ([_] true)))

(t/test "Overlapping writers retain independent records and a stable order"
  (def a (put-record "alice-001" (stroke "stroke-001" "alice-001" [[10 10] [20 20]])))
  (def b (put-record "bob-00001" (stroke "stroke-002" "bob-00001" [[10 20] [20 10]])))
  (t/is= 2 (length (board :records)))
  (t/is= [1 2] [(a "order") (b "order")])
  (def next (put-record "alice-001" (stroke "stroke-001" "alice-001" [[10 10] [20 20] [30 30]] 2)))
  (t/is= 1 (next "order"))
  (t/is= 5 (board :points))
  (t/is= 2 (length (board :records))))

(t/test "Retries and older versions do not duplicate or roll back a stroke"
  (def before (length persisted))
  (put-record "alice-001" (stroke "stroke-001" "alice-001" [[10 10]]))
  (t/is= before (length persisted))
  (t/is= 3 (length (((board :records) "stroke-001") "points"))))

(t/test "Cursors are persisted updatable records, including departure"
  (def cursor @{"kind" "cursor" "id" "cursor-001" "version" 1
    "name" "Alice" "color" "#608cae" "x" 12 "y" 20 "active" true "online" true})
  (put-record "alice-001" cursor)
  (put cursor "x" 30) (put cursor "version" 2)
  (put-record "alice-001" cursor)
  (put cursor "active" false) (put cursor "online" false) (put cursor "version" 3)
  (put-record "alice-001" cursor)
  (t/is= 30 (((board :records) "cursor-001") "x"))
  (t/is= false (((board :records) "cursor-001") "online"))
  (t/is= 3 (length (board :records))))

(t/test "An undo hides only its author's record and stays hidden on retry"
  (def hidden (table ;(kvs ((board :records) "stroke-001"))))
  (put hidden "version" 3) (put hidden "deleted" true) (put hidden "complete" true)
  (t/ok (rejects (fn [] (put-record "bob-00001" hidden))))
  (put-record "alice-001" hidden)
  (put hidden "version" 4) (put hidden "deleted" false)
  (t/is= true ((put-record "alice-001" hidden) "deleted"))
  (t/is= false (((board :records) "stroke-002") "deleted")))

(t/test "Validation and failed storage leave the current record intact"
  (t/ok (rejects (fn [] (put-record "alice-001" (stroke "stroke-003" "alice-001" [[-1 0]])))))
  (t/ok (rejects (fn [] (put-record "alice-001" (stroke "stroke-001" "alice-001" [[0 0]] 5)))))
  (def before (length (board :records)))
  (t/ok (rejects (fn [] (room/accept board "alice-001" (stroke "stroke-004" "alice-001" [[1 1]]) 0
    (fn [_] (error "disk full"))))))
  (t/is= before (length (board :records)))
  (t/is= 2 (board :order)))

(t/test "Restart restores every record with the same overlap order"
  (def copy (room/restore (json/decode (json/encode (values (board :records))))))
  (t/is= (board :records) (copy :records))
  (t/is= (board :points) (copy :points))
  (t/is= 3 ((room/accept copy "bob-00001" (stroke "stroke-005" "bob-00001" [[1 1]]) 0 (fn [_])) "order")))

(t/test "Independent writers can assign equal orders and converge after exchange"
  (def left (room/create))
  (def right (room/create))
  (def a (room/accept left "alice-001" (stroke "native-a1" "alice-001" [[1 1]]) 0 (fn [_]) "writer-a"))
  (def b (room/accept right "bob-00001" (stroke "native-b1" "bob-00001" [[1 1]]) 0 (fn [_]) "writer-b"))
  (t/is= [1 1] [(a "order") (b "order")])
  (room/accept left "bob-00001" b 0 (fn [_]) "writer-b" true)
  (room/accept right "alice-001" a 0 (fn [_]) "writer-a" true)
  (t/is= (left :records) (right :records))
  (t/is= (left :records) ((room/restore (values (left :records))) :records))
  (t/ok (rejects (fn [] (room/accept left "alice-001"
    (stroke "native-a1" "alice-001" [[1 1] [2 2]] 2) 0 (fn [_]) "writer-b")))))

(os/exit (if (zero? (t/report)) 0 1))
