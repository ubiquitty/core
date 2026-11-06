// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// OSC 22 — the kitty mouse-pointer-shape protocol. A name table, a shape stack
// and the query reply. Shares nothing with the OSC colour surface beyond the
// dispatcher that reaches both.

#include "Core/OSC/MousePointerShape.h"

#include "Core/OSC/Internal.h"

#include <cstring>
#include <string_view>

namespace Ubiquitty::OSC {

// OSC 22 mouse pointer shape (kitty). OSC 22 ; [<op>] name[,name...] ST. Op char: none/'=' SETS (last name
// wins), '>' PUSHES onto the stack, '<' POPS one (list ignored), '?' QUERIES. Empty SET resets to Default.
// Names are CSS/X11 cursor names; directional resize shapes retain their direction.
struct PointerShapeName {
    const char* name;
    Ubiquitty::MousePointerShape shape;
};
// Sorted by name for a binary search; static_asserted below.
// CSS's names, and the X cursor-font names xterm's OSC 22 takes, which kitty's
// pointer-shape protocol accepts beside them (`left_ptr`, `xterm`, `watch`).
static constexpr PointerShapeName pointerShapeNames[] = {
    { "alias", Ubiquitty::MousePointerShape::Alias },
    { "bottom_left_corner", Ubiquitty::MousePointerShape::SouthwestResize },
    { "bottom_right_corner", Ubiquitty::MousePointerShape::SoutheastResize },
    { "bottom_side", Ubiquitty::MousePointerShape::SouthResize },
    { "cell", Ubiquitty::MousePointerShape::Cell },
    { "col-resize", Ubiquitty::MousePointerShape::ColResize },
    { "copy", Ubiquitty::MousePointerShape::Copy },
    { "crosshair", Ubiquitty::MousePointerShape::Crosshair },
    { "default", Ubiquitty::MousePointerShape::Default },
    { "e-resize", Ubiquitty::MousePointerShape::EastResize },
    { "ew-resize", Ubiquitty::MousePointerShape::ColResize },
    { "fleur", Ubiquitty::MousePointerShape::Move },
    { "grab", Ubiquitty::MousePointerShape::Grab },
    { "grabbing", Ubiquitty::MousePointerShape::Grabbing },
    { "hand1", Ubiquitty::MousePointerShape::Pointer },
    { "hand2", Ubiquitty::MousePointerShape::Pointer },
    { "help", Ubiquitty::MousePointerShape::Help },
    { "left_ptr", Ubiquitty::MousePointerShape::Default },
    { "left_ptr_watch", Ubiquitty::MousePointerShape::Progress },
    { "left_side", Ubiquitty::MousePointerShape::WestResize },
    { "move", Ubiquitty::MousePointerShape::Move },
    { "n-resize", Ubiquitty::MousePointerShape::NorthResize },
    { "ne-resize", Ubiquitty::MousePointerShape::NortheastResize },
    { "nesw-resize", Ubiquitty::MousePointerShape::NESWResize },
    { "no-drop", Ubiquitty::MousePointerShape::NoDrop },
    { "not-allowed", Ubiquitty::MousePointerShape::NotAllowed },
    { "ns-resize", Ubiquitty::MousePointerShape::RowResize },
    { "nw-resize", Ubiquitty::MousePointerShape::NorthwestResize },
    { "nwse-resize", Ubiquitty::MousePointerShape::NWSEResize },
    { "plus", Ubiquitty::MousePointerShape::Cell },
    { "pointer", Ubiquitty::MousePointerShape::Pointer },
    { "progress", Ubiquitty::MousePointerShape::Progress },
    { "question_arrow", Ubiquitty::MousePointerShape::Help },
    { "right_side", Ubiquitty::MousePointerShape::EastResize },
    { "row-resize", Ubiquitty::MousePointerShape::RowResize },
    { "s-resize", Ubiquitty::MousePointerShape::SouthResize },
    { "sb_h_double_arrow", Ubiquitty::MousePointerShape::ColResize },
    { "sb_v_double_arrow", Ubiquitty::MousePointerShape::RowResize },
    { "se-resize", Ubiquitty::MousePointerShape::SoutheastResize },
    { "sw-resize", Ubiquitty::MousePointerShape::SouthwestResize },
    { "tcross", Ubiquitty::MousePointerShape::Crosshair },
    { "text", Ubiquitty::MousePointerShape::Text },
    { "top_left_corner", Ubiquitty::MousePointerShape::NorthwestResize },
    { "top_right_corner", Ubiquitty::MousePointerShape::NortheastResize },
    { "top_side", Ubiquitty::MousePointerShape::NorthResize },
    { "vertical-text", Ubiquitty::MousePointerShape::VerticalText },
    { "w-resize", Ubiquitty::MousePointerShape::WestResize },
    { "wait", Ubiquitty::MousePointerShape::Wait },
    { "watch", Ubiquitty::MousePointerShape::Wait },
    { "xterm", Ubiquitty::MousePointerShape::Text },
    { "zoom-in", Ubiquitty::MousePointerShape::ZoomIn },
    { "zoom-out", Ubiquitty::MousePointerShape::ZoomOut },
};
template<size_t N> static constexpr bool pointerNamesSorted(const PointerShapeName (&a)[N], size_t i = 1) { return (i >= N) || (std::string_view(a[i - 1].name) < std::string_view(a[i].name) && pointerNamesSorted(a, i + 1)); }
static_assert(pointerNamesSorted(pointerShapeNames), "pointerShapeNames must be sorted by name for the binary search");

// Resolve a CSS/X11 cursor name to a portable shape. False on an unknown name — a SET then skips it
// rather than failing the whole list.
static bool resolvePointerShapeName(std::string_view name, Ubiquitty::MousePointerShape& out)
{
    constexpr int count = static_cast<int>(sizeof(pointerShapeNames) / sizeof(pointerShapeNames[0]));
    int lo = 0, hi = count - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        const int cmp = name.compare(pointerShapeNames[mid].name);
        if (cmp == 0) {
            out = pointerShapeNames[mid].shape;
            return true;
        }
        if (cmp < 0)
            hi = mid - 1;
        else
            lo = mid + 1;
    }
    return false;
}

// The portable shape → its canonical wire name, for the OSC 22 '?' query reply.
const char* pointerShapeWireName(Ubiquitty::MousePointerShape s)
{
    using S = Ubiquitty::MousePointerShape;
    switch (s) {
    case S::Default:
        return "default";
    case S::Pointer:
        return "pointer";
    case S::Text:
        return "text";
    case S::VerticalText:
        return "vertical-text";
    case S::Crosshair:
        return "crosshair";
    case S::Cell:
        return "cell";
    case S::Move:
        return "move";
    case S::Grab:
        return "grab";
    case S::Grabbing:
        return "grabbing";
    case S::NoDrop:
        return "no-drop";
    case S::NotAllowed:
        return "not-allowed";
    case S::Alias:
        return "alias";
    case S::Copy:
        return "copy";
    case S::Help:
        return "help";
    case S::Progress:
        return "progress";
    case S::Wait:
        return "wait";
    case S::ZoomIn:
        return "zoom-in";
    case S::ZoomOut:
        return "zoom-out";
    case S::ColResize:
        return "ew-resize";
    case S::RowResize:
        return "ns-resize";
    case S::NWSEResize:
        return "nwse-resize";
    case S::NESWResize:
        return "nesw-resize";
    case S::NorthResize:
        return "n-resize";
    case S::NortheastResize:
        return "ne-resize";
    case S::EastResize:
        return "e-resize";
    case S::SoutheastResize:
        return "se-resize";
    case S::SouthResize:
        return "s-resize";
    case S::SouthwestResize:
        return "sw-resize";
    case S::WestResize:
        return "w-resize";
    case S::NorthwestResize:
        return "nw-resize";
    }
    return "default";
}

namespace {

template<typename TerminalType>
auto& activePointerShapeStack(TerminalType& terminal)
{
    return terminal.screens.isUsingAlternateScreen ? terminal.alternateMousePointerShapeStack : terminal.mousePointerShapeStack;
}

template<typename TerminalType>
auto& activePointerShapeStackLength(TerminalType& terminal)
{
    return terminal.screens.isUsingAlternateScreen ? terminal.alternateMousePointerShapeStackLength : terminal.mousePointerShapeStackLength;
}

} // namespace

bool isAppPointerShapeSet(const Ubiquitty::Terminal& t) { return activePointerShapeStackLength(t) > 0; }

Ubiquitty::MousePointerShape appRequestedPointerShape(const Ubiquitty::Terminal& t)
{
    return isAppPointerShapeSet(t) ? activePointerShapeStack(t)[activePointerShapeStackLength(t) - 1] : Ubiquitty::MousePointerShape::Default;
}

// Push the resolved top of the stack out to the frontend.
void notifyMousePointerShape(Ubiquitty::Terminal& t)
{
    // The nearest capability the vocabulary has: a program telling the terminal
    // what shape to draw a cursor, which SetCursorShape's tier already sizes as
    // low-risk display metadata rather than a host escape.
    if (auto* w = Ubiquitty::effectClient(t, t.windowClient, Ubiquitty::Capability::SetCursorShape))
        w->setMousePointerShape(appRequestedPointerShape(t));
}

void handleMousePointerShape(Ubiquitty::Terminal& t, std::string_view p)
{
    constexpr int maxStackDepth = static_cast<int>(sizeof(activePointerShapeStack(t)) / sizeof(activePointerShapeStack(t)[0]));

    // Query OSC 22 ; ? name[,...] ST: reply 1 (supported)/0 per name, comma-separated, framed as OSC 22.
    // __current__ reports the stack top, or 0 for an empty stack.
    if (p.starts_with('?')) {
        const int bulkStart = t.bulkOutput.length;
        bool isReplyComplete = true;
        const auto appendReply = [&](std::string_view bytes) {
            if (isReplyComplete)
                isReplyComplete = respondBulk(t, Capability::StatusReport, { bytes.data(), bytes.size() });
        };
        appendReply("\033]22;");
        std::string_view rest = p.substr(1);
        bool first = true;
        for (;;) {
            const std::size_t comma = rest.find(',');
            const std::string_view tok = rest.substr(0, comma);
            if (!first)
                appendReply(",");
            first = false;
            const char* answer = "0";
            Ubiquitty::MousePointerShape ignored;
            if (tok == "__current__")
                answer = activePointerShapeStackLength(t) ? pointerShapeWireName(appRequestedPointerShape(t)) : "0";
            else if (tok == "__default__" || tok == "__grabbed__")
                answer = "default";
            else if (resolvePointerShapeName(tok, ignored))
                answer = "1";
            appendReply(answer);
            if (comma == std::string_view::npos)
                break;
            rest.remove_prefix(comma + 1);
        }
        appendReply(oscReplyTerminator(t));
        if (!isReplyComplete) {
            t.bulkOutput.length = bulkStart;
            if (t.bulkOutput.data)
                t.bulkOutput.data[bulkStart] = 0;
        }
        return;
    }

    // The stack is the effect: both macOS shells read its top off the
    // published snapshot, so a request a policy refuses must not be stored,
    // or a frontend reading the snapshot would apply what the client push
    // below refused. The query above is a reply and carries its own gate.
    if (!Ubiquitty::effectAllowed(t, Ubiquitty::Capability::SetCursorShape))
        return;

    // Pop: OSC 22 ; < ST — drop one entry (the name list is ignored).
    if (p.starts_with('<')) {
        if (activePointerShapeStackLength(t) > 0)
            activePointerShapeStackLength(t)--;
        notifyMousePointerShape(t);
        return;
    }

    // Push (>) vs Set (=/none). Set resets the stack to a single entry (empty Set clears it → Default);
    // Push appends each name.
    const bool isPush = p.starts_with('>');
    std::string_view names = p;
    if (isPush || p.starts_with('='))
        names.remove_prefix(1);

    if (!isPush) {
        // Set: empty payload → reset to Default (clear the stack).
        if (names.empty()) {
            activePointerShapeStackLength(t) = 0;
            notifyMousePointerShape(t);
            return;
        }
        activePointerShapeStackLength(t) = 0; // Set is not additive
    }

    // Push each resolvable name; the last ends up on top = the current shape.
    for (std::string_view rest = names;;) {
        const std::size_t comma = rest.find(',');
        const std::string_view tok = rest.substr(0, comma);
        {
            Ubiquitty::MousePointerShape shape;
            if (!tok.empty() && resolvePointerShapeName(tok, shape)) {
                if (activePointerShapeStackLength(t) >= maxStackDepth) {
                    // Overflow: drop the oldest (FIFO).
                    for (int i = 1; i < maxStackDepth; i++)
                        activePointerShapeStack(t)[i - 1] = activePointerShapeStack(t)[i];
                    activePointerShapeStackLength(t) = static_cast<uint8_t>(maxStackDepth - 1);
                }
                activePointerShapeStack(t)[activePointerShapeStackLength(t)++] = shape;
            }
        }
        if (comma == std::string_view::npos)
            break;
        rest.remove_prefix(comma + 1);
    }
    notifyMousePointerShape(t);
}

} // namespace Ubiquitty::OSC
