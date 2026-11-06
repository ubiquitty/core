// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/create_session_refusal.cpp — a session whose structure cannot be
// allocated is refused, and the daemon that refused it goes on serving.
//
// The daemon hosts every tenant in one process and builds without exceptions,
// so a throwing allocation that fails is std::terminate for all of them. The
// structural allocations behind one session — the Session record, the
// Terminal, the grid's buffers — are therefore nothrow, and a null at any of
// them refuses that one session (Docs/coding-conventions.md §6.1).
//
// Allocation failure cannot be waited for, so it is injected: the nothrow
// operator new is replaceable, and this binary replaces it with one that
// refuses a chosen allocation and grants the rest. The choice is by size,
// which is what tells the three apart — the Session record and the Terminal
// are their own sizeof, and a grid cell store is a whole number of Cells
// holding at least the geometry asked for. Refusing by size rather than by
// count is what makes each case say which allocation it covers: a budget that
// refuses everything past the Nth would refuse the PTY's own allocations just
// as well, and a session that never made it to the grid proves nothing about
// the grid.
//
// Each refusal must leave the registry as it was. The last case is the other
// half: with nothing refused, the session made holds the geometry it was asked
// for, so the refusals above did not pass by refusing to build anything.
//
// Exit 0 on success.
//

#include "Platform/Host/Daemon.h"

#include "Assertions.h"

#include "Core/CellGrid.h"
#include "Core/GridBounds.h"
#include "Core/Terminal.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>

namespace {

constexpr int columnsAsked = 80;
constexpr int rowsAsked = 24;

enum class Refuse : unsigned char {
    Nothing,
    SessionRecord,
    Terminal,
    GridCellStore,
};

Refuse refusing = Refuse::Nothing;
// Session is a private nested type; the friend seam below sets this.
std::size_t sessionRecordSize = 0;

// A whole number of Cells holding at least the geometry asked for, and not a
// size this file already names by sizeof.
//
// The shape of the store is not fixed — a grid rounds its capacity, and the
// floor and ceiling geometries allocate their own sizes — so the test cannot
// match one exact number without the refusal quietly matching nothing, which
// makes the case pass by never refusing. But the loose form alone answers for
// any struct whose size happens to divide by a Cell and clear the threshold,
// and Terminal is far past it: at 161312 bytes it is 5041 Cells on the nose,
// so a field added anywhere in the terminal turns a Terminal allocation into a
// grid store as far as this injector can tell. Both halves are needed: the
// range says what a store looks like, the exclusions say which two
// same-shaped allocations are already spoken for.
bool isGridCellStore(std::size_t size)
{
    if (size == sessionRecordSize || size == sizeof(Ubiquitty::Terminal))
        return false;
    return size % sizeof(Ubiquitty::Cell) == 0
        && size / sizeof(Ubiquitty::Cell) >= std::size_t(columnsAsked) * std::size_t(rowsAsked);
}

// Only the nothrow forms are replaced: a throwing one that failed would
// terminate, which is what this file shows cannot happen on the path it
// covers.
void* grantOrRefuse(std::size_t size) noexcept
{
    switch (refusing) {
    case Refuse::Nothing:
        break;
    case Refuse::SessionRecord:
        if (size == sessionRecordSize)
            return nullptr;
        break;
    case Refuse::Terminal:
        if (size == sizeof(Ubiquitty::Terminal))
            return nullptr;
        break;
    case Refuse::GridCellStore:
        if (isGridCellStore(size))
            return nullptr;
        break;
    }
    return std::malloc(size ? size : 1);
}

} // namespace

void* operator new(std::size_t size, const std::nothrow_t&) noexcept { return grantOrRefuse(size); }
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept { return grantOrRefuse(size); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }

namespace Ubiquitty {
namespace Host {

struct CreateSessionRefusalAccess {
    static std::size_t sessionRecordSize() { return sizeof(Daemon::Session); }
    static void useShell(Daemon& d, const std::string& path) { d.m_shellPath = path; }
    static std::size_t sessionCount(const Daemon& d) { return d.m_sessions.size(); }
    // The geometry the grid ended up holding comes back rather than the
    // session pointer.
    static bool createSession(Daemon& d, int* gridColumns, int* gridRows, int columns = columnsAsked, int rows = rowsAsked)
    {
        Daemon::Session* s = d.createSession(columns, rows, {}, -1, 0);
        if (s && s->term && s->term->grid) {
            *gridColumns = s->term->grid->columns();
            *gridRows = s->term->grid->rows();
        }
        return s != nullptr;
    }
};

} // namespace Host
} // namespace Ubiquitty

using Ubiquitty::Host::Daemon;
using A = Ubiquitty::Host::CreateSessionRefusalAccess;

int main()
{
    sessionRecordSize = A::sessionRecordSize();
    // Named sizes, not a bare verdict: the three collide by arithmetic a
    // reader cannot do in their head, and a struct grows a field for reasons
    // that have nothing to do with this file. Saying which pair met, and at
    // what numbers, is the difference between a fix and a hunt.
    char sizes[256];
    std::snprintf(sizes, sizeof(sizes),
        "the three structural sizes are not distinct, so a refusal cannot name its target: "
        "Session %zu, Terminal %zu, Cell %zu, a store of %dx%d is %zu; "
        "Session-is-store %d, Terminal-is-store %d, Session-equals-Terminal %d",
        sessionRecordSize, sizeof(Ubiquitty::Terminal), sizeof(Ubiquitty::Cell),
        columnsAsked, rowsAsked,
        sizeof(Ubiquitty::Cell) * std::size_t(columnsAsked) * std::size_t(rowsAsked),
        int(isGridCellStore(sessionRecordSize)), int(isGridCellStore(sizeof(Ubiquitty::Terminal))),
        int(sessionRecordSize == sizeof(Ubiquitty::Terminal)));
    CHECK_MSG(sessionRecordSize != sizeof(Ubiquitty::Terminal) && !isGridCellStore(sessionRecordSize) && !isGridCellStore(sizeof(Ubiquitty::Terminal)),
        sizes);

    Daemon d;
    A::useShell(d, "/bin/sh");

    struct Case {
        Refuse refuse;
        const char* what;
    };
    const Case cases[] = {
        { Refuse::SessionRecord, "the Session record" },
        { Refuse::Terminal, "the Terminal" },
        { Refuse::GridCellStore, "a grid cell store" },
    };
    for (const Case& c : cases) {
        int gridColumns = 0;
        int gridRows = 0;
        refusing = c.refuse;
        const bool wasMade = A::createSession(d, &gridColumns, &gridRows);
        refusing = Refuse::Nothing;
        if (wasMade)
            std::fprintf(stderr, "create_session_refusal: %s refused, yet a session was made holding %d by %d\n", c.what, gridColumns, gridRows);
        CHECK_MSG(!wasMade, "a refused structural allocation did not refuse the session");
        CHECK_MSG(A::sessionCount(d) == 0, "a refused session was left in the registry");
    }

    // Nothing refused: the same request makes a session holding its geometry.
    {
        int gridColumns = 0;
        int gridRows = 0;
        CHECK_MSG(A::createSession(d, &gridColumns, &gridRows), "with nothing refused the session was not made");
        CHECK(A::sessionCount(d) == 1);
        CHECK_MSG(gridColumns == columnsAsked && gridRows == rowsAsked, "the session made does not hold the geometry it was asked for");
    }

    // A PTY smaller than Core represents is not a refusal: the grid holds the
    // floor Core applies to the request, and the session is made around it.
    {
        int gridColumns = 0;
        int gridRows = 0;
        CHECK_MSG(A::createSession(d, &gridColumns, &gridRows, 1, 1), "a one-cell PTY was refused");
        CHECK(A::sessionCount(d) == 2);
        CHECK_MSG(gridColumns == Ubiquitty::minColumns && gridRows == Ubiquitty::minRows, "a one-cell PTY's grid does not hold the floor Core applies");
    }

    // Nor is a request past the ceiling: the grid holds the ceiling Core
    // applies, and the session is made around it.
    {
        int gridColumns = 0;
        int gridRows = 0;
        CHECK_MSG(A::createSession(d, &gridColumns, &gridRows, Ubiquitty::maxColumns + 1000, Ubiquitty::maxRows + 1000), "a PTY past the ceiling was refused");
        CHECK(A::sessionCount(d) == 3);
        CHECK_MSG(gridColumns == Ubiquitty::maxColumns && gridRows == Ubiquitty::maxRows, "a past-the-ceiling PTY's grid does not hold the ceiling Core applies");
    }

    if (failures == 0)
        std::printf("create_session_refusal: a refused Session record, Terminal and grid cell store each refuse the session and leave the registry empty; unrefused, the session holds %d by %d, a one-cell PTY holds the %d by %d floor, and a PTY past the ceiling holds %d by %d\n",
            columnsAsked, rowsAsked, Ubiquitty::minColumns, Ubiquitty::minRows, Ubiquitty::maxColumns, Ubiquitty::maxRows);
    return UBIQUITTY_TEST_SUMMARY() == 0 ? 0 : 1;
}
