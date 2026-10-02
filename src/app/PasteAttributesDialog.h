#pragma once
#include <QDialog>
#include <QHash>

class QCheckBox;

// „Attribute einfügen“ (Alt+V) wie DaVinci Paste Attributes: Häkchen je Eigenschaft (Video/Audio/Allgemein),
// nicht verfügbare (keine Quelle dieser Art kopiert) sind ausgegraut. Letzte Wahl wird gemerkt.
class PasteAttributesDialog : public QDialog {
    Q_OBJECT
public:
    // available = Editor::pasteAttributesAvailable(), source = Name des kopierten Clips
    PasteAttributesDialog(int available, const QString& source, int targetCount, QWidget* parent = nullptr);
    int attributes() const; // Editor::PasteAttr-Bits

    void accept() override;

private:
    QHash<int, QCheckBox*> m_boxes;
};
