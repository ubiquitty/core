// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Platform/Host/ClipboardTool.h"
#include "Core/Clipboard/ToolInvocation.h"
#include "Core/Codec/Base64.h"
#include "Core/Protocol/Local/Clipboard.h"
#include "PAL/Socket.h"
#include "Platform/Host/HostPaths.h"
#include "Platform/Host/HostSocketName.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

namespace Ubiquitty::Host {
namespace {

bool writeOutput(std::span<const std::uint8_t> bytes)
{
    while (!bytes.empty()) {
        const auto written = ::write(STDOUT_FILENO, bytes.data(), bytes.size());
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            return false;
        }
        bytes = bytes.subspan(static_cast<std::size_t>(written));
    }
    return true;
}

std::string nativeSearchPath(std::string_view paths)
{
    std::string result;
    while (!paths.empty()) {
        const auto separator = paths.find(':');
        const auto directory = paths.substr(0, separator);
        bool isHelper = false;
        for (const auto& component : std::filesystem::path(directory)) {
            isHelper |= component == "clipboard-tools";
        }
        if (!directory.empty() && !isHelper) {
            if (!result.empty()) {
                result += ':';
            }
            result += directory;
        }
        if (separator == std::string_view::npos) {
            break;
        }
        paths.remove_prefix(separator + 1);
    }
    return result;
}

bool writeFileAtomically(const std::filesystem::path& path, std::string_view contents)
{
    std::string temporary = path.string() + ".XXXXXX";
    const int descriptor = ::mkstemp(temporary.data());
    if (descriptor < 0) {
        return false;
    }
    struct stat existing {};
    bool didWrite = true;
    if (::lstat(path.c_str(), &existing) == 0 && S_ISREG(existing.st_mode)) {
        didWrite = ::fchmod(descriptor, existing.st_mode & 0777) == 0;
    }
    while (didWrite && !contents.empty()) {
        const auto count = ::write(descriptor, contents.data(), contents.size());
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            didWrite = false;
            break;
        }
        contents.remove_prefix(static_cast<std::size_t>(count));
    }
    didWrite = ::close(descriptor) == 0 && didWrite;
    if (didWrite && ::rename(temporary.c_str(), path.c_str()) == 0) {
        return true;
    }
    ::unlink(temporary.c_str());
    return false;
}

int runNative(std::span<char*> arguments)
{
    const char* originalPath = std::getenv("UBIQUITTY_CLIPBOARD_NATIVE_PATH");
    const char* shimPath = std::getenv("UBIQUITTY_CLIPBOARD_TOOLS");
    if (!originalPath || !shimPath) {
        return 1;
    }
    const std::string name = std::filesystem::path(arguments[0]).filename().string();
    const std::string self = resolveSelfExecutablePath();
    const auto nativePath = nativeSearchPath(originalPath);
    if (::setenv("PATH", nativePath.c_str(), 1) != 0) {
        return 1;
    }
    std::string_view paths(nativePath);
    while (!paths.empty()) {
        const auto separator = paths.find(':');
        const auto directory = paths.substr(0, separator);
        if (!directory.empty() && directory != shimPath) {
            const auto candidate = std::filesystem::path(directory) / name;
            std::error_code error;
            const auto resolved = std::filesystem::canonical(candidate, error);
            if (!error && resolved.string() != self && ::access(candidate.c_str(), X_OK) == 0) {
                std::vector<char*> argv(arguments.begin(), arguments.end());
                argv.push_back(nullptr);
                ::execv(candidate.c_str(), argv.data());
            }
        }
        if (separator == std::string_view::npos) {
            break;
        }
        paths.remove_prefix(separator + 1);
    }
    std::fprintf(stderr, "ubiquitty-host: no native clipboard tool is available\n");
    return 1;
}

}

std::vector<std::string> clipboardToolEnvironment(const std::string& socketPath, const std::string& shellPath)
{
    const std::string executable = resolveSelfExecutablePath();
    if (executable.empty() || socketPath.empty()) {
        return {};
    }
    // The helper directory goes into the child's PATH, and a relative entry
    // there resolves against whatever directory the shell is in at the time.
    if (!std::filesystem::path(socketPath).is_absolute()) {
        std::fprintf(stderr, "ubiquitty-host: clipboard helpers require an absolute socket path\n");
        return {};
    }
    const auto directory = std::filesystem::path(socketPath).parent_path() / "clipboard-tools" / std::filesystem::path(socketPath).filename();
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        return {};
    }
    struct stat info {};
    if (::lstat(directory.c_str(), &info) != 0 || !S_ISDIR(info.st_mode) || info.st_uid != ::getuid()
        || ::chmod(directory.c_str(), 0700) != 0) {
        return {};
    }
    for (const char* name : { "pbpaste", "pngpaste", "wl-paste", "xclip", "xsel" }) {
        const auto alias = directory / name;
        const auto existing = std::filesystem::read_symlink(alias, error);
        if (!error && existing == executable) {
            continue;
        }
        error.clear();
        if (std::filesystem::exists(alias, error) || std::filesystem::is_symlink(alias, error)) {
            return {};
        }
        error.clear();
        std::filesystem::create_symlink(executable, alias, error);
        if (error) {
            return {};
        }
    }
    const char* path = std::getenv("PATH");
    const std::string nativePath = nativeSearchPath(path ? path : "/usr/bin:/bin");
    std::vector<std::string> environment { "PATH=" + directory.string() + ":" + nativePath,
        "UBIQUITTY_CLIPBOARD_SOCKET=" + socketPath,
        "UBIQUITTY_CLIPBOARD_TOOLS=" + directory.string(),
        "UBIQUITTY_CLIPBOARD_NATIVE_PATH=" + nativePath };
    if (std::filesystem::path(shellPath).filename() == "zsh") {
        const auto startup = directory / "startup";
        std::filesystem::create_directory(startup, error);
        struct stat startupInfo {};
        if (error || ::lstat(startup.c_str(), &startupInfo) != 0 || !S_ISDIR(startupInfo.st_mode)
            || startupInfo.st_uid != ::getuid() || ::chmod(startup.c_str(), 0700) != 0) {
            return {};
        }
        constexpr std::string_view environmentScript = R"ZSH(if [[ $UBIQUITTY_CLIPBOARD_USER_ZDOTDIR_SET == 1 ]]; then
    export ZDOTDIR=$UBIQUITTY_CLIPBOARD_USER_ZDOTDIR
else
    unset ZDOTDIR
fi
[[ -r ${ZDOTDIR-$HOME}/.zshenv ]] && source "${ZDOTDIR-$HOME}/.zshenv"
export UBIQUITTY_CLIPBOARD_USER_ZDOTDIR_SET=${+ZDOTDIR}
export UBIQUITTY_CLIPBOARD_USER_ZDOTDIR=${ZDOTDIR-}
if [[ -o login && -o rcs ]]; then
    export ZDOTDIR=$UBIQUITTY_CLIPBOARD_TOOLS/startup
fi
)ZSH";
        constexpr std::string_view profileScript = R"ZSH(if [[ $UBIQUITTY_CLIPBOARD_USER_ZDOTDIR_SET == 1 ]]; then
    export ZDOTDIR=$UBIQUITTY_CLIPBOARD_USER_ZDOTDIR
else
    unset ZDOTDIR
fi
[[ -r ${ZDOTDIR-$HOME}/.zprofile ]] && source "${ZDOTDIR-$HOME}/.zprofile"
export PATH="$UBIQUITTY_CLIPBOARD_TOOLS:$PATH"
unset UBIQUITTY_CLIPBOARD_USER_ZDOTDIR UBIQUITTY_CLIPBOARD_USER_ZDOTDIR_SET
)ZSH";
        if (!writeFileAtomically(startup / ".zshenv", environmentScript)
            || !writeFileAtomically(startup / ".zprofile", profileScript)) {
            return {};
        }
        const char* originalDirectory = std::getenv("ZDOTDIR");
        environment.push_back("UBIQUITTY_CLIPBOARD_USER_ZDOTDIR_SET=" + std::string(originalDirectory ? "1" : "0"));
        environment.push_back("UBIQUITTY_CLIPBOARD_USER_ZDOTDIR=" + std::string(originalDirectory ? originalDirectory : ""));
        environment.push_back("ZDOTDIR=" + startup.string());
    }
    return environment;
}

namespace {

int emitEnvironment(std::span<const std::string> environment)
{
    for (const auto& entry : environment) {
        const auto separator = entry.find('=');
        std::string line = "export " + entry.substr(0, separator) + "='";
        for (const char character : std::string_view(entry).substr(separator + 1)) {
            if (character == '\'') {
                line += "'\\''";
            } else {
                line += character;
            }
        }
        line += "'\n";
        if (!writeOutput({ reinterpret_cast<const std::uint8_t*>(line.data()), line.size() })) {
            return 1;
        }
    }
    return 0;
}

int runClipboardRequest(std::span<char*> arguments, std::span<const std::string> environment)
{
    if (arguments.empty()) {
        return 2;
    }
    std::vector<std::string_view> options;
    for (std::size_t index = 1; index < arguments.size(); ++index) {
        options.emplace_back(arguments[index]);
    }
    const auto invocation = Clipboard::parseInvocation(arguments[0], options);
    const char* socketPath = std::getenv("UBIQUITTY_CLIPBOARD_SOCKET");
    if (!socketPath || !*socketPath) {
        std::fprintf(stderr, "ubiquitty-host: clipboard helper has no owning session endpoint\n");
        return 1;
    }
    auto connection = PAL::connectLocal(socketPath);
    if (!connection || !PAL::peerIsSameUser(connection->handle())) {
        return 1;
    }
    const bool isRouteOnly = invocation.action == Clipboard::Action::Write || invocation.action == Clipboard::Action::Unsupported || !invocation.isClipboardSelection;
    const Protocol::Local::LocalClipboardRequest request {
        .kind = isRouteOnly                                ? Protocol::Local::LocalClipboardKind::Route
            : invocation.action == Clipboard::Action::Read ? Protocol::Local::LocalClipboardKind::Item
                                                           : Protocol::Local::LocalClipboardKind::Types,
        .mime = !isRouteOnly && invocation.action == Clipboard::Action::Read ? invocation.mime : std::string_view {},
    };
    // This connection carries exactly one request. Correlation is local to it;
    // no received field can choose another session or a native fallback.
    constexpr std::uint64_t correlation = 1;
    const auto wire = Protocol::Local::encodeLocalClipboardRequest(correlation, request);
    if (!wire || connection->writeAll(std::span<const char>(reinterpret_cast<const char*>(wire->bytes.get()), wire->size)) < 0) {
        return 1;
    }
    Protocol::Local::LocalClipboardReplyReader decoder(correlation);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd descriptor { static_cast<PAL::UnixFileDescriptor&>(connection->handle()).get(), static_cast<short>(POLLIN | (connection->hasPendingWrite() ? POLLOUT : 0)), 0 };
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        const int ready = ::poll(&descriptor, 1, static_cast<int>(remaining));
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready <= 0) {
            break;
        }
        if (descriptor.revents & POLLOUT) {
            connection->drainPendingWrite();
        }
        if (!(descriptor.revents & (POLLIN | POLLHUP | POLLERR))) {
            continue;
        }
        std::array<char, 65536> buffer;
        const auto count = connection->read(buffer);
        if (count <= 0) {
            if (count < 0 && (connection->lastError() == EAGAIN || connection->lastError() == EINTR)) {
                continue;
            }
            break;
        }
        if (connection->hasPendingWrite())
            break; // a reply cannot precede the complete request
        const auto state = decoder.consume({ reinterpret_cast<const std::uint8_t*>(buffer.data()), static_cast<std::size_t>(count) });
        if (state == Protocol::Local::LocalClipboardRequestReader::State::Refused)
            break;
        if (const auto response = decoder.take()) {
            if (response->isHostSelected) {
                return environment.empty() ? runNative(arguments) : emitEnvironment(environment);
            }
            if (request.kind == Protocol::Local::LocalClipboardKind::Route
                && response->status != Protocol::Sync::EffectClipboardStatus::Empty
                && response->status != Protocol::Sync::EffectClipboardStatus::Refused) {
                return 1;
            }
            if (!environment.empty()) {
                return response->status == Protocol::Sync::EffectClipboardStatus::Empty ? emitEnvironment(environment) : 1;
            }
            if (invocation.action == Clipboard::Action::Write || invocation.action == Clipboard::Action::Unsupported) {
                std::fprintf(stderr, "ubiquitty-host: this clipboard operation or output format is not supported for the selected client\n");
                return 1;
            }
            if (response->status == Protocol::Sync::EffectClipboardStatus::Refused || response->status == Protocol::Sync::EffectClipboardStatus::TooLarge)
                return 1;
            if (!invocation.isClipboardSelection || response->status == Protocol::Sync::EffectClipboardStatus::Empty) {
                return invocation.tool == Clipboard::Tool::PNGPaste ? 1 : 0;
            }
            if (response->status != Protocol::Sync::EffectClipboardStatus::OK) {
                return 1;
            }
            if (invocation.action == Clipboard::Action::ListTypes) {
                std::vector<std::string> types;
                for (const auto& item : std::span(response->bundle.items).first(response->bundle.count)) {
                    if (!item.data.empty())
                        return 1;
                    types.emplace_back(item.mime);
                }
                const auto listing = Clipboard::targetsListing(types);
                return writeOutput({ reinterpret_cast<const std::uint8_t*>(listing.data()), listing.size() }) ? 0 : 1;
            }
            if (response->bundle.count != 1 || response->bundle.items.front().mime != invocation.mime) {
                return 1;
            }
            const auto bytes = response->bundle.items.front().data;
            if (!invocation.outputFile.empty()) {
                const auto contents = std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                return writeFileAtomically(invocation.outputFile, contents) ? 0 : 1;
            }
            if (invocation.shouldEncodeBase64) {
                std::vector<char> encoded(Codec::base64EncodedLength(bytes.size()));
                const auto characters = Codec::base64Encode({ reinterpret_cast<const char*>(bytes.data()), bytes.size() }, encoded);
                if (!characters) {
                    return 1;
                }
                return writeOutput({ reinterpret_cast<const std::uint8_t*>(characters->data()), characters->size() }) ? 0 : 1;
            }
            if (!writeOutput(bytes)) {
                return 1;
            }
            if (invocation.shouldAppendNewline) {
                constexpr std::array<std::uint8_t, 1> newline { '\n' };
                if (!writeOutput(newline)) {
                    return 1;
                }
            }
            return 0;
        }
    }
    std::fprintf(stderr, "ubiquitty-host: clipboard request did not complete\n");
    return 1;
}

}

int runClipboardTool(std::span<char*> arguments)
{
    return runClipboardRequest(arguments, {});
}

int runClipboardEnvironment(const std::string& socketPath)
{
    if (!std::filesystem::path(socketPath).is_absolute()) {
        std::fprintf(stderr, "ubiquitty-host: clipboard-env requires an absolute live session socket path\n");
        return 2;
    }
    const auto environment = clipboardToolEnvironment(socketPath, {});
    if (environment.empty() || ::setenv("UBIQUITTY_CLIPBOARD_SOCKET", socketPath.c_str(), 1) != 0) {
        std::fprintf(stderr, "ubiquitty-host: could not prepare clipboard helpers\n");
        return 1;
    }
    char name[] = "clipboard-env";
    std::array<char*, 1> arguments { name };
    return runClipboardRequest(arguments, environment);
}

}
