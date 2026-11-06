// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// signature_policy — which binaries a host takes for its own, from the
// verdicts its platform reads off their code signatures
// (Source/Platform/Host/SignaturePolicy.h). The verdicts here are stated, not
// read: same team, another signer, unsigned, ad hoc, and a platform that
// signs no code, against both builds' policies.

#include "Harness.h"

#include "Platform/Host/CodeSignature.h"
#include "Platform/Host/SignaturePolicy.h"

#include <array>
#include <cstddef>
#include <string>
#include <utility>

using namespace Ubiquitty::Host;

namespace {

CodeSignature signedBy(Signer signer, std::string identifier = std::string(hostSigningIdentifier))
{
    return { .signer = signer, .identifier = std::move(identifier) };
}

CodeSignature teamHost() { return signedBy(Signer::Team); }
CodeSignature teamApp() { return signedBy(Signer::Team, std::string(appSigningIdentifier)); }
CodeSignature teamAgent() { return signedBy(Signer::Team, "ubq"); }
CodeSignature otherHost() { return signedBy(Signer::OtherSigner); }
CodeSignature adHocHost() { return signedBy(Signer::AdHoc); }
CodeSignature unsignedHost() { return signedBy(Signer::None, {}); }
CodeSignature unsupported() { return signedBy(Signer::Unsupported, {}); }

constexpr std::array everyPolicy { SigningPolicy::Release, SigningPolicy::Development };
constexpr std::array everyPlace { InstallPlace::AtInstallPath, InstallPlace::Elsewhere };

} // namespace

// A host binary the team signed succeeds a holder the team signed wherever it
// lies, in either build: the signature, not the path, names it.
TEST(aTeamSignedHostSucceedsATeamSignedHolderWhereverItLies)
{
    for (const auto policy : everyPolicy) {
        for (const auto place : everyPlace)
            CHECK(successorRoute(policy, teamHost(), teamHost(), place) == SuccessorRoute::ByTeam);
    }
}

// A release build admits nothing the team did not sign as a host binary, even
// the file at the install path: another signer's, an unsigned or an ad hoc
// binary keeps the sessions where they are, as does another team binary.
TEST(aReleaseBuildRefusesEverySuccessorTheTeamDidNotSignAsAHost)
{
    for (const auto place : everyPlace) {
        for (const auto& candidate : { otherHost(), adHocHost(), unsignedHost(), teamAgent(), teamApp() })
            CHECK(successorRoute(SigningPolicy::Release, teamHost(), candidate, place) == SuccessorRoute::Refused);
    }
}

// A release holder that is not itself the team's admits no successor by
// signature, and none by place: it fails closed.
TEST(aReleaseHolderTheTeamDidNotSignAdmitsNoSuccessor)
{
    for (const auto place : everyPlace) {
        for (const auto& holder : { otherHost(), adHocHost(), unsignedHost() })
            CHECK(successorRoute(SigningPolicy::Release, holder, teamHost(), place) == SuccessorRoute::Refused);
    }
}

// A development build, which runs unsigned, ad hoc or under a development
// certificate, admits the file at the install path whatever signs it, and
// nothing else that the team did not sign.
TEST(aDevelopmentBuildAlsoAdmitsTheFileAtTheInstallPath)
{
    for (const auto& holder : { otherHost(), adHocHost(), unsignedHost() }) {
        for (const auto& candidate : { otherHost(), adHocHost(), unsignedHost(), teamAgent() }) {
            CHECK(successorRoute(SigningPolicy::Development, holder, candidate, InstallPlace::AtInstallPath) == SuccessorRoute::ByPlace);
            CHECK(successorRoute(SigningPolicy::Development, holder, candidate, InstallPlace::Elsewhere) == SuccessorRoute::Refused);
        }
    }
    // A team holder still takes an installed file it did not sign by place.
    CHECK(successorRoute(SigningPolicy::Development, teamHost(), adHocHost(), InstallPlace::AtInstallPath) == SuccessorRoute::ByPlace);
    CHECK(successorRoute(SigningPolicy::Development, teamHost(), adHocHost(), InstallPlace::Elsewhere) == SuccessorRoute::Refused);
}

// A platform that signs no code decides by the install path alone, in either
// build, as every host did before signatures were read.
TEST(aPlatformThatSignsNoCodeAdmitsTheInstalledFileAlone)
{
    for (const auto policy : everyPolicy) {
        CHECK(successorRoute(policy, unsupported(), unsupported(), InstallPlace::AtInstallPath) == SuccessorRoute::ByPlace);
        CHECK(successorRoute(policy, unsupported(), unsupported(), InstallPlace::Elsewhere) == SuccessorRoute::Refused);
    }
}

// The released app and a host binary the team signed are the host's own in
// either build, wherever they lie; another binary the team signed, as ubq, is
// not, nor is one the team did not sign.
TEST(theTeamsAppAndHostAreTheHostsOwnAndNothingElseItSigned)
{
    for (const auto policy : everyPolicy) {
        CHECK(isOwnBinary(policy, teamApp(), FileTrust::Untrusted));
        CHECK(isOwnBinary(policy, teamHost(), FileTrust::Untrusted));
        CHECK(!isOwnBinary(policy, teamAgent(), FileTrust::Untrusted));
        CHECK(!isOwnBinary(policy, signedBy(Signer::Team, "org.ubiquitty.Ubiquitty.dev"), FileTrust::Untrusted));
    }
}

// A release build trusts no file by identity: a binary at the very path the
// host resolved, unsigned, ad hoc or another signer's, is an agent.
TEST(aReleaseBuildTrustsNoFileTheTeamDidNotSign)
{
    for (const auto& peer : { otherHost(), adHocHost(), unsignedHost(), teamAgent() }) {
        CHECK(!isOwnBinary(SigningPolicy::Release, peer, FileTrust::Trusted));
        CHECK(!isOwnBinary(SigningPolicy::Release, peer, FileTrust::Untrusted));
    }
}

// A development build trusts the files it resolved whatever signs them, and
// no other file the team did not sign.
TEST(aDevelopmentBuildAlsoTrustsTheFilesItResolved)
{
    for (const auto& peer : { otherHost(), adHocHost(), unsignedHost(), teamAgent() }) {
        CHECK(isOwnBinary(SigningPolicy::Development, peer, FileTrust::Trusted));
        CHECK(!isOwnBinary(SigningPolicy::Development, peer, FileTrust::Untrusted));
    }
}

// A platform that signs no code trusts by file alone, in either build.
TEST(aPlatformThatSignsNoCodeTrustsByFileAlone)
{
    for (const auto policy : everyPolicy) {
        CHECK(isOwnBinary(policy, unsupported(), FileTrust::Trusted));
        CHECK(!isOwnBinary(policy, unsupported(), FileTrust::Untrusted));
    }
}

// A binary is judged once: its verdict is kept for its file, a definite one
// only, and a file changed since is another key. The memo is bounded, and the
// oldest verdict gives way.
TEST(aSignatureVerdictIsKeptForItsFileAndBounded)
{
    SignatureMemo memo;
    const SignatureKey key { .device = 1, .inode = 2, .size = 3, .modifiedSeconds = 4, .modifiedNanoseconds = 5, .changedSeconds = 6, .changedNanoseconds = 7 };
    CHECK(!memo.find(key));
    memo.remember(key, teamHost());
    REQUIRE(memo.find(key));
    CHECK(*memo.find(key) == teamHost());
    SignatureKey written = key;
    written.changedNanoseconds = 8;
    CHECK(!memo.find(written));
    // An unread signature may read at the next connection.
    memo.remember(written, unsignedHost());
    CHECK(!memo.find(written));
    memo.remember(key, adHocHost());
    REQUIRE(memo.find(key));
    CHECK(*memo.find(key) == adHocHost());
    CHECK_EQ(memo.size(), std::size_t { 1 });
    for (std::size_t index = 0; index < SignatureMemo::maxEntries; ++index) {
        SignatureKey another = key;
        another.inode = static_cast<ino_t>(100 + index);
        memo.remember(another, otherHost());
    }
    CHECK_EQ(memo.size(), SignatureMemo::maxEntries);
    CHECK(!memo.find(key));
}

UBIQUITTY_TEST_MAIN()
