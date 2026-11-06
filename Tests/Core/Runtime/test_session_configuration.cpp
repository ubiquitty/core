// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_session_configuration.cpp — a session reads the preferences it was
// handed, not the process-wide instance (Core/Runtime/WorkspaceSession.h,
// Core/Runtime/TerminalSession.h).
//
// Two models live in one process under two configurations, and each launch
// resolves from its own: the identity the child's environment is stamped with
// and the scrollback the terminal keeps. The child prints TERM_PROGRAM back
// through the PTY, so the assertion is on what the process actually received,
// read off the grid. Against a process-wide read both sessions would answer
// with the same identity and the same limit, and the divergence asserted here
// cannot hold.

#include "Core/Runtime/WorkspaceSession.h"

#include "Core/CellGrid.h"
#include "Core/OSC/TextSizing.h"
#include "Core/Preferences/Preferences.h"
#include "Core/Runtime/LaunchCommand.h"
#include "Core/Terminal.h"

#include <memory>
#include <string>
#include <unistd.h>

using namespace Ubiquitty::Runtime;

namespace {

struct ModelHandle {
    WorkspaceSession* model = nullptr;
    explicit ModelHandle(const Ubiquitty::Preferences::Values& configuration)
        : model(WorkspaceSession::make(WorkspaceSession::IngestPolicy::ThreadDrained, DrawnText::SizedAndProportional, configuration))
    {
    }
    ~ModelHandle()
    {
        if (model != nullptr)
            releaseWorkspaceSession(model);
    }
};

// The child echoes the identity it was launched under, then holds the PTY
// open long enough for the drain to land the bytes.
LaunchCommand echoTerminalProgram()
{
    LaunchCommand command = LaunchCommand::fromArguments({ "/bin/sh", "-c", "printf '%s' \"$TERM_PROGRAM\"; sleep 2" });
    command.setArgument0Style(Ubiquitty::PAL::Argument0Style::ProgramPath);
    return command;
}

// The first row's text, trimmed of trailing blanks, or empty with no model.
std::string firstRowText(TerminalSession& session)
{
    std::string text;
    session.lockForRead();
    if (Ubiquitty::Terminal* term = session.terminal()) {
        for (int column = 0; column < term->columns; ++column) {
            const Ubiquitty::Cell* cell = term->grid->cellAt(Ubiquitty::Row(0), Ubiquitty::Column(column));
            if (cell == nullptr || cell->codePoint == 0 || cell->codePoint == ' ')
                break;
            text.push_back(static_cast<char>(cell->codePoint));
        }
    }
    session.unlockForRead();
    return text;
}

int scrollbackLimit(TerminalSession& session)
{
    int limit = 0;
    session.lockForRead();
    if (Ubiquitty::Terminal* term = session.terminal())
        limit = term->scrollbackLimit;
    session.unlockForRead();
    return limit;
}

template<typename Condition>
bool waitFor(Condition condition, int budgetMilliseconds)
{
    for (int waited = 0; waited < budgetMilliseconds; waited += 10) {
        if (condition())
            return true;
        ::usleep(10 * 1000);
    }
    return condition();
}

} // namespace

TEST(twoSessionsInOneProcessHoldDivergentIdentityAndScrollback)
{
    Ubiquitty::Preferences::Values alpha;
    alpha.termProgram = "alpha-host";
    alpha.scrollbackLines = 500;
    Ubiquitty::Preferences::Values beta;
    beta.termProgram = "beta-host";
    beta.scrollbackLines = 2000;
    CHECK_MSG(alpha.termProgram != Ubiquitty::Preferences::values().termProgram
            || beta.termProgram != Ubiquitty::Preferences::values().termProgram,
        "at least one fixture differs from the process-wide identity, or the case cannot fail");

    ModelHandle alphaHandle(alpha);
    ModelHandle betaHandle(beta);
    REQUIRE(alphaHandle.model != nullptr);
    REQUIRE(betaHandle.model != nullptr);
    WorkspaceSession& alphaModel = *alphaHandle.model;
    WorkspaceSession& betaModel = *betaHandle.model;

    const auto alphaSurface = alphaModel.addWindowSurface(echoTerminalProgram());
    const auto betaSurface = betaModel.addWindowSurface(echoTerminalProgram());
    REQUIRE(alphaSurface != Ubiquitty::Session::Layout::invalidSurface);
    REQUIRE(betaSurface != Ubiquitty::Session::Layout::invalidSurface);

    TerminalSession* alphaSession = alphaModel.session(alphaModel.focusedPane(alphaSurface));
    TerminalSession* betaSession = betaModel.session(betaModel.focusedPane(betaSurface));
    REQUIRE(alphaSession != nullptr);
    REQUIRE(betaSession != nullptr);

    // Both limits are read while both sessions are alive: the point is that
    // they hold at the same time, in the same process.
    CHECK_EQ(scrollbackLimit(*alphaSession), 500);
    CHECK_EQ(scrollbackLimit(*betaSession), 2000);

    CHECK_MSG(waitFor([&] { return firstRowText(*alphaSession) == "alpha-host"; }, 5000),
        "the alpha child was launched under alpha's identity");
    CHECK_MSG(waitFor([&] { return firstRowText(*betaSession) == "beta-host"; }, 5000),
        "the beta child was launched under beta's identity");
}

TEST(defaultSessionIdentifiesUbiquittyToTheChild)
{
    const Ubiquitty::Preferences::Values configuration;
    ModelHandle handle(configuration);
    REQUIRE(handle.model != nullptr);
    const auto surface = handle.model->addWindowSurface(echoTerminalProgram());
    REQUIRE(surface != Ubiquitty::Session::Layout::invalidSurface);
    TerminalSession* session = handle.model->session(handle.model->focusedPane(surface));
    REQUIRE(session != nullptr);
    CHECK_MSG(waitFor([&] { return firstRowText(*session) == "Ubiquitty"; }, 5000),
        "a default session exports TERM_PROGRAM=Ubiquitty to its child");
}

TEST(sessionStartDeclaresBothRendererCapabilitiesIndependently)
{
    struct Declaration {
        DrawnText drawnText;
        bool drawsSized;
        bool drawsProportional;
    };
    for (const auto declaration : { Declaration { DrawnText::GridOnly, false, false },
             Declaration { DrawnText::Sized, true, false },
             Declaration { DrawnText::Proportional, false, true },
             Declaration { DrawnText::SizedAndProportional, true, true } }) {
        std::unique_ptr<TerminalSession, decltype(&releaseTerminalSession)> session(TerminalSession::make(), releaseTerminalSession);
        REQUIRE(session);
        session->start(4, 20, declaration.drawnText);
        const auto* terminal = session->terminal();
        REQUIRE(terminal);
        CHECK(terminal->drawsSizedText == declaration.drawsSized);
        CHECK(terminal->drawsProportionalText == declaration.drawsProportional);
        CHECK(Ubiquitty::OSC::TextSizing::mayOfferFaceFacts(*terminal) == declaration.drawsProportional);
    }
}

UBIQUITTY_TEST_MAIN()
