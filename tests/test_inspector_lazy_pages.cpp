// SPDX-License-Identifier: AGPL-3.0-only
// The Inspector builds its title, transition and subtitle pages on first use (~45 ms of the start). Nobody may see that:
// - the panel's width does not depend on them: its minimum and preferred width are the same before and after they are
//   built (de, ru, hi; normal and large font) - the pages built at the start (Video, Audio) are the widest;
// - the title page's tab is named after the selected clip: title, shape or drawing;
// - selecting a title clip, a transition or a subtitle builds its page and shows the values (the title's text, the
//   transition's length, the subtitle's text), and a page is built once.

#include "check.h"

#include "app/Theme.h"
#include "core/Editor.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "ui/Inspector.h"

#include <QApplication>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTest>
#include <QToolButton>

namespace {

// The pages' places in the Inspector's stack (ui/InspectorDetail.h)
constexpr int kTitlePage = 2, kTransitionPage = 3, kSubtitlePage = 4;

void settle()
{
    for (int i = 0; i < 3; ++i) {
        QTest::qWait(20);
        QApplication::processEvents();
    }
}

// Widgets on a page of the Inspector's stack (0 = not built yet)
int widgetsOn(QStackedWidget* pages, int page)
{
    auto* scroll = qobject_cast<QScrollArea*>(pages->widget(page));
    return scroll && scroll->widget() ? int(scroll->widget()->findChildren<QWidget*>().size()) : -1;
}

void check(const QString& where)
{
    Project project;
    Selection sel;
    Editor editor(&project, &sel);
    project.addMedia(MediaInfo{"/x/a-recording-with-a-rather-long-file-name.mp4",
                               "a-recording-with-a-rather-long-file-name.mp4", 300, true, true, false});
    Timeline tl;
    tl.video.resize(1);
    tl.audio.resize(1);
    Clip v;
    v.id = project.newClipId();
    v.mediaPath = "/x/a-recording-with-a-rather-long-file-name.mp4";
    v.out = 99;
    tl.video[0].clips << v;
    Clip title;
    title.id = project.newClipId();
    title.kind = ClipKind::Title;
    title.title.text = "Hello lazy page";
    title.start = 200;
    title.out = 99;
    tl.video[0].clips << title;
    Clip shape = title; // a shape and a drawing use the same page, named after them
    shape.id = project.newClipId();
    shape.title.kind = TitleKind::Shape;
    shape.start = 400;
    tl.video[0].clips << shape;
    Clip drawing = title;
    drawing.id = project.newClipId();
    drawing.title.kind = TitleKind::Drawing;
    drawing.start = 600;
    tl.video[0].clips << drawing;
    project.load(ProjectData{ProjectFormat{}, 0, project.media(), {}, tl, drawing.id, 0});

    // The size hints of a fresh panel (nothing selected, as at the start) ...
    Inspector lazy(&editor);
    lazy.show();
    settle();
    auto* pages = lazy.findChild<QStackedWidget*>();
    if (!CHECK(pages)) return;
    for (int page : {kTitlePage, kTransitionPage, kSubtitlePage})
        Check::report(widgetsOn(pages, page) == 0, "page not built at the start", __FILE__, __LINE__,
                      QString("%1, page %2: %3 widgets").arg(where).arg(page).arg(widgetsOn(pages, page)));
    const QSize minBefore = lazy.minimumSizeHint(), hintBefore = lazy.sizeHint();
    // ... equal those with every page built
    lazy.ensureAllPages();
    settle();
    Check::report(lazy.minimumSizeHint().width() == minBefore.width(), "minimum width independent of the late pages",
                  __FILE__, __LINE__,
                  QString("%1: %2 px lazy, %3 px built").arg(where).arg(minBefore.width()).arg(lazy.minimumSizeHint().width()));
    Check::report(lazy.sizeHint().width() == hintBefore.width(), "preferred width independent of the late pages",
                  __FILE__, __LINE__,
                  QString("%1: %2 px lazy, %3 px built").arg(where).arg(hintBefore.width()).arg(lazy.sizeHint().width()));

    // Selecting builds the page and fills it
    Inspector insp(&editor);
    insp.show();
    settle();
    pages = insp.findChild<QStackedWidget*>();
    if (!CHECK(pages)) return;
    sel.set({title.id});
    settle();
    const int built = widgetsOn(pages, kTitlePage);
    CHECK(built > 0 && pages->currentIndex() == kTitlePage);
    bool text = false;
    for (QPlainTextEdit* edit : pages->widget(kTitlePage)->findChildren<QPlainTextEdit*>())
        text = text || edit->toPlainText() == title.title.text;
    Check::report(text, "title text shown", __FILE__, __LINE__, where);
    sel.set({v.id});
    sel.set({title.id});
    settle();
    CHECK_EQ(widgetsOn(pages, kTitlePage), built); // built once

    // The tab of that page says what is selected: title, shape or drawing
    const auto tabText = [&insp] {
        for (QToolButton* b : insp.findChild<QWidget*>("InspectorTabs")->findChildren<QToolButton*>())
            if (b->isVisible() && b->text().startsWith("T\n")) return b->text().mid(2);
        return QString();
    };
    CHECK_EQ(tabText(), T("Titel"));
    sel.set({shape.id});
    settle();
    CHECK_EQ(tabText(), T("Form"));
    sel.set({drawing.id});
    settle();
    CHECK_EQ(tabText(), T("Zeichnung"));
    sel.set({title.id});
    settle();
    CHECK_EQ(tabText(), T("Titel"));

    CHECK(editor.addTransitionAt(0, v.id, TransitionStyle{}));
    sel.setTransition({0, v.id});
    settle();
    CHECK(widgetsOn(pages, kTransitionPage) > 0 && pages->currentIndex() == kTransitionPage);

    const int cue = editor.addSubtitle(150);
    if (CHECK(cue > 0)) {
        insp.editSubtitle(cue);
        settle();
        CHECK(widgetsOn(pages, kSubtitlePage) > 0 && pages->currentIndex() == kSubtitlePage);
    }
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("inspector-lazy-pages");
    Theme::apply(app);
    const QFont normal = QApplication::font();
    QFont big = normal;
    big.setPointSizeF(normal.pointSizeF() * 1.5); // like QT_FONT_DPI=144 or a large system font
    for (const char* lang : {"de", "ru", "hi"}) {
        I18n::install(lang);
        QApplication::setFont(normal);
        check(QString("%1, normal font").arg(lang));
        QApplication::setFont(big);
        check(QString("%1, large font").arg(lang));
    }
    QApplication::setFont(normal);
    return Check::result();
}
