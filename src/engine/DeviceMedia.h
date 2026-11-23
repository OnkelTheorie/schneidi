// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// The videos and photos on the device for schneidi's own media gallery (ui/MediaGallery): Android's MediaStore,
// newest first; elsewhere the user's videos and pictures folders (a stand-in, the gallery is a phone thing). Reading
// them on Android needs READ_MEDIA_VIDEO/IMAGES (13+) or READ_EXTERNAL_STORAGE: hasPermission/requestPermission.

#include <QDateTime>
#include <QImage>
#include <QSize>
#include <QString>
#include <QVector>
#include <functional>

struct DeviceMediaItem {
    QString path;       // what import opens: the file's path where readable, else its content:// address
    QString uri;        // Android: the item's content:// address (thumbnails); empty elsewhere
    QString name;       // file name
    QDateTime date;     // taken (else added) date
    qint64 durationMs = 0;
    bool video = true;
};

namespace DeviceMedia {

QVector<DeviceMediaItem> list(); // videos and photos, newest first
// A thumbnail about `size` (cropped by the caller): Android's own (fast, cached by the system); elsewhere a photo
// read small, nothing for a video
QImage thumbnail(const DeviceMediaItem& item, const QSize& size);
bool hasPermission();
// Asks Android (the system dialog); `done` gets whether it was granted. Elsewhere: true at once
void requestPermission(QObject* context, std::function<void(bool granted)> done);

// Tests: the items list() returns instead of the device's (empty = the device's again)
void setTestItems(const QVector<DeviceMediaItem>& items);
bool hasTestItems();

} // namespace DeviceMedia
