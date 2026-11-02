#include "sweep.h"

#include <algorithm>

namespace phi::endpoint {

namespace {
constexpr int kQos = 1;
}

Sweep::Sweep(phi::runtime::Loop *loop, BrokerLink::LogFn log) : m_loop(loop), m_log(std::move(log)) {}

Sweep::~Sweep() = default;

void Sweep::start(const BrokerLink::Account &account, std::vector<std::string> filters, std::chrono::milliseconds window,
                  Done done)
{
    m_window.reset();
    m_topics.clear();
    m_done = std::move(done);
    m_client = std::make_unique<mqtt::Client>(*m_loop);
    m_client->onMessage([this](const mqtt::Message &m) {
        // Only what the broker replays from its store: a live publish is somebody speaking now.
        if (m.retained && !m.payload.empty() && std::find(m_topics.begin(), m_topics.end(), m.topic) == m_topics.end())
            m_topics.push_back(m.topic);
    });
    m_client->onConnected([this, filters = std::move(filters), window]() {
        std::string error;
        for (const std::string &filter : filters)
            if (!m_client->subscribe(filter, kQos, &error)) m_log(3, "sweep: subscribe to " + filter + " failed: " + error);
        m_window = m_loop->timerAfter(window, [this]() { finish(); });
    });
    m_client->onDisconnected([this](const std::string &reason) {
        // Only reached when the line breaks before the window ends: finish() disconnects quietly.
        m_window.reset();
        m_log(3, "sweep: the broker line broke: " + reason);
    });
    std::string error;
    if (!m_client->connect(BrokerLink::settingsFor(account, "phi-transport-mqtt-sweep"), &error)) {
        m_log(3, "sweep: " + error);
        m_client.reset();
    }
}

void Sweep::finish()
{
    m_window.reset();
    m_client->disconnect();
    Done done = std::move(m_done);
    if (done) done(m_topics);
}

} // namespace phi::endpoint
