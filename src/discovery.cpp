#include "discovery.h"

#include <unistd.h>

#include <cctype>
#include <cmath>
#include <set>

#ifndef PHI_MQTT_VERSION
#define PHI_MQTT_VERSION "0"
#endif

namespace phi::endpoint {

namespace {

// The wire texts of a Bool channel: the channel topics carry JSON scalars, and
// `true` / `false` as text is what Home Assistant compares against.
constexpr const char *kOn = "true";
constexpr const char *kOff = "false";

// A string, an enum name or a flags array sits on its topic as JSON: quoted.
constexpr const char *kStringTemplate = "{{ value_json }}";
constexpr const char *kFlagsTemplate =
    "{{ value_json | join(', ') if value_json is iterable and value_json is not string else value_json }}";

// phi keeps a colour as {"r","g","b"} in 0..1 on the wire; Home Assistant wants "r,g,b" in
// 0..255 and hands `red`, `green`, `blue` in 0..255 to the command template. Both ways here,
// so the wire keeps its 0..1.
constexpr const char *kRgbValueTemplate =
    "{{ (value_json.r * 255) | round(0) | int }},{{ (value_json.g * 255) | round(0) | int }},"
    "{{ (value_json.b * 255) | round(0) | int }}";
constexpr const char *kRgbCommandTemplate =
    "{\"r\": {{ (red / 255) | round(4) }}, \"g\": {{ (green / 255) | round(4) }}, \"b\": {{ (blue / 255) | round(4) }}}";

// --- the mapping as data ------------------------------------------------------------

// Read-only numbers Home Assistant knows a device class for. The unit is the class's own:
// Home Assistant drops an entity whose unit does not fit its class, and phi's units are the
// ones the SDK documents for each kind (types.h).
struct SensorSpec {
    int kind;
    const char *deviceClass; // null: none
    const char *unit;
    const char *stateClass;
    bool diagnostic;
};
constexpr SensorSpec kSensors[] = {
    {kind::Temperature, "temperature", "\xC2\xB0" "C", "measurement", false},
    {kind::Humidity, "humidity", "%", "measurement", false},
    {kind::Illuminance, "illuminance", "lx", "measurement", false},
    {kind::Battery, "battery", "%", "measurement", true},
    {kind::CO2, "carbon_dioxide", "ppm", "measurement", false},
    {kind::Power, "power", "W", "measurement", false},
    {kind::Voltage, "voltage", "V", "measurement", false},
    {kind::Current, "current", "A", "measurement", false},
    {kind::Energy, "energy", "kWh", "total_increasing", false},
    {kind::LinkQuality, nullptr, "%", "measurement", true},
    {kind::SignalStrength, "signal_strength", "dBm", "measurement", true},
};

struct BinarySpec {
    int kind;
    const char *deviceClass;
};
constexpr BinarySpec kBinarySensors[] = {
    {kind::Contact, "opening"}, // phi true = open = Home Assistant ON
    {kind::Motion, "motion"},
    {kind::Tamper, "tamper"},
    {kind::Presence, "presence"},
};

// Kinds of the media and camera sets: phase (d) gives them their composite entities.
bool deferred(int k)
{
    return (k >= kind::Mute && k <= kind::MediaPlayedOn) || k == kind::VideoStream || k == kind::Snapshot
        || (k >= kind::PtzMove && k <= kind::NightVision);
}

// --- small helpers --------------------------------------------------------------------

std::string slugify(std::string_view text)
{
    std::string out;
    for (const char raw : text) {
        const unsigned char c = static_cast<unsigned char>(raw);
        if (std::isalnum(c) || c == '_' || c == '-') out += static_cast<char>(std::tolower(c));
        else out += '_';
    }
    return out;
}

std::string displayName(const Channel &ch)
{
    if (!ch.name.empty()) return ch.name;
    return ch.externalId.empty() ? ch.slug : ch.externalId;
}

Json availabilityEntry(const std::string &topic)
{
    return Json{{"topic", topic}, {"payload_available", kOnline}, {"payload_not_available", kOffline}};
}

Json origin(const DiscoveryContext &ctx)
{
    return Json{{"name", "phi"}, {"sw", ctx.version.empty() ? PHI_MQTT_VERSION : ctx.version},
                {"url", "https://github.com/phi-systems-tech/phi-transport-mqtt"}};
}

Json component(const char *platform, const std::string &uniqueId, Json name)
{
    return Json{{"p", platform}, {"unique_id", uniqueId}, {"name", std::move(name)}};
}

// --- one component per kind of channel --------------------------------------------------

struct Builder {
    const DiscoveryContext &ctx;
    const Device &device;
    const std::string objectId;

    std::string uid(const Channel &ch) const { return objectId + "_" + ch.slug; }
    std::string state(const Channel &ch) const { return ctx.topics.channel(device.id, ch.slug); }
    std::string set(const Channel &ch) const { return ctx.topics.channelSet(device.id, ch.slug); }

    Json light(const Channel &power, const Channel *brightness, const Channel *rgb, const Channel *colorTemp) const
    {
        Json j = component("light", uid(power), nullptr);
        j["state_topic"] = state(power);
        j["command_topic"] = set(power);
        j["payload_on"] = kOn;
        j["payload_off"] = kOff;
        if (brightness) {
            j["brightness_state_topic"] = state(*brightness);
            j["brightness_command_topic"] = set(*brightness);
            j["brightness_scale"] = brightness->max > 1 ? static_cast<int>(brightness->max) : 100;
        }
        if (rgb) {
            j["rgb_state_topic"] = state(*rgb);
            j["rgb_command_topic"] = set(*rgb);
            j["rgb_value_template"] = kRgbValueTemplate;
            j["rgb_command_template"] = kRgbCommandTemplate;
        }
        if (colorTemp) {
            // phi's ColorTemperature is mired, which is what the topics of this light carry.
            j["color_temp_state_topic"] = state(*colorTemp);
            j["color_temp_command_topic"] = set(*colorTemp);
            if (colorTemp->min > 0 && colorTemp->max > colorTemp->min) {
                j["min_kelvin"] = static_cast<int>(std::lround(1.0e6 / colorTemp->max));
                j["max_kelvin"] = static_cast<int>(std::lround(1.0e6 / colorTemp->min));
            }
        }
        return j;
    }

    Json switchOf(const Channel &ch, Json name, bool outlet) const
    {
        Json j = component("switch", uid(ch), std::move(name));
        j["state_topic"] = state(ch);
        j["command_topic"] = set(ch);
        j["payload_on"] = kOn;
        j["payload_off"] = kOff;
        j["state_on"] = kOn;
        j["state_off"] = kOff;
        if (outlet) j["device_class"] = "outlet";
        return j;
    }

    Json binarySensor(const Channel &ch, const char *deviceClass, bool diagnostic) const
    {
        Json j = component("binary_sensor", uid(ch), displayName(ch));
        j["state_topic"] = state(ch);
        j["payload_on"] = kOn;
        j["payload_off"] = kOff;
        if (deviceClass) j["device_class"] = deviceClass;
        if (diagnostic) j["entity_category"] = "diagnostic";
        return j;
    }

    Json numericSensor(const Channel &ch, const SensorSpec *spec) const
    {
        Json j = component("sensor", uid(ch), displayName(ch));
        j["state_topic"] = state(ch);
        if (spec) {
            if (spec->deviceClass) j["device_class"] = spec->deviceClass;
            if (spec->unit) j["unit_of_measurement"] = spec->unit;
            j["state_class"] = spec->stateClass;
            if (spec->diagnostic) j["entity_category"] = "diagnostic";
        } else {
            if (!ch.unit.empty()) j["unit_of_measurement"] = ch.unit;
            j["state_class"] = "measurement";
        }
        return j;
    }

    // Text, enum names and flag names: a string state. No device_class enum with options from
    // the enumMap: that map holds the adapter's own tokens, while the state topic carries core's
    // name for the value, and Home Assistant turns a state outside `options` into an error.
    Json textSensor(const Channel &ch) const
    {
        Json j = component("sensor", uid(ch), displayName(ch));
        j["state_topic"] = state(ch);
        j["value_template"] = ch.dataType == dataType::Flags ? kFlagsTemplate : kStringTemplate;
        if (ch.kind == kind::ConnectivityStatus) j["entity_category"] = "diagnostic";
        return j;
    }

    Json number(const Channel &ch) const
    {
        Json j = component("number", uid(ch), displayName(ch));
        j["state_topic"] = state(ch);
        j["command_topic"] = set(ch);
        // Home Assistant's own default minimum is 1, so the range is always said.
        if (ch.max > ch.min) {
            j["min"] = ch.min;
            j["max"] = ch.max;
        } else if (ch.kind == kind::Brightness || ch.kind == kind::Volume) {
            j["min"] = 0;
            j["max"] = 100;
        } else {
            j["min"] = -1000000;
            j["max"] = 1000000;
            j["mode"] = "box";
        }
        if (ch.step > 0) j["step"] = ch.step;
        else if (ch.dataType == dataType::Int) j["step"] = 1;
        if (!ch.unit.empty()) j["unit_of_measurement"] = ch.unit;
        return j;
    }

    Json text(const Channel &ch) const
    {
        Json j = component("text", uid(ch), displayName(ch));
        j["state_topic"] = state(ch);
        j["command_topic"] = set(ch);
        j["value_template"] = kStringTemplate;
        return j;
    }

    // HA presses phi: the channel's set topic, `true`.
    Json button(const Channel &ch) const
    {
        Json j = component("button", uid(ch), displayName(ch));
        j["command_topic"] = set(ch);
        j["payload_press"] = kOn;
        return j;
    }

    // The payload is the message text; the command parser wraps it as {"body": text}.
    Json notify(const Channel &ch) const
    {
        Json j = component("notify", uid(ch), displayName(ch));
        j["command_topic"] = set(ch);
        return j;
    }

    Json event(const Channel &ch) const
    {
        Json j = component("event", uid(ch), displayName(ch));
        j["state_topic"] = state(ch);
        if (ch.kind == kind::DoorbellRing) {
            j["device_class"] = "doorbell";
            j["event_types"] = Json::array({"ring"});
        } else {
            j["device_class"] = "button";
            j["event_types"] = buttonEventTypes();
        }
        return j;
    }
};

const Channel *firstWritable(const Device &device, int kindWanted, int dataTypeWanted)
{
    for (const Channel &ch : device.channels)
        if (ch.kind == kindWanted && ch.writable() && ch.dataType == dataTypeWanted && !ch.momentary()) return &ch;
    return nullptr;
}

const Channel *firstWritableNumber(const Device &device, int kindWanted)
{
    for (const Channel &ch : device.channels)
        if (ch.kind == kindWanted && ch.writable() && !ch.momentary()
            && (ch.dataType == dataType::Int || ch.dataType == dataType::Float))
            return &ch;
    return nullptr;
}

template <typename Spec, std::size_t N>
const Spec *specFor(const Spec (&table)[N], int k)
{
    for (const Spec &s : table)
        if (s.kind == k) return &s;
    return nullptr;
}

} // namespace

// --- ids and topics ----------------------------------------------------------------------

std::string currentHostname()
{
    char buffer[256] = {};
    if (::gethostname(buffer, sizeof buffer - 1) != 0) return {};
    return buffer;
}

std::string boxIdFor(std::string_view hostname, std::string_view tenant)
{
    std::string id = slugify(hostname);
    if (id.empty()) id = "box";
    const std::string t = slugify(tenant);
    return t.empty() ? id : id + "_" + t;
}

std::string deviceObjectId(const std::string &boxId, int deviceId)
{
    return "phi_" + boxId + "_" + std::to_string(deviceId);
}

std::string scenesObjectId(const std::string &boxId)
{
    return "phi_" + boxId + "_scenes";
}

std::string configTopic(const std::string &prefix, const std::string &objectId)
{
    return prefix + "/device/" + objectId + "/config";
}

std::string statusTopic(const std::string &prefix)
{
    return prefix + "/status";
}

bool isOwnConfigTopic(const DiscoveryContext &ctx, std::string_view topic)
{
    const std::string head = ctx.prefix + "/device/phi_" + ctx.boxId + "_";
    constexpr std::string_view tail = "/config";
    if (topic.size() <= head.size() + tail.size() || topic.compare(0, head.size(), head) != 0) return false;
    if (topic.compare(topic.size() - tail.size(), tail.size(), tail) != 0) return false;
    const std::string_view middle = topic.substr(head.size(), topic.size() - head.size() - tail.size());
    return middle.find('/') == std::string_view::npos;
}

const std::vector<std::string> &buttonEventTypes()
{
    // v1::ButtonEventCode without None, in snake_case, as eventPayload() spells them.
    static const std::vector<std::string> types = {
        "initial_press", "double_press", "triple_press", "quadruple_press", "quintuple_press",
        "long_press", "long_press_release", "short_press_release", "repeat",
    };
    return types;
}

// --- the device ---------------------------------------------------------------------------

TopicMap deviceDiscovery(const DiscoveryContext &ctx, const Model &model, const Device &device)
{
    if (model.excluded(device)) return {};
    const Builder b{ctx, device, deviceObjectId(ctx.boxId, device.id)};
    Json components = Json::object();
    std::set<int> used; // channel ids a component already speaks for
    bool mainTaken = false; // the first power switch or light carries the device's own name

    // A light is one component over its power, brightness, colour and colour temperature.
    if (device.deviceClass == deviceClass::Light) {
        if (const Channel *power = firstWritable(device, kind::PowerOnOff, dataType::Bool)) {
            const Channel *brightness = firstWritableNumber(device, kind::Brightness);
            const Channel *rgb = firstWritable(device, kind::ColorRGB, dataType::Color);
            const Channel *colorTemp = firstWritableNumber(device, kind::ColorTemperature);
            components[power->slug] = b.light(*power, brightness, rgb, colorTemp);
            used.insert(power->id);
            for (const Channel *c : {brightness, rgb, colorTemp})
                if (c) used.insert(c->id);
            mainTaken = true;
        }
    }

    for (const Channel &ch : device.channels) {
        if (used.count(ch.id) || deferred(ch.kind)) continue;
        Json c;
        if (ch.kind == kind::ButtonEvent || ch.kind == kind::DoorbellRing) {
            c = b.event(ch);
        } else if (ch.isEvent()) {
            continue; // SceneTrigger, ObjectDetected: no event types to declare
        } else if (ch.kind == kind::Message) {
            if (ch.writable()) c = b.notify(ch);
        } else if (ch.momentary()) {
            // Only a key that is pressed with `true`; a momentary enum (a chime pattern) needs a value.
            if (ch.writable() && ch.dataType == dataType::Bool) c = b.button(ch);
        } else if (ch.kind == kind::PowerOnOff && ch.dataType == dataType::Bool && ch.writable()) {
            c = b.switchOf(ch, mainTaken ? Json(displayName(ch)) : Json(), device.deviceClass == deviceClass::Plug);
            mainTaken = true;
        } else if (const BinarySpec *binary = specFor(kBinarySensors, ch.kind);
                   binary && ch.dataType == dataType::Bool && !ch.writable()) {
            c = b.binarySensor(ch, binary->deviceClass, false);
        } else if (const SensorSpec *sensor = specFor(kSensors, ch.kind);
                   sensor && (ch.dataType == dataType::Int || ch.dataType == dataType::Float)) {
            c = b.numericSensor(ch, sensor);
        } else if (ch.writable() && ch.dataType == dataType::Bool) {
            c = b.switchOf(ch, displayName(ch), false);
        } else if (ch.writable() && (ch.dataType == dataType::Int || ch.dataType == dataType::Float)) {
            c = b.number(ch);
        } else if (ch.writable() && ch.dataType == dataType::String) {
            c = b.text(ch);
        } else if (ch.dataType == dataType::Bool) {
            c = b.binarySensor(ch, nullptr, false);
        } else if (ch.dataType == dataType::Int || ch.dataType == dataType::Float) {
            c = b.numericSensor(ch, nullptr);
        } else if (ch.dataType == dataType::String || ch.dataType == dataType::Enum || ch.dataType == dataType::Flags) {
            c = b.textSensor(ch);
        }
        // Colour and composite values outside a light have no shape Home Assistant shows.
        if (!c.is_null()) components[ch.slug] = std::move(c);
    }
    if (components.empty()) return {};

    Json dev = Json::object();
    dev["ids"] = Json::array({b.objectId});
    dev["name"] = device.name.empty() ? device.externalId : device.name;
    if (!device.manufacturer.empty()) dev["mf"] = device.manufacturer;
    if (!device.model.empty()) dev["mdl"] = device.model;
    if (!device.firmware.empty()) dev["sw"] = device.firmware;
    if (const std::string room = model.roomNameOf(device); !room.empty()) dev["sa"] = room;

    Json root = Json::object();
    root["dev"] = std::move(dev);
    root["o"] = origin(ctx);
    // Both must say online: the bridge (a dead transport leaves its retained states standing)
    // and the device's adapter.
    root["availability"] = Json::array({availabilityEntry(ctx.topics.bridgeState()),
                                        availabilityEntry(ctx.topics.deviceAvailability(device.id))});
    root["availability_mode"] = "all";
    root["cmps"] = std::move(components);
    return {{configTopic(ctx.prefix, b.objectId), dumpJson(root)}};
}

TopicMap sceneDiscovery(const DiscoveryContext &ctx, const Model &model)
{
    if (model.scenes().empty()) return {};
    const std::string objectId = scenesObjectId(ctx.boxId);
    Json components = Json::object();
    for (const auto &[id, scene] : model.scenes()) {
        const std::string key = "scene_" + std::to_string(id);
        Json c = component("scene", "phi_" + ctx.boxId + "_scene_" + std::to_string(id),
                           scene.name.empty() ? key : scene.name);
        c["command_topic"] = ctx.topics.sceneSet(id);
        c["payload_on"] = "ON";
        components[key] = std::move(c);
    }
    Json root = Json::object();
    root["dev"] = Json{{"ids", Json::array({objectId})}, {"name", "phi scenes"}, {"mf", "phi"}};
    root["o"] = origin(ctx);
    // A scene has no adapter behind it: only the bridge can make it unavailable.
    root["availability"] = Json::array({availabilityEntry(ctx.topics.bridgeState())});
    root["availability_mode"] = "all";
    root["cmps"] = std::move(components);
    return {{configTopic(ctx.prefix, objectId), dumpJson(root)}};
}

std::optional<std::string> removalStep(std::string_view standing, std::string_view replacement)
{
    const Json old = Json::parse(standing.begin(), standing.end(), nullptr, false);
    Json next = Json::parse(replacement.begin(), replacement.end(), nullptr, false);
    if (!old.is_object() || !next.is_object()) return std::nullopt;
    const auto oldComponents = old.find("cmps");
    const auto nextComponents = next.find("cmps");
    if (oldComponents == old.end() || !oldComponents->is_object() || nextComponents == next.end()
        || !nextComponents->is_object())
        return std::nullopt;
    bool any = false;
    for (auto it = oldComponents->begin(); it != oldComponents->end(); ++it) {
        if (nextComponents->contains(it.key())) continue;
        const auto platform = it->find("p");
        if (platform == it->end() || !platform->is_string()) continue;
        (*nextComponents)[it.key()] = Json{{"p", *platform}};
        any = true;
    }
    if (!any) return std::nullopt;
    return dumpJson(next);
}

} // namespace phi::endpoint
