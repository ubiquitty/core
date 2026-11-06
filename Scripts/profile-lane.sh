#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0
#
# profile-lane.sh — where one bench lane's time goes, by symbol.
#
# The question this answers is "what is this function actually bound by", which
# a disassembly cannot answer: static counts of work (divides, spills, bytes
# written, instructions) do not predict wall clock. A count of work is not the
# critical path. This reads the artefact instead.
#
# Two things make the reading attributable:
#
#   * bench_stack --only <group> runs one lane group instead of forty, so the
#     samples are the lane's rather than the suite's. Without it a profile of
#     the whole bench is mostly other people's lanes.
#   * The trace is exported and symbolised here rather than opened in the
#     Instruments UI, so the output is a diffable text table a commit message
#     can quote.
#
# It reports leaf (self) and inclusive weights separately, in the exported
# units. Self weight says where the work lands; inclusive says who asked for it.
# A function with a heavy inclusive weight and a light self weight is a
# dispatcher, not a cost.
#
# Usage:
#   Scripts/profile-lane.sh render                 # profile the render group
#   Scripts/profile-lane.sh render 40              # keep the top 40 symbols
#   UBQ_BENCH=/tmp/bench_arm Scripts/profile-lane.sh render
#
# Prerequisites are refused, not skipped. Instruments ships with Xcode, not the
# command line tools, so a machine without it gets a message rather than an
# empty table — a profile nobody could record must not read like a profile with
# nothing in it.
#
# The default binary is BuildBench/bench_stack, built by
# Scripts/build-perf-bench.sh. Build it the way you mean to measure it: with
# UBIQUITTY_PGO=OFF for a like-for-like arm comparison, or with a profile
# trained on this arm. A PGO binary whose profile predates the change under
# study reports profile staleness as the change's cost.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# Build output lives outside the checkout; see Scripts/build-root.sh.
# shellcheck source=Scripts/build-root.sh
. "$ROOT/Scripts/build-root.sh"
cd "$ROOT"

GROUP="${1:-}"
TOP="${2:-25}"
# --symbol <substring>: annotate that function's disassembly with sample counts,
# instead of listing symbols. It answers "which instructions", which a self-time
# percentage raises and cannot settle. A static count of divides or spills does
# not predict cost.
SYMBOL=""
if [ "${2:-}" = "--symbol" ]; then SYMBOL="${3:-}"; TOP="${4:-25}"; fi
BENCH="${UBQ_BENCH:-$UBQ_BUILD_ROOT/BuildBench/bench_stack}"
# Extra bench arguments, for lengthening a short lane. A lane that finishes in a
# few hundred milliseconds yields tens of stacks, and a percentage off tens of
# stacks resolves nothing — UBQ_BENCH_ARGS="--iters 40" is the fix the warning
# at the end of the report asks for.
read -r -a BENCH_ARGS <<< "${UBQ_BENCH_ARGS:-}"

if [ -z "$GROUP" ]; then
    echo "profile-lane: name a lane group, e.g. 'render'. bench_stack --only <group> selects it." >&2
    echo "profile-lane: groups are the bench's own function names minus 'Benches' — render," >&2
    echo "profile-lane: snapshot, parse, delta, scrollback, reflow, tmux, kitty, sync." >&2
    exit 2
fi

if ! xcrun -f xctrace >/dev/null 2>&1; then
    echo "profile-lane: xctrace is missing — Instruments ships with Xcode, not the CLI tools." >&2
    echo "profile-lane: install Xcode, or run this on a machine that has it. REFUSED rather than" >&2
    echo "profile-lane: reporting an empty profile, which reads the same as a cheap one." >&2
    exit 3
fi

if [ ! -x "$BENCH" ]; then
    echo "profile-lane: $BENCH is missing or not executable." >&2
    echo "profile-lane: build it first: UBIQUITTY_PGO=OFF Scripts/build-perf-bench.sh bench_stack" >&2
    exit 3
fi

if ! "$BENCH" --only "$GROUP" 2>/dev/null | python3 -c '
import json, sys
lanes = json.load(sys.stdin)["benchmarks"]
# machine_control is emitted unconditionally by the harness, so it is not
# evidence that the filter matched anything.
sys.exit(0 if [b for b in lanes if b["name"] != "machine_control"] else 1)
'; then
    echo "profile-lane: --only $GROUP selected no lane. A profile of nothing is not a fast profile." >&2
    exit 4
fi

TRACE="$(mktemp -d)/lane.trace"
echo "profile-lane: recording $GROUP from $(basename "$BENCH") ..." >&2
xcrun xctrace record --template 'CPU Profiler' --output "$TRACE" \
    --target-stdout /dev/null --launch -- "$BENCH" --only "$GROUP" ${BENCH_ARGS[@]+"${BENCH_ARGS[@]}"} >/dev/null 2>&1

# Which table holds the stacks depends on the template: 'CPU Profiler' writes
# cpu-profile, 'CPU Counters' writes time-profile. Try both rather than pin one,
# because an export that silently returns an empty document reads exactly like a
# lane with no samples in it.
XML="$(mktemp)"
for schema in cpu-profile time-profile; do
    xcrun xctrace export --input "$TRACE" \
        --xpath "/trace-toc/run[@number=\"1\"]/data/table[@schema=\"$schema\"]" > "$XML" 2>/dev/null || true
    if grep -Eq "<(tagged-)?backtrace([[:space:]>])" "$XML"; then break; fi
done
if ! grep -Eq "<(tagged-)?backtrace([[:space:]>])" "$XML"; then
    echo "profile-lane: the trace carried no backtraces in cpu-profile or time-profile." >&2
    echo "profile-lane: xctrace's schema names move between Instruments versions; run" >&2
    echo "profile-lane:   xcrun xctrace export --input <trace> --toc | grep schema=" >&2
    echo "profile-lane: and add the one that holds stacks to the loop above." >&2
    exit 4
fi

python3 - "$XML" "$TOP" "$GROUP" "$BENCH" "$SYMBOL" <<'PY'
import collections, os, re, subprocess, sys
import xml.etree.ElementTree as ET

xml, top, group = open(sys.argv[1]).read(), int(sys.argv[2]), sys.argv[3]
binary, want = sys.argv[4], sys.argv[5]

# Every row is a sample, including rows that refer to an earlier stack. Both
# CPU Profiler and older time-profile exports intern frames, stacks and weights.
# Count weights in their exported units; cycle weights are not elapsed time.
def profileRows(document):
    root = ET.fromstring(document)
    definitions = {}
    for element in root.iter():
        identity = element.get("id")
        if identity is not None:
            if identity in definitions:
                raise ValueError(f"duplicate XML identity {identity}")
            definitions[identity] = element

    def resolve(element):
        seen = set()
        while element is not None and "ref" in element.attrib:
            identity = element.get("ref")
            if identity in seen:
                raise ValueError(f"cyclic XML reference {identity}")
            if identity not in definitions:
                raise ValueError(f"unresolved XML reference {identity}")
            seen.add(identity)
            element = definitions[identity]
        return element

    result, kinds = [], set()
    for node in root.findall("node"):
        schema = node.find("schema")
        if schema is None:
            continue
        columns = [(column.findtext("mnemonic"), column.findtext("engineering-type"))
                   for column in schema.findall("col")]
        stackIndex = next((i for i, (_, kind) in enumerate(columns)
                           if kind in ("backtrace", "tagged-backtrace")), None)
        weightIndex = next((i for i, (name, _) in enumerate(columns) if name == "weight"), None)
        if stackIndex is None:
            continue
        for row in node.findall("row"):
            values = list(row)
            if len(values) != len(columns):
                raise ValueError(f"expected {len(columns)} columns, got {len(values)}")
            stack = resolve(values[stackIndex])
            frames = [resolve(frame) for frame in stack.iter("frame")]
            weightElement = resolve(values[weightIndex]) if weightIndex is not None else None
            weight = int(weightElement.text) if weightElement is not None else 1
            if weight < 0:
                raise ValueError("negative sample weight")
            kinds.add(weightElement.tag if weightElement is not None else "sample-count")
            result.append((frames, weight))
    if len(kinds) > 1:
        raise ValueError(f"cannot combine incompatible sample weights: {sorted(kinds)}")
    if not any(frames for frames, _ in result):
        raise ValueError(f"the trace held no stacks for {group}")
    return result, next(iter(kinds))


try:
    rows, weightKind = profileRows(xml)
except (ValueError, TypeError, ET.ParseError) as error:
    print(f"profile-lane: {error}.", file=sys.stderr)
    raise SystemExit(4)

leafSample: list[tuple[str, int, int]] = []
leaf: collections.Counter[str] = collections.Counter()
stacks: list[tuple[list[str], int]] = []
samples = len(rows)
totalWeight = sum(weight for _, weight in rows)
if totalWeight <= 0:
    print("profile-lane: the trace held no positive sample weight.", file=sys.stderr)
    raise SystemExit(4)
for frames, weight in rows:
    names = [frame.get("name", "?") for frame in frames] or ["<no stack>"]
    leaf[names[0]] += weight                 # ordered leaf-first
    stacks.append((names, weight))
    if frames and frames[0].get("addr"):
        leafSample.append((names[0], int(frames[0].get("addr"), 16), weight))


def display(sym: str) -> str:
    # A bare address means the frame never symbolised — say so rather than print
    # a hex number that reads like a function nobody can find.
    if not sym or re.fullmatch(r"0x[0-9a-f]+", sym):
        return "<unsymbolicated>"
    # Strip the trailing parameter list by matching parens from the end. A blind
    # cut at the first "(" erases the whole name of anything in an anonymous
    # namespace, which is most of this builder's helpers.
    # Trailing qualifiers first: a const member function does not end in ")".
    sym = re.sub(r"(?:\s+(?:const|volatile|noexcept|&&?))+$", "", sym.strip())
    if sym.endswith(")"):
        depth = 0
        for i in range(len(sym) - 1, -1, -1):
            depth += (sym[i] == ")") - (sym[i] == "(")
            if depth == 0:
                sym = sym[:i]
                break
    sym = sym.replace("(anonymous namespace)::", "")
    return sym.rsplit("::", 1)[-1] if "::" in sym else sym or "<unsymbolicated>"


# One symbol can appear under several mangled entries — the optimiser splits a
# function into hot and cold parts, and each carries the same name. Folding them
# is what makes the percentages answer "how much is this function", which is the
# question; leaving them split reports one function as three small ones.
def fold(counter):
    out = collections.Counter()
    for sym, n in counter.items():
        out[display(sym)] += n
    return out


leafByName = fold(leaf)
# Deduped per stack once the fold has happened, so a function split into hot and
# cold halves counts once for the stack it appears in rather than once per half.
inclByName: collections.Counter[str] = collections.Counter()
for frames, weight in stacks:
    for name in dict.fromkeys(display(f) for f in frames):
        inclByName[name] += weight

if want:
    # The slide: xctrace reports where the image loaded, the binary's symbol
    # table reports where the linker put the symbol. Offsets must be anchored to
    # the SYMBOL's base — anchoring to the lowest sampled address instead is the
    # mistake that makes every offset plausible and wrong, and it does not
    # announce itself, because the disassembly at the wrong offset is still
    # disassembly.
    # Pick the load address by binary name. The first <binary> in the export is
    # dyld, and its slide resolves every offset to somewhere plausible inside a
    # function that took no samples.
    base = os.path.basename(binary)
    slide = None
    for tag in re.findall(r"<binary\b[^>]*>", xml):
        nameAttr = re.search(r'\bname="([^"]*)"', tag)
        loadAttr = re.search(r'\bload-addr="(0x[0-9a-f]+)"', tag)
        if nameAttr and loadAttr and nameAttr.group(1) == base:
            slide = int(loadAttr.group(1), 16) - 0x100000000
            break
    if slide is None:
        print(f"profile-lane: the export names no load address for {base}.", file=sys.stderr)
        raise SystemExit(4)

    # Match on the DEMANGLED name at both ends: the trace's frames are
    # demangled and nm's are not, so a substring that finds one misses the other.
    nm = subprocess.run(["nm", binary], capture_output=True, text=True).stdout
    rows = [ln.split() for ln in nm.splitlines()]
    rows = [r for r in rows if len(r) >= 3 and r[1] in ("t", "T")]
    plain = subprocess.run(["c++filt"], input="\n".join(r[2] for r in rows),
                           capture_output=True, text=True).stdout.splitlines()
    hits = [r for r, human in zip(rows, plain) if want in human or want in r[2]]
    if not hits:
        print(f"profile-lane: no symbol in {binary} contains {want!r}.", file=sys.stderr)
        raise SystemExit(4)
    mangled, symBase = hits[0][2], int(hits[0][0], 16)
    humanName = subprocess.run(["c++filt"], input=mangled, capture_output=True, text=True).stdout.strip()
    counts: collections.Counter[int] = collections.Counter()
    for nmOfSample, ad, weight in leafSample:
        if want in nmOfSample:
            # Back to a binary address, then down to the instruction. Every
            # sampled address here arrives one byte past a 4-byte boundary, so
            # an exact match against the disassembly finds nothing at all —
            # ranges that overlap, an intersection of zero, and a table that
            # prints no rows while claiming every sample landed elsewhere.
            counts[(ad - slide) & ~3] += weight
    if not counts:
        print(f"profile-lane: {want} took no samples in this lane.", file=sys.stderr)
        raise SystemExit(4)

    dis = subprocess.run(["objdump", "-d", "--no-show-raw-insn",
                          f"--disassemble-symbols={mangled}", binary],
                         capture_output=True, text=True).stdout
    total = sum(counts.values())
    print(f"\nlane group   {group}")
    print(f"symbol       {display(humanName)}  ({total} of {totalWeight} {weightKind}, {100 * total / totalWeight:.1f}%)")
    print(f"base         0x{symBase:x} in the binary, slide 0x{slide:x}\n")
    print(f"{weightKind:>12} {'off':>7}  instruction")
    print(f"{'-' * 12} {'-' * 7}  {'-' * 58}")
    shown = 0
    for ln in dis.splitlines():
        mm = re.match(r"^\s*([0-9a-f]+):\s+(.*)$", ln)
        if not mm:
            continue
        at = int(mm.group(1), 16)
        n = counts.get(at, 0)
        if n:
            print(f"{n:>12} {at - symBase:>+7}  {re.sub(r'<[^>]*>', '', mm.group(2)).strip()[:58]}")
            shown += n
    if shown < total:
        print(f"\n{total - shown} {weightKind} landed outside the disassembled symbol — "
              f"an inlined copy elsewhere, or a second entry under the same name.")
    raise SystemExit(0)

print(f"\nlane group   {group}")
print(f"samples      {samples} stacks")
print(f"weight       {totalWeight} {weightKind}; percentages use these units, not elapsed wall time")
# One sample is the smallest thing this report can show, so a row near it is a
# coin flip rather than a cost. Said in the report because a percentage prints
# to one decimal whatever it rests on, and a 4.2% row off 24 stacks reads
# exactly like a 4.2% row off 2,400.
if weightKind == "sample-count":
    resolution = 100.0 / samples
    print(f"resolution   one sample is {resolution:.2f}% — nothing below that is separable from zero")
if samples < 400:
    print(f"             {samples} stacks is thin for attribution. Lengthen the lane:")
    print(f'             UBQ_BENCH_ARGS="--iters 40" ./Scripts/profile-lane.sh {group}')
print()
print(f"{'self%':>7} {'incl%':>7}  symbol")
print(f"{'-' * 7} {'-' * 7}  {'-' * 62}")
for sym, n in leafByName.most_common(top):
    print(f"{100 * n / totalWeight:>6.1f}% {100 * inclByName[sym] / totalWeight:>6.1f}%  {sym[:62]}")

# Nearest OWNED frame per stack. A leaf histogram spreads one call site across
# whatever it happens to run, so an expensive caller reads as a flat tail of
# library symbols and no target. Walking each stack out to the first frame
# this project owns puts that call back together.
ownedPrefixes = ("Ubiquitty", "-[UB", "+[UB", "UbiquittyRenderKit", "SwiftFrameDrive",
                 "LigatureSheet", "GlyphAtlas", "TerminalSurface", "TerminalRenderer",
                 "FrameInstances")


def isOwned(sym: str) -> bool:
    return any(sym.startswith(p) or p in sym for p in ownedPrefixes)


ownerByName: collections.Counter[str] = collections.Counter()
for frames, weight in stacks:
    for f in frames:
        if isOwned(f):
            ownerByName[display(f)] += weight
            break
    else:
        ownerByName["<no owned frame>"] += weight

deep = [(s, c) for s, c in inclByName.most_common(top)
        if s != "<unsymbolicated>" and leafByName[s] / max(c, 1) < 0.10 and c / totalWeight > 0.05]
if deep:
    print("\ncallers of the cost above, not the cost (under 10% of their inclusive time is their own):")
    for sym, c in deep[:8]:
        print(f"{100 * leafByName[sym] / totalWeight:>6.1f}% {100 * c / totalWeight:>6.1f}%  {sym[:62]}")

print("\nby nearest frame this project owns — one call site, however many library")
print("symbols it runs. Read beside the leaf table, not instead of it: a leaf")
print("histogram finds a hot helper, this finds a hot caller, and neither finds both.")
print(f"{'owned%':>7}  symbol")
print(f"{'-' * 7}  {'-' * 62}")
for sym, n in ownerByName.most_common(top):
    print(f"{100 * n / totalWeight:>6.1f}%  {sym[:62]}")
PY

rm -rf "$(dirname "$TRACE")" "$XML"
