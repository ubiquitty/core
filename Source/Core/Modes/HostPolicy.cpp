// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Modes/HostPolicy.h"
#include "Core/Terminal.h"

namespace Ubiquitty::Modes::HostPolicy {

void setTitlePolicy(Ubiquitty::Terminal* t, OperationPolicy policy)
{
    if (t)
        t->allowTitleOperations = (policy == OperationPolicy::Allowed);
}

void setFontPolicy(Ubiquitty::Terminal* t, OperationPolicy policy)
{
    if (t)
        t->allowFontOperations = (policy == OperationPolicy::Allowed);
}

void setColorPolicy(Ubiquitty::Terminal* t, OperationPolicy policy)
{
    if (t)
        t->allowColorOperations = (policy == OperationPolicy::Allowed);
}

void setWindowPolicy(Ubiquitty::Terminal* t, OperationPolicy policy)
{
    if (t)
        t->allowWindowOperations = (policy == OperationPolicy::Allowed);
}

void setCapabilityQueryPolicy(Ubiquitty::Terminal* t, OperationPolicy policy)
{
    if (t)
        t->allowTerminalCapabilityOperations = (policy == OperationPolicy::Allowed);
}

void setMousePolicy(Ubiquitty::Terminal* t, OperationPolicy policy)
{
    if (t)
        t->allowMouseOperations = (policy == OperationPolicy::Allowed);
}

void setPasteControlsPolicy(Ubiquitty::Terminal* t, OperationPolicy policy)
{
    if (t)
        t->allowPasteControls = (policy == OperationPolicy::Allowed);
}

} // namespace Ubiquitty::Modes::HostPolicy
