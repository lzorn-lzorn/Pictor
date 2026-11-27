#pragma once

/**
 * @file HashTableDetail.hpp
 * @brief LinkedHashTable / FlatHashTable 共享的哈希表工具（终结器、指纹、掩码、
 *        2 的幂取整与分组运算），避免两份实现各自重复维护。
 *
 * @notice 全部函数遵循 infra 的 C++11 约束：布尔查询用 ::value，多语句函数用
 *         PICTOR_CONSTEXPR14（C++11 下退化为普通 inline 函数）。
 */

#include <cstddef>
#include <cstdint>
#include <limits>

#include <infra/common/SIMDControlByte.hpp>
#include <infra/common/VersionCompatibility.hpp>

namespace infra {
namespace hashtable_detail {

using MaskType = SIMDControlByte::MaskType;

// 控制字节组大小（16 槽）。
constexpr std::size_t GroupSize = SIMDControlByte::GroupSize;

// --------------------------------------------------------------------------
// 哈希终结器：扩散熵，使弱哈希（恒等、步进等）在 2 的幂桶数下仍能良好分布。
// 按 sizeof(size_t) 做标签分派，替代 C++17 的 if constexpr。
// --------------------------------------------------------------------------
PICTOR_CONSTEXPR14 inline size_t mixHashImpl(size_t X, std::true_type) noexcept {
    // splitmix64
    X += 0x9e3779b97f4a7c15ULL;
    X = (X ^ (X >> 30)) * 0xbf58476d1ce4e5b9ULL;
    X = (X ^ (X >> 27)) * 0x94d049bb133111ebULL;
    return X ^ (X >> 31);
}

PICTOR_CONSTEXPR14 inline size_t mixHashImpl(size_t X, std::false_type) noexcept {
    // murmur3 fmix32
    X ^= X >> 16;
    X *= 0x85ebca6bU;
    X ^= X >> 13;
    X *= 0xc2b2ae35U;
    X ^= X >> 16;
    return X;
}

PICTOR_CONSTEXPR14 inline size_t mixHash(size_t X) noexcept {
    return mixHashImpl(X, std::integral_constant<bool, sizeof(size_t) == 8>());
}

// 取（混合后）哈希的高 7 位：0..127，永不为 0xFF 空标记。
constexpr std::uint8_t fingerprintOf(size_t HashValue) noexcept {
    return static_cast<std::uint8_t>(HashValue >> (sizeof(size_t) * 8 - 7));
}

// 16 槽组内位于 `From` 及其之后的控制字节槽位掩码。
constexpr MaskType tailMask(size_t From) noexcept {
    return static_cast<MaskType>(~((MaskType{1} << From) - 1));
}

// 2 的幂向上取整（下限 16，上限为最高位）。
PICTOR_CONSTEXPR14 inline size_t roundupPowerOfTwo(size_t Value) noexcept {
    if (Value <= 16) {
        return 16;
    }
    const size_t Top = size_t{1} << (std::numeric_limits<size_t>::digits - 1);
    if (Value > Top) {
        return Top;
    }
    size_t P = 16;
    while (P < Value) {
        P <<= 1;
    }
    return P;
}

// 槽位下标 -> 控制字节组下标 / 组内偏移。
constexpr size_t groupIndexOf(size_t SlotIndex) noexcept {
    return SlotIndex / GroupSize;
}

constexpr size_t slotInGroup(size_t SlotIndex) noexcept {
    return SlotIndex % GroupSize;
}

} // namespace hashtable_detail
} // namespace infra
