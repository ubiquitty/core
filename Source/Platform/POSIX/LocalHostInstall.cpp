// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Platform/POSIX/LocalHostInstall.h"

#include "Core/Protocol/Sync/AppVersion.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/Messages.h"
#include "Core/Protocol/Sync/WireVersion.h"
#include "Core/Session/ControlPaths.h"
#include "Core/Session/SessionShare.h"
#include "PAL/UnixFileDescriptor.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <format>
#include <optional>
#include <poll.h>
#include <span>
#include <spawn.h>
#include <string_view>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <libproc.h>
#include <sys/proc.h>
#include <sys/ucred.h>
#endif

extern char** environ;

namespace Ubiquitty::Session {

namespace {

using Clock = std::chrono::steady_clock;

/// How long an installed host has to say its version.
constexpr auto versionBudget = std::chrono::seconds(5);
/// How long a host has to answer a greeting through the relay.
constexpr auto answerBudget = std::chrono::seconds(3);
/// How long a relay has to end once its input closes.
constexpr auto relayEndBudget = std::chrono::seconds(1);
/// How long a bundled host asked to stop has to exit.
constexpr auto retireBudget = std::chrono::seconds(2);

std::string errorWords(int error)
{
    return std::strerror(error);
}

/// Reads up to `buffer`'s size at `offset`, retrying an interrupted read.
///
/// - Returns: The bytes read, 0 at the end, or -1 on an error.
ssize_t readAt(int file, std::span<char> buffer, off_t offset)
{
    for (;;) {
        const ssize_t count = ::pread(file, buffer.data(), buffer.size(), offset);
        if (count >= 0 || errno != EINTR)
            return count;
    }
}

bool writeAll(int file, std::span<const char> bytes)
{
    while (!bytes.empty()) {
        const ssize_t count = ::write(file, bytes.data(), bytes.size());
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return false;
        bytes = bytes.subspan(static_cast<std::size_t>(count));
    }
    return true;
}

/// Writes all of `bytes` to a socket, without a signal when its peer has gone.
bool sendAll(int channel, std::span<const std::uint8_t> bytes)
{
#if defined(MSG_NOSIGNAL)
    constexpr int flags = MSG_NOSIGNAL;
#else
    constexpr int flags = 0;
#endif
    while (!bytes.empty()) {
        const ssize_t count = ::send(channel, bytes.data(), bytes.size(), flags);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return false;
        bytes = bytes.subspan(static_cast<std::size_t>(count));
    }
    return true;
}

/// Starts `path` with standard input on `input` (/dev/null when it is -1),
/// standard output on `output`, standard error on /dev/null, no signal
/// blocked, and, where the platform can say so, no other descriptor of this
/// process.
///
/// - Returns: The child, or nothing when it could not be started.
std::optional<pid_t> spawnWith(const std::string& path, std::span<char* const> arguments, int input, int output)
{
    posix_spawn_file_actions_t actions;
    if (::posix_spawn_file_actions_init(&actions) != 0)
        return std::nullopt;
    posix_spawnattr_t attributes;
    if (::posix_spawnattr_init(&attributes) != 0) {
        ::posix_spawn_file_actions_destroy(&actions);
        return std::nullopt;
    }
    if (input < 0)
        (void)::posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    else
        (void)::posix_spawn_file_actions_adddup2(&actions, input, STDIN_FILENO);
    (void)::posix_spawn_file_actions_adddup2(&actions, output, STDOUT_FILENO);
    (void)::posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    sigset_t none;
    sigemptyset(&none);
    (void)::posix_spawnattr_setsigmask(&attributes, &none);
#if defined(__APPLE__)
    constexpr int flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_CLOEXEC_DEFAULT;
#else
    constexpr int flags = POSIX_SPAWN_SETSIGMASK;
#endif
    (void)::posix_spawnattr_setflags(&attributes, static_cast<short>(flags));
    pid_t child = -1;
    const int spawned = ::posix_spawn(&child, path.c_str(), &actions, &attributes, arguments.data(), environ);
    ::posix_spawnattr_destroy(&attributes);
    ::posix_spawn_file_actions_destroy(&actions);
    if (spawned != 0 || child <= 0)
        return std::nullopt;
    return child;
}

/// Reaps a child of this process by `deadline`, ending it if it runs past.
///
/// - Returns: Its status, or nothing when it had to be ended or another
///   waiter reaped it.
std::optional<int> reapBy(pid_t child, Clock::time_point deadline)
{
    int status = 0;
    for (;;) {
        const pid_t reaped = ::waitpid(child, &status, WNOHANG);
        if (reaped == child)
            return status;
        if (reaped < 0 && errno != EINTR)
            return std::nullopt;
        if (Clock::now() >= deadline)
            break;
        ::usleep(5'000);
    }
    ::kill(child, SIGKILL);
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    return std::nullopt;
}

/// Whether the file at `path` holds exactly the bytes `source` holds.
///
/// - Parameters:
///   - source: The open bundled host, read by offset.
///   - length: Its size.
///   - path: The installed host.
bool hasSameBytes(int source, off_t length, const std::string& path)
{
    PAL::UnixFileDescriptor installed(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    struct stat installedInfo {};
    if (!installed || ::fstat(installed.get(), &installedInfo) != 0 || !S_ISREG(installedInfo.st_mode) || installedInfo.st_size != length)
        return false;
    std::array<char, 65536> left {};
    std::array<char, 65536> right {};
    for (off_t offset = 0; offset < length;) {
        const ssize_t count = readAt(source, left, offset);
        if (count <= 0 || readAt(installed.get(), std::span(right).first(static_cast<std::size_t>(count)), offset) != count
            || std::memcmp(left.data(), right.data(), static_cast<std::size_t>(count)) != 0)
            return false;
        offset += count;
    }
    return true;
}

/// A name beside the install that no other install, in this process or
/// another, stages under.
std::string stagePath(const std::string& directory, std::string_view purpose)
{
    std::array<std::uint8_t, 8> entropy {};
    if (::getentropy(entropy.data(), entropy.size()) != 0)
        entropy.fill(0);
    std::string suffix;
    for (const std::uint8_t byte : entropy)
        suffix += std::format("{:02x}", byte);
    return std::format("{}/.ubiquitty-host.{}-{}-{}", directory, purpose, ::getpid(), suffix);
}

/// Writes an owner-only copy of `source` at `staged`, a path nothing holds.
///
/// - Returns: Nothing on success, else why.
std::string copyTo(int source, off_t length, const std::string& staged)
{
    PAL::UnixFileDescriptor target(::open(staged.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0700));
    if (!target)
        return std::format("{} could not be created: {}", staged, errorWords(errno));
    std::array<char, 65536> chunk {};
    for (off_t offset = 0; offset < length;) {
        const ssize_t count = readAt(source, chunk, offset);
        if (count <= 0)
            return "the bundled host could not be read";
        if (!writeAll(target.get(), std::span(chunk).first(static_cast<std::size_t>(count))))
            return std::format("{} could not be written: {}", staged, errorWords(errno));
        offset += count;
    }
    if (::fchmod(target.get(), 0700) != 0 || ::fsync(target.get()) != 0)
        return std::format("{} could not be finished: {}", staged, errorWords(errno));
    return {};
}

/// What an installed host said when asked its version.
struct VersionReply {
    /// The first word `--version` printed, when it exited 0 having printed
    /// a version; empty otherwise.
    std::string version;
    /// Whether it ran past the budget, so whether it is older is unknown.
    bool isSlow = false; // bool-ok: names the one fact it reports
};

/// Asks an installed host its version.
///
/// The host is run with no input and its output bounded, and is ended when
/// it does not answer in time.
///
/// - Parameter path: The installed host.
/// - Returns: What it said.
VersionReply statedVersion(const std::string& path)
{
    std::array<int, 2> ends { -1, -1 };
    if (::pipe(ends.data()) != 0)
        return {};
    PAL::UnixFileDescriptor reader(ends[0]);
    PAL::UnixFileDescriptor writer(ends[1]);
    (void)::fcntl(reader.get(), F_SETFD, FD_CLOEXEC);
    (void)::fcntl(writer.get(), F_SETFD, FD_CLOEXEC);
    std::array<char*, 3> arguments { const_cast<char*>(path.c_str()), const_cast<char*>("--version"), nullptr };
    const auto child = spawnWith(path, arguments, -1, writer.get());
    writer.reset();
    if (!child)
        return {};
    constexpr std::size_t outputLimit = 4096;
    std::string output;
    const auto deadline = Clock::now() + versionBudget;
    std::array<char, 512> chunk {};
    while (output.size() < outputLimit) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        pollfd item { reader.get(), POLLIN, 0 };
        if (remaining <= 0 || ::poll(&item, 1, static_cast<int>(remaining)) <= 0)
            break;
        const ssize_t count = ::read(reader.get(), chunk.data(), chunk.size());
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            break;
        output.append(chunk.data(), static_cast<std::size_t>(count));
    }
    // A host that printed past the limit says no version; one still running
    // at the deadline might yet.
    const bool isOverLimit = output.size() >= outputLimit;
    const auto status = reapBy(*child, isOverLimit ? Clock::now() : deadline);
    if (!status)
        return { .version = {}, .isSlow = !isOverLimit };
    std::string version = output.substr(0, output.find_first_of(" \t\r\n"));
    if (!WIFEXITED(*status) || WEXITSTATUS(*status) != 0 || !Protocol::Sync::parseAppVersion(version))
        return {};
    return { .version = std::move(version), .isSlow = false };
}

/// The bundled app's executable, when the host is an app bundle's helper.
std::string bundledInterfaceOf(const std::string& bundledHostPath)
{
    constexpr std::string_view helperTail = "/Contents/Helpers/ubiquitty-host";
    if (!std::string_view(bundledHostPath).ends_with(helperTail))
        return {};
    return bundledHostPath.substr(0, bundledHostPath.size() - helperTail.size()) + "/Contents/MacOS/Ubiquitty";
}

/// Names the app beside the install, or removes the name: a host started
/// from the install trusts that app only while the app's helper is the
/// install's own file or holds the same code.
void nameInterface(const std::string& installPath, const std::string& bundledHostPath, const std::string& directory)
{
    const std::string link = installPath + ".interface";
    const std::string interface = bundledInterfaceOf(bundledHostPath);
    if (interface.empty()) {
        (void)::unlink(link.c_str());
        return;
    }
    std::array<char, 4096> current {};
    const ssize_t length = ::readlink(link.c_str(), current.data(), current.size() - 1);
    if (length > 0 && std::string_view(current.data(), static_cast<std::size_t>(length)) == interface)
        return;
    const std::string staged = stagePath(directory, "interface");
    if (::symlink(interface.c_str(), staged.c_str()) != 0)
        return;
    if (::rename(staged.c_str(), link.c_str()) != 0)
        (void)::unlink(staged.c_str());
}

/// Whether a socket leaf names a host's own socket: `host-<16 hex>.sock`.
bool isHostSocketLeaf(std::string_view name)
{
    constexpr std::string_view prefix = "host-";
    constexpr std::string_view suffix = ".sock";
    constexpr std::size_t digits = 16;
    if (name.size() != prefix.size() + digits + suffix.size() || !name.starts_with(prefix) || !name.ends_with(suffix))
        return false;
    for (const char digit : name.substr(prefix.size(), digits)) {
        if (!((digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f')))
            return false;
    }
    return true;
}

/// The process the kernel names as serving a local socket; nothing when
/// nothing answers there or it is another user's.
///
/// A connection proves a host listens there. The process it names is the
/// one that made or last accepted on the listener, which a successor that
/// inherited the listener with a holder's sessions need not be yet.
std::optional<pid_t> servingProcess(const std::string& path)
{
    sockaddr_un address {};
    if (path.size() >= sizeof(address.sun_path))
        return std::nullopt;
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    PAL::UnixFileDescriptor channel(::socket(AF_UNIX, SOCK_STREAM, 0));
    if (!channel || ::fcntl(channel.get(), F_SETFD, FD_CLOEXEC) != 0
        || ::connect(channel.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
        return std::nullopt;
#if defined(__APPLE__)
    uid_t user = 0;
    gid_t group = 0;
    pid_t process = 0;
    socklen_t length = sizeof(process);
    if (::getpeereid(channel.get(), &user, &group) != 0 || user != ::geteuid()
        || ::getsockopt(channel.get(), SOL_LOCAL, LOCAL_PEERPID, &process, &length) != 0 || process <= 0)
        return std::nullopt;
    return process;
#elif defined(__linux__)
    struct ucred credentials {};
    socklen_t length = sizeof(credentials);
    if (::getsockopt(channel.get(), SOL_SOCKET, SO_PEERCRED, &credentials, &length) != 0 || credentials.uid != ::geteuid() || credentials.pid <= 0)
        return std::nullopt;
    return credentials.pid;
#else
    return std::nullopt;
#endif
}

/// The canonical greeting for an unbound control connection, framed.
std::vector<std::uint8_t> controlGreeting()
{
    namespace Sync = Protocol::Sync;
    std::array<std::uint64_t, 2> instance {};
    if (::getentropy(instance.data(), sizeof(instance)) != 0)
        instance = { static_cast<std::uint64_t>(::getpid()), static_cast<std::uint64_t>(Clock::now().time_since_epoch().count()) };
    Sync::Greet greeting;
    greeting.wireVersion = Sync::wireVersion;
    greeting.appVersion = std::string(Sync::appVersion());
    greeting.clientInstance = { .low = instance[0], .high = instance[1] };
    greeting.clientKind = Sync::ClientKind::Human;
    greeting.requestedRole = Sync::Role::Owner;
    greeting.request.target = Sync::AdmissionTarget::Control;
    greeting.facts.viewportRows = 24;
    greeting.facts.viewportColumns = 80;
    std::vector<std::uint8_t> body;
    Sync::encode(body, greeting);
    std::vector<std::uint8_t> frame;
    if (body.empty() || !Sync::appendFrame(frame, Sync::FrameKind::Greet, 0, body))
        return {};
    return frame;
}

/// What a host said when greeted.
struct HostAnswer {
    /// The app version it stated, empty when it stated none this build reads.
    std::string appVersion;
    /// Its instance, when it admitted the greeting; zero when it refused it.
    std::uint64_t instance = 0;
};

/// Greets the host serving `leaf` in the control directory through a relay
/// of the bundled host (`--attach`), asking for an unbound control
/// connection, and reads its answer.
///
/// The host's own answer, not the kernel's account of the socket. A host
/// that trusts the bundled host's file welcomes the relay and names its
/// instance, so a socket a successor inherited is known as the successor's;
/// any other host states its build as it refuses.
///
/// - Returns: What it said, empty when it said nothing this build reads.
HostAnswer askHost(const std::string& bundledHostPath, const std::string& leaf)
{
    namespace Sync = Protocol::Sync;
    HostAnswer answer;
    const auto greeting = controlGreeting();
    std::array<int, 2> ends { -1, -1 };
    if (greeting.empty() || ::socketpair(AF_UNIX, SOCK_STREAM, 0, ends.data()) != 0)
        return answer;
    PAL::UnixFileDescriptor channel(ends[0]);
    PAL::UnixFileDescriptor relayEnd(ends[1]);
    (void)::fcntl(channel.get(), F_SETFD, FD_CLOEXEC);
    (void)::fcntl(relayEnd.get(), F_SETFD, FD_CLOEXEC);
#if defined(SO_NOSIGPIPE)
    const int isQuiet = 1;
    (void)::setsockopt(channel.get(), SOL_SOCKET, SO_NOSIGPIPE, &isQuiet, sizeof(isQuiet));
#endif
    std::array<char*, 5> arguments { const_cast<char*>(bundledHostPath.c_str()), const_cast<char*>("--attach"), const_cast<char*>("--socket"),
        const_cast<char*>(leaf.c_str()), nullptr };
    const auto relay = spawnWith(bundledHostPath, arguments, relayEnd.get(), relayEnd.get());
    relayEnd.reset();
    if (!relay)
        return answer;
    if (sendAll(channel.get(), greeting)) {
        Sync::FrameDecoder decoder;
        std::array<std::uint8_t, 4096> chunk {};
        std::size_t received = 0;
        const auto deadline = Clock::now() + answerBudget;
        bool isAnswered = false; // bool-ok: ends the read once the host has answered
        while (!isAnswered && received <= 2 * Sync::maxFrameBodyBytes) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
            pollfd item { channel.get(), POLLIN, 0 };
            if (remaining <= 0 || ::poll(&item, 1, static_cast<int>(remaining)) <= 0)
                break;
            const ssize_t count = ::read(channel.get(), chunk.data(), chunk.size());
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                break;
            received += static_cast<std::size_t>(count);
            decoder.append(std::span(chunk).first(static_cast<std::size_t>(count)));
            while (const auto frame = decoder.next()) {
                if (frame->kind == Sync::FrameKind::Welcome) {
                    if (const auto welcome = Sync::decodeWelcome(frame->body)) {
                        answer.appVersion = welcome->appVersion;
                        answer.instance = welcome->selection.daemonInstanceID;
                    }
                    isAnswered = true;
                    break;
                }
                if (frame->kind == Sync::FrameKind::Bye) {
                    if (const auto bye = Sync::decodeBye(frame->body); bye && bye->peer)
                        answer.appVersion = bye->peer->build.appVersion;
                    isAnswered = true;
                    break;
                }
            }
            if (decoder.error() != Sync::EnvelopeError::None)
                break;
        }
    }
    // The relay ends with its input.
    channel.reset();
    (void)reapBy(*relay, Clock::now() + relayEndBudget);
    return answer;
}

/// The path a process runs from, as the platform names it; on Linux, a
/// replaced file's name without its deleted marker.
std::string executablePathOf(pid_t process)
{
#if defined(__APPLE__)
    std::array<char, PROC_PIDPATHINFO_MAXSIZE> image {};
    if (::proc_pidpath(process, image.data(), image.size()) <= 0)
        return {};
    return image.data();
#elif defined(__linux__)
    std::array<char, 4096> image {};
    const ssize_t length = ::readlink(std::format("/proc/{}/exe", process).c_str(), image.data(), image.size() - 1);
    if (length <= 0)
        return {};
    std::string path(image.data(), static_cast<std::size_t>(length));
    constexpr std::string_view deletedMarker = " (deleted)";
    if (std::string_view(path).ends_with(deletedMarker))
        path.resize(path.size() - deletedMarker.size());
    return path;
#else
    (void)process;
    return {};
#endif
}

/// Whether a process runs the very file at `path`.
bool runsFile(pid_t process, const std::string& path)
{
    struct stat expected {}, running {};
    if (::stat(path.c_str(), &expected) != 0)
        return false;
#if defined(__APPLE__)
    std::array<char, PROC_PIDPATHINFO_MAXSIZE> image {};
    if (::proc_pidpath(process, image.data(), image.size()) <= 0 || ::stat(image.data(), &running) != 0)
        return false;
#elif defined(__linux__)
    if (::stat(std::format("/proc/{}/exe", process).c_str(), &running) != 0)
        return false;
#else
    return false;
#endif
    return running.st_dev == expected.st_dev && running.st_ino == expected.st_ino;
}

/// Whether the file at the path a process runs from is now `bundled`: a
/// holder admits as its successor the file found at the path it started
/// from.
bool startedFromBundledFile(pid_t process, const struct stat& bundled)
{
    const std::string path = executablePathOf(process);
    struct stat there {};
    return !path.empty() && ::stat(path.c_str(), &there) == 0 && there.st_dev == bundled.st_dev && there.st_ino == bundled.st_ino;
}

/// Whether a path names a pseudo-terminal's controlling side, which only a
/// process that runs a session holds.
bool isPseudoTerminalController(std::string_view path)
{
    return path == "/dev/ptmx" || path == "/dev/pts/ptmx";
}

/// Whether a process holds a pseudo-terminal's controlling side: a host
/// that runs a session holds one for it, including a session a predecessor
/// handed it, whose program is no child of its own. A list that cannot be
/// read says none.
bool holdsPseudoTerminal(pid_t process)
{
#if defined(__APPLE__)
    const int bytes = ::proc_pidinfo(process, PROC_PIDLISTFDS, 0, nullptr, 0);
    if (bytes <= 0)
        return false;
    // Room for descriptors opened between the two calls.
    std::vector<proc_fdinfo> descriptors(static_cast<std::size_t>(bytes) / sizeof(proc_fdinfo) + 16);
    const int listed = ::proc_pidinfo(process, PROC_PIDLISTFDS, 0, descriptors.data(), static_cast<int>(descriptors.size() * sizeof(proc_fdinfo)));
    if (listed <= 0)
        return false;
    descriptors.resize(std::min(descriptors.size(), static_cast<std::size_t>(listed) / sizeof(proc_fdinfo)));
    return std::ranges::any_of(descriptors, [process](const proc_fdinfo& descriptor) {
        vnode_fdinfowithpath vnode {};
        return descriptor.proc_fdtype == PROX_FDTYPE_VNODE
            && ::proc_pidfdinfo(process, descriptor.proc_fd, PROC_PIDFDVNODEPATHINFO, &vnode, sizeof(vnode)) == static_cast<int>(sizeof(vnode))
            && isPseudoTerminalController(vnode.pvip.vip_path);
    });
#elif defined(__linux__)
    const std::string directory = std::format("/proc/{}/fd", process);
    DIR* listing = ::opendir(directory.c_str());
    if (!listing)
        return false;
    bool isHeld = false; // bool-ok: ends the walk at the first one held
    while (!isHeld) {
        const dirent* entry = ::readdir(listing);
        if (!entry)
            break;
        std::array<char, 64> target {};
        const ssize_t length = ::readlinkat(::dirfd(listing), entry->d_name, target.data(), target.size() - 1);
        isHeld = length > 0 && isPseudoTerminalController(std::string_view(target.data(), static_cast<std::size_t>(length)));
    }
    ::closedir(listing);
    return isHeld;
#else
    (void)process;
    return false;
#endif
}

/// Whether a process has exited: gone, or a zombie, which holds no
/// descriptor and so no lock.
bool hasExited(pid_t process)
{
    if (::kill(process, 0) != 0)
        return errno == ESRCH;
#if defined(__APPLE__)
    proc_bsdshortinfo info {};
    return ::proc_pidinfo(process, PROC_PIDT_SHORTBSDINFO, 0, &info, sizeof(info)) == sizeof(info) && info.pbsi_status == SZOMB;
#elif defined(__linux__)
    PAL::UnixFileDescriptor status(::open(std::format("/proc/{}/stat", process).c_str(), O_RDONLY | O_CLOEXEC));
    std::array<char, 512> text {};
    const ssize_t count = status ? ::read(status.get(), text.data(), text.size() - 1) : -1;
    if (count <= 0)
        return false;
    const std::string_view line(text.data(), static_cast<std::size_t>(count));
    const auto close = line.rfind(')');
    return close != std::string_view::npos && close + 2 < line.size() && line[close + 2] == 'Z';
#else
    return false;
#endif
}

/// A host of another build serving in the control directory.
struct OtherHost {
    HostAnswer answer;
    /// The process the kernel names for its socket.
    std::optional<pid_t> process;
    /// Whether it is from a newer build than the bundled host.
    bool isNewer = false; // bool-ok: names the one fact it reports
};

/// The hosts of other builds serving in the control directory, each once.
///
/// A socket the bundled host serves, as it does a predecessor's it
/// inherited, is the bundled host's: by the instance it names, or, when it
/// names none, by the kernel's account.
///
/// - Parameters:
///   - bundledHostPath: The relay each is greeted through.
///   - root: The control directory.
///   - ownLeaf: The bundled host's own socket.
///   - bundledProcess: The process serving it, if one does.
///   - bundledInstance: Its instance, zero when unknown.
///   - bundledVersion: The successor's version, including when it comes from an update.
std::vector<OtherHost> otherHosts(const std::string& bundledHostPath, const std::string& root, std::string_view ownLeaf, std::optional<pid_t> bundledProcess,
    std::uint64_t bundledInstance, std::string_view bundledVersion)
{
    std::vector<std::string> leaves;
    if (DIR* listing = ::opendir(root.c_str())) {
        while (const dirent* entry = ::readdir(listing)) {
            const std::string_view name = entry->d_name;
            if (name != ownLeaf && isHostSocketLeaf(name))
                leaves.emplace_back(name);
        }
        ::closedir(listing);
    }
    std::vector<OtherHost> hosts;
    for (const std::string& leaf : leaves) {
        const auto process = servingProcess(root + "/" + leaf);
        if (!process)
            continue;
        HostAnswer answer = askHost(bundledHostPath, leaf);
        if (answer.instance ? answer.instance == bundledInstance : process == bundledProcess)
            continue;
        const bool isSeen = std::ranges::any_of(hosts, [&](const OtherHost& host) {
            return answer.instance ? host.answer.instance == answer.instance : !host.answer.instance && host.process == process;
        });
        if (isSeen)
            continue;
        const auto order = Protocol::Sync::compareAppVersions(answer.appVersion, bundledVersion);
        hosts.push_back({ .answer = std::move(answer), .process = process, .isNewer = order == std::strong_ordering::greater });
    }
    return hosts;
}

/// Asks the bundled host, when it holds nothing, to stop, and waits until
/// it has exited, when the start lock beside its socket is free again.
///
/// It is signalled only while the process serving its socket runs the
/// bundled file itself, which handles the request, and holds no session's
/// terminal; one that holds a workspace or an adoption keeps serving.
///
/// - Returns: Whether it exited.
bool retireIdleBundledHost(pid_t process, const std::string& bundledHostPath)
{
    if (!runsFile(process, bundledHostPath) || holdsPseudoTerminal(process) || ::kill(process, SIGUSR2) != 0)
        return false;
    const auto deadline = Clock::now() + retireBudget;
    while (Clock::now() < deadline) {
        if (hasExited(process))
            return true;
        ::usleep(10'000);
    }
    return false;
}

} // namespace

LocalHostInstallOutcome installBundledHost(const std::string& bundledHostPath, const std::string& installPath, std::string_view bundledVersion)
{
    const auto refuse = [](std::string reason) {
        return LocalHostInstallOutcome { .kind = LocalHostInstallKind::Refused, .reason = std::move(reason), .isOnAnotherVolume = false };
    };
    if (bundledHostPath.empty() || bundledHostPath.front() != '/' || installPath.size() < 2 || installPath.front() != '/' || installPath.back() == '/')
        return refuse("the bundled host or its install path is not an absolute path");
    const std::string directory = installPath.substr(0, std::max<std::size_t>(installPath.find_last_of('/'), 1));
    struct stat directoryInfo {};
    if (::lstat(directory.c_str(), &directoryInfo) != 0)
        return refuse(std::format("{} could not be read: {}", directory, errorWords(errno)));
    if (!S_ISDIR(directoryInfo.st_mode))
        return refuse(std::format("{} is not a directory", directory));
    if (directoryInfo.st_uid != ::geteuid())
        return refuse(std::format("{} belongs to another user", directory));

    PAL::UnixFileDescriptor bundled(::open(bundledHostPath.c_str(), O_RDONLY | O_CLOEXEC));
    struct stat bundledInfo {};
    if (!bundled || ::fstat(bundled.get(), &bundledInfo) != 0 || !S_ISREG(bundledInfo.st_mode) || bundledInfo.st_size <= 0)
        return refuse(std::format("{} is not a host that can be read", bundledHostPath));
    // A link makes the install the bundle's own file, with its owner and
    // mode; a file another user owns, or others can write, is copied.
    std::string linkRefusal;
    if (bundledInfo.st_uid != ::geteuid())
        linkRefusal = std::format("{} belongs to another user", bundledHostPath);
    else if (bundledInfo.st_mode & (S_IWGRP | S_IWOTH))
        linkRefusal = std::format("{} can be written by other users", bundledHostPath);
    const bool isOnAnotherVolume = bundledInfo.st_dev != directoryInfo.st_dev;
    if (linkRefusal.empty() && isOnAnotherVolume)
        linkRefusal = std::format("{} is on another volume than {}", bundledHostPath, directory);

    struct stat installedInfo {};
    if (::lstat(installPath.c_str(), &installedInfo) == 0) {
        if (!S_ISREG(installedInfo.st_mode))
            return refuse(std::format("{} is not a regular file", installPath));
        if (installedInfo.st_dev == bundledInfo.st_dev && installedInfo.st_ino == bundledInfo.st_ino) {
            nameInterface(installPath, bundledHostPath, directory);
            return { .kind = LocalHostInstallKind::Current, .reason = {}, .isOnAnotherVolume = false };
        }
        const bool isSameBytes = hasSameBytes(bundled.get(), bundledInfo.st_size, installPath);
        // A copy with the bundled bytes stays when the bundled file cannot
        // be linked; one that can is replaced by the bundled file itself.
        if (isSameBytes && !linkRefusal.empty()) {
            nameInterface(installPath, bundledHostPath, directory);
            return { .kind = LocalHostInstallKind::Copied, .reason = linkRefusal, .isOnAnotherVolume = isOnAnotherVolume };
        }
        if (!isSameBytes) {
            // A newer host is never replaced by an older app's, nor one too
            // slow to say which it is.
            const auto installed = statedVersion(installPath);
            if (installed.isSlow)
                return refuse(std::format("{} did not say its version within {} seconds", installPath, versionBudget.count()));
            const auto order = Protocol::Sync::compareAppVersions(installed.version, bundledVersion);
            if (order == std::strong_ordering::greater)
                return { .kind = LocalHostInstallKind::NewerInstalled, .reason = installed.version };
        }
    } else if (errno != ENOENT) {
        return refuse(std::format("{} could not be read: {}", installPath, errorWords(errno)));
    }

    const std::string staged = stagePath(directory, "install");
    if (linkRefusal.empty()) {
        // The same file, so a holder that names either path as its install
        // finds the successor there. Followed through a link, so a bundle
        // reached by one stages the file itself; the stage is checked to be
        // the file held open, so a bundle replaced since it was opened
        // stages nothing.
        if (::linkat(AT_FDCWD, bundledHostPath.c_str(), AT_FDCWD, staged.c_str(), AT_SYMLINK_FOLLOW) == 0) {
            struct stat stagedInfo {};
            if (::lstat(staged.c_str(), &stagedInfo) != 0 || stagedInfo.st_dev != bundledInfo.st_dev || stagedInfo.st_ino != bundledInfo.st_ino) {
                (void)::unlink(staged.c_str());
                return refuse(std::format("{} changed while it was being installed", bundledHostPath));
            }
            if (::rename(staged.c_str(), installPath.c_str()) != 0) {
                const int error = errno;
                (void)::unlink(staged.c_str());
                return refuse(std::format("{} could not be replaced: {}", installPath, errorWords(error)));
            }
            // A rename onto another link of the same file, as another install
            // made at once leaves, moves nothing and leaves the stage behind.
            (void)::unlink(staged.c_str());
            nameInterface(installPath, bundledHostPath, directory);
            return { .kind = LocalHostInstallKind::Linked, .reason = {}, .isOnAnotherVolume = false };
        }
        linkRefusal = std::format("{} could not be linked into {}: {}", bundledHostPath, directory, errorWords(errno));
    }
    if (const std::string failure = copyTo(bundled.get(), bundledInfo.st_size, staged); !failure.empty()) {
        (void)::unlink(staged.c_str());
        return refuse(failure);
    }
    if (::rename(staged.c_str(), installPath.c_str()) != 0) {
        const int error = errno;
        (void)::unlink(staged.c_str());
        return refuse(std::format("{} could not be replaced: {}", installPath, errorWords(error)));
    }
    nameInterface(installPath, bundledHostPath, directory);
    return { .kind = LocalHostInstallKind::Copied, .reason = linkRefusal, .isOnAnotherVolume = isOnAnotherVolume };
}

bool isLocalHostUpgradable(LocalHostBuild build)
{
    // A build from a checkout leaves the person's own control directory, its
    // install and the hosts running there as they are.
    const char* privateHome = ::getenv("UBIQUITTY_HOME");
    return build == LocalHostBuild::Release || (privateHome && privateHome[0]);
}

static LocalHostTakeoverOutcome takeOverLocalHostsForVersion(const std::string&, LocalHostTakeoverMode, std::string_view);
static LocalHostUpgradeOutcome upgradeLocalHostAtVersion(const std::string&, LocalHostBuild, std::string_view);

LocalHostUpgradeOutcome upgradeLocalHost(const std::string& bundledHostPath, LocalHostBuild build)
{
    return upgradeLocalHostAtVersion(bundledHostPath, build, Protocol::Sync::appVersion());
}

LocalHostUpgradeOutcome upgradeLocalHostForUpdate(const std::string& bundledHostPath, LocalHostBuild build, const std::string& version)
{
    if (!Protocol::Sync::parseAppVersion(version) || statedVersion(bundledHostPath).version != version) {
        LocalHostUpgradeOutcome outcome;
        outcome.install = { .kind = LocalHostInstallKind::Refused, .reason = "the host does not state the update's version" };
        return outcome;
    }
    return upgradeLocalHostAtVersion(bundledHostPath, build, version);
}

static LocalHostUpgradeOutcome upgradeLocalHostAtVersion(const std::string& bundledHostPath, LocalHostBuild build, std::string_view version)
{
    LocalHostUpgradeOutcome outcome;
    if (!isLocalHostUpgradable(build))
        return outcome;
    const std::string installPath = localHostInstallPath();
    outcome.install = installPath.empty() || !ensureControlDirectory()
        ? LocalHostInstallOutcome { .kind = LocalHostInstallKind::Refused, .reason = "the control directory could not be made", .isOnAnotherVolume = false }
        : installBundledHost(bundledHostPath, installPath, version);
    const auto kind = outcome.install.kind;
    outcome.isInstalled = kind == LocalHostInstallKind::Current || kind == LocalHostInstallKind::Linked;
    const auto takeover = takeOverLocalHostsForVersion(bundledHostPath, kind == LocalHostInstallKind::NewerInstalled ? LocalHostTakeoverMode::Inspect : LocalHostTakeoverMode::Start, version);
    outcome.didStartSuccessor = takeover.didStartSuccessor;
    outcome.isBundledHostAnswering = takeover.isHostAnswering;
    outcome.olderHostCount = takeover.olderHostCount;
    outcome.refusingHostCount = takeover.refusingHostCount;
    return outcome;
}

LocalHostTakeoverOutcome takeOverLocalHosts(const std::string& bundledHostPath, LocalHostTakeoverMode mode)
{
    return takeOverLocalHostsForVersion(bundledHostPath, mode, Protocol::Sync::appVersion());
}

static LocalHostTakeoverOutcome takeOverLocalHostsForVersion(const std::string& bundledHostPath, LocalHostTakeoverMode mode, std::string_view version)
{
    LocalHostTakeoverOutcome outcome;
    const std::string ownSocket = localDaemonSocketPath(bundledHostPath);
    if (ownSocket.empty())
        return outcome;
    const std::string root = ownSocket.substr(0, ownSocket.find_last_of('/'));
    const std::string ownLeaf = ownSocket.substr(ownSocket.find_last_of('/') + 1);
    auto bundledProcess = servingProcess(ownSocket);
    std::uint64_t bundledInstance = bundledProcess ? askHost(bundledHostPath, ownLeaf).instance : 0;
    outcome.isHostAnswering = bundledProcess.has_value();
    auto hosts = otherHosts(bundledHostPath, root, ownLeaf, bundledProcess, bundledInstance, version);
    // Nothing is started over a newer installed host, whose app installed
    // it, nor for hosts of newer builds alone. Wherever an older host runs
    // from, the bundled host asks it; one installed where the bundled file
    // is not refuses, and keeps serving.
    if (mode == LocalHostTakeoverMode::Start && std::ranges::any_of(hosts, [](const OtherHost& host) { return !host.isNewer; })) {
        // A bundled host already running asked the older hosts as it
        // started; one that holds nothing stops, to ask them again.
        if (!bundledProcess || retireIdleBundledHost(*bundledProcess, bundledHostPath)) {
            outcome.didStartSuccessor = true;
            outcome.isHostAnswering = !readyLocalDaemon(bundledHostPath).socketPath.empty();
            bundledProcess = servingProcess(ownSocket);
            bundledInstance = bundledProcess ? askHost(bundledHostPath, ownLeaf).instance : 0;
            hosts = otherHosts(bundledHostPath, root, ownLeaf, bundledProcess, bundledInstance, version);
        }
    }
    struct stat bundledInfo {};
    const bool isBundledReadable = ::stat(bundledHostPath.c_str(), &bundledInfo) == 0;
    for (const OtherHost& host : hosts) {
        if (host.isNewer)
            continue;
        ++outcome.olderHostCount;
        if (!isBundledReadable || !host.process || !startedFromBundledFile(*host.process, bundledInfo))
            ++outcome.refusingHostCount;
    }
    return outcome;
}

} // namespace Ubiquitty::Session
