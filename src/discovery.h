#pragma once

// Home Assistant device-based MQTT discovery: one retained config per phi
// device, one more for all the scenes. Pure functions from the model to
// {topic -> payload text}, like payload.h, so the shapes are testable as
// goldens and the ledger (retained.h) treats them like any other group.
//
// Nothing here depends on a channel's value: a state change must not
// republish the discovery. What a payload carries is what the device is.

#include "model.h"
#include "payload.h"
#include "topics.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace phi::endpoint {

struct DiscoveryContext {
    Topics topics{"phi"};
    /// Discovery prefix, `homeassistant` unless the household changed it in HA.
    std::string prefix = "homeassistant";
    /// This box: hostname and tenant, e.g. `pi5_1`. Ids that must stay unique across boxes carry it.
    std::string boxId;
    std::string version;
};

/// The machine's hostname. Stable where the instance label is free text.
std::string currentHostname();

/// Hostname slugified ([a-z0-9_-], lowercase) + `_` + tenant (slugified likewise; left out when empty).
std::string boxIdFor(std::string_view hostname, std::string_view tenant);

/// `phi_<box>_<deviceId>`: the discovery object id, `dev.ids[0]`, and the front of every component's unique id.
std::string deviceObjectId(const std::string &boxId, int deviceId);
/// `phi_<box>_scenes`: the one synthetic device that holds every scene.
std::string scenesObjectId(const std::string &boxId);

std::string configTopic(const std::string &prefix, const std::string &objectId);
/// Where Home Assistant says online / offline about itself.
std::string statusTopic(const std::string &prefix);
/// Whether `topic` is a discovery config this box published: `<prefix>/device/phi_<box>_<x>/config`.
/// The sweep clears what it does not own, and must never touch another box's or another ecosystem's.
bool isOwnConfigTopic(const DiscoveryContext &ctx, std::string_view topic);

/// The `event_types` of a button: the snake_case names of v1::ButtonEventCode (phi-adapter-sdk types.h).
const std::vector<std::string> &buttonEventTypes();

/// The one retained config of a device. Empty when the device is left off the broker or has no
/// component Home Assistant could show.
TopicMap deviceDiscovery(const DiscoveryContext &ctx, const Model &model, const Device &device);

/// The config of the scenes device; empty while there is no scene.
TopicMap sceneDiscovery(const DiscoveryContext &ctx, const Model &model);

/// Home Assistant drops an entity only when the device payload names it with nothing but
/// its platform. Given the payload that stands on the broker and the one that replaces it, the
/// payload to send first: the new one with every vanished component reduced to `{"p": <platform>}`.
/// Nothing when no component vanished.
std::optional<std::string> removalStep(std::string_view standing, std::string_view replacement);

} // namespace phi::endpoint
