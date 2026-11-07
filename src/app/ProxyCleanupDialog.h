// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// "Clean up proxy media…" (Playback menu, Media Pool): shows which proxy files no open or recent project needs, why,
// and how much space deleting them frees (engine/ProxyCleanup has the rule), and deletes them only when confirmed.

#include <QStringList>
#include <QtGlobal>

#include <functional>

class QWidget;
class ProxyManager;
struct ProjectData;

namespace ProxyCleanupDialog {

// open: the project open in the app right now (its proxies and cutout analyses always stay; asked again after the
// dialog, as it may have changed meanwhile - an analysis finished, an AI imported). openFiles: more files of the open
// project whose media and analyses stay (its autosave). Returns the bytes freed (0 = nothing deleted).
qint64 run(QWidget* parent, const std::function<ProjectData()>& open, const ProxyManager* proxies,
           const QStringList& openFiles = {});

} // namespace ProxyCleanupDialog
