// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_deploypolicy.cpp — the portable auto-deploy decision
// (Session/DeployPolicy.h::decideDeployAction), the gate every remote session's host
// deploy passes. Pins the 2x2 matrix over the two inputs that
// drive the verdict (auto-install setting x bundled-binary presence):
//
//   autoInstall  hasBinary  ->  action
//   -----------  ---------      ------
//   off          *          ->  Off          (checked first: it's a setting)
//   on           no         ->  ManualInstall (no prebuilt for this platform)
//   on           yes        ->  Push          (stream the bundled binary)
//
// The trust-on-first-use list that feeds `hostAlreadyTrusted` is pinned here
// too, through Runtime::HostDirectory's persistence seam.

#include "Core/Protocol/Sync/AppVersion.h"
#include "Core/Runtime/HostDirectory.h"
#include "Core/Session/DeployPolicy.h"

#include <algorithm>
#include <compare>
#include <cstdio>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using Ubiquitty::Session::decideDeployAction;
using Ubiquitty::Session::DeployAction;
using Ubiquitty::Session::deployFailureDetail;
using Ubiquitty::Session::lastLines;
using Ubiquitty::Session::manualHostInstallCommands;
using Ubiquitty::Session::publishedHostAsset;

static void expectTail(const std::string& text, std::size_t maxLines, const std::string& want, const char* label)
{
    const std::string got = lastLines(text, maxLines);
    if (got != want) {
        printf("FAIL lastLines %s: got \"%s\", want \"%s\"\n", label, got.c_str(), want.c_str());
        ++failures;
    }
}

TEST(deploypolicyCases)
{

    // ---- auto-install OFF: always Off, regardless of binary or trust ----
    // Checked FIRST so the user learns it's a setting, not a missing binary.
    CHECK(decideDeployAction(/*configAutoInstall=*/false, "Linux x86_64",
              /*hasBundledBinary=*/true, /*hostAlreadyTrusted=*/true)
        == DeployAction::Off);
    CHECK(decideDeployAction(false, "Linux x86_64", false, false) == DeployAction::Off);
    CHECK(decideDeployAction(false, "Darwin arm64", true, false) == DeployAction::Off);

    // ---- auto-install ON, no bundled binary: ManualInstall (trust irrelevant) ----
    CHECK(decideDeployAction(true, "Linux x86_64", false, true) == DeployAction::ManualInstall);
    CHECK(decideDeployAction(true, "FreeBSD amd64", false, false) == DeployAction::ManualInstall); // exotic platform
    CHECK(decideDeployAction(true, "", false, false) == DeployAction::ManualInstall); // empty arch

    // ---- auto-install ON, bundled binary, host not yet trusted: ConfirmPush (TOFU) ----
    CHECK(decideDeployAction(true, "Linux x86_64", true, false) == DeployAction::ConfirmPush);
    CHECK(decideDeployAction(true, "Darwin arm64", true, false) == DeployAction::ConfirmPush);

    // ---- auto-install ON, bundled binary, host already trusted: Push ----
    CHECK(decideDeployAction(true, "Linux x86_64", true, true) == DeployAction::Push);
    CHECK(decideDeployAction(true, "Linux aarch64", true, true) == DeployAction::Push);
    CHECK(decideDeployAction(true, "Darwin arm64", true, true) == DeployAction::Push);

    // ---- lastLines: the deploy-log tail helper (DeployPolicy.h) -----------
    // Fewer lines than the cap: the whole text comes back.
    expectTail("a\nb\nc", 20, "a\nb\nc", "shorter than cap returns all");
    expectTail("only", 20, "only", "single line, no newline");
    expectTail("", 20, "", "empty input");

    // Exactly the cap, and more than the cap (keep the LAST maxLines).
    expectTail("l1\nl2\nl3", 3, "l1\nl2\nl3", "exactly the cap");
    expectTail("l1\nl2\nl3\nl4\nl5", 2, "l4\nl5", "more than cap keeps last");
    expectTail("a\nb\nc\nd", 1, "d", "cap of one keeps final line");

    // Trailing empties are trimmed before the tail is taken, so a build log
    // ending in a blank line still surfaces real content.
    expectTail("err1\nerr2\n", 20, "err1\nerr2", "single trailing newline trimmed");
    expectTail("err1\nerr2\n\n\n", 20, "err1\nerr2", "run of trailing blanks trimmed");
    expectTail("a\nb\nc\n", 2, "b\nc", "trim then tail keeps real lines, not blanks");
    expectTail("\n\n\n", 20, "", "all-blank input collapses to empty");

    // Interior blank lines are preserved (only TRAILING empties are trimmed).
    expectTail("a\n\nb", 20, "a\n\nb", "interior blank preserved");

    // maxLines == 0 yields empty.
    expectTail("a\nb\nc", 0, "", "cap of zero yields empty");
}

// A failed push shows the person the last lines the push printed, which are
// ssh's or the remote shell's own words; a push that printed nothing still
// says something.
TEST(deployFailureDetailCases)
{
    std::string output;
    for (int line = 1; line <= 30; ++line)
        output += "line " + std::to_string(line) + "\n";
    const std::string detail = deployFailureDetail(output);
    CHECK(detail.rfind("line 11\n", 0) == 0);
    CHECK(detail.ends_with("line 30"));
    CHECK(detail.find("line 10\n") == std::string::npos);
    CHECK(deployFailureDetail("ssh: Could not resolve hostname nowhere: nodename nor servname provided\n")
        == "ssh: Could not resolve hostname nowhere: nodename nor servname provided");
    CHECK(deployFailureDetail("") == "the host install did not complete");
    CHECK(deployFailureDetail("\n\n") == "the host install did not complete");
}

// The manual steps download the published host for a platform a release
// carries, keyed like the bundled deploy map, at the client's own named
// version, and check it against the release's SHA256SUMS before it replaces
// anything. Any other platform, and a build that names no release, is told so
// in a sentence with nothing to run: no checkout, no build.
TEST(manualHostInstallCommandCases)
{
    CHECK(publishedHostAsset("Linux x86_64") == "ubiquitty-host-linux-x86_64");
    CHECK(publishedHostAsset("Linux aarch64") == "ubiquitty-host-linux-arm64");
    CHECK(publishedHostAsset("Darwin arm64") == "ubiquitty-host-macos-arm64");
    // The app is arm64 only, so no release builds an Intel Mac's host.
    CHECK(publishedHostAsset("Darwin x86_64").empty());
    CHECK(publishedHostAsset("Linux armv7l").empty());
    CHECK(publishedHostAsset("FreeBSD amd64").empty());
    CHECK(publishedHostAsset("").empty());

    CHECK_STR_EQ(manualHostInstallCommands("Linux aarch64", "0.2.1"),
        "sh -c 'set -eu\n"
        "url=https://ubiquitty.org/download/host/0.2.1\n"
        "asset=ubiquitty-host-linux-arm64\n"
        "mkdir -p ~/.ubiquitty\n"
        "cd ~/.ubiquitty\n"
        "curl -fsSL -o ubiquitty-host.download \"$url/$asset\"\n"
        "want=$(curl -fsSL \"$url/SHA256SUMS\" | grep \"  $asset\\$\" || true)\n"
        "if command -v sha256sum >/dev/null; then got=$(sha256sum ubiquitty-host.download); else got=$(shasum -a 256 ubiquitty-host.download); fi\n"
        "if [ \"${got%% *}\" != \"${want%% *}\" ]; then echo \"The download doesn’t match SHA256SUMS.\" >&2; exit 1; fi\n"
        "chmod 755 ubiquitty-host.download\n"
        "mv -f ubiquitty-host.download ubiquitty-host\n"
        "./ubiquitty-host --version'");

    // Every published platform gets the same steps, naming its own file.
    for (const auto& [platform, asset] : std::vector<std::pair<std::string, std::string>> {
             { "Linux x86_64", "ubiquitty-host-linux-x86_64" },
             { "Linux aarch64", "ubiquitty-host-linux-arm64" },
             { "Darwin arm64", "ubiquitty-host-macos-arm64" },
         }) {
        const std::string steps = manualHostInstallCommands(platform, "0.2.1");
        CHECK(steps.find("\nasset=" + asset + "\n") != std::string::npos);
        CHECK(steps.find("url=https://ubiquitty.org/download/host/0.2.1\n") != std::string::npos);
        CHECK(steps.find("$url/SHA256SUMS") != std::string::npos);
        CHECK(steps.find("mv -f ubiquitty-host.download ubiquitty-host\n") != std::string::npos);
        // Pinned, never the latest pointer, and built nowhere.
        CHECK(steps.find("/download/host/linux-") == std::string::npos);
        CHECK(steps.find("/download/host/macos-") == std::string::npos);
        CHECK(steps.find("git") == std::string::npos);
        CHECK(steps.find("build-host") == std::string::npos);
        // `sh -c '…'` holds the whole script, so the script holds no quote of
        // its own that would end it early.
        CHECK(steps.starts_with("sh -c '") && steps.ends_with("'"));
        CHECK(steps.find('\'', std::string_view("sh -c '").size()) == steps.size() - 1);
    }

    // A build between named versions downloads the one it follows.
    CHECK(manualHostInstallCommands("Linux x86_64", "0.2.1.5").find("url=https://ubiquitty.org/download/host/0.2.1\n") != std::string::npos);
    CHECK(manualHostInstallCommands("Darwin arm64", "12.3.40").find("url=https://ubiquitty.org/download/host/12.3.40\n") != std::string::npos);

    CHECK_STR_EQ(manualHostInstallCommands("FreeBSD amd64", "0.2.1"), "There’s no prebuilt ubiquitty-host for FreeBSD amd64 yet.");
    CHECK_STR_EQ(manualHostInstallCommands("Darwin x86_64", "0.2.1"), "There’s no prebuilt ubiquitty-host for Darwin x86_64 yet.");
    CHECK_STR_EQ(manualHostInstallCommands("", "0.2.1"), "There’s no prebuilt ubiquitty-host for this platform yet.");
    for (const char* version : { "0.0.0-dev", "", "0.2", "v0.2.1", "0.2.1.0" }) {
        CHECK_STR_EQ(manualHostInstallCommands("Linux x86_64", version),
            "This build of Ubiquitty isn’t a release, so no prebuilt ubiquitty-host matches it.");
    }
}

namespace {

using Ubiquitty::Protocol::Sync::PeerBuild;

/// A build as a refusal states it.
PeerBuild build(const char* appVersion, std::uint16_t version)
{
    return { .isKnown = true, .wireVersion = version, .appVersion = appVersion };
}

/// The facts of a refusal between this Mac at `local` and the host "Studio" at
/// `host`.
Ubiquitty::Session::UpdateFacts refusalBetween(PeerBuild local, PeerBuild host)
{
    Ubiquitty::Session::UpdateFacts facts;
    facts.local = std::move(local);
    facts.host = std::move(host);
    facts.hostLabel = "Studio";
    facts.localDevice.kind = Ubiquitty::Session::LocalDeviceKind::Mac;
    facts.hasBundledBinary = true;
    return facts;
}

} // namespace

// A version refusal offers the update wherever this Mac carries a host for the
// machine and the host is the older end, and the manual commands where it
// carries none. A refusal that names this client the older end offers nothing:
// a push there would overwrite a newer host with this build. A host that states
// no build predates the published wire, so it is older and is offered the
// update.
TEST(aVersionRefusalDoesNotDowngradeAKnownNewerHost)
{
    using Ubiquitty::Session::decideUpdateAction;
    using Ubiquitty::Session::hostUpdateNotice;
    using Ubiquitty::Session::UpdateAction;
    using Ubiquitty::Session::UpdateFacts;

    UpdateFacts hostOlder = refusalBetween(build("0.2.1.4", 5), build("0.2.0.7", 3));
    CHECK(decideUpdateAction(hostOlder) == UpdateAction::OfferHostUpdate);
    CHECK(!hostUpdateNotice(hostOlder).empty());
    hostOlder.hasBundledBinary = false;
    CHECK(decideUpdateAction(hostOlder) == UpdateAction::ManualUpdate);

    UpdateFacts silent = refusalBetween(build("0.2.1.4", 5), PeerBuild {});
    CHECK(decideUpdateAction(silent) == UpdateAction::OfferHostUpdate);

    const UpdateFacts clientOlder = refusalBetween(build("0.2.0.7", 3), build("0.2.1.4", 5));
    CHECK(decideUpdateAction(clientOlder) == UpdateAction::None);
    CHECK(hostUpdateNotice(clientOlder).empty());

    // Equal wire versions identify different builds without choosing an
    // older side. Installing this app's matching host remains an explicit choice.
    const UpdateFacts same = refusalBetween(build("0.2.1.4", 5), build("0.2.2", 5));
    CHECK(decideUpdateAction(same) == UpdateAction::OfferHostUpdate);

    // The notice explains that live sessions move and refusing holders keep them.
    UpdateFacts facts = refusalBetween(build("0.2.1.4", 5), build("0.2.0.7", 3));
    facts.heldSessionCount = 0;
    CHECK(hostUpdateNotice(facts).find("no sessions") != std::string::npos);
    facts.heldSessionCount = 1;
    CHECK(hostUpdateNotice(facts).find("running session moves to the new host") != std::string::npos);
    facts.heldSessionCount = 4;
    CHECK(hostUpdateNotice(facts).find("4 running sessions") != std::string::npos);
}

// A host installed before versions counted commits states `0.9.1`, a larger
// number than any `0.2.0.N` this client states. Another wire version decides
// independently of the app numbers: that host is offered this client's build
// when its protocol is below the client's. Equal protocols name no older side.
TEST(aHostFromBeforeCountedVersionsIsJudgedByItsProtocolNotItsNumber)
{
    using Ubiquitty::Protocol::Sync::compareAppVersions;
    using Ubiquitty::Session::decideUpdateAction;
    using Ubiquitty::Session::olderSideOf;
    using Ubiquitty::Session::UpdateAction;
    using Ubiquitty::Session::versionRefusalSentence;
    using Ubiquitty::Session::versionRefusalTitle;

    // The app versions alone order the installed host after this client.
    const std::optional<std::strong_ordering> byNumber = compareAppVersions("0.9.1", "0.2.0.7");
    CHECK(byNumber && *byNumber == std::strong_ordering::greater);

    // Its protocol is below this client's: the host is the older end
    // and is offered the update.
    const auto installedHostOlder = refusalBetween(build("0.2.0.7", 20), build("0.9.1", 19));
    CHECK(olderSideOf(installedHostOlder) == Ubiquitty::Protocol::Sync::OlderSide::Host);
    CHECK(decideUpdateAction(installedHostOlder) == UpdateAction::OfferHostUpdate);
    CHECK(versionRefusalSentence(installedHostOlder)
        == "Studio’s Ubiquitty Host is from an older build (0.9.1) than this Mac (0.2.0.7). Update the host on Studio.");
    CHECK(versionRefusalTitle(installedHostOlder) == "Studio Runs an Older Ubiquitty Host");

    // The same host speaking this client's protocol names no newer build.
    const auto installedHostCurrent = refusalBetween(build("0.2.0.7", 20), build("0.9.1", 20));
    CHECK(olderSideOf(installedHostCurrent) == Ubiquitty::Protocol::Sync::OlderSide::Unknown);
    CHECK(decideUpdateAction(installedHostCurrent) == UpdateAction::OfferHostUpdate);
    CHECK(versionRefusalTitle(installedHostCurrent) == "Studio Runs a Different Build of Ubiquitty");
    CHECK(versionRefusalSentence(installedHostCurrent)
        == "This Mac (0.2.0.7) and Studio’s host (0.9.1) are from different builds of Ubiquitty. Install the same build on both.");

    // A counted version reads in the sentence as it is stated.
    const auto clientOlder = refusalBetween(build("0.2.0.7", 20), build("0.2.1.12", 22));
    CHECK(decideUpdateAction(clientOlder) == UpdateAction::None);
    CHECK(versionRefusalSentence(clientOlder)
        == "This Mac is from an older build of Ubiquitty (0.2.0.7) than Studio’s host (0.2.1.12). Update Ubiquitty on this Mac.");
}

// The refusal says which end is older, with both builds' facts, and which end
// to update — never "whichever is older".
TEST(aVersionRefusalNamesTheOlderEndAndBothBuilds)
{
    using Ubiquitty::Session::versionRefusalSentence;
    using Ubiquitty::Session::versionRefusalTitle;

    const auto hostOlder = refusalBetween(build("0.2.1.4", 5), build("0.2.0.7", 3));
    CHECK(versionRefusalSentence(hostOlder)
        == "Studio’s Ubiquitty Host is from an older build (0.2.0.7) than this Mac (0.2.1.4). Update the host on Studio.");
    CHECK(versionRefusalTitle(hostOlder) == "Studio Runs an Older Ubiquitty Host");

    const auto clientOlder = refusalBetween(build("0.2.0.7", 3), build("0.2.1.4", 5));
    CHECK(versionRefusalSentence(clientOlder)
        == "This Mac is from an older build of Ubiquitty (0.2.0.7) than Studio’s host (0.2.1.4). Update Ubiquitty on this Mac.");
    CHECK(versionRefusalTitle(clientOlder) == "This Mac Runs an Older Ubiquitty");

    // A host that states no build predates the published wire.
    const auto silent = refusalBetween(build("0.2.1.4", 6), PeerBuild {});
    CHECK(versionRefusalSentence(silent)
        == "Studio’s Ubiquitty Host doesn’t say which build it runs, so it’s older than this Mac (0.2.1.4). Update the host on Studio.");
    CHECK(versionRefusalTitle(silent) == "Studio Runs an Older Ubiquitty Host");

    // A host that sent no app version is named without one, and the wire's
    // number is never the person's.
    const auto bare = refusalBetween(build("0.2.1.4", 5), build("", 3));
    CHECK(versionRefusalSentence(bare).starts_with("Studio’s Ubiquitty Host is from an older build than this Mac (0.2.1.4)."));
    CHECK(versionRefusalSentence(bare).find("protocol") == std::string::npos);

    // Equal protocol versions never use app labels to choose an older end.
    const auto sameWireHostOlder = refusalBetween(build("0.2.0.9", 1), build("0.2.0.3", 1));
    CHECK(versionRefusalSentence(sameWireHostOlder)
        == "This Mac (0.2.0.9) and Studio’s host (0.2.0.3) are from different builds of Ubiquitty. Install the same build on both.");
    const auto sameWireClientOlder = refusalBetween(build("0.2.0.3", 1), build("0.2.0.9", 1));
    CHECK(versionRefusalSentence(sameWireClientOlder)
        == "This Mac (0.2.0.3) and Studio’s host (0.2.0.9) are from different builds of Ubiquitty. Install the same build on both.");
    const auto unordered = refusalBetween(build("0.2.0.7", 1), build("0.2.0.7", 1));
    CHECK(versionRefusalSentence(unordered)
        == "This Mac and Studio’s host run different builds of Ubiquitty 0.2.0.7. Install the same build on both.");
    CHECK(versionRefusalTitle(unordered) == "Studio Runs a Different Build of Ubiquitty");

    // Without a name for the host the sentence still reads.
    auto unnamed = hostOlder;
    unnamed.hostLabel.clear();
    CHECK(versionRefusalSentence(unnamed)
        == "The Ubiquitty Host there is from an older build (0.2.0.7) than this Mac (0.2.1.4). Update the host on that machine.");
    CHECK(versionRefusalTitle(unnamed) == "The Host Runs an Older Ubiquitty Host");

    for (const auto& facts : { hostOlder, clientOlder, silent, unnamed, sameWireHostOlder, sameWireClientOlder, unordered }) {
        CHECK(versionRefusalSentence(facts).find("whichever") == std::string::npos);
        CHECK(versionRefusalSentence(facts).find("protocol") == std::string::npos);
    }
}

// The local device is a fact the frontend reports, named the way the remote
// host is: by its kind, by its name where the platform gives one, and as
// "this device" where the frontend said nothing.
TEST(aVersionRefusalNamesThisDeviceByWhatTheFrontendReports)
{
    using Ubiquitty::Session::LocalDeviceKind;
    using Ubiquitty::Session::versionRefusalSentence;
    using Ubiquitty::Session::versionRefusalTitle;

    struct Case {
        LocalDeviceKind kind;
        const char* name;
        const char* sentence;
        const char* title;
    };
    const Case cases[] = {
        { LocalDeviceKind::Mac, "",
            "This Mac is from an older build of Ubiquitty (0.2.0.7) than Studio’s host (0.2.1.4). Update Ubiquitty on this Mac.",
            "This Mac Runs an Older Ubiquitty" },
        { LocalDeviceKind::IPhone, "",
            "This iPhone is from an older build of Ubiquitty (0.2.0.7) than Studio’s host (0.2.1.4). Update Ubiquitty on this iPhone.",
            "This iPhone Runs an Older Ubiquitty" },
        { LocalDeviceKind::IPad, "",
            "This iPad is from an older build of Ubiquitty (0.2.0.7) than Studio’s host (0.2.1.4). Update Ubiquitty on this iPad.",
            "This iPad Runs an Older Ubiquitty" },
        { LocalDeviceKind::Computer, "",
            "This computer is from an older build of Ubiquitty (0.2.0.7) than Studio’s host (0.2.1.4). Update Ubiquitty on this computer.",
            "This Computer Runs an Older Ubiquitty" },
        { LocalDeviceKind::Browser, "",
            "This browser is from an older build of Ubiquitty (0.2.0.7) than Studio’s host (0.2.1.4). Update Ubiquitty on this browser.",
            "This Browser Runs an Older Ubiquitty" },
        { LocalDeviceKind::Unknown, "",
            "This device is from an older build of Ubiquitty (0.2.0.7) than Studio’s host (0.2.1.4). Update Ubiquitty on this device.",
            "This Device Runs an Older Ubiquitty" },
        { LocalDeviceKind::Computer, "build-07",
            "build-07 is from an older build of Ubiquitty (0.2.0.7) than Studio’s host (0.2.1.4). Update Ubiquitty on build-07.",
            "This Computer Runs an Older Ubiquitty" },
    };
    for (const Case& expected : cases) {
        auto facts = refusalBetween(build("0.2.0.7", 3), build("0.2.1.4", 5));
        facts.localDevice = { .kind = expected.kind, .name = expected.name };
        CHECK_MSG(versionRefusalSentence(facts) == expected.sentence, versionRefusalSentence(facts).c_str());
        CHECK_MSG(versionRefusalTitle(facts) == expected.title, versionRefusalTitle(facts).c_str());
    }

    // The host-older sentence names the device in what it needs.
    auto hostOlder = refusalBetween(build("0.2.1.4", 5), build("0.2.0.7", 3));
    hostOlder.localDevice = { .kind = LocalDeviceKind::IPhone, .name = {} };
    CHECK(versionRefusalSentence(hostOlder)
        == "Studio’s Ubiquitty Host is from an older build (0.2.0.7) than this iPhone (0.2.1.4). Update the host on Studio.");

    // Nothing reported reads as this device, never as a Mac.
    Ubiquitty::Session::UpdateFacts silent;
    silent.host = build("0.2.1.4", 5);
    silent.local = build("0.2.0.7", 3);
    CHECK(versionRefusalSentence(silent).starts_with("This device is from an older build"));
}

namespace {

/// A store standing in for the embedder's, holding one record by key.
struct TrustStore {
    std::map<std::string, std::string> entries;
    int stores = 0;

    static std::size_t load(void* context, const char* key, std::uint8_t* buffer, std::size_t capacity)
    {
        const TrustStore& store = *static_cast<TrustStore*>(context);
        const auto found = store.entries.find(key);
        if (found == store.entries.end())
            return 0;
        if (buffer != nullptr)
            std::memcpy(buffer, found->second.data(), std::min(capacity, found->second.size()));
        return found->second.size();
    }

    static void store(void* context, const char* key, const std::uint8_t* bytes, std::size_t byteCount)
    {
        TrustStore& store = *static_cast<TrustStore*>(context);
        ++store.stores;
        if (byteCount == 0) {
            store.entries.erase(key);
            return;
        }
        store.entries[key] = std::string(reinterpret_cast<const char*>(bytes), byteCount);
    }
};

} // namespace

// Trust is the person's consent to push a binary to a host, kept by the
// embedder under Core's key and read back by Core: trusting, listing and
// revoking round-trip through the store, and the decision reads the store
// rather than any frontend list.
TEST(deployTrustRoundTripsThroughTheEmbeddersStore)
{
    using Ubiquitty::Runtime::HostDirectory;
    TrustStore store;
    HostDirectory::setDeployTrustPersistence(&TrustStore::load, &TrustStore::store, &store);

    CHECK(!HostDirectory::isDeployTrusted("user@fresh-vm"));
    HostDirectory::trustDeploy("user@fresh-vm");
    HostDirectory::trustDeploy("build.example");
    HostDirectory::trustDeploy("user@fresh-vm");
    CHECK(HostDirectory::isDeployTrusted("user@fresh-vm"));
    CHECK(HostDirectory::isDeployTrusted("build.example"));
    CHECK(store.entries.contains(HostDirectory::deployTrustStoreKey));
    const std::vector<std::string> trusted = HostDirectory::deployTrustedHosts();
    REQUIRE(trusted.size() == 2);
    CHECK_STR_EQ(trusted[0], "user@fresh-vm");
    CHECK_STR_EQ(trusted[1], "build.example");

    CHECK(decideDeployAction(true, "Linux aarch64", true, HostDirectory::isDeployTrusted("user@fresh-vm")) == DeployAction::Push);

    // Revoked, the next push asks again: the consent is gone, not cached.
    HostDirectory::revokeDeployTrust("user@fresh-vm");
    CHECK(!HostDirectory::isDeployTrusted("user@fresh-vm"));
    CHECK(HostDirectory::isDeployTrusted("build.example"));
    CHECK(decideDeployAction(true, "Linux aarch64", true, HostDirectory::isDeployTrusted("user@fresh-vm")) == DeployAction::ConfirmPush);

    HostDirectory::revokeDeployTrust("build.example");
    CHECK(HostDirectory::deployTrustedHosts().empty());
    CHECK(!store.entries.contains(HostDirectory::deployTrustStoreKey));
    HostDirectory::setDeployTrustPersistence(nullptr, nullptr, nullptr);
}

// A host is trusted only by an exact entry, and a store holding bytes this
// build did not write trusts nobody. Either failure the other way is a push
// to a machine the person never approved.
TEST(deployTrustIsNeverInferred)
{
    using Ubiquitty::Runtime::HostDirectory;
    TrustStore store;
    HostDirectory::setDeployTrustPersistence(&TrustStore::load, &TrustStore::store, &store);

    HostDirectory::trustDeploy("user@fresh-vm");
    CHECK(!HostDirectory::isDeployTrusted("fresh-vm"));
    CHECK(!HostDirectory::isDeployTrusted("user@fresh-vm.example"));
    CHECK(!HostDirectory::isDeployTrusted(""));

    std::string& bytes = store.entries[HostDirectory::deployTrustStoreKey];
    bytes[0] = static_cast<char>(Ubiquitty::Session::deployTrustRecordVersion + 1);
    CHECK(!HostDirectory::isDeployTrusted("user@fresh-vm"));

    store.entries[HostDirectory::deployTrustStoreKey] = std::string(1, static_cast<char>(Ubiquitty::Session::deployTrustRecordVersion)) + std::string("\x00\x40", 2) + "short";
    CHECK(HostDirectory::deployTrustedHosts().empty());

    // With no store installed nothing is trusted and nothing is written.
    HostDirectory::setDeployTrustPersistence(nullptr, nullptr, nullptr);
    const int storesBefore = store.stores;
    HostDirectory::trustDeploy("build.example");
    CHECK(!HostDirectory::isDeployTrusted("build.example"));
    CHECK_EQ(store.stores, storesBefore);
}

TEST(equalVersionsOfferAMatchingHostWithoutClaimingItIsNewer)
{
    using Ubiquitty::Session::decideUpdateAction;
    using Ubiquitty::Session::hostUpdateNotice;
    using Ubiquitty::Session::olderSideOf;
    using Ubiquitty::Session::UpdateAction;
    using Ubiquitty::Session::UpdateFacts;
    using Ubiquitty::Session::versionRefusalTitle;
    UpdateFacts facts = refusalBetween(build("0.2.0", 20), build("0.2.0", 20));
    CHECK(olderSideOf(facts) == Ubiquitty::Protocol::Sync::OlderSide::Unknown);
    CHECK(decideUpdateAction(facts) == UpdateAction::OfferHostUpdate);
    CHECK(versionRefusalTitle(facts) == "Studio Runs a Different Build of Ubiquitty");
    facts.heldSessionCount = 3;
    CHECK(hostUpdateNotice(facts).find("3 running sessions move to the new host") != std::string::npos);
    facts.hasBundledBinary = false;
    CHECK(decideUpdateAction(facts) == UpdateAction::ManualUpdate);
    CHECK(versionRefusalTitle(facts) == "Studio Runs a Different Build of Ubiquitty");
}

// This Mac's own host is taken over by the host this app bundles: nothing is
// uploaded, nothing is installed by hand, and no session ends. The words name
// the machine and the app, never the host's label, which for This Mac is the
// row's title, "Ubiquitty Host".
TEST(thisMachinesOlderHostIsTakenOverByTheBundledHost)
{
    using Ubiquitty::Session::decideUpdateAction;
    using Ubiquitty::Session::hostUpdateNotice;
    using Ubiquitty::Session::UpdateAction;
    using Ubiquitty::Session::UpdateFacts;
    using Ubiquitty::Session::versionRefusalSentence;
    using Ubiquitty::Session::versionRefusalTitle;

    const auto local = [](PeerBuild here, PeerBuild host) {
        UpdateFacts facts = refusalBetween(std::move(here), std::move(host));
        facts.hostLabel = "Ubiquitty Host";
        facts.isThisMachine = true;
        return facts;
    };

    UpdateFacts hostOlder = local(build("0.2.1", 2), build("0.2.0", 1));
    CHECK(decideUpdateAction(hostOlder) == UpdateAction::TakeOverLocalHost);
    CHECK_STR_EQ(versionRefusalTitle(hostOlder), "This Mac Runs an Older Ubiquitty Host");
    CHECK_STR_EQ(versionRefusalSentence(hostOlder), "The Ubiquitty host on this Mac is from an older build (0.2.0) than this app (0.2.1).");
    CHECK_STR_EQ(hostUpdateNotice(hostOlder), "Updating moves its sessions to the host included with this app without ending them.");
    // Held sessions are not asked about: none of them ends.
    hostOlder.heldSessionCount = 3;
    CHECK(hostUpdateNotice(hostOlder).find("ask") == std::string::npos);

    // A machine with a name is named in the sentence; a title names its kind.
    UpdateFacts named = hostOlder;
    named.localDevice.name = "Sam’s MacBook Pro";
    CHECK_STR_EQ(versionRefusalSentence(named), "The Ubiquitty host on Sam’s MacBook Pro is from an older build (0.2.0) than this app (0.2.1).");
    CHECK_STR_EQ(versionRefusalTitle(named), "This Mac Runs an Older Ubiquitty Host");

    const UpdateFacts silent = local(build("0.2.1", 2), PeerBuild {});
    CHECK(decideUpdateAction(silent) == UpdateAction::TakeOverLocalHost);
    CHECK_STR_EQ(versionRefusalSentence(silent), "The Ubiquitty host on this Mac doesn’t say which build it runs, so it’s older than this app (0.2.1).");
    CHECK_STR_EQ(versionRefusalTitle(silent), "This Mac Runs an Older Ubiquitty Host");

    const UpdateFacts unordered = local(build("0.2.1", 2), build("0.2.1", 2));
    CHECK(decideUpdateAction(unordered) == UpdateAction::TakeOverLocalHost);
    CHECK_STR_EQ(versionRefusalTitle(unordered), "This Mac Runs a Different Build of the Ubiquitty Host");
    CHECK_STR_EQ(versionRefusalSentence(unordered), "This app and the Ubiquitty host on this Mac run different builds of Ubiquitty 0.2.1.");

    // A newer host on this Mac is never replaced by an older app's.
    const UpdateFacts clientOlder = local(build("0.2.0", 1), build("0.2.1", 2));
    CHECK(decideUpdateAction(clientOlder) == UpdateAction::None);
    CHECK(hostUpdateNotice(clientOlder).empty());
    CHECK_STR_EQ(versionRefusalTitle(clientOlder), "This Mac Runs a Newer Ubiquitty Host");
    CHECK_STR_EQ(versionRefusalSentence(clientOlder),
        "This app is from an older build of Ubiquitty (0.2.0) than the Ubiquitty host on this Mac (0.2.1). Update Ubiquitty on this Mac.");

    // An app with no host of its own has nothing to offer, and still never
    // sends the person to install one by hand.
    UpdateFacts noBundle = local(build("0.2.1", 2), build("0.2.0", 1));
    noBundle.hasBundledBinary = false;
    CHECK(decideUpdateAction(noBundle) == UpdateAction::None);

    for (const UpdateFacts& facts : { hostOlder, named, silent, unordered, clientOlder, noBundle }) {
        const std::string words = versionRefusalTitle(facts) + " " + versionRefusalSentence(facts) + " " + hostUpdateNotice(facts);
        CHECK(words.find("Ubiquitty Host’s") == std::string::npos);
        CHECK(words.find("Ubiquitty Host Runs") == std::string::npos);
        CHECK(words.find("no host build") == std::string::npos);
        CHECK(words.find("by hand") == std::string::npos);
        CHECK(words.find("daemon") == std::string::npos);
        CHECK(words.find("socket") == std::string::npos);
    }
}

// An update of this Mac's host that left something undone says what, and
// what the person can do; one that moved every session says nothing.
TEST(anUpdateOfThisMachinesHostSaysWhatItLeftUndone)
{
    using Ubiquitty::Session::LocalHostUpgradeFacts;
    using Ubiquitty::Session::localHostUpgradeSentence;
    using Ubiquitty::Session::localHostUpgradeTitle;

    LocalHostUpgradeFacts facts;
    CHECK(localHostUpgradeTitle(facts).empty() && localHostUpgradeSentence(facts).empty());

    facts.olderHostCount = 1;
    CHECK_STR_EQ(localHostUpgradeTitle(facts), "An Older Ubiquitty Host Keeps Its Sessions");
    CHECK_STR_EQ(localHostUpgradeSentence(facts),
        "An older Ubiquitty host on this Mac didn’t hand its sessions to the host included with this app. It keeps them until they end.");
    facts.olderHostCount = 2;
    CHECK_STR_EQ(localHostUpgradeTitle(facts), "Older Ubiquitty Hosts Keep Their Sessions");
    CHECK_STR_EQ(localHostUpgradeSentence(facts),
        "Older Ubiquitty hosts on this Mac didn’t hand their sessions to the host included with this app. They keep them until they end.");

    // Moving the app is advised only where it would let a host hand over:
    // one refusing a copy made because the app is on another volume.
    facts.olderHostCount = 1;
    facts.isMoveAdvised = true;
    CHECK_STR_EQ(localHostUpgradeSentence(facts),
        "An older Ubiquitty host on this Mac didn’t hand its sessions to the host included with this app. It keeps them until they end. "
        "To bring them to this app’s host, move Ubiquitty to the Applications folder, then open it again.");

    facts.isNewerInstalled = true;
    CHECK_STR_EQ(localHostUpgradeTitle(facts), "This Mac Runs a Newer Ubiquitty Host");
    CHECK(localHostUpgradeSentence(facts).find("Update Ubiquitty") != std::string::npos);
    for (const std::string& words : { localHostUpgradeTitle(facts), localHostUpgradeSentence(facts) }) {
        CHECK(words.find("daemon") == std::string::npos);
        CHECK(words.find("socket") == std::string::npos);
    }
}

UBIQUITTY_TEST_MAIN()
