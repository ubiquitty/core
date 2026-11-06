// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// Writing `[[font.language_fallbacks]]` back to config.toml: the per-script
// font choices the route consults after the range map and the Han stage.

#include <string>
#include <string_view>

namespace Ubiquitty::Preferences {

/// Renders one `[[font.language_fallbacks]]` block, with a leading newline so
/// it appends cleanly after whatever precedes it.
///
/// Always `mode = "custom"`, because a written fallback names a family: the
/// other modes describe a routing policy nobody picks a font for, and the
/// reader drops a custom entry whose family is empty.
///
/// No `language` is written. The route matches a script fallback on its script
/// alone, so a language would be a value the file carries and nothing reads.
///
/// - Parameters:
///   - script: The script token, as the reader spells one (han, arabic, …).
///   - family: The family to route that script to.
/// - Returns: The block text.
[[nodiscard]] std::string renderLanguageFallbackBlock(std::string_view script, std::string_view family);

/// Routes `script` to `family`, rewriting `body`.
///
/// One entry per script, last write winning: two entries naming one script
/// would leave the route answering from whichever came first, which is not a
/// thing a person choosing in a list means to express. An existing entry for
/// the same script is replaced in place, so its position in the file is kept.
///
/// A block for another script is not touched — not reparsed, not reformatted
/// and not moved — so comments and hand-authored spelling inside it survive.
///
/// - Parameters:
///   - body: The whole config file.
///   - script: The script token to route.
///   - family: The family to route it to.
/// - Returns: The rewritten file.
[[nodiscard]] std::string spliceLanguageFallback(std::string body, std::string_view script, std::string_view family);

/// Removes the entry naming `script`, if there is one.
///
/// Deleting rather than writing an explicit no-op: a file that accumulates
/// inert entries is worse to read, and the absence of one is what "no script
/// override" already means.
///
/// - Parameters:
///   - body: The whole config file.
///   - script: The script token to stop routing.
/// - Returns: The rewritten file.
[[nodiscard]] std::string removeLanguageFallback(std::string body, std::string_view script);

} // namespace Ubiquitty::Preferences
