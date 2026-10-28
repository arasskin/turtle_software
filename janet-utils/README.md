# Janet utilities

Reusable modules extracted from Visualize, with no npm packages or native extension
required by the modules themselves. Import a module using a path relative to your
Janet source file.

| Module | API |
| --- | --- |
| `http.janet` | Bounded request parsing, localhost server, keep-alive, response and static-path helpers |
| `websocket.janet` | Upgrade validation, framing, fragmentation, ping/pong, and connection serving |
| `json.janet` | JSON encoding/decoding and base64 encoding |
| `trace.janet` | Optional bounded timing samples and event-loop lag measurement |

`http/serve` returns `[listener port accept-loop]`; the application provides a handler
returning `[status content-type body]` or an upgrade callback. It binds to loopback.
The application still owns routing, authorization, state, and the browser UI.
Host validation alone is not application authorization; follow Visualize's session-token
and Origin checks before exposing local operations to a browser.

```janet
(import ./http)
(import ./json)

(defn handle [request]
  (case (request :path)
    "/api/hello" ["200 OK" "application/json" (json/encode {:message "Hello from Janet"})]
    ["404 Not Found" "text/plain" "Not found"]))

(def [listener port run] (http/serve 8780 10 handle))
(printf "http://127.0.0.1:%d/api/hello" port)
(run)
```

This example assumes the file is next to these modules. The HTTP and WebSocket
modules can be used together through the `:upgrade` response callback. These utilities
are not a generic Datafriend binding or an application packaging system yet.

Run `janet janet-utils/tests/run.janet` from the repository root. The extracted tests cover
the reusable modules; tests of Visualize's actual HTML/assets and its patched Janet
Unix-socket runtime remain in Visualize.
Tracing retains the `VISUALIZE_TRACE=1` environment variable for compatibility.
The modules and extracted tests retain the license in `LICENSE`.
