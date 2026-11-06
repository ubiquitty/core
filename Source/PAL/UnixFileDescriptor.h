// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// Move-only RAII owner for one POSIX file descriptor. The destructor closes a
// live fd, so every teardown path is leak-free by construction. Move-only:
// exactly one owner at a time, so a moved-from owner never double-closes. Hand an
// fd off to a C API that takes ownership via release(); take one over via reset().
//
// Backend code that belongs under Platform/POSIX, parked here because Core still
// declares members of this type in Transport/PollChannel.h and
// Session/BoundedSocketServer.{h,cpp}. Moving it to the backend makes those a
// Core -> Platform edge, which is a dependency cycle, so the header follows once
// both reach the OS through PAL::Socket and PAL::Poll instead.
//
// Those two are what is left. This said "six ... and the four TUs on the POSIX
// ledger, and Scripts/check-core-posix.py tracks the same six"; that ledger is
// now empty and the gate reports "0 known, 0 new", so the four cleared and the
// sentence outlived them. Named rather than counted, because a count is what
// went stale — grep the type to check this paragraph.

#include "PAL/FileDescriptor.h"

#include <unistd.h>

#include <utility>

namespace Ubiquitty::PAL {

class UnixFileDescriptor final : public FileDescriptor {
public:
    UnixFileDescriptor() = default;
    explicit UnixFileDescriptor(int fileDescriptor)
        : m_fileDescriptor(fileDescriptor)
    {
    }

    UnixFileDescriptor(UnixFileDescriptor&& other) noexcept
        : m_fileDescriptor(std::exchange(other.m_fileDescriptor, -1))
    {
    }
    UnixFileDescriptor& operator=(UnixFileDescriptor&& other) noexcept
    {
        if (this != &other)
            reset(std::exchange(other.m_fileDescriptor, -1));
        return *this;
    }

    UnixFileDescriptor(const UnixFileDescriptor&) = delete;
    UnixFileDescriptor& operator=(const UnixFileDescriptor&) = delete;

    /// Closes the owned descriptor, if there is one.
    ///
    /// `override` rather than a plain destructor: the base declares one virtual,
    /// so a signature that stops matching becomes a compile error instead of a
    /// function that silently no longer runs on delete-through-base.
    ~UnixFileDescriptor() override { reset(); }

    // The owned descriptor, or -1 when empty. Borrowed — the owner still closes it.
    [[nodiscard]] int get() const { return m_fileDescriptor; }
    [[nodiscard]] bool isValid() const override { return m_fileDescriptor >= 0; }

    // FileDescriptor::closeHandle — the portable teardown. Discards close()'s
    // errno: a caller that needs it calls close() directly.
    void closeHandle() override { (void)close(); }

    [[nodiscard]] UnixFileDescriptor duplicate() const
    {
        if (!isValid())
            return {};
        return UnixFileDescriptor(::dup(m_fileDescriptor));
    }

    // Close the current fd (if any) and take `fileDescriptor` (default: become empty).
    void reset(int fileDescriptor = -1)
    {
        if (m_fileDescriptor >= 0 && m_fileDescriptor != fileDescriptor)
            ::close(m_fileDescriptor);
        m_fileDescriptor = fileDescriptor;
    }

    [[nodiscard]] int close()
    {
        const int fileDescriptor = std::exchange(m_fileDescriptor, -1);
        if (fileDescriptor < 0)
            return 0;
        return ::close(fileDescriptor);
    }

    // Relinquish ownership without closing — for handing the fd to a C API that
    // adopts it (e.g. a struct that will close it itself). Returns the fd; the
    // owner becomes empty.
    [[nodiscard]] int release() { return std::exchange(m_fileDescriptor, -1); }

private:
    int m_fileDescriptor { -1 };
};

struct UnixPipe {
    UnixFileDescriptor read;
    UnixFileDescriptor write;

    [[nodiscard]] bool isValid() const { return read && write; }
    explicit operator bool() const { return isValid(); }
};

[[nodiscard]] inline UnixPipe makeUnixPipe()
{
    int fileDescriptors[2] = { -1, -1 };
    if (::pipe(fileDescriptors) != 0)
        return {};
    return { UnixFileDescriptor(fileDescriptors[0]), UnixFileDescriptor(fileDescriptors[1]) };
}

} // namespace Ubiquitty::PAL
