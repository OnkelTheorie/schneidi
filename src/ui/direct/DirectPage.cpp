// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/direct/DirectPage.h"

#include "app/Theme.h"
#include "core/I18n.h"
#include "core/TimelineOps.h"
#include "ui/ElidedLabel.h"
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
#include <algorithm>
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
    m_resultText = new ElidedLabel(Qt::ElideMiddle); // a long path never pushes the buttons out
    m_resultText->setObjectName("direct_result_text");
    m_resultText->setMinimumWidth(40);
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
        else if (!m_suggestions->hasSession()) m_suggestions->setHint(DirectText::needsMaterial()); // say why nothing runs
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
    if (busy) setResultNote({});
    updateResult();
}

void DirectPage::setResultNote(const QString& text) { m_resultText->setFullText(text); }

QString DirectPage::resultNote() const { return m_resultText->fullText(); }

void DirectPage::setSourceTimeline(const Timeline& tl, double fps)
{
    m_source = tl;
    if (fps > 0) m_sourceFps = fps;
    m_hasSource = true;
    updateResult();
}

void DirectPage::setApplying(bool applying, const QString& text)
{
    m_applying = applying;
    m_resultText->setFullText(applying ? (text.isEmpty() ? T("Baut die Timeline …") : text) : QString());
    if (applying) m_finish->setEnabled(false);
    updateResult();
}

void DirectPage::setMadeProject(const QString& path)
{
    m_madeProject = path;
    m_openProject->setVisible(!path.isEmpty());
    m_openProject->setToolTip(path.isEmpty() ? QString() : T("Das neue Projekt öffnen: %1").arg(QDir::toNativeSeparators(path)));
    if (!path.isEmpty()) m_resultText->setFullText(T("Neues Projekt: %1").arg(QDir::toNativeSeparators(path)));
}

void DirectPage::setFinishOffered(bool offered)
{
    m_finish->setVisible(offered);
    m_finish->setEnabled(offered && !m_wishBusy);
}

namespace {
// The timeline without the ranges (frames [from, to)), every track rippled: what cutting the pauses gives (to look
// at only; the real cut is cut_pauses)
Timeline withoutRanges(Timeline tl, QVector<QPair<int, int>> ranges, double fps)
{
    std::sort(ranges.begin(), ranges.end(), [](const auto& a, const auto& b) { return a.first > b.first; }); // last first
    int next = 1 << 24;
    const IdGen newId = [&next] { return next++; };
    for (const auto& [from, to] : ranges) {
        if (to <= from) continue;
        for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
            for (Track& t : tl.tracks(k)) {
                TimelineOps::clearRange(t, from, to, newId, fps);
                TimelineOps::shiftFrom(t, to, from - to);
            }
    }
    return tl;
}
} // namespace

Timeline DirectPage::resultTimeline() const
{
    // A session on a timeline (talk): that timeline without the checked ranges
    if (m_hasSource && m_suggestions->session().value("recipe").toString() == QLatin1String("talk")) {
        QVector<QPair<int, int>> ranges;
        for (int n : m_suggestions->checkedOrder()) {
            const auto [from, to] = m_suggestions->range(n);
            ranges.append({int(std::lround(from * m_sourceFps)), int(std::lround(to * m_sourceFps))});
        }
        return withoutRanges(m_source, ranges, m_sourceFps);
    }
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
    m_apply->setEnabled(!m_busy && !m_applying && !m_wishBusy && any);
    // What Übernehmen does for this recipe; suggestions never change the project before it
    const QString recipe = m_suggestions->session().value("recipe").toString();
    const QString what = recipe == "talk"
                             ? T("Baut eine neue Timeline: eine Kopie der aktuellen ohne die gewählten Pausen")
                         : recipe == "template" ? T("Baut eine neue Timeline aus der Vorlage mit den gewählten Clips")
                                                : T("Baut aus den gewählten Vorschlägen eine neue Timeline");
    m_apply->setToolTip(what + "\n" + T("Ein Schritt zum Rückgängigmachen. Vorschläge ändern das Projekt nicht, bis man übernimmt."));
}

QWidget* DirectPage::wishSlot() const { return m_wish; }

void DirectPage::setWishBusy(bool busy)
{
    m_wishBusy = busy;
    m_wish->setBusy(busy);
    m_finish->setEnabled(m_finish->isVisible() && !busy && !m_applying);
    updateResult();
}

namespace {
// A text as a JSON string for the AI's note: escaped, without control characters, no raw brackets (the chat wraps
// notes in [ ])
QString noteString(QString text)
{
    text.removeIf([](QChar c) { return c.category() == QChar::Other_Control || c.category() == QChar::Other_Format; });
    QString json = QString::fromUtf8(QJsonDocument(QJsonArray{text}).toJson(QJsonDocument::Compact));
    json = json.mid(1, json.size() - 2); // the string of ["…"]
    return json.replace('[', "\\u005b").replace(']', "\\u005d");
}

// A JSON value for the note: strings as noteString, arrays as (a, b), objects as {k: v}
QString noteValue(const QJsonValue& v)
{
    if (v.isString()) return noteString(v.toString());
    if (v.isArray()) {
        QStringList parts;
        for (const QJsonValue& x : v.toArray()) parts << noteValue(x);
        return '(' + parts.join(", ") + ')';
    }
    if (v.isObject()) {
        QStringList parts;
        const QJsonObject o = v.toObject();
        for (auto it = o.begin(); it != o.end(); ++it) parts << noteString(it.key()) + ": " + noteValue(it.value());
        return '{' + parts.join(", ") + '}';
    }
    if (v.isNull() || v.isUndefined()) return "null";
    if (v.isBool()) return v.toBool() ? "true" : "false";
    return QString::number(v.toDouble(), 'g', 10);
}
} // namespace

QString DirectPage::wishContext(bool plan, int maxChars) const
{
    const auto num = [](double s) { return QString::number(s, 'f', 1); };
    const QJsonObject session = m_suggestions->session();
    const QString recipe = session.isEmpty() ? m_recipes->recipe() : session.value("recipe").toString();
    const QJsonObject options = session.isEmpty() ? m_recipes->options() : session.value("options").toObject();
    const QString id = session.value("id").toString();

    // How to answer (lists written as <list of …>: no raw brackets in a note)
    QString text = "the user typed the wish below on the Direct page (\"Regie\"). Answer it by calling the tool `direct`, "
                   "not with text only: best with a new suggestion list (direct project:'@app', recipe, files (folder + "
                   "name) or timeline, changed options) - it replaces the page's list; or apply a choice of this session "
                   "(direct project:'@app', session:'<id>', apply:{on:<list of n>, order:<list of n>, adjust:<list of "
                   "{n, from_s, to_s}>"
                   + QString(recipe == QLatin1String("short") ? ", variant" : "") + "}"
                   + QString(plan ? ", plan:true - the user wants every change as a plan first" : "")
                   + "). Then say in one short sentence what you changed.\n";
    QString opts = noteValue(options);
    if (opts.size() > kWishOptionsChars) opts = opts.left(kWishOptionsChars - 1) + "…";
    const QString recipeLine = QString("Recipe: %1, options %2\n").arg(noteString(recipe), opts);
    const QList<int> rows = m_suggestions->rowOrder();
    QString sessionLine;
    if (!id.isEmpty()) {
        sessionLine = QString("Session %1: %2 suggestions as the page lists them (the checked ones are the result's "
                              "order):\n")
                          .arg(noteString(id))
                          .arg(rows.size());
        if (m_suggestions->variantCount() > 0)
            sessionLine += QString("Variant %1 of %2 is shown (the list has only its rows)\n")
                               .arg(m_suggestions->variant())
                               .arg(m_suggestions->variantCount());
        sessionLine += "n kind file from-to s reason on/off\n";
    } else {
        sessionLine = "No suggestions yet.\n";
    }

    // Material: names (with their lengths, the session's) under their folder once, numbered for the suggestions;
    // never more than the room the head, recipe and session lines leave (minus a few suggestion lines)
    QHash<QString, double> lengths;
    for (const QJsonValue& v : session.value("material").toArray())
        lengths.insert(v.toObject().value("file").toString(), v.toObject().value("length_s").toDouble(-1));
    QHash<QString, QString> fileKey; // listed file -> "f1"
    QString material;
    if (m_material->usesTimeline() || !session.value("timeline").toString().isEmpty()) {
        material = "Material: the current timeline (timeline:'@current')\n";
    } else {
        const QStringList files = m_material->isEmpty() ? QStringList() : m_material->files();
        int budget = std::min<int>(maxChars * 2 / 5, maxChars - int(text.size() + recipeLine.size() + sessionLine.size()) - 250);
        QStringList groups;
        QString folder, group;
        int left = 0;
        for (int i = 0; i < files.size(); ++i) {
            const QFileInfo fi(files[i]);
            const QString dir = QDir::toNativeSeparators(fi.absolutePath());
            const QString key = QString("f%1").arg(fileKey.size() + 1);
            const double len = lengths.value(files[i], -1);
            QString part = key + '=' + noteString(fi.fileName()) + (len >= 0 ? QString(" (%1 s)").arg(num(len)) : QString());
            const bool newFolder = group.isEmpty() || dir != folder;
            const QString head = newFolder ? "folder " + noteString(dir) + ": " : QString();
            const int cost = int(head.size() + part.size()) + 2;
            if (budget - cost < 0) {
                ++left;
                continue;
            }
            budget -= cost;
            if (newFolder) {
                if (!group.isEmpty()) groups << group;
                folder = dir;
                group = head + part;
            } else {
                group += ", " + part;
            }
            fileKey.insert(files[i], key);
        }
        if (!group.isEmpty()) groups << group;
        material = "Material: " + (groups.isEmpty() ? QString("none listed") : groups.join("; "));
        if (left > 0) material += QString(" (+%1 more files not listed)").arg(left);
        material += '\n';
    }
    text += material + recipeLine + sessionLine;
    if (id.isEmpty()) return text;

    const QList<int> checked = m_suggestions->checkedOrder();
    int listed = 0;
    for (int n : rows) {
        const QJsonObject s = m_suggestions->suggestion(n);
        const QString file = s.value("file").toString();
        const auto [from, to] = m_suggestions->range(n);
        const QString fileText = fileKey.contains(file) ? fileKey.value(file)
                                 : file.startsWith('@') ? file
                                                        : noteString(QFileInfo(file).fileName());
        QString line = QString("%1 %2 %3 %4-%5 %6 %7")
                           .arg(n)
                           .arg(noteString(s.value("kind").toString()).remove('"'), fileText, num(from), num(to),
                                noteString(s.value("reason").toObject().value("key").toString()).remove('"'),
                                checked.contains(n) ? "on" : "off");
        if (s.contains("variant")) line += QString(" v%1").arg(s.value("variant").toInt());
        if (const QJsonValue t = s.value("track"); !t.isUndefined()) line += " track " + noteValue(t);
        line += '\n';
        // Room for the last line that counts the rest
        if (text.size() + line.size() + 40 > maxChars) break;
        text += line;
        ++listed;
    }
    if (listed < rows.size()) text += QString("(+%1 more suggestions not listed)\n").arg(rows.size() - listed);
    return text;
}
