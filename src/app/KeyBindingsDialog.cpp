#include "app/KeyBindingsDialog.h"

#include "app/InputBindings.h"
#include "app/Theme.h"

#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QKeyEvent>
#include <QKeySequenceEdit>
#include <QMouseEvent>
#include <functional>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {
enum Column { ColCategory, ColText, ColKey, ColMouse, ColCount };

// Maustaste einfach drücken statt aus einer Liste wählen (wie die Tasten-Spalte).
// Entf/Rücktaste = entfernen, Esc oder Wegklicken = abbrechen.
class MouseCapture : public QLabel {
public:
    explicit MouseCapture(std::function<void(int)> done) : m_done(std::move(done))
    {
        setText("Maustaste drücken …");
        setFocusPolicy(Qt::StrongFocus);
        setStyleSheet(QString("color: %1; padding-left: 4px;").arg(Theme::accent.name()));
        setToolTip("Seitentaste drücken. Entf = entfernen, Esc = abbrechen");
    }

protected:
    void mousePressEvent(QMouseEvent* e) override
    {
        e->accept();
        if (InputBindings::bindableMouseButtons().contains(e->button())) finish(int(e->button()));
        else if (e->button() != Qt::LeftButton) setText("Nur Seitentasten/Zusatztasten");
    }
    void mouseDoubleClickEvent(QMouseEvent* e) override { mousePressEvent(e); }
    void keyPressEvent(QKeyEvent* e) override
    {
        if (e->key() == Qt::Key_Escape) finish(-1);
        else if (e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) finish(int(Qt::NoButton));
    }
    void focusOutEvent(QFocusEvent*) override { finish(-1); }

private:
    void finish(int button)
    {
        if (!m_done) return;
        auto done = std::move(m_done);
        m_done = nullptr;
        done(button); // -1 = nichts ändern
    }
    std::function<void(int)> m_done;
};
}

KeyBindingsDialog::KeyBindingsDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle("Tastenbelegung");
    resize(780, 640);

    m_filter = new QLineEdit;
    m_filter->setPlaceholderText("Suchen (Funktion oder Taste) …");
    m_filter->setClearButtonEnabled(true);
    connect(m_filter, &QLineEdit::textChanged, this, &KeyBindingsDialog::applyFilter);

    m_table = new QTableWidget(0, ColCount);
    m_table->setHorizontalHeaderLabels({"Bereich", "Funktion", "Taste", "Maustaste"});
    m_table->verticalHeader()->hide();
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->horizontalHeader()->setSectionResizeMode(ColText, QHeaderView::Stretch);
    m_table->setColumnWidth(ColCategory, 110);
    m_table->setColumnWidth(ColKey, 150);
    m_table->setColumnWidth(ColMouse, 150);
    connect(m_table, &QTableWidget::cellDoubleClicked, this,
            [this](int row, int col) { col == ColMouse ? editMouse(row) : editRow(row); });

    auto* hint = new QLabel("Doppelklick auf eine Zeile, dann die neue Taste drücken. "
                            "Doppelklick in der Spalte Maustaste, dann die Seitentaste der Maus drücken. "
                            "Fett = eigene Belegung, rot = doppelt belegt.");
    hint->setWordWrap(true);
    hint->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));

    auto* resetOne = new QPushButton("Standard");
    resetOne->setToolTip("Gewählte Zeile auf Standard zurücksetzen");
    connect(resetOne, &QPushButton::clicked, this, [this] { resetRow(m_table->currentRow()); });
    auto* clearOne = new QPushButton("Entfernen");
    clearOne->setToolTip("Taste und Maustaste der gewählten Zeile entfernen");
    connect(clearOne, &QPushButton::clicked, this, [this] {
        const int row = m_table->currentRow();
        if (row < 0) return;
        const QString id = m_table->item(row, ColText)->data(Qt::UserRole).toString();
        InputBindings::instance().setShortcut(id, {});
        InputBindings::instance().setMouseButton(id, Qt::NoButton);
        fill();
    });
    auto* resetAll = new QPushButton("Alle zurücksetzen");
    connect(resetAll, &QPushButton::clicked, this, [this] {
        InputBindings::instance().resetAll();
        fill();
    });
    auto* close = new QPushButton("Schließen");
    close->setDefault(true);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);

    auto* buttons = new QHBoxLayout;
    buttons->addWidget(resetOne);
    buttons->addWidget(clearOne);
    buttons->addWidget(resetAll);
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

    QHash<QString, int> uses; // Taste -> Anzahl Aktionen (Konflikte markieren)
    for (const auto& e : entries) {
        const QString k = bindings.current(e.id).toString(QKeySequence::PortableText);
        if (!k.isEmpty()) ++uses[k];
    }

    for (int r = 0; r < m_table->rowCount(); ++r) {
        m_table->removeCellWidget(r, ColKey);
        m_table->removeCellWidget(r, ColMouse);
    }
    m_table->setRowCount(entries.size());
    for (int i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        const QKeySequence key = bindings.current(e.id);
        auto* cat = new QTableWidgetItem(e.category);
        cat->setForeground(Theme::textDim);
        auto* text = new QTableWidgetItem(e.text);
        text->setData(Qt::UserRole, e.id);
        auto* keyItem = new QTableWidgetItem(key.toString(QKeySequence::NativeText));
        if (key != e.defaultKey) {
            QFont f = keyItem->font();
            f.setBold(true); // eigene Belegung
            keyItem->setFont(f);
        }
        if (uses.value(key.toString(QKeySequence::PortableText)) > 1) {
            keyItem->setForeground(QColor(0xe8, 0x41, 0x4a));
            keyItem->setToolTip("Diese Taste ist mehrfach belegt");
        }
        m_table->setItem(i, ColCategory, cat);
        m_table->setItem(i, ColText, text);
        m_table->setItem(i, ColKey, keyItem);

        const Qt::MouseButton mb = bindings.mouseButtonFor(e.id);
        auto* mouseItem = new QTableWidgetItem(InputBindings::mouseButtonText(mb));
        if (mb != bindings.defaultMouseButtonFor(e.id)) {
            QFont f = mouseItem->font();
            f.setBold(true);
            mouseItem->setFont(f);
        }
        m_table->setItem(i, ColMouse, mouseItem);
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

void KeyBindingsDialog::editRow(int row)
{
    if (row < 0) return;
    const QString id = m_table->item(row, ColText)->data(Qt::UserRole).toString();
    auto* edit = new QKeySequenceEdit(InputBindings::instance().current(id));
    edit->setMaximumSequenceLength(1);
    edit->setClearButtonEnabled(true);
    m_table->setCellWidget(row, ColKey, edit);
    edit->setFocus();
    connect(edit, &QKeySequenceEdit::editingFinished, this, [this, edit, id] {
        InputBindings::instance().setShortcut(id, edit->keySequence());
        QMetaObject::invokeMethod(this, [this] { fill(); }, Qt::QueuedConnection); // Editor erst danach entfernen
    });
}

void KeyBindingsDialog::resetRow(int row)
{
    if (row < 0) return;
    const QString id = m_table->item(row, ColText)->data(Qt::UserRole).toString();
    for (const auto& e : InputBindings::instance().entries())
        if (e.id == id) InputBindings::instance().setShortcut(id, e.defaultKey);
    InputBindings::instance().setMouseButton(id, InputBindings::instance().defaultMouseButtonFor(id));
    fill();
}

void KeyBindingsDialog::editMouse(int row)
{
    if (row < 0) return;
    const QString id = m_table->item(row, ColText)->data(Qt::UserRole).toString();
    auto* capture = new MouseCapture([this, id](int b) {
        if (b >= 0) InputBindings::instance().setMouseButton(id, Qt::MouseButton(b));
        QMetaObject::invokeMethod(this, [this] { fill(); }, Qt::QueuedConnection); // Editor erst danach entfernen
    });
    m_table->setCellWidget(row, ColMouse, capture);
    capture->setFocus();
}
