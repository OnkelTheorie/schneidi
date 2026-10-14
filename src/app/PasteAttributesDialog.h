#pragma once
#include <QDialog>
#include <QHash>

class QCheckBox;

// "Paste Attributes" (Alt+V) like DaVinci Paste Attributes: checkbox per property (Video/Audio/General),
// unavailable ones (no source of this kind copied) are grayed out. Last choice is remembered.
class PasteAttributesDialog : public QDialog {
    Q_OBJECT
public:
    // available = Editor::pasteAttributesAvailable(), source = name of the copied clip
    PasteAttributesDialog(int available, const QString& source, int targetCount, QWidget* parent = nullptr);
    int attributes() const; // Editor::PasteAttr-Bits

    void accept() override;

private:
    QHash<int, QCheckBox*> m_boxes;
};
