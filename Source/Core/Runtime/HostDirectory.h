// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// HostDirectory.h — Ubiquitty::Session::RememberedSessions, as a frontend
// reaches it.
//
// The memory and the connect rule are Core's and stay there: a frontend that
// spelled `decide` itself would answer the same three inputs differently from
// every other frontend, which is the one thing RememberedSessions exists to
// prevent. What this adds is only the crossing: the rule reads the host's
// tree, which a frontend reaches through the session that listed the host,
// and the record recalled through the embedder's store, which crosses as two
// function pointers over a byte buffer rather than over std::string, which no
// @convention(c) function may name.
//
// Process-wide rather than one instance per caller: an app has one saved-host
// list, so a second directory would be a second answer to what a host was last
// attached on, and a window that remembered through one while another recalled
// through the other would reattach to a session nothing had paired it with.
//
// Where a record is kept stays the embedder's — a preferences file, a keychain
// item, a JSON store beside the saved host — and nothing here reads a record's
// bytes: RememberedSessions encodes and decodes them, so a build that spells the
// record differently refuses its own old bytes rather than every store having to
// know what changed.
//
// Exception/rtti-free (WebKit style); no platform types.

#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/Session/DeployPolicy.h"
#include "Core/Session/RememberedSessions.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Ubiquitty::Runtime {

/// The host-to-session pairing and the connect rule over it, for one app.
///
/// Every member is static because the directory is the process's, not the
/// caller's; see the file comment for why there is exactly one.
class HostDirectory {
public:
    /// Reads a host's stored bytes into the caller's buffer.
    ///
    /// Bytes rather than a std::string, and a length rather than a bool: a
    /// Swift `@convention(c)` function may name neither a C++ reference nor a
    /// non-trivial C++ value, so the shape RememberedSessions declares cannot
    /// itself cross. The length is answered whole, so a caller learns when its
    /// buffer was too small instead of reading a truncated record as a whole
    /// one.
    ///
    /// - Parameters:
    ///   - context: The embedder's, as ``setPersistence`` was given it.
    ///   - hostKey: The host being recalled, NUL-terminated.
    ///   - buffer: Receives up to `capacity` bytes; null asks for the length
    ///     alone.
    ///   - capacity: How many bytes `buffer` holds.
    /// - Returns: The full stored length, which may exceed `capacity`; zero
    ///   when the host has nothing stored.
    using Load = std::size_t (*)(void* context, const char* hostKey, std::uint8_t* buffer, std::size_t capacity);

    /// Writes a host's bytes, replacing whatever was there.
    ///
    /// - Parameters:
    ///   - context: The embedder's, as ``setPersistence`` was given it.
    ///   - hostKey: The host being remembered, NUL-terminated.
    ///   - bytes: The record.
    ///   - byteCount: Its length; zero means forget the host.
    using Store = void (*)(void* context, const char* hostKey, const std::uint8_t* bytes, std::size_t byteCount);

    /// Installs the embedder's storage.
    ///
    /// Until this is called every recall answers nothing and every remember is
    /// dropped, which is right for an embedder with nowhere to keep a record:
    /// it connects as though it had never been to the host.
    ///
    /// Replacement publishes one owned lease — callback plus context together —
    /// so a concurrent recall, remember, forget, or decision can never invoke
    /// a callback paired with the wrong context, and an in-flight operation
    /// finishes on the lease it started with while the next one sees the
    /// newest. The embedder's own closures always run with no directory lock
    /// held, so a closure that re-enters this directory (including one that
    /// replaces persistence again) cannot deadlock; see the reentrancy note on
    /// ``State``.
    ///
    /// Lifetime split, stated once: the lease keeps the callback/context pair
    /// coherent, not the context alive. A C++ embedder's context must outlive
    /// replacement plus any in-flight call; the Swift facade retains its box
    /// behind its own lock instead. `context` borrowed on those terms; install
    /// a pair of nulls to clear it.
    ///
    /// - Parameters:
    ///   - load: Reads a host's bytes.
    ///   - store: Writes them.
    ///   - context: Handed to both.
    static void setPersistence(Load load, Store store, void* context)
    {
        State& state = directoryState();
        {
            const std::lock_guard<std::mutex> guard(state.mutex);
            state.lease = std::make_shared<const Lease>(load, store, context);
            // Hook the rule's memory to the lease-carrying thunks exactly once:
            // rewriting the rule's own hook fields on every install would tear
            // them against a concurrent recall. A cleared lease (both null) refuses inside the
            // thunks, which reads exactly as an unhooked rule.
            if (!state.hasInstalledHooks) {
                state.remembered.setPersistence(&loadThroughEmbedder, &storeThroughEmbedder, &state);
                state.hasInstalledHooks = true;
            }
        }
    }

    /// The session a host was last attached on.
    ///
    /// - Parameter hostKey: The embedder's name for the host.
    /// - Returns: The daemon session id, or 0 when the host is unknown or its
    ///   pairing names only a name.
    [[nodiscard]] static std::uint32_t rememberedSessionID(const std::string& hostKey)
    {
        const std::optional<Session::RememberedSessions::Record> record = directoryState().remembered.recall(hostKey);
        return record.has_value() ? record->lastSessionID : 0;
    }

    /// The workspace a host was last attached under.
    ///
    /// - Parameter hostKey: The embedder's name for the host.
    /// - Returns: The name, empty for an unnamed session or an unknown host.
    [[nodiscard]] static std::string rememberedWorkspaceName(const std::string& hostKey)
    {
        const std::optional<Session::RememberedSessions::Record> record = directoryState().remembered.recall(hostKey);
        return record.has_value() ? record->lastWorkspaceName : std::string();
    }

    /// Pairs a host with the session it is attached on.
    ///
    /// - Parameters:
    ///   - hostKey: The embedder's name for the host.
    ///   - sessionID: The session now attached, as the host pinned it.
    ///   - workspaceName: The workspace it is attached under, empty for an
    ///     unnamed session.
    static void remember(const std::string& hostKey, std::uint32_t sessionID, const std::string& workspaceName)
    {
        directoryState().remembered.remember(hostKey, sessionID, workspaceName);
    }

    /// Drops a host's pairing, so the next connect decides as a first one.
    ///
    /// - Parameter hostKey: The embedder's name for the host.
    static void forget(const std::string& hostKey) { directoryState().remembered.forget(hostKey); }

    /// The key the deploy-trust list is kept under, handed to the trust
    /// persistence's Load and Store in place of a host key.
    ///
    /// Core's name rather than the embedder's, so every frontend that keeps
    /// the list keeps it under one name and none of them reads it.
    static constexpr const char* deployTrustStoreKey = "UBDeployTrust";

    /// Installs the embedder's storage for the hosts trusted to receive a
    /// `ubiquitty-host` push.
    ///
    /// The same hook shape as ``setPersistence``, over one record: the whole
    /// list under ``deployTrustStoreKey``. Until this is called every host is
    /// untrusted and every trust is dropped, so a frontend with nowhere to keep
    /// the list asks before every push rather than pushing unasked.
    ///
    /// - Parameters:
    ///   - load: Reads the list's bytes.
    ///   - store: Writes them; zero bytes trust nobody.
    ///   - context: Handed to both, borrowed on ``setPersistence``'s terms.
    static void setDeployTrustPersistence(Load load, Store store, void* context)
    {
        State& state = directoryState();
        const std::lock_guard<std::mutex> guard(state.mutex);
        state.trustLease = std::make_shared<const Lease>(load, store, context);
    }

    /// Whether the person has trusted a host to receive a push.
    ///
    /// The `hostAlreadyTrusted` input of ``Session::decideDeployAction``.
    ///
    /// - Parameter sshHost: The ssh destination a push targets.
    /// - Returns: Whether it is in the trusted list.
    [[nodiscard]] static bool isDeployTrusted(const std::string& sshHost)
    {
        if (sshHost.empty())
            return false;
        const std::vector<std::string> hosts = deployTrustedHosts();
        return std::find(hosts.begin(), hosts.end(), sshHost) != hosts.end();
    }

    /// Records the person's consent to push to a host, so the next push does
    /// not ask.
    ///
    /// - Parameter sshHost: The ssh destination; empty trusts nothing.
    static void trustDeploy(const std::string& sshHost)
    {
        if (sshHost.empty())
            return;
        std::vector<std::string> hosts = deployTrustedHosts();
        if (std::find(hosts.begin(), hosts.end(), sshHost) != hosts.end())
            return;
        hosts.push_back(sshHost);
        storeDeployTrust(hosts);
    }

    /// Withdraws the consent to push to a host, so the next push asks again.
    ///
    /// - Parameter sshHost: The ssh destination.
    static void revokeDeployTrust(const std::string& sshHost)
    {
        std::vector<std::string> hosts = deployTrustedHosts();
        const auto removed = std::remove(hosts.begin(), hosts.end(), sshHost);
        if (removed == hosts.end())
            return;
        hosts.erase(removed, hosts.end());
        storeDeployTrust(hosts);
    }

    /// Every host trusted to receive a push, in the order trusted.
    ///
    /// - Returns: The hosts; empty when nothing is stored or the stored bytes
    ///   are not a list this build reads.
    [[nodiscard]] static std::vector<std::string> deployTrustedHosts()
    {
        const std::shared_ptr<const Lease> lease = snapshotTrustLease();
        if (!lease || lease->load == nullptr)
            return {};
        // One inline probe answers every list a person builds by hand; a
        // longer one is read again into a buffer of the length the store
        // answered, bounded so no store-controlled length sizes an allocation
        // past what a list can hold.
        std::array<std::uint8_t, 4096> inlineBuffer {};
        const std::size_t length = lease->load(lease->context, deployTrustStoreKey, inlineBuffer.data(), inlineBuffer.size());
        if (length == 0 || length > maxStoredTrustLength)
            return {};
        std::string bytes;
        if (length <= inlineBuffer.size()) {
            bytes.assign(reinterpret_cast<const char*>(inlineBuffer.data()), length);
        } else {
            std::vector<std::uint8_t> buffer(length);
            if (lease->load(lease->context, deployTrustStoreKey, buffer.data(), buffer.size()) != length)
                return {};
            bytes.assign(reinterpret_cast<const char*>(buffer.data()), buffer.size());
        }
        return Session::decodeDeployTrust(bytes).value_or(std::vector<std::string> {});
    }

    /// The connect to make, over the host's tree and the host's own record.
    ///
    /// - Parameters:
    ///   - hostKey: The embedder's name for the host, whose record is recalled
    ///     through the installed persistence.
    ///   - predefinedWorkspaceName: The name the saved host carries, empty when
    ///     it names none.
    ///   - listed: The host's tree, held fresh; null when the host has not
    ///     answered or what is held has gone stale.
    ///   - chosenSessionID: The listed session the human selected, or 0 for
    ///     none.
    /// - Returns: The connect to make.
    [[nodiscard]] static Session::AttachDecision decide(const std::string& hostKey, const std::string& predefinedWorkspaceName, const Session::SessionTree* listed, std::uint32_t chosenSessionID = 0)
    {
        return Session::RememberedSessions::decide(directoryState().remembered.recall(hostKey), predefinedWorkspaceName, listed, chosenSessionID);
    }

    /// The connect to make before the host has answered, over the host's
    /// own record alone.
    ///
    /// The rule over no tree: a predefined name is created under, a
    /// remembered session is returned to, and otherwise the listing is
    /// waited for. Never read as an empty host.
    ///
    /// - Parameters:
    ///   - hostKey: The embedder's name for the host.
    ///   - predefinedWorkspaceName: The name the saved host carries, empty when
    ///     it names none.
    /// - Returns: The connect to make.
    [[nodiscard]] static Session::AttachDecision decideBeforeListing(const std::string& hostKey, const std::string& predefinedWorkspaceName)
    {
        return decide(hostKey, predefinedWorkspaceName, nullptr);
    }

private:
    /// One installed persistence: the embedder's hooks plus the context they
    /// take, published and read as a single owned unit.
    ///
    /// A recall that read `load` and `context` as two fields could invoke a
    /// callback paired with the wrong context when a replacement landed
    /// between the reads. Readers snapshot this whole lease and run the pair
    /// they took, so replacement changes what the NEXT operation sees and
    /// never what an in-flight one invokes.
    struct Lease {
        Load load = nullptr;
        Store store = nullptr;
        void* context = nullptr;
    };

    /// The one directory's parts.
    ///
    /// Reentrancy contract, read before touching: the mutex guards FIELDS
    /// only — publishing a lease.
    /// No embedder closure ever runs under it: every public member snapshots
    /// what it needs, unlocks, and only then invokes. An embedder closure
    /// that calls back into this directory (recall, remember, decide, or a
    /// further replacement) therefore takes a free lock, and an in-flight
    /// operation keeps the lease it snapshotted rather than the newest one.
    struct State {
        /// The memory the rule recalls through. Hooked once, to the
        /// lease-carrying thunks below, so its own hook fields never tear.
        Session::RememberedSessions remembered;
        /// Whether ``remembered`` is hooked yet.
        bool hasInstalledHooks = false;
        /// The current persistence lease; replaced whole on every install.
        std::shared_ptr<const Lease> lease;
        /// The deploy-trust persistence lease, published and read the same
        /// way as ``lease``.
        std::shared_ptr<const Lease> trustLease;
        /// Guards ``lease`` and ``hasInstalledHooks``. Never held across an
        /// embedder call.
        std::mutex mutex;
    };

    /// The process's directory.
    ///
    /// One instance, because a pairing read through a second copy would answer
    /// from a staler store than the one the last connect wrote.
    ///
    /// - Returns: The directory state, built on first use.
    static State& directoryState()
    {
        static State state;
        return state;
    }

    /// The persistence lease one operation runs on.
    ///
    /// Snapshotted under the state lock and invoked after it is released, so
    /// the callback/context pair an operation runs is always the pair one
    /// install published — never a mix of two.
    ///
    /// - Returns: The current lease (null hooks when nothing is installed).
    [[nodiscard]] static std::shared_ptr<const Lease> snapshotLease()
    {
        State& state = directoryState();
        const std::lock_guard<std::mutex> guard(state.mutex);
        return state.lease;
    }

    /// The deploy-trust lease one operation runs on, snapshotted like
    /// ``snapshotLease``.
    ///
    /// - Returns: The current trust lease (null hooks when nothing is
    ///   installed).
    [[nodiscard]] static std::shared_ptr<const Lease> snapshotTrustLease()
    {
        State& state = directoryState();
        const std::lock_guard<std::mutex> guard(state.mutex);
        return state.trustLease;
    }

    /// Writes the trusted list through the embedder's store.
    ///
    /// A read-modify-write over the embedder's store with no lock across it,
    /// because no directory lock is ever held across an embedder call: two
    /// threads trusting at once can drop one of the two. Trust is granted from
    /// a dialog on the main thread, so the race has no caller.
    ///
    /// - Parameter hosts: The whole list.
    static void storeDeployTrust(const std::vector<std::string>& hosts)
    {
        const std::shared_ptr<const Lease> lease = snapshotTrustLease();
        if (!lease || lease->store == nullptr)
            return;
        const std::string bytes = hosts.empty() ? std::string() : Session::encodeDeployTrust(hosts);
        lease->store(lease->context, deployTrustStoreKey, reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
    }

    /// The longest trust list a store may answer: every host the list holds
    /// at the longest destination ssh is handed, plus the version byte.
    static constexpr std::size_t maxStoredTrustLength = 1 + Session::maxDeployTrustedHosts * (2 + 1024);

    /// The buffer a recall offers the embedder.
    ///
    /// Sized for the largest record the rule can read — version, id, length
    /// and a full-bound workspace name — so every recall is ONE embedder call.
    /// One call matters more than saving stack: a frontend that resolves its
    /// store per call (rather than per recall) would serve the probe and the
    /// sized read from different installations, and a record that changed
    /// between two calls is refused rather than read as half of each. A
    /// length the store answers past this is not a record and is refused
    /// without a second call, so no callback-controlled length sizes an
    /// allocation.
    static constexpr std::size_t maxStoredRecordLength = 1 + 4 + 2 + Protocol::Sync::maxWorkspaceNameLength;

    /// ``Session::RememberedSessions::Load`` over the embedder's buffer-filling
    /// one.
    ///
    /// - Parameters:
    ///   - context: The directory's own state.
    ///   - hostKey: The host being recalled.
    ///   - bytesOut: Receives the stored bytes.
    /// - Returns: Whether the host has a record this read whole.
    static bool loadThroughEmbedder(void* context, const std::string& hostKey, std::string& bytesOut)
    {
        (void)context; // the rule's hook context is this state; the lease below is what varies
        const std::shared_ptr<const Lease> lease = snapshotLease();
        if (!lease || lease->load == nullptr)
            return false;
        std::array<std::uint8_t, maxStoredRecordLength> inlineBuffer {};
        const std::size_t length = lease->load(lease->context, hostKey.c_str(), inlineBuffer.data(), inlineBuffer.size());
        if (length == 0 || length > inlineBuffer.size())
            return false;
        bytesOut.assign(reinterpret_cast<const char*>(inlineBuffer.data()), length);
        return true;
    }

    /// ``Session::RememberedSessions::Store`` over the embedder's.
    ///
    /// - Parameters:
    ///   - context: The directory's own state.
    ///   - hostKey: The host being remembered.
    ///   - bytes: The record, or empty to forget the host.
    static void storeThroughEmbedder(void* context, const std::string& hostKey, const std::string& bytes)
    {
        (void)context; // as above: the lease varies, the rule's hook context does not
        const std::shared_ptr<const Lease> lease = snapshotLease();
        if (!lease || lease->store == nullptr)
            return;
        lease->store(lease->context, hostKey.c_str(),
            reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
    }
};

} // namespace Ubiquitty::Runtime
