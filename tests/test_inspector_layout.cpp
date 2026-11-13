// SPDX-License-Identifier: AGPL-3.0-only
// The Inspector never shows a horizontal scrollbar: every page (Video, Audio, Title, Effects, Subtitle, Transition) fits
// the panel's width, also with a large UI font (QT_FONT_DPI=144 or a large system font) and in a long language
// (Russian, Hindi at a large font), also when the
// panel is made narrow - it then stays as wide as its widest row (nothing cut off), and that width stays moderate:
// within a real user's 308 px column at the normal font (Noto Sans; with another font the limit moves with the width
// of a reference text, see forFont). Before, the rows needed ~350 px (a long clip name, long labels, choice boxes,
// fixed number fields) and the pages scrolled sideways, at a large font up to 576 px.
// INSPECTOR_LAYOUT_DUMP=<folder>: a picture per failing case (INSPECTOR_LAYOUT_ALL=1: of every case).

#include "check.h"

#include "app/Theme.h"
#include "core/Editor.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "ui/Inspector.h"

#include <QAbstractButton>
#include <QApplication>
#include <QButtonGroup>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QTest>

#include <cmath>

namespace {

void settle()
{
    for (int i = 0; i < 3; ++i) {
        QTest::qWait(20);
        QApplication::processEvents();
    }
}

void dump(QWidget* w, const QString& name)
{
    const QString dir = qEnvironmentVariable("INSPECTOR_LAYOUT_DUMP");
    if (!dir.isEmpty()) w->grab().save(dir + "/" + name + ".png");
}

// A width limit measured with Noto Sans (the main development machine's font) at `notoPt`, for the font set now: scaled
// by the width of a reference text in the language's script (its letters), so a wider font (DejaVu Sans, the fallback
// of a bare Debian) or another Noto version moves the limit with it. +2 px: rounding and hinting.
int forFont(int notoPx, double notoPt, const QString& lang)
{
    // The reference texts and their widths in Noto Sans (Devanagari) 9 pt (QFontMetricsF, offscreen)
    struct Ref {
        const char* text;
        double noto;
    };
    const Ref latin{"abcdefghijklmnopqrstuvwxyz ABCDEFGHIJKLMNOPQRSTUVWXYZ", 366.1};
    const Ref cyrillic{"абвгдежзийклмнопрстуфхцчшщъыьэюя АБВГДЕЖЗИЙКЛМНОПРСТУФХЦЧШЩЪЫЬЭЮЯ", 515.4};
    const Ref devanagari{"कखगघङचछजझञटठडढणतथदधनपफबभमयरलवशषसह कि की कु कू के कै को कौ", 358.7};
    const Ref& ref = lang == "ru" ? cyrillic : lang == "hi" ? devanagari : latin;
    const double now = QFontMetricsF(QApplication::font()).horizontalAdvance(QString::fromUtf8(ref.text));
    return int(std::ceil(notoPx * now / (ref.noto * notoPt / 9.0))) + 2;
}

void checkPages(const QString& font, const QString& lang, int maxWidth)
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
    Clip a = v;
    a.id = project.newClipId();
    a.linkId = 1;
    v.linkId = 1;
    tl.video[0].clips << v;
    tl.audio[0].clips << a;
    Clip title;
    title.id = project.newClipId();
    title.kind = ClipKind::Title;
    title.start = 200;
    title.out = 99;
    tl.video[0].clips << title;
    // A shape and a drawing (0.22): their own sections first, no text section
    Clip shape = title;
    shape.id = project.newClipId();
    shape.start = 400;
    shape.title.kind = TitleKind::Shape;
    tl.video[0].clips << shape;
    Clip drawing = title;
    drawing.id = project.newClipId();
    drawing.start = 600;
    drawing.title.kind = TitleKind::Drawing;
    tl.video[0].clips << drawing;
    project.load(ProjectData{ProjectFormat{}, 0, project.media(), {}, tl, title.id, 0});

    Inspector insp(&editor);
    insp.show();
    // Several effects: their sections (Color, Blur, Green Screen …) on the Effects page
    for (const char* fx : {"color", "blur", "chromakey", "stabilize"}) editor.addEffect({v.id}, fx);

    // A subtitle (Subtitle page) and a fade-in at the video clip's start (Transition page)
    const int cue = editor.addSubtitle(150);
    CHECK(cue > 0);
    CHECK(editor.addTransitionAt(0, v.id, TransitionStyle{}));

    auto* group = insp.findChild<QStackedWidget*>();
    if (!CHECK(group)) return;
    for (const QSet<int>& selected : {QSet<int>{v.id, a.id}, QSet<int>{title.id}, QSet<int>{shape.id}, QSet<int>{drawing.id},
                                       QSet<int>{cue}, QSet<int>{}}) {
        if (selected.isEmpty()) sel.setTransition({0, v.id});
        else sel.set(selected);
        settle();
        std::printf("%s font, %s, page %d: the Inspector needs %d px (limit %d)\n", qPrintable(font), qPrintable(lang),
                    group->currentIndex(), insp.minimumSizeHint().width(), maxWidth);
        for (QAbstractButton* tab : insp.findChildren<QAbstractButton*>()) {
            if (!tab->isCheckable() || !tab->group() || !tab->isVisible() || !tab->isEnabled()) continue;
            if (tab->group()->id(tab) < 0) continue;
            tab->click();
            for (int width : {300, 380}) {
                insp.resize(width, 900);
                settle();
                const QString where = QString("%1 font, %2, %3 px, tab %4")
                                          .arg(font, lang)
                                          .arg(width)
                                          .arg(tab->text().replace('\n', ' '));
                auto* scroll = qobject_cast<QScrollArea*>(group->currentWidget());
                if (!CHECK(scroll)) continue;
                const bool bar = scroll->horizontalScrollBar()->isVisible();
                const bool cut = scroll->widget()->width() > scroll->viewport()->width();
                if (bar || cut || qEnvironmentVariableIsSet("INSPECTOR_LAYOUT_ALL"))
                    dump(&insp, QString("%1-%2-%3-%4").arg(font, lang).arg(width).arg(group->currentIndex()));
                Check::report(!bar, "no horizontal scrollbar", __FILE__, __LINE__, where);
                // Nothing cut off on the right instead: the panel is at least as wide as its widest row
                Check::report(!cut, "page fits the panel", __FILE__, __LINE__,
                              QString("%1: page %2 px, viewport %3 px")
                                  .arg(where)
                                  .arg(scroll->widget()->width())
                                  .arg(scroll->viewport()->width()));
                // ... but not wider than needed: long labels wrap, number fields and choice boxes give a little
                Check::report(insp.minimumSizeHint().width() <= maxWidth, "Inspector not much wider", __FILE__,
                              __LINE__, QString("%1: needs %2 px").arg(where).arg(insp.minimumSizeHint().width()));
            }
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("inspector-layout");
    Theme::apply(app);
    const QFont normal = QApplication::font();
    QFont latin = normal; // the UI font itself (the families after it only fill in other scripts)
    latin.setFamilies({normal.family()});
    std::printf("font: %s %.1f pt\n", qPrintable(QFontInfo(latin).family()), normal.pointSizeF());
    QFont big = normal;
    big.setPointSizeF(normal.pointSizeF() * 1.5); // like QT_FONT_DPI=144 or a large system font
    for (const char* lang : {"de", "ru", "hi"}) {
        I18n::install(lang);
        QApplication::setFont(normal);
        // A real user's Inspector column: 308 px with Noto Sans (tests/data/tours/user-settings.conf, test_tours)
        if (QString(lang) != "hi") checkPages("normal", lang, forFont(308, 9.0, lang));
        QApplication::setFont(big);
        checkPages("large", lang, forFont(460, 13.5, lang));
    }
    QApplication::setFont(normal);
    return Check::result();
}
