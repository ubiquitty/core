// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Source/Core/Session/RememberedSessions.cpp — see RememberedSessions.h.

#include "Core/Session/RememberedSessions.h"

#include "Core/Protocol/Sync/WireFormat.h"
#include "Core/Session/AttachTarget.h"
#include "Core/Session/SessionTree.h"
#include "Core/Terminal.h"

#include <algorithm>
#include <cstddef>
#include <mutex>
#include <span>
#include <vector>

namespace Ubiquitty::Session {

void RememberedSessions::setPersistence(Load load, Store store, void* context)
{
    const std::lock_guard<std::mutex> guard(m_mutex);
    m_load = load;
    m_store = store;
    m_context = context;
}

std::string RememberedSessions::encodeRecord(const Record& record)
{
    if (record.lastWorkspaceName.size() > maxWorkspaceNameBytes)
        return {};
    std::vector<uint8_t> bytes;
    Protocol::Sync::putU8(bytes, rememberedSessionRecordVersion);
    Protocol::Sync::putU32(bytes, record.lastSessionID);
    const std::string& name = record.lastWorkspaceName;
    Protocol::Sync::putU16(bytes, static_cast<uint16_t>(name.size()));
    Protocol::Sync::putBytes(bytes, name.data(), name.size());
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

std::optional<RememberedSessions::Record> RememberedSessions::decodeRecord(const std::string& bytes)
{
    Protocol::Sync::Reader reader(
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()));
    const uint8_t version = reader.u8();
    if (!reader.ok || version != rememberedSessionRecordVersion)
        return {};
    Record record;
    record.lastSessionID = reader.u32();
    const uint16_t nameLength = reader.u16();
    if (!reader.ok || nameLength > Protocol::Sync::maxWorkspaceNameLength)
        return {};
    const std::span<const uint8_t> name = reader.bytes(nameLength);
    if (!reader.ok)
        return {};
    record.lastWorkspaceName.assign(reinterpret_cast<const char*>(name.data()), name.size());
    return record;
}

std::optional<RememberedSessions::Record> RememberedSessions::recall(const std::string& hostKey) const
{
    Load load = nullptr;
    void* context = nullptr;
    {
        const std::lock_guard<std::mutex> guard(m_mutex);
        load = m_load;
        context = m_context;
    }
    if (load == nullptr)
        return {};
    std::string bytes;
    if (!load(context, hostKey, bytes))
        return {};
    return decodeRecord(bytes);
}

void RememberedSessions::remember(const std::string& hostKey, uint32_t sessionID, const std::string& workspaceName)
{
    if (workspaceName.size() > maxWorkspaceNameBytes)
        return;
    Store store = nullptr;
    void* context = nullptr;
    {
        const std::lock_guard<std::mutex> guard(m_mutex);
        store = m_store;
        context = m_context;
    }
    if (store == nullptr)
        return;
    Record record;
    record.lastSessionID = sessionID;
    record.lastWorkspaceName = workspaceName;
    store(context, hostKey, encodeRecord(record));
}

void RememberedSessions::forget(const std::string& hostKey)
{
    Store store = nullptr;
    void* context = nullptr;
    {
        const std::lock_guard<std::mutex> guard(m_mutex);
        store = m_store;
        context = m_context;
    }
    if (store == nullptr)
        return;
    // Empty bytes rather than a record naming nothing: a store that answers
    // them decodes to no record, so a forgotten host reads the same as one
    // never visited whether the embedder erases the entry or keeps it empty.
    store(context, hostKey, std::string());
}

namespace {

/// The live workspace whose pane a session draws into.
const TreeWorkspace* workspaceListing(const SessionTree& tree, uint32_t sessionID)
{
    if (sessionID == 0)
        return nullptr;
    for (const TreeWorkspace& workspace : tree.workspaces) {
        if (workspace.isRestorable())
            continue;
        for (const TreeWindow& window : workspace.windows) {
            for (const TreeTab& tab : window.tabs) {
                if (std::ranges::any_of(tab.panes, [sessionID](const TreePane& pane) { return pane.sessionID == sessionID; }))
                    return &workspace;
            }
        }
    }
    return nullptr;
}

/// The session a live workspace is joined on, as opening its row lands
/// (`attachTargetOf`); 0 when no session draws into it.
uint32_t landingSession(const TreeWorkspace& workspace)
{
    return attachTargetOf(workspace).sessionID;
}

/// The workspaces bearing a name, live and restorable apart.
struct Bearers {
    std::vector<const TreeWorkspace*> live;
    std::vector<const TreeWorkspace*> restorable;
    /// The workspaces that do not bear the name and are labelled it.
    std::vector<const TreeWorkspace*> labelled;
};

/// A workspace as one the person may pick.
AttachCandidate candidateOf(const TreeWorkspace& workspace)
{
    return { .workspaceID = workspace.id, .sessionID = attachTargetOf(workspace).sessionID, .open = attachTargetOf(workspace) };
}

/// Makes a decision the question about a name that picked nothing alone.
void askAboutName(AttachDecision& decision, const Bearers& bearers, const std::string& name)
{
    decision.plan = AttachPlan::ChooseFromList;
    decision.workspaceName.clear();
    decision.askedName = name;
    const bool isBorne = !bearers.live.empty() || !bearers.restorable.empty(); // bool-ok: names the one state it reports
    decision.ask = isBorne ? AttachAsk::SeveralBearTheName : AttachAsk::NameIsOnlyALabel;
    for (const auto* list : { &bearers.live, &bearers.restorable }) {
        for (const TreeWorkspace* workspace : *list)
            decision.candidates.push_back(candidateOf(*workspace));
    }
    if (!isBorne) {
        for (const TreeWorkspace* workspace : bearers.labelled)
            decision.candidates.push_back(candidateOf(*workspace));
    }
}

Bearers bearersOf(const SessionTree& tree, const std::string& name)
{
    Bearers bearers;
    for (const TreeWorkspace& workspace : tree.workspaces) {
        if (workspace.name == name)
            (workspace.isRestorable() ? bearers.restorable : bearers.live).push_back(&workspace);
        else if (workspace.label == name)
            bearers.labelled.push_back(&workspace);
    }
    return bearers;
}

/// The rule's plan and target, before the actions beside it are added.
AttachDecision decideTargetOnTree(const std::optional<RememberedSessions::Record>& remembered, const std::string& predefinedWorkspaceName, const SessionTree* listed, uint32_t chosenSessionID)
{
    AttachDecision decision;
    const bool hasRememberedSession = remembered.has_value() && remembered->lastSessionID != 0;
    const TreeWorkspace* rememberedIn = listed && hasRememberedSession ? workspaceListing(*listed, remembered->lastSessionID) : nullptr;
    // Only an answer can say a session ended.
    if (hasRememberedSession && listed && !rememberedIn)
        decision.vanishedSessionID = remembered->lastSessionID;
    if (const TreeWorkspace* chosenIn = listed ? workspaceListing(*listed, chosenSessionID) : nullptr) {
        decision.plan = AttachPlan::AttachByID;
        decision.sessionID = chosenSessionID;
        decision.workspaceID = chosenIn->id;
        decision.workspaceName = chosenIn->name;
        return decision;
    }
    if (!predefinedWorkspaceName.empty()) {
        decision.workspaceName = predefinedWorkspaceName;
        decision.plan = AttachPlan::CreateNamed;
        if (!listed)
            return decision;
        const Bearers bearers = bearersOf(*listed, predefinedWorkspaceName);
        const TreeWorkspace* one = bearers.live.size() == 1 ? bearers.live.front() : bearers.live.empty() && bearers.restorable.size() == 1 ? bearers.restorable.front()
                                                                                                                                            : nullptr;
        if (one) {
            decision.plan = AttachPlan::AttachNamed;
            decision.workspaceID = one->id;
            decision.sessionID = one->isRestorable() ? 0 : landingSession(*one);
            return decision;
        }
        // The name picks more than one workspace, or only a label: the
        // person's to settle, never the host's first match and never a
        // second workspace made beside the one they may mean.
        if (!bearers.live.empty() || !bearers.restorable.empty() || !bearers.labelled.empty())
            askAboutName(decision, bearers, predefinedWorkspaceName);
        return decision;
    }
    // The host restarted: the session is gone and its workspace is recorded.
    if (decision.vanishedSessionID != 0 && !remembered->lastWorkspaceName.empty()) {
        const Bearers bearers = bearersOf(*listed, remembered->lastWorkspaceName);
        if (bearers.live.empty() && bearers.restorable.size() == 1) {
            decision.plan = AttachPlan::AttachNamed;
            decision.workspaceID = bearers.restorable.front()->id;
            decision.workspaceName = remembered->lastWorkspaceName;
            return decision;
        }
    }
    if (hasRememberedSession && (!listed || rememberedIn)) {
        decision.plan = AttachPlan::AttachByID;
        decision.sessionID = remembered->lastSessionID;
        decision.workspaceID = rememberedIn ? rememberedIn->id : 0;
        // What the workspace is called now, once the host has said.
        decision.workspaceName = rememberedIn ? rememberedIn->name : remembered->lastWorkspaceName;
        return decision;
    }
    if (!listed) {
        decision.plan = AttachPlan::AwaitListing;
        return decision;
    }
    if (listed->workspaces.empty()) {
        decision.plan = AttachPlan::CreateNew;
        return decision;
    }
    decision.plan = AttachPlan::ChooseFromList;
    decision.ask = AttachAsk::WhichWorkspace;
    for (const TreeWorkspace& workspace : listed->workspaces)
        decision.candidates.push_back(candidateOf(workspace));
    return decision;
}

} // namespace

AttachDecision RememberedSessions::decide(const std::optional<Record>& remembered,
    const std::string& predefinedWorkspaceName,
    const SessionTree* listed,
    uint32_t chosenSessionID)
{
    AttachDecision decision = decideTargetOnTree(remembered, predefinedWorkspaceName, listed, chosenSessionID);
    // The new session is the plan itself on an empty host; beside any other
    // plan it is a different action.
    decision.isNewSessionOffered = decision.plan != AttachPlan::CreateNew;
    return decision;
}

} // namespace Ubiquitty::Session
