#include "app/ProjectSettingsDialog.h"

#include "app/Theme.h"
#include "core/I18n.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QSpinBox>
#include <QVBoxLayout>

ProjectSettingsDialog::ProjectSettingsDialog(const ProjectFormat& current, bool rateLocked, QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(T("Projekteinstellungen"));

    auto* title = new QLabel(T("Timeline-Format"));
    title->setObjectName("PanelTitle");

    // Vorlagen wie DaVinci ("Timeline resolution"), dahinter "Eigene" mit freien Maßen
    m_resolution = new QComboBox;
    for (const auto& p : kResolutionPresets) m_resolution->addItem(resolutionLabel(p.width, p.height), QSize(p.width, p.height));
    m_resolution->addItem(T("Eigene"), QSize());

    auto makeSpin = [](int value) {
        auto* s = new QSpinBox;
        s->setRange(16, 8192);
        s->setSingleStep(2); // gerade Maße (Export in yuv420p)
        s->setValue(value);
        return s;
    };
    m_width = makeSpin(current.width);
    m_height = makeSpin(current.height);
    auto* sizeRow = new QHBoxLayout;
    sizeRow->addWidget(m_width);
    sizeRow->addWidget(new QLabel("×"));
    sizeRow->addWidget(m_height);
    sizeRow->addStretch(1);

    m_rate = new QComboBox;
    for (const FrameRate& r : kFrameRates) m_rate->addItem(r.label(), QVariant::fromValue(QSize(r.num, r.den)));
    int rateIndex = m_rate->findData(QSize(current.rate.num, current.rate.den));
    if (rateIndex < 0) { // ungewöhnliche Rate aus einer Projektdatei: trotzdem anzeigen
        m_rate->addItem(current.rate.label(), QSize(current.rate.num, current.rate.den));
        rateIndex = m_rate->count() - 1;
    }
    m_rate->setCurrentIndex(rateIndex);
    m_rate->setEnabled(!rateLocked);

    auto* form = new QFormLayout;
    form->setContentsMargins(12, 12, 12, 4);
    form->setVerticalSpacing(8);
    form->addRow(T("Timeline-Auflösung"), m_resolution);
    form->addRow(T("Größe"), sizeRow);
    form->addRow(T("Timeline-Framerate"), m_rate);

    auto* hint = new QLabel;
    hint->setWordWrap(true);
    hint->setStyleSheet(QString("color: %1; padding: 0 12px;").arg(Theme::textDim.name()));
    hint->setText(rateLocked ? T("Die Framerate lässt sich nicht mehr ändern, weil schon Clips in der Timeline liegen.")
                             : T("Beim ersten Clip in einer leeren Timeline bietet schneidi an, dessen Format zu übernehmen."));
    auto* note = new QLabel(T("Positionen, Beschneiden und Titel werden an eine neue Auflösung angepasst."));
    note->setWordWrap(true);
    note->setStyleSheet(hint->styleSheet());

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 10);
    lay->addWidget(title);
    lay->addLayout(form);
    lay->addWidget(hint);
    lay->addWidget(note);
    lay->addSpacing(6);
    auto* bottom = new QHBoxLayout;
    bottom->setContentsMargins(12, 0, 12, 0);
    bottom->addWidget(buttons);
    lay->addLayout(bottom);
    setMinimumWidth(420);

    connect(m_resolution, qOverload<int>(&QComboBox::activated), this, &ProjectSettingsDialog::presetChosen);
    connect(m_width, qOverload<int>(&QSpinBox::valueChanged), this, &ProjectSettingsDialog::sizeEdited);
    connect(m_height, qOverload<int>(&QSpinBox::valueChanged), this, &ProjectSettingsDialog::sizeEdited);
    sizeEdited();
}

void ProjectSettingsDialog::presetChosen()
{
    const QSize s = m_resolution->currentData().toSize();
    if (s.isEmpty()) { // "Eigene": Maße freigeben
        m_width->setFocus();
        return;
    }
    m_updating = true;
    m_width->setValue(s.width());
    m_height->setValue(s.height());
    m_updating = false;
}

void ProjectSettingsDialog::sizeEdited()
{
    if (m_updating) return;
    // Passende Vorlage anzeigen, sonst "Eigene"
    const int i = m_resolution->findData(QSize(m_width->value(), m_height->value()));
    m_resolution->setCurrentIndex(i >= 0 ? i : m_resolution->count() - 1);
}

ProjectFormat ProjectSettingsDialog::format() const
{
    ProjectFormat f;
    f.width = m_width->value() & ~1;
    f.height = m_height->value() & ~1;
    const QSize r = m_rate->currentData().toSize();
    f.rate = {r.width(), r.height()};
    return f;
}
