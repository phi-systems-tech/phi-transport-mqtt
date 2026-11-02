#include "managementcard.h"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace phi::endpoint {

using Json = nlohmann::json;

namespace {

std::string trimmed(std::string s)
{
    const auto space = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    s.erase(s.begin(), std::find_if_not(s.begin(), s.end(), space));
    s.erase(std::find_if_not(s.rbegin(), s.rend(), space).base(), s.end());
    return s;
}

Json field(const char *key, const char *label, const char *type, Json value, const char *description = nullptr)
{
    Json f{{"key", key}, {"label", label}, {"type", type}, {"default", std::move(value)}};
    if (description) f["description"] = description;
    return f;
}

// What the form handed over, read leniently: a dialog sends numbers as numbers or as text, a cleared
// text field as "", and a field the client left out means "unchanged".
bool boolOf(const Json &form, const char *key, bool current)
{
    const auto it = form.find(key);
    return it != form.end() && it->is_boolean() ? it->get<bool>() : current;
}

std::string textOf(const Json &form, const char *key, const std::string &current, bool trim = true)
{
    const auto it = form.find(key);
    if (it == form.end() || !it->is_string()) return current;
    return trim ? trimmed(it->get<std::string>()) : it->get<std::string>();
}

bool portOf(const Json &form, int current, int *port, std::string *error)
{
    *port = current;
    const auto it = form.find("brokerPort");
    if (it == form.end() || it->is_null()) return true;
    long value = 0;
    if (it->is_number_integer()) {
        value = it->get<long>();
    } else if (it->is_string()) {
        const std::string text = trimmed(it->get<std::string>());
        if (text.empty()) return true; // cleared: the current one
        std::size_t used = 0;
        try {
            value = std::stol(text, &used);
        } catch (const std::exception &) {
            used = 0;
        }
        if (used != text.size()) value = 0;
    }
    if (value < 1 || value > 65535) {
        *error = "The broker port must be a number from 1 to 65535";
        return false;
    }
    *port = static_cast<int>(value);
    return true;
}

} // namespace

std::string managementSummary(const ManagementState &s)
{
    if (!s.running) return "not running";
    if (!s.hasLink) return s.foreignBroker ? "starting" : "waiting for the broker account";
    if (!s.connected) return "broker not reachable: " + (s.lastError.empty() ? std::string("no answer yet") : s.lastError);
    std::string out = "on the broker " + s.host + ":" + std::to_string(s.port);
    if (!s.username.empty()) out += " as " + s.username;
    out += ", " + std::to_string(s.devices) + (s.devices == 1 ? " device, " : " devices, ");
    out += std::to_string(s.topics) + (s.topics == 1 ? " topic, " : " topics, ");
    out += std::string("Home Assistant discovery ") + (s.homeAssistant ? "on" : "off");
    if (s.readOnly) out += ", read-only";
    return out;
}

Json settingsForm(const Config &c)
{
    const BrokerOverride none;
    const BrokerOverride &b = c.broker ? *c.broker : none;
    Json fields = Json::array();
    fields.push_back(field("homeAssistant", "Home Assistant discovery", "boolean", c.discovery.homeAssistant,
                           "Says every device to Home Assistant so it appears there on its own."));
    fields.push_back(field("discoveryPrefix", "Discovery prefix", "string", c.discovery.prefix,
                           "The topic prefix Home Assistant listens on; \"homeassistant\" unless it was changed there."));
    fields.push_back(field("readOnly", "Read-only", "boolean", c.readOnly,
                           "Publishes the state but listens to no command from the broker."));
    fields.push_back(field("baseTopic", "Base topic", "string", c.baseTopic,
                           "Everything is published under this. Other than \"phi\" only works on a broker of your own."));
    fields.push_back(field("useBroker", "Use a broker of my own", "boolean", c.broker.has_value(),
                           "Off: the broker on this box, with the account phi mints for the transport."));
    fields.push_back(field("brokerHost", "Broker host", "string", b.host));
    fields.push_back(field("brokerPort", "Broker port", "integer", b.port));
    fields.push_back(field("brokerUsername", "Username", "string", b.username));
    fields.push_back(field("brokerPassword", "Password", "password", "", "Left empty, the stored password stays."));
    fields.push_back(field("brokerTls", "TLS", "boolean", b.tls, "The certificate chain is always verified."));
    fields.push_back(field("brokerCaFile", "CA file", "string", b.caFile,
                           "PEM file of the authority to trust; empty uses the system's."));
    return Json{{"fields", std::move(fields)}};
}

namespace {

// The form's values, resolved against the config that is running.
struct Settings {
    bool homeAssistant = true;
    std::string prefix, baseTopic;
    bool readOnly = false;
    bool useBroker = false;
    BrokerOverride broker;
    std::string password; // as typed; empty means "keep"
};

// Puts the form into `doc`: an override, or the merged config for the check. The broker block it extends
// is the doc's own, else `fallbackBroker` (the one the merged config carries); extending rather than
// rebuilding keeps the password an empty field means to keep, and any key of the block this form does
// not know.
void apply(Json &doc, const Settings &s, const Json &fallbackBroker)
{
    doc["baseTopic"] = s.baseTopic;
    doc["readOnly"] = s.readOnly;
    Json discovery = doc.contains("discovery") && doc["discovery"].is_object() ? doc["discovery"] : Json::object();
    discovery["homeAssistant"] = s.homeAssistant;
    discovery["prefix"] = s.prefix;
    doc["discovery"] = std::move(discovery);

    if (!s.useBroker) {
        doc.erase("broker");
        return;
    }
    Json broker = doc.contains("broker") && doc["broker"].is_object() ? doc["broker"]
                  : fallbackBroker.is_object()                         ? fallbackBroker
                                                                       : Json::object();
    broker["host"] = s.broker.host;
    broker["port"] = s.broker.port;
    if (s.broker.username.empty()) broker.erase("username");
    else broker["username"] = s.broker.username;
    if (!s.password.empty()) broker["password"] = s.password;
    if (s.broker.tls) {
        Json tls = broker.contains("tls") && broker["tls"].is_object() ? broker["tls"] : Json::object();
        if (s.broker.caFile.empty()) tls.erase("caFile");
        else tls["caFile"] = s.broker.caFile;
        broker["tls"] = std::move(tls);
    } else {
        broker.erase("tls");
    }
    doc["broker"] = std::move(broker);
}

} // namespace

bool mergeSettings(const Json &override, const Json &effective, const Json &form, Json *newOverride, std::string *error)
{
    std::string scratch;
    std::string &err = error ? *error : scratch;
    if (!form.is_object()) {
        err = "The settings are not a form";
        return false;
    }
    // What is running now is what a field the form leaves out stays at.
    Config current;
    parseConfig(effective.is_object() ? effective.dump() : std::string("{}"), &current, nullptr);
    const BrokerOverride currentBroker = current.broker ? *current.broker : BrokerOverride{};

    Settings s;
    s.homeAssistant = boolOf(form, "homeAssistant", current.discovery.homeAssistant);
    s.prefix = textOf(form, "discoveryPrefix", current.discovery.prefix);
    s.readOnly = boolOf(form, "readOnly", current.readOnly);
    s.baseTopic = textOf(form, "baseTopic", current.baseTopic);
    s.useBroker = boolOf(form, "useBroker", current.broker.has_value());
    s.password = textOf(form, "brokerPassword", "", false);
    if (!validTopicPrefix(s.prefix)) {
        err = "The discovery prefix is not a valid topic prefix (no wildcards, no empty level, no leading or trailing '/')";
        return false;
    }
    if (!validTopicPrefix(s.baseTopic)) {
        err = "The base topic is not a valid topic prefix (no wildcards, no empty level, no leading or trailing '/')";
        return false;
    }
    if (s.useBroker) {
        s.broker.host = textOf(form, "brokerHost", currentBroker.host);
        if (s.broker.host.empty()) {
            err = "The broker host is required when a broker of your own is used";
            return false;
        }
        if (!portOf(form, currentBroker.port, &s.broker.port, &err)) return false;
        s.broker.username = textOf(form, "brokerUsername", currentBroker.username);
        s.broker.tls = boolOf(form, "brokerTls", currentBroker.tls);
        s.broker.caFile = textOf(form, "brokerCaFile", currentBroker.caFile);
    } else if (s.baseTopic != "phi") {
        err = "The base topic may differ from \"phi\" only on a broker of your own: the box broker account is granted phi/# alone";
        return false;
    }

    const Json effectiveBroker = effective.is_object() && effective.contains("broker") ? effective["broker"] : Json();
    Json result = override.is_object() ? override : Json::object();
    apply(result, s, effectiveBroker);

    // The same changes on the merged config, through the transport's own parser: whatever it would refuse
    // at start is refused here, before core restarts anything.
    Json check = effective.is_object() ? effective : Json::object();
    apply(check, s, effectiveBroker);
    Config parsed;
    if (!parseConfig(check.dump(), &parsed, &err)) return false;

    if (newOverride) *newOverride = std::move(result);
    return true;
}

} // namespace phi::endpoint
