// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Channel formats of the mix (0.23, docs/plan-0.23.md Block 1): stereo, 5.1 and 7.1. The project chooses one
// (ProjectFormat::audioChannels); the mixer, the export, the loudness measurement and the monitor downmix read the
// channel order and the coefficients from here. Pure C++ without Qt (apart from the names) -> testable, same on
// Linux and Windows.
//
// Channel order = FFmpeg's layouts "stereo", "5.1" and "7.1" (what AAC, Opus and FLAC write without remapping):
//   2: FL FR
//   6: FL FR FC LFE BL BR            (the surrounds are FFmpeg's "back" pair; AC-3 calls the same pair "side")
//   8: FL FR FC LFE BL BR SL SR

#include <QString>

#include <array>

namespace ChannelLayout {

enum Channel { FL = 0, FR = 1, FC = 2, LFE = 3, BL = 4, BR = 5, SL = 6, SR = 7 };

constexpr int kMaxChannels = 8;

// A supported channel count (2, 6, 8); anything else -> 2
constexpr int valid(int channels) { return channels == 6 || channels == 8 ? channels : 2; }
constexpr bool surround(int channels) { return valid(channels) > 2; }

// "stereo", "5.1", "7.1" (project file, CLI, probe)
QString name(int channels);
// "stereo"/"2.0"/"2", "5.1"/"6", "7.1"/"8" (any case) -> 2/6/8; 0 = unknown
int parse(const QString& text);
// Short label of channel c of the layout: "L", "R", "C", "LFE", "Ls", "Rs", "Lss", "Rss" (meters, probe)
QString label(int channels, int c);
// FFmpeg layout name of the format ("stereo", "5.1", "7.1")
const char* ffmpegName(int channels);

// Loudness weight of channel c (ITU-R BS.1770-4): front 1.0, LFE not counted, surrounds 1.41 (+1.5 dB)
double loudnessWeight(int channels, int c);

// Conversion from the mix format to another channel count (monitor downmix, export in another format):
// out[o] = sum_i m[o][i] * in[i]. To stereo: ITU-R BS.775 (L = FL + 0.707 C + 0.707 Ls, LFE left out; 7.1: both
// surround pairs with 0.707), not normalized (a loud centre can go over full scale like in every editor's monitor).
// 2 -> 5.1/7.1: front left/right only. 5.1 -> 7.1: the surrounds stay the back pair. 7.1 -> 5.1: side and back pair
// each 0.707 into the surround pair. Same count: identity.
using Matrix = std::array<std::array<double, kMaxChannels>, kMaxChannels>;
Matrix conversion(int from, int to);

// Surround panner of a track (mix in 5.1 or 7.1): where its front left/right pair goes.
//   pan    -100..100  balance like the stereo pan (the far side turned down)
//   depth     0..100  front (0) -> surround (100); 7.1: front -> side (50) -> back (100); constant power
//   center    0..100  share of the front pair sent to the centre speaker instead (linear; 100 = centre only)
//   lfeDb  send to the LFE channel (sum of left and right, -6 dB each); <= kLfeOff = off
// Channels 2.. of a track (the centre, LFE and surrounds of a 5.1/7.1 source) pass through unchanged.
constexpr double kLfeOff = -60.0;
struct PanGains {
    // gain of the track's left (index 0) and right (index 1) channel into each output channel (LFE excluded)
    std::array<std::array<double, 2>, kMaxChannels> m{};
    double lfe = 0; // gain of L + R into the LFE channel (half the send level), before its 120 Hz low pass
};
PanGains surroundPan(int channels, double pan, double depth, double center, double lfeDb);

} // namespace ChannelLayout
