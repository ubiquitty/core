# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

#
# Tests/CMakeLists.txt — finds the host tests, builds them, and registers them
# with CTest.
#
# A test is added by adding its file. Any `test_*.cpp` under Tests/<Subsystem>/
# is found by the glob further down, compiled into an executable of its own and
# run by `ctest`; no list names it, because a source list kept by hand falls
# out of step with the files on disk and nothing reports the drift.
#
# Scripts/cmake/UbiquittyTests.cmake compiles and registers them. This file
# says what each test needs: the boundary tests and the link line each is
# allowed, every other test's extra sources, definitions and libraries, and
# which tests this host leaves out. Those lines are written out here, beside
# the lists, where a reader can see exactly what each test is allowed to reach.
#

include(${UBIQUITTY_CORE_SOURCE_DIR}/Scripts/cmake/UbiquittyTests.cmake)

# The boundary tests, which never link ubiquitty-core. Each compiles its own
# source and whatever its <test>_EXTRA_SOURCES grants it, and nothing more.
set(UBIQUITTY_BOUNDARY_TESTS
    test_types           # Base/Types.h on the bare standard library, no platform header
    test_owned_child     # mocked process ownership with no Core or process creation
    test_local_control   # tagged local control codec and authority with no Core or I/O link
    test_local_control_client # bounded reload exchange with mocked transport and clock
    test_local_quic_control # fixed local QUIC codec and injected authority/mint values only
    # The aggregate's SystemPAL.cpp is built without the test hook, which leaves
    # the symlink seam dead: linked against it, this test would check a
    # situation it never managed to arrange. It compiles its own hooked copy.
    test_atomic_storage_perms
)
set(test_atomic_storage_perms_EXTRA_SOURCES Source/Platform/POSIX/SystemPAL.cpp)
set(test_session_handoff_deadline_EXTRA_SOURCES Source/Platform/POSIX/SessionHandoff.cpp)
set(test_handover_transport_EXTRA_SOURCES Source/Platform/POSIX/HandoverTransport.cpp)
set(test_session_reclaim_EXTRA_SOURCES Source/Platform/POSIX/SessionHandoff.cpp)
foreach(_hooked_test test_atomic_storage_perms test_bytebuffer test_channel)
    set(${_hooked_test}_EXTRA_DEFINES UBIQUITTY_ENABLE_TEST_HOOKS=1)
endforeach()
set(test_channel_EXTRA_SOURCES Source/Core/Transport/PollChannel.cpp)
set(test_datagram_batch_EXTRA_SOURCES Source/Platform/POSIX/DatagramBatch.cpp)
# Named rather than taken from the aggregate: the coverage build instruments
# the sources a test target compiles, so a TU reached only through the
# library reports as having no coverage at all.
set(test_tool_invocation_EXTRA_SOURCES Source/Core/Clipboard/ToolInvocation.cpp)
set(test_clipboard_serve_EXTRA_SOURCES Source/Core/Session/ClipboardServe.cpp)
set(test_boundedsocketserver_EXTRA_DEFINES UBIQUITTY_ENABLE_TEST_HOOKS=1)
set(test_boundedsocketserver_EXTRA_SOURCES Source/Core/Session/BoundedSocketServer.cpp)
set(test_accessibility_palette_EXTRA_DEFINES UBIQUITTY_ENABLE_TEST_HOOKS=1)
# The seam lives in the implementation, so the define has to reach it: the
# archive's copy is compiled without hooks and carries no ForTest symbols.
set(test_accessibility_palette_EXTRA_SOURCES Source/Core/Accessibility/Palette.cpp)

# Exceptions. A test lands here only when it exercises a TU that
# Scripts/portable-core-sources.txt excludes — a platform backend or an optional
# codec the portable set does not carry — so the aggregate cannot supply it.
# The entries stay few on purpose. A list here that kept growing would say the
# portable source set is drawn in the wrong place, and the fix would belong in
# that set rather than in one more line below.
#
# Each of these needs a symbol the portable Core does not carry, and names the
# file that defines it; that link line is all the test lacks.
set(test_controlpaths_EXTRA_SOURCES    Source/Platform/Host/HostPaths.cpp)
set(test_attach_relay_EXTRA_SOURCES    Source/Platform/Host/AttachRelay.cpp)
set(test_handover_effects_EXTRA_SOURCES Source/Platform/Host/HandoverEffects.cpp)
set(test_handover_authority_tables_EXTRA_SOURCES Source/Platform/Host/HandoverAuthorityTables.cpp)
set(test_handover_global_tables_EXTRA_SOURCES Source/Platform/Host/HandoverGlobalTables.cpp)
set(test_handover_runtime_EXTRA_SOURCES Source/Platform/Host/HandoverRuntime.cpp)
set(test_handover_tables_EXTRA_SOURCES Source/Platform/Host/HandoverTables.cpp)
set(test_handover_workspace_tables_EXTRA_SOURCES Source/Platform/Host/HandoverWorkspaceTables.cpp)
# A private socket pair drives the real agent reader; no host process or user
# configuration participates in its request-lifetime tests.
set(test_agent_capture_EXTRA_SOURCES Source/Agent/AgentSession.cpp
                                    Source/Agent/RemoteCarrier.cpp
                                    Source/Platform/Host/HostPaths.cpp)
# The preferences backend is bundled now, so a target needing it names one file
# on every host rather than an Objective-C++ one here and a C++ one there — and
# no target needs Foundation for it any more.
set(test_themes_EXTRA_SOURCES          Source/Platform/POSIX/Preferences.cpp
                                       Source/Platform/POSIX/SystemPAL.cpp)
set(test_cfauto_EXTRA_LIBS "-framework CoreFoundation")
# The ImageIO decoder the macOS app installs, against PNGs the test deflates
# itself through the system zlib.
set(test_image_decode_EXTRA_SOURCES Source/Platform/Apple/ImageDecode.cpp)
set(test_image_decode_EXTRA_LIBS "-framework CoreFoundation" "-framework CoreGraphics" "-framework ImageIO" z)
# The daemon subset compiles HandoverAdmission.cpp, whose Apple half verifies a
# peer's code signature through Security and CoreFoundation.
if(APPLE)
    set(test_daemon_session_configuration_EXTRA_LIBS "-framework CoreFoundation" "-framework Security")
endif()
# These compile on this host — what they need is a link line, not a
# toolchain. Each names the file that defines the symbols its test calls; none
# of them needs GTK. The preferences backend is bundled, and this is the same
# file every other target here names.
set(test_preferences_backend_EXTRA_SOURCES    Source/Platform/POSIX/Preferences.cpp
                                              Source/Platform/POSIX/SystemPAL.cpp)
set(test_pty_EXTRA_SOURCES             Source/Platform/POSIX/PTYStream.cpp
                                       Source/Platform/POSIX/PTYFacts.cpp)
set(test_appruntime_EXTRA_SOURCES      Source/Core/App/AppRuntime.cpp)
set(test_welcome_screen_EXTRA_SOURCES   Source/Core/App/WelcomeScreen.cpp)
set(test_shell_integration_install_EXTRA_SOURCES Source/Core/App/ShellIntegrationInstall.cpp)
# The gated kitty file read and its verified unlink live in Platform/POSIX so no
# single arm owns the hardening rules. The portable Core does not carry them, so
# each test over them names the TU.
set(test_gated_file_read_EXTRA_SOURCES Source/Platform/POSIX/GatedFileRead.cpp
                                       Source/Platform/POSIX/SystemPAL.cpp)
# The model lock's handoff spins on PAL::monotonicMicros and yields through
# PAL::schedulerYield, so these need the real clock: a fake that advances a
# microsecond per call turns the handoff band into an iteration count.
set(test_model_lock_EXTRA_SOURCES Source/Platform/POSIX/SystemPAL.cpp Tests/Core/ModelLockArchiveCopy.cpp)
# The claim case releases only once both its waiters are counted, which it
# reads through the lock's test hook.
set(test_model_lock_EXTRA_DEFINES UBIQUITTY_ENABLE_TEST_HOOKS=1)
# Built as a frontend's Core archive is, optimised and with NDEBUG, beside the
# test's own translation unit, which keeps assertions as a frontend's C++
# interop does: the lock's holder must not depend on which copy took it.
set_source_files_properties(${UBIQUITTY_CORE_SOURCE_DIR}/Tests/Core/ModelLockArchiveCopy.cpp
    PROPERTIES COMPILE_OPTIONS "-O2;-DNDEBUG" SKIP_PRECOMPILE_HEADERS ON)
set(test_drain_host_pacing_EXTRA_SOURCES Source/Platform/POSIX/SystemPAL.cpp)
# The tmux delivery path releases each pane's lock through ModelLock's handoff,
# which reads the PAL clock and yields through the PAL.
set(test_tmux_control_host_EXTRA_SOURCES Source/Platform/POSIX/SystemPAL.cpp
                                         Source/Platform/Host/HostPAL.cpp)
# The libFuzzer harness, linked into an ordinary test so its corpus can be
# replayed. LLVMFuzzerTestOneInput is a plain function; only the driver needs
# -fsanitize=fuzzer, and the replay supplies its own driver by reading files.
set(test_sync_corpus_replay_EXTRA_SOURCES Tests/Core/Protocol/Sync/fuzz_sync.cpp)
# The absolute source root, which only this side still has: the compiler cache
# rewrites source paths on the command line so builds at different checkouts
# share one cache, and anything the test derives from __FILE__ goes relative
# with them.
set(test_sync_corpus_replay_EXTRA_DEFINES
    UBQ_SYNC_FUZZ_CORPUS_DIR="${UBIQUITTY_CORE_SOURCE_DIR}/Tests/Core/Protocol/Sync/fuzz-corpus")
# The tmux Control Mode corpus, replayed the same way: fuzz_tmux.cpp is the
# harness, and the path is handed in for the same source-root reason as above.
set(test_tmux_corpus_replay_EXTRA_SOURCES Tests/Core/Protocol/Tmux/fuzz_tmux.cpp)
set(test_tmux_corpus_replay_EXTRA_DEFINES
    UBQ_TMUX_FUZZ_CORPUS_DIR="${UBIQUITTY_CORE_SOURCE_DIR}/Tests/Core/Protocol/Tmux/fuzz-corpus")
# The RTF corpus, replayed through fuzz_richtext.cpp, which checks the
# document invariants on every body the reader accepts.
set(test_richtext_corpus_replay_EXTRA_SOURCES Tests/Core/RichText/fuzz_richtext.cpp)
set(test_richtext_corpus_replay_EXTRA_DEFINES
    UBQ_RICHTEXT_CORPUS_DIR="${UBIQUITTY_CORE_SOURCE_DIR}/Tests/Corpus/richtext")
# The measurements the session layer's constants are read against.
set(test_sync_measured_policy_EXTRA_DEFINES
    UBQ_MEASUREMENTS_DOC="${UBIQUITTY_CORE_SOURCE_DIR}/Docs/sync-measurements.md")
set(test_sync_memory_owner_EXTRA_DEFINES
    UBQ_MEASUREMENTS_DOC="${UBIQUITTY_CORE_SOURCE_DIR}/Docs/sync-measurements.md")
# The Sync PTY corpus (Tests/Corpus/sync), read by Tests/SyncCorpus.h; the path
# is handed in for the same source-root reason as above.
set(test_sync_pty_corpus_EXTRA_DEFINES
    UBQ_SYNC_PTY_CORPUS_DIR="${UBIQUITTY_CORE_SOURCE_DIR}/Tests/Corpus/sync")
set(test_sync_corpus_oracle_EXTRA_DEFINES
    UBQ_SYNC_PTY_CORPUS_DIR="${UBIQUITTY_CORE_SOURCE_DIR}/Tests/Corpus/sync")
set(test_sync_corpus_EXTRA_DEFINES
    UBQ_SYNC_PTY_CORPUS_DIR="${UBIQUITTY_CORE_SOURCE_DIR}/Tests/Corpus/sync")
set(test_sync_shaped_link_EXTRA_DEFINES
    UBQ_SYNC_PTY_CORPUS_DIR="${UBIQUITTY_CORE_SOURCE_DIR}/Tests/Corpus/sync")
# The frozen handover pane record lives in the handover fuzzer's corpus.
set(test_sync_handover_body_EXTRA_DEFINES
    UBQ_HANDOVER_FIXTURE_DIR="${UBIQUITTY_CORE_SOURCE_DIR}/Tests/Corpus/sync/fuzz_sync_handover")
set(test_sync_handover_resume_EXTRA_DEFINES
    UBQ_SYNC_PTY_CORPUS_DIR="${UBIQUITTY_CORE_SOURCE_DIR}/Tests/Corpus/sync")
set(test_sync_connection_EXTRA_DEFINES
    UBQ_SYNC_PTY_CORPUS_DIR="${UBIQUITTY_CORE_SOURCE_DIR}/Tests/Corpus/sync")
# The UAX #29 grapheme conformance corpus, vendored beside the test. UCD inputs
# for table generation are fetched per build-grapheme-tables.py rather than
# checked in; a test fixture is different in kind — one that must be present for
# the test to execute cannot be an optional download, or the check never runs.
# Same source-root reason as the sync corpus above: the compiler cache rewrites
# __FILE__, so the path is handed in by the build system.
set(test_grapheme_conformance_EXTRA_DEFINES
    GRAPHEME_BREAK_TEST_CORPUS="${UBIQUITTY_CORE_SOURCE_DIR}/Tests/Core/Unicode/GraphemeBreakTest.txt")
# The UAX #9 bidi conformance corpus, on the same terms and for the same reason.
# It is 6.6 MB against the grapheme corpus's 124 KB, which is a real weight
# decision rather than a rounding error: vendored whole because a sampled
# conformance corpus invites "which cases did you drop", and a number nobody can
# reproduce from the tree is the kind of claim this suite exists to refuse. It
# changes once per Unicode version and compresses to ~0.4 MB in the object store.
set(test_bidi_conformance_EXTRA_DEFINES
    BIDI_CHARACTER_TEST_CORPUS="${UBIQUITTY_CORE_SOURCE_DIR}/Tests/Core/Unicode/BidiCharacterTest.txt")
# The Emoji_Presentation authority (emoji-data.txt), vendored beside the test.
# build-unicode-tables.py emits that property from this same file, so the check
# holds the generated table against the authority — an emit bug or a version
# skew between the two surfaces here. Path handed in by the build, as above.
# build-grapheme-tables.py reads the same file for Extended_Pictographic, so
# the two generators want one UCD drop between them: a skew puts the width
# tables and the GB11 rule on different Unicode versions, which neither
# generator can see from its own side.
set(test_emoji_presentation_conformance_EXTRA_DEFINES
    EMOJI_DATA_FILE="${UBIQUITTY_CORE_SOURCE_DIR}/Tests/Core/Unicode/emoji-data.txt")
# The PAL clock conformance check links a frontend's PAL and asserts its clock
# runs at the rate PAL.h promises. Pointed at the host daemon's, which is the
# PAL this repository ships; any PAL that compiles natively can be swapped in.
set(test_clock_conformance_EXTRA_SOURCES Source/Platform/Host/HostPAL.cpp)
# The determinism pin is one backend's, not a contract every PAL answers:
# UBIQUITTY_FAKE_TICKS exists in the macOS clock alone. Naming a source under
# Platform/Apple is also what keeps a Linux configure from building this one —
# off an Apple host, ubiquitty_add_host_tests drops any target whose extra
# sources live there, whatever their extension: Clock.cpp is C++ over
# mach/mach_time.h.
set(test_clock_pin_EXTRA_SOURCES       Source/Platform/Apple/Clock.cpp)

set(test_serial_EXTRA_SOURCES          Source/Platform/POSIX/SerialStream.cpp)
# PTYFacts carries gatherSubtree / firstChildProcess / stopProcessTracker, which
# PTYStream references and the portable Core does not define. Naming it here is
# what lets this test build as a CMake target instead of a shell stanza.
# The Terminal -> ModeSnapshot sampler, so the key seam can be exercised against
# a live Terminal — the overload the web, wscons and GTK arms call.
set(test_key_encode_EXTRA_SOURCES      Source/Core/Runtime/ModeSnapshot.cpp)
# A live TerminalSession, so it needs the PTY-backed stack the session tests
# use: the runtime, the PTY pair, and the PAL the clock and spawn resolve to.
set(test_palette_apply_EXTRA_SOURCES   Source/Core/Runtime/TerminalRuntime.cpp
                                       Source/Platform/POSIX/PTYStream.cpp
                                       Source/Platform/POSIX/PTYFacts.cpp
                                       Source/Platform/POSIX/Poll.cpp
                                       Source/Platform/POSIX/SystemPAL.cpp
                                       Source/Platform/POSIX/ProcessSpawnAndWait.cpp
                                       Source/Platform/Host/HostPAL.cpp
                                       Source/Core/Session/ProcessSubtree.cpp)
# Stands a real TerminalSession, so it needs the same PTY-backed stack
# test_palette_apply does — including the PAL the session's clock resolves to,
# which the session reaches whether or not a case ever launches a shell.
set(test_attach_over_carrier_EXTRA_SOURCES Source/Core/Runtime/TerminalRuntime.cpp
                                       Source/Platform/POSIX/PTYStream.cpp
                                       Source/Platform/POSIX/PTYFacts.cpp
                                       Source/Platform/POSIX/Poll.cpp
                                       Source/Platform/POSIX/SystemPAL.cpp
                                       Source/Platform/POSIX/ProcessSpawnAndWait.cpp
                                       Source/Platform/Host/HostPAL.cpp
                                       Source/Core/Session/ProcessSubtree.cpp)
# It drives a session's withdrawal of a held effect with no change of state
# after it, which only the test hooks reach.
set(test_attach_over_carrier_EXTRA_DEFINES UBIQUITTY_ENABLE_TEST_HOOKS=1)
# The directory client's test doors are compiled only under the hook, so the
# target that drives them owns that translation unit as it owns the runtime's.
set(test_runtime_EXTRA_SOURCES         Source/Core/Runtime/TerminalRuntime.cpp
                                       Source/Core/Runtime/HostDirectoryClient.cpp
                                       Source/Platform/POSIX/PTYStream.cpp
                                       Source/Platform/POSIX/PTYFacts.cpp
                                       Source/Core/Session/ProcessSubtree.cpp)
# The copy/capture counters are compiled out of the shared Core archives. These
# targets own their implementation translation unit so the test-hook layout and
# the code updating it are built under the same definition.
set(test_runtime_EXTRA_DEFINES         UBIQUITTY_ENABLE_TEST_HOOKS=1)
# A TerminalSession reaches the PAL for its clock whether or not a case ever
# launches a shell, so this carries what test_attach_over_carrier carries.
set(test_window_request_queue_EXTRA_SOURCES Source/Core/Runtime/TerminalRuntime.cpp
                                       Source/Platform/POSIX/PTYStream.cpp
                                       Source/Platform/POSIX/PTYFacts.cpp
                                       Source/Platform/POSIX/Poll.cpp
                                       Source/Platform/POSIX/SystemPAL.cpp
                                       Source/Platform/POSIX/ProcessSpawnAndWait.cpp
                                       Source/Platform/Host/HostPAL.cpp
                                       Source/Core/Session/ProcessSubtree.cpp)
set(test_session_key_modes_EXTRA_SOURCES ${test_window_request_queue_EXTRA_SOURCES})
# The in-process link is a real TerminalSession's, so it links the same set.
set(test_pane_link_conformance_EXTRA_SOURCES ${test_window_request_queue_EXTRA_SOURCES})
# The notification sink stands the same real TerminalSession the window-request
# queue does, so it links the same PTY-backed PAL set.
set(test_notification_sink_EXTRA_SOURCES ${test_window_request_queue_EXTRA_SOURCES})
# The trigger queue stands the same real TerminalSession, for the same reason.
set(test_trigger_effects_EXTRA_SOURCES ${test_window_request_queue_EXTRA_SOURCES})
# A share hands a live shell's PTY over and resumes it, so it stands the same
# real TerminalSession over a real PTY.
set(test_session_share_reclaim_EXTRA_SOURCES ${test_window_request_queue_EXTRA_SOURCES})
set(test_terminal_frame_inputs_EXTRA_SOURCES Source/Core/Rendering/TerminalFrameInputs.cpp)
set(test_terminal_frame_inputs_EXTRA_DEFINES UBIQUITTY_ENABLE_TEST_HOOKS=1)
# The rewrap's copy count, so the scaling case reads work rather than time.
set(test_reflow_scaling_EXTRA_SOURCES Source/Core/Screen/Reflow.cpp)
set(test_reflow_scaling_EXTRA_DEFINES UBIQUITTY_ENABLE_TEST_HOOKS=1)
set(test_frame_copy_statistics_disabled_EXTRA_SOURCES Source/Core/Rendering/TerminalFrameInputs.cpp)
set(test_frame_copy_statistics_disabled_EXTRA_DEFINES UBIQUITTY_PERF_DIAG=0)
# The ingest loop runs over a real PTY with the real clock, poll and waker —
# the daemon's PAL set, which is the same "portable POSIX, no GUI" shape.
set(test_ingest_loop_EXTRA_SOURCES     Source/Platform/POSIX/PTYStream.cpp
                                       Source/Platform/POSIX/PTYFacts.cpp
                                       Source/Platform/POSIX/Poll.cpp
                                       Source/Platform/POSIX/SystemPAL.cpp
                                       Source/Platform/Host/HostPAL.cpp
                                       Source/Core/Session/ProcessSubtree.cpp)
# test_flood_start runs the ingest loop over a pipe, on the real clock, poll and waker.
set(test_flood_start_EXTRA_SOURCES ${test_ingest_loop_EXTRA_SOURCES})
# The frame cadence runs a real drain and a frontend tick on the real clock.
set(test_frame_cadence_EXTRA_SOURCES ${test_ingest_loop_EXTRA_SOURCES} Tests/Platform/FrameCadence.cpp)
# Reads the model lock's acquisition count around each frame decision.
set(test_frame_cadence_EXTRA_DEFINES UBIQUITTY_ENABLE_TEST_HOOKS=1)
if(APPLE)
    # A service's inherited utility clamp still coalesces timed wakes after a
    # thread requests interactive QoS. The display probe needs a UI job's
    # process policy as well; its timing assertions remain the same.
    set(test_frame_cadence_LAUNCHER ${Python3_EXECUTABLE}
        ${UBIQUITTY_CORE_SOURCE_DIR}/Scripts/run-presentation-test.py)
endif()
# The settings fan-out runs against a started session, and a session is the
# runtime whether or not the case opens a PTY: the same PAL set has to link.
set(test_apply_terminal_preferences_EXTRA_SOURCES
                                       Source/Platform/POSIX/PTYStream.cpp
                                       Source/Platform/POSIX/PTYFacts.cpp
                                       Source/Platform/POSIX/Poll.cpp
                                       Source/Platform/POSIX/SystemPAL.cpp
                                       Source/Platform/Host/HostPAL.cpp
                                       Source/Core/Session/ProcessSubtree.cpp)
# The workspace model launches real pane sessions, so it needs the same
# PTY-backed stack the ingest-loop test does.
set(test_workspace_clients_EXTRA_SOURCES Source/Platform/POSIX/PTYStream.cpp
                                       Source/Platform/POSIX/PTYFacts.cpp
                                       Source/Platform/POSIX/Poll.cpp
                                       Source/Platform/POSIX/SystemPAL.cpp
                                       Source/Platform/Host/HostPAL.cpp
                                       Source/Core/Session/ProcessSubtree.cpp)
# Two models launch real pane sessions under two configurations, so it needs
# the same PTY-backed stack the workspace-clients test does.
# Three sessions in one model launch real children and answer for their own
# lifetimes, over the same PTY-backed stack.
set(test_session_exit_lifetime_EXTRA_SOURCES Source/Platform/POSIX/PTYStream.cpp
                                       Source/Platform/POSIX/PTYFacts.cpp
                                       Source/Platform/POSIX/Poll.cpp
                                       Source/Platform/POSIX/SystemPAL.cpp
                                       Source/Platform/Host/HostPAL.cpp
                                       Source/Core/Session/ProcessSubtree.cpp)
# A pane the app builds starts and publishes its model over the same stack.
set(test_workspace_session_start_EXTRA_SOURCES ${test_session_exit_lifetime_EXTRA_SOURCES})
# A launched pane's child reports the context it was given, over the same
# PTY-backed stack; PTYStream.cpp is where an inherited context is dropped.
set(test_launch_context_EXTRA_SOURCES Source/Platform/POSIX/PTYStream.cpp
                                       Source/Platform/POSIX/PTYFacts.cpp
                                       Source/Platform/POSIX/Poll.cpp
                                       Source/Platform/POSIX/SystemPAL.cpp
                                       Source/Platform/Host/HostPAL.cpp
                                       Source/Core/Session/ProcessSubtree.cpp)
set(test_session_configuration_EXTRA_SOURCES Source/Platform/POSIX/PTYStream.cpp
                                       Source/Platform/POSIX/PTYFacts.cpp
                                       Source/Platform/POSIX/Poll.cpp
                                       Source/Platform/POSIX/SystemPAL.cpp
                                       Source/Platform/Host/HostPAL.cpp
                                       Source/Core/Session/ProcessSubtree.cpp)
# A split window's captured layout is measured from its panes' live sessions,
# so the case launches them over the same PTY-backed stack.
set(test_workspace_layout_capture_EXTRA_SOURCES Source/Platform/POSIX/PTYStream.cpp
                                       Source/Platform/POSIX/PTYFacts.cpp
                                       Source/Platform/POSIX/Poll.cpp
                                       Source/Platform/POSIX/SystemPAL.cpp
                                       Source/Platform/Host/HostPAL.cpp
                                       Source/Core/Session/ProcessSubtree.cpp)
# The daemon creates real PTY sessions, so it links the host daemon's own
# objects over the same PTY-backed stack.
set(test_daemon_session_configuration_EXTRA_SOURCES
                                       Source/Platform/Host/Daemon.cpp
                                       Source/Platform/Host/HandoverAdmission.cpp
                                       # The reactor routes a successor's first record into the
                                       # handover transaction, which reaches every carried owner.
                                       Source/Platform/Host/HandoverStartup.cpp
                                       Source/Platform/Host/HandoverRequest.cpp
                                       Source/Platform/Host/HostHandover.cpp
                                       Source/Platform/Host/HostTableHandover.cpp
                                       Source/Platform/Host/HandoverTables.cpp
                                       Source/Platform/Host/HandoverWorkspaceTables.cpp
                                       Source/Platform/Host/HandoverAuthorityTables.cpp
                                       Source/Platform/Host/HandoverGlobalTables.cpp
                                       Source/Platform/Host/HandoverEffects.cpp
                                       Source/Platform/Host/HandoverRuntime.cpp
                                       Source/Platform/Host/SessionHandover.cpp
                                       Source/Platform/Host/SessionHandoverFacts.cpp
                                       Source/Platform/POSIX/HandoverLease.cpp
                                       Source/Platform/POSIX/HandoverTransport.cpp
                                       Source/Platform/Host/ClipboardTool.cpp
                                       Source/Platform/Host/HostPaths.cpp
                                       Source/Platform/Host/HostPAL.cpp
                                       Source/Platform/POSIX/PTYStream.cpp
                                       Source/Platform/POSIX/PTYFacts.cpp
                                       Source/Platform/POSIX/Poll.cpp
                                       Source/Platform/POSIX/SystemPAL.cpp
                                       Source/Platform/POSIX/SessionHandoff.cpp
                                       Source/Core/Session/ProcessSubtree.cpp)
# The socket tests drive the daemon's receive loop without launching a shell.
set(test_daemon_client_io_EXTRA_SOURCES ${test_daemon_session_configuration_EXTRA_SOURCES})
set(test_daemon_scrollback_maintenance_EXTRA_SOURCES ${test_daemon_session_configuration_EXTRA_SOURCES})
set(test_daemon_input_withdrawal_EXTRA_SOURCES ${test_daemon_session_configuration_EXTRA_SOURCES})
set(test_daemon_client_io_EXTRA_LIBS ${test_daemon_session_configuration_EXTRA_LIBS})
set(test_daemon_scrollback_maintenance_EXTRA_LIBS ${test_daemon_session_configuration_EXTRA_LIBS})
set(test_daemon_input_withdrawal_EXTRA_LIBS ${test_daemon_session_configuration_EXTRA_LIBS})
# The Core rules a daemon session installs, driven through the daemon's own
# pass; the coherence cases replay the Sync PTY corpus.
set(test_daemon_core_rules_EXTRA_SOURCES ${test_daemon_session_configuration_EXTRA_SOURCES})
# A hosted pane's measure in its clients' faces, through the daemon's pass.
set(test_proportional_measure_attached_EXTRA_SOURCES ${test_daemon_session_configuration_EXTRA_SOURCES})
set(test_proportional_measure_attached_EXTRA_LIBS ${test_daemon_session_configuration_EXTRA_LIBS})
set(test_daemon_core_rules_EXTRA_LIBS ${test_daemon_session_configuration_EXTRA_LIBS})
set(test_daemon_core_rules_EXTRA_DEFINES
    UBQ_SYNC_PTY_CORPUS_DIR="${UBIQUITTY_CORE_SOURCE_DIR}/Tests/Corpus/sync")
# The clipboard helpers are installed by the daemon's own ClipboardTool TU.
set(test_clipboard_tool_environment_EXTRA_SOURCES ${test_daemon_session_configuration_EXTRA_SOURCES})
set(test_clipboard_tool_environment_EXTRA_LIBS ${test_daemon_session_configuration_EXTRA_LIBS})
# Codec::JSON calls the vendored yyjson TU. It is a member of the aggregate
# beside the codec (ubiquitty-core-model), so a target that reaches JSON
# through Core links the parser and one that does not links neither; the two
# targets below name it because they compile the codec themselves.
# The rows a browser's renderer is tested against are the ones Core spells,
# held to it here; the path is handed in because anything derived from __FILE__
# goes relative under the compiler cache.
set(test_session_outline_text_EXTRA_DEFINES
    UBQ_OUTLINE_ROWS_FIXTURE="${UBIQUITTY_CORE_SOURCE_DIR}/Tests/Core/App/outline-rows.json")
set(test_json_depth_EXTRA_SOURCES      Source/Core/Codec/JSON.cpp
                                       Source/ThirdParty/yyjson/yyjson.c)
set(test_json_EXTRA_SOURCES            Source/Core/Codec/JSON.cpp
                                       Source/ThirdParty/yyjson/yyjson.c)
set(test_json_EXTRA_DEFINES            UBIQUITTY_ENABLE_TEST_HOOKS=1)
# The vectorised decoder is compiled only into the macOS app, so a parity target
# built the ordinary way has one path and compares it with itself. The define and
# the two sources are what give this target a second arm.
set(test_utf8_simd_parity_EXTRA_DEFINES UBIQUITTY_ENABLE_TEST_HOOKS=1)
set(test_utf8_simd_parity_EXTRA_SOURCES Source/Core/Unicode/UTF8Decoder.cpp
                                        Source/ThirdParty/simdutf/simdutf.cpp)
set(test_utf8_simd_parity_EXTRA_INCLUDES ${UBIQUITTY_CORE_SOURCE_DIR}/Source/ThirdParty/simdutf)
# Counts the bytes every run decode consumes while the parser places text, so
# the decoder is compiled here with its hooks rather than taken from Core.
set(test_text_run_decode_once_EXTRA_DEFINES UBIQUITTY_ENABLE_TEST_HOOKS=1)
set(test_text_run_decode_once_EXTRA_SOURCES Source/Core/Unicode/UTF8Decoder.cpp
                                            Source/ThirdParty/simdutf/simdutf.cpp)
set(test_text_run_decode_once_EXTRA_INCLUDES ${UBIQUITTY_CORE_SOURCE_DIR}/Source/ThirdParty/simdutf)
# Same suppression the agent target gives it: a vendored TU must not fail the
# build on a compiler bump under -Werror, and -std=c99 keeps the C++ options off
# a C source.
# SKIP_PRECOMPILE_HEADERS keeps the shared C++ precompiled header off this C
# translation unit; a target mixing the two cannot resolve one PCH for both.
# The vendored UTF-8 library under the same policy: a newer clang's opinion of
# an unused static in a file this tree does not own (utf16_length_from_utf8 on
# a newer Linux clang) must not fail the parity test's build under -Werror.
set_source_files_properties(${UBIQUITTY_CORE_SOURCE_DIR}/Source/ThirdParty/simdutf/simdutf.cpp
    PROPERTIES COMPILE_OPTIONS "-Wno-error;-w")
set_source_files_properties(${UBIQUITTY_CORE_SOURCE_DIR}/Source/ThirdParty/yyjson/yyjson.c
    PROPERTIES COMPILE_FLAGS "-std=c99 -Wno-error -w"
               SKIP_PRECOMPILE_HEADERS ON)

# A test of a layer built on top of the aggregate links that layer by name.
set(test_kit_session_EXTRA_LIBS ubiquitty-kit)
set(test_kit_workspace_EXTRA_LIBS ubiquitty-kit)

# Directories whose tests are left to Scripts/run-tests.sh as a whole. Empty:
# every test target gets the real source root, so the tests under
# Tests/Platform, Host, Linux and Agent build against the Core aggregate even
# though Platform/<Frontend>/*.h stages into no layer. A test that needs a
# symbol only a frontend supplies is deferred by name below.
set(UBIQUITTY_FRONTEND_TEST_DIRS)
# Individually deferred — each needs a symbol only a frontend or an external dep
# supplies, so it cannot link in a Core-only configure. Not skipped because they
# are broken; skipped because the portable Core carries no frontend and no
# platform backend, which is where each one's missing symbol lives. They stay in
# Scripts/run-tests.sh until the frontends get the same treatment.
#
# Named individually with the reason beside the name, rather than by directory:
# skipping whole directories hid 12 tests that build fine. The reason travels
# WITH the entry so removing one removes both — a list where they are separated
# leaves a paragraph explaining an entry no one can find.
list(APPEND UBIQUITTY_DEFERRED_TESTS
    # SerialStream is a POSIX backend (Source/Platform/POSIX/SerialStream.cpp),
    # and it sits behind PAL::OutputQueue: a Core-only configure links neither.
    test_serial)

# Both this directory and every directory under it. `*/test_*.cpp` requires a
# subdirectory, so a test written beside this file is picked up by neither
# driver: not here, and not by run-tests.sh, whose skip list is generated from
# what this glob found. It would report nothing and be counted by no one.
set(_ubq_test_patterns
    "${UBIQUITTY_CORE_SOURCE_DIR}/Tests/test_*.cpp"
    "${UBIQUITTY_CORE_SOURCE_DIR}/Tests/*/test_*.cpp")
file(GLOB_RECURSE _ubq_core_tests RELATIVE ${UBIQUITTY_CORE_SOURCE_DIR}/Tests
    CONFIGURE_DEPENDS ${_ubq_test_patterns})
# Leaves every test whose path under Tests/ matches <pattern> out of the list.
function(ubq_leave_out_tests pattern)
    list(FILTER _ubq_core_tests EXCLUDE REGEX "${pattern}")
    set(_ubq_core_tests ${_ubq_core_tests} PARENT_SCOPE)
endfunction()
# These tests own real Apple framework objects and run on the desktop host.
if(NOT APPLE OR NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    ubq_leave_out_tests("(^|/)Platform/Apple/test_(cfauto|image_decode)\\.cpp$")
endif()

# Tests whose single-process runtime sets the suite's wall clock. The binary
# must accept `--shard I/N` and partition its own outermost dimension; the swept
# space is unchanged, only the process count. Sharding a test that finishes in
# milliseconds costs more in process startup than it saves.
set(test_keymap_exhaustive_SHARDS 8)

# Tests that cannot reuse the shared precompiled header beyond those the module
# keeps off it (boundary tests, EXTRA_DEFINES, Objective-C++).
#
#   Tests that declare a name the precompiled header also declares. Each of
#   these defines its own Row, Cell or Pen and never included the Core header
#   that declares the same name, so force-including it makes the name ambiguous.
set(UBIQUITTY_NO_PCH_TESTS
    test_filetransfer test_linkrun test_pastenormalize
    test_scroll_lane_differential test_sgr_combined_parity
    test_terminalimage_widen test_title_walk_pacing
    test_unifont_sizing test_width)

# What every test and the precompiled header read: Source/ and Tests/.
set(_ubq_test_include_directories
    ${UBIQUITTY_CORE_SOURCE_DIR}/Source
    ${UBIQUITTY_CORE_SOURCE_DIR}/Tests)

# The libFuzzer harnesses, driven over a fixed corpus by Tests/FuzzDriver.cpp.
#
# They are outside the glob above, which takes `test_*.cpp`, so the only entry
# reaching them is shell_fuzz_harnesses_compile — syntax-only by its own header.
# A harness therefore compiled and never ran, and the assertions inside one had
# never been evaluated: fuzz_sync carries sixteen, added to make it an oracle.
#
# Core only. Tests/Host/fuzz_quic_preamble.cpp needs the QUIC stack, which is a
# fetched dependency and off by default — it stays compile-checked, where the
# twelve here become ctest entries that run in milliseconds.
file(GLOB_RECURSE UBIQUITTY_FUZZ_SOURCES RELATIVE ${UBIQUITTY_CORE_SOURCE_DIR}/Tests CONFIGURE_DEPENDS
    "${UBIQUITTY_CORE_SOURCE_DIR}/Tests/Core/fuzz_*.cpp"
    "${UBIQUITTY_CORE_SOURCE_DIR}/Tests/Core/*/fuzz_*.cpp"
    "${UBIQUITTY_CORE_SOURCE_DIR}/Tests/Core/*/*/fuzz_*.cpp")
foreach(_fuzz_source IN LISTS UBIQUITTY_FUZZ_SOURCES)
    get_filename_component(_fuzz_name ${_fuzz_source} NAME_WE)
    set(${_fuzz_name}_EXTRA_SOURCES Tests/FuzzDriver.cpp)
endforeach()
# fuzz_sync_screen, fuzz_sync_connection, fuzz_sync_catalog and fuzz_resume_record reach the model only through the Sync
# codec, so nothing either links pulls the POSIX PAL out of ubiquitty-core
# before the model archive is scanned, and the model's own PAL calls (OSC 7's host name) stay undefined
# under a linker that scans each archive once. The backend is named directly.
list(APPEND fuzz_sync_screen_EXTRA_SOURCES Source/Platform/POSIX/SystemPAL.cpp)
list(APPEND fuzz_sync_connection_EXTRA_SOURCES Source/Platform/POSIX/SystemPAL.cpp)
list(APPEND fuzz_sync_catalog_EXTRA_SOURCES Source/Platform/POSIX/SystemPAL.cpp)
list(APPEND fuzz_resume_record_EXTRA_SOURCES Source/Platform/POSIX/SystemPAL.cpp)

# A repository that carries this tree as a library registers its own tests in
# the same pass, through Tests/dependent-tests.cmake in its top directory. The file
# is read here, in this directory's scope, so it sets the same <test>_EXTRA_*
# variables the lists above set and names its tests in UBQ_DEPENDENT_TESTS, as
# paths under its own Tests/. Nothing reads it where this tree is the top
# directory.
set(UBQ_DEPENDENT_TESTS "")
if(NOT CMAKE_SOURCE_DIR STREQUAL UBIQUITTY_CORE_SOURCE_DIR
        AND EXISTS "${CMAKE_SOURCE_DIR}/Tests/dependent-tests.cmake")
    include("${CMAKE_SOURCE_DIR}/Tests/dependent-tests.cmake")
endif()

# The tests, then the harnesses after them. Both
# calls take every list above, so a harness named in one is held to it as a
# test is.
set(_ubq_host_test_lists
    INCLUDE_DIRECTORIES ${_ubq_test_include_directories}
    BOUNDARY ${UBIQUITTY_BOUNDARY_TESTS}
    NO_PCH ${UBIQUITTY_NO_PCH_TESTS}
    DEFERRED ${UBIQUITTY_DEFERRED_TESTS}
    UNAVAILABLE ${UBIQUITTY_UNAVAILABLE_TESTS}
    FRONTEND_DIRECTORIES ${UBIQUITTY_FRONTEND_TEST_DIRS})
ubiquitty_add_host_tests(
    ROOT ${UBIQUITTY_CORE_SOURCE_DIR} SOURCES ${_ubq_core_tests}
    ROOT ${CMAKE_SOURCE_DIR} SOURCES ${UBQ_DEPENDENT_TESTS}
    ${_ubq_host_test_lists})
ubiquitty_add_host_tests(
    ROOT ${UBIQUITTY_CORE_SOURCE_DIR} SOURCES ${UBIQUITTY_FUZZ_SOURCES}
    ${_ubq_host_test_lists})

# test_sync_handles drives 100,000 bind and unbind round trips through a whole
# Sync link. The count is the property under test: the handle slots stay at
# three however many rebindings pass. It takes about 30 seconds in a Linux
# Debug build on a fast workstation, and it ran past the shared bound on a
# hosted ubuntu-24.04 runner beside the rest of the suite. It takes its own
# bound for the reason above.
if(TEST test_sync_handles)
    # The instrumented frame allocations and serialization run without
    # competing test processes under the same timeout.
    set_tests_properties(test_sync_handles PROPERTIES TIMEOUT 300)
    set_property(TEST test_sync_handles PROPERTY RUN_SERIAL TRUE)
endif()

# test_sync_corpus_oracle replays every Sync corpus entry through a whole
# loopback link on a simulated clock, so nothing it asserts reads wall time. It
# takes about 11 seconds alone on the build host (Linux Debug, CPU-bound) and ran past the
# shared 60-second bound on the hosted three-core macOS runner beside the rest
# of the suite. It takes test_sync_handles' bound for the same reason.
if(TEST test_sync_corpus_oracle)
    set_tests_properties(test_sync_corpus_oracle PROPERTIES TIMEOUT 300)
endif()

# test_flood_start bounds how soon the drain captures a flood's first frame on
# the wall clock, so it runs with no other test competing for the cores.
if(TEST test_flood_start)
    set_tests_properties(test_flood_start PROPERTIES RUN_SERIAL TRUE RESOURCE_LOCK timing)
endif()

# test_frame_cadence times a 60 Hz frontend tick and every frontend call against
# a one-millisecond bound while a drain floods, so it runs alone: a test
# beside it takes the cores its threads are timed on.
if(TEST test_frame_cadence)
    set_tests_properties(test_frame_cadence PROPERTIES RUN_SERIAL TRUE RESOURCE_LOCK timing)
endif()

add_test(NAME test_presentation_launcher
    COMMAND ${Python3_EXECUTABLE} ${UBIQUITTY_CORE_SOURCE_DIR}/Scripts/test-presentation-launcher.py)
ubiquitty_set_test_entry_properties(test_presentation_launcher)

# test_ingest_loop paces writers a millisecond or so apart and holds the loop to
# a one-millisecond quiet interval, an eight-millisecond frame interval and the
# title edge's settle; test_pty waits on wall-clock deadlines while it starts
# shells. Each runs alone for the reason above: beside the rest of the suite on
# a three-core runner a one-millisecond sleep took about six and a half.
if(TEST test_ingest_loop)
    set_tests_properties(test_ingest_loop PROPERTIES RUN_SERIAL TRUE RESOURCE_LOCK timing)
endif()
if(TEST test_pty)
    set_tests_properties(test_pty PROPERTIES RUN_SERIAL TRUE RESOURCE_LOCK timing)
endif()

# ----------------------------------------------------------------------
# The Rule-10 layer boundary, tried by the compiler.
# check-core-layer-partition.sh proves that each Core source has exactly one
# layer; it cannot see whether the staged include directories keep a higher
# layer's headers out of the model's reach. A module list entry broad enough to
# match every behaviour subtree, such as a bare "Core", stages them all into the
# model directory while that gate and the build both stay green, so these
# probes attempt the includes themselves.
# ----------------------------------------------------------------------
# Registers a ctest entry that compiles Boundary/<source> with the model stage
# as its one include directory, and fails unless that compile succeeds.
function(ubq_add_layer_probe probeName probeSource)
    add_test(NAME ${probeName}
        COMMAND ${CMAKE_COMMAND}
            -DCXX=${CMAKE_CXX_COMPILER}
            -DINCLUDE_DIR=${UBIQUITTY_LAYER_STAGE_DIR}/model
            -DSRC=${UBIQUITTY_CORE_SOURCE_DIR}/Tests/Boundary/${probeSource}
            -DEXPECT=pass -DOUT=${CMAKE_CURRENT_BINARY_DIR}
            -P ${UBIQUITTY_CORE_SOURCE_DIR}/Tests/Boundary/run_boundary_probe.cmake)
    set_tests_properties(${probeName} PROPERTIES
        FIXTURES_REQUIRED ubq_build_fixture
        TIMEOUT ${UBIQUITTY_TEST_TIMEOUT})
endfunction()

# A model header has to be reachable, or the probe after it passes on an empty stage.
ubq_add_layer_probe(test_layer_boundary_control probe_model_from_model.cpp)
# The behaviour headers that probe names must not be.
ubq_add_layer_probe(test_layer_boundary probe_behaviour_from_model.cpp)

# ----------------------------------------------------------------------
# The shell-only remainder: every stanza of Scripts/run-tests.sh this tree does
# not register above, one ctest entry each, so `ctest` is the single entry point
# for the whole suite. -DUBIQUITTY_CTEST_SHELL_REMAINDER=OFF leaves them out.
# The stanzas meant are libcore_consumer, the snapshot harness, the fuzzers and
# the QUIC, capability, consent and token integration groups.
#
# ubiquitty_schedule_shell_stanzas schedules them. This function wires
# run-tests.sh's own graph into its shell_<stanza>_* settings: the fixtures that
# build the trees and binaries the stanzas share, which stanzas wait for each,
# what each is told about them, and the locks on the trees they write.
# ----------------------------------------------------------------------
function(ubq_schedule_run_tests_stanzas driver)
    ubiquitty_read_shell_stanzas(_stanzas DRIVER ${driver})
    if(NOT UBIQUITTY_CTEST_SHELL_REMAINDER)
        ubiquitty_schedule_shell_stanzas(DRIVER ${driver} STANZAS ${_stanzas})
        return()
    endif()
    # The platform jobs depend on a successful exact-head preflight, whose
    # style aggregate already executes every gate self-test. The workflow sets
    # this only for those jobs; standalone and local suites retain the stanza.
    if("$ENV{UBQ_CI_PREFLIGHT_COMPLETE}" STREQUAL "1")
        list(REMOVE_ITEM _stanzas gate_self_tests)
        message(STATUS "Ubiquitty tests: gate self-tests covered by exact-head preflight")
    endif()

    # The stanzas that share a CMake build directory, each paired with the
    # directory it shares. They cannot run beside another stanza using the same
    # tree — two configures into one directory race on compiler inspection under
    # CMakeFiles/, and the socket name adopt_session derives from the binary they
    # both write makes the loser talk to the winner's daemon.
    #
    # One lock per directory rather than one for the whole set: stanzas on
    # different trees do not contend, and a single lock would serialize forty of
    # them to protect against a collision most of them cannot have.
    # The listing's failure stops the configure, because this list going empty
    # is silent by nature: no locks are attached, every stanza still runs, and
    # the suite still reports a result -- just an unreliable one. That is what
    # happened when build output moved to $UBQ_BUILD_ROOT and the detector kept
    # matching $ROOT/build/: it emitted nothing for a day and nothing said so.
    # The script now exits non-zero rather than emit an empty list.
    ubiquitty_read_shell_stanzas(_serial_pairs DRIVER ${driver} LISTING --list-serial-stanzas)
    # "<stanza> <directory>" -> shell_<stanza>_RESOURCE_LOCK holding the directory.
    foreach(_pair IN LISTS _serial_pairs)
        if(_pair STREQUAL "")
            continue()
        endif()
        string(REGEX REPLACE "^([^ ]+) .*$" "\\1" _pair_stanza "${_pair}")
        string(REGEX REPLACE "^[^ ]+ (.*)$" "\\1" _pair_directory "${_pair}")
        # A stanza touching two trees needs both locks, so this accumulates.
        list(APPEND shell_${_pair_stanza}_RESOURCE_LOCK "ubq_build_${_pair_directory}")
        if(_pair_directory STREQUAL "host-noquic")
            list(APPEND _host_noquic_stanzas ${_pair_stanza})
        elseif(_pair_directory STREQUAL "host-quic")
            list(APPEND _host_quic_stanzas ${_pair_stanza})
        endif()
    endforeach()

    # The Host fixture is a build, not a test, and every stanza that needs the
    # binary is stopped until it finishes — so a width chosen for a test makes
    # all of them wait longer. It gets a build's width and declares it, which is
    # what keeps ctest from starting that much other work beside it.
    set(UBIQUITTY_HOST_FIXTURE_JOBS 8 CACHE STRING
        "Compilers the Host-frontend fixture may fork, and the ctest slots it holds")

    # Stanzas that run the built Host frontend. One fixture builds that tree;
    # they require the fixture and only read it, so no stanza configures or
    # builds it and none holds a lock against another.
    ubiquitty_read_shell_stanzas(_host_binary_stanzas DRIVER ${driver} LISTING --list-host-binary-stanzas)
    # Stanzas that run the built agent. The agent_binary fixture builds it once,
    # with a build's width, for the same reasons as the Host fixture.
    ubiquitty_read_shell_stanzas(_agent_binary_stanzas DRIVER ${driver} LISTING --list-agent-binary-stanzas)
    # A cold dependency prefix is built once. Every stanza that probes those
    # archives waits for that setup instead of independently observing a
    # transient absence and reporting a green skip.
    ubiquitty_read_shell_stanzas(_quic_dependency_stanzas DRIVER ${driver} LISTING --list-quic-dependency-stanzas)

    set(_quic_test_stanzas quic_channel_lifecycle quic_daemon_accept quic_bound_socket)
    # The stanzas that render with the portable snapshot renderer.
    set(_snapshot_renderer_stanzas snapshots)
    foreach(_stanza IN LISTS _stanzas)
        if(_stanza STREQUAL "")
            continue()
        endif()
        set(_entry shell_${_stanza})
        if(NOT _stanza STREQUAL "cache_sweep_fixture")
            list(APPEND ${_entry}_ENVIRONMENT UBQ_CACHE_SWEEP_COMPLETE=1)
        endif()
        if(_stanza STREQUAL "quic_test_fixture")
            set(${_entry}_JOBS ${UBIQUITTY_HOST_FIXTURE_JOBS})
            list(APPEND ${_entry}_ENVIRONMENT UBQ_COVERAGE_BUILD_FIXTURE=1)
        endif()
        if(_stanza STREQUAL "libcore_fixture")
            set(${_entry}_JOBS ${UBIQUITTY_HOST_FIXTURE_JOBS})
            list(APPEND ${_entry}_ENVIRONMENT UBQ_COVERAGE_BUILD_FIXTURE=1)
        elseif(_stanza STREQUAL "libcore_consumer" OR _stanza STREQUAL "driver_shapes")
            list(APPEND ${_entry}_ENVIRONMENT UBQ_LIBCORE_PREBUILT=1)
        endif()
        # The portable snapshot renderer is a full portable Core build, which
        # does not fit a rendering stanza's timeout beside the rest of the suite.
        # Its fixture builds it with a build's width; the snapshots entry is
        # ordered after it and refuses a stale renderer instead of rebuilding.
        if(_stanza STREQUAL "snapshot_renderer_fixture")
            set(${_entry}_JOBS ${UBIQUITTY_HOST_FIXTURE_JOBS})
            list(APPEND ${_entry}_ENVIRONMENT UBQ_COVERAGE_BUILD_FIXTURE=1)
        elseif(_stanza IN_LIST _snapshot_renderer_stanzas)
            list(APPEND ${_entry}_ENVIRONMENT UBQ_SNAPSHOT_RENDERER_PREBUILT=1)
        endif()
        if(_stanza IN_LIST _host_binary_stanzas)
            list(APPEND ${_entry}_ENVIRONMENT UBQ_HOST_PREBUILT=1)
        elseif(_stanza STREQUAL "host_binary" OR _stanza STREQUAL "agent_binary")
            set(${_entry}_JOBS ${UBIQUITTY_HOST_FIXTURE_JOBS})
            # The fixture builds and runs nothing, and that is its design: the stanzas
            # ordered after it execute what it compiled, and their profiles
            # carry their own entry names. Holding the fixture itself to a
            # profile would fail every coverage run for the one stanza whose
            # job ends at the link. Declared here, beside the fact that makes
            # it true, rather than as a name list in the gate.
            list(APPEND ${_entry}_ENVIRONMENT UBQ_COVERAGE_BUILD_FIXTURE=1)
        endif()
        if(_stanza IN_LIST _agent_binary_stanzas)
            list(APPEND ${_entry}_ENVIRONMENT UBQ_AGENT_PREBUILT=1)
        endif()
        # The Host frontend and the agent are the exception to building on
        # demand: their fixtures build them for this run, so a stanza the two
        # lists above did not order after them refuses to build either rather
        # than relink it under the stanzas executing it.
        if(NOT _stanza STREQUAL "host_binary" AND NOT _stanza STREQUAL "agent_binary")
            list(APPEND ${_entry}_ENVIRONMENT UBQ_SHARED_BINARIES_FROM_FIXTURES=1)
        endif()
        if(_stanza IN_LIST _host_noquic_stanzas
                AND NOT _stanza STREQUAL "host_noquic_fixture")
            list(APPEND ${_entry}_ENVIRONMENT UBQ_HOST_NOQUIC_PREBUILT=1)
            list(REMOVE_ITEM ${_entry}_RESOURCE_LOCK "ubq_build_host-noquic")
        endif()
        if(_stanza IN_LIST _host_quic_stanzas
                AND NOT _stanza STREQUAL "host_quic_fixture")
            list(APPEND ${_entry}_ENVIRONMENT UBQ_HOST_QUIC_PREBUILT=1)
            list(REMOVE_ITEM ${_entry}_RESOURCE_LOCK "ubq_build_host-quic")
        endif()
        if(_stanza STREQUAL "host_noquic_fixture"
                OR _stanza STREQUAL "host_quic_fixture")
            set(${_entry}_JOBS ${UBIQUITTY_HOST_FIXTURE_JOBS})
            list(APPEND ${_entry}_ENVIRONMENT UBQ_COVERAGE_BUILD_FIXTURE=1)
        endif()
        if(_stanza STREQUAL "cache_sweep_fixture")
            list(APPEND ${_entry}_ENVIRONMENT UBQ_COVERAGE_BUILD_FIXTURE=1)
        endif()
        if(_stanza STREQUAL "quic_deps_fixture")
            set(${_entry}_JOBS ${UBIQUITTY_HOST_FIXTURE_JOBS})
            list(APPEND ${_entry}_ENVIRONMENT UBQ_COVERAGE_BUILD_FIXTURE=1)
        endif()
        if(DEFINED ${_entry}_RESOURCE_LOCK)
            list(REMOVE_DUPLICATES ${_entry}_RESOURCE_LOCK)
        endif()

        # The cache sweep runs first and waits for nothing; the dependency
        # prefix waits only for the sweep; every other stanza waits for the
        # build and the sweep, and then for the fixtures of what it runs.
        set(${_entry}_FIXTURES_REQUIRED "")
        if(_stanza STREQUAL "quic_deps_fixture")
            list(APPEND ${_entry}_FIXTURES_REQUIRED ubq_cache_sweep_fixture)
        elseif(NOT _stanza STREQUAL "cache_sweep_fixture")
            list(APPEND ${_entry}_FIXTURES_REQUIRED
                ubq_build_fixture
                ubq_cache_sweep_fixture)
        endif()
        if(_stanza IN_LIST _host_binary_stanzas)
            list(APPEND ${_entry}_FIXTURES_REQUIRED ubq_host_binary_fixture)
        endif()
        if(_stanza IN_LIST _agent_binary_stanzas)
            list(APPEND ${_entry}_FIXTURES_REQUIRED ubq_agent_binary_fixture)
        endif()
        if(_stanza IN_LIST _host_noquic_stanzas
                AND NOT _stanza STREQUAL "host_noquic_fixture")
            list(APPEND ${_entry}_FIXTURES_REQUIRED ubq_host_noquic_fixture)
        endif()
        if(_stanza IN_LIST _host_quic_stanzas
                AND NOT _stanza STREQUAL "host_quic_fixture")
            list(APPEND ${_entry}_FIXTURES_REQUIRED ubq_host_quic_fixture)
        endif()
        if(_stanza STREQUAL "host_binary"
                OR _stanza STREQUAL "host_noquic_fixture"
                OR _stanza STREQUAL "host_quic_fixture")
            list(APPEND ${_entry}_FIXTURES_REQUIRED ubq_quic_deps_fixture)
        endif()
        if(_stanza IN_LIST _quic_dependency_stanzas)
            list(APPEND ${_entry}_FIXTURES_REQUIRED ubq_quic_deps_fixture)
        endif()
        if(_stanza STREQUAL "libcore_consumer" OR _stanza STREQUAL "driver_shapes")
            list(APPEND ${_entry}_FIXTURES_REQUIRED ubq_libcore_fixture)
        endif()
        if(_stanza IN_LIST _snapshot_renderer_stanzas)
            list(APPEND ${_entry}_FIXTURES_REQUIRED ubq_snapshot_renderer_fixture)
        endif()
        if(_stanza IN_LIST _quic_test_stanzas)
            list(APPEND ${_entry}_FIXTURES_REQUIRED ubq_quic_test_fixture)
        endif()
        list(REMOVE_DUPLICATES ${_entry}_FIXTURES_REQUIRED)
    endforeach()

    # The CI battery runs these tiers separately. Its explicit opt-out is a
    # skipped test, not a passing render or an unmatched filter.
    #
    # On macOS the stanza renders the portable, macOS and Swift tiers, about
    # fifteen hundred cases through the app, which took seventeen minutes on a
    # ten-core laptop with nothing else running: a rendering stanza's ten
    # minutes cannot hold it.
    set(shell_snapshots_SKIP_REGULAR_EXPRESSION
        "(^|\n)SKIP: snapshots: explicitly disabled with SNAPSHOT=skip(\n|$)")
    set(shell_snapshots_TIMEOUT 2400)
    set(shell_clipboard_tool_no_rcs_SKIP_REGULAR_EXPRESSION
        "SKIP: clipboard_tool_no_rcs requires zsh")
    set(shell_test_font_fanout_SKIP_RETURN_CODE 77)
    # It builds the tree the others read, so it is the one that may write it
    # and the only one ctest orders first.
    set(shell_host_binary_FIXTURES_SETUP ubq_host_binary_fixture)
    # This fixture includes a cold Release build of the agent.
    set(shell_agent_binary_FIXTURES_SETUP ubq_agent_binary_fixture)
    set(shell_agent_binary_TIMEOUT 1800)
    set(shell_quic_test_fixture_FIXTURES_SETUP ubq_quic_test_fixture)
    set(shell_quic_test_fixture_TIMEOUT 900)
    set(shell_libcore_fixture_FIXTURES_SETUP ubq_libcore_fixture)
    set(shell_libcore_fixture_TIMEOUT 1800)
    # Sized for a cold optimized build of the portable Core.
    set(shell_snapshot_renderer_fixture_FIXTURES_SETUP ubq_snapshot_renderer_fixture)
    set(shell_snapshot_renderer_fixture_TIMEOUT 1800)
    # This stanza builds and runs a separate instrumented tree.
    set(shell_sanitized_sync_TIMEOUT 1800)
    set(shell_host_noquic_fixture_FIXTURES_SETUP ubq_host_noquic_fixture)
    set(shell_host_noquic_fixture_TIMEOUT 900)
    set(shell_cache_sweep_fixture_FIXTURES_SETUP ubq_cache_sweep_fixture)
    set(shell_cache_sweep_fixture_TIMEOUT 60)
    set(shell_quic_deps_fixture_FIXTURES_SETUP ubq_quic_deps_fixture)
    set(shell_quic_deps_fixture_TIMEOUT 900)
    set(shell_host_quic_fixture_FIXTURES_SETUP ubq_host_quic_fixture)
    set(shell_host_quic_fixture_TIMEOUT 900)
    # The soak's thousand attaches each start a relay and a client, so it is
    # CPU-bound and takes as long as the cores the rest of the suite leaves
    # it. It asserts no timing, only resident growth and a clean exit, so its
    # bound guards against a hang, and it is the stanza's own in run-tests.sh.
    set(shell_host_soak_TIMEOUT 900)

    # This stanza compiles the terminal Core twice, for TSan and ASan. Give
    # those cold builds a build's width and no competing tests, while keeping
    # the same 600-second deadline for the builds and both stress runs.
    set(shell_test_session_stress_JOBS ${UBIQUITTY_HOST_FIXTURE_JOBS})
    set(shell_test_session_stress_RUN_SERIAL TRUE)

    # Frame-count and handoff deadlines require a quiet scheduler. The
    # sanitizer stanza also runs the ingest loop's timing cases inside its
    # instrumented binary, so it needs the same isolation as that unit test.
    set(shell_sanitized_sync_RUN_SERIAL TRUE)
    list(APPEND shell_sanitized_sync_RESOURCE_LOCK timing)
    set(shell_sync_output_e2e_RUN_SERIAL TRUE)
    list(APPEND shell_sync_output_e2e_RESOURCE_LOCK timing)

    ubiquitty_schedule_shell_stanzas(DRIVER ${driver} STANZAS ${_stanzas})
endfunction()
ubiquitty_default_shell_driver(_ubq_shell_driver)
if(CMAKE_SOURCE_DIR STREQUAL UBIQUITTY_CORE_SOURCE_DIR
        AND _ubq_shell_driver STREQUAL "${UBIQUITTY_CORE_SOURCE_DIR}/Scripts/run-tests.sh")
    ubq_schedule_run_tests_stanzas("${_ubq_shell_driver}")
else()
    ubiquitty_schedule_shell_stanzas(DRIVER "${_ubq_shell_driver}")
endif()

# input_settled_echo reaches a verdict only for a key its program echoes within
# the settle timer E, and beside the rest of the suite on a small runner the
# program misses E on every key. It runs alone, as the wall-clock unit tests
# do, keeping the scheduler's locks; a run that still misses E fails on that
# premise.
if(TEST shell_input_settled_echo)
    set_tests_properties(shell_input_settled_echo PROPERTIES RUN_SERIAL TRUE)
    set_property(TEST shell_input_settled_echo APPEND PROPERTY RESOURCE_LOCK timing)
endif()

# ----------------------------------------------------------------------
# Dev tools that are not tests: flood benchmarks, a fuzz-corpus generator, a
# PTY-replay client. They have no pass/fail behaviour, so they are compiled and
# never run — an OBJECT library is exactly that, and it puts them in the default
# build so a header or API they call cannot drift without failing ninja.
#
# They rotted this way before, invisibly, because nothing referenced them.
# ----------------------------------------------------------------------
# Globbed, never listed. A hand-maintained list of tools is the same thing that
# let bench_stack.cpp and four siblings sit uncompiled: a file nothing names is
# a file nothing checks. The naming convention is the registration.
# Tools/ is a sibling of Tests/ and holds the same kind of thing: programs with
# no pass/fail behaviour. They were built by run-tests.sh stanzas, which both
# compiled and ran them — and font-diagnose contains no assertion at all, so
# running it asserted nothing and only cost the time. Compiled here, never run,
# which is what the rest of this block already does for the tools inside Tests/.
file(GLOB UBIQUITTY_SIBLING_TOOL_SOURCES CONFIGURE_DEPENDS
    "${UBIQUITTY_CORE_SOURCE_DIR}/Tools/*.cpp")

# No `*/` before the name. GLOB_RECURSE already reaches every depth, and a `*/`
# would additionally require a subdirectory — so a tool placed directly in
# Tests/ would follow the naming convention and go silently unregistered, the
# one case where "the naming convention is the registration" above would be
# false. Measured with cmake -P over a probe tree: `Tests/*/bench_*.cpp` matches
# depth 2 and depth 3 and misses `Tests/bench_x.cpp`; the form below matches all
# three.
file(GLOB_RECURSE UBIQUITTY_TOOL_SOURCES CONFIGURE_DEPENDS
    "${UBIQUITTY_CORE_SOURCE_DIR}/Tests/bench_*.cpp"
    "${UBIQUITTY_CORE_SOURCE_DIR}/Tests/perf_*.cpp"
    "${UBIQUITTY_CORE_SOURCE_DIR}/Tests/profile_*.cpp"
    "${UBIQUITTY_CORE_SOURCE_DIR}/Tests/build_corpus.cpp"
    "${UBIQUITTY_CORE_SOURCE_DIR}/Tests/smoke_client.cpp")
list(APPEND UBIQUITTY_TOOL_SOURCES ${UBIQUITTY_SIBLING_TOOL_SOURCES})

foreach(_tool_source IN LISTS UBIQUITTY_TOOL_SOURCES)
    get_filename_component(_tool_name ${_tool_source} NAME_WE)
    add_library(ubq-tool-${_tool_name} OBJECT ${_tool_source})
    target_include_directories(ubq-tool-${_tool_name} PRIVATE
        ${UBIQUITTY_CORE_SOURCE_DIR}/Source
        ${UBIQUITTY_CORE_SOURCE_DIR}/Source/ThirdParty/simdutf
        ${UBIQUITTY_CORE_SOURCE_DIR}/Tests)
    target_compile_features(ubq-tool-${_tool_name} PRIVATE cxx_std_23)
    target_compile_options(ubq-tool-${_tool_name} PRIVATE
        -Wall -Wextra -Werror -Wno-deprecated-declarations -Wno-multichar -Wno-comment)
endforeach()

add_executable(ubq-host-smoke-client $<TARGET_OBJECTS:ubq-tool-smoke_client>)
target_link_libraries(ubq-host-smoke-client PRIVATE ubiquitty-core)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

function(ubq_configure_shell_target target)
    target_include_directories(${target} PRIVATE
        ${UBIQUITTY_CORE_SOURCE_DIR}/Source
        ${UBIQUITTY_CORE_SOURCE_DIR}/Source/ThirdParty/simdutf
        ${UBIQUITTY_CORE_SOURCE_DIR}/Tests)
    target_compile_features(${target} PRIVATE cxx_std_23)
    set_target_properties(${target} PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -fno-exceptions -fno-rtti)
endfunction()

# These integration stanzas retain their process setup and assertions in the
# shell driver, but link the same Core as the rest of this test tree.
foreach(_shell_test session_directory remote_session_smoke remote_workspace_smoke
        named_layout_restore remote_clear_e2e tmux_real_e2e ack_rides_input remote_pane_link_e2e)
    add_executable(ubq-shell-${_shell_test} ${UBIQUITTY_CORE_SOURCE_DIR}/Tests/Host/${_shell_test}.cpp)
    ubq_configure_shell_target(ubq-shell-${_shell_test})
    target_link_libraries(ubq-shell-${_shell_test} PRIVATE ubiquitty-core Threads::Threads)
endforeach()

# Handover's socket tests keep their restricted link set; sharing a build
# must not supply missing dependencies from the full Core aggregate.
add_library(ubq-shell-handover-transport OBJECT
    ${UBIQUITTY_CORE_SOURCE_DIR}/Source/Platform/POSIX/HandoverTransport.cpp
    ${UBIQUITTY_CORE_SOURCE_DIR}/Source/Core/Protocol/Sync/HandoverChannel.cpp)
ubq_configure_shell_target(ubq-shell-handover-transport)
add_executable(ubq-shell-handover_admission
    ${UBIQUITTY_CORE_SOURCE_DIR}/Tests/Host/handover_admission.cpp
    ${UBIQUITTY_CORE_SOURCE_DIR}/Source/Platform/Host/HandoverAdmission.cpp
    ${UBIQUITTY_CORE_SOURCE_DIR}/Source/Platform/POSIX/PTYFacts.cpp
    ${UBIQUITTY_CORE_SOURCE_DIR}/Source/Core/Session/ProcessSubtree.cpp)
add_executable(ubq-shell-handover_request
    ${UBIQUITTY_CORE_SOURCE_DIR}/Tests/Host/handover_request.cpp
    ${UBIQUITTY_CORE_SOURCE_DIR}/Source/Platform/Host/HandoverRequest.cpp)
foreach(_handover ubq-shell-handover_admission ubq-shell-handover_request)
    ubq_configure_shell_target(${_handover})
    target_compile_definitions(${_handover} PRIVATE UBIQUITTY_TEST_WITHOUT_CORE)
    target_link_libraries(${_handover} PRIVATE ubq-shell-handover-transport)
endforeach()
if(APPLE)
    target_link_libraries(ubq-shell-handover_admission PRIVATE "-framework CoreFoundation" "-framework Security")
endif()

# These targets are built only by the QUIC shell fixture, after its dependency
# fixture supplies the TLS archives. Core-only builds need not fetch them.
set(_ubq_shell_quic_libraries
    "${UBIQUITTY_QUIC_PREFIX}/lib/libngtcp2_crypto_boringssl.a"
    "${UBIQUITTY_QUIC_PREFIX}/lib/libngtcp2.a"
    "${UBIQUITTY_QUIC_PREFIX}/lib/libssl.a"
    "${UBIQUITTY_QUIC_PREFIX}/lib/libcrypto.a")
if(APPLE)
    list(APPEND _ubq_shell_quic_libraries "-framework CoreFoundation" "-framework Security")
endif()
add_library(ubq-shell-quic-client OBJECT EXCLUDE_FROM_ALL
    ${UBIQUITTY_CORE_SOURCE_DIR}/Source/Core/Transport/QUICChannel.cpp
    ${UBIQUITTY_CORE_SOURCE_DIR}/Source/Platform/POSIX/QUICClient.cpp
    ${UBIQUITTY_CORE_SOURCE_DIR}/Source/Platform/POSIX/DatagramBatch.cpp)
ubq_configure_shell_target(ubq-shell-quic-client)
target_include_directories(ubq-shell-quic-client PUBLIC "${UBIQUITTY_QUIC_PREFIX}/include")
target_compile_definitions(ubq-shell-quic-client PUBLIC UBIQUITTY_QUIC=1)
target_link_libraries(ubq-shell-quic-client PUBLIC ${_ubq_shell_quic_libraries} Threads::Threads)
add_executable(ubq-shell-quic_channel_lifecycle EXCLUDE_FROM_ALL ${UBIQUITTY_CORE_SOURCE_DIR}/Tests/Host/quic_channel_lifecycle.cpp)
ubq_configure_shell_target(ubq-shell-quic_channel_lifecycle)
target_link_libraries(ubq-shell-quic_channel_lifecycle PRIVATE ubq-shell-quic-client)

add_library(ubq-shell-quic-server OBJECT EXCLUDE_FROM_ALL
    ${UBIQUITTY_CORE_SOURCE_DIR}/Source/Platform/Host/HostQUIC.cpp
    ${UBIQUITTY_CORE_SOURCE_DIR}/Source/Platform/POSIX/DatagramBatch.cpp
    ${UBIQUITTY_CORE_SOURCE_DIR}/Source/Platform/Host/HostPaths.cpp
    ${UBIQUITTY_CORE_SOURCE_DIR}/Source/Core/Session/ControlPaths.cpp
    ${UBIQUITTY_CORE_SOURCE_DIR}/Source/Platform/POSIX/SocketPath.cpp
    ${UBIQUITTY_CORE_SOURCE_DIR}/Source/Platform/POSIX/SystemPAL.cpp
    ${UBIQUITTY_CORE_SOURCE_DIR}/Source/Platform/POSIX/Preferences.cpp
    ${UBIQUITTY_CORE_SOURCE_DIR}/Source/Core/Preferences/PreferencesPaths.cpp)
ubq_configure_shell_target(ubq-shell-quic-server)
target_include_directories(ubq-shell-quic-server PUBLIC "${UBIQUITTY_QUIC_PREFIX}/include")
target_link_libraries(ubq-shell-quic-server PUBLIC ${_ubq_shell_quic_libraries} Threads::Threads)
foreach(_server_test quic_daemon_accept quic_bound_socket)
    add_executable(ubq-shell-${_server_test} EXCLUDE_FROM_ALL ${UBIQUITTY_CORE_SOURCE_DIR}/Tests/Host/${_server_test}.cpp)
    ubq_configure_shell_target(ubq-shell-${_server_test})
    target_link_libraries(ubq-shell-${_server_test} PRIVATE ubq-shell-quic-server)
endforeach()
target_compile_definitions(ubq-shell-quic_bound_socket PRIVATE UBIQUITTY_TEST_WITHOUT_CORE)

# tmux control mode under load against a real tmux server, hosted as a
# frontend hosts it: run-tests.sh's tmux_control_stress runs it. Its sessions
# launch over real PTYs, so it links the PAL set the ingest-loop test does.
set(_ubq_tmux_stress_sources ${UBIQUITTY_CORE_SOURCE_DIR}/Tests/Host/tmux_control_stress.cpp)
foreach(_ubq_pal_source IN LISTS test_ingest_loop_EXTRA_SOURCES)
    list(APPEND _ubq_tmux_stress_sources ${UBIQUITTY_CORE_SOURCE_DIR}/${_ubq_pal_source})
endforeach()
add_executable(ubq-tmux-control-stress ${_ubq_tmux_stress_sources})
target_include_directories(ubq-tmux-control-stress PRIVATE
    ${UBIQUITTY_CORE_SOURCE_DIR}/Source
    ${UBIQUITTY_CORE_SOURCE_DIR}/Source/ThirdParty/simdutf
    ${UBIQUITTY_CORE_SOURCE_DIR}/Tests)
target_compile_features(ubq-tmux-control-stress PRIVATE cxx_std_23)
target_compile_options(ubq-tmux-control-stress PRIVATE
    -Wall -Wextra -Werror -Wno-deprecated-declarations -Wno-multichar -Wno-comment)
target_link_libraries(ubq-tmux-control-stress PRIVATE ubiquitty-core)

# The structured seeds of the Sync fuzz corpus, written at the current wire;
# Scripts/refresh-sync-fuzz-corpus.py runs it.
add_executable(ubq-sync-corpus-builder $<TARGET_OBJECTS:ubq-tool-build_corpus>)
target_link_libraries(ubq-sync-corpus-builder PRIVATE ubiquitty-core)

# ctest-covered.txt, which tells Scripts/run-tests.sh what ctest has run.
ubiquitty_finish_host_tests()
