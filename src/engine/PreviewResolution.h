// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// The user's choice of the preview's resolution (Viewer's right-click and Playback › Preview Resolution, Settings ›
// General), separately for the main window (viewer, full-screen viewer) and for the output on a second monitor:
// automatic (fits the display - the default, playback speed first), half or the full project resolution. The engine
// has one preview: when the main window and the monitor want different sizes, the larger wins (MainWindow::
// updatePreviewEdge); nothing is rendered twice.

#include <QSettings>
#include <QSize>

#include <algorithm>
#include <climits>

namespace PreviewResolution {

enum class Choice { Auto, Half, Full };
enum class Target { Viewer, Monitor };

// The longer edge to ask the engine for (Engine::setPreviewEdge): `autoEdge` for Auto (what the display wants; 0 =
// the engine's default), half the project's longer edge, or the project size (INT_MAX)
inline int edge(Choice choice, int autoEdge, const QSize& project)
{
    switch (choice) {
    case Choice::Half: return std::max(2, std::max(project.width(), project.height()) / 2);
    case Choice::Full: return INT_MAX;
    case Choice::Auto: break;
    }
    return autoEdge;
}

inline const char* settingKey(Target target)
{
    return target == Target::Viewer ? "viewer/previewResolution" : "viewer/monitorResolution";
}

// Stored as "auto" (or nothing), "half", "full"
inline Choice stored(Target target)
{
    const QString v = QSettings().value(settingKey(target)).toString();
    return v == "full" ? Choice::Full : v == "half" ? Choice::Half : Choice::Auto;
}

inline void store(Target target, Choice choice)
{
    QSettings().setValue(settingKey(target), choice == Choice::Full ? "full" : choice == Choice::Half ? "half" : "auto");
}

} // namespace PreviewResolution
