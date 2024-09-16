#include "nvme_utils.hpp"

#include "fru_utils.hpp"
#include "utils.hpp"

#include <array>
#include <iostream>

static constexpr bool debug = false;

resCodes
    formatNvmeVid(const std::vector<uint8_t>& nvmeBytes,
                  boost::container::flat_map<std::string, std::string>& result)
{
    if (nvmeBytes.size() < nvme::vendorIdSize)
    {
        std::cerr << "Error: trying to parse empty NVMe Vendor ID \n";
        return resCodes::resErr;
    }

    auto vidString = std::to_string((nvmeBytes[0] << 8) + nvmeBytes[1]);
    result["VendorId"] = vidString;

    return resCodes::resOK;
}

void addNvmeObjectToDbus(
    std::vector<uint8_t>& device,
    boost::container::flat_map<
        std::pair<size_t, size_t>,
        std::shared_ptr<sdbusplus::asio::dbus_interface>>& dbusInterfaceMap,
    uint32_t bus, uint32_t address, size_t& unknownBusObjectCount,
    sdbusplus::asio::object_server& objServer)
{
    boost::container::flat_map<std::string, std::string> formattedNvme;
    resCodes res = formatNvmeVid(device, formattedNvme);
    if (res == resCodes::resErr)
    {
        std::cerr << "failed to parse NVMe Vendor ID for device at bus " << bus
                  << " address " << address << "\n";
        return;
    }

    auto vendorIdFind = formattedNvme.find("VendorId");
    std::string vendorId;

    // Found under Vendor ID section and not an empty string.
    if (vendorIdFind != formattedNvme.end() && !vendorIdFind->second.empty())
    {
        vendorId = vendorIdFind->second;
    }
    else
    {
        vendorId = "UNKNOWN" + std::to_string(unknownBusObjectCount);
        unknownBusObjectCount++;
    }

    vendorId = "/xyz/openbmc_project/FruDevice/" + vendorId + "_" +
               std::to_string(bus);
    std::shared_ptr<sdbusplus::asio::dbus_interface> iface =
        objServer.add_interface(vendorId,
                                "xyz.openbmc_project.Inventory.Item.I2CDevice");
    dbusInterfaceMap[std::pair<size_t, size_t>(bus, address)] = iface;

    for (auto& property : formattedNvme)
    {
        std::regex_replace(property.second.begin(), property.second.begin(),
                           property.second.end(), nonAsciiRegex, "_");
        std::string key = std::regex_replace(property.first, nonAsciiRegex,
                                             "_");
        std::string value = property.second;
        // Remove the spaces from the end of the key string
        value.erase(std::find_if(value.rbegin(), value.rend(),
                                 [](unsigned char ch) {
            return (0 == std::isspace(ch));
        }).base(),
                    value.end());

        if (!iface->register_property(key, value + '\0'))
        {
            std::cerr << "Illegal key: " << key << "\n";
        }
        if (debug)
        {
            std::cout << property.first << ": " << value << "\n";
        }
    }

    iface->register_property("Bus", bus);
    iface->register_property("Address", address);

    iface->initialize();
}

bool findNvmeVendorId(int file, const std::string& errorHelp,
                      std::array<uint8_t, I2C_SMBUS_BLOCK_MAX>& blockData,
                      uint8_t& baseOffset)
{
    std::vector<uint16_t> invalidVid = {0x0000, 0xffff};

    // Vendor ID is in at offset 10:09
    int wordData = i2c_smbus_read_word_data(file, baseOffset);
    if (wordData < 0)
    {
        std::cerr << "Failed to read " << errorHelp << " base offset "
                  << baseOffset << "\n";
        return false;
    }

    blockData[0] = wordData & 0x00ff;
    blockData[1] = (wordData >> 8) & 0x00ff;

    uint16_t vendorId = __builtin_bswap16(wordData);
    std::vector<uint16_t>::iterator it = std::find(invalidVid.begin(),
                                                   invalidVid.end(), vendorId);
    if (it != invalidVid.end())
    {
        if (debug)
        {
            std::cerr << "Illegal Vendor ID " << errorHelp << " base offset "
                      << std::to_string(baseOffset) << "\n";
        }
        return false;
    }

    return true;
}

std::pair<std::vector<uint8_t>, bool>
    readNvmeContents(int bus, int file, const std::string& errorHelp)
{
    std::array<uint8_t, I2C_SMBUS_BLOCK_MAX> blockData{};
    uint8_t baseOffset = nvme::baseOffsetVendorId;
    int retry = 0;

    // Give the tolerance for NVMe drive access because sometimes we need to
    // wait for device ready if switch MUX
    for (retry = 0; retry < 3; retry++)
    {
        blockData.fill(0);
        if (findNvmeVendorId(file, errorHelp, blockData, baseOffset))
        {
            std::vector<uint8_t> device;
            device.insert(device.end(), blockData.begin(),
                          blockData.begin() + nvme::vendorIdSize);
            std::cout << "Success in reading NVMe Drive on I2C Bus "
                      << std::to_string(bus)
                      << ". Retry = " << std::to_string(retry) << "\n";
            return {device, true};
        }

        usleep(1000);
    }

    std::cerr << "Failed to get the correct Vendor ID of NVMe Drive on I2C Bus "
              << std::to_string(bus) << ". Retry = " << std::to_string(retry)
              << "\n";

    return {{}, false};
}
