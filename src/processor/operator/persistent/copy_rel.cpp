#include "processor/operator/persistent/copy_rel.h"

#include "common/exception/copy.h"
#include "common/exception/message.h"
#include "common/string_format.h"
#include "processor/result/factorized_table.h"
#include "storage/store/rel_table.h"

using namespace kuzu::common;
using namespace kuzu::storage;

namespace kuzu {
namespace processor {

CopyRelSharedState::CopyRelSharedState(table_id_t tableID, RelTable* table,
    std::vector<std::unique_ptr<common::LogicalType>> columnTypes, RelsStoreStats* relsStatistics,
    MemoryManager* memoryManager)
    : tableID{tableID}, table{table}, columnTypes{std::move(columnTypes)},
      relsStatistics{relsStatistics}, numRows{0} {
    auto ftTableSchema = std::make_unique<FactorizedTableSchema>();
    ftTableSchema->appendColumn(
        std::make_unique<ColumnSchema>(false /* flat */, 0 /* dataChunkPos */,
            LogicalTypeUtils::getRowLayoutSize(LogicalType{LogicalTypeID::STRING})));
    fTable = std::make_shared<FactorizedTable>(memoryManager, std::move(ftTableSchema));
}

// NOLINTNEXTLINE(readability-make-member-function-const): Semantically non-const.
void CopyRelSharedState::logCopyRelWALRecord(WAL* wal) {
    wal->logCopyTableRecord(tableID, TableType::REL);
    wal->flushAllPages();
}

void CopyRel::initLocalStateInternal(ResultSet* /*resultSet_*/, ExecutionContext* /*context*/) {
    localState = std::make_unique<CopyRelLocalState>();
    localState->nodeGroup = NodeGroupFactory::createNodeGroup(
        info->dataFormat, sharedState->columnTypes, info->compressionEnabled);
}

void CopyRel::initGlobalStateInternal(ExecutionContext* /*context*/) {
    if (!isCopyAllowed()) {
        throw CopyException(ExceptionMessage::notAllowCopyOnNonEmptyTableException());
    }
    sharedState->logCopyRelWALRecord(info->wal);
}

void CopyRel::executeInternal(ExecutionContext* /*context*/) {
    while (true) {
        localState->currentPartition =
            partitionerSharedState->getNextPartition(info->partitioningIdx);
        if (localState->currentPartition == INVALID_PARTITION_IDX) {
            break;
        }
        // Read the whole partition, and set to node group.
        auto partitioningBuffer = partitionerSharedState->getPartitionBuffer(
            info->partitioningIdx, localState->currentPartition);
        auto startOffset = StorageUtils::getStartOffsetOfNodeGroup(localState->currentPartition);
        auto offsetVectorIdx = info->dataDirection == RelDataDirection::FWD ? 0 : 1;
        for (auto dataChunk : partitioningBuffer->getChunks()) {
            setOffsetToWithinNodeGroup(
                dataChunk->getValueVector(offsetVectorIdx).get(), startOffset);
        }
        // Calculate num of source nodes in this node group.
        // This will be used to set the num of values of the node group.
        auto numNodes = std::min(StorageConstants::NODE_GROUP_SIZE,
            partitionerSharedState->maxNodeOffsets[info->partitioningIdx] - startOffset + 1);
        if (info->dataFormat == ColumnDataFormat::CSR) {
            prepareCSRNodeGroup(partitioningBuffer, offsetVectorIdx, numNodes);
        } else {
            localState->nodeGroup->setAllNull();
            localState->nodeGroup->getColumnChunk(0)->setNumValues(numNodes);
        }
        for (auto dataChunk : partitioningBuffer->getChunks()) {
            localState->nodeGroup->write(dataChunk, offsetVectorIdx);
        }
        localState->nodeGroup->finalize(localState->currentPartition);
        // Flush node group to table.
        sharedState->table->append(localState->nodeGroup.get(), info->dataDirection);
        sharedState->incrementNumRows(localState->nodeGroup->getNumRows());
        localState->nodeGroup->resetToEmpty();
    }
}

void CopyRel::prepareCSRNodeGroup(
    DataChunkCollection* partition, vector_idx_t offsetVectorIdx, offset_t numNodes) {
    auto csrNodeGroup = ku_dynamic_cast<NodeGroup*, CSRNodeGroup*>(localState->nodeGroup.get());
    auto csrOffsetChunk = csrNodeGroup->getCSROffsetChunk();
    csrOffsetChunk->setNumValues(numNodes);
    csrNodeGroup->getCSRLengthChunk()->setNumValues(numNodes);
    populateCSROffsetsAndLengths(csrNodeGroup, numNodes, partition, offsetVectorIdx);
    // Resize csr data column chunks.
    offset_t numRels = 0;
    for (auto dataChunk : partition->getChunks()) {
        numRels += dataChunk->getValueVector(offsetVectorIdx).get()->state->selVector->selectedSize;
    }
    localState->nodeGroup->resizeChunks(numRels);
    for (auto dataChunk : partition->getChunks()) {
        auto offsetVector = dataChunk->getValueVector(offsetVectorIdx).get();
        setOffsetFromCSROffsets(offsetVector, (offset_t*)csrOffsetChunk->getData());
    }
}

void CopyRel::populateCSROffsetsAndLengths(CSRNodeGroup* csrNodeGroup, offset_t numNodes,
    common::DataChunkCollection* partition, common::vector_idx_t offsetVectorIdx) {
    auto csrOffsetChunk = csrNodeGroup->getCSROffsetChunk();
    csrOffsetChunk->setNumValues(numNodes);
    auto csrLengthChunk = csrNodeGroup->getCSRLengthChunk();
    csrLengthChunk->setNumValues(numNodes);
    auto csrOffsets = (offset_t*)csrOffsetChunk->getData();
    auto csrLengths = (length_t*)csrLengthChunk->getData();
    std::fill(csrOffsets, csrOffsets + csrOffsetChunk->getCapacity(), 0);
    std::fill(csrLengths, csrLengths + csrLengthChunk->getCapacity(), 0);
    // Calculate length for each node. Store the num of tuples of node i at csrOffsets[i].
    for (auto chunk : partition->getChunks()) {
        auto offsetVector = chunk->getValueVector(offsetVectorIdx);
        for (auto i = 0u; i < offsetVector->state->selVector->selectedSize; i++) {
            auto pos = offsetVector->state->selVector->selectedPositions[i];
            auto nodeOffset = offsetVector->getValue<offset_t>(pos);
            csrLengths[nodeOffset]++;
        }
    }
    // Calculate starting offset of each node.
    for (auto i = 1u; i < csrOffsetChunk->getCapacity(); i++) {
        csrOffsets[i] = csrOffsets[i - 1] + csrLengths[i - 1];
    }
}

void CopyRel::setOffsetToWithinNodeGroup(ValueVector* vector, offset_t startOffset) {
    KU_ASSERT(vector->dataType.getPhysicalType() == PhysicalTypeID::INT64 &&
              vector->state->selVector->isUnfiltered());
    auto offsets = (offset_t*)vector->getData();
    for (auto i = 0u; i < vector->state->selVector->selectedSize; i++) {
        offsets[i] -= startOffset;
    }
}

void CopyRel::setOffsetFromCSROffsets(ValueVector* offsetVector, offset_t* csrOffsets) {
    KU_ASSERT(offsetVector->dataType.getPhysicalType() == PhysicalTypeID::INT64 &&
              offsetVector->state->selVector->isUnfiltered());
    for (auto i = 0u; i < offsetVector->state->selVector->selectedSize; i++) {
        auto nodeOffset = offsetVector->getValue<offset_t>(i);
        offsetVector->setValue(i, csrOffsets[nodeOffset]);
        csrOffsets[nodeOffset]++;
    }
}

void CopyRel::finalize(ExecutionContext* context) {
    if (info->partitioningIdx == partitionerSharedState->partitioningBuffers.size() - 1) {
        sharedState->updateRelsStatistics();
        auto outputMsg = stringFormat("{} number of tuples has been copied to table {}.",
            sharedState->numRows.load(), info->schema->tableName);
        FactorizedTableUtils::appendStringToTable(
            sharedState->fTable.get(), outputMsg, context->clientContext->getMemoryManager());
    }
    sharedState->numRows.store(0);
    partitionerSharedState->resetState();
    partitionerSharedState->partitioningBuffers[info->partitioningIdx].reset();
}

} // namespace processor
} // namespace kuzu
