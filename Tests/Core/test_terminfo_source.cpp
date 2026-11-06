// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_terminfo_source — every spelling terminfo(5) allows, decoded.
//
// The entry this project ships uses a fraction of the format: `^I` for tab
// rather than `\t`, decimal and plain hex for its numbers, never a hex letter
// or a leading-zero octal. Reached only through that entry, most of the
// decoder is unexecuted — and an unexecuted branch is one nobody has ever
// seen answer. The cost is not hypothetical: a capability added later in a
// spelling nothing exercises decodes to whatever the fallback does, silently,
// and the damage surfaces as an application reading one byte where the entry
// meant another.
//
// So the decoder is tested against the FORMAT rather than against our entry.
// Each case below names the spelling, the bytes it denotes, and why terminfo
// writes it that way.

#include "Core/TerminfoSource.h"
#include "Harness.h"

#include <string>

using Ubiquitty::Terminfo::Source::decodeNumber;
using Ubiquitty::Terminfo::Source::decodeValue;

/// The named single-character escapes.
///
/// Every one of these has a control-character spelling as well (`^I` for tab),
/// and an entry may use either. A decoder that knows only the spelling ours
/// happens to use turns the other into a literal letter.
TEST(namedEscapesDecodeToTheCharactersTheyName)
{
    CHECK_STR_EQ(decodeValue("\\E"), "\033");
    CHECK_STR_EQ(decodeValue("\\e"), "\033");
    CHECK_STR_EQ(decodeValue("\\n"), "\n");
    CHECK_STR_EQ(decodeValue("\\l"), "\n");
    CHECK_STR_EQ(decodeValue("\\r"), "\r");
    CHECK_STR_EQ(decodeValue("\\t"), "\t");
    CHECK_STR_EQ(decodeValue("\\b"), "\b");
    CHECK_STR_EQ(decodeValue("\\f"), "\f");
    CHECK_STR_EQ(decodeValue("\\s"), " ");
}

/// The control spelling, and the one control it does not reach.
///
/// `^X` is the character with the low five bits of X, which puts DEL outside
/// the pattern — terminfo spells it `^?`, and an entry's `kbs` uses it.
TEST(caretSpellsAControlAndQuestionMarkSpellsDelete)
{
    CHECK_STR_EQ(decodeValue("^I"), "\t");
    CHECK_STR_EQ(decodeValue("^G"), "\a");
    CHECK_STR_EQ(decodeValue("^H"), "\b");
    CHECK_STR_EQ(decodeValue("^M"), "\r");
    CHECK_STR_EQ(decodeValue("^?"), "\x7f");
}

/// Octal, including the zero terminfo cannot store as one.
///
/// A compiled entry is a run of NUL-terminated strings, so a capability that
/// means NUL cannot hold one: terminfo writes `\0` and the entry carries 0200,
/// which is the byte tputs puts on the wire.
TEST(octalEscapesDecodeToTheirByteAndNulBecomes0200)
{
    CHECK_STR_EQ(decodeValue("\\007"), "\a");
    CHECK_STR_EQ(decodeValue("\\033"), "\033");
    CHECK_STR_EQ(decodeValue("\\177"), "\x7f");
    CHECK_STR_EQ(decodeValue("\\0"), "\200");

    // Three digits at most, so a fourth is text: \0334 is ESC then '4', which
    // is what an entry means when a parameter digit follows an escape.
    CHECK_STR_EQ(decodeValue("\\0334"), "\0334");
}

/// The punctuation that would otherwise end a field, and the fallback.
///
/// A comma separates capabilities and a colon separates the termcap form, so
/// both are escaped inside a value. Anything else after a backslash is itself:
/// that is the rule, not a gap, and it is what lets a `%` sequence through.
TEST(escapedPunctuationIsItselfAndAnUnknownEscapeIsItsOwnCharacter)
{
    CHECK_STR_EQ(decodeValue("\\,"), ",");
    CHECK_STR_EQ(decodeValue("\\:"), ":");
    CHECK_STR_EQ(decodeValue("\\\\"), "\\");
    CHECK_STR_EQ(decodeValue("\\^"), "^");
    CHECK_STR_EQ(decodeValue("\\q"), "q");

    // A trailing backslash has nothing to escape and stands for itself rather
    // than eating the end of the value.
    CHECK_STR_EQ(decodeValue("a\\"), "a\\");
}

/// A real capability, decoded whole.
///
/// The parameter and padding forms pass through untouched: they are read by
/// tparm and by the pad writer, not here.
TEST(aCapabilityValueDecodesItsEscapesAndKeepsItsParameters)
{
    CHECK_STR_EQ(decodeValue("\\E[%i%p1%d;%p2%dr"), "\033[%i%p1%d;%p2%dr");
    CHECK_STR_EQ(decodeValue("\\E[?5h$<100/>\\E[?5l"), "\033[?5h$<100/>\033[?5l");
    CHECK_STR_EQ(decodeValue("\\E]52;%p1%s;%p2%s\\007"), "\033]52;%p1%s;%p2%s\a");
}

/// The three bases a numeric may be written in.
///
/// terminfo takes them as C does, and an entry picks whichever reads best:
/// ours writes `colors#0x100` because 256 colours is a round number in hex and
/// an awkward one in decimal.
TEST(numbersDecodeInEveryBaseTerminfoAllows)
{
    CHECK_STR_EQ(decodeNumber("80"), "80");
    CHECK_STR_EQ(decodeNumber("0x100"), "256");
    CHECK_STR_EQ(decodeNumber("0X100"), "256");
    CHECK_STR_EQ(decodeNumber("010"), "8");

    // The hex letters, in both cases. A decoder that stops at 9 answers a
    // colour count short of the truth rather than refusing.
    CHECK_STR_EQ(decodeNumber("0xff"), "255");
    CHECK_STR_EQ(decodeNumber("0xFF"), "255");
    CHECK_STR_EQ(decodeNumber("0xAbCd"), "43981");
}

/// A number stops at the first character that is not a digit of its base.
///
/// The field is already delimited by the parser, so this is the guard against
/// a malformed entry rather than a parsing step: it answers the part that is a
/// number and leaves the rest, instead of running off into the next field.
TEST(aNumberStopsAtTheFirstCharacterThatIsNotItsDigit)
{
    CHECK_STR_EQ(decodeNumber("12x"), "12");
    CHECK_STR_EQ(decodeNumber("0x1g"), "1");
    CHECK_STR_EQ(decodeNumber("09"), "0"); // 9 is not an octal digit
    CHECK_STR_EQ(decodeNumber(""), "0");
    CHECK_STR_EQ(decodeNumber("0"), "0");
    CHECK_STR_EQ(decodeNumber("0x"), "0");
}

UBIQUITTY_TEST_MAIN()
