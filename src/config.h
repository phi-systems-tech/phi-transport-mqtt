#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace phi::endpoint {

// A broker given by hand: a foreign one (the Home Assistant household that
// already runs a broker). When it is present the transport logs in with this
// and never asks phi-hostd for an account.
struct BrokerOverride {
    std::string host;
    int port = 1883;
    std::string username, password;
    bool tls = false;
    std::string caFile; // PEM of the authority to trust; empty = the system store
};

// Parsed now, used by the Home Assistant discovery step.
struct DiscoveryConfig {
    bool homeAssistant = false;
    std::string prefix = "homeassistant";
};

// The transport's config as core resolves it (/etc/phi/@1/transports/mqtt.json
// merged with the tenant's override). "hosting" is core's key.
struct Config {
    // Reserved: the transport keeps no state of its own today.
    std::string stateDir = "/var/lib/phi/@1/transports/mqtt/state";
    // On the box it is `phi`: the broker account is granted phi/# and nothing
    // else. A foreign broker (broker{}) may use any prefix.
    std::string baseTopic = "phi";
    // true: subscribe to nothing, so no command can arrive.
    bool readOnly = false;
    // Adapter plugin types or adapter ids whose devices are never published.
    std::vector<std::string> excludeAdapters;
    std::optional<BrokerOverride> broker;
    DiscoveryConfig discovery;

    bool excludes(const std::string &pluginType, int adapterId) const;
};

bool parseConfig(std::string_view json, Config *out, std::string *error);

/// A topic prefix as MQTT takes it: levels separated by `/`, no wildcards, no
/// empty level, not a `$SYS`-style reserved one.
bool validTopicPrefix(const std::string &prefix);

} // namespace phi::endpoint
