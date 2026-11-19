// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/direct/DirectSuggestions.h"

#include "app/InputBindings.h"
#include "app/Theme.h"
#include "core/I18n.h"
#include "ui/ElidedLabel.h"
#include "ui/direct/DirectText.h"

#include <QAction>
#include <QApplication>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QThreadPool>
#include <QVBoxLayout>

namespace {
constexpr int kNumberRole = Qt::UserRole;
const QSize kThumb(64, 36);

QIcon placeholder()
{
    QPixmap pm(kThumb);
    pm.fill(Theme::thumbBg);
    return QIcon(pm);
}
} // namespace

DirectSuggestions::DirectSuggestions(QWidget* parent) : QWidget(parent)
{
    setObjectName("direct_suggestions");
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(6, 4, 6, 6);
    lay->setSpacing(4);
    auto* title = new ElidedLabel;
    title->setFullText(T("Vorschläge"));
    title->setObjectName("PanelTitle");
    lay->addWidget(title);
    auto* top = new QHBoxLayout;
    m_sum = new QLabel;
    m_sum->setObjectName("direct_sum");
    m_allOn = new QPushButton(T("Alle an"));
    m_allOn->setObjectName("direct_all_on");
    m_allOff = new QPushButton(T("Alle aus"));
    m_allOff->setObjectName("direct_all_off");
    connect(m_allOn, &QPushButton::clicked, this, [this] { setAllChecked(true); });
    connect(m_allOff, &QPushButton::clicked, this, [this] { setAllChecked(false); });
    top->addWidget(m_sum, 1);
    top->addWidget(m_allOn);
    top->addWidget(m_allOff);
    lay->addLayout(top);
    m_list = new QListWidget;
    m_list->setObjectName("direct_suggestion_list");
    m_list->setIconSize(kThumb);
    m_list->setWordWrap(true);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->setDragDropMode(QAbstractItemView::InternalMove); // the order of the result
    m_list->setDefaultDropAction(Qt::MoveAction);
    m_list->installEventFilter(this);
    lay->addWidget(m_list, 1);
    m_empty = new QLabel;
    m_empty->setObjectName("direct_suggestions_empty");
    m_empty->setWordWrap(true);
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));
    lay->addWidget(m_empty);
    m_notes = new QLabel;
    m_notes->setObjectName("direct_notes");
    m_notes->setWordWrap(true);
    m_notes->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));
    lay->addWidget(m_notes);

    connect(m_list, &QListWidget::itemChanged, this, [this](QListWidgetItem*) {
        if (m_filling) return;
        updateSum();
        emit changed();
    });
    // A drop inside the list moves rows: the model's rowsMoved
    connect(m_list->model(), &QAbstractItemModel::rowsMoved, this, [this] {
        if (m_filling) return;
        emit changed();
    });
    connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        const QJsonObject s = m_byN.value(item->data(kNumberRole).toInt());
        if (!s.isEmpty())
            emit previewRequested(s.value("file").toString(), s.value("from_s").toDouble(), s.value("to_s").toDouble());
    });
    m_remove = new QAction(T("Vorschlag entfernen"), this);
    m_remove->setShortcuts({QKeySequence(Qt::Key_Delete), QKeySequence(Qt::Key_Backspace)});
    m_remove->setShortcutContext(Qt::WidgetShortcut);
    connect(m_remove, &QAction::triggered, this, &DirectSuggestions::removeSelected);
    clear();
}

void DirectSuggestions::clear()
{
    ++m_generation;
    m_session = {};
    m_byN.clear();
    m_list->clear();
    m_notes->clear();
    m_notes->hide();
    m_empty->setText(T("Material links hineinziehen und ein Rezept wählen."));
    updateSum();
}

void DirectSuggestions::setBusyText(const QString& text)
{
    if (text.isEmpty()) {
        updateSum();
        return;
    }
    m_empty->setText(text);
    m_empty->show();
}

void DirectSuggestions::setError(const QString& text)
{
    m_empty->setText(text);
    m_empty->show();
}

void DirectSuggestions::setSession(const QJsonObject& session)
{
    ++m_generation;
    m_session = session;
    m_byN.clear();
    m_filling = true;
    m_list->clear();
    const QJsonArray all = session.value("suggestions").toArray();
    for (const QJsonValue& v : all) m_byN.insert(v.toObject().value("n").toInt(), v.toObject());
    QList<int> done;
    for (const QJsonValue& v : session.value("order").toArray())
        if (const int n = v.toInt(); m_byN.contains(n) && !done.contains(n)) {
            addRow(m_byN.value(n), true);
            done << n;
        }
    for (const QJsonValue& v : all) {
        const QJsonObject s = v.toObject();
        const int n = s.value("n").toInt();
        if (done.contains(n)) continue;
        addRow(s, s.value("on").toBool() && session.value("order").toArray().isEmpty());
        done << n;
    }
    m_filling = false;
    QStringList notes;
    for (const QJsonValue& v : session.value("notes").toArray()) notes << DirectText::note(v.toObject());
    m_notes->setText(notes.join("\n"));
    m_notes->setVisible(!notes.isEmpty());
    m_empty->setText(T("Keine Vorschläge in diesem Material. Ein anderes Rezept oder mehr Material versuchen."));
    updateSum();
    loadThumbs();
    emit changed();
}

void DirectSuggestions::addRow(const QJsonObject& s, bool on)
{
    const double from = s.value("from_s").toDouble(), to = s.value("to_s").toDouble();
    const QString file = s.value("file").toString();
    const QString where = file.startsWith('@') ? T("Timeline") : QFileInfo(file).fileName();
    QString text = DirectText::reason(s.value("reason").toObject()) + "\n"
                   + QString("%1 · %2–%3 (%4)").arg(where, DirectText::clock(from), DirectText::clock(to),
                                                    DirectText::seconds(to - from));
    if (const int variant = s.value("variant").toInt(); variant > 1) text += " · " + T("Variante %1").arg(variant);
    auto* item = new QListWidgetItem(placeholder(), text, m_list);
    item->setData(kNumberRole, s.value("n").toInt());
    item->setFlags((item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsDragEnabled) & ~Qt::ItemIsDropEnabled);
    item->setCheckState(on ? Qt::Checked : Qt::Unchecked);
    item->setToolTip(s.value("reason").toObject().value("text").toString());
}

QList<int> DirectSuggestions::rowOrder() const
{
    QList<int> out;
    for (int i = 0; i < m_list->count(); ++i) out << m_list->item(i)->data(kNumberRole).toInt();
    return out;
}

QList<int> DirectSuggestions::checkedOrder() const
{
    QList<int> out;
    for (int i = 0; i < m_list->count(); ++i)
        if (m_list->item(i)->checkState() == Qt::Checked) out << m_list->item(i)->data(kNumberRole).toInt();
    return out;
}

void DirectSuggestions::setChecked(int n, bool on)
{
    for (int i = 0; i < m_list->count(); ++i)
        if (m_list->item(i)->data(kNumberRole).toInt() == n) m_list->item(i)->setCheckState(on ? Qt::Checked : Qt::Unchecked);
}

void DirectSuggestions::setAllChecked(bool on)
{
    m_filling = true;
    for (int i = 0; i < m_list->count(); ++i) m_list->item(i)->setCheckState(on ? Qt::Checked : Qt::Unchecked);
    m_filling = false;
    updateSum();
    emit changed();
}

void DirectSuggestions::removeSelected()
{
    const QList<QListWidgetItem*> gone = m_list->selectedItems();
    if (gone.isEmpty()) return;
    int row = m_list->row(gone.first());
    m_filling = true;
    for (QListWidgetItem* item : gone) delete item;
    m_filling = false;
    if (m_list->count()) m_list->setCurrentRow(std::min(row, m_list->count() - 1));
    updateSum();
    emit changed();
}

double DirectSuggestions::checkedSeconds() const
{
    double sum = 0;
    for (int n : checkedOrder()) {
        const QJsonObject s = m_byN.value(n);
        sum += std::max(0.0, s.value("to_s").toDouble() - s.value("from_s").toDouble());
    }
    return sum;
}

QString DirectSuggestions::sumText() const
{
    if (!hasSession()) return {};
    const double sum = checkedSeconds(), target = m_session.value("target_s").toDouble();
    if (target > 0) return T("%1 von %2").arg(DirectText::seconds(sum), DirectText::seconds(target));
    const QString recipe = m_session.value("recipe").toString();
    if (recipe == "talk") return T("%1 Pausen raus").arg(DirectText::seconds(sum));
    return DirectText::seconds(sum);
}

void DirectSuggestions::updateSum()
{
    m_sum->setText(sumText());
    const bool rows = m_list->count() > 0;
    m_allOn->setEnabled(rows);
    m_allOff->setEnabled(rows);
    m_list->setVisible(rows);
    m_empty->setVisible(!rows);
}

QJsonObject DirectSuggestions::choice() const
{
    QJsonArray on, order;
    for (int n : checkedOrder()) on.append(n);
    for (int n : rowOrder()) order.append(n);
    return {{"on", on}, {"order", order}};
}

QJsonObject DirectSuggestions::suggestion(int n) const { return m_byN.value(n); }

void DirectSuggestions::setThumbnailer(std::function<QImage(const QString&, double, const QSize&)> thumb)
{
    m_thumb = std::move(thumb);
}

void DirectSuggestions::loadThumbs()
{
    if (!m_thumb) return;
    const int generation = m_generation;
    const auto thumb = m_thumb;
    const QSize size = kThumb * devicePixelRatioF();
    QPointer<DirectSuggestions> self(this);
    for (int i = 0; i < m_list->count(); ++i) {
        const int n = m_list->item(i)->data(kNumberRole).toInt();
        const QJsonObject s = m_byN.value(n);
        const QString file = s.value("file").toString();
        if (!QFileInfo(file).isFile()) continue;
        const double at = s.value("from_s").toDouble();
        QThreadPool::globalInstance()->start([self, thumb, file, at, size, generation, n] {
            const QImage img = thumb(file, at, size);
            if (img.isNull()) return;
            QMetaObject::invokeMethod(qApp, [self, img, generation, n] {
                if (!self || self->m_generation != generation) return;
                for (int i = 0; i < self->m_list->count(); ++i)
                    if (QListWidgetItem* item = self->m_list->item(i); item->data(kNumberRole).toInt() == n) {
                        const QSignalBlocker b(self->m_list);
                        item->setIcon(QIcon(QPixmap::fromImage(img)));
                    }
            });
        });
    }
}

bool DirectSuggestions::eventFilter(QObject* obj, QEvent* e)
{
    // Delete takes rows out and never reaches the window (timeline clips)
    if (obj == m_list && (e->type() == QEvent::ShortcutOverride || e->type() == QEvent::KeyPress)
        && InputBindings::matches(m_remove, static_cast<QKeyEvent*>(e))) {
        e->accept();
        if (e->type() == QEvent::KeyPress) m_remove->trigger();
        return e->type() == QEvent::KeyPress;
    }
    return QWidget::eventFilter(obj, e);
}
