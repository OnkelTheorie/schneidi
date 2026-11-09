// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// The user's choice of the preview's resolution (Viewer's right-click and Workspace › Preview Resolution, Settings ›
// General), separately for the main window (viewer, full-screen viewer) and for the output on a second monitor:
// automatic (as many pixels as the display shows the picture with - the default, playback speed first), a fixed size
// by lines (360p … 2160p: the shorter edge, the longer one follows the project's aspect, a 9:16 project at 1080p is
// 1080 x 1920) or the project's own resolution. The engine has one preview: when the main window and the monitor want
// different sizes, the larger wins (MainWindow::updatePreviewEdge); nothing is rendered twice.

#include "core/I18n.h"

#include <QSettings>
#include <QSize>
#include <QString>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <iterator>

namespace PreviewResolution {

enum class Choice { Auto, P360, P540, P720, P1080, P1440, P2160, Project };
enum class Target { Viewer, Monitor };

inline constexpr Choice kChoices[] = {Choice::Auto,  Choice::P360,  Choice::P540,  Choice::P720,
                                      Choice::P1080, Choice::P1440, Choice::P2160, Choice::Project};
inline constexpr int kChoiceCount = int(std::size(kChoices));
// Automatic never asks for more than this longer edge (fitted viewers; playback speed first)
inline constexpr int kAutoCap = 1920;
// The engine's preview when nobody asks for a size (longer edge)
inline constexpr int kDefaultEdge = 960;

// Lines (the shorter edge) of a fixed size; 0 for Auto and Project
inline int lines(Choice choice)
{
    switch (choice) {
    case Choice::P360: return 360;
    case Choice::P540: return 540;
    case Choice::P720: return 720;
    case Choice::P1080: return 1080;
    case Choice::P1440: return 1440;
    case Choice::P2160: return 2160;
    case Choice::Auto:
    case Choice::Project: break;
    }
    return 0;
}

// A fixed size asks for more lines than the project has (the menu shows it disabled)
inline bool largerThanProject(Choice choice, const QSize& project)
{
    return lines(choice) > 0 && !project.isEmpty() && lines(choice) > std::min(project.width(), project.height());
}

// Preview frame size for a longer `edge` (Engine::setPreviewEdge): the project scaled down to it, even dimensions;
// 0 = kDefaultEdge, the project itself at or above its longer edge (never larger)
inline QSize sizeFor(const QSize& project, int edge)
{
    if (project.isEmpty()) return project;
    if (edge <= 0) edge = kDefaultEdge;
    if (edge >= std::max(project.width(), project.height())) return project;
    const QSize fit = project.scaled(edge, edge, Qt::KeepAspectRatio);
    return QSize(std::max(2, fit.width() & ~1), std::max(2, fit.height() & ~1));
}

// The longer edge to ask the engine for: `autoEdge` for Auto (what the display wants; 0 = the engine's default), the
// project size (INT_MAX) for Project and for a fixed size at or above the project's lines, else the longer edge of
// that many lines in the project's aspect
inline int edge(Choice choice, int autoEdge, const QSize& project)
{
    if (choice == Choice::Auto) return autoEdge;
    const int want = lines(choice);
    const int shorter = std::min(project.width(), project.height());
    if (choice == Choice::Project || want <= 0 || shorter <= 0 || want >= shorter) return INT_MAX;
    const int longer = std::max(project.width(), project.height());
    return int(std::lround(double(want) * longer / shorter));
}

// The preview size a choice gives for this project (menu tooltips, the viewer's notice); `autoEdge` as in edge()
inline QSize resultSize(Choice choice, int autoEdge, const QSize& project)
{
    return sizeFor(project, edge(choice, autoEdge, project));
}

// The choice's name in menus and the settings ("Automatisch (passend zur Anzeige)", "720p", "Projektauflösung")
inline QString label(Choice choice)
{
    switch (choice) {
    case Choice::Auto: return T("Automatisch (passend zur Anzeige)");
    case Choice::Project: return T("Projektauflösung");
    case Choice::P2160: return T("%1p (4K)").arg(lines(choice));
    default: break;
    }
    return QString("%1p").arg(lines(choice));
}

// A preview size as the user reads it: "1280×720"
inline QString sizeText(const QSize& size)
{
    return QString("%1×%2").arg(size.width()).arg(size.height());
}

inline const char* settingKey(Target target)
{
    return target == Target::Viewer ? "viewer/previewResolution" : "viewer/monitorResolution";
}

// Stored as "auto" (or nothing), "360" … "2160", "project". Older versions stored "half" and "full": full = the
// project, half = the fixed size nearest to half the project's lines (`project`: the format known when reading)
inline Choice fromStored(const QString& v, const QSize& project)
{
    if (v == "project" || v == "full") return Choice::Project;
    if (v == "half") {
        const int half = std::max(1, std::min(project.width(), project.height()) / 2);
        Choice best = Choice::P540;
        for (const Choice c : kChoices)
            if (lines(c) > 0 && std::abs(lines(c) - half) < std::abs(lines(best) - half)) best = c;
        return best;
    }
    for (const Choice c : kChoices)
        if (lines(c) > 0 && v == QString::number(lines(c))) return c;
    return Choice::Auto;
}

inline QString toStored(Choice choice)
{
    if (choice == Choice::Project) return "project";
    if (lines(choice) > 0) return QString::number(lines(choice));
    return "auto";
}

inline Choice stored(Target target, const QSize& project = QSize(1920, 1080))
{
    return fromStored(QSettings().value(settingKey(target)).toString(), project);
}

inline void store(Target target, Choice choice)
{
    QSettings().setValue(settingKey(target), toStored(choice));
}

} // namespace PreviewResolution
