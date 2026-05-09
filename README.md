# espnow

DIY ESP-NOW → MQTT → Home Assistant pipeline. Monorepo of one bridge and N sensors,
all in a single PlatformIO project.

## How it works

```
 ┌─────────────────┐                  ┌──────────────────┐               ┌──────────────┐
 │  ESP32-C3       │   ESP-NOW LR     │  ESP32 bridge    │     MQTT      │ Home         │
 │  battery sensor │ ──── JSON ────►  │  STA + ESP-NOW   │ ───retained─► │ Assistant    │
 │  deep sleep     │   (~150 bytes)   │  receiver        │               │ (auto-       │
 │  30 min cycle   │                  │  + HA discovery  │               │  discovers)  │
 └─────────────────┘                  └──────────────────┘               └──────────────┘
```

- **Sensor** wakes, reads, builds JSON, sends one ESP-NOW unicast frame, sleeps.
  Sleep dominates the duty cycle — wake takes ~1s.
- **Bridge** runs always-on. ESP-NOW receive callback memcpys raw bytes; the
  main loop parses JSON, publishes each metric to `espnow/<id>/<metric>`, and
  emits HA MQTT-discovery configs the first time it sees a new `(id, metric)`
  pair per boot. Discovery is retained, so HA remembers across bridge reboots.
- **Self-describing wire format** — sensors carry their own id, name, model,
  metric names, units, icons. Bridge has zero per-sensor code. Adding a sensor
  is sensor-side only; the bridge stays untouched.

The bridge also publishes its own `connectivity` and `uptime` entities under
device `espnow_bridge`.

## Wire format

```json
{
  "id":    "soil_moisture",
  "name":  "Soil Moisture",
  "model": "ESP32-C3 Soil Moisture",
  "v":     { "moisture": 60 },
  "m":     { "moisture": { "u": "%", "i": "mdi:water-percent", "dc": "humidity" } }
}
```

- `id`, `v` required; `name`, `model`, `m` optional.
- `v[k]` → MQTT topic `espnow/<id>/<k>`.
- `m[k]` carries optional HA discovery metadata: `u`nit, `i`con, `d`evice
  `c`lass. Defaults are sensible if absent.
- Contract + `ESPNOW_MAX_JSON` constant live in `shared/espnow_packet.h`.

## ESP-NOW Long Range mode

Used because default rates couldn't cover the full house range.

- Sensor protocol mask: `WIFI_PROTOCOL_LR` only (sensors never associate
  with a router).
- Bridge protocol mask: `B | G | N | LR` — must be set **before**
  `WiFi.begin()`. Setting LR post-association silently drops the AP link.
- Battery cost of LR is negligible at 30-min cadence.

Battery sensors retry up to 3× per cycle (50ms gap, 200ms TX-confirm timeout)
before sleeping, so single-frame losses are absorbed.

## Repo layout

```
.
├── platformio.ini           # all envs (bridge + every sensor)
├── shared/
│   ├── secrets.h.example    # committed template
│   ├── secrets.h            # gitignored, real values
│   └── espnow_packet.h      # protocol contract
├── bridge/main.cpp          # ESP32 — ESP-NOW receive → MQTT + HA discovery
└── sensors/
    ├── test_sender/         # bench tool: counter every 3s, no sleep
    ├── soil_moisture/       # plant probe + deep sleep 30 min
    └── lemon_moisture/      # ditto, separate identity
```

## Setup

1. `cp shared/secrets.h.example shared/secrets.h` and fill real values.
2. Flash the bridge first; note MAC + channel from serial.
3. Put `BRIDGE_MAC` + `ESPNOW_CHANNEL` into `shared/secrets.h`.
4. Flash a sensor.

## Build / flash

```
pio run -e bridge          -t upload -t monitor --upload-port <bridge-port>  --monitor-port <bridge-port>
pio run -e soil_moisture   -t upload -t monitor --upload-port <sensor-port>  --monitor-port <sensor-port>
```

Ctrl-T then Ctrl-C exits the monitor.

### Flashing a deep-sleeping sensor

Native USB CDC drops during deep sleep, so the port closes mid-handshake.
Force the C3 into ROM download mode:

1. Hold **BOOT** (GPIO9)
2. Tap **RESET**
3. Release BOOT

USB now stays alive indefinitely. Upload, then press RESET to boot the new firmware.

## Adding a sensor

Bridge stays untouched.

1. `cp -r sensors/soil_moisture sensors/<name>` (or `test_sender` for a non-sleeping template).
2. Edit `SENSOR_ID`, `SENSOR_NAME`, `SENSOR_MODEL`, and the `v`/`m` payload.
3. Add an env to `platformio.ini`:
   ```ini
   [env:<name>]
   board = esp32-c3-devkitm-1
   build_src_filter = -<*> +<sensors/<name>/>
   build_flags =
       ${env.build_flags}
       -DARDUINO_USB_MODE=1
       -DARDUINO_USB_CDC_ON_BOOT=1
   lib_deps =
       bblanchon/ArduinoJson@^7
   ```
4. Flash. The new device + its entities auto-appear in HA on first packet.
