// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// The Rule-10 negative probe. It is compiled with the model layer's stage as its
// only include directory.
//
// Each test uses __has_include rather than #include. An #include fails as soon
// as any header it pulls in is missing, so a boundary that holds and a header
// that drags in something unstaged would fail the same way and tell nothing
// apart. __has_include asks the one question that matters: whether the model
// stage can reach the behaviour header at all.
#if __has_include("Core/Preferences/Preferences.h")
#error "Core/Preferences/Preferences.h can be reached from the model stage, so the Rule-10 boundary does not hold. Look at the module lists Scripts/check-core-layer-partition.sh emits and Scripts/cmake/UbiquittyCoreLibrary.cmake stages: an entry as broad as a bare \"Core\" matches every behaviour subtree."
#endif
#if __has_include("Core/Runtime/TerminalRuntime.h")
#error "Core/Runtime/TerminalRuntime.h can be reached from the model stage, so the Rule-10 boundary does not hold."
#endif
#if __has_include("Core/Session/Workspace.h")
#error "Core/Session/Workspace.h can be reached from the model stage, so the Rule-10 boundary does not hold."
#endif

int main() { }
