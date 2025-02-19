#include "binder/binder.h"
#include "binder/copy/bound_copy_from.h"
#include "binder/copy/bound_copy_to.h"
#include "catalog/node_table_schema.h"
#include "catalog/rel_table_schema.h"
#include "common/enums/table_type.h"
#include "common/exception/binder.h"
#include "common/exception/message.h"
#include "common/string_format.h"
#include "function/table_functions/bind_input.h"
#include "main/client_context.h"
#include "parser/copy.h"

using namespace kuzu::binder;
using namespace kuzu::catalog;
using namespace kuzu::common;
using namespace kuzu::parser;

namespace kuzu {
namespace binder {

std::unique_ptr<BoundStatement> Binder::bindCopyToClause(const Statement& statement) {
    auto& copyToStatement = ku_dynamic_cast<const Statement&, const CopyTo&>(statement);
    auto boundFilePath = copyToStatement.getFilePath();
    auto fileType = bindFileType(boundFilePath);
    std::vector<std::string> columnNames;
    std::vector<LogicalType> columnTypes;
    auto parsedQuery =
        ku_dynamic_cast<const Statement*, const RegularQuery*>(copyToStatement.getStatement());
    auto query = bindQuery(*parsedQuery);
    auto columns = query->getStatementResult()->getColumns();
    for (auto& column : columns) {
        auto columnName = column->hasAlias() ? column->getAlias() : column->toString();
        columnNames.push_back(columnName);
        columnTypes.push_back(column->getDataType());
    }
    if (fileType != FileType::CSV && fileType != FileType::PARQUET) {
        throw BinderException(ExceptionMessage::validateCopyToCSVParquetExtensionsException());
    }
    if (fileType != FileType::CSV && copyToStatement.getParsingOptionsRef().size() != 0) {
        throw BinderException{"Only copy to csv can have options."};
    }
    auto csvConfig =
        CSVReaderConfig::construct(bindParsingOptions(copyToStatement.getParsingOptionsRef()));
    return std::make_unique<BoundCopyTo>(boundFilePath, fileType, std::move(columnNames),
        std::move(columnTypes), std::move(query), csvConfig.option.copy());
}

// As a temporary constraint, we require npy files loaded with COPY FROM BY COLUMN keyword.
// And csv and parquet files loaded with COPY FROM keyword.
static void validateByColumnKeyword(FileType fileType, bool byColumn) {
    if (fileType == FileType::NPY && !byColumn) {
        throw BinderException(ExceptionMessage::validateCopyNPYByColumnException());
    }
    if (fileType != FileType::NPY && byColumn) {
        throw BinderException(ExceptionMessage::validateCopyCSVParquetByColumnException());
    }
}

static void validateCopyNpyNotForRelTables(TableSchema* schema) {
    if (schema->tableType == TableType::REL) {
        throw BinderException(
            ExceptionMessage::validateCopyNpyNotForRelTablesException(schema->tableName));
    }
}

std::unique_ptr<BoundStatement> Binder::bindCopyFromClause(const Statement& statement) {
    auto& copyStatement = ku_dynamic_cast<const Statement&, const CopyFrom&>(statement);
    auto tableName = copyStatement.getTableName();
    validateTableExist(tableName);
    // Bind to table schema.
    auto tableID = catalog.getTableID(clientContext->getTx(), tableName);
    auto tableSchema = catalog.getTableSchema(clientContext->getTx(), tableID);
    switch (tableSchema->tableType) {
    case TableType::REL_GROUP: {
        throw BinderException(stringFormat("Cannot copy into {} table with type {}.", tableName,
            TableTypeUtils::toString(tableSchema->tableType)));
    }
    default:
        break;
    }
    auto filePaths = bindFilePaths(copyStatement.getFilePaths());
    auto fileType = bindFileType(filePaths);
    auto readerConfig = std::make_unique<ReaderConfig>(fileType, std::move(filePaths));
    readerConfig->options = bindParsingOptions(copyStatement.getParsingOptionsRef());
    validateByColumnKeyword(readerConfig->fileType, copyStatement.byColumn());
    if (readerConfig->fileType == FileType::NPY) {
        validateCopyNpyNotForRelTables(tableSchema);
    }
    switch (tableSchema->tableType) {
    case TableType::NODE:
        return bindCopyNodeFrom(statement, std::move(readerConfig), tableSchema);
    case TableType::REL:
        return bindCopyRelFrom(statement, std::move(readerConfig), tableSchema);
    case TableType::RDF:
        return bindCopyRdfFrom(statement, std::move(readerConfig), tableSchema);
        // LCOV_EXCL_START
    default: {
        KU_UNREACHABLE;
    }
        // LCOV_EXCL_STOP
    }
}

std::unique_ptr<BoundStatement> Binder::bindCopyNodeFrom(const Statement& statement,
    std::unique_ptr<common::ReaderConfig> config, TableSchema* tableSchema) {
    auto& copyStatement = ku_dynamic_cast<const Statement&, const CopyFrom&>(statement);
    auto func = getScanFunction(config->fileType, *config);
    // For table with SERIAL columns, we need to read in serial from files.
    auto containsSerial = tableSchema->containPropertyType(*LogicalType::SERIAL());
    std::vector<std::string> expectedColumnNames;
    std::vector<common::LogicalType> expectedColumnTypes;
    bindExpectedNodeColumns(
        tableSchema, copyStatement.getColumnNames(), expectedColumnNames, expectedColumnTypes);
    auto bindInput = std::make_unique<function::ScanTableFuncBindInput>(config->copy(),
        std::move(expectedColumnNames), std::move(expectedColumnTypes), clientContext);
    auto bindData =
        func->bindFunc(clientContext, bindInput.get(), (Catalog*)&catalog, storageManager);
    expression_vector columns;
    for (auto i = 0u; i < bindData->columnTypes.size(); i++) {
        columns.push_back(createVariable(bindData->columnNames[i], bindData->columnTypes[i]));
    }
    auto offset = expressionBinder.createVariableExpression(
        LogicalType(LogicalTypeID::INT64), std::string(InternalKeyword::ANONYMOUS));
    auto boundFileScanInfo =
        std::make_unique<BoundFileScanInfo>(func, std::move(bindData), columns, std::move(offset));
    auto boundCopyFromInfo = BoundCopyFromInfo(
        tableSchema, std::move(boundFileScanInfo), containsSerial, nullptr /* extraInfo */);
    return std::make_unique<BoundCopyFrom>(std::move(boundCopyFromInfo));
}

std::unique_ptr<BoundStatement> Binder::bindCopyRelFrom(const parser::Statement& statement,
    std::unique_ptr<common::ReaderConfig> config, TableSchema* tableSchema) {
    auto& copyStatement = ku_dynamic_cast<const Statement&, const CopyFrom&>(statement);
    auto func = getScanFunction(config->fileType, *config);
    // For table with SERIAL columns, we need to read in serial from files.
    auto containsSerial = tableSchema->containPropertyType(*LogicalType::SERIAL());
    KU_ASSERT(containsSerial == false);
    std::vector<std::string> expectedColumnNames;
    std::vector<common::LogicalType> expectedColumnTypes;
    bindExpectedRelColumns(
        tableSchema, copyStatement.getColumnNames(), expectedColumnNames, expectedColumnTypes);
    auto bindInput = std::make_unique<function::ScanTableFuncBindInput>(std::move(*config),
        std::move(expectedColumnNames), std::move(expectedColumnTypes), clientContext);
    auto bindData =
        func->bindFunc(clientContext, bindInput.get(), (Catalog*)&catalog, storageManager);
    expression_vector columns;
    for (auto i = 0u; i < bindData->columnTypes.size(); i++) {
        columns.push_back(createVariable(bindData->columnNames[i], bindData->columnTypes[i]));
    }
    auto offset = expressionBinder.createVariableExpression(
        *LogicalType::INT64(), std::string(InternalKeyword::ROW_OFFSET));
    auto boundFileScanInfo =
        std::make_unique<BoundFileScanInfo>(func, std::move(bindData), columns, offset);
    auto relSchema = ku_dynamic_cast<TableSchema*, RelTableSchema*>(tableSchema);
    auto srcTableID = relSchema->getSrcTableID();
    auto dstTableID = relSchema->getDstTableID();
    auto srcSchema = ku_dynamic_cast<TableSchema*, NodeTableSchema*>(
        catalog.getTableSchema(clientContext->getTx(), srcTableID));
    auto dstSchema = ku_dynamic_cast<TableSchema*, NodeTableSchema*>(
        catalog.getTableSchema(clientContext->getTx(), dstTableID));
    auto srcKey = columns[0];
    auto dstKey = columns[1];
    expression_vector propertyColumns;
    for (auto i = 2u; i < columns.size(); ++i) {
        propertyColumns.push_back(columns[i]);
    }
    auto srcOffset = createVariable(InternalKeyword::SRC_OFFSET, LogicalTypeID::INT64);
    auto dstOffset = createVariable(InternalKeyword::DST_OFFSET, LogicalTypeID::INT64);
    auto srcPkType = srcSchema->getPrimaryKey()->getDataType();
    auto dstPkType = dstSchema->getPrimaryKey()->getDataType();
    auto srcLookUpInfo = IndexLookupInfo(srcTableID, srcOffset, srcKey, *srcPkType);
    auto dstLookUpInfo = IndexLookupInfo(dstTableID, dstOffset, dstKey, *dstPkType);
    auto extraCopyRelInfo = std::make_unique<ExtraBoundCopyRelInfo>();
    extraCopyRelInfo->fromOffset = srcOffset;
    extraCopyRelInfo->toOffset = dstOffset;
    extraCopyRelInfo->propertyColumns = std::move(propertyColumns);
    extraCopyRelInfo->infos.push_back(std::move(srcLookUpInfo));
    extraCopyRelInfo->infos.push_back(std::move(dstLookUpInfo));
    auto boundCopyFromInfo = BoundCopyFromInfo(
        tableSchema, std::move(boundFileScanInfo), containsSerial, std::move(extraCopyRelInfo));
    return std::make_unique<BoundCopyFrom>(std::move(boundCopyFromInfo));
}

static bool skipPropertyInFile(const Property& property) {
    return property.getDataType()->getLogicalTypeID() == LogicalTypeID::SERIAL ||
           TableSchema::isReservedPropertyName(property.getName());
}

static void bindExpectedColumns(TableSchema* tableSchema,
    const std::vector<std::string>& inputColumnNames, std::vector<std::string>& columnNames,
    std::vector<common::LogicalType>& columnTypes) {
    if (!inputColumnNames.empty()) {
        std::unordered_set<std::string> inputColumnNamesSet;
        for (auto& columName : inputColumnNames) {
            if (inputColumnNamesSet.contains(columName)) {
                throw BinderException(
                    stringFormat("Detect duplicate column name {} during COPY.", columName));
            }
            inputColumnNamesSet.insert(columName);
        }
        // Search column data type for each input column.
        for (auto& columnName : inputColumnNames) {
            if (!tableSchema->containProperty(columnName)) {
                throw BinderException(stringFormat(
                    "Table {} does not contain column {}.", tableSchema->tableName, columnName));
            }
            auto propertyID = tableSchema->getPropertyID(columnName);
            auto property = tableSchema->getProperty(propertyID);
            if (skipPropertyInFile(*property)) {
                continue;
            }
            columnNames.push_back(columnName);
            columnTypes.push_back(*property->getDataType());
        }
    } else {
        // No column specified. Fall back to schema columns.
        for (auto& property : tableSchema->properties) {
            if (skipPropertyInFile(property)) {
                continue;
            }
            columnNames.push_back(property.getName());
            columnTypes.push_back(*property.getDataType());
        }
    }
}

void Binder::bindExpectedNodeColumns(catalog::TableSchema* tableSchema,
    const std::vector<std::string>& inputColumnNames, std::vector<std::string>& columnNames,
    std::vector<common::LogicalType>& columnTypes) {
    KU_ASSERT(columnNames.empty() && columnTypes.empty());
    bindExpectedColumns(tableSchema, inputColumnNames, columnNames, columnTypes);
}

void Binder::bindExpectedRelColumns(TableSchema* tableSchema,
    const std::vector<std::string>& inputColumnNames, std::vector<std::string>& columnNames,
    std::vector<common::LogicalType>& columnTypes) {
    KU_ASSERT(columnNames.empty() && columnTypes.empty());
    auto relTableSchema = ku_dynamic_cast<TableSchema*, RelTableSchema*>(tableSchema);
    auto srcTable = ku_dynamic_cast<TableSchema*, NodeTableSchema*>(
        catalog.getTableSchema(clientContext->getTx(), relTableSchema->getSrcTableID()));
    auto dstTable = ku_dynamic_cast<TableSchema*, NodeTableSchema*>(
        catalog.getTableSchema(clientContext->getTx(), relTableSchema->getDstTableID()));
    columnNames.push_back("from");
    columnNames.push_back("to");
    auto srcPKColumnType = *srcTable->getPrimaryKey()->getDataType();
    if (srcPKColumnType.getLogicalTypeID() == LogicalTypeID::SERIAL) {
        srcPKColumnType = *LogicalType::INT64();
    }
    auto dstPKColumnType = *dstTable->getPrimaryKey()->getDataType();
    if (dstPKColumnType.getLogicalTypeID() == LogicalTypeID::SERIAL) {
        dstPKColumnType = *LogicalType::INT64();
    }
    columnTypes.push_back(std::move(srcPKColumnType));
    columnTypes.push_back(std::move(dstPKColumnType));
    bindExpectedColumns(tableSchema, inputColumnNames, columnNames, columnTypes);
}

} // namespace binder
} // namespace kuzu
