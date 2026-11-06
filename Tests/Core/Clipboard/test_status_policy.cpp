// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_status_policy.cpp — the status decisions every frontend renders.
//
// What a badge SAYS is the platform's business; what it MEANS is not. These are
// the decisions that would otherwise be written once per frontend, which is how
// two of them come to disagree about whether a clipboard is exposed.
//
// The facts arrive as named fields rather than a settings fold: both policies
// live in the L1 model layer and the settings are L2, so taking the fold would
// invert the layering. The caller already holds them.

#include "Core/Clipboard/Scope.h"
#include "Core/Security/SecureInputPolicy.h"

using Ubiquitty::Clipboard::Scope;
using Ubiquitty::Clipboard::scopeFor;
using Ubiquitty::Clipboard::ServeDecision;
using Ubiquitty::Clipboard::serveDecisionFor;
using Ubiquitty::Security::SecureInputReason;
using Ubiquitty::Security::secureInputReason;
using Ubiquitty::Security::shouldHoldSecureInput;

/// A local session reports nothing: one machine, one pasteboard.
///
/// Held whatever the other two facts say, so a frontend cannot show the
/// alarming badge on a session that never leaves the machine.
TEST(testClipboardScopeIsLocalWhateverElseHolds)
{
    CHECK(scopeFor({ .isRemoteSession = false }) == Scope::Local);
    CHECK(scopeFor({ .isRemoteSession = false, .isServingEnabled = true, .didClientClaimServe = true })
        == Scope::Local);
}

/// All three facts must hold before a badge claims the user's clipboard is
/// reachable, because that badge is the only warning there is.
TEST(testClipboardScopeNeedsTheSettingAndTheCapability)
{
    CHECK(scopeFor({ .isRemoteSession = true, .isServingEnabled = true, .didClientClaimServe = true })
        == Scope::Client);

    // The setting off: the host's own clipboard is what its programs read, and
    // on a headless machine that is nothing.
    CHECK(scopeFor({ .isRemoteSession = true, .isServingEnabled = false, .didClientClaimServe = true })
        == Scope::Host);

    // The setting on but the client never claimed the capability — an older
    // build, or a claim the host refused. Nothing will be served, so a badge
    // promising otherwise would describe a session that cannot do it.
    CHECK(scopeFor({ .isRemoteSession = true, .isServingEnabled = true, .didClientClaimServe = false })
        == Scope::Host);

    CHECK(scopeFor({ .isRemoteSession = true }) == Scope::Host);
}

/// Secure input is a process-wide grab, so an app holding it in the background
/// takes the keyboard from whatever the user actually switched to.
TEST(testSecureInputNeedsTheAppToBeFrontmost)
{
    CHECK(secureInputReason({ .isAlwaysPreferred = true, .isApplicationActive = true })
        == SecureInputReason::Always);
    CHECK(secureInputReason({ .isAlwaysPreferred = true, .isApplicationActive = false })
        == SecureInputReason::None);
    CHECK(!shouldHoldSecureInput({ .isAlwaysPreferred = true,
        .isPreferredAtPasswordPrompts = true,
        .isAnyPaneAtPasswordPrompt = true,
        .isApplicationActive = false }));
}

/// The prompt-following preference needs a prompt to follow: the setting alone
/// holds nothing, and a prompt alone holds nothing when the setting is off.
TEST(testSecureInputFollowsAPromptOnlyWhenBothHold)
{
    CHECK(secureInputReason({ .isPreferredAtPasswordPrompts = true,
              .isAnyPaneAtPasswordPrompt = true,
              .isApplicationActive = true })
        == SecureInputReason::PasswordPrompt);
    CHECK(secureInputReason({ .isPreferredAtPasswordPrompts = true,
              .isAnyPaneAtPasswordPrompt = false,
              .isApplicationActive = true })
        == SecureInputReason::None);
    CHECK(secureInputReason({ .isPreferredAtPasswordPrompts = false,
              .isAnyPaneAtPasswordPrompt = true,
              .isApplicationActive = true })
        == SecureInputReason::None);
}

/// The standing preference wins the REASON when both apply, because that is
/// what a badge should say: a hold that outlives the prompt is not explained by
/// the prompt.
TEST(testSecureInputNamesTheStandingPreferenceFirst)
{
    const Ubiquitty::Security::SecureInputFacts both { .isAlwaysPreferred = true,
        .isPreferredAtPasswordPrompts = true,
        .isAnyPaneAtPasswordPrompt = true,
        .isApplicationActive = true };
    CHECK(secureInputReason(both) == SecureInputReason::Always);
    CHECK(shouldHoldSecureInput(both));
}

/// A request is answered only when the connection claimed it.
///
/// The claim is the wire's record of consent. A host that asks without one is
/// asking a question this client never invited, and answering would serve a
/// pasteboard over a connection whose capabilities say it does not.
TEST(testAClipboardRequestIsRefusedWithoutTheClaim)
{
    CHECK(serveDecisionFor({ .didClaimServe = false, .isClipboardReadDenied = false })
        == ServeDecision::Refuse);
    CHECK(serveDecisionFor({ .didClaimServe = true, .isClipboardReadDenied = false })
        == ServeDecision::Answer);
}

/// `clipboardRead = deny` refuses the shims too.
///
/// The gate that would otherwise be the way around the other. A program on the
/// host reads a clipboard by shelling out to xclip or by writing an OSC 52
/// query, whichever works; a person who set deny said programs may not read
/// their clipboard, and the sentence does not name a syscall. Refused whatever
/// the shim preference says, so turning the shims on cannot re-open a door
/// that was explicitly shut.
TEST(testADeniedClipboardReadRefusesTheShimRouteAsWell)
{
    CHECK(serveDecisionFor({ .didClaimServe = true, .isClipboardReadDenied = true })
        == ServeDecision::Refuse);
    CHECK(serveDecisionFor({ .didClaimServe = false, .isClipboardReadDenied = true })
        == ServeDecision::Refuse);
}

UBIQUITTY_TEST_MAIN()
