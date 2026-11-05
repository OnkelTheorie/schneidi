// SPDX-License-Identifier: AGPL-3.0-only
// Content templates in the app (0.18): the open project keeps its template description (Project load/data), marking a
// clip as placeholder through the menu action and changing its rules in the Inspector (one undo step each), dropping a
// Media Pool file onto a placeholder fills it (one undo step) while Alt drops as always, the template browser lists
// built-in and own content templates and the start templates, "New" gives an untitled project without template
// description and "Edit" keeps it, saving as template from the window, "Check Fields". Everything stays in the test's
// own settings and folders.
// TEMPLATE_DUMP=<folder>: pictures (slot-inspector.png, slot-timeline.png, template-browser.png, slot-check.png,
// save-template.png)

#include "check.h"

#include "app/InputBindings.h"
#include "app/MainWindow.h"
#include "app/TemplateDialogs.h"
#include "app/Theme.h"
#include "core/Editor.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/Selection.h"
#include "core/TemplateFill.h"
#include "core/TemplateLibrary.h"
#include "core/TimelineOps.h"
#include "engine/Engine.h"
#include "ui/Inspector.h"
#include "ui/MediaPool.h"
#include "ui/timeline/TimelineView.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDropEvent>
#include <QFile>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QPainter>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QUndoStack>
#include <clocale>
#include <tuple>

namespace {

const QString kTmpl = "/m/tmpl.mp4", kLong = "/m/long.mp4", kShort = "/m/short.mp4", kSong = "/m/song.wav";

void dump(QWidget* w, const QString& name)
{
    const QString dir = qEnvironmentVariable("TEMPLATE_DUMP");
    if (dir.isEmpty()) return;
    QDir().mkpath(dir);
    w->grab().save(QDir(dir).filePath(name));
}

MediaInfo media(const QString& path, int length, bool video, bool audio)
{
    return {path, QFileInfo(path).fileName(), length, video, audio, false};
}

Clip clip(int id, const QString& path, int start, int in, int len, int link = 0)
{
    Clip c;
    c.id = id;
    c.mediaPath = path;
    c.start = start;
    c.in = in;
    c.out = in + len - 1;
    c.linkId = link;
    return c;
}

ClipSlot slot(const QString& id, const QString& label, SlotKind kind = SlotKind::Clip)
{
    ClipSlot s;
    s.id = id;
    s.label = label;
    s.kind = kind;
    return s;
}

// V1: placeholder "clip1" 0-100 (linked A1) | plain clip 100-200. V2: title "HOOK" 0-50 (placeholder "hook" when
// `marked`). A2: music 0-200 (placeholder "music" when `marked`)
ProjectData project(bool marked = true)
{
    ProjectData d;
    d.format = ProjectFormat{1920, 1080, {25, 1}};
    d.media = {media(kTmpl, 500, true, true), media(kLong, 300, true, true), media(kShort, 50, true, true),
               media(kSong, 1500, false, true)};
    Timeline& tl = d.timeline;
    tl.video.resize(2);
    tl.audio.resize(2);
    for (Track& t : tl.audio) t.kind = TrackKind::Audio;
    Clip c1 = clip(1, kTmpl, 0, 0, 100, 1);
    c1.slot = slot("clip1", "Bester Moment");
    c1.slot.hint = "der lauteste Kill";
    tl.video[0].clips = {c1, clip(2, kTmpl, 100, 200, 100, 2)};
    tl.audio[0].clips = {clip(3, kTmpl, 0, 0, 100, 1), clip(4, kTmpl, 100, 200, 100, 2)};
    Clip title;
    title.id = 5;
    title.kind = ClipKind::Title;
    title.title.text = "HOOK";
    title.out = 49;
    if (marked) title.slot = slot("hook", "Hook-Text", SlotKind::Text);
    tl.video[1].clips = {title};
    Clip music = clip(7, kSong, 0, 0, 200);
    if (marked) music.slot = slot("music", "Song", SlotKind::Music);
    tl.audio[1].clips = {music};
    d.lastClipId = 7;
    d.lastLinkId = 2;
    return d;
}

QString touch(const QString& path, const QByteArray& data = "x")
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write(data);
    return path;
}

// A preview picture with a colour and a word (browser tiles)
QString previewPng(const QString& path, int w, int h, const QColor& color, const QString& word)
{
    QImage img(w, h, QImage::Format_RGB32);
    img.fill(color);
    QPainter p(&img);
    p.setPen(Qt::white);
    QFont f = p.font();
    f.setPixelSize(h / 6);
    f.setBold(true);
    p.setFont(f);
    p.drawText(img.rect(), Qt::AlignCenter, word);
    p.end();
    QDir().mkpath(QFileInfo(path).absolutePath());
    img.save(path);
    return path;
}

// The project with its media as (tiny) files in `dir`: opening it in the window does not ask for missing media
ProjectData withFiles(ProjectData d, const QString& dir)
{
    QHash<QString, QString> to;
    for (MediaInfo& m : d.media) {
        to[m.path] = touch(QDir(dir).filePath(QFileInfo(m.path).fileName()));
        m.path = to[m.path];
    }
    for (QVector<Track>* tracks : {&d.timeline.video, &d.timeline.audio})
        for (Track& t : *tracks)
            for (Clip& c : t.clips)
                if (to.contains(c.mediaPath)) c.mediaPath = to[c.mediaPath];
    return d;
}

void clean()
{
    QDir(TemplateLibrary::dir()).removeRecursively();
    // The built-in ones' copies too: copied again only where sizes differ (a test folder of the same name length)
    QDir(QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)).filePath("templates-builtin")).removeRecursively();
    QSettings().remove("templates");
}

// Project::data()/load() keep the template description (a template opened and saved again keeps its name)
void testProjectKeepsInfo()
{
    ProjectData d = project();
    d.templateInfo = TemplateInfo{"Gaming-Short", "Drei Clips auf Musik", {"gaming", "short"}, "9:16"};
    Project p;
    p.load(d);
    CHECK(p.templateInfo() == d.templateInfo);
    CHECK(p.data().templateInfo == d.templateInfo);
    // Through the file and back
    QTemporaryDir tmp;
    const QString path = tmp.filePath("t.schneidi");
    QString err;
    CHECK(ProjectFile::save(p.data(), path, &err));
    ProjectData back;
    CHECK(ProjectFile::load(path, &back, &err));
    Project q;
    q.load(back);
    CHECK(q.data().templateInfo == d.templateInfo);
    // Another project without one: gone
    q.load(project());
    CHECK(!q.data().templateInfo.has_value());
}

// Drop of a Media Pool file at `pt` (with `mods`)
void dropFile(TimelineView& tv, const QString& path, const QPoint& pt, Qt::KeyboardModifiers mods)
{
    QMimeData mime;
    mime.setData(MediaPool::MimeType, path.toUtf8());
    QDragEnterEvent enter(pt, Qt::CopyAction, &mime, Qt::LeftButton, mods);
    QApplication::sendEvent(&tv, &enter);
    QDragMoveEvent move(pt, Qt::CopyAction, &mime, Qt::LeftButton, mods);
    QApplication::sendEvent(&tv, &move);
    QDropEvent drop(QPointF(pt), Qt::CopyAction, &mime, Qt::LeftButton, mods);
    QApplication::sendEvent(&tv, &drop);
}

// Drag-fill: a file dropped onto a placeholder fills it (one undo step), with Alt it drops as always
void testDropFill()
{
    Project project;
    Selection sel;
    Editor editor(&project, &sel);
    project.load(::project());
    TimelineView tv(&editor);
    tv.resize(1200, 420);
    tv.show();
    QApplication::processEvents();
    auto mediaOf = [&](int id) { return TimelineOps::findClip(project.timeline(), id)->mediaPath; };

    QStringList messages, normal;
    QObject::connect(&tv, &TimelineView::slotFilled, [&](const QString& m, bool ok) {
        if (ok) messages << m;
    });
    QObject::connect(&tv, &TimelineView::dropRequested, [&](const QStringList& paths, int, int) { normal << paths; });
    const int before = project.undoStack()->count();
    // The placeholder clip 1 on V1 at the front: find its row
    int hitY = -1;
    const int x = TimelineView::kHeaderW + 4;
    for (int y = TimelineView::kRulerH + 2; y < 420 && hitY < 0; y += 4) {
        dropFile(tv, kLong, QPoint(x, y), Qt::NoModifier);
        if (mediaOf(1) == kLong) hitY = y;
    }
    if (!CHECK(hitY >= 0)) return;
    CHECK_EQ(project.undoStack()->count(), before + 1);
    CHECK_EQ(int(messages.size()), 1);
    CHECK(messages.value(0).contains("Bester Moment"));
    CHECK(messages.value(0).contains(T("gekürzt %1 → %2").section(' ', 0, 0))); // 300 frames into a 100 frame slot
    CHECK_EQ(TimelineOps::findClip(project.timeline(), 1)->length(), 100);
    // The linked sound followed
    CHECK_EQ(mediaOf(3), kLong);
    // Undo: one step back to the template's file
    project.undoStack()->undo();
    CHECK_EQ(mediaOf(1), kTmpl);
    project.undoStack()->redo();
    // Alt: a normal drop (the main window places it), the placeholder stays as it is
    const int count = project.undoStack()->count();
    normal.clear();
    dropFile(tv, kShort, QPoint(x, hitY), Qt::AltModifier);
    CHECK_EQ(normal, QStringList{kShort});
    CHECK_EQ(mediaOf(1), kLong);
    CHECK_EQ(project.undoStack()->count(), count);
    // A plain clip (no placeholder): normal drop too
    normal.clear();
    const int plainX = TimelineView::kHeaderW + int(150 * tv.view().pxPerFrame) - int(tv.view().leftFrame * tv.view().pxPerFrame);
    dropFile(tv, kShort, QPoint(plainX, hitY), Qt::NoModifier);
    CHECK_EQ(normal, QStringList{kShort});
    CHECK_EQ(mediaOf(2), kTmpl);
    // Over the linked audio of the placeholder: fills the placeholder (its video)
    int audioY = -1;
    // (past V1: a video row is at most ViewState::videoTrackHeight high and hitY its first hit)
    for (int y = hitY + ViewState{}.videoTrackHeight + 2; y < 420 && audioY < 0; y += 4) {
        const int n = project.undoStack()->count();
        dropFile(tv, kTmpl, QPoint(x, y), Qt::NoModifier);
        if (project.undoStack()->count() == n + 1 && mediaOf(3) == kTmpl) audioY = y;
    }
    CHECK(audioY > hitY);
    CHECK_EQ(mediaOf(1), kTmpl);

    // Picture: the timeline with three placeholders (clip, text, music)
    tv.update();
    QApplication::processEvents();
    dump(&tv, "slot-timeline.png");
}

// Inspector section "Placeholder": only for placeholders, kinds the clip can hold, each change one undo step
void testInspector()
{
    Project project;
    Selection sel;
    Editor editor(&project, &sel);
    project.load(::project());
    Inspector insp(&editor);
    insp.resize(380, 760);
    insp.show();
    sel.set({2}); // a plain clip: no section
    QApplication::processEvents();
    auto* body = insp.findChild<QWidget*>("SlotSectionVideo");
    if (!CHECK(body)) return;
    CHECK(!body->isVisible());
    sel.set({1});
    QApplication::processEvents();
    CHECK(body->isVisible());
    auto* label = insp.findChild<QLineEdit*>("slotV_label");
    auto* kind = insp.findChild<QComboBox*>("slotV_kind");
    auto* fit = insp.findChild<QComboBox*>("slotV_fit");
    auto* length = insp.findChild<QComboBox*>("slotV_length");
    auto* maxChars = insp.findChild<QWidget*>("slotV_maxChars");
    if (!CHECK(label && kind && fit && length && maxChars)) return;
    CHECK_EQ(label->text(), QString("Bester Moment"));
    // A video clip of a file: clip or logo, never text/music
    CHECK_EQ(kind->count(), 2);
    CHECK(kind->findData(int(SlotKind::Text)) < 0 && kind->findData(int(SlotKind::Music)) < 0);
    CHECK(fit->isVisible() && !maxChars->isVisible());
    auto slotOf = [&](int id) { return TimelineOps::findClip(project.timeline(), id)->slot; };
    const int before = project.undoStack()->count();
    label->setText("Mein Clip");
    label->setModified(true);
    emit label->editingFinished();
    CHECK_EQ(slotOf(1).label, QString("Mein Clip"));
    CHECK_EQ(project.undoStack()->count(), before + 1);
    emit label->editingFinished(); // unchanged: no step
    CHECK_EQ(project.undoStack()->count(), before + 1);
    fit->setCurrentIndex(fit->findData(int(SlotFit::Freeze)));
    emit fit->activated(fit->currentIndex());
    CHECK(slotOf(1).fit == SlotFit::Freeze);
    CHECK_EQ(project.undoStack()->count(), before + 2);
    length->setCurrentIndex(length->findData(int(SlotLength::Flexible)));
    emit length->activated(length->currentIndex());
    QApplication::processEvents();
    CHECK(slotOf(1).length == SlotLength::Flexible);
    CHECK(insp.findChild<QWidget*>("slotV_max")->isVisible());
    project.undoStack()->undo();
    CHECK(slotOf(1).length == SlotLength::Fixed);
    // The title: text rules
    sel.set({5});
    QApplication::processEvents();
    CHECK(maxChars->isVisibleTo(body) && !fit->isVisibleTo(body)); // (the Title tab is in front now)
    CHECK_EQ(kind->count(), 1);
    sel.set({1});
    QApplication::processEvents();
    dump(&insp, "slot-inspector.png");
    // Trash in the header: the mark goes (one step)
    const int n = project.undoStack()->index();
    if (auto* trash = insp.findChild<QToolButton*>("slotV_remove"); CHECK(trash)) trash->click();
    CHECK(!slotOf(1).isSet());
    CHECK_EQ(project.undoStack()->index(), n + 1);
    QApplication::processEvents();
    CHECK(!body->isVisible());
    // Music on the audio page
    sel.set({7});
    QApplication::processEvents();
    auto* music = insp.findChild<QCheckBox*>("slotA_fitMusic");
    if (CHECK(music)) CHECK(music->isVisible());
}

// Built-in templates from a test folder, an own one
QString makeBuiltIns(const QString& root)
{
    QString err;
    for (const auto& [id, name, w, h, color] :
         {std::tuple{"short-hook", "Short mit Hook", 1080, 1920, QColor(0xc0, 0x50, 0x40)},
          std::tuple{"gaming", "Gaming-Highlight", 1920, 1080, QColor(0x40, 0x60, 0xb0)},
          std::tuple{"product", "Produktclip 15 s", 1080, 1080, QColor(0x40, 0x90, 0x60)}}) {
        ProjectData d = withFiles(project(), QDir(root).filePath("files"));
        d.format = ProjectFormat{w, h, {25, 1}};
        d.templateInfo = TemplateInfo{name, QString("Vorlage %1").arg(name), {QString(id) == "gaming" ? "gaming" : "short"}, {}};
        const QString dir = QDir(root).filePath(id);
        QDir().mkpath(dir);
        CHECK(ProjectFile::save(d, QDir(dir).filePath(TemplateLibrary::ProjectFileName), &err));
        if (QString(id) != "product") previewPng(QDir(dir).filePath("preview.png"), w / 4, h / 4, color, name);
    }
    return root;
}

void testBrowserAndWindow(const QString& tmp)
{
    clean();
    TemplateLibrary::setBuiltInRoot(makeBuiltIns(tmp + "/builtin"));
    // An own template (media stay where they are: fake paths)
    TemplateLibrary::SaveOptions o;
    o.description = "Meine eigene";
    o.tags = {"podcast"};
    o.copyMedia = false;
    QString err;
    const std::optional<ContentTemplate> own =
        TemplateLibrary::save(withFiles(project(), tmp + "/files"), "Mein Podcast", o, &err);
    if (!CHECK(own.has_value())) {
        std::printf("%s\n", qPrintable(err));
        return;
    }

    {
        TemplateBrowserDialog dlg;
        const QStringList names = dlg.shownNames();
        // Built-in ones first, then own, then the start templates
        CHECK_EQ(names.value(0), QString("Gaming-Highlight"));
        CHECK(names.indexOf("Mein Podcast") > names.indexOf("Short mit Hook"));
        CHECK(names.contains(T("Neutral")));
        CHECK(names.indexOf(T("Neutral")) > names.indexOf("Mein Podcast"));
        // Filter by shape and tag
        auto* aspect = dlg.findChild<QComboBox*>("template_aspect_filter");
        auto* tag = dlg.findChild<QComboBox*>("template_tag_filter");
        if (CHECK(aspect && tag)) {
            aspect->setCurrentIndex(aspect->findData("9:16"));
            emit aspect->activated(aspect->currentIndex());
            CHECK(dlg.shownNames().contains("Short mit Hook") && !dlg.shownNames().contains("Gaming-Highlight"));
            aspect->setCurrentIndex(0);
            emit aspect->activated(0);
            tag->setCurrentIndex(tag->findData("gaming"));
            emit tag->activated(tag->currentIndex());
            CHECK_EQ(dlg.shownNames(), QStringList{"Gaming-Highlight"});
            tag->setCurrentIndex(0);
            emit tag->activated(0);
        }
        // Built-in: New only; own: also edit, rename, delete; ZIP buttons wait for package B
        auto* edit = dlg.findChild<QPushButton*>("template_edit");
        auto* remove = dlg.findChild<QPushButton*>("template_delete");
        CHECK(dlg.select("Gaming-Highlight"));
        CHECK(!edit->isEnabled() && !remove->isEnabled());
        CHECK(dlg.select("Mein Podcast"));
        CHECK(edit->isEnabled() && remove->isEnabled());
        CHECK(!dlg.findChild<QPushButton*>("template_import")->isEnabled());
        CHECK(dlg.select("Short mit Hook"));
        dlg.resize(840, 560);
        dlg.show();
        QApplication::processEvents();
        dump(&dlg, "template-browser.png");
        dlg.findChild<QPushButton*>("template_new")->click();
        CHECK(dlg.choice() == TemplateBrowserDialog::Choice::NewContent);
        CHECK_EQ(dlg.content().name, QString("Short mit Hook"));
    }

    Engine engine;
    if (!CHECK(engine.init(&err))) return;
    std::setlocale(LC_NUMERIC, "C");
    MainWindow w(&engine);
    w.disableAutosave();
    Project* p = w.liveProject();
    // New: an untitled copy without template description; Edit: the template itself, with it
    const std::optional<ContentTemplate> hook = TemplateLibrary::find("short-hook");
    if (!CHECK(hook.has_value())) return;
    CHECK(w.newProjectFromContentTemplate(*hook));
    CHECK(!p->templateInfo().has_value());
    CHECK(w.liveProjectPath().isEmpty());
    CHECK_EQ(p->format().height, 1920);
    CHECK_EQ(TemplateFill::slotIds(p->data()), (QStringList{"clip1", "hook", "music"}));
    CHECK(w.editContentTemplate(*own));
    CHECK(p->templateInfo().has_value() && p->templateInfo()->name == "Mein Podcast");
    CHECK_EQ(w.liveProjectPath(), own->project);
    CHECK(!w.editContentTemplate(*hook)); // built-in: never edited in its cache copy

    // Mark through the menu action: the title becomes a text placeholder, one undo step
    p->load(project(false));
    Selection* sel = w.liveSelection();
    sel->set({5});
    const int before = p->undoStack()->count();
    QAction* mark = InputBindings::instance().action("slot_mark");
    if (!CHECK(mark)) return;
    mark->trigger();
    const ClipSlot s = TimelineOps::findClip(p->timeline(), 5)->slot;
    CHECK_EQ(s.id, QString("text1"));
    CHECK(s.kind == SlotKind::Text);
    CHECK_EQ(p->undoStack()->count(), before + 1);
    // The Inspector shows its section
    QApplication::processEvents();
    auto* label = w.findChild<QLineEdit*>("slotV_label");
    if (CHECK(label)) CHECK_EQ(label->text(), s.label);
    // A second mark gets the next free id; the music clip a music placeholder
    sel->set({7});
    mark->trigger();
    CHECK_EQ(TimelineOps::findClip(p->timeline(), 7)->slot.id, QString("music1"));
    CHECK(TimelineOps::findClip(p->timeline(), 7)->slot.kind == SlotKind::Music);
    // Remove: both selected at once, one step
    sel->set({5, 7});
    const int n = p->undoStack()->count();
    InputBindings::instance().action("slot_remove")->trigger();
    CHECK(!TimelineOps::findClip(p->timeline(), 5)->slot.isSet() && !TimelineOps::findClip(p->timeline(), 7)->slot.isSet());
    CHECK_EQ(p->undoStack()->count(), n + 1);
    p->undoStack()->undo();
    CHECK(TimelineOps::findClip(p->timeline(), 5)->slot.isSet());

    // Check Fields: the placeholders with their state (the fake files are missing)
    {
        ProjectData d = project();
        ClipSlot& hookSlot = TimelineOps::findClip(d.timeline, 5)->slot;
        hookSlot.maxChars = 3; // "HOOK" is 4
        hookSlot.shrink = false;
        p->load(d);
        SlotCheckDialog check(p->data());
        auto* tree = check.findChild<QTreeWidget*>("slot_check_list");
        if (CHECK(tree)) CHECK_EQ(tree->topLevelItemCount(), 3);
        CHECK_EQ(check.problemCount(), 1);
        // One problem in the singular
        auto* summary = check.findChild<QLabel*>("slot_check_summary");
        if (CHECK(summary)) CHECK_EQ(summary->text(), QString("3 Felder, 1 Problem."));
        if (tree && tree->topLevelItemCount() == 3) CHECK_EQ(tree->topLevelItem(1)->text(6), T("Text zu lang"));
        check.show();
        QApplication::processEvents();
        dump(&check, "slot-check.png");
    }

    // Save as Template from the window: name, tags; preview from the viewer
    {
        QTimer answer;
        answer.setInterval(20);
        QObject::connect(&answer, &QTimer::timeout, [&] {
            auto* dlg = qobject_cast<SaveTemplateDialog*>(QApplication::activeModalWidget());
            if (!dlg) return;
            answer.stop();
            dlg->findChild<QLineEdit*>("template_name")->setText("Aus dem Fenster");
            dlg->findChild<QLineEdit*>("template_tags")->setText("a, b, a");
            dlg->findChild<QCheckBox*>("template_copy_media")->setChecked(false);
            dump(dlg, "save-template.png");
            dlg->accept();
        });
        answer.start();
        w.saveContentTemplate();
        answer.stop();
        const std::optional<ContentTemplate> saved = TemplateLibrary::find("Aus dem Fenster");
        if (CHECK(saved.has_value())) {
            CHECK_EQ(saved->tags, (QStringList{"a", "b"}));
            CHECK_EQ(saved->slotIds.size(), 3);
            CHECK(saved->preview.endsWith("preview.png"));
        }
    }
    clean();
    TemplateLibrary::setBuiltInRoot({});
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("template-app");
    Theme::apply(app);
    I18n::install(qEnvironmentVariable("TEMPLATE_LANG", "de"));
    QTemporaryDir dir;
    testProjectKeepsInfo();
    testDropFill();
    testInspector();
    testBrowserAndWindow(dir.path());
    clean();
    return Check::result();
}
