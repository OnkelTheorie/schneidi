// Development switches of the H.264 encoder: the H264_* environment variables that measurements and
// experiments set. They are read only when SCHNEIDI_S264_DEV=1 is set as well, so a stray variable in
// a user's environment cannot change a stream; without the opt-in every switch has its default.
#pragma once

#include <cstdlib>
#include <utility>

namespace schneidi::h264 {

inline const char* devEnv(const char* name)
{
    static const bool on = [] {
        const char* e = std::getenv("SCHNEIDI_S264_DEV");
        return e && e[0] == '1' && e[1] == '\0';
    }();
    return on ? std::getenv(name) : nullptr;
}

// A value computed once at namespace scope (a development switch, a derived constant):
//   S264_DEV(double, gName, devEnv("H264_NAME") ? std::atof(devEnv("H264_NAME")) : 1.0);
// In the build for every CPU this is `static const double gName = (...)`, computed while the program loads.
// The x86-64-v3 build of the encoder's files (EncoderAlt in H264Encoder.h) must not run any of its code
// before the CPU check - a dynamic initialiser would run at load on every CPU and stop the program with an
// illegal instruction on one without AVX. There the variable is constant-initialised (constinit, zero) and
// devFill<File>() sets it at the first use of that build (schneidi_s264_v3_*), in the order of the file.
// CMake refuses the build if that object has initialisers left (cmake/S264V3Check.cmake).
namespace v3init {
template <int N>
struct Slot {};
} // namespace v3init

#if defined(SCHNEIDI_S264_V3_BUILD)
#define S264_DEV(T, name, ...)                                                                                     \
    constinit static T name{};                                                                                     \
    [[maybe_unused]] static void s264DevFill(::schneidi::h264::v3init::Slot<__COUNTER__>) { name = (__VA_ARGS__); } \
    static_assert(true)
// At the end of a file that has S264_DEV values, in namespace schneidi::h264: the function setting them all
// (every __COUNTER__ value of the file must belong to an S264_DEV, otherwise this does not compile).
#define S264_DEV_FILL_ALL(fn)                                                                                      \
    void fn()                                                                                                      \
    {                                                                                                              \
        []<int... K>(std::integer_sequence<int, K...>) {                                                           \
            (s264DevFill(::schneidi::h264::v3init::Slot<K>{}), ...);                                               \
        }(std::make_integer_sequence<int, __COUNTER__>{});                                                         \
    }                                                                                                              \
    static_assert(true)
void devFillEncoder();
void devFillLookahead();
void devFillCabac();
#else
#define S264_DEV(T, name, ...) static const T name = (__VA_ARGS__)
#define S264_DEV_FILL_ALL(fn) static_assert(true)
#endif

} // namespace schneidi::h264
