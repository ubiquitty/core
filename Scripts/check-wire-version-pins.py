#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0
"""check-wire-version-pins.py — what quotes the wire's numbers tracks them.

`Sync/Envelope.h` holds the one wire version this build speaks; two peers
connect only when they speak the same one. A client written in C++ includes
the header. The editor clients read
tables generated from the headers, which a wire-table gate holds current. This
gate holds what that generation cannot cover.

There is one active version: every wire change increments it, and a peer at
any other version is refused. A frozen published identity detects an omitted
increment; it does not admit an older peer. The gate fails when the version
headers name a minimum peer version or a negotiation history.

A body version is only a version if somebody reads it back, so every
`*BodyVersion` constant `Sync/HandoverBody.h` declares has to be both stamped onto a
frame and compared off one.

Protocol references name authoritative version symbols and generated tables,
not the version a particular build currently speaks. DOC_REFERENCES holds those
links; copied current-version claims are rejected. HEADER_CLAIMS holds the
numeric bounds in `Docs/sync-protocol.md` against their header constants. A size
claim names its unit, so the byte constant is compared in the unit the sentence uses
and a constant that does not divide into it fails rather than rounding. A
constant reached through an alias is resolved, not skipped: `maxClipboardServeBytes`
is `Transport::guaranteedOutboundFrameBytes`, which lives in another header, and
a claim pinned to a name that evaluates to nothing would pass against nothing. A
reworded sentence stops matching and fails, so a claim that moves out from
under its check is registered again and never goes unread.

The tables each editor keeps by hand are held to these headers by the gate that
repository owns, the one that holds the editor clients.

Stdlib only.

    python3 Scripts/check-wire-version-pins.py
    python3 Scripts/check-wire-version-pins.py --self-test
"""

import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gate_report  # noqa: E402 — the path above is what makes it importable
import ubq_roots  # noqa: E402
from wire_tables import headerConstants  # noqa: E402

REPO = ubq_roots.subject_root()

HEADER = os.path.join("Source", "Core", "Protocol", "Sync", "NativeAttachment.h")
# Where the wire version is declared.
VERSION_HEADER = os.path.join("Source", "Core", "Protocol", "Sync", "Envelope.h")
# Where a build describes its version to a peer.
BUILD_HEADER = os.path.join("Source", "Core", "Protocol", "Sync", "WireVersion.h")
# Where a body version is declared, and where it is stamped onto a body and
# compared off one.
BODY_HEADER = os.path.join("Source", "Core", "Protocol", "Sync", "HandoverBody.h")
BODY_SOURCE = os.path.join("Source", "Core", "Protocol", "Sync", "HandoverBody.cpp")

# Read alongside the header because the header's ceilings are aliases into it:
# Native clipboard ceilings alias the carrier floor. Remote clipboard payloads,
# paste uploads and envelope bodies each retain their own domain constants.
ALIAS_HEADERS = (os.path.join("Source", "Core", "Transport", "CarrierCapacity.h"),
                 os.path.join("Source", "Core", "Protocol", "Control", "PastePush.h"),
                 os.path.join("Source", "Core", "Protocol", "Sync", "EffectCodec.h"), VERSION_HEADER)

# Version prose points at one source of truth rather than copying the current
# build's version. Exact paths and symbol names remain checked after rewrapping.
DOC_REFERENCES = {
    os.path.join("Docs", "sync-protocol.md"): (
        "`wireVersion`", "Docs/sync.md",
    ),
    os.path.join("Docs", "sync.md"): (
        "`wireVersion`", "`Scripts/check-wire-version-pins.py`",
    ),
}

# These current-build claims are not protocol rules, even when their number
# happens to match today's header. Historical layouts and example versions are
# separate and are not rejected by these patterns.
CURRENT_VERSION_CLAIMS = (
    re.compile(r"speaks Sync wire version\s+(?:\*\*)?\d+", re.I),
    re.compile(r"The host is at\s+version\s+(?:\*\*)?\d+", re.I),
    re.compile(r"`wireVersion`\s+(?:\(currently|is currently)\s+\d+", re.I),
)

# The unit a sentence quotes a constant in: the divisor, and how the verdict
# spells it. A constant the unit does not divide is an offender — a doc saying
# "4 MiB" against 4.5 MiB is wrong in the direction that matters.
ONE = (1, "")
BYTES = (1, " bytes")
MIB = (1024 * 1024, " MiB")

# Prose in a spec, pinned against the header constants it quotes — no client in
# between, because these are numbers every client hard-codes from this document
# rather than from a pin this gate already holds.
#
#   file -> ((sentence, ((header constant, unit), ...)), ...)
#
# The sentence's capture groups are its constants in order. Matching runs over
# the document with whitespace collapsed, so a rewrap is not a rewrite; a reworded
# claim stops matching and fails.
HEADER_CLAIMS = {
    os.path.join("Docs", "sync-protocol.md"): (
        (re.compile(r"frame body is bounded by `maxFrameBodyBytes` \((\d+) bytes\)"),
         (("maxFrameBodyBytes", BYTES),)),
        (re.compile(r"At most (\d+) MiB per upload"),
         (("maxPastePushBytes", MIB),)),
        (re.compile(r"Canonical clipboard reads allow at most (\d+) items and "
                    r"(\d+) MiB of aggregate item bytes; clipboard-write effects allow at most (\d+) MiB"),
         (("maxEffectClipboardItems", ONE), ("maxEffectClipboardReadBytes", MIB),
          ("maxEffectClipboardWriteBytes", MIB))),
        (re.compile(r"at most (\d+) types and (\d+) MiB of aggregate item bytes"),
         (("maxClipboardServeItems", ONE), ("maxClipboardServeBytes", MIB))),
    ),
}

# What a version constant looks like in the header.
HEADER_FORM = re.compile(r"constexpr\s+[\w:]+\s+(\w*[Vv]ersion)\s*=\s*(\d+)")


def read(path):
    try:
        with open(os.path.join(REPO, path), encoding="utf-8", errors="replace") as handle:
            return handle.read()
    except OSError:
        return ""


def headerVersions(text):
    """The authoritative constants, as {name: value}."""
    return {name: int(value) for name, value in HEADER_FORM.findall(text)}


def docReferences(sources, registered=None):
    """Check authoritative references without requiring current-build prose.

    - Parameters:
      - sources: Registered document paths mapped to their contents.
      - registered: The document-to-references table; DOC_REFERENCES by default.
    - Returns: Missing-reference and copied-current-version diagnostics.
    """
    out = []
    for path, references in (DOC_REFERENCES if registered is None else registered).items():
        text = sources.get(path, "")
        if not text:
            out.append("%s is recorded here and could not be read" % path)
            continue
        flat = " ".join(text.split())
        for reference in references:
            if reference not in flat:
                out.append("%s no longer names %s as an authoritative reference" % (path, reference))
        for claim in CURRENT_VERSION_CLAIMS:
            if claim.search(flat):
                out.append("%s copies a current wire version; name its authoritative "
                           "header or generated table instead" % path)
    return out


def headerClaims(constants, sources=None):
    """Numbers a spec quotes, against the header constants they name.

    - Parameters:
      - constants: Resolved authoritative values, including cross-header aliases.
      - sources: In-memory document fixtures, or None to read registered files.
    - Returns: offender strings, empty when every quoted number is the live one.
    """
    out = []
    for path, claims in HEADER_CLAIMS.items():
        text = read(path) if sources is None else sources.get(path, "")
        if not text:
            out.append("%s is recorded here and could not be read" % path)
            continue
        # Collapsed, so a rewrap of the same sentence still reads as that
        # sentence; a rewording still does not.
        flat = " ".join(text.split())
        for sentence, subjects in claims:
            names = ", ".join(name for name, _unit in subjects)
            matches = list(sentence.finditer(flat))
            if not matches:
                out.append("%s no longer carries the sentence quoting %s — re-point "
                           "HEADER_CLAIMS at the wording that replaced it" % (path, names))
                continue
            # A sentence that matches twice checks the first and says nothing
            # about the second, which is a second place to drift.
            if len(matches) > 1:
                out.append("%s carries the sentence quoting %s %d times — narrow the "
                           "wording so the claim names one number"
                           % (path, names, len(matches)))
                continue
            found = matches[0]
            if len(found.groups()) != len(subjects):
                out.append("%s: the sentence quoting %s captures %d number(s) for %d "
                           "constant(s) — the registration and the wording disagree"
                           % (path, subjects[0][0], len(found.groups()), len(subjects)))
                continue
            for quoted, (name, (scale, label)) in zip(found.groups(), subjects):
                live = constants.get(name)
                if live is None:
                    out.append("%s quotes %s, which the headers do not define as a value "
                               "this gate can evaluate" % (path, name))
                elif live % scale:
                    out.append("%s quotes %s in%s, which does not divide %d bytes"
                               % (path, name, label or " whole units", live))
                elif int(quoted) != live // scale:
                    out.append("%s says %s is %d%s, the header has %d%s"
                               % (path, name, int(quoted), label, live // scale, label))
    return out


NAMED_CONSTANT = r"constexpr\s+[\w:]+\s+%s\s*=\s*(\d+)\s*;"
# What a range of versions or a history of them is spelled as.
RETIRED_VERSION_NAMES = re.compile(r"\b(minPeerWireVersion|wireVersionHistory|WireChange|negotiateWireVersion)\b")


def versionFailures(envelopeText, buildText):
    """Returns a report line per way the version headers stop stating one version.

    The wire has one version: two builds connect when they speak the same one,
    and a change edits that version's grammar rather than appending a version
    an older peer is still served at. A minimum peer version or a history of
    versions is the machinery for serving an older peer, so either name
    returning to the headers is reported.

    - Parameters:
      - envelopeText: Envelope.h, which declares `wireVersion`.
      - buildText: WireVersion.h, which describes a build to a peer.
    - Returns: The report lines, empty when the headers state one version.
    """
    lines = []
    if not re.search(NAMED_CONSTANT % "wireVersion", envelopeText):
        lines.append("  %s declares no wireVersion" % VERSION_HEADER)
    for path, text in ((VERSION_HEADER, envelopeText), (BUILD_HEADER, buildText)):
        for name in sorted(set(RETIRED_VERSION_NAMES.findall(text))):
            lines.append("  %s names %s; the wire has one version and serves no older peer" % (path, name))
    return lines


def bodyVersionFailures(headerText, sourceText):
    """Returns a report line per body version that is written and never compared.

    A version byte is only a version if somebody reads it back. ROWDELTA's was
    stamped by the encoder and discarded by both C++ readers while the two
    hand-written clients compared their own copies, so a peer one layout ahead
    was refused by them and taken here — walked field by field at another
    layout's offsets. REQUEST_KEYFRAME's was the same, on a body small enough
    that nobody expected it to grow, which is the body a field gets appended to.

    So the rule is: a `*BodyVersion` constant the header declares has to be both
    stamped and read back against. Reading the constants from the header rather
    than a list means a new frame's version is held to it the day it is named.

    Any comparison counts. IMAGE takes a range — `version < 1 || version >
    imageBodyVersion` — so a peer still writing v1 has its frames decoded rather
    than refused, which is what a layout that is still readable wants. A rule
    demanding equality would have named that frame and pushed it toward refusing
    those peers. What the rule is about is whether the byte is consulted, not how.

    - Parameters:
      - headerText: Sync/HandoverBody.h, which declares the constants.
      - sourceText: Sync/HandoverBody.cpp, which stamps and compares them.
    - Returns: The report lines, empty when every declared version is compared.
    """
    lines = []
    for name in sorted(n for n in headerVersions(headerText) if n.endswith("BodyVersion")):
        if name == "lastPublishedHandoverBodyVersion":
            # A release pin is not a separately encoded layout. It owns the
            # reader floor, which must remain an alias to the published value.
            if not re.search(r"\boldestHandoverBodyVersion\s*=\s*lastPublishedHandoverBodyVersion\s*;", headerText):
                lines.append("  oldestHandoverBodyVersion must retain the last published handover format")
            continue
        # An `oldest…BodyVersion` is the floor a reader still decodes, not a
        # layout any frame is written in: it is held to the comparison alone.
        isFloor = name.startswith("oldest")
        if not isFloor and not re.search(r"(?:putU8|\.u8|\.u16|\.scalar)\([^;)]*\b%s\b" % re.escape(name), sourceText):
            lines.append("  %s is declared and never stamped — a version no frame carries "
                         "says nothing about the frame" % name)
        if not re.search(r"(?:[<>]=?|[!=]=)\s*%s\b" % re.escape(name), sourceText):
            lines.append("  %s is stamped and never read back against — a reader that "
                         "discards it "
                         "takes a peer's frame at another layout's offsets rather than "
                         "refusing it" % name)
    return lines


def selfTest():
    ok = True

    # The rule the two write-only version bytes broke, each in its own frame.
    stampedAndCompared = ("void e() { putU8(out, fooBodyVersion); }\n"
                          "bool d() { if (r.u8() != fooBodyVersion) return false; }")
    cases = [
        ("stamped and compared", "constexpr uint8_t fooBodyVersion = 1;", stampedAndCompared, False),
        # A range check is a comparison: an older layout still decodes.
        ("stamped and range-checked", "constexpr uint8_t fooBodyVersion = 2;",
         "void e() { putU8(out, fooBodyVersion); }\n"
         "bool d() { if (v < 1 || v > fooBodyVersion) return false; }", False),
        ("stamped and discarded", "constexpr uint8_t fooBodyVersion = 1;",
         "void e() { putU8(out, fooBodyVersion); }\nbool d() { (void)r.u8(); }", True),
        ("declared and never stamped", "constexpr uint8_t fooBodyVersion = 1;",
         "bool d() { if (r.u8() != fooBodyVersion) return false; }", True),
        # The oldest layout a reader still decodes is compared, never stamped.
        ("a floor compared", "constexpr uint16_t oldestFooBodyVersion = 1;",
         "bool d() { if (v < oldestFooBodyVersion) return false; }", False),
        ("a floor never compared", "constexpr uint16_t oldestFooBodyVersion = 1;", "", True),
        ("a published handover floor", "constexpr uint16_t lastPublishedHandoverBodyVersion = 35;\n"
         "constexpr uint16_t oldestHandoverBodyVersion = lastPublishedHandoverBodyVersion;", "", False),
        ("a discarded published handover floor", "constexpr uint16_t lastPublishedHandoverBodyVersion = 35;", "", True),
        # A version constant that is not a body version is not this rule's subject.
        ("not a body version", "constexpr uint32_t snapshotVersion = 19;", "", False),
    ]
    for title, header, source, expected in cases:
        reported = len(bodyVersionFailures(header, source)) > 0
        if reported != expected:
            print("self-test: body version case %r reported=%s expected=%s"
                  % (title, reported, expected), file=sys.stderr)
            ok = False

    versions = headerVersions("constexpr uint32_t snapshotVersion = 14;\n"
                              "constexpr uint16_t syncProtocolVersion = 1;")
    if versions != {"snapshotVersion": 14, "syncProtocolVersion": 1}:
        print("self-test: header constants not read, got %s" % versions, file=sys.stderr)
        ok = False

    documents = {path: read(path) for path in DOC_REFERENCES}
    if docReferences(documents):
        print("self-test: the tree's authoritative references fail: %s"
              % "; ".join(docReferences(documents)), file=sys.stderr)
        ok = False
    for path, references in DOC_REFERENCES.items():
        # The gate accepts references alone, independent of today's version.
        referenceOnly = dict(documents, **{path: "\n".join(references)})
        if docReferences(referenceOnly):
            print("self-test: references without a current version failed for %s" % path, file=sys.stderr)
            ok = False
        for reference in references:
            edited = dict(documents, **{path: documents[path].replace(reference, "missing-reference")})
            if not docReferences(edited):
                print("self-test: removing %s from %s passed" % (reference, path), file=sys.stderr)
                ok = False
        if not docReferences(dict(documents, **{path: ""})):
            print("self-test: an unreadable document passed: %s" % path, file=sys.stderr)
            ok = False
        claims = (
            "speaks Sync wire version **4**.", "The host is at\nversion **14**.",
            "`wireVersion` (currently 20) covers the catalog.",
            "`wireVersion` is currently 20. It covers this grammar.",
        )
        for claim in claims:
            edited = dict(documents, **{path: documents[path] + "\n" + claim})
            if not docReferences(edited):
                print("self-test: copied current-version claim passed for %s: %s" % (path, claim), file=sys.stderr)
                ok = False
        examples = documents[path] + "\nA peer from version 6 reads version 10's layout."
        if docReferences(dict(documents, **{path: examples})):
            print("self-test: a historical layout was treated as a current version: %s" % path, file=sys.stderr)
            ok = False

    # A ceiling is a product, and an alias into another header. Both resolve or
    # the claim that quotes them is checked against nothing.
    constants = headerConstants([
        "constexpr std::size_t maxFrameBodyBytes = 65'535;\n"
        "constexpr std::size_t maxEffectClipboardWriteBytes = 1024u * 1024u;\n"
        "constexpr std::size_t maxPastePushBytes = Transport::guaranteedOutboundFrameBytes;\n"
        "inline constexpr std::size_t maxClipboardServeBytes = maxPastePushBytes;\n"
        "constexpr std::size_t opaque = someCall(3);\n",
        "inline constexpr std::size_t guaranteedOutboundFrameBytes = 4u * 1024u * 1024u;\n",
    ])
    if constants.get("maxEffectClipboardWriteBytes") != 1024 * 1024:
        print("self-test: a product of literals did not evaluate, got %s"
              % constants.get("maxEffectClipboardWriteBytes"), file=sys.stderr)
        ok = False
    if constants.get("maxFrameBodyBytes") != 65535:
        print("self-test: the byte-sized frame bound did not evaluate, got %s"
              % constants.get("maxFrameBodyBytes"), file=sys.stderr)
        ok = False
    if constants.get("maxClipboardServeBytes") != 4 * 1024 * 1024:
        print("self-test: an alias chained through a second header did not resolve, got %s"
              % constants.get("maxClipboardServeBytes"), file=sys.stderr)
        ok = False
    if "opaque" in constants:
        print("self-test: a constant computed by a call was given a value", file=sys.stderr)
        ok = False

    # The claims as registered, against the numbers the header carries. The
    # values are read rather than written down: a copy of them here would be the
    # third, in the gate whose whole subject is that a retyped number drifts, and
    # a version bump that correctly moved both header and spec would then fail
    # this arm with a sentence naming the number the copy still held.
    live = dict(headerConstants([read(HEADER)] + [read(path) for path in ALIAS_HEADERS]))
    live.update(headerVersions(read(HEADER) + read(VERSION_HEADER)))
    if headerClaims(live):
        print("self-test: the spec's claims are registered and do not match the header: %s"
              % "; ".join(headerClaims(live)), file=sys.stderr)
        ok = False
    claimDocuments = {path: " ".join(read(path).split()) for path in HEADER_CLAIMS}
    for path, claims in HEADER_CLAIMS.items():
        for sentence, subjects in claims:
            matches = list(sentence.finditer(claimDocuments[path]))
            if len(matches) != 1:
                print("self-test: cannot exercise a claim not present exactly once: %s"
                      % sentence.pattern, file=sys.stderr)
                ok = False
                continue
            phrase = matches[0].group()
            for title, replacement in (("missing", ""), ("duplicate", phrase + " " + phrase)):
                edited = dict(claimDocuments)
                edited[path] = edited[path].replace(phrase, replacement, 1)
                if not headerClaims(live, edited):
                    print("self-test: %s registered claim passed: %s" % (title, phrase), file=sys.stderr)
                    ok = False
            for name, _unit in subjects:
                missing = dict(live)
                missing.pop(name, None)
                if not headerClaims(missing, claimDocuments):
                    print("self-test: an unresolved claim constant passed: %s" % name, file=sys.stderr)
                    ok = False
                changed = dict(live, **{name: live.get(name, 0) + 1})
                if not headerClaims(changed, claimDocuments):
                    print("self-test: a changed claim constant passed: %s" % name, file=sys.stderr)
                    ok = False
    drifted = dict(live, maxPastePushBytes=2 * live.get("maxPastePushBytes", 0))
    # Every registered sentence quoting the drifted constant is reported, and
    # the count is read off the registry so that pinning one more doc is not a
    # second edit here.
    quoting = sum(1 for claims in HEADER_CLAIMS.values() for _, quoted in claims
                  if any(name == "maxPastePushBytes" for name, _ in quoted))
    if quoting < 1 or len(headerClaims(drifted)) != quoting:
        print("self-test: a ceiling away from the spec's number was not "
              "reported, got %s" % headerClaims(drifted), file=sys.stderr)
        ok = False
    # A byte count the doc's unit cannot express is wrong in the direction that
    # matters: it reads as a smaller ceiling than the host enforces.
    if not headerClaims(dict(live, maxPastePushBytes=live.get("maxPastePushBytes", 0) + 1)):
        print("self-test: a ceiling the quoted unit does not divide passed", file=sys.stderr)
        ok = False

    # One version, and nothing that serves another.
    envelope = "inline constexpr std::uint16_t wireVersion = 1;\n"
    versionCases = [
        ("one version", envelope, "", False),
        ("no version at all", "", "", True),
        ("a minimum peer version", envelope + "inline constexpr std::uint16_t minPeerWireVersion = 1;\n", "", True),
        ("a version history", envelope, "inline constexpr std::array<WireVersionEntry, 1> wireVersionHistory {};\n", True),
    ]
    for title, envelopeText, buildText, expected in versionCases:
        reported = len(versionFailures(envelopeText, buildText)) > 0
        if reported != expected:
            print("self-test: version case %r reported=%s expected=%s"
                  % (title, reported, expected), file=sys.stderr)
            ok = False
    if versionFailures(read(VERSION_HEADER), read(BUILD_HEADER)):
        print("self-test: the tree's own version headers fail: %s"
              % "; ".join(versionFailures(read(VERSION_HEADER), read(BUILD_HEADER))), file=sys.stderr)
        ok = False

    print("check-wire-version-pins: self-test %s" % ("ok" if ok else "FAILED"))
    return 0 if ok else 1


def main():
    if "--self-test" in sys.argv[1:]:
        return selfTest()

    versions = headerVersions(read(HEADER) + read(VERSION_HEADER))
    if not versions:
        print("check-wire-version-pins: FAILED — no version constant found in %s, "
              "so every claim below was compared against nothing" % VERSION_HEADER, file=sys.stderr)
        return 1

    constants = headerConstants([read(HEADER)] + [read(path) for path in ALIAS_HEADERS])
    if not constants:
        print("check-wire-version-pins: FAILED — no constant resolved from %s, so every "
              "doc claim below was compared against nothing" % HEADER, file=sys.stderr)
        return 1

    offenders = bodyVersionFailures(read(BODY_HEADER), read(BODY_SOURCE))
    offenders += versionFailures(read(VERSION_HEADER), read(BUILD_HEADER))
    offenders += docReferences({path: read(path) for path in DOC_REFERENCES})
    offenders += headerClaims(constants)

    if offenders:
        status = gate_report.failed("check-wire-version-pins",
                                    "wire number(s) quoted or named that the headers do not hold", offenders)
        print("\n  Version prose names authoritative headers and generated tables; numeric\n"
              "  bounds must agree with those sources.\n"
              "  Correct the reference, bound, or table reported above.",
              file=sys.stderr)
        return status

    # One claim per number a document quotes, since that is what drifts.
    quoted = sum(len(subjects) for claims in HEADER_CLAIMS.values()
                 for _sentence, subjects in claims)
    print("check-wire-version-pins: ok (%d numeric doc claim(s) live, authoritative version "
          "references checked, one wire version stated)" % quoted)
    return 0


if __name__ == "__main__":
    sys.exit(main())
