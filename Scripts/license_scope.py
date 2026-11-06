# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Checks file-level license declarations without rewriting upstream material."""

import collections
import fnmatch
import json
import re
import subprocess
from pathlib import Path

MANIFEST = "license-scope.json"
APACHE_LICENSE = "Apache-2.0"
# The license the project grants for its own files; the boundary assigns it, or
# a separately licensed exception, to every first-party path.
PROJECT_LICENSES = (APACHE_LICENSE,)
HEADER_SUFFIXES = {
    ".awk", ".bash", ".c", ".cc", ".cmake", ".cpp", ".desktop", ".el",
    ".css", ".entitlements", ".fish", ".frag", ".h", ".html", ".inc", ".js", ".lua",
    ".md", ".metal", ".mjs", ".mm", ".modulemap", ".py", ".sdef", ".sh",
    ".svg", ".swift", ".toml", ".vert", ".xcprivacy", ".xcworkspacedata", ".yml", ".zsh",
}
HEADER_NAMES = {".clang-format", ".clang-tidy", ".clangd", ".editorconfig", ".gitignore", ".mailmap", "CMakeLists.txt"}
HEADER_PATTERN = re.compile(
    r"^\s*(?://|\#+|;+|--|<!--|/\*|\*)\s*SPDX-License-Identifier:\s*(.*?)\s*(?:\*/|-->)?\s*$")
COPYRIGHT_PATTERN = re.compile(
    r"^\s*(?://|\#+|;+|--|<!--|/\*|\*)\s*(?:SPDX-FileCopyrightText:|Copyright\s|©\s)\S*.+",
    re.IGNORECASE)


def repository_paths(directory):
    """Returns every present tracked or unignored new file, including audit reports.

    Source exports have no Git metadata; their entire tree is the shipped corpus.
    A checkout failure is an error, never an empty successful audit.
    """
    directory = Path(directory)
    if (directory / ".git").exists():
        result = subprocess.run(
            ["git", "-C", str(directory), "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
            capture_output=True, check=True)
        paths = [name.decode("utf-8") for name in result.stdout.split(b"\0") if name]
        # A library mounted as a submodule is a directory in the index: its files
        # belong to its own repository, which audits them.
        return sorted({name for name in paths
                       if (directory / name).is_file() or (directory / name).is_symlink()})
    return sorted(path.relative_to(directory).as_posix() for path in directory.rglob("*")
                  if path.is_file() or path.is_symlink())


def header_declarations(path):
    """Reads bounded leading comments without decoding binary payloads or source literals."""
    with Path(path).open("rb") as handle:
        prefix = handle.read(65536)
    if b"\0" in prefix:
        return [], False
    lines = leading_comments(path, prefix.decode("utf-8", errors="replace").lstrip("\ufeff").splitlines()[:64])
    identifiers = [match.group(1).strip() for line in lines
                   if (match := HEADER_PATTERN.fullmatch(line))]
    return identifiers, any(COPYRIGHT_PATTERN.match(line) for line in lines)


def leading_comments(path, lines):
    """Stops at the first source statement, excluding docstrings and generated templates."""
    path = Path(path)
    suffix = path.with_suffix("").suffix if path.suffix == ".in" else path.suffix
    hash_comments = {".awk", ".bash", ".cmake", ".desktop", ".fish", ".py", ".sh", ".toml", ".yml", ".zsh"}
    xml_comments = {".md", ".html", ".svg", ".plist", ".entitlements", ".sdef", ".xcprivacy", ".xcworkspacedata"}
    if suffix in hash_comments or path.name in HEADER_NAMES or (not suffix and lines and lines[0].startswith("#!")):
        line_markers, block_markers = ("#",), {}
    elif suffix in xml_comments:
        line_markers, block_markers = (), {"<!--": "-->"}
    elif suffix == ".el":
        line_markers, block_markers = (";",), {}
    elif suffix == ".lua":
        line_markers, block_markers = ("--",), {"--[[": "]]"}
    elif supports_header(path.as_posix()):
        line_markers, block_markers = ("//",), {"/*": "*/"}
    else:
        line_markers, block_markers = ("//", "#", ";", "--"), {"/*": "*/", "<!--": "-->"}
    comments = []
    closing = None
    for number, line in enumerate(lines):
        remaining = line.strip()
        if number == 0 and remaining.startswith("#!"):
            continue
        if suffix in {".vert", ".frag"} and re.fullmatch(r"#version\s+\d+(?:\s+\w+)?", remaining):
            continue
        if suffix in xml_comments and (remaining.startswith("<?xml") or remaining.upper().startswith("<!DOCTYPE")):
            if not remaining.endswith(">"):
                break
            continue
        while remaining or closing:
            if closing:
                content, separator, remaining = remaining.partition(closing)
                comments.append("// " + content.lstrip("* ").strip())
                if not separator:
                    break
                closing = None
                remaining = remaining.strip()
                continue
            opening = next((marker for marker in block_markers if remaining.startswith(marker)), None)
            if opening:
                closing = block_markers[opening]
                remaining = remaining[len(opening):].strip()
                continue
            marker = next((marker for marker in line_markers if remaining.startswith(marker)), None)
            if marker:
                comments.append("// " + remaining[len(marker):].lstrip(marker).strip())
                break
            return comments
    return comments


def supports_header(name):
    """Recognizes formats whose leading comments preserve their interpreted content."""
    path = Path(name)
    if path.suffix == ".in":
        return path.with_suffix("").suffix in {".cmake", ".h", ".modulemap", ".plist"}
    return path.name in HEADER_NAMES or path.suffix in HEADER_SUFFIXES


def rule_matches(rule, name):
    """Matches exact paths literally, including bracketed font names, or explicit patterns."""
    if "suffixes" in rule and Path(name).suffix not in rule["suffixes"]:
        return False
    return name in rule.get("paths", []) or any(
        fnmatch.fnmatchcase(name, pattern) for pattern in rule.get("patterns", []))


def boundary_specificity(entry_path, name):
    """Ranks how closely one boundary path claims a file, or returns None.

    An exact file outranks a glob pattern, which outranks a directory prefix;
    within a kind the longer spelling wins. `./` claims only top-level files.
    """
    if entry_path == name:
        return (3, len(entry_path))
    if any(character in entry_path for character in "*?["):
        return (2, len(entry_path)) if fnmatch.fnmatchcase(name, entry_path) else None
    if entry_path == "./":
        return (1, 0) if "/" not in name else None
    if entry_path.endswith("/") and name.startswith(entry_path):
        return (1, len(entry_path))
    return None


def boundary_license(manifest, name):
    """Returns the boundary license for a first-party path, or None when unclassified.

    Two entries that claim a path equally closely with different licenses are
    a manifest defect, reported by load_manifest, and resolve to None here.
    """
    best, licenses = None, set()
    for entry in manifest.get("boundary", []):
        for entry_path in entry["paths"]:
            rank = boundary_specificity(entry_path, name)
            if rank is None or (best is not None and rank < best):
                continue
            if best is None or rank > best:
                best, licenses = rank, set()
            licenses.add(entry["license"])
    return next(iter(licenses)) if len(licenses) == 1 else None


def expression_identifiers(expression):
    """Splits an SPDX expression into its license identifiers."""
    return [token for token in re.findall(r"[A-Za-z0-9.+-]+", expression)
            if token not in {"AND", "OR", "WITH"}]


def load_boundary(manifest, label_prefix):
    """Rejects malformed boundary entries and paths claimed equally by two licenses."""
    failures = []
    boundary = manifest.get("boundary")
    if not isinstance(boundary, list) or not boundary:
        return [f"file-license: {MANIFEST} has no boundary table"]
    claims = {}
    for index, entry in enumerate(boundary):
        label = f"{label_prefix} boundary entry {index + 1}"
        if not isinstance(entry, dict) or entry.keys() != {"paths", "license", "reason"}:
            failures.append(f"file-license: {label} must have exactly paths, license and reason")
            continue
        if not isinstance(entry["license"], str) or not entry["license"].strip():
            failures.append(f"file-license: {label} has no license")
        if not isinstance(entry["reason"], str) or not entry["reason"].strip():
            failures.append(f"file-license: {label} has no rationale")
        if not isinstance(entry["paths"], list) or not entry["paths"] or any(
                not isinstance(value, str) or not value for value in entry["paths"]):
            failures.append(f"file-license: {label} has invalid paths")
            continue
        for entry_path in entry["paths"]:
            if entry_path.startswith("/") or ".." in Path(entry_path).parts:
                failures.append(f"file-license: {label} has a non-repository path: {entry_path}")
            previous = claims.setdefault(entry_path, entry["license"])
            if previous != entry["license"]:
                failures.append(f"file-license: {MANIFEST} boundary assigns {entry_path} both {previous} and {entry['license']}")
    return failures


def load_manifest(directory):
    """Rejects incomplete scopes and references to missing or external license evidence."""
    directory = Path(directory)
    try:
        manifest = json.loads((directory / MANIFEST).read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        return None, [f"file-license: cannot read {MANIFEST}: {error}"]
    if not isinstance(manifest, dict) or manifest.get("version") != 1 or not isinstance(manifest.get("rules"), list):
        return None, [f"file-license: {MANIFEST} has an unsupported schema"]
    failures = load_boundary(manifest, MANIFEST)
    for index, rule in enumerate(manifest["rules"]):
        label = f"{MANIFEST} rule {index + 1}"
        if not isinstance(rule, dict):
            failures.append(f"file-license: {label} must be an object")
            continue
        known_fields = {"paths", "patterns", "suffixes", "mode", "license", "reason", "copyright",
                        "license_files", "review", "payload", "header_licenses"}
        if rule.keys() - known_fields:
            failures.append(f"file-license: {label} has unknown fields: {sorted(rule.keys() - known_fields)}")
        if not isinstance(rule.get("mode"), str) or rule["mode"] not in {"header", "metadata", "upstream"}:
            failures.append(f"file-license: {label} has no valid mode")
            continue
        if not isinstance(rule.get("license"), str) or not rule["license"].strip():
            failures.append(f"file-license: {label} has no license expression")
        if not isinstance(rule.get("reason"), str) or not rule["reason"].strip():
            failures.append(f"file-license: {label} has no scope rationale")
        if not rule.get("paths") and not rule.get("patterns"):
            failures.append(f"file-license: {label} matches no paths")
        invalid_lists = False
        for key in ("paths", "patterns", "suffixes", "license_files", "copyright", "header_licenses"):
            if key in rule and (not isinstance(rule[key], list) or any(not isinstance(value, str) for value in rule[key])):
                failures.append(f"file-license: {label} has invalid {key}")
                invalid_lists = True
        if invalid_lists:
            continue
        if "payload" in rule and not isinstance(rule["payload"], bool):
            failures.append(f"file-license: {label} has an invalid payload flag")
        if rule.get("payload") and rule["mode"] != "metadata":
            failures.append(f"file-license: {label} can mark only metadata as byte-exact payload")
        if rule.get("mode") in {"metadata", "upstream"} and not rule.get("copyright") and not rule.get("license_files"):
            failures.append(f"file-license: {label} lacks copyright attribution or preserved notice files")
        for name in rule.get("paths", []) + rule.get("patterns", []) + rule.get("license_files", []):
            if name.startswith("/") or ".." in Path(name).parts:
                failures.append(f"file-license: {label} has a non-repository path: {name}")
        for name in rule.get("license_files", []):
            path = directory / name
            if not path.is_file() or path.is_symlink():
                failures.append(f"file-license: {label} has missing license evidence: {name}")
    return (None if failures else manifest), failures


def file_license_audit(directory, paths):
    """Accounts for each path through its header or one reviewed metadata scope.

    Upstream scopes preserve original notices. Metadata scopes identify raw
    fixtures and formats that cannot accept comments without changing payloads.
    Unknown formats and overlapping declarations fail rather than inheriting an
    accidental exemption. Unresolved provenance prevents a release audit.
    """
    directory = Path(directory)
    manifest, failures = load_manifest(directory)
    warnings = []
    counts = collections.Counter()
    if manifest is None:
        return failures, warnings, counts
    for name in sorted(set(paths)):
        path = directory / name
        rules = [rule for rule in manifest["rules"] if rule_matches(rule, name)]
        if len(rules) > 1:
            failures.append(f"file-license: {name} has overlapping scope declarations")
            counts["unclassified"] += 1
            continue
        rule = rules[0] if rules else None
        upstream_boundary = name.startswith(("Source/ThirdParty/", "Resources/fonts/", "Docs/third-party-licenses/"))
        generated_table = name.startswith("Source/Core/Unicode/") and name.endswith(".tables.inc")
        if rule is None and (not supports_header(name) or upstream_boundary or generated_table):
            failures.append(f"file-license: {name} needs explicit metadata for its format in {MANIFEST}")
            counts["unclassified"] += 1
            continue
        mode = rule["mode"] if rule else "header"
        boundary = boundary_license(manifest, name)
        if mode != "upstream" and boundary is None:
            failures.append(f"file-license: {name} is outside the license boundary in {MANIFEST}")
            counts["unclassified"] += 1
            continue
        identifier = rule["license"] if rule else boundary
        if rule and mode != "upstream" and boundary not in expression_identifiers(identifier):
            failures.append(f"file-license: {name} is declared {identifier} in {MANIFEST}, "
                            f"but its boundary license is {boundary}")
            continue
        counts[mode] += 1
        if path.is_symlink() or not path.is_file():
            failures.append(f"file-license: {name} is not a regular file; declare its ownership explicitly")
            continue
        if rule and rule.get("review"):
            warning = f"provenance: {rule['review']}"
            if warning not in warnings:
                warnings.append(warning)
        if mode == "metadata" and rule.get("payload"):
            continue
        identifiers, copyright_present = header_declarations(path)
        if mode == "upstream":
            if name not in rule.get("license_files", []) and identifiers and any(
                    value not in rule.get("header_licenses", [identifier]) for value in identifiers):
                failures.append(f"file-license: {name} must preserve its declared upstream license identifier")
            continue
        if mode == "metadata":
            if identifiers and identifiers != [identifier]:
                failures.append(f"file-license: {name} header conflicts with its metadata license {identifier}")
            continue
        if mode == "header" and identifiers != [identifier]:
            failures.append(f"file-license: {name} must declare exactly SPDX-License-Identifier: {identifier}")
        if mode == "header" and not copyright_present:
            failures.append(f"file-license: {name} needs a copyright notice in its leading comments")
    return failures, warnings, counts
