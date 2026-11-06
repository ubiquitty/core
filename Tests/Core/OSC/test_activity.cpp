// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "ScreenPlaneFixture.h"
#include "TerminalFixture.h"

#include "Core/App/ProgramActivity.h"
#include "Core/Codec/Base64.h"
#include "Core/OSC/Activity.h"
#include "Core/OSC/Dispatch.h"
#include "Core/Protocol/Sync/ScreenPlane.h"
#include "Core/Terminfo.h"

using namespace Ubiquitty;
using namespace Ubiquitty::OSC;
using UbiquittyTest::feed;

namespace {
void osc(Terminal& terminal, std::string_view body)
{
    feed(terminal, "\033]" + std::string(body) + "\033\\");
}
std::string encoded(std::string_view text)
{
    std::string result(Codec::base64EncodedLength(text.size()), '\0');
    CHECK(Codec::base64Encode(std::span<const char>(text), std::span<char>(result)).has_value());
    return result;
}
}

TEST(contextNestingReplacementAndTermination)
{
    Terminal terminal;
    osc(terminal, "3008;start=shell;type=shell;user=alice;cwd=/tmp");
    osc(terminal, "3008;start=elevated;targetuser=root;type=elevate");
    osc(terminal, "3008;start=command;cmdline=echo a\\x3bb\\x5cc;pid=18446744073709551615");
    CHECK_EQ(terminal.contexts.active.size(), 3u);
    CHECK_STR_EQ(terminal.contexts.active.back().value(ContextField::CommandLine), "echo a;b\\c");
    osc(terminal, "3008;end=elevated;exit=failure;status=2;signal=SIGTERM");
    CHECK_EQ(terminal.contexts.active.size(), 1u);
    CHECK(terminal.contexts.lastEnded.has_value());
    CHECK_STR_EQ(terminal.contexts.lastEnded->value(ContextField::TargetUser), "root");
    CHECK_STR_EQ(terminal.contexts.lastEnded->value(ContextField::Status), "2");
    osc(terminal, "3008;start=shell;hostname=host;pid=18446744073709551616;type=unknown;user=bob;status=5");
    CHECK_EQ(terminal.contexts.active.size(), 1u);
    CHECK_STR_EQ(terminal.contexts.active[0].value(ContextField::User), "bob");
    CHECK(terminal.contexts.active[0].value(ContextField::WorkingDirectory).empty());
    CHECK(terminal.contexts.active[0].value(ContextField::Type).empty());
    CHECK(terminal.contexts.active[0].value(ContextField::PID).empty());
    CHECK(terminal.contexts.active[0].value(ContextField::Status).empty());
    const auto before = terminal.contexts;
    osc(terminal, "3008;end=unknown");
    osc(terminal, "3008;start=bad\\escape");
    CHECK(terminal.contexts == before);
}

TEST(contextBoundsAndResetSafety)
{
    Terminal terminal;
    for (std::size_t index = 0; index < contextDepthLimit + 10; ++index) {
        osc(terminal, "3008;start=" + std::to_string(index));
    }
    CHECK_EQ(terminal.contexts.active.size(), contextDepthLimit);
    feed(terminal, "\033c\033[!p\033[?1049h\033[?1049l");
    CHECK_EQ(terminal.contexts.active.size(), contextDepthLimit);
    osc(terminal, "3008;start=0;comm=" + std::string(256, 'x') + ";user=ok");
    CHECK_EQ(terminal.contexts.active.size(), 1u);
    CHECK(terminal.contexts.active.front().value(ContextField::Command).empty());
    CHECK_STR_EQ(terminal.contexts.active.front().value(ContextField::User), "ok");
    osc(terminal, "3008;end=0");
    CHECK(terminal.contexts.active.empty());
}

TEST(programFeatureDetectionAndReports)
{
    Terminal terminal;
    feed(terminal, "\033]7501;?\a");
    std::array<char, 64> response {};
    UbiquittyTest::drain(terminal, response);
    CHECK_STR_EQ(std::string(response.data()), "\033]7501;?\a");
    osc(terminal, "7501;state=idle:app=build");
    osc(terminal, "7501;state=working:id=compile:progress=33:title=" + encoded("Compile"));
    osc(terminal, "7501;state=blocked:id=compile/worker:kind=permission:msg=" + encoded("Allow this?"));
    CHECK_EQ(terminal.programStatus.records.size(), 3u);
    const auto* primary = terminal.programStatus.primary();
    CHECK(primary);
    CHECK_STR_EQ(primary->message, "Allow this?");
    CHECK(primary->kind == ProgramBlockKind::Permission);
    CHECK_STR_EQ(terminal.programStatus.effectiveApp(*primary), "build");
    osc(terminal, "7501;state=working:id=compile/worker:progress=70:progress=90");
    CHECK_EQ(terminal.programStatus.records.back().progress, 90);
    CHECK(terminal.programStatus.records.back().message.empty());
    CHECK(terminal.programStatus.records.back().kind == ProgramBlockKind::None);
    osc(terminal, "7501;state=clear:id=compile");
    CHECK_EQ(terminal.programStatus.records.size(), 1u);
    osc(terminal, "7501;state=clear");
    CHECK(terminal.programStatus.records.empty());
}

TEST(programReportsAreAtomicAndBounded)
{
    Terminal terminal;
    osc(terminal, "7501;state=done:app=good");
    const auto before = terminal.programStatus;
    for (const auto body : {
             "state=working:msg=A", "state=working:msg=AA==", "state=working:msg=woA=", "state=working:msg=wK8=",
             "state=working:id=", "state=working:id=a//b", "state=working:id=/a", "state=working:id=a/",
             "state=working:id=a/b/c/d/e/f/g/h/i", "state=working:msg=AA==:msg=b2s=", "state=unknown", "msg=b2s=",
             "state=clear:msg=bad===", "state=working:abcdefghijklmnopq=x" }) {
        osc(terminal, "7501;" + std::string(body));
        CHECK(terminal.programStatus == before);
    }
    osc(terminal, "7501;state=working:app=" + std::string(33, 'a') + ":app=ok");
    CHECK(terminal.programStatus == before);
    osc(terminal, "7501;state=working:msg=" + encoded(std::string(2049, 'x')));
    CHECK(terminal.programStatus == before);
    osc(terminal, "7501;state=working:x=" + std::string(4096, 'x'));
    CHECK(terminal.programStatus == before);
    osc(terminal, "7501;broken:state = working :unknown=x:progress=101:kind=auth:msg=" + encoded("Привет"));
    CHECK_STR_EQ(terminal.programStatus.records.front().message, "Привет");
    CHECK_EQ(terminal.programStatus.records.front().progress, -1);
    CHECK(terminal.programStatus.records.front().kind == ProgramBlockKind::None);
}

TEST(programLifetimesAndEviction)
{
    Terminal terminal;
    for (const auto state : { "working", "blocked", "done", "error", "idle" }) {
        osc(terminal, "7501;state=" + std::string(state) + ":id=" + state);
    }
    feed(terminal, "\033[?1049h\033[!p\033[?1049l");
    CHECK_EQ(terminal.programStatus.records.size(), 5u);
    osc(terminal, "133;A");
    CHECK_EQ(terminal.programStatus.records.size(), 3u);
    osc(terminal, "7501;state=blocked:id=worker");
    finishProgramActivity(terminal);
    CHECK_EQ(terminal.programStatus.records.size(), 3u);
    feed(terminal, "\033c");
    CHECK(terminal.programStatus.records.empty());
    for (std::size_t index = 0; index < programStatusLimit; ++index) {
        osc(terminal, "7501;state=done:id=" + std::to_string(index));
    }
    osc(terminal, "7501;state=done:id=0");
    osc(terminal, "7501;state=done:id=new");
    CHECK_EQ(terminal.programStatus.records.size(), programStatusLimit);
    CHECK_STR_EQ(terminal.programStatus.records.front().id, "2");
    CHECK_STR_EQ(terminal.programStatus.records[programStatusLimit - 2].id, "0");
}

TEST(activitySurvivesSyncAndRejectsTruncation)
{
    using namespace Protocol::Sync;
    Terminal host, client;
    resize(host, 2, 4);
    resize(client, 2, 4);
    osc(host, "3008;start=shell;type=shell");
    osc(host, "3008;start=job;type=command");
    osc(host, "3008;end=job;exit=failure;status=3");
    osc(host, "7501;state=blocked:kind=question:app=build:msg=" + encoded("Continue?"));
    const std::vector<std::uint8_t> body = UbiquittyTest::checkpointBody(host);
    REQUIRE(!body.empty());
    ScreenReplica replica;
    const std::uint64_t generationBefore = client.activityGeneration;
    CHECK(applyCheckpoint(client, body, replica) == ApplyOutcome::Applied);
    CHECK(client.contexts == host.contexts);
    CHECK(client.programStatus == host.programStatus);
    CHECK(client.activityGeneration != generationBefore);
    const auto contexts = client.contexts;
    const auto statuses = client.programStatus;
    for (std::size_t size = 0; size < body.size(); ++size) {
        ScreenReplica refused;
        CHECK(applyCheckpoint(client, std::span(body).first(size), refused) != ApplyOutcome::Applied);
        CHECK(client.contexts == contexts);
        CHECK(client.programStatus == statuses);
    }
    host.contexts = {};
    host.programStatus = {};
    CHECK(UbiquittyTest::copyThroughCheckpoint(host, client) == ApplyOutcome::Applied);
    CHECK(client.contexts.active.empty());
    CHECK(!client.contexts.lastEnded);
    CHECK(client.programStatus.records.empty());
}

TEST(activityPresentationAndSequenceGates)
{
    Terminal terminal;
    const int denied[] = { 3008, 7501 };
    CHECK(setOSCDenyList(terminal, denied, 2));
    osc(terminal, "3008;start=blocked");
    osc(terminal, "7501;?");
    osc(terminal, "7501;state=working");
    CHECK(terminal.contexts.active.empty());
    CHECK(terminal.programStatus.records.empty());
    std::array<char, 64> response {};
    CHECK_EQ(UbiquittyTest::drain(terminal, response), 0);
    CHECK(setOSCDenyList(terminal, nullptr, 0));
    // tmux's DCS envelope doubles ESC inside its OSC payload.
    feed(terminal, "\033Ptmux;\033\033]7501;state=working:app=build:progress=42\033\033\\\033\\");
    REQUIRE(!terminal.programStatus.records.empty());
    CHECK_STR_EQ(App::programActivityLabel(terminal.programStatus), "build: Working 42%");
    osc(terminal, "7501;state=blocked:kind=permission:app=build:msg=" + encoded("OK\xe2\x80\xae.txt"));
    CHECK_STR_EQ(terminal.programStatus.records.front().message, "OK\xe2\x80\xae.txt");
    CHECK_STR_EQ(App::programActivityLabel(terminal.programStatus), "build: Approval needed — OK.txt");
    CHECK_MSG(App::programActivityText(terminal.programStatus) == "OK.txt", "a window title's status text is the message alone, cleaned as the label cleans it");
    const auto capability = Terminfo::lookup("Pst");
    REQUIRE(capability.has_value());
    CHECK_STR_EQ(*capability, "\033]7501;%p1%s\033\\");
}

TEST(programActivityTextIsWhatTheProgramSaysAndNothingItsMarkCarries)
{
    Terminal terminal;
    CHECK_MSG(App::programActivityText(terminal.programStatus).empty(), "no record says nothing");
    osc(terminal, "7501;state=working:app=build:progress=40");
    CHECK_MSG(App::programActivityText(terminal.programStatus).empty(), "a record with neither title nor message says nothing: its name, state and percent are the mark's");
    osc(terminal, "7501;state=working:app=build:progress=40:msg=" + encoded("Re-running the branch"));
    CHECK_STR_EQ(App::programActivityText(terminal.programStatus), "Re-running the branch");
    osc(terminal, "7501;state=working:app=build:title=" + encoded("Tests") + ":msg=" + encoded("Re-running the branch"));
    CHECK_STR_EQ(App::programActivityText(terminal.programStatus), "Tests — Re-running the branch");
    osc(terminal, "7501;state=blocked:kind=question:title=" + encoded("Which file?"));
    CHECK_STR_EQ(App::programActivityText(terminal.programStatus), "Which file?");
    const std::string message(400, 'x');
    osc(terminal, "7501;state=working:msg=" + encoded(message));
    CHECK_MSG(App::programActivityText(terminal.programStatus) == message, "the text is whole, not the snapshot's few dozen bytes");
}

TEST(activityFragmentationAndMaximumText)
{
    Terminal terminal;
    const std::string message(2048, 'x');
    const std::string report = "\033]7501;state=working:msg=" + encoded(message) + "\033\\";
    for (char byte : report) {
        feed(terminal, std::string_view(&byte, 1));
    }
    REQUIRE(!terminal.programStatus.records.empty());
    CHECK_STR_EQ(terminal.programStatus.records.front().message, message);
    osc(terminal, "7501;state=idle:state=done:id=bad//path:id=good");
    CHECK_STR_EQ(terminal.programStatus.records.back().id, "good");
    CHECK(terminal.programStatus.records.back().state == ProgramState::Done);
    const auto before = terminal.programStatus;
    for (const auto id : { "bad^id", "a,b", "a=b" }) {
        osc(terminal, "7501;state=clear:id=" + std::string(id));
        CHECK(terminal.programStatus == before);
    }
    osc(terminal, "3008;start=ctx;user=bad\nname;hostname=ok;comm=\xff");
    REQUIRE(!terminal.contexts.active.empty());
    CHECK(terminal.contexts.active.back().value(ContextField::User).empty());
    CHECK(terminal.contexts.active.back().value(ContextField::Command).empty());
    CHECK_STR_EQ(terminal.contexts.active.back().value(ContextField::Hostname), "ok");
}

// Activity that starts after the attach reaches the client in an update, not
// only in a checkpoint: a frontend's job list and program status follow the
// host while the client stays attached, and they end there when they end on
// the host.
TEST(activityTravelsInAnUpdate)
{
    using namespace Protocol::Sync;
    Terminal host, client;
    resize(host, 2, 4);
    resize(client, 2, 4);
    ScreenPublisher publisher;
    AttachmentBaseline attachment;
    std::vector<std::uint8_t> body;
    REQUIRE(publisher.capture(host) && publisher.checkpoint(host, 1, body, attachment));
    ScreenReplica replica;
    REQUIRE(applyCheckpoint(client, body, replica) == ApplyOutcome::Applied);

    const auto update = [&] {
        body.clear();
        if (!publisher.capture(host) || publisher.rowUpdate(host, attachment, body) != UpdateOutcome::Encoded)
            return ApplyOutcome::Malformed;
        return applyRowUpdate(client, body, replica);
    };
    osc(host, "3008;start=job;type=command");
    osc(host, "7501;state=blocked:kind=question:app=build:msg=" + encoded("Continue?"));
    const std::uint64_t generationBefore = client.activityGeneration;
    CHECK(update() == ApplyOutcome::Applied);
    CHECK_MSG(client.contexts == host.contexts, "a context begun after the attach reaches the client");
    CHECK_MSG(client.programStatus == host.programStatus, "and so does a program's status");
    CHECK_MSG(client.activityGeneration != generationBefore, "and the client is told its activity moved");

    osc(host, "3008;end=job;exit=success");
    host.programStatus = {};
    CHECK(update() == ApplyOutcome::Applied);
    CHECK_MSG(client.contexts == host.contexts, "a context that ends on the host ends on the client");
    CHECK_MSG(client.programStatus.records.empty(), "and a cleared status clears there");
}

TEST(syncRefusesInvalidActivityWithoutMutation)
{
    using namespace Protocol::Sync;
    Terminal host, client;
    resize(host, 2, 4);
    resize(client, 2, 4);
    osc(client, "3008;start=kept");
    osc(client, "7501;state=done:app=kept");
    const auto contexts = client.contexts;
    const auto statuses = client.programStatus;
    osc(host, "7501;state=working");
    host.programStatus.records.front().message = "\x01";
    CHECK(UbiquittyTest::copyThroughCheckpoint(host, client) != ApplyOutcome::Applied);
    CHECK(client.contexts == contexts);
    CHECK(client.programStatus == statuses);
}

UBIQUITTY_TEST_MAIN()
