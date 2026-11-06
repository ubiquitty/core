// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// Tests/PAL/test_log.cpp
//
// A diagnostic write that fails must not end the process.
//
// std::println throws std::system_error when the write fails. Every binary
// here is built -fno-exceptions, the throw comes out of libc++'s dylib, and
// nothing catches it: the process aborts. The failures are ordinary — a full
// disk, a log pipe whose reader went away, a file-size limit — so a daemon
// running on someone else's machine can die of writing about something else:
// a case in paste_push_landing caps RLIMIT_FSIZE, so with its stdout a regular
// file, the host's next logged line fails the write.
//
// The failing-write case is the one that matters, and it only means anything
// beside a case proving the same call writes what it was given.

#include "PAL/Log.h"

#include <cstdio>
#include <cstring>
#include <string>

#include <csignal>

#include <unistd.h>

namespace {

// The bytes a stream received, or empty when it received none.
std::string contentsOf(const std::string& path)
{
    std::FILE* reader = std::fopen(path.c_str(), "r");
    if (reader == nullptr)
        return std::string();
    std::string text;
    char buffer[512];
    std::size_t got = 0;
    while ((got = std::fread(buffer, 1, sizeof(buffer), reader)) > 0)
        text.append(buffer, got);
    std::fclose(reader);
    return text;
}

} // namespace

int main()
{
    char directoryTemplate[] = "/tmp/ub-log-XXXXXX";
    const char* directory = ::mkdtemp(directoryTemplate);
    CHECK(directory != nullptr);
    if (directory == nullptr)
        return 1;

    // What the call is for: the line, the arguments, and one newline.
    {
        const std::string path = std::string(directory) + "/line.txt";
        std::FILE* destination = std::fopen(path.c_str(), "w");
        CHECK(destination != nullptr);
        if (destination != nullptr) {
            Ubiquitty::PAL::logLine(destination, "session {} reaped, status {}", 7, 0);
            Ubiquitty::PAL::logLine(destination, "second");
            std::fclose(destination);
        }
        CHECK(contentsOf(path) == "session 7 reaped, status 0\nsecond\n");
    }

    // The same without the newline, for a line assembled from several calls.
    {
        const std::string path = std::string(directory) + "/text.txt";
        std::FILE* destination = std::fopen(path.c_str(), "w");
        CHECK(destination != nullptr);
        if (destination != nullptr) {
            Ubiquitty::PAL::logText(destination, "{:02x}", 0xBE);
            Ubiquitty::PAL::logText(destination, "{:02x}", 0xEF);
            std::fclose(destination);
        }
        CHECK(contentsOf(path) == "beef");
    }

    // The case this file exists for. A pipe whose reader has gone is the
    // failure a supervised daemon actually meets, and it fails at the syscall
    // rather than on anything about this machine's state. SIGPIPE is ignored
    // for the duration, or the signal ends the process before the write can
    // report anything. std::println here throws, and reaching the next line at
    // all is the assertion.
    {
        int ends[2] = { -1, -1 };
        CHECK(::pipe(ends) == 0);
        ::close(ends[0]);
        void (*previousHandler)(int) = ::signal(SIGPIPE, SIG_IGN);
        std::FILE* destination = ::fdopen(ends[1], "w");
        CHECK(destination != nullptr);
        if (destination != nullptr)
            std::setvbuf(destination, nullptr, _IONBF, 0);
        bool didWriteFail = false;
        if (destination != nullptr) {
            Ubiquitty::PAL::logLine(destination, "a host says why it is stopping: {}", "disk full");
            Ubiquitty::PAL::logText(destination, "and keeps going");
            // Unbuffered, so each call above reached the pipe rather than a
            // buffer that fclose would flush after the case had ended.
            didWriteFail = std::ferror(destination) != 0;
            std::fclose(destination);
        } else {
            ::close(ends[1]);
        }
        ::signal(SIGPIPE, previousHandler);
        // The write really failed. Without this the case proves nothing: a
        // pipe that happened to accept the bytes would pass it just by not
        // aborting, which is also what a stream nobody wrote to would do.
        CHECK(didWriteFail);
    }

    if (failures == 0)
        std::printf("test_log: all passed\n");
    else
        std::printf("test_log: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
