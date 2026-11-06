// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
//
// The one table of every operation Ubiquitty exposes to people and agents:
// the control socket's methods, `ubq`'s command-line verbs, and its
// MCP tools. Each entry carries a one-sentence description, a JSON Schema for
// its parameters and one for its result, an example that satisfies both, and
// the principals that may call it. An operation a Sync connection may call
// also names the roles that may call it and the scope the call needs, which is
// the whole of what the host checks before serving it.
//
// The surfaces read from here rather than restating it: the control socket
// validates every request's parameters against the entry's schema before any
// effect and answers `describe` and `list` from the table; the MCP server's
// `tools/list` is generated from the entries that name a tool, and a tool's
// arguments are validated the same way. The method-registry gate
// refuses a control-socket method, a command-line verb or an MCP tool that is
// dispatched without an entry, so a surface cannot grow an operation the
// registry does not describe.
//
// Entries are written one field per line, `.name = "..."`, because the gate
// reads them as text.

#include "Base/Bits/OptionSet.h"
#include "Core/Codec/JSON.h"
#include "Core/Protocol/ControlRPC.h"
#include "Core/Protocol/Sync/Roles.h"
#include "Core/Protocol/Sync/Scope.h"
#include "PAL/Capability.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace Ubiquitty::Protocol::Control {

/// Who may call an operation.
enum class Principal : std::uint8_t {
    /// A person at a prompt, answering for what they type.
    Person = 1 << 0,
    /// A process running as the same user that speaks to the control socket.
    LocalProcess = 1 << 1,
    /// An AI agent, through `ubq` or its MCP server.
    Agent = 1 << 2,
};

/// A set of principals.
using Principals = Bits::OptionSet<Principal>;

/// How the control socket serves an operation, and what the agent policy is
/// asked before it does.
///
/// The socket's dispatcher switches on this and never on a method's name, so a
/// method is served exactly as its entry routes it and an entry is the whole
/// of what a new method needs in Core. A host route hands the method to the
/// frontend under its control-method name.
enum class ControlRoute : std::uint8_t {
    /// The socket does not serve the operation.
    NotServed,
    /// The server answers with its name and the methods it serves.
    Handshake,
    /// The server lists the operations a local process may call.
    ListOperations,
    /// The server describes one operation by any name it is reached by.
    DescribeOperation,
    /// The host performs it unscreened: a raise, a title, a listing of what is
    /// already on the caller's display.
    Host,
    /// The host types the `text` parameter into the resolved pane once the
    /// policy allows the line.
    HostTypesText,
    /// The host reads the resolved pane once the policy allows a screen read.
    HostReadsScreen,
    /// The host opens a window or tab once the policy allows the method's
    /// words in angle brackets: `new-window` is screened as `<new window>`.
    HostOpens,
    /// The host splits the resolved window once the policy allows
    /// `<split right>` or `<split down>`, from the `direction` parameter.
    HostSplits,
};

/// What a Sync call is addressed to.
enum class CallTarget : std::uint8_t {
    /// A pane bound to the caller's connection, named by its handle in the
    /// `pane` parameter.
    Pane,
    /// The host itself: the call needs no pane bound to the attachment. Its
    /// parameters may name host-owned objects, including a pane by identity
    /// rather than by a handle on the caller's connection.
    Host,
    /// The calling physical connection itself; no object selector is accepted.
    Connection,
};

/// Where a caller must stand for a Sync call to be served.
enum class CallOrigin : std::uint8_t {
    /// Any client the host admitted.
    Any,
    /// Only a client the host verified as running on it as its own user. A
    /// role and a scope cannot say this: a token carrying every scope is
    /// still a token someone else may hold.
    Local,
};

/// One operation, described once for every surface that exposes it.
struct MethodDescriptor {
    /// The registry's name for the operation; unique.
    std::string_view name;
    /// One sentence in the third person saying what the operation does.
    std::string_view description;
    /// The JSON Schema every call's parameters must satisfy.
    std::string_view parametersSchema;
    /// The JSON Schema of what a successful call returns.
    std::string_view resultSchema;
    /// `{"params": ..., "result": ...}`, each satisfying its schema.
    std::string_view example;
    /// Who may call it.
    Principals principals;
    /// The control socket's method name; empty when the socket does not serve it.
    std::string_view controlMethod;
    /// How the control socket serves it; `NotServed` exactly when
    /// `controlMethod` is empty.
    ControlRoute controlRoute;
    /// The `ubq` verb; empty when the command line has none.
    std::string_view commandLineVerb;
    /// The MCP tool name; empty when the MCP server does not offer it.
    std::string_view mcpTool;
    /// The method a Sync `Call` names (Docs/sync.md §7.3); empty when a
    /// Sync connection does not serve it.
    std::string_view syncMethod {};
    /// The roles whose attachments may call it over Sync; empty exactly
    /// when `syncMethod` is.
    Sync::Roles roles {};
    /// The scope a caller's credential must carry for a Sync call, beside
    /// holding one of `roles`. `Observe` is what attaching already needs, so
    /// it asks nothing more of a caller that is attached.
    Sync::Scope scope = Sync::Scope::Observe;
    /// What a Sync call is addressed to. The host routes on this, never on
    /// whether a call happens to carry a `pane` parameter.
    CallTarget target = CallTarget::Pane;
    /// Where the caller must stand, beside holding a role and the scope.
    CallOrigin origin = CallOrigin::Any;
    /// The scopes a token must carry for `ubq` to run the verb: what every
    /// connection it opens and every call it makes needs, whatever options it
    /// is given. An option that needs more says so in its schema's
    /// description. Empty for a verb that reaches no daemon itself.
    Sync::ScopeSet commandLineScopes {};
};

/// Who is making a Sync call, as the host verified them.
struct CallerAuthority {
    /// The role the host granted the caller's attachment.
    Sync::Role role = Sync::Role::Viewer;
    /// The scopes the caller's credential carries. A local client with no
    /// token carries every scope, which the host states here rather than
    /// have each check restate the exception.
    Sync::ScopeSet scopes;
    /// Where the host verified the caller to stand.
    CapabilityOrigin origin = CapabilityOrigin::Remote;
};

/// Why a call was refused before any effect.
struct CallRefusal {
    ErrorCode code = ErrorCode::Forbidden;
    /// A sentence saying what was refused and what would have been needed.
    std::string reason;
};

/// Decides whether a Sync call is served: the one check at the operation
/// boundary, made before any effect and before any parameter is acted on.
///
/// In order: the method is one a Sync connection may call; the caller's
/// role is one the entry names; the caller stands where the entry requires;
/// the caller's credential carries the entry's scope; and the parameters fit
/// the entry's schema. The first that fails is the refusal, so a caller is
/// never told about a parameter of a method it may not call.
///
/// - Parameters:
///   - method: The entry `findSyncMethod` found, or null.
///   - parameters: The call's parameters.
///   - caller: Who is calling.
/// - Returns: Nothing when the call is served, or why it is not.
[[nodiscard]] std::optional<CallRefusal> admitCall(const MethodDescriptor* method, const Codec::JSON::Value& parameters, const CallerAuthority& caller);

/// Returns every registered operation, in the order a reader scans them.
///
/// - Returns: The table; it lives for the program's lifetime.
[[nodiscard]] std::span<const MethodDescriptor> methodRegistry();

/// Finds an operation by its registry name.
///
/// - Parameter name: A registry name.
/// - Returns: The entry, or null when none has that name.
[[nodiscard]] const MethodDescriptor* findMethod(std::string_view name);

/// Finds the operation a control-socket method name invokes.
///
/// - Parameters:
///   - method: The JSON-RPC method.
///   - registry: The table to search; a test passes its own.
/// - Returns: The entry, or null when the socket serves no such method.
[[nodiscard]] const MethodDescriptor* findControlMethod(std::string_view method, std::span<const MethodDescriptor> registry = methodRegistry());

/// Finds the operation an MCP tool name invokes.
///
/// - Parameter tool: The tool's name.
/// - Returns: The entry, or null when the server offers no such tool.
[[nodiscard]] const MethodDescriptor* findMCPTool(std::string_view tool);

/// Finds the operation a `ubq` verb invokes.
///
/// - Parameter verb: The command-line verb.
/// - Returns: The entry, or null when the command line has no such verb.
[[nodiscard]] const MethodDescriptor* findCommandLineVerb(std::string_view verb);

/// Finds the operation a Sync `Call` names.
///
/// The host serves a call only when this finds it, so the registry's Sync
/// column is the one list of what a Sync connection may call.
///
/// - Parameter method: The `Call`'s JSON-RPC method.
/// - Returns: The entry, or null when a Sync connection serves no such method.
[[nodiscard]] const MethodDescriptor* findSyncMethod(std::string_view method);

/// The surfaces an operation is reached from, each with its own namespace.
enum class Surface : std::uint8_t {
    /// The app's control socket, by control method.
    ControlSocket,
    /// The MCP server, by tool name.
    MCP,
    /// `ubq`, by verb.
    CommandLine,
    /// A Sync connection's `Call`, by its method.
    SyncCall,
};

/// Finds an operation by any of its names, the asking surface's own first.
///
/// The surfaces share words: `ls` and `split` are the control socket's
/// methods and also `ubq` verbs for other operations. A name is read first as
/// the surface's own, then as a registry name, then as another surface's, so
/// `ubq api describe split` describes `ubq split` and the control socket's
/// `describe split` describes its own `split`.
///
/// - Parameters:
///   - name: Any name an operation is reached by.
///   - surface: The surface asking, whose names win.
///   - registry: The table to search; a test passes its own.
/// - Returns: The entry, or null when nothing is reached by that name.
[[nodiscard]] const MethodDescriptor* findMethodFromSurface(std::string_view name, Surface surface, std::span<const MethodDescriptor> registry = methodRegistry());

/// Names a set of scopes as JSON, the way `api describe` and the host's
/// `host.describe` give them.
///
/// - Parameter scopes: The set.
/// - Returns: An array of the scopes' `--scope` names, lowest bit first.
[[nodiscard]] Codec::JSON::Value scopeNames(Sync::ScopeSet scopes);

/// Parses a schema or example the registry holds as text.
///
/// The texts are fixed at build time and the registry test parses every one,
/// so a parse failure is a defect in this file; it yields an empty object.
///
/// - Parameter text: One of a descriptor's JSON texts.
/// - Returns: The parsed value.
[[nodiscard]] Codec::JSON::Value parseRegistryJSON(std::string_view text);

/// Describes an operation as `describe` answers: every field, schemas parsed.
///
/// - Parameter method: The entry to describe.
/// - Returns: A JSON object.
[[nodiscard]] Codec::JSON::Value describeMethod(const MethodDescriptor& method);

/// Lists the operations a principal may call, as `list` answers.
///
/// - Parameters:
///   - principal: The caller's principal.
///   - registry: The table to list; a test passes its own.
/// - Returns: `{methods: [{name, description, controlMethod?, commandLineVerb?, mcpTool?}]}`.
[[nodiscard]] Codec::JSON::Value listMethods(Principal principal, std::span<const MethodDescriptor> registry = methodRegistry());

/// Lists the Sync methods a caller may call, as `host.methods` answers.
///
/// A method is listed when `admitCall` would not refuse the caller for its
/// role, where it stands or its scope: what it lists is what it may call.
///
/// - Parameters:
///   - caller: Who is asking.
///   - registry: The table to list; a test passes its own.
/// - Returns: `{methods: [{name, description, target, scope}]}`.
[[nodiscard]] Codec::JSON::Value listSyncMethods(const CallerAuthority& caller, std::span<const MethodDescriptor> registry = methodRegistry());

} // namespace Ubiquitty::Protocol::Control
