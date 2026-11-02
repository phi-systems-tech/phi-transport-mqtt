# phi-transport-mqtt

## Overview

phi as an MQTT endpoint. The tenant's devices, channels, rooms and scenes are
published on a broker and commands are accepted there, so Home Assistant,
openHAB, ioBroker, Node-RED and plain scripts can consume a phi box without
speaking `phi-core-ws.v1`.

This is the **northbound** side. It is not the device-side broker traffic
(`zigbee2mqtt/...`, the voice satellites, ThinQ): that stays on the adapter
plane and is untouched by this transport.

Status: phases (a) and (b): the model mirror, the commands, and Home Assistant
device-based discovery (see "Home Assistant" below). Media players, cameras and
device effects are phase (d).

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
  drops every `set` subscription, so no command can arrive (it still listens to Home
  Assistant's status topic, which is not a command).
- The endpoint never publishes a retained command, and ignores one it is handed
  (a retained `set` is replayed at every subscribe and would switch the device
  again each time).

## Known Issues

- The start sweep clears everything retained under `baseTopic`/ that the transport does
  not own. Two phi boxes on one foreign broker therefore need two different `baseTopic`
  values, or each clears the other's topics at every start.
- A clean `stop()` flushes "offline" before it disconnects; a backlog of more than the
  socket buffer queued just before (a very large house replayed right before a stop) could
  still keep it from going out. The will covers a connection that dies.
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

### Home Assistant

`discovery.homeAssistant: true` publishes Home Assistant's device-based discovery (format of
Home Assistant 2024.11): one retained config per phi device,
`<prefix>/device/phi_<box>_<deviceId>/config`, and one for all scenes,
`<prefix>/device/phi_<box>_scenes/config`. openHAB 5 reads the same topics through its Home
Assistant binding. The per-entity discovery format is never published.

```json
"discovery": { "homeAssistant": true, "prefix": "homeassistant" }
```

**Ids.** `<box>` is the machine's hostname (lowercase, anything outside `[a-z0-9_-]` turned
into `_`) + `_` + the tenant, e.g. `pi5_1`: the hostname is stable where the instance label
is free text. The device's object id and `dev.ids[0]` are `phi_<box>_<deviceId>`, a
component's `unique_id` is `phi_<box>_<deviceId>_<slug>`, a scene's
`phi_<box>_scene_<sceneId>`. Core ids and slugs do not change on a rename, so neither does
anything Home Assistant has stored.

**What a payload carries.** `dev` (`ids`, `name`, `mf`, `mdl`, `sw` from the device's
manufacturer, model and firmware, `sa` the room's name), `o` (`{name: "phi", sw, url}`),
`availability` with **both** `phi/bridge/state` and `phi/device/<id>/availability`
(`availability_mode: "all"`, `online` / `offline`: a dead transport leaves its retained
states standing, an unreachable adapter leaves the bridge up), and `cmps`, one component per
channel that maps (key = the channel's slug). The scenes device is available on the bridge
alone. A payload never depends on a channel's value, so a state change publishes no
discovery. A device with no component Home Assistant could show gets no payload (and one
published earlier is cleared); a device of an excluded adapter gets none.

**Channel -> component.**

| phi | Home Assistant | notes |
| --- | --- | --- |
| a `Light` device's `PowerOnOff` (writable Bool) with `Brightness`, `ColorRGB`, `ColorTemperature` | one `light` (default schema) | state and command topic per attribute; `brightness_scale` 100; colour `{"r","g","b"}` 0..1 on the wire, converted by `rgb_value_template` / `rgb_command_template`; colour temperature in mired, `min_kelvin` / `max_kelvin` from the channel's range |
| `PowerOnOff` elsewhere, other writable Bool | `switch` | `device_class: outlet` for a `Plug`; the first one is the device's main entity (`name: null`) |
| `Brightness`, `Volume`, any other writable Int / Float | `number` | `min` / `max` / `step` from the channel; no range: 0..100 for brightness and volume, else a wide `box` |
| `Contact`, `Motion`, `Tamper`, `Presence` (read-only Bool) | `binary_sensor` | `opening` / `motion` / `tamper` / `presence`; phi `Contact` true = open = ON. Any other read-only Bool: `binary_sensor` without a class |
| `Temperature`, `Humidity`, `Illuminance`, `Battery`, `CO2`, `Power`, `Voltage`, `Current`, `Energy`, `LinkQuality`, `SignalStrength` | `sensor` | `device_class`, the class's unit, `state_class` `measurement` (`total_increasing` for `Energy`); `Battery`, `LinkQuality`, `SignalStrength` are `diagnostic` |
| any other read-only Int / Float | `sensor` | the channel's unit, `measurement` |
| String, enum and flags state | `sensor` (string) | JSON-quoted on the topic, so `value_template: "{{ value_json }}"`; flags are joined. No `device_class: enum`: see below |
| `ButtonEvent` | `event` | `device_class: button`, `event_types` = the snake_case names of `ButtonEventCode` (`initial_press` ... `repeat`) |
| `DoorbellRing` | `event` | `device_class: doorbell`, `event_types: ["ring"]`; the event payload's `event_type` is `ring` whatever code came |
| writable Bool, `Momentary` (`RemoteKey`) | `button` | `command_topic` = the channel's set topic, `payload_press: "true"` |
| `Message` (writable) | `notify` | `command_topic` = the channel's set topic; the text is the message, the command parser wraps a bare string as `{"body": text}` |
| writable String (`Announcement`) | `text` | |
| a scene | `scene` | `phi/scene/<id>/set`, `payload_on: "ON"`; all on the device "phi scenes" |

Enum sensors are plain string sensors: the state topic carries core's name of the value
(`High`), the channel's `enumMap` holds the adapter's own tokens (`high`), and Home Assistant
turns a state outside a `device_class: enum` sensor's `options` into an error.

**Republish.** On every broker connect everything is published again (the ledger). When
Home Assistant says `online` on `<prefix>/status` (not retained), every discovery payload is
published again after a random 0-5 s. A device whose channel set changed has its payload
republished; a component that vanished is first sent as `{"p": "<platform>"}` alone, which
is what makes Home Assistant remove the entity, and the payload without it follows at once.
Home Assistant mode off, a removed device or an excluded adapter: an empty retained payload
on the config topic.

**The sweep.** On the first connect after start, a second short-lived connection
(`phi-transport-mqtt-sweep`) subscribes to `<baseTopic>/#` and `<prefix>/device/+/config`,
collects every message that arrives with the retained flag for 2 s, and the transport clears
(empty retained publish) every such topic it does not own: everything under `baseTopic`, and
the configs named `phi_<box>_...` of this box. Another box's configs and other ecosystems'
devices are never touched. One Info line, "swept N stale topic(s)", when N > 0. A separate
connection because the client cannot unsubscribe and an overlapping filter on the command
link would deliver a command twice.

**Not mapped yet** (phase (d)): the media set (`PlayPause`, `Mute`, `HdmiInput`, `Media*`),
the camera set (`VideoStream`, `Snapshot`, `PtzMove`, `PtzPreset`, `NightVision`,
`ObjectDetected`), `SceneTrigger`, a momentary enum (a chime pattern needs a value), writable
enums (no `select`: same naming reason as the enum sensor), colour and composite values
outside a light, the "last message" sensor beside `notify` (a Message has no state topic), and
a `RemoteKey` that only reports (no event is published for it).

### Configuration

`/etc/phi/@1/transports/mqtt.json`, merged with the tenant's override by core:

```json
{
  "hosting": "sidecar",
  "stateDir": "/var/lib/phi/@1/transports/mqtt/state",
  "baseTopic": "phi",
  "readOnly": false,
  "excludeAdapters": [],
  "discovery": { "homeAssistant": false, "prefix": "homeassistant" }
}
```

| Key | Default | Meaning |
| --- | --- | --- |
| `hosting` | | core's key: `sidecar` runs the plugin in `phi-transport-host` |
| `stateDir` | `/var/lib/phi/@1/transports/mqtt/state` | reserved; the transport keeps no state of its own yet |
| `baseTopic` | `phi` | the topic prefix. On the box it must stay `phi`: the broker account is granted `phi/#` and `homeassistant/#` only. With a `broker{}` block it may be anything valid (`home/phi`) |
| `readOnly` | `false` | `true`: no `set` subscription, so no command can arrive (Home Assistant's status topic is still heard) |
| `excludeAdapters` | `[]` | adapter plugin types (`"tuya"`) or adapter ids (`7`) whose devices are never published |
| `broker` | absent | `{host, port, username, password, tls:{caFile}}`: a foreign broker. Used instead of the hostd account. `port` defaults to 1883, or 8883 when `tls` is present; a `tls` block turns TLS on (the chain is always verified, `caFile` names the authority, empty = the system store) |
| `discovery` | absent | `{homeAssistant: false, prefix: "homeassistant"}`: `homeAssistant: true` publishes the Home Assistant discovery, `prefix` is Home Assistant's discovery prefix. With it off nothing is published and an earlier run's configs are cleared at start |

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
  `discovery.*` maps the model to the Home Assistant payloads (pure functions), `sweep.*` is
  the start-up look at the broker's retained topics.

### Build

Needs `phi-transport-api-dev` (>= 2.2.0), `libphi-runtime-dev`, `libphi-runtime-mqtt-dev`
and `nlohmann-json3-dev`; the integration test needs `/usr/sbin/mosquitto` and skips without.

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

- `mqtt_discovery_tests`: the Home Assistant payloads against the golden files in
  `tests/golden/` (compared after parsing; `PHI_UPDATE_GOLDEN=1` rewrites them, read the diff),
  no-component and excluded devices, removal, the two-step component removal, the ids.
- `mqtt_tests`: slugs, payload shapes per data type, state versus event, the device JSON,
  command parsing, the topic parser, the config, the model's events, the ledger.
- `mqtt_integration_tests`: the plugin against a mosquitto on a free port with a fake core,
  including the discovery, the sweep (a stale topic pre-seeded), the status republish and
  the retained `offline` after a clean stop.

### Installation

`usr/lib/<multiarch>/phi/plugins/transports/libphi_transport_mqtt.so`,
`/etc/phi/@1/transports/mqtt.json`, `/usr/share/phi/backends/backend-mqtt.json`,
`/usr/share/phi/intents/backends/intent-backend-mqtt.json`. The package `Suggests:
phi-mosquitto` (the broker may be another machine's).

### Issue Tracker

`https://github.com/phi-systems-tech/phi-transport-mqtt/issues`
