#include "utils.hpp"

#include "../dbus_util.hpp"
#include "../utils.hpp"
#include "../variant_visitors.hpp"
#include "expression.hpp"

#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/bus/match.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <regex>
#include <vector>

namespace em_utils
{

constexpr const char* templateChar = "$";

bool fwVersionIsSame()
{
    std::ifstream version(versionFile);
    if (!version.good())
    {
        lg2::error("Can't read {PATH}", "PATH", versionFile);
        return false;
    }

    std::string versionData;
    std::string line;
    while (std::getline(version, line))
    {
        versionData += line;
    }

    std::string expectedHash =
        std::to_string(std::hash<std::string>{}(versionData));

    std::error_code ec;
    std::filesystem::create_directory(configurationOutDir, ec);

    if (ec)
    {
        lg2::error("could not create directory {DIR}", "DIR",
                   configurationOutDir);
        return false;
    }

    std::ifstream hashFile(versionHashFile);
    if (hashFile.good())
    {
        std::string hashString;
        hashFile >> hashString;

        if (expectedHash == hashString)
        {
            return true;
        }
        hashFile.close();
    }

    std::ofstream output(versionHashFile);
    output << expectedHash;
    return false;
}

// A variable that is still in a string after the substitution (the platform
// does not define it) is a variable to skip: the map entry, the property or
// the array element that holds it is not published, and a map or array that
// is left empty by this is not published either. Name, Type and Probe keep
// the old behavior (the variable is removed from the string), and so do the
// string properties of the D-Bus interfaces of a record
// (handleLeftOverRecordTemplateVars).
static bool hasLeftOverVar(const std::string& value)
{
    for (size_t pos = value.find(templateChar); pos != std::string::npos;
         pos = value.find(templateChar, pos + 1))
    {
        if (pos + 1 < value.size() &&
            (std::isalpha(static_cast<unsigned char>(value[pos + 1])) != 0 ||
             value[pos + 1] == '_'))
        {
            return true;
        }
    }
    return false;
}

static bool keepsLeftOverVar(const std::string& key)
{
    return key == "Name" || key == "Type" || key == "Probe";
}

static bool skipLeftOverTemplateVars(nlohmann::json& value);

// @returns true if something was skipped and nothing is left
static bool skipLeftOverTemplateVars(nlohmann::json::object_t& obj)
{
    bool skipped = false;
    for (auto it = obj.begin(); it != obj.end();)
    {
        bool drop = false;
        if (keepsLeftOverVar(it->first))
        {
            handleLeftOverTemplateVars(it->second);
        }
        else if (hasLeftOverVar(it->first))
        {
            drop = true;
        }
        else
        {
            const std::string* str = it->second.get_ptr<const std::string*>();
            drop = (str != nullptr && hasLeftOverVar(*str)) ||
                   skipLeftOverTemplateVars(it->second);
        }
        if (drop)
        {
            it = obj.erase(it);
            skipped = true;
        }
        else
        {
            ++it;
        }
    }
    return skipped && obj.empty();
}

static bool skipLeftOverTemplateVars(nlohmann::json::array_t& arr)
{
    bool skipped = false;
    for (auto it = arr.begin(); it != arr.end();)
    {
        const std::string* str = it->get_ptr<const std::string*>();
        if ((str != nullptr && hasLeftOverVar(*str)) ||
            skipLeftOverTemplateVars(*it))
        {
            it = arr.erase(it);
            skipped = true;
        }
        else
        {
            ++it;
        }
    }
    return skipped && arr.empty();
}

static bool skipLeftOverTemplateVars(nlohmann::json& value)
{
    if (auto* objPtr = value.get_ptr<nlohmann::json::object_t*>())
    {
        return skipLeftOverTemplateVars(*objPtr);
    }
    if (auto* arrPtr = value.get_ptr<nlohmann::json::array_t*>())
    {
        return skipLeftOverTemplateVars(*arrPtr);
    }
    return false;
}

void handleLeftOverTemplateVars(nlohmann::json& value)
{
    nlohmann::json::object_t* objPtr =
        value.get_ptr<nlohmann::json::object_t*>();
    if (objPtr != nullptr)
    {
        handleLeftOverTemplateVars(*objPtr);
        return;
    }

    nlohmann::json::array_t* arrPtr = value.get_ptr<nlohmann::json::array_t*>();
    if (arrPtr != nullptr)
    {
        handleLeftOverTemplateVars(*arrPtr);
        return;
    }

    std::string* strPtr = value.get_ptr<std::string*>();
    if (strPtr == nullptr)
    {
        return;
    }
    handleLeftOverTemplateVars(*strPtr);
}

void handleLeftOverTemplateVars(nlohmann::json::object_t& value)
{
    skipLeftOverTemplateVars(value);
}

void handleLeftOverTemplateVars(nlohmann::json::array_t& value)
{
    skipLeftOverTemplateVars(value);
}

// The properties of a D-Bus interface of a record are read by name (bmcweb
// fails a Chassis request when AssetTag or LocationContext is missing), so a
// string property keeps the old behavior: the variable is removed from the
// string and the property stays, as "" for a FRU field the FRU does not
// have. Maps and arrays in it skip entries as above.
// @returns true if something was skipped and nothing is left
static bool handleLeftOverInterfaceTemplateVars(nlohmann::json::object_t& iface)
{
    bool skipped = false;
    for (auto it = iface.begin(); it != iface.end();)
    {
        bool drop = false;
        if (hasLeftOverVar(it->first))
        {
            drop = true;
        }
        else if (auto* str = it->second.get_ptr<std::string*>())
        {
            handleLeftOverTemplateVars(*str);
        }
        else
        {
            drop = skipLeftOverTemplateVars(it->second);
        }
        if (drop)
        {
            it = iface.erase(it);
            skipped = true;
        }
        else
        {
            ++it;
        }
    }
    return skipped && iface.empty();
}

void handleLeftOverRecordTemplateVars(nlohmann::json::object_t& record)
{
    for (auto it = record.begin(); it != record.end();)
    {
        const std::string& key = it->first;
        nlohmann::json& value = it->second;
        auto* iface = value.get_ptr<nlohmann::json::object_t*>();
        bool drop = false;
        if (key == "Name" || key == "Probe" || key == "Exposes")
        {
            // Name is substituted on its own, Probe has no variables and
            // the Exposes items are handled one by one
        }
        else if (iface != nullptr && key != "Associations")
        {
            drop = handleLeftOverInterfaceTemplateVars(*iface);
        }
        else if (value.is_string())
        {
            handleLeftOverTemplateVars(value);
        }
        else
        {
            drop = skipLeftOverTemplateVars(value);
        }
        it = drop ? record.erase(it) : std::next(it);
    }
}

// @brief    finds a template var in a string
// @returns  subrange where the template var was found
// @returns  std::nullopt  if no template var was found
static std::optional<std::ranges::subrange<std::string::const_iterator>>
    findLeftOverTemplateVar(std::string& value)
{
    // Walking through the string to find $<templateVar>
    const std::ranges::subrange<std::string::const_iterator> findStart =
        iFindFirst(value, std::string_view(templateChar));

    if (!findStart)
    {
        return std::nullopt;
    }

    const std::ranges::subrange<std::string::const_iterator> searchRange(
        findStart.end(), value.end());

    const std::ranges::subrange<std::string::const_iterator> findSpace =
        iFindFirst(searchRange, " ");

    // No space means the template var spans to the end of
    // of the keyPair value, otherwise a space marks the end

    return std::ranges::subrange<std::string::const_iterator>(
        findStart.begin(), (findSpace) ? findSpace.begin() : value.end());
}

static bool handleLeftOverTemplateVarOnce(std::string& value)
{
    const std::optional<std::ranges::subrange<std::string::const_iterator>> tv =
        findLeftOverTemplateVar(value);

    if (!tv.has_value())
    {
        return false;
    }

    lg2::error(
        "There's still template variable {VAR} un-replaced. Removing it from the string.\n",
        "VAR", tv.value());
    value.erase(tv.value().begin(), tv.value().end());

    return true;
}

void handleLeftOverTemplateVars(std::string& value)
{
    while (handleLeftOverTemplateVarOnce(value))
    {}
}

// Replaces the template character like the other version of this function,
// but checks all properties on all interfaces provided to do the substitution
// with.
std::optional<std::string> templateCharReplace(
    nlohmann::json& value, const DBusObject& object, const size_t index,
    const std::optional<std::string>& replaceStr, bool handleLeftOver)
{
    // Substitute from one merged view of all interfaces, so that the longest
    // variable name wins across interfaces as well. A property name that
    // appears on more than one interface keeps the value of the first
    // interface, as before.
    DBusInterface merged;
    for (const auto& [_, interface] : object)
    {
        for (const auto& [name, propValue] : interface)
        {
            merged.emplace(name, propValue);
        }
    }

    // fru_device publishes the slot of a FRU as SLOT_NUMBER
    // (i2cFruSlotInfo.json, like the other FRU properties); configurations
    // write it as $SlotNumber.
    if (auto slot = merged.find("SLOT_NUMBER");
        slot != merged.end() && !merged.contains("SlotNumber"))
    {
        merged.emplace("SlotNumber", slot->second);
    }

    auto ret = templateCharReplace(value, merged, index, replaceStr);
    if (handleLeftOver)
    {
        handleLeftOverTemplateVars(value);
    }
    return ret;
}

DBusInterface platformTemplateVariables(
    const nlohmann::json& systemConfiguration)
{
    DBusInterface variables;
    for (const auto& [_, record] : systemConfiguration.items())
    {
        if (!record.is_object() || record.value("Type", "") != "Platform")
        {
            continue;
        }
        for (const auto& [name, propValue] : record.items())
        {
            if (name == "Name" || name == "Type" || name == "Probe")
            {
                continue;
            }
            const std::string variable = "Platform." + name;
            if (propValue.is_string())
            {
                variables.emplace(variable, propValue.get<std::string>());
            }
            else if (propValue.is_boolean())
            {
                variables.emplace(variable, propValue.get<bool>());
            }
            else if (propValue.is_number_unsigned())
            {
                variables.emplace(variable, propValue.get<uint64_t>());
            }
            else if (propValue.is_number_integer())
            {
                variables.emplace(variable, propValue.get<int64_t>());
            }
            else if (propValue.is_number_float())
            {
                variables.emplace(variable, propValue.get<double>());
            }
            // objects and arrays (Exposes, PlatformExposes, interface
            // objects) are not variables
        }
    }
    return variables;
}

static bool templateCharReplaceOneProperty(
    std::string& str, const std::string& propName,
    const DBusValueVariant& propValue, std::optional<std::string>& ret,
    bool& keepAsString)
{
    std::string templateName = templateChar + propName;
    std::ranges::subrange<std::string::const_iterator> find =
        iFindFirst(str, templateName);
    if (!find)
    {
        return false;
    }

    size_t start = find.begin() - str.begin();

    // check for additional operations
    if ((start == 0U) && find.end() == str.end())
    {
        // Whole-value substitution carries the source type; re-parsing a
        // string source would retype an all-digit serial as a number.
        keepAsString = std::holds_alternative<std::string>(propValue);
        str = std::visit(VariantToStringVisitor(), propValue);
        return true;
    }

    constexpr const std::array<char, 5> mathChars = {'+', '-', '%', '*', '/'};
    size_t nextItemIdx = start + templateName.size() + 1;

    if (nextItemIdx >= str.size() ||
        std::find(mathChars.begin(), mathChars.end(), str.at(nextItemIdx)) ==
            mathChars.end())
    {
        std::string val = std::visit(VariantToStringVisitor(), propValue);
        iReplaceAll(str, templateName, val);
        return false;
    }

    // save the prefix
    std::string prefix = str.substr(0, start);

    // operate on the rest
    std::string end = str.substr(nextItemIdx);

    std::vector<std::string> splitResult = split(end, ' ');

    // need at least 1 operation and number
    if (splitResult.size() < 2)
    {
        lg2::error("Syntax error on template replacement of {STR}", "STR", str);
        for (const std::string& data : splitResult)
        {
            lg2::error("{SPLIT} ", "SPLIT", data);
        }
        lg2::error("");
        return false;
    }

    // we assume that the replacement is a number, because we can
    // only do math on numbers.. we might concatenate strings in the
    // future, but that's later
    int number = std::visit(VariantToIntVisitor(), propValue);
    auto exprBegin = splitResult.begin();
    auto exprEnd = splitResult.end();

    number = expression::evaluate(number, exprBegin, exprEnd);

    std::string replaced(find.begin(), find.end());
    while (exprBegin != exprEnd)
    {
        replaced.append(" ").append(*exprBegin++);
    }
    ret = replaced;

    std::string result = prefix + std::to_string(number);
    while (exprEnd != splitResult.end())
    {
        result.append(" ").append(*exprEnd++);
    }
    str = result;

    return false;
}

static std::optional<DBusValueVariant> templateCharReplaceLoop(
    std::string& str, const DBusInterface& interface,
    std::optional<std::string>& ret, bool& keepAsString)
{
    // The longest names are tried first, so that a property whose name is the
    // beginning of another property's name (Bridge and BridgeEID) cannot
    // replace a part of the longer variable.
    std::vector<std::pair<const std::string*, const DBusValueVariant*>> ordered;
    ordered.reserve(interface.size());
    for (const auto& [propName, propValue] : interface)
    {
        ordered.emplace_back(&propName, &propValue);
    }
    std::ranges::stable_sort(ordered, [](const auto& a, const auto& b) {
        return a.first->size() > b.first->size();
    });

    for (const auto& [propName, propValue] : ordered)
    {
        if (templateCharReplaceOneProperty(str, *propName, *propValue, ret,
                                           keepAsString))
        {
            return *propValue;
        }
    }
    return std::nullopt;
}

static std::optional<DBusValueVariant> templateCharReplaceStr(
    std::string& str, const DBusInterface& interface, size_t index,
    const std::optional<std::string>& replaceStr, bool& keepAsString);

// The keys of an object are template targets too: the maps of a PLDMDevice
// item are keyed by names built from $Name or $Instance.
static void templateCharReplaceObj(
    nlohmann::json::object_t& objPtr, const DBusInterface& interface,
    const size_t index, const std::optional<std::string>& replaceStr)
{
    nlohmann::json::object_t replaced;
    for (auto& [key, value] : objPtr)
    {
        templateCharReplace(value, interface, index, replaceStr);

        std::string newKey = key;
        if (newKey.find(templateChar) != std::string::npos)
        {
            bool keepAsString = false;
            templateCharReplaceStr(newKey, interface, index, replaceStr,
                                   keepAsString);
        }
        replaced.insert_or_assign(std::move(newKey), std::move(value));
    }
    objPtr = std::move(replaced);
}

static void templateCharReplaceArray(
    nlohmann::json::array_t& arrayPtr, const DBusInterface& interface,
    const size_t index, const std::optional<std::string>& replaceStr)
{
    for (auto& value : arrayPtr)
    {
        templateCharReplace(value, interface, index, replaceStr);
    }
}

// @param strView: string to parse as number
// @param strView: may be in base 10 or base 16 with '0x' prefix
// @returns uint64_t number if it can be parsed as such
static std::optional<uint64_t> parseAsNumber(std::string_view strView)
{
    if (strView.empty())
    {
        return std::nullopt;
    }

    int base = 10;
    if (strView.starts_with("0x"))
    {
        strView.remove_prefix(2);
        base = 16;
    }
    else
    {
        // zeroed out FRU fields are not seen as numbers
        if (strView.starts_with("00"))
        {
            return std::nullopt;
        }
    }

    uint64_t temp = 0;
    bool fullMatch = false;
    const std::from_chars_result res =
        fromCharsWrapper(strView, temp, fullMatch, base);
    if (res.ec == std::errc{} && fullMatch)
    {
        return temp;
    }

    return std::nullopt;
}

static std::optional<DBusValueVariant> templateCharReplaceStr(
    std::string& str, const DBusInterface& interface, const size_t index,
    const std::optional<std::string>& replaceStr, bool& keepAsString)
{
    std::optional<std::string> ret = std::nullopt;

    if (replaceStr)
    {
        replaceAll(str, *replaceStr, std::to_string(index));
    }

    templateCharReplaceOneProperty(str, "index", index, ret, keepAsString);

    return templateCharReplaceLoop(str, interface, ret, keepAsString);
}

// finds the template character (currently set to $) and replaces the value with
// the field found in a dbus object i.e. $ADDRESS would get populated with the
// ADDRESS field from a object on dbus
std::optional<std::string> templateCharReplace(
    nlohmann::json& value, const DBusInterface& interface, const size_t index,
    const std::optional<std::string>& replaceStr)
{
    std::optional<std::string> ret = std::nullopt;

    nlohmann::json::object_t* objPtr =
        value.get_ptr<nlohmann::json::object_t*>();
    if (objPtr != nullptr)
    {
        templateCharReplaceObj(*objPtr, interface, index, replaceStr);
        return ret;
    }

    nlohmann::json::array_t* arrPtr = value.get_ptr<nlohmann::json::array_t*>();
    if (arrPtr != nullptr)
    {
        templateCharReplaceArray(*arrPtr, interface, index, replaceStr);
        return ret;
    }

    const std::string* strPtr = value.get_ptr<std::string*>();

    if (strPtr == nullptr)
    {
        return ret;
    }

    std::string str = *strPtr;

    bool keepAsString = false;
    auto exactVariant =
        templateCharReplaceStr(str, interface, index, replaceStr, keepAsString);
    if (exactVariant.has_value())
    {
        std::visit([&value](auto&& val) { value = val; }, *exactVariant);
        return ret;
    }

    const std::optional<uint64_t> optNum =
        keepAsString ? std::nullopt : parseAsNumber(str);
    if (optNum.has_value())
    {
        value = optNum.value();
    }
    else
    {
        value = str;
    }

    return ret;
}

sdbusplus::object_path buildInventorySystemPath(std::string& boardName,
                                                const std::string& boardType)
{
    const sdbusplus::object_path basePath(
        "/xyz/openbmc_project/inventory/system");

    boardName = dbus_util::sanitizeForDBusPathSegment(boardName);

    return basePath / toLowerCopy(boardType) / boardName;
}

std::string getExposesKey(const nlohmann::json& config)
{
    auto typeIt = config.find("Type");
    if (typeIt != config.end() && typeIt->is_string() &&
        typeIt->get<std::string>() == "Platform")
    {
        return "PlatformExposes";
    }
    return "Exposes";
}
} // namespace em_utils
