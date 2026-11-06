// Copyright (C) 2026 Stanislav Yaglo
// Copyright © 1991-2026 Unicode, Inc.
// SPDX-License-Identifier: Apache-2.0 AND Unicode-3.0

// Source/Core/Unicode/Joining.tables.inc — curated Arabic joining tables.
//
// Sourced from the Unicode Character Database (ArabicShaping.txt for the
// Joining_Type field; the Arabic Presentation Forms-A/B blocks U+FB50+
// and U+FE70+ for the contextual forms). Curated like
// GraphemeBreak.cpp's GCB ranges: a future regeneration from the UCD via
// Scripts/build-unicode-tables.py is a file overwrite, not a hand-edit.
//
// The table lists every code point in the blocks covered that Unicode 11.0.0
// gives a joining type other than U: U+08D3, new in 11.0.0, is listed, and
// U+08BE..U+08C7, new in 13.0.0, are not; 12.0.0 and 12.1.0 add nothing here.
// The assigned code points it leaves out are all U and take the default.
//
// The ranges are grouped by hand, and 25 code points carry a type that
// ArabicShaping.txt does not give them, in 11.0.0 or any later release: U+0678
// is R where Unicode has D; U+08AD is D where it has U; and U+0688..U+068D,
// U+0759..U+075B, U+076B..U+076C, U+0771, U+0773..U+0774, U+0778..U+0779,
// U+08AA..U+08AC, U+08AE, U+08B1..U+08B2 and U+08B9 are D where it has R, so
// the letter after an Urdu DDAL (U+0688), for one, takes a form that joins
// back to it.
//
// Three structures:
//   joiningRanges[]   — {first,last,JoiningType} over the joining scripts
//                        and the script-neutral joining controls, sorted
//                        by `first`, binary-searched. The default outside
//                        these ranges is NonJoining.
//   presFormB[]       — per Arabic letter, its {isolated,final,initial,
//                        medial} Presentation-Form-B codepoints (0 where a
//                        shape does not exist). Keyed by the base letter
//                        codepoint, sorted, binary-searched.
//   lamAlef[]         — the four lam+alef ligatures, {alef, isolated,
//                        final} from the FEF5..FEFC range.
//
// Joining_Type letters (ArabicShaping.txt): U Non_Joining, R Right,
// L Left, D Dual, C Join_Causing, T Transparent.

// --- joining types ----------------------------------------------------
//
// Covers Arabic (0600..06FF), Arabic Supplement (0750..077F), and the
// common Arabic Extended-A letters, plus the script-neutral controls
// ZWNJ (U, breaks a join), ZWJ (C, forces a join), and TATWEEL (C).
// Within a block: harakat / combining marks are Transparent; spaces,
// digits, punctuation and the standalone hamza are Non_Joining; alef
// family / dal / reh / zain / waw and friends are Right_Joining; the
// bulk of the consonants are Dual_Joining.
struct JoinRange { std::uint32_t first; std::uint32_t last; JoiningType type; };

// Sorted ascending by `first` (binary-searched).
static const JoinRange joiningRanges[] = {
    // Arabic block 0600..06FF.
    { 0x000600, 0x000605, JoiningType::NonJoining },   // Arabic number signs (Prepend, not joiners)
    { 0x000608, 0x000608, JoiningType::NonJoining },
    { 0x00060B, 0x00060B, JoiningType::NonJoining },
    { 0x00060D, 0x00060D, JoiningType::NonJoining },
    { 0x000610, 0x00061A, JoiningType::Transparent },   // combining marks (high hamza, etc.)
    { 0x00061C, 0x00061C, JoiningType::Transparent },   // ARABIC LETTER MARK (format)
    { 0x000620, 0x000620, JoiningType::DualJoining },   // KASHMIRI YEH
    { 0x000621, 0x000621, JoiningType::NonJoining },   // HAMZA (isolated, non-joining)
    { 0x000622, 0x000625, JoiningType::RightJoining },   // ALEF madda/hamza variants
    { 0x000626, 0x000626, JoiningType::DualJoining },   // YEH WITH HAMZA ABOVE
    { 0x000627, 0x000627, JoiningType::RightJoining },   // ALEF
    { 0x000628, 0x000628, JoiningType::DualJoining },   // BEH
    { 0x000629, 0x000629, JoiningType::RightJoining },   // TEH MARBUTA
    { 0x00062A, 0x00062E, JoiningType::DualJoining },   // TEH..KHAH
    { 0x00062F, 0x000632, JoiningType::RightJoining },   // DAL DHAL REH ZAIN
    { 0x000633, 0x00063F, JoiningType::DualJoining },   // SEEN..(extended) all dual
    { 0x000640, 0x000640, JoiningType::JoinCausing },   // TATWEEL / kashida (join-causing)
    { 0x000641, 0x000647, JoiningType::DualJoining },   // FEH..HEH dual
    { 0x000648, 0x000648, JoiningType::RightJoining },   // WAW
    { 0x000649, 0x00064A, JoiningType::DualJoining },   // ALEF MAKSURA, YEH
    { 0x00064B, 0x00065F, JoiningType::Transparent },   // harakat / combining marks
    { 0x000660, 0x000669, JoiningType::NonJoining },   // Arabic-Indic digits
    { 0x00066A, 0x00066D, JoiningType::NonJoining },   // percent, decimal sep, etc.
    { 0x00066E, 0x00066F, JoiningType::DualJoining },   // dotless beh / qaf
    { 0x000670, 0x000670, JoiningType::Transparent },   // superscript alef (combining)
    { 0x000671, 0x000673, JoiningType::RightJoining },   // ALEF WASLA + variants
    { 0x000674, 0x000674, JoiningType::NonJoining },   // HIGH HAMZA (non-joining)
    { 0x000675, 0x000678, JoiningType::RightJoining },   // high-hamza alef/waw/yeh forms
    { 0x000679, 0x00068D, JoiningType::DualJoining },   // extended dual-joining consonants
    { 0x00068E, 0x000699, JoiningType::RightJoining },   // extended right-joining (dal/reh family)
    { 0x00069A, 0x0006BF, JoiningType::DualJoining },   // more extended dual consonants
    { 0x0006C0, 0x0006C0, JoiningType::RightJoining },   // HEH WITH YEH ABOVE
    { 0x0006C1, 0x0006C2, JoiningType::DualJoining },   // HEH GOAL variants
    { 0x0006C3, 0x0006CB, JoiningType::RightJoining },   // teh-marbuta-goal / waw family (right)
    { 0x0006CC, 0x0006CC, JoiningType::DualJoining },   // FARSI YEH
    { 0x0006CD, 0x0006CD, JoiningType::RightJoining },   // YEH WITH TAIL
    { 0x0006CE, 0x0006CE, JoiningType::DualJoining },   // YEH WITH SMALL V
    { 0x0006CF, 0x0006CF, JoiningType::RightJoining },   // WAW WITH DOT ABOVE
    { 0x0006D0, 0x0006D1, JoiningType::DualJoining },   // E / yeh variants (dual)
    { 0x0006D2, 0x0006D3, JoiningType::RightJoining },   // YEH BARREE variants
    { 0x0006D4, 0x0006D4, JoiningType::NonJoining },   // full stop
    { 0x0006D5, 0x0006D5, JoiningType::RightJoining },   // AE
    { 0x0006D6, 0x0006DC, JoiningType::Transparent },   // small high combining marks
    { 0x0006DD, 0x0006DD, JoiningType::NonJoining },   // end of ayah (Prepend)
    { 0x0006DF, 0x0006E4, JoiningType::Transparent },   // small high combining marks
    { 0x0006E5, 0x0006E6, JoiningType::NonJoining },   // small waw / yeh (non-joining)
    { 0x0006E7, 0x0006E8, JoiningType::Transparent },   // small high marks
    { 0x0006EA, 0x0006ED, JoiningType::Transparent },   // combining marks below/above
    { 0x0006EE, 0x0006EF, JoiningType::RightJoining },   // dal/reh inverted-v forms
    { 0x0006F0, 0x0006F9, JoiningType::NonJoining },   // extended Arabic-Indic digits
    { 0x0006FA, 0x0006FC, JoiningType::DualJoining },   // sheen/dad/ghain with dot (dual)
    { 0x0006FF, 0x0006FF, JoiningType::DualJoining },   // HEH WITH INVERTED V

    // Arabic Supplement 0750..077F — all dual-joining consonants.
    { 0x000750, 0x00077F, JoiningType::DualJoining },

    // Arabic Extended-A 08A0..08FF — letters dual, marks transparent.
    { 0x0008A0, 0x0008B4, JoiningType::DualJoining },
    { 0x0008B6, 0x0008BD, JoiningType::DualJoining },
    { 0x0008D3, 0x0008E1, JoiningType::Transparent },   // combining marks
    { 0x0008E2, 0x0008E2, JoiningType::NonJoining },   // disputed end of ayah
    { 0x0008E3, 0x0008FF, JoiningType::Transparent },   // combining marks

    // Script-neutral joining controls (sort after the Arabic blocks).
    { 0x00200C, 0x00200C, JoiningType::NonJoining },   // ZERO WIDTH NON-JOINER (breaks join)
    { 0x00200D, 0x00200D, JoiningType::JoinCausing },   // ZERO WIDTH JOINER (forces join)
};

// --- presentation forms (Form-B, U+FE70..FEFC) ------------------------
//
// {base, isolated, final, initial, medial}. A 0 in a slot means the
// letter has no glyph for that shape; resolveForm + presentationForm
// fall back to the isolated form (or the base) in that case. Right-
// joining letters have only isolated + final; dual-joining letters have
// all four.
struct PresFormB {
    std::uint32_t base;
    std::uint32_t isol, fina, init, medi;
};

static const PresFormB presFormB[] = {
    { 0x0621, 0xFE80, 0,      0,      0      },  // HAMZA
    { 0x0622, 0xFE81, 0xFE82, 0,      0      },  // ALEF MADDA
    { 0x0623, 0xFE83, 0xFE84, 0,      0      },  // ALEF HAMZA ABOVE
    { 0x0624, 0xFE85, 0xFE86, 0,      0      },  // WAW HAMZA
    { 0x0625, 0xFE87, 0xFE88, 0,      0      },  // ALEF HAMZA BELOW
    { 0x0626, 0xFE89, 0xFE8A, 0xFE8B, 0xFE8C },  // YEH HAMZA (dual)
    { 0x0627, 0xFE8D, 0xFE8E, 0,      0      },  // ALEF
    { 0x0628, 0xFE8F, 0xFE90, 0xFE91, 0xFE92 },  // BEH
    { 0x0629, 0xFE93, 0xFE94, 0,      0      },  // TEH MARBUTA
    { 0x062A, 0xFE95, 0xFE96, 0xFE97, 0xFE98 },  // TEH
    { 0x062B, 0xFE99, 0xFE9A, 0xFE9B, 0xFE9C },  // THEH
    { 0x062C, 0xFE9D, 0xFE9E, 0xFE9F, 0xFEA0 },  // JEEM
    { 0x062D, 0xFEA1, 0xFEA2, 0xFEA3, 0xFEA4 },  // HAH
    { 0x062E, 0xFEA5, 0xFEA6, 0xFEA7, 0xFEA8 },  // KHAH
    { 0x062F, 0xFEA9, 0xFEAA, 0,      0      },  // DAL
    { 0x0630, 0xFEAB, 0xFEAC, 0,      0      },  // THAL
    { 0x0631, 0xFEAD, 0xFEAE, 0,      0      },  // REH
    { 0x0632, 0xFEAF, 0xFEB0, 0,      0      },  // ZAIN
    { 0x0633, 0xFEB1, 0xFEB2, 0xFEB3, 0xFEB4 },  // SEEN
    { 0x0634, 0xFEB5, 0xFEB6, 0xFEB7, 0xFEB8 },  // SHEEN
    { 0x0635, 0xFEB9, 0xFEBA, 0xFEBB, 0xFEBC },  // SAD
    { 0x0636, 0xFEBD, 0xFEBE, 0xFEBF, 0xFEC0 },  // DAD
    { 0x0637, 0xFEC1, 0xFEC2, 0xFEC3, 0xFEC4 },  // TAH
    { 0x0638, 0xFEC5, 0xFEC6, 0xFEC7, 0xFEC8 },  // ZAH
    { 0x0639, 0xFEC9, 0xFECA, 0xFECB, 0xFECC },  // AIN
    { 0x063A, 0xFECD, 0xFECE, 0xFECF, 0xFED0 },  // GHAIN
    { 0x0641, 0xFED1, 0xFED2, 0xFED3, 0xFED4 },  // FEH
    { 0x0642, 0xFED5, 0xFED6, 0xFED7, 0xFED8 },  // QAF
    { 0x0643, 0xFED9, 0xFEDA, 0xFEDB, 0xFEDC },  // KAF
    { 0x0644, 0xFEDD, 0xFEDE, 0xFEDF, 0xFEE0 },  // LAM
    { 0x0645, 0xFEE1, 0xFEE2, 0xFEE3, 0xFEE4 },  // MEEM
    { 0x0646, 0xFEE5, 0xFEE6, 0xFEE7, 0xFEE8 },  // NOON
    { 0x0647, 0xFEE9, 0xFEEA, 0xFEEB, 0xFEEC },  // HEH
    { 0x0648, 0xFEED, 0xFEEE, 0,      0      },  // WAW
    { 0x0649, 0xFEEF, 0xFEF0, 0xFBE8, 0xFBE9 },  // ALEF MAKSURA
    { 0x064A, 0xFEF1, 0xFEF2, 0xFEF3, 0xFEF4 },  // YEH
};

// --- lam-alef ligatures (U+FEF5..FEFC) --------------------------------
//
// {alef, isolated, final}. The lam (U+0644) preceding any of these four
// alef variants collapses into one ligature glyph; `final` is used when
// a letter precedes the lam (so the lam joins on its right), `isolated`
// otherwise.
struct LamAlef { std::uint32_t alef; std::uint32_t isol, fina; };

static const LamAlef lamAlef[] = {
    { 0x0622, 0xFEF5, 0xFEF6 },   // LAM + ALEF MADDA
    { 0x0623, 0xFEF7, 0xFEF8 },   // LAM + ALEF HAMZA ABOVE
    { 0x0625, 0xFEF9, 0xFEFA },   // LAM + ALEF HAMZA BELOW
    { 0x0627, 0xFEFB, 0xFEFC },   // LAM + ALEF
};
