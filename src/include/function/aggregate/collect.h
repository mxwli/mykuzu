#pragma once

#include "function/aggregate_function.h"
#include "processor/result/factorized_table.h"
#include "storage/storage_utils.h"

namespace kuzu {
namespace function {

struct CollectFunction {

    struct CollectState : public AggregateState {
        CollectState() : factorizedTable{nullptr} {}
        inline uint32_t getStateSize() const override { return sizeof(*this); }
        void moveResultToVector(common::ValueVector* outputVector, uint64_t pos) override {
            auto listEntry =
                common::ListVector::addList(outputVector, factorizedTable->getNumTuples());
            outputVector->setValue<common::list_entry_t>(pos, listEntry);
            auto outputDataVector = common::ListVector::getDataVector(outputVector);
            for (auto i = 0u; i < listEntry.size; i++) {
                outputDataVector->copyFromRowData(
                    listEntry.offset + i, factorizedTable->getTuple(i));
            }
            // CollectStates are stored in factorizedTable entries. When the factorizedTable is
            // destructed, the destructor of CollectStates won't be called. Therefore, we need to
            // manually deallocate the memory of CollectStates.
            factorizedTable.reset();
        }

        std::unique_ptr<processor::FactorizedTable> factorizedTable;
    };

    static std::unique_ptr<AggregateState> initialize() { return std::make_unique<CollectState>(); }

    static void updateAll(uint8_t* state_, common::ValueVector* input, uint64_t multiplicity,
        storage::MemoryManager* memoryManager) {
        KU_ASSERT(!input->state->isFlat());
        auto state = reinterpret_cast<CollectState*>(state_);
        if (input->hasNoNullsGuarantee()) {
            for (auto i = 0u; i < input->state->selVector->selectedSize; ++i) {
                auto pos = input->state->selVector->selectedPositions[i];
                updateSingleValue(state, input, pos, multiplicity, memoryManager);
            }
        } else {
            for (auto i = 0u; i < input->state->selVector->selectedSize; ++i) {
                auto pos = input->state->selVector->selectedPositions[i];
                if (!input->isNull(pos)) {
                    updateSingleValue(state, input, pos, multiplicity, memoryManager);
                }
            }
        }
    }

    static inline void updatePos(uint8_t* state_, common::ValueVector* input, uint64_t multiplicity,
        uint32_t pos, storage::MemoryManager* memoryManager) {
        auto state = reinterpret_cast<CollectState*>(state_);
        updateSingleValue(state, input, pos, multiplicity, memoryManager);
    }

    static void initCollectStateIfNecessary(
        CollectState* state, storage::MemoryManager* memoryManager, common::LogicalType& dataType) {
        if (state->factorizedTable == nullptr) {
            auto tableSchema = std::make_unique<processor::FactorizedTableSchema>();
            tableSchema->appendColumn(
                std::make_unique<processor::ColumnSchema>(false /* isUnflat */,
                    0 /* dataChunkPos */, storage::StorageUtils::getDataTypeSize(dataType)));
            state->factorizedTable =
                std::make_unique<processor::FactorizedTable>(memoryManager, std::move(tableSchema));
        }
    }

    static void updateSingleValue(CollectState* state, common::ValueVector* input, uint32_t pos,
        uint64_t multiplicity, storage::MemoryManager* memoryManager) {
        initCollectStateIfNecessary(state, memoryManager, input->dataType);
        for (auto i = 0u; i < multiplicity; ++i) {
            auto tuple = state->factorizedTable->appendEmptyTuple();
            state->isNull = false;
            input->copyToRowData(pos, tuple, state->factorizedTable->getInMemOverflowBuffer());
        }
    }

    static void combine(
        uint8_t* state_, uint8_t* otherState_, storage::MemoryManager* /*memoryManager*/) {
        auto otherState = reinterpret_cast<CollectState*>(otherState_);
        if (otherState->isNull) {
            return;
        }
        auto state = reinterpret_cast<CollectState*>(state_);
        if (state->isNull) {
            state->factorizedTable = std::move(otherState->factorizedTable);
            state->isNull = false;
        } else {
            state->factorizedTable->merge(*otherState->factorizedTable);
        }
        otherState->factorizedTable.reset();
    }

    static void finalize(uint8_t* /*state_*/) {}

    static std::unique_ptr<FunctionBindData> bindFunc(
        const binder::expression_vector& arguments, Function* definition) {
        KU_ASSERT(arguments.size() == 1);
        auto aggFuncDefinition = reinterpret_cast<AggregateFunction*>(definition);
        aggFuncDefinition->parameterTypeIDs[0] = arguments[0]->dataType.getLogicalTypeID();
        auto returnType = common::LogicalType::VAR_LIST(
            std::make_unique<common::LogicalType>(arguments[0]->dataType));
        return std::make_unique<FunctionBindData>(std::move(returnType));
    }
};

} // namespace function
} // namespace kuzu
