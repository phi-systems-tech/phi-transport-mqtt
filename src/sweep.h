#pragma once

// One look at what the broker still holds from earlier runs. A retained topic
// the ledger does not own - a device that is gone, an adapter that is now
// excluded, a discovery config of a switched-off Home Assistant mode - would
// stay on the broker for ever, and a consumer would go on seeing it.
//
// The look is a connection of its own: subscribe to the wide filters, collect
// whatever arrives with the retained flag for a short window, hand the topics
// over, disconnect. Not a second subscription on the transport's link, because
// the client cannot unsubscribe and an overlapping filter would deliver a
// command twice.

#include "broker.h"

#include <phi/runtime/loop.h>
#include <phi/runtime/mqtt/client.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace phi::endpoint {

class Sweep
{
public:
    /// Every topic that arrived retained during the window, once each.
    using Done = std::function<void(const std::vector<std::string> &retainedTopics)>;

    Sweep(phi::runtime::Loop *loop, BrokerLink::LogFn log);
    ~Sweep();

    /// `Done` is not called when the broker cannot be reached; the next start of the transport looks again.
    void start(const BrokerLink::Account &account, std::vector<std::string> filters, std::chrono::milliseconds window,
               Done done);

private:
    void finish();

    phi::runtime::Loop *m_loop;
    BrokerLink::LogFn m_log;
    std::unique_ptr<mqtt::Client> m_client;
    phi::runtime::Timer m_window;
    std::vector<std::string> m_topics;
    Done m_done;
};

} // namespace phi::endpoint
