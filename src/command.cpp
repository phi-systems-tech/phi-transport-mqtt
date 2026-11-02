#include "command.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>

namespace phi::endpoint {

namespace {

ParsedValue good(Json value)
{
    ParsedValue p;
    p.ok = true;
    p.value = std::move(value);
    return p;
}

ParsedValue bad(std::string error)
{
    ParsedValue p;
    p.error = std::move(error);
    return p;
}

std::string_view trimmed(std::string_view s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

std::string lower(std::string_view s)
{
    std::string out(s);
    for (char &c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool parseDouble(std::string_view text, double *out)
{
    text = trimmed(text);
    if (text.empty()) return false;
    double v = 0;
    const auto r = std::from_chars(text.data(), text.data() + text.size(), v);
    if (r.ec != std::errc() || r.ptr != text.data() + text.size() || !std::isfinite(v)) return false;
    *out = v;
    return true;
}

bool parseInteger(std::string_view text, std::int64_t *out)
{
    text = trimmed(text);
    if (text.empty()) return false;
    std::int64_t v = 0;
    const auto r = std::from_chars(text.data(), text.data() + text.size(), v);
    if (r.ec != std::errc() || r.ptr != text.data() + text.size()) return false;
    *out = v;
    return true;
}

bool integral(const Json &j, std::int64_t *out)
{
    if (j.is_number_integer()) {
        *out = j.get<std::int64_t>();
        return true;
    }
    if (j.is_number_float()) {
        const double d = j.get<double>();
        if (std::isfinite(d) && d == std::floor(d) && std::fabs(d) < 9.0e15) {
            *out = static_cast<std::int64_t>(d);
            return true;
        }
    }
    return false;
}

ParsedValue toColor(double r, double g, double b)
{
    for (const double v : {r, g, b})
        if (v < 0.0 || v > 1.0) return bad("colour components are 0..1");
    return good(Json{{"r", r}, {"g", g}, {"b", b}});
}

ParsedValue colorFromText(std::string_view text)
{
    std::vector<double> parts;
    std::size_t i = 0;
    while (i <= text.size()) {
        std::size_t end = text.find_first_of(", \t", i);
        if (end == std::string_view::npos) end = text.size();
        const std::string_view token = text.substr(i, end - i);
        if (!token.empty()) {
            double v = 0;
            if (!parseDouble(token, &v)) return bad("a colour is {\"r\",\"g\",\"b\"} or \"r,g,b\" with numbers");
            parts.push_back(v);
        }
        i = end + 1;
    }
    if (parts.size() != 3) return bad("a colour is {\"r\",\"g\",\"b\"} or \"r,g,b\"");
    return toColor(parts[0], parts[1], parts[2]);
}

ParsedValue enumFromName(const Channel &ch, const std::string &name)
{
    std::int64_t n = 0;
    if (parseInteger(name, &n)) return good(n);
    if (ch.enumMap.is_object()) {
        const auto exact = ch.enumMap.find(name);
        if (exact != ch.enumMap.end() && exact->is_number_integer()) return good(*exact);
        const std::string wanted = lower(name);
        for (auto it = ch.enumMap.begin(); it != ch.enumMap.end(); ++it)
            if (lower(it.key()) == wanted && it->is_number_integer()) return good(*it);
    }
    return bad("\"" + name + "\" is not a value of this enum channel; send its number");
}

// `parsed` is the payload read as JSON (null pointer when it was not JSON);
// `raw` is the payload as text.
ParsedValue convert(const Channel &ch, const Json *parsed, std::string_view raw)
{
    const std::string text = parsed && parsed->is_string() ? parsed->get<std::string>() : std::string(trimmed(raw));
    switch (ch.dataType) {
    case dataType::Bool: {
        if (parsed && parsed->is_boolean()) return good(*parsed);
        if (parsed && parsed->is_number()) return good(parsed->get<double>() != 0.0);
        const std::string t = lower(text);
        if (t == "on" || t == "true") return good(true);
        if (t == "off" || t == "false") return good(false);
        return bad("a boolean channel takes ON, OFF, true, false or a number");
    }
    case dataType::Int: {
        std::int64_t n = 0;
        if (parsed && integral(*parsed, &n)) return good(n);
        if (parsed && parsed->is_number()) return bad("an integer channel takes a whole number");
        if (parseInteger(text, &n)) return good(n);
        return bad("an integer channel takes a whole number");
    }
    case dataType::Float: {
        double d = 0;
        if (parsed && parsed->is_number()) return good(*parsed);
        if (parseDouble(text, &d)) return good(d);
        return bad("a number channel takes a number");
    }
    case dataType::String: {
        if (parsed && parsed->is_string()) return good(*parsed);
        // Bare text is the string as sent; only a trailing newline (mosquitto_pub -f, echo) goes.
        std::string s(raw);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        return good(s);
    }
    case dataType::Color: {
        if (parsed && parsed->is_object()) {
            const auto r = parsed->find("r"), g = parsed->find("g"), b = parsed->find("b");
            if (r == parsed->end() || g == parsed->end() || b == parsed->end() || !r->is_number()
                || !g->is_number() || !b->is_number())
                return bad("a colour is {\"r\",\"g\",\"b\"} or \"r,g,b\" with numbers");
            return toColor(r->get<double>(), g->get<double>(), b->get<double>());
        }
        return colorFromText(text);
    }
    case dataType::Enum: {
        std::int64_t n = 0;
        if (parsed && integral(*parsed, &n)) return good(n);
        if (parsed && !parsed->is_string()) return bad("an enum channel takes a number or a name");
        return enumFromName(ch, text);
    }
    case dataType::Flags: {
        std::int64_t n = 0;
        if (parsed && integral(*parsed, &n) && n >= 0) return good(n);
        if ((!parsed || parsed->is_string()) && parseInteger(text, &n) && n >= 0) return good(n);
        return bad("a flags channel takes its mask as a whole number");
    }
    case dataType::Json: {
        if (parsed && parsed->is_object()) return good(*parsed);
        if (ch.kind == kind::Message) {
            // Home Assistant's notify entity publishes the bare message text; that is a message with a body and nothing else.
            std::string body;
            if (parsed && parsed->is_string()) {
                body = parsed->get<std::string>();
            } else {
                body = std::string(raw);
                while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) body.pop_back();
            }
            if (body.empty()) return bad("a message needs a body");
            return good(Json{{"body", body}});
        }
        return bad("this channel takes a JSON object");
    }
    default:
        if (parsed) return good(*parsed);
        return good(text);
    }
}

} // namespace

ParsedValue parseChannelPayload(const Channel &channel, std::string_view payload)
{
    const Json parsed = Json::parse(payload.begin(), payload.end(), nullptr, false);
    if (parsed.is_discarded()) return convert(channel, nullptr, payload);
    return convert(channel, &parsed, payload);
}

ParsedValue parseChannelJson(const Channel &channel, const Json &value)
{
    return convert(channel, &value, value.is_string() ? value.get<std::string>() : value.dump());
}

bool parseDeviceSet(std::string_view payload, std::vector<std::pair<std::string, Json>> *items, std::string *error)
{
    // ordered_json keeps the keys as the sender wrote them; json would sort them.
    const nlohmann::ordered_json doc = nlohmann::ordered_json::parse(payload.begin(), payload.end(), nullptr, false);
    if (!doc.is_object()) {
        if (error) *error = "a device set payload is a JSON object of channel slugs";
        return false;
    }
    for (auto it = doc.begin(); it != doc.end(); ++it)
        items->emplace_back(it.key(), Json::parse(it.value().dump()));
    return true;
}

bool parseSceneSet(std::string_view payload)
{
    std::string t = lower(trimmed(payload));
    if (t.size() >= 2 && t.front() == '"' && t.back() == '"') t = t.substr(1, t.size() - 2);
    return t == "on" || t == "true";
}

} // namespace phi::endpoint
