// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// schneidi's own media gallery (the phone's "+ Videos hinzufügen"): the device's videos and photos newest first,
// grouped by day, big tiles with their length; tapping picks them in order (a number on each), "Hinzufügen (n)" takes
// them in that order. "Andere Datei…" leaves for the system's file picker (MainWindow opens it). Data and pictures:
// engine/DeviceMedia.

#include <QDialog>
#include <QVector>
#include <memory>
#include <atomic>

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
    ~MediaGallery() override;
    static constexpr int kPage = 120; // tiles built at a time
    bool loading() const { return m_loading; } // the device's list still being read (in the background)
    int thumbnailsRequested() const { return m_requested; } // (tests: only tiles in sight ask)

    QStringList chosenPaths() const; // in the order they were picked
    bool otherFileWanted() const { return m_other; }
    void setFilter(Filter filter);
    Filter filter() const { return m_filter; }
    QVector<GalleryTile*> tiles() const; // shown ones (tests)
    QPushButton* addButton() const { return m_add; }

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void rebuild();              // sections and tiles for the filter (its first page)
    void appendPage();           // the next kPage tiles (a library of thousands: tiles as the list is scrolled)
    bool morePages() const;      // items for the filter not yet shown
    void toggle(int item);       // picked / not picked (the order numbers follow)
    void updatePicks();
    void loadThumbnails(); // the tiles in sight (and a screen below) that have no picture yet
    void listed(const QVector<DeviceMediaItem>& items);
    int columns() const;

    QVector<DeviceMediaItem> m_items;
    QVector<int> m_picked;       // indexes into m_items, in picking order
    Filter m_filter = Filter::All;
    QVector<GalleryTile*> m_tiles;
    QVector<QAbstractButton*> m_filterButtons;
    QScrollArea* m_scroll;
    QWidget* m_body = nullptr;
    class QVBoxLayout* m_bodyLay = nullptr; // appendPage: the sections so far, a stretch last
    class QGridLayout* m_grid = nullptr;    // the last day's grid, its tile count and day
    int m_gridCount = 0;
    QDate m_day;
    int m_next = 0; // the next item appendPage looks at

    QLabel* m_empty;
    QPushButton* m_add;
    bool m_other = false;
    int m_columns = 0;
    bool m_loading = false;
    int m_requested = 0;
    class QTimer* m_thumbTimer = nullptr; // loadThumbnails once the layout settled (a scroll, a rebuild, a resize)
    class QThreadPool* m_pool;           // its own two threads: never the app's pool (the engine's work runs there)
    std::shared_ptr<std::atomic<int>> m_generation; // a rebuild or the end: queued pictures for old tiles skip their work
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
