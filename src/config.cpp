#include "config.h"

#include <nlohmann/json.hpp>

namespace phi::endpoint {

using Json = nlohmann::json;

bool Config::excludes(const std::string &pluginType, int adapterId) const
{
    const std::string id = std::to_string(adapterId);
    for (const std::string &e : excludeAdapters)
        if (e == pluginType || e == id) return true;
    return false;
}

bool validTopicPrefix(const std::string &prefix)
{
    if (prefix.empty() || prefix.front() == '/' || prefix.back() == '/' || prefix.front() == '$')
        return false;
    bool previousSlash = false;
    for (const char c : prefix) {
        if (c == '+' || c == '#' || c == '\0') return false;
        if (c == '/' && previousSlash) return false;
        previousSlash = c == '/';
    }
    return true;
}

namespace {

bool stringKey(const Json &doc, const char *key, std::string &dst, std::string *error, bool allowEmpty = false)
{
    const auto it = doc.find(key);
    if (it == doc.end()) return true;
    if (!it->is_string() || (!allowEmpty && it->get<std::string>().empty())) {
        *error = std::string(key) + " must be " + (allowEmpty ? "a string" : "a non-empty string");
        return false;
    }
    dst = it->get<std::string>();
    return true;
}

bool parseBroker(const Json &b, BrokerOverride *out, std::string *error)
{
    if (!b.is_object()) {
        *error = "broker must be an object";
        return false;
    }
    BrokerOverride o;
    if (!stringKey(b, "host", o.host, error) || o.host.empty()) {
        if (error->empty()) *error = "broker.host is required";
        return false;
    }
    if (!stringKey(b, "username", o.username, error, true) || !stringKey(b, "password", o.password, error, true))
        return false;
    bool portGiven = false;
    if (const auto it = b.find("port"); it != b.end()) {
        if (!it->is_number_integer() || it->get<int>() < 1 || it->get<int>() > 65535) {
            *error = "broker.port must be 1..65535";
            return false;
        }
        o.port = it->get<int>();
        portGiven = true;
    }
    if (const auto it = b.find("tls"); it != b.end()) {
        if (!it->is_object()) {
            *error = "broker.tls must be an object";
            return false;
        }
        // A tls block means TLS; the chain is always verified, so the only
        // knob is which authority to trust.
        o.tls = true;
        if (!stringKey(*it, "caFile", o.caFile, error)) return false;
        if (!portGiven) o.port = 8883;
    }
    *out = std::move(o);
    return true;
}

} // namespace

bool parseConfig(std::string_view json, Config *out, std::string *error)
{
    std::string scratch;
    std::string &err = error ? *error : scratch;
    const Json doc = Json::parse(json, nullptr, false);
    if (!doc.is_object()) {
        err = "transport config is not a JSON object";
        return false;
    }
    Config c;
    if (!stringKey(doc, "stateDir", c.stateDir, &err) || !stringKey(doc, "baseTopic", c.baseTopic, &err))
        return false;
    if (const auto it = doc.find("readOnly"); it != doc.end()) {
        if (!it->is_boolean()) {
            err = "readOnly must be a boolean";
            return false;
        }
        c.readOnly = it->get<bool>();
    }
    if (const auto it = doc.find("excludeAdapters"); it != doc.end()) {
        if (!it->is_array()) {
            err = "excludeAdapters must be an array";
            return false;
        }
        for (const Json &e : *it) {
            if (e.is_string()) c.excludeAdapters.push_back(e.get<std::string>());
            else if (e.is_number_integer()) c.excludeAdapters.push_back(std::to_string(e.get<int>()));
            else {
                err = "excludeAdapters entries are plugin types or adapter ids";
                return false;
            }
        }
    }
    if (const auto it = doc.find("broker"); it != doc.end() && !it->is_null()) {
        BrokerOverride b;
        if (!parseBroker(*it, &b, &err)) return false;
        c.broker = std::move(b);
    }
    if (const auto it = doc.find("discovery"); it != doc.end() && !it->is_null()) {
        if (!it->is_object()) {
            err = "discovery must be an object";
            return false;
        }
        if (const auto h = it->find("homeAssistant"); h != it->end()) {
            if (!h->is_boolean()) {
                err = "discovery.homeAssistant must be a boolean";
                return false;
            }
            c.discovery.homeAssistant = h->get<bool>();
        }
        if (!stringKey(*it, "prefix", c.discovery.prefix, &err)) return false;
        if (!validTopicPrefix(c.discovery.prefix)) {
            err = "discovery.prefix is not a valid topic prefix";
            return false;
        }
    }
    if (!validTopicPrefix(c.baseTopic)) {
        err = "baseTopic is not a valid topic prefix (no wildcards, no empty level, no leading or trailing '/')";
        return false;
    }
    // The box account is granted phi/# (the descriptor's brokerTopics); any
    // other prefix would connect and then be refused every publish.
    if (!c.broker && c.baseTopic != "phi") {
        err = "baseTopic may differ from \"phi\" only with a broker{} block: the box broker account is granted phi/# alone";
        return false;
    }
    *out = std::move(c);
    return true;
}

} // namespace phi::endpoint
