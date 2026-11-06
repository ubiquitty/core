// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Session/ProcessState.h"

namespace Ubiquitty::Session {

// Assign and report whether the value moved — the per-field change test the
// reducer sums into its render-visible `changed`.
static bool assign(std::string& destination, const std::string& source)
{
    if (destination == source)
        return false;
    destination = source;
    return true;
}

bool applyProcessEvent(ProcessState& state, const ProcessEvent& event)
{
    bool changed = false;
    switch (event.kind) {
    case ProcessEventKind::LaunchSeed:
        changed = assign(state.launchSeed, event.text);
        break;
    case ProcessEventKind::PollResult:
        state.lastPollTick = event.tick;
        // The pgid is internal (it gates the chain rewalk); update it but do not
        // count it as a render-visible change on its own.
        state.foregroundProcessGroup = event.foregroundProcessGroup;
        changed |= assign(state.foregroundName, event.foregroundName);
        changed |= assign(state.currentDirectory, event.currentDirectory);
        // The newest sample always wins — the reducer never latches a transient
        // chain (a stale latch is a rewalk-guard bug upstream of
        // here; the value semantics make a stale chain impossible to retain).
        changed |= assign(state.processChain, event.processChain);
        changed |= assign(state.processChainWithArguments, event.processChainWithArguments);
        // A rendered change: the chain already carries the destination, and a
        // window tag matching on it has to be re-resolved when it moves.
        if (state.remoteTarget.kind != event.remoteTarget.kind
            || state.remoteTarget.name != event.remoteTarget.name
            || state.remoteTarget.user != event.remoteTarget.user
            || state.remoteTarget.via != event.remoteTarget.via) {
            state.remoteTarget = event.remoteTarget;
            changed = true;
        }
        break;
    case ProcessEventKind::OSCTitle:
        changed = assign(state.oscTitle, event.text);
        break;
    case ProcessEventKind::TmuxRename:
        changed = assign(state.tmuxWindowName, event.text);
        break;
    }
    if (changed) {
        ++state.generation;
        state.lastChangeTick = event.tick;
    }
    return changed;
}

std::string_view appTitleForSurface(std::string_view tmuxWindowName, std::string_view oscTitle, TitleSurface surface)
{
    // A window title is the pane's own; only a tab prefers the window's name.
    // The name is never merged into the pane's Terminal — this chooses what the
    // surface SHOWS, and the pane keeps saying what it said. Twin of
    // Protocol::Tmux::chooseTabTitle, which states the same line for the tiler:
    // that module is L1 and this is L2, and the Linux arm trims it, so neither
    // can call the other.
    if (surface != TitleSurface::Tab)
        return oscTitle;
    return !tmuxWindowName.empty() ? tmuxWindowName : oscTitle;
}

ResolvedAppTitle resolveAppTitle(const ProcessState& state, TitleSurface surface)
{
    const std::string_view chosen = appTitleForSurface(state.tmuxWindowName, state.oscTitle, surface);
    if (chosen.empty())
        return { std::string_view {}, TitleSource::None };
    // Which source won is decided by what the chooser returned: on a tab a
    // non-empty window name always wins, so anything else came from the pane.
    const bool isTmuxName = surface == TitleSurface::Tab && !state.tmuxWindowName.empty();
    return { chosen, isTmuxName ? TitleSource::TmuxRename : TitleSource::OSCTitle };
}

ResolvedTitleName resolveTitleNames(const TitleNames& names, TitleSurface surface)
{
    if (surface == TitleSurface::Pane) {
        if (!names.paneLabel.empty())
            return { .replacement = std::string(names.paneLabel), .appTitle = {}, .isCustomName = false };
        return { .replacement = {}, .appTitle = std::string(appTitleForSurface(names.tmuxWindowName, names.oscTitle, surface)), .isCustomName = false };
    }
    if (surface == TitleSurface::Tab && !names.tabTitle.empty())
        return { .replacement = {}, .appTitle = std::string(names.tabTitle), .isCustomName = true };
    if (!names.windowTitle.empty())
        return { .replacement = {}, .appTitle = std::string(names.windowTitle), .isCustomName = true };
    return { .replacement = {},
        .appTitle = std::string(appTitleForSurface(names.tmuxWindowName, names.oscTitle, surface)),
        .isCustomName = false };
}

} // namespace Ubiquitty::Session
