#!/bin/bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# pgo-train.sh — (re)generate the target's checked-in PGO training profile in
# Support/pgo, consumed by shipping builds unless UBIQUITTY_PGO=OFF.
#
#   ./Scripts/pgo-train.sh
#   UBQ_PGO_TRAIN_OUT=/tmp/arm-a.profdata ./Scripts/pgo-train.sh
#
# The second form writes the profile and its sidecars to the given path
# instead of the tracked one, which is what comparing two profiles needs:
# train each arm somewhere private, then point UBQ_PGO_PROFILE at each in turn.
#
# Builds bench_stack against the portable Core at the SHIPPING flags plus
# -fprofile-generate, runs the fixed-input lane battery once (the training corpus:
# parse/emit/sync/render-build/scrollback — the hot set the perf campaign
# optimized), and merges the raw counters into one profdata. The profile is
# a compact branch/call-frequency summary, not performance evidence. Training
# disables clock-calibrated call repeats and the nonshipping wall-time flood,
# so load cannot change their input weights. Real transport still uses OS-driven
# reads and polling: fixed input counts do not promise byte-identical profiles.
#
# Commit the regenerated profile as its OWN commit: codegen changes with the
# profile, so a retrain folded into an unrelated change contaminates the perf
# timeline's attribution.
set -euo pipefail

# Train with REPO-RELATIVE compile paths: LLVM keys internal-linkage (file-
# static) functions as "<source path as compiled>:<name>", so an absolute-path
# training build embeds this machine's checkout path in the profile — a
# personal-path leak in a committed artifact and a key mismatch for any
# consumer compiling from a different directory. Relative paths keep the
# artifact clean; the external-linkage hot set (class methods) is name-keyed
# and portable regardless.
cd "$(dirname "$0")/.."
case "${UBQ_PGO_PLATFORM:-$(uname -s)}:${UBQ_CI_LINUX_IMAGE_ID:-}" in
    Linux:) exec python3 Scripts/pgo_toolchain.py run "$PWD/Scripts/pgo-train.sh" "$@" ;;
esac
if [ -n "${UBQ_CI_LINUX_IMAGE_ID:-}" ]; then
    export PATH="${UBQ_PGO_LLVM:?release LLVM directory is required}:$PATH"
    export CC=clang CXX=clang++
fi
ROOT="."
CXX="${CXX:-c++}"
# The merged artifact is LLVM's format. Refuse an incompatible compiler before
# building the corpus, and use the same portable tool lookup as its consumers.
# shellcheck source=Scripts/pgo-flags.sh
source "$ROOT/Scripts/pgo-flags.sh"
if ! ubiquitty_compiler_reads_llvm_profile "$CXX"; then
    echo "pgo-train: LLVM profile training requires clang; set CXX=clang++." >&2
    exit 1
fi
PROFDATA="$(python3 - <<'PY'
import sys
sys.path.insert(0, "Scripts")
from pgo_localize import llvm_profdata

tool = llvm_profdata()
if tool is None:
    sys.exit("pgo-train: llvm-profdata not found; install the LLVM tools.")
print(tool[0])
PY
)"
# Launcher plus compiler, unquoted at the call so an absent launcher adds no
# argument. A bare "$CXX" would compile fine and skip ccache without saying so.
CXX_LAUNCHED="${UBQ_COMPILER_LAUNCHER:+$UBQ_COMPILER_LAUNCHER }$CXX"
BUILD="${UBQ_BUILD_ROOT:-.}/BuildPGOTrain"
# Where the merged profile lands. The default is the tracked artifact every
# consumer reads, so without an override the only way to compare two profiles is
# to train, copy the result aside, and `git checkout --` the file — which leaves
# the shared profile replaced for the length of every training run, with nothing
# announcing it to the checkouts building against it. UBQ_PGO_PROFILE already lets
# the four consumers read from anywhere; this is the same knob on the producing
# side, so an A/B never touches Support/pgo at all.
PROFILE="${UBQ_PGO_TRAIN_OUT:-$(ubiquitty_default_pgo_profile "$ROOT" "$CXX")}"
# The override's directory, not the default one: pointing the profile elsewhere
# and creating Support/pgo would fail the merge on a missing path.
mkdir -p "$BUILD" "$(dirname "$PROFILE")"

# shellcheck source=Scripts/portable-core-lib.sh
source "$ROOT/Scripts/portable-core-lib.sh"

# Shipping flags (run-bench.sh parity: -O3 + ThinLTO) + instrumentation. The
# raw-counter directory must be clean or stale shards from an aborted run
# merge into the fresh ones.
RAW_DIRECTORY="$BUILD/profraw"
rm -rf "$RAW_DIRECTORY"
mkdir -p "$RAW_DIRECTORY"
INSTRUMENT_FLAGS="-O3 $(ubiquitty_lto_flag) -DNDEBUG -fprofile-generate=$RAW_DIRECTORY"

echo "pgo-train: building instrumented libUbiquittyCore + bench_stack ..." >&2
build_portable_core_archive "$BUILD" "$INSTRUMENT_FLAGS"
ubiquitty_core_pal_stub_source "$BUILD/bench_pal_stub.cpp"
# shellcheck disable=SC2086
# -I Tests alongside -I Source: the bench drives the glyph seam through the
# same fixture adapter the render tests use, and it is the training corpus's
# render lanes that reach it. build-perf-bench.sh carries both, and a training
# build that carries fewer include roots than the MEASUREMENT build compiles a
# different program than the one the profile is meant to describe.
$CXX_LAUNCHED -std=c++23 $INSTRUMENT_FLAGS -DUBIQUITTY_PGO_TRAINING=1 -fno-exceptions -fno-rtti -pthread \
    -I"$ROOT/Source" \
    -I"$ROOT/Tests" \
    "$ROOT/Tests/Performance/bench_stack.cpp" "$BUILD/bench_pal_stub.cpp" \
    "$PORTABLE_CORE_ARCHIVE" -o "$BUILD/bench_stack_instrumented"

echo "pgo-train: running fixed-input lanes (training counters, not performance evidence) ..." >&2
env -u UBQ_BENCH_STREAM "$BUILD/bench_stack_instrumented" --platform "$(uname -s)-$(uname -m)" \
    --commit "$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)" \
    > "$BUILD/training-run.json"
if ! grep -q '"training_mode": "fixed-input-counts"' "$BUILD/training-run.json"; then
    echo "pgo-train: FAILED — bench_stack did not run with fixed training input counts" >&2
    exit 1
fi

# The corpora battery is the second half of the training corpus. bench_stack's
# parse lanes are ASCII/SGR/scroll shaped: the wide, cluster and multibyte emit
# paths (emitWideCharacter, emitClusterRun, graphemeExtends, writeWideCell) take
# few counts from it, and -fprofile-use reads few as cold. A profile is a claim
# about what the terminal receives, and a terminal receives text.
# shellcheck disable=SC2086
$CXX_LAUNCHED -std=c++23 $INSTRUMENT_FLAGS -fno-exceptions -fno-rtti -pthread \
    -I"$ROOT/Source" \
    -I"$ROOT/Tests" \
    "$ROOT/Tests/Performance/bench_ingest_corpora.cpp" "$BUILD/bench_pal_stub.cpp" \
    "$PORTABLE_CORE_ARCHIVE" -o "$BUILD/bench_corpora_instrumented"
env -u UBQ_BENCH_STREAM "$BUILD/bench_corpora_instrumented" --repetitions 2 > "$BUILD/training-corpora.json"

# One lane per shipping configuration: the list is the claim about what the
# corpus reaches. A configuration with no lane trains to zero counts, and
# -fprofile-use reads zero as cold rather than unknown — the branch is outlined,
# declined for inlining, and laid out off the hot path. The tolerance flags keep
# that silent, so it is asserted here.
REQUIRED_LANES="render_build render_build_subpixel render_build_full render_build_underline parse_plain parse_plain_syncfeed parse_sgr_syncfeed parse_wide_syncfeed parse_emoji_syncfeed parse_charset_syncfeed sync_burst_k8 key_encode parse_charset"
MISSING=""
for lane in $REQUIRED_LANES; do
    grep -q "\"$lane\"" "$BUILD/training-run.json" || MISSING="$MISSING $lane"
done
# The corpora half, asserted the same way and for the same reason.
for lane in ascii latin1_multibyte cjk_wide combining sgr_truecolor legacy_graphics; do
    grep -q "\"$lane\"" "$BUILD/training-corpora.json" || MISSING="$MISSING $lane"
done
if [ -n "$MISSING" ]; then
    echo "pgo-train: FAILED — the corpus never ran:$MISSING" >&2
    echo "pgo-train: a shipping mode with no counts compiles COLD, not neutral." >&2
    echo "pgo-train: add its lane to bench_stack, or drop it from REQUIRED_LANES" >&2
    echo "pgo-train: here if it genuinely stopped shipping." >&2
    exit 1
fi

echo "pgo-train: merging counters ..." >&2
"$PROFDATA" merge -output "$PROFILE" "$RAW_DIRECTORY"/*.profraw

SIZE=$(du -h "$PROFILE" | cut -f1)
echo "pgo-train: wrote $PROFILE ($SIZE)" >&2

# The relative-path discipline above is a hard property of the artifact:
# a committed profile must never carry this machine's paths.
if strings "$PROFILE" | grep -qE '/(Users|home)/[a-z]'; then
    echo "pgo-train: FAILED — absolute home paths leaked into the profile" >&2
    exit 1
fi
# The tree these counters describe, beside the artifact that holds them.
#
# The profile's own commit cannot answer it: a rebase-merge lands the profile on
# top of commits it was trained before, so the file looks newer than its
# contents and every staleness reading comes back clean. Recorded here, at the
# only moment the answer is known.
#
# Derived from the profile path by the same rule pgo-staleness.sh reads it back
# with (`${1%.profdata}.trained-at`), so it follows UBQ_PGO_TRAIN_OUT. A fixed
# path here would let a training run that writes its profile to /tmp still
# overwrite the tracked sidecar — recording that the SHIPPING profile was
# trained at this HEAD when it was not, which every staleness reading would then
# report as no drift.
trainedAt=$(git -C "$ROOT" rev-parse HEAD 2>/dev/null || true)
if [ -n "$trainedAt" ]; then
    printf '%s\n' "$trainedAt" > "${PROFILE%.profdata}.trained-at"
    echo "pgo-train: trained on $trainedAt (recorded beside the profile)" >&2
fi

# The tree the profile describes, beside the commit that named it.
#
# The commit above cannot survive its own landing: it is written here, in a
# worktree, before the merge exists, and a rebase merge rewrites every commit
# it replays. A trained-at sidecar names a commit no clone has, so read alone it
# answers "unknown", silently, with the fallback covering for it.
#
# A tree hash is the content, and the replay preserves content: a profile
# trained here and merged there describes the same Source/ either way. It
# answers "does this profile describe this source tree" outright, where a
# commit count answers a proxy for it.
trainedTree=$(git -C "$ROOT" rev-parse HEAD:Source 2>/dev/null || true)
if [ -n "$trainedTree" ]; then
    printf '%s\n' "$trainedTree" > "${PROFILE%.profdata}.trained-tree"
    echo "pgo-train: describes Source/ tree $trainedTree" >&2
fi

# A profile that still does not apply is a training run that did not work, and
# nothing downstream would say so: every build path suppresses the diagnostic
# that reports a discarded function, so the counters would be lost on every
# build until someone measured for it. Asked here because this is the one moment
# a fresh profile and the tree it describes are both known to be in hand.
echo "pgo-train: checking the profile against the tree it was trained on ..." >&2
python3 "$ROOT/Scripts/pgo_toolchain.py" record "$PROFILE" "$CXX"
# The same run records the drift budget beside the profile: the total this
# fresh profile discards, which the bound of zero below holds at nothing, plus
# the allowance check-pgo-applies.py carries, keyed to the Source/ tree
# recorded above. The pre-push gate compares every later tree's total with it
# and refuses past it, so the number has to be written here, at the one moment
# the baseline is known — a profile without the sidecar is unmeasured there,
# and one keyed to a tree this run did not name would be refused as stale.
recordBudget=""
[ -n "$trainedTree" ] && recordBudget="--record-drift-budget"
# shellcheck disable=SC2086
if ! python3 "$ROOT/Scripts/check-pgo-applies.py" --profile "$PROFILE" --max-discarded 0 $recordBudget --jobs "${UBQ_BUILD_JOBS:-4}" >&2; then
    echo "pgo-train: FAILED — the fresh profile does not apply to this tree" >&2
    exit 1
fi

echo "pgo-train: commit it as its own change — the profile and its sidecars" >&2
echo "pgo-train: (.trained-at, .trained-tree, .drift-budget), nothing else; codegen follows the profile." >&2
