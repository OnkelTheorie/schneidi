// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/TimelineFormatDialog.h"

#include "core/I18n.h"
#include "core/ProjectFormat.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QSpinBox>
#include <QVBoxLayout>

namespace {
constexpr int kProject = 0; // first entry: the project's resolution
}

TimelineFormatDialog::TimelineFormatDialog(const QString& timeline, QSize own, QSize project, QWidget* parent)
    : QDialog(parent), m_project(project)
{
    setWindowTitle(T("Timeline-Auflösung"));
    m_choice = new QComboBox;
    m_choice->setObjectName("timeline_resolution");
    m_choice->addItem(T("Projekteinstellungen verwenden (%1)").arg(resolutionLabel(project.width(), project.height())),
                      QSize());
    for (const auto& p : kResolutionPresets)
        m_choice->addItem(resolutionLabel(p.width, p.height), QSize(p.width, p.height));
    m_choice->addItem(T("Eigene"), QSize(-1, -1));

    auto makeSpin = [](int value) {
        auto* s = new QSpinBox;
        s->setRange(kMinFrameSize, kMaxFrameSize);
        s->setSingleStep(2); // even dimensions (export in yuv420p)
        s->setValue(value);
        return s;
    };
    const QSize shown = own.isEmpty() ? project : own;
    m_width = makeSpin(shown.width());
    m_height = makeSpin(shown.height());
    auto* sizeRow = new QHBoxLayout;
    sizeRow->addWidget(m_width);
    sizeRow->addWidget(new QLabel("×"));
    sizeRow->addWidget(m_height);
    sizeRow->addStretch(1);

    auto* form = new QFormLayout;
    form->addRow(T("Timeline"), new QLabel(timeline));
    form->addRow(T("Auflösung"), m_choice);
    form->addRow(T("Größe"), sizeRow);
    auto* note = new QLabel(T("Positionen, Beschneiden und Titel dieser Timeline werden an eine neue Auflösung "
                              "angepasst. Die anderen Timelines bleiben, wie sie sind."));
    note->setWordWrap(true);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto* lay = new QVBoxLayout(this);
    lay->addLayout(form);
    lay->addWidget(note);
    lay->addWidget(buttons);
    setMinimumWidth(420);

    if (own.isEmpty()) {
        m_choice->setCurrentIndex(kProject);
    } else {
        const int i = m_choice->findData(own, Qt::UserRole);
        m_choice->setCurrentIndex(i > kProject ? i : m_choice->count() - 1);
    }
    connect(m_choice, qOverload<int>(&QComboBox::activated), this, &TimelineFormatDialog::chosen);
    connect(m_width, qOverload<int>(&QSpinBox::valueChanged), this, &TimelineFormatDialog::sizeEdited);
    connect(m_height, qOverload<int>(&QSpinBox::valueChanged), this, &TimelineFormatDialog::sizeEdited);
    chosen();
}

void TimelineFormatDialog::chosen()
{
    const QSize s = m_choice->currentData().toSize();
    const bool custom = s == QSize(-1, -1);
    m_updating = true;
    if (m_choice->currentIndex() == kProject) {
        m_width->setValue(m_project.width());
        m_height->setValue(m_project.height());
    } else if (!custom) {
        m_width->setValue(s.width());
        m_height->setValue(s.height());
    }
    m_updating = false;
    if (custom) m_width->setFocus();
}

void TimelineFormatDialog::sizeEdited()
{
    if (m_updating) return;
    // Typed: the matching preset, otherwise "Eigene" (never "project settings": the timeline then has its own size)
    const QSize typed(m_width->value(), m_height->value());
    int i = -1;
    for (int k = kProject + 1; k < m_choice->count(); ++k)
        if (m_choice->itemData(k).toSize() == typed) i = k;
    m_choice->setCurrentIndex(i >= 0 ? i : m_choice->count() - 1);
}

QSize TimelineFormatDialog::size() const
{
    if (m_choice->currentIndex() == kProject) return {};
    return {m_width->value() & ~1, m_height->value() & ~1};
}
