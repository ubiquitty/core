// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Portable-POSIX half of the PAL "Core calls down" surface: login shell,
// scheduler yield, atomic file storage, process spawn — identical on macOS,
// Linux, and the headless host/agent, so they live in one shared backend TU.
// Every per-OS PAL backend compiles this alongside its own clock/clipboard/bell/
// preferences; the symbol sets do not overlap.

#include "PAL/BoundedFile.h"
#include "PAL/DurableFile.h"
#include "PAL/PAL.h"
#include "PAL/UnixFileDescriptor.h"
#include "Platform/POSIX/DurableCommit.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <string>

#include <fcntl.h>
#include <pthread.h>
#include <pwd.h>
#include <sched.h>
#if defined(__linux__)
#include <sys/syscall.h>
#include <sys/time.h>
#include <utmpx.h>
#endif
#include <spawn.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#include "Platform/iOS/ProtectedFile.h"
#endif
#endif

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

extern char** environ;

namespace Ubiquitty::PAL {

int systemLoginShellPath(char* outputBuffer, int outputCapacity)
{
    if (!outputBuffer || outputCapacity <= 0)
        return -1;
    const struct passwd* entry = ::getpwuid(::getuid());
    if (!entry || !entry->pw_shell || entry->pw_shell[0] == '\0')
        return -1;
    const std::size_t length = std::strlen(entry->pw_shell);
    if (length + 1 > static_cast<std::size_t>(outputCapacity))
        return -1; // would not fit with the NUL terminator
    std::memcpy(outputBuffer, entry->pw_shell, length + 1);
    return static_cast<int>(length);
}

int systemLoginUserName(char* outputBuffer, int outputCapacity)
{
    if (!outputBuffer || outputCapacity <= 0)
        return -1;
    const struct passwd* entry = ::getpwuid(::getuid());
    if (!entry || !entry->pw_name || entry->pw_name[0] == '\0')
        return -1;
    const std::size_t length = std::strlen(entry->pw_name);
    if (length + 1 > static_cast<std::size_t>(outputCapacity))
        return -1; // would not fit with the NUL terminator
    std::memcpy(outputBuffer, entry->pw_name, length + 1);
    return static_cast<int>(length);
}

std::string systemHostName()
{
    std::array<char, 256> name {};
    if (::gethostname(name.data(), name.size()) != 0 || !std::memchr(name.data(), '\0', name.size())) {
        return {};
    }
    return std::string(name.data());
}

LoginSessionKind loginSessionKind()
{
#ifdef __APPLE__
    return LoginSessionKind::ThroughLoginProgram;
#elif defined(__linux__)
    return LoginSessionKind::ByRecordingThePTY;
#else
    return LoginSessionKind::Unregistered;
#endif
}

#if defined(__linux__)

namespace {

// The record's line is the device without its /dev/ prefix, which is what
// `who` prints and what a later removal has to match exactly.
const char* utmpLineFor(const char* ptyName)
{
    if (!ptyName)
        return nullptr;
    static constexpr char devPrefix[] = "/dev/";
    constexpr std::size_t prefixLength = sizeof(devPrefix) - 1;
    return std::strncmp(ptyName, devPrefix, prefixLength) == 0 ? ptyName + prefixLength : ptyName;
}

bool writeSessionRecord(const char* ptyName, short type)
{
    const char* line = utmpLineFor(ptyName);
    if (!line || line[0] == '\0')
        return false;

    utmpx record {};
    record.ut_type = type;
    record.ut_pid = ::getpid();
    std::strncpy(record.ut_line, line, sizeof(record.ut_line) - 1);
    // The id is the line's tail, which is what the platform's own tools use to
    // pair a record with the one that replaces it.
    const std::size_t lineLength = std::strlen(line);
    const std::size_t idLength = sizeof(record.ut_id) - 1;
    std::strncpy(record.ut_id, lineLength > idLength ? line + lineLength - idLength : line, idLength);
    if (type == USER_PROCESS) {
        std::array<char, 256> user {};
        const int n = systemLoginUserName(user.data(), static_cast<int>(user.size()));
        if (n <= 0)
            return false;
        std::strncpy(record.ut_user, user.data(), sizeof(record.ut_user) - 1);
    }
    timeval now {};
    ::gettimeofday(&now, nullptr);
    record.ut_tv.tv_sec = static_cast<decltype(record.ut_tv.tv_sec)>(now.tv_sec);
    record.ut_tv.tv_usec = static_cast<decltype(record.ut_tv.tv_usec)>(now.tv_usec);

    ::setutxent();
    const bool didWrite = ::pututxline(&record) != nullptr;
    ::endutxent();
    return didWrite;
}

} // namespace

bool registerLoginSession(const char* ptyName) { return writeSessionRecord(ptyName, USER_PROCESS); }

void unregisterLoginSession(const char* ptyName) { (void)writeSessionRecord(ptyName, DEAD_PROCESS); }

#else

// Inert off the ByRecordingThePTY kind: Darwin's login program writes and
// clears the record itself.
bool registerLoginSession(const char* /*ptyName*/) { return false; }

void unregisterLoginSession(const char* /*ptyName*/) { }

#endif

int loginSessionProgram(char* outputBuffer, int outputCapacity)
{
#ifdef __APPLE__
    // Darwin's login honours the seam's contract: -p preserves the
    // environment, -f skips authentication for the calling user, -l keeps the
    // caller's working directory. util-linux's spells -p and -f the same way
    // and -l not at all, so one argument vector cannot serve both.
    static constexpr char program[] = "/usr/bin/login";
    if (!outputBuffer || outputCapacity <= 0)
        return -1;
    constexpr std::size_t length = sizeof(program) - 1;
    if (length + 1 > static_cast<std::size_t>(outputCapacity))
        return -1;
    std::memcpy(outputBuffer, program, length + 1);
    return static_cast<int>(length);
#else
    (void)outputBuffer;
    (void)outputCapacity;
    return -1;
#endif
}

int systemHomeDirectory(char* outputBuffer, int outputCapacity)
{
    if (!outputBuffer || outputCapacity <= 0)
        return -1;

    // $HOME first. The account database is the fallback, not the answer:
    // NSHomeDirectory and getpwuid both report pw_dir and neither consults the
    // environment, so a process tree that mixed the two would split across two
    // directories the moment a user set HOME to anywhere else.
    const char* home = ::getenv("HOME");
    if (!home || home[0] == '\0') {
        const struct passwd* entry = ::getpwuid(::getuid());
        home = (entry && entry->pw_dir && entry->pw_dir[0] != '\0') ? entry->pw_dir : nullptr;
    }
    if (!home)
        return -1;

    const std::size_t length = std::strlen(home);
    if (length + 1 > static_cast<std::size_t>(outputCapacity))
        return -1; // would not fit with the NUL terminator
    std::memcpy(outputBuffer, home, length + 1);
    return static_cast<int>(length);
}

void schedulerYield() { ::sched_yield(); }

bool isMainThread()
{
#if defined(__linux__)
    // The main thread's kernel thread id is the process id; glibc names the
    // syscall wrapper only from 2.30, so the syscall is used directly.
    return static_cast<pid_t>(::syscall(SYS_gettid)) == ::getpid();
#else
    return ::pthread_main_np() != 0;
#endif
}

void raiseCurrentThreadToInteractivePriority()
{
#if defined(__APPLE__)
    // USER_INITIATED, the band the Objective-C bridge names when it creates
    // its ingest queue. Parity rather than speed: a paired drain bench found
    // no throughput difference, because a thread created from the main one
    // already inherits a band at least this high. What it buys is that the
    // band stops depending on which thread happened to start ingest.
    ::pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
#endif
}

void lowerCurrentThreadToUtilityPriority()
{
#if defined(__APPLE__)
    ::pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#elif defined(__linux__)
    // A thread's nice value is its own on Linux, so this lowers the worker and
    // nothing else. A refusal leaves it where it was, which is still correct.
    errno = 0;
    const auto thread = static_cast<id_t>(::syscall(SYS_gettid));
    const int current = ::getpriority(PRIO_PROCESS, thread);
    if (errno == 0)
        (void)::setpriority(PRIO_PROCESS, thread, std::min(current + 10, 19));
#endif
}

namespace {

// A pthread mutex with the inheritance protocol: both Darwin and Linux honour
// PTHREAD_PRIO_INHERIT, so one implementation serves every POSIX arm.
class POSIXPriorityInheritingLock final : public PriorityInheritingLock {
public:
    [[nodiscard]] bool initialize()
    {
        pthread_mutexattr_t attributes;
        if (::pthread_mutexattr_init(&attributes) != 0)
            return false;
        // A platform without the protocol still gets a working lock.
        (void)::pthread_mutexattr_setprotocol(&attributes, PTHREAD_PRIO_INHERIT);
        const bool isInitialized = ::pthread_mutex_init(&m_mutex, &attributes) == 0;
        ::pthread_mutexattr_destroy(&attributes);
        m_isInitialized = isInitialized;
        return isInitialized;
    }

    ~POSIXPriorityInheritingLock() override
    {
        if (m_isInitialized)
            ::pthread_mutex_destroy(&m_mutex);
    }

    void lock() override { ::pthread_mutex_lock(&m_mutex); }
    void unlock() override { ::pthread_mutex_unlock(&m_mutex); }

private:
    pthread_mutex_t m_mutex {};
    bool m_isInitialized = false;
};

} // namespace

std::unique_ptr<PriorityInheritingLock> makePriorityInheritingLock()
{
    std::unique_ptr<POSIXPriorityInheritingLock> lock(new (std::nothrow) POSIXPriorityInheritingLock);
    if (!lock || !lock->initialize())
        return nullptr;
    return lock;
}

int fileCanonicalPathForDescriptor(int descriptor, char* outputBuffer, int outputCapacity)
{
    if (descriptor < 0 || !outputBuffer || outputCapacity <= 0)
        return -1;

    std::array<char, PATH_MAX> resolved {};
#if defined(__APPLE__)
    // F_GETPATH fills PATH_MAX and NUL-terminates; it answers for an unlinked
    // file too, with the last path it had and nothing to say so.
    if (::fcntl(descriptor, F_GETPATH, resolved.data()) == -1)
        return -1;
    resolved[resolved.size() - 1] = '\0';
#else
    // The link under /proc/self/fd is the kernel's own answer for this
    // descriptor. readlink does not NUL-terminate, and it reports the length it
    // would have written, so a result filling the buffer is a truncation and
    // refused rather than terminated at the wrong place.
    std::array<char, 64> linkPath {};
    const int written = std::snprintf(linkPath.data(), linkPath.size(), "/proc/self/fd/%d", descriptor);
    if (written <= 0 || written >= static_cast<int>(linkPath.size()))
        return -1;
    const ssize_t length = ::readlink(linkPath.data(), resolved.data(), resolved.size());
    if (length <= 0 || length >= static_cast<ssize_t>(resolved.size()))
        return -1;
    resolved[static_cast<std::size_t>(length)] = '\0';
#endif

    // The path has to still name this descriptor, and the two legs arrive here
    // in different states. Darwin's F_GETPATH reports the old path bare, so it
    // resolves to whatever holds that name now and this comparison is what
    // refuses it. Linux appends " (deleted)" as soon as the opened name is
    // unlinked — per dentry, whether or not the inode keeps other links — so the
    // stat above has already failed and this never runs. Kept on both because
    // the function answers one question, and it is the question a caller asks
    // instead of trusting a name.
    struct ::stat descriptorStatus {};
    struct ::stat pathStatus {};
    if (::fstat(descriptor, &descriptorStatus) != 0)
        return -1;
    if (::stat(resolved.data(), &pathStatus) != 0)
        return -1;
    if (descriptorStatus.st_dev != pathStatus.st_dev || descriptorStatus.st_ino != pathStatus.st_ino)
        return -1;

    const std::size_t resolvedLength = std::strlen(resolved.data());
    // A path that does not fit is refused, not truncated: a truncated path names
    // a different file, and usually a directory.
    if (resolvedLength + 1 > static_cast<std::size_t>(outputCapacity))
        return -1;
    std::memcpy(outputBuffer, resolved.data(), resolvedLength + 1);
    return static_cast<int>(resolvedLength);
}

std::expected<BoundedFileBytes, FileReadError> storageReadBounded(const char* path, std::size_t maxBytes)
{
    if (!path || !*path || !O_NOFOLLOW)
        return std::unexpected(FileReadError::Unavailable);
    UnixFileDescriptor fileDescriptor(::open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
    struct stat status {};
    if (!fileDescriptor || ::fstat(fileDescriptor.get(), &status) || !S_ISREG(status.st_mode) || status.st_size < 0)
        return std::unexpected(FileReadError::Unavailable);
    const auto size = static_cast<std::uintmax_t>(status.st_size);
    if (size > maxBytes || size > static_cast<std::uintmax_t>(std::numeric_limits<ssize_t>::max()))
        return std::unexpected(FileReadError::TooLarge);
    BoundedFileBytes result { std::unique_ptr<std::uint8_t[]>(new (std::nothrow) std::uint8_t[size ? size : 1]), static_cast<std::size_t>(size) };
    if (!result.bytes)
        return std::unexpected(FileReadError::AllocationFailed);
    std::size_t received = 0;
    while (received < result.size) {
        const auto count = ::read(fileDescriptor.get(), result.bytes.get() + received, result.size - received);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return std::unexpected(FileReadError::Changed);
        received += static_cast<std::size_t>(count);
    }
    std::uint8_t extra = 0;
    ssize_t count;
    do {
        count = ::read(fileDescriptor.get(), &extra, 1);
    } while (count < 0 && errno == EINTR);
    if (count != 0)
        return std::unexpected(FileReadError::Changed);
    return result;
}

int storageReadFile(const char* path, void* outputBuffer, int outputCapacity)
{
    if (!path || path[0] == '\0')
        return -1;
    UnixFileDescriptor fileDescriptor(::open(path, O_RDONLY));
    if (!fileDescriptor)
        return -1;

    // No buffer (or zero capacity): probe the total readable size.
    if (!outputBuffer || outputCapacity <= 0) {
        int total = 0;
        char scratch[4096];
        for (;;) {
            const ssize_t n = ::read(fileDescriptor.get(), scratch, sizeof(scratch));
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                return -1;
            }
            if (n == 0)
                break;
            total += static_cast<int>(n);
        }
        return total;
    }

    int written = 0;
    auto* out = static_cast<char*>(outputBuffer);
    while (written < outputCapacity) {
        const ssize_t n = ::read(fileDescriptor.get(), out + written, static_cast<std::size_t>(outputCapacity - written));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            break;
        written += static_cast<std::size_t>(n);
    }
    return written;
}

bool storageEnsureDirectory(const char* path)
{
    if (!path || path[0] == '\0')
        return false;
    // Sandboxed apps may read existing ancestors without permission to
    // attempt mkdir there. Only create a component that is actually absent.
    const auto ensureComponent = [](const char* component) {
        struct stat status {};
        if (::stat(component, &status) == 0)
            return S_ISDIR(status.st_mode);
        if (errno != ENOENT)
            return false;
        // Preferences directories can hold sensitive files.
        if (::mkdir(component, 0700) == 0)
            return true;
        return errno == EEXIST && ::stat(component, &status) == 0 && S_ISDIR(status.st_mode);
    };
    std::string accumulated;
    const std::string target(path);
    for (char ch : target) {
        accumulated.push_back(ch);
        if (ch == '/' && accumulated.size() > 1) {
            if (!ensureComponent(accumulated.c_str()))
                return false;
        }
    }
    return ensureComponent(target.c_str());
}

// Non-predictable temp name: mix clock_gettime nanoseconds and PID. Not
// cryptographic, but sufficient against predictable-name preimage attacks.
static std::string makeTempName(const std::string& prefix, const std::string& suffix)
{
    char hex[17];
    struct timespec ts;
    ::clock_gettime(CLOCK_REALTIME, &ts);
    const unsigned long entropy = (static_cast<unsigned long>(ts.tv_nsec) ^ static_cast<unsigned long>(::getpid()));
    std::snprintf(hex, sizeof(hex), "%016lx", entropy);
    return prefix + hex + suffix;
}

bool storageWriteAtomic(const char* path, std::span<const std::byte> buffer)
{
    if (!path || path[0] == '\0')
        return false;

    // Create the parent directory chain so the first write on a fresh install
    // lands.
    const std::string target(path);
    const std::size_t slash = target.rfind('/');
    if (slash != std::string::npos && slash > 0) {
        const std::string parent = target.substr(0, slash);
        if (!storageEnsureDirectory(parent.c_str()))
            return false;
    }

    // Non-predictable temp name alongside the target so a pre-placed symlink at a
    // fixed name can't redirect the write. O_EXCL refuses an existing path,
    // O_NOFOLLOW refuses a symlink, O_CLOEXEC won't leak the fd, mode 0600 keeps
    // the bytes owner-only from creation.
#if UBIQUITTY_ENABLE_TEST_HOOKS
    // Test seam: a known suffix lets tests plant a symlink at the exact temp path
    // and verify it is refused. Production never sets this env var.
    const char* testSuffix = ::getenv("UBIQUITTY_TEST_ATOMIC_SUFFIX");
    const std::string tmp = testSuffix ? target + testSuffix : makeTempName(target + ".", ".tmp");
#else
    const std::string tmp = makeTempName(target + ".", ".tmp");
#endif

    UnixFileDescriptor fileDescriptor(::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (!fileDescriptor)
        return false;
    const auto* bytes = reinterpret_cast<const char*>(buffer.data());
    std::size_t written = 0;
    while (written < buffer.size()) {
        const ssize_t n = ::write(fileDescriptor.get(), bytes + written, buffer.size() - written);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            ::unlink(tmp.c_str());
            return false;
        }
        written += static_cast<std::size_t>(n);
    }
    // fchmod before close/rename so the atomically-committed file is 0600
    // regardless of umask.
    if (::fchmod(fileDescriptor.get(), 0600) != 0) {
        ::unlink(tmp.c_str());
        return false;
    }
    // The rename is atomic against a crash; the data reaching the disk is not.
    // A journalling filesystem can commit the directory entry before the pages
    // behind it, which leaves a zero-length file where the settings were —
    // exactly the torn state the temp-and-rename exists to prevent. These are
    // one-shot config commits, so the flush costs nothing worth counting.
    if (::fsync(fileDescriptor.get()) != 0 && errno != EINVAL) {
        // EINVAL is a filesystem that does not implement fsync for this file
        // (some virtual filesystems), not a failure to persist.
        ::unlink(tmp.c_str());
        return false;
    }
    if (fileDescriptor.close() != 0) {
        ::unlink(tmp.c_str());
        return false;
    }
    if (::rename(tmp.c_str(), target.c_str()) != 0) {
        ::unlink(tmp.c_str());
        return false;
    }
    // The directory entry itself needs the same treatment, or the rename can
    // be the part that is lost. A failure here means the new contents are on
    // disk under a name that may not have arrived — the write succeeded, so it
    // is not reported as a failure that would make a caller rewrite it.
    if (slash != std::string::npos && slash > 0) {
        const std::string parent = target.substr(0, slash);
        UnixFileDescriptor directory(::open(parent.c_str(), O_RDONLY | O_CLOEXEC));
        if (directory)
            (void)::fsync(directory.get());
    }
    return true;
}

namespace {

bool flushDurably(int descriptor)
{
    int result;
    do {
#if defined(__APPLE__)
        result = ::fcntl(descriptor, F_FULLFSYNC);
#else
        result = ::fsync(descriptor);
#endif
    } while (result < 0 && errno == EINTR);
    return result == 0;
}

class DurableFileOperations {
public:
    DurableFileOperations(UnixFileDescriptor directory, const char* destination)
        : m_directory(std::move(directory))
        , m_destination(destination)
    {
    }

    ~DurableFileOperations()
    {
        if (m_hasTemporaryFile)
            (void)::unlinkat(m_directory.get(), m_temporaryName.data(), 0);
    }

    bool create()
    {
        timespec now {};
        if (::clock_gettime(CLOCK_MONOTONIC, &now))
            return false;
        for (unsigned attempt = 0; attempt < 8; ++attempt) {
#if defined(__APPLE__) && TARGET_OS_IPHONE
            std::array<unsigned long long, 2> nonce {};
            ::arc4random_buf(nonce.data(), sizeof(nonce));
            const int count = std::snprintf(m_temporaryName.data(), m_temporaryName.size(), ".ubq-sealed-%016llx%016llx", nonce[0], nonce[1]);
#else
            // Names need not carry secrets: exclusive creation in the pinned
            // private directory is the ownership boundary, including collisions.
            const int count = std::snprintf(m_temporaryName.data(), m_temporaryName.size(), ".ubq-sealed-%lx-%lx-%x", static_cast<unsigned long>(::getpid()), static_cast<unsigned long>(now.tv_nsec), attempt);
#endif
            if (count <= 0 || static_cast<std::size_t>(count) >= m_temporaryName.size())
                return false;
#if defined(__APPLE__) && TARGET_OS_IPHONE
            m_file = Platform::createCompleteProtectedFile(m_directory.get(), m_temporaryName.data());
#else
            m_file.reset(::openat(m_directory.get(), m_temporaryName.data(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
#endif
            if (m_file) {
                m_hasTemporaryFile = true;
                return ::fchmod(m_file.get(), 0600) == 0;
            }
            if (errno != EEXIST)
                return false;
        }
        return false;
    }

    std::size_t write(std::span<const std::byte> bytes)
    {
        ssize_t count;
        do {
            count = ::write(m_file.get(), bytes.data(), bytes.size());
        } while (count < 0 && errno == EINTR);
        return count > 0 ? static_cast<std::size_t>(count) : 0;
    }

    bool flushFile() { return flushDurably(m_file.get()); }
    bool closeFile() { return m_file.close() == 0; }

    bool replace()
    {
        if (::renameat(m_directory.get(), m_temporaryName.data(), m_directory.get(), m_destination))
            return false;
        m_hasTemporaryFile = false;
        return true;
    }

    bool flushDirectory() { return flushDurably(m_directory.get()); }

private:
    UnixFileDescriptor m_directory;
    UnixFileDescriptor m_file;
    const char* m_destination;
    std::array<char, 80> m_temporaryName {};
    bool m_hasTemporaryFile { false };
};

} // namespace

bool storageAppendOwnerOnly(const char* path, std::span<const std::byte> bytes)
{
    if (!path || !path[0])
        return false;
    // Created exclusively, so the explicit mode lands only on a file this call
    // made; the open's own mode is still narrowed by the umask, hence fchmod.
    UnixFileDescriptor file(::open(path, O_WRONLY | O_APPEND | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (file) {
        if (::fchmod(file.get(), 0600) != 0)
            return false;
    } else if (errno == EEXIST) {
        file.reset(::open(path, O_WRONLY | O_APPEND | O_NOFOLLOW | O_CLOEXEC));
    }
    if (!file)
        return false;
    while (!bytes.empty()) {
        const auto written = ::write(file.get(), bytes.data(), bytes.size());
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return false;
        bytes = bytes.subspan(static_cast<std::size_t>(written));
    }
    return true;
}

DurableFileResult storageWriteDurable(const char* path, std::span<const std::byte> buffer)
{
    if (!path || path[0] != '/' || !O_NOFOLLOW || buffer.size() > static_cast<std::size_t>(SSIZE_MAX))
        return DurableFileResult::NotCommitted;
    const auto length = ::strnlen(path, PATH_MAX);
    if (length == 0 || length >= PATH_MAX)
        return DurableFileResult::NotCommitted;
    std::array<char, PATH_MAX> parent {};
    std::memcpy(parent.data(), path, length + 1);
    char* separator = std::strrchr(parent.data(), '/');
    const char* destination = path + (separator - parent.data()) + 1;
    if (!*destination || std::strcmp(destination, ".") == 0 || std::strcmp(destination, "..") == 0)
        return DurableFileResult::NotCommitted;
    if (separator == parent.data())
        separator[1] = '\0';
    else
        *separator = '\0';
    UnixFileDescriptor directory(::open(parent.data(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    struct stat status {};
    if (!directory || ::fstat(directory.get(), &status) || status.st_uid != ::geteuid() || (status.st_mode & 0022))
        return DurableFileResult::NotCommitted;
    DurableFileOperations operations(std::move(directory), destination);
    if (!operations.create())
        return DurableFileResult::NotCommitted;
    return Platform::commitDurableFile(operations, buffer);
}

void storageRemoveFile(const char* path)
{
    if (path && path[0] != '\0')
        (void)::unlink(path);
}

bool systemExecutableExists(const char* path) { return path && path[0] != '\0' && ::access(path, X_OK) == 0; }

std::string downloadsDirectoryDefaultPath()
{
    const char* home = ::getenv("HOME");
    if (!home || home[0] == '\0')
        return {};
    std::string path = home;
    path += "/Downloads";
    return path;
}

bool storageWriteTempFile(const char* directoryHint, const char* suffix, std::span<const std::byte> buffer, std::span<char> pathOut)
{
    if (pathOut.empty())
        return false;

    std::string directory = (directoryHint && directoryHint[0] != '\0') ? std::string(directoryHint) : std::string("/tmp");
    if (directory.back() != '/')
        directory.push_back('/');

    const std::string wantSuffix = (suffix && suffix[0] != '\0') ? std::string(suffix) : std::string();
    const std::string namePrefix = directory + "ubiquitty.";

#if UBIQUITTY_ENABLE_TEST_HOOKS
    // Test seam: a known suffix lets tests plant a symlink at the exact temp path
    // and verify it is refused. Production never sets this env var.
    const char* testSuffix = ::getenv("UBIQUITTY_TEST_TEMP_SUFFIX");
    const std::string tmp = testSuffix ? namePrefix + testSuffix + wantSuffix : makeTempName(namePrefix, wantSuffix);
#else
    const std::string tmp = makeTempName(namePrefix, wantSuffix);
#endif

    if (tmp.size() + 1 > pathOut.size())
        return false;

    // O_EXCL (no clobber), O_NOFOLLOW (refuse a symlink), O_CLOEXEC (no leak into
    // the tic child), mode 0600 (owner-only).
    UnixFileDescriptor fileDescriptor(::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (!fileDescriptor)
        return false;

    const auto* bytes = reinterpret_cast<const char*>(buffer.data());
    std::size_t written = 0;
    while (written < buffer.size()) {
        const ssize_t n = ::write(fileDescriptor.get(), bytes + written, buffer.size() - written);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            ::unlink(tmp.c_str());
            return false;
        }
        written += static_cast<std::size_t>(n);
    }
    if (fileDescriptor.close() != 0) {
        ::unlink(tmp.c_str());
        return false;
    }

    std::memcpy(pathOut.data(), tmp.c_str(), tmp.size() + 1);
    return true;
}

} // namespace Ubiquitty::PAL
