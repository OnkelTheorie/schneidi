// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <QObject>

class QWidget;

// Scrolling a panel with the mouse wheel must not change the values the cursor passes over: sliders, spin boxes and
// combo boxes below `root` (also ones added later) take the wheel only while they have the keyboard focus (clicked
// into); otherwise the wheel goes on to the scroll area around them, which scrolls. Combo and spin boxes no longer get
// the focus from the wheel itself (StrongFocus instead of WheelFocus). One call per panel; the guard lives as long as
// `root`.
namespace WheelGuard {
void install(QWidget* root);
}
