#ifndef CORE_LIBS_PRX_LIBKERNEL_SYSTEM_INCLUDE_GUESTSIGNALMASK_HPP
#define CORE_LIBS_PRX_LIBKERNEL_SYSTEM_INCLUDE_GUESTSIGNALMASK_HPP

#include "prx/libkernel/Pthread/include/Pthread.hpp"
#include <array>
#include <cstdint>

namespace GuestSignalMask {

using Mask = std::array<std::uint32_t, 4>;

Mask Stored(const PthreadPrivate& thread);
void Store(PthreadPrivate& thread, Mask mask);
#ifdef _WIN32
void DeliverUnblocked(PthreadPrivate& thread);
#else
void ReadHost(const sigset_t& host, Mask& mask);
void WriteHost(const Mask& mask, sigset_t& host);
#endif

}  // namespace GuestSignalMask

#endif
