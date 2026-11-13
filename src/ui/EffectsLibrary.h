// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "core/Types.h"

#include "core/EffectFolders.h"

#include <QHash>
#include <QKeySequence>
#include <QPixmap>
#include <QWidget>
#include <functional>

class QFileSystemWatcher;
class QLineEdit;
class QTimer;
class QListWidget;
class QTreeWidget;

// Effects Library (Edit page): on the left categories (Favourites, Recently used; Transitions → Video, Audio, gradient
// wipes; Titles; Shapes & drawing; Video effects and Audio effects → their purpose (EffectCategory, the registry's);
// LUTs; Sounds; Compositing recipes; Presets (Shotcut/Kdenlive); bundled/custom below those with files, custom = effects
// folder, its subfolders as subcategories), on the right the entries with preview icon, under small group headers
// where a category shows several groups. The search looks through everything and names each hit's group. Categories
// can be collapsed (saved). Favourites: context menu or key F on an entry (a star on its icon), recently used = the
// last entries applied (double-click or a drop that was taken; core/EffectFavorites). "Import effects…" opens the effects folder, new files appear on their own. Dragging onto a cut/clip edge creates
// the transition, "Text" ends up as a title on the video track, a filter on a video clip attaches the effect;
// double-click applies the entry at the playhead/to the selection. "Klänge" (effect sounds of op 'sfx': built-in
// recordings and computed ones, installed sound packs, the user's own folder): double-click = listen, dragging into
// the timeline places it as op 'sfx' does (its moment where it is dropped, own SFX track, one undo step). Nothing is
// ever placed by itself.
class EffectsLibrary : public QWidget {
    Q_OBJECT
public:
    // Drag data of a transition: "video:<kind id>", "luma:<image>" (gradient wipe) resp. "audio:<curve id>"
    // (titles use MediaPool::MimeType)
    static constexpr const char* MimeType = "application/x-schneidi-transition";
    static bool parseTransition(const QByteArray& data, TrackKind* kind, TransitionStyle* style);
    // Drag data of a filter (Open FX): id from the EffectRegistry, resp. "lut:<path>" for a LUT, "preset:<path>"
    static constexpr const char* EffectMimeType = "application/x-schneidi-effect";
    // Drag data of an effect sound ("Klänge"): the fields of op 'sfx' without op and at, as JSON ({"kind":"whoosh",
    // "seed":2}, {"kind":"riser","length":"0.9s","seed":1} = computed, {"kind":"<file>"} = a pack's or the user's file)
    static constexpr const char* SoundMimeType = "application/x-schneidi-sound";
    // The file a sound entry plays (built-in: made into the cache on first use); empty = none
    static QString soundFile(const QByteArray& data);

    explicit EffectsLibrary(QWidget* parent = nullptr);

    // Compositing recipes ("Comp-Rezepte", entry data "recipe:<id>" sent through effectRequested / the effect drag):
    // the list anew after an own recipe was saved, the category shown, sharing as ZIP (core/CompRecipes.h; the
    // context menu asks for the file). importRecipe returns the new recipe's id (empty + err = refused)
    void refreshRecipes();
    void showRecipes();
    bool exportRecipe(const QString& id, const QString& zipPath, QString* err = nullptr);
    QString importRecipe(const QString& zipPath, QString* err = nullptr);
    // The paint tool's key, shown in the "Malen" entry ("Malen (P)"); empty = none
    void setDrawShortcut(const QKeySequence& key);
    // Favourite of the list entry with this data (tests; the user: context menu, key F)
    void toggleFavorite(const QString& data);
    QListWidget* list() const { return m_list; } // tests
    qsizetype cachedIconCount() const { return m_icons.size(); } // tests

signals:
    void transitionRequested(TrackKind kind, const TransitionStyle& style);
    void titleRequested(int kind); // TitleKind
    void effectRequested(const QString& effectId);
    void soundPreviewRequested(const QString& file); // double-click on a sound: hear it (nothing is placed)
    void shapeRequested(const QString& kindId);       // a shape title of this kind ("arrow") at the playhead
    void drawRequested();                             // "Malen": the paint tool on

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void rebuild();
    void rescanFolders(); // re-read custom LUTs/transitions (folder changed), subcategories anew
    void fillGroups(class QTreeWidgetItem* root, int groupCategory, const QVector<EffectFolders::LutEntry>& entries);
    QPixmap fileIcon(const QString& path, bool lut, QString* error); // LUT: sample image, transition: A -> B
    // Drawn icons (transitions, filters, titles, sounds, recipes) per entry key, made once: typing in the search
    // rebuilds the list on every key, the icons are reused (the filter icons render a picture each)
    const QPixmap& cachedIcon(const QString& key, const std::function<QPixmap()>& make);
    void saveCollapsed();
    void noteUsed(const QString& data); // recently used (and the list anew when it is shown)
    void rebuildKeepingPlace();         // rebuild with the same entry selected and the same scroll position
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
    class QTreeWidgetItem* m_soundRoot = nullptr;
    QVector<EffectFolders::LutEntry> m_userLuts, m_userTransitions, m_userPresets, m_userSounds;
    QFileSystemWatcher* m_watcher;
    QTimer* m_rescan;
    struct IconEntry {
        qint64 stamp = 0; // modification time + size of the file
        QPixmap icon;
        QString error;
    };
    QHash<QString, IconEntry> m_lutIcons;
    QHash<QString, PresetEntry> m_presets;
    QHash<QString, QPixmap> m_icons; // cachedIcon
    QString m_drawKey;
};
