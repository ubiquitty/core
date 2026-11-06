// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Runtime/TriggerEffect.h"

#include "Core/OSC/Notification.h"
#include "Core/Preferences/PreferencesAccessors.h"
#include "Core/Triggers/Actions.h"
#include "Core/Triggers/Engine.h"

namespace Ubiquitty::Runtime {

bool doesTriggerActionDispatch(std::uint8_t action)
{
    switch (static_cast<Triggers::Action>(action)) {
    case Triggers::Action::Notify:
    case Triggers::Action::Copy:
    case Triggers::Action::SetTitle:
    case Triggers::Action::RunCommand:
    case Triggers::Action::Webhook:
        return true;
    case Triggers::Action::Highlight:
    case Triggers::Action::None:
        return false;
    }
    return false;
}

TriggerEffect resolveTriggerEffect(std::uint8_t action, std::string_view argument, std::string_view matchedText,
    const Preferences::Values& configuration)
{
    TriggerEffect effect;
    switch (static_cast<Triggers::Action>(action)) {
    case Triggers::Action::Notify: {
        // The rule's payload is the heading and the match the body; a rule
        // with no payload promotes the match into the heading, by the policy
        // OSC 9 notifications already go through, so the banner is never a
        // blank title over the text.
        OSC::NotificationDisplay display = OSC::resolveNotificationDisplay(argument, matchedText);
        if (!display.shouldPost)
            break;
        effect.kind = TriggerEffectKind::Notify;
        effect.title = std::move(display.title);
        effect.body = std::move(display.body);
        break;
    }
    case Triggers::Action::Copy:
        effect.kind = TriggerEffectKind::Copy;
        effect.text = std::string(matchedText);
        break;
    case Triggers::Action::SetTitle: {
        // A literal title when the rule gives one; the match stands in when it
        // does not, so a bare `title` rule names the window after what it saw.
        const std::string_view title = !argument.empty() ? argument : matchedText;
        if (title.empty())
            break;
        effect.kind = TriggerEffectKind::SetTitle;
        effect.title = std::string(title);
        break;
    }
    case Triggers::Action::RunCommand: {
        // Closed by default: a `run_command` rule is inert until the reader opens the
        // gate by name.
        if (!Preferences::Effective::allowRunCommand(configuration))
            break;
        std::vector<std::string> arguments = Triggers::buildCommandArgv(argument, matchedText);
        if (arguments.empty())
            break;
        effect.kind = TriggerEffectKind::RunCommand;
        effect.arguments = std::move(arguments);
        break;
    }
    case Triggers::Action::Webhook:
        // Closed by default, and http(s) only when open, so a webhook cannot
        // double as a local opener. The match rides in the body as a JSON
        // value and never in the URL.
        if (!Preferences::Effective::allowWebhook(configuration))
            break;
        if (!Triggers::webhookSchemeAllowed(argument))
            break;
        effect.kind = TriggerEffectKind::Webhook;
        effect.url = std::string(argument);
        effect.requestBody = Triggers::buildWebhookBody(matchedText);
        break;
    case Triggers::Action::Highlight:
    case Triggers::Action::None:
        break;
    }
    return effect;
}

} // namespace Ubiquitty::Runtime
