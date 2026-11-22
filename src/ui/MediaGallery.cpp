// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/MediaGallery.h"

#include "app/Theme.h"
#include "core/I18n.h"

#include <QApplication>
#include <QButtonGroup>
#include <QGridLayout>
#include <QLabel>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QThreadPool>
#include <QToolButton>
#include <QVBoxLayout>

namespace {
QString length(qint64 ms)
{
    const qint64 s = (ms + 500) / 1000;
    return s >= 3600 ? QString("%1:%2:%3").arg(s / 3600).arg(s / 60 % 60, 2, 10, QChar('0')).arg(s % 60, 2, 10, QChar('0'))
                     : QString("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QChar('0'));
}

QString dayName(const QDate& d)
{
    const QDate today = QDate::currentDate();
    if (d == today) return T("Heute");
    if (d == today.addDays(-1)) return T("Gestern");
    return QLocale().toString(d, d.year() == today.year() ? QStringLiteral("dddd, d. MMMM") : QStringLiteral("d. MMMM yyyy"));
}
} // namespace

GalleryTile::GalleryTile(int item, const DeviceMediaItem& media, QWidget* parent)
    : QWidget(parent), m_item(item), m_media(media)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    setToolTip(media.name);
    setAttribute(Qt::WA_OpaquePaintEvent);
}

void GalleryTile::setPicture(const QImage& image)
{
    m_picture = image;
    update();
}

void GalleryTile::setNumber(int number)
{
    if (number == m_number) return;
    m_number = number;
    update();
}

void GalleryTile::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint())) emit tapped(m_item);
}

void GalleryTile::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    const QRect r = rect();
    p.fillRect(r, Theme::well);
    if (!m_picture.isNull()) { // cropped to the square, centred
        const QSize s = m_picture.size().scaled(r.size(), Qt::KeepAspectRatioByExpanding);
        p.drawImage(QRect(QPoint(r.center().x() - s.width() / 2, r.center().y() - s.height() / 2), s), m_picture);
    } else {
        p.setPen(Theme::textFaint);
        QFont f = font();
        f.setPixelSize(std::max(12, r.height() / 4));
        p.setFont(f);
        p.drawText(r, Qt::AlignCenter, m_media.video ? QStringLiteral("▶") : QStringLiteral("▣"));
    }
    QFont small = font();
    small.setBold(true);
    small.setPixelSize(std::max(11, r.height() / 9));
    p.setFont(small);
    if (m_media.video) { // the length, bottom right, on a dark strip
        const QString text = length(m_media.durationMs);
        const QRect tr = QFontMetrics(small).boundingRect(text).adjusted(-5, -2, 5, 2);
        const QRect box(r.right() - tr.width() - 4, r.bottom() - tr.height() - 4, tr.width(), tr.height());
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 160));
        p.drawRoundedRect(box, 3, 3);
        p.setPen(Qt::white);
        p.drawText(box, Qt::AlignCenter, text);
    }
    // Picked: the primary color's frame and its number; not picked: an empty ring (tap to pick)
    const int d = std::max(20, r.height() / 5);
    const QRect ring(r.right() - d - 5, r.top() + 5, d, d);
    if (m_number > 0) {
        p.setPen(QPen(Theme::primary, 4));
        p.setBrush(Qt::NoBrush);
        p.drawRect(r.adjusted(2, 2, -2, -2));
        p.setPen(Qt::NoPen);
        p.setBrush(Theme::primary);
        p.drawEllipse(ring);
        p.setPen(Theme::onPrimary);
        p.drawText(ring, Qt::AlignCenter, QString::number(m_number));
    } else {
        p.setPen(QPen(QColor(255, 255, 255, 220), 2));
        p.setBrush(QColor(0, 0, 0, 60));
        p.drawEllipse(ring);
    }
}

MediaGallery::MediaGallery(QWidget* parent) : QDialog(parent)
{
    setObjectName("mediaGallery");
    setWindowTitle(T("Videos hinzufügen"));
    setStyleSheet(QString("QDialog#mediaGallery { background: %1; }").arg(Theme::window.name()));
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    // Head: the title and the filter (as the desktop's page bar: dim names, the chosen one underlined)
    auto* head = new QWidget;
    head->setObjectName("galleryHead");
    head->setAttribute(Qt::WA_StyledBackground);
    head->setStyleSheet(QString("QWidget#galleryHead { background: %1; border-bottom: 1px solid %2; }"
                                "QToolButton { color: %3; border: none; border-bottom: 2px solid transparent;"
                                " padding: 6px 12px; background: transparent; }"
                                "QToolButton:checked { color: %4; border-bottom: 2px solid %4; }")
                            .arg(Theme::topBar.name(), Theme::border.name(), Theme::textDim.name(), Theme::primary.name()));
    auto* headLay = new QVBoxLayout(head);
    headLay->setContentsMargins(12, 8, 12, 0);
    auto* title = new QLabel(T("Videos hinzufügen"));
    QFont tf = title->font();
    tf.setBold(true);
    tf.setPointSizeF(tf.pointSizeF() * 1.25);
    title->setFont(tf);
    headLay->addWidget(title);
    auto* filters = new QHBoxLayout;
    filters->setSpacing(0);
    auto* group = new QButtonGroup(this);
    const QPair<Filter, QString> names[] = {{Filter::All, T("Alle")}, {Filter::Videos, T("Videos")}, {Filter::Photos, T("Fotos")}};
    for (const auto& [f, text] : names) {
        auto* b = new QToolButton;
        b->setText(text);
        b->setCheckable(true);
        b->setChecked(f == m_filter);
        b->setObjectName(QString("galleryFilter_%1").arg(int(f)));
        group->addButton(b);
        connect(b, &QToolButton::clicked, this, [this, filter = f] { setFilter(filter); });
        filters->addWidget(b);
        m_filterButtons << b;
    }
    filters->addStretch(1);
    headLay->addLayout(filters);
    lay->addWidget(head);

    m_scroll = new QScrollArea;
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setWidgetResizable(true);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    lay->addWidget(m_scroll, 1);
    m_empty = new QLabel;
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setWordWrap(true);
    m_empty->setStyleSheet(QString("color: %1; padding: 24px;").arg(Theme::textDim.name()));
    lay->addWidget(m_empty, 1);

    // Foot: another file (the system's picker), cancel, add
    auto* foot = new QWidget;
    foot->setObjectName("galleryFoot");
    foot->setAttribute(Qt::WA_StyledBackground);
    foot->setStyleSheet(QString("QWidget#galleryFoot { background: %1; border-top: 1px solid %2; }")
                            .arg(Theme::topBar.name(), Theme::border.name()));
    auto* footLay = new QHBoxLayout(foot);
    footLay->setContentsMargins(8, 8, 8, 8);
    auto* other = new QPushButton(T("Andere Datei…"));
    other->setObjectName("galleryOther");
    connect(other, &QPushButton::clicked, this, [this] {
        m_other = true;
        reject();
    });
    auto* cancel = new QPushButton(T("Abbrechen"));
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    m_add = new QPushButton;
    m_add->setObjectName("galleryAdd");
    m_add->setStyleSheet(Theme::primaryButtonStyle());
    connect(m_add, &QPushButton::clicked, this, &QDialog::accept);
    footLay->addWidget(other);
    footLay->addStretch(1);
    footLay->addWidget(cancel);
    footLay->addWidget(m_add);
    lay->addWidget(foot);

    m_items = DeviceMedia::list();
    rebuild();
    updatePicks();
}

int MediaGallery::columns() const
{
    // Tiles of ~120 px or more: 3 on a phone upright, more on a wider window
    return std::clamp(m_scroll->viewport()->width() / 120, 3, 8);
}

void MediaGallery::resizeEvent(QResizeEvent* event)
{
    QDialog::resizeEvent(event);
    if (columns() != m_columns) rebuild();
}

void MediaGallery::setFilter(Filter filter)
{
    m_filter = filter;
    for (QAbstractButton* b : m_filterButtons) b->setChecked(b->objectName() == QString("galleryFilter_%1").arg(int(filter)));
    rebuild();
}

QVector<GalleryTile*> MediaGallery::tiles() const { return m_tiles; }

void MediaGallery::rebuild()
{
    m_columns = columns();
    m_tiles.clear();
    m_body = new QWidget;
    auto* lay = new QVBoxLayout(m_body);
    lay->setContentsMargins(4, 4, 4, 4);
    lay->setSpacing(4);
    QDate day;
    QGridLayout* grid = nullptr;
    int n = 0;
    for (int i = 0; i < m_items.size(); ++i) {
        const DeviceMediaItem& m = m_items[i];
        if ((m_filter == Filter::Videos && !m.video) || (m_filter == Filter::Photos && m.video)) continue;
        if (!grid || m.date.date() != day) { // a new day: its heading and grid
            day = m.date.date();
            auto* heading = new QLabel(dayName(day));
            heading->setStyleSheet(QString("color: %1; font-weight: 600; padding: 10px 4px 2px 4px;").arg(Theme::text.name()));
            lay->addWidget(heading);
            grid = new QGridLayout;
            grid->setSpacing(3);
            for (int c = 0; c < m_columns; ++c) grid->setColumnStretch(c, 1);
            lay->addLayout(grid);
            n = 0;
        }
        auto* tile = new GalleryTile(i, m);
        tile->setNumber(int(m_picked.indexOf(i)) + 1);
        connect(tile, &GalleryTile::tapped, this, &MediaGallery::toggle);
        grid->addWidget(tile, n / m_columns, n % m_columns);
        ++n;
        m_tiles << tile;
    }
    lay->addStretch(1);
    m_scroll->setWidget(m_body); // (the old body goes)
    const bool none = m_tiles.isEmpty();
    m_scroll->setVisible(!none);
    m_empty->setVisible(none);
    m_empty->setText(m_items.isEmpty() ? T("Keine Videos oder Fotos auf dem Gerät gefunden. „Andere Datei…“ öffnet die Dateiauswahl.")
                                       : T("Hier ist nichts von dieser Art."));
    loadThumbnails();
}

void MediaGallery::loadThumbnails()
{
    // In the background, one tile after another as they come (Android: the system's cached thumbnails)
    for (GalleryTile* tile : m_tiles) {
        const DeviceMediaItem item = m_items[tile->item()];
        QPointer<GalleryTile> target(tile);
        QThreadPool::globalInstance()->start([item, target] {
            const QImage picture = DeviceMedia::thumbnail(item, QSize(320, 320));
            if (picture.isNull()) return;
            QMetaObject::invokeMethod(qApp, [picture, target] {
                if (target) target->setPicture(picture);
            }, Qt::QueuedConnection);
        });
    }
}

void MediaGallery::toggle(int item)
{
    if (const qsizetype at = m_picked.indexOf(item); at >= 0) m_picked.remove(at);
    else m_picked << item;
    updatePicks();
}

void MediaGallery::updatePicks()
{
    for (GalleryTile* tile : m_tiles) tile->setNumber(int(m_picked.indexOf(tile->item())) + 1);
    m_add->setText(m_picked.isEmpty() ? T("Hinzufügen") : T("Hinzufügen (%1)").arg(m_picked.size()));
    m_add->setEnabled(!m_picked.isEmpty());
}

QStringList MediaGallery::chosenPaths() const
{
    QStringList out;
    for (int i : m_picked) out << m_items[i].path;
    return out;
}
