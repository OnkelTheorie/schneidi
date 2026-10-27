#include "ui/DeliverPanel.h"

#include "app/Theme.h"
#include "core/I18n.h"
#include "core/ExportCheck.h"
#include "core/Project.h"
#include "core/Timecode.h"
#include "core/TimelineOps.h"
#include "engine/AudioAnalysis.h"
#include "engine/CheckMeasure.h"
#include "engine/Exporter.h"
#include "engine/HardwareEncoders.h"
#include "engine/RenderQueue.h"
#include "ui/FileManager.h"
#include "ui/RenderQueuePanel.h"

#include <QApplication>
#include <QCheckBox>
#include <QProgressDialog>
#include <QComboBox>
#include <QStandardItemModel>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>

namespace {

// Smart render copies at "High" and "Web" (below its cap) only: copied pictures keep the file's bit rate
bool smartQuality(int quality)
{
    return quality == 0 || quality == RenderSettings::kQualityWeb;
}

// Same output? (resolution resolved via the timeline, quality/bit rate only where they take effect)
bool sameOutput(const RenderSettings& a, const RenderSettings& b, QSize timeline)
{
    if (a.format != b.format) return false;
    const RenderFormatInfo& f = renderFormat(a.format);
    if (!a.audioOnly() && a.outputSize(timeline) != b.outputSize(timeline)) return false;
    if (f.hasQuality() && a.quality != b.quality) return false;
    if (f.hardware && a.encoder != b.encoder) return false;
    if (f.hardware && smartQuality(a.quality) && a.smartRender != b.smartRender) return false;
    if (a.hasAudioBitrate() && a.audioBitrateK != b.audioBitrateK) return false;
    if (!a.audioBitChoices().isEmpty() && a.audioCodec() != b.audioCodec()) return false;
    if (a.audioSampleFormat() != b.audioSampleFormat()) return false;
    const FrameRate none{0, 1};
    if (!a.audioOnly() && a.outputRate(none) != b.outputRate(none)) return false;
    return a.subtitles == b.subtitles;
}

QString smartTip()
{
    return T("Smart Render: Wo ein Clip ohne Effekt, Übergang oder Titel läuft und die Datei schon passt "
             "(gleicher Codec, Größe, Bildrate), werden ihre Bilder verlustfrei übernommen statt neu kodiert – "
             "um ein Vielfaches schneller. Nur Schnitte bis zum nächsten Schlüsselbild und Stellen mit Effekten "
             "werden kodiert. Passt etwas nicht, wird automatisch normal kodiert.");
}

} // namespace

DeliverPanel::DeliverPanel(Project* project, QWidget* parent)
    : QWidget(parent), m_project(project), m_queue(new RenderQueue(project, this))
{
    setObjectName("Panel");
    setMinimumWidth(300);
    m_queuePanel = new RenderQueuePanel(project, m_queue);

    auto* title = new QLabel(T("Render-Einstellungen"));
    title->setObjectName("PanelTitle");

    m_preset = new QComboBox;
    m_presetMenu = new QToolButton;
    m_presetMenu->setText("⋯");
    m_presetMenu->setToolTip(T("Vorlage speichern oder löschen"));
    connect(m_presetMenu, &QToolButton::clicked, this, &DeliverPanel::presetMenu);
    auto* presetRow = new QHBoxLayout;
    presetRow->addWidget(m_preset, 1);
    presetRow->addWidget(m_presetMenu);

    m_name = new QLineEdit;
    m_name->setObjectName("renderName");
    m_name->setToolTip(T("Name der Datei. Leer oder unverändert: der Name der geöffneten Timeline"));
    m_folder = new QLineEdit(QDir::toNativeSeparators(QStandardPaths::writableLocation(QStandardPaths::MoviesLocation)));
    m_folder->setObjectName("renderFolder");
    auto* browseBtn = new QToolButton;
    browseBtn->setText(T("Durchsuchen…"));
    connect(browseBtn, &QToolButton::clicked, this, &DeliverPanel::browse);
    auto* folderRow = new QHBoxLayout;
    folderRow->addWidget(m_folder, 1);
    folderRow->addWidget(browseBtn);

    m_format = new QComboBox;
    m_format->setObjectName("renderFormat");
    // Groups like DaVinci's format list: delivery, mastering, GIF, image sequences, audio only
    const auto group = [](const RenderFormatInfo& f) { return int(f.kind) * 2 + (f.hasQuality() ? 0 : 1); };
    for (const RenderFormatInfo& f : renderFormats()) {
        if (m_format->count() > 0 && group(renderFormat(m_format->itemData(m_format->count() - 1).toString())) != group(f))
            m_format->insertSeparator(m_format->count());
        m_format->addItem(T(f.label), QString(f.id));
    }

    m_resolution = new QComboBox;
    // Like DaVinci "Frame rate" when rendering: default = timeline, otherwise converted on export
    m_rate = new QComboBox;
    m_rate->setObjectName("renderRate");
    m_rate->addItem(QString(), QSize());
    for (const FrameRate& r : kExportOnlyRates) m_rate->addItem(QString("%1 fps").arg(r.label()), QSize(r.num, r.den));
    for (const FrameRate& r : kFrameRates) m_rate->addItem(QString("%1 fps").arg(r.label()), QSize(r.num, r.den));
    m_rate->setToolTip(T("Weicht die Bildrate von der Timeline ab, werden Bilder beim Export ausgelassen oder "
                         "wiederholt (der Ton bleibt unverändert)."));

    m_quality = new QComboBox;
    m_quality->addItem(T("Hoch"));
    m_quality->addItem(T("Mittel"));
    m_quality->addItem(T("Klein"));
    m_quality->addItem(T("Web (begrenzte Bitrate)"));
    m_quality->setItemData(RenderSettings::kQualityWeb, T("Hohe Qualität, Bitrate begrenzt wie für YouTube empfohlen (kleinere Datei)"), Qt::ToolTipRole);

    // Like DaVinci's "Encoder" (Auto / Native / NVIDIA …): only hardware encoders that work on this computer
    m_encoder = new QComboBox;
    m_encoder->setObjectName("renderEncoder");
    // currentIndexChanged, not activated: Qt sends activated only after currentIndexChanged, whose handler below
    // (updateControls -> updateEncoders) would otherwise put the old choice back first. Connected before it, so
    // the wish is known there; updateEncoders' own changes are blocked.
    connect(m_encoder, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int i) {
        if (i >= 0) m_wantedEncoder = VideoEncoder(m_encoder->itemData(i).toInt());
    });
    connect(HardwareEncoders::notifier(), &HardwareEncoders::Notifier::probed, this, [this] {
        updateEncoders();
        syncPresetToSettings();
    });

    m_audioBitrate = new QComboBox;
    for (int k : {128, 192, 256, 320}) m_audioBitrate->addItem(T("%1 kbit/s").arg(k), k);

    // Bit depth (WAV/AIFF/ProRes/Apple Lossless); entries depending on the format, see updateControls
    m_audioBits = new QComboBox;
    m_audioBits->setObjectName("renderAudioBits");
    m_audioBits->setPlaceholderText(T("Bei diesem Format fest")); // compressed formats: no choices, not an empty box
    connect(m_audioBits, qOverload<int>(&QComboBox::activated), this,
            [this] { m_wantedBits = m_audioBits->currentData().toInt(); });

    // Like DaVinci: "Render: Entire Timeline / In/Out Range"
    // Subtitles like DaVinci "Subtitle Settings": burn in the visible subtitle track or as a separate file
    m_subtitles = new QComboBox;
    m_subtitles->addItem(T("Keine"), int(RenderSettings::NoSubtitles));
    m_subtitles->addItem(T("Ins Bild einbrennen"), int(RenderSettings::BurnSubtitles));
    m_subtitles->addItem(T("Als SRT-Datei daneben"), int(RenderSettings::SrtFile));
    m_subtitles->setToolTip(T("Gilt für die sichtbare Untertitelspur"));
    m_smart = new QCheckBox(T("Unveränderte Stellen kopieren"));
    m_smart->setObjectName("renderSmart");
    m_smart->setChecked(true);
    m_smart->setToolTip(smartTip());
    m_range = new QComboBox;
    m_range->addItem(T("Ganze Timeline"));
    m_range->addItem(T("In/Out-Bereich"));
    connect(project, &Project::timelineChanged, this, &DeliverPanel::updateRange);
    updateRange();
    // File name = the opened timeline's name (another timeline opened or renamed: follows, unless typed over)
    connect(project, &Project::currentSequenceChanged, this, &DeliverPanel::updateName);
    connect(project, &Project::sequencesChanged, this, &DeliverPanel::updateName);
    updateName();
    connect(project, &Project::formatChanged, this, &DeliverPanel::updateFormat);
    updateFormat();

    // Threads (logical processors) for rendering: applies per machine (not in presets/jobs), takes effect from the next job
    m_threads = new QComboBox;
    m_threads->setObjectName("renderThreads");
    const int available = Exporter::availableThreads();
    m_threads->addItem(T("Alle (%1)").arg(available), 0);
    for (int n = available - 1; n >= 1; --n) m_threads->addItem(QString::number(n), n);
    m_threads->setCurrentIndex(std::max(0, m_threads->findData(Exporter::savedThreads())));
    // Also with a graphics card encoder: it only compresses, decoding and computing the frames stays on the CPU
    m_threads->setToolTip(T("Wie viele Threads (logische Prozessoren) die Bilder dekodieren und berechnen. Gilt auch mit "
                          "Grafikkarten-Encoder: Die Grafikkarte komprimiert nur, Dekodieren, Effekte und Zusammensetzen "
                          "macht der Prozessor. Weniger = der Rechner bleibt nebenbei flüssiger. Mit Green Screen wird "
                          "trotzdem nur ein Bild nach dem anderen berechnet."));
    connect(m_threads, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this] { Exporter::setSavedThreads(m_threads->currentData().toInt()); });

    auto* form = new QFormLayout;
    form->setContentsMargins(12, 12, 12, 12);
    form->setVerticalSpacing(8);
    form->addRow(T("Vorlage"), presetRow);
    form->addRow(T("Dateiname"), m_name);
    form->addRow(T("Ort"), folderRow);
    form->addRow(T("Format"), m_format);
    form->addRow(T("Auflösung"), m_resolution);
    form->addRow(T("Bildrate"), m_rate);
    form->addRow(T("Qualität"), m_quality);
    form->addRow(T("Encoder"), m_encoder);
    form->addRow(T("Audio-Bitrate"), m_audioBitrate);
    form->addRow(T("Audio-Bittiefe"), m_audioBits);
    form->addRow(T("Untertitel"), m_subtitles);
    form->addRow(T("Smart Render"), m_smart);
    form->addRow(T("Bereich"), m_range);
    form->addRow(T("CPU-Threads"), m_threads);

    m_addBtn = new QPushButton(T("Zur Render-Warteschlange hinzufügen"));
    m_addBtn->setMinimumHeight(30);
    m_addBtn->setStyleSheet(Theme::primaryButtonStyle());
    m_checkBtn = new QPushButton(T("Vor dem Export prüfen"));
    m_checkBtn->setToolTip(T("Sucht, was in fertigen Videos oft schiefgeht: Lücken ohne Bild, fehlende Dateien, "
                             "Ruckler in Aufnahmen, Untertitel zu kurz oder zu schnell, Ton übersteuert, zu leise oder "
                             "zu laut, Stille am Ende, Spielton oder Sprache unter der Musik; bei Hochformat Text, den "
                             "TikTok, Reels und Shorts verdecken, und ein langsamer Einstieg"));
    m_checkList = new QListWidget;
    m_checkList->setWordWrap(true);
    m_checkList->setMaximumHeight(200);
    m_checkList->hide();
    m_fixBtn = new QPushButton;
    m_fixBtn->setObjectName("check_fix");
    m_fixBtn->hide();
    m_status = new QLabel;
    m_status->setWordWrap(true);
    m_status->setStyleSheet(QString("color: %1; padding: 0 12px;").arg(Theme::textDim.name()));

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 12);
    lay->setSpacing(6);
    lay->addWidget(title);
    lay->addLayout(form);
    lay->addStretch(1);
    auto* bottom = new QVBoxLayout;
    bottom->setContentsMargins(12, 0, 12, 0);
    bottom->addWidget(m_checkList);
    bottom->addWidget(m_fixBtn);
    bottom->addWidget(m_checkBtn);
    bottom->addWidget(m_addBtn);
    lay->addLayout(bottom);
    lay->addWidget(m_status);

    connect(m_preset, qOverload<int>(&QComboBox::activated), this, &DeliverPanel::applyPreset);
    for (QComboBox* c : {m_format, m_resolution, m_rate, m_quality, m_encoder, m_audioBitrate, m_audioBits, m_subtitles})
        connect(c, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
            updateControls();
            syncPresetToSettings();
        });
    connect(m_smart, &QCheckBox::toggled, this, [this] { syncPresetToSettings(); });
    connect(m_addBtn, &QPushButton::clicked, this, [this] { addToQueue(true); });
    connect(m_checkBtn, &QPushButton::clicked, this, [this] { runCheck(); });
    connect(m_project, &Project::currentSequenceChanged, this, [this] {
        if (m_fix && m_project->currentSequence() != m_fixSequence) {
            m_fix = {};
            m_fixBtn->hide();
        }
    });
    connect(m_fixBtn, &QPushButton::clicked, this, [this] {
        if (!m_fix || m_project->currentSequence() != m_fixSequence) return;
        m_project->edit(T("Prüfung: Ton behoben"), m_fix);
        m_fix = {};
        m_fixBtn->hide();
        QTimer::singleShot(0, this, [this] { runCheck(); }); // the result of the fix
    });
    connect(m_checkList, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        const int frame = item->data(Qt::UserRole).toInt();
        if (frame >= 0) emit seekRequested(frame);
    });
    // Double-click in the queue: take over the job's settings (like DaVinci "Edit Job")
    connect(m_queuePanel, &RenderQueuePanel::loadJobRequested, this, [this](int id) {
        for (const RenderJob& j : m_project->renderQueue())
            if (j.id == id) {
                setSettings(j.settings);
                if (!j.settings.audioOnly() && !j.size.isEmpty()) selectSize(j.size);
                const QFileInfo fi(j.path);
                m_name->setText(fi.completeBaseName());
                m_folder->setText(QDir::toNativeSeparators(fi.absolutePath()));
            }
    });

    connect(m_queuePanel, &RenderQueuePanel::revealFileRequested, this, [](const QString& path) { FileManager::reveal(path); });
    connect(m_queuePanel, &RenderQueuePanel::openFileRequested, this, [](const QString& path) { FileManager::open(path); });

    reloadPresets();
    selectPreset(0); // YouTube 1080p
    updateControls();
}

DeliverPanel::~DeliverPanel()
{
    delete m_queuePanel; // otherwise it would stay attached to the project with an orphaned queue
}

// ---------- Presets

void DeliverPanel::reloadPresets(const QString& select)
{
    m_presets = RenderPresets::all();
    const QSignalBlocker block(m_preset);
    m_preset->clear();
    m_preset->addItem(T("Eigene Einstellungen"));
    int selectIndex = -1;
    for (int i = 0; i < m_presets.size(); ++i) {
        if (i > 0 && m_presets[i - 1].builtin && !m_presets[i].builtin) m_preset->insertSeparator(m_preset->count());
        m_preset->addItem(m_presets[i].name, i);
        if (!select.isEmpty() && m_presets[i].name == select && !m_presets[i].builtin) selectIndex = m_preset->count() - 1;
    }
    if (selectIndex >= 0) m_preset->setCurrentIndex(selectIndex);
    else syncPresetToSettings();
}

int DeliverPanel::currentPreset() const
{
    const QVariant v = m_preset->currentData();
    return v.isValid() ? v.toInt() : -1;
}

void DeliverPanel::selectPreset(int index)
{
    const int combo = m_preset->findData(index);
    if (combo >= 0) applyPreset(combo);
}

void DeliverPanel::applyPreset(int comboIndex)
{
    // Combo index -> list index (mind the separator)
    const QVariant v = m_preset->itemData(comboIndex);
    if (!v.isValid()) return; // "Eigene Einstellungen": change nothing
    const RenderPreset& p = m_presets.value(v.toInt());
    m_applying = true;
    setSettings(p.settings);
    m_applying = false;
    m_preset->setCurrentIndex(comboIndex);
}

bool DeliverPanel::savePreset(const QString& rawName)
{
    const QString name = rawName.trimmed();
    if (name.isEmpty()) return false;
    for (const RenderPreset& p : RenderPresets::builtins())
        if (p.name == name) return false; // do not overwrite built-in ones
    QVector<RenderPreset> user = RenderPresets::loadUser();
    RenderPreset preset{name, settings(), false};
    const auto it = std::find_if(user.begin(), user.end(), [&](const RenderPreset& p) { return p.name == name; });
    if (it != user.end()) *it = preset;
    else user << preset;
    if (!RenderPresets::saveUser(user)) return false;
    reloadPresets(name);
    return true;
}

bool DeliverPanel::deletePreset(int listIndex)
{
    if (listIndex < 0 || listIndex >= m_presets.size() || m_presets[listIndex].builtin) return false;
    const QString name = m_presets[listIndex].name;
    QVector<RenderPreset> user = RenderPresets::loadUser();
    user.erase(std::remove_if(user.begin(), user.end(), [&](const RenderPreset& p) { return p.name == name; }), user.end());
    if (!RenderPresets::saveUser(user)) return false;
    reloadPresets();
    return true;
}

void DeliverPanel::presetMenu()
{
    QMenu menu(this);
    menu.addAction(T("Als neue Vorlage speichern…"), this, [this] {
        bool ok = false;
        const QString name = QInputDialog::getText(this, T("Vorlage speichern"), T("Name der Vorlage:"),
                                                   QLineEdit::Normal, m_name->text(), &ok);
        if (!ok || name.trimmed().isEmpty()) return;
        if (!savePreset(name))
            QMessageBox::warning(this, T("Vorlage speichern"),
                                 T("„%1“ konnte nicht gespeichert werden (eingebaute Vorlagen lassen sich nicht ersetzen).")
                                     .arg(name.trimmed()));
    });
    const int idx = currentPreset();
    const bool user = idx >= 0 && idx < m_presets.size() && !m_presets[idx].builtin;
    QAction* update = menu.addAction(T("Vorlage aktualisieren"), this, [this, idx] { savePreset(m_presets[idx].name); });
    QAction* del = menu.addAction(T("Vorlage löschen"), this, [this, idx] {
        if (QMessageBox::question(this, T("Vorlage löschen"), T("Vorlage „%1“ löschen?").arg(m_presets[idx].name))
            == QMessageBox::Yes)
            deletePreset(idx);
    });
    // Update: a custom preset with changed settings is currently "Eigene Einstellungen" -> remember the last one
    update->setEnabled(user);
    del->setEnabled(user);
    menu.exec(m_presetMenu->mapToGlobal(QPoint(0, m_presetMenu->height())));
}

void DeliverPanel::syncPresetToSettings()
{
    if (m_applying) return;
    const RenderSettings cur = settings();
    const QSize tl = m_project->format().size();
    // Current preset still matches -> stays; otherwise the first matching one, otherwise "Eigene Einstellungen"
    const int cur_idx = currentPreset();
    if (cur_idx >= 0 && cur_idx < m_presets.size() && sameOutput(m_presets[cur_idx].settings, cur, tl)) return;
    for (int i = 0; i < m_preset->count(); ++i) {
        const QVariant v = m_preset->itemData(i);
        if (v.isValid() && sameOutput(m_presets[v.toInt()].settings, cur, tl)) {
            m_preset->setCurrentIndex(i);
            return;
        }
    }
    m_preset->setCurrentIndex(0);
}

// ---------- Settings

RenderSettings DeliverPanel::settings() const
{
    RenderSettings s;
    s.format = m_format->currentData().toString();
    s.quality = m_quality->currentIndex();
    if (m_encoder->currentIndex() >= 0) s.encoder = VideoEncoder(m_encoder->currentData().toInt());
    s.audioBitrateK = m_audioBitrate->currentData().toInt();
    if (m_audioBits->currentIndex() >= 0) s.audioBits = m_audioBits->currentData().toInt();
    if (const QSize r = m_rate->currentData().toSize(); r.isValid() && !s.audioOnly()) {
        s.rateNum = r.width();
        s.rateDen = r.height();
    }
    s.subtitles = m_subtitles->currentData().toInt();
    s.smartRender = m_smart->isChecked();
    if (s.audioOnly() && s.subtitles == RenderSettings::BurnSubtitles) s.subtitles = RenderSettings::NoSubtitles;
    const QSize size = m_resolution->currentData().toSize();
    const QSize tl = m_project->format().size();
    if (size.isValid() && size != tl) {
        const int shortSide = std::min(size.width(), size.height());
        if (sizeForShortSide(tl, shortSide) == size) s.shortSide = shortSide; // aspect ratio of the timeline
        else s.size = size;
    }
    return s;
}

void DeliverPanel::setSettings(const RenderSettings& s)
{
    const bool wasApplying = m_applying;
    m_applying = true;
    m_wantedBits = s.audioBits;
    m_wantedEncoder = s.encoder;
    m_format->setCurrentIndex(std::max(0, m_format->findData(renderFormat(s.format).id)));
    m_quality->setCurrentIndex(std::clamp(s.quality, 0, RenderSettings::kQualityWeb));
    const int br = m_audioBitrate->findData(s.audioBitrateK);
    m_audioBitrate->setCurrentIndex(br >= 0 ? br : m_audioBitrate->count() - 1);
    updateControls(); // Bit depths of the format
    if (const int b = m_audioBits->findData(s.audioBits); b >= 0) m_audioBits->setCurrentIndex(b);
    m_rate->setCurrentIndex(s.rateNum > 0 ? std::max(0, m_rate->findData(QSize(s.rateNum, s.rateDen))) : 0);
    m_subtitles->setCurrentIndex(std::max(0, m_subtitles->findData(s.subtitles)));
    m_smart->setChecked(s.smartRender);
    if (!s.audioOnly()) selectSize(s.outputSize(m_project->format().size()));
    m_applying = wasApplying;
    updateControls();
    syncPresetToSettings();
}

int DeliverPanel::selectSize(QSize s)
{
    int i = m_resolution->findData(s);
    if (i < 0 && s.isValid() && !s.isEmpty()) {
        const QSize tl = m_project->format().size();
        const bool other = s.width() * tl.height() != s.height() * tl.width();
        QString label = resolutionLabel(s.width(), s.height());
        if (other) label += s.width() < s.height() ? " (9:16)" : " (16:9)";
        m_resolution->addItem(label, s);
        i = m_resolution->count() - 1;
    }
    if (i >= 0) m_resolution->setCurrentIndex(i);
    return i;
}

void DeliverPanel::updateControls()
{
    const RenderFormatInfo& f = renderFormat(m_format->currentData().toString());
    const bool video = *f.videoCodec;
    m_resolution->setEnabled(video);
    m_quality->setEnabled(f.hasQuality());
    m_quality->setToolTip(f.hasQuality() ? QString() : T("Bei diesem Format fest"));
    // "Web" (bit rate cap) only for H.264/H.265
    if (auto* model = qobject_cast<QStandardItemModel*>(m_quality->model()))
        if (QStandardItem* item = model->item(RenderSettings::kQualityWeb)) item->setEnabled(f.webQuality);
    if (!f.webQuality && f.hasQuality() && m_quality->currentIndex() == RenderSettings::kQualityWeb)
        m_quality->setCurrentIndex(0);
    updateEncoders();
    const bool copies = smartQuality(m_quality->currentIndex());
    m_smart->setEnabled(f.hardware && copies); // H.264/H.265
    m_smart->setToolTip(copies || !f.hardware
                            ? smartTip()
                            : T("Bei „Mittel“ und „Klein“ wird alles neu kodiert: kopierte Stellen behielten die Bitrate "
                                "der Originaldatei."));
    RenderSettings probe;
    probe.format = f.id;
    m_audioBitrate->setEnabled(probe.hasAudioBitrate());
    m_rate->setEnabled(video);
    // Bit depths of the format; the wanted one (preset/last choice) stays, otherwise 24 bit
    const QVector<int> bits = probe.audioBitChoices();
    QVector<int> shown;
    for (int i = 0; i < m_audioBits->count(); ++i) shown << m_audioBits->itemData(i).toInt();
    if (shown != bits) {
        const QSignalBlocker block(m_audioBits); // clear() would otherwise report a change -> updateControls() nested
        m_audioBits->clear();
        for (int b : bits) m_audioBits->addItem(b == 32 ? T("32 Bit Float") : T("%1 Bit").arg(b), b);
        const int want = m_audioBits->findData(m_wantedBits);
        m_audioBits->setCurrentIndex(want >= 0 ? want : std::max(0, m_audioBits->findData(24)));
    }
    m_audioBits->setEnabled(!bits.isEmpty());
    m_audioBits->setToolTip(bits.isEmpty() ? T("Bei diesem Format fest") : QString());
    // Audio only: nothing to burn in (entry grayed out, SRT beside it still works)
    if (auto* model = qobject_cast<QStandardItemModel*>(m_subtitles->model()))
        if (QStandardItem* item = model->item(1)) item->setEnabled(video);
    if (!video && m_subtitles->currentIndex() == 1) m_subtitles->setCurrentIndex(0);
}

void DeliverPanel::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    HardwareEncoders::probeInBackground(); // once: which hardware encoders work (does not block)
}

void DeliverPanel::updateEncoders()
{
    const RenderFormatInfo& f = renderFormat(m_format->currentData().toString());
    QVector<VideoEncoder> list{VideoEncoder::Auto, VideoEncoder::Software};
    const bool probed = HardwareEncoders::probed();
    const QVector<VideoEncoder> hardware = probed ? HardwareEncoders::available(f.id) : QVector<VideoEncoder>();
    list += hardware;
    // A hardware encoder from a preset/job that does not work here (or is not known yet) stays selectable
    const bool missing = f.hardware && !list.contains(m_wantedEncoder);
    if (missing) list << m_wantedEncoder;
    QStringList labels;
    for (VideoEncoder e : list) {
        QString label = videoEncoderLabel(e);
        if (e == VideoEncoder::Auto)
            label = hardware.isEmpty() ? T("Automatisch (Prozessor)")
                                       : T("Automatisch (%1)").arg(hardware.first() == VideoEncoder::Nvenc   ? "NVENC"
                                                                   : hardware.first() == VideoEncoder::Vaapi ? "VAAPI"
                                                                                                             : "Quick Sync");
        else if (missing && probed && e == m_wantedEncoder)
            label = T("%1 (nicht verfügbar)").arg(label);
        labels << label;
    }
    QStringList shown;
    for (int i = 0; i < m_encoder->count(); ++i) shown << m_encoder->itemText(i);
    if (shown != labels) {
        const QSignalBlocker block(m_encoder); // no nested updateControls()
        m_encoder->clear();
        for (int i = 0; i < list.size(); ++i) m_encoder->addItem(labels[i], int(list[i]));
    }
    if (f.hardware) {
        const QSignalBlocker block(m_encoder);
        m_encoder->setCurrentIndex(std::max(0, m_encoder->findData(int(m_wantedEncoder))));
    }
    m_encoder->setEnabled(f.hardware);
    m_encoder->setToolTip(!f.hardware ? T("Bei diesem Format fest")
                          : !probed     ? T("Grafikkarte wird noch geprüft …")
                                        : T("Prozessor (Software) = der Prozessor komprimiert das Video: kleinste "
                                            "Datei bei gleicher Qualität, aber langsamer. Die Hardware-Encoder der "
                                            "Grafikkarte komprimieren viel schneller, die Datei wird etwas größer. "
                                            "Klappt einer nicht, rechnet der Prozessor."));
}

void DeliverPanel::updateName()
{
    const QString name = renderFileBaseName(m_project->sequenceName(m_project->currentSequence()));
    const QString typed = m_name->text().trimmed();
    if (typed.isEmpty() || typed == m_autoName) m_name->setText(name);
    m_autoName = name;
}

void DeliverPanel::updateRange()
{
    // Range only selectable if In or Out is set; then preselected automatically
    const Timeline& tl = m_project->timeline();
    const bool has = tl.markIn >= 0 || tl.markOut >= 0;
    const int fps = m_project->fps();
    m_range->setItemText(1, has ? T("In/Out-Bereich") : T("In/Out-Bereich (nicht gesetzt)"));
    m_range->setToolTip(has ? QString("%1 – %2")
                                  .arg(tl.markIn >= 0 ? Timecode::format(tl.markIn, fps) : T("Anfang"),
                                       tl.markOut >= 0 ? Timecode::format(tl.markOut, fps) : T("Ende"))
                            : T("In/Out mit I und O in der Timeline setzen"));
    auto* model = qobject_cast<QStandardItemModel*>(m_range->model());
    if (auto* item = model ? model->item(1) : nullptr) item->setEnabled(has);
    if (has != m_hadRange || (!has && m_range->currentIndex() == 1)) m_range->setCurrentIndex(has ? 1 : 0);
    m_hadRange = has;
}

void DeliverPanel::updateFormat()
{
    // Like DaVinci: default is the timeline resolution; plus common sizes in the same aspect ratio
    // (portrait project -> 720 × 1280, 1080 × 1920 …) and the respective other format (16:9 resp. 9:16)
    const ProjectFormat& f = m_project->format();
    const QSize timeline = f.size();
    const QSize previous = m_resolution->currentData().toSize();
    QVector<QSize> sizes{timeline};
    for (int side : {720, 1080, 1440, 2160}) {
        const QSize s = sizeForShortSide(timeline, side);
        if (!sizes.contains(s)) sizes << s;
    }
    std::sort(sizes.begin(), sizes.end(), [](QSize a, QSize b) { return a.width() * a.height() < b.width() * b.height(); });
    const bool applying = m_applying;
    m_applying = true;
    m_resolution->clear();
    for (const QSize& s : sizes) {
        QString label = resolutionLabel(s.width(), s.height());
        if (s == timeline) label += " " + T("(Timeline)");
        m_resolution->addItem(label, s);
    }
    for (QSize other : {QSize(1080, 1920), QSize(1920, 1080)})
        if (!sizes.contains(other) && other.width() * timeline.height() != other.height() * timeline.width())
            selectSize(other);
    // If the timeline resolution was chosen, it stays the (new) timeline resolution
    const int keep = previous.isValid() && previous != m_timelineSize ? m_resolution->findData(previous) : -1;
    m_timelineSize = timeline;
    m_resolution->setCurrentIndex(keep >= 0 ? keep : m_resolution->findData(timeline));
    m_applying = applying;
    m_rate->setItemText(0, T("%1 fps (Timeline)").arg(f.rate.label()));
}

void DeliverPanel::browse()
{
    const QString dir = QFileDialog::getExistingDirectory(this, T("Speicherort wählen"), m_folder->text());
    if (!dir.isEmpty()) m_folder->setText(QDir::toNativeSeparators(dir));
}

// ---------- Queue

RenderJob DeliverPanel::makeJob() const
{
    RenderJob job;
    job.settings = settings();
    const RenderFormatInfo& f = renderFormat(job.settings.format);
    QString name = m_name->text().trimmed();
    const QString ext = QString(".") + f.extension;
    // Replace a different extension (e.g. .mp4 after switching to WAV)
    for (const RenderFormatInfo& other : renderFormats())
        if (name.endsWith(QString(".") + other.extension, Qt::CaseInsensitive)) {
            name.chop(int(qstrlen(other.extension)) + 1);
            break;
        }
    // Empty: the timeline's name. Typed names too: no characters a file system refuses (Windows: : ? * …)
    name = renderFileBaseName(name.isEmpty() ? m_project->sequenceName(m_project->currentSequence()) : name);
    job.path = QDir(QDir::fromNativeSeparators(m_folder->text().trimmed())).absoluteFilePath(name + ext);
    const int idx = currentPreset();
    if (idx >= 0 && idx < m_presets.size()) job.preset = m_presets[idx].name;
    if (!job.settings.audioOnly()) job.size = m_resolution->currentData().toSize();
    job.sequence = m_project->currentSequence(); // this timeline gets rendered, even if another one is open later
    if (m_range->currentIndex() == 1) {
        const Timeline& tl = m_project->timeline();
        job.inOut = true;
        job.from = std::max(0, tl.markIn);
        job.to = tl.markOut;
    }
    int maxId = 0;
    for (const RenderJob& j : m_project->renderQueue()) maxId = std::max(maxId, j.id);
    job.id = maxId + 1;
    return job;
}

bool DeliverPanel::addToQueue(bool confirm)
{
    const RenderJob job = makeJob();
    const QString file = QFileInfo(job.path).fileName();
    if (Exporter::readsFile(m_project->renderTimeline(job.sequence), job.path)) {
        QMessageBox::warning(this, T("Zur Render-Warteschlange hinzufügen"),
                             T("%1 wird in der Timeline verwendet und kann nicht überschrieben werden.").arg(file));
        return false;
    }
    QVector<RenderJob> q = m_project->renderQueue();
    const bool inQueue = std::any_of(q.cbegin(), q.cend(), [&](const RenderJob& j) {
        return j.path == job.path && j.status != RenderStatus::Done;
    });
    if (confirm && (inQueue || QFileInfo::exists(firstOutputFile(job.path, job.settings)))) {
        const QString text = inQueue ? T("%1 ist schon in der Render-Warteschlange. Trotzdem hinzufügen?").arg(file)
                                     : T("%1 existiert schon. Beim Rendern überschreiben?").arg(file);
        if (QMessageBox::question(this, T("Zur Render-Warteschlange hinzufügen"), text) != QMessageBox::Yes)
            return false;
    }
    q << job;
    m_project->setRenderQueue(q);
    m_status->setText(T("Job %1 hinzugefügt: %2").arg(job.id).arg(file));
    return true;
}

void DeliverPanel::prepareFix(const QVector<ExportCheck::Issue>& sound, double integratedLufs, const Timeline& tl)
{
    const ExportCheck::Options o;
    const auto has = [&sound](const char* check) {
        return std::any_of(sound.begin(), sound.end(), [check](const ExportCheck::Issue& i) { return i.check == check; });
    };
    const double ceiling = std::min(tl.masterLimiterDb, o.maxTruePeakDb);
    const bool limiterOn = tl.masterLimiter && tl.masterLimiterDb <= o.maxTruePeakDb;
    // Loudness: the master fader by the difference to the target (as far as it goes); up = the limiter catches the
    // peaks that grow with it
    double volume = tl.masterVolumeDb;
    if (has("loudness"))
        volume = std::clamp(tl.masterVolumeDb + (o.targetLufs - integratedLufs), kMinVolumeDb, kMaxVolumeDb);
    const bool louder = volume > tl.masterVolumeDb + 0.05, quieter = volume < tl.masterVolumeDb - 0.05;
    const bool limiter = !limiterOn && (has("clipping") || has("true_peak") || louder);
    if (!limiter && !louder && !quieter) return;
    volume = std::round(volume * 10) / 10;
    QStringList what;
    if (limiter) what << T("Limiter an (%1 dBTP)").arg(QString::number(ceiling, 'f', 1).replace('.', I18n::decimalPoint()));
    if (louder || quieter)
        what << T("Master %1%2 dB").arg(volume > tl.masterVolumeDb ? "+" : "")
                    .arg(QString::number(volume - tl.masterVolumeDb, 'f', 1).replace('.', I18n::decimalPoint()));
    m_fixBtn->setText(T("Beheben: %1").arg(what.join(", ")));
    m_fixBtn->setToolTip(T("Ein Rückgängig-Schritt im Mixer: %1. Danach wird neu geprüft.").arg(what.join(", ")));
    m_fix = [limiter, ceiling, volume, level = louder || quieter](Timeline& t) {
        if (limiter) {
            t.masterLimiter = true;
            t.masterLimiterDb = ceiling;
        }
        if (level) t.masterVolumeDb = volume;
    };
    m_fixSequence = m_project->currentSequence();
    m_fixBtn->show();
}

bool DeliverPanel::runCheck()
{
    const RenderJob job = makeJob();
    // Copies (cheap, shared data): the progress dialog lets events run, an AI command may change the project meanwhile
    const ProjectFormat f = m_project->format();
    const Timeline tl = m_project->timeline();
    const int end = TimelineOps::endFrame(tl);
    const int from = job.inOut ? job.from : 0;
    const int to = job.inOut && job.to >= 0 ? std::min(end, job.to + 1) : end;
    m_checkList->clear();
    m_fix = {};
    m_fixBtn->hide();
    if (to <= from) {
        m_checkList->hide();
        m_status->setText(T("Die Timeline ist leer."));
        return true;
    }
    QVector<ExportCheck::Issue> issues = ExportCheck::timeline(tl, from, to, f) + ExportCheck::layout(tl, from, to, f);
    // What this export leaves out does not matter: the picture for sound-only formats, subtitles when not exported
    issues.removeIf([&](const ExportCheck::Issue& i) {
        return (job.settings.audioOnly() && (i.check == "black" || i.check == "safe_area" || i.check.startsWith("subtitle")))
               || (job.settings.subtitles == RenderSettings::NoSubtitles && i.check.startsWith("subtitle"));
    });

    QString sound;
    bool canceled = false;
    QProgressDialog progress(T("Aufnahmen werden geprüft…"), T("Abbrechen"), 0, 1000, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(400);
    const auto step = [&progress](double a, double b) {
        return [&progress, a, b](double p) {
            progress.setValue(int((a + (b - a) * p) * 1000));
            QApplication::processEvents();
            return !progress.wasCanceled();
        };
    };
    const Timeline render = m_project->renderTimeline(job.sequence);
    const auto media = [this](const QString& path) { return m_project->mediaInfo(path); };
    const double picture = job.settings.hasAudio() ? 0.1 : 1.0;
    if (!job.settings.audioOnly()) {
        // Stutter in the recordings (time stamps, cached) and the start of a vertical video
        if (const auto st = CheckMeasure::stutter(
                f, tl, from, to, [&](const QString& p) { return media(p) ? media(p)->length : 0; }, step(0, picture)))
            issues += *st;
        canceled = progress.wasCanceled();
        if (!canceled) issues += CheckMeasure::hook(f, render, from, to);
    }
    if (job.settings.hasAudio() && !canceled) {
        progress.setLabelText(T("Ton wird gemessen…"));
        std::vector<float> peaks;
        const auto loud = AudioAnalysis::timelineLoudness(f, render, from, to, step(picture, 0.6), &peaks);
        if (loud && !progress.wasCanceled()) {
            const QVector<ExportCheck::Issue> heard =
                ExportCheck::sound(peaks, from, loud->integrated, loud->truePeakDb, f, {}, render.masterLimiter);
            issues += heard;
            prepareFix(heard, loud->integrated, tl);
            if (loud->integrated > -200)
                sound = T("Lautheit %1 LUFS, True Peak %2 dBTP")
                            .arg(QString::number(loud->integrated, 'f', 1).replace('.', I18n::decimalPoint()),
                                 QString::number(loud->truePeakDb, 'f', 1).replace('.', I18n::decimalPoint()));
            // Music against game sound / speech: each track alone
            const auto hasPicture = [&](const QString& p) { return media(p) && media(p)->hasVideo && !media(p)->isImage; };
            if (const auto balance = CheckMeasure::mix(f, render, from, to, hasPicture, {}, step(0.6, 1)))
                issues += *balance;
        }
        canceled = progress.wasCanceled();
    }

    std::stable_sort(issues.begin(), issues.end(), [](const ExportCheck::Issue& a, const ExportCheck::Issue& b) {
        return a.severity != b.severity ? int(a.severity) < int(b.severity) : a.from < b.from;
    });
    const int fps = m_project->fps();
    for (const ExportCheck::Issue& i : issues) {
        // Time first: the message may wrap
        const QString at = i.from >= 0 ? Timecode::format(i.from, fps) + "  " : QString();
        auto* item = new QListWidgetItem(QString::fromUtf8("● ") + at + i.message, m_checkList);
        item->setToolTip(i.message);
        item->setForeground(i.severity == ExportCheck::Severity::Error     ? Theme::warning
                            : i.severity == ExportCheck::Severity::Warning ? Theme::meterMid
                                                                           : Theme::textDim);
        item->setData(Qt::UserRole, i.from);
    }
    m_checkList->setVisible(!issues.isEmpty());
    const int errors = ExportCheck::errors(issues), warnings = ExportCheck::warnings(issues);
    QString summary = errors == 0 && warnings == 0 ? T("Prüfung: alles in Ordnung.")
                                                    : T("Prüfung: %1 Fehler, %2 Warnungen.").arg(errors).arg(warnings);
    if (!sound.isEmpty()) summary += ' ' + sound;
    if (canceled) summary += ' ' + T("Ton nicht geprüft (abgebrochen).");
    m_status->setText(summary);
    return !canceled;
}
