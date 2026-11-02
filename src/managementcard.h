#pragma once

// What the transport's management card says and does with the settings form.
// Pure functions: the card's summary line from a snapshot of the state, the
// settings form with the running config as its values, and the merge of a
// submitted form into the runtime override that core owns. Nothing here
// touches the broker or core, so the text and the merge are tested alone.

#include "config.h"

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <string>

namespace phi::endpoint {

struct ManagementState {
    bool running = false;
    bool foreignBroker = false; // the config names a broker; no account is asked from the host
    bool hasLink = false;       // a broker account is there and the link was started
    bool connected = false;
    std::string host;
    int port = 0;
    std::string username;
    std::string lastError; // why the link is down, as the client said it
    std::size_t devices = 0;
    std::size_t topics = 0;
    bool homeAssistant = true;
    bool readOnly = false;
};

/// The one line under the card's title.
std::string managementSummary(const ManagementState &state);

/// The settings form: the fields the transport lets the household change, with `config` as the values.
/// The password is never prefilled; its description says what an empty one means.
nlohmann::json settingsForm(const Config &config);

/// Merges a submitted settings form into the runtime override.
///
/// `override` and `effective` are what core's `cmd.transport.config.get` answered. The new override is
/// `override` with the form's keys applied: keys the form does not own (`hosting`, `stateDir`, `enabled`,
/// `excludeAdapters`, anything unknown) stay as they were, `broker` is removed when the form switches the
/// foreign broker off, and an empty password keeps the stored one. The result is checked the way the
/// transport checks its config at start, against `effective` with the same changes, so a save that would
/// not start is refused here and not after the restart.
bool mergeSettings(const nlohmann::json &override, const nlohmann::json &effective, const nlohmann::json &form,
                   nlohmann::json *newOverride, std::string *error);

} // namespace phi::endpoint
