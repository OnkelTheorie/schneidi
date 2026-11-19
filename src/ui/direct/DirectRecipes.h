// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// The Direct page's recipes: five cards (Short, Highlights, Gespräch ohne Pausen, Vorlage füllen, Aus Skript), each
// with at most five settings in view and the rest under "Mehr". A click on a card's title chooses it and asks for
// suggestions; "Vorschläge" asks again with the changed settings. options() gives the chosen card's settings with
// the names of the `direct` command (plan-0.25 › "Vertrag `direct`"). Defaults are neutral: no effect sounds, no
// music, no filler removal switched on by itself.

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QToolButton;

class DirectRecipes : public QWidget {
    Q_OBJECT
public:
    static const QStringList& ids(); // short, highlights, talk, template, script

    explicit DirectRecipes(QWidget* parent = nullptr);

    QString recipe() const { return m_recipe; }
    void setRecipe(const QString& id); // the card shown as chosen (no request)
    QJsonObject options() const;       // of the chosen recipe
    QJsonObject options(const QString& id) const;
    QWidget* card(const QString& id) const { return m_cards.value(id); }
    QPushButton* suggestButton() const { return m_suggest; }

    // The template card's choice (MainWindow opens TemplateBrowserDialog on templateChooseRequested)
    void setTemplate(const QString& id, const QString& name);
    QString templateId() const { return m_templateId; }
    // The script card's file instead of pasted text (empty = the text)
    void setScriptFile(const QString& path);

    // Turned off while a run is going (the cards stay readable)
    void setBusy(bool busy);

signals:
    void recipeChosen(const QString& id); // a card's title clicked: suggestions for it
    void suggestRequested();              // "Vorschläge" with the chosen card
    void templateChooseRequested();
    void scriptFileRequested();

private:
    QWidget* makeCard(const QString& id, const QString& title, const QString& text);
    void addRow(const QString& id, const QString& label, QWidget* field, bool more = false);
    void updateCards();

    QString m_recipe = "short";
    QHash<QString, QWidget*> m_cards;
    QHash<QString, QWidget*> m_cardForms; // the settings in view
    QHash<QString, QWidget*> m_cardMore;  // the settings under "Mehr"
    QHash<QString, QToolButton*> m_titles;
    QPushButton* m_suggest = nullptr;
    // short
    QComboBox* m_shortFor = nullptr;
    QSpinBox* m_shortMin = nullptr;
    QSpinBox* m_shortMax = nullptr;
    QComboBox* m_shortAspect = nullptr;
    QComboBox* m_shortCaptions = nullptr;
    QSpinBox* m_shortVariants = nullptr;
    QComboBox* m_shortSfx = nullptr;
    // highlights
    QComboBox* m_hlLength = nullptr;
    QComboBox* m_hlAspect = nullptr;
    QComboBox* m_hlMusic = nullptr;
    QComboBox* m_hlOrder = nullptr;
    QComboBox* m_hlTransitions = nullptr;
    // talk
    QDoubleSpinBox* m_talkPause = nullptr;
    QDoubleSpinBox* m_talkPad = nullptr;
    QCheckBox* m_talkFillers = nullptr;
    QCheckBox* m_talkRoomTone = nullptr;
    QComboBox* m_talkLoudness = nullptr;
    // template
    QLabel* m_templateName = nullptr;
    QCheckBox* m_templateAuto = nullptr;
    QString m_templateId;
    // script
    QPlainTextEdit* m_script = nullptr;
    QLabel* m_scriptFileLabel = nullptr;
    QString m_scriptFile;
};
