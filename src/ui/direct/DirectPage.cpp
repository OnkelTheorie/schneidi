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
#include "ui/direct/DirectWish.h"
#include "ui/timeline/TimelineOverview.h"

#include <QFileInfo>
#include <QHash>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QDir>
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

    // Right: the own wish line (hidden until MainWindow sees a chat provider) above the suggestions
    auto* right = new QWidget;
    right->setObjectName("Panel");
    auto* rl = new QVBoxLayout(right);
    rl->setContentsMargins(0, 0, 0, 0);
    rl->setSpacing(0);
    m_wish = new DirectWish;
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

    // After a short is built: its captions and effect sounds, a step of their own (shown only then)
    m_finish = new QPushButton(T("Untertitel + Effektklänge"));
    m_finish->setObjectName("direct_finish");
    m_finish->setToolTip(T("Untertitel aus der Sprache (mit Whisper) und die gewählten Effektklänge auf den eben gebauten "
                           "Short. Ein eigener Schritt zum Rückgängigmachen."));
    m_finish->hide();
    // A template filled into a project of its own: that file, opened like File › Open
    m_openProject = new QPushButton(T("Öffnen"));
    m_openProject->setObjectName("direct_open_project");
    m_openProject->hide();

    m_openEdit = new QPushButton(T("In Schneiden öffnen"));
    m_openEdit->setObjectName("direct_open_edit");
    m_export = new QPushButton(T("Exportieren…"));
    m_export->setObjectName("direct_export");
    buttons->addWidget(m_resultText, 1);
    buttons->addWidget(m_apply);
    buttons->addWidget(m_finish);
    buttons->addWidget(m_openProject);
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
        m_recipes->suggestButton()->setEnabled(!m_busy && !m_material->isEmpty() && m_recipes->available(m_recipes->recipe()));
    });
    connect(m_apply, &QPushButton::clicked, this, &DirectPage::applyRequested);
    connect(m_openEdit, &QPushButton::clicked, this, &DirectPage::openEditRequested);
    connect(m_finish, &QPushButton::clicked, this, &DirectPage::finishRequested);
    connect(m_openProject, &QPushButton::clicked, this, [this] { emit openProjectRequested(m_madeProject); });
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
    m_recipes->suggestButton()->setEnabled(!busy && !m_material->isEmpty() && m_recipes->available(m_recipes->recipe()));
    m_suggestions->setBusyText(busy ? text : QString());
    updateResult();
}

void DirectPage::setApplying(bool applying, const QString& text)
{
    m_applying = applying;
    m_resultText->setText(applying ? (text.isEmpty() ? T("Baut die Timeline …") : text) : QString());
    if (applying) m_finish->setEnabled(false);
    updateResult();
}

void DirectPage::setMadeProject(const QString& path)
{
    m_madeProject = path;
    m_openProject->setVisible(!path.isEmpty());
    m_openProject->setToolTip(path.isEmpty() ? QString() : T("Das neue Projekt öffnen: %1").arg(QDir::toNativeSeparators(path)));
    if (!path.isEmpty()) m_resultText->setText(T("Neues Projekt: %1").arg(QFileInfo(path).fileName()));
}

void DirectPage::setFinishOffered(bool offered)
{
    m_finish->setVisible(offered);
    m_finish->setEnabled(offered);
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

QWidget* DirectPage::wishSlot() const { return m_wish; }

QString DirectPage::wishContext(int maxChars) const
{
    const auto num = [](double s) { return QString::number(s, 'f', 1); };
    const QJsonObject session = m_suggestions->session();
    const QString recipe = session.isEmpty() ? m_recipes->recipe() : session.value("recipe").toString();
    const QJsonObject options = session.isEmpty() ? m_recipes->options() : session.value("options").toObject();
    const QString id = session.value("id").toString();

    QString head = "the user typed the wish below on the Direct page (\"Regie\"). Answer it by calling the tool `direct`, "
                   "not with text only: best with a new suggestion list (direct project:'@app', recipe, files or "
                   "timeline, changed options) - it replaces the page's list; or apply a choice of this session "
                   "(direct project:'@app', session:'<id>', apply:{on:[n…], order:[n…], adjust:[{n, from_s, to_s}]"
                   + QString(recipe == QLatin1String("short") ? ", variant" : "")
                   + "}). Then say in one short sentence what you changed.\n";

    // Material: files with their lengths (the session's), numbered for the suggestions below
    QHash<QString, double> lengths;
    for (const QJsonValue& v : session.value("material").toArray())
        lengths.insert(v.toObject().value("file").toString(), v.toObject().value("length_s").toDouble(-1));
    QHash<QString, QString> fileKey; // file -> "f1"
    QString material;
    if (m_material->usesTimeline() || !session.value("timeline").toString().isEmpty()) {
        material = "Material: the current timeline (timeline:'@current')\n";
    } else {
        const QStringList files = m_material->isEmpty() ? QStringList() : m_material->files();
        QStringList parts;
        int left = 0, budget = maxChars * 2 / 5; // the material never takes the room of the suggestions
        for (int i = 0; i < files.size(); ++i) {
            const QString key = QString("f%1").arg(i + 1);
            fileKey.insert(files[i], key);
            const double len = lengths.value(files[i], -1);
            const QString part = key + '=' + files[i] + (len >= 0 ? QString(" (%1 s)").arg(num(len)) : QString());
            if (budget - part.size() - 2 < 0) {
                ++left;
                continue;
            }
            budget -= part.size() + 2;
            parts << part;
        }
        material = "Material: " + (parts.isEmpty() ? QString("none yet") : parts.join(", "));
        if (left > 0) material += QString(" (+%1 more files not listed)").arg(left);
        material += '\n';
    }
    const QString opts = QString::fromUtf8(QJsonDocument(options).toJson(QJsonDocument::Compact));
    QString text = head + material + QString("Recipe: %1, options %2\n").arg(recipe, opts);
    if (id.isEmpty()) return (text + "No suggestions yet.").left(maxChars);

    const QList<int> rows = m_suggestions->rowOrder();
    const QList<int> checked = m_suggestions->checkedOrder();
    text += QString("Session %1: %2 suggestions as the page lists them (the checked ones are the result's order):\n"
                    "n kind file from-to s reason on/off\n")
                .arg(id)
                .arg(rows.size());
    if (m_suggestions->variantCount() > 0)
        text += QString("Variant %1 of %2 is shown (the list has only its rows)\n").arg(m_suggestions->variant()).arg(m_suggestions->variantCount());
    int listed = 0;
    for (int n : rows) {
        const QJsonObject s = m_suggestions->suggestion(n);
        const QString file = s.value("file").toString();
        const auto [from, to] = m_suggestions->range(n);
        QString line = QString("%1 %2 %3 %4-%5 %6 %7")
                           .arg(n)
                           .arg(s.value("kind").toString(), fileKey.value(file, file.startsWith('@') ? file : QFileInfo(file).fileName()),
                                num(from), num(to), s.value("reason").toObject().value("key").toString(),
                                checked.contains(n) ? "on" : "off");
        if (s.contains("variant")) line += QString(" v%1").arg(s.value("variant").toInt());
        if (const QJsonValue t = s.value("track"); !t.isUndefined())
            line += " track " + (t.isArray() ? QString::fromUtf8(QJsonDocument(t.toArray()).toJson(QJsonDocument::Compact))
                                             : t.toVariant().toString());
        line += '\n';
        // Room for the last line that counts the rest
        if (text.size() + line.size() + 40 > maxChars) break;
        text += line;
        ++listed;
    }
    if (listed < rows.size()) text += QString("(+%1 more suggestions not listed)\n").arg(rows.size() - listed);
    return text.left(maxChars);
}
