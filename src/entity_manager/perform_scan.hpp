#pragma once

#include "../utils.hpp"
#include "entity_manager.hpp"
#include "probe_lexer.hpp"

#include <systemd/sd-journal.h>

#include <nlohmann/json.hpp>
#include <sdbusplus/asio/object_server.hpp>

#include <flat_map>
#include <flat_set>
#include <functional>
#include <list>
#include <memory>
#include <optional>
#include <set>
#include <vector>

namespace probe
{
struct PerformProbe;
}

namespace scan
{
struct DBusDeviceDescriptor
{
    DBusInterface interface;
    std::string path;
};

using FoundDevices = std::vector<DBusDeviceDescriptor>;

struct PerformScan final : std::enable_shared_from_this<PerformScan>
{
    PerformScan(EntityManager& em, nlohmann::json& missingConfigurations,
                std::vector<nlohmann::json>& configurations,
                boost::asio::io_context& io, std::function<void()>&& callback);

    void updateSystemConfiguration(const nlohmann::json& recordRef,
                                   const std::string& probeName,
                                   FoundDevices& foundDevices);
    void run();
    ~PerformScan();
    EntityManager& _em;
    MapperGetSubTreeResponse dbusProbeObjects;
    std::vector<std::string> passedProbes;

  private:
    void updateAssemblyRecords(const nlohmann::json& compactRecord,
                               FoundDevices& foundDevices);
    void restorePersistedConfigurations(
        FoundDevices& foundDevices, const std::string& probeName,
        const std::string& probeType, std::set<nlohmann::json>& usedNames,
        std::list<size_t>& indexes);

    void updateSystemConfigurationForDevice(
        const nlohmann::json& recordRef, const std::string& probeName,
        const DBusDeviceDescriptor& device, std::set<nlohmann::json>& usedNames,
        std::list<size_t>& indexes, std::optional<std::string>& replaceStr);

    // Walk _configurations, dropping malformed or already-probed entries and
    // starting a PerformProbe for each remaining one. Collects the D-Bus
    // interfaces to look up into dbusProbeInterfaces / dbusProbePointers.
    // Returns false if a config had an unparsable Probe, in which case the
    // scan must not continue.
    bool processConfigurations(
        std::flat_set<std::string, std::less<>>& dbusProbeInterfaces,
        std::vector<std::shared_ptr<probe::PerformProbe>>& dbusProbePointers);

    nlohmann::json& _missingConfigurations;
    std::vector<nlohmann::json> _configurations;
    std::function<void()> _callback;
    bool _passed = false;
    // The first pass of a scan probes only the Platform records that are
    // pending, so that their properties are $Platform.<Property> variables
    // of every record probed in the later passes.
    bool _platformPass = true;

    boost::asio::io_context& io;
};

namespace detail
{
// Parse a config "Probe" field (an array of statements, or a single statement
// string) into a token stream. The statements are joined with single spaces
// and lexed. Returns an empty vector on error (a non-string statement or a
// lexing error); a valid probe is never empty.
std::vector<probe::Token> parseProbeCommand(const nlohmann::json& probeField);

// Collect the "Name" of every entry in systemConfiguration, i.e. the names of
// the already-applied configs.
std::vector<std::string> collectConfiguredNames(
    const nlohmann::json& systemConfiguration);

void pruneMissingByName(nlohmann::json& missingConfigurations,
                        const std::vector<std::string>& names);

// True if a pending (not yet applied) Platform record is in configurations:
// the scan then probes the Platform records first, in a pass of their own.
bool hasPendingPlatformRecord(const std::vector<nlohmann::json>& configurations,
                              const std::vector<std::string>& passedProbes);

// Whether a configuration is probed in this pass: the Platform pass probes
// only Platform records, the other passes probe every configuration.
bool probedInPass(const nlohmann::json& config, bool platformPass);

// Whether a scan runs another pass after one that matched something
// (passed) or was the Platform pass: nullopt when the scan is done, else
// whether the next pass is a Platform pass (never: only the first pass of a
// scan is).
std::optional<bool> nextPass(bool passed, bool platformPass);

// The template variables of a record matched on object: the properties of
// the object, plus the properties of the Platform record(s) already in
// systemConfiguration as $Platform.<Property>.
DBusObject withPlatformVariables(DBusObject object,
                                 const nlohmann::json& systemConfiguration);

// Substitute the template variables of every property of a record except
// Name, then handle the variables left over
// (em_utils::handleLeftOverRecordTemplateVars). The Exposes items get their
// left-over variables handled later, one by one.
void replaceTemplateFields(nlohmann::json::object_t& record,
                           const DBusObject& dbusObject, size_t foundDeviceIdx,
                           const std::optional<std::string>& replaceStr);
} // namespace detail

} // namespace scan
