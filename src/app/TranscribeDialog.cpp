// SPDX-License-Identifier: AGPL-3.0-only
#include "app/TranscribeDialog.h"

#include "core/I18n.h"
#include "core/Vocabulary.h"
#include "ui/timeline/TimelineOptions.h"
#include "engine/Extensions.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

namespace {
// What a model entry is (item data role beside the id/path in Qt::UserRole)
constexpr int kKindRole = Qt::UserRole + 1;
enum ModelKind { CatalogModel, CustomModel, AddModel };
// The quality levels, offered for download when missing: "Genau" (large-v3), "Schnell" (turbo), "Sehr schnell"
// (Parakeet, only for its languages)
const char* const kLevels[] = {"whisper-model-large", "whisper-model-turbo", "parakeet"};
QString megabytes(qint64 bytes) { return T("%1 MB").arg(bytes / 1e6, 0, 'f', 0); }
// The program a catalog model runs with is there or comes with the download (the whisper package)
bool runnable(const Extensions::Item& item)
{
    if (Extensions::find("whisper")) return true;
    return item.kind == Extensions::Kind::ParakeetModel ? !Extensions::parakeetProgram().isEmpty()
                                                        : !Extensions::whisperProgram().isEmpty();
}
} // namespace

TranscribeDialog::TranscribeDialog(const Timeline& timeline, const ProjectFormat& format, int from, int to,
                                   const QStringList& trackLabels, QWidget* parent)
    : QDialog(parent), m_timeline(timeline), m_format(format), m_from(from), m_to(to)
{
    setWindowTitle(T("Untertitel aus Audio erzeugen"));
    QSettings settings;
    auto* form = new QFormLayout;
    form->addRow(T("Bereich"), new QLabel(QString("%1 – %2").arg(TimelineOptions::formatTime(from, format.rate.fps()),
                                                                 TimelineOptions::formatTime(to, format.rate.fps()))));
    // Audio tracks: like the timeline sounds by default; unticking music/game sound helps whisper a lot
    QVector<int> used;
    for (int i = 0; i < timeline.audio.size() && i < trackLabels.size(); ++i)
        if (!trackLabels[i].isEmpty()) used << i;
    if (used.size() > 1) {
        bool anySolo = false;
        for (const Track& t : timeline.audio) anySolo = anySolo || t.solo;
        // The voice tracks alone ("Mikrofon") when there is more to hear: song lyrics never land in the subtitles
        Timeline named = timeline;
        for (int i = 0; i < named.audio.size(); ++i) named.audio[i].name = i < trackLabels.size() ? trackLabels[i] : QString();
        const QVector<int> voice = Transcript::voiceTracks(named, {});
        auto* tracks = new QVBoxLayout;
        tracks->setSpacing(2);
        for (int i : used) {
            const Track& t = timeline.audio[i];
            const QString& label = trackLabels[i];
            const qsizetype dash = label.lastIndexOf(QString::fromUtf8(" – "));
            // "A1  Mix – Mix": the stream named like the track is said once
            const QString stream = dash >= 0 ? label.mid(dash + 3) : QString();
            const bool same = dash >= 0 && label.left(dash).endsWith(" " + stream);
            auto* box = new QCheckBox(same ? label.left(dash) : label);
            box->setChecked(voice.isEmpty() ? !t.muted && (!anySolo || t.solo) : voice.contains(i));
            connect(box, &QCheckBox::toggled, this, &TranscribeDialog::updateState);
            tracks->addWidget(box);
            m_tracks << TrackBox{i, dash >= 0 ? label.mid(dash + 3) : trackShortName({TrackKind::Audio, i}), box};
        }
        m_separate = new QCheckBox(T("Eine Untertitelspur pro Tonspur"));
        m_separate->setToolTip(T("Jede angehakte Tonspur wird einzeln erkannt (genauer als gemischt). An: eine "
                                 "Untertitelspur je Tonspur, übereinander. Aus: alles in einer Spur, wer gleichzeitig "
                                 "spricht, steht in einer zweiten Zeile."));
        m_separate->setChecked(settings.value("transcribe/separate", true).toBool());
        auto* label = new QLabel(T("Tonspuren"));
        label->setToolTip(T("Nur die Spuren mit Sprache anhaken – Musik oder Spielton verschlechtern die Erkennung"));
        form->addRow(label, tracks);
        // The hint and the option as rows of their own: a wrapped label inside the tracks' layout got the height of
        // one line (its second line cut off, over the last track; user report 21.11.) - a form row gets its
        // heightForWidth
        if (!voice.isEmpty()) {
            auto* note = new QLabel(T("Nur die Stimme ist angehakt – Musik und Spielton würden sonst mitgeschrieben."));
            note->setObjectName("transcribe_voice_note");
            note->setWordWrap(true);
            note->setEnabled(false); // dim, a hint
            form->addRow(QString(), note);
        }
        form->addRow(QString(), m_separate);
    }
    m_lang = new QComboBox;
    m_lang->setObjectName("transcribe_language");
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
    // "Sehr schnell" (Parakeet) only for the languages it knows
    connect(m_lang, &QComboBox::currentIndexChanged, this, &TranscribeDialog::updateState);
    m_model = new QComboBox;
    m_model->setObjectName("transcribe_model");
    // Own models: the last entry asks for a whisper.cpp file; an own one can be taken out of the list again
    connect(m_model, &QComboBox::activated, this, [this](int index) {
        if (m_model->itemData(index, kKindRole).toInt() == AddModel) chooseCustomModel();
    });
    m_removeModel = new QToolButton;
    m_removeModel->setObjectName("transcribe_model_remove");
    m_removeModel->setText(QString::fromUtf8("✕"));
    m_removeModel->setAutoRaise(true);
    m_removeModel->setToolTip(T("Eigenes Modell aus der Liste nehmen (die Datei bleibt)"));
    connect(m_removeModel, &QToolButton::clicked, this, &TranscribeDialog::removeCustomModel);
    connect(m_model, &QComboBox::currentIndexChanged, this, &TranscribeDialog::updateRemoveButton);
    auto* modelRow = new QHBoxLayout;
    modelRow->setContentsMargins(0, 0, 0, 0);
    modelRow->addWidget(m_model, 1);
    modelRow->addWidget(m_removeModel);
    form->addRow(T("Modell"), modelRow);
    auto* where = new QLabel(QString("<a href=\"https://huggingface.co/ggerganov/whisper.cpp/tree/main\">%1</a>")
                                 .arg(T("Wo finde ich Modelle?").toHtmlEscaped()));
    where->setObjectName("transcribe_model_link");
    where->setOpenExternalLinks(true);
    where->setToolTip(T("whisper.cpp-Modelle (ggml-….bin) zum Herunterladen; danach mit „Eigenes Modell…“ wählen"));
    form->addRow(QString(), where);
    m_chars = new QSpinBox;
    m_chars->setRange(16, 120);
    m_chars->setValue(settings.value("transcribe/maxChars", 42).toInt());
    m_chars->setSuffix(T(" Zeichen"));
    m_chars->setToolTip(T("Längste Untertitelzeile; Sätze und längere Pausen beginnen immer einen neuen Untertitel"));
    // Cue length: short ones for TikTok/Shorts; "automatic" follows the timeline (vertical = short)
    m_length = new QComboBox;
    m_length->setObjectName("transcribe_length");
    const bool vertical = Subtitles::resolve(Subtitles::CueLength::Auto, format.size()) != Subtitles::CueLength::Normal;
    m_length->addItem(vertical ? T("Automatisch (Kurz, Hochformat)") : T("Automatisch (Normal)"), "auto");
    m_length->addItem(T("Normal (1–2 Zeilen)"), "normal");
    m_length->addItem(T("Kurz (eine Zeile, für TikTok/Shorts)"), "short");
    m_length->addItem(T("Sehr kurz (2–3 Wörter)"), "very_short");
    m_length->setCurrentIndex(std::max(0, m_length->findData(settings.value("transcribe/length", "auto"))));
    m_length->setToolTip(T("Kurz: höchstens ein kurzer Satz pro Untertitel, geteilt an Satzzeichen und Nebensätzen, "
                           "nie mitten im Wort. Automatisch: kurz bei Hochformat (9:16), sonst normal."));
    connect(m_length, &QComboBox::currentIndexChanged, this, &TranscribeDialog::updateState);
    form->addRow(T("Untertitel-Länge"), m_length);
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
    // Never narrower than its rows need: a layout with a wrapping label left the dialog at a default width that cut
    // the per-track option (Russian, large font)
    lay->setSizeConstraint(QLayout::SetMinimumSize);
    lay->addLayout(form);
    lay->addWidget(m_missing);
    lay->addWidget(m_extensions, 0, Qt::AlignLeft);
    lay->addWidget(m_progress);
    lay->addWidget(buttons);

    // A missing model (and the whisper package) downloaded before the run
    connect(&m_installer, &Extensions::Installer::progress, this, [this](qint64 received, qint64 total) {
        m_progress->setValue(total > 0 ? int(1000 * received / total) : 0);
        if (m_downloading)
            m_missing->setText(T("Lade %1 … %2 von %3").arg(T(m_downloading->name), megabytes(received), megabytes(total)));
    });
    connect(&m_installer, &Extensions::Installer::finished, this, [this](bool ok, const QString& message) {
        const Extensions::Item* item = m_downloading;
        m_downloading = nullptr;
        if (ok) return downloadNext();
        m_downloads.clear();
        m_progress->hide();
        updateState();
        if (message != T("Abgebrochen") && item)
            QMessageBox::warning(this, windowTitle(),
                                 T("%1 konnte nicht installiert werden:\n%2").arg(T(item->name), message));
    });
    connect(&m_transcriber, &Transcriber::progress, this, [this](double p) {
        m_progress->setValue(int((m_job + p) / std::max<qsizetype>(1, m_jobs.size()) * 1000));
    });
    connect(&m_transcriber, &Transcriber::finished, this, [this](bool ok, const QString& error) {
        if (!ok) {
            m_progress->hide();
            updateState();
            if (error != T("Abgebrochen")) QMessageBox::warning(this, windowTitle(), error);
            return;
        }
        m_language = m_transcriber.result().language;
        // Parakeet did not know the language (whisper wrote it, or it may be poor): said after the run
        if (const QString note = m_transcriber.engineNote(); !note.isEmpty()) m_engineNote = note;
        if (const QString heard = m_transcriber.otherLanguage(); !heard.isEmpty() && m_languageHint.isEmpty()) {
            const auto name = [this](const QString& code) {
                const int i = m_lang->findData(code);
                return i > 0 ? m_lang->itemText(i) : code;
            };
            m_languageHint = T("Whisper hört eher %1 (zu %2 %) als die gewählte Sprache %3. Falls das stimmt, die "
                               "Untertitel mit „Automatisch erkennen“ neu erzeugen.")
                                 .arg(name(heard))
                                 .arg(int(std::lround(m_transcriber.detectedProbability() * 100)))
                                 .arg(name(m_language));
        }
        Transcript::Result r = m_transcriber.result();
        const double fps = m_format.rate.fps();
        // The words belong to their clip, no subtitle spans a cut between clips (the next clip's line showed at the
        // end of the clip before, user report 23.11.)
        const QVector<qint64> cuts =
            Transcript::cutsMs(Transcript::cutFrames(m_timeline, m_from, m_to, m_jobs[m_job].tracks), m_from, fps);
        Transcript::fitToCuts(r.words, cuts);
        QVector<SubtitleCue> cues;
        if (const Subtitles::CueLength length = cueLength(); length == Subtitles::CueLength::Normal) {
            cues = Transcript::toCues(r.words, fps, m_chars->value(), 700, m_from, cuts);
            Transcript::startAtSound(cues, r.levels, fps, m_from);
            Transcript::endAtSound(cues, r.levels, fps, m_from);
        } else { // short cues, each word keeps its time (Transcript panel, re-split later)
            cues = Transcript::readableCues(r.words, r.levels, fps, m_from, false, Subtitles::rulesFor(length), cuts);
        }
        for (SubtitleCue& c : cues) c.end = std::min(c.end, m_to);
        if (!cues.isEmpty()) m_results << Result{m_jobs[m_job].name, cues};
        if (++m_job < m_jobs.size()) return startJob();
        if (m_results.size() > 1 && !m_separate->isChecked()) {
            QVector<QVector<SubtitleCue>> all;
            for (const Result& r : m_results) all << r.cues;
            m_results = {Result{T("Transkript"), Transcript::mergeCues(all)}};
        }
        if (m_results.isEmpty()) {
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
    // No entry or "Eigenes Modell…" current (the file dialog, an own model just added or removed): the model before
    const bool none = m_model->currentIndex() < 0 || m_model->currentData(kKindRole).toInt() == AddModel;
    const QString current = none ? m_lastModel : m_model->currentData().toString();
    const QSignalBlocker block(m_model);
    m_model->clear();
    auto* items = qobject_cast<QStandardItemModel*>(m_model->model());
    const auto addCatalog = [&](const Extensions::Item& item) {
        const bool installed = Extensions::isInstalled(item);
        const int i = m_model->count();
        m_model->addItem(installed ? T(item.name) : T("%1 – Download %2").arg(T(item.name), megabytes(item.size)), item.id);
        m_model->setItemData(i, CatalogModel, kKindRole);
        m_model->setItemData(i, T(item.description), Qt::ToolTipRole);
    };
    // The quality levels first, missing ones as a download; "Sehr schnell" only for a language Parakeet knows
    const QString language = m_lang->currentData().toString();
    for (const char* id : kLevels) {
        const Extensions::Item* item = Extensions::find(id);
        if (!item || !runnable(*item)) continue;
        if (item->kind == Extensions::Kind::ParakeetModel && language != QLatin1String("auto")
            && !Transcriber::parakeetLanguage(language))
            continue;
        addCatalog(*item);
    }
    // Other installed catalog models (small, base, tiny)
    for (const QString& id : Extensions::installedWhisperModels())
        if (m_model->findData(id) < 0) addCatalog(*Extensions::find(id));
    // Own models after the catalog: by file name, the full path as tooltip; a file that is gone stays (marked, off)
    for (const QString& path : Extensions::customWhisperModels()) {
        const QFileInfo file(path);
        const bool there = file.isFile();
        const int i = m_model->count();
        m_model->addItem(there ? file.fileName() : T("%1 (fehlt)").arg(file.fileName()), file.absoluteFilePath());
        m_model->setItemData(i, CustomModel, kKindRole);
        m_model->setItemData(i, QDir::toNativeSeparators(file.absoluteFilePath()), Qt::ToolTipRole);
        if (!there && items) items->item(i)->setEnabled(false);
    }
    m_model->addItem(T("Eigenes Modell…"));
    m_model->setItemData(m_model->count() - 1, AddModel, kKindRole);
    m_model->setItemData(m_model->count() - 1, T("Ein whisper.cpp-Modell (ggml-….bin) von der Festplatte wählen"),
                         Qt::ToolTipRole);
    const auto usable = [&](int i) {
        return i >= 0 && m_model->itemData(i, kKindRole).toInt() != AddModel && (!items || items->item(i)->isEnabled());
    };
    const auto installed = [&](int i) {
        if (!usable(i)) return false;
        const Extensions::Item* item = Extensions::find(m_model->itemData(i).toString());
        return !item || Extensions::isInstalled(*item); // an own model whose file is there
    };
    // The previous choice, else the stored one, else the default quality (large-v3 with a graphics card, else turbo)
    // when it is installed, else the first installed model, else the default quality as a download
    int pick = m_model->findData(current.isEmpty() ? QSettings().value("transcribe/model").toString() : current);
    if (!usable(pick)) pick = -1;
    const int preferred = m_model->findData(Extensions::qualityModel(Extensions::defaultQuality()));
    if (pick < 0 && installed(preferred)) pick = preferred;
    for (int i = 0; pick < 0 && i < m_model->count(); ++i)
        if (installed(i)) pick = i;
    if (pick < 0 && usable(preferred)) pick = preferred;
    for (int i = 0; pick < 0 && i < m_model->count(); ++i)
        if (usable(i)) pick = i;
    m_model->setCurrentIndex(pick);
    m_lastModel = m_model->currentData().toString();
    updateRemoveButton();
    const bool downloading = m_installer.isRunning() || !m_downloads.isEmpty();
    const bool running = m_transcriber.isRunning() || downloading;
    const bool ready = usable(m_model->currentIndex());
    const QVector<const Extensions::Item*> missing = ready ? missingDownloads() : QVector<const Extensions::Item*>{};
    if (!downloading) {
        qint64 bytes = 0;
        for (const Extensions::Item* i : missing) bytes += i->size;
        m_missing->setText(!ready ? T("Dafür wird die Erweiterung „Whisper“ mit einem Sprachmodell gebraucht (einmaliger Download).")
                                  : T("Wird beim Start einmalig heruntergeladen (%1).").arg(megabytes(bytes)));
        m_missing->setVisible(!ready || !missing.isEmpty());
    }
    m_extensions->setVisible(!ready);
    m_start->setText(missing.isEmpty() ? T("Erzeugen") : T("Herunterladen und erzeugen"));
    int ticked = 0;
    for (const TrackBox& t : m_tracks) {
        ticked += t.box->isChecked();
        t.box->setEnabled(!running);
    }
    if (m_separate) m_separate->setEnabled(!running && ticked > 1);
    m_start->setEnabled(ready && (m_tracks.isEmpty() || ticked > 0) && !running);
    m_lang->setEnabled(!running);
    m_model->setEnabled(!running);
    m_removeModel->setEnabled(!running);
    m_length->setEnabled(!running);
    m_chars->setEnabled(!running && cueLength() == Subtitles::CueLength::Normal); // short ones have their own size
}

QVector<const Extensions::Item*> TranscribeDialog::missingDownloads() const
{
    const Extensions::Item* item = Extensions::find(m_model->currentData().toString());
    if (!item || m_model->currentData(kKindRole).toInt() != CatalogModel) return {}; // an own model: its file
    QVector<const Extensions::Item*> out;
    for (const Extensions::Item* i : Extensions::withDependencies({item}))
        if (!Extensions::isInstalled(*i)) out << i;
    return out;
}

void TranscribeDialog::updateRemoveButton()
{
    m_removeModel->setVisible(m_model->currentData(kKindRole).toInt() == CustomModel);
}

bool TranscribeDialog::addCustomModel(const QString& path, QString* error)
{
    if (!Extensions::isWhisperModelFile(path, error)) return false;
    const QString file = QFileInfo(path).absoluteFilePath();
    QStringList own = Extensions::customWhisperModels();
    if (!own.contains(file)) Extensions::setCustomWhisperModels(own << file);
    m_lastModel = file;
    m_model->setCurrentIndex(-1); // updateState picks m_lastModel: the new file
    updateState();
    return true;
}

void TranscribeDialog::chooseCustomModel()
{
    QSettings settings;
    QString folder = settings.value("transcribe/customModelFolder").toString();
    if (folder.isEmpty() || !QFileInfo(folder).isDir())
        folder = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    const QString path = QFileDialog::getOpenFileName(this, T("Eigenes Whisper-Modell wählen"), folder,
                                                      T("whisper.cpp-Modelle (*.bin)") + ";;" + T("Alle Dateien (*)"));
    if (path.isEmpty()) { // cancelled: back to the model chosen before
        m_model->setCurrentIndex(-1); // updateState picks m_lastModel again
        updateState();
        return;
    }
    settings.setValue("transcribe/customModelFolder", QFileInfo(path).absolutePath());
    QString error;
    if (!addCustomModel(path, &error)) {
        QMessageBox::warning(this, windowTitle(), error);
        m_model->setCurrentIndex(-1); // updateState picks m_lastModel again
        updateState();
    }
}

void TranscribeDialog::removeCustomModel()
{
    if (m_model->currentData(kKindRole).toInt() != CustomModel) return;
    const QString file = m_model->currentData().toString();
    QStringList own = Extensions::customWhisperModels();
    for (qsizetype i = own.size() - 1; i >= 0; --i) // compared as shown (absolute), however it was stored
        if (QFileInfo(own[i]).absoluteFilePath() == file) own.removeAt(i);
    Extensions::setCustomWhisperModels(own);
    QSettings settings;
    if (QFileInfo(settings.value("transcribe/model").toString()).absoluteFilePath() == file) settings.remove("transcribe/model");
    m_lastModel.clear();
    m_model->setCurrentIndex(-1);
    updateState(); // falls back to the stored or the best installed model
}

Subtitles::CueLength TranscribeDialog::cueLength() const
{
    Subtitles::CueLength length = Subtitles::CueLength::Auto;
    Subtitles::parseCueLength(m_length->currentData().toString(), &length);
    return Subtitles::resolve(length, m_format.size());
}

void TranscribeDialog::run()
{
    QSettings settings;
    settings.setValue("transcribe/language", m_lang->currentData());
    settings.setValue("transcribe/model", m_model->currentData());
    settings.setValue("transcribe/maxChars", m_chars->value());
    settings.setValue("transcribe/length", m_length->currentData()); // the last choice, "auto" too
    if (m_separate) settings.setValue("transcribe/separate", m_separate->isChecked());
    // One job for the mix (all ticked tracks, or the timeline as heard), or one per ticked track
    m_jobs.clear();
    m_results.clear();
    m_languageHint.clear();
    m_engineNote.clear();
    m_job = 0;
    // Always one whisper run per ticked track: in a mix whisper mixes up the speakers (measured: "oder" of one put
    // 3 s early onto the other's "Ja, ja"); without "one track each" the results are merged afterwards
    for (const TrackBox& t : m_tracks)
        if (t.box->isChecked()) m_jobs << Job{{t.index}, T("Transkript – %1").arg(t.name)};
    if (m_jobs.isEmpty()) m_jobs << Job{{}, T("Transkript")}; // single audio track: as heard
    // A missing model first (with the whisper package if that is missing too), then the jobs
    m_downloads = missingDownloads();
    downloadNext();
}

void TranscribeDialog::downloadNext()
{
    if (m_downloads.isEmpty()) {
        m_missing->hide();
        updateState();
        return startJob();
    }
    m_downloading = m_downloads.takeFirst();
    m_missing->setText(T("Lade %1 …").arg(T(m_downloading->name)));
    m_missing->show();
    m_progress->setValue(0);
    m_progress->show();
    updateState();
    QString error;
    if (!m_installer.start(*m_downloading, &error)) {
        const Extensions::Item* item = m_downloading;
        m_downloading = nullptr;
        m_downloads.clear();
        m_progress->hide();
        updateState();
        QMessageBox::warning(this, windowTitle(), T("%1 konnte nicht installiert werden:\n%2").arg(T(item->name), error));
    }
}

void TranscribeDialog::startJob()
{
    TranscribeRequest req;
    req.timeline = m_timeline;
    req.format = m_format;
    req.from = m_from;
    req.to = m_to;
    req.audioTracks = m_jobs[m_job].tracks;
    req.language = m_lang->currentData().toString();
    req.vocabulary = Vocabulary::stored(); // the word list of the settings
    // A chosen language is checked against whisper's own detection (a wrong one gives made-up words): told afterwards
    req.checkLanguage = req.language != QLatin1String("auto");
    // Parakeet ("Sehr schnell"): a language it does not know goes to the best whisper model there is
    const QString chosen = m_model->currentData().toString();
    req.model = chosen == QLatin1String("parakeet") ? Extensions::parakeetModel() : Extensions::whisperModelPath(chosen);
    if (Transcriber::isParakeet(req.model)) req.whisperModel = Extensions::appWhisperModel(QString());
    QString error;
    if (!m_transcriber.start(req, &error)) {
        m_progress->hide();
        updateState();
        QMessageBox::warning(this, windowTitle(), error);
        return;
    }
    m_progress->setValue(int(double(m_job) / m_jobs.size() * 1000));
    m_progress->show();
    updateState();
}

void TranscribeDialog::reject()
{
    if (m_installer.isRunning() || !m_downloads.isEmpty()) { // the download stops; the dialog stays open
        m_downloads.clear();
        m_installer.cancel(); // finished(false, "Abgebrochen") follows
        return;
    }
    if (m_transcriber.isRunning()) {
        m_transcriber.cancel(); // finished(false) follows; the dialog stays open
        return;
    }
    QDialog::reject();
}
