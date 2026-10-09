#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libc/include/HeapDiagnostics.hpp"
#include "prx/libc/include/HostThreadLocal.hpp"
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

extern "C" void APS5_VABI sceKernelSetThreadDtors(thread_dtors_func_t dtors);
extern "C" int APS5_VABI sceKernelGetModuleInfoFromAddr(std::uint64_t address, int flags, ModuleInfoEx* info);

namespace {

using ThreadDestructorFunction = void (APS5_VABI*)(void*);

struct ThreadDestructor {
    ThreadDestructorFunction function;
    void* object;
    void* dsoSymbol;
};

struct ThreadDestructorsTag {};

std::vector<ThreadDestructor>& ThreadDestructors() {
    return HostThreadLocal<std::vector<ThreadDestructor>, ThreadDestructorsTag>();
}

bool IsInLoadedImage(const void* address) {
    if (address == nullptr)
        return false;
#ifdef _WIN32
    HMODULE module = nullptr;
    return GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(address), &module) != 0;
#else
    Dl_info info{};
    return dladdr(address, &info) != 0;
#endif
}

void CallThreadDestructor(const ThreadDestructor& destructor) {
    const auto* function = reinterpret_cast<const void*>(destructor.function);
    if (!IsInLoadedImage(function)) {
        std::ostringstream message;
        message << "thread_local destructor " << function << " of dso " << destructor.dsoSymbol << " is not in a loaded image";
        throw std::runtime_error(message.str());
    }
    destructor.function(destructor.object);
}

void APS5_VABI RunThreadDestructors_nid_no_patch() {
    auto& destructors = ThreadDestructors();
    while (!destructors.empty()) {
        const ThreadDestructor destructor = destructors.back();
        destructors.pop_back();
        CallThreadDestructor(destructor);
    }
}

bool FindDsoModule(const void* dsoSymbol, KernelModule& handle) {
    ModuleInfoEx info{};
    info.st_size = sizeof(ModuleInfoEx);
    if (sceKernelGetModuleInfoFromAddr(reinterpret_cast<std::uintptr_t>(dsoSymbol), 2, &info) != 0) {
        handle = 0;
        return false;
    }
    handle = info.id;
    return true;
}

bool ForceThreadDestructorPass(KernelModule handle) {
    auto& destructors = ThreadDestructors();
    bool found = false;
    for (std::size_t index = destructors.size(); index-- > 0;) {
        const ThreadDestructor destructor = destructors[index];
        KernelModule module = 0;
        const bool loaded = FindDsoModule(destructor.dsoSymbol, module);
        if (module != handle)
            continue;
        found = true;
        destructors.erase(destructors.begin() + static_cast<std::ptrdiff_t>(index));
        if (loaded && *static_cast<void* const*>(destructor.dsoSymbol) == destructor.dsoSymbol)
            CallThreadDestructor(destructor);
    }
    return found;
}

void RegisterThreadExitHook() {
    [[maybe_unused]] static const bool registered = [] {
        sceKernelSetThreadDtors(RunThreadDestructors_nid_no_patch);
        return true;
    }();
}

enum class WideClass : std::uintptr_t {
    None,
    Alnum,
    Alpha,
    Blank,
    Control,
    Digit,
    Graph,
    Lower,
    Print,
    Punct,
    Space,
    Upper,
    HexDigit
};

bool IsAsciiUpper(std::uint32_t value) {
    return value >= 'A' && value <= 'Z';
}

bool IsAsciiLower(std::uint32_t value) {
    return value >= 'a' && value <= 'z';
}

bool IsAsciiDigit(std::uint32_t value) {
    return value >= '0' && value <= '9';
}

bool MatchesWideClass(std::uint32_t value, WideClass type) {
    const bool upper = IsAsciiUpper(value);
    const bool lower = IsAsciiLower(value);
    const bool digit = IsAsciiDigit(value);
    const bool alpha = upper || lower;
    const bool alnum = alpha || digit;
    const bool graph = value >= 0x21 && value <= 0x7e;
    switch (type) {
    case WideClass::Alnum: return alnum;
    case WideClass::Alpha: return alpha;
    case WideClass::Blank: return value == ' ' || value == '\t';
    case WideClass::Control: return value <= 0x1f || value == 0x7f;
    case WideClass::Digit: return digit;
    case WideClass::Graph: return graph;
    case WideClass::Lower: return lower;
    case WideClass::Print: return value >= 0x20 && value <= 0x7e;
    case WideClass::Punct: return graph && !alnum;
    case WideClass::Space: return value == ' ' || (value >= '\t' && value <= '\r');
    case WideClass::Upper: return upper;
    case WideClass::HexDigit: return digit || (value >= 'A' && value <= 'F') || (value >= 'a' && value <= 'f');
    default: return false;
    }
}

WideClass FindWideClass(const char* name) {
    if (name == nullptr)
        return WideClass::None;
    constexpr std::string_view names[] = {"", "alnum", "alpha", "blank", "cntrl", "digit", "graph", "lower", "print", "punct", "space", "upper", "xdigit"};
    for (std::size_t index = 1; index < std::size(names); ++index) {
        if (names[index] == name)
            return static_cast<WideClass>(index);
    }
    return WideClass::None;
}

}

extern "C" {

int Need_sceLibcInternal_nid_postfix = 1;

void APS5_VABI __cxa_finalize_nid_postfix(void* dsoHandle) {
    CxaFinalize_nid_no_patch(dsoHandle);
}

void APS5_VABI sceLibcHeapGetTraceInfo_nid_postfix(Info* info) {
    LibcHeapTraceInfo_nid_no_patch(info);
}

int APS5_VABI _sceLibcInternalThreadAtexit_nid_postfix(ThreadDestructorFunction destructor, void* object, void* dsoSymbol) {
    RegisterThreadExitHook();
    ThreadDestructors().push_back({destructor, object, dsoSymbol});
    return 0;
}

void APS5_VABI _sceLibcInternalThreadDtors_nid_postfix() {
    RunThreadDestructors_nid_no_patch();
}

int APS5_VABI _sceLibcInternalForceTlsDestructor_nid_postfix(KernelModule handle) {
    for (int pass = 0; pass < 4; ++pass) {
        if (!ForceThreadDestructorPass(handle))
            break;
    }
    return 0;
}

std::uintptr_t APS5_VABI wctype_nid_postfix(const char* name) {
    return static_cast<std::uintptr_t>(FindWideClass(name));
}

int APS5_VABI iswctype_nid_postfix(std::uint32_t value, std::uintptr_t type) {
    return MatchesWideClass(value, static_cast<WideClass>(type)) ? 1 : 0;
}

int APS5_VABI iswalnum_nid_postfix(std::uint32_t value) { return MatchesWideClass(value, WideClass::Alnum) ? 1 : 0; }
int APS5_VABI iswalpha_nid_postfix(std::uint32_t value) { return MatchesWideClass(value, WideClass::Alpha) ? 1 : 0; }
int APS5_VABI iswblank_nid_postfix(std::uint32_t value) { return MatchesWideClass(value, WideClass::Blank) ? 1 : 0; }
int APS5_VABI iswcntrl_nid_postfix(std::uint32_t value) { return MatchesWideClass(value, WideClass::Control) ? 1 : 0; }
int APS5_VABI iswdigit_nid_postfix(std::uint32_t value) { return MatchesWideClass(value, WideClass::Digit) ? 1 : 0; }
int APS5_VABI iswgraph_nid_postfix(std::uint32_t value) { return MatchesWideClass(value, WideClass::Graph) ? 1 : 0; }
int APS5_VABI iswlower_nid_postfix(std::uint32_t value) { return MatchesWideClass(value, WideClass::Lower) ? 1 : 0; }
int APS5_VABI iswprint_nid_postfix(std::uint32_t value) { return MatchesWideClass(value, WideClass::Print) ? 1 : 0; }
int APS5_VABI iswpunct_nid_postfix(std::uint32_t value) { return MatchesWideClass(value, WideClass::Punct) ? 1 : 0; }
int APS5_VABI iswspace_nid_postfix(std::uint32_t value) { return MatchesWideClass(value, WideClass::Space) ? 1 : 0; }
int APS5_VABI iswupper_nid_postfix(std::uint32_t value) { return MatchesWideClass(value, WideClass::Upper) ? 1 : 0; }
int APS5_VABI iswxdigit_nid_postfix(std::uint32_t value) { return MatchesWideClass(value, WideClass::HexDigit) ? 1 : 0; }

std::uint32_t APS5_VABI towupper_nid_postfix(std::uint32_t value) {
    return IsAsciiLower(value) ? value - ('a' - 'A') : value;
}

std::uint32_t APS5_VABI towlower_nid_postfix(std::uint32_t value) {
    return IsAsciiUpper(value) ? value + ('a' - 'A') : value;
}

std::uint32_t APS5_VABI btowc_nid_postfix(int value) {
    return value >= 0 && value <= 0xff ? static_cast<std::uint32_t>(value) : UINT32_MAX;
}

}
