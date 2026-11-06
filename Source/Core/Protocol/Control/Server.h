// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
//
// The control socket's server: a unix-domain socket speaking newline-delimited
// JSON-RPC 2.0 that drives a running frontend. Off unless `[control] enable`.
//
// Everything that needs no window server lives here: the socket path, the
// owner-only bind, the peer-uid check, the token, the agent policy and its
// audit, the method list, and the screening of every verb that types, reads or
// creates. What a verb does to a window is the frontend's, reached through
// `HostOperations`, a C-ABI table a Swift host fills with `@convention(c)`
// functions. The host never sees a request it was not screened for: the gate
// runs here, with the window and pane ids the host resolved, before the verb is
// handed over.
//
// The methods a client may call, their parameter and result schemas, who may
// call each, and how the socket serves it are the method registry's
// (Core/Protocol/Control/MethodRegistry.h): the dispatcher looks a request's
// method up there and follows the entry's `ControlRoute`, never comparing the
// method's name with one of its own. Every request is validated against its
// entry's schema before any effect, and every error answers in the one shape
// `rpcError` builds. The host verbs are the host-routed methods' window-server
// halves, named as the registry names the method, plus `target`, which
// resolves a request's window and pane to the ids `ls` reports before a gated
// verb runs.
// A host reply is `{"result": <any>}` or `{"error": "<message>"}`.

#include "Base/SwiftInterop.h"
#include "Core/Codec/JSON.h"
#include "Core/Protocol/Control/MethodRegistry.h"
#include "Core/Protocol/ControlRPC.h"
#include "Core/Security/AgentPolicy.h"
#include "Core/Security/InjectionGate.h"

#include <atomic>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>

namespace Ubiquitty::Preferences {
struct Values;
}

namespace Ubiquitty::Session {
class BoundedSocketServer;
}

namespace Ubiquitty::Protocol::Control {
class Server;
}

/// Claims a reference to `server` on Swift's behalf.
///
/// Declared at global scope because the Swift importer resolves the retain and
/// release named by `UBIQUITTY_SHARED_REFERENCE` there and nowhere else.
///
/// - Parameter server: The server to retain. A null pointer is ignored.
void retainControlServer(Ubiquitty::Protocol::Control::Server* server);

/// Drops a reference to `server`, stopping and destroying it at zero.
///
/// - Parameter server: The server to release. A null pointer is ignored.
void releaseControlServer(Ubiquitty::Protocol::Control::Server* server);

namespace Ubiquitty::Protocol::Control {

// The seam's function types have C language linkage: a Swift host fills them
// with `@convention(c)` functions, and the C ABI has no span to pass.
extern "C" {

/// Receives one host verb's reply as UTF-8 JSON text.
///
/// The bytes are borrowed for the call; the receiver copies what it keeps.
using HostReply = void (*)(void* replyContext, const char* json, std::size_t length);

/// Performs one host verb and hands the reply to `reply`.
///
/// Called on the server's socket threads, possibly several at once; the host
/// marshals to whatever thread owns its windows and answers before returning.
/// Every string is UTF-8 with an explicit length.
using HostPerform = void (*)(void* context, const char* verb, std::size_t verbLength, const char* parametersJSON, std::size_t parametersLength, HostReply reply, void* replyContext);
}

/// The window-server half of the control socket, supplied by a frontend.
///
/// A plain table of C function pointers so a Swift host can fill it with
/// `@convention(c)` closures and an unmanaged `context`, and no `std::string`
/// or `std::function` crosses the seam.
struct HostOperations {
    /// The host's own state, handed back to `perform` unchanged.
    void* context = nullptr;

    /// Performs one host verb; see ``HostPerform``.
    HostPerform perform = nullptr;
};

/// What a server binds and how it screens, resolved from the preferences or
/// written by a test.
struct Configuration {
    /// The socket's path. Must fit a `sockaddr_un`.
    std::string socketPath;
    /// The shared secret every request must carry; empty disables the check.
    std::string token;
    /// Whether a `confirm` verdict counts as approved: `[control] approve`.
    Security::ApprovalMode approval = Security::ApprovalMode::RequireConfirmation;
};

/// Decodes one request line, screens it, and runs it through a host.
///
/// Holds no socket and no thread, so a test drives the whole method surface
/// line by line. Immutable once built: the socket's client threads share one.
class Dispatcher {
public:
    /// Creates a dispatcher over a host.
    ///
    /// - Parameters:
    ///   - token: The shared secret; empty disables the check.
    ///   - policy: The rule set every gated verb is held to.
    ///   - approval: What a `confirm` verdict becomes.
    ///   - host: The window-server half.
    ///   - registry: The operations served, each as its entry routes it; a
    ///     test passes its own. Borrowed for the dispatcher's lifetime.
    Dispatcher(std::string token, Security::AgentPolicy policy, Security::ApprovalMode approval, HostOperations host, std::span<const MethodDescriptor> registry = methodRegistry());

    /// Answers one request line.
    ///
    /// - Parameter line: One JSON-RPC request, without its newline.
    /// - Returns: The serialized response, or empty for a blank line or a
    ///   notification.
    [[nodiscard]] std::string handleLine(std::string_view line) const;

private:
    /// The outcome of one host verb: a result, or an error message and the
    /// code it is reported under.
    struct HostAnswer {
        Codec::JSON::Value result;
        std::string error;
        ErrorCode code = ErrorCode::InvalidParameters;
    };

    [[nodiscard]] Codec::JSON::Value dispatch(const std::string& method, const Codec::JSON::Value& request, const Codec::JSON::Value* id) const;
    [[nodiscard]] HostAnswer perform(std::string_view verb, const Codec::JSON::Value& parameters) const;

    std::string m_token;
    Security::AgentPolicy m_policy;
    Security::ApprovalMode m_approval;
    HostOperations m_host;
    std::span<const MethodDescriptor> m_registry;
};

/// The listening control socket and the thread that serves it.
///
/// A shared reference for Swift: the frontend holds it, and the last release
/// stops the listener, closes every client, joins the threads and removes the
/// socket file. Releasing it blocks until in-flight requests finish, so a host
/// that marshals onto its main thread must not drop the last reference there
/// while a request may be waiting for that thread.
class Server {
public:
    /// Starts the server the preferences ask for.
    ///
    /// - Parameters:
    ///   - values: The preferences; `[control] enable`, `socket_path`, `token`
    ///     and `approve` are read.
    ///   - host: The window-server half.
    /// - Returns: A listening server with one reference, or null when the
    ///   socket is disabled or could not be bound.
    [[nodiscard]] static Server* startIfEnabled(const Preferences::Values& values, HostOperations host) UBIQUITTY_RETURNS_RETAINED;

    /// Binds `configuration.socketPath` and starts serving it.
    ///
    /// The agent policy is the default rule file, read once here as the agent
    /// reads it once at launch.
    ///
    /// - Parameters:
    ///   - configuration: The path, token and approval mode.
    ///   - host: The window-server half.
    /// - Returns: A listening server with one reference, or null when the
    ///   path cannot be bound.
    [[nodiscard]] static Server* start(const Configuration& configuration, HostOperations host) UBIQUITTY_RETURNS_RETAINED;

    /// Stops the server; see ``stop()``.
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    /// Stops listening, closes every client, waits for the serving thread, and
    /// removes the socket file. Idempotent.
    void stop();

    /// The path the server is bound to.
    [[nodiscard]] std::string socketPath() const { return m_socketPath; }

    /// Answers one request line exactly as a socket client would be answered.
    ///
    /// - Parameter line: One JSON-RPC request, without its newline.
    /// - Returns: The serialized response, or empty when none is owed.
    [[nodiscard]] std::string handleLine(std::string_view line) const { return m_dispatcher.handleLine(line); }

    /// The reference count Swift manipulates through the global retain and
    /// release. Public because those are free functions.
    std::atomic<int> swiftReferences { 1 };

private:
    /// Creates a server that answers through `dispatcher` once it is given a
    /// listener.
    ///
    /// Private because the type is a shared reference: a stack instance would
    /// be released as though it were heap memory the first time Swift saw it.
    ///
    /// - Parameters:
    ///   - socketPath: The path bound, removed again when the server stops.
    ///   - dispatcher: What answers each request line.
    Server(std::string socketPath, Dispatcher dispatcher);

    std::string m_socketPath;
    Dispatcher m_dispatcher;
    std::unique_ptr<Session::BoundedSocketServer> m_boundedServer;
    std::thread m_thread;
} UBIQUITTY_SHARED_REFERENCE(retainControlServer, releaseControlServer);

} // namespace Ubiquitty::Protocol::Control
