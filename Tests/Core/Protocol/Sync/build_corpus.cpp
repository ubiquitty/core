// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// build_corpus.cpp — the structured seeds of the Sync fuzz corpus, written
// by the encoders at the current wire, so every handshake a seed carries is
// one a peer of this build admits.
//
// Each seed opens with the selector byte the two harnesses that share the
// corpus read: fuzz_sync takes it modulo 10, fuzz_consent modulo 9. A seed
// meant for one names the path it feeds there, and is a stray input to the
// other.
//
// Built as ubq-sync-corpus-builder (Tests/CMakeLists.txt) and run by
// Scripts/refresh-sync-fuzz-corpus.py, which names the seeds by their SHA-1
// and merges them with the corpus by each harness's coverage.

#include "Core/Attributes/Hyperlinks.h"
#include "Core/CellGrid.h"
#include "Core/Crypto/SHA256.h"
#include "Core/Protocol/Local/AdoptWire.h"
#include "Core/Protocol/Local/Handoff.h"
#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/Protocol/Sync/Records.h"
#include "Core/Protocol/Sync/ScreenPlane.h"
#include "Core/Screen/Addressing.h"
#include "Core/Session/SessionShare.h"
#include "Core/TerminalOperations.h"
#include <memory>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace Ubiquitty;
using namespace Ubiquitty::Protocol::Sync;

static std::string gDirectory;

// Write one seed: [selector byte][bytes].
static void emit(const char* name, uint8_t selector, const std::vector<uint8_t>& bytes)
{
    std::string path = gDirectory + "/" + name;
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::perror(path.c_str());
        std::exit(1);
    }
    std::fputc(selector, f);
    if (!bytes.empty())
        std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
}

static std::unique_ptr<Ubiquitty::Terminal> makeTerm() { return std::make_unique<Ubiquitty::Terminal>(); }

// A Sync checkpoint body of a terminal, the form a screen travels in.
static std::vector<uint8_t> checkpointBody(const Ubiquitty::Terminal& host)
{
    Ubiquitty::Protocol::Sync::ScreenPublisher publisher;
    Ubiquitty::Protocol::Sync::AttachmentBaseline baseline;
    std::vector<uint8_t> body;
    if (!publisher.capture(host) || !publisher.checkpoint(host, 1, body, baseline))
        body.clear();
    return body;
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <corpus-directory>\n", argv[0]);
        return 2;
    }
    gDirectory = argv[1];

    // --- A direct handshake + control-frame stream (selector 3). ---
    {
        namespace Sync = Ubiquitty::Protocol::Sync;
        std::vector<uint8_t> wire;
        Sync::Greet hello;
        hello.wireVersion = Sync::wireVersion;
        hello.clientInstance = { 3, 4 };
        hello.facts.viewportColumns = 120;
        hello.facts.viewportRows = 40;
        std::vector<std::uint8_t> body;
        Sync::encode(body, hello);
        if (!Sync::appendFrame(wire, Sync::FrameKind::Greet, 0, body))
            return 1;
        Sync::Welcome ack;
        ack.wireVersion = Sync::wireVersion;
        ack.hostInstanceID = { 9, 10 };
        ack.attachmentID = { 11, 12 };
        ack.resumeSecret = { 13, 14 };
        ack.selection = { .target = Sync::AdmissionTarget::Session, .sessionID = 0x12345678u, .workspaceID = 0, .daemonInstanceID = 9, .socketName = "fixture.sock", .buildFlags = 0, .hostPlatform = "fixture" };
        body.clear();
        Sync::encode(body, ack);
        if (!Sync::appendFrame(wire, Sync::FrameKind::Welcome, 0, body))
            return 1;
        body.clear();
        Sync::encode(body, Sync::Call { .text = R"({"jsonrpc":"2.0","id":"capture.9","method":"pane.capture","params":{"pane":1,"source":"rows","start":200,"end":260}})" });
        if (!Sync::appendFrame(wire, Sync::FrameKind::Call, 0, body))
            return 1;
        const char* title = "~/work \xE2\x80\x94 zsh";
        Protocol::Sync::PaneFacts facts;
        facts.programTitle = title;
        body.clear();
        Protocol::Sync::encodePaneFacts(body, facts);
        if (!Sync::appendFrame(wire, Sync::FrameKind::PaneFacts, 1, body))
            return 1;
        body.clear();
        Sync::encode(body, Sync::Bye {});
        if (!Sync::appendFrame(wire, Sync::FrameKind::Bye, 0, body))
            return 1;
        emit("handshake-stream", 3, wire);
    }

    // --- A checkpoint body (selector 2). ---
    {
        auto host = makeTerm();
        Ubiquitty::resize(*host, 4, 6);
        for (int r = 0; r < host->rows; ++r) {
            for (int c = 0; c < host->columns; ++c) {
                Cell cell {};
                cell.codePoint = static_cast<uint32_t>('A' + (r * host->columns + c));
                cell.pen.attributes = Ubiquitty::VT::AttributeSet::fromRaw(static_cast<uint16_t>(r));
                cell.pen.foregroundIndex = static_cast<uint16_t>(c);
                cell.pen.backgroundIndex = colorDefault;
                cell.pen.underlineIndex = colorDefault;
                cell.pen.foregroundRed = static_cast<uint8_t>(r * 16);
                cell.pen.foregroundGreen = static_cast<uint8_t>(c * 8);
                cell.pen.foregroundBlue = 0x33;
                if (r == 1 && c == 2) {
                    cell.combining[0] = 0x0301;
                    cell.setCombiningLength(1);
                }
                host->grid->setCell(Ubiquitty::Row(r), Ubiquitty::Column(c), cell);
            }
        }
        host->screens.active->cursorY = 2;
        host->screens.active->cursorX = 4;
        host->modes.isCursorVisible = true;
        host->cursorShapePref = 1;
        host->modes.reverseVideo = true;
        host->screens.active->lineAttribute[0] = Ubiquitty::lineAttributeDoubleWidth;
        Ubiquitty::Screen::addRowMark(host.get(), 1, Screen::osc133MarkPromptStart);
        uint16_t linkID = Ubiquitty::Attributes::Hyperlinks::intern(host.get(), "lk", "https://example.com/path");
        {
            Cell linked {};
            linked.codePoint = 'L';
            linked.pen.foregroundIndex = colorDefault;
            linked.pen.backgroundIndex = colorDefault;
            linked.pen.underlineIndex = colorDefault;
            linked.setLinkID(linkID);
            host->grid->setCell(Ubiquitty::Row(3), Ubiquitty::Column(0), linked);
        }
        host->palette.indexed[1] = { 0x1234, 0x5678, 0x9ABC };
        Ubiquitty::setTitleUTF8String(*host, "checkpoint-title-\xC3\xA9");

        emit("checkpoint-body", 2, checkpointBody(*host));

        host.reset();
    }

    // --- A checkpoint carrying an overflow grapheme cluster (selector 2). ---
    {
        auto host = makeTerm();
        Ubiquitty::resize(*host, 2, 4);
        const char* zwj = "\xF0\x9F\x91\xA8"
                          "\xE2\x80\x8D"
                          "\xF0\x9F\x91\xA9"
                          "\xE2\x80\x8D"
                          "\xF0\x9F\x91\xA7"
                          "\xE2\x80\x8D"
                          "\xF0\x9F\x91\xA6";
        Ubiquitty::processBytes(host.get(), zwj);
        emit("checkpoint-cluster-body", 2, checkpointBody(*host));
        host.reset();
    }

    // --- Bare typed-message bodies (selector 1). ---
    {
        namespace Sync = Ubiquitty::Protocol::Sync;
        Sync::Greet hello;
        hello.wireVersion = Sync::wireVersion;
        hello.clientInstance = { 3, 4 };
        hello.facts.viewportColumns = 80;
        hello.facts.viewportRows = 24;
        hello.request.sessionID = 7;
        hello.request.isAttachOnly = true;
        std::vector<uint8_t> body;
        Sync::encode(body, hello);
        emit("greet-body", 1, body);
    }
    {
        namespace Sync = Ubiquitty::Protocol::Sync;
        std::vector<uint8_t> body;
        Sync::encode(body, Sync::Call { .text = R"({"jsonrpc":"2.0","id":"capture.9","method":"pane.capture","params":{"pane":1,"source":"rows","start":132,"end":182}})" });
        emit("capture-call-body", 1, body);
    }

    // --- More bare typed bodies (selector 1): a Welcome, a refusing Bye that
    // names its build, and a Greet at another version, the refusal's input. ---
    {
        namespace Sync = Ubiquitty::Protocol::Sync;
        Sync::Welcome welcome;
        welcome.wireVersion = Sync::wireVersion;
        welcome.hostInstanceID = { 21, 22 };
        welcome.attachmentID = { 23, 24 };
        welcome.resumeSecret = { 25, 26 };
        welcome.selection = { .target = Sync::AdmissionTarget::Session, .sessionID = 7, .workspaceID = 0, .daemonInstanceID = 21, .socketName = "fixture.sock", .buildFlags = 0, .hostPlatform = "fixture" };
        welcome.principal = "fixture";
        std::vector<uint8_t> body;
        Sync::encode(body, welcome);
        emit("welcome-body", 1, body);
        body.clear();
        Sync::encode(body, Sync::spokenBye(Sync::wireVersion, "0.2.0", "Linux x86_64", Sync::ByeReason::VersionMismatch, "refused"));
        emit("bye-peer-body", 1, body);
        Sync::Greet other;
        other.wireVersion = Sync::wireVersion + 1;
        other.clientInstance = { 5, 6 };
        body.clear();
        Sync::encode(body, other);
        emit("greet-other-version-body", 1, body);
    }

    // --- A stream whose first byte is its first frame's kind (selector 0):
    // a Bye's kind, 10, is the one whose residue reaches that path. ---
    {
        namespace Sync = Ubiquitty::Protocol::Sync;
        std::vector<uint8_t> wire;
        std::vector<uint8_t> body;
        Sync::encode(body, Sync::Bye { .reason = Sync::ByeReason::Normal, .detail = "done" });
        if (!Sync::appendFrame(wire, Sync::FrameKind::Bye, 0, body))
            return 1;
        Sync::Greet hello;
        hello.wireVersion = Sync::wireVersion;
        hello.clientInstance = { 7, 8 };
        body.clear();
        Sync::encode(body, hello);
        if (!Sync::appendFrame(wire, Sync::FrameKind::Greet, 0, body))
            return 1;
        std::FILE* f = std::fopen((gDirectory + "/bye-led-stream").c_str(), "wb");
        if (!f)
            return 1;
        std::fwrite(wire.data(), 1, wire.size(), f);
        std::fclose(f);
    }

    // --- The host's half through the client dispatch (selector 5): a
    // Welcome, then a refusing Bye. ---
    {
        namespace Sync = Ubiquitty::Protocol::Sync;
        std::vector<uint8_t> wire;
        Sync::Welcome welcome;
        welcome.wireVersion = Sync::wireVersion;
        welcome.hostInstanceID = { 31, 32 };
        welcome.attachmentID = { 33, 34 };
        welcome.resumeSecret = { 35, 36 };
        welcome.selection = { .target = Sync::AdmissionTarget::Session, .sessionID = 9, .workspaceID = 0, .daemonInstanceID = 31, .socketName = "fixture.sock", .buildFlags = 0, .hostPlatform = "fixture" };
        std::vector<uint8_t> body;
        Sync::encode(body, welcome);
        if (!Sync::appendFrame(wire, Sync::FrameKind::Welcome, 0, body))
            return 1;
        body.clear();
        Sync::encode(body, Sync::spokenBye(Sync::wireVersion, "0.2.0", "fixture", Sync::ByeReason::SessionGone, "gone"));
        if (!Sync::appendFrame(wire, Sync::FrameKind::Bye, 0, body))
            return 1;
        emit("welcome-stream", 5, wire);
    }

    // --- A local share: the whole-terminal body (fuzz_sync 6, fuzz_consent 3)
    // and the Prepare record that carries it (fuzz_consent 0). ---
    {
        auto window = makeTerm();
        Ubiquitty::resize(*window, 4, 20);
        window->scrollbackLimit = 64;
        Ubiquitty::processBytes(window.get(), "first line\r\nsecond line\r\n\x1b[31mred\x1b[m prompt");
        const auto captured = Ubiquitty::Session::captureShareTerminal(*window, Ubiquitty::Protocol::Local::maxAdoptBodyBytes);
        if (!captured)
            return 1;
        const std::vector<uint8_t> share(captured->view().begin(), captured->view().end());
        emit("share-body", 66, share);
        namespace Local = Ubiquitty::Protocol::Local;
        Local::AdoptWirePrepare prepare { .correlation = 7,
            .preparation = { .operation = { .operationID = { 1, 2 }, .daemonInstanceID = 9 }, .bodyBytes = share.size() },
            .terminalBody = share };
        Ubiquitty::Crypto::sha256(share, prepare.preparation.bodySHA256);
        const auto encoded = Local::encodeAdoptWirePrepare(prepare, Local::maxAdoptBodyBytes + Local::adoptWirePrepareHeaderBytes);
        if (!encoded)
            return 1;
        std::vector<uint8_t> record(encoded->header.begin(), encoded->header.end());
        record.insert(record.end(), share.begin(), share.end());
        emit("prepare-record", 36, record);
    }

    std::printf("seeded fuzz-corpus in %s\n", gDirectory.c_str());
    return 0;
}
