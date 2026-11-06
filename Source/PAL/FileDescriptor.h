// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// The abstract owner of one native transport handle. PAL::Stream is written
// against this, not against a file descriptor: the portable transport interface
// must not name a POSIX type, or every consumer of Stream compiles against
// <unistd.h> to read and write bytes.
//
// UnixFileDescriptor is the POSIX implementation and the only one today. It adds
// the raw-int accessors (get / release / reset) that POSIX callers need; code
// holding a FileDescriptor& sees ownership and validity only, which is the whole
// portable contract — open it, know whether it is live, close it exactly once.
//
// Not here: any way to obtain a native handle. A `nativeHandle()`
// on this base would make the abstraction ornamental, since every caller would
// reach through it. Code that genuinely needs the descriptor names
// UnixFileDescriptor and lives behind the POSIX seam (Platform/POSIX/, or a
// backend under Platform/).

namespace Ubiquitty::PAL {

class FileDescriptor {
public:
    virtual ~FileDescriptor() = default;

    // True while this owns a live handle.
    [[nodiscard]] virtual bool isValid() const = 0;

    // Release the handle back to the OS. Idempotent: closing twice is not an
    // error, and the destructor calls it, so every teardown path is leak-free.
    virtual void closeHandle() = 0;

    explicit operator bool() const { return isValid(); }

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

protected:
    FileDescriptor() = default;
    FileDescriptor(FileDescriptor&&) = default;
    FileDescriptor& operator=(FileDescriptor&&) = default;
};

} // namespace Ubiquitty::PAL
