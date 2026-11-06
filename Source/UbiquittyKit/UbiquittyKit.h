// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// Source/UbiquittyKit/UbiquittyKit.h — the Tier-3 embedding umbrella.
//
// One include gives an embedder the batteries-included session facade
// (UbiquittyKit::Session — local or remote behind one object) plus the Tier-1
// model surface it renders from (UbiquittyCore/UbiquittyCore.h: the Terminal, the cell
// grid, the render plan, and the PAL contract the embedder implements), plus the
// one configuration door: hand `Preferences::configure` the TOML document and
// every session and renderer follows it (Core/Preferences/Configuration.h,
// Core/Preferences/RenderConfigurationFold.h).
//
// The supported facade surface is this header's reachable set, versioned by
// UbiquittyKit/Version.h (kitVersion) independently of coreVersion.
//

#include "Core/Preferences/Configuration.h"
#include "Core/Preferences/RenderConfigurationFold.h"
#include "UbiquittyCore/UbiquittyCore.h"
#include "UbiquittyKit/Session.h"
#include "UbiquittyKit/Version.h"
