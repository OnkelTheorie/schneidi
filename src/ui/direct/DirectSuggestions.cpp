// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/direct/DirectSuggestions.h"

#include "app/InputBindings.h"
#include "app/Theme.h"
#include "core/I18n.h"
#include "ui/ElidedLabel.h"
#include "ui/direct/DirectText.h"

#include <QAction>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QSet>
#include <QVBoxLayout>
#include <algorithm>

namespace {
constexpr int kNumberRole = Qt::UserRole;
const QSize kThumb(64, 36);

QIcon placeholder()
{
    QPixmap pm(kThumb);
    pm.fill(Theme::thumbBg);
    return QIcon(pm);
}

QDoubleSpinBox* secondsBox(const QString& name)
{
    auto* b = new QDoubleSpinBox;
    b->setObjectName(name);
    b->setDecimals(1);
    b->setSingleStep(0.1);
    b->setSuffix(" s");
    b->setKeyboardTracking(false);
    return b;
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
    m_variantBox = new QComboBox;
    m_variantBox->setObjectName("direct_variant");
    m_variantBox->setToolTip(T("Welche Variante übernommen wird"));
    m_variantBox->hide();
    connect(m_variantBox, &QComboBox::activated, this, [this](int i) { setVariant(i + 1); });
    m_allOn = new QPushButton(T("Alle an"));
    m_allOn->setObjectName("direct_all_on");
    m_allOff = new QPushButton(T("Alle aus"));
    m_allOff->setObjectName("direct_all_off");
    connect(m_allOn, &QPushButton::clicked, this, [this] { setAllChecked(true); });
    connect(m_allOff, &QPushButton::clicked, this, [this] { setAllChecked(false); });
    top->addWidget(m_sum, 1);
    top->addWidget(m_variantBox);
    top->addWidget(m_allOn);
    top->addWidget(m_allOff);
    lay->addLayout(top);
    m_staleRow = new QWidget;
    m_staleRow->setObjectName("direct_stale");
    auto* staleLay = new QHBoxLayout(m_staleRow);
    staleLay->setContentsMargins(0, 0, 0, 0);
    m_staleText = new QLabel;
    m_staleText->setWordWrap(true);
    m_staleText->setStyleSheet(QString("color: %1;").arg(Theme::primary.name()));
    auto* again = new QPushButton(T("Neu vorschlagen"));
    again->setObjectName("direct_resuggest");
    connect(again, &QPushButton::clicked, this, &DirectSuggestions::resuggestRequested);
    staleLay->addWidget(m_staleText, 1);
    staleLay->addWidget(again);
    m_staleRow->hide();
    lay->addWidget(m_staleRow);
    m_list = new QListWidget;
    m_list->setObjectName("direct_suggestion_list");
    m_list->setIconSize(kThumb);
    m_list->setWordWrap(true);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->setDragDropMode(QAbstractItemView::InternalMove); // the order of the result
    m_list->setDefaultDropAction(Qt::MoveAction);
    m_list->installEventFilter(this);
    m_list->viewport()->installEventFilter(this);
    lay->addWidget(m_list, 1);
    // In/Out of the current row
    m_rangeRow = new QWidget;
    m_rangeRow->setObjectName("direct_range");
    auto* rangeLay = new QHBoxLayout(m_rangeRow);
    rangeLay->setContentsMargins(0, 0, 0, 0);
    m_in = secondsBox("direct_range_in");
    m_out = secondsBox("direct_range_out");
    rangeLay->addWidget(new QLabel(T("Von")));
    rangeLay->addWidget(m_in, 1);
    rangeLay->addWidget(new QLabel(T("bis")));
    rangeLay->addWidget(m_out, 1);
    m_rangeRow->setToolTip(T("Anfang und Ende des gewählten Vorschlags in seiner Datei"));
    lay->addWidget(m_rangeRow);
    const auto edited = [this] {
        QListWidgetItem* item = m_list->currentItem();
        if (!item || m_filling) return;
        setRange(item->data(kNumberRole).toInt(), m_in->value(), m_out->value());
    };
    connect(m_in, &QDoubleSpinBox::valueChanged, this, edited);
    connect(m_out, &QDoubleSpinBox::valueChanged, this, edited);
    connect(m_list, &QListWidget::currentItemChanged, this, [this] { updateRangeBoxes(); });
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
        const int n = item->data(kNumberRole).toInt();
        // The click toggled the checkbox (the view toggles before it reports the click): no preview
        if (n == m_pressedN && item->checkState() != m_pressedState) return;
        const QJsonObject s = m_byN.value(n);
        if (s.isEmpty()) return;
        const auto [from, to] = range(n);
        emit previewRequested(s.value("file").toString(), from, to);
    });
    m_remove = new QAction(T("Vorschlag entfernen"), this);
    m_remove->setShortcuts({QKeySequence(Qt::Key_Delete), QKeySequence(Qt::Key_Backspace)});
    m_remove->setShortcutContext(Qt::WidgetShortcut);
    connect(m_remove, &QAction::triggered, this, &DirectSuggestions::removeSelected);
    clear();
}

void DirectSuggestions::setRemoveAction(QAction* action)
{
    if (!action || action == m_remove) return;
    if (m_remove->parent() == this) delete m_remove;
    m_remove = action;
    m_remove->setParent(this); // its panel (InputBindings::scopeOf)
    connect(m_remove, &QAction::triggered, this, &DirectSuggestions::removeSelected);
}

void DirectSuggestions::clear()
{
    m_gen.next();
    m_session = {};
    m_byN.clear();
    m_adjust.clear();
    m_variants = 0;
    m_variant = 1;
    m_filling = true;
    m_list->clear();
    m_filling = false;
    m_notes->clear();
    m_notes->hide();
    m_staleRow->hide();
    m_variantBox->hide();
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

void DirectSuggestions::setStale(const QString& text)
{
    m_staleText->setText(text);
    m_staleRow->setVisible(!text.isEmpty());
}

void DirectSuggestions::setSession(const QJsonObject& session)
{
    m_gen.next();
    m_session = session;
    m_byN.clear();
    m_adjust.clear();
    m_staleRow->hide();
    m_filling = true;
    m_list->clear();
    const QJsonArray all = session.value("suggestions").toArray();
    int maxVariant = 0;
    for (const QJsonValue& v : all) {
        const QJsonObject s = v.toObject();
        m_byN.insert(s.value("n").toInt(), s);
        maxVariant = std::max(maxVariant, s.value("variant").toInt());
    }
    m_variants = maxVariant > 1 ? maxVariant : 0;
    m_variant = 1;
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
    m_variantBox->clear();
    for (int i = 1; i <= m_variants; ++i) m_variantBox->addItem(T("Variante %1").arg(i));
    m_variantBox->setVisible(m_variants > 0);
    m_filling = false;
    setVariant(1);
    // The first shown row is the current one (its In/Out in the boxes; no preview: that needs a click)
    for (int i = 0; i < m_list->count(); ++i)
        if (!m_list->item(i)->isHidden()) {
            m_list->setCurrentRow(i, QItemSelectionModel::NoUpdate);
            break;
        }
    QStringList notes;
    for (const QJsonValue& v : session.value("notes").toArray()) notes << DirectText::note(v.toObject());
    m_notes->setText(notes.join("\n"));
    m_notes->setVisible(!notes.isEmpty());
    m_empty->setText(T("Keine Vorschläge in diesem Material. Ein anderes Rezept oder mehr Material versuchen."));
    updateSum();
    updateRangeBoxes();
    loadThumbs();
    emit changed();
}

void DirectSuggestions::setVariant(int v)
{
    if (m_variants == 0) v = 1;
    v = std::clamp(v, 1, std::max(1, m_variants));
    m_variant = v;
    m_filling = true;
    bool anyOn = false;
    for (int i = 0; i < m_list->count(); ++i) {
        QListWidgetItem* item = m_list->item(i);
        const int variant = m_byN.value(item->data(kNumberRole).toInt()).value("variant").toInt(1);
        const bool shown = m_variants == 0 || variant == v;
        item->setHidden(!shown);
        anyOn = anyOn || (shown && item->checkState() == Qt::Checked);
    }
    // A variant chosen the first time with nothing checked in it: all of its rows (it is a pick of its own)
    if (!anyOn)
        for (int i = 0; i < m_list->count(); ++i)
            if (!m_list->item(i)->isHidden()) m_list->item(i)->setCheckState(Qt::Checked);
    m_filling = false;
    if (m_variantBox->currentIndex() != v - 1 && v - 1 < m_variantBox->count()) m_variantBox->setCurrentIndex(v - 1);
    updateSum();
    emit changed();
}

QString DirectSuggestions::rowText(int n) const
{
    const QJsonObject s = m_byN.value(n);
    const auto [from, to] = range(n);
    const QString file = s.value("file").toString();
    const QString where = file.startsWith('@') ? T("Timeline") : QFileInfo(file).fileName();
    QString text = DirectText::reason(s.value("reason").toObject()) + "\n"
                   + QString("%1 · %2–%3 (%4)").arg(where, DirectText::clock(from), DirectText::clock(to),
                                                    DirectText::seconds(to - from));
    if (m_adjust.contains(n)) text += " · " + T("angepasst");
    return text;
}

void DirectSuggestions::addRow(const QJsonObject& s, bool on)
{
    const int n = s.value("n").toInt();
    auto* item = new QListWidgetItem(placeholder(), rowText(n), m_list);
    item->setData(kNumberRole, n);
    item->setFlags((item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsDragEnabled) & ~Qt::ItemIsDropEnabled);
    item->setCheckState(on ? Qt::Checked : Qt::Unchecked);
    item->setToolTip(s.value("reason").toObject().value("text").toString());
}

QList<int> DirectSuggestions::rowOrder() const
{
    QList<int> out;
    for (int i = 0; i < m_list->count(); ++i)
        if (!m_list->item(i)->isHidden()) out << m_list->item(i)->data(kNumberRole).toInt();
    return out;
}

QList<int> DirectSuggestions::checkedOrder() const
{
    QList<int> out;
    for (int i = 0; i < m_list->count(); ++i)
        if (const QListWidgetItem* item = m_list->item(i); !item->isHidden() && item->checkState() == Qt::Checked)
            out << item->data(kNumberRole).toInt();
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
    for (int i = 0; i < m_list->count(); ++i)
        if (!m_list->item(i)->isHidden()) m_list->item(i)->setCheckState(on ? Qt::Checked : Qt::Unchecked);
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

double DirectSuggestions::fileLength(const QString& file) const
{
    for (const QJsonValue& v : m_session.value("material").toArray())
        if (v.toObject().value("file").toString() == file) return v.toObject().value("length_s").toDouble();
    return 0;
}

QPair<double, double> DirectSuggestions::range(int n) const
{
    if (m_adjust.contains(n)) return m_adjust.value(n);
    const QJsonObject s = m_byN.value(n);
    return {s.value("from_s").toDouble(), s.value("to_s").toDouble()};
}

void DirectSuggestions::setRange(int n, double from, double to)
{
    const QJsonObject s = m_byN.value(n);
    if (s.isEmpty()) return;
    const double len = fileLength(s.value("file").toString());
    from = std::max(0.0, from);
    if (len > 0) from = std::min(from, std::max(0.0, len - 0.1));
    to = std::max(to, from + 0.1);
    if (len > 0) to = std::min(to, len);
    const bool original = std::abs(from - s.value("from_s").toDouble()) < 0.005 && std::abs(to - s.value("to_s").toDouble()) < 0.005;
    if (original) m_adjust.remove(n);
    else m_adjust.insert(n, {from, to});
    for (int i = 0; i < m_list->count(); ++i)
        if (QListWidgetItem* item = m_list->item(i); item->data(kNumberRole).toInt() == n) {
            m_filling = true;
            item->setText(rowText(n));
            m_filling = false;
        }
    updateSum();
    updateRangeBoxes();
    emit changed();
}

void DirectSuggestions::updateRangeBoxes()
{
    QListWidgetItem* item = m_list->currentItem();
    const int n = item ? item->data(kNumberRole).toInt() : -1;
    const QJsonObject s = m_byN.value(n);
    m_rangeRow->setVisible(hasSession() && m_list->count() > 0);
    m_rangeRow->setEnabled(!s.isEmpty());
    m_filling = true;
    const double len = s.isEmpty() ? 0 : fileLength(s.value("file").toString());
    const double max = len > 0 ? len : 24 * 3600.0;
    m_in->setRange(0, max);
    m_out->setRange(0, max);
    const auto [from, to] = s.isEmpty() ? QPair<double, double>{0, 0} : range(n);
    m_in->setValue(from);
    m_out->setValue(to);
    m_filling = false;
}

double DirectSuggestions::checkedSeconds() const
{
    double sum = 0;
    for (int n : checkedOrder()) {
        const auto [from, to] = range(n);
        sum += std::max(0.0, to - from);
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
    QJsonArray on, adjust;
    for (int n : checkedOrder()) {
        on.append(n);
        if (m_adjust.contains(n)) {
            const auto [from, to] = m_adjust.value(n);
            adjust.append(QJsonObject{{"n", n}, {"from_s", from}, {"to_s", to}});
        }
    }
    QJsonObject c{{"on", on}, {"order", on}};
    if (!adjust.isEmpty()) c["adjust"] = adjust;
    if (m_variants > 0) c["variant"] = m_variant;
    return c;
}

QJsonObject DirectSuggestions::suggestion(int n) const { return m_byN.value(n); }

void DirectSuggestions::loadThumbs()
{
    if (!m_thumbs || !m_thumbs->hasDecoder()) return;
    const QSize size = kThumb * devicePixelRatioF();
    for (int i = 0; i < m_list->count(); ++i) {
        const int n = m_list->item(i)->data(kNumberRole).toInt();
        const QJsonObject s = m_byN.value(n);
        const QString file = s.value("file").toString();
        if (!QFileInfo(file).isFile()) continue;
        m_thumbs->request(file, s.value("from_s").toDouble(), size, m_gen, this, [this, n](const QImage& img) {
            for (int i = 0; i < m_list->count(); ++i)
                if (QListWidgetItem* item = m_list->item(i); item->data(kNumberRole).toInt() == n) {
                    const QSignalBlocker b(m_list);
                    item->setIcon(QIcon(QPixmap::fromImage(img)));
                }
        });
    }
}

bool DirectSuggestions::eventFilter(QObject* obj, QEvent* e)
{
    // The row and its check state at the press: a click that toggles the checkbox is no preview (itemClicked)
    if (obj == m_list->viewport() && e->type() == QEvent::MouseButtonPress) {
        const auto* me = static_cast<QMouseEvent*>(e);
        if (QListWidgetItem* item = m_list->itemAt(me->position().toPoint())) {
            m_pressedN = item->data(kNumberRole).toInt();
            m_pressedState = item->checkState();
        } else {
            m_pressedN = -1;
        }
    }
    // Delete takes rows out and never reaches the window (timeline clips)
    if (obj == m_list && (e->type() == QEvent::ShortcutOverride || e->type() == QEvent::KeyPress)
        && InputBindings::matches(m_remove, static_cast<QKeyEvent*>(e))) {
        e->accept();
        if (e->type() == QEvent::KeyPress) m_remove->trigger();
        return e->type() == QEvent::KeyPress;
    }
    return QWidget::eventFilter(obj, e);
}
