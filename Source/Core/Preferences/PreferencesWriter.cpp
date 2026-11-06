// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Emitting TOML and editing a config file in place. A write preserves the comments,
// key order and formatting already in the file: a key that exists is replaced on its own
// line, a key that does not is appended under its section, and a named block is spliced
// whole. The file on disk is a document a person edits, not a serialization of Values.

#include "Core/Preferences/Preferences.h"
#include "Core/Preferences/TOMLString.h"

#include "Core/Preferences/Internal.h"

#include "PAL/PAL.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Ubiquitty::Preferences {

std::string tomlInt(long long v)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%lld", v);
    return { buffer };
}

std::string tomlBool(bool isTrue) { return isTrue ? std::string("true") : std::string("false"); }

std::string tomlDouble(double value)
{
    // %.6g drops trailing zeros while preserving precision for menu-set
    // values (half-points, percents); guarantee a decimal point so the
    // parser routes the result through the float branch.
    if (std::isnan(value) || std::isinf(value))
        value = 0.0;
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "%.6g", value);
    std::string s(buffer);
    if (s.find('.') == std::string::npos && s.find('e') == std::string::npos && s.find('E') == std::string::npos && s.find("nan") == std::string::npos && s.find("inf") == std::string::npos)
        s += ".0";
    return s;
}

std::string tomlString(std::string_view v)
{
    std::string outValue;
    outValue.reserve(v.size() + 2);
    outValue += '"';
    for (char c : v) {
        switch (c) {
        case '"':
            outValue += "\\\"";
            break;
        case '\\':
            outValue += "\\\\";
            break;
        case '\n':
            outValue += "\\n";
            break;
        case '\r':
            outValue += "\\r";
            break;
        case '\t':
            outValue += "\\t";
            break;
        default:
            outValue += c;
            break;
        }
    }
    outValue += '"';
    return outValue;
}

namespace {

// Read whole file into a list of lines (with trailing newline stripped).
// A missing or unreadable file is indistinguishable from an empty one here, so the
// caller writes a fresh file; the atomic write reports any real I/O failure.
std::vector<std::string> splitLines(const std::string& body)
{
    std::vector<std::string> lines;
    std::string current;
    for (char c : body) {
        if (c == '\n') {
            if (!current.empty() && current.back() == '\r')
                current.pop_back();
            lines.push_back(std::move(current));
            current.clear();
        } else {
            current += c;
        }
    }
    if (!current.empty()) {
        if (current.back() == '\r')
            current.pop_back();
        lines.push_back(std::move(current));
    }
    return lines;
}

bool writeFileAtomic(const std::string& path, const std::string& body)
{
    // PAL::storageWriteAtomic mkdir -p's the parent, stages a sibling temp
    // file, and rename(2)s it over the target so a reader never sees a torn
    // write.
    return PAL::storageWriteAtomic(path.c_str(), std::as_bytes(std::span { body }));
}

// True when `t` (already trimmed) opens a section header — `[name]` or
// `[[name]]`. Writes the bare leaf name into `out` regardless of style.
bool parseHeaderName(const std::string& t, std::string& outValue)
{
    if (t.size() < 3 || t[0] != '[')
        return false;
    const bool isArrayOfTables = (t[1] == '[');
    auto close = isArrayOfTables ? t.find("]]") : t.find(']');
    if (close == std::string::npos)
        return false;
    std::string hdr = t.substr(isArrayOfTables ? 2 : 1, close - (isArrayOfTables ? 2 : 1));
    // trim
    size_t b = 0, e = hdr.size();
    while (b < e && (hdr[b] == ' ' || hdr[b] == '\t'))
        ++b;
    while (e > b && (hdr[e - 1] == ' ' || hdr[e - 1] == '\t'))
        --e;
    hdr = hdr.substr(b, e - b);
    auto dot = hdr.rfind('.');
    outValue = (dot == std::string::npos) ? hdr : hdr.substr(dot + 1);
    return true;
}

// Match a `[# ]<key> = ...` line inside the section body. On match writes
// the new line (preserving the indent + any trailing comment) into `out`.
bool tryReplaceKeyLine(const std::string& line, const std::string& key, const std::string& tomlValue, std::string& outValue)
{
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
        ++i;
    std::string indent = line.substr(0, i);
    // A commented-out key still matches, and the '#' is not reproduced: writing a key
    // that the default file ships disabled is what enables it.
    if (i < line.size() && line[i] == '#') {
        ++i;
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
            ++i;
    }
    if (i + key.size() > line.size())
        return false;
    if (line.compare(i, key.size(), key) != 0)
        return false;
    size_t j = i + key.size();
    // The next non-space char must be '=' for this to be the key line.
    size_t k = j;
    while (k < line.size() && (line[k] == ' ' || line[k] == '\t'))
        ++k;
    if (k >= line.size() || line[k] != '=')
        return false;
    // Preserve a trailing `# comment` after the value. Walk the value
    // text honoring quotes so a `#` inside a string is kept.
    size_t v = k + 1;
    while (v < line.size() && (line[v] == ' ' || line[v] == '\t'))
        ++v;
    size_t hash = std::string::npos;
    bool isInsideString = false;
    for (size_t p = v; p < line.size(); ++p) {
        char ch = line[p];
        if (isInsideString) {
            if (ch == '\\' && p + 1 < line.size()) {
                ++p;
                continue;
            }
            if (ch == '"')
                isInsideString = false;
        } else {
            if (ch == '"')
                isInsideString = true;
            else if (ch == '#') {
                hash = p;
                break;
            }
        }
    }
    std::string trailing;
    if (hash != std::string::npos) {
        // Keep one space before the comment marker.
        trailing = "  ";
        trailing.append(line, hash, std::string::npos);
    }
    outValue = indent + key + " = " + tomlValue + trailing;
    return true;
}

} // namespace

std::string setSectionKey(std::string body, const std::string& section, const std::string& key, const std::string& tomlValue)
{
    std::vector<std::string> lines = splitLines(body);

    // Locate [section] header.
    int sectionStart = -1; // index of header line
    int sectionEnd = static_cast<int>(lines.size()); // one past last body line
    {
        std::string hdrName;
        for (size_t i = 0; i < lines.size(); ++i) {
            const std::string& ln = lines[i];
            // trim leading whitespace for header detection only
            size_t b = 0;
            while (b < ln.size() && (ln[b] == ' ' || ln[b] == '\t'))
                ++b;
            if (b < ln.size() && ln[b] == '[') {
                std::string t = ln.substr(b);
                if (parseHeaderName(t, hdrName)) {
                    if (sectionStart < 0 && hdrName == section) {
                        sectionStart = static_cast<int>(i);
                    } else if (sectionStart >= 0) {
                        sectionEnd = static_cast<int>(i);
                        break;
                    }
                }
            }
        }
    }

    if (sectionStart < 0) {
        // Append the section + the key. Ensure separation from prior content.
        if (!lines.empty() && !lines.back().empty())
            lines.emplace_back();
        lines.push_back("[" + section + "]");
        lines.push_back(key + " = " + tomlValue);
    } else {
        // Scan body lines for an existing (possibly commented) key.
        int hit = -1;
        std::string replacement;
        for (int i = sectionStart + 1; i < sectionEnd; ++i) {
            std::string outValue;
            if (tryReplaceKeyLine(lines[i], key, tomlValue, outValue)) {
                hit = i;
                replacement = std::move(outValue);
                break;
            }
        }
        if (hit >= 0) {
            lines[hit] = replacement;
        } else {
            // Append at the end of the section body, just before any trailing
            // blank lines that separate this section from the next.
            int insertAt = sectionEnd;
            while (insertAt > sectionStart + 1 && lines[insertAt - 1].empty())
                --insertAt;
            lines.insert(lines.begin() + insertAt, key + " = " + tomlValue);
        }
    }

    std::string result;
    for (const auto& line : lines) {
        result += line;
        result += '\n';
    }
    return result;
}

bool writeKeyValue(const std::string& path, const std::string& section, const std::string& key, const std::string& tomlValue)
{
    const std::optional<std::string> body = readWholeFile(path);
    // A missing file is not a failure: the section and the key are appended to
    // an empty body, which is what a first write onto a fresh machine does.
    return writeFileAtomic(path, setSectionKey(body.value_or(std::string {}), section, key, tomlValue));
}

// --- Block-level TOML mutation -----------------------------------------

std::string tomlHexColor(std::uint32_t rgba)
{
    std::uint8_t r = (rgba >> 24) & 0xFF, g = (rgba >> 16) & 0xFF, b = (rgba >> 8) & 0xFF, a = rgba & 0xFF;
    char buffer[12];
    if (a == 255)
        std::snprintf(buffer, sizeof(buffer), "#%02x%02x%02x", r, g, b);
    else
        std::snprintf(buffer, sizeof(buffer), "#%02x%02x%02x%02x", r, g, b, a);
    return { buffer };
}

bool isBindingLine(const std::string& line)
{
    size_t k = 0;
    while (k < line.size() && (line[k] == ' ' || line[k] == '\t'))
        ++k;
    return k < line.size() && line[k] == '"';
}

std::optional<BlockRange> findNamedBlock(const std::string& body, const char* header, const std::string& wanted)
{
    const std::string open = std::string("[[") + header + "]]";
    size_t i = 0;
    while (i < body.size()) {
        size_t lineStart = i;
        size_t lineEnd = body.find('\n', i);
        if (lineEnd == std::string::npos)
            lineEnd = body.size();
        size_t k = lineStart;
        while (k < lineEnd && (body[k] == ' ' || body[k] == '\t'))
            ++k;
        bool isHeader = (lineEnd - k) >= open.size() && body.compare(k, open.size(), open) == 0;
        if (isHeader) {
            size_t current = lineEnd + 1;
            size_t blockEnd = body.size();
            bool match = false;
            while (current < body.size()) {
                size_t le = body.find('\n', current);
                if (le == std::string::npos)
                    le = body.size();
                size_t kk = current;
                while (kk < le && (body[kk] == ' ' || body[kk] == '\t'))
                    ++kk;
                if (kk < le && body[kk] == '[') {
                    blockEnd = current;
                    break;
                }
                if ((le - kk) >= 4 && body.compare(kk, 4, "name") == 0) {
                    size_t eq = body.find('=', kk);
                    if (eq != std::string::npos && eq < le) {
                        size_t q1 = body.find('"', eq);
                        if (q1 != std::string::npos && q1 < le) {
                            size_t q2 = body.find('"', q1 + 1);
                            if (q2 != std::string::npos && q2 < le && body.substr(q1 + 1, q2 - q1 - 1) == wanted)
                                match = true;
                        }
                    }
                }
                current = le + 1;
            }
            if (match)
                return BlockRange { lineStart, blockEnd };
            i = blockEnd;
            continue;
        }
        i = lineEnd + 1;
    }
    return std::nullopt;
}

std::string spliceNamedBlock(std::string body, const char* header, const std::string& name, const std::string& block)
{
    const std::optional<BlockRange> range = findNamedBlock(body, header, name);
    if (!range) {
        body += block;
        return body;
    }
    std::string repl = block;
    if (!repl.empty() && repl[0] == '\n')
        repl.erase(0, 1);
    body.replace(range->start, range->end - range->start, repl);
    return body;
}

std::string removeNamedBlock(std::string body, const char* header, const std::string& name)
{
    if (const std::optional<BlockRange> range = findNamedBlock(body, header, name))
        body.erase(range->start, range->end - range->start);
    return body;
}

namespace {

bool isLineBlank(char c) { return c == ' ' || c == '\t' || c == '\r'; }

// The ranges of the first `limit` `[[header]]` blocks, in text order, as
// findIndexedBlock documents them.
std::vector<BlockRange> indexedBlocks(const std::string& body, std::string_view wanted, std::size_t limit)
{
    std::vector<BlockRange> blocks;
    // The first non-blank offset of the line [start, end), or `end`.
    const auto firstNonBlank = [&](std::size_t start, std::size_t end) {
        while (start < end && isLineBlank(body[start]))
            ++start;
        return start;
    };
    const auto isWantedHeader = [&](std::size_t start, std::size_t end) {
        const std::size_t first = firstNonBlank(start, end);
        if (end - first < 4 || body.compare(first, 2, "[[") != 0)
            return false;
        const std::size_t close = body.find("]]", first + 2);
        if (close == std::string::npos || close >= end)
            return false;
        std::size_t nameStart = first + 2;
        std::size_t nameEnd = close;
        while (nameStart < nameEnd && isLineBlank(body[nameStart]))
            ++nameStart;
        while (nameEnd > nameStart && isLineBlank(body[nameEnd - 1]))
            --nameEnd;
        return std::string_view { body }.substr(nameStart, nameEnd - nameStart) == wanted;
    };
    std::size_t lineStart = 0;
    while (lineStart < body.size() && blocks.size() < limit) {
        std::size_t lineEnd = body.find('\n', lineStart);
        if (lineEnd == std::string::npos)
            lineEnd = body.size();
        if (!isWantedHeader(lineStart, lineEnd)) {
            lineStart = lineEnd + 1;
            continue;
        }
        std::size_t blockEnd = body.size();
        std::size_t current = lineEnd + 1;
        while (current < body.size()) {
            std::size_t next = body.find('\n', current);
            if (next == std::string::npos)
                next = body.size();
            const std::size_t first = firstNonBlank(current, next);
            if (first < next && body[first] == '[') {
                blockEnd = current;
                break;
            }
            current = next + 1;
        }
        blocks.push_back(BlockRange { lineStart, blockEnd });
        lineStart = blockEnd;
    }
    return blocks;
}

} // namespace

std::optional<BlockRange> findIndexedBlock(const std::string& body, const char* header, int index)
{
    if (index < 0)
        return std::nullopt;
    const std::vector<BlockRange> blocks = indexedBlocks(body, header, static_cast<std::size_t>(index) + 1);
    if (blocks.size() != static_cast<std::size_t>(index) + 1)
        return std::nullopt;
    return blocks.back();
}

std::string spliceIndexedBlock(std::string body, const char* header, int index, const std::string& block)
{
    const std::optional<BlockRange> range = findIndexedBlock(body, header, index);
    if (!range) {
        body += block;
        return body;
    }
    std::string replacement = block;
    if (!replacement.empty() && replacement[0] == '\n')
        replacement.erase(0, 1);
    body.replace(range->start, range->end - range->start, replacement);
    return body;
}

std::string removeIndexedBlock(std::string body, const char* header, int index)
{
    if (const std::optional<BlockRange> range = findIndexedBlock(body, header, index))
        body.erase(range->start, range->end - range->start);
    return body;
}

std::string moveIndexedBlock(std::string body, const char* header, int from, int to)
{
    if (from < 0 || to < 0 || from == to)
        return body;
    const std::vector<BlockRange> blocks = indexedBlocks(body, header, std::numeric_limits<std::size_t>::max());
    const std::size_t count = blocks.size();
    const auto source = static_cast<std::size_t>(from);
    const auto destination = static_cast<std::size_t>(to);
    if (source >= count || destination >= count)
        return body;

    // The start of the line holding the byte before `position`, which is a
    // line boundary or the end of the text.
    const auto previousLineStart = [&](std::size_t position) -> std::size_t {
        if (position <= 1)
            return 0;
        const std::size_t newline = body.rfind('\n', position - 2);
        return newline == std::string::npos ? 0 : newline + 1;
    };
    // The line [start, position) without its newline, read for what it holds.
    const auto firstNonBlankOf = [&](std::size_t start, std::size_t position) {
        std::size_t end = position;
        if (end > start && body[end - 1] == '\n')
            --end;
        while (start < end && isLineBlank(body[start]))
            ++start;
        return std::pair { start, end };
    };
    const auto isCommentLine = [&](std::size_t start, std::size_t position) {
        const auto [first, end] = firstNonBlankOf(start, position);
        return first < end && body[first] == '#';
    };
    const auto isBlankLine = [&](std::size_t start, std::size_t position) {
        const auto [first, end] = firstNonBlankOf(start, position);
        return first == end;
    };

    // What moves with each block: the comments directly above its header,
    // the header, and its lines down to the blank lines that end it, short of
    // the comments that lead whatever header follows.
    std::vector<BlockRange> spans;
    spans.reserve(count);
    for (const BlockRange& block : blocks) {
        const std::size_t headerNewline = body.find('\n', block.start);
        const std::size_t afterHeader = headerNewline == std::string::npos ? body.size() : headerNewline + 1;
        std::size_t start = block.start;
        while (start > 0) {
            const std::size_t above = previousLineStart(start);
            if (!isCommentLine(above, start))
                break;
            start = above;
        }
        std::size_t end = block.end;
        if (end < body.size()) {
            while (end > afterHeader) {
                const std::size_t above = previousLineStart(end);
                if (!isCommentLine(above, end))
                    break;
                end = above;
            }
        }
        while (end > afterHeader) {
            const std::size_t above = previousLineStart(end);
            if (!isBlankLine(above, end))
                break;
            end = above;
        }
        spans.push_back(BlockRange { start, end });
    }

    std::vector<std::size_t> order(count);
    for (std::size_t index = 0; index < count; ++index)
        order[index] = index;
    order.erase(order.begin() + static_cast<std::ptrdiff_t>(source));
    order.insert(order.begin() + static_cast<std::ptrdiff_t>(destination), source);

    std::string out;
    out.reserve(body.size() + count);
    out.append(body, 0, spans.front().start);
    for (std::size_t slot = 0; slot < count; ++slot) {
        const BlockRange& moved = spans[order[slot]];
        out.append(body, moved.start, moved.end - moved.start);
        // A block that ended the file without a newline is given one, so the
        // spacing after its new slot reads as it did.
        if (out.empty() || out.back() != '\n')
            out.push_back('\n');
        const std::size_t gapEnd = slot + 1 < count ? spans[slot + 1].start : body.size();
        out.append(body, spans[slot].end, gapEnd - spans[slot].end);
    }
    return out;
}

std::string removeSectionKey(std::string body, const std::string& section, const std::string& key)
{
    bool isInSection = false;
    size_t current = 0;
    while (current < body.size()) {
        const size_t newline = body.find('\n', current);
        const size_t lineEnd = newline == std::string::npos ? body.size() : newline;
        const size_t next = newline == std::string::npos ? body.size() : newline + 1;
        std::string_view line(body.data() + current, lineEnd - current);
        const size_t first = line.find_first_not_of(" \t\r");
        line = first == std::string_view::npos ? std::string_view() : line.substr(first);
        if (line.starts_with('[')) {
            isInSection = false;
            const size_t close = line.find(']');
            if (!line.starts_with("[[") && close != std::string_view::npos) {
                const std::string_view name = line.substr(1, close - 1);
                const size_t nameStart = name.find_first_not_of(" \t");
                const size_t nameEnd = name.find_last_not_of(" \t");
                isInSection = nameStart != std::string_view::npos && name.substr(nameStart, nameEnd - nameStart + 1) == section;
            }
        } else if (isInSection && line.starts_with(key)) {
            const size_t equals = line.find_first_not_of(" \t", key.size());
            if (equals != std::string_view::npos && line[equals] == '=') {
                body.erase(current, next - current);
                continue;
            }
        }
        current = next;
    }
    return body;
}

std::string rewriteKeysSection(std::string body, const std::vector<std::string>& lines)
{
    // Find the [keys] header line.
    size_t headerStart = std::string::npos, headerEnd = 0;
    {
        size_t i = 0;
        while (i < body.size()) {
            size_t ls = i;
            size_t le = body.find('\n', i);
            if (le == std::string::npos)
                le = body.size();
            size_t k = ls;
            while (k < le && (body[k] == ' ' || body[k] == '\t'))
                ++k;
            if ((le - k) >= 6 && body.compare(k, 6, "[keys]") == 0) {
                headerStart = ls;
                headerEnd = le;
                break;
            }
            i = le + 1;
        }
    }

    std::string joined;
    for (const std::string& l : lines) {
        joined += l;
        joined += "\n";
    }

    if (headerStart == std::string::npos) {
        if (!body.empty() && body.back() != '\n')
            body.push_back('\n');
        body += "\n[keys]\n";
        body += joined;
        return body;
    }

    // Section body runs from after the header line to the next section
    // header (line starting with '[') or EOF.
    size_t bodyStart = (headerEnd < body.size()) ? headerEnd + 1 : body.size();
    size_t sectionEnd = body.size();
    {
        size_t current = bodyStart;
        while (current < body.size()) {
            size_t le = body.find('\n', current);
            if (le == std::string::npos)
                le = body.size();
            size_t k = current;
            while (k < le && (body[k] == ' ' || body[k] == '\t'))
                ++k;
            if (k < le && body[k] == '[') {
                sectionEnd = current;
                break;
            }
            current = le + 1;
        }
    }

    // Keep every non-binding line of the existing section body.
    std::string kept;
    {
        size_t current = bodyStart;
        while (current < sectionEnd) {
            size_t le = body.find('\n', current);
            bool hasNL = (le != std::string::npos && le < sectionEnd);
            size_t lineEnd = hasNL ? le : sectionEnd;
            std::string line = body.substr(current, lineEnd - current);
            if (!isBindingLine(line)) {
                kept += line;
                kept += "\n";
            }
            current = lineEnd + 1;
            if (!hasNL)
                break;
        }
    }

    std::string replacement = kept + joined;
    body.replace(bodyStart, sectionEnd - bodyStart, replacement);
    return body;
}

} // namespace Ubiquitty::Preferences
