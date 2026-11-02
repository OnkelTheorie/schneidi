// SPDX-License-Identifier: AGPL-3.0-only
#include "app/SpeechDialog.h"

#include "core/I18n.h"
#include "engine/Extensions.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

SpeechDialog::SpeechDialog(const QString& folder, QWidget* parent) : QDialog(parent), m_folder(folder)
{
    setWindowTitle(T("Sprechertext"));
    QSettings settings;
    m_text = new QPlainTextEdit;
    m_text->setPlaceholderText(T("Was die Stimme sagen soll …"));
    m_text->setPlainText(settings.value("speech/text").toString());
    m_voice = new QComboBox;
    m_speed = new QDoubleSpinBox;
    m_speed->setRange(Speech::kMinSpeed, Speech::kMaxSpeed);
    m_speed->setSingleStep(0.1);
    m_speed->setDecimals(2);
    m_speed->setValue(settings.value("speech/speed", 1.0).toDouble());
    m_speed->setSuffix(T("×"));
    m_speed->setToolTip(T("1 = normal, 2 = doppelt so schnell"));
    m_pause = new QDoubleSpinBox;
    m_pause->setRange(0, Speech::kMaxPause);
    m_pause->setSingleStep(0.1);
    m_pause->setDecimals(1);
    m_pause->setValue(settings.value("speech/pause", 0.2).toDouble());
    m_pause->setSuffix(T(" s"));
    m_pause->setToolTip(T("Stille nach jedem Satz"));
    m_captions = new QCheckBox(T("Wort-Untertitel"));
    m_captions->setChecked(settings.value("speech/captions", false).toBool());
    m_captions->setToolTip(T("Die Wörter als Untertitel, Wort für Wort zur Stimme (mit Whisper gemessen, sonst geschätzt)"));
    auto* form = new QFormLayout;
    form->addRow(T("Stimme"), m_voice);
    form->addRow(T("Tempo"), m_speed);
    form->addRow(T("Pause nach Sätzen"), m_pause);
    form->addRow(QString(), m_captions);

    m_missing = new QLabel;
    m_missing->setWordWrap(true);
    m_extensions = new QPushButton(T("Weitere Stimmen…"));
    connect(m_extensions, &QPushButton::clicked, this, [this] {
        emit openExtensions();
        fillVoices();
        updateState();
    });
    m_progress = new QProgressBar;
    m_progress->setRange(0, 1000);
    m_progress->hide();

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    m_preview = buttons->addButton(T("Anhören"), QDialogButtonBox::ActionRole);
    m_insert = buttons->addButton(T("Einfügen"), QDialogButtonBox::AcceptRole);
    connect(m_preview, &QPushButton::clicked, this, [this] { make(false); });
    connect(m_insert, &QPushButton::clicked, this, [this] { make(true); });
    connect(buttons, &QDialogButtonBox::rejected, this, &SpeechDialog::reject);
    connect(m_text, &QPlainTextEdit::textChanged, this, &SpeechDialog::updateState);

    auto* lay = new QVBoxLayout(this);
    lay->addWidget(m_text, 1);
    lay->addLayout(form);
    lay->addWidget(m_missing);
    lay->addWidget(m_extensions, 0, Qt::AlignLeft);
    lay->addWidget(m_progress);
    lay->addWidget(buttons);

    connect(&m_speaker, &Tts::Speaker::progress, this, [this](double p) { m_progress->setValue(int(p * 1000)); });
    connect(&m_speaker, &Tts::Speaker::finished, this, [this](bool ok, const QString& error) {
        m_progress->hide();
        updateState();
        if (!ok) {
            if (error != T("Abgebrochen")) QMessageBox::warning(this, windowTitle(), error);
            return;
        }
        if (m_insertWhenDone) QDialog::accept();
        else emit preview(m_speaker.result().path);
    });
    fillVoices();
    resize(520, 360);
    updateState();
}

void SpeechDialog::fillVoices()
{
    const QString current = m_voice->currentData().toString();
    const QString saved = current.isEmpty() ? QSettings().value("speech/voice").toString() : current;
    m_voice->clear();
    for (const QString& id : Extensions::installedVoices()) {
        const Extensions::Item* item = Extensions::find(id);
        m_voice->addItem(item ? T(item->name) : Extensions::voiceName(id), id);
    }
    int index = m_voice->findData(saved);
    if (index < 0) index = m_voice->findData(Tts::defaultVoice());
    m_voice->setCurrentIndex(std::max(0, index));
}

void SpeechDialog::updateState()
{
    const bool running = m_speaker.isRunning();
    const QString why = Tts::notReady(m_voice->currentData().toString());
    m_missing->setText(why);
    m_missing->setVisible(!why.isEmpty());
    m_extensions->setText(why.isEmpty() ? T("Weitere Stimmen…") : T("Erweiterungen öffnen…"));
    const bool ready = why.isEmpty() && !m_text->toPlainText().trimmed().isEmpty() && !running;
    m_preview->setEnabled(ready);
    m_insert->setEnabled(ready);
    m_voice->setEnabled(!running);
    m_text->setReadOnly(running);
}

void SpeechDialog::make(bool insert)
{
    QSettings settings;
    settings.setValue("speech/text", m_text->toPlainText());
    settings.setValue("speech/voice", m_voice->currentData());
    settings.setValue("speech/speed", m_speed->value());
    settings.setValue("speech/pause", m_pause->value());
    settings.setValue("speech/captions", m_captions->isChecked());
    Tts::Request req;
    req.text = m_text->toPlainText();
    req.options.voice = m_voice->currentData().toString();
    req.options.speed = m_speed->value();
    req.options.pause = m_pause->value();
    req.folder = m_folder;
    req.words = insert && m_captions->isChecked(); // the preview needs no word times
    m_insertWhenDone = insert;
    QString error;
    if (!m_speaker.start(req, &error)) {
        QMessageBox::warning(this, windowTitle(), error);
        return;
    }
    m_progress->setValue(0);
    m_progress->show();
    updateState();
}

bool SpeechDialog::captions() const { return m_captions->isChecked() && !m_speaker.result().words.isEmpty(); }

void SpeechDialog::reject()
{
    if (m_speaker.isRunning()) {
        m_speaker.cancel(); // finished(false) follows; the dialog stays open
        return;
    }
    QDialog::reject();
}
