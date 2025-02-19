#pragma once

#include "binder/bound_statement.h"
#include "bound_file_scan_info.h"
#include "catalog/table_schema.h"
#include "index_look_up_info.h"

namespace kuzu {
namespace binder {

struct ExtraBoundCopyFromInfo {
    virtual ~ExtraBoundCopyFromInfo() = default;
    virtual std::unique_ptr<ExtraBoundCopyFromInfo> copy() const = 0;
};

struct BoundCopyFromInfo {
    catalog::TableSchema* tableSchema;
    std::unique_ptr<BoundFileScanInfo> fileScanInfo;
    bool containsSerial;
    std::unique_ptr<ExtraBoundCopyFromInfo> extraInfo;

    BoundCopyFromInfo(catalog::TableSchema* tableSchema,
        std::unique_ptr<BoundFileScanInfo> fileScanInfo, bool containsSerial,
        std::unique_ptr<ExtraBoundCopyFromInfo> extraInfo)
        : tableSchema{tableSchema}, fileScanInfo{std::move(fileScanInfo)},
          containsSerial{containsSerial}, extraInfo{std::move(extraInfo)} {}
    EXPLICIT_COPY_DEFAULT_MOVE(BoundCopyFromInfo);

private:
    BoundCopyFromInfo(const BoundCopyFromInfo& other)
        : tableSchema{other.tableSchema}, containsSerial{other.containsSerial} {
        if (other.fileScanInfo) {
            fileScanInfo = std::make_unique<BoundFileScanInfo>(other.fileScanInfo->copy());
        }
        if (other.extraInfo) {
            extraInfo = other.extraInfo->copy();
        }
    }
};

struct ExtraBoundCopyRelInfo final : public ExtraBoundCopyFromInfo {
    std::vector<IndexLookupInfo> infos;
    std::shared_ptr<Expression> fromOffset;
    std::shared_ptr<Expression> toOffset;
    expression_vector propertyColumns;

    ExtraBoundCopyRelInfo() = default;
    ExtraBoundCopyRelInfo(const ExtraBoundCopyRelInfo& other)
        : infos{copyVector(other.infos)}, fromOffset{other.fromOffset}, toOffset{other.toOffset},
          propertyColumns{other.propertyColumns} {}

    inline std::unique_ptr<ExtraBoundCopyFromInfo> copy() const override {
        return std::make_unique<ExtraBoundCopyRelInfo>(*this);
    }
};

struct ExtraBoundCopyRdfInfo final : public ExtraBoundCopyFromInfo {
    BoundCopyFromInfo rInfo;
    BoundCopyFromInfo lInfo;
    BoundCopyFromInfo rrrInfo;
    BoundCopyFromInfo rrlInfo;

    ExtraBoundCopyRdfInfo(BoundCopyFromInfo rInfo, BoundCopyFromInfo lInfo,
        BoundCopyFromInfo rrrInfo, BoundCopyFromInfo rrlInfo)
        : rInfo{std::move(rInfo)}, lInfo{std::move(lInfo)}, rrrInfo{std::move(rrrInfo)},
          rrlInfo{std::move(rrlInfo)} {}
    ExtraBoundCopyRdfInfo(const ExtraBoundCopyRdfInfo& other)
        : rInfo{other.rInfo.copy()}, lInfo{other.lInfo.copy()}, rrrInfo{other.rrrInfo.copy()},
          rrlInfo{other.rrlInfo.copy()} {}

    inline std::unique_ptr<ExtraBoundCopyFromInfo> copy() const override {
        return std::make_unique<ExtraBoundCopyRdfInfo>(*this);
    }
};

class BoundCopyFrom : public BoundStatement {
public:
    explicit BoundCopyFrom(BoundCopyFromInfo info)
        : BoundStatement{common::StatementType::COPY_FROM,
              BoundStatementResult::createSingleStringColumnResult()},
          info{std::move(info)} {}

    inline const BoundCopyFromInfo* getInfo() const { return &info; }

private:
    BoundCopyFromInfo info;
};

} // namespace binder
} // namespace kuzu
