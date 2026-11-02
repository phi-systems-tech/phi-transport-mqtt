#include "topics.h"

#include <charconv>

namespace phi::endpoint {

namespace {

std::vector<std::string_view> levels(std::string_view text)
{
    std::vector<std::string_view> out;
    std::size_t start = 0;
    while (true) {
        const std::size_t slash = text.find('/', start);
        if (slash == std::string_view::npos) {
            out.push_back(text.substr(start));
            return out;
        }
        out.push_back(text.substr(start, slash - start));
        start = slash + 1;
    }
}

// An id is digits only: "+5", " 5" and "5x" are not ids, and neither is a
// number that does not fit.
bool parseId(std::string_view text, int *out)
{
    if (text.empty() || text.size() > 9) return false;
    for (const char c : text)
        if (c < '0' || c > '9') return false;
    int value = 0;
    std::from_chars(text.data(), text.data() + text.size(), value);
    if (value <= 0) return false;
    *out = value;
    return true;
}

// A slug is lower-case letters, digits, '_' and '-' (model.h): nothing else names a channel.
bool validSlug(std::string_view text)
{
    if (text.empty()) return false;
    for (const char c : text)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    return true;
}

} // namespace

std::vector<std::string> Topics::subscriptions() const
{
    return {m_base + "/device/+/+/set", m_base + "/device/+/set", m_base + "/scene/+/set"};
}

Topics::Command Topics::parseCommand(std::string_view topic) const
{
    Command none;
    if (topic.size() <= m_base.size() + 1 || topic.compare(0, m_base.size(), m_base) != 0
        || topic[m_base.size()] != '/')
        return none;
    const std::vector<std::string_view> parts = levels(topic.substr(m_base.size() + 1));
    Command cmd;
    if (parts.size() == 3 && parts[0] == "device" && parts[2] == "set") {
        if (!parseId(parts[1], &cmd.id)) return none;
        cmd.kind = Command::Kind::DeviceSet;
        return cmd;
    }
    if (parts.size() == 3 && parts[0] == "scene" && parts[2] == "set") {
        if (!parseId(parts[1], &cmd.id)) return none;
        cmd.kind = Command::Kind::SceneSet;
        return cmd;
    }
    if (parts.size() == 4 && parts[0] == "device" && parts[3] == "set" && validSlug(parts[2])) {
        if (!parseId(parts[1], &cmd.id)) return none;
        cmd.kind = Command::Kind::ChannelSet;
        cmd.slug = std::string(parts[2]);
        return cmd;
    }
    return none;
}

} // namespace phi::endpoint
