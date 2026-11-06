// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Platform/Host/ResumeRecordTool.h"

#include "Core/Protocol/Sync/Connection.h"
#include "Core/Session/ResumeRecordFormat.h"
#include "PAL/UnixFileDescriptor.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace Ubiquitty::Host {
namespace {

/// One window's record in a private file. A write replaces the file whole:
/// the bytes go to a fresh sibling that is flushed, renamed over the record,
/// and the directory flushed, so a crash leaves the old record or the new.
class PrivateFileSlot final : public Session::ResumeRecordSlot {
public:
    explicit PrivateFileSlot(std::string path)
        : m_path(std::move(path))
        , m_directory(m_path.substr(0, m_path.find_last_of('/')))
    {
    }

    /// Whether the directory is this user's alone, so the record's screen
    /// text is readable by nobody else.
    [[nodiscard]] bool isPrivate() const
    {
        struct stat status {};
        return !m_directory.empty() && ::lstat(m_directory.c_str(), &status) == 0 && S_ISDIR(status.st_mode)
            && status.st_uid == ::geteuid() && !(status.st_mode & 0077);
    }

    bool write(std::span<const std::uint8_t> bytes) override
    {
        const std::string temporary = m_path + ".writing";
        (void)::unlink(temporary.c_str());
        PAL::UnixFileDescriptor file(::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
        if (!file)
            return false;
        for (std::size_t written = 0; written < bytes.size();) {
            const ssize_t step = ::write(file.get(), bytes.data() + written, bytes.size() - written);
            if (step < 0 && errno == EINTR)
                continue;
            if (step <= 0) {
                (void)::unlink(temporary.c_str());
                return false;
            }
            written += static_cast<std::size_t>(step);
        }
        if (::fsync(file.get()) != 0 || ::rename(temporary.c_str(), m_path.c_str()) != 0) {
            (void)::unlink(temporary.c_str());
            return false;
        }
        return flushDirectory();
    }

    std::optional<std::vector<std::uint8_t>> read(std::size_t maxBytes) override
    {
        PAL::UnixFileDescriptor file(::open(m_path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
        struct stat status {};
        if (!file || ::fstat(file.get(), &status) != 0 || !S_ISREG(status.st_mode) || status.st_size < 0 || static_cast<std::size_t>(status.st_size) > maxBytes)
            return std::nullopt;
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(status.st_size));
        for (std::size_t done = 0; done < bytes.size();) {
            const ssize_t step = ::read(file.get(), bytes.data() + done, bytes.size() - done);
            if (step < 0 && errno == EINTR)
                continue;
            if (step <= 0)
                return std::nullopt;
            done += static_cast<std::size_t>(step);
        }
        return bytes;
    }

    bool erase() override
    {
        if (::unlink(m_path.c_str()) != 0 && errno != ENOENT)
            return false;
        return flushDirectory();
    }

private:
    [[nodiscard]] bool flushDirectory() const
    {
        PAL::UnixFileDescriptor directory(::open(m_directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        return directory && ::fsync(directory.get()) == 0;
    }

    std::string m_path;
    std::string m_directory;
};

/// Reads all of standard input, refusing more than `maxBytes`.
std::optional<std::vector<std::uint8_t>> readStandardInput(std::size_t maxBytes)
{
    std::vector<std::uint8_t> bytes;
    std::array<std::uint8_t, 65536> chunk {};
    for (;;) {
        const ssize_t step = ::read(STDIN_FILENO, chunk.data(), chunk.size());
        if (step < 0 && errno == EINTR)
            continue;
        if (step < 0)
            return std::nullopt;
        if (step == 0)
            return bytes;
        if (static_cast<std::size_t>(step) > maxBytes - bytes.size())
            return std::nullopt;
        bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + step);
    }
}

/// Writes all of `bytes` to standard output.
bool writeStandardOutput(std::span<const std::uint8_t> bytes)
{
    for (std::size_t done = 0; done < bytes.size();) {
        const ssize_t step = ::write(STDOUT_FILENO, bytes.data() + done, bytes.size() - done);
        if (step < 0 && errno == EINTR)
            continue;
        if (step <= 0)
            return false;
        done += static_cast<std::size_t>(step);
    }
    return true;
}

} // namespace

int runResumeRecordTool(std::span<char*> arguments)
{
    if (arguments.size() != 2 || !arguments[1] || arguments[1][0] != '/') {
        std::fprintf(stderr, "usage: ubiquitty-host resume-record <write|read|purge> ABSOLUTE_PATH\n");
        return 2;
    }
    const std::string_view verb = arguments[0];
    PrivateFileSlot slot(arguments[1]);
    if (!slot.isPrivate()) {
        std::fprintf(stderr, "ubiquitty-host: a resume record lives in a directory only its owner can open\n");
        return 2;
    }
    // A record holds at most the client's checkpoint cap of pictures.
    const std::size_t budget = Protocol::Sync::measuredClientPolicy().maxCheckpointBytes;
    Session::EncodedResumeRecordStore store(slot, budget);
    if (verb == "write") {
        const auto request = readStandardInput(budget + 4096);
        if (!request)
            return 2;
        const auto record = Session::recordFromPictureRequest(*request, budget);
        if (record && store.write(*record) == Protocol::Sync::ResumeWriteOutcome::Committed)
            return 0;
        return store.purge() == Protocol::Sync::ResumePurgeOutcome::Purged ? 1 : 2;
    }
    if (verb == "read") {
        const auto reply = Session::resumeReplyFromStore(store);
        if (!reply)
            return 1;
        return writeStandardOutput(*reply) ? 0 : 2;
    }
    if (verb == "purge")
        return store.purge() == Protocol::Sync::ResumePurgeOutcome::Purged ? 0 : 2;
    std::fprintf(stderr, "usage: ubiquitty-host resume-record <write|read|purge> ABSOLUTE_PATH\n");
    return 2;
}

}
