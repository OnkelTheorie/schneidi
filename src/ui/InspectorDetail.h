// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Internal helpers of Inspector, shared by its implementation files (not part of the API).

#include "ui/Inspector.h"
#include "app/Theme.h"
#include "core/EffectFolders.h"
#include "core/EffectRegistry.h"
#include "core/Editor.h"
#include "core/I18n.h"
#include "core/Keyframes.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/Subtitles.h"
#include "core/Timecode.h"
#include "core/TimelineOps.h"
#include "ui/ScrubField.h"
#include <QButtonGroup>
#include <QCheckBox>
#include <QColorDialog>
#include <QDir>
#include <QComboBox>
#include <QFontComboBox>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <cmath>

namespace InspectorDetail {

constexpr int kSliderSteps = 1000;
constexpr int kTitlePage = 2;
constexpr int kTransitionPage = 3; // only with a selected transition, then the only tab
constexpr int kSubtitlePage = 4;   // only with selected subtitles, then the only tab
constexpr int kEffectsPage = 5;    // effects of the video clip (like DaVinci's Inspector "Effects" tab), only when it has some

inline EffectInstance* findEffect(Clip& c, const QString& id)
{
    for (auto& e : c.effects)
        if (e.effectId == id) return &e;
    return nullptr;
}

inline EffectInstance& ensureEffect(Clip& c, const QString& id)
{
    if (EffectInstance* e = findEffect(c, id)) return *e;
    EffectInstance inst;
    inst.effectId = id;
    inst.enabled = false; // only switch on via the red dot (or a setting)
    if (const EffectDescriptor* d = EffectRegistry::find(id))
        for (const auto& p : d->params) inst.params[p.key] = p.defaultValue;
    c.effects << inst;
    return c.effects.last();
}

inline QVariant effectParam(const Clip& c, const QString& effectId, const QString& key)
{
    for (const auto& e : c.effects)
        if (e.effectId == effectId && e.params.contains(key)) return e.params.value(key);
    if (const EffectDescriptor* d = EffectRegistry::find(effectId))
        for (const auto& p : d->params)
            if (p.key == key) return p.defaultValue;
    return {};
}

// Red dot like in DaVinci: on = red, off = gray
inline QToolButton* makeDot()
{
    auto* dot = new QToolButton;
    dot->setCheckable(true);
    dot->setFixedSize(10, 10);
    dot->setFocusPolicy(Qt::NoFocus);
    dot->setToolTip(T("Bereich an/aus"));
    dot->setStyleSheet(QString("QToolButton { border: none; border-radius: 5px; background: %1; }"
                               "QToolButton:checked { background: %2; }")
                           .arg(Theme::controlOff.name(), Theme::warning.name()));
    return dot;
}

inline QLabel* axisLabel(const QString& t)
{
    auto* l = new QLabel(t);
    l->setStyleSheet(QString("color: %1; font-size: 8pt;").arg(Theme::textDim.name()));
    return l;
}

// Chain icon (Zoom X/Y linked): on = bright, off = dimmed
inline QIcon chainIcon()
{
    auto draw = [](const QColor& col) {
        QPixmap pm(32, 32);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(col, 3));
        p.translate(16, 16);
        p.rotate(-45);
        p.drawRoundedRect(QRectF(-13, -5, 15, 10), 5, 5);
        p.drawRoundedRect(QRectF(-2, -5, 15, 10), 5, 5);
        return pm;
    };
    QIcon icon;
    icon.addPixmap(draw(Theme::textDim), QIcon::Normal, QIcon::Off);
    icon.addPixmap(draw(Theme::text), QIcon::Normal, QIcon::On);
    return icon;
}

// Alignment icon: lines of different length, left/center/right
inline QIcon alignIcon(int align)
{
    auto draw = [align](const QColor& col) {
        QPixmap pm(32, 32);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setPen(QPen(col, 3));
        const int widths[] = {24, 16, 24, 12};
        for (int i = 0; i < 4; ++i) {
            const int w = widths[i];
            const int x = align == 0 ? 4 : align == 2 ? 28 - w : 16 - w / 2;
            p.drawLine(x, 6 + i * 7, x + w, 6 + i * 7);
        }
        return pm;
    };
    QIcon icon;
    icon.addPixmap(draw(Theme::textDim), QIcon::Normal, QIcon::Off);
    icon.addPixmap(draw(Theme::text), QIcon::Normal, QIcon::On);
    return icon;
}

// Toggle button (bold, italic, alignment)
inline QToolButton* toggleButton()
{
    auto* b = new QToolButton;
    b->setCheckable(true);
    b->setFixedSize(26, 22);
    b->setIconSize(QSize(16, 16));
    b->setFocusPolicy(Qt::NoFocus);
    b->setStyleSheet(QString("QToolButton { color: %1; border: 1px solid %2; border-radius: 2px; background: transparent; }"
                             "QToolButton:checked { color: %3; background: %4; }")
                         .arg(Theme::textDim.name(), Theme::border.name(), Theme::text.name(), Theme::panelHeader.name()));
    return b;
}

// Trash can (remove effect) like in the DaVinci Inspector
inline QIcon trashIcon()
{
    auto draw = [](const QColor& col) {
        QPixmap pm(32, 32);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(col, 2.5));
        p.drawLine(QPointF(7, 9), QPointF(25, 9));    // lid
        p.drawLine(QPointF(13, 9), QPointF(13, 6));
        p.drawLine(QPointF(13, 6), QPointF(19, 6));
        p.drawLine(QPointF(19, 6), QPointF(19, 9));
        p.drawLine(QPointF(9, 12), QPointF(10.5, 27)); // bucket
        p.drawLine(QPointF(10.5, 27), QPointF(21.5, 27));
        p.drawLine(QPointF(21.5, 27), QPointF(23, 12));
        p.drawLine(QPointF(14, 14), QPointF(14.3, 24));
        p.drawLine(QPointF(18, 14), QPointF(17.7, 24));
        return pm;
    };
    QIcon icon;
    icon.addPixmap(draw(Theme::textDim), QIcon::Normal);
    icon.addPixmap(draw(Theme::text), QIcon::Active);
    return icon;
}

// Grip (two columns of dots) on the header of an effect that can be dragged into another place of the order
inline QPixmap gripPixmap(qreal dpr)
{
    QPixmap pm(QSize(8, 14) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(Theme::textDim);
    for (int col = 0; col < 2; ++col)
        for (int row = 0; row < 3; ++row) p.drawEllipse(QPointF(2 + col * 4, 3 + row * 4), 1.1, 1.1);
    return pm;
}

// Pipette (pick the key color from the picture)
inline QIcon pipetteIcon()
{
    auto draw = [](const QColor& col) {
        QPixmap pm(32, 32);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.translate(16, 16);
        p.rotate(45);
        p.setPen(Qt::NoPen);
        p.setBrush(col);
        p.drawRoundedRect(QRectF(-4.5, -15, 9, 9), 3, 3); // bulb
        p.drawRect(QRectF(-6.5, -6.5, 13, 3));            // collar
        p.setPen(QPen(col, 2.5));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(-2.5, -3, 5, 13)); // tube
        p.drawLine(QPointF(0, 10), QPointF(0, 15));
        return pm;
    };
    QIcon icon;
    icon.addPixmap(draw(Theme::textDim), QIcon::Normal, QIcon::Off);
    icon.addPixmap(draw(Theme::text), QIcon::Normal, QIcon::On);
    return icon;
}

inline int sliderPos(double v, double min, double max)
{
    return int(std::lround((v - min) / std::max(1e-9, max - min) * kSliderSteps));
}

} // namespace InspectorDetail
