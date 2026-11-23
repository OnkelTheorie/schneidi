// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/DeviceMedia.h"

#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QStandardPaths>
#include <QTimer>
#include <algorithm>

#ifdef Q_OS_ANDROID
#include <QCoreApplication>
#include <QFutureWatcher>
#include <QJniEnvironment>
#include <QJniObject>
#include <QtCore/private/qandroidextras_p.h>
#endif

namespace DeviceMedia {

namespace {
QVector<DeviceMediaItem>& testItems()
{
    static QVector<DeviceMediaItem> items;
    return items;
}
} // namespace

void setTestItems(const QVector<DeviceMediaItem>& items) { testItems() = items; }
bool hasTestItems() { return !testItems().isEmpty(); }

#ifdef Q_OS_ANDROID
namespace {

QJniObject resolver()
{
    QJniObject context = QNativeInterface::QAndroidApplication::context();
    return context.isValid() ? context.callObjectMethod("getContentResolver", "()Landroid/content/ContentResolver;")
                             : QJniObject();
}

int sdk() { return QNativeInterface::QAndroidApplication::sdkVersion(); }

QStringList wantedPermissions()
{
    if (sdk() >= 33) return {"android.permission.READ_MEDIA_VIDEO", "android.permission.READ_MEDIA_IMAGES"};
    return {"android.permission.READ_EXTERNAL_STORAGE"};
}

// One MediaStore collection ("Video" / "Images"), newest first
void query(const char* kind, bool video, QVector<DeviceMediaItem>* out)
{
    QJniObject r = resolver();
    if (!r.isValid()) return;
    QJniEnvironment env;
    const QByteArray cls = QByteArray("android/provider/MediaStore$") + kind + "$Media";
    QJniObject base = QJniObject::getStaticObjectField(cls.constData(), "EXTERNAL_CONTENT_URI", "Landroid/net/Uri;");
    if (!base.isValid()) return;
    QStringList cols{"_id", "_display_name", "datetaken", "date_added", "_data"};
    if (video) cols << "duration";
    jobjectArray projection = env->NewObjectArray(jsize(cols.size()), env.findClass("java/lang/String"), nullptr);
    for (int i = 0; i < cols.size(); ++i)
        env->SetObjectArrayElement(projection, i, QJniObject::fromString(cols[i]).object<jstring>());
    QJniObject cursor = r.callObjectMethod(
        "query",
        "(Landroid/net/Uri;[Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;Ljava/lang/String;)Landroid/database/Cursor;",
        base.object(), projection, nullptr, nullptr, QJniObject::fromString("date_added DESC").object<jstring>());
    env->DeleteLocalRef(projection);
    if (env.checkAndClearExceptions() || !cursor.isValid()) return;
    QVector<jint> idx;
    for (const QString& c : cols)
        idx << cursor.callMethod<jint>("getColumnIndex", "(Ljava/lang/String;)I", QJniObject::fromString(c).object<jstring>());
    while (cursor.callMethod<jboolean>("moveToNext")) {
        DeviceMediaItem item;
        item.video = video;
        const jlong id = cursor.callMethod<jlong>("getLong", "(I)J", idx[0]);
        item.name = cursor.callObjectMethod("getString", "(I)Ljava/lang/String;", idx[1]).toString();
        const jlong taken = idx[2] >= 0 ? cursor.callMethod<jlong>("getLong", "(I)J", idx[2]) : 0;
        const jlong added = idx[3] >= 0 ? cursor.callMethod<jlong>("getLong", "(I)J", idx[3]) : 0;
        item.date = taken > 0 ? QDateTime::fromMSecsSinceEpoch(taken) : QDateTime::fromSecsSinceEpoch(added);
        const QString data = idx[4] >= 0 ? cursor.callObjectMethod("getString", "(I)Ljava/lang/String;", idx[4]).toString() : QString();
        if (video && idx.size() > 5 && idx[5] >= 0) item.durationMs = cursor.callMethod<jlong>("getLong", "(I)J", idx[5]);
        item.uri = QJniObject::callStaticObjectMethod("android/content/ContentUris", "withAppendedId",
                                                      "(Landroid/net/Uri;J)Landroid/net/Uri;", base.object(), id)
                       .toString();
        const QFileInfo fi(data);
        item.path = !data.isEmpty() && fi.isFile() && fi.isReadable() ? data : item.uri;
        out->append(item);
    }
    cursor.callMethod<void>("close");
    env.checkAndClearExceptions();
}

} // namespace

QVector<DeviceMediaItem> list()
{
    if (!testItems().isEmpty()) return testItems();
    QVector<DeviceMediaItem> out;
    query("Video", true, &out);
    query("Images", false, &out);
    std::stable_sort(out.begin(), out.end(), [](const DeviceMediaItem& a, const DeviceMediaItem& b) { return a.date > b.date; });
    return out;
}

QImage thumbnail(const DeviceMediaItem& item, const QSize& size)
{
    if (item.uri.isEmpty() || sdk() < 29) return {};
    QJniEnvironment env;
    QJniObject r = resolver();
    QJniObject uri = QJniObject::callStaticObjectMethod("android/net/Uri", "parse", "(Ljava/lang/String;)Landroid/net/Uri;",
                                                        QJniObject::fromString(item.uri).object<jstring>());
    QJniObject want("android/util/Size", "(II)V", jint(size.width()), jint(size.height()));
    QJniObject bmp = r.callObjectMethod(
        "loadThumbnail", "(Landroid/net/Uri;Landroid/util/Size;Landroid/os/CancellationSignal;)Landroid/graphics/Bitmap;",
        uri.object(), want.object(), nullptr);
    if (env.checkAndClearExceptions() || !bmp.isValid()) return {};
    QJniObject bos("java/io/ByteArrayOutputStream");
    QJniObject jpeg = QJniObject::getStaticObjectField("android/graphics/Bitmap$CompressFormat", "JPEG",
                                                       "Landroid/graphics/Bitmap$CompressFormat;");
    bmp.callMethod<jboolean>("compress", "(Landroid/graphics/Bitmap$CompressFormat;ILjava/io/OutputStream;)Z",
                             jpeg.object(), jint(85), bos.object());
    bmp.callMethod<void>("recycle");
    QJniObject bytes = bos.callObjectMethod("toByteArray", "()[B");
    if (env.checkAndClearExceptions() || !bytes.isValid()) return {};
    const auto arr = bytes.object<jbyteArray>();
    const jsize n = env->GetArrayLength(arr);
    QByteArray data(n, Qt::Uninitialized);
    env->GetByteArrayRegion(arr, 0, n, reinterpret_cast<jbyte*>(data.data()));
    return QImage::fromData(data);
}

bool hasPermission()
{
    const auto granted = [](const char* p) {
        return QtAndroidPrivate::checkPermission(QString::fromLatin1(p)).result() == QtAndroidPrivate::Authorized;
    };
    if (sdk() < 33) return granted("android.permission.READ_EXTERNAL_STORAGE");
    // Videos (photos too, ideally); Android 14+'s "limited access" (what the user picked) counts as well
    return granted("android.permission.READ_MEDIA_VIDEO")
           || (sdk() >= 34 && granted("android.permission.READ_MEDIA_VISUAL_USER_SELECTED"));
}

void requestPermission(QObject* context, std::function<void(bool)> done)
{
    // One after the other (Android shows one dialog per kind); granted = videos may be read
    const QStringList all = wantedPermissions();
    auto ask = std::make_shared<std::function<void(int, bool)>>();
    // (itself only weakly: a strong self-capture kept the function alive forever; each pending answer holds it)
    std::weak_ptr<std::function<void(int, bool)>> self = ask;
    *ask = [context, all, done, self](int i, bool any) {
        const auto ask = self.lock();
        if (!ask) return;
        if (i >= all.size()) {
            done(any || hasPermission());
            return;
        }
        auto* watch = new QFutureWatcher<QtAndroidPrivate::PermissionResult>(context);
        QObject::connect(watch, &QFutureWatcherBase::finished, context, [watch, i, any, ask] {
            const bool ok = watch->result() == QtAndroidPrivate::Authorized;
            watch->deleteLater();
            (*ask)(i + 1, any || (ok && i == 0)); // the first one is the videos'
        });
        watch->setFuture(QtAndroidPrivate::requestPermission(all[i]));
    };
    (*ask)(0, false);
}

#else

QVector<DeviceMediaItem> list()
{
    if (!testItems().isEmpty()) return testItems();
    // A stand-in off Android: the videos and pictures folders (not their subfolders)
    QVector<DeviceMediaItem> out;
    static const QStringList videoExt{"mp4", "mov", "mkv", "webm", "avi", "m4v"};
    static const QStringList photoExt{"jpg", "jpeg", "png", "webp", "heic"};
    for (const auto where : {QStandardPaths::MoviesLocation, QStandardPaths::PicturesLocation}) {
        const QDir dir(QStandardPaths::writableLocation(where));
        for (const QFileInfo& fi : dir.entryInfoList(QDir::Files)) {
            const QString ext = fi.suffix().toLower();
            if (!videoExt.contains(ext) && !photoExt.contains(ext)) continue;
            DeviceMediaItem item;
            item.path = fi.absoluteFilePath();
            item.name = fi.fileName();
            item.date = fi.lastModified();
            item.video = videoExt.contains(ext);
            out << item;
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const DeviceMediaItem& a, const DeviceMediaItem& b) { return a.date > b.date; });
    return out;
}

QImage thumbnail(const DeviceMediaItem& item, const QSize& size)
{
    if (item.video) return {};
    QImageReader reader(item.path);
    reader.setAutoTransform(true);
    const QSize full = reader.size();
    if (full.isValid()) reader.setScaledSize(full.scaled(size, Qt::KeepAspectRatioByExpanding));
    return reader.read();
}

bool hasPermission() { return true; }

void requestPermission(QObject* context, std::function<void(bool)> done)
{
    QTimer::singleShot(0, context, [done] { done(true); });
}

#endif

} // namespace DeviceMedia
