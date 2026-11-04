#include "entity_manager/perform_scan.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
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
