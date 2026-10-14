#pragma once
#include "core/Types.h"

#include "core/EffectFolders.h"

#include <QHash>
#include <QPixmap>
#include <QWidget>

class QFileSystemWatcher;
class QLineEdit;
class QTimer;
class QListWidget;
class QTreeWidget;

// Effects Library (Edit page like DaVinci): on the left categories (Toolbox → Video Transitions, Audio Transitions,
// Titles; Open FX → Filters; Presets (Shotcut/Kdenlive); LUTs and gradient wipes → each Bundled/Custom, Custom = effects folder,
// its subfolders as subcategories), on the right the entries with preview icon. Categories can be collapsed
// (saved). "Import effects…" opens the effects folder, new files appear on their own. Dragging onto a cut/clip edge creates
// the transition, "Text" ends up as a title on the video track, a filter on a video clip attaches the effect;
// double-click applies the entry at the playhead/to the selection.
class EffectsLibrary : public QWidget {
    Q_OBJECT
public:
    // Drag data of a transition: "video:<kind id>", "luma:<image>" (gradient wipe) resp. "audio:<curve id>"
    // (titles use MediaPool::MimeType)
    static constexpr const char* MimeType = "application/x-schneidi-transition";
    static bool parseTransition(const QByteArray& data, TrackKind* kind, TransitionStyle* style);
    // Drag data of a filter (Open FX): id from the EffectRegistry, resp. "lut:<path>" for a LUT, "preset:<path>"
    static constexpr const char* EffectMimeType = "application/x-schneidi-effect";

    explicit EffectsLibrary(QWidget* parent = nullptr);

signals:
    void transitionRequested(TrackKind kind, const TransitionStyle& style);
    void titleRequested();
    void effectRequested(const QString& effectId);

private:
    void rebuild();
    void rescanFolders(); // re-read custom LUTs/transitions (folder changed), subcategories anew
    void fillGroups(class QTreeWidgetItem* root, int groupCategory, const QVector<EffectFolders::LutEntry>& entries);
    QPixmap fileIcon(const QString& path, bool lut, QString* error); // LUT: sample image, transition: A -> B
    void saveCollapsed();
    struct PresetEntry {
        qint64 stamp = 0;
        QString name, tip;
        QPixmap icon;
        bool ok = false;
    };
    const PresetEntry& presetEntry(const QString& path); // read + mapped, cached until the file changes

    QTreeWidget* m_categories;
    QLineEdit* m_search;
    QListWidget* m_list;
    class QTreeWidgetItem* m_lutRoot = nullptr;
    class QTreeWidgetItem* m_transRoot = nullptr;
    class QTreeWidgetItem* m_presetRoot = nullptr;
    QVector<EffectFolders::LutEntry> m_userLuts, m_userTransitions, m_userPresets;
    QFileSystemWatcher* m_watcher;
    QTimer* m_rescan;
    struct IconEntry {
        qint64 stamp = 0; // modification time + size of the file
        QPixmap icon;
        QString error;
    };
    QHash<QString, IconEntry> m_lutIcons;
    QHash<QString, PresetEntry> m_presets;
};
