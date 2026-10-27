#pragma once
// "Clean up proxy media…" (Playback menu, Media Pool): shows which proxy files no open or recent project needs, why,
// and how much space deleting them frees (engine/ProxyCleanup has the rule), and deletes them only when confirmed.

#include <QtGlobal>

class QWidget;
class ProxyManager;
struct ProjectData;

namespace ProxyCleanupDialog {

// open: the project open in the app (its proxies always stay). Returns the bytes freed (0 = nothing deleted).
qint64 run(QWidget* parent, const ProjectData& open, const ProxyManager* proxies);

} // namespace ProxyCleanupDialog
