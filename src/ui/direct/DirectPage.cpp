// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/direct/DirectPage.h"

#include "app/Theme.h"
#include "core/I18n.h"
#include "core/TimelineOps.h"
#include "ui/direct/DirectMaterial.h"
#include "ui/direct/DirectRecipes.h"
#include "ui/direct/DirectSuggestions.h"
#include "ui/direct/DirectText.h"
#include "ui/direct/DirectThumbs.h"
#include "ui/timeline/TimelineOverview.h"

#include <QFileInfo>
#include <QHash>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QPushButton>
#include <QSplitter>
#include <QVBoxLayout>
#include <cmath>

namespace {
QWidget* panel(QWidget* content)
{
    auto* box = new QWidget;
    box->setObjectName("Panel");
    auto* lay = new QVBoxLayout(box);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addWidget(content);
    return box;
}
} // namespace

DirectPage::DirectPage(QWidget* parent) : QWidget(parent)
{
    setObjectName("direct_page");
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    m_root = new QSplitter(Qt::Vertical);
    m_columns = new QSplitter(Qt::Horizontal);
    m_center = new QSplitter(Qt::Vertical);
    m_material = new DirectMaterial;
    m_recipes = new DirectRecipes;
    m_suggestions = new DirectSuggestions;
    m_thumbs = new DirectThumbs(this);
    m_material->setThumbs(m_thumbs);
    m_suggestions->setThumbs(m_thumbs);

    // Middle: the viewer comes in at index 0 (MainWindow::showPage), the recipes under it
    m_center->addWidget(panel(m_recipes));
    m_center->setChildrenCollapsible(false);

    // Right: the own wish line (R5, hidden) above the suggestions
    auto* right = new QWidget;
    right->setObjectName("Panel");
    auto* rl = new QVBoxLayout(right);
    rl->setContentsMargins(0, 0, 0, 0);
    rl->setSpacing(0);
    m_wish = new QWidget;
    m_wish->setObjectName("direct_wish");
    m_wish->setLayout(new QHBoxLayout);
    m_wish->layout()->setContentsMargins(6, 4, 6, 0);
    m_wish->hide();
    rl->addWidget(m_wish);
    rl->addWidget(m_suggestions, 1);

    m_columns->addWidget(panel(m_material));
    m_columns->addWidget(m_center);
    m_columns->addWidget(right);
    m_columns->setStretchFactor(1, 1);
    m_columns->setChildrenCollapsible(false);

    // Bottom: the result
    auto* result = new QWidget;
    result->setObjectName("direct_result");
    result->setAttribute(Qt::WA_StyledBackground);
    result->setStyleSheet(QString("QWidget#direct_result { background: %1; }").arg(Theme::panelHeader.name()));
    auto* res = new QVBoxLayout(result);
    res->setContentsMargins(8, 4, 8, 6);
    res->setSpacing(4);
    m_overview = new TimelineOverview;
    m_overview->setObjectName("direct_result_strip");
    m_overview->setToolTip(T("So wird die Timeline aus den gewählten Vorschlägen"));
    res->addWidget(m_overview);
    auto* buttons = new QHBoxLayout;
    m_resultText = new QLabel;
    m_resultText->setObjectName("direct_result_text");
    m_apply = new QPushButton(T("Übernehmen"));
    m_apply->setObjectName("direct_apply");

    m_openEdit = new QPushButton(T("In Schneiden öffnen"));
    m_openEdit->setObjectName("direct_open_edit");
    m_export = new QPushButton(T("Exportieren…"));
    m_export->setObjectName("direct_export");
    buttons->addWidget(m_resultText, 1);
    buttons->addWidget(m_apply);
    buttons->addWidget(m_openEdit);
    buttons->addWidget(m_export);
    res->addLayout(buttons);

    m_root->addWidget(m_columns);
    m_root->addWidget(result);
    m_root->setStretchFactor(0, 1);
    m_root->setChildrenCollapsible(false);
    lay->addWidget(m_root);

    connect(m_recipes, &DirectRecipes::recipeChosen, this, [this] {
        if (!m_material->isEmpty()) emit suggestRequested();
    });
    connect(m_recipes, &DirectRecipes::suggestRequested, this, &DirectPage::suggestRequested);
    connect(m_suggestions, &DirectSuggestions::resuggestRequested, this, &DirectPage::suggestRequested);
    connect(m_suggestions, &DirectSuggestions::changed, this, &DirectPage::updateResult);
    connect(m_material, &DirectMaterial::changed, this, [this] {
        m_recipes->suggestButton()->setEnabled(!m_busy && !m_material->isEmpty());
    });
    connect(m_apply, &QPushButton::clicked, this, &DirectPage::applyRequested);
    connect(m_openEdit, &QPushButton::clicked, this, &DirectPage::openEditRequested);
    connect(m_export, &QPushButton::clicked, this, &DirectPage::exportRequested);
    m_recipes->suggestButton()->setEnabled(false);
    updateResult();
}

void DirectPage::setFps(double fps)
{
    if (fps > 0 && fps != m_fps) {
        m_fps = fps;
        updateResult();
    }
}

void DirectPage::setBusy(bool busy, const QString& text)
{
    m_busy = busy;
    m_recipes->setBusy(busy);
    m_recipes->suggestButton()->setEnabled(!busy && !m_material->isEmpty());
    m_suggestions->setBusyText(busy ? text : QString());
    updateResult();
}

void DirectPage::setApplying(bool applying)
{
    m_applying = applying;
    m_resultText->setText(applying ? T("Baut die Timeline …") : QString());
    updateResult();
}

Timeline DirectPage::resultTimeline() const
{
    Timeline tl;
    TimelineOps::ensureTracks(tl, TrackKind::Video, 1);
    TimelineOps::ensureTracks(tl, TrackKind::Audio, 1);
    QHash<QString, bool> picture; // file -> has a picture (unknown: yes)
    for (const QJsonValue& v : m_suggestions->session().value("material").toArray())
        picture.insert(v.toObject().value("file").toString(), v.toObject().value("has_video").toBool(true));
    int at = 0, id = 1;
    for (int n : m_suggestions->checkedOrder()) {
        const QJsonObject s = m_suggestions->suggestion(n);
        const QString file = s.value("file").toString();
        if (file.startsWith('@')) continue; // a range of the timeline itself (pauses): nothing to lay out
        const auto [from, to] = m_suggestions->range(n);
        const int in = int(std::lround(from * m_fps));
        const int out = int(std::lround(to * m_fps)) - 1;
        if (out < in) continue;
        Clip a;
        a.id = id++;
        a.mediaPath = file;
        a.start = at;
        a.in = in;
        a.out = out;
        if (picture.value(file, true)) {
            Clip v = a;
            a.id = id++;
            v.linkId = a.linkId = v.id;
            tl.video[0].clips.push_back(v);
        }
        tl.audio[0].clips.push_back(a);
        at += out - in + 1;
    }
    return tl;
}

void DirectPage::updateResult()
{
    m_overview->setTimeline(resultTimeline(), m_fps);
    const bool any = !m_suggestions->checkedOrder().isEmpty();
    m_apply->setEnabled(!m_busy && !m_applying && any);
    // What Übernehmen does for this recipe; suggestions never change the project before it
    const QString recipe = m_suggestions->session().value("recipe").toString();
    const QString what = recipe == "talk"
                             ? T("Baut eine neue Timeline: eine Kopie der aktuellen ohne die gewählten Pausen")
                         : recipe == "template" ? T("Baut eine neue Timeline aus der Vorlage mit den gewählten Clips")
                                                : T("Baut aus den gewählten Vorschlägen eine neue Timeline");
    m_apply->setToolTip(what + "\n" + T("Ein Schritt zum Rückgängigmachen. Vorschläge ändern das Projekt nicht, bis man übernimmt."));
}
