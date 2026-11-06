// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Clipboard/ReadStage.h"
#include "Core/Clipboard/StagedClient.h"
#include "Harness.h"

#include <string>
#include <vector>

TEST(testReadStageServesBytesOnceAndDistinguishesEmptyFromUnstaged)
{
    Ubiquitty::Clipboard::ReadStage stage;
    char out[16];
    // Nothing staged: -1, the refused-read answer Core stays silent on.
    CHECK(stage.serveBytes(out) == -1);

    stage.stageBytes({ "Hi", 2 });
    CHECK(stage.serveBytes(out) == 2);
    CHECK(std::string_view(out, 2) == "Hi");
    // One-shot: the same staging never answers twice.
    CHECK(stage.serveBytes(out) == -1);

    // Staged-but-empty answers 0 — Core frames an empty reply, not silence.
    stage.stageBytes({});
    CHECK(stage.serveBytes(out) == 0);
}

TEST(testReadStageServesTheTypedCaptureRepeatedly)
{
    Ubiquitty::Clipboard::ReadStage stage;
    Ubiquitty::Clipboard::Snapshot snapshot;
    snapshot.add("text/plain", { 'H', 'i' });
    snapshot.add("Image/PNG", { 'P' });
    stage.stageTyped(std::move(snapshot));

    char out[64];
    int n = stage.serveTypes(out);
    // The stored spelling rides the list verbatim, and the caseless match
    // folds it — the capitals live on the stored side here.
    CHECK(n > 0 && std::string_view(out, static_cast<std::size_t>(n)) == "text/plain Image/PNG");
    // A list longer than the span names what fits, never a torn name.
    char tiny[12];
    n = stage.serveTypes(tiny);
    CHECK(n == 10 && std::string_view(tiny, 10) == "text/plain");
    CHECK(stage.serveItem("image/png", out) == 1 && out[0] == 'P');
    // The typed capture persists across serves: one approval answers the
    // type list and every item read of its completion.
    CHECK(stage.serveItem("text/plain", out) == 2);
    CHECK(stage.serveItem("text/plain", out) == 2);
    CHECK(stage.serveItem("text/rtf", out) == -1);
}

TEST(testReadStageTypedAndBytesStagingsAreIndependent)
{
    Ubiquitty::Clipboard::ReadStage stage;
    Ubiquitty::Clipboard::Snapshot snapshot;
    snapshot.add("text/plain", { 'T' });
    stage.stageTyped(std::move(snapshot));
    stage.stageBytes({ "B", 1 });

    char out[16];
    // Serving the one-shot bytes leaves the typed capture in place.
    CHECK(stage.serveBytes(out) == 1 && out[0] == 'B');
    CHECK(stage.serveItem("text/plain", out) == 1 && out[0] == 'T');
}

TEST(testReadStageEmptyTypedCaptureClearsTheTypedStaging)
{
    Ubiquitty::Clipboard::ReadStage stage;
    Ubiquitty::Clipboard::Snapshot snapshot;
    snapshot.add("text/plain", { 'T' });
    stage.stageTyped(std::move(snapshot));

    char out[16];
    CHECK(stage.serveTypes(out) > 0);
    // An empty capture clears: a later approval of an untyped source must
    // not leave the earlier source's types answering for it.
    stage.stageTyped(Ubiquitty::Clipboard::Snapshot {});
    CHECK(stage.serveTypes(out) == -1);
    CHECK(stage.serveItem("text/plain", out) == -1);
}

// The whole payload, without a buffer to size first.
//
// A remote session serving this Mac's clipboard ships the bytes entire, so it
// cannot use the bounded read: sizing a span for a payload of unknown length
// means either an 8 MiB buffer to probe with or a truncation that nothing
// detects. Both readers match names through this one function, which is the
// point — a second comparison would answer "no such type" for a name the
// bounded read serves happily.
TEST(testFindAnswersTheWholePayloadAndMatchesTheWayTheBoundedReadDoes)
{
    Ubiquitty::Clipboard::Snapshot snapshot;
    snapshot.add("Image/PNG", { 'P', 'N', 'G', '\0', '\x89' });
    snapshot.add("text/plain", { 'H', 'i' });

    const Ubiquitty::Clipboard::SnapshotItem* found = snapshot.find("image/png");
    CHECK(found != nullptr);
    // Guarded rather than dereferenced straight: CHECK records and continues,
    // so a miss here would crash the case instead of reporting it, and a test
    // that fails by signal cannot say which assertion was wrong.
    if (found != nullptr) {
        // Whole, and the embedded NUL survives: a length, not a C string.
        CHECK(found->bytes.size() == 5 && found->bytes[3] == '\0' && found->bytes[4] == '\x89');
        // The stored spelling comes back, not the requested one — a caller
        // echoing this into an answer names the type the pasteboard holds.
        CHECK(found->mime == "Image/PNG");
    }

    // The two matching rules the bounded read documents, asserted on the
    // function that owns them: parameters are stripped, case is folded.
    CHECK(snapshot.find("IMAGE/PNG;base64") != nullptr);
    CHECK(snapshot.find("text/plain; charset=utf-8") != nullptr);
    // And a type the capture does not hold is absent rather than approximate.
    CHECK(snapshot.find("text/rtf") == nullptr);
    CHECK(snapshot.find("image/png2") == nullptr);
    CHECK(snapshot.find("") == nullptr);

    // The bounded read agrees with it, which is the property extracting it
    // was for: same names match, same names miss.
    char out[8];
    CHECK(snapshot.item("IMAGE/PNG;base64", out) == 5);
    CHECK(snapshot.item("text/rtf", out) == -1);
}

UBIQUITTY_TEST_MAIN()

TEST(testStagedClientsOwnCapturesAndKeepSessionsIsolated)
{
    Ubiquitty::Clipboard::StagedClient first;
    Ubiquitty::Clipboard::StagedClient second;
    Ubiquitty::ClipboardClient& client = first;
    constexpr auto buffer = Ubiquitty::ClipboardBuffer::Clipboard;
    CHECK(client.requiresAsynchronousClipboardRead());
    char output[64];
    {
        std::string mime = "image/png";
        std::string payload { 'A', '\0', 'B' };
        const Ubiquitty::ClipboardItem item { mime, payload };
        client.provideClipboardReadItems(buffer, { &item, 1 });
        client.provideClipboardReadBytes(buffer, payload);
        payload.assign("overwritten");
        mime.assign("text/plain");
    }
    CHECK(second.readClipboard(buffer, output) == -1);
    CHECK(second.readClipboardTypes(buffer, output) == -1);
    CHECK(client.readClipboardItem(buffer, "IMAGE/PNG;charset=binary", output) == 3);
    CHECK(output[0] == 'A' && output[1] == '\0' && output[2] == 'B');
    CHECK(client.readClipboard(buffer, {}) == -1);
    CHECK(client.readClipboard(buffer, output) == 3);
    CHECK(client.readClipboard(buffer, output) == -1);
    CHECK(client.readClipboardItem(buffer, "image/png", output) == 3);
    client.provideClipboardReadItems(buffer, {});
    CHECK(client.readClipboardTypes(buffer, output) == -1);
    CHECK(client.readClipboardItem(buffer, "image/png", output) == -1);
    client.provideClipboardReadBytes(buffer, {});
    CHECK(client.readClipboard(buffer, output) == 0);
}
