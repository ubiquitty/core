// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include "Core/Protocol/Sync/EffectCodec.h"
#include "Core/Protocol/Sync/SyncClientCore.h"

#include <array>
#include <cstdint>
#include <set>
#include <variant>
#include <vector>

namespace {

namespace Sync = Ubiquitty::Protocol::Sync;

// Listed by hand rather than derived from frameKindOf: a new enum member
// joins the catalog only when a test names it here, and no undeclared byte
// may become a recognized kind.
constexpr Sync::FrameKind allKinds[] {
    Sync::FrameKind::Greet,
    Sync::FrameKind::Welcome,
    Sync::FrameKind::Facts,
    Sync::FrameKind::Ack,
    Sync::FrameKind::Probe,
    Sync::FrameKind::Echo,
    Sync::FrameKind::PaneBind,
    Sync::FrameKind::PaneUnbind,
    Sync::FrameKind::BulkChunk,
    Sync::FrameKind::Bye,
    Sync::FrameKind::TypographyFacts,
    Sync::FrameKind::BulkAbort,
    Sync::FrameKind::Call,
    Sync::FrameKind::Result,
    Sync::FrameKind::Notify,
    Sync::FrameKind::WorkspaceStructure,
    Sync::FrameKind::PaneListed,
    Sync::FrameKind::ProportionalFaces,
    Sync::FrameKind::CheckpointHead,
    Sync::FrameKind::Update,
    Sync::FrameKind::Resync,
    Sync::FrameKind::View,
    Sync::FrameKind::PaneDirty,
    Sync::FrameKind::PanePalette,
    Sync::FrameKind::HistoryInline,
    Sync::FrameKind::HistoryHead,
    Sync::FrameKind::HistoryReset,
    Sync::FrameKind::HistoryWant,
    Sync::FrameKind::PaneFacts,
    Sync::FrameKind::StatusRecords,
    Sync::FrameKind::Effect,
    Sync::FrameKind::ClipboardRequest,
    Sync::FrameKind::ClipboardData,
    Sync::FrameKind::ImageHead,
    Sync::FrameKind::ImageWant,
    Sync::FrameKind::ImagePlacements,
    Sync::FrameKind::SampleHead,
    Sync::FrameKind::RichTextMeasure,
    Sync::FrameKind::RichTextMeasured,
    Sync::FrameKind::NativeDragCommand,
    Sync::FrameKind::NativeDragEvent,
    Sync::FrameKind::Receipt,
    Sync::FrameKind::Keys,
    Sync::FrameKind::Pointer,
    Sync::FrameKind::Paste,
    Sync::FrameKind::PasteAbort,
    Sync::FrameKind::Raw,
    Sync::FrameKind::Focus,
    Sync::FrameKind::PasteReceipt,
    Sync::FrameKind::InputDiscarded,
    Sync::FrameKind::Viewport,
};

// Negative byte fixtures only. Old low tags overlap current kinds, so their
// five-byte grammar must fail current state/body validation, not an artificial
// global reservation that would also prohibit a valid canonical kind.
void refusesFormerHostFrame(std::uint8_t tag)
{
    if (!Sync::frameKindOf(tag)) {
        Sync::FrameDecoder decoder;
        decoder.append(std::span(&tag, 1));
        CHECK(!decoder.next());
        CHECK(decoder.error() == Sync::EnvelopeError::UnknownKind);
    }
    for (const bool isWelcomed : { false, true }) {
        for (const std::uint32_t size : { 0u, 72u * 1024u * 1024u }) {
            Sync::SyncClientCore core(nullptr, nullptr);
            core.setClock([] { return 1; });
            core.setClientInstance({ 3, 4 });
            std::vector<std::uint8_t> greeting;
            core.buildHello(Sync::HandshakeRequest {}, greeting);
            REQUIRE(!greeting.empty());
            if (isWelcomed) {
                Sync::Welcome welcome;
                welcome.wireVersion = Sync::wireVersion;
                welcome.hostInstanceID = { 17, 19 };
                welcome.attachmentID = { 23, 29 };
                welcome.resumeSecret = { 31, 37 };
                welcome.selection = { .target = Sync::AdmissionTarget::Session, .sessionID = 7, .workspaceID = 0, .daemonInstanceID = 17, .socketName = "catalog-refusal.sock", .buildFlags = 0, .hostPlatform = "fixture" };
                std::vector<std::uint8_t> body;
                Sync::encode(body, welcome);
                std::vector<std::uint8_t> frame;
                REQUIRE(Sync::appendFrame(frame, Sync::FrameKind::Welcome, 0, body));
                REQUIRE(core.feed(std::as_bytes(std::span(frame))));
            }
            const std::array<std::uint8_t, 5> oldHeader { tag, static_cast<std::uint8_t>(size),
                static_cast<std::uint8_t>(size >> 8), static_cast<std::uint8_t>(size >> 16), static_cast<std::uint8_t>(size >> 24) };
            CHECK(!core.feed(std::as_bytes(std::span(oldHeader))));
            CHECK(core.faulted());
            CHECK(!core.isPaneBound(7));
        }
    }
}

TEST(testEveryDefinedTagIsRecognised)
{
    for (const auto kind : allKinds)
        CHECK(Sync::frameKindOf(std::to_underlying(kind)) == kind);
}

TEST(aBellHasOnlyItsTypedEffectRepresentation)
{
    refusesFormerHostFrame(0x84);
    const auto encoded = Sync::encodeEffectBody(Sync::BellEffect {});
    REQUIRE(encoded);
    const auto decoded = Sync::decodeEffectBody(Sync::EffectKind::Bell, encoded->view());
    REQUIRE(decoded);
    CHECK(std::holds_alternative<Sync::BellEffect>(*decoded));
}

TEST(soundsAndSamplesHaveNoAlternateCatalogEncoding)
{
    for (const std::uint8_t tag : { 0x9F, 0xA0 })
        refusesFormerHostFrame(tag);
}

TEST(commandCompletionsHaveNoAlternateCatalogEncoding)
{
    refusesFormerHostFrame(0x87);
}

TEST(imagesHaveNoAlternateCatalogEncoding)
{
    for (const std::uint8_t tag : { 0x98, 0x99 })
        refusesFormerHostFrame(tag);
}

TEST(clipboardWritesAndNotificationsHaveNoAlternateCatalogEncoding)
{
    for (const std::uint8_t tag : { 0xA6, 0xA7 })
        refusesFormerHostFrame(tag);
}

TEST(clipboardReadsAndLocalHelpersHaveNoAlternateCatalogEncoding)
{
    for (const std::uint8_t tag : { 0x13, 0x19, 0x9D, 0xA2 })
        refusesFormerHostFrame(tag);
}

TEST(eventStreamsUseOnlyCanonicalCallsAndNotifies)
{
    for (const std::uint8_t tag : { 0x1B, 0xA4 })
        refusesFormerHostFrame(tag);
}

TEST(pastedFilesUseOnlyCreditedCanonicalUploadsAndResults)
{
    for (const std::uint8_t tag : { 0x12, 0x9B })
        refusesFormerHostFrame(tag);
}

TEST(topologyOperationsHaveNoAlternateCatalogEncoding)
{
    for (const std::uint8_t tag : { 0x17, 0x1C, 0x9E, 0xA5 })
        refusesFormerHostFrame(tag);
}

TEST(consentAndPresenceHaveNoAlternateCatalogEncoding)
{
    for (const std::uint8_t tag : { 0x0A, 0x0B, 0x8C, 0x8D, 0x8E, 0x90, 0x91 })
        refusesFormerHostFrame(tag);
}

TEST(testUndefinedTagsAreRejected)
{
    std::set<unsigned> defined;
    for (const auto kind : allKinds)
        CHECK(defined.insert(std::to_underlying(kind)).second);
    for (unsigned raw = 0; raw <= 0xFF; ++raw)
        CHECK(Sync::frameKindOf(static_cast<std::uint8_t>(raw)).has_value() == defined.contains(raw));
    for (const std::uint8_t tag : { 0x00, 35, 0x96, 0x97, 0xFF })
        refusesFormerHostFrame(tag);
}

} // namespace

UBIQUITTY_TEST_MAIN()
