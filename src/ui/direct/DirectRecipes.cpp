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
#include <QSpinBox>
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
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* inner = new QWidget;
    auto* flow = new FlowLayout(inner, 8, 8);
    flow->setContentsMargins(8, 8, 8, 8);
    scroll->setWidget(inner);
    outer->addWidget(scroll, 1);

    // Short
    flow->addWidget(makeCard("short", T("Short"), T("Die besten Momente als Hochformat-Video mit Untertiteln.")));
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
    flow->addWidget(makeCard("highlights", T("Highlights"),
                             T("Die besten Momente aller Clips: Spiel, Urlaub, Sport. Mit Begründung je Moment.")));
    m_hlLength = combo("direct_hl_length", {{T("1 min"), 60}, {T("3 min"), 180}, {T("5 min"), 300}, {T("Alle"), "all"}});
    addRow("highlights", T("Ziellänge"), m_hlLength);
    m_hlAspect = combo("direct_hl_aspect", {{QString("16:9"), "16:9"}, {QString("9:16"), "9:16"}, {QString("1:1"), "1:1"}});
    addRow("highlights", T("Format"), m_hlAspect);
    m_hlMusic = combo("direct_hl_music", {{T("Keine"), QString()}, {T("Musikbett ruhig"), "calm"},
                                          {T("Musikbett treibend"), "driving"}, {T("Musikbett hype"), "hype"}});
    addRow("highlights", T("Musik"), m_hlMusic);
    m_hlOrder = combo("direct_hl_order", {{T("Beste zuerst"), "best"}, {T("Zeitlich"), "time"}});
    addRow("highlights", T("Reihenfolge"), m_hlOrder);
    m_hlTransitions = combo("direct_hl_transitions", {{T("Harter Schnitt"), "cut"}, {T("Weich"), "soft"}});
    addRow("highlights", T("Übergänge"), m_hlTransitions);

    // Talk without pauses
    flow->addWidget(makeCard("talk", T("Gespräch ohne Pausen"),
                             T("Podcast, Interview, Vlog, Tutorial: Pausen raus, alle Spuren bleiben synchron.")));
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

    // Template
    flow->addWidget(makeCard("template", T("Vorlage füllen"), T("Eine Vorlage wählen, schneidi belegt ihre Felder mit Clips.")));
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
    flow->addWidget(makeCard("script", T("Aus Skript"), T("Text einfügen: schneidi sucht je Satz den besten Take.")));
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

    // "Mehr" only where a card has more
    for (const QString& id : ids())
        if (QWidget* more = m_cardMore.value(id); more && more->layout()->count() == 0)
            if (auto* b = m_cards.value(id)->findChild<QToolButton*>("direct_more_" + id)) b->hide();
    updateCards();
}

QWidget* DirectRecipes::makeCard(const QString& id, const QString& title, const QString& text)
{
    auto* card = new QFrame;
    card->setObjectName("direct_card_" + id);
    card->setFrameShape(QFrame::StyledPanel);
    // As wide as about 34 letters of the font (a large font or a long language: wider cards, fewer per row)
    card->setFixedWidth(std::max(220, fontMetrics().averageCharWidth() * 34));
    auto* lay = new QVBoxLayout(card);
    lay->setContentsMargins(8, 6, 8, 8);
    lay->setSpacing(4);
    auto* head = new QToolButton;
    head->setObjectName("direct_recipe_" + id);
    head->setText(title);
    head->setCheckable(true);
    head->setToolButtonStyle(Qt::ToolButtonTextOnly);
    head->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    QFont f = head->font();
    f.setBold(true);
    head->setFont(f);
    head->setToolTip(T("Dieses Rezept wählen und Vorschläge holen"));
    connect(head, &QToolButton::clicked, this, [this, id] {
        setRecipe(id);
        emit recipeChosen(id);
    });
    m_titles.insert(id, head);
    lay->addWidget(head);
    auto* desc = new QLabel(text);
    desc->setWordWrap(true);
    desc->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));
    lay->addWidget(desc);
    auto* form = new QWidget;
    auto* fl = new QFormLayout(form);
    fl->setContentsMargins(0, 0, 0, 0);
    fl->setRowWrapPolicy(QFormLayout::WrapLongRows);
    fl->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    lay->addWidget(form);
    auto* moreButton = new QToolButton;
    moreButton->setObjectName("direct_more_" + id);
    moreButton->setText(T("Mehr"));
    moreButton->setCheckable(true);
    moreButton->setArrowType(Qt::RightArrow);
    moreButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    moreButton->setAutoRaise(true);
    auto* more = new QWidget;
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
    for (const QString& id : ids()) {
        const bool on = id == m_recipe;
        m_titles.value(id)->setChecked(on);
        m_cards.value(id)->setStyleSheet(QString("QFrame#direct_card_%1 { border: %2px solid %3; border-radius: 4px; }")
                                             .arg(id)
                                             .arg(on ? 2 : 1)
                                             .arg((on ? Theme::primary : Theme::controlLight).name()));
    }
}

void DirectRecipes::setBusy(bool busy)
{
    m_suggest->setEnabled(!busy);
    for (QToolButton* t : std::as_const(m_titles)) t->setEnabled(!busy);
}

void DirectRecipes::setTemplate(const QString& id, const QString& name)
{
    m_templateId = id;
    m_templateName->setText(id.isEmpty() ? T("Keine gewählt") : name);
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
        const QString mood = m_hlMusic->currentData().toString();
        o["music"] = mood.isEmpty() ? QJsonValue() : QJsonValue(QJsonObject{{"mood", mood}});
        return o;
    }
    if (id == "talk") {
        QJsonObject o{{"min_pause_s", m_talkPause->value()},
                      {"pad_s", m_talkPad->value()},
                      {"fillers", m_talkFillers->isChecked()},
                      {"room_tone", m_talkRoomTone->isChecked()}};
        const QVariant lufs = m_talkLoudness->currentData();
        o["target_lufs"] = lufs.isValid() ? QJsonValue(lufs.toDouble()) : QJsonValue();
        return o;
    }
    if (id == "template") return {{"template", m_templateId}, {"auto", m_templateAuto->isChecked()}};
    if (id == "script") {
        if (!m_scriptFile.isEmpty()) return {{"script_file", m_scriptFile}};
        return {{"script", m_script->toPlainText()}};
    }
    return {};
}
