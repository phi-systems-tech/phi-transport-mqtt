#include "payload.h"

#include <cctype>
#include <cmath>

namespace phi::endpoint {

std::string dumpJson(const Json &json)
{
    return json.dump(-1, ' ', false, Json::error_handler_t::replace);
}

std::string snakeCase(std::string_view name)
{
    std::string out;
    for (std::size_t i = 0; i < name.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        if (c == ' ' || c == '-') {
            if (!out.empty() && out.back() != '_') out += '_';
            continue;
        }
        if (std::isupper(c)) {
            const bool prevLowerOrDigit = i > 0 && (std::islower(static_cast<unsigned char>(name[i - 1]))
                                                    || std::isdigit(static_cast<unsigned char>(name[i - 1])));
            // The last capital of an acronym starts the next word: "NFCTag" -> "nfc_tag".
            const bool acronymEnd = i > 0 && std::isupper(static_cast<unsigned char>(name[i - 1])) && i + 1 < name.size()
                                    && std::islower(static_cast<unsigned char>(name[i + 1]));
            if ((prevLowerOrDigit || acronymEnd) && !out.empty() && out.back() != '_') out += '_';
            out += static_cast<char>(std::tolower(c));
        } else {
            out += static_cast<char>(c);
        }
    }
    return out;
}

Json stateValue(const Channel &ch)
{
    if (ch.dataType == dataType::Enum && !ch.valueName.empty()) return ch.valueName;
    if (ch.dataType == dataType::Flags && ch.valueFlags.is_array()) return ch.valueFlags;
    return ch.value;
}

std::string eventPayload(const Channel &ch, const Json &value, const std::string &valueName,
                         const Json &valueFlags, std::int64_t tsMs)
{
    Json out = Json::object();
    // A ring is a ring whichever ButtonEventCode the adapter wrapped it in; the Home
    // Assistant event entity of a doorbell declares this one type (discovery.cpp).
    if (ch.kind == kind::DoorbellRing) out["event_type"] = "ring";
    else if (!valueName.empty()) out["event_type"] = snakeCase(valueName);
    else if (value.is_string()) out["event_type"] = snakeCase(value.get<std::string>());
    else out["event_type"] = "event";
    if (value.is_number_integer()) out["code"] = value;
    if (valueFlags.is_array()) out["flags"] = valueFlags;
    out["ts"] = tsMs;
    return dumpJson(out);
}

namespace {

Json roomValue(const Model &model, const Device &device)
{
    const std::string name = model.roomNameOf(device);
    return name.empty() ? Json() : Json(name);
}

Json roomIdValue(int id)
{
    return id > 0 ? Json(id) : Json();
}

} // namespace

Json deviceJson(const Model &model, const Device &device)
{
    Json out = Json::object();
    out["name"] = device.name;
    out["room"] = roomValue(model, device);
    out["roomId"] = roomIdValue(device.roomId());
    out["class"] = device.className;
    for (const Channel &ch : device.channels)
        if (ch.hasState() && ch.hasValue) out[ch.slug] = stateValue(ch);
    return out;
}

TopicMap deviceTopics(const Topics &topics, const Model &model, const Device &device)
{
    TopicMap out;
    if (model.excluded(device)) return out;
    out[topics.device(device.id)] = dumpJson(deviceJson(model, device));
    out[topics.deviceAvailability(device.id)] = model.adapterConnected(device.adapterId) ? kOnline : kOffline;
    for (const Channel &ch : device.channels)
        if (ch.hasState() && ch.hasValue) out[topics.channel(device.id, ch.slug)] = dumpJson(stateValue(ch));
    return out;
}

TopicMap roomTopics(const Topics &topics, const Model &, const Room &room)
{
    Json j = Json::object();
    j["name"] = room.name;
    j["class"] = room.className;
    j["floor"] = roomIdValue(room.floorId);
    return {{topics.room(room.id), dumpJson(j)}};
}

TopicMap sceneTopics(const Topics &topics, const Model &model, const Scene &scene)
{
    Json j = Json::object();
    j["name"] = scene.name;
    const std::string room = model.roomName(scene.roomId);
    j["room"] = room.empty() ? Json() : Json(room);
    j["roomId"] = roomIdValue(scene.roomId);
    return {{topics.scene(scene.id), dumpJson(j)}};
}

namespace {

Json describeChannel(const Channel &ch)
{
    Json j = Json::object();
    j["id"] = ch.id;
    j["slug"] = ch.slug;
    j["name"] = ch.name;
    j["kind"] = ch.kindName;
    j["dataType"] = ch.dataTypeName;
    j["unit"] = ch.unit.empty() ? Json() : Json(ch.unit);
    const bool ranged = ch.max > ch.min;
    j["min"] = ranged ? Json(ch.min) : Json();
    j["max"] = ranged ? Json(ch.max) : Json();
    j["step"] = ch.step > 0 ? Json(ch.step) : Json();
    j["writable"] = ch.writable();
    j["momentary"] = ch.momentary();
    j["event"] = ch.isEvent();
    return j;
}

} // namespace

TopicMap bridgeTopics(const Topics &topics, const Model &model, const BridgeInfo &info)
{
    TopicMap out;
    Json meta = Json::object();
    meta["version"] = info.version;
    meta["sdk"] = info.sdk;
    meta["instance"] = info.instance;
    meta["tenant"] = info.tenant;
    out[topics.bridgeInfo()] = dumpJson(meta);

    Json devices = Json::array();
    for (const auto &[id, device] : model.devices()) {
        if (model.excluded(device)) continue;
        Json d = Json::object();
        d["id"] = device.id;
        d["name"] = device.name;
        d["room"] = roomValue(model, device);
        d["roomId"] = roomIdValue(device.roomId());
        d["class"] = device.className;
        Json channels = Json::array();
        for (const Channel &ch : device.channels) channels.push_back(describeChannel(ch));
        d["channels"] = std::move(channels);
        devices.push_back(std::move(d));
    }
    out[topics.bridgeDevices()] = dumpJson(devices);
    return out;
}

} // namespace phi::endpoint
