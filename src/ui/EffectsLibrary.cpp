// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/EffectsLibrary.h"
#include "ui/SplitterMemory.h"

#include "app/Theme.h"
#include "core/CompRecipes.h"
#include "core/EffectFavorites.h"
#include "core/EffectRegistry.h"
#include "core/I18n.h"
#include "core/Presets.h"
#include "core/SfxSynth.h"
#include "core/Shapes.h"
#include "engine/Extensions.h"
#include "engine/ColorGrade.h"
#include "engine/Lumas.h"
#include "engine/VideoEffects.h"
#include "ui/CompactButton.h"
#include "ui/ElidedLabel.h"
#include "ui/MediaPool.h"
#include "ui/comp/CompShared.h"

#include <QCollator>
#include <QDateTime>
#include <QDrag>
#include <QKeyEvent>
#include <QScrollBar>
#include <QDesktopServices>
#include <QFileDialog>
#include <QMenu>
#include <QMessageBox>
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
    RecipesCat,                                                   // compositing recipes (0.19, op `comp recipe`)
    TransitionsAll,                                               // 0.22: video + audio transitions + gradient wipes
    VideoFxGroup, AudioFxGroup,                                   // an EffectCategory (kGroupRole = its id)
    Favorites, Recent,                                            // the user's favourites, recently used
    ShapesCat,                                                    // shape titles by kind + the paint tool
    // Luts/Transitions = own effects folder, …Group = its subfolder
    // (numbers are saved with the collapsed state: append new categories at the end; All (the old Toolbox) and Filters
    // are no longer shown, their saved state is just ignored)
};
// Shown collapsed until the user opens them (the long file lists)
bool collapsedByDefault(int cat) { return cat == LumasAll || cat == LutsAll || cat == SoundsAll || cat == PresetsAll; }
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
constexpr const char* kShapePrefix = "shape:"; // List entry of a shape kind: "shape:<kind id>"
constexpr const char* kDrawData = "draw";       // the paint tool
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
    std::function<void(const QString&)> dropped; // a drag of this entry data was taken by the target

protected:
    QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override
    {
        auto* data = new QMimeData;
        if (items.isEmpty()) return data;
        const QString d = items.first()->data(Qt::UserRole).toString();
        if (d == kTitleData) data->setData(MediaPool::MimeType, QByteArray(MediaPool::TitleItem)); // like "Text" in the pool
        else if (d == kCounterData) data->setData(MediaPool::MimeType, QByteArray(MediaPool::CounterItem));
        else if (d == kProgressData) data->setData(MediaPool::MimeType, QByteArray(MediaPool::ProgressItem));
        else if (d.startsWith(kShapePrefix))
            data->setData(MediaPool::MimeType, MediaPool::shapeItem(d.mid(int(qstrlen(kShapePrefix)))).toUtf8());
        else if (d.startsWith(kEffectPrefix)) data->setData(EffectsLibrary::EffectMimeType, d.mid(3).toUtf8());
        else if (d.startsWith(kSoundPrefix)) data->setData(EffectsLibrary::SoundMimeType, d.mid(6).toUtf8());
        else if (d.startsWith(EffectFolders::LutPrefix) || d.startsWith(Presets::Prefix) || CompUi::isRecipe(d))
            data->setData(EffectsLibrary::EffectMimeType, d.toUtf8());
        else data->setData(EffectsLibrary::MimeType, d.toUtf8());
        return data;
    }
    QStringList mimeTypes() const override
    {
        return {EffectsLibrary::MimeType, EffectsLibrary::EffectMimeType, EffectsLibrary::SoundMimeType, MediaPool::MimeType};
    }
    // Own drag (instead of QAbstractItemView's) to learn whether the drop was taken: that counts as used
    void startDrag(Qt::DropActions supported) override
    {
        QListWidgetItem* it = currentItem();
        if (!it || !(it->flags() & Qt::ItemIsDragEnabled)) return;
        const QString d = it->data(Qt::UserRole).toString();
        auto* drag = new QDrag(this);
        drag->setMimeData(mimeData({it}));
        drag->setPixmap(it->icon().pixmap(iconSize()));
        if (drag->exec(supported, Qt::CopyAction) != Qt::IgnoreAction && dropped) dropped(d);
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
    if (kind == TitleKind::Shape) {
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(QColor(0xf0, 0xf0, 0xf0), 2));
        p.setBrush(QColor(255, 214, 0));
        p.drawEllipse(QRectF(pm.width() * 0.2, pm.height() * 0.2, pm.width() * 0.6, pm.height() * 0.6));
        return pm;
    }
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

// A shape kind: its outline (core/Shapes) filled yellow with a light stroke, a line as a yellow stroke
QPixmap shapeIcon(ShapeKind kind)
{
    QPixmap pm(kIcon);
    pm.fill(Theme::titleClip.darker(135));
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    ShapeStyle s;
    s.kind = kind;
    const double h = pm.height() - 8.0;
    double w = h;
    if (kind == ShapeKind::Rect) w = h * 1.5;
    if (kind == ShapeKind::Arrow || kind == ShapeKind::Line) w = pm.width() - 12.0;
    const Shapes::Outline o = Shapes::outline(s, w, kind == ShapeKind::Line ? 1.0 : (kind == ShapeKind::Arrow ? h * 0.7 : h));
    QPolygonF poly(o.points);
    poly.translate(pm.width() / 2.0, pm.height() / 2.0);
    const QColor yellow(255, 214, 0);
    if (o.closed && kind != ShapeKind::Line) {
        p.setPen(QPen(QColor(0xf0, 0xf0, 0xf0), 1.2));
        p.setBrush(yellow);
        p.drawPolygon(poly);
    } else {
        p.setPen(QPen(yellow, 2.5, Qt::SolidLine, Qt::RoundCap));
        p.drawPolyline(poly);
    }
    return pm;
}

// The paint tool: a free brush stroke
QPixmap drawIcon()
{
    QPixmap pm(kIcon);
    pm.fill(Theme::titleClip.darker(135));
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath stroke(QPointF(6, pm.height() - 7));
    stroke.cubicTo(14, 2, 22, pm.height() - 2, 30, 10);
    stroke.cubicTo(34, 4, 40, 8, pm.width() - 5, 6);
    p.setPen(QPen(QColor(255, 214, 0), 2.5, Qt::SolidLine, Qt::RoundCap));
    p.drawPath(stroke);
    return pm;
}

// A favourite: the icon with a small star in its top right corner
QPixmap starred(const QPixmap& icon)
{
    QPixmap pm = icon.isNull() ? QPixmap(kIcon) : icon.copy();
    if (icon.isNull()) pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const double r = 5.5, cx = pm.width() / pm.devicePixelRatio() - r - 1, cy = r + 1;
    QPolygonF star;
    for (int i = 0; i < 10; ++i) {
        const double a = -std::numbers::pi / 2 + i * std::numbers::pi / 5, rr = i % 2 ? r * 0.45 : r;
        star << QPointF(cx + rr * std::cos(a), cy + rr * std::sin(a));
    }
    p.setPen(QPen(QColor(0, 0, 0, 170), 1));
    p.setBrush(QColor(255, 200, 40));
    p.drawPolygon(star);
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
    if (id == QLatin1String(Shapes::kClipShapeEffect)) { // the colourful picture cut to a circle with a white rim
        QPixmap pm(kIcon);
        pm.fill(QColor(0x2a, 0x2a, 0x30));
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        const double d = pm.height() * 0.8;
        const QRectF circle(pm.width() / 2.0 - d / 2, pm.height() / 2.0 - d / 2, d, d);
        QPainterPath clip;
        clip.addEllipse(circle);
        p.setClipPath(clip);
        p.drawPixmap(pm.rect(), ownLookIcon(QString()));
        p.setClipping(false);
        p.setPen(QPen(QColor(0xf0, 0xf0, 0xf0), 1.5));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(circle);
        return pm;
    }
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

// Recipe: three small nodes wired into one (built-in teal, own orange like the sounds)
QPixmap recipeIcon(bool own)
{
    QPixmap pm(kIcon);
    pm.fill(QColor(0x2a, 0x2a, 0x30));
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const QColor c = own ? QColor(0xc8, 0x8a, 0x3c) : QColor(0x3b, 0x9a, 0x8a);
    const QRectF a(5, 4, 13, 7), b(5, 16, 13, 7), out(30, 10, 13, 7);
    p.setPen(QPen(QColor(0xd0, 0xd0, 0xd8), 1.2));
    for (const QRectF& from : {a, b}) {
        QPainterPath w(QPointF(from.right(), from.center().y()));
        w.cubicTo(from.right() + 7, from.center().y(), out.left() - 7, out.center().y(), out.left(), out.center().y());
        p.drawPath(w);
    }
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    for (const QRectF& r : {a, b, out}) p.drawRoundedRect(r, 2, 2);
    p.setPen(QColor(0, 0, 0, 120));
    p.setBrush(Qt::NoBrush);
    p.drawRect(QRectF(pm.rect()).adjusted(0, 0, -1, -1));
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

    // A narrow column: the title ends in "…", the import button shows "+" (its tooltip says what it does)
    auto* title = new ElidedLabel;
    title->setFullText(T("Effekte"));
    title->setObjectName("PanelTitle");
    m_search = new QLineEdit;
    m_search->setPlaceholderText(T("Suchen"));
    m_search->setClearButtonEnabled(true);
    m_search->setMaximumWidth(180);
    // Import effects: opens the effects folder (like "Show in file manager"), files there appear on their own
    auto* import = new CompactButton;
    import->setText(T("Effekte importieren…"));
    import->setCompactText("+");
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

    // Categories by what they are for: favourites and recently used on top, then transitions, titles, shapes, the
    // effects by purpose, LUTs, sounds, recipes, presets; everything collapsible
    m_categories = new QTreeWidget;
    m_categories->setHeaderHidden(true);
    m_categories->setIndentation(12);
    auto add = [](auto* parent, const QString& text, int cat, const QString& group = {}) {
        auto* it = new QTreeWidgetItem(parent, {text});
        it->setData(0, Qt::UserRole, cat);
        if (!group.isEmpty()) it->setData(0, kGroupRole, group);
        return it;
    };
    add(m_categories, T("Favoriten"), Favorites)
        ->setToolTip(0, T("Deine markierten Einträge: Rechtsklick auf einen Eintrag › Zu Favoriten hinzufügen (oder Taste F)"));
    add(m_categories, T("Zuletzt benutzt"), Recent)->setToolTip(0, T("Die zuletzt angewendeten Einträge, der neueste oben"));
    auto* transitions = add(m_categories, T("Übergänge"), TransitionsAll);
    add(transitions, T("Video"), VideoTransitions);
    add(transitions, T("Audio"), AudioTransitions);
    // Gradient wipes: bundled and the effects folder's (subfolders = subcategories)
    auto* lumas = add(transitions, T("Verlaufsblenden"), LumasAll);
    add(lumas, T("Mitgeliefert"), BuiltinTransitions);
    m_transRoot = add(lumas, T("Eigene"), Transitions);
    m_transRoot->setToolTip(0, QDir::toNativeSeparators(EffectFolders::transitionDir()));
    add(m_categories, T("Titel"), Titles);
    add(m_categories, T("Formen & Zeichnen"), ShapesCat)
        ->setToolTip(0, T("Kreis, Pfeil, Stern … als eigener Clip über dem Bild, und Malen direkt im Bild"));
    // Effects by purpose (EffectCategory, defined next to each effect in the registry)
    for (const bool video : {true, false}) {
        auto* root = video ? add(m_categories, T("Videoeffekte"), OpenFx) : add(m_categories, T("Audioeffekte"), AudioFx);
        if (!video) root->setToolTip(0, T("Effekte für Audioclips"));
        for (const EffectCategoryInfo& c : kEffectCategories)
            if (std::any_of(EffectRegistry::all().begin(), EffectRegistry::all().end(), [&](const EffectDescriptor& d) {
                    return d.library && d.video == video && d.category == c.category && d.category != EffectCategory::Custom;
                }))
                add(root, T(c.name), video ? VideoFxGroup : AudioFxGroup, QString::fromLatin1(c.id));
    }
    auto* luts = add(m_categories, QStringLiteral("LUTs"), LutsAll);
    luts->setToolTip(0, T("Farb-Looks als LUT-Datei (.cube …)"));
    add(luts, T("Mitgeliefert"), BuiltinLuts);
    m_lutRoot = add(luts, T("Eigene"), Luts);
    m_lutRoot->setToolTip(0, QDir::toNativeSeparators(EffectFolders::lutDir()));
    auto* sounds = add(m_categories, T("Klänge"), SoundsAll);
    sounds->setToolTip(0, T("Effekt-Klänge: Doppelklick = anhören, in die Timeline ziehen = an dieser Stelle einfügen"));
    add(sounds, T("Mitgeliefert"), BuiltinSounds);
    add(sounds, T("Pakete"), PackSounds);
    m_soundRoot = add(sounds, T("Eigene"), Sounds);
    m_soundRoot->setToolTip(0, QDir::toNativeSeparators(EffectFolders::soundDir()));
    add(m_categories, T("Compositing-Rezepte"), RecipesCat)
        ->setToolTip(0, T("Fertige Compositings (Greenscreen, Text hinter Person …) für einen Clip"));
    m_presetRoot = add(m_categories, QStringLiteral("Presets"), PresetsAll);
    m_presetRoot->setToolTip(0, T("Mitgelieferte Looks und Presets aus Kdenlive und Shotcut\n%1").arg(QDir::toNativeSeparators(EffectFolders::presetDir())));
    for (QTreeWidgetItemIterator it(m_categories); *it; ++it)
        (*it)->setExpanded(!collapsedByDefault((*it)->data(0, Qt::UserRole).toInt()));
    m_categories->setCurrentItem(transitions);
    connect(m_categories, &QTreeWidget::itemExpanded, this, &EffectsLibrary::saveCollapsed);
    connect(m_categories, &QTreeWidget::itemCollapsed, this, &EffectsLibrary::saveCollapsed);

    auto* list = new EffectList;
    list->dropped = [this](const QString& d) { noteUsed(d); };
    m_list = list;
    m_list->setViewMode(QListView::ListMode);
    m_list->setIconSize(kIcon);
    m_list->setSpacing(1);
    m_list->setDragEnabled(true);
    m_list->setDragDropMode(QAbstractItemView::DragOnly);
    m_list->setToolTip(T("Auf einen Schnitt, einen Clip bzw. in die Timeline ziehen, Doppelklick = am Abspielkopf bzw. "
                         "auf die Auswahl anwenden"));
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* it) {
        const QString d = it->data(Qt::UserRole).toString();
        if (d.isEmpty()) return;
        if (d == kDrawData) {
            emit drawRequested();
            return;
        }
        if (!d.startsWith(kSoundPrefix)) noteUsed(d); // a sound is only heard here
        if (d == kTitleData || d == kCounterData || d == kProgressData) {
            emit titleRequested(int(d == kCounterData    ? TitleKind::Counter
                                    : d == kProgressData ? TitleKind::Progress
                                                         : TitleKind::Text));
            return;
        }
        if (d.startsWith(kShapePrefix)) {
            emit shapeRequested(d.mid(int(qstrlen(kShapePrefix))));
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
        if (d.startsWith(EffectFolders::LutPrefix) || d.startsWith(Presets::Prefix) || CompUi::isRecipe(d)) {
            // LUT/preset/recipe -> like a filter
            emit effectRequested(d);
            return;
        }
        TrackKind kind;
        TransitionStyle style;
        if (parseTransition(d.toUtf8(), &kind, &style)) emit transitionRequested(kind, style);
    });

    // Context menu: favourite on/off for every entry with a stable key; recipes: share as ZIP, take one in, remove an
    // own one
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_list, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        const QTreeWidgetItem* cur = m_categories->currentItem();
        const bool recipes = cur && cur->data(0, Qt::UserRole).toInt() == RecipesCat;
        const QListWidgetItem* it = m_list->itemAt(pos);
        const QString d = it ? it->data(Qt::UserRole).toString() : QString();
        const QString id = CompUi::isRecipe(d) ? d.mid(int(qstrlen(CompUi::kRecipePrefix))) : QString();
        const std::optional<CompRecipe> r = id.isEmpty() ? std::nullopt : CompRecipes::find(id);
        QMenu menu(this);
        if (const QString key = EffectFavorites::key(d); !key.isEmpty()) {
            menu.addAction(EffectFavorites::isFavorite(key) ? T("Aus Favoriten entfernen") : T("Zu Favoriten hinzufügen"),
                           this, [this, d] { toggleFavorite(d); });
        }
        if (r) {
            if (!menu.isEmpty()) menu.addSeparator();
            menu.addAction(T("Als ZIP exportieren…"), this, [this, r] {
                const QString file = QFileDialog::getSaveFileName(this, T("Rezept exportieren"), r->id + ".zip",
                                                                  T("ZIP-Archiv (*.zip)"));
                if (file.isEmpty()) return;
                QString err;
                if (!exportRecipe(r->id, file, &err)) QMessageBox::warning(this, T("Rezept exportieren"), err);
            });
        }
        if (recipes) {
            if (!menu.isEmpty() && !r) menu.addSeparator();
            menu.addAction(T("Rezept importieren (ZIP)…"), this, [this] {
                const QString file = QFileDialog::getOpenFileName(this, T("Rezept importieren"), {}, T("ZIP-Archiv (*.zip)"));
                if (file.isEmpty()) return;
                QString err;
                if (importRecipe(file, &err).isEmpty()) QMessageBox::warning(this, T("Rezept importieren"), err);
            });
        }
        if (r && !r->builtIn) {
            menu.addSeparator();
            menu.addAction(T("Rezept löschen…"), this, [this, r] {
                if (QMessageBox::question(this, T("Rezept löschen"),
                                          T("Das eigene Rezept „%1“ samt seinen Dateien löschen? Das lässt sich nicht "
                                            "rückgängig machen.").arg(CompRecipes::shownName(*r)))
                    != QMessageBox::Yes)
                    return;
                QString err;
                if (!CompRecipes::remove(r->id, &err)) QMessageBox::warning(this, T("Rezept löschen"), err);
                rebuild();
            });
        }
        if (!menu.isEmpty()) menu.exec(m_list->viewport()->mapToGlobal(pos));
    });
    // Key F on an entry: favourite on/off (the list takes the key before a window shortcut would)
    m_list->installEventFilter(this);

    auto* split = new QSplitter(Qt::Horizontal);
    split->addWidget(m_categories);
    split->addWidget(m_list);
    split->setStretchFactor(1, 1);
    SplitterMemory::remember(split, "effectsCategories", {130, 230}); // the user's drag comes back on the next start

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

void EffectsLibrary::refreshRecipes()
{
    const QTreeWidgetItem* cur = m_categories->currentItem();
    if (cur && cur->data(0, Qt::UserRole).toInt() == RecipesCat) rebuild();
}

void EffectsLibrary::showRecipes()
{
    for (QTreeWidgetItemIterator it(m_categories); *it; ++it)
        if ((*it)->data(0, Qt::UserRole).toInt() == RecipesCat) m_categories->setCurrentItem(*it);
}

bool EffectsLibrary::exportRecipe(const QString& id, const QString& zipPath, QString* err)
{
    QString why, code;
    if (CompRecipes::exportZip(id, zipPath, true, &why, &code)) return true;
    if (err) *err = T("Das Rezept lässt sich nicht exportieren: %1").arg(why);
    return false;
}

QString EffectsLibrary::importRecipe(const QString& zipPath, QString* err)
{
    QString why, code;
    const std::optional<CompRecipe> r = CompRecipes::importZip(zipPath, false, &why, &code);
    if (!r) {
        if (err) *err = T("Das Rezept lässt sich nicht importieren: %1").arg(why);
        return {};
    }
    showRecipes();
    rebuild();
    return r->id;
}

// Collapsed and expanded categories by number + subfolder (collapseKey). Older versions saved only the collapsed
// ones (everything else was open): a category missing from both lists takes its default
void EffectsLibrary::saveCollapsed()
{
    QStringList collapsed, expanded;
    for (QTreeWidgetItemIterator it(m_categories); *it; ++it)
        if ((*it)->childCount()) ((*it)->isExpanded() ? expanded : collapsed) << collapseKey(*it);
    QSettings settings;
    settings.setValue("effects/collapsed", collapsed);
    settings.setValue("effects/expanded", expanded);
}

void EffectsLibrary::setDrawShortcut(const QKeySequence& key)
{
    m_drawKey = key.toString(QKeySequence::NativeText);
    const QTreeWidgetItem* cur = m_categories->currentItem();
    if (cur && cur->data(0, Qt::UserRole).toInt() == ShapesCat) rebuildKeepingPlace();
}

void EffectsLibrary::toggleFavorite(const QString& data)
{
    const QString key = EffectFavorites::key(data);
    if (key.isEmpty()) return;
    EffectFavorites::toggleFavorite(key);
    rebuildKeepingPlace(); // the star, and the Favourites category itself
}

void EffectsLibrary::noteUsed(const QString& data)
{
    EffectFavorites::noteUsed(EffectFavorites::key(data));
    const QTreeWidgetItem* cur = m_categories->currentItem();
    if (cur && cur->data(0, Qt::UserRole).toInt() == Recent) rebuildKeepingPlace();
}

void EffectsLibrary::rebuildKeepingPlace()
{
    const QString current = m_list->currentItem() ? m_list->currentItem()->data(Qt::UserRole).toString() : QString();
    const int scroll = m_list->verticalScrollBar()->value();
    rebuild();
    for (int i = 0; !current.isEmpty() && i < m_list->count(); ++i)
        if (m_list->item(i)->data(Qt::UserRole).toString() == current) {
            m_list->setCurrentRow(i);
            break;
        }
    m_list->verticalScrollBar()->setValue(scroll);
}

bool EffectsLibrary::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_list && (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress)) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_F && !(key->modifiers() & ~Qt::KeypadModifier) && m_list->currentItem()
            && !EffectFavorites::key(m_list->currentItem()->data(Qt::UserRole).toString()).isEmpty()) {
            event->accept();
            if (event->type() == QEvent::KeyPress) toggleFavorite(m_list->currentItem()->data(Qt::UserRole).toString());
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
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
    // Restore collapsed state (default: open, the long file lists closed)
    const QSettings settings;
    const QStringList collapsed = settings.value("effects/collapsed").toStringList();
    const QStringList expanded = settings.value("effects/expanded").toStringList();
    for (QTreeWidgetItemIterator it(m_categories); *it; ++it) {
        if ((*it)->childCount()) {
            const QString key = collapseKey(*it);
            (*it)->setExpanded(collapsed.contains(key)  ? false
                               : expanded.contains(key) ? true
                                                        : !collapsedByDefault((*it)->data(0, Qt::UserRole).toInt()));
        }
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

const QPixmap& EffectsLibrary::cachedIcon(const QString& key, const std::function<QPixmap()>& make)
{
    auto it = m_icons.find(key);
    if (it == m_icons.end()) it = m_icons.insert(key, make());
    return *it;
}

void EffectsLibrary::rebuild()
{
    m_list->clear();
    const QTreeWidgetItem* cur = m_categories->currentItem();
    const int shown = cur ? cur->data(0, Qt::UserRole).toInt() : int(TransitionsAll);
    const QString group = cur ? cur->data(0, kGroupRole).toString() : QString();
    const QString filter = m_search->text().trimmed();

    // load: what costs reading a file (a LUT's picture, whether it can be read) runs only for the entries shown
    struct Entry {
        QString name;
        QString data;
        QPixmap icon;
        QString tip;
        bool ok = true;
        QStringList keywords;
        std::function<void(Entry&)> load = {};
        bool drag = true; // false: double-click only (the paint tool)
    };
    struct Group { QString header; QVector<Entry> entries; };
    QVector<Group> groups;

    // Files from the effects folder resp. bundled; inFolder: only the selected subfolder (and its subfolders)
    auto files = [&](const QVector<EffectFolders::LutEntry>& list, bool lut, bool builtin, bool inFolder) {
        QVector<Entry> out;
        for (const auto& l : list) {
            if (inFolder && !builtin && !group.isEmpty() && l.group != group && !l.group.startsWith(group + '/')) continue;
            const QString tip = builtin ? (lut ? T("LUT (mitgeliefert)") : T("Übergang (mitgeliefert)"))
                                        : QDir::toNativeSeparators(l.path);
            const QString path = l.path;
            out << Entry{l.name, (lut ? EffectFolders::LutPrefix : kLumaPrefix) + l.path, {}, tip, true, {},
                         [this, path, lut](Entry& e) {
                             QString error;
                             e.icon = fileIcon(path, lut, &error);
                             if (!error.isEmpty()) {
                                 e.tip += "\n" + T("Lässt sich nicht lesen: %1").arg(error);
                                 e.ok = false;
                             }
                         }};
        }
        return out;
    };
    auto videoTransitions = [&] {
        QVector<Entry> out;
        for (const auto& i : kTransitionTypes)
            if (i.type != TransitionType::Luma) // Gradient wipes come individually (per image)
                out << Entry{T(i.name), QString("video:%1").arg(i.id), cachedIcon(QString("video:%1").arg(i.id), [&] { return transitionIcon(i.type); })};
        return out;
    };
    auto audioTransitions = [&] {
        QVector<Entry> out;
        for (const auto& i : kAudioCurves)
            out << Entry{T(i.name), QString("audio:%1").arg(i.id), cachedIcon("audio", [] { return audioIcon(); })};
        return out;
    };
    // Video or audio effects by purpose (one group per category, `only` = just that one), A–Z in the language shown
    QCollator collator(QLocale{});
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    auto effects = [&](bool video, const QString& only = {}) {
        QVector<Group> out;
        for (const EffectCategoryInfo& c : kEffectCategories) {
            if (!only.isEmpty() && only != QLatin1String(c.id)) continue;
            QVector<Entry> list;
            for (const auto& e : EffectRegistry::all())
                // The custom effects (filter chain, shader) come with their code through `edit`: empty they do nothing
                if (e.library && e.video == video && e.category == c.category && e.category != EffectCategory::Custom)
                    list << Entry{e.name, kEffectPrefix + e.id, cachedIcon(kEffectPrefix + e.id, [&] { return effectIcon(e.id); }),
                                  e.description, true, e.keywords};
            std::sort(list.begin(), list.end(), [&](const Entry& a, const Entry& b) { return collator.compare(a.name, b.name) < 0; });
            if (!list.isEmpty()) out << Group{T(c.name), list};
        }
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
    auto presets = [&](bool inFolder) {
        QVector<Entry> out;
        for (const auto& l : m_userPresets) {
            if (inFolder && !group.isEmpty() && l.group != group && !l.group.startsWith(group + '/')) continue;
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
                             soundData({{"kind", SfxSynth::kindName(k)}, {"seed", int(i + 1)}}), cachedIcon("sound", [] { return soundIcon(false); }),
                             T("Aufnahme von %1 (%2), %3").arg(r.author, r.licence, secondsText(r.length))};
            }
            // The computed ones (a length = computed, also where recordings are the default)
            const double length = SfxSynth::defaultLength(k);
            for (int seed = 1; seed <= 3; ++seed)
                out << Entry{T("%1 %2 (berechnet)").arg(soundKindName(k)).arg(seed),
                             soundData({{"kind", SfxSynth::kindName(k)}, {"seed", seed},
                                        {"length", QString::number(length, 'f', 3) + "s"}}),
                             cachedIcon("sound", [] { return soundIcon(false); }), T("Berechnet (eigener Klang, keine Aufnahme), %1").arg(secondsText(length))};
        }
        return out;
    };
    auto soundFiles = [&](const QVector<EffectFolders::LutEntry>& list, bool own, bool inFolder) {
        QVector<Entry> out;
        for (const auto& l : list) {
            if (inFolder && own && !group.isEmpty() && l.group != group && !l.group.startsWith(group + '/')) continue;
            out << Entry{l.name, soundData({{"kind", l.path}}), cachedIcon(own ? "sound:own" : "sound", [own] { return soundIcon(own); }), QDir::toNativeSeparators(l.path)};
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
        return soundFiles(list, false, false);
    };
    auto recipes = [&](bool builtIn) {
        // Built-in and own recipes (CompRecipes::all); dragged onto a clip / double-clicked = applied (one undo step)
        QVector<Entry> out;
        for (const CompRecipe& r : CompRecipes::all()) {
            if (r.builtIn != builtIn) continue;
            QStringList tip{CompRecipes::shownDescription(r)};
            QStringList slotIds;
            for (const CompRecipeSlot& slot : r.slotList) slotIds << slot.id;
            if (!slotIds.isEmpty()) tip << T("Einstellbar: %1").arg(slotIds.join(", "));
            if (!r.builtIn) tip << QDir::toNativeSeparators(r.dir);
            out << Entry{CompRecipes::shownName(r), CompUi::kRecipePrefix + r.id,
                         cachedIcon(r.builtIn ? "recipe" : "recipe:own", [&] { return recipeIcon(!r.builtIn); }), tip.join('\n')};
        }
        return out;
    };
    const QVector<Entry> titles{Entry{T("Text"), kTitleData, cachedIcon("title:text", [] { return titleIcon(); })},
                                Entry{T("Zähler"), kCounterData, cachedIcon("title:counter", [] { return titleIcon(TitleKind::Counter); }),
                                      T("Zahl, die über die Länge des Clips hochzählt (z. B. 0 → 1000 Kills)")},
                                Entry{T("Fortschrittsbalken"), kProgressData, cachedIcon("title:progress", [] { return titleIcon(TitleKind::Progress); }),
                                      T("Balken am Bildrand, der zeigt, wie viel vom Video noch kommt (über die "
                                        "ganze Timeline)")}};
    // Shapes & drawing: the paint tool first, then every shape kind (dropped = a shape title of that kind)
    auto shapes = [&] {
        QVector<Entry> out;
        out << Entry{m_drawKey.isEmpty() ? T("Malen") : T("Malen (%1)").arg(m_drawKey), kDrawData,
                     cachedIcon("draw", [] { return drawIcon(); }),
                     T("Doppelklick: mit Maus oder Stift direkt ins Bild malen, die Striche werden ein Clip über dem Bild "
                       "(wie Timeline › Werkzeuge › Malen)"),
                     true, {"zeichnen", "draw", "paint", "pen", "Stift"}, {}, false};
        for (const auto& k : kShapeKinds)
            out << Entry{T(k.name), kShapePrefix + QString::fromLatin1(k.id),
                         cachedIcon(QString("shape:%1").arg(k.id), [&] { return shapeIcon(k.kind); }),
                         T("In die Timeline ziehen oder Doppelklick (am Abspielkopf): eine Form als eigener Clip über dem "
                           "Bild, mit Füllung, Rand und Schatten; auch als Unschärfe oder Spotlight"),
                         true, {"Form", "shape"}};
        return out;
    };
    // Everything, each group under its full name: the search, the favourites and recently used look through it
    auto everything = [&] {
        QVector<Group> all{{T("Übergänge › Video"), videoTransitions()},
                           {T("Übergänge › Audio"), audioTransitions()},
                           {T("Übergänge › Verlaufsblenden"), files(EffectFolders::builtinTransitions(), false, true, false)
                                                                  + files(m_userTransitions, false, false, false)},
                           {T("Titel"), titles},
                           {T("Formen & Zeichnen"), shapes()}};
        for (Group& g : effects(true)) all << Group{T("Videoeffekte › %1").arg(g.header), g.entries};
        for (Group& g : effects(false)) all << Group{T("Audioeffekte › %1").arg(g.header), g.entries};
        all << Group{QStringLiteral("LUTs"), files(EffectFolders::builtinLuts(), true, true, false) + files(m_userLuts, true, false, false)}
            << Group{T("Klänge"), builtinSounds() + packSounds() + soundFiles(m_userSounds, true, false)}
            << Group{T("Compositing-Rezepte"), recipes(true) + recipes(false)}
            << Group{QStringLiteral("Presets"), builtinPresets() + presets(false)};
        return all;
    };

    const QStringList favorites = EffectFavorites::favorites();
    bool listed = false; // Favourites/Recently used: entries whose key matched (no header for "nothing found")
    if (!filter.isEmpty()) {
        groups = everything(); // the search looks through everything, whatever category is selected
    } else {
        switch (shown) {
        case Favorites:
            for (Group& g : everything()) {
                QVector<Entry> hits;
                for (const Entry& e : g.entries)
                    if (favorites.contains(EffectFavorites::key(e.data))) hits << e;
                if (!hits.isEmpty()) groups << Group{g.header, hits};
            }
            listed = !groups.isEmpty();
            break;
        case Recent: {
            // Most recent first: one group without header (an item that no longer exists is left out)
            QHash<QString, Entry> byKey;
            for (const Group& g : everything())
                for (const Entry& e : g.entries) {
                    Entry withGroup = e;
                    withGroup.tip = e.tip.isEmpty() ? g.header : g.header + "\n" + e.tip;
                    byKey.insert(EffectFavorites::key(e.data), withGroup);
                }
            QVector<Entry> hits;
            for (const QString& key : EffectFavorites::recent())
                if (auto it = byKey.constFind(key); it != byKey.cend()) hits << *it;
            groups = {{{}, hits}};
            listed = !hits.isEmpty();
            break;
        }
        case All: // the old Toolbox (not shown any more)
        case TransitionsAll:
            groups = {{T("Video"), videoTransitions()}, {T("Audio"), audioTransitions()},
                      {T("Verlaufsblenden"), files(EffectFolders::builtinTransitions(), false, true, true)
                                                 + files(m_userTransitions, false, false, true)}};
            break;
        case VideoTransitions: groups = {{{}, videoTransitions()}}; break;
        case AudioTransitions: groups = {{{}, audioTransitions()}}; break;
        case Titles: groups = {{{}, titles}}; break;
        case ShapesCat: groups = {{{}, shapes()}}; break;
        case OpenFx:
        case Filters: groups = effects(true); break;
        case AudioFx: groups = effects(false); break;
        case VideoFxGroup:
        case AudioFxGroup:
            groups = effects(shown == VideoFxGroup, group);
            if (!groups.isEmpty()) groups.first().header.clear(); // the category itself: no header
            break;
        case LutsAll:
            groups = {{T("Mitgeliefert"), files(EffectFolders::builtinLuts(), true, true, true)},
                      {T("Eigene"), files(m_userLuts, true, false, true)}};
            break;
        case LumasAll:
            groups = {{T("Mitgeliefert"), files(EffectFolders::builtinTransitions(), false, true, true)},
                      {T("Eigene"), files(m_userTransitions, false, false, true)}};
            break;
        case BuiltinLuts: groups = {{{}, files(EffectFolders::builtinLuts(), true, true, true)}}; break;
        case BuiltinTransitions: groups = {{{}, files(EffectFolders::builtinTransitions(), false, true, true)}}; break;
        case Luts:
        case LutGroup: groups = {{{}, files(m_userLuts, true, false, true)}}; break;
        case Transitions:
        case TransitionGroup: groups = {{{}, files(m_userTransitions, false, false, true)}}; break;
        case PresetsAll: groups = {{T("Mitgeliefert"), builtinPresets()}, {T("Eigene"), presets(true)}}; break;
        case PresetGroup: groups = {{{}, presets(true)}}; break;
        case SoundsAll:
            groups = {{T("Mitgeliefert"), builtinSounds()}, {T("Pakete"), packSounds()},
                      {T("Eigene"), soundFiles(m_userSounds, true, true)}};
            break;
        case BuiltinSounds: groups = {{{}, builtinSounds()}}; break;
        case PackSounds: groups = {{{}, packSounds()}}; break;
        case Sounds:
        case SoundGroup: groups = {{{}, soundFiles(m_userSounds, true, true)}}; break;
        case RecipesCat: groups = {{T("Mitgeliefert"), recipes(true)}, {T("Eigene"), recipes(false)}}; break;
        }
    }

    for (Group& g : groups) {
        QVector<Entry*> hits;
        for (Entry& e : g.entries)
            if (filter.isEmpty() || e.name.contains(filter, Qt::CaseInsensitive)
                || std::any_of(e.keywords.begin(), e.keywords.end(),
                               [&](const QString& k) { return k.contains(filter, Qt::CaseInsensitive); }))
                hits << &e;
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
        for (Entry* e : hits) {
            if (e->load) e->load(*e);
            const QString key = EffectFavorites::key(e->data);
            const bool favorite = !key.isEmpty() && favorites.contains(key);
            auto* it = new QListWidgetItem(QIcon(favorite ? starred(e->icon) : e->icon), e->name);
            it->setData(Qt::UserRole, e->ok ? e->data : QString()); // broken file: visible, but not applicable
            it->setFlags(!e->ok   ? Qt::ItemIsEnabled
                         : e->drag ? Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled
                                   : Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            if (!e->ok) it->setForeground(Theme::warning);
            QString tip = e->tip;
            if (favorite) tip = tip.isEmpty() ? T("Favorit") : T("Favorit") + "\n" + tip;
            if (!tip.isEmpty()) it->setToolTip(tip);
            m_list->addItem(it);
        }
    }
    if (m_list->count() == 0 && filter.isEmpty()) {
        QString hint;
        if (shown == Favorites && !listed)
            hint = T("Noch keine Favoriten – Rechtsklick auf einen Eintrag › Zu Favoriten hinzufügen (oder Taste F)");
        if (shown == Recent && !listed) hint = T("Noch nichts benutzt – was du anwendest, steht hier, das Neueste oben");
        if (shown == Luts || shown == LutGroup) hint = T("Noch keine LUTs – „Effekte importieren…“ öffnet den Ordner");
        if (shown == Transitions || shown == TransitionGroup)
            hint = T("Noch keine Verlaufsblenden – „Effekte importieren…“ öffnet den Ordner");
        if (shown == PresetGroup)
            hint = T("Noch keine Presets – „Effekte importieren…“ öffnet den Ordner");
        if (shown == Sounds || shown == SoundGroup)
            hint = T("Noch keine eigenen Klänge – „Effekte importieren…“ öffnet den Ordner (Unterordner „Sounds“)");
        if (shown == PackSounds) hint = T("Noch keine Klangpakete installiert");
        if (shown == RecipesCat) hint = T("Noch keine Compositing-Rezepte");
        if (!hint.isEmpty()) {
            auto* h = new QListWidgetItem(hint);
            h->setFlags(Qt::NoItemFlags);
            h->setForeground(Theme::textDim);
            h->setData(Qt::ToolTipRole, hint);
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
