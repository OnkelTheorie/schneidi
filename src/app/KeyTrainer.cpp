// SPDX-License-Identifier: AGPL-3.0-only
#include "app/KeyTrainer.h"

#include "app/InputBindings.h"
#include "app/Theme.h"
#include "core/I18n.h"

#include <QAction>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QRandomGenerator>
#include <QVBoxLayout>
#include <algorithm>

namespace {

// The actions worth knowing by heart, most common first
const QStringList& commonIds()
{
    static const QStringList ids = {
        "split",       "play_pause",   "mark_in",      "mark_out",     "shuttle_back", "stop",
        "shuttle_fwd", "tool_blade",   "tool_select",  "ripple_delete", "add_marker",  "undo",
        "zoom_in",     "zoom_out",     "zoom_fit",     "snapping",     "insert_clip",  "overwrite_clip",
        "edit_prev",   "edit_next",    "trim_start",   "trim_end",     "marker_next",  "toggle_enabled",
        "project_save",
    };
    return ids;
}

QStringList keysOf(const QString& id)
{
    QStringList keys;
    for (const QString& in : InputBindings::instance().inputs(id))
        if (!in.isEmpty() && InputBindings::inputMouseButton(in) == Qt::NoButton) keys << in;
    return keys;
}

QString actionText(const QString& id)
{
    for (const auto& e : InputBindings::instance().entries())
        if (e.id == id) {
            QString t = e.action ? e.action->text() : e.text;
            t.remove('&');
            if (t.endsWith(QChar(0x2026))) t.chop(1); // "…"
            return t;
        }
    return id;
}

QString keyText(const QString& input)
{
    return InputBindings::inputText(input);
}

} // namespace

QStringList KeyTrainer::candidates()
{
    QStringList ids;
    for (const QString& id : commonIds())
        if (!keysOf(id).isEmpty() && InputBindings::instance().action(id)) ids << id;
    return ids;
}

KeyTrainer::KeyTrainer(QWidget* parent, quint32 seed) : QFrame(parent, Qt::Tool), m_seed(seed)
{
    setObjectName("key_trainer");
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(T("Tasten üben"));
    setFocusPolicy(Qt::StrongFocus);
    setStyleSheet(QString("QFrame#key_trainer { background: %1; }").arg(Theme::panelHeader.name()));

    m_progress = new QLabel;
    m_progress->setObjectName("trainer_progress");
    m_progress->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));
    m_ask = new QLabel(T("Drück die Taste für:"));
    m_action = new QLabel;
    m_action->setObjectName("trainer_action");
    m_action->setWordWrap(true);
    QFont big = m_action->font();
    big.setPointSizeF(big.pointSizeF() * 1.4);
    big.setBold(true);
    m_action->setFont(big);
    m_key = new QLabel;
    m_key->setObjectName("trainer_key");
    QFont cap = m_key->font();
    cap.setPointSizeF(cap.pointSizeF() * 1.2);
    cap.setBold(true);
    m_key->setFont(cap);
    m_key->setStyleSheet(QString("QLabel { background: %1; border: 1px solid %2; border-radius: 6px; padding: 4px 10px; }")
                             .arg(Theme::well.name(), Theme::controlLight.name()));
    m_feedback = new QLabel;
    m_feedback->setObjectName("trainer_feedback");
    m_feedback->setWordWrap(true);
    auto* quiet = new QLabel(T("Die Tasten lösen hier nichts aus."));
    quiet->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));
    quiet->setWordWrap(true);

    m_skip = new QPushButton(T("Überspringen"));
    m_again = new QPushButton(T("Nochmal"));
    m_close = new QPushButton(T("Beenden"));
    for (QPushButton* b : {m_skip, m_again, m_close}) b->setFocusPolicy(Qt::NoFocus); // keys stay with the card
    m_skip->setObjectName("trainer_skip");
    m_again->setObjectName("trainer_again");
    m_close->setObjectName("trainer_close");
    connect(m_skip, &QPushButton::clicked, this, &KeyTrainer::skip);
    connect(m_again, &QPushButton::clicked, this, &KeyTrainer::restart);
    connect(m_close, &QPushButton::clicked, this, &QWidget::close);

    auto* keyRow = new QHBoxLayout;
    keyRow->addWidget(m_key);
    keyRow->addStretch(1);
    auto* buttons = new QHBoxLayout;
    buttons->addWidget(m_skip);
    buttons->addWidget(m_again);
    buttons->addStretch(1);
    buttons->addWidget(m_close);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(16, 14, 16, 12);
    lay->addWidget(m_progress);
    lay->addSpacing(4);
    lay->addWidget(m_ask);
    lay->addWidget(m_action);
    lay->addLayout(keyRow);
    lay->addWidget(m_feedback);
    lay->addStretch(1);
    lay->addWidget(quiet);
    lay->addLayout(buttons);
    setMinimumWidth(380);

    restart();
}

void KeyTrainer::restart()
{
    // Up to 10 rounds from the common actions, in random order (fewer if the set has fewer keys)
    QStringList ids = candidates();
    QRandomGenerator rng = m_seed ? QRandomGenerator(m_seed) : QRandomGenerator::securelySeeded();
    for (qsizetype i = ids.size() - 1; i > 0; --i) ids.swapItemsAt(i, rng.bounded(int(i + 1)));
    m_ids = ids.mid(0, kRounds);
    m_round = 0;
    m_hits = 0;
    m_missed = false;
    m_feedback->clear();
    showRound();
}

QString KeyTrainer::currentId() const
{
    return finished() ? QString() : m_ids[m_round];
}

void KeyTrainer::skip()
{
    if (finished()) return;
    QStringList keys;
    for (const QString& in : keysOf(m_ids[m_round])) keys << keyText(in);
    m_feedback->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));
    m_feedback->setText(T("Übersprungen: %1").arg(keys.join(T(" oder "))));
    ++m_round;
    m_missed = false;
    showRound();
}

void KeyTrainer::showRound()
{
    m_skip->setVisible(!finished());
    m_again->setVisible(finished());
    m_close->setText(finished() ? T("Schließen") : T("Beenden"));
    if (finished()) {
        m_progress->setText(T("Fertig"));
        m_ask->setText(m_ids.isEmpty() ? T("Diese Belegung hat keine Tasten zum Üben.")
                                       : T("%1 von %2 beim ersten Versuch richtig.").arg(m_hits).arg(m_ids.size()));
        m_action->clear();
        m_action->hide();
        m_key->hide();
        return;
    }
    m_progress->setText(T("Runde %1 von %2 · %3 richtig").arg(m_round + 1).arg(m_ids.size()).arg(m_hits));
    m_ask->setText(T("Drück die Taste für:"));
    m_action->setText(actionText(m_ids[m_round]));
    m_action->show();
    QStringList keys;
    for (const QString& in : keysOf(m_ids[m_round])) keys << keyText(in);
    m_key->setText(keys.join(T(" oder ")));
    m_key->show();
}

bool KeyTrainer::event(QEvent* e)
{
    // Every key comes here as a key press: no window shortcut fires while the card has the focus
    if (e->type() == QEvent::ShortcutOverride) {
        e->accept();
        return true;
    }
    return QFrame::event(e);
}

void KeyTrainer::keyPressEvent(QKeyEvent* e)
{
    switch (e->key()) {
    case Qt::Key_Shift:
    case Qt::Key_Control:
    case Qt::Key_Alt:
    case Qt::Key_Meta:
    case Qt::Key_AltGr:
    case Qt::Key_unknown:
    case 0: return;
    default: break;
    }
    if (e->isAutoRepeat()) return;
    const Qt::KeyboardModifiers mods =
        e->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
    pressed(QKeySequence(QKeyCombination(mods, Qt::Key(e->key()))).toString(QKeySequence::PortableText));
}

void KeyTrainer::pressed(const QString& input)
{
    if (finished()) return;
    const QStringList keys = keysOf(m_ids[m_round]);
    const bool right = std::any_of(keys.begin(), keys.end(), [&](const QString& k) {
        return QKeySequence::fromString(k, QKeySequence::PortableText)
               == QKeySequence::fromString(input, QKeySequence::PortableText);
    });
    if (!right) {
        m_missed = true;
        m_feedback->setStyleSheet(QString("color: %1;").arg(Theme::warning.name()));
        m_feedback->setText(T("Das war %1. Nochmal!").arg(keyText(input)));
        return;
    }
    if (!m_missed) ++m_hits;
    m_feedback->setStyleSheet(QString("color: %1;").arg(Theme::meterLow.name()));
    m_feedback->setText(T("Richtig: %1").arg(keyText(input)));
    ++m_round;
    m_missed = false;
    showRound();
}
