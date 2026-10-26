#include "core/ContactSheet.h"

#include <QPainter>

#include <algorithm>

namespace ContactSheet {

QVector<int> spread(int from, int to, int count)
{
    QVector<int> out;
    for (int i = 0; i < count; ++i) out << from + int((i + 0.5) * (to - from) / count);
    return out;
}

QImage compose(const QVector<QImage>& images, const QStringList& labels, int columns)
{
    if (images.isEmpty()) return {};
    const int cols = columns > 0 ? std::min<int>(images.size(), columns)
                                 : std::min<int>(images.size(), images.size() <= 4 ? 2 : images.size() <= 9 ? 3 : 4);
    const int rows = (images.size() + cols - 1) / cols;
    QSize cell;
    for (const QImage& img : images) cell = cell.expandedTo(img.size());
    QImage out(cols * cell.width(), rows * cell.height(), QImage::Format_RGB32);
    out.fill(Qt::black);
    QPainter p(&out);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    QFont font = p.font();
    font.setPixelSize(std::clamp(cell.height() / 14, 12, 28));
    p.setFont(font);
    for (int i = 0; i < images.size(); ++i) {
        const QPoint at((i % cols) * cell.width(), (i / cols) * cell.height());
        QRect target(QPoint(), images[i].size().scaled(cell, Qt::KeepAspectRatio));
        target.moveCenter(QRect(at, cell).center());
        p.drawImage(target, images[i]);
        const QString label = labels.value(i);
        const QRect box(at + QPoint(4, 4), QSize(p.fontMetrics().horizontalAdvance(label) + 8, p.fontMetrics().height() + 2));
        p.fillRect(box, QColor(0, 0, 0, 170));
        p.setPen(Qt::white);
        p.drawText(box, Qt::AlignCenter, label);
        p.setPen(QColor(60, 60, 60));
        p.drawRect(QRect(at, cell).adjusted(0, 0, -1, -1));
    }
    return out;
}

} // namespace ContactSheet
