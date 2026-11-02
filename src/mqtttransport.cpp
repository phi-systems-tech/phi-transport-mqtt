#include "mqtttransport.h"

#include "command.h"
#include "payload.h"

#include <nlohmann/json.hpp>

#include <algorithm>

#ifndef PHI_MQTT_VERSION
#define PHI_MQTT_VERSION "0"
#endif

namespace phi::endpoint {

using namespace phicore::transport;
using Json = nlohmann::json;

namespace {

constexpr const char *kPluginType = "mqtt";
constexpr const char *kTopicHello = "sync.hello.get";
constexpr const char *kTopicAdaptersList = "cmd.adapters.list";
constexpr const char *kTopicRoomsList = "cmd.rooms.list";
constexpr const char *kTopicScenesList = "cmd.scenes.list";
constexpr const char *kTopicDevicesList = "cmd.devices.list";
constexpr const char *kTopicChannelInvoke = "cmd.channel.invoke";
constexpr const char *kTopicSceneInvoke = "cmd.scene.invoke";
constexpr const char *kTopicBrokerCredentials = "cmd.host.broker.credentials";
// The backend phi-hostd mints the broker account for: kind `mqtt`, id `main`
// (share/intent-backend-mqtt.json).
constexpr const char *kBackendName = "mqtt@main";

// Events held back until the first list is in; past this, one full resync.
constexpr std::size_t kMaxEarlyEvents = 5000;
constexpr std::chrono::seconds kRetryCore{10};
constexpr std::chrono::seconds kRetryCredentials{30};
// core announces a scene when it changes but never when it goes away, so the
// list is read again now and then.
constexpr std::chrono::seconds kSceneRefresh{60};
// Many device changes arrive in a burst (an adapter reconnecting); the
// description of the whole house is said once after it.
constexpr std::chrono::milliseconds kBridgeDebounce{250};

// The broker's ACL is the credential here, the argument the Matter bridge's
// fabrics and the voice satellites' household key make: a writer on the
// endpoint's topics was admitted by whoever configured the broker. Honest only
// while the broker's writers are the box's own accounts or a broker the
// household configured itself.
CallerIdentity mqttIdentity()
{
    CallerIdentity caller;
    caller.kind = CallerIdentity::Kind::TrustedLocal;
    caller.clientId = "mqtt";
    return caller;
}

std::string textOf(const Json &j, const char *key)
{
    const auto it = j.find(key);
    if (it == j.end()) return {};
    if (it->is_string()) return it->get<std::string>();
    if (it->is_number_integer()) return std::to_string(it->get<std::int64_t>());
    return {};
}

std::string errorTextOf(const Json &payload)
{
    const auto it = payload.find("error");
    if (it == payload.end() || it->is_null()) return {};
    if (it->is_string()) return it->get<std::string>();
    if (it->is_object()) {
        const auto m = it->find("message");
        if (m != it->end() && m->is_string()) return m->get<std::string>();
    }
    return {};
}

} // namespace

MqttTransport::MqttTransport() = default;

MqttTransport::~MqttTransport()
{
    // core stops a transport before it deletes it; this covers a plugin dropped without that.
    m_running = false;
    m_link.reset();
}

std::string MqttTransport::pluginType() const { return kPluginType; }
std::string MqttTransport::displayName() const { return "MQTT endpoint"; }
std::string MqttTransport::description() const
{
    return "phi's devices, channels, rooms and scenes on an MQTT broker, and commands from it";
}

void MqttTransport::logInfo(std::string_view ctx, std::string_view message, ScalarList params) const
{
    writeLog(LogLevel::Info, makeCategory(LogCategory::Transport), message, std::move(params), ctx);
}

void MqttTransport::logWarn(std::string_view ctx, std::string_view message, ScalarList params) const
{
    writeLog(LogLevel::Warn, makeCategory(LogCategory::Transport), message, std::move(params), ctx);
}

// --- lifecycle ---------------------------------------------------------------

bool MqttTransport::start(std::string_view configJson, std::string *errorString)
{
    if (m_running) stop();
    std::string error;
    if (!parseConfig(configJson, &m_config, &error)) {
        if (errorString) *errorString = std::move(error);
        return false;
    }
    m_topics = Topics(m_config.baseTopic);
    m_model = Model();
    m_model.setConfig(m_config);
    m_retained = Retained();
    m_pending.clear();
    m_earlyEvents.clear();
    m_earlyOverflow = false;
    m_syncTouched = Touched();
    m_synced = false;
    m_haveHello = false;
    m_running = true;
    // The world is pulled once the loop is back: start() has core waiting.
    m_bootstrap = runtimeLoop()->timerAfter(std::chrono::milliseconds(50), [this]() { bootstrap(); });
    logInfo("mqtt.start", "MQTT endpoint started on base topic %1%2",
            {Scalar{m_config.baseTopic}, Scalar{std::string(m_config.readOnly ? ", read-only" : "")}});
    return true;
}

void MqttTransport::stop()
{
    if (!m_running && !m_link) return;
    m_running = false;
    m_bootstrap.reset();
    m_credentialsRetry.reset();
    m_bridgeTimer.reset();
    m_sceneRefresh.reset();
    m_link.reset(); // says "offline" on the way out
    m_pending.clear();
    m_earlyEvents.clear();
}

// --- the house from core ------------------------------------------------------

void MqttTransport::bootstrap()
{
    m_bootstrap.reset();
    if (!m_running) return;
    if (!m_haveHello) {
        // Which instance this is: topics and ids that must stay unique across boxes use it later.
        const SyncResult r = callCoreSync(kTopicHello, "{}", 1500, mqttIdentity());
        if (!r.accepted) {
            logWarn("mqtt.sync", "sync.hello.get refused: %1",
                    {Scalar{r.error ? r.error->message : std::string("no reason")}});
            m_bootstrap = runtimeLoop()->timerAfter(kRetryCore, [this]() { bootstrap(); });
            return;
        }
        const Json hello = Json::parse(r.payloadJson, nullptr, false);
        if (hello.is_object()) {
            m_tenant = textOf(hello, "tenant");
            m_instance = textOf(hello, "instanceLabel");
        }
        m_haveHello = true;
    }
    startSync();
}

void MqttTransport::retryBootstrap()
{
    m_bootstrap = runtimeLoop()->timerAfter(kRetryCore, [this]() { bootstrap(); });
}

// adapters, rooms, scenes, then devices: the devices answer last, so what
// they name is already known when they land.
void MqttTransport::startSync()
{
    request(PendingKind::Adapters);
}

void MqttTransport::request(PendingKind kind)
{
    const char *topic = kTopicAdaptersList;
    switch (kind) {
    case PendingKind::Adapters: topic = kTopicAdaptersList; break;
    case PendingKind::Rooms: topic = kTopicRoomsList; break;
    case PendingKind::Scenes:
    case PendingKind::ScenesRefresh: topic = kTopicScenesList; break;
    case PendingKind::Devices: topic = kTopicDevicesList; break;
    default: return;
    }
    const AsyncResult r = callCoreAsync(topic, "{}", mqttIdentity());
    if (!r.accepted) {
        logWarn("mqtt.sync", "%1 refused: %2", {Scalar{std::string(topic)}, Scalar{r.error ? r.error->message : std::string("no reason")}});
        if (kind != PendingKind::ScenesRefresh) retryBootstrap();
        return;
    }
    m_pending[r.cmdId] = Pending{kind};
}

void MqttTransport::onCoreAsyncResult(CmdId cmdId, std::string_view payloadJson)
{
    const auto it = m_pending.find(cmdId);
    if (it == m_pending.end()) return;
    const Pending pending = it->second;
    m_pending.erase(it);
    if (!m_running) return;
    const Json payload = Json::parse(payloadJson, nullptr, false);
    if (!payload.is_object()) return;
    try {
        switch (pending.kind) {
        case PendingKind::Adapters:
            m_syncTouched.merge(m_model.applyAdapters(payload.value("adapters", Json::array())));
            request(PendingKind::Rooms);
            break;
        case PendingKind::Rooms:
            m_syncTouched.merge(m_model.applyRooms(payload.value("rooms", Json::array())));
            request(PendingKind::Scenes);
            break;
        case PendingKind::Scenes:
            m_syncTouched.merge(m_model.applyScenes(payload.value("scenes", Json::array())));
            request(PendingKind::Devices);
            break;
        case PendingKind::ScenesRefresh:
            refresh(m_model.applyScenes(payload.value("scenes", Json::array())));
            break;
        case PendingKind::Devices:
            m_syncTouched.merge(m_model.applyDevices(payload.value("devices", Json::array())));
            onSynced();
            break;
        case PendingKind::Credentials:
            onBrokerCredentials(payload);
            break;
        case PendingKind::ChannelInvoke:
        case PendingKind::SceneInvoke:
            onInvokeResult(pending, payload);
            break;
        }
    } catch (const Json::exception &e) {
        logWarn("mqtt.sync", "An answer from core was not understood: %1", {Scalar{std::string(e.what())}});
    }
}

void MqttTransport::onSynced()
{
    const bool first = !m_synced;
    Touched touched;
    touched.merge(m_syncTouched);
    m_syncTouched = Touched();
    refresh(touched);
    refreshBridge();
    m_synced = true;
    logInfo("mqtt.sync", "MQTT endpoint serves %1 device(s), %2 room(s), %3 scene(s)",
            {Scalar{static_cast<std::int64_t>(m_model.devices().size())},
             Scalar{static_cast<std::int64_t>(m_model.rooms().size())},
             Scalar{static_cast<std::int64_t>(m_model.scenes().size())}});
    if (!first) return;
    replayEarlyEvents();
    m_sceneRefresh = runtimeLoop()->timerEvery(std::chrono::duration_cast<std::chrono::milliseconds>(kSceneRefresh), [this]() {
        if (m_running && m_synced) request(PendingKind::ScenesRefresh);
    });
    // The broker only now: whoever reads "online" finds the house complete.
    if (m_config.broker) {
        BrokerLink::Account account;
        account.host = m_config.broker->host;
        account.port = m_config.broker->port;
        account.username = m_config.broker->username;
        account.password = m_config.broker->password;
        account.tls = m_config.broker->tls;
        account.caFile = m_config.broker->caFile;
        startLink(std::move(account));
    } else {
        requestBrokerAccount();
    }
}

void MqttTransport::replayEarlyEvents()
{
    std::vector<std::pair<std::string, Json>> early;
    early.swap(m_earlyEvents);
    for (const auto &[topic, payload] : early) {
        try {
            handleEvent(topic, payload);
        } catch (const Json::exception &e) {
            logWarn("mqtt.event", "An event from core was not understood: %1", {Scalar{std::string(e.what())}});
        }
    }
    if (m_earlyOverflow) {
        m_earlyOverflow = false;
        startSync();
    }
}

// --- the broker account --------------------------------------------------------

void MqttTransport::requestBrokerAccount()
{
    m_credentialsRetry.reset();
    if (!m_running) return;
    const AsyncResult r = callCoreAsync(kTopicBrokerCredentials, "{}", mqttIdentity());
    if (!r.accepted) {
        logWarn("mqtt.broker", "cmd.host.broker.credentials refused: %1",
                {Scalar{r.error ? r.error->message : std::string("no reason")}});
        m_credentialsRetry = runtimeLoop()->timerAfter(kRetryCredentials, [this]() { requestBrokerAccount(); });
        return;
    }
    m_pending[r.cmdId] = Pending{PendingKind::Credentials};
}

void MqttTransport::onBrokerCredentials(const Json &payload)
{
    BrokerLink::Account account;
    bool found = false;
    if (const auto it = payload.find("credentials"); it != payload.end() && it->is_array()) {
        for (const Json &c : *it) {
            if (!c.is_object() || textOf(c, "backend") != kBackendName) continue;
            account.username = textOf(c, "username");
            account.password = textOf(c, "password");
            found = !account.username.empty();
        }
    }
    if (!found) {
        // The backend intent is applied by phi-hostd after the package lands; ask again later.
        logInfo("mqtt.broker", "No broker account for %1 yet", {Scalar{std::string(kBackendName)}});
        m_credentialsRetry = runtimeLoop()->timerAfter(kRetryCredentials, [this]() { requestBrokerAccount(); });
        return;
    }
    startLink(std::move(account));
}

void MqttTransport::startLink(BrokerLink::Account account)
{
    m_link = std::make_unique<BrokerLink>(runtimeLoop(), [this](int level, const std::string &text) {
        // 1 debug, 2 info, 3 warn
        writeLog(static_cast<LogLevel>(level + 1), makeCategory(LogCategory::Transport), "%1",
                 {Scalar{text}}, "mqtt.broker");
    });
    m_link->onConnected([this]() { send(m_retained.replayAll()); });
    m_link->onDisconnected([this]() { m_retained.setOffline(); });
    m_link->onMessage([this](const phi::runtime::mqtt::Message &m) { onBrokerMessage(m); });
    BrokerLink::Options options;
    options.stateTopic = m_topics.bridgeState();
    // A read-only endpoint listens to nothing: no command can reach it.
    if (!m_config.readOnly) options.subscriptions = m_topics.subscriptions();
    m_link->start(std::move(account), std::move(options));
}

// --- core events ---------------------------------------------------------------

void MqttTransport::onCoreEvent(std::string_view topic, std::string_view payloadJson)
{
    if (!m_running) return;
    static const char *const handled[] = {
        "event.device.added", "event.device.changed", "event.device.removed", "event.channel.stateChanged",
        "event.adapter.added", "event.adapter.updated", "event.adapter.removed", "event.adapter.connectionStateChanged",
        "event.room.updated", "event.room.removed", "event.scene.updated",
    };
    if (std::none_of(std::begin(handled), std::end(handled), [&](const char *t) { return topic == t; })) return;
    const Json payload = Json::parse(payloadJson, nullptr, false);
    if (!payload.is_object()) return;
    // Until the first lists are in, what an event updates is not here yet; it
    // is replayed afterwards, in order.
    if (!m_synced) {
        if (m_earlyEvents.size() < kMaxEarlyEvents) m_earlyEvents.emplace_back(std::string(topic), payload);
        else m_earlyOverflow = true;
        return;
    }
    try {
        handleEvent(topic, payload);
    } catch (const Json::exception &e) {
        logWarn("mqtt.event", "An event from core was not understood: %1", {Scalar{std::string(e.what())}});
    }
}

void MqttTransport::handleEvent(std::string_view topic, const Json &payload)
{
    const auto object = [&](const char *key) -> Json {
        const auto it = payload.find(key);
        return it != payload.end() && it->is_object() ? *it : Json();
    };

    if (topic == "event.channel.stateChanged") {
        const int channelId = payload.value("channelId", 0);
        const auto value = payload.find("value");
        if (channelId <= 0 || value == payload.end()) return;
        const std::string valueName = textOf(payload, "valueName");
        const auto flags = payload.find("valueFlags");
        const Json valueFlags = flags != payload.end() && flags->is_array() ? *flags : Json();
        const Model::StateChange change = m_model.applyState(channelId, *value, valueName, valueFlags);
        if (!change.channel || m_model.excluded(*change.device)) return;
        if (change.channel->isEvent()) {
            // An occurrence: said once, never kept.
            if (m_link && m_link->connected()) {
                const std::int64_t ts = payload.value("tsMs", std::int64_t(0));
                m_link->publishEvent(m_topics.channel(change.device->id, change.channel->slug),
                                     eventPayload(*change.channel, *value, valueName, valueFlags, ts));
            }
            return;
        }
        if (!change.channel->hasState()) return;
        Touched t;
        t.devices.insert(change.device->id);
        refresh(t);
        return;
    }
    if (topic == "event.adapter.connectionStateChanged") {
        const auto connected = payload.find("connected");
        if (connected == payload.end() || !connected->is_boolean()) return;
        refresh(m_model.setAdapterConnected(payload.value("adapterId", 0), connected->get<bool>()));
        return;
    }
    if (topic == "event.adapter.added" || topic == "event.adapter.updated") {
        refresh(m_model.upsertAdapter(object("adapter")));
        return;
    }
    if (topic == "event.adapter.removed") {
        refresh(m_model.removeAdapter(object("adapter").value("id", 0)));
        return;
    }
    if (topic == "event.room.updated") {
        refresh(m_model.upsertRoom(object("room")));
        return;
    }
    if (topic == "event.room.removed") {
        refresh(m_model.removeRoom(object("room").value("id", 0)));
        return;
    }
    if (topic == "event.scene.updated") {
        refresh(m_model.upsertScene(object("scene")));
        return;
    }
    // event.device.*
    if (const Json adapter = object("adapter"); adapter.is_object()) refresh(m_model.upsertAdapter(adapter));
    const Json device = object("device");
    if (topic == "event.device.removed") {
        refresh(m_model.removeDevice(device.value("id", 0)));
        return;
    }
    const auto channels = payload.find("channels");
    refresh(m_model.upsertDevice(device, channels == payload.end() ? Json() : *channels));
}

// --- publishing ------------------------------------------------------------------

void MqttTransport::send(const std::vector<Message> &messages)
{
    if (m_link) m_link->publish(messages);
}

void MqttTransport::refresh(const Touched &touched, bool force)
{
    for (const int id : touched.devices) {
        const Device *device = m_model.device(id);
        send(m_retained.set("device/" + std::to_string(id),
                            device ? deviceTopics(m_topics, m_model, *device) : TopicMap(), force));
    }
    for (const int id : touched.rooms) {
        const Room *room = m_model.room(id);
        send(m_retained.set("room/" + std::to_string(id), room ? roomTopics(m_topics, m_model, *room) : TopicMap(), force));
    }
    for (const int id : touched.scenes) {
        const Scene *scene = m_model.scene(id);
        send(m_retained.set("scene/" + std::to_string(id), scene ? sceneTopics(m_topics, m_model, *scene) : TopicMap(), force));
    }
    if (touched.topology) scheduleBridge();
}

void MqttTransport::scheduleBridge()
{
    if (m_bridgeTimer || !m_running) return;
    m_bridgeTimer = runtimeLoop()->timerAfter(kBridgeDebounce, [this]() {
        m_bridgeTimer.reset();
        refreshBridge();
    });
}

void MqttTransport::refreshBridge()
{
    m_bridgeTimer.reset();
    BridgeInfo info;
    info.version = PHI_MQTT_VERSION;
    info.sdk = kTransportApiVersion;
    info.instance = m_instance;
    info.tenant = m_tenant;
    send(m_retained.set("bridge", bridgeTopics(m_topics, m_model, info)));
}

// The consumer's picture of a device is put back to what the house says, even
// where the retained topics already say it: whoever commanded it has not seen
// the answer.
void MqttTransport::republishDevice(int deviceId)
{
    Touched t;
    t.devices.insert(deviceId);
    refresh(t, true);
}

// --- commands from the broker -----------------------------------------------------

void MqttTransport::onBrokerMessage(const phi::runtime::mqtt::Message &message)
{
    if (!m_running || m_config.readOnly) return;
    const Topics::Command cmd = m_topics.parseCommand(message.topic);
    if (cmd.kind == Topics::Command::Kind::None) return;
    // A command is an act, not a state. A retained one is replayed at every
    // subscribe and would switch the device again each time (Tasmota's "ghost
    // switching").
    if (message.retained) {
        logWarn("mqtt.command", "Ignored a retained command on %1", {Scalar{message.topic}});
        return;
    }
    try {
        if (cmd.kind == Topics::Command::Kind::SceneSet) {
            if (!m_model.scene(cmd.id)) {
                logWarn("mqtt.command", "Scene %1 is not known", {Scalar{static_cast<std::int64_t>(cmd.id)}});
                return;
            }
            if (!parseSceneSet(message.payload)) {
                logWarn("mqtt.command", "A scene is set with ON; %1 said %2", {Scalar{message.topic}, Scalar{message.payload}});
                return;
            }
            invokeScene(cmd.id);
            return;
        }
        const Device *device = m_model.device(cmd.id);
        if (!device || m_model.excluded(*device)) {
            logWarn("mqtt.command", "Device %1 is not known", {Scalar{static_cast<std::int64_t>(cmd.id)}});
            return;
        }
        if (cmd.kind == Topics::Command::Kind::ChannelSet) {
            const Channel *channel = device->bySlug(cmd.slug);
            if (!channel) {
                logWarn("mqtt.command", "Device %1 has no channel %2", {Scalar{static_cast<std::int64_t>(cmd.id)}, Scalar{cmd.slug}});
                return;
            }
            if (!channel->writable()) {
                logWarn("mqtt.command", "Channel %1 of device %2 is read-only", {Scalar{cmd.slug}, Scalar{static_cast<std::int64_t>(cmd.id)}});
                return;
            }
            const ParsedValue parsed = parseChannelPayload(*channel, message.payload);
            if (!parsed.ok) {
                logWarn("mqtt.command", "Not a value for channel %1 of device %2: %3",
                        {Scalar{cmd.slug}, Scalar{static_cast<std::int64_t>(cmd.id)}, Scalar{parsed.error}});
                return;
            }
            invokeChannel(device->id, channel->id, parsed.value);
            return;
        }
        // device/<id>/set: one invoke per key.
        std::vector<std::pair<std::string, Json>> items;
        std::string error;
        if (!parseDeviceSet(message.payload, &items, &error)) {
            logWarn("mqtt.command", "%1: %2", {Scalar{message.topic}, Scalar{error}});
            return;
        }
        for (const auto &[key, value] : items) {
            // The device JSON's own keys: a consumer may write back what it read.
            if (key == "name" || key == "room" || key == "roomId" || key == "class") continue;
            const Channel *channel = device->bySlug(key);
            if (!channel || !channel->writable()) {
                logWarn("mqtt.command", "Device %1 has no writable channel %2", {Scalar{static_cast<std::int64_t>(cmd.id)}, Scalar{key}});
                continue;
            }
            const ParsedValue parsed = parseChannelJson(*channel, value);
            if (!parsed.ok) {
                logWarn("mqtt.command", "Not a value for channel %1 of device %2: %3",
                        {Scalar{key}, Scalar{static_cast<std::int64_t>(cmd.id)}, Scalar{parsed.error}});
                continue;
            }
            invokeChannel(device->id, channel->id, parsed.value);
        }
    } catch (const Json::exception &e) {
        logWarn("mqtt.command", "A command on %1 was dropped: %2", {Scalar{message.topic}, Scalar{std::string(e.what())}});
    }
}

void MqttTransport::invokeChannel(int deviceId, int channelId, const Json &value)
{
    const Json payload{{"channelId", channelId}, {"value", value}};
    logInfo("mqtt.invoke", "MQTT sets channel %1 = %2", {Scalar{static_cast<std::int64_t>(channelId)}, Scalar{dumpJson(value)}});
    const AsyncResult r = callCoreAsync(kTopicChannelInvoke, dumpJson(payload), mqttIdentity());
    if (!r.accepted) {
        logWarn("mqtt.invoke", "cmd.channel.invoke for channel %1 refused: %2",
                {Scalar{static_cast<std::int64_t>(channelId)}, Scalar{r.error ? r.error->message : std::string("no reason")}});
        republishDevice(deviceId);
        return;
    }
    Pending pending{PendingKind::ChannelInvoke};
    pending.deviceId = deviceId;
    pending.channelId = channelId;
    m_pending[r.cmdId] = pending;
}

void MqttTransport::invokeScene(int sceneId)
{
    const AsyncResult r = callCoreAsync(kTopicSceneInvoke, dumpJson(Json{{"sceneId", sceneId}}), mqttIdentity());
    if (!r.accepted) {
        logWarn("mqtt.invoke", "cmd.scene.invoke for scene %1 refused: %2",
                {Scalar{static_cast<std::int64_t>(sceneId)}, Scalar{r.error ? r.error->message : std::string("no reason")}});
        return;
    }
    Pending pending{PendingKind::SceneInvoke};
    pending.sceneId = sceneId;
    m_pending[r.cmdId] = pending;
}

void MqttTransport::onInvokeResult(const Pending &pending, const Json &payload)
{
    const int status = payload.value("status", 0);
    if (status == 0) return;
    const std::string reason = [&]() {
        std::string text = textOf(payload, "statusName");
        const std::string detail = errorTextOf(payload);
        if (text.empty()) text = "failed";
        return detail.empty() ? text : text + " (" + detail + ")";
    }();
    if (pending.kind == PendingKind::SceneInvoke) {
        logWarn("mqtt.invoke", "Scene %1 was not set: %2", {Scalar{static_cast<std::int64_t>(pending.sceneId)}, Scalar{reason}});
        return;
    }
    logWarn("mqtt.invoke", "Channel %1 refused a command from MQTT: %2",
            {Scalar{static_cast<std::int64_t>(pending.channelId)}, Scalar{reason}});
    // The consumer sees the truth again: the value core names if it names
    // one, else the last one this transport knows.
    if (const auto rb = payload.find("rollbackValue"); rb != payload.end() && !rb->is_null())
        m_model.applyRollback(pending.channelId, *rb);
    republishDevice(pending.deviceId);
}

} // namespace phi::endpoint
