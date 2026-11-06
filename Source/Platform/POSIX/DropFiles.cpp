// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "PAL/DropFiles.h"

#include "Base/FallibleString.h"
#include "Base/FallibleVector.h"
#include "PAL/UnixFileDescriptor.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <new>
#include <sys/stat.h>
#include <unistd.h>

namespace Ubiquitty::PAL {
namespace {
constexpr std::size_t maxEntries = 16384;
constexpr std::size_t maxDirectoryBytes = 1024 * 1024;
constexpr std::size_t maxPathBytes = 4096;

DropFileError fileError()
{
    switch (errno) {
    case ENOENT:
        return DropFileError::NotFound;
    case EACCES:
    case EPERM:
        return DropFileError::NotPermitted;
    case ENOMEM:
        return DropFileError::NoMemory;
    case EMFILE:
    case ENFILE:
        return DropFileError::TooManyFiles;
    case ELOOP:
    case ENOTDIR:
    case EINVAL:
        return DropFileError::Invalid;
    default:
        return DropFileError::InputOutput;
    }
}

bool isAdmitted(mode_t mode) { return S_ISREG(mode) || S_ISDIR(mode) || S_ISLNK(mode); }

struct DirectoryDeleter {
    void operator()(DIR* directory) const { closedir(directory); }
};
}

struct DropFiles::Impl {
    struct Entry {
        UnixFileDescriptor parent;
        FallibleString name;
    };
    struct Directory {
        UnixFileDescriptor descriptor;
        FallibleVector<FallibleString> names;
        std::int64_t handle = 0;
    };
    struct Read {
        UnixFileDescriptor descriptor;
        FallibleVector<char> bytes;
        std::size_t offset = 0;
        std::uint64_t token = 0;
    };
    FallibleVector<Entry> roots;
    std::array<Directory, 128> directories;
    std::array<Read, 64> reads;
    std::uint64_t nextToken = 0;
    std::int64_t nextDirectory = 1;

    DropFileRead start(int parent, const char* name)
    {
        auto slot = std::ranges::find_if(reads, [](const auto& read) { return !read.token; });
        if (slot == reads.end() || nextToken == std::numeric_limits<std::uint64_t>::max())
            return { .error = DropFileError::TooManyFiles };
        struct stat status {};
        if (fstatat(parent, name, &status, AT_SYMLINK_NOFOLLOW) < 0)
            return { .error = fileError() };
        if (!isAdmitted(status.st_mode))
            return { .error = DropFileError::Invalid };
        Read pending;
        std::int64_t kind = 0;
        if (S_ISLNK(status.st_mode)) {
            std::array<char, maxPathBytes> target;
            const auto size = readlinkat(parent, name, target.data(), target.size());
            if (size < 0)
                return { .error = fileError() };
            if (static_cast<std::size_t>(size) == target.size())
                return { .error = DropFileError::Invalid };
            if (!pending.bytes.tryAssign(target.begin(), target.begin() + size))
                return { .error = DropFileError::NoMemory };
            kind = 1;
        } else {
            pending.descriptor.reset(openat(parent, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK | (S_ISDIR(status.st_mode) ? O_DIRECTORY : 0)));
            if (!pending.descriptor.isValid())
                return { .error = fileError() };
            struct stat opened {};
            if (fstat(pending.descriptor.get(), &opened) < 0)
                return { .error = fileError() };
            if (opened.st_dev != status.st_dev || opened.st_ino != status.st_ino || opened.st_mode != status.st_mode)
                return { .error = DropFileError::Invalid };
            if (S_ISDIR(opened.st_mode)) {
                auto directorySlot = std::ranges::find_if(directories, [](const auto& directory) { return !directory.handle; });
                if (directorySlot == directories.end() || nextDirectory == std::numeric_limits<std::int32_t>::max())
                    return { .error = DropFileError::NoMemory };
                auto duplicate = pending.descriptor.duplicate();
                if (!duplicate.isValid())
                    return { .error = fileError() };
                std::unique_ptr<DIR, DirectoryDeleter> stream(fdopendir(duplicate.get()));
                if (!stream)
                    return { .error = fileError() };
                (void)duplicate.release();
                Directory directory;
                while (true) {
                    errno = 0;
                    const auto* entry = readdir(stream.get());
                    if (!entry) {
                        if (errno)
                            return { .error = fileError() };
                        break;
                    }
                    const std::string_view child(entry->d_name);
                    if (child == "." || child == "..")
                        continue;
                    struct stat childStatus {};
                    if (fstatat(pending.descriptor.get(), entry->d_name, &childStatus, AT_SYMLINK_NOFOLLOW) < 0)
                        return { .error = fileError() };
                    if (!isAdmitted(childStatus.st_mode))
                        continue;
                    if (directory.names.size() == maxEntries || child.size() + 1 > maxDirectoryBytes - pending.bytes.size())
                        return { .error = DropFileError::NoMemory };
                    FallibleString childName;
                    if (!childName.tryAssign(child) || !directory.names.tryPushBack(std::move(childName)))
                        return { .error = DropFileError::NoMemory };
                    const auto before = pending.bytes.size();
                    const auto needed = before + child.size() + 1;
                    if ((needed > pending.bytes.capacity()
                            && !pending.bytes.tryReserve(std::min(maxDirectoryBytes, std::max(needed, pending.bytes.capacity() * 2))))
                        || !pending.bytes.tryResize(needed))
                        return { .error = DropFileError::NoMemory };
                    std::memcpy(pending.bytes.data() + before, child.data(), child.size());
                    pending.bytes[before + child.size()] = '\0';
                }
                // The final separator is not an empty directory entry.
                if (!pending.bytes.empty())
                    pending.bytes.pop_back();
                directory.descriptor = std::move(pending.descriptor);
                directory.handle = ++nextDirectory;
                kind = directory.handle;
                *directorySlot = std::move(directory);
            }
        }
        pending.token = ++nextToken;
        *slot = std::move(pending);
        return { .token = slot->token, .kind = kind };
    }
};

DropFiles::DropFiles()
    : m_impl(new (std::nothrow) Impl)
{
}
DropFiles::~DropFiles() = default;
DropFiles::DropFiles(DropFiles&&) noexcept = default;
DropFiles& DropFiles::operator=(DropFiles&&) noexcept = default;

DropFileError DropFiles::addPath(std::string_view path)
{
    if (!m_impl)
        return DropFileError::NoMemory;
    while (path.size() > 1 && path.back() == '/')
        path.remove_suffix(1);
    if (path.empty() || path.front() != '/' || path.size() >= maxPathBytes || path.find('\0') != std::string_view::npos)
        return DropFileError::Invalid;
    if (m_impl->roots.size() == maxEntries)
        return DropFileError::TooManyFiles;
    const auto slash = path.rfind('/');
    const auto name = path == "/" ? std::string_view(".") : path.substr(slash + 1);
    if (name.empty() || (name == "." && path != "/") || name == "..")
        return DropFileError::Invalid;
    FallibleString parent;
    Impl::Entry entry;
    if (!parent.tryAssign(slash ? path.substr(0, slash) : "/") || !entry.name.tryAssign(name))
        return DropFileError::NoMemory;
    entry.parent.reset(open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!entry.parent.isValid())
        return fileError();
    if (!m_impl->roots.tryPushBack(std::move(entry)))
        return DropFileError::NoMemory;
    return DropFileError::None;
}

DropFileRead DropFiles::readRoot(std::size_t index)
{
    if (!m_impl)
        return { .error = DropFileError::NoMemory };
    if (!index || index > m_impl->roots.size())
        return { .error = DropFileError::NotFound };
    const auto& root = m_impl->roots[index - 1];
    return m_impl->start(root.parent.get(), root.name.c_str());
}

DropFileRead DropFiles::readChild(std::int64_t handle, std::size_t index)
{
    if (!m_impl)
        return { .error = DropFileError::NoMemory };
    const auto directory = std::ranges::find(m_impl->directories, handle, &Impl::Directory::handle);
    if (handle < 2 || directory == m_impl->directories.end())
        return { .error = DropFileError::Invalid };
    if (!index || index > directory->names.size())
        return { .error = DropFileError::NotFound };
    return m_impl->start(directory->descriptor.get(), directory->names[index - 1].c_str());
}

DropFileChunk DropFiles::read(std::uint64_t token, std::span<char> output)
{
    if (!m_impl)
        return { .error = DropFileError::NoMemory };
    const auto slot = std::ranges::find(m_impl->reads, token, &Impl::Read::token);
    if (!token || slot == m_impl->reads.end() || output.empty())
        return { .error = DropFileError::Invalid };
    std::size_t size = 0;
    if (slot->descriptor.isValid()) {
        ssize_t received;
        do {
            received = ::read(slot->descriptor.get(), output.data(), output.size());
        } while (received < 0 && errno == EINTR);
        if (received < 0) {
            const auto error = fileError();
            *slot = {};
            return { .error = error };
        }
        size = static_cast<std::size_t>(received);
    } else {
        size = std::min(output.size(), slot->bytes.size() - slot->offset);
        if (size)
            std::memcpy(output.data(), slot->bytes.data() + slot->offset, size);
        slot->offset += size;
    }
    if (!size)
        *slot = {};
    return { .size = size, .isComplete = !size };
}

void DropFiles::closeDirectory(std::int64_t handle)
{
    if (!m_impl || handle < 2)
        return;
    const auto directory = std::ranges::find(m_impl->directories, handle, &Impl::Directory::handle);
    if (directory != m_impl->directories.end())
        *directory = {};
}

} // namespace Ubiquitty::PAL
