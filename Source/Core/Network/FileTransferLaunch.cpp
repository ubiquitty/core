// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// FileTransferLaunch.cpp — the portable half of an in-band ZMODEM transfer
// (see FileTransferLaunch.h): where the agent lives, where received files go,
// and running the agent against an open terminal stream. The frontend keeps
// the two questions that are its own — whether to run an agent at all, and
// where the user says files may land — and delegates everything else here.
//

#include "Core/Network/FileTransferLaunch.h"

#include "Core/Preferences/Preferences.h"
#include "PAL/PAL.h"

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace Ubiquitty::Network {

namespace {

// The prefixes a Finder- or open-launched application's minimal PATH misses.
constexpr std::array transferAgentPrefixes = {
    std::string_view { "/opt/homebrew/bin/" },
    std::string_view { "/usr/local/bin/" },
    std::string_view { "/opt/local/bin/" },
    std::string_view { "/usr/bin/" },
};

constexpr std::array receiveAgentNames = { std::string_view { "rz" }, std::string_view { "lrz" } };
constexpr std::array sendAgentNames = { std::string_view { "sz" }, std::string_view { "lsz" } };

} // namespace

std::string resolveTransferAgentPath(TransferDirection direction)
{
    for (std::string_view prefix : transferAgentPrefixes) {
        for (std::string_view name : (direction == TransferDirection::Receive) ? receiveAgentNames : sendAgentNames) {
            std::string candidate(prefix);
            candidate += name;
            if (PAL::systemExecutableExists(candidate.c_str()))
                return candidate;
        }
    }
    return {};
}

std::string resolvedTransferDownloadDirectory(const Preferences::Values& configuration)
{
    std::string directory;
    if (configuration.downloadDir.has_value())
        directory = *configuration.downloadDir;
    if (directory.empty())
        directory = PAL::downloadsDirectoryDefaultPath();
    if (directory.empty())
        return {};
    return PAL::storageEnsureDirectory(directory.c_str()) ? directory : std::string();
}

bool runTransferAgent(int terminalDescriptor, TransferDirection direction, std::string_view downloadDirectoryOrSendFile, double limitSeconds)
{
    const std::string binaryPath = resolveTransferAgentPath(direction);
    if (binaryPath.empty())
        return false;
    // A receive lands its files in the directory it runs in. A restricted send
    // refuses a file named by an absolute path or a parent reference, so it
    // runs in the file's directory and names the file from there. The `./`
    // keeps a name that begins with a dash from reading as an option.
    std::string workingDirectory;
    std::string sendFile;
    if (direction == TransferDirection::Receive) {
        workingDirectory = downloadDirectoryOrSendFile;
    } else {
        const std::size_t slash = downloadDirectoryOrSendFile.rfind('/');
        if (slash != std::string_view::npos)
            workingDirectory = slash == 0 ? std::string("/") : std::string(downloadDirectoryOrSendFile.substr(0, slash));
        const std::string_view name = slash == std::string_view::npos ? downloadDirectoryOrSendFile : downloadDirectoryOrSendFile.substr(slash + 1);
        if (!name.empty())
            sendFile = "./" + std::string(name);
    }
    const std::vector<std::string> arguments = buildZModemArguments(binaryPath, direction, sendFile);
    if (arguments.empty())
        return false;

    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (const std::string& argument : arguments)
        argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);

    const int exitCode = PAL::processSpawnWithTerminalAndWait(binaryPath.c_str(), argv.data(), workingDirectory.c_str(), terminalDescriptor, limitSeconds);
    return exitCode == 0;
}

} // namespace Ubiquitty::Network