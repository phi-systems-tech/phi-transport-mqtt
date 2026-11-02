#include "broker.h"

#include "payload.h"

#include <chrono>

namespace phi::endpoint {

namespace {
constexpr int kQos = 1;
constexpr std::chrono::seconds kRetry{10};
} // namespace

BrokerLink::BrokerLink(phi::runtime::Loop *loop, LogFn log) : m_loop(loop), m_log(std::move(log)) {}

BrokerLink::~BrokerLink()
{
    stop();
}

mqtt::Settings BrokerLink::settingsFor(const Account &account, const std::string &clientId)
{
    mqtt::Settings settings;
    settings.clientId = clientId;
    settings.host = account.host;
    settings.port = account.port;
    settings.username = account.username;
    settings.password = account.password;
    settings.tls.enabled = account.tls;
    settings.tls.caFile = account.caFile;
    return settings;
}

void BrokerLink::start(Account account, Options options)
{
    stop();
    m_account = std::move(account);
    m_options = std::move(options);
    m_running = true;
    m_client = std::make_unique<mqtt::Client>(*m_loop);
    m_client->onConnected([this]() { onConnectedInternal(); });
    m_client->onDisconnected([this](const std::string &reason) { onDisconnectedInternal(reason); });
    m_client->onMessage([this](const mqtt::Message &m) {
        if (m_onMessage) m_onMessage(m);
    });
    dial();
}

void BrokerLink::stop()
{
    m_running = false;
    m_retry.reset();
    if (m_client) {
        // The will only covers a connection that dies; a clean goodbye says it itself.
        if (m_client->connected()) m_client->publish(m_options.stateTopic, kOffline, kQos, true);
        m_client->disconnect();
        m_client.reset();
    }
}

void BrokerLink::dial()
{
    m_retry.reset();
    if (!m_running || !m_client) return;
    mqtt::Settings settings = settingsFor(m_account, m_options.clientId);
    settings.will = mqtt::Will{m_options.stateTopic, kOffline, kQos, true};
    std::string error;
    if (!m_client->connect(settings, &error)) {
        m_lastError = error;
        m_log(3, "broker: " + error);
        m_retry = m_loop->timerAfter(kRetry, [this]() { dial(); });
    }
}

void BrokerLink::onConnectedInternal()
{
    m_lastError.clear();
    std::string error;
    for (const std::string &filter : m_options.subscriptions)
        if (!m_client->subscribe(filter, kQos, &error)) m_log(3, "broker: subscribe to " + filter + " failed: " + error);
    m_client->publish(m_options.stateTopic, kOnline, kQos, true);
    m_log(2, "on the broker " + m_account.host + ":" + std::to_string(m_account.port)
                 + (m_account.username.empty() ? std::string() : " as " + m_account.username));
    if (m_onConnected) m_onConnected();
}

void BrokerLink::onDisconnectedInternal(const std::string &reason)
{
    m_lastError = reason;
    m_log(3, "broker connection lost: " + reason);
    if (m_onDisconnected) m_onDisconnected();
    if (!m_running) return;
    // A refusal right after a freshly minted account is often an acceptance ten seconds later.
    m_retry = m_loop->timerAfter(kRetry, [this]() { dial(); });
}

void BrokerLink::publish(const Message &message)
{
    if (!connected()) return;
    std::string error;
    if (!m_client->publish(message.topic, message.payload, kQos, message.retain, &error))
        m_log(3, "broker: publish to " + message.topic + " failed: " + error);
}

void BrokerLink::publish(const std::vector<Message> &messages)
{
    for (const Message &m : messages) publish(m);
}

void BrokerLink::publishEvent(const std::string &topic, const std::string &payload)
{
    publish(Message{topic, payload, false});
}

} // namespace phi::endpoint
