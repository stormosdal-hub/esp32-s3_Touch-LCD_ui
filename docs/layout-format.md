# Layout format and device API

The screen is described by one JSON document, the *layout*. The web editor
edits it for you, but you can also write it by hand, generate it from a script,
or keep it in git. `main/default_layout.json` is a complete example.

```jsonc
{
  "version": 1,
  "name": "My panel",
  "settings": { ... },   // theme, tab bar, time zone, WebSocket options
  "ports":    { ... },   // RS-485, CAN and MQTT configuration
  "signals":  [ ... ],   // optional initial values
  "tabs":     [ ... ],   // tabs with their widgets
  "inputs":   [ ... ],   // hardware inputs that write signals
  "rules":    [ ... ]    // "when signal X ... do actions"
}
```

Unknown keys are ignored, and missing keys get the defaults listed below.

## Signals

Signals are named values (a number or a text) that connect everything:

* **inputs** (GPIO, ADC, I2C, RS-485, Modbus, CAN, MQTT) write signals,
* WebSocket and REST clients write signals,
* **widgets** show a signal, and user changes write it back (two-way binding),
* **rules** watch signals and run actions,
* every change is pushed to WebSocket clients (and optionally MQTT).

Names are free-form (max 31 characters); a dotted style such as `boiler.temp`
keeps them organised. Text that looks like a number is stored as a number. For
text values, `on`/`true` count as 1 and `off`/`false` as 0 for switches and LEDs.

Built-in signals:

| Signal | Meaning |
|---|---|
| `sys.time`, `sys.date` | Local time `HH:MM:SS` / `YYYY-MM-DD` (after SNTP sync) |
| `sys.ip`, `sys.wifi`, `sys.rssi` | Network address, a status line with the editor URL, WiFi signal (dBm) |
| `sys.heap`, `sys.psram`, `sys.uptime` | Free internal RAM (kB), free PSRAM (kB), seconds since boot |
| `ui.tab` | Active tab index. Set it to switch tabs. |
| `ui.backlight` | 1/0. Set it to turn the screen on/off. |
| `rs485.rx` | Every text line received on RS-485 (raw mode) |
| `can.rx` | Every CAN frame as `ID#DATA` text (when `ports.can.log_raw` is on) |
| `ws.rx` | WebSocket messages that are not one of the commands below |
| `mqtt.connected` | 1 while connected to the broker |

## settings

| Key | Default | Description |
|---|---|---|
| `theme` | `"dark"` | `"dark"` or `"light"` |
| `primary`, `secondary` | `#2196F3`, `#FF9800` | Theme colours |
| `bg` | theme | Screen background colour |
| `font` | 16 | Default font size |
| `tabbar` | `"top"` | `top`, `bottom`, `left`, `right` or `none` |
| `tabbar_size` | 50 | Tab bar height (or width when on the side) in pixels |
| `tab_font` | theme | Font size of the tab titles |
| `swipe` | false | Allow swiping between tabs |
| `screensaver_s` | 0 | Turn the backlight off after this many seconds without touch (0 = never). The first touch only wakes the screen. |
| `ws_signals` | true | Push signal changes to WebSocket clients |
| `ws_events` | true | Push widget events to WebSocket clients |
| `timezone` | `"UTC0"` | POSIX TZ string, e.g. `CET-1CEST,M3.5.0,M10.5.0/3` |

Colours are `"#RRGGBB"` or `"#RGB"`. Fonts are Montserrat in sizes 12, 14, 16,
18, 20, 22, 24, 28, 32, 36, 40 and 48 (other sizes snap to the nearest). The
built-in fonts cover ASCII plus `°` and `•`.

## tabs and widgets

```json
{ "id": "home", "title": "Home", "bg": "#101418", "widgets": [ ... ] }
```

The content area of a tab is 800 × 430 px with the default top tab bar (the
screen minus `tabbar_size`). Widget coordinates are relative to it.

Fields shared by all widgets:

| Key | Description |
|---|---|
| `id` | Unique name, used in events (`{id}`) and WebSocket event messages |
| `type` | See the table below |
| `x`, `y`, `w`, `h` | Position and size in pixels |
| `signal` | Bound signal (not for `chart`/`log`) |
| `format` | printf-style format for displayed values: `%.1f °C`, `%d`, `%s`, `%x`, `%%` |
| `font` | Font size |
| `fg`, `bg`, `accent` | Text colour, background, indicator/knob colour |
| `border_color`, `border_width`, `radius` | Border and corner radius |
| `align` | `left`, `center`, `right` (text alignment) |
| `hidden` | true to hide |
| `events` | List of `{ "on": <event>, "actions": [ ... ] }` |

| Type | Specific keys | Events | Signal binding |
|---|---|---|---|
| `label` | `text`, `center_v` | clicked, long_pressed | Shows the formatted value (`text` until a value arrives) |
| `button` | `text`, `toggle` | clicked, pressed, released, long_pressed, changed | Toggle: on/off state. Otherwise 1 while pressed, 0 when released. |
| `switch` | | changed | 0/1 |
| `checkbox` | `text` | changed | 0/1 |
| `slider` | `min`, `max`, `decimals` | changed, released | Value. Vertical when `h > w`. |
| `arc` | `min`, `max`, `decimals`, `show_value`, `thickness`, `track` | changed, released | Value (draggable knob) |
| `gauge` | `min`, `max`, `decimals`, `thickness`, `track` | clicked | Value (read-only arc with the value in the middle) |
| `bar` | `min`, `max`, `decimals`, `show_value` | clicked | Value |
| `led` | | clicked | Lit when non-zero |
| `dropdown` | `options` (one per line) | changed | Selected index, or the option whose text equals a text signal |
| `chart` | `series` (`[{signal, color}]`, max 4), `min`, `max`, `points`, `sample_ms`, `chart_type` (`line`/`bar`), `legend` | clicked | Each series plots its signal. `sample_ms` 0 = a point per change. |
| `log` | `signals` (`*` or a list), `max_lines`, `newest_first` | | Logs changes of the listed signals (`*` = all except `sys.*`) plus all `log` actions |
| `panel` | `text` (title) | clicked | |
| `input` | `text` (placeholder), `numeric` | submit | Receives the typed text when submitted |

`decimals` (0–3) lets sliders, arcs and bars work with fractional values.

Event values: switches, checkboxes and toggle buttons send 0/1; sliders and arcs
their value; dropdowns the index (option text in `{text}`); inputs the typed
text; plain buttons 1 (`released` sends 0).

## Actions

Actions run in order in a worker task, so slow ones (HTTP, waits, RS-485) never
freeze the screen.

| `type` | Keys |
|---|---|
| `set_signal` | `signal`, `value` (default `{value}`), `mode`: `set` / `toggle` / `add` |
| `ws_send` | `payload`: text sent to every WebSocket client |
| `serial_send` | `data`, `hex` (bool), `append`: `none` / `cr` / `lf` / `crlf` / `modbus_crc` |
| `modbus_write` | `slave`, `fn` (5 coil, 6 register, 16 registers), `reg`, `value`, `format` (for fn 16) |
| `gpio` | `pin`, `level`: `0`, `1`, `toggle` or `{value}` |
| `i2c_write` | `addr`, `data` (hex bytes) |
| `can_send` | `can_id`, `ext`, `data` (hex bytes, up to 8) |
| `mqtt_publish` | `topic`, `payload`, `qos` (0/1), `retain` |
| `http` | `method` (`GET`/`POST`/`PUT`), `url`, `body`, `content_type` |
| `tab` | `tab`: id, title or index |
| `log` | `text`: appended to every log widget |
| `backlight` | `state`: `on` / `off` / `toggle` |
| `delay` | `ms` (max 10000) |

Text fields are templates:

| Placeholder | Expands to |
|---|---|
| `{value}` | The triggering value (number, or text for inputs/text signals) |
| `{text}` | Text value (dropdown option, typed text) or the number |
| `{id}` | Widget or rule id |
| `{time}`, `{date}` | Current local time / date |
| `{sig:NAME}` | Current value of signal NAME |
| `{value:hex}` | Value as one hex byte, e.g. `FF` |
| `{value:hex16}`, `{value:hex16le}` | Two hex bytes, big / little endian |
| `{value:int}`, `{value:.2}` | Rounded integer / fixed decimals |

The modifiers also work on signals: `{sig:temp:.1}`. Serial text accepts
`\r \n \t \\ \xHH` escapes. Example Modbus frame built by hand:
`"data": "01 06 00 10 {value:hex16}", "hex": true, "append": "modbus_crc"`.

## inputs

Common keys: `id`, `type`, `signal`, `enabled` (default true), `scale` and
`offset` (signal = raw × scale + offset).

| `type` | Keys | Notes |
|---|---|---|
| `gpio` | `pin`, `pull` (`up`/`down`/`none`), `invert`, `debounce_ms` | Signal is 0/1 |
| `adc` | `pin`, `mode` (`mv`/`raw`), `period_ms` | GPIO6 is the "Sensor" header |
| `i2c` | `addr`, `reg` (omit for a plain read), `format`, `len`, `period_ms`, `speed_hz` | Shares the touch bus (GPIO8/9) |
| `serial` | `mode`: `line`, `kv`, `json` | RS-485 in raw mode. `line`: whole line → `signal`. `kv`: `name=value` lines set `<signal>name`. `json`: each key of an object. |
| `modbus` | `slave`, `fn` (1–4), `reg`, `format`, `period_ms` | RS-485 in Modbus mode |
| `can` | `can_id`, `ext`, `start`, `format` | `format: "hex"` stores the data bytes as text |
| `mqtt` | `topic` (`+`/`#` wildcards), `json_key` | Payload number/text, or one key of a JSON payload |

Binary formats: `u8 s8 u16 s16 u32 s32 f32` (big endian), with `le` suffix for
little endian (`u16le`), and `bit0`…`bit7` of the first byte. Modbus formats:
`u16 s16 u32 s32 f32`, and `u32r s32r f32r` for low-word-first devices.

GPIO inputs/outputs are limited to pins listed in `BOARD_USER_GPIOS`
(`main/board/board_config.h`); everything else on this board is used by the
display, touch, PSRAM or flash.

## rules

```json
{ "id": "too_hot", "signal": "boiler.temp", "op": ">", "value": 80,
  "actions": [ { "type": "mqtt_publish", "topic": "alarm/boiler", "payload": "{value}" } ] }
```

`op` is `change` (every change), `>`, `<`, `>=`, `<=`, `==` or `!=`. Comparison
rules fire when the condition *becomes* true, not on every update while it
stays true. `==`/`!=` compare text when `value` is a non-numeric string.

## ports

```json
"ports": {
  "rs485": { "enabled": true, "baud": 9600, "parity": "none", "stop_bits": 1,
             "mode": "raw", "timeout_ms": 200, "idle_ms": 100 },
  "can":   { "enabled": false, "bitrate": 500000, "log_raw": false },
  "mqtt":  { "enabled": false, "uri": "mqtt://192.168.1.10", "username": "", "password": "",
             "client_id": "", "publish_prefix": "" }
}
```

* RS-485 `mode`: `raw` (text lines, `serial` inputs, `serial_send`) or `modbus`
  (Modbus RTU master: `modbus` inputs are polled, `modbus_write` actions).
  `idle_ms` ends a line after that much silence when no newline arrives.
* CAN uses GPIO19/20, which are shared with the native USB port. Enabling CAN
  switches them to the CAN transceiver through the CH422G.
* MQTT `publish_prefix`: if set, every signal change is published (retained) to
  `<prefix>/<signal name>`, which is handy for Home Assistant or Node-RED.

## WebSocket: `ws://<device>/ws`

Device → client (JSON text frames):

```json
{"signals": {"temp": 21.5, "mode": "Auto"}}                  // on connect: all values; then changes
{"event": "clicked", "widget": "btn1", "value": 1}            // widget events (settings.ws_events)
{"event": "changed", "widget": "mode", "value": 1, "text": "Manual"}
```

plus whatever your `ws_send` actions send.

Client → device:

```json
{"set": {"temp": 21.5, "status": "OK"}}    // set signals
{"get": 1}                                 // reply with all signal values
{"log": "Backup finished"}                 // append to log widgets
{"tab": "settings"}                        // switch tab (id, title or index)
```

Plain text `name=value` lines also set signals; any other text lands in
`ws.rx`. Browser pages from other sites are refused (Origin check); programs
such as Python scripts or Node-RED connect normally.

Signals created by remote peers (WebSocket, REST, serial `kv`/`json`) are
limited so they cannot fill the table: of the 512 signal slots, the last 128
are kept for names used by the layout. Example with Python (`pip install websockets`):

```python
import asyncio, json, websockets

async def main():
    async with websockets.connect("ws://lcd-ui.local/ws") as ws:
        await ws.send(json.dumps({"set": {"boiler.temp": 64.5}}))
        async for msg in ws:
            print(msg)

asyncio.run(main())
```

## REST API

| Method and path | Description |
|---|---|
| `GET /api/layout` | Current layout |
| `POST /api/layout` | Validate, save and apply a layout (the previous one becomes the backup) |
| `POST /api/layout/rollback` | Swap current and previous layout |
| `GET /api/layout/default` | Built-in example layout |
| `GET /api/signals` / `POST /api/signals` | Read all signals / set signals from a JSON object |
| `GET /api/info` | Firmware, memory, network, storage and port status |
| `GET /api/wifi/scan`, `POST /api/wifi` | Scan / set `{ssid, password, hostname}` |
| `POST /api/ota` | Upload a firmware `.bin` (raw body); the device restarts |
| `POST /api/reboot` | Restart |
| `GET /api/screenshot.bmp` | What the screen shows right now |

POST requests must include the header `X-LCD-UI` (any value), for example:

```bash
curl -X POST -H "X-LCD-UI: 1" --data-binary @layout.json http://lcd-ui.local/api/layout
```

This, together with checks of the `Host` and `Origin` headers, stops other web
pages open in your browser from changing the device (cross-site requests and
DNS rebinding). Address the device by IP address or by its hostname
(`lcd-ui`, `lcd-ui.local`, `lcd-ui.<anything>`). The API sends no CORS headers,
so pages served from elsewhere cannot use it from a browser.

There is no password. Keep the device on a trusted network.
