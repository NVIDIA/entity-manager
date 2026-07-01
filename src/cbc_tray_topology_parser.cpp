#include <boost/asio/io_context.hpp>
#include <boost/container/flat_map.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/bus/match.hpp>
#include <sdbusplus/message.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

PHOSPHOR_LOG2_USING;

namespace cbc_tray_topology
{

// D-Bus Constants
static constexpr const char* serviceName =
    "xyz.openbmc_project.CBCTrayTopologyParser";
static constexpr const char* vendorInfoInterface =
    "xyz.openbmc_project.Inventory.Decorator.VendorInformation";
static constexpr const char* objectMapperService =
    "xyz.openbmc_project.ObjectMapper";
static constexpr const char* objectMapperPath =
    "/xyz/openbmc_project/object_mapper";
static constexpr const char* objectMapperInterface =
    "xyz.openbmc_project.ObjectMapper";
static constexpr const char* inventoryCableCartridgePath =
    "/xyz/openbmc_project/inventory/system/cablecartridge";

// CBC Tray Topology parsing constants
static constexpr size_t trayTopologyStringLength = 16;
static constexpr size_t trayTopologyTokenLength = 2;
static constexpr size_t trayTopologyByteLength = 8;
static constexpr uint8_t trayTopologyMinRevision = 2;

// Structure for parsing CBC Tray Topology data.
// Matching the CBC FRU specification used by GB200 Chassis
#pragma pack(push, 1)
struct TrayTopology
{
    uint8_t revision;
    uint8_t reserved1;
    uint8_t chassisSlotNumber;
    uint8_t trayIndex;
    uint8_t topologyId;
    uint8_t reserved2;
    uint8_t reserved3;
    uint8_t reserved4;
};
#pragma pack(pop)

static_assert(sizeof(TrayTopology) == trayTopologyByteLength,
              "TrayTopology size must match expected byte length");
static_assert(std::is_trivially_copyable_v<TrayTopology>,
              "TrayTopology must be trivially copyable for memcpy");

// Structure to hold CBC object information
struct CBCObjectInfo
{
    std::shared_ptr<sdbusplus::asio::dbus_interface> interface;
    uint8_t chassisPhysicalSlotNumber = 0;
    uint8_t computeTrayIndex = 0;
    uint8_t revisionId = 0;
    uint8_t topologyId = 0;
};

using DBusPropertyVariant =
    std::variant<std::vector<std::string>, std::vector<double>, std::string,
                 int64_t, uint64_t, double, int32_t, uint32_t, int16_t,
                 uint16_t, uint8_t, bool, std::vector<uint8_t>>;

// Global map to track CBC objects and their interfaces
boost::container::flat_map<std::string, CBCObjectInfo> cbcObjects;

// Global pointers for D-Bus connection and object server
std::shared_ptr<sdbusplus::asio::connection> conn;
std::shared_ptr<sdbusplus::asio::object_server> server;

bool isSelfSignal(const sdbusplus::message_t& msg)
{
    if (!conn)
    {
        return false;
    }

    const std::string sender = msg.get_sender();
    const std::string selfUniqueName = conn->get_unique_name();
    return !sender.empty() && sender == selfUniqueName;
}

// Forward declarations
void processCBCCustomField(const std::string& objectPath,
                           const std::string& customField1);
void setupCBCObject(const std::string& objectPath,
                    const std::string& sourceService);

/**
 * @brief Parse the CustomField1 hex string into TrayTopology structure
 * @param customField1 The hex string to parse (16 characters = 8 bytes)
 * @param topology Output TrayTopology structure
 * @return true if parsing succeeded, false otherwise
 */
bool parseTrayTopology(const std::string& customField1, TrayTopology& topology)
{
    // CBC FRU matching GB200 FRU topology that it is 8 bytes (string length 16)
    if (customField1.length() != trayTopologyStringLength)
    {
        lg2::debug("CBC Tray ID string length is invalid: {LEN}", "LEN",
                   customField1.length());
        return false;
    }

    std::array<uint8_t, trayTopologyByteLength> byteArray{};
    for (size_t i = 0; i < trayTopologyByteLength; i++)
    {
        const char* tokenStart =
            customField1.data() + (i * trayTopologyTokenLength);
        const char* tokenEnd = tokenStart + trayTopologyTokenLength;

        auto [ptr,
              ec] = std::from_chars(tokenStart, tokenEnd, byteArray[i], 16);
        if (ec != std::errc{} || ptr != tokenEnd)
        {
            lg2::error("Invalid hex in CBC Tray ID string: {STR}", "STR",
                       customField1);
            return false;
        }
    }

    std::memcpy(&topology, byteArray.data(), sizeof(topology));

    if (topology.revision < trayTopologyMinRevision)
    {
        lg2::debug("CBC Tray ID revision {REV} must be >= {MIN}", "REV",
                   static_cast<int>(topology.revision), "MIN",
                   static_cast<int>(trayTopologyMinRevision));
        return false;
    }

    return true;
}

/**
 * @brief Process CustomField1 data and update D-Bus properties
 * @param objectPath The D-Bus object path of the CBC
 * @param customField1 The CustomField1 value to parse
 */
void processCBCCustomField(const std::string& objectPath,
                           const std::string& customField1)
{
    lg2::info("Processing CustomField1 for CBC: {PATH}", "PATH", objectPath);

    TrayTopology topology{};
    if (!parseTrayTopology(customField1, topology))
    {
        lg2::debug("Skipping OEM properties for CBC: {PATH}", "PATH",
                   objectPath);

        // If we previously had valid data and created an interface,
        // remove it now since the data is no longer valid
        auto it = cbcObjects.find(objectPath);
        if (it != cbcObjects.end())
        {
            lg2::info(
                "Removing VendorInformation interface due to invalid data: {PATH}",
                "PATH", objectPath);
            if (it->second.interface)
            {
                // Note: No try-catch here intentionally. If remove_interface
                // fails, we're in an undefined state (D-Bus interface exists
                // but we can't manage it). Let the exception propagate to crash
                // the service and restart in a clean, defined state.
                server->remove_interface(it->second.interface);
            }
            cbcObjects.erase(it);
        }
        return;
    }

    auto it = cbcObjects.find(objectPath);
    if (it == cbcObjects.end())
    {
        // Create new interface
        lg2::info("Creating VendorInformation interface for: {PATH}", "PATH",
                  objectPath);

        CBCObjectInfo info;
        info.chassisPhysicalSlotNumber = topology.chassisSlotNumber;
        info.computeTrayIndex = topology.trayIndex;
        info.revisionId = topology.revision;
        info.topologyId = topology.topologyId;

        try
        {
            info.interface =
                server->add_interface(objectPath, vendorInfoInterface);

            info.interface->register_property(
                "ChassisPhysicalSlotNumber",
                static_cast<uint8_t>(topology.chassisSlotNumber));
            info.interface->register_property(
                "ComputeTrayIndex", static_cast<uint8_t>(topology.trayIndex));
            info.interface->register_property(
                "RevisionId", static_cast<uint8_t>(topology.revision));
            info.interface->register_property(
                "TopologyId", static_cast<uint8_t>(topology.topologyId));

            info.interface->initialize();

            cbcObjects[objectPath] = std::move(info);

            lg2::info(
                "Created VendorInformation properties - ChassisSlot: {SLOT}, "
                "TrayIndex: {TRAY}, Revision: {REV}, TopologyId: {TOP}",
                "SLOT", static_cast<int>(topology.chassisSlotNumber), "TRAY",
                static_cast<int>(topology.trayIndex), "REV",
                static_cast<int>(topology.revision), "TOP",
                static_cast<int>(topology.topologyId));
        }
        catch (const std::exception& e)
        {
            lg2::error("Failed to create D-Bus interface for {PATH}: {ERR}",
                       "PATH", objectPath, "ERR", e.what());
            if (info.interface)
            {
                // Note: No try-catch here intentionally. If remove_interface
                // fails, we're in an undefined state (D-Bus interface exists
                // but we can't manage it). Let the exception propagate to crash
                // the service and restart in a clean, defined state.
                server->remove_interface(info.interface);
            }
        }
    }
    else
    {
        // Update existing interface
        lg2::info("Updating VendorInformation interface for: {PATH}", "PATH",
                  objectPath);

        auto& info = it->second;
        if (info.interface)
        {
            try
            {
                info.interface->set_property(
                    "ChassisPhysicalSlotNumber",
                    static_cast<uint8_t>(topology.chassisSlotNumber));
                info.interface->set_property(
                    "ComputeTrayIndex",
                    static_cast<uint8_t>(topology.trayIndex));
                info.interface->set_property(
                    "RevisionId", static_cast<uint8_t>(topology.revision));
                info.interface->set_property(
                    "TopologyId", static_cast<uint8_t>(topology.topologyId));

                info.chassisPhysicalSlotNumber = topology.chassisSlotNumber;
                info.computeTrayIndex = topology.trayIndex;
                info.revisionId = topology.revision;
                info.topologyId = topology.topologyId;

                lg2::info(
                    "Updated VendorInformation properties - ChassisSlot: {SLOT}, "
                    "TrayIndex: {TRAY}, Revision: {REV}, TopologyId: {TOP}",
                    "SLOT", static_cast<int>(topology.chassisSlotNumber),
                    "TRAY", static_cast<int>(topology.trayIndex), "REV",
                    static_cast<int>(topology.revision), "TOP",
                    static_cast<int>(topology.topologyId));
            }
            catch (const std::exception& e)
            {
                lg2::error(
                    "Failed to update D-Bus properties for {PATH}: {ERR}",
                    "PATH", objectPath, "ERR", e.what());
            }
        }
    }
}

/**
 * @brief Setup CBC object by reading its CustomField1 property
 * @param objectPath The D-Bus object path of the CBC
 * @param sourceService The D-Bus service providing the VendorInformation
 */
void setupCBCObject(const std::string& objectPath,
                    const std::string& sourceService)
{
    lg2::debug("Setting up CBC object: {PATH} from service {SVC}", "PATH",
               objectPath, "SVC", sourceService);

    conn->async_method_call(
        [objectPath](const boost::system::error_code& ec,
                     const DBusPropertyVariant& value) {
            if (ec)
            {
                lg2::debug("No CustomField1 for CBC {PATH}, skipping: {ERR}",
                           "PATH", objectPath, "ERR", ec.message());
                return;
            }

            const std::string* customField1 = std::get_if<std::string>(&value);
            if (customField1 == nullptr)
            {
                lg2::debug("CustomField1 is not a string for CBC {PATH}",
                           "PATH", objectPath);
                return;
            }

            processCBCCustomField(objectPath, *customField1);
        },
        sourceService, objectPath, "org.freedesktop.DBus.Properties", "Get",
        vendorInfoInterface, "CustomField1");
}

/**
 * @brief Handle PropertiesChanged signal for VendorInformation interface
 * @param msg The D-Bus message containing the signal
 */
void handlePropertiesChanged(sdbusplus::message_t& msg)
{
    if (isSelfSignal(msg))
    {
        return;
    }

    std::string interface;
    boost::container::flat_map<std::string, DBusPropertyVariant>
        propertiesChanged;

    try
    {
        msg.read(interface, propertiesChanged);
    }
    catch (const std::exception& e)
    {
        lg2::error("Failed to read PropertiesChanged signal: {ERR}", "ERR",
                   e.what());
        return;
    }

    if (interface != vendorInfoInterface)
    {
        return;
    }

    std::string objectPath = msg.get_path();
    lg2::debug("PropertiesChanged on {PATH}", "PATH", objectPath);

    auto it = propertiesChanged.find("CustomField1");
    if (it != propertiesChanged.end())
    {
        const std::string* customField1 = std::get_if<std::string>(&it->second);
        if (customField1 != nullptr)
        {
            processCBCCustomField(objectPath, *customField1);
        }
    }
}

/**
 * @brief Handle InterfacesAdded signal for new objects
 * @param msg The D-Bus message containing the signal
 */
void handleInterfacesAdded(sdbusplus::message_t& msg)
{
    if (isSelfSignal(msg))
    {
        return;
    }

    sdbusplus::object_path objPath;
    boost::container::flat_map<
        std::string,
        boost::container::flat_map<std::string, DBusPropertyVariant>>
        interfaces;

    try
    {
        msg.read(objPath, interfaces);
    }
    catch (const std::exception& e)
    {
        lg2::error("Failed to read InterfacesAdded signal: {ERR}", "ERR",
                   e.what());
        return;
    }

    std::string objectPath = objPath.str;

    // Check if this object has VendorInformation interface
    auto vendorIt = interfaces.find(vendorInfoInterface);
    if (vendorIt == interfaces.end())
    {
        return;
    }

    lg2::info("New cable cartridge object detected: {PATH}", "PATH",
              objectPath);

    // Check for CustomField1 in the interfaces data
    auto customFieldIt = vendorIt->second.find("CustomField1");
    if (customFieldIt != vendorIt->second.end())
    {
        const std::string* customField1 =
            std::get_if<std::string>(&customFieldIt->second);
        if (customField1 != nullptr)
        {
            processCBCCustomField(objectPath, *customField1);
            return;
        }
    }

    // If CustomField1 not in signal, query it via GetObject
    conn->async_method_call(
        [objectPath](const boost::system::error_code& ec,
                     const boost::container::flat_map<
                         std::string, std::vector<std::string>>& services) {
            if (ec)
            {
                lg2::error("Failed to query ObjectMapper for {PATH}: {ERR}",
                           "PATH", objectPath, "ERR", ec.message());
                return;
            }

            for (const auto& [service, interfaces] : services)
            {
                // Skip our own service to avoid self-triggered
                // feedback from our own InterfacesAdded signals
                if (service == serviceName)
                {
                    continue;
                }
                if (std::find(interfaces.begin(), interfaces.end(),
                              vendorInfoInterface) != interfaces.end())
                {
                    setupCBCObject(objectPath, service);
                    return;
                }
            }
        },
        objectMapperService, objectMapperPath, objectMapperInterface,
        "GetObject", objectPath, std::vector<std::string>{vendorInfoInterface});
}

/**
 * @brief Handle InterfacesRemoved signal for removed objects
 * @param msg The D-Bus message containing the signal
 */
void handleInterfacesRemoved(sdbusplus::message_t& msg)
{
    if (isSelfSignal(msg))
    {
        return;
    }

    sdbusplus::object_path objPath;
    std::vector<std::string> interfaces;

    try
    {
        msg.read(objPath, interfaces);
    }
    catch (const std::exception& e)
    {
        lg2::error("Failed to read InterfacesRemoved signal: {ERR}", "ERR",
                   e.what());
        return;
    }

    std::string objectPath = objPath.str;

    // Check if VendorInformation interface was removed
    bool vendorInfoRemoved = false;
    for (const auto& iface : interfaces)
    {
        if (iface == vendorInfoInterface)
        {
            vendorInfoRemoved = true;
            break;
        }
    }

    if (!vendorInfoRemoved)
    {
        return;
    }

    auto it = cbcObjects.find(objectPath);
    if (it != cbcObjects.end())
    {
        lg2::info("Removing VendorInformation interface for: {PATH}", "PATH",
                  objectPath);
        if (it->second.interface)
        {
            // Note: No try-catch here intentionally. If remove_interface fails,
            // we're in an undefined state (D-Bus interface exists but we can't
            // manage it). Let the exception propagate to crash the service and
            // restart in a clean, defined state.
            server->remove_interface(it->second.interface);
        }
        cbcObjects.erase(it);
    }
}

/**
 * @brief Discover existing CBC objects on startup
 */
void discoverExistingCBCObjects()
{
    lg2::info("Discovering existing CBC objects...");

    // NOTE: Currently no retry on errors from ObjectMapper
    // (partial mitigation w/systemd dependency)
    conn->async_method_call(
        [](const boost::system::error_code& ec,
           const boost::container::flat_map<
               std::string,
               boost::container::flat_map<std::string,
                                          std::vector<std::string>>>& subtree) {
            if (ec)
            {
                lg2::error("Failed to query ObjectMapper: {ERR}", "ERR",
                           ec.message());
                return;
            }

            lg2::info("Found {COUNT} objects with VendorInformation", "COUNT",
                      subtree.size());

            for (const auto& [path, services] : subtree)
            {
                lg2::debug("Processing cable cartridge object: {PATH}", "PATH",
                           path);

                for (const auto& [service, interfaces] : services)
                {
                    // Skip our own service to avoid reading
                    // properties from our own interfaces
                    if (service == serviceName)
                    {
                        continue;
                    }
                    if (std::find(interfaces.begin(), interfaces.end(),
                                  vendorInfoInterface) != interfaces.end())
                    {
                        setupCBCObject(path, service);
                        break;
                    }
                }
            }
        },
        objectMapperService, objectMapperPath, objectMapperInterface,
        "GetSubTree", inventoryCableCartridgePath, 0,
        std::vector<std::string>{vendorInfoInterface});
}

} // namespace cbc_tray_topology

int main()
{
    using namespace cbc_tray_topology;

    lg2::info("Starting CBC Tray Topology Parser Service");

    // The asynchronous context runner
    boost::asio::io_context io;

    // Create D-Bus connection linked to the IO context
    conn = std::make_shared<sdbusplus::asio::connection>(io);
    conn->request_name(serviceName);

    // Create the object server instance
    server = std::make_shared<sdbusplus::asio::object_server>(conn);

    // Setup D-Bus match rules for property changes
    // Monitor PropertiesChanged on all cable objects
    auto propertiesChangedMatch = std::make_unique<sdbusplus::bus::match_t>(
        *conn,
        sdbusplus::bus::match::rules::propertiesChangedNamespace(
            inventoryCableCartridgePath, vendorInfoInterface),
        handlePropertiesChanged);

    // Monitor InterfacesAdded for new CBC objects
    auto interfacesAddedMatch = std::make_unique<sdbusplus::bus::match_t>(
        *conn,
        sdbusplus::bus::match::rules::interfacesAddedAtPath(
            inventoryCableCartridgePath),
        handleInterfacesAdded);

    // Monitor InterfacesRemoved for removed CBC objects
    auto interfacesRemovedMatch = std::make_unique<sdbusplus::bus::match_t>(
        *conn,
        sdbusplus::bus::match::rules::interfacesRemovedAtPath(
            inventoryCableCartridgePath),
        handleInterfacesRemoved);

    // Discover existing CBC objects
    discoverExistingCBCObjects();

    lg2::info("D-Bus service '{SVC}' running...", "SVC", serviceName);

    // Run the main event loop
    io.run();

    return 0;
}
