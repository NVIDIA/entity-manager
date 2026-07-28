#pragma once

#include <sdbusplus/asio/connection.hpp>

namespace shutdown_monitor
{

// True once systemd has begun an orderly reboot or power off.
//
// An orderly shutdown stops the providers entity-manager probes one by one. A
// rescan triggered by that teardown sees them as absent and would persist a
// configuration with those entities pruned, discarding the runtime values that
// only exist in system.json (leak detection policy, thresholds, ...). Nothing a
// shutdown removes is worth persisting, as it all comes back on the next boot.
bool inProgress();

// Starts watching for shutdown. Call once before io.run().
void start(sdbusplus::asio::connection& bus);

} // namespace shutdown_monitor
