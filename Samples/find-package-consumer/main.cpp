// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Samples/find-package-consumer/main.cpp — proves the INSTALLED libUbiquittyCore
// package is consumable via find_package(UbiquittyCore).
//
// This TU uses public Core, Kit, and PAL headers resolved from the install prefix
// (no source-tree include paths), plus the stub PAL the caller must supply.
// It boots a Terminal, feeds VT bytes, reads a snapshot row back
// through the supported API, and asserts the grid matches — a byte-in /
// grid-out proof that the packaged library links and runs.
//
#include "PAL/Socket.h"
#include "UbiquittyCore/UbiquittyCore.h"
#include "UbiquittyKit/UbiquittyKit.h"

UBIQUITTY_KIT_REQUIRE(1, 0, 0);

#include <array>
#include <cstdio>
#include <cstring>
#include <span>
#include <string_view>

// The "system services" half of the PAL contract the bundled POSIX backend does
// not provide (see UbiquittyCore/UbiquittyCore.h). No-ops are all a headless consumer needs.
namespace Ubiquitty {
namespace PAL {
Ticks monotonicTicks() { return {}; }
uint32_t monotonicMicros() { return 0; }
void diagWrite(std::string_view message)
{
    if (!message.empty()) {
        std::fwrite(message.data(), 1, message.size(), stderr);
    }
}
void bellRing(BellMode) { }
void clipboardSetText(std::string_view) { }
int clipboardGetText(char*, int) { return -1; }
int preferencesLoad(PreferencesBlobKey, void*, int) { return -1; }
int preferencesSaveAtomic(PreferencesBlobKey, std::span<const std::byte>) { return 0; }
} // namespace PAL
} // namespace Ubiquitty

// Kit-facade smoke over that same installed prefix: a local session against a
// socketpair "PTY", the pre-start-input no-op, and the destroy-before-drain
// lifetime regression (queued work outliving the Session runs harmlessly).
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {
struct InlineQueueDispatcher final : Ubiquitty::Session::Dispatcher {
    void post(Ubiquitty::Session::PostedWork work) override
    {
        std::lock_guard<std::mutex> lock(mutex);
        queue.push_back(std::move(work));
    }
    void postDelayed(double, Ubiquitty::Session::PostedWork work) override { post(std::move(work)); }
    void drain()
    {
        for (;;) {
            Ubiquitty::Session::PostedWork work;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (queue.empty())
                    return;
                work = std::move(queue.front());
                queue.pop_front();
            }
            std::move(work)();
        }
    }
    std::mutex mutex;
    std::deque<Ubiquitty::Session::PostedWork> queue;
};
struct PairFacts final : Ubiquitty::PAL::ProcessFacts {
    std::string foregroundName() override { return {}; }
    bool foregroundIsShell() override { return false; }
    std::optional<Ubiquitty::PAL::ProcessID> foregroundProcessGroup() override { return std::nullopt; }
    std::string foregroundCurrentDirectory() override { return {}; }
    std::string ttyName() override { return {}; }
    bool hasRunningChild() override { return false; }
    int gatherProcesses(std::span<Ubiquitty::PAL::Process::Record>) override { return 0; }
};
struct PairStream final : Ubiquitty::PAL::Stream {
    explicit PairStream(int fileDescriptorIn)
        : ownedFileDescriptor(fileDescriptorIn)
    {
    }
    ~PairStream() override
    {
        if (ownedFileDescriptor >= 0)
            close(ownedFileDescriptor);
    }
    int read(std::span<char> b) override { return static_cast<int>(::read(ownedFileDescriptor, b.data(), b.size())); }
    int write(std::span<const char> b) override { return static_cast<int>(::write(ownedFileDescriptor, b.data(), b.size())); }
    int writeAll(std::span<const char> b) override { return write(b); }
    bool drainPendingWrite() override { return false; }
    bool hasPendingWrite() const override { return false; }
    void setWindowSize(int, int, int, int) override { }
    int lastError() const override { return 0; }
    int fileDescriptor() const override { return ownedFileDescriptor; }
    std::unique_ptr<Ubiquitty::PAL::PollableByteStream> duplicateAsPollableStream() override
    {
        const int duplicate = dup(ownedFileDescriptor);
        if (duplicate < 0) {
            return nullptr;
        }
        return Ubiquitty::PAL::adoptByteStream(duplicate, Ubiquitty::PAL::ReadinessWaitStrategy::WaitFirst);
    }
    Ubiquitty::PAL::ProcessFacts& process() override { return facts; }
    int ownedFileDescriptor = -1;
    PairFacts facts;
};
struct CountingObserver final : UbiquittyKit::Session::Observer {
    void onFrame() override { frames++; }
    int frames = 0;
};

bool runProportionalFactSmoke()
{
    static constexpr std::string_view query = "\x1b]66;p=1:q=1:i=9;OK\x1b\\";
    std::array<char, 64> reply {};
    Ubiquitty::Terminal bare;
    Ubiquitty::resize(bare, 24, 80);
    if (bare.drawsProportionalText)
        return false;
    Ubiquitty::processBytes(&bare, query);
    if (Ubiquitty::drainOutput(&bare, reply) != 0)
        return false;
    Ubiquitty::Terminal drawing;
    Ubiquitty::resize(drawing, 24, 80);
    drawing.drawsProportionalText = true;
    Ubiquitty::processBytes(&drawing, query);
    const int length = Ubiquitty::drainOutput(&drawing, reply);
    if (std::string_view(reply.data(), static_cast<std::size_t>(length > 0 ? length : 0)) != "\x1b]66;i=9:c=2\x1b\\")
        return false;
    std::printf("PASS: installed package — proportional runs answered only where the renderer declares them\n");
    return true;
}

bool runKitSmoke()
{
    int pair[2] = { -1, -1 };
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0)
        return false;
    auto terminal = std::make_unique<Ubiquitty::Terminal>();
    InlineQueueDispatcher dispatcher;
    CountingObserver observer;
    auto session = UbiquittyKit::Session::createLocal(*terminal, dispatcher, observer, std::make_unique<PairStream>(pair[0]));
    // Pre-start input: a no-op — nothing may reach the far end.
    session->sendInput("early");
    char preStart[8] = {};
    if (::recv(pair[1], preStart, sizeof preStart, MSG_DONTWAIT) >= 0)
        return false;
    session->start(80, 24);
    if (::write(pair[1], "kit-ok", 6) != 6)
        return false;
    constexpr std::string_view expectedText = "kit-ok";
    std::array<Ubiquitty::Cell, expectedText.size()> receivedCells;
    bool hasReceivedText = false;
    for (int attempt = 0; attempt < 100 && !hasReceivedText; ++attempt) {
        dispatcher.drain();
        Ubiquitty::Screen::getDisplayRow(*terminal, 0, 0, receivedCells.data(), static_cast<int>(receivedCells.size()));
        hasReceivedText = true;
        for (std::size_t index = 0; index < expectedText.size(); ++index) {
            if (receivedCells[index].codePoint != static_cast<std::uint32_t>(expectedText[index])) {
                hasReceivedText = false;
                break;
            }
        }
        if (!hasReceivedText) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    if (!hasReceivedText) {
        return false;
    }
    // Destroy-before-drain: queue more bytes, destroy, then drain — the queued
    // work must degrade to a no-op (weak refs), never touch freed state.
    if (::write(pair[1], "orphan", 6) != 6)
        return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const int framesAtDestroy = observer.frames;
    session.reset();
    dispatcher.drain();
    if (observer.frames != framesAtDestroy)
        return false;
    close(pair[1]);
    std::printf("PASS: installed Kit package — local session, pre-start no-op, destroy-before-drain\n");
    return true;
}
} // namespace

int main()
{
    std::printf("find_package(UbiquittyCore) consumer — public API v%s\n",
        Ubiquitty::coreVersionString);

    Ubiquitty::Terminal term;
    Ubiquitty::resize(term, 24, 80);

    static const char kStream[] = "Hello";
    Ubiquitty::processBytes(&term, kStream);

    Ubiquitty::Cell cells[80];
    Ubiquitty::Screen::getDisplayRow(term, /*viewOffset=*/0, /*row=*/0, cells, 80);

    const char* expected = "Hello";
    bool ok = true;
    for (int i = 0; expected[i] != '\0'; ++i) {
        if (cells[i].codePoint != static_cast<uint32_t>(expected[i])) {
            ok = false;
            break;
        }
    }

    std::printf(ok ? "PASS: installed package consumed via find_package; public API works\n"
                   : "FAIL: snapshot grid mismatch\n");
    if (ok)
        ok = runProportionalFactSmoke();
    if (ok)
        ok = runKitSmoke();
    return ok ? 0 : 1;
}
