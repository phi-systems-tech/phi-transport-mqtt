# phi-transport-mqtt

## Overview

phi as an MQTT endpoint. The tenant's devices, channels, rooms and scenes are
published on a broker and commands are accepted there, so Home Assistant,
openHAB, ioBroker, Node-RED and plain scripts can consume a phi box without
speaking `phi-core-ws.v1`.

This is the **northbound** side. It is not the device-side broker traffic
(`zigbee2mqtt/...`, the voice satellites, ThinQ): that stays on the adapter
plane and is untouched by this transport.

Status: phase (a), the model mirror and the commands. Home Assistant discovery
follows (see the end of this file).

## Supported Protocols / Endpoints

- MQTT 3.1.1 client, QoS 1, to one broker: the box broker (`phi-mosquitto`) or a
  foreign one given in the config.
- Plugin type `mqtt`, display name "MQTT endpoint", `libphi_transport_mqtt.so`.
  Runs as a sidecar (`"hosting": "sidecar"`); the same `.so` works in process.

## Network Exposure

The transport opens no port. It dials the broker. What the world sees is what the
broker lets in: on the box `phi-mosquitto` listens on loopback only.

## Authentication & Security

- Towards the broker: an account `phi-hostd` mints for backend `mqtt@main`, or the
  `broker{}` block of the config for a foreign broker. TLS verifies the chain
  always; `broker.tls.caFile` names the authority to trust.
- Towards core the caller identity is `TrustedLocal`, client id `mqtt`: the broker's
  ACL is the credential. That is honest while the broker's writers are the box's
  own accounts or a broker the household configured itself. `readOnly: true`
  subscribes to nothing, so no command can arrive.
- The endpoint never publishes a retained command, and ignores one it is handed
  (a retained `set` is replayed at every subscribe and would switch the device
  again each time).

## Known Issues

- A topic retained by an earlier run for a device that has since gone, or that a new
  `excludeAdapters` entry now leaves out, is cleared only while the transport is
  running when it happens; it does not sweep the broker at start.
- Scene removal has no core event; the scene list is read again every 60 s.

## License

Apache-2.0, see `LICENSE`.

---

## Developer Documentation

### Topic layout

`phi` is `baseTopic`. Ids are core's ids (they do not change when something is
renamed); channels are named by their slug, see below.

| Topic | Payload | Retained |
| --- | --- | --- |
| `phi/bridge/state` | `online` or `offline` (also the will) | yes |
| `phi/bridge/info` | `{version, sdk, instance, tenant}` | yes |
| `phi/bridge/devices` | `[{id, name, room, roomId, class, channels:[{id, slug, name, kind, dataType, unit, min, max, step, writable, momentary, event}]}]` | yes |
| `phi/room/<id>` | `{name, class, floor}` (`floor` is the floor id) | yes |
| `phi/scene/<id>` | `{name, room, roomId}` | yes |
| `phi/scene/<id>/set` | `ON` | command |
| `phi/device/<id>` | `{name, room, roomId, class, <slug>: value, ...}` | yes |
| `phi/device/<id>/availability` | `online` or `offline` (the adapter's link) | yes |
| `phi/device/<id>/<slug>` | the channel's value (state kinds) | yes |
| `phi/device/<id>/<slug>` | `{event_type, code, ts}` (event kinds) | **no** |
| `phi/device/<id>/<slug>/set` | a value, see below | command |
| `phi/device/<id>/set` | `{<slug>: value, ...}`, one invoke per key | command |

QoS 1 everywhere: retained for state and description, not retained for events,
and the command topics are subscribed with QoS 1.

### Payload conventions

- `bridge/state` and `availability` are plain text. Everything else is JSON.
- A channel topic carries a JSON scalar: `true`, `80`, `12.5`, `"hello"` (strings are
  quoted). A colour is `{"r":1,"g":0.5,"b":0}` with components 0..1, as core delivers
  it. An enum is its name as a JSON string when core has one (`"On"`), else its number.
  A flags mask is the array of flag names (`["OverCurrent"]`) when core has them, else
  the number. A composite value (data type Json) is its object.
- A channel that has not reported a value has no topic and no key in the device JSON.
- **Events are not state.** `ButtonEvent`, `SceneTrigger`, `DoorbellRing` and
  `ObjectDetected` (core's history class `event`) are published when they happen,
  not retained, as `{"event_type":"short_press_release","code":12,"ts":1730000000123}`.
  `event_type` is the snake_case of the value's name (`ShortPressRelease`), `code` the
  number, `ts` core's timestamp in ms; a flags value adds `flags` with the names. They
  are never part of the device JSON.
- **Momentary** channels (flag `0x100`, `RemoteKey`) and `Message` channels have a `set`
  topic only: no state topic, no key in the device JSON.
- The channel **slug** is the adapter's `externalId` lowercased with every character
  outside `[a-z0-9_-]` replaced by `_`. Within a device the lowest channel id keeps the
  plain slug; a channel that collides gets `_<channelId>` appended. The names the
  device topic already uses (`name`, `room`, `class`, `set`, `availability`) count as
  taken, so a channel called `set` becomes `set_<channelId>`. `bridge/devices` lists the
  slug of every channel.
- `room` in the device JSON and in `bridge/devices` is the room's name (null when the
  device is in none), `roomId` its id; the household's room wins over the adapter's.
- A device of an excluded adapter is never published. When a device is removed, or
  becomes excluded, every retained topic published for it is cleared with an empty
  retained payload.

### Commands

A payload on a `set` topic is read as JSON when it parses as JSON and as bare text
otherwise, then by the channel's data type. A command on a channel that is not writable,
an excluded or unknown device, or a payload that does not fit is dropped and logged at
Warn; nothing reaches core.

| Data type | Accepted |
| --- | --- |
| Bool | `true`, `false`, `ON`, `OFF` (any case, bare or quoted), or a number (0 is off) |
| Int | a whole number (`50`, `50.0`) |
| Float | a number |
| String | bare text as sent (a trailing newline is dropped), or a JSON string |
| Color | `{"r":1,"g":0.5,"b":0}` or `"1,0.5,0"` / `1,0.5,0` (comma or blanks), components 0..1 |
| Enum | the number, or a name from the channel's `metaAdapter.enumMap` (any case) |
| Flags | the mask as a whole number |
| Json | a JSON object |

`phi/device/<id>/set` takes an object of slugs; each key is one `cmd.channel.invoke`, in the
order written. The keys `name`, `room`, `roomId` and `class` are ignored, so a consumer may
write back what it read. `phi/scene/<id>/set` takes `ON` (or `true`).

A command that core refuses (or fails) is logged at Warn with the reason and the state
is published again, from the rollback value when core names one, else the last value
known, so a consumer sees the truth even where the retained topic already said it.

### Configuration

`/etc/phi/@1/transports/mqtt.json`, merged with the tenant's override by core:

```json
{
  "hosting": "sidecar",
  "stateDir": "/var/lib/phi/@1/transports/mqtt/state",
  "baseTopic": "phi",
  "readOnly": false,
  "excludeAdapters": []
}
```

| Key | Default | Meaning |
| --- | --- | --- |
| `hosting` | | core's key: `sidecar` runs the plugin in `phi-transport-host` |
| `stateDir` | `/var/lib/phi/@1/transports/mqtt/state` | reserved; the transport keeps no state of its own yet |
| `baseTopic` | `phi` | the topic prefix. On the box it must stay `phi`: the broker account is granted `phi/#` and `homeassistant/#` only. With a `broker{}` block it may be anything valid (`home/phi`) |
| `readOnly` | `false` | `true`: subscribe to nothing, so no command can arrive |
| `excludeAdapters` | `[]` | adapter plugin types (`"tuya"`) or adapter ids (`7`) whose devices are never published |
| `broker` | absent | `{host, port, username, password, tls:{caFile}}`: a foreign broker. Used instead of the hostd account. `port` defaults to 1883, or 8883 when `tls` is present; a `tls` block turns TLS on (the chain is always verified, `caFile` names the authority, empty = the system store) |
| `discovery` | absent | `{homeAssistant: false, prefix: "homeassistant"}`: parsed and kept; used by phase (b) |

### The broker account (box broker)

Without `broker{}` the transport asks core for `cmd.host.broker.credentials`, picks the
entry whose `backend` is `mqtt@main` and logs in as that account, client id
`phi-transport-mqtt`. The entry exists once `phi-hostd` has applied the backend intent
`share/intent-backend-mqtt.json` (`kind: mqtt`, `id: main`) that the package installs; until
then the transport asks again every 30 s. The descriptor `share/backend-mqtt.json` gives the
account `phi/#` and `homeassistant/#` besides its own prefix `mqtt_main/#`. The backend unit
`phi-backend-mqtt@main` does nothing; it exists so the host layer has something to mint the
account for.

The endpoint connects after the first full read of the house, so whoever sees `online`
finds the picture complete. If the broker is away it retries every 10 s and, on every
connect, subscribes again and publishes everything again.

### Looking at it

`phi-cli` has no command for broker credentials. The account is in the backend's
environment file, readable by `root` and group `phi`:

```bash
. <(sed 's/^/export /' /var/lib/phi/host/backends/mqtt@main.env)
mosquitto_sub -h 127.0.0.1 -u "$PHI_MQTT_BROKER_USER" -P "$PHI_MQTT_BROKER_PASSWORD" -v -t 'phi/#'
```

(or ask core: `cmd.host.broker.credentials` through any transport, the entry `mqtt@main`).
A command, with the id from `phi/bridge/devices`:

```bash
mosquitto_pub -h 127.0.0.1 -u "$PHI_MQTT_BROKER_USER" -P "$PHI_MQTT_BROKER_PASSWORD" \
    -t phi/device/42/power/set -m ON
mosquitto_pub ... -t phi/device/42/set -m '{"power":true,"brightness":40}'
mosquitto_pub ... -t phi/scene/7/set -m ON
```

With a foreign broker use the account from the `broker{}` block.

### Runtime Model

- A `TransportInterface` plugin on the loop core hands it, via `TransportPluginBase`. The
  MQTT client is `libphi-runtime-mqtt`, driven by the same loop. In process or sidecar:
  sidecar, because the broker is a network peer that may be external and flaky and core
  speaks no MQTT.
- Start: after 50 ms (`start()` has core waiting) `sync.hello.get` (tenant, instance
  label, published in `bridge/info`), then `cmd.adapters.list`, `cmd.rooms.list`,
  `cmd.scenes.list` and `cmd.devices.list`. Events that arrive before the device list are
  buffered (5000, then one full resync) and replayed.
- After that: `event.device.*`, `event.channel.stateChanged`, `event.adapter.*`,
  `event.room.*` and `event.scene.updated` keep the model current. Core sends no event when
  a scene is removed, so the scene list is read every 60 s.
- Code layout: `phi_mqtt_core` (static) holds the model (`model.*`), the topic tree
  (`topics.*`), the payloads (`payload.*`), the command parser (`command.*`) and the ledger
  of retained topics (`retained.*`) - all without broker or core - plus the broker link
  (`broker.*`) and the transport (`mqtttransport.*`). The module is `plugin.cpp` alone.

### Build

Needs `phi-transport-api-dev` (>= 2.2.0), `libphi-runtime-dev`, `libphi-runtime-mqtt-dev`
and `nlohmann-json3-dev`; the integration test needs `/usr/sbin/mosquitto` and skips without.

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

- `mqtt_tests`: slugs, payload shapes per data type, state versus event, the device JSON,
  command parsing, the topic parser, the config, the model's events, the ledger.
- `mqtt_integration_tests`: the plugin against a mosquitto on a free port with a fake core.

### Installation

`usr/lib/<multiarch>/phi/plugins/transports/libphi_transport_mqtt.so`,
`/etc/phi/@1/transports/mqtt.json`, `/usr/share/phi/backends/backend-mqtt.json`,
`/usr/share/phi/intents/backends/intent-backend-mqtt.json`. The package `Suggests:
phi-mosquitto` (the broker may be another machine's).

### Issue Tracker

`https://github.com/phi-systems-tech/phi-transport-mqtt/issues`

## Phase (b): Home Assistant discovery follows

Device-based Home Assistant discovery (`homeassistant/device/phi_<instance>_<id>/config`,
retained, cleared on removal, republished when `homeassistant/status` says `online`) is the
next step. The `discovery` config block is already parsed, the broker account already carries
`homeassistant/#`, and the model (`model.h`) and the ledger (`retained.h`) are what a
`discovery.cpp` builds on without reshaping anything.
