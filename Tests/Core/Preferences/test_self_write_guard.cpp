// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// The app's own writes to the preferences file are told apart from a person's:
// each own write records the bytes it left in the file, and a change event that
// finds exactly those bytes is the app's own and reloads nothing. These pin the
// answers — every event of a burst of own writes is the app's, a person's save
// is foreign even when it lands before the handler and even when it restores
// the app's bytes, a failed write records nothing — and that every own write
// lands whole and tells the listener mirroring the values.

#include "Core/Preferences/Configuration.h"
#include "Core/Preferences/EffectiveValue.h"
#include "Core/Preferences/Key.h"
#include "Core/Preferences/PreferencesAccessors.h"
#include "Core/Preferences/SelfWriteGuard.h"
#include "PAL/PAL.h"

#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <unistd.h>

using Ubiquitty::Preferences::ChangeOrigin;
using Ubiquitty::Preferences::configure;
using Ubiquitty::Preferences::configureOwnDocument;
using Ubiquitty::Preferences::configureOwnSectionKey;
using Ubiquitty::Preferences::originOfFileChange;
using Ubiquitty::Preferences::ParseMode;
using Ubiquitty::Preferences::removeConfiguredSectionKey;
using Ubiquitty::Preferences::SelfWriteGuard;
using Ubiquitty::Preferences::setOwnWriteListener;

namespace {

/// Points the preferences directory at a fresh temporary one for one test and
/// puts `XDG_CONFIG_HOME` back afterwards, unset and empty kept apart.
class TemporaryPreferencesHome {
public:
    TemporaryPreferencesHome()
    {
        if (const char* value = std::getenv("XDG_CONFIG_HOME"))
            m_saved = value;
        char pattern[] = "/tmp/ubq-self-write-XXXXXX";
        if (const char* made = ::mkdtemp(pattern))
            m_directory = made;
        ::setenv("XDG_CONFIG_HOME", m_directory.c_str(), 1);
    }
    ~TemporaryPreferencesHome()
    {
        if (m_saved)
            ::setenv("XDG_CONFIG_HOME", m_saved->c_str(), 1);
        else
            ::unsetenv("XDG_CONFIG_HOME");
        ::unlink(filePath().c_str());
        ::rmdir((m_directory + "/ubiquitty").c_str());
        ::rmdir(m_directory.c_str());
    }
    TemporaryPreferencesHome(const TemporaryPreferencesHome&) = delete;
    TemporaryPreferencesHome& operator=(const TemporaryPreferencesHome&) = delete;

    [[nodiscard]] std::string filePath() const { return m_directory + "/ubiquitty/config.toml"; }

    /// Points the preferences directory somewhere no file can be written for
    /// the scope of `body`, so a write in it fails.
    template<typename Body>
    void withUnwritableHome(Body body) const
    {
        ::setenv("XDG_CONFIG_HOME", "/dev/null/not-a-directory", 1);
        body();
        ::setenv("XDG_CONFIG_HOME", m_directory.c_str(), 1);
    }

private:
    std::string m_directory;
    std::optional<std::string> m_saved;
};

std::string contentsOf(const std::string& path)
{
    std::ifstream in(path);
    std::ostringstream contents;
    contents << in.rdbuf();
    return contents.str();
}

/// A save the app did not make, landed the way an editor's atomic save lands.
bool saveAsAPerson(const std::string& path, std::string_view text)
{
    return Ubiquitty::PAL::storageWriteAtomic(path.c_str(), std::as_bytes(std::span { text }));
}

/// Entries in the directory holding `path`, the dot entries aside.
int entriesBeside(const std::string& path)
{
    const std::string directory = path.substr(0, path.rfind('/'));
    int count = 0;
    if (DIR* listing = ::opendir(directory.c_str())) {
        while (const dirent* entry = ::readdir(listing)) {
            const std::string name = entry->d_name;
            if (name != "." && name != "..")
                ++count;
        }
        ::closedir(listing);
    }
    return count;
}

int listenerCalls = 0;
void countListenerCall() { ++listenerCalls; }

} // namespace

// Every event over the app's bytes is the app's, however many events one burst
// of writes raised.
TEST(testEveryEventOverTheAppsBytesIsItsOwn)
{
    SelfWriteGuard guard;
    guard.noteOwnWrite("[mouse]\nscroll_bounce = true\n");
    CHECK(guard.originOf(std::string("[mouse]\nscroll_bounce = true\n")) == ChangeOrigin::OwnWrite);
    CHECK(guard.originOf(std::string("[mouse]\nscroll_bounce = true\n")) == ChangeOrigin::OwnWrite);
    CHECK(guard.hasOwnWrite());
}

// Other bytes are a person's save, and from there the file is theirs: a later
// save that restores the app's bytes is theirs too, and reloads.
TEST(testOtherBytesAreForeignAndTheFileBecomesThePersons)
{
    SelfWriteGuard guard;
    guard.noteOwnWrite("mine");
    CHECK(guard.originOf(std::string("theirs")) == ChangeOrigin::Foreign);
    CHECK_MSG(guard.originOf(std::string("mine")) == ChangeOrigin::Foreign, "a person restoring the app's bytes still reloads");
    CHECK(!guard.hasOwnWrite());
}

// A file that cannot be read, or a guard the app never wrote through, answers
// foreign: the reload is the safe side of not knowing.
TEST(testWhatCannotBeComparedIsForeign)
{
    SelfWriteGuard unwritten;
    CHECK(unwritten.originOf(std::string("anything")) == ChangeOrigin::Foreign);
    SelfWriteGuard guard;
    guard.noteOwnWrite("mine");
    CHECK(guard.originOf(std::nullopt) == ChangeOrigin::Foreign);
}

// A burst of own writes that land before the watcher's handler runs answers as
// the app's: the handler reads the last write's bytes, whichever write raised
// the event it is answering.
TEST(testABurstOfOwnWritesIsOneOwnChange)
{
    TemporaryPreferencesHome home;
    (void)configure("", ParseMode::Permissive);
    CHECK(configureOwnSectionKey("mouse", "scroll_bounce", "true"));
    CHECK(configureOwnSectionKey("mouse", "scroll_bounce", "false"));
    CHECK(configureOwnSectionKey("selection", "copy_on_select", "true"));
    CHECK(originOfFileChange() == ChangeOrigin::OwnWrite);
    CHECK_MSG(originOfFileChange() == ChangeOrigin::OwnWrite, "a second event over the same bytes is the app's too");
}

// A person's save that lands after an own write and before the handler runs
// is foreign: the bytes are theirs.
TEST(testAPersonsSaveBeforeTheHandlerRunsReloads)
{
    TemporaryPreferencesHome home;
    (void)configure("", ParseMode::Permissive);
    CHECK(configureOwnSectionKey("mouse", "scroll_bounce", "true"));
    CHECK(saveAsAPerson(home.filePath(), "[mouse]\nscroll_bounce = false\n"));
    CHECK(originOfFileChange() == ChangeOrigin::Foreign);
}

// A write that never reached the file records nothing: the file still holds
// the last write that did, and its event is still the app's.
TEST(testAFailedWriteRecordsNothing)
{
    TemporaryPreferencesHome home;
    (void)configure("", ParseMode::Permissive);
    (void)configureOwnDocument("[mouse]\n");
    home.withUnwritableHome([] { (void)configureOwnDocument("[other]\n"); });
    CHECK(contentsOf(home.filePath()) == "[mouse]\n");
    CHECK(originOfFileChange() == ChangeOrigin::OwnWrite);
}

// A key write lands in the resolved file, keeps what the person wrote around
// it, and leaves no staging file beside it.
TEST(testAnOwnKeyWriteLandsWhole)
{
    TemporaryPreferencesHome home;
    const std::string_view persons = "# mine\n[mouse]\nscroll_bounce = false\n";
    CHECK(saveAsAPerson(home.filePath(), persons));
    (void)configure(std::string(persons), ParseMode::Permissive);
    CHECK(configureOwnSectionKey("mouse", "scroll_bounce", "true"));
    const std::string written = contentsOf(home.filePath());
    CHECK_MSG(written.find("scroll_bounce = true") != std::string::npos, "the key took the new value");
    CHECK_MSG(written.find("# mine") != std::string::npos, "the person's comment survived");
    CHECK_MSG(entriesBeside(home.filePath()) == 1, "the staging file was renamed over the target, not left beside it");
    CHECK(originOfFileChange() == ChangeOrigin::OwnWrite);
}

// A removal is an own write too.
TEST(testAnOwnRemovalIsAnOwnWrite)
{
    TemporaryPreferencesHome home;
    const std::string_view persons = "[macros]\ngreet = \"hello\"\nleave = \"bye\"\n";
    CHECK(saveAsAPerson(home.filePath(), persons));
    (void)configure(std::string(persons), ParseMode::Permissive);
    CHECK(originOfFileChange() == ChangeOrigin::Foreign);
    CHECK(removeConfiguredSectionKey("macros", "greet"));
    const std::string written = contentsOf(home.filePath());
    CHECK(written.find("greet") == std::string::npos);
    CHECK(written.find("leave = \"bye\"") != std::string::npos);
    CHECK(originOfFileChange() == ChangeOrigin::OwnWrite);
}

// Whatever mirrors the live values hears every own write, the failed ones
// included: the live values moved whatever the disk said.
TEST(testTheListenerHearsEveryOwnWrite)
{
    TemporaryPreferencesHome home;
    (void)configure("", ParseMode::Permissive);
    listenerCalls = 0;
    setOwnWriteListener(countListenerCall);
    (void)configureOwnDocument("[mouse]\n");
    CHECK(configureOwnSectionKey("mouse", "scroll_bounce", "true"));
    home.withUnwritableHome([] { (void)configureOwnDocument("[mouse]\n"); });
    setOwnWriteListener(nullptr);
    (void)configureOwnDocument("[mouse]\n");
    CHECK_MSG(listenerCalls == 3, "three writes while installed, none after");
}

// A settings row's edit moves the live values, rewrites the live document in
// place and lands that same text in the file as the app's own write, so the
// watcher's event that follows reloads nothing.
TEST(testAConfiguredKeyIsLiveAndPersistedAsOne)
{
    namespace Preferences = Ubiquitty::Preferences;
    TemporaryPreferencesHome home;
    (void)Preferences::configure("# mine\n[font]\nsize = 13.0\n", Preferences::ParseMode::Permissive);
    const std::optional<std::string> document = Preferences::configureOwnKey(Preferences::Key::FontSize,
        Preferences::Value::ofNumber(Preferences::SerializeKind::Double, 18.0));
    CHECK(document.has_value());
    CHECK(Preferences::Effective::fontSize(Preferences::values()) == 18.0);
    const std::string written = contentsOf(home.filePath());
    CHECK_MSG(written == Preferences::liveDocument(), "the file holds the live document");
    CHECK_MSG(written.find("# mine") != std::string::npos, "the reader's comment survived");
    CHECK(originOfFileChange() == ChangeOrigin::OwnWrite);

    // A value of the wrong kind changes nothing and writes nothing.
    CHECK(!Preferences::configureOwnKey(Preferences::Key::FontSize, Preferences::Value::ofBool(Preferences::SerializeKind::Bool, true)));
    CHECK(Preferences::Effective::fontSize(Preferences::values()) == 18.0);

    // A key no `Key` names goes through the section writer, as one edit.
    (void)Preferences::configureOwnSectionKey("macros", "greet", "\"hello\"");
    CHECK(contentsOf(home.filePath()).find("greet = \"hello\"") != std::string::npos);
    (void)Preferences::removeConfiguredSectionKey("macros", "greet");
    CHECK(contentsOf(home.filePath()).find("greet") == std::string::npos);
    CHECK(originOfFileChange() == ChangeOrigin::OwnWrite);
}

UBIQUITTY_TEST_MAIN()
