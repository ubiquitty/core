// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// Unicode normalization (NFC / NFD / NFKC / NFKD) per UAX #15. Hangul is
// algorithmic (UAX #15 §18); every other mapping comes from the generated
// Normalize.tables.inc. Runs on search and copy, not per-cell ingest.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Ubiquitty::Unicode {

// The four normalization forms plus the no-op default.
// None == 0 so a zero-initialized value means "do not normalize".
enum class NormalizationForm : std::uint8_t {
    None = 0,
    NFC, // canonical decomposition then canonical composition
    NFD, // canonical decomposition
    NFKC, // compatibility decomposition then canonical composition
    NFKD, // compatibility decomposition
};

// Parse a config token ("none" | "nfc" | "nfd" | "nfkc" | "nfkd") into a form.
// Returns std::nullopt on an unrecognized token so the caller keeps its default.
// Case- and separator-insensitive, like invalidUTF8PolicyFromName.
[[nodiscard]] std::optional<NormalizationForm> normalizationFormFromName(std::string_view name);

// Canonical lowercase token for a form (round-trips normalizationFormFromName).
[[nodiscard]] const char* normalizationFormToName(NormalizationForm form);

// Normalize a UTF-8 string to the given form. form == None returns the input
// unchanged. Invalid UTF-8 is passed through byte-for-byte (each undecodable
// byte is preserved), so this never corrupts non-UTF-8 search text.
[[nodiscard]] std::string normalizeUTF8(std::string_view input, NormalizationForm form);

// Normalize a run of code points in place to the given form. The vector is
// rewritten with the normalized scalar sequence. form == None is a no-op.
void normalizeCodePoints(std::vector<std::uint32_t>& codePoints, NormalizationForm form);

} // namespace Ubiquitty::Unicode
