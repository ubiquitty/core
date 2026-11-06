// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
//
// Source/Core/App/BannerTitle.h — a window's or a tab's title, as a banner's
// place line names it.
//
// The title on a tab is composed for the strip it sits in: it leads with its
// host so tabs of two machines read apart, and with a pin and a tag. A window's
// is composed for a title row, which draws the dimensions and a program's
// status beside the text and spaces a bare dash itself. A place line states the
// host as a level of its own, has no row to draw anything beside, and tells a
// window from a tab by whether their titles differ — so it needs both titles
// in one form, carrying only what names the thing.
//
// That form is `composeTitle`'s own, asked for without the parts that belong
// to the surface rather than to the name. Nothing is stripped from a finished
// title: a host mark removed as text would also remove a directory that
// happens to be spelled the same.
//

#include "Core/App/TitleCompose.h"

#include <string>
#include <utility>

namespace Ubiquitty::App {

/// Composes the title a banner's place line names a window or a tab by.
///
/// The title its surface shows, less what the surface adds around the name:
/// no host mark, pin or tag; no dimensions or program status, which change
/// under a name without changing what it names; and a window's parts joined
/// with spaces, as a tab's are. A window and a tab that weave the same parts
/// therefore compose the same text, which is how the place line knows to
/// write it once.
///
/// - Parameters:
///   - pane: The facts of the pane that names the surface.
///   - viewer: What the reader adds: its profile and its fallback.
///   - composition: The viewer's `[title]` composition for the surface.
///   - surface: The window or the tab.
/// - Returns: The title.
[[nodiscard]] inline std::string composeBannerTitle(PaneTitleFacts pane, TitleViewerFacts viewer, Preferences::Effective::TitleComposition composition, Session::TitleSurface surface)
{
    pane.hostName.clear();
    viewer.isPinned = false;
    viewer.doPaneHeadersNameHost = false;
    viewer.windowTitleForm = WindowTitleForm::PlainText;
    composition.showsDimensions = false;
    composition.showsProgramStatus = false;
    return composeTitle(pane, viewer, composition, surface).title;
}

} // namespace Ubiquitty::App
