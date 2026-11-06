// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Preferences/PreferencesAccessors.h"

namespace Ubiquitty::Runtime {

/// The platform-visible consumers of one pane's process metadata.
struct ProcessObservation {
    bool isGroupVisible { false };
    bool isWindowTitleVisible { false };
    bool isTabTitleVisible { false };
    bool isPaneTitleVisible { false };
    bool isWindowTitleReplaced { false };
    bool isTabTitleReplaced { false };
    bool isPaneTitleReplaced { false };
    bool isInspectorOpen { false };
    /// The window's close button can show the running-job marker, which is
    /// read from the process sample and so needs one taken.
    bool isCloseMarkerVisible { false };
};

/// Resolves whether a displayed consumer needs process discovery.
///
/// Rendering visibility and metadata visibility are independent: an inactive
/// tab can display a title, and an inspector can observe a covered terminal.
///
/// - Parameters:
///   - observation: The platform's consumer visibility facts.
///   - configuration: The effective title configuration.
/// - Returns: Whether process discovery has a reader.
[[nodiscard]] inline bool needsProcessObservation(const ProcessObservation& observation, const Preferences::Values& configuration)
{
    if (observation.isInspectorOpen) {
        return true;
    }
    if (!observation.isGroupVisible) {
        return false;
    }
    // Only the "running" policy asks the sample; "always" and "never" answer
    // without one.
    if (observation.isCloseMarkerVisible && Preferences::Effective::closeConfirmation(configuration) == "running") {
        return true;
    }
    const auto needsProcesses = [](const Preferences::Effective::TitleComposition& composition) {
        return composition.showsProcess || composition.showsShellCommand;
    };
    return (observation.isWindowTitleVisible && !observation.isWindowTitleReplaced
               && needsProcesses(Preferences::Effective::windowTitleComposition(configuration)))
        || (observation.isTabTitleVisible && !observation.isTabTitleReplaced
            && needsProcesses(Preferences::Effective::tabTitleComposition(configuration)))
        || (observation.isPaneTitleVisible && !observation.isPaneTitleReplaced
            && needsProcesses(Preferences::Effective::paneTitleComposition(configuration)));
}

} // namespace Ubiquitty::Runtime
