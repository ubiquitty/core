// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// Source/UbiquittyCore/UbiquittyCore.h — the libUbiquittyCore public umbrella.
//
// The supported surface for EXTERNAL consumers of the portable terminal Core.
// The in-tree app is the reference implementation and reaches Core/Rendering/
// Protocol internals directly (it co-evolves with them) — it is not itself a
// conformance example for this umbrella. An external embedder includes this one
// header, links the
// `ubiquitty-core` static library, supplies a PAL implementation, and has a
// full VT/xterm terminal model: feed bytes, read the cell grid, drive the
// paint model — with no AppKit / GTK / Metal / windowing dependency.
//
// Public vs. internal
// -------------------
// The library compiles ~100 portable translation units and ships ~120 headers,
// but only the small set re-exported below is the SUPPORTED API. Everything
// else under Source/Core, Source/Rendering, … is an internal implementation
// detail: it may change without a version bump. Include this header, not the
// individual module headers — see Docs/libubiquittycore.md for the contract and
// Source/UbiquittyCore/Version.h for the semver policy.
//
// Minimal consumer shape (see Samples/core-consumer/main.cpp for the full one):
//
//   #include "UbiquittyCore/UbiquittyCore.h"
//   Ubiquitty::Terminal t;
//   Ubiquitty::resize(&t, 24, 80, ...);
//   Ubiquitty::processBytes(&t, std::string_view(bytes, static_cast<std::size_t>(length)));
//   Ubiquitty::Cell row[80];
//   Ubiquitty::getDisplayRow(&t, 0 /*viewOffset*/, 0 /*row*/, row, 80);
//
// The caller must implement the PAL "system services" half — the free
// functions in namespace Ubiquitty::PAL declared in PAL/PAL.h that are not
// provided by the library's bundled POSIX backend (Platform/POSIX/SystemPAL.cpp):
// monotonicTicks, monotonicMicros, diagWrite, bellRing, clipboardSetText,
// clipboardGetText, preferencesLoad, preferencesSaveAtomic. The bundled POSIX
// backend already supplies the storage / login-shell / spawn down-functions.
//

// ---- Ubiquitty::Terminal model: lifecycle, byte ingest, grid + snapshot read ----------
//
// The heart of the public surface, named part by part. An umbrella exists to
// spare a consumer the assembly, so it lists what it publishes rather than
// relying on one header to drag the rest along — a re-export nobody declared is
// how a consumer comes to depend on a header that was only ever passing through.
#include "Core/Screen/Addressing.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

// ---- Key encoding: an event, the key's identity, the encoder, the flags ----
//
// A frontend's other half. The model above reads bytes into a grid; this turns
// a key event back into the bytes a shell reads, and a key event becomes bytes
// only through this encoder — the rule encodeMouseReport states for the
// pointer, which this model already publishes. Published as a set because the
// pieces need each other: the encoder takes the event, the event names its key
// through VirtualKey, and the encoder consults the Kitty flags.
#include "Core/Keys/KeyEvent.h"
#include "Core/Keys/KeyTranslation.h"
#include "Core/Keys/Keymap.h"
#include "Core/Kitty/KeyboardFlags.h"

// ---- Cell / pen value types (named explicitly for the doc, already pulled) --
#include "Core/Cell.h"
#include "Core/Pen.h"
#include "Core/TermPalette.h"

// ---- Paint / render model --------------------------------------------------
//
// CellGrid: the single owner of the cell model + back-buffer shadow + paint
// session. RenderPlan: the portable per-frame damage model. CellInstance: the
// byte-pinned GPU-instance POD a backend uploads. CellInstanceBuilder: the
// portable Cell -> instance build the software / Metal / Vulkan backends share.
#include "Core/CellGrid.h"
#include "Core/Rendering/CellInstance.h"
#include "Core/Rendering/CellInstanceBuilder.h"
#include "Core/Rendering/RenderPlan.h"
#include "Core/Rendering/SoftwareRenderer.h"

// ---- The PAL contract the caller implements --------------------------------
//
// PAL.h: the system-services + hook seam. Stream.h: the byte-transport object
// (a PTY/serial/socket carrier the caller may open). TerminalClients.h: the
// per-Ubiquitty::Terminal frontend clients Core invokes for irreducible OS actions.
// TaskQueue.h: the submit/drain job seam.
#include "PAL/PAL.h"
#include "PAL/Stream.h"
#include "PAL/TaskQueue.h"
#include "PAL/TerminalClients.h"

// ---- Library version stamp -------------------------------------------------
#include "UbiquittyCore/Version.h"
