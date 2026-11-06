// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Serial / RS-232 byte-stream backend for the portable Stream object. Opens a
// tty line in raw 8N1 mode on a non-blocking fd and presents it as a session. No
// child process: the process facet reports an empty session, and the session ends
// when the device read errors (cable pulled). Single-threaded by construction;
// write backpressure reuses the shared OutputQueue like PTYStream. Pure POSIX
// termios; the standard baud constants are handled with a portable rate→speed_t map.

#include "PAL/OutputQueue.h"
#include "PAL/Stream.h"
#include "PAL/UnixFileDescriptor.h"

#include <new>
#if defined(__APPLE__)
#include <os/lock.h>
#else
#include <mutex>
#endif

#include <errno.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace Ubiquitty::PAL {

namespace {

#if defined(__APPLE__)
struct SerialOutputLock {
    os_unfair_lock lock = OS_UNFAIR_LOCK_INIT;
    void acquire() { os_unfair_lock_lock(&lock); }
    void release() { os_unfair_lock_unlock(&lock); }
};
#else
struct SerialOutputLock {
    std::mutex lock;
    void acquire() { lock.lock(); }
    void release() { lock.unlock(); }
};
#endif

int setNonBlocking(int fileDescriptor)
{
    int flags = ::fcntl(fileDescriptor, F_GETFL, 0);
    if (flags == -1)
        return errno;
    if (::fcntl(fileDescriptor, F_SETFL, flags | O_NONBLOCK) == -1)
        return errno;
    return 0;
}

// Map a numeric baud rate to its termios speed_t constant. Returns false for an
// unsupported rate so the caller can leave the port's current speed untouched.
bool baudConstant(int baudRate, speed_t& out)
{
    switch (baudRate) {
    case 1200:
        out = B1200;
        return true;
    case 2400:
        out = B2400;
        return true;
    case 4800:
        out = B4800;
        return true;
    case 9600:
        out = B9600;
        return true;
    case 19200:
        out = B19200;
        return true;
    case 38400:
        out = B38400;
        return true;
    case 57600:
        out = B57600;
        return true;
    case 115200:
        out = B115200;
        return true;
    case 230400:
        out = B230400;
        return true;
    default:
        return false;
    }
}

// Put `fileDescriptor` in raw 8N1 mode at `baudRate` (0 = keep current speed).
// Returns 0 on success, else errno.
int configureRaw8N1(int fileDescriptor, int baudRate)
{
    struct termios settings;
    settings = {};
    if (::tcgetattr(fileDescriptor, &settings) != 0)
        return errno;
    // cfmakeraw: no canonical line discipline, no echo, no signal chars, 8-bit,
    // pass every byte through unaltered — the terminal does its own VT parsing.
    ::cfmakeraw(&settings);
    settings.c_cflag |= (CLOCAL | CREAD); // ignore modem control lines; enable receiver
    settings.c_cflag &= ~static_cast<tcflag_t>(CSTOPB); // one stop bit
    settings.c_cflag &= ~static_cast<tcflag_t>(PARENB); // no parity (8N1)
    settings.c_cflag &= ~static_cast<tcflag_t>(CSIZE);
    settings.c_cflag |= CS8; // 8 data bits
    settings.c_cc[VMIN] = 0; // non-blocking read semantics on top of O_NONBLOCK
    settings.c_cc[VTIME] = 0;
    speed_t speed = B0;
    if (baudRate != 0 && baudConstant(baudRate, speed)) {
        ::cfsetispeed(&settings, speed);
        ::cfsetospeed(&settings, speed);
    }
    if (::tcsetattr(fileDescriptor, TCSANOW, &settings) != 0)
        return errno;
    return 0;
}

// The empty process facet for a serial line: there is no shell / process tree, so
// every query reports "no session". foregroundIsShell() returns true (a serial
// line is never "running a foreground job" in the sense the title poll means).
class SerialProcessFacts final : public ProcessFacts {
public:
    std::string foregroundName() override { return {}; }
    bool foregroundIsShell() override { return true; }
    std::optional<ProcessID> foregroundProcessGroup() override { return std::nullopt; }
    std::string foregroundCurrentDirectory() override { return {}; }
    std::string ttyName() override { return m_ttyName; }
    bool hasRunningChild() override { return false; }
    int gatherProcesses(std::span<PAL::Process::Record>) override { return 0; }

    std::string m_ttyName; // the device basename, e.g. "cu.usbserial-1410"
};

} // anonymous namespace

class SerialStream final : public Stream {
public:
    SerialStream(UnixFileDescriptor fileDescriptor, std::string ttyName)
        : m_fileDescriptor(std::move(fileDescriptor))
    {
        m_processFacts.m_ttyName = std::move(ttyName);
    }
    ~SerialStream() override;

    int read(std::span<char> buffer) override;
    int write(std::span<const char> bytes) override;
    int writeAll(std::span<const char> bytes) override;
    bool drainPendingWrite() override;
    bool hasPendingWrite() const override;
    void setWindowSize(int, int, int, int) override { } // a serial line has no winsize
    int lastError() const override { return m_lastError; }
    int fileDescriptor() const override { return m_fileDescriptor.get(); }
    ProcessFacts& process() override { return m_processFacts; }

private:
    int writeOnceToDevice(std::string_view bytes);
    static int writeSink(void* context, std::span<const char> bytes) { return static_cast<SerialStream*>(context)->writeOnceToDevice({ bytes.data(), bytes.size() }); }

    UnixFileDescriptor m_fileDescriptor;
    int m_lastError = 0;
    bool m_endOfFile = false;
    SerialProcessFacts m_processFacts;
    mutable SerialOutputLock m_outputLock;
    /* owned */ OutputQueue* m_outputQueue = nullptr;
};

int SerialStream::read(std::span<char> buffer)
{
    if (buffer.empty())
        return std::to_underlying(StreamStatus::Fatal);
    if (!m_fileDescriptor)
        return std::to_underlying(StreamStatus::Closed);
    if (m_endOfFile)
        return std::to_underlying(StreamStatus::EndOfFile);

    ssize_t n = ::read(m_fileDescriptor.get(), buffer.data(), buffer.size());
    if (n > 0) {
        m_lastError = 0;
        return static_cast<int>(n);
    }
    if (n == 0) {
        m_endOfFile = true;
        m_lastError = 0;
        return std::to_underlying(StreamStatus::EndOfFile);
    }
    int err = errno;
    if (err == EAGAIN || err == EWOULDBLOCK || err == EINTR) {
        m_lastError = 0;
        return std::to_underlying(StreamStatus::Again);
    }
    // ENXIO/EIO: the device was unplugged. End the session.
    if (err == ENXIO || err == EIO || err == EBADF) {
        m_endOfFile = true;
        m_lastError = 0;
        return std::to_underlying(StreamStatus::EndOfFile);
    }
    m_lastError = err;
    return std::to_underlying(StreamStatus::Fatal);
}

int SerialStream::write(std::span<const char> bytes)
{
    if (!m_fileDescriptor)
        return std::to_underlying(StreamStatus::Closed);
    if (bytes.empty()) {
        m_lastError = 0;
        return 0;
    }
    ssize_t n = ::write(m_fileDescriptor.get(), bytes.data(), bytes.size());
    if (n > 0) {
        m_lastError = 0;
        return static_cast<int>(n);
    }
    if (n == 0) {
        m_lastError = 0;
        return std::to_underlying(StreamStatus::Again);
    }
    int err = errno;
    if (err == EAGAIN || err == EWOULDBLOCK || err == EINTR) {
        m_lastError = 0;
        return std::to_underlying(StreamStatus::Again);
    }
    if (err == ENXIO || err == EIO || err == EBADF)
        return std::to_underlying(StreamStatus::Closed);
    m_lastError = err;
    return std::to_underlying(StreamStatus::Fatal);
}

int SerialStream::writeOnceToDevice(std::string_view bytes)
{
    if (!m_fileDescriptor)
        return -1;
    ssize_t n = ::write(m_fileDescriptor.get(), bytes.data(), bytes.size());
    if (n >= 0)
        return static_cast<int>(n);
    int err = errno;
    if (err == EAGAIN || err == EWOULDBLOCK || err == EINTR)
        return 0; // device buffer full — retry later
    if (err == ENXIO || err == EIO || err == EBADF)
        return -1; // device gone
    m_lastError = err;
    return -1;
}

int SerialStream::writeAll(std::span<const char> bytes)
{
    if (!m_fileDescriptor)
        return std::to_underlying(StreamStatus::Closed);
    if (bytes.empty())
        return 0;
    m_outputLock.acquire();
    if (!m_outputQueue)
        m_outputQueue = new (std::nothrow) OutputQueue();
    int rc = 0;
    if (m_outputQueue) {
        rc = m_outputQueue->submit(bytes, writeSink, this);
    } else {
        ssize_t n = ::write(m_fileDescriptor.get(), bytes.data(), bytes.size());
        (void)n;
    }
    m_outputLock.release();
    if (rc < 0)
        return static_cast<int>(m_fileDescriptor ? StreamStatus::Fatal : StreamStatus::Closed);
    return 0;
}

bool SerialStream::drainPendingWrite()
{
    m_outputLock.acquire();
    bool pending = false;
    if (m_outputQueue) {
        m_outputQueue->drain(writeSink, this);
        pending = m_outputQueue->hasPending();
    }
    m_outputLock.release();
    return pending;
}

bool SerialStream::hasPendingWrite() const
{
    m_outputLock.acquire();
    bool pending = (m_outputQueue != nullptr) && m_outputQueue->hasPending();
    m_outputLock.release();
    return pending;
}

SerialStream::~SerialStream()
{
    m_fileDescriptor.reset();
    delete m_outputQueue;
    m_outputQueue = nullptr;
}

std::unique_ptr<Stream> openSerial(std::string_view devicePath, int baudRate)
{
    if (devicePath.empty())
        return {};
    // NUL-terminate the path for ::open (string_view isn't guaranteed terminated).
    std::string path(devicePath);
    // O_NOCTTY: never make the serial line our controlling terminal. O_NONBLOCK on
    // open avoids hanging on DCD for a modem line with no carrier.
    UnixFileDescriptor fileDescriptor(::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK));
    if (!fileDescriptor)
        return {};
    if (configureRaw8N1(fileDescriptor.get(), baudRate) != 0)
        return {};
    if (setNonBlocking(fileDescriptor.get()) != 0)
        return {};
    // The tty basename for the Inspector/title (matches how PTYStream names its
    // replica).
    std::string base = path;
    const std::size_t slash = base.find_last_of('/');
    if (slash != std::string::npos)
        base = base.substr(slash + 1);

    Stream* stream = new (std::nothrow) SerialStream(std::move(fileDescriptor), std::move(base));
    if (!stream)
        return {};
    return std::unique_ptr<Stream>(stream);
}

} // namespace Ubiquitty::PAL
