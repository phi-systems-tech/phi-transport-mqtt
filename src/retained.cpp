#include "retained.h"

namespace phi::endpoint {

std::vector<Message> Retained::set(const std::string &group, std::map<std::string, std::string> desired, bool force)
{
    std::vector<Message> out;
    auto &current = m_groups[group];
    for (const auto &[topic, payload] : desired) {
        const auto old = current.find(topic);
        if (force || old == current.end() || old->second != payload) {
            if (m_online) out.push_back(Message{topic, payload, true});
        }
        // A topic that comes back before its clear was sent needs no clear.
        m_pendingClears.erase(topic);
    }
    for (const auto &[topic, payload] : current) {
        if (desired.count(topic)) continue;
        if (m_online) out.push_back(Message{topic, std::string(), true});
        else m_pendingClears[topic] = true;
    }
    if (desired.empty()) m_groups.erase(group);
    else current = std::move(desired);
    return out;
}

std::vector<Message> Retained::drop(const std::string &group)
{
    return set(group, {});
}

std::vector<Message> Retained::replayAll()
{
    m_online = true;
    std::vector<Message> out;
    for (const auto &[group, topics] : m_groups)
        for (const auto &[topic, payload] : topics) out.push_back(Message{topic, payload, true});
    for (const auto &[topic, unused] : m_pendingClears) {
        (void)unused;
        out.push_back(Message{topic, std::string(), true});
    }
    m_pendingClears.clear();
    return out;
}

void Retained::setOffline()
{
    m_online = false;
}

bool Retained::has(const std::string &topic) const
{
    for (const auto &[group, topics] : m_groups)
        if (topics.count(topic)) return true;
    return false;
}

std::size_t Retained::topicCount() const
{
    std::size_t n = 0;
    for (const auto &[group, topics] : m_groups) n += topics.size();
    return n;
}

} // namespace phi::endpoint
