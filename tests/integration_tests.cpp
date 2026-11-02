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

    void completeAction(CmdId, std::string_view) override {}

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

std::string configFor(int port, const std::string &base, bool readOnly)
{
    return Json{{"hosting", "sidecar"}, {"baseTopic", base}, {"readOnly", readOnly}, {"excludeAdapters", Json::array({"tuya"})},
                {"broker", {{"host", "127.0.0.1"}, {"port", port}}}}.dump();
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
        CHECK(j["instance"] == "Test House" && j["tenant"] == "1" && j["version"] == "0.1.0" && j["sdk"] == kTransportApiVersion);
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
