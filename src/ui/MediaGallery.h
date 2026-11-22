// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// schneidi's own media gallery (the phone's "+ Videos hinzufügen"): the device's videos and photos newest first,
// grouped by day, big tiles with their length; tapping picks them in order (a number on each), "Hinzufügen (n)" takes
// them in that order. "Andere Datei…" leaves for the system's file picker (MainWindow opens it). Data and pictures:
// engine/DeviceMedia.

#include <QDialog>
#include <QVector>

#include "engine/DeviceMedia.h"

class QAbstractButton;
class QGridLayout;
class QLabel;
class QPushButton;
class QScrollArea;
class QVBoxLayout;
class GalleryTile;

class MediaGallery : public QDialog {
    Q_OBJECT
public:
    enum class Filter { All, Videos, Photos };
    explicit MediaGallery(QWidget* parent = nullptr);

    QStringList chosenPaths() const; // in the order they were picked
    bool otherFileWanted() const { return m_other; }
    void setFilter(Filter filter);
    Filter filter() const { return m_filter; }
    QVector<GalleryTile*> tiles() const; // shown ones (tests)
    QPushButton* addButton() const { return m_add; }

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void rebuild();              // sections and tiles for the filter
    void toggle(int item);       // picked / not picked (the order numbers follow)
    void updatePicks();
    void loadThumbnails();
    int columns() const;

    QVector<DeviceMediaItem> m_items;
    QVector<int> m_picked;       // indexes into m_items, in picking order
    Filter m_filter = Filter::All;
    QVector<GalleryTile*> m_tiles;
    QVector<QAbstractButton*> m_filterButtons;
    QScrollArea* m_scroll;
    QWidget* m_body = nullptr;
    QLabel* m_empty;
    QPushButton* m_add;
    bool m_other = false;
    int m_columns = 0;
};

// One tile: the picture cropped to its square, the length at the bottom right (videos), the pick number at the top right
class GalleryTile : public QWidget {
    Q_OBJECT
public:
    GalleryTile(int item, const DeviceMediaItem& media, QWidget* parent = nullptr);
    int item() const { return m_item; }
    void setPicture(const QImage& image);
    void setNumber(int number); // 0 = not picked
    int number() const { return m_number; }
    QSize sizeHint() const override { return {120, 120}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override { return w; }

signals:
    void tapped(int item);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseReleaseEvent(class QMouseEvent* event) override;

private:
    int m_item;
    DeviceMediaItem m_media;
    QImage m_picture;
    int m_number = 0;
};
