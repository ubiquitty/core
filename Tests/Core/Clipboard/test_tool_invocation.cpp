// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_tool_invocation.cpp — the clipboard shims' argument reading.
//
// The cases that matter are not invented: they are the command lines a real
// consumer runs. Claude Code probes and reads a clipboard with the exact
// strings pinned below, and a shim that misreads one of them answers the wrong
// thing to the program the shims exist for.

#include "Core/Clipboard/ToolInvocation.h"

#include <string>
#include <string_view>
#include <vector>

using namespace Ubiquitty::Clipboard;

namespace {

/// Parses a command line given as whole arguments.
Invocation parse(std::string_view argumentZero, std::vector<std::string_view> arguments)
{
    return parseInvocation(argumentZero, arguments);
}

} // namespace

/// The name decides the tool, and a path is as good as a name: the shims are
/// links in a directory on PATH, so argv[0] arrives as whatever the shell
/// resolved.
TEST(testToolComesFromArgumentZero)
{
    CHECK(toolFromArgumentZero("xclip") == Tool::XClip);
    CHECK(toolFromArgumentZero("/run/ubiquitty/bin/xclip") == Tool::XClip);
    CHECK(toolFromArgumentZero("wl-paste") == Tool::WaylandPaste);
    CHECK(toolFromArgumentZero("wl-copy") == Tool::WaylandCopy);
    CHECK(toolFromArgumentZero("pbpaste") == Tool::PasteboardPaste);
    CHECK(toolFromArgumentZero("pbcopy") == Tool::PasteboardCopy);
    CHECK(toolFromArgumentZero("xsel") == Tool::XSel);

    // A name no shim answers to is not served. The shims sit early on PATH, so
    // answering for a tool we do not model would shadow the real one.
    CHECK(toolFromArgumentZero("xdotool") == Tool::Unknown);
    CHECK(toolFromArgumentZero("") == Tool::Unknown);
}

/// The probe a consumer runs to ask whether the clipboard holds an image.
///
/// Verbatim from Claude Code's own clipboard reader:
///
///     xclip -selection clipboard -t TARGETS -o | grep -E "image/(png|jpeg|...)"
///     wl-paste -l | grep -E "image/(png|jpeg|...)"
///
/// It greps the output, so the answer has to be a type per line — and TARGETS
/// has to read as a type LIST rather than as a read of a type named TARGETS,
/// which would answer bytes and match nothing.
TEST(testTheImageProbeReadsAsATypeList)
{
    const Invocation xclipProbe = parse("xclip", { "-selection", "clipboard", "-t", "TARGETS", "-o" });
    CHECK(xclipProbe.tool == Tool::XClip);
    CHECK(xclipProbe.action == Action::ListTypes);
    CHECK(xclipProbe.isClipboardSelection);

    const Invocation waylandProbe = parse("wl-paste", { "-l" });
    CHECK(waylandProbe.tool == Tool::WaylandPaste);
    CHECK(waylandProbe.action == Action::ListTypes);

    // The listing is one MIME per line, which is what the grep above matches.
    const std::vector<std::string> held { "image/png", "text/plain" };
    CHECK(targetsListing(held) == "image/png\ntext/plain\n");
    CHECK(targetsListing({}).empty());
}

/// The read that follows a successful probe, again verbatim:
///
///     xclip -selection clipboard -t image/png -o
///     wl-paste --type image/png
TEST(testTheImageReadNamesItsType)
{
    const Invocation xclipRead = parse("xclip", { "-selection", "clipboard", "-t", "image/png", "-o" });
    CHECK(xclipRead.action == Action::Read);
    CHECK(xclipRead.mime == "image/png");
    CHECK(xclipRead.isClipboardSelection);

    const Invocation waylandRead = parse("wl-paste", { "--type", "image/png" });
    CHECK(waylandRead.action == Action::Read);
    CHECK(waylandRead.mime == "image/png");

    // The BMP fallback the same reader tries when PNG is absent.
    const Invocation bitmapRead = parse("xclip", { "-selection", "clipboard", "-t", "image/bmp", "-o" });
    CHECK(bitmapRead.action == Action::Read);
    CHECK(bitmapRead.mime == "image/bmp");
}

/// Reading and writing are the same tool under different flags, and the
/// default differs per tool: xclip writes unless told -o, wl-paste only reads.
TEST(testDirectionFollowsTheToolAndItsFlags)
{
    CHECK(parse("xclip", {}).action == Action::Write);
    CHECK(parse("xclip", { "-i" }).action == Action::Write);
    CHECK(parse("xclip", { "-o" }).action == Action::Read);
    CHECK(parse("xsel", { "-o" }).action == Action::Read);

    CHECK(parse("wl-copy", {}).action == Action::Write);
    CHECK(parse("wl-paste", {}).action == Action::Read);

    CHECK(parse("pbcopy", {}).action == Action::Write);
    CHECK(parse("pbpaste", {}).action == Action::Read);

    // A reader that named no type means text, rather than the richest type
    // held: pbpaste has no way to ask for anything else, so answering an image
    // would hand binary to a program expecting characters.
    CHECK(parse("pbpaste", {}).mime == "text/plain");
    CHECK(parse("wl-paste", {}).mime == "text/plain");
    CHECK(parse("xclip", { "-o" }).mime == "text/plain");
}

/// One pasteboard is served, so a request for a different X selection is not
/// answered with it. The primary selection is the middle-click buffer, which
/// is a different thing a program is entitled to tell apart.
TEST(testSelectionIsReadAndNotAssumed)
{
    CHECK(parse("xclip", { "-selection", "clipboard", "-o" }).isClipboardSelection);
    CHECK(parse("xclip", { "-selection=clipboard", "-o" }).isClipboardSelection);
    CHECK(parse("xclip", { "-selection", "c", "-o" }).isClipboardSelection);

    CHECK(!parse("xclip", { "-selection", "primary", "-o" }).isClipboardSelection);
    CHECK(!parse("xclip", { "-p" }).isClipboardSelection);
    CHECK(!parse("xsel", { "-p", "-o" }).isClipboardSelection);
    CHECK(!parse("wl-paste", { "--primary" }).isClipboardSelection);

    CHECK(!parse("xclip", { "-o" }).isClipboardSelection);
    CHECK(!parse("xsel", { "-o" }).isClipboardSelection);
    CHECK(!parse("pbpaste", { "-pboard", "find" }).isClipboardSelection);
    CHECK(parse("pbpaste", { "-pboard", "general" }).isClipboardSelection);
}

/// The other spellings the same tools accept for the same thing.
///
/// A shim that reads only the form one caller happens to use answers the wrong
/// thing to the next one. xclip takes a joined `-t=value` as well as a
/// separated pair, names the clipboard as `-b` as well as `-selection
/// clipboard`, and wl-paste joins `--type=` the same way.
TEST(testTheOtherSpellingsOfTheSameFlags)
{
    // -b is xclip's short way of naming the clipboard selection, and xsel's.
    CHECK(parse("xclip", { "-b", "-o" }).isClipboardSelection);
    CHECK(parse("xclip", { "--clipboard", "-o" }).isClipboardSelection);
    CHECK(parse("xsel", { "-b", "-o" }).isClipboardSelection);

    // -b after a primary flag wins, because the last word is what the caller
    // meant: a wrapper appending its own flag must not be overruled by one
    // baked into the command it wrapped.
    CHECK(parse("xclip", { "-p", "-b", "-o" }).isClipboardSelection);

    // The joined forms of the type flag, which several callers prefer.
    CHECK(parse("xclip", { "-t=image/png", "-o" }).mime == "image/png");
    CHECK(parse("xclip", { "--target=image/png", "-o" }).mime == "image/png");
    CHECK(parse("xclip", { "-target=image/png", "-o" }).mime == "image/png");
    CHECK(parse("wl-paste", { "--type=image/png" }).mime == "image/png");
    CHECK(parse("wl-paste", { "-t=image/png" }).mime == "image/png");

    // And joined TARGETS still reads as a type list rather than as a type.
    CHECK(parse("xclip", { "-t=TARGETS", "-o" }).action == Action::ListTypes);

    // The long output flags xclip documents beside -o.
    CHECK(parse("xclip", { "-out" }).action == Action::Read);
    CHECK(parse("xclip", { "--output" }).action == Action::Read);
    CHECK(parse("xclip", { "-in" }).action == Action::Write);
    CHECK(parse("xclip", { "--input" }).action == Action::Write);

    // wl-paste's long list flag beside its short one.
    CHECK(parse("wl-paste", { "--list-types" }).action == Action::ListTypes);
}

/// A name the shims do not model resolves to no tool, and its invocation to no
/// action. The shims sit early on PATH, so answering for a tool nobody wrote a
/// case for would shadow the real one with whatever this file happened to do.
TEST(testAnUnknownToolServesNothing)
{
    const Invocation unknown = parse("xdotool", { "key", "ctrl+v" });
    CHECK(unknown.tool == Tool::Unknown);
    CHECK(unknown.action == Action::Unsupported);
}

TEST(malformedOptionsNeverReadAnotherClipboard)
{
    for (const auto& arguments : std::vector<std::vector<std::string_view>> {
             { "-selection" }, { "-selection", "-o" }, { "-t" }, { "-t=" },
             { "-o", "unexpected-file" }, { "-selection", "invalid", "-o" } }) {
        CHECK(parse("xclip", arguments).action == Action::Unsupported);
    }
    CHECK(parse("wl-paste", { "--type" }).action == Action::Unsupported);
    CHECK(parse("wl-paste", { "--watch", "consumer" }).action == Action::Unsupported);
    CHECK(parse("pbpaste", { "-pboard" }).action == Action::Unsupported);
    CHECK(parse("pbpaste", { "--unknown" }).action == Action::Unsupported);
    CHECK(parse("xclip", { "-t", "TARGETS" }).action == Action::Write);
    CHECK(parse("xclip", { "-t", "image/png", "-selection", "clipboard", "-o" }).mime == "image/png");
}

TEST(binaryClipboardReadsNeverAppendTextNewlines)
{
    CHECK(parse("wl-paste", {}).shouldAppendNewline);
    CHECK(!parse("wl-paste", { "-n" }).shouldAppendNewline);
    CHECK(!parse("wl-paste", { "--type", "image/png" }).shouldAppendNewline);
    CHECK(!parse("wl-paste", { "--list-types" }).shouldAppendNewline);
    CHECK(!parse("xclip", { "-selection", "clipboard", "-o" }).shouldAppendNewline);
}

TEST(pngPastePreservesOutputIntent)
{
    const auto standardOutput = parse("/usr/local/bin/pngpaste", { "-" });
    CHECK(standardOutput.tool == Tool::PNGPaste);
    CHECK(standardOutput.action == Action::Read);
    CHECK(standardOutput.mime == "image/png");
    CHECK(standardOutput.outputFile.empty());
    CHECK(!standardOutput.shouldAppendNewline);
    CHECK(parse("pngpaste", { "-b" }).shouldEncodeBase64);
    CHECK(parse("pngpaste", { "image.PNG" }).outputFile == "image.PNG");
    CHECK(parse("pngpaste", { "image.unknown" }).mime == "image/png");
    CHECK(parse("pngpaste", {}).action == Action::Unsupported);
    CHECK(parse("pngpaste", { "image.JPG" }).action == Action::Unsupported);
    CHECK(parse("pngpaste", { "image.png", "extra" }).action == Action::Unsupported);
}

UBIQUITTY_TEST_MAIN()
