# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

#
# ubiquitty-core: libubiquitty and the multiplexer.
#
# One tree, selected at configure time by -DUBIQUITTY_FRONTEND=<name>:
#
#   Core    libUbiquittyCore and its layer libraries, the core-consumer sample,
#           the snapshot tool and the installable package. The default.
#           (Docs/libubiquittycore.md)
#   Host    ubiquitty-host, the persistent session daemon.
#           (Scripts/build-host.sh)
#   Agent   ubq, the headless Sync client and MCP server for AI agents.
#           (Scripts/build-agent.sh)
#
# The renderers, the terminal views and the apps live in the repositories that
# depend on this one. A project that compiles Core inside its own build includes
# the modules under Scripts/cmake from its top directory, in the order
# Scripts/cmake/UbiquittyCoreRoot.cmake lays out.
#
# The Core builds as C++23 with -fno-exceptions and -fno-rtti (WebKit style).
# See Source/PAL/PAL.h and Docs/architecture.md.

cmake_minimum_required(VERSION 3.13)

include("${CMAKE_CURRENT_LIST_DIR}/Scripts/cmake/UbiquittyCoreRoot.cmake")
# The version is this tree's: its commit and its VERSION file.
if(NOT DEFINED UBIQUITTY_VERSION_SOURCE_DIR)
    set(UBIQUITTY_VERSION_SOURCE_DIR "${CMAKE_SOURCE_DIR}")
endif()
set(UBIQUITTY_SOURCE_INCLUDE_DIRECTORIES "${UBIQUITTY_CORE_SOURCE_DIR}/Source")

# The compiler launcher, the deployment floors, the SDK and the version, which
# project() reads.
include("${UBIQUITTY_CORE_SOURCE_DIR}/Scripts/cmake/UbiquittyPreProject.cmake")
project(UbiquittyCore VERSION "${UBIQUITTY_VERSION_NAMED}" LANGUAGES C CXX)

if(NOT DEFINED UBIQUITTY_FRONTEND)
    set(UBIQUITTY_FRONTEND "Core")
endif()
set(UBIQUITTY_FRONTEND "${UBIQUITTY_FRONTEND}" CACHE STRING "Frontend to build (Core, Host, Agent)")
message(STATUS "Ubiquitty frontend: ${UBIQUITTY_FRONTEND}")

# Core's toolchain, optimisation, sources and libraries, which every arm below
# builds on.
include("${UBIQUITTY_CORE_SOURCE_DIR}/Scripts/cmake/UbiquittyToolchain.cmake")
include("${UBIQUITTY_CORE_SOURCE_DIR}/Scripts/cmake/UbiquittyOptimize.cmake")
include("${UBIQUITTY_CORE_SOURCE_DIR}/Scripts/cmake/UbiquittyCoreSources.cmake")
include("${UBIQUITTY_CORE_SOURCE_DIR}/Scripts/cmake/UbiquittyCoreLibrary.cmake")

# ----------------------------------------------------------------------
# Host — ubiquitty-host, the persistent session daemon: the Core runs on the
# server and Ubiquitty attaches as a thin client (Docs/architecture.md §9a).
#
# A portable-POSIX binary (no Cocoa / Metal / Swift):
# the portable Core subset + Source/Core/Protocol/Sync (the wire protocol) + the
# POSIX PTY (Source/Platform/Host/PTY) + the host PAL down-functions
# (HostPAL) + the host main. Builds with clang on macOS (local testing)
# and gcc on Linux. The Core source list mirrors the test_sync
# link set in Scripts/run-tests.sh — a live Terminal pulls in the full OSC /
# SGR / Tek / Kitty / Unicode dispatch surface.
# ----------------------------------------------------------------------
if(UBIQUITTY_FRONTEND STREQUAL "Host")

    # The host daemon's Core link set = the canonical portable-Core list minus
    # the TUs a headless, render-free server never references. The minimal
    # daemon drops the on-screen renderers (box/glyph/software/Unifont),
    # the input encoders, selection, session-persistence + the layout/process
    # model, the local-socket/poll transports, and the QUIC client carrier (the
    # daemon listens via HostQUIC, gated below). A Core .cpp added to the SSOT
    # flows in here automatically unless it joins this trim list.
    # Theme resolution, palette adaptation, and trigger configuration stay:
    # the host seeds terminals with the same launch policy as graphical clients.
    ubiquitty_core_subset(UBIQUITTY_HOST_CORE ABSOLUTE EXCLUDE
        # App-level theme/appearance/config-reload policy, which drives a host
        # seam a GUI frontend implements. A headless arm follows no system
        # appearance and watches no config file, and keeping it drags the whole
        # Themes resolution in behind it.
        Source/Core/App/AppRuntime.cpp
        # The first-launch Welcome screen and the shell wrapper one of its
        # buttons installs. A headless arm shows no screen and installs nothing
        # into a reader's startup files.
        Source/Core/App/ShellIntegrationInstall.cpp
        Source/Core/App/WelcomeScreen.cpp
        # The theme list and a theme's colours, for a frontend's picker. A
        # headless arm shows none, and keeping it drags Themes resolution in
        # behind it, exactly as AppRuntime above does.
        Source/Core/Themes/ThemeApply.cpp
        # Codec::JSON, ControlRPC and the method registry stay: a Sync
        # `Call` is a JSON-RPC request the daemon reads (Sync/Control.cpp) and
        # serves only when the registry's Sync column names it, so yyjson
        # joins the target below. The schema validator stays with them: the
        # daemon checks a call's parameters against its entry's schema before
        # any effect (Control::admitCall). The control socket's server goes:
        # the daemon serves its calls over the Sync protocol, not the
        # frontends' JSON-RPC socket.
        Source/Core/Protocol/Control/Server.cpp
        # LinkRun.cpp STAYS in the daemon's set since the frame capture
        # learned to resolve link kinds: TerminalFrameInputs.cpp is kept and
        # references kindForLinkID unconditionally, and this list resolves
        # names, not the call graph — trimming it again surfaces only at
        # link time, in every stanza linking the daemon's objects. The
        # pointer-hover walk below still goes: nothing kept calls it.
        # PAL/Process.cpp STAYS: the daemon reads a hosted pane's facts with
        # the walk reading a local pane uses (Session::walkedProcessFacts), so
        # the chain a title shows is formatted by the same code on both sides.
        Source/Core/Screen/VisibleLink.cpp
        Source/PAL/ProcessSignal.cpp
        # ScrollSearch.cpp stays: `pane.search` runs its history search on the
        # host (Sync/Control.cpp), and RE2 joins the target below.
        Source/Core/Screen/ScrollTuning.cpp
        Source/Core/Session/SessionInfo.cpp
        # The drawing-debug readout draws for a frontend the daemon is not.
        # The heatmap's counters stay: the frame capture kept here feeds them.
        Source/Core/Diagnostics/DrawingDebug.cpp
        Source/Core/Input/ComposeEngine.cpp
        Source/Core/Input/Keybindings.cpp
        # Keys/Keymap.cpp and Keys/KittyKeyboard.cpp stay: a remote client's
        # keys are encoded here, under the pane's live modes
        # (Runtime/RemoteInput.cpp).
        Source/Core/Input/PointerInput.cpp
        Source/Core/Input/PointerLadder.cpp
        Source/Core/Protocol/Tmux/Commands.cpp
        Source/Core/Protocol/Tmux/Session.cpp
        Source/Core/Runtime/TmuxControlHost.cpp
        Source/Core/Protocol/Tmux/Tiler.cpp
        Source/Core/Rendering/BoxDrawCommands.cpp
        Source/Core/Rendering/BoxDrawing.cpp
        Source/Core/Rendering/BoxEdgeStretch.cpp
        Source/Core/Rendering/BoxFillEmit.cpp
        Source/Core/Rendering/BoxGlyphRaster.cpp
        Source/Core/Rendering/BoxStrokeProfile.cpp
        Source/Core/Rendering/CellInstanceBuilder.cpp
        Source/Core/Rendering/LetterLayout.cpp
        Source/Core/Rendering/OverlayAssembly.cpp
        Source/Core/Selection/SelectionSpans.cpp
        # The Terminal-bound selection facade is per-view UI machinery over the
        # span gathers the line above trims: a headless daemon drives no mouse
        # gesture, and keeping the facade while trimming its gathers is exactly
        # the link error that motivates trimming them together.
        Source/Core/Selection/TerminalSelection.cpp
        Source/Core/Selection/SelectionOverCapture.cpp
        # The overlay span gathers walk search/hilite/trigger state into row
        # spans for a frame the daemon never draws; they reference the trimmed
        # renderers (ScrollSearch, OverlayGeometry), so they travel with them.
        Source/Core/Rendering/OverlaySpanGather.cpp
        Source/Core/Rendering/ColorResolve.cpp
        Source/Core/Rendering/FontFeatureSpecification.cpp
        Source/Core/Rendering/FontVariationSpecification.cpp
        Source/Core/Rendering/ShapedCellCache.cpp
        Source/Core/Rendering/LigatureClassification.cpp
        Source/Core/Rendering/FontMetrics.cpp
        Source/Core/Rendering/GlyphSlotCache.cpp
        Source/Core/Rendering/LigatureSheet.cpp
        Source/Core/Rendering/OverlayGeometry.cpp
        Source/Core/Rendering/PNGEncoder.cpp
        Source/Core/Rendering/ProportionalGlyphSheet.cpp
        Source/Core/Rendering/SoftwareRenderer.cpp
        Source/Core/Rendering/BackgroundImage.cpp
        Source/Core/Rendering/TerminalSnapshot.cpp
        Source/Core/Rendering/BundledRichText.cpp
        Source/Core/Rendering/Unifont.cpp
        Source/Core/Rendering/ProportionalStrike.cpp
        Source/Core/Rendering/ProportionalStrikeData.cpp
        Source/Core/Rendering/ProportionalOutline.cpp
        Source/Core/Rendering/ProportionalOutlineData.cpp
        Source/Core/Rendering/UnifontData.cpp
        # The proportional strip's raster and the copy decision arrived in Core
        # on 2026-09-05 and flowed into every subset, as a new Core .cpp does.
        # The daemon rasters nothing — it keeps no rasterizer, which is what
        # this one needs — and it runs no clipboard: the frontend decides a
        # copy and the daemon relays the bytes.
        Source/Core/Rendering/ProportionalStripRaster.cpp
        Source/Core/Selection/CopyClipboard.cpp
        Source/Core/Rendering/UnifontRasterizer.cpp
        Source/Core/Runtime/TerminalRuntime.cpp
        # Selection.cpp stays: Core/Screen/ClearBuffer.cpp is in the
        # SSOT and calls Selection::clear, so excluding it drops a definition a
        # kept translation unit needs. The exclude list resolves names, not the
        # call graph, so a trim like that surfaces only at link time.
        Source/Core/Selection/StyledClipboard.cpp
        # Layout::Workspace, Surface and the workspace record stay: the host is
        # the authority for a hosted workspace's topology (Docs/session-model.md
        # §6 — a daemon Session is a pane's leaf backing, the Workspace sits above
        # them). They are fixed-pool data with no platform or render types.
        # Runtime::WorkspaceSession is a different thing: the frontend's runtime
        # (TerminalSession, SwiftInterop) and stays out with TerminalRuntime.
        Source/Core/Terminfo/UbiquittyTerminfo.cpp
        Source/Core/Themes/ThemeImport.cpp
        Source/Core/Transport/LocalSocketChannel.cpp
        # LocalPTYChannel derives from PollChannel, so it must be trimmed
        # wherever its base is — otherwise its vtable, ctor and dtor have no
        # definition and the link fails (Release hides it by eliding the
        # unused vtable; Debug does not).
        Source/Core/Transport/DelegatedChannel.cpp
        Source/Core/Transport/KnownHosts.cpp
        Source/Core/Transport/LocalPTYChannel.cpp)
    # Unicode/Normalize.cpp is not trimmed: ScrollSearch, which `pane.search`
    # runs, calls it.

    add_executable(ubiquitty-host
        # Host frontend: the daemon main, the forkpty transport + platform seam,
        # and the host PAL down-functions (none of these live in the portable
        # SSOT — they are the server's own glue).
        ${UBIQUITTY_CORE_SOURCE_DIR}/Source/Platform/Host/main.cpp
        ${UBIQUITTY_CORE_SOURCE_DIR}/Source/Platform/POSIX/LocalHostInstall.cpp
        ${UBIQUITTY_CORE_SOURCE_DIR}/Source/Platform/Host/AttachRelay.cpp
        ${UBIQUITTY_CORE_SOURCE_DIR}/Source/Platform/Host/Daemon.cpp
        ${UBIQUITTY_CORE_SOURCE_DIR}/Source/Platform/Host/HandoverAdmission.cpp
        ${UBIQUITTY_CORE_SOURCE_DIR}/Source/Platform/Host/HandoverRequest.cpp
        ${UBIQUITTY_CORE_SOURCE_DIR}/Source/Platform/Host/HandoverAuthorityTables.cpp
        ${UBIQUITTY_CORE_SOURCE_DIR}/Source/Platform/Host/HandoverGlobalTables.cpp
        ${UBIQUITTY_CORE_SOURCE_DIR}/Source/Platform/Host/HandoverEffects.cpp
        ${UBIQUITTY_CORE_SOURCE_DIR}/Source/Platform/Host/HandoverRuntime.cpp
        ${UBIQUITTY_CORE_SOURCE_DIR}/Source/Platform/Host/HandoverTables.cpp
        ${UBIQUITTY_CORE_SOURCE_DIR}/Source/Platform/Host/HandoverWorkspaceTables.cpp
        ${UBIQUITTY_CORE_SOURCE_DIR}/Source/Platform/Host/HostTableHandover.cpp
        ${UBIQUITTY_CORE_SOURCE_DIR}/Source/Platform/Host/HostHandover.cpp
        ${UBIQUITTY_CORE_SOURCE_DIR}/Source/Platform/Host/HandoverStartup.cpp
        ${UBIQUITTY_CORE_SOURCE_DIR}/Source/Platform/Host/SessionHandoverFacts.cpp
        ${UBIQUITTY_CORE_SOURCE_DIR}/Source/Platform/Host/SessionHandover.cpp
        ${UBIQUITTY_CORE_SOURCE_DIR}/Source/Platform/POSIX/PTYStream.cpp
        ${UBIQUITTY_CORE_SOURCE_DIR}/Source/Platform/POSIX/PTYFacts.cpp
        ${UBIQUITTY_CORE_SOURCE_DIR}/Source/PAL/OutputQueue.cpp
        ${UBIQUITTY_CORE_SOURCE_DIR}/Source/Platform/Host/HostPaths.cpp
        ${UBIQUITTY_CORE_SOURCE_DIR}/Source/Platform/Host/ClipboardTool.cpp
        ${UBIQUITTY_CORE_SOURCE_DIR}/Source/Platform/Host/ResumeRecordTool.cpp
        ${UBIQUITTY_CORE_SOURCE_DIR}/Source/Platform/Host/ShellIntegrationFiles.cpp
        ${UBIQUITTY_CORE_SOURCE_DIR}/Source/Platform/Host/HostPAL.cpp
        # In-process adoption fixtures link this same POSIX ownership sender
        # as the native app, without an exiting descriptor-adoption CLI helper.
        ${UBIQUITTY_CORE_SOURCE_DIR}/Source/Platform/POSIX/SessionHandoff.cpp
        ${UBIQUITTY_CORE_SOURCE_DIR}/Source/Platform/POSIX/HandoverLease.cpp
        ${UBIQUITTY_CORE_SOURCE_DIR}/Source/Platform/POSIX/HandoverTransport.cpp
        # Portable Core (single-sourced from Scripts/portable-core-sources.txt;
        # ControlPaths + Tmux.cpp + NativeAttachment + QUICBootstrap come in via the
        # SSOT — Core's Terminal embeds the tmux -CC client, so Tmux.cpp stays).
        ${UBIQUITTY_HOST_CORE}
        ${UBIQUITTY_SIMDUTF_SOURCES}
        # Codec/JSON.cpp wraps it, for the calls Sync/Control.cpp reads.
        ${UBIQUITTY_YYJSON_SOURCE}
        # ScrollSearch.cpp stays in this subset for `pane.search`, so the RE2
        # units ride along.
        ${UBIQUITTY_RE2_SOURCES}
    )
    ubiquitty_embed_builtin_themes(_ubq_host_themes_gen)
    target_sources(ubiquitty-host PRIVATE ${_ubq_host_themes_gen})
    set(_ubq_host_shell_integration_gen ${CMAKE_CURRENT_BINARY_DIR}/ShellIntegrationScripts.gen.cpp)
    ubiquitty_generate_shell_integration_source(${_ubq_host_shell_integration_gen})
    target_sources(ubiquitty-host PRIVATE ${_ubq_host_shell_integration_gen})
    target_include_directories(ubiquitty-host PRIVATE "${UBIQUITTY_CORE_SOURCE_DIR}/Source")
    target_include_directories(ubiquitty-host SYSTEM PRIVATE "${UBIQUITTY_SIMDUTF_DIR}" "${UBIQUITTY_RE2_DIR}")
    target_compile_features(ubiquitty-host PRIVATE cxx_std_23)
    set_target_properties(ubiquitty-host PROPERTIES
        CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    # No try/catch, no dynamic_cast/typeid in this target's code — drop
    # the C++ runtime overhead we don't use, matching the host test suite.
    target_compile_options(ubiquitty-host PRIVATE
        -fno-exceptions -fno-rtti -Wall -Wextra
        ${UBIQUITTY_NARROWING_WARNING}
        ${UBIQUITTY_OWNERSHIP_WARNINGS}
        "$<$<BOOL:${UBIQUITTY_WERROR}>:-Werror>")
    # A release's host trusts its successor, its app and its relay by the
    # release team's code signature alone, and refuses an unsigned or ad hoc
    # binary wherever it lies; a development build also trusts the files it
    # was installed as and resolved, as it runs unsigned or under a
    # development certificate (Source/Platform/Host/SignaturePolicy.h). Off
    # unless a release build turns it on through
    # build-macos.sh --release-signing and build-host.sh.
    option(UBIQUITTY_RELEASE_SIGNING "The host trusts only the release team's code signature" OFF)
    target_compile_definitions(ubiquitty-host PRIVATE UBIQUITTY_RELEASE_SIGNING=$<BOOL:${UBIQUITTY_RELEASE_SIGNING}>)
    # forkpty lives in libutil on Linux (glibc); on macOS it is in libc
    # via <util.h>, so the link is a no-op there.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_libraries(ubiquitty-host PRIVATE util)
    endif()
    if(APPLE)
        target_link_libraries(ubiquitty-host PRIVATE "-framework CoreFoundation" "-framework Security")
    endif()
    # QUIC listener (ngtcp2 + AWS-LC), opt-in. Link order: crypto backend →
    # ngtcp2 → AWS-LC ssl → crypto (most-dependent first for static archives).
    if(UBIQUITTY_QUIC)
        ubiquitty_ensure_quic_deps() # fetch the pinned QUIC TLS deps if absent
        # DatagramBatch beside it: the write pump sends through the same batch
        # the client reads through, so the platform difference lives in one file
        # rather than once per direction.
        target_sources(ubiquitty-host PRIVATE
            ${UBIQUITTY_CORE_SOURCE_DIR}/Source/Platform/Host/HostQUIC.cpp
            ${UBIQUITTY_CORE_SOURCE_DIR}/Source/Platform/POSIX/DatagramBatch.cpp)
        target_include_directories(ubiquitty-host PRIVATE "${UBIQUITTY_QUIC_PREFIX}/include")
        target_compile_definitions(ubiquitty-host PRIVATE UBIQUITTY_QUIC=1)
        ubiquitty_link_quic_archives(ubiquitty-host PRIVATE "${UBIQUITTY_QUIC_PREFIX}")
    endif()

    if(UBIQUITTY_BUILD_SYNC_TRANSPORT_ACCEPTANCE)
        # The fixture's server uses the shipping host source set and reactor.
        # Its attach/bootstrap modes call the same production entry point;
        # renaming only this separate object leaves ubiquitty-host unchanged.
        get_target_property(_ubq_transport_host_sources ubiquitty-host SOURCES)
        list(REMOVE_ITEM _ubq_transport_host_sources
            ${UBIQUITTY_CORE_SOURCE_DIR}/Source/Platform/Host/main.cpp)
        add_library(ubq-sync-transport-host-objects OBJECT EXCLUDE_FROM_ALL
            ${_ubq_transport_host_sources}
            ${UBIQUITTY_CORE_SOURCE_DIR}/Source/Platform/POSIX/QUICClient.cpp
            ${UBIQUITTY_CORE_SOURCE_DIR}/Source/Core/Transport/QUICChannel.cpp)
        add_library(ubq-sync-transport-main-object OBJECT EXCLUDE_FROM_ALL
            ${UBIQUITTY_CORE_SOURCE_DIR}/Source/Platform/Host/main.cpp)
        target_compile_definitions(ubq-sync-transport-main-object PRIVATE main=ubiquittyHostMain)
        add_executable(sync_transport_acceptance EXCLUDE_FROM_ALL
            ${UBIQUITTY_CORE_SOURCE_DIR}/Tests/Host/sync_transport_acceptance.cpp
            $<TARGET_OBJECTS:ubq-sync-transport-host-objects>
            $<TARGET_OBJECTS:ubq-sync-transport-main-object>)
        foreach(_ubq_transport_target IN ITEMS
                ubq-sync-transport-host-objects ubq-sync-transport-main-object sync_transport_acceptance)
            target_include_directories(${_ubq_transport_target} PRIVATE
                "$<TARGET_PROPERTY:ubiquitty-host,INCLUDE_DIRECTORIES>" "${UBIQUITTY_CORE_SOURCE_DIR}/Tests")
            target_include_directories(${_ubq_transport_target} SYSTEM PRIVATE
                "${UBIQUITTY_SIMDUTF_DIR}" "${UBIQUITTY_RE2_DIR}")
            target_compile_definitions(${_ubq_transport_target} PRIVATE
                "$<TARGET_PROPERTY:ubiquitty-host,COMPILE_DEFINITIONS>")
            target_compile_options(${_ubq_transport_target} PRIVATE
                "$<TARGET_PROPERTY:ubiquitty-host,COMPILE_OPTIONS>")
            target_compile_features(${_ubq_transport_target} PRIVATE cxx_std_23)
            set_target_properties(${_ubq_transport_target} PROPERTIES
                CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
        endforeach()
        find_package(Threads REQUIRED)
        target_link_libraries(sync_transport_acceptance PRIVATE
            "$<TARGET_PROPERTY:ubiquitty-host,LINK_LIBRARIES>" Threads::Threads)
    endif()

# ----------------------------------------------------------------------
# Agent — ubq, the headless terminal frontend for AI agents
# (see Source/Agent/README.md).
#
# A portable-POSIX binary (no Cocoa / Metal / Swift): a Sync
# client that attaches to the ubiquitty-host daemon's host.sock, applies
# SNAPSHOTs into a passive Core Terminal, and exposes it programmatically
# (Source/Agent). It runs no PTY and no parser, so it drops the daemon's
# forkpty / Daemon sources; it shares the same Core + Sync + Preferences +
# HostPaths/PAL link set the host uses, minus the PTY transport.
# ----------------------------------------------------------------------
elseif(UBIQUITTY_FRONTEND STREQUAL "Agent")

    # The agent's Core link set = the canonical portable-Core list minus the
    # TUs a headless Sync client never references. It runs no PTY and no parser
    # (it applies SNAPSHOTs into a passive Terminal), so it drops the input encoders,
    # selection, the process model, session-persistence, the Tek/box
    # decorators + font-spec, the QUIC client carrier, and the
    # local-socket/poll/subprocess transports. It keeps the workspace layout
    # model, because the Sync client it is mirrors the workspace it attaches to. It KEEPS the software renderer
    # (Unifont/SoftwareRenderer/CellInstanceBuilder) for the screenshot raster.
    # A Core .cpp added to the SSOT flows in here unless it joins this trim list.
    ubiquitty_core_subset(UBIQUITTY_AGENT_CORE ABSOLUTE EXCLUDE
        # App-level theme/appearance/config-reload policy, which drives a host
        # seam a GUI frontend implements. A headless arm follows no system
        # appearance and watches no config file, and keeping it drags the whole
        # Themes resolution in behind it.
        Source/Core/App/AppRuntime.cpp
        # The first-launch Welcome screen and the shell wrapper one of its
        # buttons installs. A headless arm shows no screen and installs nothing
        # into a reader's startup files.
        Source/Core/App/ShellIntegrationInstall.cpp
        Source/Core/App/WelcomeScreen.cpp
        # The theme list and a theme's colours, for a frontend's picker. A
        # headless arm shows none, and keeping it drags Themes resolution in
        # behind it, exactly as AppRuntime above does.
        Source/Core/Themes/ThemeApply.cpp
        # The copy decision arrived in Core on 2026-09-05 and flowed in here as
        # a new Core .cpp does. A headless agent has no clipboard to copy to,
        # and keeping it drags the theme palette, the effective preferences and
        # Unicode normalisation in behind it. TerminalSelection.cpp goes with
        # it — it now builds a styled clipboard, which is the same cut, and the
        # daemon's subset has trimmed it since it was written.
        Source/Core/Selection/CopyClipboard.cpp
        Source/Core/Selection/TerminalSelection.cpp
        Source/Core/Selection/SelectionOverCapture.cpp
        Source/Core/Preferences/PreferencesAccessors.cpp
        # The frontends' control socket reads its switch through the accessors
        # trimmed above, and a headless agent serves no windows to drive.
        Source/Core/Protocol/Control/Server.cpp
        Source/Core/Preferences/EffectiveValue.cpp
        # The app's own config writes, marked so its file watcher can tell
        # them from a person's edit. It writes through EffectiveValue.cpp,
        # trimmed above, and only AppRuntime.cpp, trimmed with it, asks; a
        # headless agent writes no configuration and watches no file.
        Source/Core/Preferences/SelfWriteGuard.cpp
        # The renderer-configuration fold reads the accessors trimmed above;
        # the agent's screenshot raster is configured from its own arguments.
        Source/Core/Preferences/FontRoutePreferences.cpp
        Source/Core/Preferences/RenderConfigurationFold.cpp
        # The overlay spans a GUI pane paints over its grid; a headless
        # screenshot raster draws the grid alone.
        Source/Core/Rendering/OverlaySpanGather.cpp
        # LinkRun.cpp is kept. The agent's screenshot raster runs the frame
        # capture, whose TerminalFrameInputs.cpp calls kindForLinkID on every
        # path; this list is matched by name and knows nothing of who calls
        # whom, so cutting it would pass here and fail at link time on each arm
        # whose linker keeps the caller. The pointer-hover walk that follows
        # has no caller left in the set and is cut.
        Source/Core/Screen/VisibleLink.cpp
        # Panes/PaneTree.cpp and Session/LayoutTree.cpp STAY: the Sync client
        # mirrors the workspace it attaches to (RemoteSessionCore.cpp builds a
        # Layout::Workspace; WorkspaceWire.cpp and Protocol/Tmux/PaneLayout.cpp
        # walk its pane trees), and a headless client attaches over Sync like
        # any other. Trimming either leaves a kept file needing a definition
        # only the trimmed one has, which check-core-subset-symbols refuses.
        Source/PAL/Process.cpp
        Source/PAL/ProcessSignal.cpp
        Source/Core/Session/ProcessSubtree.cpp
        # The matcher goes, with the RE2 and normalisation it needs, and the
        # search built on it: the agent runs no search.
        # Screen/ScrollSearchResults.cpp STAYS: the frame capture kept for the
        # screenshot raster reads the active match through FrameDamage.cpp to
        # damage its highlight, and those readers live there rather than
        # beside the matcher for this cut.
        Source/Core/Screen/ScrollSearch.cpp
        Source/Core/Search/HistoryScan.cpp
        Source/Core/Search/SearchController.cpp
        Source/Core/Search/ScreenScan.cpp
        Source/Core/Search/SearchExecutor.cpp
        Source/Core/Screen/ScrollTuning.cpp
        Source/Core/Session/SessionInfo.cpp
        # The drawing-debug readout is a GUI overlay; a headless agent shows
        # none. The heatmap's counters stay: the frame capture kept here feeds
        # them.
        Source/Core/Diagnostics/DrawingDebug.cpp
        Source/Core/Input/ComposeEngine.cpp
        Source/Core/Input/Keybindings.cpp
        # VT.cpp uses the key encoders for XTGETTCAP replies.
        # The routing ladder: which mode a press, a drag or a wheel notch turns
        # into. A headless arm routes no pointer. PointerInput.cpp beside it
        # stays, because it holds more than the encoders — the kept
        # TerminalSelection.cpp calls viewportAfterOutput and
        # isSnapPolicyConsulted, declared in PointerInput.h, to decide where a
        # view sits after output arrives.
        Source/Core/Input/PointerLadder.cpp
        Source/Core/Protocol/Sync/QUICBootstrap.cpp
        Source/Core/Protocol/Sync/QUICBootstrapReader.cpp
        # The host's half of a Sync call: it serves a client's request, a
        # history search among them, against the pane it owns. The agent is a
        # client that sends calls and serves none, and the search it would
        # answer with lives in ScrollSearch.cpp, trimmed above.
        Source/Core/Protocol/Sync/Control.cpp
        # The host's encoder for a remote client's keys and pointer: it turns
        # a client's input into bytes for the pane's program, through the
        # keymap trimmed above. Only the daemon builds one; the agent sends
        # its input to the host and encodes none of it.
        Source/Core/Runtime/RemoteInput.cpp
        Source/Core/Protocol/Tmux/Commands.cpp
        Source/Core/Protocol/Tmux/Session.cpp
        Source/Core/Runtime/TmuxControlHost.cpp
        Source/Core/Protocol/Tmux/Tiler.cpp
        Source/Core/Rendering/BoxEdgeStretch.cpp
        Source/Core/Rendering/BoxGlyphRaster.cpp
        Source/Core/Rendering/BoxStrokeProfile.cpp
        Source/Core/Rendering/FontFeatureSpecification.cpp
        Source/Core/Rendering/FontVariationSpecification.cpp
        # ShapedCellCache.cpp and FontMetrics.cpp are not on this list:
        # LigatureSheet.cpp, kept here, answers a shaped run by identity
        # through the one and places a proportional face's pen through the
        # other.
        Source/Core/Rendering/LigatureClassification.cpp
        Source/Core/Rendering/RenderPlan.cpp
        Source/Core/Rendering/TerminalSnapshot.cpp
        Source/Core/Rendering/BundledRichText.cpp
        Source/Core/Runtime/LaunchCommand.cpp
        Source/Core/Runtime/LaunchArguments.cpp
        Source/Core/Runtime/TerminalRuntime.cpp
        Source/Core/Runtime/TriggerConfig.cpp
        # Reads the two [triggers] gates through PreferencesAccessors, trimmed
        # above; a headless agent runs no trigger engine to resolve.
        Source/Core/Runtime/TriggerEffect.cpp
        # Selection.cpp stays, for the reason the host subset states:
        # Core/Screen/ClearBuffer.cpp is in the SSOT and calls Selection::clear,
        # so excluding it drops a definition a kept translation unit needs.
        Source/Core/Selection/StyledClipboard.cpp
        Source/Core/Session/HostLayoutRecord.cpp
        Source/Core/Session/Surface.cpp
        # Workspace.cpp stays: RemoteSessionCore serializes shared-window calls.
        Source/Core/Session/WorkspaceCapture.cpp
        Source/Core/Terminfo/UbiquittyTerminfo.cpp
        Source/Core/Accessibility/Palette.cpp
        # CellColor.cpp STAYS: the screenshot raster's frame inputs
        # (TerminalFrameInputs.cpp, kept for that raster) resolve each cell's
        # colours through CellPalette, which it defines.
        Source/Core/Themes/CellPaletteFromTheme.cpp
        Source/Core/Themes/TermPaletteFromTheme.cpp
        Source/Core/Themes/ThemeImport.cpp
        Source/Core/Themes/Themes.cpp
        Source/Core/Themes/ThemesCustom.cpp
        Source/Core/Transport/LocalSocketChannel.cpp
        # LocalPTYChannel derives from PollChannel, so it must be trimmed
        # wherever its base is — otherwise its vtable, ctor and dtor have no
        # definition and the link fails (Release hides it by eliding the
        # unused vtable; Debug does not).
        Source/Core/Transport/DelegatedChannel.cpp
        Source/Core/Transport/KnownHosts.cpp
        Source/Core/Transport/LocalPTYChannel.cpp
        Source/Core/Unicode/Normalize.cpp)

    add_executable(ubq
        # Agent frontend: the headless main + the MCP/policy/session glue + JSON,
        # and the host PAL/paths it shares with the daemon (none in the SSOT).
        ${UBIQUITTY_CORE_SOURCE_DIR}/Source/Agent/main.cpp
        ${UBIQUITTY_CORE_SOURCE_DIR}/Source/Agent/AgentSession.cpp
        ${UBIQUITTY_CORE_SOURCE_DIR}/Source/Agent/AgentMCP.cpp
        ${UBIQUITTY_CORE_SOURCE_DIR}/Source/Agent/RemoteCarrier.cpp
        ${UBIQUITTY_CORE_SOURCE_DIR}/Source/Core/Codec/JSON.cpp
        ${UBIQUITTY_YYJSON_SOURCE}
        ${UBIQUITTY_CORE_SOURCE_DIR}/Source/Platform/Host/HostPaths.cpp
        ${UBIQUITTY_CORE_SOURCE_DIR}/Source/Platform/Host/HostPAL.cpp
        # Portable Core (single-sourced from Scripts/portable-core-sources.txt;
        # ControlPaths + NativeAttachment/SyncClientCore + Tmux.cpp arrive via SSOT).
        ${UBIQUITTY_AGENT_CORE}
        ${UBIQUITTY_SIMDUTF_SOURCES}
    )
    ubiquitty_generate_completions_header(
        "${CMAKE_CURRENT_BINARY_DIR}/generated/Agent/CompletionScripts.h")
    target_include_directories(ubq PRIVATE "${UBIQUITTY_CORE_SOURCE_DIR}/Source"
        "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_include_directories(ubq SYSTEM PRIVATE "${UBIQUITTY_SIMDUTF_DIR}")
    target_compile_features(ubq PRIVATE cxx_std_23)
    set_target_properties(ubq PROPERTIES
        CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    target_compile_options(ubq PRIVATE
        -fno-exceptions -fno-rtti -Wall -Wextra
        ${UBIQUITTY_NARROWING_WARNING}
        ${UBIQUITTY_OWNERSHIP_WARNINGS}
        "$<$<BOOL:${UBIQUITTY_WERROR}>:-Werror>")

# ----------------------------------------------------------------------
# Core — the standalone libUbiquittyCore static library + its consumer
# sample. The portable terminal Core as a real,
# reusable build artifact: no frontend, no Apple/GTK/Vulkan, no external
# transport stack. The library links against nothing but Source/ + a
# caller-supplied PAL. The source set is the SINGLE canonical list in
# Scripts/portable-core-sources.txt, read by UbiquittyCoreSources.cmake and by
# the bash perf drivers so the list can never drift.
#
#   cmake -S . -B build/core -DUBIQUITTY_FRONTEND=Core -DUBIQUITTY_QUIC=OFF
#   cmake --build build/core
#
# Public umbrella header: Source/UbiquittyCore/UbiquittyCore.h. Curated public surface
# (Terminal lifecycle + processBytes, snapshot/getDisplayRow, the paint model,
# Preferences, the PAL contract). See Docs/libubiquittycore.md.
# ----------------------------------------------------------------------
elseif(UBIQUITTY_FRONTEND STREQUAL "Core")

    # The standalone lib links the FULL canonical portable-Core set (the same
    # list the host/agent/linux frontends trim from), as the layer libraries
    # every consumer of Core links.
    ubiquitty_add_core_layer_libraries()

    # Standalone consumer sample: links ubiquitty-core against an in-file STUB
    # PAL (the system-services half the bundled POSIX backend does not supply),
    # feeds bytes into a Terminal, reads a snapshot row, prints it. PROVES reuse
    # with no frontend.
    add_executable(core-consumer ${UBIQUITTY_CORE_SOURCE_DIR}/Samples/core-consumer/main.cpp)
    target_link_libraries(core-consumer PRIVATE ubiquitty-core)
    target_compile_features(core-consumer PRIVATE cxx_std_23)
    set_target_properties(core-consumer PROPERTIES
        CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    target_compile_options(core-consumer PRIVATE
        -fno-exceptions -fno-rtti -Wall -Wextra
        ${UBIQUITTY_NARROWING_WARNING}
        ${UBIQUITTY_OWNERSHIP_WARNINGS}
        "$<$<BOOL:${UBIQUITTY_WERROR}>:-Werror>")

    # Cross-platform golden-image producer. It links the same installable Core
    # and always renders through the embedded GNU Unifont software backend.
    add_executable(ubiquitty-snapshot ${UBIQUITTY_CORE_SOURCE_DIR}/Tools/snapshot/main.cpp)
    target_link_libraries(ubiquitty-snapshot PRIVATE ubiquitty-core)
    target_compile_features(ubiquitty-snapshot PRIVATE cxx_std_23)
    set_target_properties(ubiquitty-snapshot PROPERTIES
        CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    target_compile_options(ubiquitty-snapshot PRIVATE
        -fno-exceptions -fno-rtti -Wall -Wextra
        ${UBIQUITTY_NARROWING_WARNING}
        ${UBIQUITTY_OWNERSHIP_WARNINGS}
        "$<$<BOOL:${UBIQUITTY_WERROR}>:-Werror>")
    # forkpty lives in libutil on Linux/glibc (PTYStream is in the lib via the
    # POSIX PAL backend's transitive surface); a no-op on macOS.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_libraries(core-consumer PRIVATE util)
    endif()
    # wasm32: the Core's maxColumns-sized stack buffers (rowBuffer[maxColumns])
    # overflow emscripten's 64 KB default stack — grow it and allow heap growth
    # for the Terminal's heap model. The result runs under node; no source impact.
    # Stack first, so an overflow past even this traps rather than rewriting
    # the static data an optimised link places below it.
    if(EMSCRIPTEN)
        target_link_options(core-consumer PRIVATE
            -sSTACK_SIZE=8388608 -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=33554432
            -Wl,--stack-first)
    endif()

    # The package find_package(UbiquittyCore) reads, with its licence notices.
    ubiquitty_install_core_package()


else()

    message(FATAL_ERROR
        "UBIQUITTY_FRONTEND='${UBIQUITTY_FRONTEND}' is not recognised. "
        "Valid values: Core, Host, Agent.")

endif()

# ----------------------------------------------------------------------
# The host tests. Tests/CMakeLists.txt finds them on disk; nothing here or there
# keeps a list of them.
# ----------------------------------------------------------------------
if(UBIQUITTY_BUILD_TESTS AND NOT TARGET ubiquitty-core)
    message(FATAL_ERROR
        "UBIQUITTY_BUILD_TESTS links every test against ubiquitty-core, which only "
        "the Core frontend defines; configure with -DUBIQUITTY_FRONTEND=Core.")
elseif(UBIQUITTY_BUILD_TESTS)
    enable_testing()
    add_subdirectory("${UBIQUITTY_CORE_SOURCE_DIR}/Tests" "${CMAKE_CURRENT_BINARY_DIR}/Tests")
endif()

# Last, once every target of every arm exists.
ubiquitty_order_after_build_version("${CMAKE_CURRENT_SOURCE_DIR}")
