#include "app/PasteAttributesDialog.h"
#include "core/Editor.h"
#include "core/I18n.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

PasteAttributesDialog::PasteAttributesDialog(int available, const QString& source, int targetCount, QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(T("Attribute einfügen"));
    // Vorgabe wie DaVinci: nichts angehakt; danach die letzte Wahl
    const int last = QSettings().value("edit/pasteAttributes", 0).toInt();
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(targetCount == 1 ? T("Von „%1“ auf den ausgewählten Clip").arg(source)
                                                  : T("Von „%1“ auf %2 ausgewählte Clips").arg(source).arg(targetCount)));

    auto group = [&](const QString& title, std::initializer_list<std::pair<int, QString>> items) {
        auto* box = new QGroupBox(title);
        auto* v = new QVBoxLayout(box);
        for (const auto& [bit, text] : items) {
            auto* cb = new QCheckBox(text);
            cb->setEnabled(available & bit);
            cb->setChecked((available & bit) && (last & bit));
            v->addWidget(cb);
            m_boxes.insert(bit, cb);
        }
        v->addStretch();
        return box;
    };
    auto* columns = new QHBoxLayout;
    columns->addWidget(group(T("Video"), {{Editor::AttrZoom, T("Zoom")},
                                          {Editor::AttrPosition, T("Position")},
                                          {Editor::AttrRotation, T("Drehung")},
                                          {Editor::AttrCrop, T("Beschneiden")},
                                          {Editor::AttrComposite, T("Deckkraft")},
                                          {Editor::AttrEffects, T("Effekte")},
                                          {Editor::AttrColor, T("Farbkorrektur (Color)")}}));
    columns->addWidget(group(T("Audio"), {{Editor::AttrVolume, T("Lautstärke")}, {Editor::AttrPan, T("Panorama")}}));
    columns->addWidget(group(T("Allgemein"), {{Editor::AttrSpeed, T("Geschwindigkeit")},
                                              {Editor::AttrFades, T("Ein-/Ausblenden (Fades)")}}));
    layout->addLayout(columns);
    layout->addWidget(new QLabel(T("Keyframes werden mit übernommen (gleiche Zeitpunkte).")));

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    QPushButton* all = buttons->addButton(T("Alle"), QDialogButtonBox::ResetRole);
    connect(all, &QPushButton::clicked, this, [this] {
        bool any = false; // sind schon alle an, schaltet „Alle“ aus
        for (QCheckBox* cb : m_boxes)
            if (cb->isEnabled() && !cb->isChecked()) any = true;
        for (QCheckBox* cb : m_boxes)
            if (cb->isEnabled()) cb->setChecked(any);
    });
    buttons->button(QDialogButtonBox::Ok)->setText(T("Anwenden"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

int PasteAttributesDialog::attributes() const
{
    int attrs = 0;
    for (auto it = m_boxes.begin(); it != m_boxes.end(); ++it)
        if (it.value()->isEnabled() && it.value()->isChecked()) attrs |= it.key();
    return attrs;
}

void PasteAttributesDialog::accept()
{
    // Nicht verfügbare Häkchen behalten ihren gemerkten Zustand
    QSettings settings;
    int keep = settings.value("edit/pasteAttributes", 0).toInt();
    for (auto it = m_boxes.begin(); it != m_boxes.end(); ++it)
        if (it.value()->isEnabled()) keep = it.value()->isChecked() ? keep | it.key() : keep & ~it.key();
    settings.setValue("edit/pasteAttributes", keep);
    QDialog::accept();
}
