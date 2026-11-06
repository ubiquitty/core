// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// ToolInvocation.cpp — see Core/Clipboard/ToolInvocation.h.

#include "Core/Clipboard/ToolInvocation.h"

namespace Ubiquitty::Clipboard {

namespace {

/// The basename of a path, or the whole string when it holds no separator.
std::string_view baseName(std::string_view path)
{
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

/// Whether an argument is `name` or `name=<value>`, yielding the value.
bool matchesOption(std::string_view argument, std::string_view name, std::string_view& outValue)
{
    if (argument == name) {
        outValue = {};
        return true;
    }
    if (argument.size() > name.size() && argument.starts_with(name) && argument[name.size()] == '=') {
        outValue = argument.substr(name.size() + 1);
        return true;
    }
    return false;
}

/// The pseudo-type xclip answers a type list under.
constexpr std::string_view targetsPseudoType = "TARGETS";

/// What a reader naming no type is asking for.
constexpr std::string_view defaultReadMime = "text/plain";

} // namespace

Tool toolFromArgumentZero(std::string_view argumentZero)
{
    const std::string_view name = baseName(argumentZero);
    if (name == "xclip")
        return Tool::XClip;
    if (name == "xsel")
        return Tool::XSel;
    if (name == "wl-copy")
        return Tool::WaylandCopy;
    if (name == "wl-paste")
        return Tool::WaylandPaste;
    if (name == "pbcopy")
        return Tool::PasteboardCopy;
    if (name == "pbpaste")
        return Tool::PasteboardPaste;
    if (name == "pngpaste") {
        return Tool::PNGPaste;
    }
    return Tool::Unknown;
}

Invocation parseInvocation(std::string_view argumentZero, std::span<const std::string_view> arguments)
{
    Invocation invocation;
    invocation.tool = toolFromArgumentZero(argumentZero);
    if (invocation.tool == Tool::Unknown) {
        return invocation;
    }
    if (invocation.tool == Tool::PNGPaste) {
        if (arguments.size() != 1 || arguments.front().empty()) {
            return invocation;
        }
        const auto argument = arguments.front();
        if (argument == "-b") {
            invocation.shouldEncodeBase64 = true;
        } else if (argument != "-") {
            if (argument.front() == '-') {
                return invocation;
            }
            const auto dot = argument.find_last_of('.');
            std::string extension(dot == std::string_view::npos ? std::string_view {} : argument.substr(dot + 1));
            for (auto& character : extension) {
                if (character >= 'A' && character <= 'Z') {
                    character += 'a' - 'A';
                }
            }
            if (extension == "gif" || extension == "jpg" || extension == "jpeg" || extension == "tif" || extension == "tiff") {
                return invocation;
            }
            invocation.outputFile = argument;
        }
        invocation.action = Action::Read;
        invocation.mime = "image/png";
        return invocation;
    }
    const bool isXSelection = invocation.tool == Tool::XClip || invocation.tool == Tool::XSel;
    const bool isWayland = invocation.tool == Tool::WaylandCopy || invocation.tool == Tool::WaylandPaste;
    invocation.isClipboardSelection = !isXSelection;
    bool wantsRead = invocation.tool == Tool::PasteboardPaste || invocation.tool == Tool::WaylandPaste;
    bool wantsList = false;
    bool shouldAppendNewline = invocation.tool == Tool::WaylandPaste;
    std::string_view type = defaultReadMime;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const auto argument = arguments[index];
        std::string_view value;
        const auto consumeValue = [&]() {
            if (value.empty() && argument.find('=') == std::string_view::npos && index + 1 < arguments.size()) {
                value = arguments[++index];
            }
            return !value.empty() && value.front() != '-';
        };
        if (isXSelection) {
            if (argument == "-o" || argument == "-out" || argument == "--output") {
                wantsRead = true;
            } else if (argument == "-i" || argument == "-in" || argument == "--input") {
                wantsRead = false;
            } else if (argument == "-b" || argument == "--clipboard") {
                invocation.isClipboardSelection = true;
            } else if (argument == "-p" || argument == "--primary" || argument == "-s" || argument == "--secondary") {
                invocation.isClipboardSelection = false;
            } else if (matchesOption(argument, "-selection", value) || matchesOption(argument, "--selection", value)) {
                if (!consumeValue() || (value.front() != 'c' && value.front() != 'p' && value.front() != 's')) {
                    return invocation;
                }
                invocation.isClipboardSelection = value.front() == 'c';
            } else if (matchesOption(argument, "-t", value) || matchesOption(argument, "-target", value)
                || matchesOption(argument, "--target", value)) {
                if (!consumeValue()) {
                    return invocation;
                }
                type = value;
            } else {
                return invocation;
            }
        } else if (isWayland) {
            if ((argument == "-l" || argument == "--list-types") && wantsRead) {
                wantsList = true;
            } else if ((argument == "-n" || argument == "--no-newline") && wantsRead) {
                shouldAppendNewline = false;
            } else if (argument == "-p" || argument == "--primary") {
                invocation.isClipboardSelection = false;
            } else if (matchesOption(argument, "--type", value) || matchesOption(argument, "-t", value)) {
                if (!consumeValue()) {
                    return invocation;
                }
                type = value;
            } else {
                return invocation;
            }
        } else if (matchesOption(argument, "-pboard", value)) {
            if (!consumeValue()) {
                return invocation;
            }
            invocation.isClipboardSelection = value == "general";
        } else {
            return invocation;
        }
    }
    invocation.action = wantsRead ? Action::Read : Action::Write;
    if (wantsRead && (wantsList || type == targetsPseudoType)) {
        invocation.action = Action::ListTypes;
    }
    invocation.mime = type;
    invocation.shouldAppendNewline = shouldAppendNewline && type.starts_with("text/") && invocation.action == Action::Read;
    return invocation;
}

std::string targetsListing(std::span<const std::string> mimes)
{
    std::string listing;
    for (const std::string& mime : mimes) {
        listing += mime;
        listing += '\n';
    }
    return listing;
}

} // namespace Ubiquitty::Clipboard
