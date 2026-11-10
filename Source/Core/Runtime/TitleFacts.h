// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace Ubiquitty::Runtime {

/// A pane's title and working directory as its model's writer left them, in a
/// fixed record a reader copies without the model lock.
///
/// The writer publishes it beside the pane's other facts when either text
/// changes, so a frontend told that the title or the directory changed reads
/// the new text from here rather than waiting for the parser. Each text is
/// cut at a UTF-8 boundary when it outgrows its field; the model's own title
/// is bounded to the same size, so only a directory can be cut.
struct TitleFacts {
    /// The most bytes either text keeps.
    static constexpr std::size_t maxBytes = 4096;

    /// The title's length in bytes.
    std::uint16_t titleLength = 0;
    /// The directory's length in bytes.
    std::uint16_t directoryLength = 0;
    /// Whether the directory outgrew its field and was cut.
    bool isDirectoryCut = false;
    /// The title's bytes.
    std::array<char, maxBytes> title {};
    /// The directory's bytes.
    std::array<char, maxBytes> directory {};

    /// The title.
    ///
    /// - Returns: A view into this record.
    [[nodiscard]] std::string_view titleView() const { return { title.data(), titleLength }; }

    /// The working directory the program reported.
    ///
    /// - Returns: A view into this record.
    [[nodiscard]] std::string_view directoryView() const { return { directory.data(), directoryLength }; }

    /// Fills the fields from the model's two texts.
    ///
    /// - Parameters:
    ///   - titleText: The title.
    ///   - directoryText: The working directory.
    void assign(std::string_view titleText, std::string_view directoryText)
    {
        titleLength = static_cast<std::uint16_t>(copyBounded(titleText, title));
        const std::size_t directoryKept = copyBounded(directoryText, directory);
        directoryLength = static_cast<std::uint16_t>(directoryKept);
        isDirectoryCut = directoryKept < directoryText.size();
    }

    /// Whether the fields already hold these two texts.
    ///
    /// - Parameters:
    ///   - titleText: The title.
    ///   - directoryText: The working directory.
    /// - Returns: True when assigning them would change nothing.
    [[nodiscard]] bool holds(std::string_view titleText, std::string_view directoryText) const
    {
        return titleView() == cut(titleText) && directoryView() == cut(directoryText);
    }

private:
    /// The longest prefix of a text that fits and ends on a character boundary.
    ///
    /// - Parameter text: The text.
    /// - Returns: The prefix, a view into `text`.
    static std::string_view cut(std::string_view text)
    {
        if (text.size() <= maxBytes)
            return text;
        std::size_t end = maxBytes;
        while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)
            --end;
        return text.substr(0, end);
    }

    /// Copies the bounded prefix of a text into a field.
    ///
    /// - Parameters:
    ///   - text: The text.
    ///   - into: The field.
    /// - Returns: The bytes copied.
    static std::size_t copyBounded(std::string_view text, std::array<char, maxBytes>& into)
    {
        const std::string_view kept = cut(text);
        for (std::size_t index = 0; index < kept.size(); ++index)
            into[index] = kept[index];
        return kept.size();
    }
};

static_assert(std::is_trivially_copyable_v<TitleFacts>);

} // namespace Ubiquitty::Runtime
