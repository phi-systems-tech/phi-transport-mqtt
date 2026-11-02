#pragma once

// What arrives on the `set` topics, turned into the value cmd.channel.invoke
// carries. A payload is JSON when it parses as JSON, and bare text otherwise
// ("ON", "OFF", "1,0.5,0"); either way it is read by the channel's data type.

#include "model.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace phi::endpoint {

struct ParsedValue {
    bool ok = false;
    Json value;
    std::string error; // English, for the log
};

/// A `<channel>/set` payload for `channel`.
ParsedValue parseChannelPayload(const Channel &channel, std::string_view payload);

/// A value that is already JSON (one key of a `device/<id>/set` object).
ParsedValue parseChannelJson(const Channel &channel, const Json &value);

/// `device/<id>/set`: a JSON object, its keys in the order the sender wrote them (a light's power before its brightness).
/// Values are left as JSON; they are read per channel afterwards. False, with `error`, when it is not an object.
bool parseDeviceSet(std::string_view payload, std::vector<std::pair<std::string, Json>> *items, std::string *error);

/// `scene/<id>/set`: ON (or true), in any case.
bool parseSceneSet(std::string_view payload);

} // namespace phi::endpoint
