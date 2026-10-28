// SPDX-License-Identifier: AGPL-3.0-only
#include "app/StreamPresetsDialog.h"
#include "core/I18n.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>
#include <algorithm>

namespace {
enum Column { Title, Action, Volume, TrackName };
}

StreamPresetsDialog::StreamPresetsDialog(const StreamPresets& presets, QWidget* parent) : QDialog(parent)
{
    setWindowTitle(T("Import-Vorgaben für Ton-Streams"));
    auto* layout = new QVBoxLayout(this);
    auto* intro = new QLabel(T("Gilt für Dateien mit mehreren Ton-Streams (z. B. OBS), wenn Clips in die Timeline "
                               "kommen. Regeln gehen nach dem Stream-Titel (Groß-/Kleinschreibung egal)."));
    intro->setWordWrap(true);
    layout->addWidget(intro);

    m_projectOnly = new QCheckBox(T("Nur für dieses Projekt (im Projekt gespeichert)"));
    m_projectOnly->setObjectName("projectOnly");
    m_projectOnly->setToolTip(T("Aus: gilt für alle Projekte ohne eigene Vorgaben (Einstellung von schneidi). Projekte aus "
                                "einer Startvorlage mit Import-Vorgaben haben eigene."));
    layout->addWidget(m_projectOnly);

    m_muteMix = new QCheckBox(T("Mix-Stream stumm schalten, wenn Einzelspuren da sind"));
    m_muteMix->setObjectName("muteMix");
    m_muteMix->setToolTip(T("Streams mit dem Titel „Mix“, „Mix 1“ … kommen deaktiviert in die Timeline, "
                            "damit nichts doppelt zu hören ist"));
    m_muteMix->setChecked(presets.muteMix);
    layout->addWidget(m_muteMix);

    m_rules = new QTableWidget(0, 4);
    m_rules->setObjectName("rules");
    m_rules->setHorizontalHeaderLabels({T("Stream-Titel"), T("Aktion"), T("Lautstärke"), T("Spurname")});
    m_rules->horizontalHeader()->setSectionResizeMode(Title, QHeaderView::Stretch);
    m_rules->horizontalHeader()->setSectionResizeMode(TrackName, QHeaderView::Stretch);
    m_rules->verticalHeader()->hide();
    m_rules->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_rules->setSelectionMode(QAbstractItemView::SingleSelection);
    for (const StreamRule& r : presets.rules) addRow(r);
    layout->addWidget(m_rules, 1);

    auto* buttons = new QHBoxLayout;
    auto* add = new QPushButton(T("Regel hinzufügen"));
    add->setObjectName("addRule");
    auto* remove = new QPushButton(T("Regel entfernen"));
    remove->setObjectName("removeRule");
    buttons->addWidget(add);
    buttons->addWidget(remove);
    buttons->addStretch();
    layout->addLayout(buttons);
    connect(add, &QPushButton::clicked, this, [this] {
        addRow({});
        m_rules->setCurrentCell(m_rules->rowCount() - 1, Title);
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        if (const int row = m_rules->currentRow(); row >= 0) m_rules->removeRow(row);
    });

    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(box);
    resize(620, 380);
}

void StreamPresetsDialog::addRow(const StreamRule& rule)
{
    const int row = m_rules->rowCount();
    m_rules->insertRow(row);
    m_rules->setItem(row, Title, new QTableWidgetItem(rule.title));
    auto* action = new QComboBox;
    action->addItems({T("Platzieren"), T("Stumm"), T("Weglassen")}); // order of StreamRule::Action
    action->setCurrentIndex(int(rule.action));
    m_rules->setCellWidget(row, Action, action);
    auto* volume = new QDoubleSpinBox;
    volume->setRange(kMinVolumeDb, kMaxVolumeDb);
    volume->setDecimals(1);
    volume->setSingleStep(0.5);
    volume->setSuffix(" dB");
    volume->setValue(rule.volumeDb);
    m_rules->setCellWidget(row, Volume, volume);
    m_rules->setItem(row, TrackName, new QTableWidgetItem(rule.trackName));
}

void StreamPresetsDialog::setProjectOnly(bool on) { m_projectOnly->setChecked(on); }

bool StreamPresetsDialog::projectOnly() const { return m_projectOnly->isChecked(); }

StreamPresets StreamPresetsDialog::presets() const
{
    StreamPresets p;
    p.muteMix = m_muteMix->isChecked();
    for (int row = 0; row < m_rules->rowCount(); ++row) {
        StreamRule r;
        r.title = m_rules->item(row, Title) ? m_rules->item(row, Title)->text().trimmed() : QString();
        if (r.title.isEmpty()) continue;
        if (auto* a = qobject_cast<QComboBox*>(m_rules->cellWidget(row, Action)))
            r.action = StreamRule::Action(std::clamp(a->currentIndex(), 0, 2));
        if (auto* v = qobject_cast<QDoubleSpinBox*>(m_rules->cellWidget(row, Volume))) r.volumeDb = v->value();
        r.trackName = m_rules->item(row, TrackName) ? m_rules->item(row, TrackName)->text().trimmed() : QString();
        p.rules << r;
    }
    return p;
}
