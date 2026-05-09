#pragma once

// ESP-NOW wire format (JSON, UTF-8, no trailing null):
//
// {
//   "id":    "<slug>",          // REQUIRED. Used in topics + HA unique_ids.
//   "name":  "<Display Name>",  // optional. HA device name. Falls back to id.
//   "model": "<Model>",         // optional. HA device model.
//   "v":     { "<metric>": <number>, ... },                  // REQUIRED, non-empty.
//   "m":     { "<metric>": { "u":"<unit>",                   // optional, per-metric.
//                            "i":"<mdi:icon>",
//                            "dc":"<device_class>" } }
// }
//
// Bridge re-publishes HA discovery on first sight per boot of any new
// (id) and (id,metric). Retained MQTT means re-publishes are idempotent.

#define ESPNOW_MAX_JSON 250
