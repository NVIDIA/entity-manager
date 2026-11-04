#pragma once

#include "../utils.hpp"

#include <boost/asio/io_context.hpp>
#include <nlohmann/json.hpp>
#include <sdbusplus/asio/connection.hpp>

constexpr const char* configurationOutDir = "/var/configuration/";
constexpr const char* emDbusName = "xyz.openbmc_project.EntityManager";
constexpr const char* emDbusPath = "/xyz/openbmc_project/EntityManager";
constexpr const char* versionHashFile = "/var/configuration/version";
constexpr const char* versionFile = "/etc/os-release";

namespace em_utils
{

namespace properties
{
constexpr const char* interface = "org.freedesktop.DBus.Properties";
constexpr const char* get = "Get";
} // namespace properties

bool fwVersionIsSame();

void handleLeftOverTemplateVars(nlohmann::json& value);
void handleLeftOverTemplateVars(nlohmann::json::object_t& value);
void handleLeftOverTemplateVars(nlohmann::json::array_t& value);
void handleLeftOverTemplateVars(std::string& value);

// Left-over variables of a record after its properties are substituted
// (Name, Probe and Exposes are not touched). A string property of one of its
// D-Bus interfaces, and a string at the record level, keeps the old behavior
// (the variable is removed from the string); map entries and array elements
// are skipped as handleLeftOverTemplateVars does. A map, array or interface
// that is left empty by skipping is removed from the record.
void handleLeftOverRecordTemplateVars(nlohmann::json::object_t& record);

std::optional<std::string> templateCharReplace(
    nlohmann::json& value, const DBusObject& object, size_t index,
    const std::optional<std::string>& replaceStr = std::nullopt,
    bool handleLeftOver = true);

std::optional<std::string> templateCharReplace(
    nlohmann::json& value, const DBusInterface& interface, size_t index,
    const std::optional<std::string>& replaceStr = std::nullopt);

// The scalar properties of the Platform record(s) in systemConfiguration, as
// the variables Platform.<Property> that are available to the templates of
// every record (for example $Platform.NamePrefix).
DBusInterface platformTemplateVariables(
    const nlohmann::json& systemConfiguration);

sdbusplus::object_path buildInventorySystemPath(std::string& boardName,
                                                const std::string& boardType);

std::string getExposesKey(const nlohmann::json& config);

} // namespace em_utils
