// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Tests/Core/Preferences/test_effective_value.cpp — the key-addressed read and
// write, against the surface they are supposed to agree with.
//
// The point of `effectiveValue` is that a caller asking by key gets the same
// answer as a caller asking by name. So the sweep below does not compare it
// against a transcribed table of expected values — a second transcription
// agrees with itself and says nothing. It compares it against the parse and
// against `Effective::`, which are what a second answer would have to
// contradict.
//
// Pins:
//   - every key in writeDescriptors resolves; none is unanswerable
//   - the kind a key resolves to is the kind its descriptor declares
//   - a value written through writeValue parses back to the value read
//   - a key whose kind disagrees with the value refuses rather than writing
//   - the population is asserted, so a table that lost most of its rows fails
//   - the body-taking write agrees with the file-taking one, key for key

#include "Harness.h"

#include <optional>
#include <string>

#include "Core/Preferences/Configuration.h"
#include "Core/Preferences/EffectiveValue.h"
#include "Core/Preferences/Key.h"
#include "Core/Preferences/Preferences.h"
#include "Core/Preferences/PreferencesAccessors.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

using Ubiquitty::Preferences::effectiveValue;
using Ubiquitty::Preferences::Key;
using Ubiquitty::Preferences::RemoteClipboardTools;
using Ubiquitty::Preferences::SerializeKind;
using Ubiquitty::Preferences::Value;
using Ubiquitty::Preferences::Values;
using Ubiquitty::Preferences::writeDescriptorCount;
using Ubiquitty::Preferences::writeDescriptors;
using Ubiquitty::Preferences::writeValue;

/// Every key answers, and answers in the kind its descriptor declares.
///
/// A key that resolved to nothing would show a settings row a blank where a
/// value belongs, and a key resolving to the wrong kind would have the row read
/// the wrong slot — a `Bool` read out of `text` is an empty string, which looks
/// like a value rather than like a mistake.
TEST(testEveryKeyResolvesInItsDeclaredKind)
{
    const Values defaulted;
    int checked = 0;
    for (int i = 0; i < writeDescriptorCount; ++i) {
        const auto& descriptor = writeDescriptors[i];
        const auto resolved = effectiveValue(descriptor.id, defaulted);
        if (!resolved.has_value()) {
            std::printf("FAIL [%s] %s resolves to nothing\n", descriptor.section, descriptor.key);
            failures++;
            continue;
        }
        // The read reports the VALUE's kind, which is the field's. For seven
        // keys that is Double where the write descriptor says Int -- the parse
        // stores `size = 15.5` as 15.5 and only the writer truncates -- so an
        // Int descriptor over a Double read is expected rather than a mismatch.
        const bool hasExpectedKind = resolved->kind == descriptor.kind
            || (descriptor.kind == SerializeKind::Int && resolved->kind == SerializeKind::Double);
        if (!hasExpectedKind) {
            std::printf("FAIL [%s] %s resolves as kind %d, descriptor says %d\n",
                descriptor.section, descriptor.key,
                static_cast<int>(resolved->kind), static_cast<int>(descriptor.kind));
            failures++;
        }
        ++checked;
    }
    // The sweep covers a population, so its size is asserted: a descriptor table
    // that lost most of itself would still pass every comparison above.
    CHECK_EQ(checked, writeDescriptorCount);
    CHECK_MSG(checked >= 130, "the sweep still covers the whole key surface");
}

/// A key-addressed read agrees with the accessor for the keys that have one.
///
/// These are the keys whose resolution is interesting — a default, a clamp, a
/// shorthand an edge falls back to — and they are exactly the ones a
/// table-derived answer would get wrong. Naming a handful rather than all of
/// them because the point is the disagreement, and a spot-check that names its
/// subjects is readable where a loop over accessors would not be.
TEST(testKeyAddressedReadAgreesWithTheAccessor)
{
    namespace Effective = Ubiquitty::Preferences::Effective;
    Values c;

    // Blur radius resolves through its default when no radius is stated,
    // which a field read cannot express.
    const auto blur = effectiveValue(Key::AppearanceBlurRadius, c);
    CHECK(blur.has_value());
    CHECK_EQ(blur->number, Effective::blurRadius(c));
    CHECK_MSG(blur->number != 0.0, "an unset radius resolves to the visible default");

    // A clamp the accessor applies and the field does not.
    c.titlelessPaddingTop = 999.0;
    const auto padding = effectiveValue(Key::AppearanceTitlelessPaddingTop, c);
    CHECK(padding.has_value());
    CHECK_EQ(padding->number, Effective::titlelessPaddingTop(c));
    CHECK_MSG(padding->number < 999.0, "the accessor's clamp is the answer, not the stated value");

    // The `padding` shorthand fills every edge no per-edge key states.
    Values shorthand;
    shorthand.padding = 12.0;
    shorthand.paddingTop = 3.0;
    const auto left = effectiveValue(Key::AppearancePaddingLeft, shorthand);
    const auto top = effectiveValue(Key::AppearancePaddingTop, shorthand);
    CHECK(left.has_value() && top.has_value());
    CHECK_EQ(left->number, shorthand.resolvedPadding().left);
    CHECK_MSG(left->number == 12.0, "an edge with no key of its own reads the shorthand");
    CHECK_MSG(top->number == 3.0, "a per-edge key outranks the shorthand");

    // A default the file does not state.
    const Values defaulted;
    const auto ligatures = effectiveValue(Key::FontLigatures, defaulted);
    CHECK(ligatures.has_value());
    CHECK(ligatures->boolean == Effective::ligatures(defaulted));
}

/// A stated value reaches the reader through the key, not only through the field.
TEST(testAStatedValueIsWhatTheKeyReads)
{
    Values c;
    c.fontSize = 17.5;
    const auto size = effectiveValue(Key::FontSize, c);
    CHECK(size.has_value());
    CHECK_EQ(size->number, 17.5);

    c.baselineOffset = -1.25;
    const auto baseline = effectiveValue(Key::FontBaselineOffset, c);
    CHECK(baseline.has_value());
    CHECK_EQ(baseline->number, -1.25);

    c.cursorBlink = false;
    const auto blink = effectiveValue(Key::CursorBlink, c);
    CHECK(blink.has_value());
    CHECK(blink->boolean == false);
    CHECK_MSG(blink->kind == SerializeKind::Bool, "a Bool-kind key reads in the boolean slot");
}

/// `[clipboard] remote_tools` reads as the boolean a settings row would edit,
/// and reads FALSE when nothing is set.
///
/// The sweep above proves every key resolves in its declared KIND, which an
/// inverted read passes — kind is not value. This is the value, and it is worth
/// stating for this key in particular: true here means every process on a
/// remote host can read what the user copied, so a preference that read
/// backwards would serve a clipboard nobody offered.
///
/// The unset case is the one a default cannot be trusted to carry: an absent
/// key must read as off rather than as whatever an empty optional produces.
TEST(testRemoteClipboardToolsReadsAsTheToggleARowWouldEdit)
{
    Values unset;
    const auto absent = effectiveValue(Key::ClipboardRemoteTools, unset);
    CHECK(absent.has_value());
    CHECK_MSG(absent->boolean == false, "an unset clipboard preference does not offer this machine's clipboard");
    CHECK_MSG(absent->kind == SerializeKind::LegacyBoolOrToken,
        "the key reads in the kind its descriptor declares, so a writer emits a shape the parser takes back");

    Values serving;
    serving.remoteClipboardTools = RemoteClipboardTools::Client;
    const auto on = effectiveValue(Key::ClipboardRemoteTools, serving);
    CHECK(on.has_value());
    CHECK_MSG(on->boolean == true, "client means the row is on");

    Values off;
    off.remoteClipboardTools = RemoteClipboardTools::Off;
    const auto explicitlyOff = effectiveValue(Key::ClipboardRemoteTools, off);
    CHECK(explicitlyOff.has_value());
    CHECK(explicitlyOff->boolean == false);
}

/// A write refuses a value whose kind is not the key's.
///
/// Without the check the literal would be written in the wrong shape and the
/// parse would skip it as the wrong value kind — a silent no-op, which is the
/// failure a settings row cannot see.
TEST(testAWriteRefusesTheWrongKind)
{
    const Value wrong = Value::ofBool(SerializeKind::Bool, true);
    CHECK_MSG(!writeValue(Key::FontSize, wrong, "/dev/null"),
        "a boolean offered for a numeric key is refused rather than written");
}

/// A value written through the key parses back as the value read through it.
///
/// The round trip is the claim worth making: it exercises the descriptor, the
/// serializer, the writer and the parse together, so a disagreement anywhere in
/// that chain shows up here rather than as a setting that will not stick.
TEST(testAWrittenValueParsesBackThroughTheKey)
{
    char directoryTemplate[] = "/tmp/ubiquitty-effective-value.XXXXXX";
    const char* directory = ::mkdtemp(directoryTemplate);
    CHECK(directory != nullptr);
    if (directory == nullptr)
        return;
    const std::string path = std::string(directory) + "/config.toml";

    // Written in the descriptor's kind, which for this key is Double -- the
    // field, the parse and `effectiveValue` all hold one, and the write recipe
    // now agrees with them. A HALF point is the value on purpose: a whole one
    // would round-trip under either spelling and say nothing about which.
    CHECK(writeValue(Key::FontSize, Value::ofNumber(SerializeKind::Double, 15.5), path));
    CHECK(writeValue(Key::FontBaselineOffset, Value::ofNumber(SerializeKind::Double, -1.25), path));
    CHECK(writeValue(Key::FontLigatures, Value::ofBool(SerializeKind::Bool, false), path));

    const std::string previous = Ubiquitty::Preferences::liveDocument();
    CHECK(Ubiquitty::Preferences::configureFromFile(path, Ubiquitty::Preferences::ParseMode::Strict).isApplied);
    const Values parsed = Ubiquitty::Preferences::values();
    (void)Ubiquitty::Preferences::configure(previous, Ubiquitty::Preferences::ParseMode::Permissive);

    const auto size = effectiveValue(Key::FontSize, parsed);
    CHECK(size.has_value());
    CHECK_EQ(size->number, 15.5);

    const auto baseline = effectiveValue(Key::FontBaselineOffset, parsed);
    CHECK(baseline.has_value());
    CHECK_EQ(baseline->number, -1.25);

    const auto ligatures = effectiveValue(Key::FontLigatures, parsed);
    CHECK(ligatures.has_value());
    CHECK_MSG(ligatures->boolean == false, "a false written through the key reads back false");

    ::unlink(path.c_str());
    ::rmdir(directory);
}

// The body overload makes the same edit as the path one for a caller holding
// the bytes rather than a name — the browser has no file, and the POSIX backend
// does not publish where a blob lives, so `preferencesLoad` is all a frontend
// has. What it must not do is reformat what it did not change: a person's
// config.toml is a file they wrote, and a writer that reflows it is one they
// stop using.
TEST(testBodyWritePreservesEverythingItDidNotChange)
{
    using Ubiquitty::Preferences::Key;
    using Ubiquitty::Preferences::SerializeKind;
    using Ubiquitty::Preferences::Value;
    using Ubiquitty::Preferences::writeValueIntoBody;

    const std::string before = "# a person's own note\n"
                               "[font]\n"
                               "family = \"Menlo\"  # trailing comment\n"
                               "size = 12\n"
                               "\n"
                               "[cursor]\n"
                               "blink = true\n";

    const std::optional<std::string> after
        = writeValueIntoBody(Key::FontSize, Value::ofNumber(SerializeKind::Double, 16.0), before);
    CHECK(after.has_value());
    if (!after.has_value())
        return;
    // `16.0`, not `16`: [font] size holds a double, so its serializer writes one
    // — which is what lets a half point survive a round trip through the file.
    CHECK_MSG(after->find("size = 16.0") != std::string::npos, "the key it was asked to change is changed");
    CHECK_MSG(after->find("# a person's own note") != std::string::npos, "a leading comment survives");
    CHECK_MSG(after->find("family = \"Menlo\"  # trailing comment") != std::string::npos,
        "an untouched line keeps its trailing comment and its spacing");
    CHECK_MSG(after->find("[cursor]") != std::string::npos && after->find("blink = true") != std::string::npos,
        "a later section is not disturbed");
    CHECK_MSG(after->find("size = 12") == std::string::npos, "the old value is gone rather than duplicated");

    // A key whose section is absent brings the section with it, which is what a
    // first write of an untouched setting does.
    const std::optional<std::string> added
        = writeValueIntoBody(Key::BellAudible, Value::ofBool(SerializeKind::Bool, false), before);
    CHECK(added.has_value());
    if (added.has_value()) {
        CHECK_MSG(added->find("[bell]") != std::string::npos, "the missing section is appended");
        CHECK_MSG(added->find("audible = false") != std::string::npos, "with the key in it");
    }

    // An empty store is a first write on a fresh machine, not a failure.
    const std::optional<std::string> fresh
        = writeValueIntoBody(Key::FontSize, Value::ofNumber(SerializeKind::Double, 14.5), std::string {});
    CHECK(fresh.has_value() && fresh->find("[font]") != std::string::npos && fresh->find("size = 14.5") != std::string::npos);
}

// The two refusals, kept distinguishable from a body that came back unchanged —
// which is why this returns an optional rather than a string.
TEST(testBodyWriteRefusesAKindItCannotHonour)
{
    using Ubiquitty::Preferences::Key;
    using Ubiquitty::Preferences::SerializeKind;
    using Ubiquitty::Preferences::Value;
    using Ubiquitty::Preferences::writeValueIntoBody;

    // [terminal] scrollback_lines is a genuine Int key — a count of rows — so a
    // double handed to it would write a literal the reader parses back as
    // something else. [font] size cannot be the example: it holds a double, so a
    // case using it would pass because the kinds agree rather than because the
    // mismatch was refused.
    CHECK(!writeValueIntoBody(Key::TerminalScrollbackLines, Value::ofNumber(SerializeKind::Double, 15.5), std::string {})
            .has_value());
    // And a key cast in from outside the enumeration has no descriptor.
    CHECK(!writeValueIntoBody(static_cast<Key>(9999), Value::ofBool(SerializeKind::Bool, true), std::string {}).has_value());
}

/// The body-taking write lands the text the file-taking one would have left.
///
/// Two write paths, one splice underneath, so what is worth pinning is that
/// they stay one: a frontend without a file path must produce the same bytes as
/// the frontend that has one, or the same preference set from two surfaces
/// writes two different files.
///
/// Compared over every key rather than a chosen few, because a divergence would
/// live in whichever key had the shape nobody picked. The two are accumulated
/// key by key against a shared file and a shared body, so this compares two
/// splice sequences rather than two single writes -- an edit that is correct
/// into empty text and wrong into text that already holds the key shows up
/// here and nowhere else.
///
/// This does not overlap ``testBodyWritePreservesEverythingItDidNotChange``:
/// that one reads the surrounding text back, and would still pass if the two
/// paths shared a single broken splice. This one would still pass if the splice
/// discarded everything, so long as it discarded identically. Each is the
/// other's blind spot.
TEST(testTheBodyWriteAgreesWithTheFileWrite)
{
    using Ubiquitty::Preferences::writeValueIntoBody;

    char directoryTemplate[] = "/tmp/ubiquitty-body-agreement.XXXXXX";
    const char* directory = ::mkdtemp(directoryTemplate);
    CHECK(directory != nullptr);
    if (directory == nullptr)
        return;
    const std::string path = std::string(directory) + "/config.toml";

    std::string body;
    int written = 0;
    for (int i = 0; i < writeDescriptorCount; ++i) {
        const auto& descriptor = writeDescriptors[i];
        // A value in the descriptor's own kind, so neither path refuses.
        Value value;
        switch (descriptor.kind) {
        case SerializeKind::Bool:
        case SerializeKind::LegacyBoolOrToken:
            value = Value::ofBool(descriptor.kind, true);
            break;
        case SerializeKind::Int:
            value = Value::ofInteger(descriptor.kind, 7);
            break;
        case SerializeKind::Double:
            value = Value::ofNumber(descriptor.kind, 1.5);
            break;
        case SerializeKind::EnumToken:
        case SerializeKind::String:
            value = Value::ofText(descriptor.kind, "x");
            break;
        }

        const auto spliced = writeValueIntoBody(descriptor.id, value, body);
        if (!spliced.has_value()) {
            std::printf("FAIL [%s] %s refused a value in its own kind\n", descriptor.section, descriptor.key);
            failures++;
            continue;
        }
        if (!writeValue(descriptor.id, value, path)) {
            std::printf("FAIL [%s] %s did not write through the path\n", descriptor.section, descriptor.key);
            failures++;
            continue;
        }
        body = *spliced;
        ++written;
    }
    // The sweep covers a population, so its size is asserted: a descriptor
    // table that lost most of itself would still pass every comparison above.
    CHECK_EQ(written, writeDescriptorCount);

    std::ifstream file(path);
    std::ostringstream onDisk;
    onDisk << file.rdbuf();
    CHECK_MSG(onDisk.str() == body, "the two write paths leave the same text");

    ::unlink(path.c_str());
    ::rmdir(directory);
}

TEST(testDefaultTerminalIdentityAndExplicitOverrides)
{
    using Ubiquitty::Preferences::terminalIdentity;
    Values configuration;
    CHECK(terminalIdentity(configuration).program == "Ubiquitty");
    CHECK(effectiveValue(Key::TermProgram, configuration)->text == "Ubiquitty");

    configuration.termProgram = "custom-terminal";
    CHECK(terminalIdentity(configuration).program == "custom-terminal");
    CHECK(effectiveValue(Key::TermProgram, configuration)->text == "custom-terminal");

    configuration.termProgram = "";
    CHECK(terminalIdentity(configuration).program.empty());
    CHECK(effectiveValue(Key::TermProgram, configuration)->text.empty());
}

UBIQUITTY_TEST_MAIN()
