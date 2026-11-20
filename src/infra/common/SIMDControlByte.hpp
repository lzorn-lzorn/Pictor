//
// Created by Lzorn on 2026/11/20.
//

#ifndef PICTOR_SIMDCONTROLBYTE_H
#define PICTOR_SIMDCONTROLBYTE_H

#include <cstdint>
#include <cstring>
#include <cstddef>

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#  define G_HAS_SSE2 1
#  include <emmintrin.h>
#elif defined(__ARM_NEON)
#  define G_HAS_NEON 1
#  include <arm_neon.h>
#else
#  define G_HAS_SCALAR 1
#endif

class SIMDControlByte
{
public:
    static constexpr std::size_t  GroupSize = 16;
    static constexpr std::uint8_t EmptyByte = 0xFF;
    using MaskType = std::uint16_t;

    SimdControlByte() noexcept { std::memset(Bytes, EmptyByte, GroupSize); }

    // @brief 返回与指纹匹配的槽位掩码
    [[nodiscard]] MaskType matchFingerprint(std::uint8_t fingerprint) const noexcept {
#if G_HAS_SSE2
        const __m128i Vec    = _mm_load_si128(reinterpret_cast<const __m128i*>(Bytes));
        const __m128i Target = _mm_set1_epi8(static_cast<char>(fingerprint));
        return static_cast<MaskType>(_mm_movemask_epi8(_mm_cmpeq_epi8(Vec, Target)));
#elif G_HAS_NEON
        const uint8x16_t Vec    = vld1q_u8(Bytes);
        const uint8x16_t Target = vdupq_n_u8(fingerprint);
        const uint8x16_t Eq     = vceqq_u8(Vec, Target);
        // NEON 没有直接 movemask，用标量提取
        MaskType Mask = 0;
        alignas(16) std::uint8_t Tmp[16];
        vst1q_u8(Tmp, Eq);
        for (std::size_t i = 0; i < 16; ++i) Mask |= (Tmp[i] ? (1u << i) : 0u);
        return Mask;
#else
        MaskType Mask = 0;
        for (std::size_t i = 0; i < GroupSize; ++i)
            if (Bytes[i] == fingerprint) Mask |= (MaskType{1} << i);
        return Mask;
#endif
    }

    [[nodiscard]] MaskType matchEmpty() const noexcept { return matchFingerprint(EmptyByte); }

    void set(std::size_t index, std::uint8_t value) noexcept { Bytes[index] = value; }
    [[nodiscard]] std::uint8_t get(std::size_t index) const noexcept { return Bytes[index]; }

private:
    alignas(16) std::uint8_t Bytes[GroupSize];
};

#endif //PICTOR_SIMDCONTROLBYTE_H
