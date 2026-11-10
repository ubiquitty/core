<!-- Copyright (C) 2026 Stanislav Yaglo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Scripts

Every file in this directory, grouped by purpose. Run scripts from the repository root.
Each script's header documents its options; most also answer `--help`.

## Build

Building the frontends, the host, and their shared libraries. Every build writes outside the checkout, under the root `build_root.py` prints.

| Script | Purpose |
| --- | --- |
| [`build-agent.sh`](build-agent.sh) | Builds the `ubq` command-line client and MCP server. |
| [`build-all.sh`](build-all.sh) | Builds the host, `ubq`, and the macOS app; `--headless` builds only the first two. |
| [`build-host-linux.sh`](build-host-linux.sh) | Cross-builds static Linux `ubiquitty-host` binaries for deployment to servers. |
| [`build-host.sh`](build-host.sh) | Builds the `ubiquitty-host` daemon. |
| [`build-ios.sh`](build-ios.sh) | Builds the iOS and iPadOS app for the device and simulator SDKs. |
| [`build-jobs.sh`](build-jobs.sh) | How many compilers this machine can run at once, for shell callers. |
| [`build-lane.py`](build-lane.py) | Runs a build in one of a few long-lived worktrees whose build roots stay warm. |
| [`build-linux-gtk.sh`](build-linux-gtk.sh) | Builds the Linux GTK4 and Vulkan frontend. |
| [`build-macos.sh`](build-macos.sh) | Builds the macOS app. |
| [`build-provenance.sh`](build-provenance.sh) | Sourced freshness guard that notices a build directory moved between checkouts. |
| [`build-root.sh`](build-root.sh) | Where a checkout's build output goes, for shell callers. |
| [`build-tests.sh`](build-tests.sh) | Compile every test translation unit, run none of them. |
| [`build-version.sh`](build-version.sh) | Print the version a build of this tree carries. |
| [`build-core-wasm.sh`](build-core-wasm.sh) | Builds portable Core's layer libraries to wasm32, without the page that links them. |
| [`build-web.sh`](build-web.sh) | Builds the browser frontend to wasm32 and stages a servable directory. |
| [`build_jobs.py`](build_jobs.py) | How many compilers this machine can run at once. |
| [`build_root.py`](build_root.py) | Where a checkout's build output goes. |
| [`ccache-env.sh`](ccache-env.sh) | The compiler-cache settings this tree's builds need. |
| [`cmake/AppleSharedSwift.cmake`](cmake/AppleSharedSwift.cmake) | The Swift layer macOS and iOS both compile. |
| [`cmake/ArchiveSwiftPMObjects.cmake`](cmake/ArchiveSwiftPMObjects.cmake) | Hands a SwiftPM library product's archive, and the flags that import its modules, to a build outside SwiftPM. |
| [`cmake/BuildVersion.cmake`](cmake/BuildVersion.cmake) | The version of a build, derived from the commit it is built from. |
| [`cmake/CheckMemcmpFold.cmake`](cmake/CheckMemcmpFold.cmake) | Does this compiler fold memcmp at a runtime offset? |
| [`cmake/CompilerIdentity.cmake`](cmake/CompilerIdentity.cmake) | A stamp naming the compiler and its standard library, so a step reruns after an in-place toolchain upgrade. |
| [`cmake/IOSPlatformStep.cmake`](cmake/IOSPlatformStep.cmake) | The iOS build steps whose output belongs to one SDK. |
| [`cmake/MetalLibraryStep.cmake`](cmake/MetalLibraryStep.cmake) | The render kit's shader library, compiled for one iOS SDK when the build runs. |
| [`cmake/ToolchainFloor.cmake`](cmake/ToolchainFloor.cmake) | The oldest compilers the tree builds with, checked at configure time. |
| [`cmake/TypedPasteboard.cmake`](cmake/TypedPasteboard.cmake) | Builds the Swift typed-pasteboard library the tests link. |
| [`cmake/UbiquittyCoreLibrary.cmake`](cmake/UbiquittyCoreLibrary.cmake) | Core's libraries, created in the calling directory: the layer chain, the iOS library, the session facade, and the installed package. |
| [`cmake/UbiquittyCoreRoot.cmake`](cmake/UbiquittyCoreRoot.cmake) | Where Core's sources are, and the order in which a project that compiles Core includes the modules below at its top directory. |
| [`cmake/UbiquittyCoreSources.cmake`](cmake/UbiquittyCoreSources.cmake) | What Core compiles: the canonical source list, the vendored sources it carries, and the generated translation units. |
| [`cmake/UbiquittyKit.cmake`](cmake/UbiquittyKit.cmake) | The Kit's Apple products, created in the calling directory: the Swift flags, the render kit and UbiquittyKit modules, the shader library, and the framework. |
| [`cmake/UbiquittyOptimize.cmake`](cmake/UbiquittyOptimize.cmake) | Link-time optimization, the training profile, and the release path map of an optimized build of Core. |
| [`cmake/UbiquittyPreProject.cmake`](cmake/UbiquittyPreProject.cmake) | What a build of Core decides before `project()`: the compiler launcher, the deployment floors, the macOS SDK, and the version. |
| [`cmake/UbiquittyTests.cmake`](cmake/UbiquittyTests.cmake) | Registers a repository's host tests with CTest, each rooted at the checkout that owns it, and makes each stanza its shell driver lists a CTest entry of its own. |
| [`cmake/UbiquittyToolchain.cmake`](cmake/UbiquittyToolchain.cmake) | What every target of a build of Core is compiled under: the compiler floor, the build type, the version header, the warnings, the instrumentation, and the QUIC dependencies. |
| [`ensure-quic-deps.sh`](ensure-quic-deps.sh) | Validates the cached QUIC archives and rebuilds only a missing or foreign prefix. |
| [`fetch-quic-deps.sh`](fetch-quic-deps.sh) | Fetches and builds the pinned ngtcp2 and AWS-LC sources the QUIC transport links. |
| [`generate-xcode-workspace.sh`](generate-xcode-workspace.sh) | Generate the Xcode projects collected by Ubiquitty.xcworkspace. |
| [`install-macos.sh`](install-macos.sh) | Builds the release app and installs it into /Applications. |
| [`linux-images.txt`](linux-images.txt) | The Linux container images the Linux builds use. |
| [`macos-deployment-target.txt`](macos-deployment-target.txt) | The macOS version floors the build steps share. |
| [`pgo-flags.sh`](pgo-flags.sh) | Who may be handed the training profile, decided in one place. |
| [`pgo-staleness.sh`](pgo-staleness.sh) | Shared staleness nudge for the PGO consumers (build-macos.sh, run-bench.sh). |
| [`pgo_localize.py`](pgo_localize.py) | The training profile, keyed for the checkout that compiles it. |
| [`portable-core-lib.sh`](portable-core-lib.sh) | Builds the libUbiquittyCore static archive from the canonical source list. |
| [`portable-core-sources.txt`](portable-core-sources.txt) | The canonical libUbiquittyCore source list. |
| [`run-linux.sh`](run-linux.sh) | Runs a repository command in the arm64 Ubuntu build container, with the working tree mounted. |
| [`run-macos.sh`](run-macos.sh) | Launches the built macOS app, optionally streaming its diagnostics to the terminal. |

## Test

Running the test suites and the harnesses they use. See [Tests](../Tests/README.md) for the suites.

| Script | Purpose |
| --- | --- |
| [`build-core-snapshot.sh`](build-core-snapshot.sh) | The portable snapshot renderer, built and stamped. |
| [`build-coverage-index.sh`](build-coverage-index.sh) | Which tests reach which source file. |
| [`check-grid-cache-differential.py`](check-grid-cache-differential.py) | The grid cache switched on against switched off, frame for frame, over a multi-frame feed on the Swift arm. |
| [`compare-paste-panel.py`](compare-paste-panel.py) | The paste-progress panel against its reference. |
| [`compare-snapshot.py`](compare-snapshot.py) | Say what two snapshot rasters disagree about, with the predicate taken from the image. |
| [`core-srcs-excluded.txt`](core-srcs-excluded.txt) | Portable-Core sources the live-Terminal tests skip. |
| [`coverage-changed-lines.py`](coverage-changed-lines.py) | Changed-line coverage: which lines a branch added that no test reaches. |
| [`coverage-gaps.py`](coverage-gaps.py) | Which Core files no test reaches. |
| [`coverage_index_freshness.py`](coverage_index_freshness.py) | Reports drift in the covering-tests index without rebuilding coverage. |
| [`ctest-shell-lane-census.sh`](ctest-shell-lane-census.sh) | Says how much of the suite an invocation of ctest left out. |
| [`ctest_report.py`](ctest_report.py) | Runs CTest with quiet liveness, JUnit results, and durable diagnostics. |
| [`drive-font-catalogue.swift`](drive-font-catalogue.swift) | Runs the Swift font catalogue against CoreText. |
| [`drive-frame-encoder.swift`](drive-frame-encoder.swift) | The Swift renderer's passes reach the GPU. |
| [`drive-frame-instances.swift`](drive-frame-instances.swift) | Core builds a frame into Swift-owned memory. |
| [`drive-glyph-atlas.swift`](drive-glyph-atlas.swift) | Slots are distinct, stable, and keyed per face. |
| [`drive-glyph-rasterizer.swift`](drive-glyph-rasterizer.swift) | A glyph reaches the coverage plane as ink. |
| [`drive-rich-text.swift`](drive-rich-text.swift) | Driver that renders rich-text blocks through the Swift renderer. |
| [`drive-terminal-renderer.swift`](drive-terminal-renderer.swift) | One render call, cells to pixels. |
| [`FrameEncoderFixture.h`](FrameEncoderFixture.h) | Shared fixture for the frame-encoder driver. |
| [`ios-simulator.sh`](ios-simulator.sh) | The simulator a checkout owns: found or made, booted for the run, and shut down again when the run that booted it ends. |
| [`ios-snapshot-shim.sh`](ios-snapshot-shim.sh) | Renders a snapshot inside the iOS simulator app and copies the PNG out. |
| [`ligature-determinism.py`](ligature-determinism.py) | Holds a busy screen to one rendering of the text that did not change. |
| [`linux-host.sh`](linux-host.sh) | Runs CI's Linux stages on a native Linux host over ssh, from a synced per-run directory that is removed afterwards; the hook's Linux arms use it when `ubiquitty.linuxHost` names a host that answers. |
| [`linux-snapshot-shim.sh`](linux-snapshot-shim.sh) | The Linux renderer, callable from a Mac. |
| [`mutate.py`](mutate.py) | Applies one source mutation for check-mutants.sh. |
| [`record-pty-corpus.sh`](record-pty-corpus.sh) | Re-record the real-program entries of the Sync corpus. |
| [`rename-snapshot-cases.py`](rename-snapshot-cases.py) | Renames snapshot cases with their goldens, waiver lines, and every mention, checking the catalogue before and after. |
| [`render-png.py`](render-png.py) | Renders a PNG from arbitrary text and configuration through a snapshot backend. |
| [`run-coverage.sh`](run-coverage.sh) | Source-based line coverage for the host test suite. |
| [`run-bounded.sh`](run-bounded.sh) | Sourced: the bound every stanza runs under, which stops a timed-out stanza's process group with SIGTERM before SIGKILL, and the HOME of its own under `/tmp` every stanza binary runs with, bounded or not. |
| [`run-ctest.sh`](run-ctest.sh) | Runs one CTest build tree through the shared structured Python reporter. |
| [`run-esctest-linux.sh`](run-esctest-linux.sh) | Run the esctest2 conformance suite against the GTK build. |
| [`run-esctest.sh`](run-esctest.sh) | Run the esctest2 conformance suite against a Ubiquitty build. |
| [`run-fuzzer.py`](run-fuzzer.py) | Runs one libFuzzer invocation with quiet progress and lossless diagnostic logs. |
| [`run-gate-self-tests.py`](run-gate-self-tests.py) | Every gate that advertises a self-test runs it. |
| [`run-ios-protected-file-tests.sh`](run-ios-protected-file-tests.sh) | Builds and runs the iOS protected-file backend tests on a simulator. |
| [`run-ios-smokes.sh`](run-ios-smokes.sh) | Run the iOS shell's --smoke-* battery on a simulator and fail on the first non-passing one. |
| [`run-ios-sync-conformance.sh`](run-ios-sync-conformance.sh) | Builds the Sync wire tests for iOS and runs them on a simulator. |
| [`run-ios-ui-tests.sh`](run-ios-ui-tests.sh) | Build and run the iOS XCUITest suite against the installed app on a simulator. |
| [`run-ios-live-lane.sh`](run-ios-live-lane.sh) | Runs the iOS UI suite's live cases against a private loopback host built from this checkout. |
| [`run-linux-smokes.sh`](run-linux-smokes.sh) | Runs the Linux frontend's headless smoke battery. |
| [`run-sanitized-tests.sh`](run-sanitized-tests.sh) | The CMake-registered test suite under AddressSanitizer and UndefinedBehaviorSanitizer. |
| [`run-tests-stanzas-app.sh`](run-tests-stanzas-app.sh) | The shell stanzas of the app, sourced by run-tests.sh. |
| [`run-tests-stanzas-kit.sh`](run-tests-stanzas-kit.sh) | The shell stanzas of ubiquitty-kit, sourced by run-tests.sh. |
| [`run-tests.sh`](run-tests.sh) | The host suite's single entry point. |
| [`run-ui-tests.sh`](run-ui-tests.sh) | Build + run the in-process UI integration suite. |
| [`snapshot-binary-is-current.py`](snapshot-binary-is-current.py) | Whether a built binary is newer than everything it was actually compiled from. |
| [`stanza-declarations.awk`](stanza-declarations.awk) | The run-tests.sh stanza declaration forms, shared by every inventory. |
| [`swift-driver-typecheck.sh`](swift-driver-typecheck.sh) | The half of a renderer driver that needs no device. |
| [`test-apple-development-teams.py`](test-apple-development-teams.py) | Tests signing discovery with public-metadata fixtures. |
| [`test-build-provenance.sh`](test-build-provenance.sh) | Tests the build-directory freshness guard. |
| [`test-build-version.sh`](test-build-version.sh) | The version rule, against throwaway repositories. |
| [`test-build-wrappers.py`](test-build-wrappers.py) | Tests build wrappers with stand-in tools. |
| [`test-ci-batteries.py`](test-ci-batteries.py) | Exercises CI's command map, fail-fast accounting, and safe reach baselines. |
| [`test-ci-pairing.py`](test-ci-pairing.py) | Tests the reading of a pull request's `Pairs-With:` line for the cross-repository CI jobs. |
| [`test-compare-bench-direction.py`](test-compare-bench-direction.py) | Which way each benchmark unit points, and that the merge of several rounds agrees with the gate about it. |
| [`test-compare-bench-staleness.py`](test-compare-bench-staleness.py) | Tests compare-bench.py's baseline-staleness warnings. |
| [`test-coverage-changed-lines.sh`](test-coverage-changed-lines.sh) | Tests the changed-line coverage gate against a throwaway repository. |
| [`test-esctest-security.py`](test-esctest-security.py) | Tests the conformance runner's policy selection. |
| [`test-font-fanout.py`](test-font-fanout.py) | Compiles the controller's font fan-out bodies with headless Swift doubles. |
| [`test-gate-framework.py`](test-gate-framework.py) | Negative controls for the shared gate reporting. |
| [`test-gate-subjects.py`](test-gate-subjects.py) | Tests probe ownership, isolation, discovery, and refusal in gate_subjects.py. |
| [`test-install-hooks.sh`](test-install-hooks.sh) | The hook install, against a throwaway clone with a linked worktree. |
| [`test-library-roots.py`](test-library-roots.py) | A repository carrying ubiquitty-core's Scripts/ as a library, directly or through its Kit, runs its roots, check-style.py, hooks and push gates from it. |
| [`test-libubiquittycore-package.sh`](test-libubiquittycore-package.sh) | Builds, installs, and consumes the Core and Kit package from a relocated prefix. |
| [`test-linux-host.py`](test-linux-host.py) | Tests the Linux host route against a stand-in ssh and rsync. |
| [`test-personal-paths.py`](test-personal-paths.py) | Tests notice attribution allowances in the privacy gate. |
| [`test-run-hygiene.py`](test-run-hygiene.py) | Checks what a test run leaves behind: processes and files in the user's state directory. |
| [`test-run-hygiene.sh`](test-run-hygiene.sh) | Sourced by tests that start processes, so a run leaves nothing behind. |
| [`test-run-local-hooks.py`](test-run-local-hooks.py) | The clone's own hook checks, against a throwaway clone with a linked worktree. |
| [`test-ui-pasteboard-isolation.sh`](test-ui-pasteboard-isolation.sh) | Tests that UI tests use an isolated pasteboard. |
| [`wasm-smoke.sh`](wasm-smoke.sh) | Proves libUbiquittyCore compiles to wasm32 and runs. |
| [`web-proxy.py`](web-proxy.py) | Bridge a browser WebSocket to the ubiquitty-host socket. |
| [`web-snapshot-shim.mjs`](web-snapshot-shim.mjs) | The browser frontend, rendered from a shell. |

## Gates

The checks the commit hook, the pre-push gate, and CI run. `check-style.py` runs the content gates in one pass.

| Script | Purpose |
| --- | --- |
| [`acronym_rules.py`](acronym_rules.py) | The naming rules check-acronyms applies, as data. |
| [`acronym_scan.py`](acronym_scan.py) | The whole-tree half of check-acronyms. |
| [`check-acronym-rules.py`](check-acronym-rules.py) | Every naming rule is reachable and distinguishable. |
| [`check-acronyms.py`](check-acronyms.py) | WebKit/Cocoa naming: acronyms all-caps, abbreviations spelled out. |
| [`check-adopted-term-cursor.py`](check-adopted-term-cursor.py) | Require adopted Swift surfaces to seed cursor preferences before presenting. |
| [`check-aggregate-cases.py`](check-aggregate-cases.py) | One registered case must not hide many tests. |
| [`check-approved-goldens.py`](check-approved-goldens.py) | An approved golden keeps the bytes it was approved with. |
| [`check-assertions-against-magic-bounds.py`](check-assertions-against-magic-bounds.py) | Catch the assertion that measures the environment instead of the behaviour. |
| [`check-backlog-growth.py`](check-backlog-growth.py) | Did this branch make a tolerated backlog worse? |
| [`check-bench-baseline.sh`](check-bench-baseline.sh) | Do these two commits carry comparable PGO profiles? |
| [`check-bench-compiles.sh`](check-bench-compiles.sh) | Every benchmark translation unit still compiles. |
| [`check-bench-harness-reach.py`](check-bench-harness-reach.py) | Every bench times and reports through the shared harness. |
| [`check-bench-mirrors.py`](check-bench-mirrors.py) | A hand-copied struct still matches the one it copies. |
| [`check-binary-footprint.py`](check-binary-footprint.py) | Report a binary's instruction count. |
| [`check-blink-hint-chokepoint.py`](check-blink-hint-chokepoint.py) | The blink hint and its stamp move together. |
| [`check-boundaries.py`](check-boundaries.py) | The module boundaries the tree is built on. |
| [`check-build-arm.py`](check-build-arm.py) | Name the build a change needs when no portable build compiles its consumer. |
| [`check-build-dirs.py`](check-build-dirs.py) | One build directory configured as two build types. |
| [`check-build-reached.py`](check-build-reached.py) | Did the build compile what your change reaches? |
| [`check-build-root.py`](check-build-root.py) | One build root, spelled the same by both languages. |
| [`check-bundled-fonts.py`](check-bundled-fonts.py) | Every family Core names has its file beside each binary. |
| [`check-capability-ssot.py`](check-capability-ssot.py) | The capability vocabulary has one source of truth. |
| [`check-census-claims.py`](check-census-claims.py) | A count of the tree, written in prose, still holds. |
| [`check-colour-slot-helper.py`](check-colour-slot-helper.py) | Hold every colour-atlas arm to Core's slot-side helper. |
| [`check-comment-file-references.py`](check-comment-file-references.py) | Check that a file named in prose is a file that exists. |
| [`check-comments.sh`](check-comments.sh) | Comment hygiene lint (the no-temporal-comments rule). |
| [`check-compiler-launcher.py`](check-compiler-launcher.py) | Every compile goes through the launcher. |
| [`check-conditional-assertions.py`](check-conditional-assertions.py) | Cases that assert nothing on some path. |
| [`check-config-template-covers-keys.py`](check-config-template-covers-keys.py) | The shipped reference lists the keys. |
| [`check-conformance-claims.py`](check-conformance-claims.py) | Check that the ECMA-48 conformance table agrees with the CSI dispatch table. |
| [`check-core-builder-frames.sh`](check-core-builder-frames.sh) | Drives the built macOS app through the portable builder and fails when a frame contradicts the build's claims. |
| [`check-core-layer-partition.sh`](check-core-layer-partition.sh) | The L0/L1/L2 partition of the portable-core source list, classified by the rules in Docs/package-layers.md. |
| [`check-core-namespaces.sh`](check-core-namespaces.sh) | A Core header declares the namespace its folder names. |
| [`check-core-posix.py`](check-core-posix.py) | Core does not talk to a POSIX kernel. |
| [`check-core-srcs-complete.py`](check-core-srcs-complete.py) | The CORE_SRCS list in run-tests.sh stays reconciled with the portable-Core source list. |
| [`check-core-subset-parity.sh`](check-core-subset-parity.sh) | The Wscons Core trim is the Linux one, and this is what keeps it that way. |
| [`check-core-subset-paths.sh`](check-core-subset-paths.sh) | Every path a target trims must name a real SSOT entry. |
| [`check-core-subset-symbols.py`](check-core-subset-symbols.py) | Refuse a kept translation unit that needs a symbol only a trimmed one defines. |
| [`check-core-subset-trims.py`](check-core-subset-trims.py) | Every path a core subset list names must exist. |
| [`check-coverage-floor.sh`](check-coverage-floor.sh) | Exit non-zero if host coverage drops below documented per-file floors. |
| [`check-doc-references.py`](check-doc-references.py) | A reference doc names code that exists. |
| [`check-docs.py`](check-docs.py) | Documentation hygiene lint for Markdown + TOML. |
| [`check-duplicate-decls.py`](check-duplicate-decls.py) | One declaration, one home. |
| [`check-editor-wire-tables.py`](check-editor-wire-tables.py) | What an editor still writes by hand tracks the wire's headers. |
| [`check-effect-escape-hatch.py`](check-effect-escape-hatch.py) | Count every use of the capability escape hatch. |
| [`check-empty-array-expansion.py`](check-empty-array-expansion.py) | Finds array expansions that abort the script on the bash macOS ships. |
| [`check-enum-switches.py`](check-enum-switches.py) | An exhaustive switch that a new enumerator broke. |
| [`check-enums.sh`](check-enums.sh) | WebKit bool and enum naming gate. |
| [`check-esctest.sh`](check-esctest.sh) | Esctest2's verdict, compared as a SET rather than a count. |
| [`check-file-content-loss.py`](check-file-content-loss.py) | Check that no tracked source file has quietly lost its contents. |
| [`check-findings-on-board.py`](check-findings-on-board.py) | A finding is tracked in a GitHub issue, not in a doc. |
| [`check-folder-map.py`](check-folder-map.py) | Every drawing of the source tree is the source tree. |
| [`check-other-repositories.py`](check-other-repositories.py) | The ledger of other repositories' names lists only names used here, and paths the owner still has. |
| [`check-font-catalogue.sh`](check-font-catalogue.sh) | The Swift font catalogue answers Core correctly. |
| [`check-forks.sh`](check-forks.sh) | One implementation, checked against the symbol tables. |
| [`check-frame-encoder.sh`](check-frame-encoder.sh) | The Swift renderer's passes reach the GPU. |
| [`check-frame-instances.sh`](check-frame-instances.sh) | Core builds a frame into the Swift renderer's memory. |
| [`check-frontend-capability-policy.py`](check-frontend-capability-policy.py) | A frontend that wires a client, or builds a session that does, installs a policy. |
| [`check-frontend-claims.py`](check-frontend-claims.py) | A header that says a frontend uses it is checked. |
| [`check-frontend-default-literals.py`](check-frontend-default-literals.py) | A frontend must not spell a default Core owns. |
| [`check-gate-coverage.py`](check-gate-coverage.py) | A gate the commit runs is a gate the push runs, or it says why not. |
| [`check-gate-scopes.py`](check-gate-scopes.py) | A scoped gate declares every input it reaches. |
| [`check-gate-subjects.sh`](check-gate-subjects.sh) | Proves tree-scoped gates discover untracked violations. |
| [`check-gate-verdict-cache.sh`](check-gate-verdict-cache.sh) | The cache's key is checked, not trusted. |
| [`check-gates-invoked.py`](check-gates-invoked.py) | Check that every gate in Scripts is invoked by something. |
| [`check-git-isolation.py`](check-git-isolation.py) | A destructive git command names its tree and gets it. |
| [`check-glyph-atlas.sh`](check-glyph-atlas.sh) | Slots are distinct, stable, and keyed per face. |
| [`check-glyph-backend-claims.py`](check-glyph-backend-claims.py) | A backend that leaves a lossy entry null has to say which one, and where. |
| [`check-glyph-rasterizer.sh`](check-glyph-rasterizer.sh) | A glyph reaches the coverage plane as ink. |
| [`check-glyph-slot-arms.py`](check-glyph-slot-arms.py) | Each frontend declares which glyph path it drives. |
| [`check-graphemebreak-generated.py`](check-graphemebreak-generated.py) | Extended_Pictographic is generated from emoji-data.txt, not carried by hand. |
| [`check-healing-frame-pair.py`](check-healing-frame-pair.py) | Check that a row painted incrementally renders as one painted whole. |
| [`check-hooks-installed.py`](check-hooks-installed.py) | The gates a clone thinks it has are the ones it has. |
| [`check-inflight-depths.py`](check-inflight-depths.py) | The pin window outlasts the frames in flight. |
| [`check-interface-docs.py`](check-interface-docs.py) | Every declared interface carries a DocC comment. |
| [`check-licenses.py`](check-licenses.py) | Checks project license scope and third-party inventory, notices, and pins. |
| [`check-linux-build-prerequisites.sh`](check-linux-build-prerequisites.sh) | Refuse a Linux frontend build that would stop at configure time, and say what to install instead. |
| [`check-linux-smokes.sh`](check-linux-smokes.sh) | Require every GTK smoke to pass, print a verdict, and agree with its exit code. |
| [`check-literal-lengths.py`](check-literal-lengths.py) | A hand-written length beside a literal has to be the literal's length. |
| [`check-literal-qualifiers.py`](check-literal-qualifiers.py) | A namespace qualifier that leaked into a string. |
| [`check-log-writes.py`](check-log-writes.py) | No diagnostic write that aborts the process when it fails. |
| [`check-macos-smokes.py`](check-macos-smokes.py) | Run every headless test mode the macOS binary dispatches. |
| [`check-memcmp-fold.py`](check-memcmp-fold.py) | No memcmp against a literal at a runtime offset into a constant array. |
| [`check-merge-ready.sh`](check-merge-ready.sh) | A branch's own readiness: rebased onto main, clean, index current. |
| [`check-merged-main.sh`](check-merged-main.sh) | Build what the merge actually produced. |
| [`check-message-gates.py`](check-message-gates.py) | Checks canonical frame admission and its actual Core/host dispatch owners. |
| [`check-method-registry.py`](check-method-registry.py) | No operation is exposed outside the registry. |
| [`check-mirror-modules-do-not-parse.py`](check-mirror-modules-do-not-parse.py) | A mirrored Terminal has no parser. |
| [`check-mirrored-enums.py`](check-mirrored-enums.py) | A mirrored enum numbers its enumerators explicitly. |
| [`check-model-lock-holds.py`](check-model-lock-holds.py) | A frontend call that waits on the parser fails here. |
| [`check-mutants.sh`](check-mutants.sh) | Would any test notice if this change were wrong? |
| [`check-name-collisions.py`](check-name-collisions.py) | One name, two namespaces, two values. |
| [`check-optional-bool-reads.py`](check-optional-bool-reads.py) | An `std::optional<bool>` in a boolean context answers ENGAGEMENT, not its value. |
| [`check-osc-payload-view.py`](check-osc-payload-view.py) | The OSC handlers take the payload the dispatcher measured. |
| [`check-pen-idempotency-fields.py`](check-pen-idempotency-fields.py) | Every Pen field a renderer reads is named in the write path's idempotency probe. |
| [`check-personal-paths.py`](check-personal-paths.py) | Public-tree privacy lint. |
| [`check-pgo-applies.py`](check-pgo-applies.py) | How much of the profile the compiler could still use. |
| [`check-pgo-commit-shape.py`](check-pgo-commit-shape.py) | Hold a profile change to its own commit, with its sidecar. |
| [`check-pgo-profile-consumer.sh`](check-pgo-profile-consumer.sh) | The profile is only handed to a reader of it. |
| [`check-preference-guard-suppresses-default.py`](check-preference-guard-suppresses-default.py) | A presence guard around an accessor read suppresses the default it supplies. |
| [`check-preferences-documented.py`](check-preferences-documented.py) | Every setting Core reads is documented in the file the user edits. |
| [`check-preferences-vocabulary.py`](check-preferences-vocabulary.py) | One concept, one word: the store Core reads is Preferences and nothing else. |
| [`check-profile-orphans.py`](check-profile-orphans.py) | Namespaces the PGO profile knows and the tree no longer has. |
| [`check-quic-mandatory.py`](check-quic-mandatory.py) | Shipping native builds include QUIC. |
| [`check-rename-coverage.sh`](check-rename-coverage.sh) | What a semantic rename would actually see. |
| [`check-rename-diff.py`](check-rename-diff.py) | Every line a rename changed is explained by the rename. |
| [`check-renderer-arm-selection.py`](check-renderer-arm-selection.py) | Hold the pre-push arm selection to the arms each change actually needs. |
| [`check-selection-bounds-callers.py`](check-selection-bounds-callers.py) | Hold each frontend to the selection-bounds entry point it is recorded under. |
| [`check-settings-live-apply.py`](check-settings-live-apply.py) | Require live settings changes to reach open windows through one subscription. |
| [`check-settings-schema-live.py`](check-settings-schema-live.py) | A range belongs to a setting that exists. |
| [`check-shared-kit-imports.py`](check-shared-kit-imports.py) | Fail when a shared Apple source imports a module only one of the two arms has. |
| [`check-shell.sh`](check-shell.sh) | Shellcheck over every tracked shell script, gated at zero. |
| [`check-showcase-goldens.py`](check-showcase-goldens.py) | Check that every showcase entry and every golden name the other. |
| [`check-slop.py`](check-slop.py) | Prose that performs understanding instead of carrying it. |
| [`check-snapshot-faces.py`](check-snapshot-faces.py) | Refuse a snapshot profile naming a face nobody ships, or one nobody reads. |
| [`check-snapshot-harness.py`](check-snapshot-harness.py) | Run the snapshot catalog's own consistency tests when the catalog or its loader is staged. |
| [`check-snapshot-prerequisites.py`](check-snapshot-prerequisites.py) | The snapshot binaries exist. |
| [`check-source-file-names.py`](check-source-file-names.py) | Every tracked file under Source/ carries an extension the build can see. |
| [`check-stale-cmake-cache.py`](check-stale-cmake-cache.py) | Check that no build directory holds a cached default the tree has moved past. |
| [`check-standalone-tools-compile.py`](check-standalone-tools-compile.py) | A tool with its own runner still parses. |
| [`check-stanza-comments.py`](check-stanza-comments.py) | Check that a stanza comment in run-tests.sh has a stanza under it. |
| [`check-style.py`](check-style.py) | Every content gate, in one run: the runner and the sections ubiquitty-core owns. |
| [`check-swift-cxx-exports.py`](check-swift-cxx-exports.py) | Assert every RenderKit Swift export naming a C++ interop type survives the seam. |
| [`check-swift-interop.py`](check-swift-interop.py) | The Swift toolchain is reached through one header. |
| [`check-swift-module-compiles.sh`](check-swift-module-compiles.sh) | Compile the Swift renderer from nothing. |
| [`check-swift-modules-depend-on-core.py`](check-swift-modules-depend-on-core.py) | Hold every Swift module that inlines C++ to a declared dependency on it. |
| [`check-swift-renderer.sh`](check-swift-renderer.sh) | Every driver over the Swift renderer, in one run. |
| [`check-swift-sources.py`](check-swift-sources.py) | Fail when a .swift file under Source/ is in no CMake source list, and so is never compiled. |
| [`check-swift-unsafe.py`](check-swift-unsafe.py) | Refuse a new `Unsafe*` or `__unsafe*` spelling in Swift. |
| [`check-sync-catalog-complete.py`](check-sync-catalog-complete.py) | Hold the remote reference to FrameKind names, byte values and directionOf. |
| [`check-sync-memory.py`](check-sync-memory.py) | Builds and runs the reviewed, in-memory Sync regression binaries only. |
| [`check-sync-struct-defaults.py`](check-sync-struct-defaults.py) | Hold the Sync seam's undefaulted scalars to a recorded set. |
| [`check-sync-transport.py`](check-sync-transport.py) | Exercises real Sync carriers with fixture-owned peers, keys, and processes. |
| [`check-terminal-pointer-intent.py`](check-terminal-pointer-intent.py) | A Terminal* that promises nothing. |
| [`check-terminal-renderer.sh`](check-terminal-renderer.sh) | One render call, cells to pixels. |
| [`check-terminfo.sh`](check-terminfo.sh) | The terminfo entry compiles and names what ships. |
| [`check-test-hook-layout.py`](check-test-hook-layout.py) | A test hook never changes a type's layout. |
| [`check-test-only-operations.py`](check-test-only-operations.py) | Name the terminal operations whose only callers are their own tests. |
| [`check-test-reporting.py`](check-test-reporting.py) | Every test reports through the harness. |
| [`check-tests-reaching-the-diff.py`](check-tests-reaching-the-diff.py) | Run the tests this branch can break. |
| [`check-tidy.sh`](check-tidy.sh) | The tree's clang-tidy rules, as a gate and as a fixer. |
| [`check-tracking-area-owners.py`](check-tracking-area-owners.py) | Hold every tracking area's owner to a type that answers AppKit's rollover. |
| [`check-ubiquittykit.sh`](check-ubiquittykit.sh) | An external app consumes UbiquittyKit.framework. |
| [`check-ui-scenarios.py`](check-ui-scenarios.py) | Checks UI scenario results against explicit known failures and flaky cases. |
| [`check-unwired-flags.py`](check-unwired-flags.py) | Report command-line flags whose only occurrence in the tree is their own read. |
| [`check-verdict-lines.py`](check-verdict-lines.py) | A line that says FAILED says what failed. |
| [`check-vocabulary-types.py`](check-vocabulary-types.py) | A pointer and a count are two parameters that can disagree; a C array is a size that binds to nothing. |
| [`check-wasm-oracle.sh`](check-wasm-oracle.sh) | The wasm arm catches a break the host misses, proven freshly rather than cited from history. |
| [`check-web-fixture.py`](check-web-fixture.py) | One browser fixture under Tests/Web, not one per probe. |
| [`check-web-gpu-parity.sh`](check-web-gpu-parity.sh) | Does the browser's GPU path draw what its CPU path draws? |
| [`check-web-host-bytes.py`](check-web-host-bytes.py) | The PAGE never hands socket bytes to the parser. |
| [`check-web-init-order.py`](check-web-init-order.py) | Nothing in terminal.js may reach the wasm terminal before it is created. |
| [`check-web-parser-entries.py`](check-web-parser-entries.py) | The web page's parser is fed locally, never by the host. |
| [`check-wire-tables.py`](check-wire-tables.py) | The editors' wire numbers are the headers'. |
| [`check-wire-version-pins.py`](check-wire-version-pins.py) | What quotes the wire's numbers tracks them. |
| [`check-workflows.sh`](check-workflows.sh) | Lint .github/workflows/*.yml with a pinned actionlint. |
| [`check-wscons-compiles.sh`](check-wscons-compiles.sh) | Type-check the Wscons frontend on a host that has no wscons. |
| [`check-xcode-workspace.py`](check-xcode-workspace.py) | The generated Xcode projects can be built for what Xcode offers. |
| [`ci-linux-image.sh`](ci-linux-image.sh) | Run a CI battery in the pinned Linux image on a GitHub-hosted runner, or directly on a self-hosted one. |
| [`ci-toolchain.sh`](ci-toolchain.sh) | Decide whether a CI job needs to install anything. |
| [`ci_batteries.py`](ci_batteries.py) | Runs CI's ordered, fail-fast batteries and reports every stage. |
| [`ci_pairing.py`](ci_pairing.py) | Names the Kit or app revision a cross-repository CI job builds, from a `Pairs-With:` line. |
| [`ci_reach.py`](ci_reach.py) | Computes CI reach against a fully successful platform-battery checkpoint. |
| [`clang-format-pin`](clang-format-pin) | The clang-format major every C-family formatting check requires. |
| [`core_namespaces.py`](core_namespaces.py) | Which namespace a header declares, by reading the header. |
| [`format-code.sh`](format-code.sh) | Apply repository clang-format style to owned C/H source. |
| [`gate-ledger.core.txt`](gate-ledger.core.txt) | What ubiquitty-core's gates are held to: the counts they must equal, the self-tests the driver depends on, and the gates a person runs. The kit's and the app's ledgers are `gate-ledger.kit.txt` and `gate-ledger.app.txt`. |
| [`gate-report-lib.sh`](gate-report-lib.sh) | One shape for every gate's verdict line. |
| [`gate-verdict-cache.sh`](gate-verdict-cache.sh) | A gate's verdict is a fact about the tree it read. |
| [`gate_corpus.py`](gate_corpus.py) | The files a gate reads, selected the same way every time. |
| [`gate_framework.py`](gate_framework.py) | One shape for a gate: what it examines, what it found, how long it took. |
| [`gate_report.py`](gate_report.py) | One shape for every gate's verdict line. |
| [`gate_subjects.py`](gate_subjects.py) | Proves untracked discovery with absent, valid, and violating controls. |
| [`gate_verdict.py`](gate_verdict.py) | A gate's verdict is a fact about the tree it read. |
| [`hooks-dispatch`](hooks-dispatch) | The dispatcher install-hooks.sh copies under each hook name; it runs the hook from the worktree git is operating on. |
| [`hooks/commit-msg`](hooks/commit-msg) | Checks commit-message wording with check-slop. |
| [`hooks/pre-commit`](hooks/pre-commit) | Runs the staged-file gates: formatting, naming, boundaries, and the content checks, then the clone's own checks. |
| [`hooks/pre-commit-parts/app.sh`](hooks/pre-commit-parts/app.sh) | The pre-commit gates whose subject is the app, sourced by the hook. |
| [`hooks/pre-push`](hooks/pre-push) | Runs pre-push-gates.sh before a push, then the clone's own checks. |
| [`install-hooks.sh`](install-hooks.sh) | Install the repo's git hooks (run once per clone). |
| [`library-scripts.sh`](library-scripts.sh) | Where the libraries an app script builds on are, for shell callers. |
| [`library_scripts.py`](library_scripts.py) | Makes the libraries' modules importable and finds a file, a listing or a grep across the checkouts, for the app's gates. |
| [`license_notices.py`](license_notices.py) | Collects distribution notices from the third-party inventory. |
| [`license_scope.py`](license_scope.py) | Checks file-level license declarations. |
| [`pre-push-cache.sh`](pre-push-cache.sh) | Caches a passing gate's verdict for the exact source snapshot and toolchain. |
| [`pre-push-gates-core.sh`](pre-push-gates-core.sh) | The pre-push gate families ubiquitty-core answers for, and the tiers it runs alone. |
| [`pre-push-gates-lib.sh`](pre-push-gates-lib.sh) | What every repository's pre-push-gates.sh shares: the tier, the runner, the artifact root and the summary. |
| [`pre-push-gates.sh`](pre-push-gates.sh) | Runs CI's gates locally before a push, at a selectable tier. |
| [`pre-push-runner.sh`](pre-push-runner.sh) | The gate runner pre-push-gates.sh drives. |
| [`run-local-hooks.sh`](run-local-hooks.sh) | Runs the checks a clone's owner placed in `$GIT_COMMON_DIR/hooks.d/<hook>/`; a clone with none runs nothing. |
| [`style_sections_app.py`](style_sections_app.py) | check-style.py's sections the app owns. |
| [`style_sections_kit.py`](style_sections_kit.py) | check-style.py's sections ubiquitty-kit owns. |
| [`style_support.py`](style_support.py) | The run check-style.py's sections report into, and the readers they share. |
| [`swift-unsafe-baseline.app.txt`](swift-unsafe-baseline.app.txt) | The recorded `Unsafe*` sites in the app's Swift. |
| [`swift-unsafe-baseline.kit.txt`](swift-unsafe-baseline.kit.txt) | The recorded `Unsafe*` sites in ubiquitty-kit's Swift. |
| [`ubq-roots.sh`](ubq-roots.sh) | Which tree a script judges and where ubiquitty-core and ubiquitty-kit are, for shell callers. |
| [`ubq_roots.py`](ubq_roots.py) | Which tree a script judges and where ubiquitty-core and ubiquitty-kit are, for python callers. |
| [`wire_tables.py`](wire_tables.py) | The wire's numbers as the C++ headers declare them. |

## Benchmarks and diagnostics

Performance measurement. [Performance measurements](../Docs/benchmarks.md) says how to run them and what a published result records.

| Script | Purpose |
| --- | --- |
| [`ablate-sweep.py`](ablate-sweep.py) | Ranks the cell builder's phases by what skipping each one saves. |
| [`alloc-interpose.c`](alloc-interpose.c) | Allocator interposer count-allocations.sh loads. |
| [`bench-ab.py`](bench-ab.py) | An A/B between two bench binaries, normalised against lanes the change cannot reach. |
| [`bench-gate.sh`](bench-gate.sh) | Runs the portable benchmarks and gates on regressions against the external per-machine baseline. |
| [`bench-one-binary-ab.sh`](bench-one-binary-ab.sh) | An interleaved A/B of two code paths inside one binary, chosen by an environment variable. |
| [`bench-render-ab.sh`](bench-render-ab.sh) | A paired A/B of two built app bundles on one render-profile lane. |
| [`bench-shader-ab.sh`](bench-shader-ab.sh) | Time one of the terminal's shading passes on the GPU, two shader revisions against each other. |
| [`bench-transport.py`](bench-transport.py) | What each remote carrier costs and what a person feels. |
| [`build-perf-bench.sh`](build-perf-bench.sh) | Builds the PTY-capable perf tools with shipping flags against the portable Core. |
| [`compare-bench.py`](compare-bench.py) | Diffs two benchmark result files and flags regressions. |
| [`compare-profile.py`](compare-profile.py) | Diffs two latency-profile result files and reports how each distribution moved. |
| [`count-allocations.sh`](count-allocations.sh) | Count every allocation a bench lane group makes, by DYLD interposition. |
| [`dev-instance.sh`](dev-instance.sh) | Sourced helpers that launch this checkout's build pinned to a grid, with its own config and home. |
| [`energy-follow.py`](energy-follow.py) | A live comparison of an energy run still in progress, read from its NDJSON. |
| [`energy-scenarios.py`](energy-scenarios.py) | What the shipping macOS app costs per wall second. |
| [`energy-watch.py`](energy-watch.py) | Samples every running Ubiquitty's energy while a person drives it. |
| [`imgcat.sh`](imgcat.sh) | Displays an image inline with the OSC 1337 image protocol. |
| [`launch-dev-instance.sh`](launch-dev-instance.sh) | A window of this checkout's build for a person to look at, pinned to a grid, with debugging switches. |
| [`measure-energy.py`](measure-energy.py) | Per-process energy accounting for a live app, no root. |
| [`merge-bench-rounds.py`](merge-bench-rounds.py) | Fold several bench-results.json rounds into one by keeping each lane's best value. |
| [`perf-baseline-path.sh`](perf-baseline-path.sh) | Resolves the external directory that holds machine-specific performance baselines. |
| [`profile-gate.sh`](profile-gate.sh) | Runs the portable latency profiles and gates on a tail regression against the external baseline. |
| [`profile-lane.sh`](profile-lane.sh) | Where one bench lane's time goes, by symbol. |
| [`pty-size-probe.sh`](pty-size-probe.sh) | What a program in a pinned window reads as its terminal size, straight from the PTY. |
| [`record-stream.py`](record-stream.py) | Record what a program writes to its terminal, with timing. |
| [`render-profile-gate.sh`](render-profile-gate.sh) | Runs the macOS GPU render profile and gates on a tail regression against the external baseline. |
| [`replay-stream.py`](replay-stream.py) | Feed a recorded byte stream to a terminal, and say how much got through. |
| [`resize-census.sh`](resize-census.sh) | Drives a window through a resize and reads back what the terminal and its program went through. |
| [`run-bench.sh`](run-bench.sh) | Builds and runs the portable benchmarks and emits one JSON result. |
| [`run-parse-scenarios.sh`](run-parse-scenarios.sh) | Times the ubench parse scenarios through the portable Core; needs an optional ubench checkout (`UBENCH_DIR`). |
| [`run-profile.sh`](run-profile.sh) | Builds and runs the portable latency profiler and emits per-operation distributions. |
| [`run-render-profile.sh`](run-render-profile.sh) | Builds the macOS app and runs its headless GPU render profiler. |
| [`run-web-demo.sh`](run-web-demo.sh) | One command from a clean tree to a browser attached to a local host, torn down on exit. |
| [`sample-replay.sh`](sample-replay.sh) | Where a pinned instance spends its CPU on a recorded stream, by call tree. |
| [`sample-resize.sh`](sample-resize.sh) | Where the renderer spends its time while a window zooms, by call tree. |
| [`stamp-bench-baseline.py`](stamp-bench-baseline.py) | Writes a captured run out as a baseline file, with its hardware and provenance. |
| [`winch-probe.py`](winch-probe.py) | Runs as a window's program and records every SIGWINCH it sees, with timing. |

## Release

Signing and packaging. See [Building and releasing](../Docs/building-and-releasing.md).

| Script | Purpose |
| --- | --- |
| [`archive-macos.sh`](archive-macos.sh) | Fetches Sparkle, builds with distribution signing, notarizes and checks the macOS app, then archives the zip, its symbols and a build record outside the build root. |
| [`fetch-sparkle.sh`](fetch-sparkle.sh) | Downloads the pinned Sparkle framework and tools the macOS auto-update build links. |
| [`notarize-macos.sh`](notarize-macos.sh) | Builds with distribution signing, notarizes, staples, verifies, and zips the macOS app. |
| [`package-linux-appimage.sh`](package-linux-appimage.sh) | Builds the Linux frontend and packages it as a self-updating AppImage. |
| [`package-macos.sh`](package-macos.sh) | Zips the signed macOS app. |
| [`release-bundle.sh`](release-bundle.sh) | The bundle identifier an installed or shipped app carries, and the check that refuses any other. |
| [`sign-sparkle.sh`](sign-sparkle.sh) | Code-signs the embedded Sparkle framework inner-to-outer. |
| [`snapshot-clean.sh`](snapshot-clean.sh) | Validates a source export from committed HEAD. |

## Maintenance

Regenerating checked-in tables and assets, profiles, renames, and one-time setup.

| Script | Purpose |
| --- | --- |
| [`apple-development-teams.py`](apple-development-teams.py) | Prints the teams of usable Apple signing certificates without changing the Keychain. |
| [`ast-graph.py`](ast-graph.py) | A class and method graph of the C++ sources, with a naming audit. |
| [`build-encoding-tables.py`](build-encoding-tables.py) | Generates Source/Core/Unicode/Encodings.tables.inc, the legacy-charset maps. |
| [`build-grapheme-tables.py`](build-grapheme-tables.py) | Generate Source/Core/Unicode/GraphemeBreak.tables.inc from the Unicode Character Database (UCD). |
| [`build-normalization-tables.py`](build-normalization-tables.py) | Generate Source/Core/Unicode/Normalize.tables.inc from the Unicode Character Database (UCD). |
| [`build-unicode-tables.py`](build-unicode-tables.py) | Generate Source/Core/Unicode/Width.tables.inc from Unicode Character Database (UCD) source files. |
| [`clean-build-roots.py`](clean-build-roots.py) | The build roots nobody needs, listed, and removed on request. |
| [`embed-builtin-themes.py`](embed-builtin-themes.py) | Embed Source/Core/Themes/builtin-themes.toml as a C string in a generated .cpp. |
| [`export-app-icon.py`](export-app-icon.py) | The app icon's derived files, from the Icon Composer document. |
| [`export-welcome-icon.py`](export-welcome-icon.py) | The Welcome screen's icon, from the Icon Composer document. |
| [`fonts.py`](fonts.py) | The bundled fonts, checked against and refreshed from Resources/fonts/manifest.json. |
| [`gen-golden-themes.py`](gen-golden-themes.py) | Regenerates the golden theme table in Tests/Core/Themes/test_builtin_themes_toml.cpp. |
| [`gen-portable-core-sources.sh`](gen-portable-core-sources.sh) | Regenerate / verify the canonical libUbiquittyCore source list (Scripts/portable-core-sources.txt). |
| [`gen-unifont.py`](gen-unifont.py) | Bakes GNU Unifont into the compressed table in Source/Core/Rendering/UnifontData.{h,cpp}. |
| [`gen-wire-tables.py`](gen-wire-tables.py) | Write the editors' wire numbers from the C++ headers. |
| [`identify-artefact.sh`](identify-artefact.sh) | Says which commit a file's content is, or that it is not committed. |
| [`make-signing-cert.sh`](make-signing-cert.sh) | Create a self-signed code-signing identity for Ubiquitty. |
| [`pgo-retrain-check.sh`](pgo-retrain-check.sh) | Is a retrain due, and the retrain itself, left on a branch. |
| [`pgo-symbol-mass.sh`](pgo-symbol-mass.sh) | What a rename is about to cost the profile. |
| [`pgo-weekly.sh`](pgo-weekly.sh) | The weekly retrain check, run in a clone of its own. |
| [`pgo-train-swift.sh`](pgo-train-swift.sh) | Regenerates the Swift renderer's PGO profile. |
| [`pgo-train.sh`](pgo-train.sh) | Regenerates a target's checked-in PGO training profile in Support/pgo. |
| [`refactor.py`](refactor.py) | Semantic C++ rename driven by clangd. |
| [`where-did-it-go.sh`](where-did-it-go.sh) | A path that no longer exists, and where it went. |
