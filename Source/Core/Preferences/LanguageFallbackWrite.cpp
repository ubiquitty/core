// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Preferences/LanguageFallbackWrite.h"

#include "Core/Preferences/TOMLString.h"

#include <cstddef>
#include <optional>
#include <vector>

namespace Ubiquitty::Preferences {

namespace {

constexpr const char* languageFallbackHeader = "[[font.language_fallbacks]]";

// One `[[font.language_fallbacks]]` block found in the file: where its text
// lives and which script it claims.
struct FoundBlock {
    std::string::size_type start = 0; // the header line's first byte
    std::string::size_type end = 0; // one past the block's last byte
    std::string script;
    bool hasScript = false; // a block naming no script is not this writer's to move
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

char lowered(char character) { return (character >= 'A' && character <= 'Z') ? static_cast<char>(character - 'A' + 'a') : character; }

// The reader compares scripts case-insensitively, so this writer has to as
// well: "Arabic" and "arabic" name one script, and treating them as two would
// leave the file with a duplicate the route answers from arbitrarily.
bool sameScript(std::string_view left, std::string_view right)
{
    if (left.size() != right.size())
        return false;
    for (std::string_view::size_type i = 0; i < left.size(); ++i) {
        if (lowered(left[i]) != lowered(right[i]))
            return false;
    }
    return true;
}

// The value of a `key = "..."` line, unescaped only as far as the two
// characters the writer escapes.
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

// Every `[[font.language_fallbacks]]` block in the file, in file order. A block
// runs from its header line to just before the next section header, which is
// the extent rule the theme and range-map writers use.
std::vector<FoundBlock> findLanguageFallbackBlocks(const std::string& body)
{
    std::vector<FoundBlock> found;
    std::string::size_type cursor = 0;
    while (cursor < body.size()) {
        std::string::size_type lineEnd = body.find('\n', cursor);
        if (lineEnd == std::string::npos)
            lineEnd = body.size();
        const std::string_view line(body.data() + cursor, lineEnd - cursor);
        if (trimmed(line) != languageFallbackHeader) {
            cursor = lineEnd + 1;
            continue;
        }

        FoundBlock block;
        block.start = cursor;
        std::string::size_type inner = lineEnd < body.size() ? lineEnd + 1 : body.size();
        block.end = body.size();
        while (inner < body.size()) {
            std::string::size_type innerEnd = body.find('\n', inner);
            if (innerEnd == std::string::npos)
                innerEnd = body.size();
            const std::string_view innerLine(body.data() + inner, innerEnd - inner);
            if (lineOpensASection(innerLine)) {
                block.end = inner;
                break;
            }
            if (const std::optional<std::string> value = quotedValueOnLine(innerLine, "script")) {
                block.script = *value;
                block.hasScript = true;
            }
            inner = innerEnd + 1;
        }
        found.push_back(std::move(block));
        cursor = found.back().end;
    }
    return found;
}

// Rewrites the block naming `script` to `replacement`, back to front so the
// earlier blocks' offsets stay valid. A duplicate naming the same script is
// deleted rather than rewritten twice: the route answers from one of them, so
// leaving both would leave the file saying something it cannot mean.
//
// Returns whether any block named the script.
bool replaceBlocksNaming(std::string& body, std::string_view script, const std::string& replacement)
{
    const std::vector<FoundBlock> blocks = findLanguageFallbackBlocks(body);
    std::vector<std::size_t> matches;
    for (std::size_t index = 0; index < blocks.size(); ++index) {
        if (blocks[index].hasScript && sameScript(blocks[index].script, script))
            matches.push_back(index);
    }
    if (matches.empty())
        return false;

    // Back to front, so a replacement does not move the blocks still to come.
    for (auto it = matches.rbegin(); it != matches.rend(); ++it) {
        const FoundBlock& block = blocks[*it];
        // Only the earliest keeps the replacement; the rest go.
        std::string text = (*it == matches.front()) ? replacement : std::string();
        // The blocks render with a leading newline for appending; spliced in
        // place they follow one already, so the first is dropped.
        if (!text.empty() && text.front() == '\n')
            text.erase(0, 1);
        body.replace(block.start, block.end - block.start, text);
    }
    return true;
}

} // namespace

std::string renderLanguageFallbackBlock(std::string_view script, std::string_view family)
{
    std::string out = "\n";
    out += languageFallbackHeader;
    out += "\nscript = \"";
    out += escapeTOMLBasicString(script);
    out += "\"\nmode   = \"custom\"\nfamily = \"";
    out += escapeTOMLBasicString(family);
    out += "\"\n";
    return out;
}

std::string spliceLanguageFallback(std::string body, std::string_view script, std::string_view family)
{
    if (script.empty() || family.empty())
        return body;
    const std::string block = renderLanguageFallbackBlock(script, family);
    if (replaceBlocksNaming(body, script, block))
        return body;
    body += block;
    return body;
}

std::string removeLanguageFallback(std::string body, std::string_view script)
{
    if (script.empty())
        return body;
    replaceBlocksNaming(body, script, std::string());
    return body;
}

} // namespace Ubiquitty::Preferences
