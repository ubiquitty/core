// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Tests/Platform/test_preferences_backend.cpp — the bundled POSIX preferences
// backend, driven through the PAL seam rather than through its own helpers.
//
// One bundled backend serves macOS, Linux and the host, so the surface worth
// asserting is the one Core actually calls, not an internal spelling no caller
// uses, and the path rules are observable through it: a key that would escape
// its directory refuses, and nothing appears outside the root when it does.
//
// The size probe matters most. A seam that answers only fixed-length reads
// cannot carry configuration text, which leaves the browser with no
// configuration at all.

#include "Harness.h"

#include "PAL/PAL.h"

#include "Helpers/TempDir.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

int main()
{
    Ubiquitty::Test::TempDir tempDir("/tmp/ubiquitty-preferences-backend.XXXXXX");
    CHECK(tempDir.valid());
    if (!tempDir.valid())
        return 1;
    const std::string directory = tempDir.path();

    const char* previousConfigHome = std::getenv("XDG_CONFIG_HOME");
    const std::string savedConfigHome = previousConfigHome != nullptr ? previousConfigHome : "";
    ::setenv("XDG_CONFIG_HOME", directory.c_str(), 1);

    constexpr Ubiquitty::PAL::PreferencesBlobKey key { "preferences" };
    const std::string blobPath = std::string(directory) + "/ubiquitty/preferences.blob";

    // Absent is not failure-with-noise: a first run has no blob, and Core reads
    // the negative as "nothing stored" and keeps its defaults.
    std::array<char, 32> output {};
    CHECK_MSG(Ubiquitty::PAL::preferencesLoad(key, output.data(), static_cast<int>(output.size())) < 0,
        "an absent blob reports absent");

    constexpr std::string_view payload = "opaque-data";
    CHECK_MSG(Ubiquitty::PAL::preferencesSaveAtomic(key, std::as_bytes(std::span { payload })) == 0,
        "a blob saves");

    // The probe, which is what lets one seam carry a payload whose length the
    // caller cannot know: a null buffer and a zero capacity ask for the size.
    CHECK_MSG(Ubiquitty::PAL::preferencesLoad(key, nullptr, 0) == static_cast<int>(payload.size()),
        "a null buffer probes the size rather than refusing");
    CHECK_MSG(Ubiquitty::PAL::preferencesLoad(key, output.data(), 0) == static_cast<int>(payload.size()),
        "and so does a zero capacity with a buffer");

    // A capacity with nowhere to put the bytes is the caller's error, and is the
    // one shape of this call that is neither a read nor a probe.
    CHECK_MSG(Ubiquitty::PAL::preferencesLoad(key, nullptr, 8) < 0,
        "a capacity with no buffer refuses");

    const int read = Ubiquitty::PAL::preferencesLoad(key, output.data(), static_cast<int>(output.size()));
    CHECK_MSG(read == static_cast<int>(payload.size()), "a sized read returns the whole blob");
    CHECK_MSG(std::string_view(output.data(), static_cast<std::size_t>(read)) == payload, "and its bytes");

    // Clamped rather than overrun when the caller's buffer is the smaller.
    CHECK_MSG(Ubiquitty::PAL::preferencesLoad(key, output.data(), 6) == 6, "a short read clamps to the capacity");
    CHECK_MSG(std::string_view(output.data(), 6) == payload.substr(0, 6), "and returns that prefix");

    struct stat fileStatus {};
    CHECK(::stat(blobPath.c_str(), &fileStatus) == 0);
    CHECK_MSG((fileStatus.st_mode & 0777) == 0600, "a blob is readable by its owner alone");

    // A key names a file and may not describe a path. Both directions: the load
    // refuses, the save refuses, and nothing lands outside the root.
    CHECK_MSG(Ubiquitty::PAL::preferencesLoad({ "../escape" }, output.data(), static_cast<int>(output.size())) < 0,
        "a traversing key does not load");
    CHECK_MSG(Ubiquitty::PAL::preferencesLoad({ "nested/name" }, output.data(), static_cast<int>(output.size())) < 0,
        "nor does a key naming a directory");
    CHECK_MSG(Ubiquitty::PAL::preferencesSaveAtomic({ "../escape" }, std::as_bytes(std::span { payload })) < 0,
        "a traversing key does not save");
    CHECK_MSG(::access((std::string(directory) + "/escape.blob").c_str(), F_OK) != 0,
        "and wrote nothing outside the root");

    // The configuration key is the one blob a person edits, so it keeps the name
    // and extension they edit it under rather than taking the generated one.
    constexpr std::string_view configuration = "[appearance]\n";
    CHECK(Ubiquitty::PAL::preferencesSaveAtomic(Ubiquitty::PAL::configurationBlobKey,
              std::as_bytes(std::span { configuration }))
        == 0);
    const std::string configurationPath = std::string(directory) + "/ubiquitty/config.toml";
    CHECK_MSG(::access(configurationPath.c_str(), F_OK) == 0,
        "the configuration key resolves to the file a person edits");
    CHECK_MSG(Ubiquitty::PAL::preferencesLoad(Ubiquitty::PAL::configurationBlobKey, nullptr, 0)
            == static_cast<int>(configuration.size()),
        "and its length is probeable, which configuration text needs");

    if (previousConfigHome != nullptr)
        ::setenv("XDG_CONFIG_HOME", savedConfigHome.c_str(), 1);
    else
        ::unsetenv("XDG_CONFIG_HOME");

    if (failures == 0) {
        std::printf("test_preferences_backend: PASS\n");
        return 0;
    }
    std::printf("test_preferences_backend: %d failure(s)\n", failures);
    return 1;
}
