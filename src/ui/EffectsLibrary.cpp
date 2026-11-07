// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/EffectsLibrary.h"

#include "app/Theme.h"
#include "core/EffectRegistry.h"
#include "core/I18n.h"
#include "core/Presets.h"
#include "core/SfxSynth.h"
#include "engine/Extensions.h"
#include "engine/ColorGrade.h"
#include "engine/Lumas.h"
#include "engine/VideoEffects.h"
#include "ui/MediaPool.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineF>
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
#include <algorithm>
#include <cmath>
#include <numbers>

namespace {

constexpr QSize kIcon{48, 27};

enum Category {
    All, VideoTransitions, AudioTransitions, Titles, OpenFx, Filters,
    LutsAll, BuiltinLuts, Luts, LutGroup,                         // LUTs: bundled + custom
    LumasAll, BuiltinTransitions, Transitions, TransitionGroup,   // Gradient wipes: bundled + custom
    PresetsAll, PresetGroup,                                      // Shotcut/Kdenlive presets (own folder only)
    AudioFx,                                                      // audio effects (DaVinci: Audio FX → Fairlight FX)
    SoundsAll, BuiltinSounds, PackSounds, Sounds, SoundGroup,     // effect sounds: built-in, packs, own folder
    CompRecipes,                                                  // compositing recipes (0.19, op `comp recipe`)
    // Luts/Transitions = own effects folder, …Group = its subfolder
    // (numbers are saved with the collapsed state: append new categories at the end)
};
constexpr int kGroupRole = Qt::UserRole + 1; // …Group: subfolder relative to the folder
constexpr const char* kLumaPrefix = "luma:"; // List entry of a gradient wipe: "luma:<image>"

// Key of a category for saving the collapsed state
QString collapseKey(const QTreeWidgetItem* it)
{
    return QString::number(it->data(0, Qt::UserRole).toInt()) + ':' + it->data(0, kGroupRole).toString();
}
constexpr const char* kTitleData = "title";
constexpr const char* kCounterData = "title:counter";
constexpr const char* kProgressData = "title:progress";
constexpr const char* kEffectPrefix = "fx:"; // List entry of a filter: "fx:<effect ID>"
constexpr const char* kSoundPrefix = "sound:"; // List entry of an effect sound: "sound:<op fields as JSON>"

// The shown name of a sound kind
QString soundKindName(SfxSynth::Kind k)
{
    switch (k) {
    case SfxSynth::Kind::Whoosh: return T("Whoosh");
    case SfxSynth::Kind::Riser: return T("Riser");
    case SfxSynth::Kind::Hit: return T("Treffer");
    case SfxSynth::Kind::Impact: return T("Einschlag");
    case SfxSynth::Kind::Click: return T("Klick");
    case SfxSynth::Kind::Swish: return T("Swish");
    }
    return {};
}

QString soundData(const QJsonObject& fields)
{
    return kSoundPrefix + QString::fromUtf8(QJsonDocument(fields).toJson(QJsonDocument::Compact));
}

QString secondsText(double s) { return T("%1 s").arg(QString::number(s, 'f', 2).replace('.', I18n::decimalPoint())); }

// A speaker as icon (sounds have no picture)
QPixmap soundIcon(bool own)
{
    QPixmap pm(kIcon);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const QColor c = own ? QColor(0xc8, 0x8a, 0x3c) : QColor(0x3b, 0x9a, 0x8a);
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    const int cy = kIcon.height() / 2;
    p.drawPolygon(QPolygon({{12, cy - 4}, {17, cy - 4}, {23, cy - 9}, {23, cy + 9}, {17, cy + 4}, {12, cy + 4}}));
    p.setPen(QPen(c, 1.6));
    p.setBrush(Qt::NoBrush);
    for (int r : {5, 9}) p.drawArc(QRectF(23 - r, cy - r, 2 * r, 2 * r), -45 * 16, 90 * 16);
    return pm;
}

class EffectList : public QListWidget {
public:
    using QListWidget::QListWidget;

protected:
    QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override
    {
        auto* data = new QMimeData;
        if (items.isEmpty()) return data;
        const QString d = items.first()->data(Qt::UserRole).toString();
        if (d == kTitleData) data->setData(MediaPool::MimeType, QByteArray(MediaPool::TitleItem)); // like "Text" in the pool
        else if (d == kCounterData) data->setData(MediaPool::MimeType, QByteArray(MediaPool::CounterItem));
        else if (d == kProgressData) data->setData(MediaPool::MimeType, QByteArray(MediaPool::ProgressItem));
        else if (d.startsWith(kEffectPrefix)) data->setData(EffectsLibrary::EffectMimeType, d.mid(3).toUtf8());
        else if (d.startsWith(kSoundPrefix)) data->setData(EffectsLibrary::SoundMimeType, d.mid(6).toUtf8());
        else if (d.startsWith(EffectFolders::LutPrefix) || d.startsWith(Presets::Prefix))
            data->setData(EffectsLibrary::EffectMimeType, d.toUtf8());
        else data->setData(EffectsLibrary::MimeType, d.toUtf8());
        return data;
    }
    QStringList mimeTypes() const override
    {
        return {EffectsLibrary::MimeType, EffectsLibrary::EffectMimeType, EffectsLibrary::SoundMimeType, MediaPool::MimeType};
    }
};

// Small preview icon: A (blue) transitions into B (orange)
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
    case TransitionType::Luma: break; // listed per image with its own preview (Lumas::preview)
    default: { // push/whip: B pushes A out (both halves), a whip with speed lines
        const int dir = TransitionStyle{type}.direction();
        const bool horizontal = dir < 2, forward = dir == 0 || dir == 2;
        p.fillRect(r, kA);
        p.fillRect(horizontal ? (forward ? QRect(0, 0, cx, h) : QRect(cx, 0, w - cx, h))
                              : (forward ? QRect(0, 0, w, cy) : QRect(0, cy, w, h - cy)),
                   kB);
        p.setRenderHint(QPainter::Antialiasing);
        if (TransitionStyle{type}.isWhip()) {
            p.setPen(QPen(QColor(0xf0, 0xf0, 0xf0, 150), 1));
            for (int k = -1; k <= 1; ++k) {
                if (horizontal) p.drawLine(4, cy + 6 * k, w - 4, cy + 6 * k);
                else p.drawLine(cx + 6 * k, 4, cx + 6 * k, h - 4);
            }
        }
        // Double arrow: both pictures move
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0xf0, 0xf0, 0xf0));
        for (int off : {-5, 4}) {
            QPolygon arrow;
            if (horizontal) {
                const int x = cx + off, s = forward ? 1 : -1;
                arrow = QPolygon({{x - 3 * s, cy - 5}, {x + 3 * s, cy}, {x - 3 * s, cy + 5}});
            } else {
                const int y = cy + off, s = forward ? 1 : -1;
                arrow = QPolygon({{cx - 5, y - 3 * s}, {cx + 5, y - 3 * s}, {cx, y + 3 * s}});
            }
            p.drawPolygon(arrow);
        }
        break;
    }
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
    out.cubicTo(r.center().x(), r.top(), r.center().x(), r.bottom(), r.right(), r.bottom()); // getting quieter
    in.cubicTo(r.center().x(), r.bottom(), r.center().x(), r.top(), r.right(), r.top());      // getting louder
    p.drawPath(out);
    p.drawPath(in);
    return pm;
}

QPixmap titleIcon(TitleKind kind = TitleKind::Text)
{
    QPixmap pm(kIcon);
    pm.fill(Theme::titleClip.darker(135));
    QPainter p(&pm);
    if (kind == TitleKind::Progress) {
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        const QRectF bar(4, 5, pm.width() - 8, 5);
        p.setBrush(QColor(0, 0, 0, 110));
        p.drawRoundedRect(bar, 2.5, 2.5);
        p.setBrush(QColor(255, 214, 0));
        p.drawRoundedRect(QRectF(bar.x(), bar.y(), bar.width() * 0.6, bar.height()), 2.5, 2.5);
        return pm;
    }
    QFont f = p.font();
    f.setPixelSize(kind == TitleKind::Counter ? 13 : 17);
    f.setBold(true);
    p.setFont(f);
    p.setPen(QColor(0xf0, 0xf0, 0xf0));
    p.drawText(pm.rect(), Qt::AlignCenter, kind == TitleKind::Counter ? "123" : "T");
    return pm;
}

// Own picture effects (engine/VideoEffects): the effect itself applied to a small colorful picture
QPixmap ownLookIcon(const QString& id)
{
    QImage img(kIcon * 4, QImage::Format_RGBA8888);
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x) {
            const bool sun = QLineF(x, y, img.width() * 0.68, img.height() * 0.42).length() < img.height() * 0.22;
            img.setPixelColor(x, y, sun ? QColor(0xf8, 0xe8, 0xb0)
                                        : QColor::fromHsvF(0.55 + 0.25 * x / img.width(), 0.55, 0.35 + 0.5 * y / img.height()));
        }
    uint8_t* px = img.bits();
    const int w = img.width(), h = img.height();
    if (id == "frei0r.glow") VideoEffects::glow(px, w, h, 0.3);
    else if (id == "frei0r.colorhalftone") VideoEffects::halftone(px, w, h, 0.6, 0.3, 0.45, 0.25);
    else if (id == "frei0r.rgbnoise") VideoEffects::noise(px, w, h, 0.35, 1);
    else if (id == "frei0r.scanline0r") {
        for (int y = 0; y < h; y += 4) // lines of the icon size, not of the picture
            for (int k = 2; k < 4 && y + k < h; ++k)
                for (int x = 0; x < w; ++x) img.setPixelColor(x, y + k, img.pixelColor(x, y + k).darker(200));
    } else if (id == "frei0r.nosync0r") VideoEffects::noSync(px, w, h, 0.3);
    else if (id == "frei0r.cairogradient") {
        VideoEffects::GradientParams g;
        g.startColor = QColor(0xff, 0x60, 0x20);
        g.endColor = QColor(0x20, 0x40, 0xff);
        g.startOpacity = g.endOpacity = 0.7;
        g.startX = 0, g.endX = 1, g.startY = g.endY = 0.5;
        VideoEffects::gradient(px, w, h, g);
    } else if (id == "frei0r.alpha0ps_alpha0ps") {
        for (int y = 0; y < h; ++y) // alpha shown as gray: the sun
            for (int x = 0; x < w; ++x) {
                const bool sun = QLineF(x, y, w * 0.68, h * 0.42).length() < h * 0.22;
                const int v = sun ? 235 : 25;
                img.setPixelColor(x, y, QColor(v, v, v));
            }
    }
    return QPixmap::fromImage(img.scaled(kIcon, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
}

// Filter icons: color correction = color gradient with brightness wedge, blur = soft circle
QPixmap effectIcon(const QString& id)
{
    if (VideoEffects::isOwn(id) && id != "chromakey") return ownLookIcon(id);
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
    } else if (id == "stabilize") { // shaky frame outlines around a calm one
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(0x80, 0x88, 0xa0), 1));
        p.drawRect(r.adjusted(9, 4, -15, -9));
        p.drawRect(r.adjusted(15, 8, -9, -5));
        p.setPen(QPen(QColor(0xe8, 0xe8, 0xf0), 2));
        p.drawRect(r.adjusted(12, 6, -12, -7));
    } else if (id == "denoise") { // noisy wave that becomes clean
        p.fillRect(r, Theme::audioClip.darker(160));
        QPainterPath wave;
        const double mid = r.center().y();
        for (int x = 4; x <= int(r.width()) - 4; ++x) {
            const double t = (x - 4) / (r.width() - 8);
            const double noise = t < 0.5 ? ((x * 7919) % 7 - 3) * 0.9 * (1 - 2 * t) : 0.0;
            const double y = mid - std::sin(t * 4 * std::numbers::pi) * 6 + noise;
            if (x == 4) wave.moveTo(x, y);
            else wave.lineTo(x, y);
        }
        p.setPen(QPen(QColor(0xe0, 0xf0, 0xe0), 1.5));
        p.drawPath(wave);
    } else if (id == "voiceclean") { // two clean word bursts with silent pauses between them
        p.fillRect(r, Theme::audioClip.darker(160));
        QPainterPath wave;
        const double mid = r.center().y();
        for (int x = 4; x <= int(r.width()) - 4; ++x) {
            const double t = (x - 4) / (r.width() - 8);
            const double word = std::max(0.0, std::sin(t * 2 * std::numbers::pi)) * (t < 0.5 ? 1.0 : 0.0)
                                + std::max(0.0, std::sin((t - 0.55) / 0.4 * std::numbers::pi)) * (t > 0.55 && t < 0.95);
            const double y = mid - std::sin(t * 18 * std::numbers::pi) * 8 * word;
            if (x == 4) wave.moveTo(x, y);
            else wave.lineTo(x, y);
        }
        p.setPen(QPen(QColor(0xe0, 0xf0, 0xe0), 1.5));
        p.drawPath(wave);
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

// Preset: tile with the program's initial and the number of effects it brings
QPixmap presetIcon(const QString& source, int effects)
{
    QPixmap pm(kIcon);
    QPainter p(&pm);
    const QRectF r = pm.rect();
    QLinearGradient g(r.topLeft(), r.bottomRight());
    g.setColorAt(0, QColor(0x4a, 0x3c, 0x5a));
    g.setColorAt(1, QColor(0x26, 0x22, 0x2e));
    p.fillRect(r, g);
    QFont f = p.font();
    f.setPixelSize(10);
    f.setBold(true);
    p.setFont(f);
    p.setPen(QColor(0xd0, 0xc0, 0xe0));
    const QString tag = source.isEmpty() ? QStringLiteral("?") : source.left(1);
    p.drawText(r, Qt::AlignCenter, effects > 1 ? QString("%1 ×%2").arg(tag).arg(effects) : tag);
    p.setPen(QColor(0, 0, 0, 120));
    p.drawRect(r.adjusted(0, 0, -1, -1));
    return pm;
}

// Template for LUT icons: hue from left to right, bright at the top, dark at the bottom, a gray wedge at the bottom
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
    // Import effects: opens the effects folder (like "Show in file manager"), files there appear on their own
    auto* import = new QToolButton;
    import->setText(T("Effekte importieren…"));
    import->setToolTip(T("Öffnet den Effekte-Ordner. LUTs (.cube, .3dl, .csp, Hald-CLUT) in den Ordner „LUTs“, "
                         "Übergänge (Graustufenbilder) in den Ordner „Transitions“ legen, Unterordner werden zu "
                         "Kategorien. Presets aus Kdenlive/Shotcut in den Ordner „Presets“.\n%1")
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

    // Categories like in DaVinci: Toolbox with sub-items; everything collapsible
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
    // Open FX → Filters (like DaVinci: the ResolveFX filters live there)
    auto* openFx = add(m_categories, QStringLiteral("Open FX"), OpenFx);
    add(openFx, T("Filter"), Filters);
    add(m_categories, QStringLiteral("Audio FX"), AudioFx)->setToolTip(0, T("Effekte für Audioclips"));
    auto* sounds = add(m_categories, T("Klänge"), SoundsAll);
    sounds->setToolTip(0, T("Effekt-Klänge: Doppelklick = anhören, in die Timeline ziehen = an dieser Stelle einfügen"));
    add(sounds, T("Mitgeliefert"), BuiltinSounds);
    add(sounds, T("Pakete"), PackSounds);
    m_soundRoot = add(sounds, T("Eigene"), Sounds);
    m_soundRoot->setToolTip(0, QDir::toNativeSeparators(EffectFolders::soundDir()));
    // Sorted by kind, below that bundled and custom effects folder each (subfolder = subcategories)
    auto* luts = add(m_categories, QStringLiteral("LUTs"), LutsAll);
    add(luts, T("Mitgeliefert"), BuiltinLuts);
    m_lutRoot = add(luts, T("Eigene"), Luts);
    m_lutRoot->setToolTip(0, QDir::toNativeSeparators(EffectFolders::lutDir()));
    auto* lumas = add(m_categories, T("Verlaufsblenden"), LumasAll);
    add(lumas, T("Mitgeliefert"), BuiltinTransitions);
    m_transRoot = add(lumas, T("Eigene"), Transitions);
    m_transRoot->setToolTip(0, QDir::toNativeSeparators(EffectFolders::transitionDir()));
    add(m_categories, T("Comp-Rezepte"), CompRecipes)
        ->setToolTip(0, T("Fertige Compositing-Graphen (Green Screen, Text hinter Person …) für einen Clip"));
    m_presetRoot = add(m_categories, QStringLiteral("Presets"), PresetsAll);
    m_presetRoot->setToolTip(0, T("Mitgelieferte Looks und Presets aus Kdenlive und Shotcut\n%1").arg(QDir::toNativeSeparators(EffectFolders::presetDir())));
    for (QTreeWidgetItem* it : {toolbox, openFx, luts, lumas}) it->setExpanded(true);
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
        if (d == kTitleData || d == kCounterData || d == kProgressData) {
            emit titleRequested(int(d == kCounterData ? TitleKind::Counter
                                    : d == kProgressData ? TitleKind::Progress : TitleKind::Text));
            return;
        }
        if (d.startsWith(kEffectPrefix)) {
            emit effectRequested(d.mid(3));
            return;
        }
        if (d.startsWith(kSoundPrefix)) { // listen only: placing is dragging it into the timeline
            if (const QString file = soundFile(d.mid(6).toUtf8()); !file.isEmpty()) emit soundPreviewRequested(file);
            return;
        }
        if (d.startsWith(EffectFolders::LutPrefix) || d.startsWith(Presets::Prefix)) { // LUT/preset -> like a filter
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

    // Watch the effects folder (folder + subfolders); read changes collected after a short pause
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
    m_userPresets = EffectFolders::userPresets();
    m_userSounds = EffectFolders::userSounds();

    // Subcategories = subfolders (nested like in the folder); selection stays if it still exists
    const QSignalBlocker block(m_categories);
    const QTreeWidgetItem* cur = m_categories->currentItem();
    const QString curKey = cur ? collapseKey(cur) : QString();
    const int curCat = cur ? cur->data(0, Qt::UserRole).toInt() : -1; // cur may get deleted soon
    for (auto [root, group, entries] : {std::tuple{m_lutRoot, int(LutGroup), &m_userLuts},
                                        std::tuple{m_transRoot, int(TransitionGroup), &m_userTransitions},
                                        std::tuple{m_presetRoot, int(PresetGroup), &m_userPresets},
                                        std::tuple{m_soundRoot, int(SoundGroup), &m_userSounds}}) {
        const bool wasInside = curCat == group;
        qDeleteAll(root->takeChildren());
        fillGroups(root, group, *entries);
        if (wasInside) m_categories->setCurrentItem(root); // more precisely below, if the folder still exists
    }
    // Restore collapsed state (default: everything open)
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
        if (lut) { // read without cache: large folders should not stay in memory permanently
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

    // Files from the effects folder resp. bundled (icons cost reading -> only for the selected category)
    auto files = [&](const QVector<EffectFolders::LutEntry>& list, bool lut, bool builtin) {
        QVector<Entry> out;
        for (const auto& l : list) {
            // Top-level category shows everything, a subcategory its folder including subfolders
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
            if (i.type != TransitionType::Luma) // Gradient wipes come individually (per image)
                out << Entry{T(i.name), QString("video:%1").arg(i.id), transitionIcon(i.type)};
        return out + files(EffectFolders::builtinTransitions(), false, true);
    };
    auto audioTransitions = [&] {
        QVector<Entry> out;
        for (const auto& i : kAudioCurves)
            out << Entry{T(i.name), QString("audio:%1").arg(i.id), audioIcon()};
        return out;
    };
    auto filters = [&](bool video) {
        QVector<Entry> out;
        for (const auto& e : EffectRegistry::all())
            if (e.library && e.video == video)
                out << Entry{e.name, kEffectPrefix + e.id, effectIcon(e.id), e.description};
        return out;
    };
    auto builtinPresets = [&] {
        QVector<Entry> out;
        for (const auto& l : EffectFolders::builtinPresets()) {
            const PresetEntry& p = presetEntry(l.path);
            out << Entry{p.name, Presets::Prefix + l.path, p.icon, p.tip, p.ok};
        }
        return out;
    };
    auto presets = [&] {
        QVector<Entry> out;
        for (const auto& l : m_userPresets) {
            if (!group.isEmpty() && l.group != group && !l.group.startsWith(group + '/')) continue;
            const PresetEntry& p = presetEntry(l.path);
            out << Entry{p.name, Presets::Prefix + l.path, p.icon, p.tip, p.ok};
        }
        return out;
    };
    // Effect sounds: the recordings and computed ones built in, the installed packs, the user's folder
    auto builtinSounds = [&] {
        QVector<Entry> out;
        for (SfxSynth::Kind k : SfxSynth::kKinds) {
            const QVector<SfxSynth::Recorded> rec = SfxSynth::recorded(k);
            for (qsizetype i = 0; i < rec.size(); ++i) {
                const SfxSynth::Recorded& r = rec[i];
                out << Entry{T("%1 %2 (Aufnahme)").arg(soundKindName(k)).arg(i + 1),
                             soundData({{"kind", SfxSynth::kindName(k)}, {"seed", int(i + 1)}}), soundIcon(false),
                             T("Aufnahme von %1 (%2), %3").arg(r.author, r.licence, secondsText(r.length))};
            }
            // The computed ones (a length = computed, also where recordings are the default)
            const double length = SfxSynth::defaultLength(k);
            for (int seed = 1; seed <= 3; ++seed)
                out << Entry{T("%1 %2 (berechnet)").arg(soundKindName(k)).arg(seed),
                             soundData({{"kind", SfxSynth::kindName(k)}, {"seed", seed},
                                        {"length", QString::number(length, 'f', 3) + "s"}}),
                             soundIcon(false), T("Berechnet (eigener Klang, keine Aufnahme), %1").arg(secondsText(length))};
        }
        return out;
    };
    auto soundFiles = [&](const QVector<EffectFolders::LutEntry>& list, bool own) {
        QVector<Entry> out;
        for (const auto& l : list) {
            if (own && !group.isEmpty() && l.group != group && !l.group.startsWith(group + '/')) continue;
            out << Entry{l.name, soundData({{"kind", l.path}}), soundIcon(own), QDir::toNativeSeparators(l.path)};
        }
        return out;
    };
    auto packSounds = [&] {
        QVector<EffectFolders::LutEntry> list;
        for (const QString& pack : Extensions::installedSoundPacks()) {
            QDirIterator it(pack, QDir::Files, QDirIterator::Subdirectories);
            QStringList found;
            while (it.hasNext())
                if (const QString f = it.next(); EffectFolders::isSoundFile(f)) found << f;
            found.sort();
            for (const QString& f : found) {
                EffectFolders::LutEntry e;
                e.name = QFileInfo(pack).fileName() + "/" + QFileInfo(f).completeBaseName();
                e.path = f;
                list << e;
            }
        }
        return soundFiles(list, false);
    };
    const QVector<Entry> titles{Entry{T("Text"), kTitleData, titleIcon()},
                                Entry{T("Zähler"), kCounterData, titleIcon(TitleKind::Counter),
                                      T("Zahl, die über die Länge des Clips hochzählt (z. B. 0 → 1000 Kills)")},
                                Entry{T("Fortschrittsbalken"), kProgressData, titleIcon(TitleKind::Progress),
                                      T("Balken am Bildrand, der zeigt, wie viel vom Video noch kommt (über die "
                                        "ganze Timeline)")}};

    switch (shown) {
    case All: // Toolbox: all groups with header like in DaVinci
        groups = {{T("Videoübergänge"), videoTransitions()}, {T("Audioübergänge"), audioTransitions()},
                  {T("Titel-Vorlagen"), titles}};
        break;
    case VideoTransitions: groups = {{{}, videoTransitions()}}; break;
    case AudioTransitions: groups = {{{}, audioTransitions()}}; break;
    case Titles: groups = {{{}, titles}}; break;
    case OpenFx: groups = {{T("Filter"), filters(true)}}; break;
    case Filters: groups = {{{}, filters(true)}}; break;
    case AudioFx: groups = {{{}, filters(false)}}; break;
    case LutsAll:
        groups = {{T("Mitgeliefert"), files(EffectFolders::builtinLuts(), true, true)},
                  {T("Eigene"), files(m_userLuts, true, false)}};
        break;
    case LumasAll:
        groups = {{T("Mitgeliefert"), files(EffectFolders::builtinTransitions(), false, true)},
                  {T("Eigene"), files(m_userTransitions, false, false)}};
        break;
    case BuiltinLuts: groups = {{{}, files(EffectFolders::builtinLuts(), true, true)}}; break;
    case BuiltinTransitions: groups = {{{}, files(EffectFolders::builtinTransitions(), false, true)}}; break;
    case Luts:
    case LutGroup: groups = {{{}, files(m_userLuts, true, false)}}; break;
    case Transitions:
    case TransitionGroup: groups = {{{}, files(m_userTransitions, false, false)}}; break;
    case PresetsAll:
        groups = {{T("Mitgeliefert"), builtinPresets()}, {T("Eigene"), presets()}};
        break;
    case PresetGroup: groups = {{{}, presets()}}; break;
    case SoundsAll:
        groups = {{T("Mitgeliefert"), builtinSounds()}, {T("Pakete"), packSounds()},
                  {T("Eigene"), soundFiles(m_userSounds, true)}};
        break;
    case BuiltinSounds: groups = {{{}, builtinSounds()}}; break;
    case PackSounds: groups = {{{}, packSounds()}}; break;
    case Sounds:
    case SoundGroup: groups = {{{}, soundFiles(m_userSounds, true)}}; break;
    case CompRecipes:
        // TODO(0.19 package I): the built-in recipes (assets/comp/*.json) as entries; data "recipe:<id>", dragging one
        // onto a clip / double-click = op `comp recipe` on it (one undo step)
        break;
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
            it->setData(Qt::UserRole, e->ok ? e->data : QString()); // broken file: visible, but not applicable
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
            hint = T("Noch keine Verlaufsblenden – „Effekte importieren…“ öffnet den Ordner");
        if (shown == PresetGroup)
            hint = T("Noch keine Presets – „Effekte importieren…“ öffnet den Ordner");
        if (shown == Sounds || shown == SoundGroup)
            hint = T("Noch keine eigenen Klänge – „Effekte importieren…“ öffnet den Ordner (Unterordner „Sounds“)");
        if (shown == PackSounds) hint = T("Noch keine Klangpakete installiert");
        if (shown == CompRecipes) hint = T("Noch keine Comp-Rezepte");
        if (!hint.isEmpty()) {
            auto* h = new QListWidgetItem(hint);
            h->setFlags(Qt::NoItemFlags);
            h->setForeground(Theme::textDim);
            m_list->addItem(h);
        }
    }
}

const EffectsLibrary::PresetEntry& EffectsLibrary::presetEntry(const QString& path)
{
    const QFileInfo fi(path);
    const qint64 stamp = fi.lastModified().toMSecsSinceEpoch() ^ (fi.size() << 20);
    auto it = m_presets.find(path);
    if (it != m_presets.end() && it->stamp == stamp) return *it;
    PresetEntry e;
    e.stamp = stamp;
    const Presets::Preset p = Presets::load(path);
    const Presets::Mapped m = Presets::map(p);
    e.name = p.name;
    QStringList tip;
    if (!p.source.isEmpty()) tip << T("Preset aus %1").arg(p.source);
    if (!p.description.isEmpty()) tip << p.description;
    QStringList names;
    for (const EffectInstance& x : m.effects)
        if (const EffectDescriptor* d = EffectRegistry::find(x.effectId)) names << d->name;
    if (!m.lut.isEmpty()) names << T("LUT") + " " + QFileInfo(m.lut).fileName();
    if (!names.isEmpty()) tip << T("Effekte: %1").arg(names.join(", "));
    if (!m.skipped.isEmpty()) tip << T("Nicht unterstützt (ausgelassen): %1").arg(m.skipped.join(", "));
    if (!m.keys.isEmpty()) tip << T("Mit Keyframes");
    if (m.keyframes) tip << T("Keyframes bei Schaltern/Farben/Auswahl: nur der erste Wert wird übernommen");
    if (!p.error.isEmpty()) tip << T("Lässt sich nicht lesen: %1").arg(p.error);
    else if (m.empty()) tip << T("Enthält keine Effekte, die schneidi kennt");
    tip << QDir::toNativeSeparators(path);
    e.tip = tip.join('\n');
    e.ok = p.error.isEmpty() && !m.empty();
    e.icon = presetIcon(p.source, int(m.effects.size()) + (m.lut.isEmpty() ? 0 : 1));
    return *m_presets.insert(path, e);
}

QString EffectsLibrary::soundFile(const QByteArray& data)
{
    const QJsonObject o = QJsonDocument::fromJson(data).object();
    const QString kind = o["kind"].toString();
    const std::optional<SfxSynth::Kind> k = SfxSynth::kindFrom(kind);
    if (!k) return QFileInfo(kind).isFile() ? kind : QString(); // a pack's or the user's file
    double length = 0;
    if (const QString l = o["length"].toString(); l.endsWith('s')) length = l.chopped(1).toDouble();
    return SfxSynth::cachedFile(*k, unsigned(std::max(1, o["seed"].toInt(1))), length);
}
