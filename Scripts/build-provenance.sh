#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# build-provenance.sh — shared build-directory freshness guard for the CMake
# frontends (build-host.sh, build-agent.sh and the macOS app's build). Sourced,
# not executed.
#
# An incremental build directory carries objects from whatever worktree state
# produced them. When that state differs from the current one, a link failure
# names symbols from the older objects, so the failure describes neither tree
# and attributing it to HEAD is wrong. The stamp records the revision and the
# worktree delta a build directory was last built from;
# build_provenance_warning compares it against the current state and refuses
# the attribution when they disagree. It is a notice, never a failure — the
# build proceeds either way. Non-git trees and unstamped directories are
# silent, because there is no claim to contradict.
#
# The build directory must be ignored by git (.gitignore carries build/), or
# the stamp lands inside the delta it measures and every build reports itself
# stale.

# _build_provenance_libraries <repository-root>
# Prints the checkouts of the libraries a repository carries, one per line: the
# submodules under Libraries/ (Core inside the Kit where the repository carries
# the Kit, as Scripts/ubq-roots.sh finds it), or where UBQ_CORE_ROOT and
# UBQ_KIT_ROOT name them. A repository's own git sees a submodule as the commit
# it pins, so the bytes of a library are fingerprinted in its own checkout.
_build_provenance_libraries() {
    local repositoryRoot="$1" library core="$1/Libraries/ubiquitty-core"
    [ -e "$repositoryRoot/Libraries/ubiquitty-kit" ] && core="$repositoryRoot/Libraries/ubiquitty-kit/Libraries/ubiquitty-core"
    for library in "${UBQ_CORE_ROOT:-$core}" \
                   "${UBQ_KIT_ROOT:-$repositoryRoot/Libraries/ubiquitty-kit}"; do
        [ -e "$library/.git" ] && [ "$library" != "$repositoryRoot" ] && printf '%s\n' "$library"
    done
    return 0
}

# _build_provenance_git <checkout> <git arguments...>
# git in a checkout, without the variables a hook gives git to name the repository
# being committed or pushed.
_build_provenance_git() {
    local checkout="$1"
    shift
    env -u GIT_DIR -u GIT_WORK_TREE -u GIT_INDEX_FILE -u GIT_PREFIX -u GIT_COMMON_DIR \
        -u GIT_OBJECT_DIRECTORY -u GIT_ALTERNATE_OBJECT_DIRECTORIES -u GIT_QUARANTINE_PATH \
        git -C "$checkout" "$@"
}

# _build_provenance_state <repository-root>
# Prints "<head-sha> <worktree-delta-digest>" for the current tree. The digest
# covers staged and unstaged tracked changes, so stashing or restoring work
# moves it even when HEAD does not.
_build_provenance_state() {
    local repositoryRoot="$1" headRevision deltaDigest
    local library
    headRevision=$(git -C "$repositoryRoot" rev-parse HEAD 2>/dev/null) || return 1
    deltaDigest=$({
        git -C "$repositoryRoot" diff HEAD --binary 2>/dev/null
        while IFS= read -r library; do
            [ -n "$library" ] || continue
            printf 'library %s\n' "$library"
            _build_provenance_git "$library" rev-parse HEAD 2>/dev/null
            _build_provenance_git "$library" diff HEAD --binary 2>/dev/null
        done < <(_build_provenance_libraries "$repositoryRoot")
    } | shasum | cut -d' ' -f1) || return 1
    echo "$headRevision ${deltaDigest:-nodelta}"
}

# build_provenance_stamp <repository-root> <build-directory>
# Records the state the build directory now holds objects for. Call after a
# successful build.
build_provenance_stamp() {
    local repositoryRoot="$1" buildDirectory="$2" state compiledState
    [ -d "$buildDirectory" ] || return 0
    state=$(_build_provenance_state "$repositoryRoot") || return 0
    compiledState=$(_build_provenance_compiled_state "$repositoryRoot" "$buildDirectory") || compiledState=unavailable
    printf '%s\n%s\n' "$state" "$compiledState" > "$buildDirectory/.ubiquitty-build-provenance"
    # Spotlight indexes a build directory the same as source, and the object
    # trees of every checkout one machine carries are large. The cost is not
    # only the walk: quic-deps vendors aws-lc, which ships its FIPS
    # certification documents as PDFs, and every one indexed spawns a
    # CoreGraphics PDF service to parse it. Those services stay resident and
    # respawn, spending CPU on documents no one will ever search.
    #
    # Written by the same function that stamps the directory, because both
    # answer "this tree is build output": a marker placed by hand does not
    # survive the rm -rf that a clean rebuild starts with.
    : > "$buildDirectory/.metadata_never_index" 2>/dev/null || true
}

# build_provenance_require <repository-root> <build-directory> <label> <build-command>
# Refuses, with exit 2, when the build directory holds objects from a different
# worktree state than the current one.
#
# Linked archives must agree with the headers the consumer sees. Content, not
# the revision or index state, determines that agreement: staging or committing
# bytes does not change the objects a successful build produced. The scope is
# the build's source trees and configuration inputs, so editing a frontend the
# archive does not compile cannot invalidate an otherwise current archive.
build_provenance_require() {
    local repositoryRoot="$1" buildDirectory="$2" label="$3" buildCommand="$4"
    build_provenance_current "$repositoryRoot" "$buildDirectory" "$label" ||
        _build_provenance_refuse "$label" "$buildCommand"
}

# build_provenance_current <repository-root> <build-directory> <label>
# Succeeds when the build directory is unstamped or was stamped from the current
# compiled inputs; otherwise says which on stderr and fails. The check
# build_provenance_require refuses on, for a caller whose consequence of a stale
# directory is not a link against today's headers.
build_provenance_current() {
    local repositoryRoot="$1" buildDirectory="$2" label="$3"
    local stampFile="$buildDirectory/.ubiquitty-build-provenance"
    local recordedState currentState

    [ -f "$stampFile" ] || return 0
    recordedState=$(sed -n '2p' "$stampFile") || recordedState=
    if [[ ! "$recordedState" =~ ^[0-9a-f]{64}$ ]]; then
        echo "$label: $buildDirectory has no usable compiled-input fingerprint." >&2
        return 1
    fi
    if ! currentState=$(_build_provenance_compiled_state "$repositoryRoot" "$buildDirectory"); then
        echo "$label: cannot fingerprint the inputs compiled by $buildDirectory." >&2
        return 1
    fi
    [ "$recordedState" != "$currentState" ] || return 0
    echo "$label: $buildDirectory holds objects built from different source or configuration bytes." >&2
    return 1
}

# _build_provenance_compiled_state <repository-root> <build-directory>
# Includes paths and raw file contents, but not staging state, mtimes, or commit
# identity. Git supplies the source inventory; it never writes the index or
# object database. NUL delimiters preserve every legal source filename, and
# batched hashing avoids one process per source file.
_build_provenance_compiled_state() (
    set -o pipefail
    local repositoryRoot="$1" buildDirectory="$2" compiledTrees manifest path graphFile
    local paths=()
    compiledTrees=$(_build_provenance_compiled_trees "$buildDirectory" "$repositoryRoot") || return 1
    [ -n "$compiledTrees" ] || return 1
    while IFS= read -r path; do
        paths+=("$path")
    done <<< "$compiledTrees"
    paths+=(CMakeLists.txt)
    manifest=$(mktemp "${TMPDIR:-/tmp}/ubiquitty-build-inputs.XXXXXX") || return 1
    trap 'rm -f "$manifest" "$manifest.checkout" "$manifest.hashes"' EXIT
    local checkout checkouts=("$repositoryRoot")
    while IFS= read -r checkout; do
        [ -n "$checkout" ] && checkouts+=("$checkout")
    done < <(_build_provenance_libraries "$repositoryRoot")
    # Each checkout lists the compiled inputs it holds: a path a library owns is
    # not in the repository's own listing, where the library is one gitlink.
    for checkout in "${checkouts[@]}"; do
        _build_provenance_git "$checkout" ls-files --cached --others --exclude-standard -z -- "${paths[@]}" |
            LC_ALL=C sort -zu |
            while IFS= read -r -d '' path; do
                if [ -f "$checkout/$path" ]; then
                    printf '%s\0' "$path"
                elif [ -e "$checkout/$path" ] || [ -L "$checkout/$path" ]; then
                    echo "PROVENANCE: cannot hash non-file input: $path" >&2
                    return 1
                fi
            done > "$manifest.checkout" || return 1
        { printf '%s\0' "$checkout"; cat "$manifest.checkout"; } >> "$manifest" || return 1
        { xargs -0 git -C "$checkout" hash-object --no-filters -- < "$manifest.checkout"; } >> "$manifest.hashes" || return 1
    done
    rm -f "$manifest.checkout"
    {
        printf '%s\0' "$compiledTrees"
        cat "$manifest" || return 1
        cat "$manifest.hashes" || return 1
        rm -f "$manifest.hashes"
        for graphFile in build.ninja compile_commands.json CMakeCache.txt CMakeFiles/rules.ninja CMakeFiles/Makefile.cmake; do
            if [ -f "$buildDirectory/$graphFile" ]; then
                printf '%s\0' "$graphFile"
                git hash-object --no-filters -- "$buildDirectory/$graphFile" || return 1
            fi
        done
    } | shasum -a 256 | cut -d' ' -f1
)

# _build_provenance_refuse <label> <build-command>
# The shared tail of a refusal: what the stale link does instead of failing, and
# the one command that heals it.
_build_provenance_refuse() {
    local label="$1" buildCommand="$2"
    echo "$label: Linking them against today's headers crashes inside the library instead of" >&2
    echo "$label: failing to link, so the crash would describe neither tree." >&2
    echo "$label: Rebuild first: $buildCommand" >&2
    exit 2
}

# _build_provenance_compiled_trees <build-directory> <repository-root>
# Prints repository-relative source trees and configuration inputs, one per
# line, read from the build graph so the scope cannot drift from what
# the archives actually contain. Source/Platform is split one level deeper: a
# directory carrying the POSIX backend does not carry the platform frontends
# beside it, so a bare Source/Platform prefix would scope the comparison to
# sources the archives never compiled.
_build_provenance_compiled_trees() {
    local buildDirectory="$1" repositoryRoot="$2" graphFile graphSourceRoot
    local graphFiles=()
    for graphFile in build.ninja compile_commands.json CMakeFiles/Makefile.cmake; do
        [ ! -f "$buildDirectory/$graphFile" ] || graphFiles+=("$buildDirectory/$graphFile")
    done
    [ "${#graphFiles[@]}" -gt 0 ] || return 1
    graphSourceRoot="$repositoryRoot"
    if [ -f "$buildDirectory/CMakeCache.txt" ]; then
        graphSourceRoot=$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$buildDirectory/CMakeCache.txt") || return 1
        graphSourceRoot="${graphSourceRoot:-$repositoryRoot}"
    fi
    awk -v sourceRoot="$graphSourceRoot/" '
        {
            line = $0
            while (match(line, /\/Source\/(Platform\/)?[[:alnum:]_-]+\//)) {
                path = substr(line, RSTART + 1, RLENGTH - 1)
                if (path != "Source/Platform/") print path
                line = substr(line, RSTART + RLENGTH)
            }
            line = $0
            while (match(line, /\/(Scripts|Support)\/[^ :$|";]+/)) {
                print substr(line, RSTART + 1, RLENGTH - 1)
                line = substr(line, RSTART + RLENGTH)
            }
            line = $0
            while (index(line, sourceRoot)) {
                line = substr(line, index(line, sourceRoot) + length(sourceRoot))
                if (match(line, /^[^ :$|";]+\.cmake/)) {
                    print substr(line, RSTART, RLENGTH)
                }
            }
        }
    ' "${graphFiles[@]}" | LC_ALL=C sort -u
}

# build_provenance_record_bundled <repository-root> <build-directory>
# Records the state the helpers copied into the .app were built from. Kept beside
# the build directory rather than inside the bundle: Contents/ is sealed by the
# code signature, and a file added there is one more thing the signature covers
# for no benefit the loader gets.
build_provenance_record_bundled() {
    local repositoryRoot="$1" buildDirectory="$2" state
    [ -d "$buildDirectory" ] || return 0
    state=$(_build_provenance_state "$repositoryRoot") || return 0
    echo "$state" > "$buildDirectory/.ubiquitty-bundled-helpers"
}

# build_provenance_warn_bundled <repository-root> <build-directory> <app-path>
# Warns when the bundle carries helpers from a different worktree state than the
# app beside them.
#
# The app and the helpers share Core and the sync protocol and are separate
# binaries, so a bundle can hold two sides of one ABI change with nothing saying
# so — an app rebuilt today over helpers from yesterday, in one .app, signed and
# launchable. A crash whose candidate mechanism is exactly this pair cannot be
# checked unless the helpers carry a record of what built them once copied, so
# the question "are these the same commit" has an answer to look up.
#
# A notice rather than a refusal. --no-helpers exists to skip the helper build,
# so refusing here would remove the fast app-only rebuild it is there to give.
# What matters is not the choice but the record: the bundle it leaves holds two
# commits, and this reads the record and says so.
build_provenance_warn_bundled() {
    local repositoryRoot="$1" buildDirectory="$2" appPath="$3"
    local recordFile="$buildDirectory/.ubiquitty-bundled-helpers"
    local recordedState currentState recordedRevision currentRevision

    [ -d "$appPath/Contents/Helpers" ] || return 0
    currentState=$(_build_provenance_state "$repositoryRoot") || return 0

    if [ ! -f "$recordFile" ]; then
        echo "HELPERS: WARNING - the bundle carries helpers this script did not copy," >&2
        echo "HELPERS: so which revision built them is not recorded anywhere." >&2
        echo "HELPERS: They share Core and the sync protocol with the app; rebuild with" >&2
        echo "HELPERS: helpers to make the pair one commit again." >&2
        return 0
    fi

    recordedState=$(cat "$recordFile") || return 0
    [ "$recordedState" != "$currentState" ] || return 0

    recordedRevision="${recordedState%% *}"
    currentRevision="${currentState%% *}"
    echo "HELPERS: WARNING - Contents/Helpers was built from ${recordedRevision:0:9}," >&2
    echo "HELPERS: the app from ${currentRevision:0:9}. They share Core and the sync" >&2
    echo "HELPERS: protocol, so this bundle can hold both sides of one ABI change." >&2
    echo "HELPERS: A smoke that passes here has not compared them — drop --no-helpers" >&2
    echo "HELPERS: before reading a verdict about the pair." >&2
}

# build_provenance_warning <repository-root> <build-directory> <clean-command>
# Warns when the build directory holds objects from a different worktree state
# than the one about to be built, naming the clean rebuild that restores the
# attribution.
build_provenance_warning() {
    local repositoryRoot="$1" buildDirectory="$2" cleanCommand="$3"
    local stampFile="$buildDirectory/.ubiquitty-build-provenance"
    local recordedState currentState recordedRevision currentRevision

    [ -f "$stampFile" ] || return 0
    currentState=$(_build_provenance_state "$repositoryRoot") || return 0
    IFS= read -r recordedState < "$stampFile" || return 0
    [ "$recordedState" != "$currentState" ] || return 0

    recordedRevision="${recordedState%% *}"
    currentRevision="${currentState%% *}"
    echo "PROVENANCE: WARNING - $buildDirectory holds objects built from ${recordedRevision:0:9}" >&2
    if [ "$recordedRevision" = "$currentRevision" ]; then
        echo "PROVENANCE: with a different worktree — uncommitted changes moved since that build." >&2
    fi
    echo "PROVENANCE: A failure from this directory does not describe ${currentRevision:0:9}; do not" >&2
    echo "PROVENANCE: attribute one to it without a clean build: $cleanCommand" >&2
}
