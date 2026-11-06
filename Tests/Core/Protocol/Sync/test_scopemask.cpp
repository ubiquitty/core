// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// Tests/Core/Protocol/Sync/test_scopemask.cpp — pins Ubiquitty::Protocol::Sync::scopeMaskToString, the
// capability-token scope-bitmask decoder.
//
// Each scope* bit maps to one label, bits emit low-to-high, an empty mask reads
// "none", and unknown high bits are ignored. Only the inline decoder is used, so
// the NativeAttachment.h header compiles + links standalone.

#include "Core/Protocol/Sync/NativeAttachment.h"

#include <cstdio>
#include <string>

using Ubiquitty::Protocol::Sync::scopeMaskToString;
using namespace Ubiquitty::Protocol::Sync;
static void expectScope(ScopeSet mask, const std::string& want, const char* label)
{
    const std::string got = scopeMaskToString(mask);
    if (got != want) {
        char ubqMessage[512];
        std::snprintf(ubqMessage, sizeof(ubqMessage), R"(%s: scopeMaskToString(0x%02x) = "%s", want "%s")", label, static_cast<unsigned>(mask.toRaw()), got.c_str(), want.c_str());
        CHECK_MSG(false, ubqMessage);
    }
}

TEST(scopemask)
{

    // Empty set.
    expectScope(ScopeSet {}, "none", "empty set -> none");

    // Each single bit, named against the enumerator (not a magic number).
    expectScope(Scope::Observe, "observe", "observe bit");
    expectScope(Scope::Drive, "drive", "drive bit");
    expectScope(Scope::Control, "control", "control bit");
    expectScope(Scope::RegionRead, "region", "region bit");
    expectScope(Scope::NewSession, "new", "new bit");
    expectScope(Scope::FileRead, "file", "file bit"); // FILE_REQUEST

    // The raw bit values themselves, pinned so an enumerator edit can't drift the decode.
    expectScope(ScopeSet::fromRaw(0x01), "observe", "0x01");
    expectScope(ScopeSet::fromRaw(0x02), "drive", "0x02");
    expectScope(ScopeSet::fromRaw(0x04), "control", "0x04");
    expectScope(ScopeSet::fromRaw(0x08), "region", "0x08");
    expectScope(ScopeSet::fromRaw(0x10), "new", "0x10");
    expectScope(ScopeSet::fromRaw(0x20), "file", "0x20");

    // Combinations emit low-to-high, comma-joined, deterministic.
    expectScope(Scope::Observe | Scope::Drive, "observe,drive", "observe+drive");
    expectScope(Scope::Drive | Scope::Observe, "observe,drive", "order-independent input");
    expectScope(Scope::Observe | Scope::Control, "observe,control", "observe+control");
    expectScope(Scope::Control | Scope::RegionRead | Scope::NewSession, "control,region,new", "control+region+new");
    expectScope(Scope::Observe | Scope::FileRead, "observe,file", "observe+file");
    expectScope(Scope::Observe | Scope::Drive | Scope::Control | Scope::RegionRead | Scope::NewSession | Scope::FileRead, "observe,drive,control,region,new,file", "all six bits");

    expectScope(Scope::Manage, "manage", "explicit management authority");
    expectScope(ScopeSet::fromRaw(0x81), "observe", "unknown bit alongside observe");
    expectScope(ScopeSet::fromRaw(0xE0), "file,manage", "file and management authority");
}

UBIQUITTY_TEST_MAIN()
