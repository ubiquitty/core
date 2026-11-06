// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Preferences/RangeMapWrite.h"

#include "Core/Preferences/CodePointRange.h"
#include "Core/Preferences/TOMLString.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <optional>
#include <string_view>
#include <vector>

namespace Ubiquitty::Preferences {

namespace {

constexpr const char* rangeMapHeader = "[[font.range_map]]";

// One `[[font.range_map]]` block found in the file: where its text lives, and
// the range it claims. `family` is carried so a split can re-render the
// remainder without re-reading the file.
struct FoundBlock {
    std::string::size_type start = 0; // the header line's first byte
    std::string::size_type end = 0; // one past the block's last byte
    std::uint32_t firstCodePoint = 0;
    std::uint32_t lastCodePoint = 0;
    std::string family;
    std::optional<bool> shouldMatchSize;
    bool hasReadableRange = false; // a block whose range this writer cannot read is left alone
};

std::string trimmed(std::string_view text)
{
    std::string::size_type first = 0;
    while (first < text.size() && (text[first] == ' ' || text[first] == '\t' || text[first] == '\r'))
        ++first;
    std::string::size_type last = text.size();
    while (last > first && (text[last - 1] == ' ' || text[last - 1] == '\t' || text[last - 1] == '\r'))
        --last;
    return std::string(text.substr(first, last - first));
}

// The value of a `key = "..."` line, unescaped only as far as the two
// characters the writer escapes. A value carrying anything else round-trips
// through re-rendering unchanged, because a split re-renders from this string.
std::optional<std::string> quotedValueOnLine(std::string_view line, std::string_view key)
{
    const std::string text = trimmed(line);
    if (text.size() <= key.size() || !std::string_view(text).starts_with(key))
        return std::nullopt;
    std::string::size_type cursor = key.size();
    while (cursor < text.size() && (text[cursor] == ' ' || text[cursor] == '\t'))
        ++cursor;
    if (cursor >= text.size() || text[cursor] != '=')
        return std::nullopt;
    const std::string::size_type open = text.find('"', cursor);
    if (open == std::string::npos)
        return std::nullopt;
    std::string value;
    for (std::string::size_type i = open + 1; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 1 < text.size()) {
            switch (const char next = text[++i]) {
            case 'n':
                value.push_back('\n');
                break;
            case 't':
                value.push_back('\t');
                break;
            default:
                value.push_back(next);
                break;
            }
            continue;
        }
        if (text[i] == '"')
            return value;
        value.push_back(text[i]);
    }
    return std::nullopt;
}

bool lineOpensASection(std::string_view line)
{
    const std::string text = trimmed(line);
    return !text.empty() && text[0] == '[';
}

// Every `[[font.range_map]]` block in the file, in file order. A block runs from
// its header line to just before the next section header, which is the same
// extent rule findNamedBlock uses for themes.
std::vector<FoundBlock> findRangeMapBlocks(const std::string& body)
{
    std::vector<FoundBlock> found;
    std::string::size_type cursor = 0;
    while (cursor < body.size()) {
        std::string::size_type lineEnd = body.find('\n', cursor);
        if (lineEnd == std::string::npos)
            lineEnd = body.size();
        const std::string_view line(body.data() + cursor, lineEnd - cursor);
        if (trimmed(line) != rangeMapHeader) {
            cursor = lineEnd + 1;
            continue;
        }

        FoundBlock block;
        block.start = cursor;
        std::string::size_type inner = lineEnd < body.size() ? lineEnd + 1 : body.size();
        block.end = body.size();
        std::string rangeText;
        while (inner < body.size()) {
            std::string::size_type innerEnd = body.find('\n', inner);
            if (innerEnd == std::string::npos)
                innerEnd = body.size();
            const std::string_view innerLine(body.data() + inner, innerEnd - inner);
            if (lineOpensASection(innerLine)) {
                block.end = inner;
                break;
            }
            if (const std::optional<std::string> value = quotedValueOnLine(innerLine, "range"))
                rangeText = *value;
            else if (const std::optional<std::string> value = quotedValueOnLine(innerLine, "family"))
                block.family = *value;
            const std::string assignment = trimmed(innerLine.substr(0, innerLine.find('#')));
            const auto equals = assignment.find('=');
            if (equals != std::string::npos && trimmed(std::string_view(assignment).substr(0, equals)) == "match_size") {
                const std::string value = trimmed(std::string_view(assignment).substr(equals + 1));
                if (value == "true" || value == "false")
                    block.shouldMatchSize = value == "true";
            }
            inner = innerEnd + 1;
        }
        if (const std::optional<CodePointRange> range = parseCodePointRange(rangeText)) {
            block.firstCodePoint = range->firstCodePoint;
            block.lastCodePoint = range->lastCodePoint;
            block.hasReadableRange = true;
        }
        found.push_back(std::move(block));
        cursor = found.back().end;
    }
    return found;
}

bool overlaps(const FoundBlock& block, std::uint32_t first, std::uint32_t last)
{
    return block.hasReadableRange && block.firstCodePoint <= last && first <= block.lastCodePoint;
}

// What survives of `block` once [first,last] is taken out of it: nothing, the
// piece below, the piece above, or both.
std::string survivingPieces(const FoundBlock& block, std::uint32_t first, std::uint32_t last)
{
    std::string pieces;
    if (block.firstCodePoint < first)
        pieces += renderRangeMapBlock(block.firstCodePoint, first - 1, block.family, block.shouldMatchSize);
    if (block.lastCodePoint > last)
        pieces += renderRangeMapBlock(last + 1, block.lastCodePoint, block.family, block.shouldMatchSize);
    // The blocks render with a leading newline for appending; spliced in place
    // they follow one already, so the first is dropped.
    if (!pieces.empty() && pieces.front() == '\n')
        pieces.erase(0, 1);
    return pieces;
}

// Rewrites every overlapping block to whatever survives the cut, back to front
// so the earlier blocks' offsets stay valid.
std::string cutRange(std::string body, std::uint32_t first, std::uint32_t last)
{
    std::vector<FoundBlock> blocks = findRangeMapBlocks(body);
    for (auto it = blocks.rbegin(); it != blocks.rend(); ++it) {
        if (!overlaps(*it, first, last))
            continue;
        body.replace(it->start, it->end - it->start, survivingPieces(*it, first, last));
    }
    return body;
}

} // namespace

std::string formatCodePointRange(std::uint32_t firstCodePoint, std::uint32_t lastCodePoint)
{
    std::array<char, 32> buffer {};
    if (firstCodePoint == lastCodePoint) {
        std::snprintf(buffer.data(), buffer.size(), "U+%04X", firstCodePoint);
        return std::string(buffer.data());
    }
    std::snprintf(buffer.data(), buffer.size(), "U+%04X-U+%04X", firstCodePoint, lastCodePoint);
    return std::string(buffer.data());
}

std::string renderRangeMapBlock(std::uint32_t firstCodePoint, std::uint32_t lastCodePoint, std::string_view family, std::optional<bool> shouldMatchSize)
{
    std::string out = "\n";
    out += rangeMapHeader;
    out += "\nrange  = \"";
    out += formatCodePointRange(firstCodePoint, lastCodePoint);
    out += "\"\nmode   = \"custom\"\nfamily = \"";
    out += escapeTOMLBasicString(family);
    out += "\"\n";
    if (shouldMatchSize)
        out += *shouldMatchSize ? "match_size = true\n" : "match_size = false\n";
    return out;
}

std::string spliceRangeMap(std::string body, std::uint32_t firstCodePoint, std::uint32_t lastCodePoint, std::string_view family, std::optional<bool> shouldMatchSize)
{
    if (firstCodePoint > lastCodePoint || family.empty())
        return body;
    body = cutRange(std::move(body), firstCodePoint, lastCodePoint);
    body += renderRangeMapBlock(firstCodePoint, lastCodePoint, family, shouldMatchSize);
    return body;
}

std::string removeRangeMap(std::string body, std::uint32_t firstCodePoint, std::uint32_t lastCodePoint)
{
    if (firstCodePoint > lastCodePoint)
        return body;
    return cutRange(std::move(body), firstCodePoint, lastCodePoint);
}

} // namespace Ubiquitty::Preferences
