// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Source/Base/ThreadSafety.h — the attributes Clang's thread-safety analysis
// (-Wthread-safety) and function-effect analysis (-Wfunction-effects) read.
//
// Each macro expands to its Clang attribute under Clang and to nothing
// elsewhere: GCC rejects the capability attributes as unknown, which this
// tree's -Werror makes fatal. The analyses are compile-time only; no macro here
// changes code generation.
//
// The capability macros follow Clang's documented vocabulary
// (https://clang.llvm.org/docs/ThreadSafetyAnalysis.html). The analysis
// compares capability expressions as written, so a capability reached through
// two different expressions is two capabilities to it; a lock reached through
// several pointers is best named by one immutable object every acquisition
// grants (see ``Ubiquitty::modelLockRole``).

#if defined(__clang__)
#define UBIQUITTY_THREAD_ANNOTATION(x) __attribute__((x))
#else
#define UBIQUITTY_THREAD_ANNOTATION(x)
#endif

/// Marks a class whose objects name a capability, such as a lock or a role.
#define UBIQUITTY_CAPABILITY(name) UBIQUITTY_THREAD_ANNOTATION(capability(name))

/// Marks an RAII class that holds a capability from its constructor to its
/// destructor.
#define UBIQUITTY_SCOPED_CAPABILITY UBIQUITTY_THREAD_ANNOTATION(scoped_lockable)

/// Marks a data member that may be read or written only while the capability
/// is held.
#define UBIQUITTY_GUARDED_BY(x) UBIQUITTY_THREAD_ANNOTATION(guarded_by(x))

/// Marks a pointer member whose pointee may be read or written only while the
/// capability is held; the pointer itself is unguarded.
#define UBIQUITTY_POINTEE_GUARDED_BY(x) UBIQUITTY_THREAD_ANNOTATION(pt_guarded_by(x))

/// Marks a function its caller may call only while holding the capability.
#define UBIQUITTY_REQUIRES(...) UBIQUITTY_THREAD_ANNOTATION(requires_capability(__VA_ARGS__))

/// Marks a function that returns holding the capability it did not hold on
/// entry.
#define UBIQUITTY_ACQUIRES(...) UBIQUITTY_THREAD_ANNOTATION(acquire_capability(__VA_ARGS__))

/// Marks a function that holds the capability on return exactly when it
/// returns the given value.
#define UBIQUITTY_TRY_ACQUIRES(...) UBIQUITTY_THREAD_ANNOTATION(try_acquire_capability(__VA_ARGS__))

/// Marks a function that returns no longer holding the capability it held on
/// entry; on a scoped capability's destructor, the capability its constructor
/// acquired.
#define UBIQUITTY_RELEASES(...) UBIQUITTY_THREAD_ANNOTATION(release_capability(__VA_ARGS__))

/// Marks a function its caller must not call while holding the capability.
#define UBIQUITTY_EXCLUDES(...) UBIQUITTY_THREAD_ANNOTATION(locks_excluded(__VA_ARGS__))

/// Marks a function that returns only when the calling thread holds the
/// capability, so the analysis takes it as held after the call. The analysis
/// takes the function's word for it; Scripts/check-model-lock-holds.py counts
/// every one outside the files that implement the model lock.
#define UBIQUITTY_ASSERTS_CAPABILITY(x) UBIQUITTY_THREAD_ANNOTATION(assert_capability(x))

/// Marks a function the thread-safety analysis does not check: the body of a
/// primitive that implements a capability, or a function that holds one by
/// means the analysis cannot follow, such as a lock held from one call to the
/// next. Each use states its reason at the declaration, and
/// Scripts/check-model-lock-holds.py counts every use outside the files that
/// implement the model lock.
#define UBIQUITTY_NO_THREAD_SAFETY_ANALYSIS UBIQUITTY_THREAD_ANNOTATION(no_thread_safety_analysis)

/// Marks a function Clang's function-effect analysis proves cannot block: it
/// takes no lock, waits on nothing, allocates nothing and calls only functions
/// that are themselves nonblocking. A frontend's per-tick calls carry it, so a
/// change that makes one wait on the parser does not compile.
#if defined(__has_cpp_attribute)
#if __has_cpp_attribute(clang::nonblocking)
#define UBIQUITTY_NONBLOCKING [[clang::nonblocking]]
#else
#define UBIQUITTY_NONBLOCKING
#endif
#else
#define UBIQUITTY_NONBLOCKING
#endif
