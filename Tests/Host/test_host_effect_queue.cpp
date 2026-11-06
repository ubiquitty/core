// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include "Core/Unicode/UTF8Repair.h"
#include "Platform/Host/HostEffects.h"

#include <string>
#include <variant>

using namespace Ubiquitty;
using namespace Ubiquitty::Host;

TEST(hostNotificationOwnsReplacementIdentityAndOccasion)
{
    HostEffectQueue queue;
    HostShellClient shell(queue);
    ShellClient& client = shell;
    for (const auto occasion : { NotificationOccasion::Always, NotificationOccasion::Unfocused, NotificationOccasion::Invisible }) {
        std::string title = "Build";
        std::string body = "Complete";
        std::string identifier = "build-1";
        client.postNotificationRequest({ title, body, identifier, occasion });
        title.clear();
        body.clear();
        identifier.clear();
        const auto effects = queue.take();
        CHECK_EQ(effects.size(), 1u);
        const auto* notification = std::get_if<HostNotification>(&effects.front());
        CHECK(notification);
        if (!notification)
            continue;
        CHECK(notification->title == "Build");
        CHECK(notification->body == "Complete");
        CHECK(notification->identifier == "build-1");
        CHECK(notification->occasion == occasion);
    }
}

TEST(hostNotificationBoundsTextWithoutBreakingUTF8)
{
    HostEffectQueue queue;
    HostShellClient shell(queue);
    const auto titleCapacity = Session::BackgroundEvents::titleCapacity;
    const auto bodyCapacity = Session::BackgroundEvents::bodyCapacity;
    const std::string title = std::string(titleCapacity - 1, 't') + "\xe2\x82\xac";
    const std::string body = std::string(bodyCapacity - 2, 'b') + "\xf0\x9f\x8e\x89";
    shell.postNotificationRequest({ title, body, std::string(100, 'i'), NotificationOccasion::Invisible });
    const auto effects = queue.take();
    const auto& notification = std::get<HostNotification>(effects.front());
    CHECK_EQ(notification.title.size(), titleCapacity - 1);
    CHECK_EQ(notification.body.size(), bodyCapacity - 2);
    CHECK_EQ(notification.identifier.size(), OSC::kittyNotificationIdentifierCapacity);
    CHECK_EQ(Unicode::wellFormedUTF8PrefixLength(notification.title), notification.title.size());
    CHECK_EQ(Unicode::wellFormedUTF8PrefixLength(notification.body), notification.body.size());

    shell.postNotification(std::string("broken \xff text"), std::string("body \xc2"));
    const auto repaired = queue.take();
    const auto& repairedNotification = std::get<HostNotification>(repaired.front());
    CHECK_EQ(Unicode::wellFormedUTF8PrefixLength(repairedNotification.title), repairedNotification.title.size());
    CHECK_EQ(Unicode::wellFormedUTF8PrefixLength(repairedNotification.body), repairedNotification.body.size());
    CHECK(repairedNotification.identifier.empty());
    CHECK(repairedNotification.occasion == NotificationOccasion::Always);
}

TEST(hostEffectQueueStillBoundsNotificationsAndClipboardTogether)
{
    HostEffectQueue queue;
    HostShellClient shell(queue);
    shell.postNotification("old", "discarded");
    for (std::size_t index = 0; index < HostEffectQueue::capacity; ++index) {
        if (index % 2)
            queue.push(HostEffectQueue::ClipboardWrite {});
        else
            shell.postNotificationRequest({ "new", "retained", "replacement", NotificationOccasion::Unfocused });
    }
    const auto effects = queue.take();
    CHECK_EQ(effects.size(), HostEffectQueue::capacity);
    CHECK(queue.isEmpty());
    for (const auto& effect : effects) {
        if (const auto* notification = std::get_if<HostNotification>(&effect)) {
            CHECK(notification->title == "new");
            CHECK(notification->identifier == "replacement");
            CHECK(notification->occasion == NotificationOccasion::Unfocused);
        }
    }
}

TEST(notificationRecorderSeesTheBurstBeforeLiveDeliveryDropsItsOldest)
{
    HostEffectQueue queue;
    HostShellClient shell(queue);
    std::vector<std::string> recorded;
    queue.setNotificationRecorder([&](const HostNotification& notification) {
        recorded.push_back(notification.body);
    });
    constexpr std::size_t notificationCount = HostEffectQueue::capacity + 5;
    for (std::size_t index = 0; index < notificationCount; ++index)
        shell.postNotification("Build", std::to_string(index));
    queue.push(HostEffectQueue::ClipboardWrite {});
    CHECK_EQ(recorded.size(), notificationCount);
    for (std::size_t index = 0; index < notificationCount; ++index)
        CHECK(recorded[index] == std::to_string(index));
    CHECK_EQ(queue.take().size(), HostEffectQueue::capacity);
    queue.setNotificationRecorder({});
    shell.postNotification("Build", "No recorder");
    CHECK_EQ(recorded.size(), notificationCount);
}

TEST(deferredNotificationKeepsItsRecordedIdentityWithoutRecordingTwice)
{
    HostEffectQueue queue;
    HostShellClient shell(queue);
    std::size_t recorded = 0;
    queue.setNotificationRecorder([&](const HostNotification& notification) {
        CHECK(notification.identifier == "once");
        CHECK(notification.body.size() == Session::BackgroundEvents::bodyCapacity);
        ++recorded;
    });
    shell.postNotificationRequest({ "Build", std::string(Session::BackgroundEvents::bodyCapacity, 'x'), "once", NotificationOccasion::Invisible });
    auto effects = queue.take();
    REQUIRE(effects.size() == 1);
    auto& notification = std::get<HostNotification>(effects.front());
    notification.primary = Protocol::Sync::ID128 { 17, 19 };
    queue.returnUndelivered(std::move(effects.front()));
    auto deferred = queue.take();
    REQUIRE(deferred.size() == 1);
    CHECK_EQ(recorded, 1u);
    const auto& retained = std::get<HostNotification>(deferred.front());
    CHECK(retained.primary == (Protocol::Sync::ID128 { 17, 19 }));
    CHECK(retained.occasion == NotificationOccasion::Invisible);
    CHECK_EQ(retained.body.size(), Session::BackgroundEvents::bodyCapacity);
}

UBIQUITTY_TEST_MAIN()
