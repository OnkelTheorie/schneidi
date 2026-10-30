// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Compatibility mode (Playback menu): everything on the CPU – no hardware decoding, the viewer draws with QPainter
// (no GPU). For machines whose graphics driver misbehaves. Environment variable SCHNEIDI_CPU_PATH=1/0 wins over the
// setting "playback/cpuPath"; read once per run, a change takes effect after a restart.

namespace CpuPath {

bool on();
void setOn(bool on); // the setting (after a restart)

} // namespace CpuPath
