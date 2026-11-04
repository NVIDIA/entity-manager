#include "entity_manager/perform_scan.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

using json = nlohmann::json;
using probe::Token;
using probe::TokenType;

// parseProbeCommand joins the array statements and lexes them into tokens.
TEST(ParseProbeCommand, ParsesArrayOfStrings)
{
    json probe = json::array({"FOUND('A')", "FOUND('B')"});
    EXPECT_EQ(
        scan::detail::parseProbeCommand(probe),
        (std::vector<Token>{{TokenType::found, "A"}, {TokenType::found, "B"}}));
}

// A single-string "Probe" field is lexed directly.
TEST(ParseProbeCommand, ParsesSingleString)
{
    json probe = "TRUE";
    EXPECT_EQ(scan::detail::parseProbeCommand(probe),
              (std::vector<Token>{{TokenType::boolTrue, ""}}));
}

// A non-string statement in the array yields an empty vector (the error / not
// a valid probe condition).
TEST(ParseProbeCommand, ReturnsEmptyOnNonStringElement)
{
    json probe = json::array({"FOUND('A')", 42});
    EXPECT_TRUE(scan::detail::parseProbeCommand(probe).empty());
}

// systemConfiguration / missingConfigurations are keyed by a numeric record
// hash (see getRecordName), so these tests use hash-like numeric keys.

// Removes from missingConfigurations any config whose "Name" is in names.
TEST(PruneMissingByName, RemovesConfigsWhoseNameIsInList)
{
    json missing = {{"16888500906263256819", {{"Name", "A"}}},
                    {"3421789056127653902", {{"Name", "B"}}},
                    {"9995127843016654321", {{"Name", "C"}}}};
    std::vector<std::string> names = {"A", "C"};
    scan::detail::pruneMissingByName(missing, names);
    EXPECT_EQ(missing.size(), 1);
    EXPECT_EQ(missing["3421789056127653902"]["Name"], "B");
}

// collectConfiguredNames returns the Name of every systemConfiguration entry
// (the resolved names of already-applied configs that run() seeds into
// passedProbes).
TEST(CollectConfiguredNames, ReturnsAllNames)
{
    json systemConfiguration = {
        {"16888500906263256819", {{"Name", "Nvidia RTX PRO 6000 Blackwell 1"}}},
        {"3421789056127653902", {{"Name", "Nvidia RTX PRO 6000 Blackwell 2"}}}};
    std::vector<std::string> names =
        scan::detail::collectConfiguredNames(systemConfiguration);
    EXPECT_EQ(names.size(), 2);
    EXPECT_NE(std::find(names.begin(), names.end(),
                        "Nvidia RTX PRO 6000 Blackwell 1"),
              names.end());
    EXPECT_NE(std::find(names.begin(), names.end(),
                        "Nvidia RTX PRO 6000 Blackwell 2"),
              names.end());
}

// Regression for the templated-config prune bug: on a rescan a config that is
// already applied (its resolved templated Name is in systemConfiguration) must
// survive, while an unrelated missing config stays eligible for pruning. This
// mirrors the seed-then-prune that PerformScan::run() performs.
TEST(SeedAndPrune, RescanKeepsAppliedTemplatedConfig)
{
    const std::string gpuKey = "16888500906263256819";
    const std::string otherKey = "9995127843016654321";

    // "...Blackwell 1" was resolved from a templated Name and is applied.
    json systemConfiguration = {
        {gpuKey, {{"Name", "Nvidia RTX PRO 6000 Blackwell 1"}}}};

    // At the start of a rescan everything currently present is provisionally
    // "missing" until re-proven this pass.
    json missing = {{gpuKey, {{"Name", "Nvidia RTX PRO 6000 Blackwell 1"}}},
                    {otherKey, {{"Name", "Some Other Board"}}}};

    std::vector<std::string> passedProbes =
        scan::detail::collectConfiguredNames(systemConfiguration);
    scan::detail::pruneMissingByName(missing, passedProbes);

    EXPECT_EQ(missing.size(), 1);
    EXPECT_FALSE(missing.contains(gpuKey));
    EXPECT_TRUE(missing.contains(otherKey));
}

// The HGX_CPU_$SlotNumber record of configurations/nvidia/vera-cpu.json on a
// FRU that has no asset tag: the AssetTag interface keeps its property as ""
// (bmcweb fails the Chassis request when it is missing), and the other
// properties are substituted or kept as they are.
TEST(ReplaceTemplateFields, FruWithoutAssetTagKeepsAssetTagProperty)
{
    json::object_t record = {
        {"Name", "HGX_CPU_$SlotNumber"},
        {"Associations",
         {{"parent_chassis", "board/HGX_ProcessorModule_$SlotNumber"}}},
        {"Type", "Component"},
        {"xyz.openbmc_project.Inventory.Decorator.Asset",
         {{"Manufacturer", "NVIDIA"}, {"PartNumber", "$BOARD_PART_NUMBER"}}},
        {"xyz.openbmc_project.Inventory.Decorator.AssetTag",
         {{"AssetTag", "$PRODUCT_ASSET_TAG"}}},
        {"xyz.openbmc_project.Inventory.Decorator.Instance",
         {{"InstanceNumber", "$SlotNumber"}}},
        {"xyz.openbmc_project.Inventory.Decorator.LocationCode",
         {{"LocationCode", "G1:$SlotNumber.0"}}},
        {"xyz.openbmc_project.Inventory.Decorator.LocationContext",
         {{"LocationContext", "HGX_Chassis_0/ProcessorModule_$SlotNumber"}}},
        {"xyz.openbmc_project.Inventory.Decorator.Replaceable",
         {{"FieldReplaceable", false}}}};

    DBusObject fru;
    fru["xyz.openbmc_project.FruDevice"] = {
        {"BOARD_PART_NUMBER", std::string("699-2G558-0205-TS3")},
        {"SLOT_NUMBER", uint64_t{1}}};

    scan::detail::replaceTemplateFields(record, fru, 0, std::nullopt);

    // Name is substituted by the caller
    const json expected = {
        {"Name", "HGX_CPU_$SlotNumber"},
        {"Associations", {{"parent_chassis", "board/HGX_ProcessorModule_1"}}},
        {"Type", "Component"},
        {"xyz.openbmc_project.Inventory.Decorator.Asset",
         {{"Manufacturer", "NVIDIA"}, {"PartNumber", "699-2G558-0205-TS3"}}},
        {"xyz.openbmc_project.Inventory.Decorator.AssetTag",
         {{"AssetTag", ""}}},
        {"xyz.openbmc_project.Inventory.Decorator.Instance",
         {{"InstanceNumber", 1}}},
        {"xyz.openbmc_project.Inventory.Decorator.LocationCode",
         {{"LocationCode", "G1:1.0"}}},
        {"xyz.openbmc_project.Inventory.Decorator.LocationContext",
         {{"LocationContext", "HGX_Chassis_0/ProcessorModule_1"}}},
        {"xyz.openbmc_project.Inventory.Decorator.Replaceable",
         {{"FieldReplaceable", false}}}};
    EXPECT_EQ(expected, json(record));
}

// Configurations are probed in file name order, so the SMM board
// (boards/SMM_Carrier_P4103.json) comes before the platform topology that
// defines its $Platform.SmmParent and $Platform.SmmLocationContext.
static const json smmBoard = {
    {"Name", "SMM_0"},
    {"Type", "Board"},
    {"Probe", "xyz.openbmc_project.FruDevice({'BUS': 5})"},
    {"Associations",
     {{"parent_chassis", "$Platform.SmmParent"},
      {"host_management_network_access",
       "/xyz/openbmc_project/control/host0/HostManagementNetworkAccess"}}},
    {"xyz.openbmc_project.Inventory.Decorator.LocationContext",
     {{"LocationContext", "$Platform.SmmLocationContext"}}}};

static json platformRecord(const std::string& name)
{
    return {{"Name", name},
            {"Type", "Platform"},
            {"Probe", "xyz.openbmc_project.FruDevice({'BUS': 9})"},
            {"SmmParent", "board/SMM_Interposer_0"},
            {"SmmLocationContext", "Chassis_0/SMM_Interposer_0"}};
}

// The passes of one scan over configs, as PerformScan runs them: the
// configurations named in matching match, are substituted and stored in
// systemConfiguration, which is returned.
static json scanPasses(const std::vector<json>& configs,
                       const std::set<std::string>& matching,
                       json systemConfiguration, bool platformPassFirst)
{
    std::vector<std::string> passedProbes =
        scan::detail::collectConfiguredNames(systemConfiguration);
    std::optional<bool> platformPass = platformPassFirst;
    for (int passes = 0; platformPass; passes++)
    {
        if (passes == 10)
        {
            ADD_FAILURE() << "the scan does not end";
            break;
        }
        const bool pass =
            *platformPass &&
            scan::detail::hasPendingPlatformRecord(configs, passedProbes);
        bool passed = false;
        for (const json& config : configs)
        {
            const std::string name = config["Name"].get<std::string>();
            if (std::ranges::contains(passedProbes, name) ||
                !scan::detail::probedInPass(config, pass) ||
                !matching.contains(name))
            {
                continue;
            }
            passedProbes.push_back(name);
            passed = true;
            auto record = config.get<json::object_t>();
            scan::detail::replaceTemplateFields(
                record,
                scan::detail::withPlatformVariables({}, systemConfiguration), 0,
                std::nullopt);
            systemConfiguration[name] = record;
        }
        platformPass = scan::detail::nextPass(passed, pass);
    }
    return systemConfiguration;
}

static const json smmBoardWithPlatformVariables = {
    {"Name", "SMM_0"},
    {"Type", "Board"},
    {"Probe", "xyz.openbmc_project.FruDevice({'BUS': 5})"},
    {"Associations",
     {{"parent_chassis", "board/SMM_Interposer_0"},
      {"host_management_network_access",
       "/xyz/openbmc_project/control/host0/HostManagementNetworkAccess"}}},
    {"xyz.openbmc_project.Inventory.Decorator.LocationContext",
     {{"LocationContext", "Chassis_0/SMM_Interposer_0"}}}};

TEST(PlatformPass, BoardThatSortsFirstGetsThePlatformVariables)
{
    const std::vector<json> configs = {smmBoard, platformRecord("VR-NVL")};

    json records =
        scanPasses(configs, {"SMM_0", "VR-NVL"}, json::object(), true);

    EXPECT_EQ(smmBoardWithPlatformVariables, records["SMM_0"]);

    // without the Platform pass the board is substituted first: no
    // parent_chassis and an empty LocationContext
    records = scanPasses(configs, {"SMM_0", "VR-NVL"}, json::object(), false);

    const json withoutPlatformVariables = {
        {"Name", "SMM_0"},
        {"Type", "Board"},
        {"Probe", "xyz.openbmc_project.FruDevice({'BUS': 5})"},
        {"Associations",
         {{"host_management_network_access",
           "/xyz/openbmc_project/control/host0/HostManagementNetworkAccess"}}},
        {"xyz.openbmc_project.Inventory.Decorator.LocationContext",
         {{"LocationContext", ""}}}};
    EXPECT_EQ(withoutPlatformVariables, records["SMM_0"]);
}

// All the platform topologies are installed and usually only one matches, so a
// scan usually starts with a Platform pass that matches nothing; the scan
// goes on to the other records and ends.
TEST(PlatformPass, PlatformRecordThatNeverMatches)
{
    const std::vector<json> configs = {smmBoard, platformRecord("Other"),
                                       platformRecord("VR-NVL")};
    const json applied = {{"1234", platformRecord("VR-NVL")}};

    json records = scanPasses(configs, {"SMM_0"}, applied, true);

    EXPECT_EQ(smmBoardWithPlatformVariables, records["SMM_0"]);
}

TEST(PlatformPass, OnlyTheFirstPassOfAScanIsAPlatformPass)
{
    EXPECT_EQ(false, scan::detail::nextPass(false, true));
    EXPECT_EQ(false, scan::detail::nextPass(true, true));
    EXPECT_EQ(false, scan::detail::nextPass(true, false));
    EXPECT_EQ(std::nullopt, scan::detail::nextPass(false, false));
}

TEST(PlatformPass, ProbedInPassSelectsPlatformRecords)
{
    EXPECT_FALSE(scan::detail::probedInPass(smmBoard, true));
    EXPECT_TRUE(scan::detail::probedInPass(platformRecord("VR-NVL"), true));
    EXPECT_TRUE(scan::detail::probedInPass(smmBoard, false));
    EXPECT_TRUE(scan::detail::probedInPass(platformRecord("VR-NVL"), false));
    // a malformed Type is not a Platform record
    EXPECT_FALSE(scan::detail::probedInPass({{"Type", 1}}, true));
}

TEST(PlatformPass, NoPlatformPassWithoutAPendingPlatformRecord)
{
    // already applied: run() puts the applied names in passedProbes
    EXPECT_FALSE(scan::detail::hasPendingPlatformRecord(
        {smmBoard, platformRecord("VR-NVL")}, {"VR-NVL"}));
    EXPECT_FALSE(scan::detail::hasPendingPlatformRecord({smmBoard}, {}));
    EXPECT_TRUE(scan::detail::hasPendingPlatformRecord(
        {smmBoard, platformRecord("VR-NVL")}, {}));
}
