// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Selection/CopyClipboard.h"

#include "Core/CellColor.h"
#include "Core/Preferences/PreferencesAccessors.h"
#include "Core/Screen/ScrollSearch.h"
#include "Core/Terminal.h"
#include "Core/Themes/ThemePreferences.h"
#include "Core/Unicode/Normalize.h"

#include <string>
#include <string_view>

namespace Ubiquitty::Selection {

CopyStyle copyStyle(const Preferences::Values& values) noexcept
{
    const std::string_view name = values.copyStyle ? std::string_view(*values.copyStyle) : std::string_view {};
    // Both spellings: the file template documents "no-background" and
    // "styles-only", the window controller compared the underscored forms, and
    // a reader who copied the template got the everything flavour.
    if (name == "plain")
        return CopyStyle::PlainText;
    if (name == "no_background" || name == "no-background")
        return CopyStyle::NoBackground;
    if (name == "styles_only" || name == "styles-only")
        return CopyStyle::StylesOnly;
    return CopyStyle::Everything;
}

CopyStyle copyStyle(const Preferences::Values& values, CopyStyle requested) noexcept
{
    return requested == CopyStyle::Configured ? copyStyle(values) : requested;
}

StyledClipboard copyClipboard(Session& session, Terminal& terminal, const Preferences::Values& values, CopyStyle requested, const StyledFont& font)
{
    const CopyStyle style = copyStyle(values, requested);
    StyledClipboard built;
    if (style == CopyStyle::PlainText) {
        built.plain = copiedText(session, terminal);
    } else {
        CellPalette live = CellPalette::fromTerminalPalette(terminal.palette);
        live.brightenBold = Preferences::Effective::brightenBold(values);
        built = styledClipboard(session, terminal, styleColorMode(style), Themes::copyPalette(values, live), font);
    }
    // Normalise-on-copy: when [unicode] normalization is set, the plain text is
    // copied in that form, the one search uses, so a paste lands in a single
    // canonical spelling. The rich flavours carry the cells as drawn.
    const Unicode::NormalizationForm form = Screen::ScrollSearch::normalizationForm();
    if (form != Unicode::NormalizationForm::None && !built.plain.empty())
        built.plain = Unicode::normalizeUTF8(built.plain, form);
    return built;
}

} // namespace Ubiquitty::Selection
