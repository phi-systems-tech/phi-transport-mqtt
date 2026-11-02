// The plugin against a real broker: a mosquitto started on a free port, the
// transport on a runtime loop of its own, a fake core behind its facade that
// answers the list calls from fixtures. Without /usr/sbin/mosquitto the test
// is skipped (exit 77), as phi-runtime's own broker test is.

#include "mqtttransport.h"

#include <transportinterface.h>

#include <phi/runtime/epollloop.h>
#include <phi/runtime/mqtt/client.h>

#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

extern char **environ;

namespace phicore {
// The contract lets only the manager and the host drive a plugin; this test is the host for the day.
class TransportHost
{
public:
    static void attach(transport::TransportInterface *t, transport::CoreFacade *f, phi::runtime::Loop *l)
    {
        t->attachRuntimeLoop(l);
        t->attachCoreFacade(f);
    }
    static void asyncResult(transport::TransportInterface *t, transport::CmdId id, std::string_view json) { t->onCoreAsyncResult(id, json); }
    static void event(transport::TransportInterface *t, std::string_view topic, std::string_view json) { t->onCoreEvent(topic, json); }
};
} // namespace phicore

namespace {

using Json = nlohmann::json;
using namespace phicore::transport;
using phi::runtime::EpollLoop;
using phi::runtime::Loop;
namespace mqtt = phi::runtime::mqtt;
using phicore::TransportHost;

constexpr const char *kBroker = "/usr/sbin/mosquitto";
int g_failures = 0;

#define CHECK(cond)                                                                     \
    do {                                                                                \
        if (!(cond)) {                                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                 \
            ++g_failures;                                                               \
        }                                                                               \
    } while (false)

int freePort()
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr);
    socklen_t len = sizeof addr;
    ::getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len);
    ::close(fd);
    return ntohs(addr.sin_port);
}

bool listening(int port)
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const bool ok = ::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) == 0;
    ::close(fd);
    return ok;
}

bool waitFor(Loop &loop, const std::function<bool()> &predicate, int timeoutMs = 5000)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return predicate();
        auto slice = loop.timerAfter(std::chrono::milliseconds(20), [&loop]() { loop.stop(); });
        loop.run();
    }
    return true;
}

void settle(Loop &loop, int ms)
{
    waitFor(loop, []() { return false; }, ms);
}

// --- what a broker client heard -------------------------------------------------

struct Heard {
    std::string topic, payload;
    bool retained = false;
};

class Spy
{
public:
    Spy(Loop &loop, int port, const std::string &id, const std::string &filter) : m_client(loop)
    {
        m_client.onMessage([this](const mqtt::Message &m) { heard.push_back({m.topic, m.payload, m.retained}); });
        m_client.onConnected([this, filter]() {
            if (!filter.empty()) m_client.subscribe(filter, 1);
        });
        mqtt::Settings s;
        s.clientId = id;
        s.host = "127.0.0.1";
        s.port = port;
        m_client.connect(s);
    }
    bool connected() const { return m_client.connected(); }
    void publish(const std::string &topic, const std::string &payload, bool retain = false)
    {
        m_client.publish(topic, payload, 1, retain);
    }
    std::size_t count(const std::string &topic) const
    {
        std::size_t n = 0;
        for (const Heard &h : heard) n += h.topic == topic;
        return n;
    }
    const Heard *last(const std::string &topic) const
    {
        for (auto it = heard.rbegin(); it != heard.rend(); ++it)
            if (it->topic == topic) return &*it;
        return nullptr;
    }
    std::vector<Heard> heard;

private:
    mqtt::Client m_client;
};

// --- a core that answers from fixtures ---------------------------------------------

Json channel(int id, const char *externalId, int kind, int dataType, int flags, Json value = nullptr, const char *name = nullptr)
{
    Json j = {{"id", id}, {"externalId", externalId}, {"name", externalId}, {"kind", kind}, {"kindName", "K"},
              {"dataType", dataType}, {"dataTypeName", "T"}, {"flags", flags}, {"unit", ""},
              {"min", 0.0}, {"max", 0.0}, {"step", 0.0}, {"metaAdapter", Json::object()}, {"hasValue", !value.is_null()}};
    if (!value.is_null()) j["lastValue"] = value;
    if (name) j["lastValueName"] = name;
    return j;
}

Json device(int id, int adapterId, const char *name, int roomId, Json channels)
{
    return {{"device", {{"id", id}, {"adapterId", adapterId}, {"externalId", "d" + std::to_string(id)}, {"name", name},
                        {"deviceClass", 1}, {"deviceClassName", "Light"}, {"userRoomId", roomId}, {"adapterRoomId", 0},
                        {"userRoomName", roomId ? Json("Kitchen") : Json()}}},
            {"channels", channels}};
}

class FakeCore final : public CoreFacade
{
public:
    struct Call {
        std::string topic;
        Json payload;
        CallerIdentity::Kind kind = CallerIdentity::Kind::Anonymous;
        std::string clientId;
    };

    explicit FakeCore(Loop &loop) : m_loop(loop) {}

    TransportInterface *plugin = nullptr;
    std::vector<Call> calls;
    std::vector<LogEntry> logs;
    std::set<int> failChannels;           // cmd.channel.invoke on these fails
    std::map<int, Json> rollbackFor;      // ... with this rollbackValue (none: omitted)
    bool holdDevices = false;             // the devices list is answered when release() is called
    bool credentialsAnswered = false;
    // core's transport config: what cmd.transport.config.get answers and cmd.transport.config.set stores
    Json configBase = Json::object();
    Json configOverride = Json::object();
    Json configEffective = Json::object();
    bool failConfigSet = false;   // config.set answers a failure
    bool stopOnConfigSet = false; // config.set restarts the transport before it answers
    std::map<CmdId, Json> completed; // what the plugin answered to an action

    void log(const LogEntry &entry) override { logs.push_back(entry); }

    SyncResult invokeSync(std::string_view topic, std::string_view, int, const CallerIdentity &caller) override
    {
        calls.push_back({std::string(topic), Json::object(), caller.kind, std::string(caller.clientId)});
        SyncResult r;
        r.accepted = topic == "sync.hello.get";
        r.payloadJson = Json{{"accepted", true}, {"tenant", "1"}, {"instanceLabel", "Test House"}}.dump();
        return r;
    }

    AsyncResult invokeAsync(std::string_view topic, std::string_view payload, std::string_view pluginType, const CallerIdentity &caller) override
    {
        Call call{std::string(topic), Json::parse(payload, nullptr, false), caller.kind, std::string(caller.clientId)};
        calls.push_back(call);
        const CmdId id = m_next++;
        pluginTypes.insert(std::string(pluginType));
        Json reply = Json::object();
        if (call.topic == "cmd.adapters.list") {
            reply["adapters"] = Json::array({{{"id", 1}, {"pluginType", "hue"}, {"externalId", "hue1"}, {"connected", true}},
                                             {{"id", 2}, {"pluginType", "z2m"}, {"externalId", "z2m1"}, {"connected", true}},
                                             {{"id", 3}, {"pluginType", "tuya"}, {"externalId", "tuya1"}, {"connected", true}}});
        } else if (call.topic == "cmd.rooms.list") {
            reply["rooms"] = Json::array({{{"id", 5}, {"name", "Kitchen"}, {"roomClassName", "Kitchen"}, {"floorId", 0}}});
        } else if (call.topic == "cmd.scenes.list") {
            reply["scenes"] = Json::array({{{"id", 20}, {"name", "Evening"}, {"roomId", 5}}});
        } else if (call.topic == "cmd.devices.list") {
            reply["devices"] = Json::array({
                device(10, 1, "Kitchen light", 5,
                       Json::array({channel(100, "power", 1, 1, 7, true), channel(101, "brightness", 10, 2, 7, 80),
                                    channel(104, "button1", 2, 2, 5, 12, "ShortPressRelease")})),
                device(11, 2, "Hall sensor", 0, Json::array({channel(110, "temperature", 50, 3, 5, 21.5)})),
                device(12, 3, "Tuya plug", 0, Json::array({channel(120, "power", 1, 1, 7, true)})),
            });
            if (holdDevices) {
                held = reply;
                heldId = id;
                return accepted(id);
            }
        } else if (call.topic == "cmd.host.broker.credentials") {
            // Somebody else's account only: the endpoint's own is not minted yet.
            reply["credentials"] = Json::array({{{"backend", "voice@satellites"}, {"username", "voice_satellites"}, {"password", "x"}}});
            credentialsAnswered = true;
        } else if (call.topic == "cmd.transport.config.get") {
            reply = {{"pluginType", "mqtt"}, {"base", configBase}, {"override", configOverride}, {"effective", configEffective}};
        } else if (call.topic == "cmd.transport.config.set") {
            if (failConfigSet) {
                reply = {{"status", 1}, {"statusName", "Failure"}, {"error", {{"message", "override not writable"}}}};
            } else {
                configOverride = call.payload["override"];
                reply = {{"pluginType", "mqtt"}, {"restarted", true}};
                if (stopOnConfigSet) {
                    m_loop.post([this]() { plugin->stop(); });
                    return accepted(id);
                }
            }
        } else if (call.topic == "cmd.channel.invoke") {
            const int channelId = call.payload.value("channelId", 0);
            reply = {{"cmd", call.topic}, {"status", 0}, {"statusName", "Success"}};
            if (failChannels.count(channelId)) {
                reply = {{"cmd", call.topic}, {"status", 1}, {"statusName", "Failure"}, {"error", {{"message", "device unreachable"}}}};
                if (rollbackFor.count(channelId)) reply["rollbackValue"] = rollbackFor[channelId];
            }
        } else if (call.topic == "cmd.scene.invoke") {
            reply = {{"cmd", call.topic}, {"status", 0}};
        }
        m_loop.post([this, id, reply]() { TransportHost::asyncResult(plugin, id, reply.dump()); });
        return accepted(id);
    }

    void completeAction(CmdId id, std::string_view result) override { completed[id] = Json::parse(result, nullptr, false); }

    void release()
    {
        if (heldId == 0) return;
        const CmdId id = heldId;
        heldId = 0;
        holdDevices = false;
        TransportHost::asyncResult(plugin, id, held.dump());
    }
    bool holding() const { return heldId != 0; }

    std::vector<Call> callsTo(const std::string &topic) const
    {
        std::vector<Call> out;
        for (const Call &c : calls)
            if (c.topic == topic) out.push_back(c);
        return out;
    }
    bool warned(const std::string &needle) const
    {
        for (const LogEntry &e : logs) {
            if (e.level != LogLevel::Warn) continue;
            std::string text = e.message;
            for (const Scalar &s : e.params)
                if (const auto *str = std::get_if<std::string>(&s)) text += " " + *str;
                else if (const auto *i = std::get_if<std::int64_t>(&s)) text += " " + std::to_string(*i);
            if (text.find(needle) != std::string::npos) return true;
        }
        return false;
    }
    std::set<std::string> pluginTypes;

private:
    static AsyncResult accepted(CmdId id)
    {
        AsyncResult r;
        r.accepted = true;
        r.cmdId = id;
        return r;
    }

    Loop &m_loop;
    CmdId m_next = 1;
    Json held;
    CmdId heldId = 0;
};

// The mirror scenarios: discovery off, so what they retain on the shared broker is the
// endpoint's own tree only and the discovery scenario finds nothing of theirs.
std::string configFor(int port, const std::string &base, bool readOnly)
{
    return Json{{"hosting", "sidecar"}, {"baseTopic", base}, {"readOnly", readOnly}, {"excludeAdapters", Json::array({"tuya"})},
                {"broker", {{"host", "127.0.0.1"}, {"port", port}}}, {"discovery", {{"homeAssistant", false}}}}.dump();
}

std::string configWithDiscovery(int port, const std::string &base, bool readOnly, bool homeAssistant)
{
    Json j = Json::parse(configFor(port, base, readOnly));
    j["discovery"] = {{"homeAssistant", homeAssistant}};
    return j.dump();
}

void feed(FakeCore &core, const std::string &topic, const Json &payload)
{
    TransportHost::event(core.plugin, topic, payload.dump());
}

// --- the scenarios ----------------------------------------------------------------

void runEndpoint(Loop &loop, int port)
{
    Spy spy(loop, port, "spy", "phi/#");
    Spy hand(loop, port, "hand", "");
    CHECK(waitFor(loop, [&]() { return spy.connected() && hand.connected(); }));

    FakeCore core(loop);
    auto plugin = std::make_unique<phi::endpoint::MqttTransport>();
    core.plugin = plugin.get();
    TransportHost::attach(plugin.get(), &core, &loop);
    CHECK(plugin->pluginType() == "mqtt" && plugin->displayName() == "MQTT endpoint");

    std::string error;
    CHECK(!plugin->start("{\"baseTopic\":\"phi/#\"}", &error) && !error.empty());

    // --- start; an event arrives before the device list is answered -----------------
    core.holdDevices = true;
    CHECK(plugin->start(configFor(port, "phi", false), &error));
    CHECK(waitFor(loop, [&]() { return core.holding(); }));
    feed(core, "event.channel.stateChanged", {{"channelId", 101}, {"value", 33}, {"tsMs", 1}});
    feed(core, "event.adapter.connectionStateChanged", {{"adapterId", 2}, {"connected", false}});
    core.release();

    CHECK(waitFor(loop, [&]() { return spy.last("phi/device/10/brightness") != nullptr && spy.last("phi/bridge/devices") != nullptr; }));
    settle(loop, 300);
    // The snapshot said 80; the buffered event came after it and wins.
    CHECK(spy.last("phi/device/10/brightness") && spy.last("phi/device/10/brightness")->payload == "33");
    CHECK(spy.last("phi/device/11/availability") && spy.last("phi/device/11/availability")->payload == "offline");
    feed(core, "event.adapter.connectionStateChanged", {{"adapterId", 2}, {"connected", true}});

    // Every call into core carried the endpoint's identity, and the hello came once.
    for (const FakeCore::Call &c : core.calls) {
        CHECK(c.kind == CallerIdentity::Kind::TrustedLocal);
        CHECK(c.clientId == "mqtt");
    }
    CHECK(core.callsTo("sync.hello.get").size() == 1);
    CHECK(core.pluginTypes.size() == 1 && *core.pluginTypes.begin() == "mqtt");
    // No broker{} and no account: it asks the host layer. Here broker{} is given, so it must not.
    CHECK(core.callsTo("cmd.host.broker.credentials").empty());

    CHECK(spy.last("phi/bridge/state") && spy.last("phi/bridge/state")->payload == "online");
    CHECK(spy.last("phi/bridge/info"));
    if (const Heard *info = spy.last("phi/bridge/info")) {
        const Json j = Json::parse(info->payload);
        CHECK(j["instance"] == "Test House" && j["tenant"] == "1" && j["version"] == "0.3.0" && j["sdk"] == kTransportApiVersion);
    }
    if (const Heard *devices = spy.last("phi/bridge/devices")) {
        const Json j = Json::parse(devices->payload);
        CHECK(j.is_array() && j.size() == 2); // the tuya plug is left out
    }
    CHECK(spy.last("phi/room/5") && Json::parse(spy.last("phi/room/5")->payload)["name"] == "Kitchen");
    CHECK(spy.last("phi/scene/20") && Json::parse(spy.last("phi/scene/20")->payload)["name"] == "Evening");
    CHECK(spy.last("phi/device/10/power") && spy.last("phi/device/10/power")->payload == "true");
    CHECK(spy.last("phi/device/10/availability") && spy.last("phi/device/10/availability")->payload == "online");
    CHECK(spy.last("phi/device/11/temperature") && spy.last("phi/device/11/temperature")->payload == "21.5");
    if (const Heard *dev = spy.last("phi/device/10")) {
        const Json j = Json::parse(dev->payload);
        CHECK(j["name"] == "Kitchen light" && j["room"] == "Kitchen" && j["power"] == true && j["brightness"] == 33);
        CHECK(!j.contains("button1"));
    } else {
        CHECK(false);
    }
    CHECK(spy.count("phi/device/12") == 0 && spy.count("phi/device/12/power") == 0 && spy.count("phi/device/12/availability") == 0);
    CHECK(spy.count("phi/device/10/button1") == 0);

    // --- retained means retained: a late subscriber is handed the picture --------------
    {
        Spy late(loop, port, "late", "phi/#");
        CHECK(waitFor(loop, [&]() { return late.last("phi/bridge/devices") != nullptr; }));
        settle(loop, 300);
        for (const char *topic : {"phi/bridge/state", "phi/bridge/info", "phi/bridge/devices", "phi/room/5", "phi/scene/20", "phi/device/10",
                                  "phi/device/10/availability", "phi/device/10/power", "phi/device/10/brightness"}) {
            const Heard *h = late.last(topic);
            CHECK(h != nullptr && h->retained);
        }
        CHECK(late.count("phi/device/10/button1") == 0);
        CHECK(late.count("phi/device/12") == 0);
    }

    // --- a button press: said once, not retained, not in the device JSON ----------------
    feed(core, "event.channel.stateChanged", {{"channelId", 104}, {"value", 12}, {"tsMs", 1730000000123}, {"valueName", "ShortPressRelease"}});
    CHECK(waitFor(loop, [&]() { return spy.count("phi/device/10/button1") == 1; }));
    {
        const Json j = Json::parse(spy.last("phi/device/10/button1")->payload);
        CHECK(j == (Json{{"event_type", "short_press_release"}, {"code", 12}, {"ts", 1730000000123}}));
    }
    const std::size_t deviceJsonBefore = spy.count("phi/device/10");
    settle(loop, 200);
    CHECK(spy.count("phi/device/10") == deviceJsonBefore); // the press touched no state
    {
        Spy late(loop, port, "late2", "phi/device/10/#");
        CHECK(waitFor(loop, [&]() { return late.last("phi/device/10/power") != nullptr; }));
        settle(loop, 200);
        CHECK(late.count("phi/device/10/button1") == 0);
    }

    // --- state events become retained topics and a new device JSON ------------------------
    feed(core, "event.channel.stateChanged", {{"channelId", 100}, {"value", false}, {"tsMs", 2}});
    CHECK(waitFor(loop, [&]() { return spy.last("phi/device/10/power") && spy.last("phi/device/10/power")->payload == "false"; }));
    CHECK(waitFor(loop, [&]() { return spy.last("phi/device/10") && Json::parse(spy.last("phi/device/10")->payload)["power"] == false; }));
    feed(core, "event.channel.stateChanged", {{"channelId", 100}, {"value", true}, {"tsMs", 3}});
    CHECK(waitFor(loop, [&]() { return spy.last("phi/device/10/power")->payload == "true"; }));

    // --- commands --------------------------------------------------------------------------
    const std::size_t invokesBefore = core.callsTo("cmd.channel.invoke").size();
    hand.publish("phi/device/10/power/set", "OFF");
    CHECK(waitFor(loop, [&]() { return core.callsTo("cmd.channel.invoke").size() == invokesBefore + 1; }));
    {
        const FakeCore::Call c = core.callsTo("cmd.channel.invoke").back();
        CHECK(c.payload == (Json{{"channelId", 100}, {"value", false}}));
        CHECK(c.kind == CallerIdentity::Kind::TrustedLocal && c.clientId == "mqtt");
    }
    // device/<id>/set: one invoke per key, in the sender's order.
    hand.publish("phi/device/10/set", "{\"brightness\":20,\"power\":true}");
    CHECK(waitFor(loop, [&]() { return core.callsTo("cmd.channel.invoke").size() == invokesBefore + 3; }));
    {
        const auto invokes = core.callsTo("cmd.channel.invoke");
        CHECK(invokes[invokesBefore + 1].payload == (Json{{"channelId", 101}, {"value", 20}}));
        CHECK(invokes[invokesBefore + 2].payload == (Json{{"channelId", 100}, {"value", true}}));
    }
    hand.publish("phi/scene/20/set", "ON");
    CHECK(waitFor(loop, [&]() { return !core.callsTo("cmd.scene.invoke").empty(); }));
    CHECK(core.callsTo("cmd.scene.invoke").back().payload == (Json{{"sceneId", 20}}));

    // What must not reach core: a value of the wrong type, a read-only channel, an excluded device, an unknown one, hostile topics.
    const std::size_t total = core.calls.size();
    hand.publish("phi/device/10/brightness/set", "banana");
    hand.publish("phi/device/10/button1/set", "1");
    hand.publish("phi/device/12/power/set", "OFF");
    hand.publish("phi/device/99/power/set", "OFF");
    hand.publish("phi/device/10/nothing/set", "OFF");
    hand.publish("phi/device/10/power", "OFF");           // a state topic is not a command
    hand.publish("phi/scene/20/set", "OFF");
    settle(loop, 500);
    CHECK(core.calls.size() == total);
    CHECK(core.warned("banana") || core.warned("whole number"));
    CHECK(core.warned("read-only"));

    // --- a refused command: the consumer is shown the truth ---------------------------------
    core.failChannels = {101, 100};
    core.rollbackFor[101] = 55;
    hand.publish("phi/device/10/brightness/set", "10");
    CHECK(waitFor(loop, [&]() { return spy.last("phi/device/10/brightness")->payload == "55"; }));
    CHECK(waitFor(loop, [&]() { return Json::parse(spy.last("phi/device/10")->payload)["brightness"] == 55; }));
    CHECK(core.warned("device unreachable") && core.warned("Channel"));
    // No rollback value: the last one known is said again, even though it is the one the broker holds.
    const std::size_t powerBefore = spy.count("phi/device/10/power");
    hand.publish("phi/device/10/power/set", "OFF");
    CHECK(waitFor(loop, [&]() { return spy.count("phi/device/10/power") > powerBefore; }));
    CHECK(spy.last("phi/device/10/power")->payload == "true");
    core.failChannels.clear();

    // --- the adapter's link is the device's availability -------------------------------------
    feed(core, "event.adapter.connectionStateChanged", {{"adapterId", 1}, {"connected", false}});
    CHECK(waitFor(loop, [&]() { return spy.last("phi/device/10/availability")->payload == "offline"; }));
    feed(core, "event.adapter.connectionStateChanged", {{"adapterId", 1}, {"connected", true}});
    CHECK(waitFor(loop, [&]() { return spy.last("phi/device/10/availability")->payload == "online"; }));

    // --- a room renamed reaches its devices ----------------------------------------------------
    feed(core, "event.room.updated", {{"room", {{"id", 5}, {"name", "Kitchenette"}, {"roomClassName", "Kitchen"}, {"floorId", 0}}}});
    CHECK(waitFor(loop, [&]() { return Json::parse(spy.last("phi/room/5")->payload)["name"] == "Kitchenette"; }));
    CHECK(waitFor(loop, [&]() { return Json::parse(spy.last("phi/device/10")->payload)["room"] == "Kitchenette"; }));

    // --- a device removed: every topic it had is cleared -------------------------------------------
    const Json removed = device(11, 2, "Hall sensor", 0, Json::array())["device"];
    feed(core, "event.device.removed", {{"device", removed}});
    CHECK(waitFor(loop, [&]() {
        const Heard *a = spy.last("phi/device/11"), *b = spy.last("phi/device/11/availability"), *c = spy.last("phi/device/11/temperature");
        return a && b && c && a->payload.empty() && b->payload.empty() && c->payload.empty();
    }));
    CHECK(waitFor(loop, [&]() { return Json::parse(spy.last("phi/bridge/devices")->payload).size() == 1; }));
    {
        Spy late(loop, port, "late3", "phi/device/+/#");
        settle(loop, 400);
        CHECK(late.count("phi/device/11") == 0 && late.count("phi/device/11/temperature") == 0);
        CHECK(late.count("phi/device/10") == 1);
    }

    // --- a retained command is not obeyed when it is replayed ----------------------------------------
    plugin->stop();
    CHECK(waitFor(loop, [&]() { return spy.last("phi/bridge/state")->payload == "offline"; }));
    {
        // A clean disconnect makes the broker drop the will, so "offline" is there only because stop() said it
        // before the line went down - and it is retained: a subscriber arriving now is told.
        Spy late(loop, port, "late-offline", "phi/bridge/state");
        CHECK(waitFor(loop, [&]() { return late.last("phi/bridge/state") != nullptr; }));
        CHECK(late.last("phi/bridge/state") && late.last("phi/bridge/state")->payload == "offline" && late.last("phi/bridge/state")->retained);
    }
    hand.publish("phi/device/10/power/set", "OFF", true);
    settle(loop, 200);
    core.calls.clear();
    core.logs.clear();
    core.holdDevices = false;
    CHECK(plugin->start(configFor(port, "phi", false), &error));
    CHECK(waitFor(loop, [&]() { return spy.last("phi/bridge/state")->payload == "online"; }));
    CHECK(waitFor(loop, [&]() { return core.warned("retained"); }));
    settle(loop, 300);
    CHECK(core.callsTo("cmd.channel.invoke").empty());
    hand.publish("phi/device/10/power/set", "", true); // clear it again
    settle(loop, 200);

    plugin->stop();
    plugin.reset();
}

void runReadOnlyAndAccount(Loop &loop, int port)
{
    Spy spy(loop, port, "spy-ro", "phiro/#");
    Spy hand(loop, port, "hand-ro", "");
    CHECK(waitFor(loop, [&]() { return spy.connected() && hand.connected(); }));

    FakeCore core(loop);
    auto plugin = std::make_unique<phi::endpoint::MqttTransport>();
    core.plugin = plugin.get();
    TransportHost::attach(plugin.get(), &core, &loop);
    std::string error;
    CHECK(plugin->start(configFor(port, "phiro", true), &error));
    CHECK(waitFor(loop, [&]() { return spy.last("phiro/device/10/power") != nullptr; }));
    // Read-only: the state is there, a command is not heard.
    hand.publish("phiro/device/10/power/set", "OFF");
    hand.publish("phiro/scene/20/set", "ON");
    settle(loop, 500);
    CHECK(core.callsTo("cmd.channel.invoke").empty() && core.callsTo("cmd.scene.invoke").empty());
    plugin->stop();

    // Without broker{} the account comes from the host layer; an answer without the endpoint's own account is asked again, not guessed.
    FakeCore core2(loop);
    auto plugin2 = std::make_unique<phi::endpoint::MqttTransport>();
    core2.plugin = plugin2.get();
    TransportHost::attach(plugin2.get(), &core2, &loop);
    CHECK(plugin2->start("{\"hosting\":\"sidecar\"}", &error));
    CHECK(waitFor(loop, [&]() { return core2.credentialsAnswered; }));
    settle(loop, 300);
    CHECK(core2.callsTo("cmd.host.broker.credentials").size() == 1);
    plugin2->stop();
}


bool logged(const FakeCore &core, LogLevel level, const std::string &needle, std::int64_t number = -1)
{
    for (const LogEntry &e : core.logs) {
        if (e.level != level || e.message.find(needle) == std::string::npos) continue;
        if (number < 0) return true;
        for (const Scalar &sc : e.params)
            if (const auto *i = std::get_if<std::int64_t>(&sc); i && *i == number) return true;
    }
    return false;
}

Json parsed(const Heard *h)
{
    return h ? Json::parse(h->payload, nullptr, false) : Json();
}

// Home Assistant discovery, the sweep and what stop() leaves behind.
void runDiscovery(Loop &loop, int port)
{
    const std::string box = phi::endpoint::boxIdFor(phi::endpoint::currentHostname(), "1");
    const auto config = [&](const std::string &object) { return "homeassistant/device/phi_" + box + "_" + object + "/config"; };
    Spy spy(loop, port, "spy-d", "phid/#");
    Spy ha(loop, port, "spy-ha", "homeassistant/#");
    Spy hand(loop, port, "hand-d", "");
    CHECK(waitFor(loop, [&]() { return spy.connected() && ha.connected() && hand.connected(); }));

    // What an earlier run left: state and a discovery config of devices that are gone, and somebody else's config.
    hand.publish("phid/device/99/power", "true", true);
    hand.publish("phid/room/77", "{}", true);
    hand.publish(config("99"), "{\"cmps\":{}}", true);
    hand.publish("homeassistant/device/zigbee_abc/config", "{\"cmps\":{}}", true);
    hand.publish("homeassistant/device/phi_" + box + "x_1/config", "{\"cmps\":{}}", true); // another box whose id starts like ours
    settle(loop, 300);

    FakeCore core(loop);
    auto plugin = std::make_unique<phi::endpoint::MqttTransport>();
    core.plugin = plugin.get();
    plugin->setRepublishWindow(std::chrono::milliseconds(200));
    TransportHost::attach(plugin.get(), &core, &loop);
    std::string error;
    CHECK(plugin->start(configWithDiscovery(port, "phid", false, true), &error));

    CHECK(waitFor(loop, [&]() { return ha.last(config("10")) && ha.last(config("11")) && ha.last(config("scenes")); }));
    // --- what appears ------------------------------------------------------------------------------
    {
        const Json j = parsed(ha.last(config("10")));
        CHECK(j["dev"]["ids"] == Json::array({"phi_" + box + "_10"}) && j["dev"]["name"] == "Kitchen light" && j["dev"]["sa"] == "Kitchen");
        CHECK(j["o"]["name"] == "phi" && j["o"]["sw"] == "0.3.0");
        CHECK(j["availability_mode"] == "all" && j["availability"].size() == 2);
        CHECK(j["availability"][0]["topic"] == "phid/bridge/state" && j["availability"][1]["topic"] == "phid/device/10/availability");
        CHECK(j["cmps"]["power"]["p"] == "light" && j["cmps"]["power"]["brightness_command_topic"] == "phid/device/10/brightness/set");
        CHECK(j["cmps"]["power"]["unique_id"] == "phi_" + box + "_10_power");
        CHECK(j["cmps"]["button1"]["p"] == "event" && j["cmps"]["button1"]["state_topic"] == "phid/device/10/button1");
        CHECK(!j["cmps"].contains("brightness"));
    }
    CHECK(parsed(ha.last(config("11")))["cmps"]["temperature"]["device_class"] == "temperature");
    CHECK(parsed(ha.last(config("scenes")))["cmps"]["scene_20"]["command_topic"] == "phid/scene/20/set");
    CHECK(ha.count(config("12")) == 0); // the excluded adapter's plug

    // --- the sweep: stale ours cleared, everything else left alone ------------------------------------
    CHECK(waitFor(loop, [&]() { return logged(core, LogLevel::Info, "swept", 3); }, 6000));
    CHECK(waitFor(loop, [&]() {
        const Heard *a = spy.last("phid/device/99/power"), *b = spy.last("phid/room/77"), *c = ha.last(config("99"));
        return a && b && c && a->payload.empty() && b->payload.empty() && c->payload.empty();
    }));
    CHECK(ha.last("homeassistant/device/zigbee_abc/config")->payload == "{\"cmps\":{}}");
    CHECK(ha.last("homeassistant/device/phi_" + box + "x_1/config")->payload == "{\"cmps\":{}}");
    {
        // Retained, and complete: the sweep took nothing of what is current.
        Spy late(loop, port, "late-d", "#");
        settle(loop, 400);
        CHECK(late.last(config("10")) && late.last(config("10"))->retained && !late.last(config("10"))->payload.empty());
        CHECK(late.last("phid/device/10") && late.last("phid/bridge/state") && late.last("phid/bridge/state")->payload == "online");
        CHECK(late.count(config("99")) == 0 && late.count("phid/device/99/power") == 0);
    }

    // --- Home Assistant restarts: everything again, after a random moment --------------------------------
    const std::size_t before10 = ha.count(config("10")), beforeScenes = ha.count(config("scenes"));
    hand.publish("homeassistant/status", "offline");
    settle(loop, 500);
    CHECK(ha.count(config("10")) == before10);
    hand.publish("homeassistant/status", "online");
    CHECK(waitFor(loop, [&]() { return ha.count(config("10")) == before10 + 1 && ha.count(config("scenes")) == beforeScenes + 1 && ha.count(config("11")) > 1; }));

    // --- a component that vanishes is named alone first, then dropped -------------------------------------
    {
        Json changed = device(10, 1, "Kitchen light", 5,
                              Json::array({channel(100, "power", 1, 1, 7, true), channel(101, "brightness", 10, 2, 7, 80)}));
        const std::size_t seen = ha.heard.size();
        feed(core, "event.device.changed", {{"device", changed["device"]}, {"channels", changed["channels"]}});
        CHECK(waitFor(loop, [&]() { return ha.heard.size() >= seen + 2; }));
        std::vector<Json> updates;
        for (std::size_t i = seen; i < ha.heard.size(); ++i)
            if (ha.heard[i].topic == config("10")) updates.push_back(Json::parse(ha.heard[i].payload));
        CHECK(updates.size() == 2);
        if (updates.size() == 2) {
            CHECK(updates[0]["cmps"]["button1"] == (Json{{"p", "event"}}) && updates[0]["cmps"].contains("power"));
            CHECK(!updates[1]["cmps"].contains("button1") && updates[1]["cmps"].contains("power"));
        }
    }

    // --- a room renamed, a scene added, a device removed --------------------------------------------------
    feed(core, "event.room.updated", {{"room", {{"id", 5}, {"name", "Cooking"}, {"roomClassName", "Kitchen"}, {"floorId", 0}}}});
    CHECK(waitFor(loop, [&]() { return parsed(ha.last(config("10")))["dev"]["sa"] == "Cooking"; }));
    feed(core, "event.scene.updated", {{"scene", {{"id", 22}, {"name", "Morning"}, {"roomId", 0}}}});
    CHECK(waitFor(loop, [&]() { return parsed(ha.last(config("scenes")))["cmps"].contains("scene_22"); }));
    feed(core, "event.device.removed", {{"device", device(11, 2, "Hall sensor", 0, Json::array())["device"]}});
    CHECK(waitFor(loop, [&]() { return ha.last(config("11")) && ha.last(config("11"))->payload.empty(); }));

    // --- a clean stop leaves "offline" on the broker, retained --------------------------------------------------
    plugin->stop();
    {
        Spy late(loop, port, "late-d2", "phid/bridge/state");
        CHECK(waitFor(loop, [&]() { return late.last("phid/bridge/state") != nullptr; }));
        CHECK(late.last("phid/bridge/state")->payload == "offline" && late.last("phid/bridge/state")->retained);
    }

    // --- read-only still hears Home Assistant, and still hears no command ---------------------------------------
    core.calls.clear();
    CHECK(plugin->start(configWithDiscovery(port, "phid", true, true), &error));
    CHECK(waitFor(loop, [&]() { return spy.last("phid/bridge/state") && spy.last("phid/bridge/state")->payload == "online" && ha.last(config("10")); }));
    settle(loop, 300);
    const std::size_t roBefore = ha.count(config("10"));
    hand.publish("homeassistant/status", "online");
    CHECK(waitFor(loop, [&]() { return ha.count(config("10")) > roBefore; }));
    hand.publish("phid/device/10/power/set", "OFF");
    settle(loop, 300);
    CHECK(core.callsTo("cmd.channel.invoke").empty());
    plugin->stop();

    // --- Home Assistant mode off: nothing published, what the earlier run left is cleared --------------------------
    CHECK(ha.last(config("10")) && !ha.last(config("10"))->payload.empty()); // the configs are still on the broker
    const std::size_t scenesBefore = ha.count(config("scenes"));
    core.logs.clear();
    CHECK(plugin->start(configWithDiscovery(port, "phid", false, false), &error));
    CHECK(waitFor(loop, [&]() { return logged(core, LogLevel::Info, "swept"); }, 6000));
    CHECK(waitFor(loop, [&]() { return ha.last(config("10"))->payload.empty() && ha.last(config("scenes"))->payload.empty(); }));
    CHECK(ha.count(config("scenes")) == scenesBefore + 1); // the clear, and no new payload
    CHECK(ha.last("homeassistant/device/zigbee_abc/config")->payload == "{\"cmps\":{}}");
    plugin->stop();
}

// The management card: summary, republish, sweep and settings against a core that keeps the override.
void runManagement(Loop &loop, int port)
{
    Spy spy(loop, port, "spy-m", "phim/#");
    Spy hand(loop, port, "hand-m", "");
    CHECK(waitFor(loop, [&]() { return spy.connected() && hand.connected(); }));

    FakeCore core(loop);
    auto plugin = std::make_unique<phi::endpoint::MqttTransport>();
    core.plugin = plugin.get();
    TransportHost::attach(plugin.get(), &core, &loop);
    const auto actionIds = [&]() {
        std::vector<std::string> ids;
        const Json d = Json::parse(plugin->describeManagement());
        if (d.contains("actions"))
            for (const Json &a : d["actions"]) ids.push_back(a["id"]);
        return ids;
    };
    const auto summary = [&]() { return Json::parse(plugin->describeManagement()).value("summary", std::string()); };
    CmdId nextAction = 9000;

    // --- before start: a summary, no crash, no action taken --------------------------------------
    CHECK(summary() == "not running");
    CHECK(actionIds() == (std::vector<std::string>{"republish", "sweep", "settings"}));
    CHECK(plugin->invokeAction(++nextAction, "republish", "{}"));
    CHECK(core.completed[nextAction]["status"] == 1);
    CHECK(!plugin->invokeAction(++nextAction, "nothing", "{}"));

    // --- running: the broker, the account, the counts ------------------------------------------------
    // Stored by hand: a user and password the broker takes without checking, so the keep rule is seen on the wire.
    const Json running = {{"hosting", "sidecar"}, {"enabled", true}, {"baseTopic", "phim"}, {"excludeAdapters", Json::array({"tuya"})},
                          {"broker", {{"host", "127.0.0.1"}, {"port", port}, {"username", "household"}, {"password", "stored-secret"}}},
                          {"discovery", {{"homeAssistant", true}}}};
    core.configEffective = running;
    core.configOverride = {{"enabled", true}, {"future", {{"a", 1}}}, {"baseTopic", "phim"}, {"excludeAdapters", Json::array({"tuya"})},
                           {"broker", running["broker"]}, {"discovery", {{"homeAssistant", true}}}};
    std::string error;
    CHECK(plugin->start(running.dump(), &error));
    CHECK(waitFor(loop, [&]() { return spy.last("phim/bridge/devices") != nullptr; }));
    CHECK(waitFor(loop, [&]() { return summary().find("on the broker 127.0.0.1:" + std::to_string(port)) == 0; }));
    {
        const std::string text = summary();
        CHECK(text.find(" as household, 2 devices, ") != std::string::npos);
        CHECK(text.find(" topics, Home Assistant discovery on") != std::string::npos);
        CHECK(text.find("read-only") == std::string::npos);
        const Json d = Json::parse(plugin->describeManagement());
        CHECK(d["actions"].size() == 3);
        CHECK(d["actions"][1]["id"] == "sweep" && d["actions"][1].contains("confirm") && !d["actions"][1].value("danger", false));
        CHECK(d["actions"][2]["id"] == "settings" && d["actions"][2]["hasForm"] == true);
        std::map<std::string, Json> defaults;
        for (const Json &f : d["actions"][2]["form"]["fields"]) defaults[f["key"]] = f["default"];
        CHECK(defaults.size() == 11);
        CHECK(defaults["baseTopic"] == "phim" && defaults["useBroker"] == true && defaults["brokerHost"] == "127.0.0.1");
        CHECK(defaults["brokerPort"] == port && defaults["brokerUsername"] == "household" && defaults["brokerPassword"] == "");
        CHECK(defaults["homeAssistant"] == true && defaults["readOnly"] == false);
    }
    settle(loop, 2800); // the sweep every start runs, and the discovery

    // --- republish: everything again, with the count --------------------------------------------------
    {
        const std::size_t before = spy.count("phim/device/10/power");
        const CmdId id = ++nextAction;
        CHECK(plugin->invokeAction(id, "republish", "{}"));
        CHECK(core.completed.count(id) == 1 && core.completed[id]["status"] == 0);
        const std::string text = core.completed[id].value("resultValue", std::string());
        CHECK(text.size() > 19 && text.substr(text.size() - 19) == " topics republished");
        CHECK(waitFor(loop, [&]() { return spy.count("phim/device/10/power") > before; }));
        // The count is what the summary says it holds, plus the clears that were waiting (none).
        const std::size_t n = std::stoul(text);
        CHECK(n > 10);
        CHECK(summary().find(std::to_string(n) + " topics") != std::string::npos);
    }

    // --- sweep: what is stale goes, a second one at the same time is refused ------------------------------
    {
        hand.publish("phim/device/99/power", "true", true);
        settle(loop, 300);
        const CmdId first = ++nextAction, second = ++nextAction;
        CHECK(plugin->invokeAction(first, "sweep", "{}"));
        CHECK(plugin->invokeAction(second, "sweep", "{}"));
        CHECK(core.completed[second]["status"] == 5); // Busy
        CHECK(waitFor(loop, [&]() { return core.completed.count(first) == 1; }, 6000));
        CHECK(core.completed[first]["status"] == 0 && core.completed[first]["resultValue"] == "1 stale topic(s) swept");
        CHECK(waitFor(loop, [&]() { return spy.last("phim/device/99/power") && spy.last("phim/device/99/power")->payload.empty(); }));
        // Nothing left: zero is an answer too.
        const CmdId again = ++nextAction;
        CHECK(plugin->invokeAction(again, "sweep", "{}"));
        CHECK(waitFor(loop, [&]() { return core.completed.count(again) == 1; }, 6000));
        CHECK(core.completed[again]["resultValue"] == "0 stale topic(s) swept");
    }

    // --- settings: the override core receives -----------------------------------------------------------
    core.calls.clear();
    {
        const CmdId id = ++nextAction;
        // The password field comes back empty: the stored one stays.
        const Json form = {{"homeAssistant", false}, {"discoveryPrefix", "homeassistant"}, {"readOnly", true}, {"baseTopic", "phim"},
                           {"useBroker", true}, {"brokerHost", "127.0.0.1"}, {"brokerPort", port}, {"brokerUsername", "household"},
                           {"brokerPassword", ""}, {"brokerTls", false}, {"brokerCaFile", ""}};
        CHECK(plugin->invokeAction(id, "settings", form.dump()));
        CHECK(waitFor(loop, [&]() { return core.completed.count(id) == 1; }));
        CHECK(core.completed[id]["status"] == 0 && core.completed[id]["resultValue"] == "Saved; the transport restarts.");
        const auto gets = core.callsTo("cmd.transport.config.get"), sets = core.callsTo("cmd.transport.config.set");
        CHECK(gets.size() == 1 && gets[0].payload == (Json{{"pluginType", "mqtt"}}));
        CHECK(sets.size() == 1);
        if (sets.size() == 1) {
            const Json &p = sets[0].payload;
            CHECK(p["pluginType"] == "mqtt" && p["restart"] == true);
            Json expected = {{"enabled", true}, {"future", {{"a", 1}}}, {"baseTopic", "phim"}, {"excludeAdapters", Json::array({"tuya"})},
                             {"readOnly", true}, {"discovery", {{"homeAssistant", false}, {"prefix", "homeassistant"}}},
                             {"broker", {{"host", "127.0.0.1"}, {"port", port}, {"username", "household"}, {"password", "stored-secret"}}}};
            CHECK(p["override"] == expected);
            if (p["override"] != expected) std::printf("  got: %s\n", p["override"].dump().c_str());
        }
        for (const FakeCore::Call &c : core.calls) CHECK(c.kind == CallerIdentity::Kind::TrustedLocal && c.clientId == "mqtt");
    }

    // A form core's parser would refuse never reaches core.set.
    core.calls.clear();
    {
        const CmdId id = ++nextAction;
        CHECK(plugin->invokeAction(id, "settings", Json{{"useBroker", true}, {"brokerHost", "127.0.0.1"}, {"brokerPort", 0}}.dump()));
        CHECK(waitFor(loop, [&]() { return core.completed.count(id) == 1; }));
        CHECK(core.completed[id]["status"] == 4 && core.completed[id]["error"]["message"].get<std::string>().find("port") != std::string::npos);
        CHECK(core.callsTo("cmd.transport.config.set").empty());
        const CmdId junk = ++nextAction;
        CHECK(plugin->invokeAction(junk, "settings", "not json"));
        CHECK(core.completed[junk]["status"] == 4);
    }
    // core's refusal is the action's error.
    {
        core.failConfigSet = true;
        const CmdId id = ++nextAction;
        CHECK(plugin->invokeAction(id, "settings", Json{{"readOnly", false}}.dump()));
        CHECK(waitFor(loop, [&]() { return core.completed.count(id) == 1; }));
        CHECK(core.completed[id]["status"] == 1 && core.completed[id]["error"]["message"].get<std::string>().find("override not writable") != std::string::npos);
        core.failConfigSet = false;
    }
    // core restarts the transport before it answers: the save still ends as saved.
    {
        core.stopOnConfigSet = true;
        const CmdId id = ++nextAction;
        CHECK(plugin->invokeAction(id, "settings", Json{{"readOnly", false}}.dump()));
        CHECK(waitFor(loop, [&]() { return core.completed.count(id) == 1; }));
        CHECK(core.completed[id]["status"] == 0 && core.completed[id]["resultValue"] == "Saved; the transport restarts.");
        core.stopOnConfigSet = false;
    }

    // --- after stop: a summary and an error, no crash ----------------------------------------------------------
    CHECK(summary() == "not running");
    CHECK(plugin->invokeAction(++nextAction, "sweep", "{}") && core.completed[nextAction]["status"] == 1);
    CHECK(plugin->invokeAction(++nextAction, "settings", "{}") && core.completed[nextAction]["status"] == 1);

    // --- without a broker account: it says what it waits for -----------------------------------------------------
    CHECK(plugin->start("{\"hosting\":\"sidecar\"}", &error));
    CHECK(waitFor(loop, [&]() { return core.credentialsAnswered; }));
    CHECK(summary() == "waiting for the broker account");
    CHECK(plugin->invokeAction(++nextAction, "republish", "{}") && core.completed[nextAction]["status"] == 1);
    plugin->stop();

    // --- a broker that is not there ---------------------------------------------------------------------------------
    CHECK(plugin->start(Json{{"hosting", "sidecar"}, {"broker", {{"host", "127.0.0.1"}, {"port", freePort()}}}}.dump(), &error));
    CHECK(waitFor(loop, [&]() { return summary().find("broker not reachable: ") == 0; }, 8000));
    plugin->stop();
}

} // namespace

int main()
{
    if (::access(kBroker, X_OK) != 0) {
        std::printf("mqtt_integration_tests: no %s, skipped\n", kBroker);
        return 77;
    }
    const int port = freePort();
    const std::string portText = std::to_string(port);
    char *argv[] = {const_cast<char *>(kBroker), const_cast<char *>("-p"), const_cast<char *>(portText.c_str()), nullptr};
    // The broker logs every connection; the test's own output is what matters.
    posix_spawn_file_actions_t quiet;
    posix_spawn_file_actions_init(&quiet);
    posix_spawn_file_actions_addopen(&quiet, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&quiet, 2, "/dev/null", O_WRONLY, 0);
    pid_t broker = 0;
    const int spawned = ::posix_spawn(&broker, kBroker, &quiet, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&quiet);
    if (spawned != 0) {
        std::printf("mqtt_integration_tests: cannot start %s\n", kBroker);
        return 1;
    }
    for (int i = 0; i < 100 && !listening(port); ++i) ::usleep(50 * 1000);

    {
        EpollLoop loop;
        runEndpoint(loop, port);
        runReadOnlyAndAccount(loop, port);
        runDiscovery(loop, port);
        runManagement(loop, port);
    }

    ::kill(broker, SIGTERM);
    ::waitpid(broker, nullptr, 0);
    if (g_failures) {
        std::printf("mqtt_integration_tests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("mqtt_integration_tests: ok\n");
    return 0;
}
