#include "prx/libc/include/General.hpp"
#include "prx/libc/include/general/VabiMacros.hpp"
#include "prx/libkernel/Pthread/include/Pthread.hpp"
#include "prx/libkernel/System/include/GuestSignalMask.hpp"
#include <array>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <mutex>
#include <stdexcept>

extern "C" int* APS5_VABI __error_nid_postfix();
extern "C" Pthread APS5_VABI scePthreadSelf();
#ifdef _WIN32
extern "C" int APS5_VABI sceKernelRaiseException(Pthread thread, int signum);
#endif
namespace {
using GuestHandler = void (APS5_VABI *)(int);
std::atomic<GuestHandler> handlers[32]{};
static_assert(std::atomic<GuestHandler>::is_always_lock_free);
std::mutex registration;
constexpr std::array<int, 6> MappedSignals{2, 4, 6, 8, 11, 15};
constexpr std::array<int, 7> MaskedSignals{2, 4, 6, 8, 11, 15, GUEST_RAISED_SIGNAL};
constexpr std::uint32_t UnblockableSignals = GuestSignalBit(9) | GuestSignalBit(17) | GuestSignalBit(32);
constexpr std::uintptr_t IgnoredHandler = 1;
int NativeSignal(int guest) {
    switch (guest) {
        case 2: return SIGINT;
        case 4: return SIGILL;
        case 6: return SIGABRT;
        case 8: return SIGFPE;
        case 11: return SIGSEGV;
        case 15: return SIGTERM;
        default: return 0;
    }
}
#ifndef _WIN32
int HostSignal(int guest) {
    return guest == GUEST_RAISED_SIGNAL ? HOST_RAISED_SIGNAL : NativeSignal(guest);
}
#endif
void Dispatch(int native) {
    int guest = 0;
    for (int candidate : MappedSignals)
        if (NativeSignal(candidate) == native) { guest = candidate; break; }
    if (!guest) return;
#ifdef _WIN32
    // Preserve the guest's persistent registration across CRT delivery.
    std::signal(native, Dispatch);
#endif
    const auto callback = handlers[guest].load();
    if (reinterpret_cast<std::uintptr_t>(callback) <= IgnoredHandler) return;
    auto* self = CurrentGuestThread();
    if (self == nullptr) {
        callback(guest);
        return;
    }
    const auto interrupted = GuestSignalMask::Stored(*self);
#ifdef _WIN32
    auto during = interrupted;
    during[0] |= GuestSignalBit(guest);
    GuestSignalMask::Store(*self, during);
#endif
    callback(guest);
    GuestSignalMask::Store(*self, interrupted);
#ifdef _WIN32
    GuestSignalMask::DeliverUnblocked(*self);
#endif
}

#ifndef _WIN32
const sigset_t& GuardedSignals() {
    static const sigset_t guarded = [] {
        sigset_t set;
        sigemptyset(&set);
        for (const int guest : MaskedSignals) sigaddset(&set, HostSignal(guest));
        return set;
    }();
    return guarded;
}
#endif
}

namespace GuestSignalMask {

Mask Stored(const PthreadPrivate& thread) {
    Mask mask{};
    for (std::size_t word = 0; word < mask.size(); ++word) mask[word] = thread.signalMask[word].load();
    return mask;
}

void Store(PthreadPrivate& thread, Mask mask) {
    mask[0] &= ~UnblockableSignals;
    for (std::size_t word = 0; word < mask.size(); ++word) thread.signalMask[word].store(mask[word]);
}

#ifdef _WIN32
void DeliverUnblocked(PthreadPrivate& thread) {
    for (const int guest : MaskedSignals) {
        const std::uint32_t bit = GuestSignalBit(guest);
        if (GuestSignalBlocked(thread, guest) || (thread.pendingSignals.fetch_and(~bit) & bit) == 0) continue;
        if (guest == GUEST_RAISED_SIGNAL) sceKernelRaiseException(&thread, guest);
        else std::raise(NativeSignal(guest));
    }
}
#else
void ReadHost(const sigset_t& host, Mask& mask) {
    for (const int guest : MaskedSignals) {
        if (sigismember(&host, HostSignal(guest)) == 1) mask[0] |= GuestSignalBit(guest);
        else mask[0] &= ~GuestSignalBit(guest);
    }
}

void WriteHost(const Mask& mask, sigset_t& host) {
    for (const int guest : MaskedSignals) {
        if ((mask[0] & GuestSignalBit(guest)) != 0) sigaddset(&host, HostSignal(guest));
        else sigdelset(&host, HostSignal(guest));
    }
}
#endif

}  // namespace GuestSignalMask

struct GuestSignalSet {
    std::uint32_t bits[4];
};

extern "C" {
GuestHandler APS5_VABI signal_nid_postfix(int guest, GuestHandler handler) {
    const auto invalid = reinterpret_cast<GuestHandler>(static_cast<std::uintptr_t>(-1));
    const int native = NativeSignal(guest);
    if (!native || handler == invalid) { *__error_nid_postfix() = 22; return invalid; }
    std::lock_guard lock(registration);
    const auto previous = handlers[guest].exchange(handler);
    const auto address = reinterpret_cast<std::uintptr_t>(handler);
    auto hostHandler = address == 0 ? SIG_DFL : address == IgnoredHandler ? SIG_IGN : Dispatch;
    if (std::signal(native, hostHandler) == SIG_ERR) {
        handlers[guest].store(previous);
        *__error_nid_postfix() = 22;
        return invalid;
    }
#ifdef _WIN32
    if (address == IgnoredHandler)
        if (auto* self = CurrentGuestThread()) self->pendingSignals.fetch_and(~GuestSignalBit(guest));
#endif
    return previous;
}
int APS5_VABI raise_nid_postfix(int guest) {
    const int native = NativeSignal(guest);
    if (!native) { *__error_nid_postfix() = 22; return -1; }
    if (reinterpret_cast<std::uintptr_t>(handlers[guest].load()) == IgnoredHandler) return 0;
#ifdef _WIN32
    auto* self = scePthreadSelf();
    if (GuestSignalBlocked(*self, guest)) {
        self->pendingSignals.fetch_or(GuestSignalBit(guest));
        return 0;
    }
#endif
    const int result = std::raise(native);
    if (result) *__error_nid_postfix() = 22;
    return result ? -1 : 0;
}
int APS5_VABI _sigprocmask_nid_postfix(int how, const GuestSignalSet* set, GuestSignalSet* previousSet) {
    if (set != nullptr && (how < 1 || how > 3)) throw std::invalid_argument("_sigprocmask: invalid how");
    auto& self = *scePthreadSelf();
    auto previous = GuestSignalMask::Stored(self);
#ifdef _WIN32
    previous[0] = self.signalMask[0].fetch_or(GuestSignalBit(GUEST_RAISED_SIGNAL));
#else
    sigset_t host;
    pthread_sigmask(SIG_BLOCK, &GuardedSignals(), &host);
    GuestSignalMask::ReadHost(host, previous);
#endif
    auto next = previous;
    if (set != nullptr) {
        for (std::size_t word = 0; word < next.size(); ++word) {
            switch (how) {
                case 1: next[word] |= set->bits[word]; break;
                case 2: next[word] &= ~set->bits[word]; break;
                default: next[word] = set->bits[word]; break;
            }
        }
    }
    GuestSignalMask::Store(self, next);
    if (previousSet != nullptr)
        for (std::size_t word = 0; word < previous.size(); ++word) previousSet->bits[word] = previous[word];
#ifdef _WIN32
    GuestSignalMask::DeliverUnblocked(self);
#else
    GuestSignalMask::WriteHost(GuestSignalMask::Stored(self), host);
    pthread_sigmask(SIG_SETMASK, &host, nullptr);
#endif
    return 0;
}

int APS5_VABI sigprocmask_nid_postfix(int how, const void* set, void* previousSet) {
    return _sigprocmask_nid_postfix(how, static_cast<const GuestSignalSet*>(set),
                                    static_cast<GuestSignalSet*>(previousSet));
}
}

extern "C" {

int APS5_VABI _is_signal_return_nid_postfix(std::uint64_t programCounter) {
    (void)programCounter;
    return 0;
}

}
