// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "PAL/ReceivedDropFiles.h"

#include "Base/FallibleString.h"
#include "Base/FallibleVector.h"
#include "PAL/UnixFileDescriptor.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <new>
#include <sys/stat.h>
#include <unistd.h>

namespace Ubiquitty::PAL {
namespace {
constexpr std::size_t maxEntries = 16384;
constexpr std::size_t maxDirectoryBytes = 1024 * 1024;
constexpr std::size_t maxFileBytes = 1024 * 1024 * 1024;
constexpr std::size_t maxRoots = 1024;
constexpr std::size_t noParent = std::numeric_limits<std::size_t>::max();

bool isEntryName(std::string_view name)
{
    return !name.empty() && name.size() <= 255 && name != "." && name != ".."
        && name.find('/') == std::string_view::npos && name.find('\0') == std::string_view::npos;
}

DropFileError destinationError()
{
    if (errno == ENOMEM)
        return DropFileError::NoMemory;
    if (errno == EMFILE || errno == ENFILE || errno == ENOSPC)
        return DropFileError::TooManyFiles;
    if (errno == EACCES || errno == EPERM)
        return DropFileError::NotPermitted;
    if (errno == EEXIST || errno == ELOOP || errno == EINVAL)
        return DropFileError::Invalid;
    return DropFileError::InputOutput;
}
}

struct ReceivedDropFiles::Impl {
    struct Entry {
        FallibleString name;
        UnixFileDescriptor descriptor;
        FallibleVector<char> data;
        std::size_t root = 0;
        std::size_t parent = noParent;
        std::size_t child = 0;
        std::int64_t kind = 0;
        bool hasStarted = false;
        bool isComplete = false;
    };
    FallibleString directoryPath;
    UnixFileDescriptor directory;
    FallibleVector<Entry> entries;
    std::array<std::size_t, maxRoots> roots {};
    std::array<UnixFileDescriptor, maxRoots> containers;
    std::size_t rootCount = 0;
    std::size_t bytes = 0;
    std::size_t directoryCount = 0;
    std::size_t openFiles = 0;
    bool hasFailed = false;

    explicit Impl(std::string_view parent)
    {
        if (parent.empty() || parent.size() > 4096 || parent.find('\0') != std::string_view::npos)
            return;
        constexpr std::string_view suffix = "/ubiquitty-drop-XXXXXX";
        if (!directoryPath.tryResize(parent.size() + suffix.size()))
            return;
        std::memcpy(directoryPath.data(), parent.data(), parent.size());
        std::memcpy(directoryPath.data() + parent.size(), suffix.data(), suffix.size());
        if (!mkdtemp(directoryPath.data())) {
            directoryPath = {};
            return;
        }
        directory.reset(open(directoryPath.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    }

    ~Impl()
    {
        for (std::size_t index = entries.size(); index > 0; --index) {
            auto& entry = entries[index - 1];
            if (!entry.hasStarted)
                continue;
            const int parent = parentDescriptor(entry);
            if (parent >= 0)
                (void)unlinkat(parent, entry.name.c_str(), entry.kind != 0 && entry.kind != 1 ? AT_REMOVEDIR : 0);
        }
        for (std::size_t root = 0; root < rootCount; ++root) {
            std::array<char, 32> name {};
            (void)std::to_chars(name.data(), name.data() + name.size() - 1, root + 1);
            (void)unlinkat(directory.get(), name.data(), AT_REMOVEDIR);
        }
        if (!directoryPath.empty())
            (void)rmdir(directoryPath.c_str());
    }

    int parentDescriptor(const Entry& entry) const
    {
        return entry.parent == noParent ? containers[entry.root - 1].get() : entries[entry.parent].descriptor.get();
    }

    DropFileError begin(std::size_t index, std::int64_t kind)
    {
        auto& entry = entries[index];
        if (kind < std::numeric_limits<std::int32_t>::min() || kind > std::numeric_limits<std::uint32_t>::max())
            return DropFileError::Invalid;
        if (entry.hasStarted)
            return entry.kind == kind && !entry.isComplete ? DropFileError::None : DropFileError::Invalid;
        const auto parent = parentDescriptor(entry);
        if (kind == 0) {
            if (openFiles == 64)
                return DropFileError::TooManyFiles;
            entry.descriptor.reset(openat(parent, entry.name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
            if (!entry.descriptor.isValid())
                return destinationError();
            ++openFiles;
        } else if (kind != 1) {
            if (directoryCount == 128)
                return DropFileError::TooManyFiles;
            if (std::ranges::any_of(entries, [kind](const auto& candidate) { return candidate.hasStarted && candidate.kind == kind; }))
                return DropFileError::Invalid;
            if (mkdirat(parent, entry.name.c_str(), 0700) < 0)
                return destinationError();
            entry.descriptor.reset(openat(parent, entry.name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
            if (!entry.descriptor.isValid()) {
                const auto error = destinationError();
                (void)unlinkat(parent, entry.name.c_str(), AT_REMOVEDIR);
                return error;
            }
            ++directoryCount;
        }
        entry.kind = kind;
        entry.hasStarted = true;
        return DropFileError::None;
    }

    DropFileError finish(std::size_t index)
    {
        auto& entry = entries[index];
        if (entry.kind == 0) {
            entry.descriptor.reset();
            --openFiles;
        } else if (entry.kind == 1) {
            const std::string_view target(entry.data.data(), entry.data.size());
            if (target.empty() || target.find('\0') != std::string_view::npos)
                return DropFileError::Invalid;
            if (!entry.data.tryPushBack('\0'))
                return DropFileError::NoMemory;
            if (symlinkat(entry.data.data(), parentDescriptor(entry), entry.name.c_str()) < 0)
                return destinationError();
            entry.data = {};
        } else {
            auto list = std::move(entry.data);
            std::string_view names(list.data(), list.size());
            if (!names.empty() && names.back() == '\0')
                names.remove_suffix(1);
            std::size_t child = 0;
            const auto root = entry.root;
            while (!names.empty()) {
                const auto end = names.find('\0');
                const auto name = names.substr(0, end);
                if (!isEntryName(name) || std::ranges::any_of(entries, [&](const auto& other) { return other.parent == index && other.name.view() == name; }))
                    return DropFileError::Invalid;
                if (entries.size() == maxEntries)
                    return DropFileError::TooManyFiles;
                Entry descendant;
                descendant.parent = index;
                descendant.root = root;
                descendant.child = ++child;
                if (!descendant.name.tryAssign(name) || !entries.tryPushBack(std::move(descendant)))
                    return DropFileError::NoMemory;
                if (end == std::string_view::npos)
                    break;
                names.remove_prefix(end + 1);
                if (names.empty())
                    return DropFileError::Invalid;
            }
        }
        entries[index].isComplete = true;
        return DropFileError::None;
    }
};

ReceivedDropFiles::ReceivedDropFiles(std::string_view directory)
    : m_impl(new (std::nothrow) Impl(directory))
{
}
ReceivedDropFiles::~ReceivedDropFiles() = default;
ReceivedDropFiles::ReceivedDropFiles(ReceivedDropFiles&&) noexcept = default;
ReceivedDropFiles& ReceivedDropFiles::operator=(ReceivedDropFiles&&) noexcept = default;

DropFileError ReceivedDropFiles::addRoot(std::string_view name)
{
    if (!m_impl)
        return DropFileError::NoMemory;
    auto& state = *m_impl;
    if (!state.directory.isValid() || state.hasFailed)
        return DropFileError::InputOutput;
    if (!isEntryName(name))
        return DropFileError::Invalid;
    if (state.rootCount == maxRoots || state.entries.size() == maxEntries)
        return DropFileError::TooManyFiles;
    Impl::Entry entry;
    if (!entry.name.tryAssign(name))
        return DropFileError::NoMemory;
    std::array<char, 32> container {};
    (void)std::to_chars(container.data(), container.data() + container.size() - 1, state.rootCount + 1);
    if (mkdirat(state.directory.get(), container.data(), 0700) < 0)
        return destinationError();
    auto& descriptor = state.containers[state.rootCount];
    descriptor.reset(openat(state.directory.get(), container.data(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!descriptor.isValid()) {
        const auto error = destinationError();
        (void)unlinkat(state.directory.get(), container.data(), AT_REMOVEDIR);
        return error;
    }
    entry.root = state.rootCount + 1;
    state.roots[state.rootCount++] = state.entries.size();
    if (!state.entries.tryPushBack(std::move(entry))) {
        state.hasFailed = true;
        return DropFileError::NoMemory;
    }
    return DropFileError::None;
}

DropFileError ReceivedDropFiles::writeEntry(std::size_t root, std::int64_t parent, std::size_t child,
    std::int64_t kind, DropFileBytes bytes, DropEntryCompletion completion)
{
    if (!m_impl)
        return DropFileError::NoMemory;
    auto& state = *m_impl;
    const auto refuse = [&](DropFileError error) { state.hasFailed = true; return error; };
    if (!state.directory.isValid() || state.hasFailed)
        return refuse(DropFileError::InputOutput);
    if (!root || root > state.rootCount || (!parent && child) || (parent && !child))
        return refuse(DropFileError::Invalid);
    std::size_t index = state.roots[root - 1];
    if (parent) {
        const auto directory = std::ranges::find_if(state.entries, [&](const auto& entry) { return entry.root == root && entry.hasStarted && entry.isComplete && entry.kind == parent && parent != 1; });
        if (directory == state.entries.end())
            return refuse(DropFileError::Invalid);
        const auto parentIndex = static_cast<std::size_t>(directory - state.entries.begin());
        const auto entry = std::ranges::find_if(state.entries, [&](const auto& value) { return value.parent == parentIndex && value.child == child; });
        if (entry == state.entries.end())
            return refuse(DropFileError::Invalid);
        index = static_cast<std::size_t>(entry - state.entries.begin());
    }
    if (bytes.size() > maxFileBytes - state.bytes)
        return refuse(DropFileError::TooManyFiles);
    if (const auto result = state.begin(index, kind); result != DropFileError::None)
        return refuse(result);
    auto& entry = state.entries[index];
    if (completion == DropEntryCompletion::Complete && !bytes.empty())
        return refuse(DropFileError::Invalid);
    state.bytes += bytes.size();
    if (kind == 0) {
        while (!bytes.empty()) {
            const auto size = ::write(entry.descriptor.get(), bytes.data(), bytes.size());
            if (size < 0 && errno == EINTR)
                continue;
            if (size <= 0)
                return refuse(destinationError());
            bytes = bytes.subspan(static_cast<std::size_t>(size));
        }
    } else {
        const std::size_t limit = kind == 1 ? 4096 : maxDirectoryBytes;
        if (bytes.size() > limit - entry.data.size())
            return refuse(DropFileError::TooManyFiles);
        const auto before = entry.data.size();
        if (!entry.data.tryResize(before + bytes.size()))
            return refuse(DropFileError::NoMemory);
        if (!bytes.empty())
            std::memcpy(entry.data.data() + before, bytes.data(), bytes.size());
    }
    if (completion == DropEntryCompletion::Complete) {
        if (const auto result = state.finish(index); result != DropFileError::None)
            return refuse(result);
    }
    return DropFileError::None;
}

bool ReceivedDropFiles::isComplete(std::size_t root) const
{
    return m_impl && !m_impl->hasFailed && root && root <= m_impl->rootCount
        && std::ranges::none_of(m_impl->entries, [root](const auto& entry) { return entry.root == root && !entry.isComplete; });
}

std::string ReceivedDropFiles::path(std::size_t root) const
{
    if (!isComplete(root))
        return {};
    return std::string(m_impl->directoryPath.view()) + "/" + std::to_string(root) + "/" + std::string(m_impl->entries[m_impl->roots[root - 1]].name.view());
}

} // namespace Ubiquitty::PAL
