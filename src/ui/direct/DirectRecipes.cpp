// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/direct/DirectRecipes.h"

#include "app/Theme.h"
#include "core/I18n.h"
#include "ui/FlowLayout.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QJsonArray>
#include <QSpinBox>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

const QStringList& DirectRecipes::ids()
{
    static const QStringList list{"short", "highlights", "talk", "template", "script"};
    return list;
}

namespace {
QComboBox* combo(const QString& name, std::initializer_list<std::pair<QString, QVariant>> items, int current = 0)
{
    auto* c = new QComboBox;
    c->setObjectName(name);
    c->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    c->setMinimumContentsLength(6);
    for (const auto& [text, data] : items) c->addItem(text, data);
    c->setCurrentIndex(current);
    return c;
}
} // namespace

DirectRecipes::DirectRecipes(QWidget* parent) : QWidget(parent)
{
    setObjectName("direct_recipes");
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    // The recipes as one row of equal buttons (they wrap as a row of their own when the width runs out), the chosen
    // one's description and settings under them
    // The recipe row and the card scroll together: in a small window with a big font the row wraps into several
    // lines and would leave the card a sliver of its own
    auto* scroll = new QScrollArea;
    scroll->setObjectName("direct_recipe_scroll");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* inner = new QWidget;
    auto* innerLay = new QVBoxLayout(inner);
    innerLay->setContentsMargins(0, 0, 0, 0);
    innerLay->setSpacing(0);
    auto* chips = new QWidget;
    chips->setObjectName("direct_recipe_row");
    m_chips = new FlowLayout(chips, 6, 6);
    m_chips->setContentsMargins(8, 8, 8, 4);
    innerLay->addWidget(chips);
    m_stack = new QStackedWidget;
    innerLay->addWidget(m_stack, 1);
    scroll->setWidget(inner);
    outer->addWidget(scroll, 1);
    // Room for the row, the description and a few settings: the shared viewer above gives way first
    setMinimumHeight(fontMetrics().lineSpacing() * 8 + 24);

    // Short
    (makeCard("short", T("Short"), T("Die besten Momente als Hochformat-Video mit Untertiteln.")));
    m_shortFor = combo("direct_short_for", {{QString("YouTube Shorts"), "shorts"}, {QString("Instagram Reels"), "reels"},
                                            {QString("TikTok"), "tiktok"}});
    addRow("short", T("Plattform"), m_shortFor);
    {
        auto* box = new QWidget;
        auto* h = new QHBoxLayout(box);
        h->setContentsMargins(0, 0, 0, 0);
        m_shortMin = new QSpinBox;
        m_shortMin->setObjectName("direct_short_min");
        m_shortMin->setRange(5, 180);
        m_shortMin->setValue(15);
        m_shortMin->setSuffix(" s");
        m_shortMax = new QSpinBox;
        m_shortMax->setObjectName("direct_short_max");
        m_shortMax->setRange(5, 180);
        m_shortMax->setValue(45);
        m_shortMax->setSuffix(" s");
        h->addWidget(m_shortMin);
        h->addWidget(new QLabel("–"));
        h->addWidget(m_shortMax);
        h->addStretch(1);
        addRow("short", T("Länge"), box);
    }
    m_shortAspect = combo("direct_short_aspect", {{QString("9:16"), "9:16"}, {QString("1:1"), "1:1"}, {QString("4:5"), "4:5"}});
    addRow("short", T("Format"), m_shortAspect);
    m_shortCaptions = combo("direct_short_captions",
                            {{T("Karaoke"), "karaoke"}, {T("Wort für Wort"), "word"}, {T("Aus"), "off"}});
    addRow("short", T("Untertitel"), m_shortCaptions);
    m_shortVariants = new QSpinBox;
    m_shortVariants->setObjectName("direct_short_variants");
    m_shortVariants->setRange(1, 3);
    m_shortVariants->setValue(1);
    addRow("short", T("Varianten"), m_shortVariants);
    m_shortSfx = combo("direct_short_sfx", {{T("Aus"), "off"}, {T("Mild"), "mild"}});
    addRow("short", T("Effektklänge"), m_shortSfx, true);

    // Highlights
    (makeCard("highlights", T("Highlights"),
                             T("Die besten Momente aller Clips: Spiel, Urlaub, Sport. Mit Begründung je Moment.")));
    m_hlLength = combo("direct_hl_length", {{T("1 min"), 60}, {T("3 min"), 180}, {T("5 min"), 300}, {T("Alle"), "all"}});
    addRow("highlights", T("Ziellänge"), m_hlLength);
    m_hlAspect = combo("direct_hl_aspect", {{QString("16:9"), "16:9"}, {QString("9:16"), "9:16"}, {QString("1:1"), "1:1"}});
    addRow("highlights", T("Format"), m_hlAspect);
    // Music: the music bed's moods (generated, fits the length), an own file, or none
    m_hlMusic = combo("direct_hl_music", {{T("Keine"), QString()}, {T("Musikbett ruhig"), "calm"},
                                          {T("Musikbett treibend"), "drive"}, {T("Musikbett hype"), "hype"},
                                          {T("Datei…"), "file"}});
    connect(m_hlMusic, &QComboBox::activated, this, [this] {
        if (m_hlMusic->currentData().toString() == "file" && m_musicFile.isEmpty()) emit musicFileRequested();
    });
    addRow("highlights", T("Musik"), m_hlMusic);
    m_hlOrder = combo("direct_hl_order", {{T("Beste zuerst"), "best"}, {T("Zeitlich"), "time"}});
    addRow("highlights", T("Reihenfolge"), m_hlOrder);
    m_hlTransitions = combo("direct_hl_transitions", {{T("Harter Schnitt"), "cut"}, {T("Weich"), "soft"}});
    addRow("highlights", T("Übergänge"), m_hlTransitions);

    // Talk without pauses
    (makeCard("talk", T("Gespräch ohne Pausen"),
                             T("Podcast, Interview, Vlog, Tutorial: Pausen raus, alle Spuren bleiben synchron. Arbeitet auf einer "
                               "Timeline: links „Aktuelle Timeline als Material“ wählen.")));
    m_talkPause = new QDoubleSpinBox;
    m_talkPause->setObjectName("direct_talk_pause");
    m_talkPause->setRange(0.2, 5);
    m_talkPause->setSingleStep(0.1);
    m_talkPause->setDecimals(1);
    m_talkPause->setValue(0.6);
    m_talkPause->setSuffix(" s");
    addRow("talk", T("Pausen ab"), m_talkPause);
    m_talkPad = new QDoubleSpinBox;
    m_talkPad->setObjectName("direct_talk_pad");
    m_talkPad->setRange(0, 1);
    m_talkPad->setSingleStep(0.05);
    m_talkPad->setDecimals(2);
    m_talkPad->setValue(0.15);
    m_talkPad->setSuffix(" s");
    addRow("talk", T("Luft"), m_talkPad);
    m_talkFillers = new QCheckBox(T("Füllwörter raus"));
    m_talkFillers->setObjectName("direct_talk_fillers");
    m_talkFillers->setToolTip(T("Füllwörter: nur, was die Abschrift enthält."));
    addRow("talk", {}, m_talkFillers);
    m_talkRoomTone = new QCheckBox(T("Raumton in den Schnitten"));
    m_talkRoomTone->setObjectName("direct_talk_room_tone");
    addRow("talk", {}, m_talkRoomTone);
    m_talkLoudness = combo("direct_talk_loudness", {{T("Unverändert"), QVariant()}, {QString("-16 LUFS"), -16.0},
                                                    {QString("-14 LUFS"), -14.0}});
    addRow("talk", T("Lautheit"), m_talkLoudness);
    m_talkTracks = new QComboBox;
    m_talkTracks->setObjectName("direct_talk_tracks");
    m_talkTracks->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_talkTracks->setMinimumContentsLength(6);
    m_talkTracks->setToolTip(T("Eine Pause zählt nur, wenn alle diese Spuren still sind"));
    setAudioTracks({});
    addRow("talk", T("Spuren"), m_talkTracks, true);

    // Template
    (makeCard("template", T("Vorlage füllen"), T("Eine Vorlage wählen, schneidi belegt ihre Felder mit Clips.")));
    {
        auto* box = new QWidget;
        auto* h = new QHBoxLayout(box);
        h->setContentsMargins(0, 0, 0, 0);
        m_templateName = new QLabel(T("Keine gewählt"));
        m_templateName->setObjectName("direct_template_name");
        m_templateName->setWordWrap(true);
        auto* choose = new QPushButton(T("Wählen…"));
        choose->setObjectName("direct_template_choose");
        connect(choose, &QPushButton::clicked, this, &DirectRecipes::templateChooseRequested);
        h->addWidget(m_templateName, 1);
        h->addWidget(choose);
        addRow("template", T("Vorlage"), box);
    }
    m_templateAuto = new QCheckBox(T("Felder automatisch belegen"));
    m_templateAuto->setObjectName("direct_template_auto");
    m_templateAuto->setChecked(true);
    addRow("template", {}, m_templateAuto);

    // Script
    (makeCard("script", T("Aus Skript"), T("Text einfügen: schneidi sucht je Satz den besten Take.")));
    m_script = new QPlainTextEdit;
    m_script->setObjectName("direct_script_text");
    m_script->setPlaceholderText(T("Skript hier einfügen…"));
    m_script->setFixedHeight(fontMetrics().lineSpacing() * 4 + 10);
    addRow("script", {}, m_script);
    {
        auto* box = new QWidget;
        auto* h = new QHBoxLayout(box);
        h->setContentsMargins(0, 0, 0, 0);
        auto* file = new QPushButton(T("Datei…"));
        file->setObjectName("direct_script_file");
        connect(file, &QPushButton::clicked, this, &DirectRecipes::scriptFileRequested);
        m_scriptFileLabel = new QLabel;
        m_scriptFileLabel->setWordWrap(true);
        h->addWidget(file);
        h->addWidget(m_scriptFileLabel, 1);
        addRow("script", {}, box);
    }

    // The request with the chosen card's settings
    auto* bottom = new QHBoxLayout;
    bottom->setContentsMargins(8, 4, 8, 6);
    m_suggest = new QPushButton(T("Vorschläge"));
    m_suggest->setObjectName("direct_suggest");
    m_suggest->setToolTip(T("Vorschläge mit dem gewählten Rezept und seinen Einstellungen"));
    connect(m_suggest, &QPushButton::clicked, this, &DirectRecipes::suggestRequested);
    bottom->addStretch(1);
    bottom->addWidget(m_suggest);
    outer->addLayout(bottom);

    updateCards();
}

QWidget* DirectRecipes::makeCard(const QString& id, const QString& title, const QString& text)
{
    // The button in the row
    auto* chip = new QToolButton;
    chip->setObjectName("direct_recipe_" + id);
    chip->setText(title);
    chip->setCheckable(true);
    chip->setToolButtonStyle(Qt::ToolButtonTextOnly);
    chip->setToolTip(text + "\n" + T("Ein Klick wählt das Rezept und holt Vorschläge"));
    chip->setStyleSheet(QString("QToolButton { background: %1; color: %2; padding: 4px 12px; border: none; border-radius: 3px; }"
                                "QToolButton:checked { background: %3; color: %4; }"
                                "QToolButton:hover:!checked { background: %5; }")
                            .arg(Theme::control.name(), Theme::text.name(), Theme::primary.name(),
                                 Theme::onPrimary.name(), Theme::controlLight.name()) +
                        Theme::disabledToolButtonStyle());
    connect(chip, &QToolButton::clicked, this, [this, id] {
        setRecipe(id);
        emit recipeChosen(id);
    });
    m_titles.insert(id, chip);
    m_chips->addWidget(chip);
    // Its settings, shown while it is the chosen one
    auto* card = new QWidget;
    card->setObjectName("direct_card_" + id);
    auto* lay = new QVBoxLayout(card);
    lay->setContentsMargins(10, 4, 10, 8);
    lay->setSpacing(6);
    auto* desc = new QLabel(text);
    desc->setWordWrap(true);
    desc->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));
    lay->addWidget(desc);
    auto* form = new QWidget;
    auto* fl = new QFormLayout(form);
    fl->setContentsMargins(0, 0, 0, 0);
    fl->setRowWrapPolicy(QFormLayout::WrapLongRows);
    fl->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setMaximumWidth(std::max(420, fontMetrics().averageCharWidth() * 60)); // fields not stretched across a wide column
    lay->addWidget(form);
    auto* moreButton = new QToolButton;
    moreButton->setObjectName("direct_more_" + id);
    moreButton->setText(T("Mehr"));
    moreButton->setCheckable(true);
    moreButton->setArrowType(Qt::RightArrow);
    moreButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    moreButton->setAutoRaise(true);
    auto* more = new QWidget;
    more->setMaximumWidth(form->maximumWidth());
    auto* ml = new QFormLayout(more);
    ml->setContentsMargins(0, 0, 0, 0);
    ml->setRowWrapPolicy(QFormLayout::WrapLongRows);
    ml->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    more->hide();
    connect(moreButton, &QToolButton::toggled, more, [more, moreButton](bool on) {
        more->setVisible(on);
        moreButton->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
    });
    lay->addWidget(moreButton);
    lay->addWidget(more);
    lay->addStretch(1);
    m_stack->addWidget(card);
    m_cards.insert(id, card);
    m_cardForms.insert(id, form);
    m_cardMore.insert(id, more);
    return card;
}

void DirectRecipes::addRow(const QString& id, const QString& label, QWidget* field, bool more)
{
    auto* fl = static_cast<QFormLayout*>((more ? m_cardMore : m_cardForms).value(id)->layout());
    if (label.isEmpty()) fl->addRow(field);
    else fl->addRow(label, field);
}

void DirectRecipes::setRecipe(const QString& id)
{
    if (!ids().contains(id)) return;
    m_recipe = id;
    updateCards();
}

void DirectRecipes::updateCards()
{
    for (const QString& id : ids()) m_titles.value(id)->setChecked(id == m_recipe);
    m_stack->setCurrentWidget(m_cards.value(m_recipe));
    // "Mehr" only where the card has more
    for (const QString& id : ids())
        if (auto* b = m_cards.value(id)->findChild<QToolButton*>("direct_more_" + id))
            b->setVisible(m_cardMore.value(id)->layout()->count() > 0);
}

void DirectRecipes::setBusy(bool busy)
{
    m_busy = busy;
    m_suggest->setEnabled(!busy && available(m_recipe));
    for (auto it = m_titles.begin(); it != m_titles.end(); ++it) it.value()->setEnabled(!busy && available(it.key()));
}

void DirectRecipes::setAvailable(const QStringList& ids)
{
    m_coming.clear();
    for (const QString& id : DirectRecipes::ids())
        if (!ids.contains(id)) m_coming << id;
    for (auto it = m_titles.begin(); it != m_titles.end(); ++it) {
        QToolButton* t = it.value();
        const QString base = t->property("baseText").isValid() ? t->property("baseText").toString() : t->text();
        t->setProperty("baseText", base);
        t->setText(available(it.key()) ? base : T("%1 (kommt gleich)").arg(base));
    }
    if (!available(m_recipe) && !ids.isEmpty()) setRecipe(ids.first());
    for (auto it = m_titles.begin(); it != m_titles.end(); ++it) it.value()->setEnabled(!m_busy && available(it.key()));
    if (!available(m_recipe)) m_suggest->setEnabled(false); // the page enables it when there is material
}

void DirectRecipes::setTemplate(const QString& id, const QString& name)
{
    m_templateId = id;
    m_templateName->setText(id.isEmpty() ? T("Keine gewählt") : name);
}

void DirectRecipes::setMusicFile(const QString& path)
{
    m_musicFile = path;
    const int at = m_hlMusic->findData("file");
    m_hlMusic->setItemText(at, path.isEmpty() ? T("Datei…") : T("Datei: %1").arg(QFileInfo(path).fileName()));
    m_hlMusic->setItemData(at, QDir::toNativeSeparators(path), Qt::ToolTipRole);
    if (path.isEmpty() && m_hlMusic->currentIndex() == at) m_hlMusic->setCurrentIndex(0); // no file chosen: none
}

void DirectRecipes::setAudioTracks(const QStringList& names)
{
    const QString keep = m_talkTracks->currentData().toString();
    const QSignalBlocker b(m_talkTracks);
    m_talkTracks->clear();
    m_talkTracks->addItem(T("Alle Tonspuren"), QString());
    for (int i = 0; i < names.size(); ++i) m_talkTracks->addItem(names[i], QString("A%1").arg(i + 1));
    m_talkTracks->setCurrentIndex(std::max(0, m_talkTracks->findData(keep)));
}

void DirectRecipes::setScriptFile(const QString& path)
{
    m_scriptFile = path;
    m_scriptFileLabel->setText(path.isEmpty() ? QString() : QFileInfo(path).fileName());
    m_scriptFileLabel->setToolTip(QDir::toNativeSeparators(path));
}

QJsonObject DirectRecipes::options() const { return options(m_recipe); }

QJsonObject DirectRecipes::options(const QString& id) const
{
    if (id == "short")
        return {{"for", m_shortFor->currentData().toString()},
                {"min_s", std::min(m_shortMin->value(), m_shortMax->value())},
                {"max_s", std::max(m_shortMin->value(), m_shortMax->value())},
                {"aspect", m_shortAspect->currentData().toString()},
                {"captions", m_shortCaptions->currentData().toString()},
                {"variants", m_shortVariants->value()},
                {"sfx", m_shortSfx->currentData().toString()}};
    if (id == "highlights") {
        QJsonObject o{{"aspect", m_hlAspect->currentData().toString()},
                      {"order", m_hlOrder->currentData().toString()},
                      {"transitions", m_hlTransitions->currentData().toString()}};
        const QVariant len = m_hlLength->currentData();
        o["length_s"] = len.typeId() == QMetaType::QString ? QJsonValue(len.toString()) : QJsonValue(len.toInt());
        const QString music = m_hlMusic->currentData().toString();
        if (music == "file" && !m_musicFile.isEmpty()) o["music"] = QJsonObject{{"file", m_musicFile}};
        else if (music.isEmpty() || music == "file") o["music"] = QJsonValue();
        else o["music"] = QJsonObject{{"mood", music}};
        return o;
    }
    if (id == "talk") {
        QJsonObject o{{"min_pause_s", m_talkPause->value()},
                      {"pad_s", m_talkPad->value()},
                      {"fillers", m_talkFillers->isChecked()},
                      {"room_tone", m_talkRoomTone->isChecked()}};
        const QVariant lufs = m_talkLoudness->currentData();
        o["target_lufs"] = lufs.isValid() ? QJsonValue(lufs.toDouble()) : QJsonValue();
        if (const QString track = m_talkTracks->currentData().toString(); !track.isEmpty()) o["tracks"] = QJsonArray{track};
        return o;
    }
    if (id == "template") return {{"template", m_templateId}, {"auto", m_templateAuto->isChecked()}};
    if (id == "script") {
        if (!m_scriptFile.isEmpty()) return {{"script_file", m_scriptFile}};
        return {{"script", m_script->toPlainText()}};
    }
    return {};
}
