#include "catalog/catalog_content.h"

#include <fcntl.h>

#include "binder/ddl/bound_create_table_info.h"
#include "catalog/node_table_schema.h"
#include "catalog/rdf_graph_schema.h"
#include "catalog/rel_table_group_schema.h"
#include "catalog/rel_table_schema.h"
#include "common/cast.h"
#include "common/exception/catalog.h"
#include "common/exception/runtime.h"
#include "common/file_system/virtual_file_system.h"
#include "common/serializer/buffered_file.h"
#include "common/serializer/deserializer.h"
#include "common/serializer/serializer.h"
#include "common/string_format.h"
#include "common/string_utils.h"
#include "storage/storage_info.h"
#include "storage/storage_utils.h"

using namespace kuzu::binder;
using namespace kuzu::common;
using namespace kuzu::storage;

namespace kuzu {
namespace catalog {

CatalogContent::CatalogContent(common::VirtualFileSystem* vfs) : nextTableID{0}, vfs{vfs} {
    registerBuiltInFunctions();
}

CatalogContent::CatalogContent(const std::string& directory, VirtualFileSystem* vfs) : vfs{vfs} {
    readFromFile(directory, FileVersionType::ORIGINAL);
    registerBuiltInFunctions();
}

table_id_t CatalogContent::addNodeTableSchema(const BoundCreateTableInfo& info) {
    table_id_t tableID = assignNextTableID();
    auto extraInfo = ku_dynamic_cast<BoundExtraCreateTableInfo*, BoundExtraCreateNodeTableInfo*>(
        info.extraInfo.get());
    auto nodeTableSchema =
        std::make_unique<NodeTableSchema>(info.tableName, tableID, extraInfo->primaryKeyIdx);
    for (auto& propertyInfo : extraInfo->propertyInfos) {
        nodeTableSchema->addProperty(propertyInfo.name, propertyInfo.type.copy());
    }
    tableNameToIDMap.emplace(nodeTableSchema->tableName, tableID);
    tableSchemas.emplace(tableID, std::move(nodeTableSchema));
    return tableID;
}

table_id_t CatalogContent::addRelTableSchema(const BoundCreateTableInfo& info) {
    table_id_t tableID = assignNextTableID();
    auto extraInfo = ku_dynamic_cast<BoundExtraCreateTableInfo*, BoundExtraCreateRelTableInfo*>(
        info.extraInfo.get());
    auto srcNodeTableSchema =
        ku_dynamic_cast<TableSchema*, NodeTableSchema*>(getTableSchema(extraInfo->srcTableID));
    auto dstNodeTableSchema =
        ku_dynamic_cast<TableSchema*, NodeTableSchema*>(getTableSchema(extraInfo->dstTableID));
    srcNodeTableSchema->addFwdRelTableID(tableID);
    dstNodeTableSchema->addBwdRelTableID(tableID);
    auto relTableSchema =
        std::make_unique<RelTableSchema>(info.tableName, tableID, extraInfo->srcMultiplicity,
            extraInfo->dstMultiplicity, extraInfo->srcTableID, extraInfo->dstTableID);
    for (auto& propertyInfo : extraInfo->propertyInfos) {
        relTableSchema->addProperty(propertyInfo.name, propertyInfo.type.copy());
    }
    tableNameToIDMap.emplace(relTableSchema->tableName, tableID);
    tableSchemas.emplace(tableID, std::move(relTableSchema));
    return tableID;
}

table_id_t CatalogContent::addRelTableGroupSchema(const binder::BoundCreateTableInfo& info) {
    table_id_t relTableGroupID = assignNextTableID();
    auto extraInfo = (BoundExtraCreateRelTableGroupInfo*)info.extraInfo.get();
    std::vector<table_id_t> relTableIDs;
    relTableIDs.reserve(extraInfo->infos.size());
    for (auto& childInfo : extraInfo->infos) {
        relTableIDs.push_back(addRelTableSchema(childInfo));
    }
    auto relTableGroupName = info.tableName;
    auto relTableGroupSchema = std::make_unique<RelTableGroupSchema>(
        relTableGroupName, relTableGroupID, std::move(relTableIDs));
    tableNameToIDMap.emplace(relTableGroupName, relTableGroupID);
    tableSchemas.emplace(relTableGroupID, std::move(relTableGroupSchema));
    return relTableGroupID;
}

table_id_t CatalogContent::addRdfGraphSchema(const BoundCreateTableInfo& info) {
    table_id_t rdfGraphID = assignNextTableID();
    auto extraInfo = ku_dynamic_cast<BoundExtraCreateTableInfo*, BoundExtraCreateRdfGraphInfo*>(
        info.extraInfo.get());
    auto& resourceInfo = extraInfo->resourceInfo;
    auto& literalInfo = extraInfo->literalInfo;
    auto& resourceTripleInfo = extraInfo->resourceTripleInfo;
    auto& literalTripleInfo = extraInfo->literalTripleInfo;
    auto resourceTripleExtraInfo =
        ku_dynamic_cast<BoundExtraCreateTableInfo*, BoundExtraCreateRelTableInfo*>(
            resourceTripleInfo.extraInfo.get());
    auto literalTripleExtraInfo =
        ku_dynamic_cast<BoundExtraCreateTableInfo*, BoundExtraCreateRelTableInfo*>(
            literalTripleInfo.extraInfo.get());
    // Resource table
    auto resourceTableID = addNodeTableSchema(resourceInfo);
    // Literal table
    auto literalTableID = addNodeTableSchema(literalInfo);
    // Resource triple table
    resourceTripleExtraInfo->srcTableID = resourceTableID;
    resourceTripleExtraInfo->dstTableID = resourceTableID;
    auto resourceTripleTableID = addRelTableSchema(resourceTripleInfo);
    // Literal triple table
    literalTripleExtraInfo->srcTableID = resourceTableID;
    literalTripleExtraInfo->dstTableID = literalTableID;
    auto literalTripleTableID = addRelTableSchema(literalTripleInfo);
    // Rdf table schema
    auto rdfGraphName = info.tableName;
    auto rdfGraphSchema = std::make_unique<RdfGraphSchema>(rdfGraphName, rdfGraphID,
        resourceTableID, literalTableID, resourceTripleTableID, literalTripleTableID);
    tableNameToIDMap.emplace(rdfGraphName, rdfGraphID);
    tableSchemas.emplace(rdfGraphID, std::move(rdfGraphSchema));
    return rdfGraphID;
}

void CatalogContent::dropTableSchema(table_id_t tableID) {
    auto tableSchema = getTableSchema(tableID);
    switch (tableSchema->tableType) {
    case common::TableType::REL_GROUP: {
        auto relTableGroupSchema = ku_dynamic_cast<TableSchema*, RelTableGroupSchema*>(tableSchema);
        for (auto& relTableID : relTableGroupSchema->getRelTableIDs()) {
            dropTableSchema(relTableID);
        }
    } break;
    default:
        break;
    }
    tableNameToIDMap.erase(tableSchema->tableName);
    tableSchemas.erase(tableID);
}

void CatalogContent::renameTable(table_id_t tableID, const std::string& newName) {
    auto tableSchema = getTableSchema(tableID);
    tableNameToIDMap.erase(tableSchema->tableName);
    tableNameToIDMap.emplace(newName, tableID);
    tableSchema->renameTable(newName);
}

static void validateStorageVersion(storage_version_t savedStorageVersion) {
    auto storageVersion = StorageVersionInfo::getStorageVersion();
    if (savedStorageVersion != storageVersion) {
        // LCOV_EXCL_START
        throw RuntimeException(
            stringFormat("Trying to read a database file with a different version. "
                         "Database file version: {}, Current build storage version: {}",
                savedStorageVersion, storageVersion));
        // LCOV_EXCL_STOP
    }
}

static void validateMagicBytes(Deserializer& deserializer) {
    auto numMagicBytes = strlen(StorageVersionInfo::MAGIC_BYTES);
    uint8_t magicBytes[4];
    for (auto i = 0u; i < numMagicBytes; i++) {
        deserializer.deserializeValue<uint8_t>(magicBytes[i]);
    }
    if (memcmp(magicBytes, StorageVersionInfo::MAGIC_BYTES, numMagicBytes) != 0) {
        throw RuntimeException(
            "This is not a valid Kuzu database directory for the current version of Kuzu.");
    }
}

static void writeMagicBytes(Serializer& serializer) {
    auto numMagicBytes = strlen(StorageVersionInfo::MAGIC_BYTES);
    for (auto i = 0u; i < numMagicBytes; i++) {
        serializer.serializeValue<uint8_t>(StorageVersionInfo::MAGIC_BYTES[i]);
    }
}

void CatalogContent::saveToFile(const std::string& directory, FileVersionType dbFileType) {
    auto catalogPath = StorageUtils::getCatalogFilePath(vfs, directory, dbFileType);
    Serializer serializer(
        std::make_unique<BufferedFileWriter>(vfs->openFile(catalogPath, O_WRONLY | O_CREAT)));
    writeMagicBytes(serializer);
    serializer.serializeValue(StorageVersionInfo::getStorageVersion());
    serializer.serializeValue((uint64_t)tableSchemas.size());
    for (auto& [tableID, tableSchema] : tableSchemas) {
        serializer.serializeValue(tableID);
        tableSchema->serialize(serializer);
    }
    serializer.serializeValue(nextTableID);
    serializer.serializeUnorderedMap(macros);
}

void CatalogContent::readFromFile(const std::string& directory, FileVersionType dbFileType) {
    auto catalogPath = StorageUtils::getCatalogFilePath(vfs, directory, dbFileType);
    Deserializer deserializer(
        std::make_unique<BufferedFileReader>(vfs->openFile(catalogPath, O_RDONLY)));
    validateMagicBytes(deserializer);
    storage_version_t savedStorageVersion;
    deserializer.deserializeValue(savedStorageVersion);
    validateStorageVersion(savedStorageVersion);
    uint64_t numTables;
    deserializer.deserializeValue(numTables);
    table_id_t tableID;
    for (auto i = 0u; i < numTables; i++) {
        deserializer.deserializeValue(tableID);
        tableSchemas[tableID] = TableSchema::deserialize(deserializer);
    }
    for (auto& [tableID_, tableSchema] : tableSchemas) {
        tableNameToIDMap[tableSchema->tableName] = tableID_;
    }
    deserializer.deserializeValue(nextTableID);
    deserializer.deserializeUnorderedMap(macros);
}

ExpressionType CatalogContent::getFunctionType(const std::string& name) const {
    auto normalizedName = StringUtils::getUpper(name);
    if (macros.contains(normalizedName)) {
        return ExpressionType::MACRO;
    }
    auto functionType = builtInFunctions->getFunctionType(name);
    switch (functionType) {
    case function::FunctionType::SCALAR:
        return ExpressionType::FUNCTION;
    case function::FunctionType::AGGREGATE:
        return ExpressionType::AGGREGATE_FUNCTION;
    default:
        KU_UNREACHABLE;
    }
}

void CatalogContent::addFunction(std::string name, function::function_set definitions) {
    StringUtils::toUpper(name);
    builtInFunctions->addFunction(std::move(name), std::move(definitions));
}

void CatalogContent::addScalarMacroFunction(
    std::string name, std::unique_ptr<function::ScalarMacroFunction> macro) {
    StringUtils::toUpper(name);
    macros.emplace(std::move(name), std::move(macro));
}

std::unique_ptr<CatalogContent> CatalogContent::copy() const {
    std::unordered_map<table_id_t, std::unique_ptr<TableSchema>> tableSchemasToCopy;
    for (auto& [tableID, tableSchema] : tableSchemas) {
        tableSchemasToCopy.emplace(tableID, tableSchema->copy());
    }
    std::unordered_map<std::string, std::unique_ptr<function::ScalarMacroFunction>> macrosToCopy;
    for (auto& macro : macros) {
        macrosToCopy.emplace(macro.first, macro.second->copy());
    }
    return std::make_unique<CatalogContent>(std::move(tableSchemasToCopy), tableNameToIDMap,
        nextTableID, builtInFunctions->copy(), std::move(macrosToCopy), vfs);
}

void CatalogContent::registerBuiltInFunctions() {
    builtInFunctions = std::make_unique<function::BuiltInFunctions>();
}

bool CatalogContent::containsTable(const std::string& tableName) const {
    return tableNameToIDMap.contains(tableName);
}

bool CatalogContent::containsTable(common::TableType tableType) const {
    for (auto& [_, schema] : tableSchemas) {
        if (schema->tableType == tableType) {
            return true;
        }
    }
    return false;
}

std::string CatalogContent::getTableName(table_id_t tableID) const {
    return getTableSchema(tableID)->tableName;
}

TableSchema* CatalogContent::getTableSchema(table_id_t tableID) const {
    KU_ASSERT(tableSchemas.contains(tableID));
    return tableSchemas.at(tableID).get();
}

std::vector<TableSchema*> CatalogContent::getTableSchemas(TableType tableType) const {
    std::vector<TableSchema*> result;
    for (auto& [id, schema] : tableSchemas) {
        if (schema->getTableType() == tableType) {
            result.push_back(schema.get());
        }
    }
    return result;
}

table_id_t CatalogContent::getTableID(const std::string& tableName) const {
    if (!tableNameToIDMap.contains(tableName)) {
        throw CatalogException(stringFormat("Cannot find a table with name {}.", tableName));
    }
    return tableNameToIDMap.at(tableName);
}

std::vector<table_id_t> CatalogContent::getTableIDs(TableType tableType) const {
    std::vector<table_id_t> tableIDs;
    for (auto& [id, schema] : tableSchemas) {
        if (schema->getTableType() == tableType) {
            tableIDs.push_back(id);
        }
    }
    return tableIDs;
}

} // namespace catalog
} // namespace kuzu
