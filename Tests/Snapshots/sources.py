# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Resolve the snapshot catalog's data without copying owner repositories."""

from pathlib import Path


class SnapshotSources:
    """Core defines cases; every configured repository retains its own data."""

    def __init__(self, roots, repository_roles):
        self.context = roots
        self.core = roots.core.resolve()
        self.subject = roots.subject.resolve()
        self.roots = tuple(dict.fromkeys(root.resolve() for root in
                                        (roots.core, roots.kit, roots.subject) if root))
        self.directories = tuple(root / "Tests" / "Snapshots" for root in self.roots)
        required = {
            "core": ("cases.json", "configs/showcase.toml", "known-failing-portable.txt", "exempt-portable.txt"),
            "kit": ("known-failing-macos.txt", "known-failing-swift.txt", "exempt-macos.txt", "exempt-swift.txt", "exempt-ios.txt"),
            "app": ("known-failing-linux.txt", "known-failing-web.txt", "exempt-linux.txt", "exempt-web.txt", "approved.tsv"),
        }
        for root in self.roots:
            for role in repository_roles(root):
                for relative in required[role]:
                    path = root / "Tests" / "Snapshots" / relative
                    if not path.is_file():
                        raise ValueError(f"{role} snapshot data is missing: {path}")
        for root in self.roots:
            candidate = root / "Tests" / "Snapshots" / "cases.json"
            if root != self.core and candidate.exists():
                raise ValueError(f"snapshot case definitions must belong to Core, also found {candidate}")
        if not (self.core / "Tools" / "showcase" / "showcase.py").is_file():
            raise ValueError(f"Core showcase definitions are missing under {self.core}")

    def path(self, relative, *, required=False):
        """Return one exact owner, refusing collisions and path escapes."""
        relative = Path(relative)
        if relative.is_absolute() or ".." in relative.parts:
            raise ValueError(f"snapshot data must be a confined relative path: {relative}")
        found = []
        for directory in self.directories:
            candidate = directory / relative
            if not candidate.exists() and not candidate.is_symlink():
                continue
            resolved = candidate.resolve()
            if not resolved.is_relative_to(directory.resolve()) or not candidate.is_file():
                raise ValueError(f"snapshot data is not a confined regular file: {candidate}")
            found.append(candidate)
        if len(found) > 1:
            raise ValueError(f"snapshot data has ambiguous owners for {relative}: " + ", ".join(map(str, found)))
        if found:
            return found[0]
        if required:
            raise ValueError(f"snapshot data is missing: {relative}")
        # A missing golden still belongs to the directory its owner keeps.
        # The existing baseline rules decide whether that missing picture is
        # required; this resolver must not weaken those comparison rules.
        parents = [directory for directory in self.directories if (directory / relative.parent).is_dir()] if relative.parts[:1] == ("golden",) else []
        if len(parents) > 1:
            raise ValueError(f"snapshot destination has ambiguous owners for {relative}")
        return (parents[0] if parents else self.core / "Tests" / "Snapshots") / relative

    def manifests(self, pattern):
        """Read every owner's policy file once; duplicate names are errors."""
        names = {path.name for directory in self.directories for path in directory.glob(pattern)}
        return [self.path(name, required=True) for name in sorted(names)]

    def repository_file(self, relative):
        """Use the shared entry-point policy, including consumer wrappers."""
        return self.context.resolve_file(relative)
