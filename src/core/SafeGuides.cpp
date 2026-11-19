// SPDX-License-Identifier: AGPL-3.0-only
#include "core/SafeGuides.h"

#include "core/ExportCheck.h"
#include "core/Types.h"

#include <QFont>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace SafeGuides {

namespace {

QRectF inset(double W, double H, double share)
{
    const double dx = W * (1 - share) / 2, dy = H * (1 - share) / 2;
    return QRectF(dx, dy, W - 2 * dx, H - 2 * dy);
}

} // namespace

void draw(QImage& img, const ProjectFormat& f, const ExportCheck::Options& o)
{
    if (img.isNull()) return;
    if (img.format() != QImage::Format_ARGB32_Premultiplied && img.format() != QImage::Format_RGB32)
        img = img.convertToFormat(QImage::Format_RGB32);
    const double W = img.width(), H = img.height();
    const double line = std::max(1.0, std::round(std::min(W, H) / 360.0));
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, false);
    QFont font = p.font();
    font.setPixelSize(int(std::clamp(std::min(W, H) / 40.0, 9.0, 28.0)));
    p.setFont(font);
    const auto label = [&](const QPointF& at, const QString& text, const QColor& color) {
        p.setPen(QColor(0, 0, 0, 200));
        p.drawText(at + QPointF(1, 1), text);
        p.setPen(color);
        p.drawText(at, text);
    };
    // The phone apps' zones first (under the lines): tinted, outlined, named
    if (ExportCheck::isVertical(f)) {
        const QColor zone(255, 64, 64);
        const double bottomY = H * (1 - o.safeBottom);
        const QRectF zones[] = {QRectF(0, bottomY, W, H - bottomY), QRectF(0, 0, W, H * o.safeTop),
                                QRectF(W * (1 - o.safeRight), H * o.safeRightFrom, W * o.safeRight,
                                       std::max(0.0, bottomY - H * o.safeRightFrom))};
        for (const QRectF& r : zones) {
            p.fillRect(r, QColor(zone.red(), zone.green(), zone.blue(), 56));
            p.setPen(QPen(zone, line, Qt::DashLine));
            p.setBrush(Qt::NoBrush);
            p.drawRect(r);
        }
        const double x = W * (1 - kTitleSafe) / 2 + line * 3; // inside the safe lines
        label(QPointF(x, bottomY + font.pixelSize() * 1.3), "app UI: caption, buttons", zone);
        label(QPointF(x, H * o.safeTop - line * 3), "app UI: top row", zone);
    }
    // Action safe (cyan) and title safe (yellow)
    const struct {
        double share;
        QColor color;
        const char* name;
    } boxes[] = {{kActionSafe, QColor(0, 220, 255), "action safe"}, {kTitleSafe, QColor(255, 214, 0), "title safe"}};
    for (const auto& b : boxes) {
        const QRectF r = inset(W, H, b.share);
        p.setPen(QPen(b.color, line));
        p.setBrush(Qt::NoBrush);
        p.drawRect(r);
        // title safe named at its top left, action safe at its bottom right (apart, not over the app zones' names)
        const QString name = QString::fromLatin1(b.name);
        if (b.share == kTitleSafe) {
            label(r.topLeft() + QPointF(line * 3, font.pixelSize() + line * 2), name, b.color);
        } else {
            const double w = p.fontMetrics().horizontalAdvance(name);
            label(r.bottomRight() - QPointF(w + line * 3, line * 3), name, b.color);
        }
    }
    // The centre (a small cross)
    const double c = std::min(W, H) / 40.0;
    p.setPen(QPen(QColor(255, 255, 255, 160), line));
    p.drawLine(QPointF(W / 2 - c, H / 2), QPointF(W / 2 + c, H / 2));
    p.drawLine(QPointF(W / 2, H / 2 - c), QPointF(W / 2, H / 2 + c));
}

} // namespace SafeGuides
