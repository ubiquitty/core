// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// Source/Base/SwiftInterop.h — Ubiquitty-namespaced Swift/C++ interop
// annotations. Portable C++ headers consumed by the Swift importer (via
// -cxx-interoperability-mode=default) annotate their types with these
// UBIQUITTY_* macros to control how Swift imports them: ownership (~Copyable),
// escapability, reference vs value semantics, naming.
//
// <swift/bridging> ships with the Swift toolchain and already stubs its own
// SWIFT_* macros to nothing when the underlying swift_attr attribute is
// unavailable; it is ABSENT on the Linux toolchain.
// This header guards the include (defending compilers that lack __has_include)
// and aliases the UBIQUITTY_* names to the toolchain macros when present, else to
// no-ops. Result: a portable header carries the annotation on an Apple build
// and compiles untouched everywhere else. The annotations are no-ops for the
// C++ compiler regardless of platform — only the Swift importer reads them, so
// nothing here pulls a platform dependency into Core.
//
// Use the UBIQUITTY_* names (not the raw SWIFT_* macros) so our headers don't
// bind to toolchain spellings and a non-Apple reader sees one obvious no-op.
//

#if defined(__has_include)
#if __has_include(<swift/bridging>)
#include <swift/bridging>
#endif
#endif

// ~Copyable: a uniquely-owned type the Swift importer must not silently copy
// (e.g. a heavy value type with std::vector / std::string members held by a
// single Swift owner). C++ semantics are unchanged.
#if defined(SWIFT_NONCOPYABLE)
#define UBIQUITTY_NONCOPYABLE SWIFT_NONCOPYABLE
#else
#define UBIQUITTY_NONCOPYABLE
#endif

// ~Escapable: a borrowed view that must not outlive its storage (Span-like).
#if defined(SWIFT_NONESCAPABLE)
#define UBIQUITTY_NONESCAPABLE SWIFT_NONESCAPABLE
#else
#define UBIQUITTY_NONESCAPABLE
#endif

// The returned view borrows the annotated argument or the method's receiver.
#if defined(__has_cpp_attribute)
#if __has_cpp_attribute(clang::lifetimebound)
#define UBIQUITTY_LIFETIMEBOUND [[clang::lifetimebound]]
#else
#define UBIQUITTY_LIFETIMEBOUND
#endif
#else
#define UBIQUITTY_LIFETIMEBOUND
#endif

// Escapable: an independent value with no borrowed lifetime. Plain scalar
// aggregates are escapable (hence safe) by default; the explicit tag GUARANTEES
// the importer keeps treating one as safe even if a field is added later.
#if defined(SWIFT_ESCAPABLE)
#define UBIQUITTY_ESCAPABLE SWIFT_ESCAPABLE
#else
#define UBIQUITTY_ESCAPABLE
#endif

// Foreign reference type: Swift manages the C++ object by reference, retaining
// / releasing through the named C functions instead of copying it by value.
#if defined(SWIFT_SHARED_REFERENCE)
#define UBIQUITTY_SHARED_REFERENCE(_retain, _release) SWIFT_SHARED_REFERENCE(_retain, _release)
#else
#define UBIQUITTY_SHARED_REFERENCE(_retain, _release)
#endif

// Returns a reference the caller already owns: Swift consumes the +1 rather
// than retaining again. What a factory for a foreign reference type returns.
//
// A factory with neither this nor its unretained counterpart is not a default,
// it is a diagnostic — the importer cannot infer which convention a function
// follows and says so. Guessing wrong leaks the object or releases one the
// caller still holds, and both survive every test that only draws a frame.
#if defined(SWIFT_RETURNS_RETAINED)
#define UBIQUITTY_RETURNS_RETAINED SWIFT_RETURNS_RETAINED
#else
#define UBIQUITTY_RETURNS_RETAINED
#endif

// Returns a reference the callee keeps owning: Swift retains it before use.
// What an accessor handing back a long-lived object returns.
#if defined(SWIFT_RETURNS_UNRETAINED)
#define UBIQUITTY_RETURNS_UNRETAINED SWIFT_RETURNS_UNRETAINED
#else
#define UBIQUITTY_RETURNS_UNRETAINED
#endif

// Unsafe reference: Swift imports the type as a class whose lifetime it does not
// manage. No retain, no release, no copying — the C++ code that owns the object
// keeps owning it, and Swift only ever borrows a pointer.
//
// For a type a frontend holds elsewhere and merely hands across the seam. Unlike
// UBIQUITTY_SHARED_REFERENCE it adds no reference count, so it suits an object
// with one owner and many borrowers. Unlike the value tags it does not require
// the type to be copyable or movable, which is what makes it the only way to
// import a class that deletes all four operations.
#if defined(SWIFT_UNSAFE_REFERENCE)
#define UBIQUITTY_UNSAFE_REFERENCE SWIFT_UNSAFE_REFERENCE
#else
#define UBIQUITTY_UNSAFE_REFERENCE
#endif

// Rename a C++ declaration as the Swift importer sees it.
#if defined(SWIFT_NAME)
#define UBIQUITTY_SWIFT_NAME(_name) SWIFT_NAME(_name)
#else
#define UBIQUITTY_SWIFT_NAME(_name)
#endif

// A span/view parameter the callee reads only for the call's duration. On a
// std::span parameter this is what makes the safe interop generate the Swift
// `Span` overload — without it Swift sees only the unsafe form and every
// caller pays a pointer ceremony the annotation exists to delete.
//
// Gated to Apple builds beside the attribute check: only the Apple
// toolchain's importer reads the annotation, and a non-Apple clang that has
// the attribute still diagnoses it on a by-value span parameter under
// -Werror — the attribute is specified for pointer parameters, and the
// span-parameter reading is the interop extension.
#if defined(__APPLE__) && defined(__has_attribute)
#if __has_attribute(noescape)
#define UBIQUITTY_NOESCAPE __attribute__((noescape))
#else
#define UBIQUITTY_NOESCAPE
#endif
#else
#define UBIQUITTY_NOESCAPE
#endif
