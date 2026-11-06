// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
//
// Source/Core/Title/Truncate.h — semantic truncation for window/tab/pane titles.
//
// A title bar that runs out of room must shorten its text. The naive fix — a
// hard tail cut, or the toolkit's blind "middle ellipsis" that counts characters
// with no idea what they mean — throws away the most identifying part: the last
// path component, the specific filename, the branch name. This shortens with
// intent instead:
//
//   * truncatePath collapses the MIDDLE path components to an ellipsis while
//     preserving the leading anchor (`~`, or `/` for an absolute root) and as
//     many of the deepest (most specific) trailing components as fit —
//     `~/Projects/ubiquitty/Source/Core/VT100` → `~/…/Core/VT100`, not
//     `~/Projects/ubiqui…`. Only when a single component alone overflows does it
//     fall back to a middle ellipsis inside that component.
//   * truncateMiddle keeps the head and the tail of an arbitrary string, eliding
//     the middle — a generic title keeps both its app-name prefix and its
//     detail suffix.
//
// Both are pure, allocation-light std::string functions with no platform types,
// promoted here so the macOS and Linux title pipelines shorten identically (and
// the host test pins the elision points). Budget is counted in UTF-8 scalar
// values, and the functions never split a multi-byte sequence; final
// pixel-width fitting for a proportional bar font stays the frontend's last pass.
//
// The ellipsis is U+2026 (…), one scalar wide.

#include <cstddef>
#include <string>
#include <string_view>

namespace Ubiquitty::Title {

// Count of UTF-8 scalar values (a byte is a scalar start unless it is a
// 0b10xxxxxx continuation byte). Invalid/truncated UTF-8 is counted leniently by
// its lead bytes — a title is display text, never a trust boundary.
[[nodiscard]] inline std::size_t titleScalarLength(std::string_view text)
{
    std::size_t count = 0;
    for (unsigned char byte : text) {
        if ((byte & 0xC0) != 0x80)
            ++count;
    }
    return count;
}

// Byte offset at which the `index`-th scalar begins; text.size() when `index` is
// at or past the end. Used to slice on scalar boundaries.
[[nodiscard]] inline std::size_t titleScalarOffset(std::string_view text, std::size_t index)
{
    std::size_t seen = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) {
            if (seen == index)
                return i;
            ++seen;
        }
    }
    return text.size();
}

// The longest prefix of `text` at most `maxBytes` bytes long that ends on a
// scalar boundary. A byte budget — a fixed buffer — must not split a multi-byte
// sequence, which a plain byte cut does as soon as the text is not ASCII.
[[nodiscard]] inline std::string_view titleBytePrefix(std::string_view text, std::size_t maxBytes)
{
    if (text.size() <= maxBytes)
        return text;
    std::size_t cut = maxBytes;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
        --cut;
    return text.substr(0, cut);
}

// The first `count` scalars of `text`.
[[nodiscard]] inline std::string titleHead(std::string_view text, std::size_t count)
{
    return std::string(text.substr(0, titleScalarOffset(text, count)));
}

// The last `count` scalars of `text`.
[[nodiscard]] inline std::string titleTail(std::string_view text, std::size_t count)
{
    const std::size_t length = titleScalarLength(text);
    if (count >= length)
        return std::string(text);
    return std::string(text.substr(titleScalarOffset(text, length - count)));
}

// Keep the head and tail of `text`, eliding the middle, so the result fits in
// `maxScalars` display scalars. Returns `text` unchanged when it already fits.
// Degenerate budgets: 0 → empty; 1 → the ellipsis alone.
[[nodiscard]] inline std::string truncateMiddle(std::string_view text, std::size_t maxScalars)
{
    const std::size_t length = titleScalarLength(text);
    if (length <= maxScalars)
        return std::string(text);
    if (maxScalars == 0)
        return std::string {};
    if (maxScalars == 1)
        return "…";
    // Reserve one scalar for the ellipsis; give the tail the odd extra, since a
    // title's suffix is usually the more identifying end.
    const std::size_t available = maxScalars - 1;
    const std::size_t headCount = available / 2;
    const std::size_t tailCount = available - headCount;
    return titleHead(text, headCount) + "…" + titleTail(text, tailCount);
}

// Shorten a (typically ~-collapsed) path to `maxScalars` by dropping whole
// middle components: keep the leading anchor and the deepest trailing components
// that fit, joining across the gap with `/…/`. A path with no separator, or one
// whose final component alone overflows, falls back to truncateMiddle.
[[nodiscard]] inline std::string truncatePath(std::string_view path, std::size_t maxScalars)
{
    if (titleScalarLength(path) <= maxScalars)
        return std::string(path);

    // Split into components on '/'. An absolute path yields a leading empty
    // component (the root), a ~-path yields "~" first.
    const auto firstSlash = path.find('/');
    if (firstSlash == std::string_view::npos)
        return truncateMiddle(path, maxScalars); // single component, no structure to exploit

    const std::string_view anchor = path.substr(0, firstSlash); // "~", or "" for a root path
    const std::string_view rest = path.substr(firstSlash + 1); // components after the anchor

    // Collect the rest's components.
    std::string components[64];
    int componentCount = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= rest.size(); ++i) {
        if (i == rest.size() || rest[i] == '/') {
            if (componentCount < 64)
                components[componentCount++] = std::string(rest.substr(start, i - start));
            start = i + 1;
        }
    }
    // With at most one component after the anchor there is no middle to drop, so
    // a "/…/" gap would be a lie — shorten the whole path in the middle instead.
    if (componentCount <= 1)
        return truncateMiddle(path, maxScalars);

    // "<anchor>/…/" prefix cost (anchor + slash + ellipsis + slash). For a root
    // path the anchor is empty, so the form is "/…/".
    const std::string gap = std::string(anchor) + "/…/";
    const std::size_t gapCost = titleScalarLength(gap);

    // Greedily keep the deepest components that still fit after the gap.
    std::string bestTail;
    std::size_t keptCost = 0;
    bool didKeepTail = false;
    for (int k = componentCount - 1; k >= 1; --k) { // never re-include everything (k>=1 keeps a gap)
        std::string tail = components[k];
        for (int j = k + 1; j < componentCount; ++j) {
            tail += "/";
            tail += components[j];
        }
        keptCost = titleScalarLength(tail);
        if (gapCost + keptCost <= maxScalars) {
            bestTail = tail;
            didKeepTail = true;
        } else {
            break; // adding an earlier component only grows the tail
        }
    }

    if (didKeepTail)
        return gap + bestTail;

    // Even the last component alone overflows: middle-truncate it under the gap.
    const std::string last = components[componentCount - 1];
    const std::size_t room = maxScalars > gapCost ? maxScalars - gapCost : 1;
    return gap + truncateMiddle(last, room);
}

} // namespace Ubiquitty::Title
