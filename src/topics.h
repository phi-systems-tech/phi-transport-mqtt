#pragma once

// The topic tree, in one place: what is published where, and which incoming
// topics are commands.

#include <string>
#include <string_view>
#include <vector>

namespace phi::endpoint {

class Topics
{
public:
    explicit Topics(std::string base) : m_base(std::move(base)) {}

    const std::string &base() const { return m_base; }

    std::string bridgeState() const { return m_base + "/bridge/state"; }
    std::string bridgeInfo() const { return m_base + "/bridge/info"; }
    std::string bridgeDevices() const { return m_base + "/bridge/devices"; }
    std::string room(int id) const { return m_base + "/room/" + std::to_string(id); }
    std::string scene(int id) const { return m_base + "/scene/" + std::to_string(id); }
    std::string sceneSet(int id) const { return scene(id) + "/set"; }
    std::string device(int id) const { return m_base + "/device/" + std::to_string(id); }
    std::string deviceAvailability(int id) const { return device(id) + "/availability"; }
    std::string deviceSet(int id) const { return device(id) + "/set"; }
    std::string channel(int deviceId, const std::string &slug) const { return device(deviceId) + "/" + slug; }
    std::string channelSet(int deviceId, const std::string &slug) const { return channel(deviceId, slug) + "/set"; }

    /// What the transport listens to; empty for a read-only endpoint.
    std::vector<std::string> subscriptions() const;

    struct Command {
        enum class Kind { None, ChannelSet, DeviceSet, SceneSet };
        Kind kind = Kind::None;
        int id = 0;        // device or scene id
        std::string slug;  // ChannelSet only
    };
    /// Which command, if any, a message on `topic` is. Ids are decimal digits only.
    Command parseCommand(std::string_view topic) const;

private:
    std::string m_base;
};

} // namespace phi::endpoint
