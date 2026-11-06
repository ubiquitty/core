#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""How many compilers this machine can run at once.

Every build entry point asked the core count and used it, which is the right
answer only where memory scales with cores. A C++23 translation unit here costs
a compiler on the order of a gigabyte, so a machine with more cores than
gigabytes-to-spare oversubscribes itself: the kernel swaps to keep the set
resident, wall-clock climbs, and the cores that caused it wait on io.

What makes it worth computing is how that reads from outside. An oversubscribed
build looks like a slow machine rather than like too many jobs, so the reading
it invites is that the hardware is at fault.

Importable by the Python callers and run as a program by the shell ones, so the
answer has one implementation rather than one per entry point.
"""

import os
import sys

# What one compiler is assumed to hold. Chosen for a C++23 translation unit
# under this tree rather than for compilation in general, so a caller running
# something cheaper passes its own figure instead of inheriting this one.
DEFAULT_MEGABYTES_PER_JOB = 1536


def cgroupHeadroomMegabytes():
    """What this process's cgroup still allows, or None when it is not bounded.

    A memory cgroup is invisible to /proc/meminfo: MemAvailable answers for the
    machine, so a process inside a 16G slice on a host with 100G free reads
    100G and asks for a fan-out the slice cannot hold. What follows is reclaim
    and then the killer, which arrives as a build that died for no stated
    reason -- the oversubscription this whole module exists to prevent,
    reached through the module.

    The pair is compared because they mean different things: memory.max is
    where allocation fails, memory.high is where the kernel starts throttling
    and reclaiming hard. A build that crosses `high` is already paying for it,
    so the lower of the two is the ceiling worth respecting.

    - Returns: Megabytes still available inside the cgroup, or `None` where
      there is no cgroup, no v2 hierarchy, or no limit set.
    """
    try:
        with open("/proc/self/cgroup", "r", encoding="ascii") as handle:
            # cgroup v2 is a single line: "0::/the/path".
            path = None
            for line in handle:
                fields = line.strip().split(":", 2)
                if len(fields) == 3 and fields[0] == "0":
                    path = fields[2]
                    break
        if not path:
            return None
    except OSError:
        return None

    def value(directory, name):
        try:
            with open(os.path.join(directory, name), "r", encoding="ascii") as handle:
                text = handle.read().strip()
        except OSError:
            return None
        if text == "max":
            return None  # the file exists and says unlimited
        try:
            return int(text)
        except ValueError:
            return None

    # The limit is inherited, so the leaf is the wrong place to read it. A
    # systemd service inside a bounded slice carries `max` on its own directory
    # and the real ceiling on the slice above it -- reading only the leaf finds
    # no limit and reports the machine's memory, which is the bug this walk
    # exists to avoid. Whichever ancestor binds first is the one that will kill
    # the build, so every level is measured and the tightest wins.
    root = "/sys/fs/cgroup"
    directory = os.path.join(root, path.lstrip("/"))
    tightest = None
    while True:
        limits = [v for v in (value(directory, "memory.max"),
                              value(directory, "memory.high")) if v is not None]
        if limits:
            headroom = max(0, min(limits) - (value(directory, "memory.current") or 0))
            tightest = headroom if tightest is None else min(tightest, headroom)
        if os.path.abspath(directory) == os.path.abspath(root):
            break
        parent = os.path.dirname(directory)
        if parent == directory or not parent.startswith(root):
            break
        directory = parent

    return None if tightest is None else tightest // (1024 * 1024)


def available_megabytes():
    """The memory a new workload can take without swapping, or None.

    MemAvailable is the kernel's own estimate of exactly that, and is what this
    wants; MemFree is not, since it counts reclaimable page cache as spoken for.
    macOS publishes no equivalent, so installed memory stands in and the cap
    lands higher there than it would on a Linux box of the same size.

    Whichever of the machine and the cgroup binds first is the answer. On the
    CI host both are real: the box has 27G and the runners share a 16G slice,
    so reading only one of them is wrong in one direction or the other
    depending on what else is running.

    - Returns: Megabytes, or `None` where the platform does not say.
    """
    host = None
    try:
        with open("/proc/meminfo", "r", encoding="ascii") as meminfo:
            for line in meminfo:
                if line.startswith("MemAvailable:"):
                    host = int(line.split()[1]) // 1024
                    break
    except (OSError, ValueError, IndexError):
        host = None
    if host is None:
        try:
            host = os.sysconf("SC_PHYS_PAGES") * os.sysconf("SC_PAGE_SIZE") // (1024 * 1024)
        except (ValueError, OSError, AttributeError):
            host = None

    cgroup = cgroupHeadroomMegabytes()
    candidates = [v for v in (host, cgroup) if v is not None]
    return min(candidates) if candidates else None


def usable_cores():
    """How many CPUs this process may actually run on.

    `os.cpu_count()` reports the machine's CPUs and ignores any affinity the
    process is confined by -- a cpuset cgroup, a container's --cpuset-cpus, a
    process pinned with taskset. The `nproc` this replaced is affinity-aware, so
    reading the machine instead of the confinement would raise the count on
    exactly the constrained boxes the cap exists for.

    - Returns: A count of at least one.
    """
    try:
        return max(1, len(os.sched_getaffinity(0)))
    except (AttributeError, OSError):
        pass
    counter = getattr(os, "process_cpu_count", None)
    if counter is not None:
        try:
            return max(1, counter() or 1)
        except OSError:
            pass
    return max(1, os.cpu_count() or 4)


def build_jobs(megabytes_per_job=DEFAULT_MEGABYTES_PER_JOB):
    """The number of jobs to run at once on this machine.

    Whichever of the two limits binds first. UBQ_BUILD_JOBS names the count
    outright, for a caller that knows better than this does -- a container given
    a share of its host, or a bisect that wants the machine left usable.
    UBQ_MEMORY_PER_JOB_MB moves the estimate rather than the answer.

    - Parameters:
      - megabytes_per_job: What one job is assumed to hold.
    - Returns: A count of at least one.
    """
    override = os.environ.get("UBQ_BUILD_JOBS", "").strip()
    if override:
        try:
            return max(1, int(override))
        except ValueError:
            pass

    cores = usable_cores()

    per_job = os.environ.get("UBQ_MEMORY_PER_JOB_MB", "").strip()
    try:
        megabytes_per_job = int(per_job) if per_job else megabytes_per_job
    except ValueError:
        pass
    megabytes_per_job = max(1, megabytes_per_job)

    megabytes = available_megabytes()
    if not megabytes or megabytes <= 0:
        # A machine that does not say keeps the core count, rather than a cap
        # invented from a number that was never read.
        return cores

    # The floor is the case worth naming: -j0 means unlimited to both make and
    # ninja, so a cap that divided to zero would ask for the opposite of a cap.
    return max(1, min(cores, megabytes // megabytes_per_job))


if __name__ == "__main__":
    print(build_jobs(int(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_MEGABYTES_PER_JOB))
