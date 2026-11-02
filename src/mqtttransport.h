#pragma once

#include "broker.h"
#include "config.h"
#include "discovery.h"
#include "model.h"
#include "retained.h"
#include "sweep.h"
#include "topics.h"

#include <transportinterface.h>

#include <chrono>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace phi::endpoint {

// phi as an MQTT endpoint. Pulls the house from core the way the Matter bridge
// does (lists at start, events after), keeps it in a Model, and says it on a
// broker: retained state per device and channel, events as they happen, and
// the commands that come back on `set` topics become cmd.channel.invoke.
// Runs on the loop core hands it; the broker link lives on the same loop.
class MqttTransport final : public phicore::transport::TransportPluginBase
{
public:
    using CmdId = phicore::transport::CmdId;

    MqttTransport();
    ~MqttTransport() override;

    std::string pluginType() const override;
    std::string displayName() const override;
    std::string description() const override;

    bool start(std::string_view configJson, std::string *errorString) override;
    void stop() override;

    /// How long after "online" on the Home Assistant status topic the discovery is said again: a random
    /// moment inside this window (Home Assistant's own advice, so a household of boxes does not answer in step).
    void setRepublishWindow(std::chrono::milliseconds window) { m_republishWindow = window; }

protected:
    void onCoreAsyncResult(CmdId cmdId, std::string_view payloadJson) override;
    void onCoreEvent(std::string_view topic, std::string_view payloadJson) override;

private:
    enum class PendingKind { Adapters, Rooms, Scenes, ScenesRefresh, Devices, Credentials, ChannelInvoke, SceneInvoke };
    struct Pending {
        PendingKind kind;
        int deviceId = 0;
        int channelId = 0;
        int sceneId = 0;
    };

    // bootstrap
    void bootstrap();
    void startSync();
    void request(PendingKind kind);
    void retryBootstrap();
    void onSynced();
    void replayEarlyEvents();
    void requestBrokerAccount();
    void onBrokerCredentials(const nlohmann::json &payload);
    void startLink(BrokerLink::Account account);

    // the house
    void handleEvent(std::string_view topic, const nlohmann::json &payload);
    void refresh(const Touched &touched, bool force = false);
    void refreshBridge();
    void scheduleBridge();
    void send(const std::vector<Message> &messages);
    void refreshDiscovery(const Touched &touched);
    void setDiscovery(const std::string &group, TopicMap desired);
    void onHomeAssistantStatus(const phi::runtime::mqtt::Message &message);
    void startSweep();
    void onSweepResult(const std::vector<std::string> &retainedTopics);
    void brokerLog(int level, const std::string &text) const;
    void republishDevice(int deviceId);

    // the broker
    void onBrokerMessage(const phi::runtime::mqtt::Message &message);
    void invokeChannel(int deviceId, int channelId, const nlohmann::json &value);
    void invokeScene(int sceneId);
    void onInvokeResult(const Pending &pending, const nlohmann::json &payload);

    void logInfo(std::string_view ctx, std::string_view message, phicore::transport::ScalarList params = {}) const;
    void logWarn(std::string_view ctx, std::string_view message, phicore::transport::ScalarList params = {}) const;

    Config m_config;
    Topics m_topics{"phi"};
    Model m_model;
    Retained m_retained;
    DiscoveryContext m_discovery;
    BrokerLink::Account m_account;
    std::unique_ptr<BrokerLink> m_link;
    std::unique_ptr<Sweep> m_sweep;
    bool m_swept = false;
    std::chrono::milliseconds m_republishWindow{5000};
    std::mt19937 m_random{std::random_device{}()};
    phi::runtime::Timer m_discoveryRepublish;
    bool m_running = false;
    bool m_synced = false;
    bool m_haveHello = false;
    std::string m_tenant, m_instance;
    Touched m_syncTouched;
    phi::runtime::Timer m_bootstrap;
    phi::runtime::Timer m_credentialsRetry;
    phi::runtime::Timer m_bridgeTimer;
    phi::runtime::Timer m_sceneRefresh;
    std::vector<std::pair<std::string, nlohmann::json>> m_earlyEvents;
    bool m_earlyOverflow = false;
    std::unordered_map<CmdId, Pending> m_pending;
};

} // namespace phi::endpoint
