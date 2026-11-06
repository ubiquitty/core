// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/FallibleVector.h"

#include <cstring>
#include <string>
#include <string_view>

namespace Ubiquitty {

/// UTF-8 bytes with an exact-capacity fallible assignment seam.
///
/// Nonempty strings own their terminator. Empty strings need no allocation.
/// Legacy string-shaped operations retain std::string's fatal allocation
/// contract; checkpoint decoding uses tryAssign and tryResize exclusively.
class FallibleString {
public:
    static constexpr std::size_t npos = std::string_view::npos;
    using value_type = char;
    using iterator = char*;
    using const_iterator = const char*;

    FallibleString() = default;
    FallibleString(std::string_view text) { require(tryAssign(text)); }
    FallibleString(const char* text)
        : FallibleString(std::string_view(text))
    {
    }
    FallibleString(const std::string& text)
        : FallibleString(std::string_view(text))
    {
    }
    FallibleString(std::size_t count, char value) { resize(count, value); }
    template<std::input_iterator Iterator>
    FallibleString(Iterator first, Iterator last) { assign(first, last); }
    FallibleString(const FallibleString& other) { require(tryAssign(other.view())); }
    FallibleString(FallibleString&&) noexcept = default;
    FallibleString& operator=(FallibleString&&) noexcept = default;
    FallibleString& operator=(const FallibleString& other)
    {
        if (this != &other)
            require(tryAssign(other.view()));
        return *this;
    }
    FallibleString& operator=(std::string_view text)
    {
        require(tryAssign(text));
        return *this;
    }
    FallibleString& operator=(const char* text) { return *this = std::string_view(text); }
    FallibleString& operator=(const std::string& text) { return *this = std::string_view(text); }

    [[nodiscard]] std::string_view view() const noexcept { return { data(), size() }; }
    operator std::string_view() const noexcept { return view(); }
    operator std::string() const { return copyToString(); }
    /// Copies the bytes into an ordinary string for publication/interop.
    /// Decoders use tryAssign instead of this allocating convenience.
    [[nodiscard]] std::string copyToString() const { return std::string(view()); }
    [[nodiscard]] const char* data() const noexcept { return m_bytes.empty() ? "" : m_bytes.data(); }
    [[nodiscard]] char* data() noexcept { return m_bytes.empty() ? emptyBuffer() : m_bytes.data(); }
    [[nodiscard]] const char* c_str() const noexcept { return data(); }
    [[nodiscard]] std::size_t size() const noexcept { return m_bytes.empty() ? 0 : m_bytes.size() - 1; }
    [[nodiscard]] std::size_t length() const noexcept { return size(); }
    [[nodiscard]] bool empty() const noexcept { return !size(); }
    [[nodiscard]] std::size_t capacity() const noexcept { return m_bytes.capacity() ? m_bytes.capacity() - 1 : 0; }
    [[nodiscard]] std::size_t allocationBytes() const noexcept { return m_bytes.allocationBytes(); }
    [[nodiscard]] iterator begin() noexcept { return data(); }
    [[nodiscard]] const_iterator begin() const noexcept { return data(); }
    [[nodiscard]] iterator end() noexcept { return data() + size(); }
    [[nodiscard]] const_iterator end() const noexcept { return data() + size(); }
    [[nodiscard]] auto rbegin() const noexcept { return std::reverse_iterator(end()); }
    [[nodiscard]] auto rend() const noexcept { return std::reverse_iterator(begin()); }
    [[nodiscard]] char& operator[](std::size_t index)
    {
        assert(index < size());
        return data()[index];
    }
    [[nodiscard]] char operator[](std::size_t index) const
    {
        assert(index < size());
        return data()[index];
    }
    [[nodiscard]] char& front() { return (*this)[0]; }
    [[nodiscard]] char front() const { return (*this)[0]; }
    [[nodiscard]] char& back() { return (*this)[size() - 1]; }
    [[nodiscard]] char back() const { return (*this)[size() - 1]; }

    [[nodiscard]] std::expected<void, StorageError> tryAssign(std::string_view text)
    {
        if (text.empty()) {
            clear();
            return {};
        }
        if (text.size() == std::numeric_limits<std::size_t>::max())
            return std::unexpected(StorageError::OverCapacity);
        const auto count = text.size() + 1;
        if (count <= m_bytes.capacity()) {
            if (count > m_bytes.size()) {
                if (auto resized = m_bytes.tryResize(count); !resized)
                    return resized;
            }
            std::memmove(m_bytes.data(), text.data(), text.size());
            if (auto resized = m_bytes.tryResize(count); !resized)
                return resized;
            m_bytes.back() = '\0';
            return {};
        }
        FallibleVector<char> candidate;
        if (auto resized = candidate.tryResize(count); !resized)
            return resized;
        std::memcpy(candidate.data(), text.data(), text.size());
        candidate.back() = '\0';
        m_bytes.swap(candidate);
        return {};
    }
    [[nodiscard]] std::expected<void, StorageError> tryAssign(const FallibleString& text) { return tryAssign(text.view()); }
    [[nodiscard]] std::expected<void, StorageError> tryAssign(const char* text) { return tryAssign(std::string_view(text)); }
    [[nodiscard]] std::expected<void, StorageError> tryAssign(const std::string& text) { return tryAssign(std::string_view(text)); }
    [[nodiscard]] std::expected<void, StorageError> tryResize(std::size_t count)
    {
        if (!count) {
            clear();
            return {};
        }
        if (count == std::numeric_limits<std::size_t>::max())
            return std::unexpected(StorageError::OverCapacity);
        if (auto resized = m_bytes.tryResize(count + 1); !resized)
            return resized;
        m_bytes.back() = '\0';
        return {};
    }

    void clear() noexcept { m_bytes.clear(); }
    void swap(FallibleString& other) noexcept { m_bytes.swap(other.m_bytes); }
    void reserve(std::size_t count)
    {
        if (!count)
            return;
        if (count == std::numeric_limits<std::size_t>::max())
            std::abort();
        require(m_bytes.tryReserve(count + 1));
    }
    void resize(std::size_t count) { require(tryResize(count)); }
    void resize(std::size_t count, char value)
    {
        const auto previous = size();
        resize(count);
        if (count > previous)
            std::fill(begin() + previous, end(), value);
    }
    FallibleString& assign(std::string_view text)
    {
        require(tryAssign(text));
        return *this;
    }
    template<std::input_iterator Iterator>
    FallibleString& assign(Iterator first, Iterator last)
    {
        if (first == last) {
            clear();
            return *this;
        }
        FallibleVector<char> candidate(first, last);
        candidate.push_back('\0');
        m_bytes.swap(candidate);
        return *this;
    }
    void push_back(char value)
    {
        if (size() == std::numeric_limits<std::size_t>::max() - 1)
            std::abort();
        if (size() == capacity()) {
            const auto current = size();
            reserve(current > (std::numeric_limits<std::size_t>::max() - 1) / 2 ? current + 1 : std::max<std::size_t>(16, 2 * current));
        }
        const auto previous = size();
        resize(previous + 1);
        data()[previous] = value;
    }
    void pop_back()
    {
        assert(size());
        resize(size() - 1);
    }
    FallibleString& append(std::string_view text)
    {
        if (text.size() > std::numeric_limits<std::size_t>::max() - 1 - size())
            std::abort();
        const auto previous = size();
        // A self-slice remains valid while capacity changes only after copying.
        FallibleString copy(text);
        resize(previous + copy.size());
        std::memcpy(data() + previous, copy.data(), copy.size());
        return *this;
    }
    FallibleString& operator+=(std::string_view text) { return append(text); }
    FallibleString& operator+=(char value)
    {
        push_back(value);
        return *this;
    }
    FallibleString& erase(std::size_t position = 0, std::size_t count = npos)
    {
        assert(position <= size());
        const auto removed = std::min(count, size() - position);
        std::memmove(data() + position, data() + position + removed, size() - position - removed);
        resize(size() - removed);
        return *this;
    }
    [[nodiscard]] std::size_t find(std::string_view text, std::size_t position = 0) const { return view().find(text, position); }
    [[nodiscard]] std::size_t find(char value, std::size_t position = 0) const { return view().find(value, position); }
    [[nodiscard]] std::size_t rfind(std::string_view text, std::size_t position = npos) const { return view().rfind(text, position); }
    [[nodiscard]] bool starts_with(std::string_view text) const { return view().starts_with(text); }
    [[nodiscard]] bool ends_with(std::string_view text) const { return view().ends_with(text); }
    [[nodiscard]] bool contains(std::string_view text) const { return view().contains(text); }
    [[nodiscard]] std::string substr(std::size_t position = 0, std::size_t count = npos) const { return std::string(view().substr(position, count)); }
    friend bool operator==(const FallibleString& left, const FallibleString& right) { return left.view() == right.view(); }
    friend bool operator==(const FallibleString& left, std::string_view right) { return left.view() == right; }
    friend bool operator==(const FallibleString& left, const char* right) { return left.view() == right; }
    friend bool operator==(const FallibleString& left, const std::string& right) { return left.view() == right; }
    friend std::string operator+(const FallibleString& left, std::string_view right) { return std::string(left.view()) + std::string(right); }
    friend std::string operator+(std::string_view left, const FallibleString& right) { return std::string(left) + std::string(right.view()); }

private:
    static void require(const std::expected<void, StorageError>& result)
    {
        if (!result)
            std::abort();
    }
    char* emptyBuffer() noexcept { return &m_empty; }
    FallibleVector<char> m_bytes;
    char m_empty = '\0';
};

} // namespace Ubiquitty
