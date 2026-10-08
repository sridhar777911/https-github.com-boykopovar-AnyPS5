#include "prx/libc/include/exceptions/Runtime.hpp"
#include <cstddef>
#ifndef _UNWIND_H
#define _UNWIND_H
#endif

#include <cxxabi.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <typeinfo>
#include <new>
#include <ios>
#include <locale>
#include <regex>
#include <functional>
#include <mutex>

#include "prx/libc/include/General.hpp"

extern "C" {

void* APS5_VABI __cxa_demangle_nid_postfix(const char* mangled, char* buf, std::size_t* len, int* status) {
    return abi::__cxa_demangle(mangled, buf, len, status);
}

int APS5_VABI __cxa_thread_atexit_impl_nid_postfix(void (APS5_VABI *func)(void*), void* arg, void* dso) {
    struct ThreadAtexitContext {
        void (APS5_VABI *destructor)(void*);
        void* object;
    };
    auto* context = new (std::nothrow) ThreadAtexitContext{func, arg};
    if (context == nullptr)
        return -1;
    const int result = __cxxabiv1::__cxa_thread_atexit(
        [](void* opaque) {
            auto* context = static_cast<ThreadAtexitContext*>(opaque);
            const auto destructor = context->destructor;
            void* object = context->object;
            delete context;
            destructor(object);
        },
        context, dso);
    if (result != 0)
        delete context;
    return result;
}

int APS5_VABI LibcInternalExtCxaThreadAtexit_nid_postfix(void (APS5_VABI *destructor)(void*), void* object, void* module_id) {
#ifdef _WIN32
    (void)module_id;
    return __cxa_thread_atexit_impl_nid_postfix(destructor, object, nullptr);
#else
    return __cxa_thread_atexit_impl_nid_postfix(destructor, object, module_id);
#endif
}

const std::error_category* _ZSt17iostream_categoryv_nid_postfix() { return &std::iostream_category(); }

int APS5_VABI _ZSt13_Execute_onceRSt9once_flagPFiPvS1_PS1_ES1__nid_postfix(
    int* flag, int (APS5_VABI *callback)(void*, void*, void**), void* arg
) {
    static std::recursive_mutex mutex;
    std::lock_guard lock(mutex);
    if (*flag != 0) return 1;
    if (callback(nullptr, arg, nullptr) == 0) return 0;
    *flag = 1;
    return 1;
}

}
