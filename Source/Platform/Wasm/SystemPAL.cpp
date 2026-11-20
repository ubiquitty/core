// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Source/Platform/Wasm/SystemPAL.cpp — the wasm32 half of the PAL
// "Core calls down" surface, the emscripten counterpart of Platform/POSIX/SystemPAL.cpp.
//
// wasm has no login shell, process spawn or persistent filesystem: such a build
// embeds Core for parsing and rendering, and the embedder supplies whatever
// system services it needs through the frontend half of the PAL. This TU carries
// the same symbol set as `Platform/POSIX/SystemPAL.cpp` so the portable library
// links unchanged, reporting the facilities wasm lacks as unavailable — the
// same -1 and false the POSIX backend returns on error, which Core already
// treats as a graceful no-service path.
//
// The clock / clipboard / bell / preferences half is supplied by the embedding
// frontend, exactly as on the POSIX backends — the symbol sets do not overlap.
//

#include "PAL/BoundedFile.h"
#include "PAL/DurableFile.h"
#include "PAL/PAL.h"

#include <memory>
#include <new>
#include <string>

namespace Ubiquitty {
namespace PAL {

// ---- Login shell -------------------------------------------------

// No login shell exists inside a wasm sandbox.
int systemLoginShellPath(char* /*outputBuffer*/, int /*outputCapacity*/) { return -1; }

int systemLoginUserName(char* /*outputBuffer*/, int /*outputCapacity*/) { return -1; }

std::string systemHostName() { return {}; }

// A page has no login program and no session to register: there is no utmp and
// no PAM. The launch falls back to the shell the embedder names, which is the
// only process a page ever runs.
LoginSessionKind loginSessionKind() { return LoginSessionKind::Unregistered; }

bool registerLoginSession(const char* /*ptyName*/) { return false; }

void unregisterLoginSession(const char* /*ptyName*/) { }

int loginSessionProgram(char* /*outputBuffer*/, int /*outputCapacity*/) { return -1; }

// ---- Home directory ---------------------------------------------

// No account database and no persistent home. An embedder that gives the page
// somewhere to keep state supplies it through the frontend half of the PAL,
// where it can name whatever the host actually offers.
int systemHomeDirectory(char* /*outputBuffer*/, int /*outputCapacity*/) { return -1; }

// ---- Scheduler ---------------------------------------------------

// Cooperative single-threaded runtime: there is no other thread to yield to,
// and a real yield would need ASYNCIFY. The Core drives forward regardless.
void schedulerYield() { }

// No thread priorities in a single-threaded wasm runtime, and no cores to
// choose between: the hint has nothing to raise.
void raiseCurrentThreadToInteractivePriority() { }

// One thread, and it is the one rendering: there is no background band.
void lowerCurrentThreadToUtilityPriority() { }

namespace {

// One thread: nothing can contend, so the lock records nothing.
class SingleThreadLock final : public PriorityInheritingLock {
public:
    void lock() override { }
    void unlock() override { }
};

} // namespace

std::unique_ptr<PriorityInheritingLock> makePriorityInheritingLock()
{
    return std::unique_ptr<PriorityInheritingLock>(new (std::nothrow) SingleThreadLock);
}

// One thread, and it is the main one.
bool isMainThread() { return true; }

// ---- Atomic file storage ----------------------------------------

// No persistent host filesystem: report empty / unavailable. An embedder that
// wants preferences persistence wires it through the frontend PAL half.
int storageReadFile(const char* /*path*/, void* /*outputBuffer*/, int /*outputCapacity*/) { return -1; }

std::expected<BoundedFileBytes, FileReadError> storageReadBounded(const char*, std::size_t) { return std::unexpected(FileReadError::Unavailable); }

bool storageEnsureDirectory(const char* /*path*/) { return false; }

DurableFileResult storageWriteDurable(const char*, std::span<const std::byte>) { return DurableFileResult::NotCommitted; }

bool storageAppendOwnerOnly(const char*, std::span<const std::byte>) { return false; }

bool storageWriteAtomic(const char* /*path*/, const void* /*buffer*/, int /*length*/) { return false; }

void storageRemoveFile(const char* /*path*/) { }

bool storageWriteTempFile(const char* /*directoryHint*/, const char* /*suffix*/, const void* /*buffer*/, int /*length*/, char* /*pathOut*/, int /*pathCapacity*/) { return false; }

// ---- Process spawn ----------------------------------------------

// wasm cannot spawn host processes; an executable-existence query is always
// false and a spawn always fails. Trigger / exec features degrade to no-ops.
//
// In-band file transfer is not one of them: a web page's agent is an
// in-process ZMODEM codec answering the protocol on the stream, not a spawned
// binary — the detection and hold flow upstream of this file are pure byte
// handling and work unchanged. Until that codec ships, the resolution below
// reports "no agent installed", which is the same absence a POSIX machine
// without lrzsz reports.
bool systemExecutableExists(const char* /*path*/) { return false; }

std::string downloadsDirectoryDefaultPath() { return {}; }

int processSpawnAndWait(const char* /*path*/, const char* const /*argv*/[]) { return -1; }

} // namespace PAL
} // namespace Ubiquitty
