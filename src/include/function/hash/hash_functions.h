#pragma once

#include <cstdint>
#include <functional>
#include <unordered_set>

#include "common/exception/runtime.h"
#include "common/type_utils.h"
#include "common/types/int128_t.h"
#include "common/types/interval_t.h"
#include "common/types/ku_string.h"
#include "common/vector/value_vector.h"

namespace kuzu {
namespace function {

template<typename T>
concept HashableTypes = (std::integral<T> || std::floating_point<T> ||
                         std::is_same_v<T, common::int128_t> ||
                         std::is_same_v<T, common::list_entry_t> ||
                         std::is_same_v<T, common::ku_string_t>);

constexpr const uint64_t NULL_HASH = UINT64_MAX;

inline common::hash_t murmurhash64(uint64_t x) {
    // taken from https://nullprogram.com/blog/2018/07/31.
    x ^= x >> 32;
    x *= 0xd6e8feb86659fd93U;
    x ^= x >> 32;
    x *= 0xd6e8feb86659fd93U;
    x ^= x >> 32;
    return x;
}

inline common::hash_t combineHashScalar(common::hash_t a, common::hash_t b) {
    return (a * UINT64_C(0xbf58476d1ce4e5b9)) ^ b;
}

struct Hash {
    template<class T>
    static inline void operation(const T& /*key*/, common::hash_t& /*result*/,
        common::ValueVector* /*keyVector*/ = nullptr) {
        // LCOV_EXCL_START
        throw common::RuntimeException(
            "Hash type: " + std::string(typeid(T).name()) + " is not supported.");
        // LCOV_EXCL_STOP
    }

    template<class T>
    static inline void operation(const T& key, bool isNull, common::hash_t& result,
        common::ValueVector* /*keyVector*/ = nullptr) {
        if (isNull) {
            result = NULL_HASH;
            return;
        }
        operation(key, result);
    }
};

struct CombineHash {
    static inline void operation(
        common::hash_t& left, common::hash_t& right, common::hash_t& result) {
        result = combineHashScalar(left, right);
    }
};

template<>
inline void Hash::operation(
    const common::internalID_t& key, common::hash_t& result, common::ValueVector* /*keyVector*/) {
    result = murmurhash64(key.offset) ^ murmurhash64(key.tableID);
}

template<>
inline void Hash::operation(
    const bool& key, common::hash_t& result, common::ValueVector* /*keyVector*/) {
    result = murmurhash64(key);
}

template<>
inline void Hash::operation(
    const uint8_t& key, common::hash_t& result, common::ValueVector* /*keyVector*/) {
    result = murmurhash64(key);
}

template<>
inline void Hash::operation(
    const uint16_t& key, common::hash_t& result, common::ValueVector* /*keyVector*/) {
    result = murmurhash64(key);
}

template<>
inline void Hash::operation(
    const uint32_t& key, common::hash_t& result, common::ValueVector* /*keyVector*/) {
    result = murmurhash64(key);
}

template<>
inline void Hash::operation(
    const uint64_t& key, common::hash_t& result, common::ValueVector* /*keyVector*/) {
    result = murmurhash64(key);
}

template<>
inline void Hash::operation(
    const int64_t& key, common::hash_t& result, common::ValueVector* /*keyVector*/) {
    result = murmurhash64(key);
}

template<>
inline void Hash::operation(
    const int32_t& key, common::hash_t& result, common::ValueVector* /*keyVector*/) {
    result = murmurhash64(key);
}

template<>
inline void Hash::operation(
    const int16_t& key, common::hash_t& result, common::ValueVector* /*keyVector*/) {
    result = murmurhash64(key);
}

template<>
inline void Hash::operation(
    const int8_t& key, common::hash_t& result, common::ValueVector* /*keyVector*/) {
    result = murmurhash64(key);
}

template<>
inline void Hash::operation(
    const common::int128_t& key, common::hash_t& result, common::ValueVector* /*keyVector*/) {
    result = murmurhash64(key.low) ^ murmurhash64(key.high);
}

template<>
inline void Hash::operation(
    const double& key, common::hash_t& result, common::ValueVector* /*keyVector*/) {
    result = std::hash<double>()(key);
}

template<>
inline void Hash::operation(
    const float& key, common::hash_t& result, common::ValueVector* /*keyVector*/) {
    result = std::hash<float>()(key);
}

template<>
inline void Hash::operation(
    const std::string& key, common::hash_t& result, common::ValueVector* /*keyVector*/) {
    result = std::hash<std::string>()(key);
}

template<>
inline void Hash::operation(
    const std::string_view& key, common::hash_t& result, common::ValueVector* /*keyVector*/) {
    result = std::hash<std::string_view>()(key);
}

template<>
inline void Hash::operation(
    const common::ku_string_t& key, common::hash_t& result, common::ValueVector* /*keyVector*/) {
    result = std::hash<std::string_view>()(key.getAsStringView());
}

template<>
inline void Hash::operation(
    const common::interval_t& key, common::hash_t& result, common::ValueVector* /*keyVector*/) {
    result = combineHashScalar(murmurhash64(key.months),
        combineHashScalar(murmurhash64(key.days), murmurhash64(key.micros)));
}

template<>
inline void Hash::operation(
    const common::list_entry_t& key, common::hash_t& result, common::ValueVector* keyVector) {
    auto dataVector = common::ListVector::getDataVector(keyVector);
    result = NULL_HASH;
    common::hash_t tmpResult;
    for (auto i = 0u; i < key.size; i++) {
        auto pos = key.offset + i;
        if (dataVector->isNull(pos)) {
            result = combineHashScalar(result, NULL_HASH);
        } else {
            common::TypeUtils::visit(
                dataVector->dataType.getPhysicalType(),
                [&]<HashableTypes T>(
                    T) { operation(dataVector->getValue<T>(pos), tmpResult, dataVector); },
                [&](common::struct_entry_t) {
                    // LCOV_EXCL_START
                    throw common::RuntimeException{"Hash on list of struct is not supported yet."};
                    // LCOV_EXCL_STOP
                },
                [](auto) { KU_UNREACHABLE; });
            result = combineHashScalar(result, tmpResult);
        }
    }
}

template<>
inline void Hash::operation(const std::unordered_set<std::string>& key, common::hash_t& result,
    common::ValueVector* /*keyVector*/) {
    for (auto&& s : key) {
        result ^= std::hash<std::string>()(s);
    }
}

struct InternalIDHasher {
    std::size_t operator()(const common::internalID_t& internalID) const {
        common::hash_t result;
        function::Hash::operation<common::internalID_t>(
            internalID, result, nullptr /* keyVector */);
        return result;
    }
};

} // namespace function
} // namespace kuzu
