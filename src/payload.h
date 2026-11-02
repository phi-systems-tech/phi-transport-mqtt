#pragma once

// What goes on the wire: the payload of every topic, built from the model.
// Pure functions - no broker, no core - so the shapes are testable as goldens.

#include "model.h"
#include "topics.h"

#include <map>
#include <string>
#include <string_view>

namespace phi::endpoint {

/// topic -> payload, all of it retained.
using TopicMap = std::map<std::string, std::string>;

inline constexpr const char *kOnline = "online";
inline constexpr const char *kOffline = "offline";

/// Compact JSON text. A string that is not valid UTF-8 (an adapter's name copied off a device) is
/// replaced rather than thrown on: one bad byte must not cost the whole house its topics.
std::string dumpJson(const Json &json);

/// `ShortPressRelease` -> `short_press_release`.
std::string snakeCase(std::string_view name);

/// The value a state channel shows: a bool, number or string as core delivers
/// it, a colour as {"r","g","b"}, an enum as its name when core has one, a
/// flags mask as the array of names when core has it.
Json stateValue(const Channel &channel);

/// The payload of an event channel's topic: {"event_type","code","ts"}.
std::string eventPayload(const Channel &channel, const Json &value, const std::string &valueName,
                         const Json &valueFlags, std::int64_t tsMs);

/// {name, room, roomId, class, <slug>: value, ...} - state channels that have a value, events left out.
Json deviceJson(const Model &model, const Device &device);

/// Every retained topic of one device: the JSON, the availability and one per state channel. Empty when the device is left off the broker.
TopicMap deviceTopics(const Topics &topics, const Model &model, const Device &device);

TopicMap roomTopics(const Topics &topics, const Model &model, const Room &room);
TopicMap sceneTopics(const Topics &topics, const Model &model, const Scene &scene);

struct BridgeInfo {
    std::string version, sdk, instance, tenant;
};
/// bridge/info and bridge/devices. (bridge/state belongs to the broker link: it is will and goodbye.)
TopicMap bridgeTopics(const Topics &topics, const Model &model, const BridgeInfo &info);

} // namespace phi::endpoint
