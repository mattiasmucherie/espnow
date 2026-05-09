# espnow

Monorepo for an ESP-NOW → MQTT → Home Assistant setup.
One bridge (ESP32, WiFi+MQTT). Many battery sensors (ESP32-C3, ESP-NOW only).

## Layout

```
.
├── platformio.ini        # all envs in one file
├── shared/
│   ├── secrets.h.example # template — committed
│   ├── secrets.h         # real secrets — gitignored
│   └── espnow_packet.h   # packet structs shared by bridge + senders
├── bridge/               # esp32dev — receives ESP-NOW, publishes MQTT
└── sensors/
    └── test_sender/      # esp32-c3-devkitm-1 — sends a counter every 3s
```

## Setup

1. `cp shared/secrets.h.example shared/secrets.h` and fill in real values.
2. Flash bridge first; note its MAC + WiFi channel from serial.
3. Put MAC + channel back into `shared/secrets.h` (`BRIDGE_MAC`, `ESPNOW_CHANNEL`).
4. Flash a sensor.

## Build / flash

```sh
pio run -e bridge -t upload
pio run -e test_sender -t upload
pio device monitor -e bridge
```

## Adding a sensor

Bridge does NOT need to be touched or reflashed. Sensors self-describe; bridge auto-publishes HA discovery on first sight.

1. `mkdir sensors/<name> && cp sensors/test_sender/main.cpp sensors/<name>/main.cpp`
2. Edit the new `main.cpp`: change `SENSOR_ID`, `SENSOR_NAME`, `SENSOR_MODEL`, and the `v`/`m` fields in `loop()`.
3. Add to `platformio.ini`:
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
4. Flash. New device + entities show up in HA.

## Wire format

ESP-NOW payload is JSON. Contract documented in `shared/espnow_packet.h`.
```json
{"id":"<slug>","name":"<Display>","model":"<Model>",
 "v":{"<metric>":<number>},
 "m":{"<metric>":{"u":"<unit>","i":"mdi:<icon>","dc":"<device_class>"}}}
```
- `id`, `v` required. `name`, `model`, `m` optional.
- Bridge publishes values to `espnow/<id>/<metric>`.
- HA discovery is republished on first sight per bridge boot — retained MQTT makes it idempotent.
