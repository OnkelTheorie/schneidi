#include "ui/Inspector.h"

#include "core/Editor.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/Timecode.h"
#include "core/TimelineOps.h"

#include <QFileInfo>
#include <QLabel>
#include <QVBoxLayout>

Inspector::Inspector(Editor* editor, QWidget* parent) : QWidget(parent), m_editor(editor)
{
    setObjectName("Panel");
    auto* title = new QLabel("Inspector");
    title->setObjectName("PanelTitle");

    m_body = new QLabel;
    m_body->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_body->setWordWrap(true);
    m_body->setMargin(10);
    m_body->setTextFormat(Qt::RichText);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addWidget(title);
    lay->addWidget(m_body, 1);

    connect(editor->selection(), &Selection::changed, this, &Inspector::refresh);
    connect(editor->project(), &Project::timelineChanged, this, &Inspector::refresh);
    refresh();
}

void Inspector::refresh()
{
    const auto& ids = m_editor->selection()->ids();
    if (ids.isEmpty()) {
        m_body->setText("<span style='color:#8c8c94'>Kein Clip ausgewählt</span>");
        return;
    }
    const int fps = m_editor->project()->fps();
    const Timeline& tl = m_editor->project()->timeline();
    QString html;
    for (int id : ids) {
        TrackRef ref;
        const Clip* c = TimelineOps::findClip(tl, id, &ref);
        if (!c) continue;
        html += QString("<b>%1</b> <span style='color:#8c8c94'>(%2%3)</span><br>"
                        "Start: %4<br>Dauer: %5<br>Quelle: %6 – %7<br><br>")
                    .arg(QFileInfo(c->mediaPath).fileName().toHtmlEscaped(),
                         ref.kind == TrackKind::Video ? "V" : "A", QString::number(ref.index + 1),
                         Timecode::format(c->start, fps), Timecode::format(c->length(), fps),
                         Timecode::format(c->in, fps), Timecode::format(c->out, fps));
    }
    m_body->setText(html);
}
