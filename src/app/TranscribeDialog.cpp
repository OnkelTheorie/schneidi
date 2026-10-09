#include "app/TranscribeDialog.h"

#include "core/I18n.h"
#include "core/Timecode.h"
#include "engine/Extensions.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QVBoxLayout>

TranscribeDialog::TranscribeDialog(const Timeline& timeline, const ProjectFormat& format, int from, int to,
                                   QWidget* parent)
    : QDialog(parent), m_timeline(timeline), m_format(format), m_from(from), m_to(to)
{
    setWindowTitle(T("Untertitel aus Audio erzeugen"));
    QSettings settings;
    auto* form = new QFormLayout;
    form->addRow(T("Bereich"), new QLabel(QString("%1 – %2").arg(Timecode::format(from, format.rate.timebase()),
                                                                 Timecode::format(to, format.rate.timebase()))));
    m_lang = new QComboBox;
    m_lang->addItem(T("Automatisch erkennen"), "auto");
    static const struct {
        const char* code;
        const char* name;
    } kLanguages[] = {{"de", N_("Deutsch")},        {"en", N_("Englisch")}, {"fr", N_("Französisch")},
                      {"es", N_("Spanisch")},       {"it", N_("Italienisch")}, {"nl", N_("Niederländisch")},
                      {"pl", N_("Polnisch")},       {"pt", N_("Portugiesisch")}, {"tr", N_("Türkisch")},
                      {"ru", N_("Russisch")},       {"uk", N_("Ukrainisch")}};
    for (const auto& l : kLanguages) m_lang->addItem(T(l.name), l.code);
    m_lang->setCurrentIndex(std::max(0, m_lang->findData(settings.value("transcribe/language", "auto"))));
    form->addRow(T("Sprache"), m_lang);
    m_model = new QComboBox;
    form->addRow(T("Modell"), m_model);
    m_chars = new QSpinBox;
    m_chars->setRange(16, 120);
    m_chars->setValue(settings.value("transcribe/maxChars", 42).toInt());
    m_chars->setSuffix(T(" Zeichen"));
    m_chars->setToolTip(T("Längste Untertitelzeile; Sätze und längere Pausen beginnen immer einen neuen Untertitel"));
    form->addRow(T("Höchstens"), m_chars);

    m_missing = new QLabel;
    m_missing->setWordWrap(true);
    m_extensions = new QPushButton(T("Erweiterungen öffnen…"));
    connect(m_extensions, &QPushButton::clicked, this, [this] {
        emit openExtensions();
        updateState();
    });
    m_progress = new QProgressBar;
    m_progress->setRange(0, 1000);
    m_progress->hide();

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    m_start = buttons->addButton(T("Erzeugen"), QDialogButtonBox::AcceptRole);
    connect(m_start, &QPushButton::clicked, this, &TranscribeDialog::run);
    connect(buttons, &QDialogButtonBox::rejected, this, &TranscribeDialog::reject);

    auto* lay = new QVBoxLayout(this);
    lay->addLayout(form);
    lay->addWidget(m_missing);
    lay->addWidget(m_extensions, 0, Qt::AlignLeft);
    lay->addWidget(m_progress);
    lay->addWidget(buttons);

    connect(&m_transcriber, &Transcriber::progress, this, [this](double p) { m_progress->setValue(int(p * 1000)); });
    connect(&m_transcriber, &Transcriber::finished, this, [this](bool ok, const QString& error) {
        if (!ok) {
            m_progress->hide();
            updateState();
            if (error != T("Abgebrochen")) QMessageBox::warning(this, windowTitle(), error);
            return;
        }
        m_language = m_transcriber.result().language;
        m_cues = Transcript::toCues(m_transcriber.result().words, m_format.rate.fps(), m_chars->value(), 700, m_from);
        for (SubtitleCue& c : m_cues) c.end = std::min(c.end, m_to);
        if (m_cues.isEmpty()) {
            m_progress->hide();
            updateState();
            QMessageBox::information(this, windowTitle(), T("Im Ton wurde keine Sprache erkannt."));
            return;
        }
        QDialog::accept();
    });
    setMinimumWidth(460);
    updateState();
}

void TranscribeDialog::updateState()
{
    const QString current = m_model->currentData().toString();
    m_model->clear();
    for (const QString& id : Extensions::installedWhisperModels())
        m_model->addItem(T(Extensions::find(id)->name), id);
    const QString preferred = current.isEmpty() ? QSettings().value("transcribe/model").toString() : current;
    if (const int i = m_model->findData(preferred); i >= 0) m_model->setCurrentIndex(i);
    const bool program = !Extensions::whisperProgram().isEmpty();
    const bool ready = program && m_model->count() > 0;
    m_missing->setText(!program ? T("Dafür wird die Erweiterung „Whisper“ mit einem Sprachmodell gebraucht (einmaliger Download).")
                                : T("Es ist noch kein Whisper-Sprachmodell installiert."));
    m_missing->setVisible(!ready);
    m_extensions->setVisible(!ready);
    const bool running = m_transcriber.isRunning();
    m_start->setEnabled(ready && !running);
    m_lang->setEnabled(!running);
    m_model->setEnabled(!running);
    m_chars->setEnabled(!running);
}

void TranscribeDialog::run()
{
    QSettings settings;
    settings.setValue("transcribe/language", m_lang->currentData());
    settings.setValue("transcribe/model", m_model->currentData());
    settings.setValue("transcribe/maxChars", m_chars->value());
    TranscribeRequest req;
    req.timeline = m_timeline;
    req.format = m_format;
    req.from = m_from;
    req.to = m_to;
    req.language = m_lang->currentData().toString();
    req.model = Extensions::whisperModelPath(m_model->currentData().toString());
    QString error;
    if (!m_transcriber.start(req, &error)) {
        QMessageBox::warning(this, windowTitle(), error);
        return;
    }
    m_progress->setValue(0);
    m_progress->show();
    updateState();
}

void TranscribeDialog::reject()
{
    if (m_transcriber.isRunning()) {
        m_transcriber.cancel(); // finished(false) follows; the dialog stays open
        return;
    }
    QDialog::reject();
}
