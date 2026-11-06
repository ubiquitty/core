// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
//
// Source/Core/Title/Directory.h — the working-directory title segment policy.
//
// A window/tab title's leading segment is the foreground process's working
// directory rendered per the [title] "Path" sub-option: the full ~-collapsed
// path ("~", "~/Projects") when fullPath, else just the folder basename
// ("Projects", or "~" exactly at home). The home prefix is collapsed to "~".
//
// This was duplicated between the macOS UBWindowController.directorySegment and
// the Linux titleDirectorySegment (each carrying a "mirrors the other" comment),
// so the policy lives here once and both frontends call it. `home` is passed in
// explicitly (NSHomeDirectory / $HOME are platform-resolved, and passing it in
// keeps the function pure + testable). The cwd is expected pre-standardized by
// the caller (the macOS frontend runs stringByStandardizingPath first); this
// helper still normalizes trailing slashes so "/Users/you/" compares equal to
// home.

#include "Base/SwiftInterop.h"

#include "Core/Title/Format.h"
#include "PAL/PAL.h"
#include "PAL/RemoteTarget.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace Ubiquitty::Title {

/// Behavioral-mode enum replacing a former boolean-trap parameter
/// (WebKit no-boolean-arguments). First enumerator == old `false`.
enum class PathFormat : uint8_t {
    BasenameOnly UBIQUITTY_SWIFT_NAME(basenameOnly),
    FullPath UBIQUITTY_SWIFT_NAME(fullPath)
};

/// Whether a reported document URL names a file on the machine running this.
///
/// OSC 6 is the file analog of OSC 7: a program that reports a focused document
/// is naming something more specific than the directory it happens to be in,
/// and that is what a reader dragging the proxy icon — or asking to reveal it —
/// means. So when there is one, it wins over the directory.
///
/// Only a local one, though. The payload is a `file://` URL and its authority
/// may name another host: a shell on the far side of an ssh session reports the
/// document on its own side, and representing that would hand the Finder a
/// path that does not exist here. An empty authority, or `localhost`, is this
/// machine; anything else is somebody else's.
///
/// Shared rather than spelled per arm, because "which location does the proxy
/// icon stand for" is one question, and two shells answering it apart disagree
/// — one preferring the document, the other never looking.
///
/// Takes `const std::string&` rather than a view because Swift calls it:
/// `std.string_view(aSwiftString)` compiles and hands over a view into a
/// temporary that is already gone.
///
/// - Parameter documentURL: The reported URL, verbatim. Empty when none.
/// - Returns: Whether to represent it in place of the working directory.
/// Whether a `file://` URL's authority names this machine.
///
/// An empty authority, `localhost`, and the current platform hostname all name
/// this machine. Comparisons fold ASCII case only.
///
/// - Parameters:
///   - authority: The URL authority, without the `file://` prefix.
///   - localHost: The current platform hostname; empty when unavailable.
/// - Returns: True when the authority means this machine.
[[nodiscard]] inline bool isLocalURLAuthority(std::string_view authority, std::string_view localHost = Ubiquitty::PAL::systemHostName())
{
    if (authority.empty()) {
        return true;
    }
    const auto matches = [authority](std::string_view name) {
        if (name.empty() || authority.size() != name.size()) {
            return false;
        }
        const auto lower = [](char character) {
            return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
        };
        for (std::size_t index = 0; index < name.size(); ++index) {
            if (lower(authority[index]) != lower(name[index])) {
                return false;
            }
        }
        return true;
    };
    return matches("localhost") || matches(localHost);
}

[[nodiscard]] inline bool isLocalDocumentURL(const std::string& documentURL)
{
    constexpr std::string_view scheme = "file://";
    const std::string_view url { documentURL };
    if (!url.starts_with(scheme))
        return false;
    const std::string_view rest = url.substr(scheme.size());
    const std::size_t slash = rest.find('/');
    return isLocalURLAuthority(slash == std::string_view::npos ? rest : rest.substr(0, slash));
}

/// Determines whether a session directory can represent a local folder.
///
/// - Parameters:
///   - target: The foreground process's destination.
///   - reportedHost: The authority from its directory report, or empty.
/// - Returns: Whether the directory belongs to this machine.
[[nodiscard]] inline bool isLocalDirectory(const PAL::Process::RemoteTarget& target, const std::string& reportedHost)
{
    return !target.isRemote() && isLocalURLAuthority(reportedHost);
}

/// What the directory a title names is chosen from.
struct TitleDirectoryFacts {
    /// What a program reported (`Terminal::reportedDirectory`), empty when
    /// nothing stands.
    std::string_view reportedDirectory;
    /// The pane's directory: the report, or the sample standing in for one
    /// (`Ubiquitty::paneDirectory`).
    std::string_view paneDirectory;
    /// Where the foreground process leads.
    PAL::Process::RemoteKind foregroundLeadsTo = PAL::Process::RemoteKind::None;
};

/// The directory a title names for a session.
///
/// A title says where the session is. The pane's directory lets the sample
/// stand in while nothing is reported, and the sample of a client that leads
/// elsewhere — ssh, mosh, a container's exec — is the client's directory on
/// this machine: a title naming it puts the session in a folder it has left,
/// and keeps it there however far the far side moves. So while the foreground
/// leads elsewhere a title names only a report, which by then is the far
/// side's own: a report retires with the program that made it.
///
/// - Parameter facts: The report, the pane's directory and where the
///   foreground leads.
/// - Returns: The directory, or empty when the title names none.
[[nodiscard]] constexpr std::string_view titleDirectory(const TitleDirectoryFacts& facts)
{
    return facts.foregroundLeadsTo != PAL::Process::RemoteKind::None ? facts.reportedDirectory : facts.paneDirectory;
}

inline std::string titleDirectorySegment(const std::string& cwd, const std::string& home, PathFormat pathFormat)
{
    if (cwd.empty())
        return std::string {};
    std::string directory = cwd;
    while (directory.size() > 1 && directory.back() == '/') // normalize trailing slash(es)
        directory.pop_back();
    // Collapse a home-anchored path to ~ (exactly home, or home + "/sub").
    std::string tildePath = directory;
    if (!home.empty() && directory.starts_with(home) && (directory.size() == home.size() || directory[home.size()] == '/'))
        tildePath = "~" + directory.substr(home.size());
    if (pathFormat == PathFormat::FullPath)
        return tildePath;
    if (tildePath == "~")
        return "~"; // exactly home → "~", not the home folder's basename
    const std::string::size_type slash = directory.find_last_of('/');
    if (slash == std::string::npos)
        return directory;
    std::string base = directory.substr(slash + 1);
    // A bare "/" (root) has no basename — keep the directory itself rather than
    // returning empty, matching NSString.lastPathComponent's "/" → "/".
    return base.empty() ? directory : base;
}

/// Composes a remote session's window title from the parts the host sends.
///
/// A host on the wire sends the INPUTS — the working directory and the
/// foreground process chain — rather than a finished title, so the frontend
/// spells the title in its own voice. That leaves the joining rule to whoever
/// composes, and it was written once per frontend until this: the folder, an
/// em dash, then the process, with the process dropped when it repeats the
/// folder name.
///
/// The repeat is worth handling rather than tolerating. A shell whose
/// foreground process reports as the directory it sits in — a `make` in a
/// folder named make, a REPL named after its project — would produce "src —
/// src", which reads as two facts and carries one.
///
/// - Parameters:
///   - currentDirectory: The session's working directory, or empty.
///   - home: The user's home, for the tilde collapse.
///   - processChain: The foreground process, or empty.
/// - Returns: The title, or empty when neither part was sent.
[[nodiscard]] inline std::string composeRemoteTitle(
    const std::string& currentDirectory, const std::string& home, const std::string& processChain,
    PAL::Process::TitleSeparator separator)
{
    const std::string folder = titleDirectorySegment(currentDirectory, home, PathFormat::BasenameOnly);
    // The repeat fold is this function's own; the WEAVE is buildRichTitle's,
    // which every local title already goes through. An em dash spelled here
    // would be a second joiner beside it, and the first edit to either would
    // give a remote window a different separator from the local one beside it.
    const std::string_view chain = processChain == folder ? std::string_view {} : std::string_view { processChain };
    return buildRichTitle({ .showsDirectory = true,
                              .showsAppTitle = false,
                              .showsShellCommand = false,
                              .showsProcess = true,
                              .showsProfileName = false,
                              .showsTTYName = false,
                              .showsDimensions = false,
                              .showsProgramStatus = false },
        { .baseTitle = {}, .lead = folder, .osc = {}, .shellCommand = {}, .chain = chain, .profileName = {}, .ttyName = {}, .programStatus = {} },
        0, 0, separator);
}

} // namespace Ubiquitty::Title
