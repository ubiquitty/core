// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Unicode/Joining.h"
#include "Core/Unicode/RangeTable.h"
#include <cstddef>

// Arabic cursive joining. Tables live in Joining.tables.inc (curated UCD
// snapshot); see Joining.h for the joining-type / form model.

namespace {

using Ubiquitty::Unicode::JoiningForm;
using Ubiquitty::Unicode::JoiningType;

#include "Core/Unicode/Joining.tables.inc"

constexpr int joinN = static_cast<int>(sizeof(joiningRanges) / sizeof(joiningRanges[0]));
constexpr int formN = static_cast<int>(sizeof(presFormB) / sizeof(presFormB[0]));
constexpr std::uint32_t lamCodePoint = 0x0644; // ARABIC LETTER LAM — the ligating half of lam-alef

// Binary search the sorted joining ranges; default Non_Joining.
JoiningType rawJoiningType(std::uint32_t codePoint)
{
    const auto* range = Ubiquitty::Unicode::findRange(joiningRanges, joinN, codePoint);
    return range ? range->type : JoiningType::NonJoining;
}

const PresFormB* findForm(std::uint32_t codePoint) { return Ubiquitty::Unicode::findByKey(presFormB, formN, &PresFormB::base, codePoint); }

// A letter that itself takes a contextual shape: Dual, Right, or Left.
// Join_Causing / Transparent / Non_Joining take no shape of their own.
bool isShapingLetter(JoiningType t) { return t == JoiningType::DualJoining || t == JoiningType::RightJoining || t == JoiningType::LeftJoining; }

// LOGICAL order throughout: "preceding" = earlier index, "following" = later.

// PRECEDING neighbour connects forward into the current letter: Dual, Left, or
// Join_Causing. (Right_Joining only joins backward.)
bool precedingConnects(JoiningType n) { return n == JoiningType::DualJoining || n == JoiningType::LeftJoining || n == JoiningType::JoinCausing; }

// FOLLOWING neighbour connects backward into the current letter: Dual, Right, or
// Join_Causing. (Left_Joining only joins forward.)
bool followingConnects(JoiningType n) { return n == JoiningType::DualJoining || n == JoiningType::RightJoining || n == JoiningType::JoinCausing; }

} // namespace

namespace Ubiquitty::Unicode {

JoiningType joiningType(std::uint32_t codePoint) { return rawJoiningType(codePoint); }

bool isCursiveJoiner(std::uint32_t codePoint)
{
    // Script-neutral joining controls first (outside the Arabic blocks).
    if (codePoint == 0x200C || codePoint == 0x200D)
        return true;
    // Arabic, Arabic Supplement, Arabic Extended-A.
    if (codePoint < 0x0600 || codePoint > 0x08FF)
        return false;
    return rawJoiningType(codePoint) != JoiningType::NonJoining;
}

JoiningForm resolveForm(std::uint32_t codePoint, JoiningType prevType, JoiningType nextType)
{
    const JoiningType self = joiningType(codePoint);
    if (!isShapingLetter(self))
        return JoiningForm::Isolated;

    // joinsBack: connects to the preceding letter (the "final"-form edge).
    // joinsFwd:  connects to the following letter (the "initial"-form edge).
    bool joinsBack = precedingConnects(prevType);
    bool joinsFwd = followingConnects(nextType);

    // A Right_Joining letter (alef, dal, reh, waw) takes no forward join;
    // a Left_Joining letter takes no backward join.
    if (self == JoiningType::RightJoining)
        joinsFwd = false;
    if (self == JoiningType::LeftJoining)
        joinsBack = false;

    if (joinsBack && joinsFwd)
        return JoiningForm::Medial;
    if (joinsBack)
        return JoiningForm::Final;
    if (joinsFwd)
        return JoiningForm::Initial;
    return JoiningForm::Isolated;
}

std::uint32_t presentationForm(std::uint32_t codePoint, JoiningForm form)
{
    const PresFormB* f = findForm(codePoint);
    if (!f)
        return codePoint;
    std::uint32_t outputBuffer = 0;
    switch (form) {
    case JoiningForm::Isolated:
        outputBuffer = f->isol;
        break;
    case JoiningForm::Final:
        outputBuffer = f->fina;
        break;
    case JoiningForm::Initial:
        outputBuffer = f->init;
        break;
    case JoiningForm::Medial:
        outputBuffer = f->medi;
        break;
    }
    // A missing shape (dual form on a right-joining letter, etc.) falls
    // back to the isolated form, then to the base codePoint.
    if (outputBuffer == 0)
        outputBuffer = f->isol;
    if (outputBuffer == 0)
        outputBuffer = codePoint;
    return outputBuffer;
}

std::uint32_t lamAlefLigature(std::uint32_t alef, LamAlefLigatureForm form)
{
    for (auto pair : lamAlef) {
        if (pair.alef == alef)
            return (form == LamAlefLigatureForm::Final) ? pair.fina : pair.isol;
    }
    return 0;
}

void shapeRun(const std::uint32_t* in, std::uint32_t* outputBuffer, int n)
{
    if (n <= 0)
        return;

    // Copy through first so non-letter cells appear in `out`; letters get
    // rewritten below.
    for (int i = 0; i < n; ++i)
        outputBuffer[i] = in[i];

    // The forward neighbour is scanned from `in` (pristine ahead of the cursor
    // even when `out` aliases `in`); the backward neighbour is carried in
    // prevType. So no already-rewritten cell is ever re-read — this is what makes
    // in-place shaping (out == in) correct.
    JoiningType prevType = JoiningType::NonJoining;
    int i = 0;
    while (i < n) {
        const std::uint32_t codePoint = in[i];
        const JoiningType self = joiningType(codePoint);

        // Marks are transparent to joining: pass through, leave prevType.
        if (self == JoiningType::Transparent) {
            ++i;
            continue;
        }
        // Non-joining runs (space/Latin/digits, ZWNJ) pass through but reset
        // the left-join context to themselves.
        if (!isShapingLetter(self)) {
            prevType = self;
            ++i;
            continue;
        }

        // Nearest non-Transparent neighbour to the right (pristine ahead).
        JoiningType nextType = JoiningType::NonJoining;
        int nextIndex = -1;
        std::uint32_t nextCodePoint = 0;
        for (int j = i + 1; j < n; ++j) {
            JoiningType t = joiningType(in[j]);
            if (t == JoiningType::Transparent)
                continue;
            nextType = t;
            nextIndex = j;
            nextCodePoint = in[j];
            break;
        }

        // Lam + a ligating alef is a MANDATORY ligature (ﻻ): one glyph. The form
        // is FINAL when the lam connects to a preceding letter, else ISOLATED.
        // Emit the ligature at the ALEF cell and collapse the LAM cell to 0: after
        // RTL reversal the alef is the lower visual column (wide-lead), the lam its
        // wide-continuation, so the glyph spans both cells with no gap.
        if (codePoint == lamCodePoint && nextIndex >= 0) {
            const JoiningForm lamForm = resolveForm(codePoint, prevType, nextType);
            const bool lamJoinsBack = (lamForm == JoiningForm::Medial || lamForm == JoiningForm::Final);
            const std::uint32_t ligature = lamAlefLigature(nextCodePoint, lamJoinsBack ? LamAlefLigatureForm::Final : LamAlefLigatureForm::Isolated);
            if (ligature != 0) {
                outputBuffer[nextIndex] = ligature; // ligature at the alef cell (visual lead)
                outputBuffer[i] = 0; // lam cell collapses to the wide-continuation
                // The alef terminates the ligature and never joins forward, so the
                // next letter sees the alef's (right-joining) context.
                prevType = JoiningType::RightJoining;
                i = nextIndex + 1;
                continue;
            }
        }

        outputBuffer[i] = presentationForm(codePoint, resolveForm(codePoint, prevType, nextType));
        prevType = self;
        ++i;
    }
}

bool isStrongRTL(std::uint32_t codePoint)
{
    // Hebrew (incl. its presentation forms at FB1D–FB4F).
    if (codePoint >= 0x0590 && codePoint <= 0x05FF)
        return true;
    // The Arabic-Indic digit subranges (0660-0669, 06F0-06F9) are weak (bidi
    // class AN), not strong RTL — exclude so a digit doesn't anchor its own
    // reversal.
    if (codePoint >= 0x0660 && codePoint <= 0x0669)
        return false;
    if (codePoint >= 0x06F0 && codePoint <= 0x06F9)
        return false;
    if (codePoint >= 0x0600 && codePoint <= 0x06FF)
        return true;
    if (codePoint >= 0x0750 && codePoint <= 0x077F)
        return true;
    if (codePoint >= 0x08A0 && codePoint <= 0x08FF)
        return true;
    // Arabic Presentation Forms-A and -B (the forms shapeRun emits).
    if (codePoint >= 0xFB1D && codePoint <= 0xFDFF)
        return true;
    if (codePoint >= 0xFE70 && codePoint <= 0xFEFF)
        return true;
    return false;
}

// BiDi neutral: whitespace takes the surrounding strong direction (N1 rule), so
// an interior space stays inside an RTL run.
static bool isBidiNeutral(std::uint32_t codePoint)
{
    return codePoint == 0x0020 // SPACE
        || codePoint == 0x0009 // TAB
        || codePoint == 0x00A0 // no-break space
        || codePoint == 0x202F; // Narrow no-break space
}

bool shapeVisualRow(const std::uint32_t* logical, std::uint32_t* outCodePoints, std::uint16_t* outLogical, int n)
{
    if (n <= 0)
        return false;

    // Identity outputs first; the RTL passes below overwrite only what
    // they reorder, and the early-out path returns these untouched.
    bool hasShaping = false;
    for (int i = 0; i < n; ++i) {
        outCodePoints[i] = logical[i];
        outLogical[i] = static_cast<std::uint16_t>(i);
        if (isCursiveJoiner(logical[i]) || isStrongRTL(logical[i]))
            hasShaping = true;
    }
    if (!hasShaping)
        return false;

    // Contextual joining in LOGICAL order. shapeRun is in-place-safe, so
    // shape outCodePoints over itself.
    shapeRun(outCodePoints, outCodePoints, n);

    // Reverse each maximal contiguous strong-RTL run into visual order,
    // absorbing interior Transparent marks and collapsed-ligature 0 cells so a
    // mark stays glued to its base. The strong-direction probe reads the ORIGINAL
    // logical codePoint (keeps the run boundary stable). Leading/trailing
    // neutrals stay in their LTR neighbourhood.
    int i = 0;
    while (i < n) {
        if (!isStrongRTL(logical[i])) {
            ++i;
            continue;
        }
        // Extend the run forward across strong-RTL cells and any interior
        // marks / collapsed cells that are followed by another strong-RTL
        // cell before the run truly ends.
        int runStart = i;
        int runEnd = i; // inclusive index of the last strong-RTL cell
        int j = i + 1;
        while (j < n) {
            if (isStrongRTL(logical[j])) {
                runEnd = j;
                ++j;
                continue;
            }
            const JoiningType jt = joiningType(logical[j]);
            // A Transparent mark, collapsed-ligature 0 cell, or BiDi neutral is
            // TENTATIVELY interior: it joins the run only if another strong-RTL
            // cell follows (N1). runEnd advances only on strong-RTL, so a
            // tentative cell with none following is left outside the reversed block.
            const bool tentativeInterior = (jt == JoiningType::Transparent) || (outCodePoints[j] == 0) || isBidiNeutral(logical[j]);
            if (tentativeInterior) {
                ++j;
                continue;
            }
            break;
        }
        // Reverse [runStart, runEnd] as a block: visual column gets the
        // logical cell from the mirror position. Carriers past runEnd
        // (trailing marks with no following strong cell) are left LTR.
        for (int a = runStart, b = runEnd; a < b; ++a, --b) {
            std::uint32_t tc = outCodePoints[a];
            outCodePoints[a] = outCodePoints[b];
            outCodePoints[b] = tc;
            std::uint16_t tl = outLogical[a];
            outLogical[a] = outLogical[b];
            outLogical[b] = tl;
        }
        i = runEnd + 1;
    }
    return true;
}

} // namespace Ubiquitty::Unicode
