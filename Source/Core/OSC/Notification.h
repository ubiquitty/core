// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
//
// Source/Core/OSC/Notification.h — what a desktop notification says, where it
// says it, whether it is shown at all and which earlier one it replaces: the
// policy shared by every frontend's notification backend.
//
// Core parses an OSC 9 / 99 / 777 notification (Dispatch.h) into a (title, body)
// pair and routes it through ShellClient::postNotification. A backend's own
// part is the final post; everything a person reads or does not read is
// decided here, so the same sequence makes the same banner on every system.
//
// Pure std::string — no UserNotifications / GTK.
//

#include "Core/Title/BannerText.h"
#include "PAL/TerminalClients.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace Ubiquitty::OSC {

// The most columns either field of a notification is shown at: three lines of
// a banner. The parser already holds each to a byte count; this is the bound
// on what is drawn, which a byte count does not give for text that is not
// ASCII, and it cuts between grapheme clusters where a byte bound cuts
// anywhere.
inline constexpr int maxNotificationTextColumns = 120;

/// The banner content to post, after the body-only promotion.
///
/// `shouldPost` is false when the input carried neither a title nor a body, and
/// the backend drops the notification. When true, `title` is non-empty and
/// `body` may be empty.
struct NotificationDisplay {
    std::string title;
    std::string body;
    bool shouldPost = false;
};

namespace detail {

/// Removes what dangles at the end of a field: a separator left with
/// nothing after it, and the blank before it.
///
/// `Build: ` and `Deploy —` are a heading written to stand before a second
/// line; shown on their own, or compared with a body, the separator is
/// noise. Punctuation that ends a sentence is the sentence's and stays.
/// Text that is nothing but separators is returned whole: it is what the
/// program said, and an empty field would say something else.
[[nodiscard]] inline std::string_view withoutTrailingNoise(std::string_view text)
{
    static constexpr std::string_view noise[] = { " ", ":", ";", ",", "-", "|", "–", "—", "·" };
    std::string_view trimmed = text;
    const auto trimOne = [&trimmed] {
        for (const std::string_view mark : noise) {
            if (trimmed.ends_with(mark)) {
                trimmed.remove_suffix(mark.size());
                return true;
            }
        }
        return false;
    };
    while (trimOne()) {
    }
    return trimmed.empty() ? text : trimmed;
}

/// Whether two fields say the same thing: equal once ASCII case and a
/// closing full stop are set aside.
[[nodiscard]] inline bool isSameNotificationText(std::string_view first, std::string_view second)
{
    const auto core = [](std::string_view text) {
        if (text.size() > 1 && text.ends_with('.') && !text.ends_with(".."))
            text.remove_suffix(1);
        return text;
    };
    first = core(first);
    second = core(second);
    if (first.size() != second.size())
        return false;
    const auto lowered = [](char character) { return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character; };
    for (std::size_t index = 0; index < first.size(); ++index) {
        if (lowered(first[index]) != lowered(second[index]))
            return false;
    }
    return true;
}

/// One field, fit to show: sanitized, without trailing noise, bounded.
[[nodiscard]] inline std::string notificationField(std::string_view raw)
{
    const std::string sanitized = Title::sanitizedBannerText(raw);
    return Title::boundedBannerText(withoutTrailingNoise(sanitized), maxNotificationTextColumns);
}

} // namespace detail

/// Resolves the title and body a notification arrived with into text to show.
///
/// Both fields are sanitized — controls become blanks, blanks fold, the
/// bidirectional controls go — lose a separator dangling at their end, and are
/// cut to ``maxNotificationTextColumns``. A program's words are otherwise its
/// own.
///
/// A body-only message — OSC 9 and kitty's OSC 99 carry no title — promotes its
/// body into the title and leaves the body empty, because a banner reads better
/// with the text as its heading than under an empty one. A body that only
/// repeats the title is dropped for the same reason: it is one text. A wholly
/// empty pair yields `shouldPost == false`.
///
/// Resolving a resolved pair returns it unchanged.
///
/// - Parameters:
///   - rawTitle: The title as parsed, possibly empty.
///   - rawBody: The body as parsed, possibly empty.
/// - Returns: What to post, and whether to post at all.
inline NotificationDisplay resolveNotificationDisplay(std::string_view rawTitle, std::string_view rawBody)
{
    NotificationDisplay display;
    display.title = detail::notificationField(rawTitle);
    display.body = detail::notificationField(rawBody);
    if (display.title.empty())
        display.title = std::exchange(display.body, {});
    else if (detail::isSameNotificationText(display.title, display.body))
        display.body.clear();
    display.shouldPost = !display.title.empty();
    return display;
}

// The most columns a banner names its sender at: one line of a banner. A
// sender is a name; a program that put a sentence there loses the end of it
// rather than the place line beneath.
inline constexpr int maxBannerSenderColumns = 40;

// The most columns an action heads a banner at. A banner's heading is one
// line, drawn in a heavier face than its body, and holds about forty columns
// before the system cuts it; an action longer than that is not a heading, and
// goes where text wraps.
inline constexpr int maxBannerHeadingColumns = 40;

/// A banner's three slots, as a system with all three draws them: a heading,
/// a line beneath it in the heading's weight, and running text that wraps.
struct Banner {
    /// The heading.
    std::string title;
    /// The line beneath the heading, or empty.
    std::string subtitle;
    /// The running text, or empty.
    std::string body;
    /// Whether there is anything to post.
    bool shouldPost = false; // bool-ok: names the one outcome it reports
};

/// Lays a notification and its session's place out in a banner's slots.
///
/// Three things may be said: the action — what happened, or what is wanted of
/// the person — the sender, when the program named itself apart from the
/// action, and the place the session sits. A program that sent both a title
/// and a body named itself in the title and said what it wants in the body; a
/// program that sent one text sent the action and has no sender, and none is
/// made up from a process name.
///
/// An action short enough to be a heading heads the banner, because it is the
/// part a person acts on: the sender stands beneath it and the place is the
/// running text.
///
/// An action longer than ``maxBannerHeadingColumns`` would be cut where a
/// heading is cut, so it becomes the running text, which wraps, and what
/// identifies it moves up: the sender heads the banner with the place beneath
/// it, or the place heads it when there is no sender. With neither, the
/// action is all there is and stays the heading.
///
/// No text appears in two slots.
///
/// - Parameters:
///   - title: The notification's title, raw or resolved.
///   - body: Its body, raw or resolved.
///   - place: The session's place line, from `Session::describeBannerPlace`.
/// - Returns: The slots, with `shouldPost` false for an empty notification.
[[nodiscard]] inline Banner composeBanner(const std::string& title, const std::string& body, const std::string& place)
{
    NotificationDisplay display = resolveNotificationDisplay(title, body);
    Banner banner;
    if (!display.shouldPost)
        return banner;
    banner.shouldPost = true;
    const bool hasSender = !display.body.empty();
    std::string action = hasSender ? std::move(display.body) : std::move(display.title);
    std::string sender = hasSender ? Title::boundedBannerText(display.title, maxBannerSenderColumns) : std::string();
    if (Title::bannerTextColumns(action) <= maxBannerHeadingColumns || (sender.empty() && place.empty())) {
        banner.title = std::move(action);
        banner.subtitle = std::move(sender);
        banner.body = place;
        return banner;
    }
    banner.body = std::move(action);
    if (sender.empty()) {
        banner.title = place;
        return banner;
    }
    banner.title = std::move(sender);
    banner.subtitle = place;
    return banner;
}

/// A banner's second and third slots as one text, for a system whose
/// notification has a heading and running text and nothing between.
///
/// The line that would stand beneath the heading leads the running text on a
/// line of its own, so the order a reader meets the parts in is the same on
/// every system.
///
/// - Parameter banner: The composed banner.
/// - Returns: The subtitle and the body, a line each; either may be absent.
[[nodiscard]] inline std::string bannerRunningText(const Banner& banner)
{
    if (banner.subtitle.empty())
        return banner.body;
    if (banner.body.empty())
        return banner.subtitle;
    return banner.subtitle + "\n" + banner.body;
}

/// Where the person's attention is when a pane asks for a banner, as the
/// frontend observes it.
struct BannerAudience {
    /// Whether this app is the one in front.
    bool isAppActive = false; // bool-ok: names the one state it reports
    /// Whether the pane's window is the one taking the keyboard.
    bool isWindowKey = false; // bool-ok: names the one state it reports
    /// Whether the pane is the focused pane of its window.
    bool isPaneFocused = false; // bool-ok: names the one state it reports
    /// Whether the pane is drawn: its tab selected, no other pane zoomed over
    /// it, its window on screen.
    bool isPaneVisible = false; // bool-ok: names the one state it reports
};

/// Whether a pane's notification is posted as a banner.
///
/// Never when the person is looking at the pane that asked: the text it
/// printed is in front of them, and a banner sliding over the window they are
/// typing in tells them what they are reading. Otherwise the program's own
/// occasion decides, where it stated one (OSC 99 `o=`):
///
/// - `Always` posts in every other case — another app in front, another
///   window, another tab, another pane of the same tab.
/// - `Unfocused` posts unless the pane has the keyboard, drawn or not.
/// - `Invisible` posts only while the pane is not drawn, so a pane in view
///   beside the focused one, or in a window behind another app, stays quiet.
///
/// The marks a window leaves for later (a tab's dot, the Dock's badge) are
/// separate decisions with their own owners; this is the banner's alone.
///
/// - Parameters:
///   - audience: Where the person's attention is.
///   - occasion: When the program wants it shown.
/// - Returns: Whether to post.
[[nodiscard]] constexpr bool isBannerWanted(const BannerAudience& audience, NotificationOccasion occasion = NotificationOccasion::Always)
{
    const bool hasKeyboard = audience.isAppActive && audience.isWindowKey && audience.isPaneFocused;
    switch (occasion) {
    case NotificationOccasion::Always:
        return !(hasKeyboard && audience.isPaneVisible);
    case NotificationOccasion::Unfocused:
        return !hasKeyboard;
    case NotificationOccasion::Invisible:
        return !audience.isPaneVisible;
    }
    return true;
}

/// What raised a banner. Each kind keeps its own standing banner per pane.
enum class BannerSource : std::uint8_t {
    /// A program's notification, or a trigger rule's.
    Program,
    /// A bell nobody was looking at.
    Bell,
    /// A consent request whose sheet nobody can see.
    Consent,
};

/// The identifier a banner is posted under, which decides what it replaces.
///
/// One per pane and source: a pane's next banner takes the place of its last
/// one still standing, so a program that says "waiting for your input" every
/// minute leaves one banner behind and not sixty. Sources do not replace each
/// other — a bell rung after a message would otherwise erase the message, and
/// a program's text could take down the banner standing in for a consent
/// sheet.
///
/// A program that names its notifications (OSC 99 `i=`) decides for itself
/// which replace which: one under a name it used before stands in place of
/// that one, and two under different names both stand. A notification sent
/// without a name shares the pane's unnamed one.
///
/// - Parameters:
///   - source: What raised the banner.
///   - window: The frontend's stable name for the posting window.
///   - pane: The posting pane, or zero for an event of the window itself.
///   - name: The program's name for the notification, already reduced to
///     letters, digits and `_-+.`; empty for none.
/// - Returns: The identifier.
[[nodiscard]] inline std::string bannerRequestIdentifier(BannerSource source, const std::string& window, std::uint64_t pane, const std::string& name = {})
{
    std::string identifier = "ubiquitty.banner.";
    switch (source) {
    case BannerSource::Program:
        identifier += "program";
        break;
    case BannerSource::Bell:
        identifier += "bell";
        break;
    case BannerSource::Consent:
        identifier += "consent";
        break;
    }
    identifier += '.';
    identifier += window;
    if (pane != 0) {
        identifier += ".pane.";
        identifier += std::to_string(pane);
    }
    if (!name.empty()) {
        identifier += ".name.";
        identifier += name;
    }
    return identifier;
}

/// The identifier banners are grouped under in the system's list.
///
/// Per window: the group is what a person collapses and clears as one, and
/// every banner in it then leads back to the same window.
///
/// - Parameter window: The frontend's stable name for the posting window.
/// - Returns: The identifier.
[[nodiscard]] inline std::string bannerThreadIdentifier(const std::string& window)
{
    return "ubiquitty.window." + window;
}

/// Who stands behind a banner request the system hands back to the app.
enum class BannerOrigin : std::uint8_t {
    /// Nothing this app posted, or a post whose facts disagree with each other.
    Refused,
    /// A host event the background inbox kept under a refresh permission.
    Retained,
    /// A program in a pane this app holds attached.
    Live,
};

/// The identifier prefix of every banner a refresh permission posts.
///
/// Renewing the permission changes the prefix, so a banner the retired
/// permission left behind never answers for the new one.
///
/// - Parameters:
///   - host: The saved host's identifier.
///   - permission: The refresh permission's identifier.
/// - Returns: The prefix.
[[nodiscard]] inline std::string retainedBannerPrefix(const std::string& host, const std::string& permission)
{
    std::string prefix = "host.";
    prefix += host;
    prefix += '.';
    prefix += permission;
    prefix += '.';
    return prefix;
}

/// The identifier prefix of every banner one attachment of a session model
/// posts.
///
/// The model takes a new incarnation each time it detaches, so a banner posted
/// before a reconnect never answers for the attachment after it.
///
/// - Parameters:
///   - host: The saved host's identifier.
///   - incarnation: The model's current attachment incarnation.
/// - Returns: The prefix.
[[nodiscard]] inline std::string liveBannerPrefix(const std::string& host, const std::string& incarnation)
{
    std::string prefix = "live.";
    prefix += host;
    prefix += '.';
    prefix += incarnation;
    prefix += '.';
    return prefix;
}

/// What a banner request handed back by the system carries, as the frontend
/// read it from the platform's payload.
///
/// An absent identifier is empty and an absent number is zero; a field present
/// in a form the frontend cannot read without loss refuses the request before
/// it reaches Core.
struct BannerOriginFacts {
    /// The request's identifier.
    std::string identifier;
    /// The saved host the request names.
    std::string host;
    /// The refresh permission it names, for a retained event.
    std::string permission;
    /// The model incarnation it names, for a live event.
    std::string incarnation;
    /// The daemon instance it names, for a live event.
    std::uint64_t daemon = 0;
    /// The host's session that raised it.
    std::uint32_t session = 0;
    /// The program's occasion as posted (`NotificationOccasion`'s raw value).
    std::uint32_t occasion = 0;
};

/// A banner request's origin, decided from what it carries.
struct BannerOriginDecision {
    /// Who stands behind it.
    BannerOrigin origin = BannerOrigin::Refused;
    /// When the program wanted it shown; `Always` for a refused request.
    NotificationOccasion occasion = NotificationOccasion::Always;
};

/// Decides who stands behind a banner request the system hands back.
///
/// A request answers for exactly one producer: a refresh permission or a live
/// attachment, never both and never neither. Its identifier must sit under that
/// producer's prefix with an event name after it, its session must be a real
/// one, and its occasion one Core defines. Anything else is refused, so an
/// unknown occasion is never read as `Always` and a request that names a
/// permission cannot borrow a live attachment's currency.
///
/// - Parameter facts: What the request carries.
/// - Returns: The origin, with the program's occasion.
[[nodiscard]] inline BannerOriginDecision decideBannerOrigin(const BannerOriginFacts& facts)
{
    BannerOriginDecision refused;
    if (facts.host.empty() || facts.session == 0 || facts.occasion > static_cast<std::uint32_t>(NotificationOccasion::Invisible))
        return refused;
    const bool hasPermission = !facts.permission.empty();
    const bool hasLiveAttachment = !facts.incarnation.empty() || facts.daemon != 0;
    if (hasPermission == hasLiveAttachment)
        return refused;
    if (hasLiveAttachment && (facts.incarnation.empty() || facts.daemon == 0))
        return refused;
    const std::string prefix = hasPermission ? retainedBannerPrefix(facts.host, facts.permission) : liveBannerPrefix(facts.host, facts.incarnation);
    if (facts.identifier.size() <= prefix.size() || !facts.identifier.starts_with(prefix))
        return refused;
    return {
        .origin = hasPermission ? BannerOrigin::Retained : BannerOrigin::Live,
        .occasion = static_cast<NotificationOccasion>(facts.occasion),
    };
}

/// Whether the system shows a banner request it hands back while the app is in
/// front.
///
/// Only for a request whose producer the frontend still holds — the same
/// permission, or the same attachment of the same daemon with the session still
/// attached — and then on the same terms as posting it (`isBannerWanted`). The
/// person may have moved to the pane between the post and the presentation.
///
/// - Parameters:
///   - decision: The request's origin, from `decideBannerOrigin`.
///   - isProducerCurrent: Whether the frontend still holds that producer.
///   - audience: Where the person's attention is now; empty when no pane of
///     the producer is drawn.
/// - Returns: Whether to show it.
[[nodiscard]] constexpr bool isBannerPresented(const BannerOriginDecision& decision, bool isProducerCurrent, const BannerAudience& audience)
{
    return decision.origin != BannerOrigin::Refused && isProducerCurrent && isBannerWanted(audience, decision.occasion);
}

} // namespace Ubiquitty::OSC
