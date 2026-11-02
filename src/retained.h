#pragma once

// The ledger of what is, or should be, retained on the broker: every retained
// topic the transport owns, grouped by what it belongs to (a device, a room,
// a scene, the bridge). It is what makes removal possible - a device that
// goes away, or is excluded, is cleared topic by topic from what was
// published for it - and what makes a reconnect cheap: the broker may have
// lost everything, and the ledger says it all again.

#include <map>
#include <string>
#include <vector>

namespace phi::endpoint {

struct Message {
    std::string topic;
    std::string payload;
    bool retain = true;
};

class Retained
{
public:
    /// Replaces a group's topics with `desired`. The messages to send: every topic
    /// whose payload changed (all of them when `force`), and an empty retained
    /// payload for every topic the group no longer has. Nothing to send while
    /// the broker is away - the next replayAll() says it all.
    std::vector<Message> set(const std::string &group, std::map<std::string, std::string> desired, bool force = false);

    /// Clears a group completely.
    std::vector<Message> drop(const std::string &group);

    /// The broker is there (again): everything as it stands, then the clears
    /// that could not be sent while it was away.
    std::vector<Message> replayAll();
    void setOffline();

    /// The payloads of every group whose name starts with `groupPrefix`, as they stand (the discovery
    /// is said again when Home Assistant comes online, the rest is untouched).
    std::vector<Message> messagesOf(const std::string &groupPrefix) const;
    /// What the ledger holds for `topic` in `group`; empty when nothing.
    std::string payloadOf(const std::string &group, const std::string &topic) const;

    /// Whether `topic` is one this ledger owns right now.
    bool has(const std::string &topic) const;
    std::size_t topicCount() const;

private:
    std::map<std::string, std::map<std::string, std::string>> m_groups;
    std::map<std::string, bool> m_pendingClears; // topic -> true; ordered for a stable replay
    bool m_online = false;
};

} // namespace phi::endpoint
