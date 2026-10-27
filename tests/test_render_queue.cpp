// Test Deliver: render settings/presets (built-in + own), render queue (project file, DeliverPanel,
// render one after another with status, error, cancel). Rendering is small and short (ffmpeg/ffprobe needed).
#include "check.h"

#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/RenderJob.h"
#include "core/TimelineOps.h"
#include "engine/Exporter.h"
#include "engine/HardwareEncoders.h"
#include "engine/RenderQueue.h"
#include "ui/DeliverPanel.h"
#include "ui/RenderQueuePanel.h"

#include <Mlt.h>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUndoStack>
#include <QUrl>
#include <clocale>
#include <functional>

namespace {

const RenderJob* job(const Project& p, int id)
{
    for (const RenderJob& j : p.renderQueue())
        if (j.id == id) return &j;
    return nullptr;
}

QVector<int> ids(const QVector<RenderJob>& jobs)
{
    QVector<int> out;
    for (const RenderJob& j : jobs) out << j.id;
    return out;
}

// ffprobe: "codec_type=video|codec_name=h264|width=..." per stream + duration
QJsonObject probe(const QString& path)
{
    QProcess p;
    p.start("ffprobe", {"-v", "error", "-show_entries", "stream=codec_type,codec_name,width,height:format=duration",
                        "-of", "json", path});
    if (!p.waitForFinished(60000)) return {};
    return QJsonDocument::fromJson(p.readAllStandardOutput()).object();
}

QJsonObject stream(const QJsonObject& probe, const QString& type)
{
    for (const QJsonValue& v : probe.value("streams").toArray())
        if (v.toObject().value("codec_type").toString() == type) return v.toObject();
    return {};
}

bool waitFor(RenderQueue& q, int ms = 120000)
{
    if (!q.isRunning()) return true;
    QEventLoop loop;
    QObject::connect(&q, &RenderQueue::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
    return !q.isRunning();
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("render-queue");
    QFile::remove(RenderPresets::userFile()); // test settings from the last run

    // --- Settings: output size
    {
        const QSize hd(1920, 1080), portrait(1080, 1920);
        RenderSettings s;
        CHECK_EQ(s.outputSize(hd), hd);
        s.shortSide = 2160;
        CHECK_EQ(s.outputSize(hd), QSize(3840, 2160));
        s.shortSide = 720;
        CHECK_EQ(s.outputSize(portrait), QSize(720, 1280));
        s.size = QSize(1080, 1920); // fixed takes precedence
        CHECK_EQ(s.outputSize(hd), QSize(1080, 1920));
        s.format = "wav";
        CHECK(s.audioOnly() && s.outputSize(hd).isEmpty());
        CHECK_EQ(renderFormat("gibtsnicht").id, QString("h264").toLatin1());
        // JSON there and back
        RenderSettings a;
        a.format = "prores";
        a.shortSide = 1080;
        a.quality = 2;
        a.audioBitrateK = 192;
        CHECK(RenderSettings::fromJson(a.toJson()) == a);
        a.size = QSize(1080, 1920);
        CHECK(RenderSettings::fromJson(a.toJson()) == a);
    }

    // --- Smart render at "High" and "Web" (the writer copies below the cap only): copied pictures keep the file's bit
    // rate (a game recording: 4x the file), "Medium"/"Small" encode everything
    {
        RenderJob job;
        job.path = "/tmp/out.mp4";
        job.settings.format = "h264";
        job.settings.encoder = VideoEncoder::Software;
        const ProjectFormat fmt;
        for (int q = 0; q <= RenderSettings::kQualityWeb; ++q) {
            job.settings.quality = q;
            CHECK_EQ(RenderQueue::exportSettings(job, fmt).smartRender, q == 0 || q == RenderSettings::kQualityWeb);
        }
        job.settings.quality = 0;
        job.settings.smartRender = false;
        CHECK(!RenderQueue::exportSettings(job, fmt).smartRender);
    }

    // --- Built-in presets
    {
        const auto b = RenderPresets::builtins();
        QStringList formats;
        for (const RenderPreset& p : b) {
            CHECK(p.builtin);
            formats << p.settings.format;
        }
        CHECK(formats.contains("h264") && formats.contains("h265") && formats.contains("prores")
              && formats.contains("wav") && formats.contains("aac"));
        CHECK_EQ(b[0].name, QString("YouTube 1080p"));
        // YouTube: high quality with a bit rate cap (1080p60 ~15.6 Mbit/s, 1080p30 ~10, 4K60 ~62), not 35 Mbit/s
        CHECK_EQ(b[0].settings.quality, RenderSettings::kQualityWeb);
        CHECK_EQ(b[0].settings.crf(), 18);
        CHECK(std::abs(b[0].settings.maxBitrateK({1920, 1080}, {60, 1}) - 15552) < 10);
        CHECK(std::abs(b[0].settings.maxBitrateK({1920, 1080}, {30, 1}) - 10264) < 10);
        CHECK(std::abs(b[1].settings.maxBitrateK({3840, 2160}, {60, 1}) - 62208) < 20);
        RenderSettings high = b[0].settings;
        high.quality = 0;
        CHECK_EQ(high.maxBitrateK({1920, 1080}, {60, 1}), 0); // other qualities: no cap
        CHECK(RenderSettings::fromJson(b[0].settings.toJson()) == b[0].settings);
        CHECK_EQ(b[1].settings.outputSize({1920, 1080}), QSize(3840, 2160));
        CHECK_EQ(b[2].settings.outputSize({1920, 1080}), QSize(1080, 1920)); // portrait 9:16
    }

    // --- Queue in the project file
    {
        ProjectData d;
        TimelineOps::ensureTracks(d.timeline, TrackKind::Audio, 1);
        RenderJob j1;
        j1.id = 1;
        j1.path = "/x/out/a.mp4";
        j1.preset = "YouTube 1080p";
        j1.settings.shortSide = 1080;
        j1.size = QSize(1920, 1080);
        j1.status = RenderStatus::Done;
        j1.renderMs = 12345;
        RenderJob j2;
        j2.id = 2;
        j2.path = "/x/out/b.wav";
        j2.settings.format = "wav";
        j2.inOut = true;
        j2.from = 10;
        j2.to = 50;
        j2.status = RenderStatus::Failed;
        j2.message = "kaputt";
        RenderJob j3 = j1;
        j3.id = 3;
        j3.status = RenderStatus::Rendering; // program quit during rendering
        d.renderQueue = {j1, j2, j3};
        ProjectData back;
        QString err;
        const QByteArray json = ProjectFile::toJson(d, "/x/p.schneidi");
        CHECK(ProjectFile::fromJson(json, "/x/p.schneidi", &back, &err));
        CHECK_EQ(back.renderQueue.size(), 3);
        if (back.renderQueue.size() == 3) {
            CHECK(back.renderQueue[0] == j1);
            CHECK(back.renderQueue[1] == j2);
            CHECK(back.renderQueue[2].status == RenderStatus::Queued);
        }
        // Save -> load -> save identical
        CHECK_EQ(ProjectFile::toJson(back, "/x/p.schneidi"),
                 ProjectFile::toJson(ProjectData{back.format, back.playhead, back.media, back.bins, back.timeline,
                                                 back.lastClipId, back.lastLinkId, back.renderQueue},
                                     "/x/p.schneidi"));
        // Empty queue: no entry (files stay as before); old file without entry
        d.renderQueue.clear();
        CHECK(!ProjectFile::toJson(d, "/x/p.schneidi").contains("renderQueue"));
        CHECK(ProjectFile::fromJson(R"({"app":"schneidi","version":1,"media":[],"timeline":{"video":[],"audio":[]}})",
                                    "/x/p.schneidi", &back, &err));
        CHECK(back.renderQueue.isEmpty());
        // Duplicate/missing ids are reassigned, jobs without a path discarded
        const auto fixed = RenderQueueJson::fromJson(QJsonDocument::fromJson(
            R"([{"id":1,"path":"/a.mp4"},{"id":1,"path":"/b.mp4"},{"path":"/c.mp4"},{"id":4}])").array());
        CHECK_EQ(fixed.size(), 3);
        if (fixed.size() == 3) CHECK(fixed[0].id == 1 && fixed[1].id == 2 && fixed[2].id == 3);
        // Numbers no program writes (found by fuzzing, tests/fuzz): id 2^31 - 1 (the next one overflowed), 10^60 ms
        const auto huge = RenderQueueJson::fromJson(QJsonDocument::fromJson(
            R"([{"id":2147483647,"path":"/a.mp4"},{"path":"/b.mp4","renderMs":1e60}])").array());
        if (CHECK(huge.size() == 2)) CHECK(huge[0].id == 1 << 30 && huge[1].id == (1 << 30) + 1 && huge[1].renderMs > 0);

        // Project: change counts as a change, but without an undo step (like DaVinci)
        Project p;
        const int undo = p.undoStack()->count();
        CHECK(!p.isModified());
        p.setRenderQueue({j1});
        CHECK(p.isModified());
        CHECK_EQ(p.undoStack()->count(), undo);
        CHECK_EQ(p.data().renderQueue.size(), 1);
        p.reset();
        CHECK(p.renderQueue().isEmpty());
    }

    // --- Order (dragging jobs): the moved jobs keep their order, the job being rendered stays, saved with the project
    {
        QVector<RenderJob> q;
        for (int i = 1; i <= 5; ++i) {
            RenderJob j;
            j.id = i;
            j.path = QString("/x/%1.mp4").arg(i);
            q << j;
        }
        CHECK_EQ(ids(moveRenderJobs(q, {4}, 2)), (QVector<int>{1, 4, 2, 3, 5}));    // up
        CHECK_EQ(ids(moveRenderJobs(q, {1}, 4)), (QVector<int>{2, 3, 1, 4, 5}));    // down
        CHECK_EQ(ids(moveRenderJobs(q, {2}, 0)), (QVector<int>{1, 3, 4, 5, 2}));    // to the end
        CHECK_EQ(ids(moveRenderJobs(q, {5, 2}, 1)), (QVector<int>{2, 5, 1, 3, 4})); // several: in queue order
        CHECK_EQ(ids(moveRenderJobs(q, {2, 3}, 3)), (QVector<int>{1, 2, 3, 4, 5})); // onto itself: nothing changes
        CHECK_EQ(ids(moveRenderJobs(q, {3, 5}, 1, 3)), (QVector<int>{5, 1, 2, 3, 4})); // 3 is rendering: stays
        CHECK_EQ(ids(moveRenderJobs(q, {3}, 1, 3)), (QVector<int>{1, 2, 3, 4, 5}));
        CHECK_EQ(ids(moveRenderJobs(q, {9}, 1)), (QVector<int>{1, 2, 3, 4, 5}));    // unknown job
        CHECK_EQ(ids(moveRenderJobs(q, {1}, 9)), (QVector<int>{2, 3, 4, 5, 1}));    // unknown target: the end

        Project p;
        RenderQueue queue(&p);
        RenderQueuePanel panel(&p, &queue);
        p.setRenderQueue(q);
        p.markSaved();
        panel.moveJobs({4, 5}, 1);
        CHECK_EQ(ids(p.renderQueue()), (QVector<int>{4, 5, 1, 2, 3}));
        CHECK(p.isModified());
        auto* list = panel.findChild<QListWidget*>();
        if (!CHECK(list && list->count() == 5)) return Check::result();
        CHECK_EQ(list->item(0)->data(Qt::UserRole + 1).toInt(), 4); // the cards in the new order
        ProjectData back;
        QString err;
        CHECK(ProjectFile::fromJson(ProjectFile::toJson(p.data(), "/x/p.schneidi"), "/x/p.schneidi", &back, &err));
        CHECK_EQ(ids(back.renderQueue), (QVector<int>{4, 5, 1, 2, 3}));

        // Del in the list deletes the selected jobs: the panel's key (deleteAction, bindable as "render_queue_delete")
        // takes the key before the window's shortcuts (the timeline's Del must not delete clips meanwhile)
        const auto press = [&](int key, Qt::KeyboardModifiers mods = Qt::NoModifier) {
            QKeyEvent over(QEvent::ShortcutOverride, key, mods);
            over.ignore(); // as Qt sends it
            QApplication::sendEvent(list, &over);
            QKeyEvent ev(QEvent::KeyPress, key, mods);
            QApplication::sendEvent(list, &ev);
            return over.isAccepted();
        };
        const auto select = [&](int id) {
            list->clearSelection();
            for (int i = 0; i < list->count(); ++i)
                if (list->item(i)->data(Qt::UserRole + 1).toInt() == id) list->item(i)->setSelected(true);
        };
        select(5);
        CHECK(press(Qt::Key_Delete));
        CHECK_EQ(ids(p.renderQueue()), (QVector<int>{4, 1, 2, 3}));
        select(1);
        CHECK(press(Qt::Key_Backspace));
        CHECK_EQ(ids(p.renderQueue()), (QVector<int>{4, 2, 3}));
        panel.deleteAction()->setShortcuts({QKeySequence("Ctrl+D")}); // bound another key (InputBindings does this)
        select(2);
        CHECK(!press(Qt::Key_Delete)); // Del is the window's again
        CHECK_EQ(ids(p.renderQueue()), (QVector<int>{4, 2, 3}));
        CHECK(press(Qt::Key_D, Qt::ControlModifier));
        CHECK_EQ(ids(p.renderQueue()), (QVector<int>{4, 3}));
        CHECK(!press(Qt::Key_A)); // other keys stay the list's
    }

    // --- Dropping dragged jobs (JobList::dropEvent): in front of the card under the mouse (its upper half), behind it
    // (lower half), between two cards in front of the lower one, below the last one at the end; a drop that did not
    // start in the list (files, another list) moves nothing. QDrag cannot run offscreen: synthetic drag/drop events,
    // the drop's data names the list as the drag's source (as a real drag from it is).
    {
        QVector<RenderJob> q;
        for (int i = 1; i <= 5; ++i) {
            RenderJob j;
            j.id = i;
            j.path = QString("/x/%1.mp4").arg(i);
            q << j;
        }
        Project p;
        RenderQueue queue(&p);
        RenderQueuePanel panel(&p, &queue);
        p.setRenderQueue(q);
        panel.resize(340, 900);
        panel.show();
        QApplication::processEvents();
        auto* list = panel.findChild<QListWidget*>();
        if (!CHECK(list && list->isVisible() && list->count() == 5)) return Check::result();
        const auto card = [&](int id) {
            for (int i = 0; i < list->count(); ++i)
                if (list->item(i)->data(Qt::UserRole + 1).toInt() == id) return list->item(i);
            return static_cast<QListWidgetItem*>(nullptr);
        };
        const auto select = [&](const QVector<int>& selected) {
            list->clearSelection();
            for (int id : selected) card(id)->setSelected(true);
        };
        const auto drop = [&](const QPoint& pos, bool fromList) {
            QMimeData data;
            data.setData("application/x-qabstractitemmodeldatalist", {});
            if (fromList) data.setProperty("schneidi_drag_source", QVariant::fromValue<QObject*>(list));
            // As Qt delivers a drag: the widget that takes the enter gets the moves and the drop
            QDragEnterEvent enter(pos, Qt::MoveAction, &data, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(list->viewport(), &enter);
            QDragMoveEvent move(pos, Qt::MoveAction, &data, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(list->viewport(), &move);
            QDropEvent e(QPointF(pos), Qt::MoveAction, &data, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(list->viewport(), &e);
            QApplication::processEvents(); // the move comes after the drag has ended
            return e.isAccepted();
        };
        const auto rect = [&](int id) { return list->visualItemRect(card(id)); };
        select({4});
        CHECK(drop(QPoint(rect(2).center().x(), rect(2).top() + 3), true)); // upper half of 2: in front of it
        CHECK_EQ(ids(p.renderQueue()), (QVector<int>{1, 4, 2, 3, 5}));
        select({1});
        CHECK(drop(QPoint(rect(3).center().x(), rect(3).bottom() - 3), true)); // lower half of 3: behind it
        CHECK_EQ(ids(p.renderQueue()), (QVector<int>{4, 2, 3, 1, 5}));
        select({5, 4});
        const QRect last = rect(ids(p.renderQueue()).last());
        CHECK(drop(QPoint(last.center().x(), last.bottom() + 20), true)); // below the last card: at the end
        CHECK_EQ(ids(p.renderQueue()), (QVector<int>{2, 3, 1, 4, 5}));
        select({5});
        const QPoint gap(rect(2).center().x(), (rect(2).bottom() + rect(3).top()) / 2 + 1); // the spacing between
        if (!list->itemAt(gap)) {
            CHECK(drop(gap, true));
            CHECK_EQ(ids(p.renderQueue()), (QVector<int>{2, 5, 3, 1, 4}));
        } else {
            std::printf("no gap between the cards at this size: spacing drop not tested\n");
        }
        // Not from the list: nothing moves, the drop is not taken
        const QVector<int> before = ids(p.renderQueue());
        select({before.last()});
        CHECK(!drop(QPoint(rect(before.first()).center().x(), rect(before.first()).top() + 3), false));
        CHECK_EQ(ids(p.renderQueue()), before);
        QMimeData files;
        files.setUrls({QUrl::fromLocalFile("/x/clip.mp4")});
        QDragEnterEvent foreign(rect(before.first()).center(), Qt::CopyAction | Qt::MoveAction, &files, Qt::LeftButton,
                                Qt::NoModifier);
        QApplication::sendEvent(list->viewport(), &foreign);
        CHECK(!foreign.isAccepted()); // files are not taken
    }

    // --- File name from the timeline's name: valid on Windows, macOS and Linux
    {
        CHECK_EQ(renderFileBaseName("Timeline 1"), QString("Timeline 1"));
        CHECK_EQ(renderFileBaseName("Intro: Teil 1/2"), QString("Intro - Teil 1-2"));
        CHECK_EQ(renderFileBaseName("Mein Video: Teil 2?"), QString("Mein Video - Teil 2")); // no "Teil 2_"
        CHECK_EQ(renderFileBaseName("Shorts 9:16"), QString("Shorts 9-16"));
        CHECK_EQ(renderFileBaseName("a<b>c\"d\\e|f?g*h"), QString("a_b_c'd-e_f_g_h"));
        CHECK_EQ(renderFileBaseName("Tab\there\n"), QString("Tab here"));
        CHECK_EQ(renderFileBaseName("  Ende. . "), QString("Ende"));
        CHECK_EQ(renderFileBaseName(".versteckt"), QString("versteckt"));
        CHECK_EQ(renderFileBaseName("Was?"), QString("Was"));
        CHECK_EQ(renderFileBaseName(" ... "), QString("Export"));
        CHECK_EQ(renderFileBaseName(""), QString("Export"));
        CHECK_EQ(renderFileBaseName("con"), QString("con_"));
        CHECK_EQ(renderFileBaseName("NUL.final"), QString("NUL_.final"));
        CHECK_EQ(renderFileBaseName("COM3"), QString("COM3_"));
        CHECK_EQ(renderFileBaseName("lpt9"), QString("lpt9_"));
        CHECK_EQ(renderFileBaseName("COM0"), QString("COM0"));
        CHECK_EQ(renderFileBaseName("Console"), QString("Console"));
        CHECK_EQ(renderFileBaseName("Größe – Ünïcödé 🎬"), QString("Größe – Ünïcödé 🎬"));
        CHECK_EQ(renderFileBaseName(QString(300, 'x')).size(), 200);
    }

    // --- DeliverPanel: presets, own presets, create jobs
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    {
        Project p;
        DeliverPanel panel(&p);
        auto* name = panel.findChild<QLineEdit*>("renderName");
        auto* folder = panel.findChild<QLineEdit*>("renderFolder");
        if (!CHECK(name && folder)) return Check::result();
        name->setText("Film");
        folder->setText(tmp.path());
        CHECK_EQ(panel.currentPreset(), 0); // YouTube 1080p
        RenderJob j = panel.makeJob();
        CHECK_EQ(j.size, QSize(1920, 1080));
        CHECK_EQ(j.path, QDir(tmp.path()).filePath("Film.mp4"));
        CHECK_EQ(j.preset, QString("YouTube 1080p"));
        CHECK(!j.inOut);
        // Portrait, audio only, ProRes
        const auto& presets = panel.presets();
        for (int i = 0; i < presets.size(); ++i) {
            panel.selectPreset(i);
            CHECK_EQ(panel.currentPreset(), i);
            CHECK(panel.settings().format == presets[i].settings.format);
            j = panel.makeJob();
            CHECK(j.path.endsWith(QString(".") + renderFormat(presets[i].settings.format).extension));
            if (presets[i].settings.audioOnly()) CHECK(j.size.isEmpty());
            else CHECK_EQ(j.size, presets[i].settings.outputSize({1920, 1080}));
        }
        // Change setting -> "Own settings"
        const RenderSettings youtube = presets[0].settings;
        panel.setSettings(RenderSettings{}); // H.264, quality high: no preset (YouTube caps the bit rate)
        CHECK_EQ(panel.currentPreset(), -1);
        auto* format = panel.findChild<QComboBox*>("renderFormat");
        int formats = 0; // entries without the separators between the groups
        for (int i = 0; format && i < format->count(); ++i) formats += format->itemData(i).isValid() ? 1 : 0;
        if (CHECK(format && formats == renderFormats().size())) {
            format->setCurrentIndex(1); // H.265 at timeline resolution = built-in H.265 preset
            CHECK(panel.currentPreset() >= 0 && presets[panel.currentPreset()].settings.format == "h265");
            RenderSettings custom;
            custom.quality = 1; // H.264 1080p Medium: no preset
            panel.setSettings(custom);
            CHECK_EQ(panel.currentPreset(), -1);
            format->setCurrentIndex(1);
            format->setCurrentIndex(0);
            panel.setSettings(youtube);
            CHECK_EQ(panel.currentPreset(), 0);
            // Encoder: built-in presets choose it automatically; another one = own settings, kept across formats
            auto* encoder = panel.findChild<QComboBox*>("renderEncoder");
            if (CHECK(encoder)) {
                CHECK(panel.settings().encoder == VideoEncoder::Auto);
                RenderSettings sw = youtube;
                sw.encoder = VideoEncoder::Software;
                panel.setSettings(sw);
                CHECK(panel.settings().encoder == VideoEncoder::Software);
                CHECK_EQ(panel.currentPreset(), -1);
                RenderSettings nv = youtube; // stays selected even when it does not work here (renders with software then)
                nv.encoder = VideoEncoder::Nvenc;
                panel.setSettings(nv);
                CHECK(panel.settings().encoder == VideoEncoder::Nvenc);
                CHECK(panel.makeJob().settings.encoder == VideoEncoder::Nvenc);
                format->setCurrentIndex(3); // AAC: no video encoder
                CHECK(!encoder->isEnabled());
                format->setCurrentIndex(1); // H.265
                CHECK(encoder->isEnabled());
                CHECK(panel.settings().encoder == VideoEncoder::Nvenc);
                panel.setSettings(youtube);
                CHECK_EQ(panel.currentPreset(), 0);
                // Picked in the list (as the user does): stays, was put back to the old choice before
                encoder->setCurrentIndex(encoder->findData(int(VideoEncoder::Software)));
                CHECK(VideoEncoder(encoder->currentData().toInt()) == VideoEncoder::Software);
                CHECK(panel.settings().encoder == VideoEncoder::Software);
                CHECK(panel.makeJob().settings.encoder == VideoEncoder::Software);
                panel.setSettings(youtube);
            }
        }
        // Save own preset, find again, overwrite, delete; built-in ones stay
        RenderSettings mine;
        mine.format = "h265";
        mine.shortSide = 720;
        mine.quality = 1;
        panel.setSettings(mine);
        CHECK(panel.savePreset("Mein Export"));
        CHECK(!panel.savePreset("YouTube 1080p"));
        CHECK(!panel.savePreset("  "));
        const int count = panel.presets().size();
        CHECK_EQ(count, RenderPresets::builtins().size() + 1);
        CHECK_EQ(panel.currentPreset(), count - 1);
        CHECK_EQ(RenderPresets::loadUser().size(), 1);
        CHECK(RenderPresets::loadUser().value(0).settings == mine);
        {
            DeliverPanel other(&p); // new instance reads the file
            CHECK_EQ(other.presets().size(), count);
            CHECK_EQ(other.presets().last().name, QString("Mein Export"));
        }
        // Its queue display dies too (otherwise it hung on the project with a deleted RenderQueue -> crash)
        const QWidgetList widgets = QApplication::allWidgets();
        CHECK_EQ(std::count_if(widgets.begin(), widgets.end(), [](QWidget* w) { return qobject_cast<RenderQueuePanel*>(w); }),
                 1);
        mine.quality = 2;
        panel.setSettings(mine);
        CHECK(panel.savePreset("Mein Export"));
        CHECK_EQ(panel.presets().size(), count);
        CHECK_EQ(RenderPresets::loadUser().value(0).settings.quality, 2);
        CHECK(!panel.deletePreset(0)); // built-in
        CHECK(panel.deletePreset(count - 1));
        CHECK(RenderPresets::loadUser().isEmpty());
        CHECK_EQ(panel.presets().size(), count - 1);

        // Jobs: ids increase, In/Out range is captured, removal
        panel.selectPreset(0);
        CHECK(panel.addToQueue(false));
        p.edit("inout", [](Timeline& tl) {
            tl.markIn = 5;
            tl.markOut = 20;
        });
        CHECK(panel.addToQueue(false));
        CHECK_EQ(p.renderQueue().size(), 2);
        if (p.renderQueue().size() == 2) {
            CHECK(p.renderQueue()[0].id == 1 && p.renderQueue()[1].id == 2);
            CHECK(!p.renderQueue()[0].inOut);
            CHECK(p.renderQueue()[1].inOut && p.renderQueue()[1].from == 5 && p.renderQueue()[1].to == 20);
        }
        panel.queuePanel()->removeJobs({1});
        CHECK_EQ(p.renderQueue().size(), 1);
        CHECK(panel.addToQueue(false));
        CHECK_EQ(p.renderQueue().last().id, 3);
    }

    // --- File name = the name of the opened timeline (several timelines), until the user types another one
    {
        Project p;
        DeliverPanel panel(&p);
        auto* name = panel.findChild<QLineEdit*>("renderName");
        auto* folder = panel.findChild<QLineEdit*>("renderFolder");
        if (!CHECK(name && folder)) return Check::result();
        folder->setText(tmp.path());
        const QString first = p.sequenceName(p.currentSequence());
        CHECK_EQ(name->text(), first); // "Timeline 1" as before
        const int intro = p.addTimeline("Intro: Teil 1/2"); // gets opened
        CHECK_EQ(p.currentSequence(), intro);
        CHECK_EQ(name->text(), QString("Intro - Teil 1-2"));
        const RenderJob j = panel.makeJob();
        CHECK_EQ(j.path, QDir(tmp.path()).filePath("Intro - Teil 1-2.mp4"));
        CHECK_EQ(j.sequence, intro);
        p.renameSequence(intro, "Outro");
        CHECK_EQ(name->text(), QString("Outro"));
        p.setCurrentSequence(1);
        CHECK_EQ(name->text(), first);
        name->setText("Mein Film?"); // typed: stays when another timeline is opened, made valid in the job
        p.setCurrentSequence(intro);
        CHECK_EQ(name->text(), QString("Mein Film?"));
        CHECK_EQ(QFileInfo(panel.makeJob().path).fileName(), QString("Mein Film.mp4"));
        name->clear(); // empty: the opened timeline again
        CHECK_EQ(QFileInfo(panel.makeJob().path).fileName(), QString("Outro.mp4"));
        p.setCurrentSequence(1);
        CHECK_EQ(name->text(), first);
    }

    // --- Rendering (small and short)
    if (!Check::haveFfmpeg() || QStandardPaths::findExecutable("ffprobe").isEmpty())
        return Check::skip("ffmpeg/ffprobe nicht gefunden");
    const QString clip = Check::makeMedia(tmp.filePath("clip.mp4"),
                                          {"-f", "lavfi", "-i", "testsrc2=size=320x180:rate=25:duration=2", "-f", "lavfi",
                                           "-i", "sine=frequency=440:duration=2", "-c:v", "libx264", "-pix_fmt",
                                           "yuv420p", "-c:a", "aac", "-shortest"});
    if (!CHECK(!clip.isEmpty())) return Check::result();
    Check::initMlt();
    std::setlocale(LC_NUMERIC, "C");

    Project p;
    {
        ProjectData d;
        d.format.width = 320;
        d.format.height = 180;
        Timeline& tl = d.timeline;
        TimelineOps::ensureTracks(tl, TrackKind::Video, 1);
        TimelineOps::ensureTracks(tl, TrackKind::Audio, 1);
        Clip v;
        v.id = 1;
        v.mediaPath = clip;
        v.start = 0;
        v.in = 0;
        v.out = 49;
        v.linkId = 1;
        Clip a = v;
        a.id = 2;
        tl.video[0].clips << v;
        tl.audio[0].clips << a;
        d.lastClipId = 2;
        d.lastLinkId = 1;
        p.load(d);
    }
    auto mk = [&](int id, const QString& file, const QString& format, QSize size = {}) {
        RenderJob j;
        j.id = id;
        j.path = tmp.filePath(file);
        j.settings.format = format;
        j.settings.quality = 2;
        j.size = size;
        return j;
    };
    RenderJob a = mk(1, "a.mp4", "h264", QSize(160, 90));
    a.inOut = true;
    a.from = 10;
    a.to = 29; // 20 Frames = 0,8 s
    RenderJob b = mk(2, "b.wav", "wav");
    RenderJob c = mk(3, "c.mov", "prores");
    c.inOut = true;
    c.from = 0;
    c.to = 4;
    QFile blocker(tmp.filePath("datei"));
    CHECK(blocker.open(QIODevice::WriteOnly));
    blocker.close();
    RenderJob bad = mk(4, "datei/unmöglich.mp4", "h264"); // folder is a file
    RenderJob d = mk(5, "d.m4a", "aac");
    d.status = RenderStatus::Done; // done: skipped on "Render all"
    p.setRenderQueue({a, b, c, bad, d});

    RenderQueue q(&p);
    QVector<int> order;
    QObject::connect(&q, &RenderQueue::jobFinished, [&](int id, RenderStatus) { order << id; });
    int progressCalls = 0;
    QObject::connect(&q, &RenderQueue::progress, [&](int, int) { ++progressCalls; });
    int done = -1, failed = -1;
    QObject::connect(&q, &RenderQueue::finished, [&](int dn, int fl) {
        done = dn;
        failed = fl;
    });
    CHECK(q.start());
    CHECK(q.isRunning());
    CHECK(!q.start()); // already running
    CHECK(waitFor(q));
    CHECK_EQ(order, (QVector<int>{1, 2, 3, 4}));
    CHECK(done == 3 && failed == 1);
    CHECK(progressCalls >= 3);
    CHECK(job(p, 1)->status == RenderStatus::Done && job(p, 1)->renderMs > 0);
    CHECK(job(p, 2)->status == RenderStatus::Done && job(p, 3)->status == RenderStatus::Done);
    CHECK(job(p, 4)->status == RenderStatus::Failed && !job(p, 4)->message.isEmpty());
    CHECK(job(p, 5)->status == RenderStatus::Done && !QFileInfo::exists(d.path)); // not rendered again

    const QJsonObject pa = probe(a.path);
    const QJsonObject va = stream(pa, "video");
    CHECK_EQ(va.value("codec_name").toString(), QString("h264"));
    CHECK(va.value("width").toInt() == 160 && va.value("height").toInt() == 90);
    CHECK(!stream(pa, "audio").isEmpty());
    const double durA = pa.value("format").toObject().value("duration").toString().toDouble();
    if (!CHECK(std::abs(durA - 0.8) < 0.1)) std::printf("       Dauer a: %.3f\n", durA);
    const QJsonObject pb = probe(b.path);
    CHECK(stream(pb, "video").isEmpty());
    CHECK_EQ(stream(pb, "audio").value("codec_name").toString(), QString("pcm_s24le"));
    const double durB = pb.value("format").toObject().value("duration").toString().toDouble();
    CHECK(std::abs(durB - 2.0) < 0.1);
    const QJsonObject pc = probe(c.path);
    CHECK_EQ(stream(pc, "video").value("codec_name").toString(), QString("prores"));
    CHECK_EQ(stream(pc, "video").value("width").toInt(), 320); // timeline resolution

    // Re-render a single finished job on purpose
    order.clear();
    CHECK(q.start({5}));
    CHECK(waitFor(q));
    CHECK_EQ(order, (QVector<int>{5}));
    CHECK(QFileInfo::exists(d.path) && stream(probe(d.path), "audio").value("codec_name").toString() == "aac");
    // Without selection: everything that is not finished -> only the faulty job 4 (fails again)
    order.clear();
    CHECK(q.start());
    CHECK(waitFor(q));
    CHECK_EQ(order, (QVector<int>{4}));
    CHECK(job(p, 4)->status == RenderStatus::Failed);

    // A finished job's file: double-click and the menu show it in the file manager (signals, the Deliver page passes
    // them on to FileManager); a failed job has none: double-click loads its settings, the entries are off.
    // QUEUE_SHOT=<file.png>: the queue with the menu of a finished job
    {
        RenderQueuePanel panel(&p, &q);
        panel.resize(360, 720);
        panel.show();
        auto* list = panel.findChild<QListWidget*>();
        QSignalSpy reveal(&panel, &RenderQueuePanel::revealFileRequested);
        QSignalSpy open(&panel, &RenderQueuePanel::openFileRequested);
        QSignalSpy load(&panel, &RenderQueuePanel::loadJobRequested);
        if (CHECK(list && list->count() == 5)) {
            emit list->itemDoubleClicked(list->item(0)); // job 1: done
            CHECK_EQ(reveal.count(), 1);
            if (!reveal.isEmpty()) CHECK_EQ(reveal.first().first().toString(), a.path);
            CHECK_EQ(load.count(), 0);
            emit list->itemDoubleClicked(list->item(3)); // job 4: failed
            CHECK_EQ(reveal.count(), 1);
            CHECK_EQ(load.count(), 1);
            const auto menuOn = [&](int row, const std::function<void(QMenu*)>& look) {
                list->clearSelection();
                list->item(row)->setSelected(true);
                bool seen = false;
                QTimer timer; // dies with this call: never fires later on a dead `seen`
                timer.setSingleShot(true);
                QObject::connect(&timer, &QTimer::timeout, [&] {
                    if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
                        seen = true;
                        look(menu);
                        menu->close();
                    }
                });
                timer.start(200);
                emit list->customContextMenuRequested(list->visualItemRect(list->item(row)).center());
                return seen;
            };
            CHECK(menuOn(0, [&](QMenu* menu) {
                if (const QString shot = qEnvironmentVariable("QUEUE_SHOT"); !shot.isEmpty()) {
                    const QPoint at = panel.mapFromGlobal(menu->pos());
                    QImage image(QSize(std::max(panel.width(), at.x() + menu->width()),
                                       std::max(panel.height(), at.y() + menu->height())),
                                 QImage::Format_RGB32);
                    image.fill(Qt::black);
                    QPainter painter(&image);
                    painter.drawPixmap(0, 0, panel.grab());
                    painter.drawPixmap(at, menu->grab());
                    painter.end();
                    image.save(shot);
                }
                QAction* r = menu->findChild<QAction*>("revealJob");
                QAction* o = menu->findChild<QAction*>("openJob");
                if (CHECK(r && o && r->isEnabled() && o->isEnabled())) {
                    r->trigger();
                    o->trigger();
                }
            }));
            CHECK_EQ(reveal.count(), 2);
            CHECK_EQ(open.count(), 1);
            if (!open.isEmpty()) CHECK_EQ(open.first().first().toString(), a.path);
            CHECK(menuOn(3, [&](QMenu* menu) {
                QAction* r = menu->findChild<QAction*>("revealJob");
                CHECK(r && !r->isEnabled());
            }));
        }
    }

    // Cancel: running job -> "Cancelled", half file gone, rest stays "Waiting"
    {
        RenderJob big = mk(6, "gross.mp4", "h264", QSize(1920, 1080));
        big.settings.quality = 0;
        RenderJob after = mk(7, "danach.mp4", "h264");
        p.setRenderQueue({big, after});
        CHECK(q.start());
        QEventLoop loop;
        QObject::connect(&q, &RenderQueue::progress, &loop, &QEventLoop::quit);
        QTimer::singleShot(30000, &loop, &QEventLoop::quit);
        loop.exec();
        q.cancel();
        CHECK(!q.isRunning());
        CHECK(job(p, 6)->status == RenderStatus::Canceled);
        CHECK(job(p, 7)->status == RenderStatus::Queued);
        CHECK(!QFileInfo::exists(big.path));
        CHECK(!QFileInfo::exists(after.path));
    }

    // Cancel between two jobs (next one starts only via timer): still counts as running, rest does not start
    {
        p.setRenderQueue({mk(8, "eins.mp4", "h264"), mk(9, "zwei.mp4", "h264")});
        CHECK(q.start());
        bool between = false;
        int finishedCount = 0;
        const auto c1 = QObject::connect(&q, &RenderQueue::jobFinished, &q, [&](int id, RenderStatus) {
            if (id != 8) return;
            between = q.isRunning();
            q.cancel();
        });
        const auto c2 = QObject::connect(&q, &RenderQueue::finished, &q, [&] { ++finishedCount; });
        CHECK(waitFor(q));
        QCoreApplication::processEvents(); // scheduled follow-up job must not start anymore
        QObject::disconnect(c1);
        QObject::disconnect(c2);
        CHECK(between);
        CHECK(!q.isRunning());
        CHECK_EQ(finishedCount, 1);
        CHECK(job(p, 8)->status == RenderStatus::Done);
        CHECK(job(p, 9)->status == RenderStatus::Queued);
    }

    // Preparing a job (graphics card probe, opening the files, analyses) runs in the background: start() returns at
    // once, the event loop goes on meanwhile (the window stays usable), the job is "preparing" until its first progress
    {
        p.setRenderQueue({mk(13, "prep.mp4", "h264")});
        int preparing = 0, firstProgress = -1;
        bool loopWhilePreparing = false;
        const auto c1 = QObject::connect(&q, &RenderQueue::preparing, [&](int id) { preparing = id; });
        const auto c2 = QObject::connect(&q, &RenderQueue::progress, [&](int, int percent) {
            if (firstProgress < 0) firstProgress = percent;
        });
        CHECK(q.start());
        CHECK_EQ(preparing, 13);
        CHECK(q.isPreparing());
        CHECK(job(p, 13)->status == RenderStatus::Rendering);
        QTimer::singleShot(0, &q, [&] { loopWhilePreparing = q.isPreparing(); });
        CHECK(waitFor(q));
        CHECK(loopWhilePreparing);
        CHECK_EQ(firstProgress, 0);
        CHECK(!q.isPreparing());
        CHECK(job(p, 13)->status == RenderStatus::Done);
        QObject::disconnect(c1);
        QObject::disconnect(c2);
    }

    // Cancel while preparing: nothing written, the job canceled, the rest waits, nothing keeps running
    {
        p.setRenderQueue({mk(14, "prepcancel.mp4", "h264"), mk(15, "prepnext.mp4", "h264")});
        int finishedCount = 0;
        const auto c = QObject::connect(&q, &RenderQueue::finished, [&] { ++finishedCount; });
        CHECK(q.start());
        CHECK(q.isPreparing());
        q.cancel();
        CHECK(!q.isRunning() && !q.isPreparing());
        CHECK(job(p, 14)->status == RenderStatus::Canceled);
        CHECK(job(p, 15)->status == RenderStatus::Queued);
        QEventLoop settle;
        QTimer::singleShot(300, &settle, &QEventLoop::quit);
        settle.exec();
        QObject::disconnect(c);
        CHECK_EQ(finishedCount, 1);
        CHECK(!Exporter::anyRunning());
        CHECK(!QFileInfo::exists(tmp.filePath("prepcancel.mp4")) && !QFileInfo::exists(tmp.filePath("prepnext.mp4")));
    }

    // Cancel right as preparing ends (the window was busy meanwhile): the finished build waits in the event queue when
    // cancel() stops it; it must do nothing then (it used the deleted thread: crash) and never start the export
    {
        RenderJob done = mk(17, "prepdone.mp4", "h264");
        done.settings.encoder = VideoEncoder::Software; // the build starts at once (no graphics card probe)
        p.setRenderQueue({done});
        int finishedCount = 0;
        const auto c = QObject::connect(&q, &RenderQueue::finished, [&] { ++finishedCount; });
        CHECK(q.start());
        CHECK(q.isPreparing());
        QThread::msleep(3000); // no events: the build thread ends, its result stays queued
        q.cancel();
        CHECK(!q.isRunning());
        QEventLoop settle;
        QTimer::singleShot(500, &settle, &QEventLoop::quit);
        settle.exec();
        QObject::disconnect(c);
        CHECK_EQ(finishedCount, 1);
        CHECK(job(p, 17)->status == RenderStatus::Canceled);
        CHECK(!Exporter::anyRunning());
        CHECK(!QFileInfo::exists(tmp.filePath("prepdone.mp4")));
    }

    // Encoder "Auto" in the first job of a session waits for the graphics card probe: in the background too, cancel
    // works meanwhile, the next start waits again and renders
    {
        HardwareEncoders::resetForTests();
        RenderJob autoJob = mk(16, "probe.mp4", "h264");
        autoJob.settings.encoder = VideoEncoder::Auto;
        p.setRenderQueue({autoJob});
        CHECK(q.start());
        CHECK(q.isPreparing());
        q.cancel();
        CHECK(!q.isRunning());
        CHECK(job(p, 16)->status == RenderStatus::Canceled);
        CHECK(q.start({16}));
        CHECK(waitFor(q));
        CHECK(HardwareEncoders::probed());
        CHECK(job(p, 16)->status == RenderStatus::Done);
        CHECK(QFileInfo(autoJob.path).size() > 0);
    }

    // Order: "Render all" takes the waiting jobs in the queue's order as it is when the next one starts, so dragging
    // during a render changes what comes next; the job being rendered cannot be moved
    {
        p.setRenderQueue({mk(10, "o1.wav", "wav"), mk(11, "o2.wav", "wav"), mk(12, "o3.wav", "wav")});
        RenderQueuePanel panel(&p, &q);
        order.clear();
        CHECK(q.start());
        CHECK_EQ(q.currentJob(), 10);
        panel.moveJobs({10, 12}, 11); // 10 is being rendered: only 12 moves
        CHECK_EQ(ids(p.renderQueue()), (QVector<int>{10, 12, 11}));
        panel.moveJobs({10}, 0);
        CHECK_EQ(ids(p.renderQueue()), (QVector<int>{10, 12, 11}));
        auto* list = panel.findChild<QListWidget*>();
        if (CHECK(list && list->count() == 3)) { // its card cannot be dragged, the others can
            CHECK(!(list->item(0)->flags() & Qt::ItemIsDragEnabled));
            CHECK(list->item(1)->flags() & Qt::ItemIsDragEnabled);
        }
        CHECK(waitFor(q));
        CHECK_EQ(order, (QVector<int>{10, 12, 11}));
    }

    // Output = source file of the timeline (also via another path to it): refused, source stays unchanged
    {
        const QByteArray before = [&] { QFile f(clip); f.open(QIODevice::ReadOnly); return f.readAll(); }();
        RenderJob self = mk(8, "clip.mp4", "h264");
        RenderJob viaDir = mk(9, "unter/../clip.mp4", "h264");
        CHECK(QDir(tmp.path()).mkpath("unter"));
        p.setRenderQueue({self, viaDir});
        order.clear();
        CHECK(q.start());
        CHECK(waitFor(q));
        CHECK_EQ(order, (QVector<int>{8, 9}));
        CHECK(job(p, 8)->status == RenderStatus::Failed && job(p, 8)->message.contains("clip.mp4"));
        CHECK(job(p, 9)->status == RenderStatus::Failed);
        QFile f(clip);
        CHECK(f.open(QIODevice::ReadOnly) && f.readAll() == before);
        CHECK(!Exporter::readsFile(p.renderTimeline(), tmp.filePath("anders.mp4")));
    }

    // --- Check before export on the Deliver page: a forgotten frame between two clips, sound measured; clicking a
    // result asks to move the playhead there (CHECK_DUMP=<file.png>: the panel with the results)
    {
        Project q;
        ProjectData d;
        d.format.width = 320;
        d.format.height = 180;
        TimelineOps::ensureTracks(d.timeline, TrackKind::Video, 1);
        TimelineOps::ensureTracks(d.timeline, TrackKind::Audio, 1);
        for (int i = 0; i < 2; ++i) {
            Clip v;
            v.id = 1 + 2 * i;
            v.mediaPath = clip;
            v.start = i * 26;
            v.out = 24;
            v.linkId = 1 + i;
            Clip a = v;
            a.id = v.id + 1;
            d.timeline.video[0].clips << v;
            d.timeline.audio[0].clips << a;
        }
        d.lastClipId = 4;
        d.lastLinkId = 2;
        q.load(d);
        DeliverPanel panel(&q);
        panel.resize(340, 700);
        CHECK(panel.runCheck());
        QListWidget* list = panel.checkList();
        int gapRow = -1;
        for (int i = 0; i < list->count(); ++i)
            if (list->item(i)->data(Qt::UserRole).toInt() == 25) gapRow = i;
        CHECK(gapRow >= 0); // frame 25 has no picture
        CHECK(!list->isHidden());
        if (gapRow >= 0) {
            QSignalSpy seek(&panel, &DeliverPanel::seekRequested);
            emit list->itemClicked(list->item(gapRow));
            CHECK_EQ(seek.count(), 1);
            if (!seek.isEmpty()) CHECK_EQ(seek.first().first().toInt(), 25);
        }
        if (const QByteArray dump = qgetenv("CHECK_DUMP"); !dump.isEmpty()) panel.grab().save(QString::fromLocal8Bit(dump));
    }

    // --- The same check for a vertical video: a title in the zone TikTok/Reels/Shorts cover is listed with its time
    // and clicking it moves the playhead to the title; a 16:9 timeline with the same title does not get it
    for (const bool vertical : {true, false}) {
        Project q;
        ProjectData d;
        d.format.width = vertical ? 180 : 320;
        d.format.height = vertical ? 320 : 180;
        TimelineOps::ensureTracks(d.timeline, TrackKind::Video, 2);
        Clip v;
        v.id = 1;
        v.mediaPath = clip;
        v.out = 49;
        d.timeline.video[0].clips << v;
        Clip t;
        t.id = 2;
        t.kind = ClipKind::Title;
        t.title.text = "x16";
        t.title.size = 20;
        t.title.posY = vertical ? -140 : -75; // the bottom 6 % of the picture
        t.start = 10;
        t.out = 9;
        d.timeline.video[1].clips << t;
        d.lastClipId = 2;
        q.load(d);
        DeliverPanel panel(&q);
        CHECK(panel.runCheck());
        QListWidget* list = panel.checkList();
        int row = -1;
        for (int i = 0; i < list->count(); ++i)
            if (list->item(i)->text().contains("x16")) row = i;
        CHECK_EQ(row >= 0, vertical);
        if (row >= 0) {
            CHECK_EQ(list->item(row)->data(Qt::UserRole).toInt(), 10);
            QSignalSpy seek(&panel, &DeliverPanel::seekRequested);
            emit list->itemClicked(list->item(row));
            if (CHECK_EQ(seek.count(), 1)) CHECK_EQ(seek.first().first().toInt(), 10);
        }
    }
    return Check::result();
}
