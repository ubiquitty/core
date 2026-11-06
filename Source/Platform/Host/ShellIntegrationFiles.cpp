// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Platform/Host/ShellIntegrationFiles.h"

#include "PAL/DurableFile.h"

#include <cerrno>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <sys/stat.h>
#include <unistd.h>

namespace Ubiquitty::Host {

namespace {

// Whether `path` already holds exactly `contents`.
bool holdsContents(const std::string& path, std::string_view contents)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return false;
    const std::string existing { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
    return existing == contents;
}

} // namespace

std::string installShellIntegration(std::string_view controlDirectory)
{
    if (controlDirectory.empty())
        return {};
    const std::string directory = std::string(controlDirectory) + "/shell-integration";
    if (::mkdir(directory.c_str(), 0700) != 0 && errno != EEXIST)
        return {};
    struct stat info {};
    if (::lstat(directory.c_str(), &info) != 0 || !S_ISDIR(info.st_mode) || info.st_uid != ::geteuid())
        return {};
    for (const auto& script : shellIntegrationScripts()) {
        const std::string path = directory + "/" + std::string(script.fileName);
        if (holdsContents(path, script.contents))
            continue;
        const std::span<const std::byte> bytes { reinterpret_cast<const std::byte*>(script.contents.data()), script.contents.size() };
        if (PAL::storageWriteDurable(path.c_str(), bytes) == PAL::DurableFileResult::NotCommitted)
            return {};
    }
    return directory;
}

} // namespace Ubiquitty::Host
