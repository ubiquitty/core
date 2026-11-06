// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Platform/Host/HandoverAdmission.h"

#include "Core/Crypto/SHA256.h"
#include "Core/Protocol/Sync/HandoverBody.h"
#include "Platform/Host/CodeSignature.h"
#include "Platform/Host/HostSocketName.h"
#include "Platform/Host/PeerProcessInstance.h"

#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits.h>
#include <limits>
#include <optional>
#include <poll.h>
#include <span>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include "Platform/Apple/CFAuto.h"
#include <Security/SecCode.h>
#include <Security/SecRequirement.h>
#include <Security/SecStaticCode.h>
#include <libproc.h>
#include <sys/event.h>
#elif defined(__linux__)
#include <asm/socket.h>
#endif

namespace Ubiquitty::Host {

namespace {

using Refusal = Protocol::Sync::HandoverRefuseReason;

bool isConnectedLocalStream(int channel)
{
    sockaddr_storage address {};
    socklen_t length = sizeof(address);
    int type = 0;
    socklen_t typeLength = sizeof(type);
    return ::getpeername(channel, reinterpret_cast<sockaddr*>(&address), &length) == 0 && address.ss_family == AF_UNIX
        && ::getsockopt(channel, SOL_SOCKET, SO_TYPE, &type, &typeLength) == 0 && typeLength == sizeof(type) && type == SOCK_STREAM;
}

bool sameFile(const struct stat& first, const struct stat& second)
{
    return first.st_dev == second.st_dev && first.st_ino == second.st_ino && S_ISREG(first.st_mode) && S_ISREG(second.st_mode);
}

bool sameExecutableContents(const struct stat& first, const struct stat& second)
{
    if (!sameFile(first, second) || first.st_size != second.st_size)
        return false;
#if defined(__APPLE__)
    return first.st_mtimespec.tv_sec == second.st_mtimespec.tv_sec && first.st_mtimespec.tv_nsec == second.st_mtimespec.tv_nsec
        && first.st_ctimespec.tv_sec == second.st_ctimespec.tv_sec && first.st_ctimespec.tv_nsec == second.st_ctimespec.tv_nsec;
#else
    return first.st_mtim.tv_sec == second.st_mtim.tv_sec && first.st_mtim.tv_nsec == second.st_mtim.tv_nsec
        && first.st_ctim.tv_sec == second.st_ctim.tv_sec && first.st_ctim.tv_nsec == second.st_ctim.tv_nsec;
#endif
}

std::optional<std::array<std::uint8_t, 32>> executableDigest(int file, off_t length)
{
    if (length <= 0)
        return std::nullopt;
    Crypto::SHA256Hasher hasher;
    std::array<std::uint8_t, 65536> chunk {};
    off_t offset = 0;
    while (offset < length) {
        const auto wanted = static_cast<std::size_t>(std::min<off_t>(length - offset, chunk.size()));
        const auto count = ::pread(file, chunk.data(), wanted, offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0 || static_cast<std::size_t>(count) > wanted || !hasher.update(std::span(chunk).first(static_cast<std::size_t>(count))))
            return std::nullopt;
        offset += count;
    }
    std::array<std::uint8_t, 32> digest {};
    if (!hasher.finalize(digest))
        return std::nullopt;
    return digest;
}

bool isAbsolutePath(std::string_view path)
{
    return !path.empty() && path.front() == '/' && path.size() < PATH_MAX && path.find('\0') == std::string_view::npos;
}

#if defined(__APPLE__)
bool validateRunningCode(audit_token_t token, const std::string& executable)
{
    // The audit token includes the process incarnation. A static code's path
    // alone is not authority: require the running guest to match that file's
    // exact CDHash under dynamic validation, which detects source replacement.
    Apple::UniqueCF<CFDataRef> audit(CFDataCreate(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(&token), sizeof(token)));
    if (!audit)
        return false;
    const void* keys[] { kSecGuestAttributeAudit };
    const void* values[] { audit.get() };
    Apple::UniqueCF<CFDictionaryRef> attributes(CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    if (!attributes)
        return false;
    SecCodeRef guest = nullptr;
    const auto guestStatus = SecCodeCopyGuestWithAttributes(nullptr, attributes.get(), kSecCSDefaultFlags, &guest);
    Apple::UniqueCF<SecCodeRef> guestOwner(guest);
    if (guestStatus != errSecSuccess || !guestOwner)
        return false;
    Apple::UniqueCF<CFURLRef> url(CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault,
        reinterpret_cast<const UInt8*>(executable.data()), static_cast<CFIndex>(executable.size()), false));
    if (!url)
        return false;
    SecStaticCodeRef code = nullptr;
    const auto staticStatus = SecStaticCodeCreateWithPath(url.get(), kSecCSDefaultFlags, &code);
    Apple::UniqueCF<SecStaticCodeRef> codeOwner(code);
    if (staticStatus != errSecSuccess || !codeOwner)
        return false;
    CFDictionaryRef information = nullptr;
    const auto informationStatus = SecCodeCopySigningInformation(codeOwner.get(), kSecCSSigningInformation, &information);
    Apple::UniqueCF<CFDictionaryRef> informationOwner(information);
    if (informationStatus != errSecSuccess || !informationOwner)
        return false;
    const auto digest = static_cast<CFDataRef>(CFDictionaryGetValue(informationOwner.get(), kSecCodeInfoUnique));
    if (!digest || CFGetTypeID(digest) != CFDataGetTypeID() || CFDataGetLength(digest) != 20)
        return false;
    std::array<char, 51> expression {};
    constexpr std::string_view prefix = "cdhash H\"";
    std::ranges::copy(prefix, expression.begin());
    constexpr std::string_view hex = "0123456789abcdef";
    const auto* bytes = CFDataGetBytePtr(digest);
    for (std::size_t index = 0; index < 20; ++index) {
        expression[prefix.size() + index * 2] = hex[bytes[index] >> 4];
        expression[prefix.size() + index * 2 + 1] = hex[bytes[index] & 15];
    }
    expression[prefix.size() + 40] = '"';
    Apple::UniqueCF<CFStringRef> text(CFStringCreateWithBytes(kCFAllocatorDefault,
        reinterpret_cast<const UInt8*>(expression.data()), prefix.size() + 41, kCFStringEncodingUTF8, false));
    if (!text)
        return false;
    SecRequirementRef requirement = nullptr;
    const auto requirementStatus = SecRequirementCreateWithString(text.get(), kSecCSDefaultFlags, &requirement);
    Apple::UniqueCF<SecRequirementRef> requirementOwner(requirement);
    if (requirementStatus != errSecSuccess || !requirementOwner)
        return false;
    // The held descriptor pins the expected CDHash, but /dev/fd has no bundle
    // context for Info.plist and resources. Validate the guest's complete bundle
    // against that exact hash as well as the audit-token-bound running image.
    SecStaticCodeRef guestCode = nullptr;
    const auto guestCodeStatus = SecCodeCopyStaticCode(guestOwner.get(), kSecCSDefaultFlags, &guestCode);
    Apple::UniqueCF<SecStaticCodeRef> guestCodeOwner(guestCode);
    return guestCodeStatus == errSecSuccess && guestCodeOwner
        && SecStaticCodeCheckValidity(guestCodeOwner.get(), kSecCSDefaultFlags, requirementOwner.get()) == errSecSuccess
        && SecCodeCheckValidity(guestOwner.get(), kSecCSDefaultFlags, requirementOwner.get()) == errSecSuccess;
}
#endif

struct PeerExecutable {
    PAL::UnixFileDescriptor file;
    PAL::UnixFileDescriptor process;
    std::string path;
    std::uint32_t processID = 0;
    std::uint64_t instance = 0;
#if defined(__APPLE__)
    audit_token_t token {};
#endif
};

std::optional<PeerExecutable> peerExecutable(int channel)
{
    PeerExecutable peer;
#if defined(__APPLE__)
    pid_t processID = 0;
    socklen_t length = sizeof(processID);
    if (::getsockopt(channel, SOL_LOCAL, LOCAL_PEERPID, &processID, &length) != 0 || length != sizeof(processID) || processID <= 0)
        return std::nullopt;
    peer.processID = static_cast<std::uint32_t>(processID);
    length = sizeof(peer.token);
    if (::getsockopt(channel, SOL_LOCAL, LOCAL_PEERTOKEN, &peer.token, &length) != 0 || length != sizeof(peer.token)
        || !peerProcessInstanceOf(peer.processID, peer.instance))
        return std::nullopt;
    std::array<char, PROC_PIDPATHINFO_MAXSIZE> path {};
    const int size = ::proc_pidpath_audittoken(&peer.token, path.data(), path.size());
    if (size <= 0 || static_cast<std::size_t>(size) >= path.size())
        return std::nullopt;
    peer.path = path.data();
    peer.file.reset(::open(peer.path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK));
#elif defined(__linux__)
    struct ucred credentials {};
    socklen_t length = sizeof(credentials);
    if (::getsockopt(channel, SOL_SOCKET, SO_PEERCRED, &credentials, &length) != 0 || length != sizeof(credentials) || credentials.pid <= 0)
        return std::nullopt;
    peer.processID = static_cast<std::uint32_t>(credentials.pid);
    int process = -1;
    length = sizeof(process);
    const auto status = ::getsockopt(channel, SOL_SOCKET, SO_PEERPIDFD, &process, &length);
    peer.process.reset(process);
    pollfd lifetime { peer.process.get(), POLLIN, 0 };
    // A bare SO_PEERCRED PID can be recycled while a connection is queued.
    // The socket's pidfd pins that original incarnation, never a fresh pidfd_open.
    if (status != 0 || length != sizeof(process) || !peer.process || ::poll(&lifetime, 1, 0) != 0)
        return std::nullopt;
    if (!peerProcessInstanceOf(peer.processID, peer.instance))
        return std::nullopt;
    peer.path = "/proc/" + std::to_string(peer.processID) + "/exe";
    // The kernel link resolves the running image, not a replaceable pathname.
    peer.file.reset(::open(peer.path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK));
#else
    (void)channel;
    return std::nullopt;
#endif
    std::uint64_t after = 0;
    if (!peer.file || !peerProcessInstanceOf(peer.processID, after) || after != peer.instance)
        return std::nullopt;
    return peer;
}

bool isExpectedListener(int descriptor, const std::string& expectedPath)
{
    const auto path = listeningSocketPath(descriptor);
    return path && *path == expectedPath;
}

#if defined(__APPLE__)
/// The hash of a file's code directory, as its own signature states it.
///
/// - Parameter path: An absolute path.
/// - Returns: The hash, or nothing when the file carries no signature.
std::optional<std::vector<std::uint8_t>> codeDirectoryHash(const std::string& path)
{
    Apple::UniqueCF<CFURLRef> url(CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault,
        reinterpret_cast<const UInt8*>(path.data()), static_cast<CFIndex>(path.size()), false));
    if (!url)
        return std::nullopt;
    SecStaticCodeRef code = nullptr;
    const auto staticStatus = SecStaticCodeCreateWithPath(url.get(), kSecCSDefaultFlags, &code);
    Apple::UniqueCF<SecStaticCodeRef> codeOwner(code);
    if (staticStatus != errSecSuccess || !codeOwner)
        return std::nullopt;
    CFDictionaryRef information = nullptr;
    const auto informationStatus = SecCodeCopySigningInformation(codeOwner.get(), kSecCSSigningInformation, &information);
    Apple::UniqueCF<CFDictionaryRef> informationOwner(information);
    if (informationStatus != errSecSuccess || !informationOwner)
        return std::nullopt;
    const auto digest = static_cast<CFDataRef>(CFDictionaryGetValue(informationOwner.get(), kSecCodeInfoUnique));
    if (!digest || CFGetTypeID(digest) != CFDataGetTypeID() || CFDataGetLength(digest) <= 0)
        return std::nullopt;
    const std::span bytes(CFDataGetBytePtr(digest), static_cast<std::size_t>(CFDataGetLength(digest)));
    return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
}

/// The text of a string a signature names, bounded as a signing identifier is.
std::string textOf(CFStringRef text)
{
    std::array<char, 256> buffer {};
    if (!text || CFGetTypeID(text) != CFStringGetTypeID() || !CFStringGetCString(text, buffer.data(), static_cast<CFIndex>(buffer.size()), kCFStringEncodingUTF8))
        return {};
    return buffer.data();
}

/// The requirement the trusted team's signature meets: a certificate chain
/// to Apple whose leaf names the team.
Apple::UniqueCF<SecRequirementRef> teamRequirement()
{
    const std::string expression = "anchor apple generic and certificate leaf[subject.OU] = \"" + std::string(trustedSigningTeam) + "\"";
    Apple::UniqueCF<CFStringRef> text(CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(expression.data()),
        static_cast<CFIndex>(expression.size()), kCFStringEncodingUTF8, false));
    if (!text)
        return Apple::UniqueCF<SecRequirementRef>(nullptr);
    SecRequirementRef requirement = nullptr;
    if (SecRequirementCreateWithString(text.get(), kSecCSDefaultFlags, &requirement) != errSecSuccess)
        return Apple::UniqueCF<SecRequirementRef>(nullptr);
    return Apple::UniqueCF<SecRequirementRef>(requirement);
}

/// What a running code's signature says, judged against the trusted team.
///
/// An ad hoc signature is told by its flags, which name no signer; any other
/// is checked against the team's requirement, which validates the running
/// code and the file it was loaded from. A refusal of the requirement alone
/// is another signer's; any other failure, no signature that holds.
///
/// - Parameter code: A running code: this process's, or a guest's.
/// - Returns: The verdict.
CodeSignature signatureOfCode(SecCodeRef code)
{
    CodeSignature signature;
    SecStaticCodeRef staticCode = nullptr;
    const auto staticStatus = SecCodeCopyStaticCode(code, kSecCSDefaultFlags, &staticCode);
    Apple::UniqueCF<SecStaticCodeRef> staticOwner(staticCode);
    if (staticStatus != errSecSuccess || !staticOwner)
        return signature;
    CFDictionaryRef information = nullptr;
    const auto informationStatus = SecCodeCopySigningInformation(staticOwner.get(), kSecCSSigningInformation, &information);
    Apple::UniqueCF<CFDictionaryRef> informationOwner(information);
    if (informationStatus != errSecSuccess || !informationOwner)
        return signature;
    // An unsigned code names no identifier.
    signature.identifier = textOf(static_cast<CFStringRef>(CFDictionaryGetValue(informationOwner.get(), kSecCodeInfoIdentifier)));
    if (signature.identifier.empty())
        return signature;
    std::uint32_t flags = 0;
    const auto number = static_cast<CFNumberRef>(CFDictionaryGetValue(informationOwner.get(), kSecCodeInfoFlags));
    if (number && CFGetTypeID(number) == CFNumberGetTypeID())
        (void)CFNumberGetValue(number, kCFNumberSInt32Type, &flags);
    if (flags & kSecCodeSignatureAdhoc) {
        signature.signer = Signer::AdHoc;
        return signature;
    }
    const auto requirement = teamRequirement();
    if (!requirement)
        return signature;
    const auto status = SecCodeCheckValidity(code, kSecCSDefaultFlags, requirement.get());
    if (status == errSecSuccess)
        signature.signer = Signer::Team;
    else if (status == errSecCSReqFailed)
        signature.signer = Signer::OtherSigner;
    return signature;
}

/// The running code of the process an audit token names.
Apple::UniqueCF<SecCodeRef> guestCode(const audit_token_t& token)
{
    Apple::UniqueCF<CFDataRef> audit(CFDataCreate(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(&token), sizeof(token)));
    if (!audit)
        return Apple::UniqueCF<SecCodeRef>(nullptr);
    const void* keys[] { kSecGuestAttributeAudit };
    const void* values[] { audit.get() };
    Apple::UniqueCF<CFDictionaryRef> attributes(CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    if (!attributes)
        return Apple::UniqueCF<SecCodeRef>(nullptr);
    SecCodeRef guest = nullptr;
    if (SecCodeCopyGuestWithAttributes(nullptr, attributes.get(), kSecCSDefaultFlags, &guest) != errSecSuccess)
        return Apple::UniqueCF<SecCodeRef>(nullptr);
    return Apple::UniqueCF<SecCodeRef>(guest);
}
#endif

} // namespace

SigningPolicy buildSigningPolicy()
{
#if defined(UBIQUITTY_RELEASE_SIGNING) && UBIQUITTY_RELEASE_SIGNING
    return SigningPolicy::Release;
#else
    return SigningPolicy::Development;
#endif
}

CodeSignature signatureOfSelf()
{
#if defined(__APPLE__)
    SecCodeRef self = nullptr;
    if (SecCodeCopySelf(kSecCSDefaultFlags, &self) != errSecSuccess)
        return {};
    Apple::UniqueCF<SecCodeRef> selfOwner(self);
    return selfOwner ? signatureOfCode(selfOwner.get()) : CodeSignature {};
#else
    return { .signer = Signer::Unsupported, .identifier = {} };
#endif
}

CodeSignature signatureOfPeer(int channel)
{
#if defined(__APPLE__)
    audit_token_t token {};
    socklen_t length = sizeof(token);
    if (::getsockopt(channel, SOL_LOCAL, LOCAL_PEERTOKEN, &token, &length) != 0 || length != sizeof(token))
        return {};
    const auto guest = guestCode(token);
    return guest ? signatureOfCode(guest.get()) : CodeSignature {};
#else
    (void)channel;
    return { .signer = Signer::Unsupported, .identifier = {} };
#endif
}

bool hasSameCode(const std::string& first, const std::string& second)
{
#if defined(__APPLE__)
    struct stat firstInfo {}, secondInfo {};
    if (!isAbsolutePath(first) || !isAbsolutePath(second) || ::stat(first.c_str(), &firstInfo) != 0 || ::stat(second.c_str(), &secondInfo) != 0
        || !S_ISREG(firstInfo.st_mode) || !S_ISREG(secondInfo.st_mode) || firstInfo.st_size != secondInfo.st_size)
        return false;
    const auto firstHash = codeDirectoryHash(first);
    return firstHash && firstHash == codeDirectoryHash(second);
#else
    (void)first;
    (void)second;
    return false;
#endif
}

std::optional<std::string> listeningSocketPath(int descriptor)
{
    sockaddr_un address {};
    socklen_t length = sizeof(address);
    int type = 0;
    socklen_t valueBytes = sizeof(type);
    if (::getsockopt(descriptor, SOL_SOCKET, SO_TYPE, &type, &valueBytes) != 0 || valueBytes != sizeof(type) || type != SOCK_STREAM)
        return std::nullopt;
#if defined(__APPLE__)
    // Darwin does not expose SO_ACCEPTCONN through getsockopt for Unix sockets.
    // Read the same kernel flag for this owned descriptor, without accept/listen.
    socket_fdinfo information {};
    if (::proc_pidfdinfo(::getpid(), descriptor, PROC_PIDFDSOCKETINFO, &information, sizeof(information)) != sizeof(information)
        || !(information.psi.soi_options & SO_ACCEPTCONN))
        return std::nullopt;
#else
    int isListening = 0;
    valueBytes = sizeof(isListening);
    if (::getsockopt(descriptor, SOL_SOCKET, SO_ACCEPTCONN, &isListening, &valueBytes) != 0 || valueBytes != sizeof(isListening) || !isListening)
        return std::nullopt;
#endif
    if (::getsockname(descriptor, reinterpret_cast<sockaddr*>(&address), &length) != 0 || address.sun_family != AF_UNIX
        || length <= offsetof(sockaddr_un, sun_path) || length > sizeof(address))
        return std::nullopt;
    const auto capacity = static_cast<std::size_t>(length) - offsetof(sockaddr_un, sun_path);
    const auto pathBytes = ::strnlen(address.sun_path, capacity);
    if (pathBytes >= capacity)
        return std::nullopt;
    return std::string(address.sun_path, pathBytes);
}

bool handoverPeerHasSameUser(int channel)
{
    if (!isConnectedLocalStream(channel))
        return false;
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    uid_t user = 0;
    gid_t group = 0;
    return ::getpeereid(channel, &user, &group) == 0 && user == ::geteuid();
#elif defined(__linux__)
    struct ucred credentials {};
    socklen_t length = sizeof(credentials);
    return ::getsockopt(channel, SOL_SOCKET, SO_PEERCRED, &credentials, &length) == 0
        && length == sizeof(credentials) && credentials.uid == ::geteuid();
#else
    return false;
#endif
}

std::expected<LocalAdoptExecutables, Session::AdoptTransactionError> LocalAdoptExecutables::capture(const std::string& hostPath)
{
    LocalAdoptExecutables result;
    if (hostPath.empty())
        return std::unexpected(Session::AdoptTransactionError::Unauthorised);
    const auto live = pathWithoutDeletedMarker(hostPath);
    const std::array paths { hostPath, live, interfacePathForHost(hostPath), interfacePathForHost(live) };
    std::size_t admitted = 0;
    for (const auto& path : paths) {
        if (path.empty())
            continue;
        PAL::UnixFileDescriptor file(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK));
        struct stat before {}, after {};
        if (!file || ::fstat(file.get(), &before) != 0 || !S_ISREG(before.st_mode)
            || std::ranges::any_of(result.m_executables, [&](const Executable& held) {
                   return held.file && held.identity.device == before.st_dev && held.identity.inode == before.st_ino;
               }))
            continue;
        if (before.st_size <= 0 || ::fstat(file.get(), &after) != 0 || !sameExecutableContents(before, after))
            continue;
        auto& executable = result.m_executables[admitted++];
        executable.file = std::move(file);
        executable.identity = { before.st_dev, before.st_ino };
        executable.captured = after;
    }
    if (!admitted)
        return std::unexpected(Session::AdoptTransactionError::Unauthorised);
    return result;
}

std::optional<std::array<std::uint8_t, 32>> LocalAdoptExecutables::digestOf(const Executable& executable)
{
    if (executable.digest)
        return executable.digest;
    // Hashed only while the held file is still the captured one, before and
    // after: a file written in place since the capture is not the executable
    // this daemon trusted.
    struct stat before {}, after {};
    if (!executable.file || ::fstat(executable.file.get(), &before) != 0 || !sameExecutableContents(executable.captured, before))
        return std::nullopt;
    const auto digest = executableDigest(executable.file.get(), before.st_size);
    if (!digest || ::fstat(executable.file.get(), &after) != 0 || !sameExecutableContents(executable.captured, after))
        return std::nullopt;
    executable.digest = digest;
    return digest;
}

std::optional<Session::AdoptAuthority> currentLocalAdoptAuthority(int channel, const LocalAdoptExecutables& trusted)
{
    if (!handoverPeerHasSameUser(channel))
        return std::nullopt;
    auto peer = peerExecutable(channel);
    if (!peer || peer->processID > static_cast<std::uint32_t>(std::numeric_limits<PAL::ProcessID>::max()))
        return std::nullopt;
    const auto processID = static_cast<PAL::ProcessID>(peer->processID);
    const auto before = PAL::ptyProcessSnapshot(processID);
    struct stat executable {};
    if (!before || (!before->birth.high && !before->birth.low) || ::fstat(peer->file.get(), &executable) != 0
        || !S_ISREG(executable.st_mode))
        return std::nullopt;
    const auto allowed = std::ranges::find_if(trusted.m_executables, [&](const LocalAdoptExecutables::Executable& held) {
        return held.file && held.identity.device == executable.st_dev && held.identity.inode == executable.st_ino;
    });
    if (allowed == trusted.m_executables.end())
        return std::nullopt;
#if defined(__APPLE__)
    if (!validateRunningCode(peer->token, "/dev/fd/" + std::to_string(peer->file.get())))
        return std::nullopt;
#endif
    // The trusted digest is taken once, on the first adoption that asks.
    // A peer file whose size, modification and status-change times all still
    // read as the captured file's holds the same contents: any write moves its
    // status-change time, which no unprivileged caller can set. Hashing it
    // again would hold the daemon's loop for as long as the read takes, which
    // for a large executable outlasts the peer's silence bound.
    const auto trustedDigest = LocalAdoptExecutables::digestOf(*allowed);
    const auto digest = !trustedDigest                          ? std::nullopt
        : sameExecutableContents(allowed->captured, executable) ? trustedDigest
                                                                : executableDigest(peer->file.get(), executable.st_size);
    if (!digest || *digest != *trustedDigest)
        return std::nullopt;
    // Reopen through the physical socket identity, not through an accepted PID
    // or a cached path. On Linux this also reacquires and polls SO_PEERPIDFD;
    // on macOS proc_pidpath_audittoken pins the socket's process incarnation.
    auto afterPeer = peerExecutable(channel);
    const auto after = PAL::ptyProcessSnapshot(processID);
    struct stat retained {}, running {};
    if (!afterPeer || afterPeer->processID != peer->processID || afterPeer->instance != peer->instance
        || !after || after->birth != before->birth || !handoverPeerHasSameUser(channel)
        || ::fstat(peer->file.get(), &retained) != 0 || ::fstat(afterPeer->file.get(), &running) != 0
        || !sameExecutableContents(executable, retained) || !sameExecutableContents(executable, running))
        return std::nullopt;
#if defined(__APPLE__)
    if (!validateRunningCode(afterPeer->token, "/dev/fd/" + std::to_string(peer->file.get())))
        return std::nullopt;
#elif defined(__linux__)
    pollfd originalLifetime { peer->process.get(), POLLIN, 0 };
    pollfd currentLifetime { afterPeer->process.get(), POLLIN, 0 };
    if (::poll(&originalLifetime, 1, 0) != 0 || ::poll(&currentLifetime, 1, 0) != 0)
        return std::nullopt;
#endif
    return Session::AdoptAuthority { .processID = processID, .birth = before->birth, .executableSHA256 = *digest };
}

LocalAdoptLifetime::LocalAdoptLifetime(PAL::UnixFileDescriptor watch, const Session::AdoptAuthority& authority)
    : m_watch(std::move(watch))
    , m_authority(authority)
{
}

LocalAdoptLifetime::LocalAdoptLifetime(LocalAdoptLifetime&& other) noexcept
    : m_watch(std::move(other.m_watch))
    , m_authority(std::exchange(other.m_authority, {}))
    , m_didExit(std::exchange(other.m_didExit, false))
{
}

LocalAdoptLifetime& LocalAdoptLifetime::operator=(LocalAdoptLifetime&& other) noexcept
{
    if (this == &other)
        return *this;
    m_watch = std::move(other.m_watch);
    m_authority = std::exchange(other.m_authority, {});
    m_didExit = std::exchange(other.m_didExit, false);
    return *this;
}

std::expected<LocalAdoptLifetime, Session::AdoptTransactionError> LocalAdoptLifetime::capture(
    int physicalSocket, const Session::AdoptAuthority& expected, const LocalAdoptExecutables& trusted)
{
    using Error = Session::AdoptTransactionError;
    const auto before = currentLocalAdoptAuthority(physicalSocket, trusted);
    if (!before || *before != expected)
        return std::unexpected(Error::StaleAuthority);
    PAL::UnixFileDescriptor watch;
#if defined(__APPLE__)
    watch.reset(::kqueue());
    if (!watch)
        return std::unexpected(errno == ENOMEM ? Error::NoMemory : Error::Unauthorised);
    const auto flags = ::fcntl(watch.get(), F_GETFD);
    if (flags < 0 || ::fcntl(watch.get(), F_SETFD, flags | FD_CLOEXEC) < 0)
        return std::unexpected(Error::Unauthorised);
    struct kevent registration {}, receipt {};
    EV_SET(&registration, static_cast<std::uintptr_t>(expected.processID), EVFILT_PROC,
        EV_ADD | EV_ONESHOT | EV_RECEIPT, NOTE_EXIT, 0, nullptr);
    const struct timespec immediate {};
    const auto registered = ::kevent(watch.get(), &registration, 1, &receipt, 1, &immediate);
    if (registered != 1 || receipt.ident != static_cast<std::uintptr_t>(expected.processID) || receipt.filter != EVFILT_PROC
        || !(receipt.flags & EV_ERROR) || receipt.data != 0)
        return std::unexpected(Error::Unauthorised);
#elif defined(__linux__)
    // The connected socket supplies a pidfd for its original incarnation.
    // Opening a new watch by PID could silently bind to a recycled process.
    auto peer = peerExecutable(physicalSocket);
    if (!peer || !peer->process || peer->processID != static_cast<std::uint32_t>(expected.processID))
        return std::unexpected(Error::StaleAuthority);
    watch = std::move(peer->process);
    const auto flags = ::fcntl(watch.get(), F_GETFD);
    if (flags < 0 || ::fcntl(watch.get(), F_SETFD, flags | FD_CLOEXEC) < 0)
        return std::unexpected(Error::Unauthorised);
#else
    return std::unexpected(Error::Unauthorised);
#endif
    const auto after = currentLocalAdoptAuthority(physicalSocket, trusted);
    if (!after || *after != expected)
        return std::unexpected(Error::StaleAuthority);
#if defined(__linux__)
    struct pollfd lifetime { .fd = watch.get(), .events = POLLIN, .revents = 0 };
    if (::poll(&lifetime, 1, 0) != 0)
        return std::unexpected(Error::StaleAuthority);
#endif
    return LocalAdoptLifetime(std::move(watch), expected);
}

bool LocalAdoptLifetime::didExit()
{
    if (!m_watch)
        return false;
    if (m_didExit)
        return true;
#if defined(__APPLE__)
    struct kevent event {};
    const struct timespec immediate {};
    const auto count = ::kevent(m_watch.get(), nullptr, 0, &event, 1, &immediate);
    if (count == 1 && event.ident == static_cast<std::uintptr_t>(m_authority.processID) && event.filter == EVFILT_PROC
        && !(event.flags & EV_ERROR) && (event.fflags & NOTE_EXIT))
        m_didExit = true;
#elif defined(__linux__)
    struct pollfd lifetime { .fd = m_watch.get(), .events = POLLIN, .revents = 0 };
    if (::poll(&lifetime, 1, 0) == 1 && (lifetime.revents & POLLIN) && !(lifetime.revents & (POLLERR | POLLNVAL)))
        m_didExit = true;
#endif
    return m_didExit;
}

std::expected<AdmittedHandoverRequest, Protocol::Sync::HandoverRefuseReason> admitHandoverRequest(
    int channel, PAL::UnixFileDescriptor listener, const Protocol::Sync::HandoverRequest& request, const HandoverAdmissionContext& context)
{
    if (!context.isFirstRecord || !listener || !handoverPeerHasSameUser(channel)
        || !isAbsolutePath(context.installPath) || !isAbsolutePath(context.controlDirectory))
        return std::unexpected(Refusal::NotSuccessor);
    if (request.maxBodyVersion < Protocol::Sync::handoverBodyVersion)
        return std::unexpected(Refusal::NewerBody);
    if (request.requesterInstance == Protocol::Sync::ID128 {})
        return std::unexpected(Refusal::Malformed);
    auto peer = peerExecutable(channel);
    if (!peer)
        return std::unexpected(Refusal::NotSuccessor);
    const std::string installPath = pathWithoutDeletedMarker(std::string(context.installPath));
    PAL::UnixFileDescriptor installed(::open(installPath.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK));
    struct stat installedInfo {}, peerInfo {};
    if (::fstat(peer->file.get(), &peerInfo) != 0)
        return std::unexpected(Refusal::NotSuccessor);
    const bool isInstalledFile = installed && ::fstat(installed.get(), &installedInfo) == 0 && sameFile(installedInfo, peerInfo); // bool-ok: names the one fact it reports
    const auto place = isInstalledFile ? InstallPlace::AtInstallPath : InstallPlace::Elsewhere;
    // Read once, from the candidate's running code: a signature check
    // validates that code against what its signature covers.
    const auto route = successorRoute(buildSigningPolicy(), context.holderSignature, signatureOfPeer(channel), place);
    if (route == SuccessorRoute::Refused)
        return std::unexpected(Refusal::NotSuccessor);
#if defined(__APPLE__)
    // Admitted as the installed file, its running code must be that file's.
    if (route == SuccessorRoute::ByPlace && !validateRunningCode(peer->token, "/dev/fd/" + std::to_string(installed.get())))
        return std::unexpected(Refusal::NotSuccessor);
#endif
    const auto leaf = socketLeafForBinary(("/dev/fd/" + std::to_string(peer->file.get())).c_str());
    if (leaf.empty())
        return std::unexpected(Refusal::NotSuccessor);
    const std::string socketPath = std::string(context.controlDirectory) + "/" + leaf;
    struct stat finalInstall {}, finalPeer {};
    std::uint64_t finalInstance = 0;
    if (!isExpectedListener(listener.get(), socketPath) || !handoverPeerHasSameUser(channel)
        || !peerProcessInstanceOf(peer->processID, finalInstance) || finalInstance != peer->instance
        || ::stat(peer->path.c_str(), &finalPeer) != 0 || !sameFile(peerInfo, finalPeer))
        return std::unexpected(Refusal::NotSuccessor);
    // Admitted as the installed file, it must still be that file.
    if (route == SuccessorRoute::ByPlace && (::stat(installPath.c_str(), &finalInstall) != 0 || !sameFile(installedInfo, finalInstall)))
        return std::unexpected(Refusal::NotSuccessor);
#if defined(__linux__)
    pollfd lifetime { peer->process.get(), POLLIN, 0 };
    if (::poll(&lifetime, 1, 0) != 0)
        return std::unexpected(Refusal::NotSuccessor);
#endif
    return AdmittedHandoverRequest { request, std::move(listener), peer->processID, { peerInfo.st_dev, peerInfo.st_ino } };
}

} // namespace Ubiquitty::Host
