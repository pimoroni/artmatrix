# Modules

## plugin_config

See [Settings](plugins.md#settings).

## fetch

`from fetch import AsyncFetch, HTTPException`

| | |
|---|---|
| `AsyncFetch(host, port=None, use_tls=True, debug=False, buffer_size=2048, max_bytes=262144, timeout=10, step_bytes=4096)` | Fetches from the web server `host` a little at a time, so the display keeps running. `port` defaults to 443, or 80 with `use_tls=False`. `timeout` is in seconds. |
| `.fetch(path, file=None, interval=None, headers=None, data=None, method="GET", blocking=False)` | Sets what to fetch. Nothing is sent until `update()`. `path` is the path on the server, not a full URL. `file` saves the response to a file instead of memory. `interval` fetches again every `interval` seconds. `blocking=True` fetches straight away, and stops the display until it's done. |
| `.update()` | Call it every frame. Returns `AsyncFetch.DONE` on the frame a fetch succeeds, `AsyncFetch.ERROR` on the frame `on_error` handles a failed one, `AsyncFetch.FETCHING` while one is under way, and `AsyncFetch.IDLE` otherwise. |
| `.on_complete(fn)` | Calls `fn(fetch)` when a fetch succeeds. Also works as a decorator. |
| `.on_error(fn)` | Calls `fn(fetch)` when the server returns a status other than 200. Return `True` from `fn` to stop `update()` raising `HTTPException`. Also works as a decorator. |
| `.to_json()` | Returns the response parsed as JSON. |
| `.body` | The response's raw bytes. The next fetch overwrites them, including one started by `interval`, so use `bytes(fetch.body)` to keep them. Raises `ValueError` if the response was saved to a file. |
| `.stream` | The response, opened for reading like a file, with `.read()`, `.readline()` or a `for` loop over its lines. It's the saved file if `file` was used, and otherwise a copy of the response. |
| `.http_status` | The response's status code, such as 200. |
| `.http_response_headers` | The response's headers, as a dict. |
| `.status` | What the last `update()` returned. |
| `.duration` | Milliseconds since the last fetch finished. |
| `.reset()` | Cancels the current fetch and closes the connection. |
| `.finish()` | Waits until the fetch is done. Stops the display while it waits. |
| `HTTPException` | Raised by `update()` when the server returns a status other than 200. `.fetch` is the `AsyncFetch` that raised it. |

`update()` also raises `OSError` when the network fails, then waits 30 seconds before trying again. Catch both exceptions, or they count as errors in your plugin.

## timezones

`import timezones`

| | |
|---|---|
| `timezones.localtime()` | Returns the local time in the owner's time zone, including daylight saving, as a tuple like `time.localtime()`'s. |
| `timezones.utc_offset(now)` | Returns how many minutes local time is ahead of UTC at `now`, a time in seconds such as `time.time()`. |
| `timezones.offset_label(minutes)` | Returns a label such as `"UTC+5:30"`. |

`time.localtime()` gives UTC on Art Matrix.

## scrolling

`from scrolling import Scroll`

| | |
|---|---|
| `Scroll(text, font_face=None, font_size=None, target=None, speed=25, gap=None, align="middle")` | A line of text that scrolls from right to left. The arguments are the same as for [`text.scroll`](https://badgewa.re/docs/api/text.md). |
| `scroll()` | Calling a `Scroll` draws the text and moves it on. Returns how many times it has scrolled through. |
| `.update(progress=None)` | Moves the text on without drawing it. Pass `progress`, the number of times it has scrolled through, to set the position yourself. |
| `.render(offset=None)` | Draws the text where it is, moved by `offset`, a `vec2`. |
| `.reset()` | Starts the scroll again. |
| `.scroll_time` | The number of seconds to scroll through once. |
| `.target` | The image it draws into. Set it to `screen` every frame. |

## easing

`from easing import easeOutCubic`

| | |
|---|---|
| `linear(x)`, `easeIn*(x)`, `easeOut*(x)`, `easeInOut*(x)` | Each takes a number from 0 to 1 and returns the eased value, which runs from 0 to 1. Replace `*` with `Quad`, `Cubic`, `Quart`, `Quint`, `Sine`, `Expo`, `Circ`, `Back`, `Elastic` or `Bounce`. |

## Not for plugins

`wifi`, `ezwifi`, `provisioning`, `splash`, `phew` and `config` belong to Art Matrix itself.
