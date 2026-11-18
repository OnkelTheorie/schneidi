// Development switches of the H.264 encoder: the H264_* environment variables that measurements and
// experiments set. They are read only when SCHNEIDI_S264_DEV=1 is set as well, so a stray variable in
// a user's environment cannot change a stream; without the opt-in every switch has its default.
#pragma once

#include <cstdlib>

namespace schneidi::h264 {

inline const char* devEnv(const char* name)
{
    static const bool on = [] {
        const char* e = std::getenv("SCHNEIDI_S264_DEV");
        return e && e[0] == '1' && e[1] == '\0';
    }();
    return on ? std::getenv(name) : nullptr;
}

} // namespace schneidi::h264
