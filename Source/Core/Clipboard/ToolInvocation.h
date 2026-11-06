// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// What a clipboard command-line tool was asked to do.
//
// A remote host has no pasteboard to offer a program that wants one: a headless
// box runs no display server, so xclip and wl-paste have nothing to talk to and
// pbpaste does not exist. The shims that stand in for them answer from the
// CLIENT's pasteboard over the session, and this decides what each invocation
// asked for.
//
// Keyed on argv[0] rather than on a flag, because that is how the shims are
// installed: one binary, linked under several names. A symlink pointing at a
// system tool would not survive the coreutils that dispatch on argv[0]
// themselves — busybox and uutils answer "unknown program" to a name they do
// not know — so the links point back at our own binary and this reads the name
// they arrived under.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace Ubiquitty::Clipboard {

/// Which clipboard tool an invocation is imitating.
enum class Tool : std::uint8_t {
    /// Not a name any shim answers to.
    Unknown,
    /// X11 `xclip`.
    XClip,
    /// X11 `xsel`.
    XSel,
    /// Wayland `wl-copy`.
    WaylandCopy,
    /// Wayland `wl-paste`.
    WaylandPaste,
    /// macOS `pbcopy`.
    PasteboardCopy,
    /// macOS `pbpaste`.
    PasteboardPaste,
    /// macOS `pngpaste` image reads.
    PNGPaste,
};

/// What the invocation wants done.
enum class Action : std::uint8_t {
    /// Write standard input to the clipboard.
    Write,
    /// Write the clipboard to standard output.
    Read,
    /// List the MIME types the clipboard holds, one per line.
    ListTypes,
    /// The arguments do not describe something a shim can serve.
    Unsupported,
};

/// One parsed clipboard-tool invocation.
struct Invocation {
    Tool tool = Tool::Unknown;
    Action action = Action::Unsupported;
    /// The MIME type asked for, empty when the tool named none.
    ///
    /// A reader that named no type wants the clipboard's own idea of text:
    /// `pbpaste` and a bare `wl-paste` both mean text/plain, and the shim
    /// answers that rather than guessing at the richest type it holds.
    std::string mime;
    /// Whether the request names the selection a paste reads, rather than X11's
    /// primary or secondary.
    ///
    /// The shims serve one pasteboard, so a request for another selection is
    /// answered as though it were empty rather than served the wrong one — a
    /// middle-click buffer is not the clipboard, and a program that asked for
    /// the difference is entitled to it.
    bool isClipboardSelection = true;
    /// Whether a text read appends the newline required by the tool.
    bool shouldAppendNewline = false;
    /// The destination file for a PNG read, or empty for standard output.
    std::string outputFile;
    /// Whether a PNG read emits base64 without a trailing newline.
    bool shouldEncodeBase64 = false;
};

/// Which tool a shim was invoked as.
///
/// - Parameter argumentZero: The path or name the process was started under;
///   its basename decides.
/// - Returns: The tool, or ``Tool/Unknown``.
[[nodiscard]] Tool toolFromArgumentZero(std::string_view argumentZero);

/// Reads an invocation of a clipboard tool.
///
/// - Parameters:
///   - argumentZero: The name the process was started under.
///   - arguments: The arguments after argv[0].
/// - Returns: What was asked for; ``Action/Unsupported`` when the shim cannot
///   serve it, which a caller answers by failing rather than by guessing.
[[nodiscard]] Invocation parseInvocation(std::string_view argumentZero, std::span<const std::string_view> arguments);

/// The MIME type list a TARGETS request answers with, for the types held.
///
/// xclip spells the type list as a pseudo-type named TARGETS, and its readers
/// grep the result: the answer is one MIME per line, in the order given.
///
/// - Parameter mimes: The types the clipboard holds.
/// - Returns: The body to write to standard output.
[[nodiscard]] std::string targetsListing(std::span<const std::string> mimes);

} // namespace Ubiquitty::Clipboard
