// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/OSC/Activity.h"

#include "Core/Codec/Base64.h"
#include "Core/Terminal.h"
#include "Core/TerminalShared.h"
#include "Core/Unicode/UTF8Decoder.h"

#include <algorithm>
#include <charconv>

namespace Ubiquitty::OSC {
namespace {

constexpr std::array<std::string_view, 18> contextKeys {
    "type", "user", "hostname", "machineid", "bootid", "pid", "pidfdid", "comm",
    "cwd", "cmdline", "vm", "container", "targetuser", "targethost", "sessionid",
    "exit", "status", "signal"
};

bool printableUTF8(std::string_view text)
{
    Unicode::UTF8Decoder decoder {};
    for (unsigned char byte : text) {
        std::uint32_t codePoint = 0;
        const auto result = Unicode::utf8Feed(&decoder, byte, &codePoint);
        if (result == Unicode::UTF8Status::Invalid) {
            return false;
        }
        if (result == Unicode::UTF8Status::Emit && (codePoint < 0x20 || (codePoint >= 0x7F && codePoint <= 0x9F))) {
            return false;
        }
    }
    return !decoder.need;
}

std::string_view takeField(std::string_view& input, char separator)
{
    const auto end = input.find(separator);
    const auto field = input.substr(0, end);
    input = end == std::string_view::npos ? std::string_view {} : input.substr(end + 1);
    return field;
}

bool asciiLetter(char character)
{
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
}

bool segmentByte(char character)
{
    return asciiLetter(character) || (character >= '0' && character <= '9') || character == '_' || character == '.' || character == '+' || character == '-';
}

bool validSegment(std::string_view value)
{
    return !value.empty() && value.size() <= 32 && std::ranges::all_of(value, segmentByte);
}

bool validStatusID(std::string_view value)
{
    if (value.empty() || value.size() > 128 || value.back() == '/') {
        return false;
    }
    unsigned depth = 0;
    while (!value.empty()) {
        if (++depth > 8 || !validSegment(takeField(value, '/'))) {
            return false;
        }
    }
    return true;
}

bool validContextID(std::string_view value)
{
    return !value.empty() && value.size() <= 64 && std::ranges::all_of(value, [](unsigned char byte) { return byte >= 32 && byte <= 126; });
}

bool unsignedNumber(std::string_view value, std::uint64_t maximum = UINT64_MAX)
{
    if (value.empty()) {
        return false;
    }
    std::uint64_t number = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), number);
    return result.ec == std::errc {} && result.ptr == value.data() + value.size() && number <= maximum;
}

bool validContextField(std::size_t index, std::string_view value)
{
    if (value.size() > 255 || !printableUTF8(value)) {
        return false;
    }
    switch (static_cast<ContextField>(index)) {
    case ContextField::Type:
        return value == "boot" || value == "container" || value == "vm" || value == "elevate" || value == "chpriv"
            || value == "subcontext" || value == "remote" || value == "shell" || value == "command" || value == "app" || value == "service" || value == "session";
    case ContextField::MachineID:
    case ContextField::BootID:
        return value.size() >= 32 && value.size() <= 36 && std::ranges::all_of(value, [](char character) {
            return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f') || (character >= 'A' && character <= 'F') || character == '-';
        });
    case ContextField::PID:
    case ContextField::PIDFDID:
        return unsignedNumber(value);
    case ContextField::Status:
        return unsignedNumber(value, 255);
    case ContextField::Exit:
        return value == "success" || value == "failure" || value == "crash" || value == "interrupt";
    case ContextField::Signal:
        return value.starts_with("SIG") && value.size() > 3 && std::ranges::all_of(value, [](char character) {
            return (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9') || character == '+' || character == '-';
        });
    case ContextField::CommandLine:
        return true;
    default:
        return !value.empty();
    }
}

std::optional<std::string> unescapeContext(std::string_view value)
{
    std::string decoded;
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] != '\\') {
            decoded += value[index];
            continue;
        }
        const auto escape = value.substr(index, 4);
        if (escape == "\\x3b" || escape == "\\x3B") {
            decoded += ';';
        } else if (escape == "\\x5c" || escape == "\\x5C") {
            decoded += '\\';
        } else {
            return std::nullopt;
        }
        index += 3;
    }
    return decoded;
}

std::string_view trim(std::string_view text)
{
    constexpr std::string_view whitespace = " \t\r\n\v\f";
    const auto first = text.find_first_not_of(whitespace);
    if (first == std::string_view::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(whitespace) - first + 1);
}

bool decodedText(std::string_view encoded, std::size_t limit, FallibleString& result)
{
    if (!result.tryResize(limit))
        return false;
    const auto decoded = Codec::base64Decode(encoded, std::span<char>(result));
    if (!decoded) {
        return false;
    }
    result.resize(decoded->size());
    return printableUTF8(result);
}

bool isDescendant(std::string_view child, std::string_view parent)
{
    return parent.empty() || (child.size() > parent.size() && child.starts_with(parent) && child[parent.size()] == '/');
}

} // namespace

bool validContext(const Context& context, ContextPhase phase)
{
    ContextView view { .id = context.id };
    std::ranges::copy(context.fields, view.fields.begin());
    return validContext(view, phase);
}

bool validContext(const ContextView& context, ContextPhase phase)
{
    if (!validContextID(context.id)) {
        return false;
    }
    for (std::size_t index = 0; index < context.fields.size(); ++index) {
        if (!context.fields[index].empty() && ((phase == ContextPhase::Active && index >= 15) || !validContextField(index, context.fields[index]))) {
            return false;
        }
    }
    return true;
}

bool applyContext(ContextState& state, std::string_view payload)
{
    if (payload.size() > 16384) {
        return false;
    }
    const auto command = takeField(payload, ';');
    const bool isStart = command.starts_with("start=");
    if (!isStart && !command.starts_with("end=")) {
        return false;
    }
    const auto id = unescapeContext(command.substr(isStart ? 6 : 4));
    if (!id || !validContextID(*id)) {
        return false;
    }
    auto found = std::ranges::find_if(state.active, [&](const auto& context) { return context.id.view() == *id; });
    if (!isStart && found == state.active.end()) {
        return false;
    }
    if (isStart && found == state.active.end() && state.active.size() >= contextDepthLimit) {
        return false;
    }
    Context context = isStart ? Context {} : *found;
    context.id = *id;
    while (!payload.empty()) {
        auto field = takeField(payload, ';');
        const auto key = takeField(field, '=');
        const auto position = std::ranges::find(contextKeys, key);
        if (position == contextKeys.end()) {
            continue;
        }
        const auto index = static_cast<std::size_t>(position - contextKeys.begin());
        if ((isStart && index >= 15) || (!isStart && index < 15)) {
            continue;
        }
        const auto value = unescapeContext(field);
        if (value && validContextField(index, *value)) {
            context.fields[index] = *value;
        }
    }
    if (found != state.active.end()) {
        state.active.erase(found, state.active.end());
    }
    if (isStart) {
        state.active.push_back(std::move(context));
    } else {
        state.lastEnded = std::move(context);
    }
    return true;
}

bool validProgramStatus(const ProgramStatus& record)
{
    return validProgramStatus(ProgramStatusView { record.id, record.app, record.title, record.message,
        record.state, record.kind, record.progress });
}

bool validProgramStatus(const ProgramStatusView& record)
{
    return (record.id.empty() || validStatusID(record.id)) && (record.app.empty() || validSegment(record.app))
        && record.state >= ProgramState::Idle && record.state <= ProgramState::Error
        && record.kind <= ProgramBlockKind::Auth && (record.state == ProgramState::Blocked || record.kind == ProgramBlockKind::None)
        && record.progress >= -1 && record.progress <= 100
        && (record.progress == -1 || record.state == ProgramState::Working || record.state == ProgramState::Blocked)
        && record.title.size() <= maxProgramStatusTitleBytes && record.message.size() <= maxProgramStatusMessageBytes && printableUTF8(record.title) && printableUTF8(record.message);
}

bool applyProgramStatus(ProgramStatusStore& store, std::string_view payload)
{
    // The body cap reserves the seven-byte introducer and two-byte ST.
    if (payload.size() > 4087) {
        return false;
    }
    ProgramStatus record;
    std::string_view state, kind, progress;
    bool hasID = false;
    while (!payload.empty()) {
        auto pair = takeField(payload, ':');
        const auto equals = pair.find('=');
        if (equals == std::string_view::npos) {
            continue;
        }
        const auto key = trim(pair.substr(0, equals));
        const auto value = trim(pair.substr(equals + 1));
        if (key.size() > 16 || (key == "msg" && value.size() > 2732) || (key == "title" && value.size() > 256)
            || (key == "app" && value.size() > 32) || (key == "id" && value.size() > 128)) {
            return false;
        }
        if (key == "id") {
            hasID = true;
            record.id = value;
            auto segments = value;
            unsigned depth = 0;
            while (!segments.empty()) {
                if (++depth > 8 || takeField(segments, '/').size() > 32) {
                    return false;
                }
            }
        }
        if (key.empty() || !std::ranges::all_of(key, [](char character) { return character >= 'a' && character <= 'z'; })
            || !std::ranges::all_of(value, [](char character) { return segmentByte(character) || character == ',' || character == '/' || character == '='; })) {
            continue;
        }
        if (key == "state") {
            state = value;
        } else if (key == "kind") {
            kind = value;
        } else if (key == "progress") {
            progress = value;
        } else if (key == "app") {
            record.app = validSegment(value) ? value : std::string_view {};
        } else if (key == "msg") {
            if (!decodedText(value, maxProgramStatusMessageBytes, record.message)) {
                return false;
            }
        } else if (key == "title") {
            if (!decodedText(value, maxProgramStatusTitleBytes, record.title)) {
                return false;
            }
        }
    }
    if (hasID && !validStatusID(record.id)) {
        return false;
    }
    if (state == "idle") {
        record.state = ProgramState::Idle;
    } else if (state == "working") {
        record.state = ProgramState::Working;
    } else if (state == "done") {
        record.state = ProgramState::Done;
    } else if (state == "blocked") {
        record.state = ProgramState::Blocked;
    } else if (state == "error") {
        record.state = ProgramState::Error;
    } else if (state == "clear") {
        const auto firstRemoved = std::remove_if(store.records.begin(), store.records.end(), [&](const ProgramStatus& existing) { return existing.id == record.id || isDescendant(existing.id, record.id); });
        const bool didRemove = firstRemoved != store.records.end();
        store.records.erase(firstRemoved, store.records.end());
        return didRemove;
    } else {
        return false;
    }
    if (record.state == ProgramState::Blocked) {
        if (kind == "permission") {
            record.kind = ProgramBlockKind::Permission;
        } else if (kind == "question") {
            record.kind = ProgramBlockKind::Question;
        } else if (kind == "auth") {
            record.kind = ProgramBlockKind::Auth;
        }
    }
    if ((record.state == ProgramState::Working || record.state == ProgramState::Blocked) && unsignedNumber(progress, 100)) {
        std::from_chars(progress.data(), progress.data() + progress.size(), record.progress);
    }
    const auto firstRemoved = std::remove_if(store.records.begin(), store.records.end(), [&](const ProgramStatus& existing) { return existing.id == record.id; });
    store.records.erase(firstRemoved, store.records.end());
    if (store.records.size() >= programStatusLimit) {
        store.records.erase(store.records.begin());
    }
    store.records.push_back(std::move(record));
    return true;
}

std::string_view ProgramStatusStore::effectiveApp(const ProgramStatus& record) const
{
    if (!record.app.empty()) {
        return record.app;
    }
    std::string_view id = record.id;
    while (!id.empty()) {
        const auto slash = id.rfind('/');
        id = slash == std::string_view::npos ? std::string_view {} : id.substr(0, slash);
        const auto found = std::ranges::find_if(records, [&](const auto& existing) { return existing.id.view() == id; });
        if (found != records.end() && !found->app.empty()) {
            return found->app;
        }
    }
    return {};
}

const ProgramStatus* ProgramStatusStore::primary() const
{
    const ProgramStatus* chosen = nullptr;
    for (const auto& record : records) {
        if (!chosen || record.state >= chosen->state) {
            chosen = &record;
        }
    }
    return chosen;
}

void activityChanged(Terminal& terminal)
{
    ++terminal.activityGeneration;
    ++terminal.tabStatusGeneration;
}

void finishProgramActivity(Terminal& terminal)
{
    auto& records = terminal.programStatus.records;
    const auto firstRemoved = std::remove_if(records.begin(), records.end(), [](const ProgramStatus& record) {
        return record.state == ProgramState::Working || record.state == ProgramState::Blocked;
    });
    if (firstRemoved != records.end()) {
        records.erase(firstRemoved, records.end());
        activityChanged(terminal);
    }
}

void foregroundReturnedToShell(Terminal& terminal)
{
    if (!terminal.hasMarkedPrompt)
        finishProgramActivity(terminal);
}

void handleContext(Terminal& terminal, std::string_view payload)
{
    if (applyContext(terminal.contexts, payload)) {
        activityChanged(terminal);
    }
}

void handleProgramStatus(Terminal& terminal, std::string_view payload)
{
    if (payload == "?") {
        respond(terminal, Capability::StatusReport, oscReplyTerminator(terminal) == "\a" ? std::string_view { "\033]7501;?\a" } : std::string_view { "\033]7501;?\033\\" });
    } else if (terminal.parser.osc.length <= 4092 && applyProgramStatus(terminal.programStatus, payload)) {
        activityChanged(terminal);
    }
}

} // namespace Ubiquitty::OSC
