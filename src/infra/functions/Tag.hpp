//
// Tag.hpp — 点分路径标签, 如 "Saved.Player.2026-12-8"
//
// SerdeServer 用它把逻辑路径映射为磁盘上的 目录(SerdeSpace)/文件(SerdeUnit)。
//

#ifndef INFRA_FUNCTIONS_TAG_HPP
#define INFRA_FUNCTIONS_TAG_HPP
#pragma once

#include <string>
#include <string_view>


#include <algorithm>
#include <cstddef>    // size_t / std::ptrdiff_t
#include <cstdint>    // int32_t
#include <functional> // std::hash
#include <optional>
#include <string>
#include <vector>

namespace infra{
class Tag {
    constexpr static size_t DefaultTagNumber = 5;
    constexpr static char Delimiter          = '.';

    friend class Tag;

public:
    using tag_type               = std::string;
    using value_type             = std::string;
    using reference              = std::string &;
    using const_reference        = const std::string &;
    using iterator               = typename std::vector<value_type>::iterator;
    using const_iterator         = typename std::vector<value_type>::const_iterator;
    using size_type              = std::size_t;
    using difference_type        = std::ptrdiff_t;
    using pointer                = typename std::vector<value_type>::pointer;
    using const_pointer          = typename std::vector<value_type>::const_pointer;
    using reverse_iterator       = typename std::vector<value_type>::reverse_iterator;
    using const_reverse_iterator = typename std::vector<value_type>::const_reverse_iterator;

    Tag() {
        Tags.reserve(DefaultTagNumber);
    }

    explicit Tag(const std::vector<std::string> &InTags) {
        Tags.reserve(DefaultTagNumber);
        Tags.insert(Tags.end(), InTags.begin(), InTags.end());
    }

    Tag(const Tag &Other) = default;

    Tag(Tag &&Other) = default;

    Tag &operator=(const Tag &Other) = default;

    Tag &operator=(Tag &&Other) = default;

    std::string operator[](size_t N) const noexcept {
        return Tags[N];
    }

    [[nodiscard]] std::string at(size_t N) const {
        return Tags.at(N);
    }

    iterator begin() noexcept {
        return Tags.begin();
    }

    iterator end() noexcept {
        return Tags.end();
    }

    [[nodiscard]] const_iterator begin() const noexcept {
        return Tags.begin();
    }

    [[nodiscard]] const_iterator end() const noexcept {
        return Tags.end();
    }

    reverse_iterator rbegin() noexcept {
        return Tags.rbegin();
    }

    reverse_iterator rend() noexcept {
        return Tags.rend();
    }

    [[nodiscard]] const_reverse_iterator rbegin() const noexcept {
        return Tags.rbegin();
    }

    [[nodiscard]] const_reverse_iterator rend() const noexcept {
        return Tags.rend();
    }

    [[nodiscard]] const_iterator cbegin() const noexcept {
        return Tags.cbegin();
    }

    [[nodiscard]] const_iterator cend() const noexcept {
        return Tags.cend();
    }

    [[nodiscard]] const_reverse_iterator crbegin() const noexcept {
        return Tags.crbegin();
    }

    [[nodiscard]] const_reverse_iterator crend() const noexcept {
        return Tags.crend();
    }

    reference front() {
        return Tags.front();
    }

    [[nodiscard]] const_reference front() const {
        return Tags.front();
    }

    reference back() {
        return Tags.back();
    }

    [[nodiscard]] const_reference back() const {
        return Tags.back();
    }

    bool operator==(const Tag &Other) const noexcept {
        return Tags == Other.Tags;
    }

    [[nodiscard]] bool has(const std::string &OneTag) const noexcept {
        return std::ranges::find(Tags, OneTag) != Tags.end();
    }


    [[nodiscard]] std::string get() const noexcept {
        std::string Result;
        // 预分配：各段长度之和 + 分隔符数量（段数 - 1）
        size_t Total = Tags.empty() ? 0 : Tags.size() - 1;
        for (const auto &OneTag: Tags) {
            Total += OneTag.size();
        }
        Result.reserve(Total);

        for (const auto &OneTag: Tags) {
            if (!Result.empty())
                Result.push_back(Delimiter);
            Result.append(OneTag);
        }

        return Result;
    }

    /**
     * @brief 查找和 InTag 第一个相同的祖先 TagName;
     * @usage this: "AAAA.BBBB.CCCC" other: "AAAA.BBBB.DDDD" 返回 1
     * @return 返回第一个根节点 Tag 的序号
     */
    [[nodiscard]] std::optional<int32_t> getFirstSameRoot([[maybe_unused]] const Tag &InTag) const noexcept {
        return std::nullopt;
    }

private:
    std::vector<std::string> Tags;
};
}

template<>
struct std::hash<infra::Tag> {
    size_t operator()(const infra::Tag &Tag) const noexcept {
        size_t Result = 0;
        for (const auto &TagName: Tag) {
            Result += std::hash<std::string>{}(TagName);
        }
        return Result;
    }
};

#endif // INFRA_FUNCTIONS_TAG_HPP
