// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Arabic cursive joining: joining-type classification, contextual form
// (isolated/initial/medial/final) resolution, Arabic Presentation Form-B
// mapping, lam-alef ligature, and run shaping over known words against the
// curated ArabicShaping snapshot.

#include "Harness.h"

#include "Core/Unicode/Joining.h"
#include <cstdint>
#include <cstdio>

using Ubiquitty::Unicode::isCursiveJoiner;
using Ubiquitty::Unicode::isStrongRTL;
using Ubiquitty::Unicode::JoiningForm;
using Ubiquitty::Unicode::JoiningType;
using Ubiquitty::Unicode::joiningType;
using Ubiquitty::Unicode::lamAlefLigature;
using Ubiquitty::Unicode::LamAlefLigatureForm;
using Ubiquitty::Unicode::presentationForm;
using Ubiquitty::Unicode::resolveForm;
using Ubiquitty::Unicode::shapeRun;
using Ubiquitty::Unicode::shapeVisualRow;

// shapeRun helper: shape in place, then compare to an expected array.
static void checkShape(const char* name, const std::uint32_t* in, int n, const std::uint32_t* expect)
{
    std::uint32_t outputBuffer[32];
    shapeRun(in, outputBuffer, n);
    for (int i = 0; i < n; ++i) {
        if (outputBuffer[i] != expect[i]) {
            printf("FAIL %s: cell %d got U+%04lX want U+%04lX\n", name, i, (unsigned long)outputBuffer[i], (unsigned long)expect[i]);
            failures++;
        }
    }
}

TEST(joiningCases)
{

    // ---- joining-type classification -------------------------------
    CHECK(joiningType(0x0627) == JoiningType::RightJoining); // ALEF
    CHECK(joiningType(0x0628) == JoiningType::DualJoining); // BEH
    CHECK(joiningType(0x062F) == JoiningType::RightJoining); // DAL
    CHECK(joiningType(0x0631) == JoiningType::RightJoining); // REH
    CHECK(joiningType(0x0648) == JoiningType::RightJoining); // WAW
    CHECK(joiningType(0x0644) == JoiningType::DualJoining); // LAM
    CHECK(joiningType(0x0645) == JoiningType::DualJoining); // MEEM
    CHECK(joiningType(0x0633) == JoiningType::DualJoining); // SEEN
    CHECK(joiningType(0x0640) == JoiningType::JoinCausing); // TATWEEL
    CHECK(joiningType(0x0621) == JoiningType::NonJoining); // HAMZA
    CHECK(joiningType(0x064E) == JoiningType::Transparent); // FATHA (haraka)
    CHECK(joiningType(0x0651) == JoiningType::Transparent); // SHADDA
    CHECK(joiningType(0x0670) == JoiningType::Transparent); // superscript alef
    CHECK(joiningType(0x200D) == JoiningType::JoinCausing); // ZWJ
    CHECK(joiningType(0x200C) == JoiningType::NonJoining); // ZWNJ
    CHECK(joiningType('A') == JoiningType::NonJoining); // Latin
    CHECK(joiningType(0x0020) == JoiningType::NonJoining); // space
    CHECK(joiningType(0x0660) == JoiningType::NonJoining); // Arabic-Indic digit

    // ---- run-level cursive gate ------------------------------------
    CHECK(isCursiveJoiner(0x0628)); // BEH
    CHECK(isCursiveJoiner(0x064E)); // a haraka still gates the row
    CHECK(isCursiveJoiner(0x200D)); // ZWJ
    CHECK(isCursiveJoiner(0x200C)); // ZWNJ
    CHECK(!isCursiveJoiner('A'));
    CHECK(!isCursiveJoiner(0x0041));
    CHECK(!isCursiveJoiner(0x4E00)); // CJK
    CHECK(!isCursiveJoiner(0x05D0)); // Hebrew alef — not shaped here

    // ---- form resolution -------------------------------------------
    // BEH (dual) with no neighbours → isolated.
    CHECK(resolveForm(0x0628, JoiningType::NonJoining, JoiningType::NonJoining) == JoiningForm::Isolated);
    // BEH followed by a dual letter → initial (joins following).
    CHECK(resolveForm(0x0628, JoiningType::NonJoining, JoiningType::DualJoining) == JoiningForm::Initial);
    // BEH preceded by a dual letter → final (joins preceding).
    CHECK(resolveForm(0x0628, JoiningType::DualJoining, JoiningType::NonJoining) == JoiningForm::Final);
    // BEH between two dual letters → medial.
    CHECK(resolveForm(0x0628, JoiningType::DualJoining, JoiningType::DualJoining) == JoiningForm::Medial);
    // ALEF (right-joining) can only ever join its preceding letter:
    // preceded by a dual letter → final; never initial/medial.
    CHECK(resolveForm(0x0627, JoiningType::DualJoining, JoiningType::DualJoining) == JoiningForm::Final);
    CHECK(resolveForm(0x0627, JoiningType::NonJoining, JoiningType::DualJoining) == JoiningForm::Isolated);
    // A Right_Joining PRECEDING neighbour (e.g. alef before beh) only
    // joins backward, so it does not connect forward into beh → isolated.
    CHECK(resolveForm(0x0628, JoiningType::RightJoining, JoiningType::NonJoining) == JoiningForm::Isolated);
    // A Right_Joining FOLLOWING neighbour (e.g. alef after beh) joins
    // backward into beh, so beh joins forward → initial.
    CHECK(resolveForm(0x0628, JoiningType::NonJoining, JoiningType::RightJoining) == JoiningForm::Initial);
    // Join_Causing on either side connects.
    CHECK(resolveForm(0x0628, JoiningType::JoinCausing, JoiningType::JoinCausing) == JoiningForm::Medial);

    // ---- presentation-form mapping ---------------------------------
    CHECK(presentationForm(0x0628, JoiningForm::Isolated) == 0xFE8F);
    CHECK(presentationForm(0x0628, JoiningForm::Final) == 0xFE90);
    CHECK(presentationForm(0x0628, JoiningForm::Initial) == 0xFE91);
    CHECK(presentationForm(0x0628, JoiningForm::Medial) == 0xFE92);
    // ALEF has only isolated + final; asking for medial falls back to isolated.
    CHECK(presentationForm(0x0627, JoiningForm::Final) == 0xFE8E);
    CHECK(presentationForm(0x0627, JoiningForm::Medial) == 0xFE8D);
    // A non-Arabic codePoint passes through unchanged.
    CHECK(presentationForm('A', JoiningForm::Initial) == 'A');

    // ---- lam-alef ligature -----------------------------------------
    CHECK(lamAlefLigature(0x0627, LamAlefLigatureForm::Isolated) == 0xFEFB); // LAM+ALEF isolated
    CHECK(lamAlefLigature(0x0627, LamAlefLigatureForm::Final) == 0xFEFC); // LAM+ALEF final
    CHECK(lamAlefLigature(0x0622, LamAlefLigatureForm::Isolated) == 0xFEF5); // LAM+ALEF MADDA
    CHECK(lamAlefLigature(0x0628, LamAlefLigatureForm::Isolated) == 0); // BEH does not ligate

    // ---- run shaping: known words ----------------------------------

    // Two dual letters "بب" (beh beh): first initial, second final.
    {
        const std::uint32_t in[] = { 0x0628, 0x0628 };
        const std::uint32_t expect[] = { 0xFE91, 0xFE90 };
        checkShape("beh-beh", in, 2, expect);
    }

    // "بسم" (BEH SEEN MEEM): initial, medial, final.
    {
        const std::uint32_t in[] = { 0x0628, 0x0633, 0x0645 };
        const std::uint32_t expect[] = { 0xFE91, 0xFEB4, 0xFEE2 };
        checkShape("bsm", in, 3, expect);
    }

    // A right-joining letter breaks the join after it: "بدب"
    // (BEH DAL BEH). DAL joins its preceding BEH (BEH initial, DAL final),
    // but DAL is right-joining so the following BEH starts fresh
    // (isolated).
    {
        const std::uint32_t in[] = { 0x0628, 0x062F, 0x0628 };
        const std::uint32_t expect[] = { 0xFE91, 0xFEAA, 0xFE8F };
        checkShape("bdb", in, 3, expect);
    }

    // "سلام" (SEEN LAM ALEF MEEM): SEEN initial; LAM joins SEEN on its right, so the
    // LAM+ALEF form the mandatory lam-alef ligature in its FINAL form (FEFC) — one
    // combined glyph. shapeRun emits the ligature at the ALEF cell and collapses the
    // LAM cell to 0 (the wide-continuation half); MEEM is isolated.
    {
        const std::uint32_t in[] = { 0x0633, 0x0644, 0x0627, 0x0645 };
        const std::uint32_t expect[] = { 0xFEB3, 0x0000, 0xFEFC, 0xFEE1 };
        checkShape("slam", in, 4, expect);
    }

    // Lam-alef at word start "لا": the mandatory ligature in ISOLATED form (FEFB,
    // no preceding letter). shapeRun emits it at the ALEF cell and collapses the LAM
    // cell to 0 (the wide-continuation half) — one combined glyph, not two forms.
    {
        const std::uint32_t in[] = { 0x0644, 0x0627 };
        const std::uint32_t expect[] = { 0x0000, 0xFEFB };
        checkShape("lam-alef", in, 2, expect);
    }

    // Harakat are Transparent: "بَب" (BEH FATHA BEH). The fatha sits on
    // the first BEH and is invisible to the neighbour scan, so the BEHs
    // still see each other: initial, (mark passes through), final.
    {
        const std::uint32_t in[] = { 0x0628, 0x064E, 0x0628 };
        const std::uint32_t expect[] = { 0xFE91, 0x064E, 0xFE90 };
        checkShape("beh-fatha-beh", in, 3, expect);
    }

    // ZWNJ breaks a join: "ب‌ب" (BEH ZWNJ BEH) → both isolated.
    {
        const std::uint32_t in[] = { 0x0628, 0x200C, 0x0628 };
        const std::uint32_t expect[] = { 0xFE8F, 0x200C, 0xFE8F };
        checkShape("beh-zwnj-beh", in, 3, expect);
    }

    // ZWJ forces a join on an otherwise-isolated letter: "ب‍" (BEH ZWJ)
    // → BEH initial (the ZWJ is a join-causing right neighbour).
    {
        const std::uint32_t in[] = { 0x0628, 0x200D };
        const std::uint32_t expect[] = { 0xFE91, 0x200D };
        checkShape("beh-zwj", in, 2, expect);
    }

    // Mixed Arabic + Latin: "Aبب" (A BEH BEH). The Latin 'A' is a
    // non-joiner, so the first BEH has no preceding joiner: initial,
    // final.
    {
        const std::uint32_t in[] = { 'A', 0x0628, 0x0628 };
        const std::uint32_t expect[] = { 'A', 0xFE91, 0xFE90 };
        checkShape("latin-beh-beh", in, 3, expect);
    }

    // Space inside a run breaks it: "ب ب" → both isolated.
    {
        const std::uint32_t in[] = { 0x0628, 0x0020, 0x0628 };
        const std::uint32_t expect[] = { 0xFE8F, 0x0020, 0xFE8F };
        checkShape("beh-space-beh", in, 3, expect);
    }

    // Tatweel (kashida) is join-causing: "بـب" (BEH TATWEEL BEH) →
    // BEH initial, tatweel passes through, BEH final. The tatweel
    // connects both sides.
    {
        const std::uint32_t in[] = { 0x0628, 0x0640, 0x0628 };
        const std::uint32_t expect[] = { 0xFE91, 0x0640, 0xFE90 };
        checkShape("beh-tatweel-beh", in, 3, expect);
    }

    // Pure Latin run is untouched.
    {
        const std::uint32_t in[] = { 'h', 'i' };
        const std::uint32_t expect[] = { 'h', 'i' };
        checkShape("latin", in, 2, expect);
    }

    // Aliasing contract: shapeRun(buffer, buffer, n) (out == in) must match the
    // separate-buffer result. This is the case the carried-prevType scan
    // fixes — 3+ connected letters and lam-alef, where an in-place backward
    // read would otherwise see an already-shaped presentation form (whose
    // type is not in the joining tables) and misclassify the neighbour.
    {
        const std::uint32_t words[][5] = {
            { 0x0628, 0x0633, 0x0645, 0, 0 }, // بسم  — initial/medial/final
            { 0x0633, 0x0644, 0x0627, 0x0645, 0 }, // سلام — lam-alef mid-run
            { 0x0628, 0x064E, 0x0628, 0, 0 }, // بَب  — transparent haraka
            { 0x0628, 0x0640, 0x0628, 0, 0 }, // بـب  — tatweel join
        };
        const int lengths[] = { 3, 4, 3, 3 };
        for (int w = 0; w < 4; ++w) {
            std::uint32_t sep[5], same[5];
            for (int i = 0; i < lengths[w]; ++i)
                same[i] = words[w][i];
            shapeRun(words[w], sep, lengths[w]); // separate out
            shapeRun(same, same, lengths[w]); // in place (out == in)
            for (int i = 0; i < lengths[w]; ++i) {
                if (sep[i] != same[i]) {
                    printf("FAIL aliased w%d: cell %d in-place U+%04lX != separate U+%04lX\n", w, i, (unsigned long)same[i], (unsigned long)sep[i]);
                    failures++;
                }
            }
        }
    }

    // ---- strong-RTL classification --------------------------------
    CHECK(isStrongRTL(0x0628)); // Arabic BEH
    CHECK(isStrongRTL(0x0627)); // Arabic ALEF
    CHECK(isStrongRTL(0xFE91)); // BEH initial presentation form
    CHECK(isStrongRTL(0xFEFB)); // lam-alef ligature (Pres Forms-A)
    CHECK(isStrongRTL(0x05D0)); // Hebrew ALEF
    CHECK(!isStrongRTL('A')); // Latin
    CHECK(!isStrongRTL(0x0660)); // Arabic-Indic digit is not strong-RTL
    CHECK(!isStrongRTL(0x4E00)); // CJK

    // ---- shapeVisualRow: shape + RTL visual ordering ---------------

    // shapeVisualRow helper: assert the visual codePoints and the
    // logical-index permutation, plus the return value.
    struct VR {
        static void check(const char* name, const std::uint32_t* in, int n, const std::uint32_t* expectCodePoints, const std::uint16_t* expectLog, bool expectRet)
        {
            std::uint32_t codePoints[64];
            std::uint16_t log[64];
            bool ret = shapeVisualRow(in, codePoints, log, n);
            if (ret != expectRet) {
                printf("FAIL %s: ret %d want %d\n", name, (int)ret, (int)expectRet);
                failures++;
            }
            for (int i = 0; i < n; ++i) {
                if (codePoints[i] != expectCodePoints[i]) {
                    printf("FAIL %s: vis %d codePoint U+%04lX want U+%04lX\n", name, i, (unsigned long)codePoints[i], (unsigned long)expectCodePoints[i]);
                    failures++;
                }
                if (log[i] != expectLog[i]) {
                    printf("FAIL %s: vis %d logIndex %d want %d\n", name, i, (int)log[i], (int)expectLog[i]);
                    failures++;
                }
            }
        }
    };

    // Pure Latin row: no shaping, identity perm, returns false.
    {
        const std::uint32_t in[] = { 'h', 'i', '!' };
        const std::uint32_t codePoints[] = { 'h', 'i', '!' };
        const std::uint16_t log[] = { 0, 1, 2 };
        VR::check("vr-latin", in, 3, codePoints, log, false);
    }

    // "بسم" — initial/medial/final shaped, then REVERSED to RTL visual
    // order: visual reads final, medial, initial left-to-right.
    {
        const std::uint32_t in[] = { 0x0628, 0x0633, 0x0645 };
        const std::uint32_t codePoints[] = { 0xFEE2, 0xFEB4, 0xFE91 };
        const std::uint16_t log[] = { 2, 1, 0 };
        VR::check("vr-bsm", in, 3, codePoints, log, true);
    }

    // "لا" lam-alef: the mandatory ligature in ISOLATED form (FEFB — the lam has no
    // preceding joiner). One combined glyph across two cells: the ligature at the
    // (reversed) alef cell = visual lead, the collapsed 0 at the lam cell = the
    // wide-continuation half. perm mirrors [0,1] → [1,0].
    {
        const std::uint32_t in[] = { 0x0644, 0x0627 };
        const std::uint32_t codePoints[] = { 0xFEFB, 0x0000 };
        const std::uint16_t log[] = { 1, 0 };
        VR::check("vr-lam-alef", in, 2, codePoints, log, true);
    }

    // "سلام" SEEN LAM ALEF MEEM: SEEN initial, LAM+ALEF the final lam-alef ligature
    // (FEFC) + its collapsed 0 continuation, MEEM isolated. Reversed as one RTL block
    // of 4 → FEE1 FEFC 0 FEB3: the ligature (visual 1) is the wide-lead, the 0
    // (visual 2 = lead+1) its continuation. perm [3,2,1,0].
    {
        const std::uint32_t in[] = { 0x0633, 0x0644, 0x0627, 0x0645 };
        const std::uint32_t codePoints[] = { 0xFEE1, 0xFEFC, 0x0000, 0xFEB3 };
        const std::uint16_t log[] = { 3, 2, 1, 0 };
        VR::check("vr-slam", in, 4, codePoints, log, true);
    }

    // "السلام" ALEF LAM SEEN LAM ALEF MEEM (as-salām, with the ال article). The
    // interior LAM+ALEF forms the FINAL lam-alef ligature (FEFC) + its 0
    // continuation — the crossed combined glyph, not two uprights. Visual, one RTL
    // block of 6: meem-isolated, ligature (wide-lead), 0 (continuation), seen-medial,
    // lam-initial, alef-isolated. perm [5,4,3,2,1,0].
    {
        const std::uint32_t in[] = { 0x0627, 0x0644, 0x0633, 0x0644, 0x0627, 0x0645 };
        const std::uint32_t codePoints[] = { 0xFEE1, 0xFEFC, 0x0000, 0xFEB4, 0xFEDF, 0xFE8D };
        const std::uint16_t log[] = { 5, 4, 3, 2, 1, 0 };
        VR::check("vr-assalam", in, 6, codePoints, log, true);
    }

    // "العربية مرحبا" — two Arabic words separated by a space. The space is
    // a BiDi neutral flanked by RTL on both sides, so by N1 it belongs to the
    // RTL run: the WHOLE line is one run and reverses as a block. Visually
    // مرحبا (word2) lands on the left and العربية (word1) on the right — each
    // word internally shaped. (Reversing each word on its own, leaving the
    // words themselves in left-to-right order, is wrong.)
    {
        const std::uint32_t in[] = {
            0x0627, 0x0644, 0x0639, 0x0631, 0x0628, 0x064A, 0x0629, // العربية
            0x0020, // space
            0x0645, 0x0631, 0x062D, 0x0628, 0x0627 // مرحبا
        };
        // Shape the whole row in logical order, then reverse the entire row
        // (one RTL run including the interior space).
        std::uint32_t shapedFull[13];
        shapeRun(in, shapedFull, 13);
        std::uint32_t expectCodePoints[13];
        std::uint16_t expectLog[13];
        for (int k = 0; k < 13; ++k) {
            expectCodePoints[k] = shapedFull[12 - k];
            expectLog[k] = (std::uint16_t)(12 - k);
        }
        VR::check("vr-alarabiyya-marhaba", in, 13, expectCodePoints, expectLog, true);
    }

    // Mixed RTL + Latin: "بب AB" — the Arabic run reverses, the Latin run
    // stays LTR, the space is neutral between them.
    {
        const std::uint32_t in[] = { 0x0628, 0x0628, 0x0020, 'A', 'B' };
        const std::uint32_t codePoints[] = { 0xFE90, 0xFE91, 0x0020, 'A', 'B' };
        const std::uint16_t log[] = { 1, 0, 2, 3, 4 };
        VR::check("vr-arabic-latin", in, 5, codePoints, log, true);
    }
}

UBIQUITTY_TEST_MAIN()
