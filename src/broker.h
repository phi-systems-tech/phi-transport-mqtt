#pragma once

// The transport's connection to the broker: login, will, subscriptions and
// the retry that keeps it coming back. The topic tree and what is said on it
// are the caller's; this only keeps the line open and says "online" /
// "offline" on the bridge's state topic.

#include "retained.h"

#include <phi/runtime/loop.h>
#include <phi/runtime/mqtt/client.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace phi::endpoint {

namespace mqtt = phi::runtime::mqtt;

class BrokerLink
{
public:
    struct Account {
        std::string host = "127.0.0.1";
        int port = 1883;
        std::string username, password;
        bool tls = false;
        std::string caFile;
    };
    struct Options {
        std::string clientId = "phi-transport-mqtt";
        std::string stateTopic;                  // retained online | offline, and the will
        std::vector<std::string> subscriptions;  // QoS 1
    };
    using LogFn = std::function<void(int level, const std::string &text)>; // 1 debug, 2 info, 3 warn

    BrokerLink(phi::runtime::Loop *loop, LogFn log);
    ~BrokerLink();

    void start(Account account, Options options);
    /// "offline" on the state topic, then a quiet disconnect.
    void stop();

    /// After every connect, once "online" has been said: what the broker may have lost is the caller's to say again.
    void onConnected(std::function<void()> fn) { m_onConnected = std::move(fn); }
    void onDisconnected(std::function<void()> fn) { m_onDisconnected = std::move(fn); }
    void onMessage(std::function<void(const mqtt::Message &)> fn) { m_onMessage = std::move(fn); }

    bool connected() const { return m_client && m_client->connected(); }
    bool running() const { return m_running; }

    /// QoS 1. Silently dropped while the broker is away (the ledger replays on connect).
    void publish(const Message &message);
    void publish(const std::vector<Message> &messages);
    /// A non-retained message (an event).
    void publishEvent(const std::string &topic, const std::string &payload);

private:
    void dial();
    void onConnectedInternal();
    void onDisconnectedInternal(const std::string &reason);

    phi::runtime::Loop *m_loop;
    LogFn m_log;
    Account m_account;
    Options m_options;
    std::function<void()> m_onConnected, m_onDisconnected;
    std::function<void(const mqtt::Message &)> m_onMessage;
    std::unique_ptr<mqtt::Client> m_client;
    phi::runtime::Timer m_retry;
    bool m_running = false;
};

} // namespace phi::endpoint
