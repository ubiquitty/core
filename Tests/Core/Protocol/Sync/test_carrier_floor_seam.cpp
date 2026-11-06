// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_carrier_floor_seam.cpp — the wire ceilings fit what every carrier
// guarantees to accept. NativeAttachment.h holds this as static_asserts, so a
// violation is a build error; these are the same facts as checked values, so
// the failure names the numbers and runs wherever the suite runs.

#include "Harness.h"

#include "Core/Protocol/Control/FileRead.h"
#include "Core/Protocol/Control/PastePush.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/Transport/CarrierCapacity.h"

using Ubiquitty::Protocol::Sync::maxClipboardServeBytes;
using Ubiquitty::Transport::guaranteedOutboundFrameBytes;

// A framed fallback carrier ends the channel rather than drop bytes from the
// middle of a frame, so a ceiling above the carrier floor turns a legal frame
// into a dead session on the degraded path. These are the remaining
// catalog single-frame ceilings; paste.push uses bounded raw chunks instead.
TEST(test_sync_frame_ceilings_fit_the_carrier_floor)
{
    CHECK_MSG(Ubiquitty::Protocol::Control::maxReadFileBytes <= guaranteedOutboundFrameBytes,
        "file.read ceiling fits within every carrier's outbound guarantee");
    CHECK_MSG(maxClipboardServeBytes <= guaranteedOutboundFrameBytes,
        "CLIPBOARD_DATA ceiling fits within every carrier's outbound guarantee");

    // Chunking changes the transport unit, not the payload promise. The new
    // upload contract keeps the pasted-file cap clients already size against.
    CHECK_MSG(Ubiquitty::Protocol::Control::maxPastePushBytes == 4u * 1024u * 1024u,
        "credited paste uploads preserve the existing 4 MiB payload limit");
}

UBIQUITTY_TEST_MAIN()
