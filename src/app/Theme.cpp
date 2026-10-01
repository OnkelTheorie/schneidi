#include "app/Theme.h"

#include "core/I18n.h"

#include <QApplication>
#include <QFile>
#include <QFont>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPalette>
#include <QSettings>
#include <QStyleFactory>

#include <algorithm>

namespace Theme {

namespace {

struct Role {
    const char* key;
    QColor* color;
    const char* label = nullptr; // nur Signalfarben (im Dialog einzeln einstellbar)
};

// Design-Farben: Werte kommen aus der JSON-Datei, fehlende Schlüssel aus „Dark Orange“ (Vorgabe in Theme.h)
const QList<Role>& designRoles()
{
    static const QList<Role> roles = {
        {"window", &window},         {"panel", &panel},           {"panelHeader", &panelHeader},
        {"topBar", &topBar},         {"border", &border},         {"well", &well},
        {"field", &field},           {"control", &control},       {"controlLight", &controlLight},
        {"controlOff", &controlOff}, {"text", &text},             {"textDim", &textDim},
        {"textFaint", &textFaint},   {"timelineBg", &timelineBg}, {"trackBg", &trackBg},
        {"trackBgAlt", &trackBgAlt}, {"trackHeader", &trackHeader}, {"ruler", &ruler},
        {"lane", &lane},             {"viewerBg", &viewerBg},     {"thumbBg", &thumbBg},
        {"primary", &primary},       {"secondary", &secondary},
    };
    return roles;
}

const QList<Role>& signalRoles()
{
    static const QList<Role> roles = {
        {"playhead", &playhead, N_("Playhead")},
        {"videoClip", &videoClip, N_("Videoclips")},
        {"audioClip", &audioClip, N_("Audioclips")},
        {"titleClip", &titleClip, N_("Titel")},
        {"compoundClip", &compoundClip, N_("Compound Clips")},
        {"subtitleClip", &subtitleClip, N_("Untertitel")},
        {"meterLow", &meterLow, N_("Pegel grün")},
        {"meterMid", &meterMid, N_("Pegel gelb")},
        {"meterHigh", &meterHigh, N_("Pegel rot")},
        {"warning", &warning, N_("Übersteuerung / Fehler")},
        {"quiet", &quiet, N_("Lautheit zu leise")},
        {"cacheReady", &cacheReady, N_("Render-Cache fertig")},
        {"cacheMissing", &cacheMissing, N_("Render-Cache fehlt")},
    };
    return roles;
}

// Vorgabewerte (Stand von Theme.h beim Programmstart), bevor activate() sie überschreibt
const Colors& builtinValues()
{
    static const Colors values = [] {
        Colors c;
        for (const Role& r : designRoles()) c.insert(r.key, *r.color);
        for (const Role& r : signalRoles()) c.insert(r.key, *r.color);
        return c;
    }();
    return values;
}

const QStringList kOrder = {"dark-orange", "midnight", "graphit", "hell", "beige"};

QJsonObject readDesign(const QString& id)
{
    QFile f(QString(":/themes/%1.json").arg(id));
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

bool isSignal(const QString& key)
{
    return std::any_of(signalRoles().begin(), signalRoles().end(), [&](const Role& r) { return key == r.key; });
}

} // namespace

QList<Design> designs()
{
    QList<Design> out;
    for (const QString& id : kOrder) {
        const QJsonObject o = readDesign(id);
        if (!o.isEmpty()) out.push_back({id, T(o.value("name").toString(id)), T(o.value("description").toString())});
    }
    return out;
}

QString defaultDesign() { return kOrder.first(); }

QString savedDesign()
{
    const QString id = QSettings().value("design/id").toString();
    return kOrder.contains(id) ? id : defaultDesign();
}

QStringList signalKeys()
{
    QStringList keys;
    for (const Role& r : signalRoles()) keys << r.key;
    return keys;
}

QString signalLabel(const QString& key)
{
    for (const Role& r : signalRoles())
        if (key == r.key) return T(r.label);
    return key;
}

Colors defaults(const QString& design)
{
    Colors c = builtinValues();
    const QJsonObject colors = readDesign(design).value("colors").toObject();
    for (auto it = colors.begin(); it != colors.end(); ++it) {
        const QColor v(it.value().toString());
        if (v.isValid() && c.contains(it.key()) && !isSignal(it.key())) c.insert(it.key(), v);
    }
    return c;
}

Colors saved(const QString& design)
{
    Colors c = defaults(design);
    QSettings s;
    for (const char* key : {"primary", "secondary"}) {
        const QColor v(s.value(QString("design/%1/%2").arg(design, key)).toString());
        if (v.isValid()) c.insert(key, v);
    }
    for (const Role& r : signalRoles()) {
        const QColor v(s.value(QString("signal/%1").arg(r.key)).toString());
        if (v.isValid()) c.insert(r.key, v);
    }
    return c;
}

void save(const QString& design, const Colors& chosen)
{
    QSettings s;
    s.setValue("design/id", design);
    const Colors def = defaults(design);
    auto store = [&](const QString& settingsKey, const QString& key) {
        const QColor v = chosen.value(key);
        if (v.isValid() && v != def.value(key)) s.setValue(settingsKey, v.name());
        else s.remove(settingsKey);
    };
    for (const char* key : {"primary", "secondary"}) store(QString("design/%1/%2").arg(design, key), key);
    for (const Role& r : signalRoles()) store(QString("signal/%1").arg(r.key), r.key);
}

void activate(const Colors& colors)
{
    for (const QList<Role>* roles : {&designRoles(), &signalRoles()})
        for (const Role& r : *roles)
            if (const QColor v = colors.value(r.key); v.isValid()) *r.color = v;
    onPrimary = readableOn(primary);
    clipSelected = selectionFrom(primary);
}

static Colors& loadedColors()
{
    static Colors c;
    return c;
}

void load(const QString& overrideDesign)
{
    builtinValues(); // Vorgaben merken, bevor sie überschrieben werden
    loadedColors() = saved(kOrder.contains(overrideDesign) ? overrideDesign : savedDesign());
    activate(loadedColors());
}

bool restartNeeded()
{
    return saved(savedDesign()) != loadedColors();
}

QColor mix(const QColor& a, const QColor& b, double t)
{
    auto ch = [t](int x, int y) { return int(x + (y - x) * t + 0.5); };
    return QColor(ch(a.red(), b.red()), ch(a.green(), b.green()), ch(a.blue(), b.blue()), ch(a.alpha(), b.alpha()));
}

QColor readableOn(const QColor& background)
{
    const double y = 0.299 * background.red() + 0.587 * background.green() + 0.114 * background.blue();
    return y > 128 ? QColor(Qt::black) : QColor(Qt::white);
}

// Rand ausgewählter Clips: Primärfarbe, aber hell genug, um sich von den Clipfarben abzuheben
QColor selectionFrom(const QColor& c)
{
    float h, s, l, a;
    c.getHslF(&h, &s, &l, &a);
    return QColor::fromHslF(h, s, std::max(l, 0.62f), a);
}

QColor alpha(const QColor& c, int a)
{
    QColor out = c;
    out.setAlpha(a);
    return out;
}

QString primaryButtonStyle()
{
    const QColor off = mix(primary, panel, 0.7);
    return QString("QPushButton { background: %1; color: %2; font-weight: 600; border-radius: 3px; }"
                   "QPushButton:hover { background: %3; }"
                   "QPushButton:disabled { background: %4; color: %5; }")
        .arg(primary.name(), onPrimary.name(), primary.lighter(110).name(), off.name(), mix(onPrimary, off, 0.5).name());
}

void apply(QApplication& app)
{
    app.setStyle(QStyleFactory::create("Fusion")); // sieht unter Linux und Windows gleich aus

    QPalette p;
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, panel);
    p.setColor(QPalette::AlternateBase, panelHeader);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, panelHeader);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::Highlight, primary);
    p.setColor(QPalette::HighlightedText, onPrimary);
    p.setColor(QPalette::ToolTipBase, panelHeader);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::PlaceholderText, textDim);
    p.setColor(QPalette::Link, primary);
    p.setColor(QPalette::Disabled, QPalette::Text, textDim);
    p.setColor(QPalette::Disabled, QPalette::WindowText, textDim);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, textDim);
    app.setPalette(p);

    QFont f = app.font();
    f.setPointSizeF(9.0);
#ifdef Q_OS_WIN
    // Symbole wie ⏮ ⏸ ✂ hat Segoe UI nicht; ohne Segoe UI Symbol als Ersatz nimmt Windows die farbige Emoji-Schrift
    f.setFamilies({f.family(), "Segoe UI Symbol"});
#endif
    app.setFont(f);

    app.setStyleSheet(QString(R"(
        QMainWindow, QWidget#Root { background: %1; }
        QSplitter::handle { background: %2; }
        QSplitter::handle:horizontal { width: 3px; }
        QSplitter::handle:vertical { height: 3px; }
        QWidget#Panel { background: %3; }
        QLabel#PanelTitle { color: %5; font-weight: 600; padding: 4px 8px; background: %4; }
        QToolButton { color: %5; background: transparent; border: none; padding: 3px 6px; border-radius: 3px; }
        QToolButton:hover { background: %8; }
        QToolButton:checked { color: %6; background: %8; }
        QToolButton:disabled { color: %10; }
        QListWidget { background: %3; border: none; }
        QListWidget::item:selected { background: %8; border: 1px solid %6; }
        QScrollBar { background: %1; border: none; }
        QScrollBar:horizontal { height: 10px; }
        QScrollBar:vertical { width: 10px; }
        QScrollBar::handle { background: %9; border-radius: 4px; min-width: 20px; min-height: 20px; }
        QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
        QMenuBar { background: %1; color: %5; }
        QMenuBar::item:selected { background: %8; }
        QMenu { background: %4; color: %5; border: 1px solid %2; }
        QMenu::item:selected { background: %8; }
        QMenu::item:disabled { color: %7; }
        QStatusBar { background: %1; color: %7; }
    )")
                          .arg(window.name(), border.name(), panel.name(), panelHeader.name(), text.name(),
                               primary.name(), textDim.name(), control.name(), controlLight.name())
                          .arg(textFaint.name()));
}

} // namespace Theme
