(var- passed 0)
(var- failed @[])
(var- current "")

(defn- begin [name] (set current name))

(defmacro test

  [name & body]
  ~(do (,begin ,name) ,;body))

(defn ok

  [value &opt why]
  (if value
    (++ passed)
    (array/push failed (string current ": " (or why "expected a truthy value"))))
  value)

(defn- frozen

  [value]
  (case (type value)
    :table (struct ;(mapcat |[(frozen $) (frozen (value $))] (keys value)))
    :struct (struct ;(mapcat |[(frozen $) (frozen (value $))] (keys value)))
    :array (tuple ;(map frozen value))
    :tuple (tuple ;(map frozen value))
    :buffer (string value)
    value))

(defn is=

  [expected actual &opt why]
  (if (deep= (frozen expected) (frozen actual))
    (++ passed)
    (array/push failed
                (string current ": " (or why "mismatch")
                        "\n      expected " (string/format "%q" expected)
                        "\n      actual   " (string/format "%q" actual))))
  actual)

(defn report

  []
  (if (empty? failed)
    (printf "  ok  %d assertions" passed)
    (do
      (printf "  FAIL  %d passed, %d failed" passed (length failed))
      (each line failed (print "    - " line))))
  (length failed))
