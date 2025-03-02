/*
// Copyright (c) 2018 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
*/
/// \file entity_manager.cpp

#include "entity_manager.hpp"

#include "overlay.hpp"
#include "topology.hpp"
#include "utils.hpp"
#include "variant_visitors.hpp"

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/container/flat_map.hpp>
#include <boost/container/flat_set.hpp>
#include <boost/range/iterator_range.hpp>
#include <nlohmann/json.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>

#include <charconv>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <regex>
#include <variant>
constexpr const char* hostConfigurationDirectory = SYSCONF_DIR "configurations";
constexpr const char* configurationDirectory = PACKAGE_DIR "configurations";
constexpr const char* schemaDirectory = PACKAGE_DIR "configurations/schemas";
constexpr const char* tempConfigDir = "/tmp/configuration/";
constexpr const char* lastConfiguration = "/tmp/configuration/last.json";
constexpr const char* currentConfiguration = "/var/configuration/system.json";
constexpr const char* globalSchema = "global.json";
constexpr auto probePath = "ProbePath";

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
bool dataUpdated = false;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

constexpr auto fruIface = "xyz.openbmc_project.FruDevice";
constexpr auto fruService = "xyz.openbmc_project.FruDevice";
constexpr auto fwdPath = "fruDevice";
constexpr auto revPath = "allFru";
const boost::container::flat_map<const char*, probe_type_codes, CmpStr>
    probeTypes{{{"FALSE", probe_type_codes::FALSE_T},
                {"TRUE", probe_type_codes::TRUE_T},
                {"AND", probe_type_codes::AND},
                {"OR", probe_type_codes::OR},
                {"FOUND", probe_type_codes::FOUND},
                {"MATCH_ONE", probe_type_codes::MATCH_ONE}}};

static constexpr std::array<const char*, 10> settableInterfaces = {
    "FanProfile",
    "Pid",
    "Pid.Zone",
    "Stepwise",
    "Thresholds",
    "Polling",
    "VoltageLeakDetector",
    "xyz.openbmc_project.Inventory.Decorator.AssetTag",
    "xyz.openbmc_project.Inventory.Decorator.Asset",
    "xyz.openbmc_project.Common.UUID"};

using JsonVariantType =
    std::variant<std::vector<std::string>, std::vector<double>, std::string,
                 int64_t, uint64_t, double, int32_t, uint32_t, int16_t,
                 uint16_t, uint8_t, bool>;

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
// store reference to all interfaces so we can destroy them later
boost::container::flat_map<
    std::string, std::vector<std::weak_ptr<sdbusplus::asio::dbus_interface>>>
    inventory;

using Interface = std::string;
using UpdatableProperties = std::unordered_map<std::string, std::string>;
using InterfaceProperties =
    std::unordered_map<std::string, UpdatableProperties>;
// store name -> writable interfaces -> mapped propeties
std::unordered_map<Interface, InterfaceProperties> probeDetails;

// store record name to name
std::unordered_map<std::string, std::string> nameToRecordName;

// todo: pass this through nicer
std::shared_ptr<sdbusplus::asio::connection> systemBus;
nlohmann::json lastJson;
Topology topology;

boost::asio::io_context io;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

const std::regex illegalDbusPathRegex("[^A-Za-z0-9_.]");
const std::regex illegalDbusMemberRegex("[^A-Za-z0-9_]");

bool loadConfigurations(std::list<nlohmann::json>& configurations);
void tryIfaceInitialize(std::shared_ptr<sdbusplus::asio::dbus_interface>& iface)
{
    try
    {
        iface->initialize();
    }
    catch (std::exception& e)
    {
        std::cerr << "Unable to initialize dbus interface : " << e.what()
                  << "\n"
                  << "object Path : " << iface->get_object_path() << "\n"
                  << "interface name : " << iface->get_interface_name() << "\n";
    }
}

FoundProbeTypeT findProbeType(const std::string& probe)
{
    boost::container::flat_map<const char*, probe_type_codes,
                               CmpStr>::const_iterator probeType;
    for (probeType = probeTypes.begin(); probeType != probeTypes.end();
         ++probeType)
    {
        if (probe.find(probeType->first) != std::string::npos)
        {
            return probeType;
        }
    }

    return std::nullopt;
}

static std::shared_ptr<sdbusplus::asio::dbus_interface>
    createInterface(sdbusplus::asio::object_server& objServer,
                    const std::string& path, const std::string& interface,
                    const std::string& parent, bool checkNull = false)
{
    // on first add we have no reason to check for null before add, as there
    // won't be any. For dynamically added interfaces, we check for null so that
    // a constant delete/add will not create a memory leak

    auto ptr = objServer.add_interface(path, interface);
    auto& dataVector = inventory[parent];
    if (checkNull)
    {
        auto it = std::find_if(dataVector.begin(), dataVector.end(),
                               [](const auto& p) { return p.expired(); });
        if (it != dataVector.end())
        {
            *it = ptr;
            return ptr;
        }
    }
    dataVector.emplace_back(ptr);
    return ptr;
}

// writes output files to persist data
bool writeJsonFiles(const nlohmann::json& systemConfiguration)
{
    std::filesystem::create_directory(configurationOutDir);
    std::ofstream output(currentConfiguration);
    if (!output.good())
    {
        return false;
    }
    output << systemConfiguration.dump(4);
    output.close();
    return true;
}

template <typename JsonType>
bool setJsonFromPointer(const std::string& ptrStr, const JsonType& value,
                        nlohmann::json& systemConfiguration)
{
    try
    {
        nlohmann::json::json_pointer ptr(ptrStr);
        nlohmann::json& ref = systemConfiguration[ptr];
        ref = value;
        return true;
    }
    catch (const std::out_of_range&)
    {
        return false;
    }
}

// template function to add array as dbus property
template <typename PropertyType>
void addArrayToDbus(const std::string& name, const nlohmann::json& array,
                    sdbusplus::asio::dbus_interface* iface,
                    sdbusplus::asio::PropertyPermission permission,
                    nlohmann::json& systemConfiguration,
                    const std::string& jsonPointerString)
{
    std::vector<PropertyType> values;
    for (const auto& property : array)
    {
        auto ptr = property.get_ptr<const PropertyType*>();
        if (ptr != nullptr)
        {
            values.emplace_back(*ptr);
        }
    }

    if (permission == sdbusplus::asio::PropertyPermission::readOnly)
    {
        iface->register_property(name, values);
    }
    else
    {
        iface->register_property(
            name, values,
            [&systemConfiguration,
             jsonPointerString{std::string(jsonPointerString)}](
                const std::vector<PropertyType>& newVal,
                std::vector<PropertyType>& val) {
            val = newVal;
            if (!setJsonFromPointer(jsonPointerString, val,
                                    systemConfiguration))
            {
                std::cerr << "error setting json field\n";
                return -1;
            }
            if (!writeJsonFiles(systemConfiguration))
            {
                std::cerr << "error setting json file\n";
                return -1;
            }
            return 1;
        });
    }
}

template <typename Property>
bool updatePropertyValue(const std::string& service, const std::string& path,
                         const std::string& interface,
                         const std::string& property, Property& propertyValue)
{
    systemBus->async_method_call(
        [property](const boost::system::error_code& ec) {
        if (ec)
        {
            std::cerr << "Error in setting property " << property << "\n";
            return false;
        }
        return true;
    },
        service, path, "org.freedesktop.DBus.Properties", "Set", interface,
        property,
        std::variant<std::decay_t<decltype(propertyValue)>>(propertyValue));

    return true;
}

sdbusplus::asio::PropertyPermission getPermission(const std::string& interface)
{
    return std::find(settableInterfaces.begin(), settableInterfaces.end(),
                     interface) != settableInterfaces.end()
               ? sdbusplus::asio::PropertyPermission::readWrite
               : sdbusplus::asio::PropertyPermission::readOnly;
}

bool isPropertyUpdatable(const std::string& propertyName,
                         const std::string& jsonPointerString,
                         std::string& mappedProp)
{
    std::filesystem::path path(jsonPointerString);
    std::string parent = (path.relative_path().parent_path()).parent_path();
    std::string interface = path.parent_path().filename();

    if (auto itr{nameToRecordName.find(parent)}; itr != nameToRecordName.end())
    {
        parent = itr->second;
    }
    else
    {
        std::cerr << "Error No Record found  " << parent << " Interface "
                  << interface << "\n";
        return false;
    }

    if (const auto& itr{probeDetails.find(parent)}; itr != probeDetails.end())
    {
        const auto& listIfaces{itr->second};
        if (const auto& it{listIfaces.find(interface)}; it != listIfaces.end())
        {
            const auto& props{it->second};
            if (const auto& itp{props.find(propertyName)}; itp != props.end())
            {
                mappedProp = itp->second;
                return true;
            }
        }
    }
    return false;
}

template <typename PropertyType>
bool persistProperty(const PropertyType& newVal, const std::string& path,
                     const std::string& fruProperty)
{
    std::string objectPath = path + "/" + fwdPath;
    std::vector<std::string> endPoints;
    systemBus->async_method_call(
        [objectPath, fruProperty,
         newVal](const boost::system::error_code& ec,
                 std::variant<std::decay_t<decltype(endPoints)>>(
                     endPoints)) mutable {
        if (ec)
        {
            std::cerr << "No Associated paths found for " << objectPath << "\n";
            std::cerr << "Error Msg " << ec.message() << "\n";
            return false;
        }
        std::vector<std::string> data =
            std::get<std::vector<std::string>>(endPoints);
        for (const auto& endPoint : data)
        {
            if (!updatePropertyValue(fruService, endPoint, fruIface,
                                     fruProperty, newVal))
            {
                std::cerr << "Error setting property " << fruProperty
                          << " in interface " << fruIface << "\n";
                return false;
            }
        }
        return true;
    },
        "xyz.openbmc_project.ObjectMapper", objectPath,
        "org.freedesktop.DBus.Properties", "Get",
        "xyz.openbmc_project.Association", "endpoints");

    return true;
}

template <typename PropertyType>
void addProperty(const std::string& name, const PropertyType& value,
                 sdbusplus::asio::dbus_interface* iface,
                 nlohmann::json& systemConfiguration,
                 const std::string& jsonPointerString,
                 sdbusplus::asio::PropertyPermission permission)
{
    if (permission == sdbusplus::asio::PropertyPermission::readOnly)
    {
        iface->register_property(name, value);
        return;
    }
    iface->register_property(
        name, value,
        [name, &systemConfiguration,
         jsonPointerString{std::string(jsonPointerString)},
         iface](const PropertyType& newVal, PropertyType& val) {
        std::string mappedProp;
        if (isPropertyUpdatable(name, jsonPointerString, mappedProp))
        {
            return persistProperty(newVal, iface->get_object_path(), mappedProp)
                       ? 1
                       : -1;
        }
        val = newVal;
        if (!setJsonFromPointer(jsonPointerString, val, systemConfiguration))
        {
            std::cerr << "error setting json field\n";
            return -1;
        }
        if (!writeJsonFiles(systemConfiguration))
        {
            std::cerr << "error setting json file\n";
            return -1;
        }
        return 1;
    });
}

void createDeleteObjectMethod(
    const std::string& jsonPointerPath,
    const std::shared_ptr<sdbusplus::asio::dbus_interface>& iface,
    sdbusplus::asio::object_server& objServer,
    nlohmann::json& systemConfiguration)
{
    std::weak_ptr<sdbusplus::asio::dbus_interface> interface = iface;
    iface->register_method("Delete",
                           [&objServer, &systemConfiguration, interface,
                            jsonPointerPath{std::string(jsonPointerPath)}]() {
        std::shared_ptr<sdbusplus::asio::dbus_interface> dbusInterface =
            interface.lock();
        if (!dbusInterface)
        {
            // this technically can't happen as the pointer is pointing to
            // us
            throw DBusInternalError();
        }
        nlohmann::json::json_pointer ptr(jsonPointerPath);
        systemConfiguration[ptr] = nullptr;

        // todo(james): dig through sdbusplus to find out why we can't
        // delete it in a method call
        boost::asio::post(io, [&objServer, dbusInterface]() mutable {
            objServer.remove_interface(dbusInterface);
        });

        if (!writeJsonFiles(systemConfiguration))
        {
            std::cerr << "error setting json file\n";
            throw DBusInternalError();
        }
    });
}

// adds simple json types to interface's properties
void populateInterfaceFromJson(
    nlohmann::json& systemConfiguration, const std::string& jsonPointerPath,
    std::shared_ptr<sdbusplus::asio::dbus_interface>& iface,
    nlohmann::json& dict, sdbusplus::asio::object_server& objServer,
    sdbusplus::asio::PropertyPermission permission =
        sdbusplus::asio::PropertyPermission::readOnly)
{
    for (const auto& [key, value] : dict.items())
    {
        auto type = value.type();
        bool array = false;

        if (key == "Parent_Chassis" ||
            key == "xyz.openbmc_project.Association.Definitions")
        {
            continue;
        }

        if (value.type() == nlohmann::json::value_t::array)
        {
            array = true;
            if (value.empty())
            {
                continue;
            }
            type = value[0].type();
            bool isLegal = true;
            for (const auto& arrayItem : value)
            {
                if (arrayItem.type() != type)
                {
                    isLegal = false;
                    break;
                }
            }
            if (!isLegal)
            {
                std::cerr << "dbus format error" << value << "\n";
                continue;
            }
        }
        if (type == nlohmann::json::value_t::object)
        {
            continue; // handled elsewhere
        }

        std::string path = jsonPointerPath;
        path.append("/").append(key);
        if (permission == sdbusplus::asio::PropertyPermission::readWrite)
        {
            // all setable numbers are doubles as it is difficult to always
            // create a configuration file with all whole numbers as decimals
            // i.e. 1.0
            if (array)
            {
                if (value[0].is_number())
                {
                    type = nlohmann::json::value_t::number_float;
                }
            }
            else if (value.is_number())
            {
                type = nlohmann::json::value_t::number_float;
            }
        }

        switch (type)
        {
            case (nlohmann::json::value_t::boolean):
            {
                if (array)
                {
                    // todo: array of bool isn't detected correctly by
                    // sdbusplus, change it to numbers
                    addArrayToDbus<uint64_t>(key, value, iface.get(),
                                             permission, systemConfiguration,
                                             path);
                }

                else
                {
                    addProperty(key, value.get<bool>(), iface.get(),
                                systemConfiguration, path, permission);
                }
                break;
            }
            case (nlohmann::json::value_t::number_integer):
            {
                if (array)
                {
                    addArrayToDbus<int64_t>(key, value, iface.get(), permission,
                                            systemConfiguration, path);
                }
                else
                {
                    addProperty(key, value.get<int64_t>(), iface.get(),
                                systemConfiguration, path,
                                sdbusplus::asio::PropertyPermission::readOnly);
                }
                break;
            }
            case (nlohmann::json::value_t::number_unsigned):
            {
                if (array)
                {
                    addArrayToDbus<uint64_t>(key, value, iface.get(),
                                             permission, systemConfiguration,
                                             path);
                }
                else
                {
                    addProperty(key, value.get<uint64_t>(), iface.get(),
                                systemConfiguration, path,
                                sdbusplus::asio::PropertyPermission::readOnly);
                }
                break;
            }
            case (nlohmann::json::value_t::number_float):
            {
                if (array)
                {
                    addArrayToDbus<double>(key, value, iface.get(), permission,
                                           systemConfiguration, path);
                }

                else
                {
                    addProperty(key, value.get<double>(), iface.get(),
                                systemConfiguration, path, permission);
                }
                break;
            }
            case (nlohmann::json::value_t::string):
            {
                if (array)
                {
                    addArrayToDbus<std::string>(key, value, iface.get(),
                                                permission, systemConfiguration,
                                                path);
                }
                else
                {
                    addProperty(key, value.get<std::string>(), iface.get(),
                                systemConfiguration, path, permission);
                }
                break;
            }
            default:
            {
                std::cerr << "Unexpected json type in system configuration "
                          << key << ": " << value.type_name() << "\n";
                break;
            }
        }
    }
    if (permission == sdbusplus::asio::PropertyPermission::readWrite)
    {
        createDeleteObjectMethod(jsonPointerPath, iface, objServer,
                                 systemConfiguration);
    }
    tryIfaceInitialize(iface);
}

void createAddObjectMethod(const std::string& jsonPointerPath,
                           const std::string& path,
                           nlohmann::json& systemConfiguration,
                           sdbusplus::asio::object_server& objServer,
                           const std::string& board)
{
    std::shared_ptr<sdbusplus::asio::dbus_interface> iface = createInterface(
        objServer, path, "xyz.openbmc_project.AddObject", board);

    iface->register_method(
        "AddObject",
        [&systemConfiguration, &objServer,
         jsonPointerPath{std::string(jsonPointerPath)}, path{std::string(path)},
         board](const boost::container::flat_map<std::string, JsonVariantType>&
                    data) {
        nlohmann::json::json_pointer ptr(jsonPointerPath);
        nlohmann::json& base = systemConfiguration[ptr];
        auto findExposes = base.find("Exposes");

        if (findExposes == base.end())
        {
            throw std::invalid_argument("Entity must have children.");
        }

        // this will throw invalid-argument to sdbusplus if invalid json
        nlohmann::json newData{};
        for (const auto& item : data)
        {
            nlohmann::json& newJson = newData[item.first];
            std::visit(
                [&newJson](auto&& val) {
                newJson = std::forward<decltype(val)>(val);
            },
                item.second);
        }

        auto findName = newData.find("Name");
        auto findType = newData.find("Type");
        if (findName == newData.end() || findType == newData.end())
        {
            throw std::invalid_argument("AddObject missing Name or Type");
        }
        const std::string* type = findType->get_ptr<const std::string*>();
        const std::string* name = findName->get_ptr<const std::string*>();
        if (type == nullptr || name == nullptr)
        {
            throw std::invalid_argument("Type and Name must be a string.");
        }

        bool foundNull = false;
        size_t lastIndex = 0;
        // we add in the "exposes"
        for (const auto& expose : *findExposes)
        {
            if (expose.is_null())
            {
                foundNull = true;
                continue;
            }

            if (expose["Name"] == *name && expose["Type"] == *type)
            {
                throw std::invalid_argument(
                    "Field already in JSON, not adding");
            }

            if (foundNull)
            {
                continue;
            }

            lastIndex++;
        }

        std::ifstream schemaFile(std::string(schemaDirectory) + "/" +
                                 boost::to_lower_copy(*type) + ".json");
        // todo(james) we might want to also make a list of 'can add'
        // interfaces but for now I think the assumption if there is a
        // schema avaliable that it is allowed to update is fine
        if (!schemaFile.good())
        {
            throw std::invalid_argument(
                "No schema avaliable, cannot validate.");
        }
        nlohmann::json schema = nlohmann::json::parse(schemaFile, nullptr,
                                                      false, true);
        if (schema.is_discarded())
        {
            std::cerr << "Schema not legal" << *type << ".json\n";
            throw DBusInternalError();
        }
        if (!validateJson(schema, newData))
        {
            throw std::invalid_argument("Data does not match schema");
        }
        if (foundNull)
        {
            findExposes->at(lastIndex) = newData;
        }
        else
        {
            findExposes->push_back(newData);
        }
        if (!writeJsonFiles(systemConfiguration))
        {
            std::cerr << "Error writing json files\n";
            throw DBusInternalError();
        }
        std::string dbusName = *name;

        std::regex_replace(dbusName.begin(), dbusName.begin(), dbusName.end(),
                           illegalDbusMemberRegex, "_");

        std::shared_ptr<sdbusplus::asio::dbus_interface> interface =
            createInterface(objServer, path + "/" + dbusName,
                            "xyz.openbmc_project.Configuration." + *type, board,
                            true);
        // permission is read-write, as since we just created it, must be
        // runtime modifiable
        populateInterfaceFromJson(
            systemConfiguration,
            jsonPointerPath + "/Exposes/" + std::to_string(lastIndex),
            interface, newData, objServer,
            sdbusplus::asio::PropertyPermission::readWrite);
    });
    tryIfaceInitialize(iface);
}
void getPropertyMapping(
    nlohmann::json::iterator& keyPair,
    std::unordered_map<std::string, std::string>& updatableProperties)
{
    if (keyPair.value().type() == nlohmann::json::value_t::object)
    {
        for (auto nextLayer = keyPair.value().begin();
             nextLayer != keyPair.value().end(); nextLayer++)
        {
            getPropertyMapping(nextLayer, updatableProperties);
        }
    }
    if (keyPair.value().type() == nlohmann::json::value_t::string)
    {
        std::string val = keyPair.value();
        size_t indexIdx = val.find('$');

        if (indexIdx != std::string::npos)
        {
            updatableProperties.emplace(keyPair.key(),
                                        val.substr(indexIdx + 1));
        }
    }
}

// Save the updatable interfaces with mapped properties
void scanUpdatableData()
{
    std::list<nlohmann::json> configurations;
    if (!loadConfigurations(configurations))
    {
        std::cerr << "cannot find json files\n";
        return;
    }
    for (auto& it : configurations)
    {
        nlohmann::json record = it;

        auto findName = record.find("Name");
        auto findProbe = record.find("Probe");

        if (findName == record.end() || findProbe == record.end())
        {
            std::cerr << "No Probe/Name found \n";
            return;
        }
        std::string probeName = *findName;

        // Template Name are not handled yet
        size_t indexIdx = probeName.find('$');
        if (indexIdx != std::string::npos)
        {
            continue;
        }

        std::unordered_map<std::string,
                           std::unordered_map<std::string, std::string>>
            ifaceProperty;
        for (auto keyPair = record.begin(); keyPair != record.end(); keyPair++)
        {
            if (keyPair.value().type() == nlohmann::json::value_t::object)
            {
                UpdatableProperties updatableProperties;
                getPropertyMapping(keyPair, updatableProperties);

                if (!updatableProperties.empty())
                {
                    ifaceProperty.emplace(keyPair.key(), updatableProperties);
                }
            }
        }
        if (!ifaceProperty.empty())
        {
            std::cerr << "Adding to Probe Details  " << probeName << "\n";
            probeDetails.emplace(probeName, ifaceProperty);
        }

        dataUpdated = true;
    }
}

using Association = std::tuple<std::string, std::string, std::string>;

void postToDbus(const nlohmann::json& newConfiguration,
                nlohmann::json& systemConfiguration,
                sdbusplus::asio::object_server& objServer)

{
    // Writable interfaces and mapped property are scanned only once
    if (!dataUpdated)
    {
        scanUpdatableData();
    }

    // these details are used to get mapped property or to get updatable
    // interface
    for (const auto& boardPair : newConfiguration.items())
    {
        std::string boardKey = boardPair.value()["Name"];

        for (auto& record : nameToRecordName)
        {
            if (record.second == boardKey)
            {
                nameToRecordName.erase(record.first);
                break;
            }
        }
        nameToRecordName.emplace(boardPair.key(), boardKey);
    }
    Topology topology;
    std::map<std::string, std::string> newBoards; // path -> name

    // iterate through boards
    for (const auto& [boardId, boardConfig] : newConfiguration.items())
    {
        std::string boardName = boardConfig["Name"];
        std::string boardNameOrig = boardConfig["Name"];
        std::string jsonPointerPath = "/" + boardId;
        // loop through newConfiguration, but use values from system
        // configuration to be able to modify via dbus later
        auto boardValues = systemConfiguration[boardId];
        auto findBoardType = boardValues.find("Type");
        auto findBoardParent = boardValues.find("Parent_Chassis");
        auto findCustomNameEnabled = boardValues.find("Custom_Name");
        std::string boardType;
        if (findBoardType != boardValues.end() &&
            findBoardType->type() == nlohmann::json::value_t::string)
        {
            boardType = findBoardType->get<std::string>();
            std::regex_replace(boardType.begin(), boardType.begin(),
                               boardType.end(), illegalDbusMemberRegex, "_");
        }
        else
        {
            std::cerr << "Unable to find type for " << boardName
                      << " reverting to Chassis.\n";
            boardType = "Chassis";
        }
        std::string boardtypeLower = boost::algorithm::to_lower_copy(boardType);

        bool customNameEnabled = false;
        if (findCustomNameEnabled != boardValues.end() &&
            findCustomNameEnabled->type() == nlohmann::json::value_t::boolean)
        {
            customNameEnabled = findCustomNameEnabled->get<bool>();
            std::clog << "Using custom name  " << boardName
                      << " for dbus object.\n";
        }

        if (!customNameEnabled)
        {
            std::regex_replace(boardName.begin(), boardName.begin(),
                               boardName.end(), illegalDbusMemberRegex, "_");
        }
        std::string boardPath = "/xyz/openbmc_project/inventory/system/";
        boardPath += boardtypeLower;
        boardPath += "/";
        boardPath += boardName;

        std::shared_ptr<sdbusplus::asio::dbus_interface> inventoryIface =
            createInterface(objServer, boardPath,
                            "xyz.openbmc_project.Inventory.Item", boardName);

        std::string boardIname = "xyz.openbmc_project.Inventory.Item." +
                                 boardType;
        std::shared_ptr<sdbusplus::asio::dbus_interface> boardIface =
            createInterface(objServer, boardPath, boardIname, boardNameOrig);

        createAddObjectMethod(jsonPointerPath, boardPath, systemConfiguration,
                              objServer, boardNameOrig);
        // iterate through board properties to match up BoardIface == PropIface
        for (const auto& [propName, propValue] : boardValues.items())
        {
            if (propValue.type() == nlohmann::json::value_t::object)
            {
                if (propName == boardIname)
                {
                    for (const auto& [key, value] : propValue.items())
                    {
                        if (boardValues.contains(key))
                        {
                            boardValues[key] = value;
                        }
                        else
                        {
                            boardValues.update(propValue, true);
                        }
                    }
                }
            }
        }

        populateInterfaceFromJson(systemConfiguration, jsonPointerPath,
                                  boardIface, boardValues, objServer);
        std::vector<Association> associations;
        if (findBoardParent != boardValues.end() &&
            findBoardParent->type() == nlohmann::json::value_t::string)
        {
            std::string boardParent = findBoardParent->get<std::string>();
            associations.emplace_back("parent_chassis", "all_chassis",
                                      boardParent);
        }

        jsonPointerPath += "/";
        // iterate through board properties
        for (const auto& [propName, propValue] : boardValues.items())
        {
            if (propValue.type() == nlohmann::json::value_t::object)
            {
                if (propName == "xyz.openbmc_project.Association.Definitions")
                {
                    for (const auto& [key, value] : propValue.items())
                    {
                        if (key == "Associations" &&
                            value.type() == nlohmann::json::value_t::array)
                        {
                            for (const auto& arr : value)
                            {
                                if (arr.is_array() && arr.size() == 3)
                                {
                                    associations.emplace_back(
                                        arr[0].get<std::string>(),
                                        arr[1].get<std::string>(),
                                        arr[2].get<std::string>());
                                }
                                else
                                {
                                    std::cerr
                                        << "Error: Association requires {forward, backward and path} \n";
                                }
                            }
                        }
                    }
                }
                else
                {
                    std::shared_ptr<sdbusplus::asio::dbus_interface> iface =
                        createInterface(objServer, boardPath, propName,
                                        boardNameOrig);

                    populateInterfaceFromJson(
                        systemConfiguration, jsonPointerPath + propName, iface,
                        propValue, objServer, getPermission(propName));
                }
            }
            if (propName == probePath)
            {
                // Creating association between the entity manager object
                // path and probe Path(FRU Path)
                associations.emplace_back(fwdPath, revPath, propValue);
            }
        }

        if (!associations.empty())
        {
            std::shared_ptr<sdbusplus::asio::dbus_interface> parentIface =
                createInterface(objServer, boardPath, association::interface,
                                boardNameOrig);
            parentIface->register_property(
                "Associations", associations,
                sdbusplus::asio::PropertyPermission::readWrite);
            parentIface->initialize();
        }

        auto exposes = boardValues.find("Exposes");
        if (exposes == boardValues.end())
        {
            continue;
        }
        // iterate through exposes
        jsonPointerPath += "Exposes/";

        // store the board level pointer so we can modify it on the way down
        std::string jsonPointerPathBoard = jsonPointerPath;
        size_t exposesIndex = -1;
        for (auto& item : *exposes)
        {
            exposesIndex++;
            jsonPointerPath = jsonPointerPathBoard;
            jsonPointerPath += std::to_string(exposesIndex);

            auto findName = item.find("Name");
            if (findName == item.end())
            {
                std::cerr << "cannot find name in field " << item << "\n";
                continue;
            }
            auto findStatus = item.find("Status");
            // if status is not found it is assumed to be status = 'okay'
            if (findStatus != item.end())
            {
                if (*findStatus == "disabled")
                {
                    continue;
                }
            }
            auto findType = item.find("Type");
            std::string itemType;
            if (findType != item.end())
            {
                itemType = findType->get<std::string>();
                std::regex_replace(itemType.begin(), itemType.begin(),
                                   itemType.end(), illegalDbusPathRegex, "_");
            }
            else
            {
                itemType = "unknown";
            }
            std::string itemName = findName->get<std::string>();
            std::regex_replace(itemName.begin(), itemName.begin(),
                               itemName.end(), illegalDbusMemberRegex, "_");
            std::string ifacePath = boardPath;
            ifacePath += "/";
            ifacePath += itemName;

            std::shared_ptr<sdbusplus::asio::dbus_interface> itemIface =
                createInterface(objServer, ifacePath,
                                "xyz.openbmc_project.Configuration." + itemType,
                                boardNameOrig);

            if (itemType == "BMC")
            {
                std::shared_ptr<sdbusplus::asio::dbus_interface> bmcIface =
                    createInterface(objServer, ifacePath,
                                    "xyz.openbmc_project.Inventory.Item.Bmc",
                                    boardNameOrig);
                populateInterfaceFromJson(systemConfiguration, jsonPointerPath,
                                          bmcIface, item, objServer,
                                          getPermission(itemType));
            }
            else if (itemType == "System")
            {
                std::shared_ptr<sdbusplus::asio::dbus_interface> systemIface =
                    createInterface(objServer, ifacePath,
                                    "xyz.openbmc_project.Inventory.Item.System",
                                    boardNameOrig);
                populateInterfaceFromJson(systemConfiguration, jsonPointerPath,
                                          systemIface, item, objServer,
                                          getPermission(itemType));
            }

            populateInterfaceFromJson(systemConfiguration, jsonPointerPath,
                                      itemIface, item, objServer,
                                      getPermission(itemType));

            for (const auto& [name, config] : item.items())
            {
                jsonPointerPath = jsonPointerPathBoard;
                jsonPointerPath.append(std::to_string(exposesIndex))
                    .append("/")
                    .append(name);
                if (config.type() == nlohmann::json::value_t::object)
                {
                    std::string ifaceName =
                        "xyz.openbmc_project.Configuration.";
                    ifaceName.append(itemType).append(".").append(name);

                    std::shared_ptr<sdbusplus::asio::dbus_interface>
                        objectIface = createInterface(objServer, ifacePath,
                                                      ifaceName, boardNameOrig);

                    populateInterfaceFromJson(
                        systemConfiguration, jsonPointerPath, objectIface,
                        config, objServer, getPermission(name));
                }
                else if (config.type() == nlohmann::json::value_t::array)
                {
                    size_t index = 0;
                    if (config.empty())
                    {
                        continue;
                    }
                    bool isLegal = true;
                    auto type = config[0].type();
                    if (type != nlohmann::json::value_t::object)
                    {
                        continue;
                    }

                    // verify legal json
                    for (const auto& arrayItem : config)
                    {
                        if (arrayItem.type() != type)
                        {
                            isLegal = false;
                            break;
                        }
                    }
                    if (!isLegal)
                    {
                        std::cerr << "dbus format error" << config << "\n";
                        break;
                    }

                    for (auto& arrayItem : config)
                    {
                        std::string ifaceName =
                            "xyz.openbmc_project.Configuration.";
                        ifaceName.append(itemType).append(".").append(name);
                        ifaceName.append(std::to_string(index));

                        std::shared_ptr<sdbusplus::asio::dbus_interface>
                            objectIface = createInterface(
                                objServer, ifacePath, ifaceName, boardNameOrig);

                        populateInterfaceFromJson(
                            systemConfiguration,
                            jsonPointerPath + "/" + std::to_string(index),
                            objectIface, arrayItem, objServer,
                            getPermission(name));
                        index++;
                    }
                }
            }

            topology.addBoard(boardPath, boardType, boardNameOrig, item);
        }

        newBoards.emplace(boardPath, boardNameOrig);
    }

    for (const auto& [assocPath, assocPropValue] :
         topology.getAssocs(newBoards))
    {
        auto findBoard = newBoards.find(assocPath);
        if (findBoard == newBoards.end())
        {
            continue;
        }

        auto ifacePtr = createInterface(
            objServer, assocPath, "xyz.openbmc_project.Association.Definitions",
            findBoard->second);

        ifacePtr->register_property("Associations", assocPropValue);
        tryIfaceInitialize(ifacePtr);
    }
}

// reads json files out of the filesystem
bool loadConfigurations(std::list<nlohmann::json>& configurations)
{
    // find configuration files
    std::vector<std::filesystem::path> jsonPaths;
    if (!findFiles(
            std::vector<std::filesystem::path>{configurationDirectory,
                                               hostConfigurationDirectory},
            R"(.*\.json)", jsonPaths))
    {
        std::cerr << "Unable to find any configuration files in "
                  << configurationDirectory << "\n";
        return false;
    }

    std::ifstream schemaStream(std::string(schemaDirectory) + "/" +
                               globalSchema);
    if (!schemaStream.good())
    {
        std::cerr
            << "Cannot open schema file,  cannot validate JSON, exiting\n\n";
        std::exit(EXIT_FAILURE);
        return false;
    }
    nlohmann::json schema = nlohmann::json::parse(schemaStream, nullptr, false,
                                                  true);
    if (schema.is_discarded())
    {
        std::cerr
            << "Illegal schema file detected, cannot validate JSON, exiting\n";
        std::exit(EXIT_FAILURE);
        return false;
    }

    for (auto& jsonPath : jsonPaths)
    {
        std::ifstream jsonStream(jsonPath.c_str());
        if (!jsonStream.good())
        {
            std::cerr << "unable to open " << jsonPath.string() << "\n";
            continue;
        }
        auto data = nlohmann::json::parse(jsonStream, nullptr, false, true);
        if (data.is_discarded())
        {
            std::cerr << "syntax error in " << jsonPath.string() << "\n";
            continue;
        }
        /*
         * todo(james): reenable this once less things are in flight
         *
        if (!validateJson(schema, data))
        {
            std::cerr << "Error validating " << jsonPath.string() << "\n";
            continue;
        }
        */

        if (data.type() == nlohmann::json::value_t::array)
        {
            for (auto& d : data)
            {
                configurations.emplace_back(d);
            }
        }
        else
        {
            configurations.emplace_back(data);
        }
    }
    return true;
}

static bool deviceRequiresPowerOn(const nlohmann::json& entity)
{
    auto powerState = entity.find("PowerState");
    if (powerState == entity.end())
    {
        return false;
    }

    const auto* ptr = powerState->get_ptr<const std::string*>();
    if (ptr == nullptr)
    {
        return false;
    }

    return *ptr == "On" || *ptr == "BiosPost";
}

static void pruneDevice(const nlohmann::json& systemConfiguration,
                        const bool powerOff, const bool scannedPowerOff,
                        const std::string& name, const nlohmann::json& device)
{
    if (systemConfiguration.contains(name))
    {
        return;
    }

    if (deviceRequiresPowerOn(device) && (powerOff || scannedPowerOff))
    {
        return;
    }

    logDeviceRemoved(device);
}

void startRemovedTimer(boost::asio::steady_timer& timer,
                       nlohmann::json& systemConfiguration)
{
    static bool scannedPowerOff = false;
    static bool scannedPowerOn = false;

    if (systemConfiguration.empty() || lastJson.empty())
    {
        return; // not ready yet
    }
    if (scannedPowerOn)
    {
        return;
    }

    if (!isPowerOn() && scannedPowerOff)
    {
        return;
    }

    timer.expires_after(std::chrono::seconds(10));
    timer.async_wait(
        [&systemConfiguration](const boost::system::error_code& ec) {
        if (ec == boost::asio::error::operation_aborted)
        {
            return;
        }

        bool powerOff = !isPowerOn();
        for (const auto& [name, device] : lastJson.items())
        {
            pruneDevice(systemConfiguration, powerOff, scannedPowerOff, name,
                        device);
        }

        scannedPowerOff = true;
        if (!powerOff)
        {
            scannedPowerOn = true;
        }
    });
}

static std::vector<std::weak_ptr<sdbusplus::asio::dbus_interface>>&
    getDeviceInterfaces(const nlohmann::json& device)
{
    return inventory[device["Name"].get<std::string>()];
}

static void pruneConfiguration(nlohmann::json& systemConfiguration,
                               sdbusplus::asio::object_server& objServer,
                               bool powerOff, const std::string& name,
                               const nlohmann::json& device)
{
    if (powerOff && deviceRequiresPowerOn(device))
    {
        // power not on yet, don't know if it's there or not
        return;
    }

    auto& ifaces = getDeviceInterfaces(device);
    for (auto& iface : ifaces)
    {
        auto sharedPtr = iface.lock();
        if (!!sharedPtr)
        {
            objServer.remove_interface(sharedPtr);
        }
    }

    ifaces.clear();
    systemConfiguration.erase(name);
    topology.remove(device["Name"].get<std::string>());
    logDeviceRemoved(device);
}

static void deriveNewConfiguration(const nlohmann::json& oldConfiguration,
                                   nlohmann::json& newConfiguration)
{
    for (auto it = newConfiguration.begin(); it != newConfiguration.end();)
    {
        auto findKey = oldConfiguration.find(it.key());
        if (findKey != oldConfiguration.end())
        {
            it = newConfiguration.erase(it);
        }
        else
        {
            it++;
        }
    }
}

static void publishNewConfiguration(
    const size_t& instance, const size_t count,
    boost::asio::steady_timer& timer, nlohmann::json& systemConfiguration,
    // Gerrit discussion:
    // https://gerrit.openbmc-project.xyz/c/openbmc/entity-manager/+/52316/6
    //
    // Discord discussion:
    // https://discord.com/channels/775381525260664832/867820390406422538/958048437729910854
    //
    // NOLINTNEXTLINE(performance-unnecessary-value-param)
    const nlohmann::json newConfiguration,
    sdbusplus::asio::object_server& objServer)
{
    loadOverlays(newConfiguration);

    boost::asio::post(io, [systemConfiguration]() {
        if (!writeJsonFiles(systemConfiguration))
        {
            std::cerr << "Error writing json files\n";
        }
    });

    boost::asio::post(io, [&instance, count, &timer, newConfiguration,
                           &systemConfiguration, &objServer]() {
        postToDbus(newConfiguration, systemConfiguration, objServer);
        if (count == instance)
        {
            startRemovedTimer(timer, systemConfiguration);
        }
    });
}

// main properties changed entry
void propertiesChangedCallback(nlohmann::json& systemConfiguration,
                               sdbusplus::asio::object_server& objServer)
{
    static bool inProgress = false;
    static boost::asio::steady_timer timer(io);
    static size_t instance = 0;
    instance++;
    size_t count = instance;

    timer.expires_after(std::chrono::seconds(5));

    // setup an async wait as we normally get flooded with new requests
    timer.async_wait([&systemConfiguration, &objServer,
                      count](const boost::system::error_code& ec) {
        if (ec == boost::asio::error::operation_aborted)
        {
            // we were cancelled
            return;
        }
        if (ec)
        {
            std::cerr << "async wait error " << ec << "\n";
            return;
        }

        if (inProgress)
        {
            propertiesChangedCallback(systemConfiguration, objServer);
            return;
        }
        inProgress = true;

        nlohmann::json oldConfiguration = systemConfiguration;
        auto missingConfigurations = std::make_shared<nlohmann::json>();
        *missingConfigurations = systemConfiguration;

        std::list<nlohmann::json> configurations;
        if (!loadConfigurations(configurations))
        {
            std::cerr << "Could not load configurations\n";
            inProgress = false;
            return;
        }

        auto perfScan = std::make_shared<PerformScan>(
            systemConfiguration, *missingConfigurations, configurations,
            objServer,
            [&systemConfiguration, &objServer, count, oldConfiguration,
             missingConfigurations]() {
            // this is something that since ac has been applied to the bmc
            // we saw, and we no longer see it
            bool powerOff = !isPowerOn();
            for (const auto& [name, device] : missingConfigurations->items())
            {
                pruneConfiguration(systemConfiguration, objServer, powerOff,
                                   name, device);
            }

            nlohmann::json newConfiguration = systemConfiguration;

            deriveNewConfiguration(oldConfiguration, newConfiguration);

            for (const auto& [_, device] : newConfiguration.items())
            {
                logDeviceAdded(device);
            }

            inProgress = false;

            boost::asio::post(
                io, std::bind_front(publishNewConfiguration, std::ref(instance),
                                    count, std::ref(timer),
                                    std::ref(systemConfiguration),
                                    newConfiguration, std::ref(objServer)));
        });
        perfScan->run();
    });
}

// Extract the D-Bus interfaces to probe from the JSON config files.
static std::set<std::string> getProbeInterfaces()
{
    std::set<std::string> interfaces;
    std::list<nlohmann::json> configurations;
    if (!loadConfigurations(configurations))
    {
        return interfaces;
    }

    for (auto it = configurations.begin(); it != configurations.end();)
    {
        auto findProbe = it->find("Probe");
        if (findProbe == it->end())
        {
            std::cerr << "configuration file missing probe:\n " << *it << "\n";
            it++;
            continue;
        }

        nlohmann::json probeCommand;
        if ((*findProbe).type() != nlohmann::json::value_t::array)
        {
            probeCommand = nlohmann::json::array();
            probeCommand.push_back(*findProbe);
        }
        else
        {
            probeCommand = *findProbe;
        }

        for (const nlohmann::json& probeJson : probeCommand)
        {
            const std::string* probe = probeJson.get_ptr<const std::string*>();
            if (probe == nullptr)
            {
                std::cerr << "Probe statement wasn't a string, can't parse";
                continue;
            }
            // Skip it if the probe cmd doesn't contain an interface.
            if (findProbeType(*probe))
            {
                continue;
            }

            // syntax requires probe before first open brace
            auto findStart = probe->find('(');
            if (findStart != std::string::npos)
            {
                std::string interface = probe->substr(0, findStart);
                interfaces.emplace(interface);
            }
        }
        it++;
    }

    return interfaces;
}

// Check if InterfacesAdded payload contains an iface that needs probing.
static bool
    iaContainsProbeInterface(sdbusplus::message_t& msg,
                             const std::set<std::string>& probeInterfaces)
{
    sdbusplus::message::object_path path;
    DBusObject interfaces;
    std::set<std::string> interfaceSet;
    std::set<std::string> intersect;

    msg.read(path, interfaces);

    std::for_each(interfaces.begin(), interfaces.end(),
                  [&interfaceSet](const auto& iface) {
        interfaceSet.insert(iface.first);
    });

    std::set_intersection(interfaceSet.begin(), interfaceSet.end(),
                          probeInterfaces.begin(), probeInterfaces.end(),
                          std::inserter(intersect, intersect.end()));
    return !intersect.empty();
}

// Check if InterfacesRemoved payload contains an iface that needs probing.
static bool
    irContainsProbeInterface(sdbusplus::message_t& msg,
                             const std::set<std::string>& probeInterfaces)
{
    sdbusplus::message::object_path path;
    std::set<std::string> interfaces;
    std::set<std::string> intersect;

    msg.read(path, interfaces);

    std::set_intersection(interfaces.begin(), interfaces.end(),
                          probeInterfaces.begin(), probeInterfaces.end(),
                          std::inserter(intersect, intersect.end()));
    return !intersect.empty();
}

int main()
{
    // setup connection to dbus
    systemBus = std::make_shared<sdbusplus::asio::connection>(io);
    systemBus->request_name("xyz.openbmc_project.EntityManager");

    // The EntityManager object itself doesn't expose any properties.
    // No need to set up ObjectManager for the |EntityManager| object.
    sdbusplus::asio::object_server objServer(systemBus, /*skipManager=*/true);

    // All other objects that EntityManager currently support are under the
    // inventory subtree.
    // See the discussion at
    // https://discord.com/channels/775381525260664832/1018929092009144380
    objServer.add_manager("/xyz/openbmc_project/inventory");

    std::shared_ptr<sdbusplus::asio::dbus_interface> entityIface =
        objServer.add_interface("/xyz/openbmc_project/EntityManager",
                                "xyz.openbmc_project.EntityManager");

    // to keep reference to the match / filter objects so they don't get
    // destroyed

    nlohmann::json systemConfiguration = nlohmann::json::object();

    std::set<std::string> probeInterfaces = getProbeInterfaces();

    // We need a poke from DBus for static providers that create all their
    // objects prior to claiming a well-known name, and thus don't emit any
    // org.freedesktop.DBus.Properties signals.  Similarly if a process exits
    // for any reason, expected or otherwise, we'll need a poke to remove
    // entities from DBus.
    sdbusplus::bus::match_t nameOwnerChangedMatch(
        static_cast<sdbusplus::bus_t&>(*systemBus),
        sdbusplus::bus::match::rules::nameOwnerChanged(),
        [&](sdbusplus::message_t& m) {
        auto [name, oldOwner,
              newOwner] = m.unpack<std::string, std::string, std::string>();

        if (name.starts_with(':'))
        {
            // We should do nothing with unique-name connections.
            return;
        }

        propertiesChangedCallback(systemConfiguration, objServer);
    });
    // We also need a poke from DBus when new interfaces are created or
    // destroyed.
    sdbusplus::bus::match_t interfacesAddedMatch(
        static_cast<sdbusplus::bus_t&>(*systemBus),
        sdbusplus::bus::match::rules::interfacesAdded(),
        [&](sdbusplus::message_t& msg) {
        if (iaContainsProbeInterface(msg, probeInterfaces))
        {
            propertiesChangedCallback(systemConfiguration, objServer);
        }
    });
    sdbusplus::bus::match_t interfacesRemovedMatch(
        static_cast<sdbusplus::bus_t&>(*systemBus),
        sdbusplus::bus::match::rules::interfacesRemoved(),
        [&](sdbusplus::message_t& msg) {
        if (irContainsProbeInterface(msg, probeInterfaces))
        {
            propertiesChangedCallback(systemConfiguration, objServer);
        }
    });

    boost::asio::post(io, [&]() {
        propertiesChangedCallback(systemConfiguration, objServer);
    });

    entityIface->register_method("ReScan", [&]() {
        propertiesChangedCallback(systemConfiguration, objServer);
    });
    tryIfaceInitialize(entityIface);

    if (fwVersionIsSame())
    {
        if (std::filesystem::is_regular_file(currentConfiguration))
        {
            // this file could just be deleted, but it's nice for debug
            std::filesystem::create_directory(tempConfigDir);
            std::filesystem::remove(lastConfiguration);
            std::filesystem::copy(currentConfiguration, lastConfiguration);
            std::filesystem::remove(currentConfiguration);

            std::ifstream jsonStream(lastConfiguration);
            if (jsonStream.good())
            {
                auto data = nlohmann::json::parse(jsonStream, nullptr, false);
                if (data.is_discarded())
                {
                    std::cerr << "syntax error in " << lastConfiguration
                              << "\n";
                }
                else
                {
                    lastJson = std::move(data);
                }
            }
            else
            {
                std::cerr << "unable to open " << lastConfiguration << "\n";
            }
        }
    }
    else
    {
        // not an error, just logging at this level to make it in the journal
        std::cerr << "Clearing previous configuration\n";
        std::filesystem::remove(currentConfiguration);
    }

    // some boards only show up after power is on, we want to not say they are
    // removed until the same state happens
    setupPowerMatch(systemBus);

    io.run();

    return 0;
}
