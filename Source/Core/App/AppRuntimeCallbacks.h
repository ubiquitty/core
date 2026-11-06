// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// AppRuntimeCallbacks.h — Core's application runtime over a host a Swift
// frontend supplies.
//
// `AppRuntimeHost` is a C++ interface, which Swift cannot subclass. This host
// forwards each of its steps to a table of C function pointers a Swift
// frontend fills with `@convention(c)` closures and an unmanaged context, and
// owns the runtime that calls it, so the two share one lifetime.
//

#include "Base/SwiftInterop.h"
#include "Core/App/AppRuntime.h"
#include "Core/Preferences/AppearanceMode.h"

#include <atomic>

namespace Ubiquitty::App {
class HostedAppRuntime;
}

/// Claims a reference to `runtime` on Swift's behalf.
///
/// Declared at global scope because the Swift importer resolves the retain and
/// release named by `UBIQUITTY_SHARED_REFERENCE` there and nowhere else.
///
/// - Parameter runtime: The runtime to retain. A null pointer is ignored.
inline void retainHostedAppRuntime(Ubiquitty::App::HostedAppRuntime* runtime);

/// Drops a reference to `runtime`, destroying it at zero.
///
/// - Parameter runtime: The runtime to release. A null pointer is ignored.
inline void releaseHostedAppRuntime(Ubiquitty::App::HostedAppRuntime* runtime);

namespace Ubiquitty::App {

// The seam's function types have C language linkage: a Swift host fills them
// with `@convention(c)` functions.
extern "C" {

/// Applies a theme to every open window without persisting it.
using AppRuntimeApplyTheme = void (*)(void* context, int themeIndex);

/// One of the host's steps that carries no argument.
using AppRuntimeHostStep = void (*)(void* context);
}

/// The host half of the runtime, as a table of C function pointers.
struct AppRuntimeOperations {
    /// The host's own state, handed back to every operation unchanged.
    void* context = nullptr;
    /// See `AppRuntimeHost::applyThemeToAllWindows`.
    AppRuntimeApplyTheme applyThemeToAllWindows = nullptr;
    /// See `AppRuntimeHost::reloadPreferencesFromFile`.
    AppRuntimeHostStep reloadPreferencesFromFile = nullptr;
    /// See `AppRuntimeHost::rebuildConfigMenus`.
    AppRuntimeHostStep rebuildConfigMenus = nullptr;
    /// See `AppRuntimeHost::reapplyPersistedSettings`.
    AppRuntimeHostStep reapplyPersistedSettings = nullptr;
};

/// `AppRuntime` over a table of operations.
///
/// Every call runs on the thread that makes it; the runtime holds no thread of
/// its own.
class HostedAppRuntime final : private AppRuntimeHost {
public:
    /// Creates a runtime over `operations`.
    ///
    /// - Parameter operations: The host table. Every entry must be set.
    /// - Returns: A runtime holding one reference.
    [[nodiscard]] static HostedAppRuntime* create(AppRuntimeOperations operations) UBIQUITTY_RETURNS_RETAINED
    {
        return new HostedAppRuntime(operations);
    }

    HostedAppRuntime(const HostedAppRuntime&) = delete;
    HostedAppRuntime& operator=(const HostedAppRuntime&) = delete;

    /// Selects a theme; see `AppRuntime::selectTheme`.
    ///
    /// - Parameters:
    ///   - themeIndex: The registry index of the theme.
    ///   - selection: Whether the pick is manual or follows the appearance.
    void selectTheme(int themeIndex, ThemeSelection selection) { m_runtime.selectTheme(themeIndex, selection); }

    /// Reports a system appearance change; see `AppRuntime::onSystemAppearanceChanged`.
    ///
    /// - Parameter scheme: The appearance the system switched to.
    void onSystemAppearanceChanged(Preferences::ColorScheme scheme) { m_runtime.onSystemAppearanceChanged(scheme); }

    /// See `AppRuntime::onConfigFileChanged`.
    void onConfigFileChanged() { m_runtime.onConfigFileChanged(); }

    /// See `AppRuntime::autoThemeIndex`.
    [[nodiscard]] int autoThemeIndex(Preferences::ColorScheme scheme) const { return m_runtime.autoThemeIndex(scheme); }

    /// See `AppRuntime::themeIndexForSystemAppearance`.
    [[nodiscard]] int themeIndexForSystemAppearance(Preferences::ColorScheme scheme) const { return m_runtime.themeIndexForSystemAppearance(scheme); }

    /// The references Swift holds; see `retainHostedAppRuntime`.
    std::atomic<int> swiftReferences { 1 };

private:
    explicit HostedAppRuntime(AppRuntimeOperations operations)
        : m_operations(operations)
        , m_runtime(*this)
    {
    }

    /// Forwards to the host table's `applyThemeToAllWindows`.
    ///
    /// - Parameter themeIndex: The registry index of the theme to apply.
    void applyThemeToAllWindows(int themeIndex) override { m_operations.applyThemeToAllWindows(m_operations.context, themeIndex); }
    /// Forwards to the host table's `reloadPreferencesFromFile`.
    void reloadPreferencesFromFile() override { m_operations.reloadPreferencesFromFile(m_operations.context); }
    /// Forwards to the host table's `rebuildConfigMenus`.
    void rebuildConfigMenus() override { m_operations.rebuildConfigMenus(m_operations.context); }
    /// Forwards to the host table's `reapplyPersistedSettings`.
    void reapplyPersistedSettings() override { m_operations.reapplyPersistedSettings(m_operations.context); }

    AppRuntimeOperations m_operations;
    AppRuntime m_runtime;
} UBIQUITTY_SHARED_REFERENCE(retainHostedAppRuntime, releaseHostedAppRuntime);

} // namespace Ubiquitty::App

/// Adds one Swift reference to a hosted runtime.
///
/// - Parameter runtime: The runtime, or null for no effect.
inline void retainHostedAppRuntime(Ubiquitty::App::HostedAppRuntime* runtime)
{
    if (runtime)
        runtime->swiftReferences.fetch_add(1, std::memory_order_relaxed);
}

/// Drops one Swift reference, destroying the runtime with the last one.
///
/// - Parameter runtime: The runtime, or null for no effect.
inline void releaseHostedAppRuntime(Ubiquitty::App::HostedAppRuntime* runtime)
{
    if (runtime && runtime->swiftReferences.fetch_sub(1, std::memory_order_acq_rel) == 1)
        delete runtime;
}
