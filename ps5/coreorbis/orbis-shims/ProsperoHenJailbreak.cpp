// etaHEN/OnionHEN app-jailbreak request broker, ported for PCSX2 Orbis.
// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from diagnostics/native-launch-probe-12-fixed/src/privilege_request.cpp
// and privilege_io.cpp (PS2-Library-Prototype). Start/step/summary logic
// unchanged; adds a bounded blocking driver for headless boot.
#include "ProsperoHenJailbreak.h"

#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" int sceKernelUsleep(unsigned microseconds);

namespace hen_jailbreak {
namespace {
bool append(std::array<char, 32>& target, std::size_t& at, char value) noexcept {
    if (at + 1 >= target.size()) return false;
    target[at++] = value; target[at] = 0; return true;
}
bool append_text(std::array<char, 32>& target, std::size_t& at, const char* value) noexcept {
    while (*value) if (!append(target, at, *value++)) return false;
    return true;
}
bool append_decimal(std::array<char, 32>& target, std::size_t& at, unsigned value) noexcept {
    std::array<char, 12> reversed{}; std::size_t count = 0;
    do { reversed[count++] = static_cast<char>('0' + value % 10); value /= 10; } while (value);
    while (count) if (!append(target, at, reversed[--count])) return false;
    return true;
}
} // namespace

int process_id() noexcept { return static_cast<int>(getpid()); }
int effective_user_id() noexcept { return static_cast<int>(geteuid()); }
int remove_request(const char* path) noexcept {
    if (unlink(path) == 0 || errno == ENOENT) return 0;
    return -1;
}
int open_request(const char* path) noexcept {
    return open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
}
int make_request_readable(int descriptor) noexcept { return fchmod(descriptor, 0666); }
int write_request(int descriptor, const void* data, std::size_t size) noexcept {
    return static_cast<int>(write(descriptor, data, size));
}
int sync_request(int descriptor) noexcept { return fsync(descriptor); }
int close_request(int descriptor) noexcept { return close(descriptor); }
int publish_request(const char* source, const char* target) noexcept { return rename(source, target); }
int request_exists(const char* path) noexcept { return access(path, F_OK) == 0 ? 0 : -1; }
int last_error() noexcept { return errno; }

bool Broker::start() noexcept {
    *this = {};
    pid = process_id(); uid_before = effective_user_id(); uid_after = uid_before;
    if (pid <= 1) { state = State::failed; failure = Failure::invalid_pid; return false; }
    // A relaunch can find the native app already running as root (for
    // example, after a previous OnionHEN request). Do not publish a second
    // request in that case.
    if (uid_before == 0) {
        state = State::ready;
        return true;
    }
    std::size_t at = 0;
    if (!append_text(request, at, "{\"PID\":") ||
        !append_decimal(request, at, static_cast<unsigned>(pid)) ||
        !append_text(request, at, "}\n")) {
        state = State::failed; failure = Failure::invalid_pid; return false;
    }
    request_size = at;
    cleanup_result = remove_request(request_path);
    if (cleanup_result != 0) { error = last_error(); state = State::failed; failure = Failure::cleanup; return false; }
    cleanup_result = remove_request(staged_request_path);
    if (cleanup_result != 0) { error = last_error(); state = State::failed; failure = Failure::cleanup; return false; }
    open_result = open_request(staged_request_path);
    if (open_result < 0) { error = last_error(); state = State::failed; failure = Failure::open; return false; }
    permission_result = make_request_readable(open_result);
    if (permission_result != 0) {
        error = last_error(); close_result = close_request(open_result); remove_request(staged_request_path);
        state = State::failed; failure = Failure::permission; return false;
    }
    std::size_t written = 0;
    while (written < request_size) {
        write_result = write_request(open_result, request.data() + written, request_size - written);
        if (write_result <= 0 || static_cast<std::size_t>(write_result) > request_size - written) {
            error = last_error(); close_result = close_request(open_result); remove_request(staged_request_path);
            state = State::failed; failure = Failure::write; return false;
        }
        written += static_cast<std::size_t>(write_result);
    }
    sync_result = sync_request(open_result);
    if (sync_result != 0) {
        error = last_error(); close_result = close_request(open_result); remove_request(staged_request_path);
        state = State::failed; failure = Failure::sync; return false;
    }
    close_result = close_request(open_result);
    if (close_result != 0) {
        error = last_error(); remove_request(staged_request_path);
        state = State::failed; failure = Failure::close; return false;
    }
    publish_result = publish_request(staged_request_path, request_path);
    if (publish_result != 0) {
        error = last_error(); remove_request(staged_request_path);
        state = State::failed; failure = Failure::publish; return false;
    }
    state = State::waiting; return true;
}

void Broker::step() noexcept {
    if (state != State::waiting) return;
    ++polls; exists_result = request_exists(request_path);
    if (!request_observed_missing && exists_result == 0) {
        if (polls >= max_polls) { error = last_error(); state = State::failed; failure = Failure::timeout; }
        return;
    }
    // The request disappearing is only the consume signal. The HEN daemon then
    // needs a short, console-dependent interval to finish the process
    // jailbreak; acting in that window sees stale (non-root) creds.
    if (!request_observed_missing) {
        error = last_error();
        request_observed_missing = true;
        post_consume_polls = 0;
    }
    ++post_consume_polls;
    uid_after = effective_user_id();
    if (uid_after == 0 || post_consume_polls >= post_consume_max_polls)
        state = State::ready;
}

std::string_view Broker::summary() const noexcept {
    if (state == State::idle) return "HEN REQUEST NOT SENT";
    if (state == State::waiting) {
        return request_observed_missing ?
            "HEN CONSUMED - WAITING FOR FULL JAILBREAK" :
            "WAITING FOR HEN TO CONSUME REQUEST";
    }
    if (state == State::ready)
        return uid_before == 0 ? "ALREADY PRIVILEGED - READY" :
            "HEN CONSUMED REQUEST - READY";
    switch (failure) {
    case Failure::invalid_pid: return "INVALID PROCESS ID - REQUEST NOT SENT";
    case Failure::cleanup: return "STALE HEN REQUEST CLEANUP FAILED";
    case Failure::open: return "HEN REQUEST OPEN FAILED";
    case Failure::permission: return "REQUEST MODE 0666 APPLY FAILED";
    case Failure::write: return "HEN REQUEST WRITE FAILED";
    case Failure::sync: return "HEN REQUEST SYNC FAILED";
    case Failure::close: return "HEN REQUEST CLOSE FAILED";
    case Failure::publish: return "ATOMIC HEN REQUEST PUBLISH FAILED";
    case Failure::observe: return "HEN REQUEST STATUS CHECK FAILED";
    case Failure::timeout: return "HEN DID NOT CONSUME REQUEST - CHECK ALLOWLIST";
    default: return "HEN REQUEST FAILED";
    }
}
} // namespace hen_jailbreak

// Privilege probe: root is the only thing that makes JIT shared memory work
// on this firmware (unprivileged create fails). One page, left mapped.
extern "C" int sceKernelJitCreateSharedMemory(int flags, unsigned long long size, int protection, int* destinationHandle);
extern "C" int sceKernelJitMapSharedMemory(int handle, int protection, void** destination);

bool orbis_probe_jit()
{
    int handle = -1;
    const int rc = sceKernelJitCreateSharedMemory(0, 0x4000, 7, &handle);
    printf("[jitprobe] create rc=%d handle=%d\n", rc, handle);
    fflush(stdout);
    if (rc != 0)
        return false;
    void* addr = nullptr;
    const int rm = sceKernelJitMapSharedMemory(handle, 7, &addr);
    printf("[jitprobe] map rc=%d addr=%p\n", rm, addr);
    fflush(stdout);
    return rm == 0 && addr != nullptr;
}

// Best-effort self-check: can this process even see the HEN config, and does
// it list our TitleID? Sandbox may deny the open; every outcome is logged.
void orbis_log_hen_config()
{
    static const char* const paths[] = {
        "/data/etaHEN/config.ini",
        "/data/OnionHEN/config.ini",
    };
    for (const char* path : paths) {
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            printf("[hen] config %s: unreadable errno=%d\n", path, errno);
            fflush(stdout);
            continue;
        }
        char buf[2048];
        ssize_t total = 0;
        while (total < (ssize_t)sizeof(buf) - 1) {
            const ssize_t r = read(fd, buf + total, sizeof(buf) - 1 - (size_t)total);
            if (r <= 0)
                break;
            total += r;
        }
        close(fd);
        buf[total < 0 ? 0 : total] = 0;
        bool has_id = false, has_enabled = false;
        for (ssize_t i = 0; i + 9 <= total; ++i) {
            if (!__builtin_memcmp(buf + i, "PPSA99213", 9))
                has_id = true;
            if (!__builtin_memcmp(buf + i, "enabled", 7))
                has_enabled = true;
        }
        printf("[hen] config %s: bytes=%d has_id=%d has_enabled=%d\n", path, (int)total,
            (int)has_id, (int)has_enabled);
        fflush(stdout);
    }
}

bool orbis_hen_jailbreak()
{
    hen_jailbreak::Broker broker;
    broker.start();
    printf("[hen] start: %.*s pid=%d uid_before=%d\n", (int)broker.summary().size(),
        broker.summary().data(), broker.pid, broker.uid_before);
    fflush(stdout);
    if (broker.state == hen_jailbreak::State::ready) {
        printf("[hen] already root, published nothing\n");
        fflush(stdout);
        return true;
    }
    if (broker.state != hen_jailbreak::State::waiting) {
        printf("[hen] failed: %.*s errno=%d\n", (int)broker.summary().size(),
            broker.summary().data(), broker.error);
        fflush(stdout);
        return false;
    }
    for (;;) {
        sceKernelUsleep(16667);
        broker.step();
        if (broker.state != hen_jailbreak::State::waiting)
            break;
        if ((broker.polls % 60) == 0) {
            printf("[hen] %.*s polls=%zu\n", (int)broker.summary().size(),
                broker.summary().data(), broker.polls);
            fflush(stdout);
        }
    }
    printf("[hen] done: %.*s pid=%d uid_before=%d uid_after=%d polls=%zu grace=%zu errno=%d\n",
        (int)broker.summary().size(), broker.summary().data(),
        broker.pid, broker.uid_before, broker.uid_after,
        broker.polls, broker.post_consume_polls, broker.error);
    fflush(stdout);
    return broker.state == hen_jailbreak::State::ready && broker.uid_after == 0;
}
