// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// UbiquittyTerminfo.cpp — see UbiquittyTerminfo.h. The installer for the entry
// Core/Terminfo.cpp carries: a temp file, `tic -x`, and the outcome. The entry
// itself sits beside the XTGETTCAP lookup that answers from it, in the model
// layer, because both are the terminal's own description of itself; this file
// is the one that spawns a process.

#include "UbiquittyTerminfo.h"

#include "Core/Terminfo.h"

#include "PAL/Log.h"
#include "PAL/PAL.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace Ubiquitty::Terminfo {

namespace {

// Resolve `tic`. Returns true and fills `out` (NUL-terminated) on success.
// Why the macOS system /usr/bin/tic is preferred: the compiled entry must be
// readable by the ncurses the user's apps LINK against. The macOS system ncurses
// (6.0) can't read the 32-bit extended-number format a newer off-PATH tic emits —
// such an entry compiles but is unopenable, so $TERM silently falls back to
// xterm-256color and terminfo-gated features (notably tmux's `Ms` OSC 52
// clipboard) break. The system tic's legacy format is readable everywhere.
bool findTic(std::string& out)
{
    static const char* const preferred[] = {
#if defined(__APPLE__)
        "/usr/bin/tic", // system ncurses — legacy format the system db can read
#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
        "/usr/pkg/bin/tic", // pkgsrc (NetBSD et al.)
        "/usr/local/bin/tic", // FreeBSD/DragonFly ports & pkg
#else
        "/home/linuxbrew/.linuxbrew/opt/ncurses/bin/tic", // Linuxbrew (keg-only)
        "/usr/local/bin/tic", // source-built ncurses
#endif
    };
    for (const char* cand : preferred) {
        if (PAL::systemExecutableExists(cand)) {
            out = cand;
            return true;
        }
    }

    const char* path = std::getenv("PATH");
    if (!path || !*path)
        path = "/usr/bin:/bin:/usr/local/bin";
    const char* p = path;
    while (*p) {
        const char* sep = std::strchr(p, ':');
        size_t length = sep ? size_t(sep - p) : std::strlen(p);
        std::string cand(p, length);
        if (cand.empty())
            cand = ".";
        if (cand.back() != '/')
            cand.push_back('/');
        cand += "tic";
        if (PAL::systemExecutableExists(cand.c_str())) {
            out = cand;
            return true;
        }
        if (!sep)
            break;
        p = sep + 1;
    }
    return false;
}

// Write the source to a unique temp file. Returns the path on success
// (caller removes it via PAL::storageRemoveFile), empty on failure.
std::string writeTempSource()
{
    const char* tmp = std::getenv("TMPDIR");
    char path[1024];
    if (!PAL::storageWriteTempFile(tmp, ".terminfo.src",
            std::as_bytes(std::span { sourceText(), std::strlen(sourceText()) }), path))
        return {};
    return { path };
}

} // anonymous namespace

InstallResult install(const char* destinationDirectory)
{
    std::string tic;
    if (!findTic(tic)) {
        Ubiquitty::PAL::logLine(stderr,
            "Ubiquitty: cannot install terminfo — `tic` not found on PATH.\nInstall ncurses (it ships tic) and retry.");
        return InstallResult::TicMissing;
    }

    // Resolve the destination terminfo db root.
    std::string directory;
    if (destinationDirectory && *destinationDirectory) {
        directory = destinationDirectory;
    } else {
        char home[1024];
        if (PAL::systemHomeDirectory(home, sizeof(home)) <= 0) {
            Ubiquitty::PAL::logLine(stderr,
                "Ubiquitty: cannot install terminfo — no home directory resolves and no destination directory was given.");
            return InstallResult::WriteFailed;
        }
        directory = home;
        if (directory.back() != '/')
            directory.push_back('/');
        directory += ".terminfo";
    }

    std::string srcPath = writeTempSource();
    if (srcPath.empty()) {
        Ubiquitty::PAL::logLine(stderr,
            "Ubiquitty: cannot install terminfo — failed to write temp source file.");
        return InstallResult::WriteFailed;
    }

    // tic -x -o <directory> <srcfile>. -x keeps the user-defined extension caps
    // (Tc, RGB, Smulx, Setulc, Ms, Sync, ...). Existing entries in <directory>
    // are overwritten, so repeated installs are idempotent.
    const char* argv[] = { tic.c_str(), "-x", "-o", directory.c_str(), srcPath.c_str(), nullptr };

    const int exitCode = PAL::processSpawnAndWait(tic.c_str(), argv);
    PAL::storageRemoveFile(srcPath.c_str());

    if (exitCode < 0) {
        Ubiquitty::PAL::logLine(stderr, "Ubiquitty: cannot install terminfo — failed to launch tic.");
        return InstallResult::SpawnFailed;
    }
    if (exitCode != 0) {
        Ubiquitty::PAL::logLine(stderr, "Ubiquitty: terminfo install failed — tic exited with {}.", exitCode);
        return InstallResult::TicFailed;
    }

    Ubiquitty::PAL::logLine(stderr,
        "Ubiquitty: terminfo entry `xterm-ubiquitty` (aliases `ubiquitty`, `ubiquitty-256color`) installed under {}.\nOpt in with:  export TERM=xterm-ubiquitty",
        directory);
    return InstallResult::Success;
}

int exitCodeFor(InstallResult r) { return r == InstallResult::Success ? 0 : 1; }

} // namespace Ubiquitty::Terminfo
