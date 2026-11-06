// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// What every compiler building portable Core must provide, asserted rather than
// described.
//
// Base/Types.h includes this, and most portable translation units reach
// Types.h, so a build on a toolchain below the floor names what is missing
// instead of only failing three hundred lines further in at the first use of a
// facility the author had no reason to doubt. Scripts/cmake/ToolchainFloor.cmake
// states the same floor as compiler versions, and
// Scripts/cmake/UbiquittyToolchain.cmake refuses a compiler below it at
// configure time, with the remedy; this header is what holds for a build that
// never ran that configure — a bash driver, an Xcode target, a Swift module
// importing Core.
//
// A portable Core file is written once and built by three toolchains: Apple
// clang, emscripten, and the GCC inside a Linux container. The oldest of them
// decides what the other two are allowed to use.
//
// There are two such containers and they are declared together in
// `Scripts/linux-images.txt`, both pinned by digest: `alpine:3.22` (g++ 14.2,
// the static-musl host) and `ubuntu:26.04` (g++ 15.2, the GTK frontend). Alpine
// is the floor and sits exactly at the requirement below, with no headroom.
//
// Pinning by digest is what makes that statement hold for longer than a day. A
// tag pins a distribution and not a compiler, so two machines rebuilding a week
// apart could get different GCCs from an identical recipe — and `run-linux.sh`
// keys its cache on a hash of its own Dockerfile text, which cannot see the base
// image moving underneath it.
//
// This is the "assert what we require" form and not "record what we have": a
// recorded version drifts silently upward as machines are upgraded, and a floor
// that rises whenever someone rebuilds is not a floor.

#include <version>

// C++23. `if consteval`, deducing this and the ranges additions are all in use
// in portable Core; a compiler offering less cannot build it.
//
// GCC 13 is what this rejects in practice. It reports 202100L whatever -std it
// is given and has no deducing this, which Base/WeakPtr.h uses; GCC stamps
// 202302L from 14. Accepting 202100L would admit a compiler that does not claim
// full C++23, which is a portability decision about what portable Core may use
// rather than an edit. `Scripts/linux-images.txt` is where that decision is
// recorded, and it is not taken here.
#if !defined(__cplusplus) || __cplusplus < 202302L
#error "Ubiquitty portable Core requires C++23: GCC 14 or newer, Clang 19 or newer, or Apple clang, with -std=c++23. This compiler reports an older standard; configuring a fresh CMake build directory names the GCC or Clang to install and the CC and CXX to set."
#endif

// std::expected, the youngest library facility portable Core depends on and
// therefore the one that sets the library floor. <print> is younger and is not
// used: std::print throws std::system_error when the write fails, and every
// binary here is built -fno-exceptions, so PAL::logLine formats with std::format
// and writes with one fwrite instead.
//
// Asked through the standard's feature-test macro rather than by header
// presence, because presence answers a different question. libstdc++ ships
// <expected> to every compiler that uses it and defines the template only when
// the compiler's concepts support clears its gate, `__cpp_concepts` of 202002L.
// Clang 18 reports 201907L, so against libstdc++ it finds the header, finds it
// empty, and fails at the first `std::expected` in Core with "no template named
// 'expected'". `__cpp_lib_expected` is defined exactly when the template is, and
// it is one macro across all three standard libraries rather than a version
// table per implementation.
//
// Two containers must clear this, and the message names both because the one
// that fails is not the one a reader assumes: `Scripts/run-linux.sh` builds and
// tests portable Core on Linux, and `Scripts/build-host-linux.sh` builds the
// static-musl ubiquitty-host that ships inside the macOS app, in a different
// image.
#if !defined(__cpp_lib_expected) || __cpp_lib_expected < 202202L
#error "Ubiquitty portable Core requires std::expected. The toolchain building this is below the floor (GCC 14 or newer, or Clang 19 or newer against libstdc++); check the compiler in whichever container this build runs in (Scripts/run-linux.sh and Scripts/build-host-linux.sh use different ones)."
#endif

// Not required, and stated so nobody reaches for it: <mdspan> is absent from the
// container's libstdc++ (GCC 15.2) while Apple clang and emscripten both have
// it. Two of three arms offering a facility is what makes it dangerous — the
// author's build succeeds and the arm nobody runs locally is the one that fails.
