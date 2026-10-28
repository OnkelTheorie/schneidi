// SPDX-License-Identifier: AGPL-3.0-only
#include "app/KeyBindingsDialog.h"

#include "app/InputBindings.h"
#include "app/Theme.h"
#include "core/I18n.h"

#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QKeyEvent>
#include <QMouseEvent>
#include <functional>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QMenu>
#include <QMessageBox>
#include <QSettings>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {
enum Column { ColCategory, ColText, ColSlot1, ColSlot2, ColCount };

// Captures the next key (with modifiers) or mouse side button, like a QKeySequenceEdit that also takes the mouse.
// Esc or clicking away = cancel.
class InputCapture : public QLabel {
public:
    explicit InputCapture(std::function<void(const QString&, bool)> done) : m_done(std::move(done))
    {
        setText(T("Taste oder Maustaste drücken …"));
        setFocusPolicy(Qt::StrongFocus);
        setStyleSheet(QString("color: %1; padding-left: 4px;").arg(Theme::primary.name()));
        setToolTip(T("Taste, Tastenkombination oder Seitentaste der Maus drücken. Esc = abbrechen"));
    }

protected:
    bool event(QEvent* e) override
    {
        // Take every key, also Tab and keys that are shortcuts somewhere else
        if (e->type() == QEvent::ShortcutOverride) {
            e->accept();
            return true;
        }
        if (e->type() == QEvent::KeyPress) {
            keyPress(static_cast<QKeyEvent*>(e));
            return true;
        }
        return QLabel::event(e);
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        e->accept();
        if (InputBindings::bindableMouseButtons().contains(e->button()))
            finish(InputBindings::mouseInput(e->button()), true);
        else if (e->button() != Qt::LeftButton) setText(T("Bei der Maus nur Seitentasten/Zusatztasten"));
    }
    void mouseDoubleClickEvent(QMouseEvent* e) override { mousePressEvent(e); }
    void focusOutEvent(QFocusEvent*) override { finish({}, false); }

private:
    void keyPress(QKeyEvent* e)
    {
        int key = e->key();
        const Qt::KeyboardModifiers mods =
            e->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
        if (key == Qt::Key_Escape && mods == Qt::NoModifier) return finish({}, false);
        if (key == Qt::Key_Shift || key == Qt::Key_Control || key == Qt::Key_Alt || key == Qt::Key_Meta
            || key == Qt::Key_AltGr || key == Qt::Key_unknown || key == 0) {
            // Only modifiers so far: show them and wait for the key
            setText(QKeySequence(mods.toInt()).toString(QKeySequence::NativeText) + " …");
            return;
        }
        if (key == Qt::Key_Backtab) key = Qt::Key_Tab; // Shift+Tab arrives as Backtab
        finish(QKeySequence(QKeyCombination(mods, Qt::Key(key))).toString(QKeySequence::PortableText), true);
    }
    void finish(const QString& input, bool ok)
    {
        if (!m_done) return;
        auto done = std::move(m_done);
        m_done = nullptr;
        done(input, ok); // ok = false: change nothing
    }
    std::function<void(const QString&, bool)> m_done;
};
}

namespace {
// Folder of the last loaded/saved set, else the config folder (where keybindings.json lives)
QString setDir()
{
    const QString dir = QSettings().value("keys/setDir").toString();
    return !dir.isEmpty() && QFileInfo(dir).isDir() ? dir : QFileInfo(InputBindings::instance().filePath()).absolutePath();
}
} // namespace

KeyBindingsDialog::KeyBindingsDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle(T("Tastenbelegung"));
    resize(780, 640);

    m_filter = new QLineEdit;
    m_filter->setPlaceholderText(T("Suchen (Funktion oder Taste) …"));
    m_filter->setClearButtonEnabled(true);
    connect(m_filter, &QLineEdit::textChanged, this, &KeyBindingsDialog::applyFilter);

    m_table = new QTableWidget(0, ColCount);
    m_table->setHorizontalHeaderLabels({T("Menü"), T("Funktion"), T("Taste 1"), T("Taste 2")});
    m_table->verticalHeader()->hide();
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->horizontalHeader()->setSectionResizeMode(ColText, QHeaderView::Stretch);
    m_table->setColumnWidth(ColCategory, 110);
    m_table->setColumnWidth(ColSlot1, 150);
    m_table->setColumnWidth(ColSlot2, 150);
    connect(m_table, &QTableWidget::cellDoubleClicked, this,
            [this](int row, int col) { editSlot(row, col == ColSlot2 ? 1 : 0); });

    auto* hint = new QLabel(T("Doppelklick auf Taste 1 oder Taste 2, dann die neue Taste oder die Seitentaste der Maus "
                            "drücken (Esc = abbrechen). Fett = eigene Belegung, rot = doppelt belegt."));
    hint->setWordWrap(true);
    hint->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));

    auto* resetOne = new QPushButton(T("Standard"));
    resetOne->setToolTip(T("Gewählte Zeile auf Standard zurücksetzen"));
    connect(resetOne, &QPushButton::clicked, this, [this] { resetRow(m_table->currentRow()); });
    auto* clearOne = new QPushButton(T("Entfernen"));
    clearOne->setToolTip(T("Gewählte Taste entfernen (Spalte Taste 1 oder 2, sonst beide)"));
    connect(clearOne, &QPushButton::clicked, this, [this] {
        const int row = m_table->currentRow();
        if (row < 0) return;
        const QString id = m_table->item(row, ColText)->data(Qt::UserRole).toString();
        const int col = m_table->currentColumn();
        if (col == ColSlot1 || col == ColSlot2) InputBindings::instance().setInput(id, col - ColSlot1, {});
        else InputBindings::instance().setInputs(id, {});
        fill();
    });
    auto* resetAll = new QPushButton(T("Alle zurücksetzen"));
    connect(resetAll, &QPushButton::clicked, this, [this] {
        InputBindings::instance().resetAll();
        fill();
    });
    // Built-in sets (standard like DaVinci, beginner)
    auto* preset = new QPushButton(T("Vorlage"));
    preset->setToolTip(T("Fertige Tastenbelegung laden (ersetzt die jetzige)"));
    auto* presetMenu = new QMenu(preset);
    for (const QString& id : InputBindings::presetIds())
        presetMenu->addAction(InputBindings::presetName(id), this, [this, id] {
            if (QMessageBox::question(this, T("Tastenbelegung"),
                                      T("Vorlage „%1“ laden? Die jetzige Belegung (auch Maustasten und Mausrad) "
                                        "wird ersetzt.")
                                          .arg(InputBindings::presetName(id)))
                != QMessageBox::Yes)
                return;
            InputBindings::instance().applyPreset(id);
            fill();
        });
    preset->setMenu(presetMenu);
    // Whole sets (to another computer, several setups): same format as keybindings.json
    auto* loadSet = new QPushButton(T("Satz laden…"));
    loadSet->setToolTip(T("Ganze Tastenbelegung aus einer Datei laden (ersetzt die jetzige)"));
    connect(loadSet, &QPushButton::clicked, this, [this] {
        QSettings settings;
        const QString path = QFileDialog::getOpenFileName(this, T("Tastenbelegung laden"), setDir(),
                                                          T("Tastenbelegung (*.json)"));
        if (path.isEmpty()) return;
        settings.setValue("keys/setDir", QFileInfo(path).absolutePath());
        QString error;
        if (!InputBindings::instance().importSet(path, &error))
            QMessageBox::warning(this, T("Tastenbelegung laden"), error);
        fill();
    });
    auto* saveSet = new QPushButton(T("Satz speichern…"));
    saveSet->setToolTip(T("Ganze Tastenbelegung als Datei speichern"));
    connect(saveSet, &QPushButton::clicked, this, [this] {
        QString path = QFileDialog::getSaveFileName(this, T("Tastenbelegung speichern"),
                                                    QDir(setDir()).filePath(T("tastenbelegung.json")),
                                                    T("Tastenbelegung (*.json)"));
        if (path.isEmpty()) return;
        if (!path.endsWith(".json", Qt::CaseInsensitive)) path += ".json";
        QSettings().setValue("keys/setDir", QFileInfo(path).absolutePath());
        if (!InputBindings::instance().exportSet(path))
            QMessageBox::warning(this, T("Tastenbelegung speichern"), T("Die Datei lässt sich nicht schreiben."));
    });
    auto* close = new QPushButton(T("Schließen"));
    close->setDefault(true);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);

    auto* buttons = new QHBoxLayout;
    buttons->addWidget(resetOne);
    buttons->addWidget(clearOne);
    buttons->addWidget(resetAll);
    buttons->addSpacing(12);
    buttons->addWidget(preset);
    buttons->addWidget(loadSet);
    buttons->addWidget(saveSet);
    buttons->addStretch(1);
    buttons->addWidget(close);

    auto* lay = new QVBoxLayout(this);
    lay->addWidget(m_filter);
    lay->addWidget(m_table, 1);
    lay->addWidget(hint);
    lay->addLayout(buttons);

    fill();
}

void KeyBindingsDialog::fill()
{
    const auto& bindings = InputBindings::instance();
    const auto& entries = bindings.entries();

    // Key or mouse button -> number of actions (mark conflicts). Keys count per scope: a panel's key wins in the panel
    // (InputBindings::registerPanelAction/registerScopedAction), the same key in the window is no conflict. A mouse
    // button belongs to one action only, whatever its scope.
    const auto useKey = [](const InputBindings::Entry& e, const QString& in) {
        return InputBindings::inputMouseButton(in) == Qt::NoButton ? QString(e.scope + '\n' + in) : in;
    };
    QHash<QString, int> uses;
    for (const auto& e : entries)
        for (const QString& in : bindings.inputs(e.id))
            if (!in.isEmpty()) ++uses[useKey(e, in)];

    for (int r = 0; r < m_table->rowCount(); ++r)
        for (int c = ColSlot1; c <= ColSlot2; ++c) m_table->removeCellWidget(r, c);
    m_table->setRowCount(entries.size());
    for (int i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        auto* cat = new QTableWidgetItem(e.category);
        cat->setForeground(Theme::textDim);
        auto* text = new QTableWidgetItem(e.text);
        text->setData(Qt::UserRole, e.id);
        m_table->setItem(i, ColCategory, cat);
        m_table->setItem(i, ColText, text);

        const QStringList inputs = bindings.inputs(e.id);
        const QStringList defaults = bindings.defaultInputs(e.id);
        for (int s = 0; s < InputBindings::kSlots; ++s) {
            auto* item = new QTableWidgetItem(InputBindings::inputText(inputs[s]));
            if (inputs[s] != defaults[s]) {
                QFont f = item->font();
                f.setBold(true); // own binding
                item->setFont(f);
            }
            if (uses.value(useKey(e, inputs[s])) > 1) {
                item->setForeground(Theme::warning);
                item->setToolTip(T("Diese Taste ist mehrfach belegt"));
            }
            m_table->setItem(i, ColSlot1 + s, item);
        }
    }
    applyFilter();
}

void KeyBindingsDialog::applyFilter()
{
    const QString f = m_filter->text().trimmed();
    for (int r = 0; r < m_table->rowCount(); ++r) {
        bool match = f.isEmpty();
        for (int c = 0; c < ColCount && !match; ++c) match = m_table->item(r, c)->text().contains(f, Qt::CaseInsensitive);
        m_table->setRowHidden(r, !match);
    }
}

void KeyBindingsDialog::resetRow(int row)
{
    if (row < 0) return;
    const QString id = m_table->item(row, ColText)->data(Qt::UserRole).toString();
    InputBindings::instance().setInputs(id, InputBindings::instance().defaultInputs(id));
    fill();
}

void KeyBindingsDialog::editSlot(int row, int slot)
{
    if (row < 0) return;
    const QString id = m_table->item(row, ColText)->data(Qt::UserRole).toString();
    auto* capture = new InputCapture([this, id, slot](const QString& input, bool ok) {
        if (ok) InputBindings::instance().setInput(id, slot, input);
        QMetaObject::invokeMethod(this, [this] { fill(); }, Qt::QueuedConnection); // remove the editor only afterwards
    });
    m_table->setCellWidget(row, ColSlot1 + slot, capture);
    capture->setFocus();
}
