#include "shutdown_monitor.hpp"

#include <sdbusplus/bus/match.hpp>

#include <algorithm>
#include <array>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

namespace shutdown_monitor
{
namespace
{

constexpr auto systemdService = "org.freedesktop.systemd1";
constexpr auto systemdPath = "/org/freedesktop/systemd1";
constexpr auto systemdManager = "org.freedesktop.systemd1.Manager";

// A job queued for any of these units means the system is going down.
constexpr std::array<std::string_view, 5> shutdownUnits = {
    "reboot.target", "poweroff.target", "halt.target", "kexec.target",
    "shutdown.target"};

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
bool shutdownStarted = false;
std::unique_ptr<sdbusplus::bus::match_t> jobNewMatch;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

void markStarted(std::string_view cause)
{
    if (shutdownStarted)
    {
        return;
    }
    shutdownStarted = true;
    std::cerr << "shutdown started (" << cause
              << "), no longer rescanning or persisting configuration\n";
}

void onJobNew(sdbusplus::message_t& msg)
{
    uint32_t id = 0;
    sdbusplus::message::object_path job;
    std::string unit;
    msg.read(id, job, unit);

    if (std::ranges::find(shutdownUnits, unit) != shutdownUnits.end())
    {
        markStarted(unit);
    }
}

} // namespace

bool inProgress()
{
    return shutdownStarted;
}

void start(sdbusplus::asio::connection& bus)
{
    // systemd queues the shutdown job before it stops the first service, so
    // this arrives while every provider we probe is still on the bus. Waiting
    // for our own SIGTERM would be too late: services with no ordering between
    // them are stopped in parallel, so dbus-sensors can be gone before we are
    // signaled.
    jobNewMatch = std::make_unique<sdbusplus::bus::match_t>(
        static_cast<sdbusplus::bus_t&>(bus),
        sdbusplus::bus::match::rules::type::signal() +
            sdbusplus::bus::match::rules::sender(systemdService) +
            sdbusplus::bus::match::rules::path(systemdPath) +
            sdbusplus::bus::match::rules::interface(systemdManager) +
            sdbusplus::bus::match::rules::member("JobNew"),
        onJobNew);

    // systemd only emits JobNew while at least one client is subscribed.
    bus.async_method_call(
        [](const boost::system::error_code& ec) {
            // This fails if the connection is already subscribed, which is
            // harmless: the signals we want are delivered either way.
            if (ec)
            {
                std::cerr << "could not subscribe to systemd jobs: "
                          << ec.message() << "\n";
            }
        },
        systemdService, systemdPath, systemdManager, "Subscribe");
}

} // namespace shutdown_monitor
