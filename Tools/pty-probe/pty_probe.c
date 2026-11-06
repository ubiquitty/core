// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// pty_probe.c — ground-truth PTY transport measurement on macOS.
// Forks a flood writer through a PTY and reads the master with no parsing.
// Reports: throughput, read-size histogram, EAGAIN count, and (optionally)
// a synthetic parse stall of P microseconds every K bytes to map how reader
// absence affects the writer.
//
//   pty_probe [total_bytes] [read_buf_bytes] [stall_us] [stall_every_bytes]
//             [writer_chunk_bytes] [spin_ns] [pace_ns]
//
// pace_ns > 0 waits that long after each successful read before the next
// read attempt — pacing the reader to the writer's refill rhythm so the
// output queue can fill toward hiwat instead of being drained mid-refill.
//
// spin_ns > 0 rides EAGAIN on the monotonic clock (syscall-free, like
// TerminalRuntime's drain); spin_ns == 0 (default) blocks in poll().

#include <errno.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <termios.h>
#include <unistd.h>
#include <util.h>

static uint64_t now_ns(void)
{
    static mach_timebase_info_data_t tb;
    if (!tb.denom)
        mach_timebase_info(&tb);
    return mach_absolute_time() * tb.numer / tb.denom;
}

int main(int argc, char** argv)
{
    long total = argc > 1 ? atol(argv[1]) : 200000000L;
    int bufsz = argc > 2 ? atoi(argv[2]) : 65536;
    int stall_us = argc > 3 ? atoi(argv[3]) : 0;
    long stall_every = argc > 4 ? atol(argv[4]) : 65536;
    int wchunk = argc > 5 ? atoi(argv[5]) : 65536;
    long spin_ns = argc > 6 ? atol(argv[6]) : 0;
    long pace_ns = argc > 7 ? atol(argv[7]) : 0;
    // spin_ns == -1 selects kevent(EVFILT_READ) blocking instead of poll/spin
    const int use_kevent = spin_ns == -1;

    int master;
    pid_t pid = forkpty(&master, NULL, NULL, NULL);
    if (pid < 0) {
        perror("forkpty");
        return 1;
    }
    if (pid == 0) {
        // writer child: raw mode, flood '#' in wchunk writes
        struct termios t;
        tcgetattr(STDOUT_FILENO, &t);
        cfmakeraw(&t);
        tcsetattr(STDOUT_FILENO, TCSANOW, &t);
        char* chunk = malloc(wchunk);
        memset(chunk, '#', wchunk);
        long sent = 0;
        while (sent < total + (long)wchunk) {
            ssize_t w = write(STDOUT_FILENO, chunk, wchunk);
            if (w > 0)
                sent += w;
            else if (errno != EINTR && errno != EAGAIN)
                _exit(0);
        }
        _exit(0);
    }

    // reader: raw, nonblocking. PROBE_BAUD sets the tty speed — xnu's
    // ttsetwater() sizes t_hiwat from ospeed (clamped at TTMAXHIWAT 2048),
    // and ptys inherit the 9600 default, which lands hiwat at ~1024.
    struct termios t;
    tcgetattr(master, &t);
    cfmakeraw(&t);
    const char* baud = getenv("PROBE_BAUD");
    if (baud && atol(baud) > 0)
        cfsetspeed(&t, (speed_t)atol(baud));
    tcsetattr(master, TCSANOW, &t);
    fcntl(master, F_SETFL, O_NONBLOCK);

    char* buf = malloc(bufsz);
    long got = 0, reads = 0, eagains = 0, polls = 0;
    long hist[18] = { 0 }; // log2 buckets of read sizes
    long since_stall = 0;
    uint64_t t0 = now_ns();
    while (got < total) {
        ssize_t n = read(master, buf, bufsz);
        if (n > 0) {
            got += n;
            reads++;
            if (pace_ns > 0) {
                uint64_t s = now_ns();
                while ((long)(now_ns() - s) < pace_ns) {
                }
            }
            int b = 0;
            for (long v = n; v > 1; v >>= 1)
                b++;
            hist[b > 17 ? 17 : b]++;
            if (stall_us > 0) {
                since_stall += n;
                if (since_stall >= stall_every) {
                    since_stall = 0;
                    uint64_t s = now_ns();
                    while ((long)(now_ns() - s) < stall_us * 1000L) {
                    }
                }
            }
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && errno == EAGAIN) {
            eagains++;
            if (use_kevent) {
                static int kq = -1;
                if (kq < 0) {
                    kq = kqueue();
                    struct kevent kev;
                    EV_SET(&kev, master, EVFILT_READ, EV_ADD, 0, 0, NULL);
                    kevent(kq, &kev, 1, NULL, 0, NULL);
                }
                struct kevent out;
                kevent(kq, NULL, 0, &out, 1, NULL);
                polls++;
                continue;
            }
            if (spin_ns > 0) {
                uint64_t s = now_ns();
                while ((long)(now_ns() - s) < spin_ns) {
                }
            } else {
                struct pollfd p = { .fd = master, .events = POLLIN };
                poll(&p, 1, 1000);
                polls++;
            }
            continue;
        }
        break; // EOF or error
    }
    uint64_t dt = now_ns() - t0;
    double s = dt / 1e9;
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    const double cpu = ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6
        + ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
    printf("reader-cpu: %.3f s (%.0f%% of wall; %.2f cpu-ms/MB)\n",
        cpu, 100.0 * cpu / s, cpu * 1e3 / (got / 1e6));
    printf("read-only: %.1f MB/s  (%ld bytes, %.3f s)\n", got / 1e6 / s, got, s);
    printf("reads=%ld avg=%ld B  eagains=%ld polls=%ld  stall=%dus/%ldB  wchunk=%d spin=%ldns pace=%ldns\n",
        reads, reads ? got / reads : 0, eagains, polls, stall_us, stall_every, wchunk, spin_ns, pace_ns);
    printf("read-size histogram (log2 buckets):\n");
    for (int i = 0; i < 18; i++)
        if (hist[i])
            printf("  %6d..%6d B: %ld\n", 1 << i, (2 << i) - 1, hist[i]);
    kill(pid, 9);
    return 0;
}
