// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/direct/DirectWish.h"

#include "app/Theme.h"
#include "core/I18n.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QToolButton>
#include <QVBoxLayout>

DirectWish::DirectWish(QWidget* parent) : QWidget(parent)
{
    setObjectName("direct_wish");
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(6, 4, 6, 0);
    lay->setSpacing(2);
    m_edit = new QLineEdit;
    m_edit->setObjectName("direct_wish_edit");
    m_edit->setPlaceholderText(T("Eigener Wunsch, z. B. „nur Runden mit AWP“ oder „lustigste Stellen zuerst“"));
    m_edit->setToolTip(T("Die KI ändert die Vorschläge nach deinem Wunsch (Enter schickt ihn an den KI-Chat)"));
    m_edit->setClearButtonEnabled(true);
    lay->addWidget(m_edit);

    m_busyRow = new QWidget;
    auto* busy = new QHBoxLayout(m_busyRow);
    busy->setContentsMargins(0, 0, 0, 0);
    busy->setSpacing(6);
    m_busyText = new QLabel(T("KI denkt …"));
    m_busyText->setObjectName("direct_wish_busy");
    m_busyText->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));
    m_stop = new QToolButton;
    m_stop->setObjectName("direct_wish_stop");
    m_stop->setText("✕");
    m_stop->setAutoRaise(true);
    m_stop->setFocusPolicy(Qt::NoFocus);
    m_stop->setToolTip(T("Anfrage an die KI abbrechen"));
    busy->addWidget(m_busyText);
    busy->addWidget(m_stop);
    busy->addStretch(1);
    m_busyRow->hide();
    lay->addWidget(m_busyRow);

    m_note = new QLabel;
    m_note->setObjectName("direct_wish_note");
    m_note->setWordWrap(true);
    m_note->setTextFormat(Qt::PlainText);
    m_note->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_note->hide();
    lay->addWidget(m_note);

    connect(m_edit, &QLineEdit::returnPressed, this, [this] {
        const QString text = m_edit->text().trimmed();
        if (text.isEmpty() || m_busy) return;
        emit wishRequested(text);
    });
    connect(m_stop, &QToolButton::clicked, this, &DirectWish::stopRequested);
    setNote({});
}

void DirectWish::setBusy(bool busy)
{
    m_busy = busy;
    m_busyRow->setVisible(busy);
    m_edit->setReadOnly(busy);
    if (busy) setNote({});
}

void DirectWish::setNote(const QString& text, bool error)
{
    m_error = error && !text.isEmpty();
    m_note->setText(text);
    m_note->setStyleSheet(QString("color: %1;").arg((m_error ? Theme::warning : Theme::textDim).name()));
    m_note->setVisible(!text.isEmpty());
}

QString DirectWish::note() const { return m_note->text(); }
