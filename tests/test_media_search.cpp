// SPDX-License-Identifier: AGPL-3.0-only
// Test the name search of the Media Storage and the Media Pool: the shared matcher (core/NameMatch: all words, one
// typo, separators alike, where it matched), the pool's search words (core/PoolFilter poolSearchWords: kind, colors,
// flags, picture format), the Media Storage search (magnifier, the folder at once, "With subfolders" in the background
// with cancel, cap and hidden folders skipped, kind filter, sort order remembered, Esc closes) and the Media Pool's
// search scope (whole project with each hit's bin, or only the open bin).
// MEDIA_SEARCH_DUMP=<folder>: pictures of both searches (icon and list view).
#include "check.h"

#include "app/Theme.h"

#include "core/I18n.h"
#include "core/NameMatch.h"
#include "core/PoolFilter.h"
#include "core/Project.h"
#include "engine/Engine.h"
#include "engine/MediaProbe.h"
#include "ui/MediaPool.h"
#include "ui/MediaStorage.h"
#include "ui/SearchHitDelegate.h"

#include <QApplication>
#include <QCheckBox>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QUrl>
#include <QTemporaryDir>
#include <QThread>
#include <QToolButton>

#include <functional>

QDebug operator<<(QDebug d, const NameMatch::Range& r)
{
    return d << "(" << r.start << "," << r.length << ")";
}

namespace {

QString g_dump;

void dumpPicture(QWidget& w, const QString& name)
{
    if (g_dump.isEmpty()) return;
    QApplication::processEvents();
    w.grab().save(QDir(g_dump).filePath(name + ".png"));
}

bool waitFor(const std::function<bool()>& done, int ms = 20000)
{
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < ms) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    return done();
}

void settle(int ms = 100)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(2);
    }
}

QListWidgetItem* itemWith(QListWidget* list, const QString& value, int role)
{
    for (int i = 0; i < list->count(); ++i)
        if (list->item(i)->data(role).toString() == value) return list->item(i);
    return nullptr;
}

// A file of `bytes` bytes changed at `when`
QString makeFile(const QString& path, int bytes, const QDateTime& when)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return {};
    f.write(QByteArray(bytes, 'x'));
    f.flush(); // else closing writes and changes the time again
    f.setFileTime(when, QFileDevice::FileModificationTime);
    f.close();
    return QFileInfo(path).absoluteFilePath();
}

void testMatcher()
{
    using namespace NameMatch;
    CHECK_EQ(normalize("Cs2_Clip-2026.11.12  Äpfel"), QString("cs2 clip 2026 11 12 apfel"));
    CHECK_EQ(normalize("__a__"), QString("a"));
    CHECK_EQ(normalize("Straße"), QString("strasse"));
    CHECK_EQ(queryWords("  cs2   11-12 "), (QStringList{"cs2", "11 12"}));
    CHECK(queryWords("  - ").isEmpty());

    const QString name = "cs2_clip_2026-11-12_21-30-34.mkv";
    CHECK(matches(queryWords("cs2 11-12"), name));
    CHECK(matches(queryWords("CS2 11_12"), name));
    CHECK(matches(queryWords("11.12 cs2"), name)); // any order
    CHECK(matches(queryWords("2026-11-12 21:30"), name));
    CHECK(!matches(queryWords("cs2 11-13"), name)); // every word
    CHECK(!matches(queryWords("cs3"), name));       // short words exactly
    CHECK(!matches(queryWords("2025"), name));      // digits exactly
    CHECK(matches(queryWords(""), name));            // no words: all
    // One typo in words of 4+ characters with a letter: replaced, missing, extra
    CHECK(!matches(queryWords("cilp"), "clip.mp4")); // two edits (swapped letters)
    CHECK(matches(queryWords("clpi"), "clip.mp4")); // one dropped: "cli" is in it
    CHECK(matches(queryWords("clips"), "my_clip.mp4"));      // one extra letter
    CHECK(matches(queryWords("clps"), "clips.mp4"));         // one missing
    CHECK(matches(queryWords("clipz"), "clips.mp4"));        // one replaced
    CHECK(matches(queryWords("holyday"), "Holiday 2026.mp4"));
    CHECK(!matches(queryWords("hollyday"), "Holiday 2026.mp4")); // one extra l and one replaced: two edits
    CHECK(!matches(queryWords("xyzw"), "clip.mp4"));
    CHECK(!matches(queryWords("a.mp4"), "b.mp4")); // a word with separators: exactly
    CHECK(matches(queryWords("A.MP4"), "a.mp4"));
    CHECK(matches(queryWords("urlaub"), "Ürlaub.mov"));     // accents do not count
    // Words may match different texts (name, color, …)
    CHECK(matches(queryWords("rot clip"), QStringList{"clip.mp4", "Rot"}));
    CHECK(!matches(queryWords("rot clip"), QStringList{"clip.mp4", "Blau"}));

    // Where: original positions (separators kept in the range between the parts of a word)
    CHECK_EQ(ranges(queryWords("cs2 11-12"), name), (QVector<Range>{{0, 3}, {14, 5}}));
    CHECK_EQ(ranges(queryWords("clip cs2_clip"), name), (QVector<Range>{{0, 8}})); // overlaps merged
    CHECK_EQ(ranges(queryWords("Äpfel"), "Große Äpfel.jpg"), (QVector<Range>{{6, 5}}));
    CHECK(ranges(queryWords("nothing"), name).isEmpty());
}

void testPoolWords()
{
    MediaInfo m{"/x/phone.mp4", "phone.mp4", 100, true, true};
    m.flags = {"red"};
    m.clipColor = "orange";
    const QStringList vertical = poolSearchWords(m, QSize(1080, 1920), 59.94);
    for (const char* w : {"1080p", "vertical", "9:16", "60 fps", "60fps", "59.94 fps", "video", "flag", "red", "orange"})
        CHECK(vertical.contains(w));
    CHECK(vertical.contains(T("hochkant")) && vertical.contains(T("Rot")));
    CHECK(NameMatch::matches(NameMatch::queryWords("phone 60 fps hochkant"), QStringList{m.name} + vertical));
    CHECK(!NameMatch::matches(NameMatch::queryWords("4k"), QStringList{m.name} + vertical));
    const QStringList uhd = poolSearchWords(m, QSize(3840, 2160), 25);
    CHECK(uhd.contains("4K") && uhd.contains("16:9") && uhd.contains("25 fps") && !uhd.contains("vertical"));
    CHECK(NameMatch::matches(NameMatch::queryWords("4k " + T("quer")), QStringList{m.name} + uhd)); // UI language
    // No format known: no format words
    const MediaInfo a{"/x/a.wav", "a.wav", 100, false, true};
    const QStringList audio = poolSearchWords(a);
    CHECK(audio.contains("audio") && !audio.contains("4K") && !audio.contains("vertical"));
}

QStringList names(const QStringList& paths)
{
    QStringList out;
    for (const QString& p : paths) out << QFileInfo(p).fileName();
    return out;
}

void testStorageSearch()
{
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return;
    const QString root = tmp.path() + "/root";
    const QDateTime base = QDateTime::currentDateTime().addDays(-10);
    const QString a = makeFile(root + "/cs2_clip_2026-11-12_21-30-34.mkv", 300, base.addSecs(30));
    const QString b = makeFile(root + "/cs2_clip_2026-11-13_20-00-00.mkv", 100, base.addSecs(10));
    const QString holiday = makeFile(root + "/Holiday trip.mp4", 500, base.addSecs(20));
    const QString music = makeFile(root + "/music.mp3", 50, base.addSecs(40));
    const QString pic = makeFile(root + "/pic.png", 20, base.addSecs(50));
    makeFile(root + "/notes.txt", 10, base); // no media file
    const QString sub = makeFile(root + "/sub/cs2_clip_2026-11-12_22-00-00.mp4", 10, base);
    const QString deeper = makeFile(root + "/sub/deeper/cs2_clip_2026-11-12_23-00-00.mkv", 10, base);
    makeFile(root + "/.hidden/cs2_clip_2026-11-12_00-00-00.mkv", 10, base); // hidden folder: skipped
    QDir().mkpath(root + "/cs2 folder");

    MediaStorage storage;
    storage.setPersistent(false);
    storage.resize(700, 400);
    storage.show();
    storage.setFolder(root);
    storage.setSort(MediaStorage::Sort::Name);
    auto* list = storage.findChild<QListWidget*>();
    auto* field = storage.findChild<QLineEdit*>("storage_search");
    auto* button = storage.findChild<QToolButton*>("storage_search_button");
    auto* status = storage.findChild<QLabel*>("storage_search_status");
    if (!CHECK(list && field && button && status)) return;
    CHECK(field->isHidden());
    CHECK_EQ(storage.shownFiles().size(), 5);

    // The magnifier opens the field; typing filters the folder at once
    button->click();
    CHECK(field->isVisible() && button->isChecked());
    field->setText("cs2 11-12");
    CHECK_EQ(storage.shownFiles(), QStringList{a});
    CHECK(!storage.searchRunning());
    QListWidgetItem* ia = itemWith(list, a, Qt::UserRole);
    if (CHECK(ia)) {
        CHECK(ia->data(SearchHitDelegate::kHighlightRole).toList().size() == 2);
        CHECK(ia->data(SearchHitDelegate::kSubtitleRole).toString().isEmpty()); // in this folder: no folder line
    }
    CHECK_EQ(status->text(), T("%1 Treffer").arg(1));
    field->setText("cs2");
    CHECK_EQ(storage.shownFiles(), (QStringList{a, b}));
    dumpPicture(storage, "storage-filter-icons");
    CHECK(itemWith(list, root + "/cs2 folder", Qt::UserRole)); // folders by name too
    CHECK(!itemWith(list, root + "/sub", Qt::UserRole));
    field->setText("holyday"); // one typo
    CHECK_EQ(storage.shownFiles(), QStringList{holiday});
    field->setText("nothing like it");
    CHECK(storage.shownFiles().isEmpty());
    CHECK_EQ(status->text(), T("Nichts gefunden"));

    // With subfolders: in the background, each hit with its folder; hidden folders skipped
    field->setText("cs2 11-12");
    storage.setSearchSubfolders(true);
    CHECK(waitFor([&] { return !storage.searchRunning(); }));
    QStringList deep = storage.shownFiles();
    deep.sort();
    QStringList want{a, sub, deeper};
    want.sort();
    CHECK_EQ(deep, want);
    if (QListWidgetItem* is = itemWith(list, sub, Qt::UserRole); CHECK(is))
        CHECK_EQ(is->data(SearchHitDelegate::kSubtitleRole).toString(), QDir::toNativeSeparators("root/sub"));
    if (QListWidgetItem* is = itemWith(list, a, Qt::UserRole); CHECK(is))
        CHECK_EQ(is->data(SearchHitDelegate::kSubtitleRole).toString(), QString("root"));
    CHECK(!itemWith(list, root + "/cs2 folder", Qt::UserRole)); // only files below
    dumpPicture(storage, "storage-deep-icons");
    list->setCurrentRow(1);
    for (QToolButton* b : storage.findChildren<QToolButton*>())
        if (b->text() == "☰") b->click();
    dumpPicture(storage, "storage-deep-list");
    for (QToolButton* b : storage.findChildren<QToolButton*>())
        if (b->text() == "▦") b->click();
    // Double-click and drag work like on a normal entry
    QStringList imported;
    QObject::connect(&storage, &MediaStorage::importRequested, [&](const QStringList& p) { imported = p; });
    if (QListWidgetItem* id = itemWith(list, deeper, Qt::UserRole); CHECK(id)) {
        emit list->itemDoubleClicked(id);
        CHECK_EQ(imported, QStringList{deeper});
        const QMimeData* mime = list->model()->mimeData({list->indexFromItem(id)});
        if (CHECK(mime)) CHECK_EQ(mime->urls().value(0).toLocalFile(), deeper);
        delete mime;
    }
    // Sort applies to the hits too: newest first
    storage.setSort(MediaStorage::Sort::Newest);
    CHECK_EQ(storage.shownFiles().value(0), a);
    storage.setSort(MediaStorage::Sort::Name);
    // Kind filter while searching below
    storage.setKindFilter(MediaStorage::Kind::Video);
    CHECK(waitFor([&] { return !storage.searchRunning(); }));
    CHECK_EQ(storage.shownFiles().size(), 3); // mkv and mp4 are both videos
    storage.setKindFilter(MediaStorage::Kind::Audio);
    CHECK(waitFor([&] { return !storage.searchRunning(); }));
    CHECK(storage.shownFiles().isEmpty());
    storage.setKindFilter(MediaStorage::Kind::All);
    storage.setSearchSubfolders(false);
    CHECK_EQ(storage.shownFiles(), QStringList{a});

    // Kind filter without words (search open): the folder by kind
    field->clear();
    CHECK(button->isChecked());
    CHECK_EQ(storage.shownFiles().size(), 5);
    storage.setKindFilter(MediaStorage::Kind::Audio);
    CHECK_EQ(storage.shownFiles(), QStringList{music});
    storage.setKindFilter(MediaStorage::Kind::Image);
    CHECK_EQ(storage.shownFiles(), QStringList{pic});
    storage.setKindFilter(MediaStorage::Kind::Video);
    CHECK_EQ(names(storage.shownFiles()), (QStringList{"cs2_clip_2026-11-12_21-30-34.mkv", "cs2_clip_2026-11-13_20-00-00.mkv",
                                                       "Holiday trip.mp4"}));

    // Esc in the field closes the search: everything again, kind filter off
    field->setText("cs2");
    field->setFocus();
    QKeyEvent over(QEvent::ShortcutOverride, Qt::Key_Escape, Qt::NoModifier);
    over.ignore();
    QApplication::sendEvent(field, &over);
    CHECK(over.isAccepted()); // no window key with Esc fires meanwhile
    QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(field, &esc);
    CHECK(!button->isChecked() && field->isHidden() && field->text().isEmpty());
    CHECK(storage.kindFilter() == MediaStorage::Kind::All);
    CHECK_EQ(storage.shownFiles().size(), 5);

    // Sort orders (folders stay on top by name)
    storage.setSort(MediaStorage::Sort::Name);
    CHECK_EQ(names(storage.shownFiles()), (QStringList{"cs2_clip_2026-11-12_21-30-34.mkv", "cs2_clip_2026-11-13_20-00-00.mkv",
                                                       "Holiday trip.mp4", "music.mp3", "pic.png"}));
    storage.setSort(MediaStorage::Sort::Newest);
    CHECK_EQ(storage.shownFiles(), (QStringList{pic, music, a, holiday, b}));
    storage.setSort(MediaStorage::Sort::Oldest);
    CHECK_EQ(storage.shownFiles(), (QStringList{b, holiday, a, music, pic}));
    storage.setSort(MediaStorage::Sort::Size);
    CHECK_EQ(storage.shownFiles(), (QStringList{holiday, a, b, music, pic}));
    CHECK_EQ(list->item(0)->data(Qt::UserRole).toString(), root + "/cs2 folder");
}

// Many folders: the search below runs in the background (typing never waits for it), a new text or closing cancels
// it (nothing of the old one arrives later), and it stops at kMaxDeepHits
void testStorageDeepCancel()
{
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return;
    const QDateTime when = QDateTime::currentDateTime().addDays(-1);
    for (int d = 0; d < 150; ++d)
        for (int f = 0; f < 5; ++f)
            makeFile(tmp.filePath(QString("d%1/x%2/take_%1_%2.mp4").arg(d).arg(f)), 1, when);
    MediaStorage storage;
    storage.setPersistent(false);
    storage.resize(700, 400);
    storage.show();
    storage.setFolder(tmp.path());
    storage.setSearchSubfolders(true);
    QElapsedTimer t;
    t.start();
    storage.setSearch("take");
    QApplication::processEvents();
    std::printf("Media Storage: search below started in %lld ms\n", t.elapsed());
    CHECK(t.elapsed() < 500);
    // Cancel: another text at once, then closing
    storage.setSearch("take_7_");
    storage.setSearch(QString());
    CHECK(!storage.searchRunning());
    settle(400);
    CHECK(!storage.searchRunning());
    CHECK_EQ(storage.shownFiles().size(), 0); // closed: the (empty) top folder, nothing of the old search
    // Cap
    storage.setSearch("take");
    CHECK(waitFor([&] { return !storage.searchRunning(); }));
    CHECK(storage.searchCapped());
    CHECK_EQ(storage.shownFiles().size(), MediaStorage::kMaxDeepHits);
    // A narrower one finds all
    storage.setSearch("take 42");
    CHECK(waitFor([&] { return !storage.searchRunning(); }));
    CHECK(!storage.searchCapped());
    CHECK_EQ(storage.shownFiles().size(), 5 + 5); // take_42_* and take_142_* (digits match exactly, as a part)
}

void testStorageSortRemembered()
{
    {
        MediaStorage s; // persistent (test settings are isolated)
        s.setSort(MediaStorage::Sort::Size);
    }
    MediaStorage again;
    CHECK(again.sort() == MediaStorage::Sort::Size);
    again.setSort(MediaStorage::Sort::Name);
}

void testPoolScope(const QString& dir)
{
    Project p;
    const int unter = p.addBin(0, "Unter");
    const int andere = p.addBin(0, "Andere");
    MediaInfo a{"/x/cs2_clip_2026-11-12.mkv", "cs2_clip_2026-11-12.mkv", 100, true, true};
    a.flags = {"red"};
    a.clipColor = "orange";
    MediaInfo b{"/x/cs2_clip_2026-11-13.mkv", "cs2_clip_2026-11-13.mkv", 100, true, true};
    b.bin = andere;
    MediaInfo h{"/x/holiday_trip.mp4", "holiday_trip.mp4", 100, true, true};
    h.bin = unter;
    for (const MediaInfo& m : {a, b, h}) p.addMedia(m);
    QString phone;
    if (Check::haveFfmpeg())
        phone = Check::makeMedia(dir + "/phone.mp4", {"-f", "lavfi", "-i", "testsrc2=size=180x320:rate=60:duration=1",
                                                      "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p"});
    if (!phone.isEmpty()) {
        MediaInfo m{phone, "phone.mp4", 60, true, false};
        m.bin = unter;
        p.addMedia(m);
    }

    Engine engine;
    MediaPool pool(&p, &engine);
    pool.setSaveSettings(false);
    pool.resize(700, 400);
    pool.show();
    QApplication::processEvents();
    auto* list = pool.findChild<QListWidget*>();
    auto* scope = pool.findChild<QToolButton*>("pool_search_scope");
    auto* hint = pool.findChild<QLabel*>("pool_empty_hint");
    if (!CHECK(list && scope && hint)) return;
    CHECK(scope->isHidden());
    pool.setCurrentBin(unter);
    auto mediaShown = [&] {
        QStringList out;
        for (int i = 0; i < list->count(); ++i)
            if (p.mediaInfo(list->item(i)->data(Qt::UserRole).toString())) out << list->item(i)->data(Qt::UserRole).toString();
        out.sort();
        return out;
    };

    // The whole project: hits of other bins say their bin
    pool.setSearch("cs2");
    CHECK(scope->isVisible() && !scope->isChecked());
    CHECK_EQ(mediaShown(), (QStringList{a.path, b.path}));
    if (QListWidgetItem* ib = itemWith(list, b.path, Qt::UserRole); CHECK(ib)) {
        CHECK_EQ(ib->data(SearchHitDelegate::kSubtitleRole).toString(), QString("Andere"));
        CHECK_EQ(ib->data(SearchHitDelegate::kHighlightRole).toList().size(), 1);
    }
    if (QListWidgetItem* ia = itemWith(list, a.path, Qt::UserRole); CHECK(ia))
        CHECK_EQ(ia->data(SearchHitDelegate::kSubtitleRole).toString(), p.binName(0));
    dumpPicture(pool, "pool-search-icons");
    pool.setViewMode(MediaPool::ViewMode::List);
    dumpPicture(pool, "pool-search-list");
    pool.setViewMode(MediaPool::ViewMode::Thumbnails);
    // Several words, separators alike, a typo
    pool.setSearch("cs2 11-12");
    CHECK_EQ(mediaShown(), QStringList{a.path});
    pool.setSearch("holyday");
    CHECK_EQ(mediaShown(), QStringList{h.path});
    if (QListWidgetItem* ih = itemWith(list, h.path, Qt::UserRole); CHECK(ih)) // its own bin: no bin line
        CHECK(ih->data(SearchHitDelegate::kSubtitleRole).toString().isEmpty());
    // Flag and clip color names
    pool.setSearch(T("Rot"));
    CHECK_EQ(mediaShown(), QStringList{a.path});
    pool.setSearch("orange cs2");
    CHECK_EQ(mediaShown(), QStringList{a.path});
    // Only this bin (and its sub-bins)
    pool.setSearch("cs2");
    pool.setSearchThisBinOnly(true);
    CHECK(mediaShown().isEmpty());
    CHECK(hint->isVisible());
    pool.setSearch("trip");
    CHECK_EQ(mediaShown(), QStringList{h.path});
    pool.setSearchThisBinOnly(false);
    // Picture format words, once the format is read (in the background)
    if (!phone.isEmpty()) {
        CHECK_EQ(MediaProbe::pictureFormat(phone).size, QSize(180, 320));
        pool.setSearch(T("hochkant") + " 60 fps");
        CHECK(waitFor([&] { return mediaShown() == QStringList{phone}; }, 10000));
        pool.setSearch("vertical");
        CHECK_EQ(mediaShown(), QStringList{phone});
        pool.setSearch("4k");
        CHECK(mediaShown().isEmpty());
    }
    // Closing the search: the open bin as before, the toggle hidden
    pool.setSearch(QString());
    CHECK(scope->isHidden());
    QStringList own{h.path};
    if (!phone.isEmpty()) own << phone;
    own.sort();
    CHECK_EQ(mediaShown(), own);
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("media_search");
    g_dump = qEnvironmentVariable("MEDIA_SEARCH_DUMP");
    if (!g_dump.isEmpty()) {
        Theme::load();
        Theme::apply(app);
    }
    testMatcher();
    testPoolWords();
    testStorageSearch();
    testStorageDeepCancel();
    testStorageSortRemembered();
    QTemporaryDir tmp;
    if (CHECK(tmp.isValid())) testPoolScope(tmp.path());
    return Check::result();
}
