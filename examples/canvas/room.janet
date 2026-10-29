# Every visible object is a native record, including cursors and unfinished lines.
# A stroke has one writer and a stable ID; overlapping strokes never replace each other.
(def width 1600)
(def height 1000)
(def max-points 1024)
(def max-records 20000)
(def max-total-points 500000)

(defn identifier? [value]
  (and (string? value) (<= 8 (length value) 100)
       (peg/match ~(* (some (+ (range "az") (range "AZ") (range "09") "-" "_")) -1) value)))
(defn color? [value]
  (and (string? value)
       (peg/match ~(* "#" (repeat 6 (+ (range "09") (range "af") (range "AF"))) -1) value)))
(defn point? [p]
  (and (indexed? p) (= 2 (length p))
       (number? (p 0)) (number? (p 1))
       (<= 0 (p 0) width) (<= 0 (p 1) height)))
(defn stroke? [record]
  (and (color? (record "color")) (number? (record "width")) (<= 1 (record "width") 32)
       (boolean? (record "complete")) (boolean? (record "deleted"))
       (indexed? (record "points")) (<= 1 (length (record "points")) max-points)
       (every? (map point? (record "points")))))
(defn create [] @{:records @{} :order 0 :points 0})

(defn accept [room actor input now persist &opt writer imported]
  (unless (and (dictionary? input) (identifier? actor) (identifier? (input "id"))
               (number? (input "version")) (<= 1 (input "version") 9007199254740991)
               (= (input "version") (math/floor (input "version"))))
    (error "Invalid drawing record."))
  (def old ((room :records) (input "id")))
  (when (and old writer (not= writer (old "writer")))
    (error "That record belongs to another native host."))
  (when (and imported (not= writer (input "writer"))) (error "Invalid record writer."))
  (when (and imported (= "stroke" (input "kind"))
             (not (and (number? (input "order")) (<= 1 (input "order") 9007199254740991)
                       (= (input "order") (math/floor (input "order"))))))
    (error "Invalid stroke order."))
  (when (and old (not= actor (old "author"))) (error "That record belongs to another person."))
  (when (and old (<= (input "version") (old "version"))) (break old))
  (when (and (not old) (>= (length (room :records)) max-records))
    (error "This canvas is full. Export it and start a new data directory."))
  (def kind (input "kind"))
  (when (and old (not= kind (old "kind"))) (error "A record cannot change kind."))
  (var added-points 0)
  (def record
    (case kind
      "stroke"
      (do
        (unless (stroke? input) (error "Invalid stroke."))
        (when old
          (unless (and (= (input "color") (old "color")) (= (input "width") (old "width"))
                       (>= (length (input "points")) (length (old "points"))))
            (error "A stroke can only be extended or hidden."))
          (eachp [i p] (old "points")
            (unless (deep= p ((input "points") i)) (error "Existing stroke points cannot change."))))
        (set added-points (- (length (input "points")) (if old (length (old "points")) 0)))
        (when (> (+ (room :points) added-points) max-total-points)
          (error "This canvas has reached its point limit."))
        {"kind" "stroke" "id" (input "id") "author" actor "version" (input "version")
         "color" (input "color") "width" (input "width") "points" (input "points")
         "complete" (or (input "complete") (and old (old "complete")) false)
         "deleted" (or (input "deleted") (and old (old "deleted")) false)
         "order" (if old (old "order") (if imported (input "order") (+ 1 (room :order))))
         "writer" writer})
      "cursor"
      (do
        (unless (and (point? [(input "x") (input "y")]) (color? (input "color"))
                     (string? (input "name")) (<= 1 (length (input "name")) 48)
                     (boolean? (input "active")) (boolean? (input "online")))
          (error "Invalid cursor."))
        {"kind" "cursor" "id" (input "id") "author" actor "version" (input "version")
         "x" (input "x") "y" (input "y") "color" (input "color") "name" (input "name")
         "active" (input "active") "online" (input "online") "seenAt" now "writer" writer})
      (error "Unknown record kind.")))
  # No event-loop yield between persistence and making a record visible.
  (persist record)
  (put (room :records) (record "id") record)
  (when (= kind "stroke")
    (update room :points + added-points)
    (put room :order (max (room :order) (record "order"))))
  record)

(defn restore [records]
  (def room (create))
  # Preserve the original overlap order even after updates/restarts.
  (each record records
    (accept room (record "author") record (get record "seenAt" 0) (fn [_]) (record "writer") true))
  room)
