#include "ui/EffectsLibrary.h"

#include "app/Theme.h"
#include "core/EffectRegistry.h"
#include "core/I18n.h"
#include "engine/ColorGrade.h"
#include "engine/Lumas.h"
#include "ui/MediaPool.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QSettings>
#include <QSplitter>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QUrl>
#include <QVBoxLayout>

namespace {

constexpr QSize kIcon{48, 27};

enum Category {
    All, VideoTransitions, AudioTransitions, Titles, OpenFx, Filters,
    Schneidi, SchneidiTransitions, SchneidiLuts, // mitgeliefert
    Luts, LutGroup, Transitions, TransitionGroup, // Effekte-Ordner (…Group = Unterordner)
};
constexpr int kGroupRole = Qt::UserRole + 1; // …Group: Unterordner relativ zum Ordner
constexpr const char* kLumaPrefix = "luma:"; // Listeneintrag einer Verlaufsblende: "luma:<Bild>"

// Schlüssel einer Kategorie fürs Speichern des Zugeklappt-Zustands
QString collapseKey(const QTreeWidgetItem* it)
{
    return QString::number(it->data(0, Qt::UserRole).toInt()) + ':' + it->data(0, kGroupRole).toString();
}
constexpr const char* kTitleData = "title";
constexpr const char* kEffectPrefix = "fx:"; // Listeneintrag eines Filters: "fx:<Effekt-ID>"

class EffectList : public QListWidget {
public:
    using QListWidget::QListWidget;

protected:
    QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override
    {
        auto* data = new QMimeData;
        if (items.isEmpty()) return data;
        const QString d = items.first()->data(Qt::UserRole).toString();
        if (d == kTitleData) data->setData(MediaPool::MimeType, QByteArray(MediaPool::TitleItem)); // wie "Text" im Pool
        else if (d.startsWith(kEffectPrefix)) data->setData(EffectsLibrary::EffectMimeType, d.mid(3).toUtf8());
        else if (d.startsWith(EffectFolders::LutPrefix)) data->setData(EffectsLibrary::EffectMimeType, d.toUtf8());
        else data->setData(EffectsLibrary::MimeType, d.toUtf8());
        return data;
    }
    QStringList mimeTypes() const override
    {
        return {EffectsLibrary::MimeType, EffectsLibrary::EffectMimeType, MediaPool::MimeType};
    }
};

// Kleines Vorschausymbol: A (blau) geht in B (orange) über
const QColor kA{0x3b, 0x6a, 0xa0};
const QColor kB{0xc8, 0x8a, 0x3c};

QPixmap transitionIcon(TransitionType type)
{
    QPixmap pm(kIcon);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    const QRect r = pm.rect();
    auto gradient = [&](std::initializer_list<QColor> stops) {
        QLinearGradient g(r.topLeft(), r.topRight());
        int i = 0;
        for (const QColor& c : stops) g.setColorAt(double(i++) / (int(stops.size()) - 1), c);
        p.fillRect(r, g);
    };
    auto wipe = [&](const QRect& b, const QPolygon& arrow) {
        p.fillRect(r, kA);
        p.fillRect(b, kB);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0xf0, 0xf0, 0xf0));
        p.drawPolygon(arrow);
    };
    const int w = r.width(), h = r.height(), cx = w / 2, cy = h / 2;
    switch (type) {
    case TransitionType::CrossDissolve: gradient({kA, kB}); break;
    case TransitionType::DipToColor: gradient({kA, Qt::black, kB}); break;
    case TransitionType::WipeRight:
        wipe(QRect(0, 0, cx, h), QPolygon({{cx - 4, cy - 5}, {cx + 4, cy}, {cx - 4, cy + 5}}));
        break;
    case TransitionType::WipeLeft:
        wipe(QRect(cx, 0, w - cx, h), QPolygon({{cx + 4, cy - 5}, {cx - 4, cy}, {cx + 4, cy + 5}}));
        break;
    case TransitionType::WipeDown:
        wipe(QRect(0, 0, w, cy), QPolygon({{cx - 5, cy - 4}, {cx + 5, cy - 4}, {cx, cy + 4}}));
        break;
    case TransitionType::WipeUp:
        wipe(QRect(0, cy, w, h - cy), QPolygon({{cx - 5, cy + 4}, {cx + 5, cy + 4}, {cx, cy - 4}}));
        break;
    }
    p.setPen(QColor(0, 0, 0, 120));
    p.setBrush(Qt::NoBrush);
    p.drawRect(r.adjusted(0, 0, -1, -1));
    return pm;
}

QPixmap audioIcon()
{
    QPixmap pm(kIcon);
    pm.fill(Theme::audioClip.darker(160));
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor(0xe0, 0xf0, 0xe0), 2));
    const QRectF r = QRectF(pm.rect()).adjusted(5, 5, -5, -5);
    QPainterPath out(r.topLeft()), in(r.bottomLeft());
    out.cubicTo(r.center().x(), r.top(), r.center().x(), r.bottom(), r.right(), r.bottom()); // leiser werden
    in.cubicTo(r.center().x(), r.bottom(), r.center().x(), r.top(), r.right(), r.top());      // lauter werden
    p.drawPath(out);
    p.drawPath(in);
    return pm;
}

QPixmap titleIcon()
{
    QPixmap pm(kIcon);
    pm.fill(Theme::titleClip.darker(135));
    QPainter p(&pm);
    QFont f = p.font();
    f.setPixelSize(17);
    f.setBold(true);
    p.setFont(f);
    p.setPen(QColor(0xf0, 0xf0, 0xf0));
    p.drawText(pm.rect(), Qt::AlignCenter, "T");
    return pm;
}

// Filter-Symbole: Farbkorrektur = Farbverlauf mit Helligkeitskeil, Unschärfe = weicher Kreis
QPixmap effectIcon(const QString& id)
{
    QPixmap pm(kIcon);
    pm.fill(QColor(0x2a, 0x2a, 0x30));
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = pm.rect();
    if (id == "color") {
        QLinearGradient hue(r.topLeft(), r.topRight());
        const QColor stops[] = {QColor(0x4a, 0x7a, 0xd0), QColor(0x60, 0xc0, 0x80), QColor(0xe0, 0xc0, 0x50),
                                QColor(0xe0, 0x70, 0x40)};
        for (int i = 0; i < 4; ++i) hue.setColorAt(i / 3.0, stops[i]);
        p.fillRect(r, hue);
        QLinearGradient shade(r.topLeft(), r.bottomLeft());
        shade.setColorAt(0, QColor(255, 255, 255, 90));
        shade.setColorAt(1, QColor(0, 0, 0, 150));
        p.fillRect(r, shade);
    } else if (id == "blur") {
        QRadialGradient g(r.center(), r.height() * 0.55);
        g.setColorAt(0, QColor(0xe8, 0xe8, 0xf0));
        g.setColorAt(0.45, QColor(0xb0, 0xb8, 0xd0, 200));
        g.setColorAt(1, QColor(0x2a, 0x2a, 0x30, 0));
        p.fillRect(r, g);
    }
    p.setPen(QColor(0, 0, 0, 120));
    p.setBrush(Qt::NoBrush);
    p.drawRect(r.adjusted(0, 0, -1, -1));
    return pm;
}

// Vorlage für LUT-Symbole: Farbton von links nach rechts, oben hell, unten dunkel, unten ein Graukeil
const QImage& lutSample()
{
    static const QImage img = [] {
        QImage im(kIcon, QImage::Format_RGBA8888);
        const int w = im.width(), h = im.height(), strip = 5;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                QColor c;
                if (y >= h - strip) {
                    const int g = x * 255 / (w - 1);
                    c = QColor(g, g, g);
                } else {
                    const double t = double(y) / (h - strip - 1);
                    c = QColor::fromHsvF(float(x) / w, 0.55f, float(0.95 - 0.6 * t));
                }
                im.setPixelColor(x, y, c);
            }
        return im;
    }();
    return img;
}

} // namespace

bool EffectsLibrary::parseTransition(const QByteArray& data, TrackKind* kind, TransitionStyle* style)
{
    const QString d = QString::fromUtf8(data);
    if (d.startsWith(kLumaPrefix)) {
        *kind = TrackKind::Video;
        *style = {};
        style->type = TransitionType::Luma;
        style->luma = d.mid(int(qstrlen(kLumaPrefix)));
        return !style->luma.isEmpty();
    }
    if (d.startsWith("audio:")) {
        for (const auto& i : kAudioCurves)
            if (d.mid(6) == i.id) {
                *kind = TrackKind::Audio;
                *style = {};
                style->audio = i.curve;
                return true;
            }
        return false;
    }
    if (!d.startsWith("video:")) return false;
    for (const auto& i : kTransitionTypes)
        if (d.mid(6) == i.id) {
            *kind = TrackKind::Video;
            *style = {};
            style->type = i.type;
            return true;
        }
    return false;
}

EffectsLibrary::EffectsLibrary(QWidget* parent) : QWidget(parent)
{
    setObjectName("Panel");

    auto* title = new QLabel(T("Effekte"));
    title->setObjectName("PanelTitle");
    m_search = new QLineEdit;
    m_search->setPlaceholderText(T("Suchen"));
    m_search->setClearButtonEnabled(true);
    m_search->setMaximumWidth(180);
    // Effekte importieren: öffnet den Effekte-Ordner (wie „Im Dateimanager zeigen“), Dateien dort erscheinen von selbst
    auto* import = new QToolButton;
    import->setText(T("Effekte importieren…"));
    import->setToolTip(T("Öffnet den Effekte-Ordner. LUTs (.cube, .3dl, .csp, Hald-CLUT) in den Ordner „LUTs“, "
                         "Übergänge (Graustufenbilder) in den Ordner „Transitions“ legen, Unterordner werden zu "
                         "Kategorien.\n%1")
                           .arg(QDir::toNativeSeparators(EffectFolders::root())));
    connect(import, &QToolButton::clicked, this, [] {
        EffectFolders::ensure();
        QDesktopServices::openUrl(QUrl::fromLocalFile(EffectFolders::root()));
    });
    auto* header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 4, 0);
    header->addWidget(title, 1);
    header->addWidget(import);
    header->addWidget(m_search);

    // Kategorien wie in DaVinci: Toolbox mit Unterpunkten; alles zuklappbar
    m_categories = new QTreeWidget;
    m_categories->setHeaderHidden(true);
    m_categories->setIndentation(12);
    auto add = [](auto* parent, const QString& text, int cat) {
        auto* it = new QTreeWidgetItem(parent, {text});
        it->setData(0, Qt::UserRole, cat);
        return it;
    };
    auto* toolbox = add(m_categories, T("Toolbox"), All);
    add(toolbox, T("Videoübergänge"), VideoTransitions);
    add(toolbox, T("Audioübergänge"), AudioTransitions);
    add(toolbox, T("Titel-Vorlagen"), Titles);
    // Open FX → Filter (wie DaVinci: dort liegen die ResolveFX-Filter)
    auto* openFx = add(m_categories, QStringLiteral("Open FX"), OpenFx);
    add(openFx, T("Filter"), Filters);
    // Mitgeliefert und eigener Effekte-Ordner
    auto* own = add(m_categories, QStringLiteral("schneidi"), Schneidi);
    add(own, T("Übergänge"), SchneidiTransitions);
    add(own, QStringLiteral("LUTs"), SchneidiLuts);
    m_lutRoot = add(m_categories, QStringLiteral("LUTs"), Luts);
    m_lutRoot->setToolTip(0, QDir::toNativeSeparators(EffectFolders::lutDir()));
    m_transRoot = add(m_categories, T("Übergänge"), Transitions);
    m_transRoot->setToolTip(0, QDir::toNativeSeparators(EffectFolders::transitionDir()));
    for (QTreeWidgetItem* it : {toolbox, openFx, own}) it->setExpanded(true);
    m_categories->setCurrentItem(toolbox);
    connect(m_categories, &QTreeWidget::itemExpanded, this, &EffectsLibrary::saveCollapsed);
    connect(m_categories, &QTreeWidget::itemCollapsed, this, &EffectsLibrary::saveCollapsed);

    m_list = new EffectList;
    m_list->setViewMode(QListView::ListMode);
    m_list->setIconSize(kIcon);
    m_list->setSpacing(1);
    m_list->setDragEnabled(true);
    m_list->setDragDropMode(QAbstractItemView::DragOnly);
    m_list->setToolTip(T("Auf einen Schnitt, einen Clip bzw. in die Timeline ziehen, Doppelklick = am Playhead bzw. "
                         "auf die Auswahl anwenden"));
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* it) {
        const QString d = it->data(Qt::UserRole).toString();
        if (d.isEmpty()) return;
        if (d == kTitleData) {
            emit titleRequested();
            return;
        }
        if (d.startsWith(kEffectPrefix)) {
            emit effectRequested(d.mid(3));
            return;
        }
        if (d.startsWith(EffectFolders::LutPrefix)) { // LUT -> wie ein Filter auf Clips
            emit effectRequested(d);
            return;
        }
        TrackKind kind;
        TransitionStyle style;
        if (parseTransition(d.toUtf8(), &kind, &style)) emit transitionRequested(kind, style);
    });

    auto* split = new QSplitter(Qt::Horizontal);
    split->addWidget(m_categories);
    split->addWidget(m_list);
    split->setStretchFactor(1, 1);
    split->setSizes({130, 230});

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addLayout(header);
    lay->addWidget(split, 1);

    // Effekte-Ordner beobachten (Ordner + Unterordner); Änderungen gesammelt nach kurzer Pause einlesen
    m_watcher = new QFileSystemWatcher(this);
    m_rescan = new QTimer(this);
    m_rescan->setSingleShot(true);
    m_rescan->setInterval(300);
    connect(m_rescan, &QTimer::timeout, this, &EffectsLibrary::rescanFolders);
    connect(m_watcher, &QFileSystemWatcher::directoryChanged, m_rescan, qOverload<>(&QTimer::start));

    connect(m_categories, &QTreeWidget::currentItemChanged, this, &EffectsLibrary::rebuild);
    connect(m_search, &QLineEdit::textChanged, this, &EffectsLibrary::rebuild);
    EffectFolders::ensure();
    rescanFolders();
}

void EffectsLibrary::saveCollapsed()
{
    QStringList collapsed;
    for (QTreeWidgetItemIterator it(m_categories); *it; ++it)
        if ((*it)->childCount() && !(*it)->isExpanded()) collapsed << collapseKey(*it);
    QSettings().setValue("effects/collapsed", collapsed);
}

void EffectsLibrary::fillGroups(QTreeWidgetItem* root, int groupCategory, const QVector<EffectFolders::LutEntry>& entries)
{
    QHash<QString, QTreeWidgetItem*> groups;
    for (const EffectFolders::LutEntry& e : entries) {
        QString path;
        QTreeWidgetItem* parent = root;
        for (const QString& part : e.group.split('/', Qt::SkipEmptyParts)) {
            path = path.isEmpty() ? part : path + '/' + part;
            QTreeWidgetItem*& it = groups[path];
            if (!it) {
                it = new QTreeWidgetItem(parent, {part});
                it->setData(0, Qt::UserRole, groupCategory);
                it->setData(0, kGroupRole, path);
            }
            parent = it;
        }
    }
}

void EffectsLibrary::rescanFolders()
{
    if (!m_watcher->directories().isEmpty()) m_watcher->removePaths(m_watcher->directories());
    m_watcher->addPaths(EffectFolders::watchDirs());
    m_userLuts = EffectFolders::userLuts();
    m_userTransitions = EffectFolders::userTransitions();

    // Unterkategorien = Unterordner (verschachtelt wie im Ordner); Auswahl bleibt, wenn es sie noch gibt
    const QSignalBlocker block(m_categories);
    const QTreeWidgetItem* cur = m_categories->currentItem();
    const QString curKey = cur ? collapseKey(cur) : QString();
    const int curCat = cur ? cur->data(0, Qt::UserRole).toInt() : -1; // cur wird gleich evtl. gelöscht
    for (auto [root, group, entries] : {std::tuple{m_lutRoot, int(LutGroup), &m_userLuts},
                                        std::tuple{m_transRoot, int(TransitionGroup), &m_userTransitions}}) {
        const bool wasInside = curCat == group;
        qDeleteAll(root->takeChildren());
        fillGroups(root, group, *entries);
        if (wasInside) m_categories->setCurrentItem(root); // unten genauer, falls es den Ordner noch gibt
    }
    // Zugeklappt-Zustand wiederherstellen (Standard: alles offen)
    const QStringList collapsed = QSettings().value("effects/collapsed").toStringList();
    for (QTreeWidgetItemIterator it(m_categories); *it; ++it) {
        if ((*it)->childCount()) (*it)->setExpanded(!collapsed.contains(collapseKey(*it)));
        if (!curKey.isEmpty() && collapseKey(*it) == curKey) m_categories->setCurrentItem(*it);
    }
    rebuild();
}

QPixmap EffectsLibrary::fileIcon(const QString& path, bool lut, QString* error)
{
    const QFileInfo fi(path);
    const qint64 stamp = fi.lastModified().toMSecsSinceEpoch() ^ (fi.size() << 20);
    const QString key = (lut ? "lut|" : "luma|") + path;
    auto it = m_lutIcons.find(key);
    if (it == m_lutIcons.end() || it->stamp != stamp) {
        IconEntry e;
        e.stamp = stamp;
        QImage img;
        if (lut) { // ohne Zwischenspeicher lesen: große Ordner sollen nicht dauerhaft im Speicher bleiben
            if (const auto l = ColorGrade::parseLut(path, &e.error)) {
                img = lutSample().copy();
                ColorGrade::apply(img.bits(), img.width(), img.height(), ColorGrade::Params(), l.get());
            }
        } else if (const QImage luma = Lumas::image(path, kIcon.width(), kIcon.height(), false, &e.error); !luma.isNull()) {
            img = Lumas::preview(luma);
        }
        if (!img.isNull()) {
            QPainter p(&img);
            p.setPen(QColor(0, 0, 0, 120));
            p.drawRect(img.rect().adjusted(0, 0, -1, -1));
            p.end();
            e.icon = QPixmap::fromImage(img);
        } else {
            QPixmap pm(kIcon);
            pm.fill(QColor(0x2a, 0x2a, 0x30));
            QPainter p(&pm);
            p.setPen(Theme::warning);
            p.drawText(pm.rect(), Qt::AlignCenter, QStringLiteral("!"));
            e.icon = pm;
        }
        it = m_lutIcons.insert(key, e);
    }
    if (error) *error = it->error;
    return it->icon;
}

void EffectsLibrary::rebuild()
{
    m_list->clear();
    const QTreeWidgetItem* cur = m_categories->currentItem();
    const int shown = cur ? cur->data(0, Qt::UserRole).toInt() : int(All);
    const QString group = cur ? cur->data(0, kGroupRole).toString() : QString();
    const QString filter = m_search->text().trimmed();

    struct Entry { QString name; QString data; QPixmap icon; QString tip; bool ok = true; };
    struct Group { QString header; QVector<Entry> entries; };
    QVector<Group> groups;

    // Dateien aus dem Effekte-Ordner bzw. mitgeliefert (Symbole kosten das Einlesen -> nur für die gewählte Kategorie)
    auto files = [&](const QVector<EffectFolders::LutEntry>& list, bool lut, bool builtin) {
        QVector<Entry> out;
        for (const auto& l : list) {
            // Oberkategorie zeigt alles, eine Unterkategorie ihren Ordner samt Unterordnern
            if (!builtin && !group.isEmpty() && l.group != group && !l.group.startsWith(group + '/')) continue;
            QString error;
            const QPixmap icon = fileIcon(l.path, lut, &error);
            QString tip = builtin ? (lut ? T("LUT (mitgeliefert)") : T("Übergang (mitgeliefert)"))
                                  : QDir::toNativeSeparators(l.path);
            if (!error.isEmpty()) tip += "\n" + T("Lässt sich nicht lesen: %1").arg(error);
            out << Entry{l.name, (lut ? EffectFolders::LutPrefix : kLumaPrefix) + l.path, icon, tip, error.isEmpty()};
        }
        return out;
    };
    auto videoTransitions = [&] {
        QVector<Entry> out;
        for (const auto& i : kTransitionTypes)
            if (i.type != TransitionType::Luma) // Verlaufsblenden kommen einzeln (je Bild)
                out << Entry{T(i.name), QString("video:%1").arg(i.id), transitionIcon(i.type)};
        return out + files(EffectFolders::builtinTransitions(), false, true);
    };
    auto audioTransitions = [&] {
        QVector<Entry> out;
        for (const auto& i : kAudioCurves)
            out << Entry{QString::fromLatin1(i.name), QString("audio:%1").arg(i.id), audioIcon()};
        return out;
    };
    auto filters = [&] {
        QVector<Entry> out;
        for (const auto& e : EffectRegistry::all())
            if (e.library) out << Entry{e.name, kEffectPrefix + e.id, effectIcon(e.id)};
        return out;
    };
    const QVector<Entry> titles{Entry{QStringLiteral("Text"), kTitleData, titleIcon()}};

    switch (shown) {
    case All: // Toolbox: alle Gruppen mit Kopfzeile wie in DaVinci
        groups = {{T("Videoübergänge"), videoTransitions()}, {T("Audioübergänge"), audioTransitions()},
                  {T("Titel-Vorlagen"), titles}};
        break;
    case VideoTransitions: groups = {{{}, videoTransitions()}}; break;
    case AudioTransitions: groups = {{{}, audioTransitions()}}; break;
    case Titles: groups = {{{}, titles}}; break;
    case OpenFx:
    case Filters: groups = {{{}, filters()}}; break;
    case Schneidi:
        groups = {{T("Übergänge"), files(EffectFolders::builtinTransitions(), false, true)},
                  {QStringLiteral("LUTs"), files(EffectFolders::builtinLuts(), true, true)}};
        break;
    case SchneidiTransitions: groups = {{{}, files(EffectFolders::builtinTransitions(), false, true)}}; break;
    case SchneidiLuts: groups = {{{}, files(EffectFolders::builtinLuts(), true, true)}}; break;
    case Luts:
    case LutGroup: groups = {{{}, files(m_userLuts, true, false)}}; break;
    case Transitions:
    case TransitionGroup: groups = {{{}, files(m_userTransitions, false, false)}}; break;
    }

    for (const Group& g : groups) {
        QVector<const Entry*> hits;
        for (const Entry& e : g.entries)
            if (filter.isEmpty() || e.name.contains(filter, Qt::CaseInsensitive)) hits << &e;
        if (hits.isEmpty()) continue;
        if (!g.header.isEmpty()) {
            auto* h = new QListWidgetItem(g.header);
            h->setFlags(Qt::NoItemFlags);
            h->setForeground(Theme::textDim);
            QFont f = h->font();
            f.setBold(true);
            h->setFont(f);
            m_list->addItem(h);
        }
        for (const Entry* e : hits) {
            auto* it = new QListWidgetItem(QIcon(e->icon), e->name);
            it->setData(Qt::UserRole, e->ok ? e->data : QString()); // kaputte Datei: sichtbar, aber nicht anwendbar
            it->setFlags(e->ok ? Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled : Qt::ItemIsEnabled);
            if (!e->ok) it->setForeground(Theme::warning);
            if (!e->tip.isEmpty()) it->setToolTip(e->tip);
            m_list->addItem(it);
        }
    }
    if (m_list->count() == 0 && filter.isEmpty()) {
        QString hint;
        if (shown == Luts || shown == LutGroup) hint = T("Noch keine LUTs – „Effekte importieren…“ öffnet den Ordner");
        if (shown == Transitions || shown == TransitionGroup)
            hint = T("Noch keine Übergänge – „Effekte importieren…“ öffnet den Ordner");
        if (!hint.isEmpty()) {
            auto* h = new QListWidgetItem(hint);
            h->setFlags(Qt::NoItemFlags);
            h->setForeground(Theme::textDim);
            m_list->addItem(h);
        }
    }
}
