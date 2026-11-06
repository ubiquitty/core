// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/RichText/RTFReader.h"

#include "Core/Unicode/UTF8Decoder.h"

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <string_view>

namespace Ubiquitty::RichText {

namespace {

/// Letters past this many in one control word name no word the subset knows;
/// the reader keeps consuming them so the word ends where RTF says it does.
constexpr int maxControlWordLetters = 32;

/// Where the bytes of the current group go.
enum class Destination : std::uint8_t {
    Text,
    FontTable,
    ColorTable,
    Skip,
};

/// Everything RTF scopes to a group: a `{` saves it and the matching `}`
/// restores it.
struct GroupState {
    /// The `\f` number in force; -1 is the document's `\deff`.
    int fontNumber = -1;
    std::uint16_t halfPoints = defaultHalfPoints;
    std::uint8_t styles = 0;
    std::uint8_t color = 0;
    std::uint8_t background = 0;
    Alignment alignment = Alignment::Unspecified;
    /// `\sl` and `\slmult` as written; the paragraph's spacing is read from
    /// the two together when text takes it.
    std::int32_t lineSpacing = 0;
    bool isLineSpacingMultiple = false; // bool-ok: \slmult is a flag
    Paragraph paragraph {};
    /// The paragraph text takes, resolved from the words above whenever one
    /// changes rather than per character; unset while `\sl` and `\slmult`
    /// together name a multiple outside its range.
    std::optional<Paragraph> resolvedParagraph = Paragraph {};
    Destination destination = Destination::Text;
    bool isAtStart = true;
    bool needsDestination = false;
};

/// Control words that stand for one character.
struct NamedCharacter {
    std::string_view word;
    std::uint32_t codePoint;
};

constexpr std::array<NamedCharacter, 10> namedCharacters = { {
    { "bullet", 0x2022 },
    { "emdash", 0x2014 },
    { "endash", 0x2013 },
    { "ldblquote", 0x201C },
    { "line", lineSeparator },
    { "lquote", 0x2018 },
    { "par", 0x000A },
    { "rdblquote", 0x201D },
    { "rquote", 0x2019 },
    { "tab", 0x0009 },
} };

/// The style a toggle word switches, for the four toggles the subset reads.
[[nodiscard]] std::optional<StyleBit> styleOfToggle(std::string_view word)
{
    if (word == "b")
        return StyleBit::Bold;
    if (word == "i")
        return StyleBit::Italic;
    if (word == "ul")
        return StyleBit::Underline;
    if (word == "strike")
        return StyleBit::Strikethrough;
    return std::nullopt;
}

[[nodiscard]] bool isLetter(std::uint8_t byte) { return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z'); }
[[nodiscard]] bool isDigit(std::uint8_t byte) { return byte >= '0' && byte <= '9'; }

[[nodiscard]] bool isLegacyEncoding(std::string_view word)
{
    return word == "ansi" || word == "ansicpg" || word == "mac" || word == "pc" || word == "pca"
        || word == "cpg" || word == "fcharset" || word == "u" || word == "uc";
}

/// Validate even optional destinations before interpreting their grammar.
/// UTF-8 sequences may not cross ASCII RTF syntax, and control characters
/// never become hidden terminal instructions or font-family bytes.
[[nodiscard]] std::optional<ReadError> validateBody(std::span<const std::uint8_t> body)
{
    Unicode::UTF8Decoder decoder {};
    Unicode::utf8Reset(&decoder);
    for (const auto byte : body) {
        std::uint32_t codePoint = 0;
        const auto status = Unicode::utf8Feed(&decoder, byte, &codePoint);
        if (status == Unicode::UTF8Status::Invalid)
            return ReadError::InvalidUTF8;
        if (status == Unicode::UTF8Status::Emit && ((codePoint < 0x20 && codePoint != '\t' && codePoint != '\r' && codePoint != '\n') || (codePoint >= 0x7F && codePoint <= 0x9F)))
            return ReadError::Malformed;
    }
    return decoder.need ? std::optional(ReadError::InvalidUTF8) : std::nullopt;
}

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> body)
        : m_body(body)
    {
        if (!m_document.text.tryReserve(std::min(body.size(), static_cast<std::size_t>(maxCodePoints))))
            fail(ReadError::AllocationRefused);
        Unicode::utf8Reset(&m_utf8);
    }

    [[nodiscard]] std::expected<Document, ReadError> read()
    {
        while (m_position < m_body.size() && !m_isFinished && !m_error) {
            const std::uint8_t byte = m_body[m_position];
            switch (byte) {
            case '{':
                ++m_position;
                openGroup();
                break;
            case '}':
                ++m_position;
                closeGroup();
                break;
            case '\\':
                ++m_position;
                readControl();
                break;
            case '\r':
            case '\n':
                ++m_position;
                break;
            default:
                ++m_position;
                textByte(byte);
                break;
            }
        }
        if (!m_error && (!m_isFinished || m_depth))
            fail(ReadError::Malformed);
        while (m_position < m_body.size() && !m_error) {
            const auto byte = m_body[m_position++];
            if (byte != ' ' && byte != '\t' && byte != '\r' && byte != '\n')
                fail(ReadError::Malformed);
        }
        if (!m_error && m_hasDefaultFont && !fontNumberIndex(m_defaultFontNumber))
            fail(ReadError::Malformed);
        if (m_error)
            return std::unexpected(*m_error);
        return std::move(m_document);
    }

private:
    void fail(ReadError error)
    {
        if (!m_error)
            m_error = error;
    }

    void openGroup()
    {
        flushPendingUTF8();
        if (m_state.needsDestination) {
            fail(ReadError::Malformed);
            return;
        }
        if (m_depth == maxGroupDepth) {
            fail(ReadError::TooDeep);
            return;
        }
        m_state.isAtStart = false;
        m_stack[static_cast<std::size_t>(m_depth)] = m_state;
        ++m_depth;
        m_state.isAtStart = true;
    }

    void closeGroup()
    {
        flushPendingUTF8();
        if (!m_depth || m_state.needsDestination || (m_state.destination == Destination::FontTable && m_hasPendingFont)
            || (m_state.destination == Destination::ColorTable && !m_pendingColor.isAutomatic)) {
            fail(ReadError::Malformed);
            return;
        }
        --m_depth;
        if (!m_depth) {
            m_isFinished = true;
            return;
        }
        m_state = m_stack[static_cast<std::size_t>(m_depth)];
    }

    void readControl()
    {
        flushPendingUTF8();
        if (m_position >= m_body.size()) {
            fail(ReadError::Malformed);
            return;
        }
        const std::uint8_t first = m_body[m_position];
        if (!isLetter(first)) {
            ++m_position;
            controlSymbol(first);
            return;
        }
        std::array<char, maxControlWordLetters> letters {};
        int letterCount = 0;
        while (m_position < m_body.size() && isLetter(m_body[m_position])) {
            if (letterCount < maxControlWordLetters)
                letters[static_cast<std::size_t>(letterCount)] = static_cast<char>(m_body[m_position]);
            ++letterCount;
            ++m_position;
        }
        bool isNegative = false;
        if (m_position < m_body.size() && m_body[m_position] == '-') {
            if (m_position + 1 == m_body.size() || !isDigit(m_body[m_position + 1])) {
                fail(ReadError::Malformed);
                return;
            }
            isNegative = true;
            ++m_position;
        }
        bool hasParameter = false;
        std::int64_t magnitude = 0;
        const std::int64_t largestMagnitude = static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max()) + (isNegative ? 1 : 0);
        while (m_position < m_body.size() && isDigit(m_body[m_position])) {
            hasParameter = true;
            const auto digit = m_body[m_position] - '0';
            if (magnitude > (largestMagnitude - digit) / 10) {
                fail(ReadError::Malformed);
                return;
            }
            magnitude = magnitude * 10 + digit;
            ++m_position;
        }
        if (m_position < m_body.size() && m_body[m_position] == ' ')
            ++m_position;
        if (letterCount > maxControlWordLetters) {
            if (m_state.destination != Destination::Skip)
                fail(ReadError::UnsupportedControl);
            m_state.needsDestination = false;
            m_state.isAtStart = false;
            return;
        }
        const std::string_view word(letters.data(), static_cast<std::size_t>(letterCount));
        const std::int32_t parameter = static_cast<std::int32_t>(isNegative ? -magnitude : magnitude);
        controlWord(word, hasParameter ? std::optional<std::int32_t>(parameter) : std::nullopt);
    }

    void controlSymbol(std::uint8_t symbol)
    {
        if (symbol == '\'') {
            fail(ReadError::UnsupportedControl);
            return;
        }
        if (m_state.needsDestination) {
            fail(ReadError::Malformed);
            return;
        }
        if (symbol == '*') {
            if (!m_state.isAtStart || m_depth < 2) {
                fail(ReadError::Malformed);
                return;
            }
            m_state.isAtStart = false;
            m_state.needsDestination = true;
            m_state.destination = Destination::Skip;
            return;
        }
        m_state.isAtStart = false;
        if (m_state.destination == Destination::FontTable && symbol == ';') {
            appendFontNameByte(symbol);
            return;
        }
        if ((m_state.destination == Destination::FontTable && symbol != '\\' && symbol != '{' && symbol != '}')
            || m_state.destination == Destination::ColorTable) {
            fail(ReadError::UnsupportedControl);
            return;
        }
        switch (symbol) {
        case '\\':
        case '{':
        case '}':
            if (m_state.destination == Destination::FontTable)
                fontNameByte(symbol);
            else
                character(symbol);
            break;
        case '~':
            character(0x00A0);
            break;
        case '_':
            character(0x2011);
            break;
        case '-':
            break;
        case '\r':
        case '\n':
            character(0x000A);
            break;
        default:
            fail(ReadError::UnsupportedControl);
            break;
        }
    }

    void controlWord(std::string_view word, std::optional<std::int32_t> parameter)
    {
        if (isLegacyEncoding(word)) {
            fail(ReadError::UnsupportedControl);
            return;
        }
        const bool isAtStart = m_state.isAtStart;
        m_state.isAtStart = false;
        m_state.needsDestination = false;
        if (m_state.destination == Destination::Skip)
            return;
        if (word == "rtf") {
            if (!isAtStart || m_depth != 1 || parameter != 1)
                fail(ReadError::Malformed);
            return;
        }
        if (word == "fonttbl" || word == "colortbl") {
            const bool isFontTable = word == "fonttbl";
            if (!isAtStart || m_depth < 2 || parameter || m_state.destination != Destination::Text || !m_document.text.empty()
                || (isFontTable ? m_hasFontTable : m_hasColorTable)) {
                fail(ReadError::Malformed);
                return;
            }
            if (isFontTable)
                m_hasFontTable = true;
            else
                m_hasColorTable = true;
            m_state.destination = isFontTable ? Destination::FontTable : Destination::ColorTable;
            m_pendingColor = {};
            return;
        }
        switch (m_state.destination) {
        case Destination::FontTable:
            fontTableWord(word, parameter);
            return;
        case Destination::ColorTable:
            colorTableWord(word, parameter);
            return;
        case Destination::Skip:
            return;
        case Destination::Text:
            break;
        }

        if (const std::optional<StyleBit> bit = styleOfToggle(word)) {
            // A toggle with no parameter, or with any but zero, turns its style on.
            const auto mask = static_cast<std::uint8_t>(*bit);
            const bool isOn = !parameter || *parameter != 0;
            m_state.styles = isOn ? static_cast<std::uint8_t>(m_state.styles | mask) : static_cast<std::uint8_t>(m_state.styles & ~mask);
            return;
        }
        const bool needsParameter = word == "f" || word == "fs" || word == "cf" || word == "highlight" || word == "deff"
            || word == "sl" || word == "slmult" || word == "sb" || word == "sa" || word == "li" || word == "ri" || word == "fi";
        if (needsParameter && !parameter) {
            fail(ReadError::Malformed);
            return;
        }
        if (!needsParameter && parameter) {
            const bool isKnown = word == "ulnone" || word == "plain" || word == "pard" || word == "ql" || word == "qj" || word == "qc" || word == "qr"
                || std::ranges::any_of(namedCharacters, [&](const NamedCharacter& named) { return named.word == word; });
            fail(isKnown ? ReadError::Malformed : ReadError::UnsupportedControl);
            return;
        }
        const std::int32_t value = parameter.value_or(0);
        if ((word == "f" || word == "deff") && value < 0) {
            fail(ReadError::Malformed);
            return;
        }
        if (word == "ulnone")
            m_state.styles = static_cast<std::uint8_t>(m_state.styles & ~static_cast<std::uint8_t>(StyleBit::Underline));
        else if (word == "f") {
            if (!fontNumberIndex(value))
                fail(ReadError::Malformed);
            else
                m_state.fontNumber = value;
        } else if (word == "fs")
            m_state.halfPoints = parameter ? static_cast<std::uint16_t>(std::clamp<std::int32_t>(value, minHalfPoints, maxHalfPoints)) : defaultHalfPoints;
        else if (word == "cf" || word == "highlight") {
            const auto color = colorIndex(value);
            if (!color)
                fail(ReadError::Malformed);
            else if (word == "cf")
                m_state.color = *color;
            else
                m_state.background = *color;
        } else if (word == "deff") {
            if (!m_document.text.empty())
                fail(ReadError::Malformed);
            else {
                m_defaultFontNumber = value;
                m_hasDefaultFont = true;
            }
        } else if (word == "plain") {
            m_state.fontNumber = -1;
            m_state.halfPoints = defaultHalfPoints;
            m_state.styles = 0;
            m_state.color = 0;
            m_state.background = 0;
        } else if (word == "pard") {
            m_state.alignment = Alignment::Unspecified;
            m_state.lineSpacing = 0;
            m_state.isLineSpacingMultiple = false;
            m_state.paragraph = {};
            m_state.resolvedParagraph = Paragraph {};
        } else if (word == "sl" || word == "slmult" || word == "sb" || word == "sa" || word == "li" || word == "ri" || word == "fi")
            paragraphWord(word, value);
        else if (word == "ql" || word == "qj")
            m_state.alignment = Alignment::Left;
        else if (word == "qc")
            m_state.alignment = Alignment::Center;
        else if (word == "qr")
            m_state.alignment = Alignment::Right;
        else {
            for (const NamedCharacter& named : namedCharacters) {
                if (named.word == word) {
                    character(named.codePoint);
                    return;
                }
            }
            fail(ReadError::UnsupportedControl);
        }
    }

    /// Reads a paragraph property, refusing the body when its value is out
    /// of the subset's range.
    void paragraphWord(std::string_view word, std::int32_t value)
    {
        const bool isSigned = word == "sl" || word == "fi"; // bool-ok: names the one fact the range test reads
        const bool isInRange = word == "slmult" ? (value == 0 || value == 1) // bool-ok: one range verdict, used once
            : isSigned                          ? value >= -maxParagraphTwips && value <= maxParagraphTwips
                                                : value >= 0 && value <= maxParagraphTwips;
        if (!isInRange) {
            fail(ReadError::ParagraphOutOfRange);
            return;
        }
        if (word == "sl")
            m_state.lineSpacing = value;
        else if (word == "slmult")
            m_state.isLineSpacingMultiple = value == 1;
        else if (word == "sb")
            m_state.paragraph.spaceBefore = static_cast<std::uint16_t>(value);
        else if (word == "sa")
            m_state.paragraph.spaceAfter = static_cast<std::uint16_t>(value);
        else if (word == "li")
            m_state.paragraph.leftIndent = static_cast<std::uint16_t>(value);
        else if (word == "ri")
            m_state.paragraph.rightIndent = static_cast<std::uint16_t>(value);
        else
            m_state.paragraph.firstIndent = static_cast<std::int16_t>(value);
        m_state.resolvedParagraph = paragraph();
    }

    /// The paragraph text takes: the group's properties, its spacing read
    /// from `\sl` and `\slmult` together. A multiple outside its range
    /// refuses the body.
    [[nodiscard]] std::optional<Paragraph> paragraph() const
    {
        Paragraph result = m_state.paragraph;
        const std::int32_t spacing = m_state.lineSpacing;
        if (!spacing)
            return result;
        if (m_state.isLineSpacingMultiple) {
            if (spacing < 0 || spacing > maxLineSpacingMultiple)
                return std::nullopt;
            result.lineSpacing = LineSpacing::Multiple;
        } else {
            result.lineSpacing = spacing > 0 ? LineSpacing::AtLeast : LineSpacing::Exactly;
        }
        result.lineSpacingValue = static_cast<std::uint16_t>(spacing < 0 ? -spacing : spacing);
        return result;
    }

    [[nodiscard]] std::optional<std::uint8_t> colorIndex(std::int32_t parameter) const
    {
        if (!parameter)
            return 0;
        if (parameter < 0 || parameter >= m_document.colorCount)
            return std::nullopt;
        if (m_document.colors[static_cast<std::size_t>(parameter)].isAutomatic)
            return 0;
        return static_cast<std::uint8_t>(parameter);
    }

    void fontTableWord(std::string_view word, std::optional<std::int32_t> parameter)
    {
        if (word == "f") {
            if (!parameter || *parameter < 0 || m_hasPendingFont) {
                fail(ReadError::Malformed);
                return;
            }
            m_pendingFont = {};
            m_pendingFontNumber = *parameter;
            m_hasPendingFont = true;
            m_pendingFontSpaces = 0;
            return;
        }
        if (word != "froman" && word != "fmodern" && word != "fnil" && word != "fswiss" && word != "fscript" && word != "fdecor" && word != "ftech" && word != "fbidi") {
            fail(ReadError::UnsupportedControl);
            return;
        }
        if (parameter || !m_hasPendingFont) {
            fail(ReadError::Malformed);
            return;
        }
        if (word == "froman")
            m_pendingFont.familyClass = FamilyClass::Serif;
        else if (word == "fmodern")
            m_pendingFont.familyClass = FamilyClass::Monospace;
        else if (word == "fnil" || word == "fswiss" || word == "fscript" || word == "fdecor" || word == "ftech" || word == "fbidi")
            m_pendingFont.familyClass = FamilyClass::SansSerif;
    }

    void commitFont()
    {
        m_hasPendingFont = false;
        m_pendingFontSpaces = 0;
        if (fontNumberIndex(m_pendingFontNumber)) {
            fail(ReadError::Malformed);
            return;
        }
        if (m_document.fontCount() == maxFonts) {
            fail(ReadError::TooManyFonts);
            return;
        }
        m_fontNumbers[m_document.fontCount()] = m_pendingFontNumber;
        if (!m_document.fonts.tryPushBack(m_pendingFont))
            fail(ReadError::AllocationRefused);
    }

    void fontNameByte(std::uint8_t byte)
    {
        if (!m_hasPendingFont) {
            if (byte != ' ')
                fail(ReadError::Malformed);
            return;
        }
        if (byte == ';') {
            commitFont();
            return;
        }
        appendFontNameByte(byte);
    }

    void appendFontNameByte(std::uint8_t byte)
    {
        if (!m_hasPendingFont) {
            fail(ReadError::Malformed);
            return;
        }
        if (byte == ' ') {
            if (m_pendingFont.nameLength)
                ++m_pendingFontSpaces;
            return;
        }
        if (m_pendingFontSpaces + m_pendingFont.nameLength >= static_cast<std::size_t>(maxFamilyNameBytes)) {
            fail(ReadError::FamilyNameTooLong);
            return;
        }
        while (m_pendingFontSpaces) {
            m_pendingFont.name[m_pendingFont.nameLength++] = ' ';
            --m_pendingFontSpaces;
        }
        m_pendingFont.name[m_pendingFont.nameLength] = static_cast<char>(byte);
        ++m_pendingFont.nameLength;
    }

    void colorTableWord(std::string_view word, std::optional<std::int32_t> parameter)
    {
        if (word != "red" && word != "green" && word != "blue") {
            fail(ReadError::UnsupportedControl);
            return;
        }
        if (!parameter) {
            fail(ReadError::Malformed);
            return;
        }
        const auto component = static_cast<std::uint8_t>(std::clamp<std::int32_t>(*parameter, 0, 255));
        if (word == "red")
            m_pendingColor.red = component;
        else if (word == "green")
            m_pendingColor.green = component;
        else if (word == "blue")
            m_pendingColor.blue = component;
        m_pendingColor.isAutomatic = false;
    }

    void textByte(std::uint8_t byte)
    {
        if (m_state.needsDestination || byte < 0x20) {
            fail(ReadError::Malformed);
            return;
        }
        m_state.isAtStart = false;
        switch (m_state.destination) {
        case Destination::Skip:
            return;
        case Destination::FontTable:
            fontNameByte(byte);
            return;
        case Destination::ColorTable:
            if (byte == ';') {
                if (m_document.colorCount == maxColors) {
                    fail(ReadError::TooManyColors);
                    return;
                }
                m_document.colors[m_document.colorCount] = m_pendingColor;
                ++m_document.colorCount;
                m_pendingColor = {};
            } else if (byte != ' ')
                fail(ReadError::Malformed);
            return;
        case Destination::Text:
            break;
        }
        if (byte < 0x80) {
            flushPendingUTF8();
            character(byte);
            return;
        }
        std::uint32_t codePoint = 0;
        switch (Unicode::utf8Feed(&m_utf8, byte, &codePoint)) {
        case Unicode::UTF8Status::Need:
            return;
        case Unicode::UTF8Status::Emit:
            character(codePoint);
            return;
        case Unicode::UTF8Status::Invalid:
            fail(ReadError::InvalidUTF8);
            return;
        }
    }

    void flushPendingUTF8()
    {
        if (!m_utf8.need)
            return;
        fail(ReadError::InvalidUTF8);
    }

    void character(std::uint32_t codePoint)
    {
        if (m_state.destination != Destination::Text)
            return;
        flushPendingUTF8();
        appendCodePoint(codePoint);
    }

    [[nodiscard]] std::optional<std::uint8_t> fontNumberIndex(int number) const
    {
        for (std::uint8_t index = 0; index < m_document.fontCount(); ++index) {
            if (m_fontNumbers[index] == number)
                return index;
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::uint8_t> fontIndex() const
    {
        if (m_state.fontNumber >= 0)
            return fontNumberIndex(m_state.fontNumber);
        if (m_hasDefaultFont)
            return fontNumberIndex(m_defaultFontNumber);
        return fontNumberIndex(0).value_or(noFontEntry);
    }

    void appendCodePoint(std::uint32_t codePoint)
    {
        if (m_error || codePoint == 0)
            return;
        if (!Unicode::isScalarValue(codePoint)) {
            fail(ReadError::InvalidUTF8);
            return;
        }
        if (m_document.text.size() >= static_cast<std::size_t>(maxCodePoints)) {
            fail(ReadError::TooMuchText);
            return;
        }
        const std::optional<Paragraph>& paragraphStyle = m_state.resolvedParagraph;
        if (!paragraphStyle) {
            fail(ReadError::ParagraphOutOfRange);
            return;
        }
        const auto font = fontIndex();
        if (!font) {
            fail(ReadError::Malformed);
            return;
        }
        if (!m_document.runs.empty()) {
            Run& last = m_document.runs.back();
            if (last.halfPoints == m_state.halfPoints && last.font == *font && last.color == m_state.color && last.background == m_state.background && last.styles == m_state.styles
                && last.alignment == m_state.alignment && last.paragraph == *paragraphStyle) {
                if (!m_document.text.tryPushBack(codePoint))
                    fail(ReadError::AllocationRefused);
                else
                    ++last.length;
                return;
            }
        }
        if (m_document.runs.size() >= static_cast<std::size_t>(maxRuns)) {
            fail(ReadError::TooManyRuns);
            return;
        }
        if (!m_document.runs.tryPushBack(Run {
                .start = static_cast<std::uint32_t>(m_document.text.size()),
                .length = 1,
                .halfPoints = m_state.halfPoints,
                .font = *font,
                .color = m_state.color,
                .background = m_state.background,
                .styles = m_state.styles,
                .alignment = m_state.alignment,
                .paragraph = *paragraphStyle,
            })) {
            fail(ReadError::AllocationRefused);
            return;
        }
        if (!m_document.text.tryPushBack(codePoint))
            fail(ReadError::AllocationRefused);
    }

    std::span<const std::uint8_t> m_body;
    std::size_t m_position = 0;
    Document m_document;
    std::array<GroupState, maxGroupDepth> m_stack {};
    GroupState m_state;
    int m_depth = 0;
    int m_defaultFontNumber = 0;
    std::array<int, maxFonts> m_fontNumbers {};
    FontEntry m_pendingFont;
    int m_pendingFontNumber = 0;
    std::size_t m_pendingFontSpaces = 0;
    ColorEntry m_pendingColor;
    Unicode::UTF8Decoder m_utf8 {};
    std::optional<ReadError> m_error;
    bool m_hasPendingFont = false;
    bool m_hasFontTable = false;
    bool m_hasColorTable = false;
    bool m_hasDefaultFont = false;
    bool m_isFinished = false;
};

[[nodiscard]] bool opensAsRTF(std::span<const std::uint8_t> body)
{
    constexpr std::string_view opening = "{\\rtf1";
    if (body.size() < opening.size())
        return false;
    for (std::size_t index = 0; index < opening.size(); ++index) {
        if (body[index] != static_cast<std::uint8_t>(opening[index]))
            return false;
    }
    return body.size() == opening.size() || !isDigit(body[opening.size()]);
}

} // namespace

std::expected<Document, ReadError> readRTF(std::span<const std::uint8_t> body)
{
    if (body.size() > maxBodyBytes)
        return std::unexpected(ReadError::BodyTooLong);
    if (!opensAsRTF(body))
        return std::unexpected(ReadError::NotRTF);
    if (const auto error = validateBody(body))
        return std::unexpected(*error);
    return Reader(body).read();
}

} // namespace Ubiquitty::RichText
