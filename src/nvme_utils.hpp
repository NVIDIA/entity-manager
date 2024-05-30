#pragma once
#include <regex>
#include <string>
#include <vector>

#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>

extern "C"
{
// Include for I2C_SMBUS_BLOCK_MAX
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <i2c/smbus.h>
}

namespace nvme
{
    constexpr uint8_t address = 0x6A;
    constexpr size_t vendorIdSize = 2;
    constexpr uint8_t baseOffsetVendorId = 0x9;
}

/// \brief Add NVMe Object to dbus.
/// \param device - vector that contains device list
/// \param dbusInterfaceMap - Map to store fru device dbus path and interface
/// \param bus - bus number of the device
/// \param address - address of the device
/// \param unknownBusObjectCount - Unknown Bus object counter variable
/// \param objServer - dbus connection
void addNvmeObjectToDbus(
    std::vector<uint8_t>& device,
    boost::container::flat_map<
        std::pair<size_t, size_t>,
        std::shared_ptr<sdbusplus::asio::dbus_interface>>& dbusInterfaceMap,
    uint32_t bus, uint32_t address, size_t& unknownBusObjectCount,
    sdbusplus::asio::object_server& objServer);

/// \brief Find a NVMe Vendor ID.
/// \param file the open file handle
/// \param errorHelp and a helper string for failures
/// \param blockData buffer to return the last read block
/// \param baseOffset the offset to start the search at;
///        set to 0 to perform search;
///        returns the offset at which a header was found
/// \return whether a Vendor ID is legal
bool findNvmeVendorId(int file, const std::string& errorHelp,
                   std::array<uint8_t, I2C_SMBUS_BLOCK_MAX>& blockData,
                   uint8_t& baseOffset);

/// \brief Read NVMe Vendor ID.
//  \param bus bus number of the device
/// \param file the open file handle
/// \param errorHelp and a helper string for failures
/// \return the NVMe contents from the file and bool indicating if the NVMe Vendor ID
///         was found
std::pair<std::vector<uint8_t>, bool> readNvmeContents(int bus, int file, const std::string& errorHelp);
