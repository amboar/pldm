#include "bios.hpp"

#include "libpldmresponder/utils.hpp"
#include "registration.hpp"
#include "xyz/openbmc_project/Common/error.hpp"

#include <array>
#include <boost/crc.hpp>
#include <chrono>
#include <ctime>
#include <memory>
#include <numeric>
#include <phosphor-logging/elog-errors.hpp>
#include <phosphor-logging/log.hpp>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

using namespace pldm::responder::bios;
using namespace bios_parser;

namespace pldm
{

using namespace phosphor::logging;
using namespace sdbusplus::xyz::openbmc_project::Common::Error;
using EpochTimeUS = uint64_t;
using BIOSTableRow = std::vector<uint8_t>;
using BIOSJsonName = std::string;

constexpr auto dbusProperties = "org.freedesktop.DBus.Properties";
constexpr auto padChksumMax = 7;

namespace responder
{

namespace utils
{

void epochToBCDTime(uint64_t timeSec, uint8_t& seconds, uint8_t& minutes,
                    uint8_t& hours, uint8_t& day, uint8_t& month,
                    uint16_t& year)
{
    auto t = time_t(timeSec);
    auto time = localtime(&t);

    seconds = decimalToBcd(time->tm_sec);
    minutes = decimalToBcd(time->tm_min);
    hours = decimalToBcd(time->tm_hour);
    day = decimalToBcd(time->tm_mday);
    month =
        decimalToBcd(time->tm_mon + 1); // The number of months in the range
                                        // 0 to 11.PLDM expects range 1 to 12
    year = decimalToBcd(time->tm_year + 1900); // The number of years since 1900
}

size_t getTableTotalsize(size_t sizeWithoutPad)
{
    auto padSize = getNumPadBytes(sizeWithoutPad);
    return sizeWithoutPad + padSize + sizeof(uint32_t) /* checksum */;
}

void padAndChecksum(Table& table)
{
    auto padSize = getNumPadBytes(table.size());
    table.insert(table.end(), padSize, 0);

    boost::crc_32_type result;
    size_t size = table.size();
    result.process_bytes(table.data(), size);
    uint32_t checkSum = result.checksum();
    uint8_t* checkSumPtr = reinterpret_cast<uint8_t*>(&checkSum);
    table.insert(table.end(), checkSumPtr, checkSumPtr + sizeof(checkSum));
}

} // namespace utils

Response getDateTime(const pldm_msg* request, size_t /*payloadLength*/)
{
    uint8_t seconds = 0;
    uint8_t minutes = 0;
    uint8_t hours = 0;
    uint8_t day = 0;
    uint8_t month = 0;
    uint16_t year = 0;

    constexpr auto timeInterface = "xyz.openbmc_project.Time.EpochTime";
    constexpr auto hostTimePath = "/xyz/openbmc_project/time/host";
    Response response(sizeof(pldm_msg_hdr) + PLDM_GET_DATE_TIME_RESP_BYTES, 0);
    auto responsePtr = reinterpret_cast<pldm_msg*>(response.data());
    std::variant<EpochTimeUS> value;

    auto bus = sdbusplus::bus::new_default();
    try
    {
        auto service = getService(bus, hostTimePath, timeInterface);

        auto method = bus.new_method_call(service.c_str(), hostTimePath,
                                          dbusProperties, "Get");
        method.append(timeInterface, "Elapsed");

        auto reply = bus.call(method);
        reply.read(value);
    }

    catch (std::exception& e)
    {
        log<level::ERR>("Error getting time", entry("PATH=%s", hostTimePath),
                        entry("TIME INTERACE=%s", timeInterface));

        encode_get_date_time_resp(request->hdr.instance_id, PLDM_ERROR, seconds,
                                  minutes, hours, day, month, year,
                                  responsePtr);
        return response;
    }

    uint64_t timeUsec = std::get<EpochTimeUS>(value);

    uint64_t timeSec = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::microseconds(timeUsec))
                           .count();

    utils::epochToBCDTime(timeSec, seconds, minutes, hours, day, month, year);

    encode_get_date_time_resp(request->hdr.instance_id, PLDM_SUCCESS, seconds,
                              minutes, hours, day, month, year, responsePtr);
    return response;
}

/** @brief Generate the next attribute handle
 *
 *  @return - uint16_t - next attribute handle
 */
AttributeHandle nextAttributeHandle()
{
    static AttributeHandle attrHdl = 0;
    return attrHdl++;
}

/** @brief Generate the next string handle
 *  *
 *  @return - uint16_t - next string handle
 */
StringHandle nextStringHandle()
{
    static StringHandle strHdl = 0;
    return strHdl++;
}

/** @brief Construct the BIOS string table
 *
 *  @param[in] BIOSStringTable - the string table
 *  @param[in] transferHandle - transfer handle to identify part of transfer
 *  @param[in] transferOpFlag - flag to indicate which part of data being
 * transferred
 *  @param[in] instanceID - instance ID to identify the command
 */
Response getBIOSStringTable(BIOSTable& BIOSStringTable,
                            uint32_t /*transferHandle*/,
                            uint8_t /*transferOpFlag*/, uint8_t instanceID)

{
    Response response(sizeof(pldm_msg_hdr) + PLDM_GET_BIOS_TABLE_MIN_RESP_BYTES,
                      0);
    auto responsePtr = reinterpret_cast<pldm_msg*>(response.data());

    if (BIOSStringTable.isEmpty())
    { // no persisted table, constructing fresh table and file
        auto biosStrings = bios_parser::getStrings();
        std::sort(biosStrings.begin(), biosStrings.end());
        // remove all duplicate strings received from bios json
        biosStrings.erase(std::unique(biosStrings.begin(), biosStrings.end()),
                          biosStrings.end());
        size_t allStringsLen =
            std::accumulate(biosStrings.begin(), biosStrings.end(), 0,
                            [](size_t sum, const std::string& elem) {
                                return sum + elem.size();
                            });
        size_t sizeWithoutPad =
            allStringsLen +
            (biosStrings.size() * (sizeof(pldm_bios_string_table_entry) - 1));
        Table stringTable;
        stringTable.reserve(utils::getTableTotalsize(sizeWithoutPad));

        stringTable.resize(sizeWithoutPad);
        auto tablePtr = stringTable.data();
        for (const auto& elem : biosStrings)
        {
            auto stringPtr =
                reinterpret_cast<struct pldm_bios_string_table_entry*>(
                    tablePtr);

            stringPtr->string_handle = nextStringHandle();
            stringPtr->string_length = elem.length();
            memcpy(stringPtr->name, elem.c_str(), elem.length());
            tablePtr += sizeof(stringPtr->string_handle) +
                        sizeof(stringPtr->string_length);
            tablePtr += elem.length();
        }

        utils::padAndChecksum(stringTable);
        BIOSStringTable.store(stringTable);
        response.resize(sizeof(pldm_msg_hdr) +
                            PLDM_GET_BIOS_TABLE_MIN_RESP_BYTES +
                            stringTable.size(),
                        0);
        responsePtr = reinterpret_cast<pldm_msg*>(response.data());
        size_t respPayloadLength = response.size();
        uint32_t nxtTransferHandle = 0;
        uint8_t transferFlag = PLDM_START_AND_END;
        encode_get_bios_table_resp(instanceID, PLDM_SUCCESS, nxtTransferHandle,
                                   transferFlag, stringTable.data(),
                                   respPayloadLength, responsePtr);
    }
    else
    { // persisted table present, constructing response
        size_t respPayloadLength = response.size();
        uint32_t nxtTransferHandle = 0;
        uint8_t transferFlag = PLDM_START_AND_END;
        encode_get_bios_table_resp(instanceID, PLDM_SUCCESS, nxtTransferHandle,
                                   transferFlag, nullptr, respPayloadLength,
                                   responsePtr); // filling up the header here
        BIOSStringTable.load(response);
    }

    return response;
}

/** @brief Find the string handle from the BIOS string table given the name
 *
 *  @param[in] name - name of the BIOS string
 *  @param[in] BIOSStringTable - the string table
 *  @return - uint16_t - handle of the string
 */
StringHandle findStringHandle(const std::string& name,
                              const BIOSTable& BIOSStringTable)
{
    StringHandle hdl{};
    Response response;
    BIOSStringTable.load(response);

    auto tableData = response.data();
    size_t tableLen = response.size();
    auto tableEntry =
        reinterpret_cast<struct pldm_bios_string_table_entry*>(response.data());
    while (1)
    {
        hdl = tableEntry->string_handle;
        uint16_t len = tableEntry->string_length;
        if (name.compare(0, name.length(), tableEntry->name, len) == 0)
        {
            break;
        }
        tableData += (sizeof(struct pldm_bios_string_table_entry) - 1) + len;

        if (std::distance(tableData, response.data() + tableLen) <=
            padChksumMax)
        {
            log<level::ERR>("Reached end of BIOS string table,did not find the "
                            "handle for the string",
                            entry("STRING=%s", name.c_str()));
            elog<InternalFailure>();
            break;
        }

        tableEntry =
            reinterpret_cast<struct pldm_bios_string_table_entry*>(tableData);
    }
    return hdl;
}

/** @brief Find the string name from the BIOS string table for a string handle
 *
 *  @param[in] stringHdl - string handle
 *  @param[in] BIOSStringTable - the string table
 *
 *  @return - std::string - name of the corresponding BIOS string
 */
std::string findStringName(StringHandle stringHdl,
                           const BIOSTable& BIOSStringTable)
{
    std::string name;
    Response response;
    BIOSStringTable.load(response);

    auto tableData = response.data();
    size_t tableLen = response.size();
    auto tableEntry =
        reinterpret_cast<struct pldm_bios_string_table_entry*>(response.data());
    while (1)
    {
        StringHandle currHdl = tableEntry->string_handle;
        uint16_t len = tableEntry->string_length;
        if (currHdl == stringHdl)
        {
            name.resize(len);
            memcpy(name.data(), tableEntry->name, len);
            break;
        }
        tableData += (sizeof(struct pldm_bios_string_table_entry) - 1) + len;

        if (std::distance(tableData, response.data() + tableLen) <=
            padChksumMax)
        {
            log<level::ERR>("Reached end of BIOS string table,did not find "
                            "string name for handle",
                            entry("STRING_HANDLE=%d", stringHdl));
            break;
        }

        tableEntry =
            reinterpret_cast<struct pldm_bios_string_table_entry*>(tableData);
    }
    return name;
}

namespace bios_type_enum
{

using namespace bios_parser::bios_enum;

/** @brief Find the indices  into the array of the possible values of string
 *  handles for the current values.This is used in attribute value table
 *
 *  @param[in] possiVals - vector of string handles comprising all the possible
 *                         values for an attribute
 *  @param[in] currVals - vector of strings comprising all current values
 *                        for an attribute
 *  @param[in] BIOSStringTable - the string table
 *
 *  @return - std::vector<uint8_t> - indices into the array of the possible
 *                                   values of string handles
 */
std::vector<uint8_t> findStrIndices(PossibleValuesByHandle possiVals,
                                    CurrentValues currVals,
                                    const BIOSTable& BIOSStringTable)
{
    std::vector<uint8_t> stringIndices;

    for (const auto& currVal : currVals)
    {
        StringHandle curHdl;
        try
        {
            curHdl = findStringHandle(currVal, BIOSStringTable);
        }
        catch (InternalFailure& e)
        {
            log<level::ERR>("Exception fetching handle for the string",
                            entry("STRING=%s", currVal.c_str()));
            continue;
        }

        uint8_t i = 0;
        for (auto possiHdl : possiVals)
        {
            if (possiHdl == curHdl)
            {
                stringIndices.push_back(i);
                break;
            }
            i++;
        }
    }
    return stringIndices;
}

/** @brief Find the indices into the array of the possible values of string
 *  handles for the default values. This is used in attribute table
 *
 *  @param[in] possiVals - vector of strings comprising all the possible values
 *                         for an attribute
 *  @param[in] defVals - vector of strings comprising all the default values
 *                       for an attribute
 *  @return - std::vector<uint8_t> - indices into the array of the possible
 *                                   values of string
 */
std::vector<uint8_t> findDefaultValHandle(const PossibleValues& possiVals,
                                          const DefaultValues& defVals)
{
    std::vector<uint8_t> defHdls;
    for (const auto& defs : defVals)
    {
        auto index = std::lower_bound(possiVals.begin(), possiVals.end(), defs);
        if (index != possiVals.end())
        {
            defHdls.push_back(index - possiVals.begin());
        }
    }

    return defHdls;
}

/** @brief Construct the attibute table for BIOS type Enumeration and
 *         Enumeration ReadOnly
 *  @param[in] BIOSStringTable - the string table
 *  @param[in] biosJsonDir - path where the BIOS json files are present
 *  @param[in,out] attributeTable - the attribute table
 *
 */
void constructAttrTable(const BIOSTable& BIOSStringTable, Table& attributeTable)
{
    const auto& attributeMap = getValues();
    StringHandle strHandle;

    for (const auto& [key, value] : attributeMap)
    {
        try
        {
            strHandle = findStringHandle(key, BIOSStringTable);
        }
        catch (InternalFailure& e)
        {
            log<level::ERR>("Could not find handle for BIOS string",
                            entry("ATTRIBUTE=%s", key.c_str()));
            continue;
        }
        uint8_t typeOfAttr = (std::get<0>(value))
                                 ? PLDM_BIOS_ENUMERATION_READ_ONLY
                                 : PLDM_BIOS_ENUMERATION;
        PossibleValues possiVals = std::get<1>(value);
        DefaultValues defVals = std::get<2>(value);
        // both the possible and default values are stored in sorted manner to
        // ease in fetching back/comparison
        std::sort(possiVals.begin(), possiVals.end());
        std::sort(defVals.begin(), defVals.end());

        std::vector<StringHandle> possiValsByHdl;
        for (const auto& elem : possiVals)
        {
            try
            {
                auto hdl = findStringHandle(elem, BIOSStringTable);
                possiValsByHdl.push_back(std::move(hdl));
            }
            catch (InternalFailure& e)
            {
                log<level::ERR>("Could not find handle for BIOS string",
                                entry("STRING=%s", elem.c_str()));
                continue;
            }
        }
        auto defValsByHdl = findDefaultValHandle(possiVals, defVals);

        BIOSTableRow enumAttrTable(
            (sizeof(struct pldm_bios_attr_table_entry) - 1) + sizeof(uint8_t) +
                possiValsByHdl.size() * sizeof(uint16_t) + sizeof(uint8_t) +
                defValsByHdl.size() * sizeof(uint8_t),
            0);
        BIOSTableRow::iterator it = enumAttrTable.begin();
        auto attrPtr = reinterpret_cast<struct pldm_bios_attr_table_entry*>(
            enumAttrTable.data());
        attrPtr->attr_handle = nextAttributeHandle();
        attrPtr->attr_type = typeOfAttr;
        attrPtr->string_handle = std::move(strHandle);
        std::advance(it, (sizeof(struct pldm_bios_attr_table_entry) - 1));
        uint8_t numPossibleVals = possiValsByHdl.size();
        std::copy_n(&numPossibleVals, sizeof(numPossibleVals), it);
        std::advance(it, sizeof(numPossibleVals));
        std::copy_n(reinterpret_cast<uint8_t*>(possiValsByHdl.data()),
                    sizeof(uint16_t) * possiValsByHdl.size(), it);
        std::advance(
            it, sizeof(uint16_t) *
                    possiValsByHdl.size()); // possible val handle is uint16_t
        uint8_t numDefaultVals = defValsByHdl.size();
        std::copy_n(&numDefaultVals, sizeof(numDefaultVals), it);
        std::advance(it, sizeof(numDefaultVals));
        std::copy(defValsByHdl.begin(), defValsByHdl.end(), it);
        std::advance(it, defValsByHdl.size());

        std::move(enumAttrTable.begin(), enumAttrTable.end(),
                  std::back_inserter(attributeTable));
    }
}

void constructAttrValueEntry(
    const struct pldm_bios_attr_table_entry* attrTableEntry,
    const std::string& attrName, const BIOSTable& BIOSStringTable,
    Table& attrValueTable)
{
    CurrentValues currVals;
    try
    {
        currVals = getAttrValue(attrName);
    }
    catch (const std::exception& e)
    {
        log<level::ERR>("getAttrValue returned error for attribute",
                        entry("NAME=%s", attrName.c_str()),
                        entry("ERROR=%s", e.what()));
        return;
    }
    uint8_t pv_num =
        pldm_bios_table_attr_entry_enum_decode_pv_num(attrTableEntry);
    PossibleValuesByHandle pvHdls(pv_num, 0);
    pldm_bios_table_attr_entry_enum_decode_pv_hdls(attrTableEntry,
                                                   pvHdls.data(), pv_num);
    std::sort(currVals.begin(), currVals.end());

    auto currValStrIndices = findStrIndices(pvHdls, currVals, BIOSStringTable);

    auto entryLength = pldm_bios_table_attr_value_entry_encode_enum_length(
        currValStrIndices.size());
    auto tableSize = attrValueTable.size();
    attrValueTable.resize(tableSize + entryLength);
    pldm_bios_table_attr_value_entry_encode_enum(
        attrValueTable.data() + tableSize, entryLength,
        attrTableEntry->attr_handle, attrTableEntry->attr_type,
        currValStrIndices.size(), currValStrIndices.data());
}

} // end namespace bios_type_enum

namespace bios_type_string
{

using namespace bios_parser::bios_string;

/** @brief Construct the attibute table for BIOS type String and
 *         String ReadOnly
 *  @param[in] BIOSStringTable - the string table
 *  @param[in] biosJsonDir - path where the BIOS json files are present
 *  @param[in,out] attributeTable - the attribute table
 *
 */
void constructAttrTable(const BIOSTable& BIOSStringTable, Table& attributeTable)
{
    const auto& attributeMap = getValues();
    StringHandle strHandle;

    for (const auto& [key, value] : attributeMap)
    {
        try
        {
            strHandle = findStringHandle(key, BIOSStringTable);
        }
        catch (InternalFailure& e)
        {
            log<level::ERR>("Could not find handle for BIOS string",
                            entry("ATTRIBUTE=%s", key.c_str()));
            continue;
        }

        const auto& [type, strType, minStrLen, maxStrLen, defaultStrLen,
                     defaultStr] = value;
        uint8_t typeOfAttr =
            type ? PLDM_BIOS_STRING_READ_ONLY : PLDM_BIOS_STRING;

        BIOSTableRow stringAttrTable(bios_parser::bios_string::attrTableSize +
                                     defaultStr.size());
        BIOSTableRow::iterator it = stringAttrTable.begin();
        auto attrPtr = reinterpret_cast<struct pldm_bios_attr_table_entry*>(
            stringAttrTable.data());
        attrPtr->attr_handle = nextAttributeHandle();
        attrPtr->attr_type = typeOfAttr;
        attrPtr->string_handle = strHandle;

        std::advance(it, (sizeof(struct pldm_bios_attr_table_entry) - 1));
        std::copy_n(&strType, sizeof(uint8_t), it);
        std::advance(it, sizeof(uint8_t));
        std::copy_n(reinterpret_cast<const uint8_t*>(&minStrLen),
                    sizeof(uint16_t), it);
        std::advance(it, sizeof(uint16_t));
        std::copy_n(reinterpret_cast<const uint8_t*>(&maxStrLen),
                    sizeof(uint16_t), it);
        std::advance(it, sizeof(uint16_t));
        std::copy_n(reinterpret_cast<const uint8_t*>(&defaultStrLen),
                    sizeof(uint16_t), it);
        std::advance(it, sizeof(uint16_t));
        std::copy_n(defaultStr.data(), defaultStr.size(), it);
        std::advance(it, defaultStr.size());

        attributeTable.insert(attributeTable.end(), stringAttrTable.begin(),
                              stringAttrTable.end());
    }
}

void constructAttrValueEntry(const pldm_bios_attr_table_entry* attrTableEntry,
                             const std::string& attrName,
                             const BIOSTable& BIOSStringTable,
                             Table& attrValueTable)
{
    std::ignore = BIOSStringTable;
    std::string currStr;
    uint16_t currStrLen = 0;
    try
    {
        currStr = getAttrValue(attrName);
        currStrLen = currStr.size();
    }
    catch (const std::exception& e)
    {
        log<level::ERR>("getAttrValue returned error for attribute",
                        entry("NAME=%s", attrName.c_str()),
                        entry("ERROR=%s", e.what()));
        return;
    }
    auto entryLength =
        pldm_bios_table_attr_value_entry_encode_string_length(currStrLen);
    auto tableSize = attrValueTable.size();
    attrValueTable.resize(tableSize + entryLength);
    pldm_bios_table_attr_value_entry_encode_string(
        attrValueTable.data() + tableSize, entryLength,
        attrTableEntry->attr_handle, attrTableEntry->attr_type, currStrLen,
        currStr.c_str());
}

} // end namespace bios_type_string

void traverseBIOSAttrTable(const Table& biosAttrTable,
                           AttrTableEntryHandler handler)
{
    std::unique_ptr<pldm_bios_table_iter, decltype(&pldm_bios_table_iter_free)>
        iter(pldm_bios_table_iter_create(biosAttrTable.data(),
                                         biosAttrTable.size(),
                                         PLDM_BIOS_ATTR_TABLE),
             pldm_bios_table_iter_free);
    while (!pldm_bios_table_iter_is_end(iter.get()))
    {
        auto table_entry = pldm_bios_table_iter_attr_entry_value(iter.get());
        try
        {
            handler(table_entry);
        }
        catch (const std::exception& e)
        {
            log<level::ERR>("handler fails when traversing BIOSAttrTable",
                            entry("ERROR=%s", e.what()));
        }
        pldm_bios_table_iter_next(iter.get());
    }
}

using typeHandler = std::function<void(const BIOSTable& BIOSStringTable,
                                       Table& attributeTable)>;
std::map<BIOSJsonName, typeHandler> attrTypeHandlers{
    {bios_parser::bIOSEnumJson, bios_type_enum::constructAttrTable},
    {bios_parser::bIOSStrJson, bios_type_string::constructAttrTable}};

/** @brief Construct the BIOS attribute table
 *
 *  @param[in] BIOSAttributeTable - the attribute table
 *  @param[in] BIOSStringTable - the string table
 *  @param[in] transferHandle - transfer handle to identify part of transfer
 *  @param[in] transferOpFlag - flag to indicate which part of data being
 * transferred
 *  @param[in] instanceID - instance ID to identify the command
 *  @param[in] biosJsonDir - path where the BIOS json files are present
 */
Response getBIOSAttributeTable(BIOSTable& BIOSAttributeTable,
                               const BIOSTable& BIOSStringTable,
                               uint32_t /*transferHandle*/,
                               uint8_t /*transferOpFlag*/, uint8_t instanceID,
                               const char* biosJsonDir)
{
    Response response(sizeof(pldm_msg_hdr) + PLDM_GET_BIOS_TABLE_MIN_RESP_BYTES,
                      0);
    auto responsePtr = reinterpret_cast<pldm_msg*>(response.data());
    uint32_t nxtTransferHandle = 0;
    uint8_t transferFlag = PLDM_START_AND_END;

    if (BIOSAttributeTable.isEmpty())
    { // no persisted table, constructing fresh table and response
        Table attributeTable;
        fs::path dir(biosJsonDir);

        for (auto it = attrTypeHandlers.begin(); it != attrTypeHandlers.end();
             it++)
        {
            fs::path file = dir / it->first;
            if (fs::exists(file))
            {
                it->second(BIOSStringTable, attributeTable);
            }
        }

        if (attributeTable.empty())
        { // no available json file is found
            encode_get_bios_table_resp(instanceID, PLDM_BIOS_TABLE_UNAVAILABLE,
                                       nxtTransferHandle, transferFlag, nullptr,
                                       response.size(), responsePtr);
            return response;
        }
        utils::padAndChecksum(attributeTable);
        BIOSAttributeTable.store(attributeTable);
        response.resize(sizeof(pldm_msg_hdr) +
                        PLDM_GET_BIOS_TABLE_MIN_RESP_BYTES +
                        attributeTable.size());
        responsePtr = reinterpret_cast<pldm_msg*>(response.data());
        encode_get_bios_table_resp(instanceID, PLDM_SUCCESS, nxtTransferHandle,
                                   transferFlag, attributeTable.data(),
                                   response.size(), responsePtr);
    }
    else
    { // persisted table present, constructing response
        encode_get_bios_table_resp(instanceID, PLDM_SUCCESS, nxtTransferHandle,
                                   transferFlag, nullptr, response.size(),
                                   responsePtr); // filling up the header here
        BIOSAttributeTable.load(response);
    }

    return response;
}

using AttrValTableEntryConstructHandler =
    std::function<void(const struct pldm_bios_attr_table_entry* tableEntry,
                       const std::string& attrName,
                       const BIOSTable& BIOSStringTable, Table& table)>;

using AttrType = uint8_t;
const std::map<AttrType, AttrValTableEntryConstructHandler>
    AttrValTableConstructMap{
        {PLDM_BIOS_STRING, bios_type_string::constructAttrValueEntry},
        {PLDM_BIOS_STRING_READ_ONLY, bios_type_string::constructAttrValueEntry},
        {PLDM_BIOS_ENUMERATION, bios_type_enum::constructAttrValueEntry},
        {PLDM_BIOS_ENUMERATION_READ_ONLY,
         bios_type_enum::constructAttrValueEntry},
    };

void constructAttrValueTableEntry(
    const struct pldm_bios_attr_table_entry* attrEntry,
    const BIOSTable& BIOSStringTable, Table& attributeValueTable)
{
    auto attrName = findStringName(attrEntry->string_handle, BIOSStringTable);
    if (attrName.empty())
    {
        log<level::ERR>("invalid string handle",
                        entry("STRING_HANDLE=%d", attrEntry->string_handle));
        return;
    }

    AttrValTableConstructMap.at(attrEntry->attr_type)(
        attrEntry, attrName, BIOSStringTable, attributeValueTable);
}

/** @brief Construct the BIOS attribute value table
 *
 *  @param[in] BIOSAttributeValueTable - the attribute value table
 *  @param[in] BIOSAttributeTable - the attribute table
 *  @param[in] BIOSStringTable - the string table
 *  @param[in] transferHandle - transfer handle to identify part of transfer
 *  @param[in] transferOpFlag - flag to indicate which part of data being
 * transferred
 *  @param[in] instanceID - instance ID to identify the command
 */
Response getBIOSAttributeValueTable(BIOSTable& BIOSAttributeValueTable,
                                    const BIOSTable& BIOSAttributeTable,
                                    const BIOSTable& BIOSStringTable,
                                    uint32_t& /*transferHandle*/,
                                    uint8_t& /*transferOpFlag*/,
                                    uint8_t instanceID)
{
    Response response(sizeof(pldm_msg_hdr) + PLDM_GET_BIOS_TABLE_MIN_RESP_BYTES,
                      0);
    auto responsePtr = reinterpret_cast<pldm_msg*>(response.data());
    uint32_t nxtTransferHandle = 0;
    uint8_t transferFlag = PLDM_START_AND_END;

    if (!BIOSAttributeValueTable.isEmpty())
    {
        encode_get_bios_table_resp(instanceID, PLDM_SUCCESS, nxtTransferHandle,
                                   transferFlag, nullptr, response.size(),
                                   responsePtr); // filling up the header here
        BIOSAttributeValueTable.load(response);
        return response;
    }

    Table attributeValueTable;
    Table attributeTable;
    BIOSAttributeTable.load(attributeTable);
    traverseBIOSAttrTable(
        attributeTable,
        [&BIOSStringTable, &attributeValueTable](
            const struct pldm_bios_attr_table_entry* tableEntry) {
            constructAttrValueTableEntry(tableEntry, BIOSStringTable,
                                         attributeValueTable);
        });
    if (attributeValueTable.empty())
    {
        encode_get_bios_table_resp(instanceID, PLDM_BIOS_TABLE_UNAVAILABLE,
                                   nxtTransferHandle, transferFlag, nullptr,
                                   response.size(), responsePtr);
        return response;
    }
    utils::padAndChecksum(attributeValueTable);
    BIOSAttributeValueTable.store(attributeValueTable);

    response.resize(sizeof(pldm_msg_hdr) + PLDM_GET_BIOS_TABLE_MIN_RESP_BYTES +
                    attributeValueTable.size());
    responsePtr = reinterpret_cast<pldm_msg*>(response.data());
    encode_get_bios_table_resp(instanceID, PLDM_SUCCESS, nxtTransferHandle,
                               transferFlag, attributeValueTable.data(),
                               response.size(), responsePtr);

    return response;
}

Response getBIOSTable(const pldm_msg* request, size_t payloadLength)
{
    fs::create_directory(BIOS_TABLES_DIR);
    auto response = internal::buildBIOSTables(request, payloadLength,
                                              BIOS_JSONS_DIR, BIOS_TABLES_DIR);

    return response;
}

namespace bios
{

void registerHandlers()
{
    registerHandler(PLDM_BIOS, PLDM_GET_DATE_TIME, std::move(getDateTime));
    registerHandler(PLDM_BIOS, PLDM_GET_BIOS_TABLE, std::move(getBIOSTable));
}

namespace internal
{

Response buildBIOSTables(const pldm_msg* request, size_t payloadLength,
                         const char* biosJsonDir, const char* biosTablePath)
{
    Response response(sizeof(pldm_msg_hdr) + PLDM_GET_BIOS_TABLE_MIN_RESP_BYTES,
                      0);
    auto responsePtr = reinterpret_cast<pldm_msg*>(response.data());

    if (setupConfig(biosJsonDir) != 0)
    {
        encode_get_bios_table_resp(
            request->hdr.instance_id, PLDM_BIOS_TABLE_UNAVAILABLE,
            0 /* nxtTransferHandle */, PLDM_START_AND_END, nullptr,
            response.size(), responsePtr);
        return response;
    }

    uint32_t transferHandle{};
    uint8_t transferOpFlag{};
    uint8_t tableType{};

    auto rc = decode_get_bios_table_req(request, payloadLength, &transferHandle,
                                        &transferOpFlag, &tableType);
    if (rc == PLDM_SUCCESS)
    {
        BIOSTable BIOSStringTable(
            ((std::string(biosTablePath) + "/stringTable")).c_str());
        BIOSTable BIOSAttributeTable(
            ((std::string(biosTablePath) + "/attributeTable")).c_str());
        BIOSTable BIOSAttributeValueTable(
            ((std::string(biosTablePath) + "/attributeValueTable")).c_str());
        switch (tableType)
        {
            case PLDM_BIOS_STRING_TABLE:

                response = getBIOSStringTable(BIOSStringTable, transferHandle,
                                              transferOpFlag,
                                              request->hdr.instance_id);
                break;
            case PLDM_BIOS_ATTR_TABLE:

                if (BIOSStringTable.isEmpty())
                {
                    rc = PLDM_BIOS_TABLE_UNAVAILABLE;
                }
                else
                {
                    response = getBIOSAttributeTable(
                        BIOSAttributeTable, BIOSStringTable, transferHandle,
                        transferOpFlag, request->hdr.instance_id, biosJsonDir);
                }
                break;
            case PLDM_BIOS_ATTR_VAL_TABLE:
                if (BIOSAttributeTable.isEmpty())
                {
                    rc = PLDM_BIOS_TABLE_UNAVAILABLE;
                }
                else
                {
                    response = getBIOSAttributeValueTable(
                        BIOSAttributeValueTable, BIOSAttributeTable,
                        BIOSStringTable, transferHandle, transferOpFlag,
                        request->hdr.instance_id);
                }
                break;
            default:
                rc = PLDM_INVALID_BIOS_TABLE_TYPE;
                break;
        }
    }

    if (rc != PLDM_SUCCESS)
    {
        uint32_t nxtTransferHandle{};
        uint8_t transferFlag{};
        size_t respPayloadLength{};

        encode_get_bios_table_resp(request->hdr.instance_id, rc,
                                   nxtTransferHandle, transferFlag, nullptr,
                                   respPayloadLength, responsePtr);
    }

    return response;
}

} // end namespace internal
} // namespace bios

} // namespace responder
} // namespace pldm