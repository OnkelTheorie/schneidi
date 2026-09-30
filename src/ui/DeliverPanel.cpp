#include "ui/DeliverPanel.h"

#include "app/Theme.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/Timecode.h"
#include "engine/Exporter.h"
#include "engine/RenderQueue.h"
#include "ui/RenderQueuePanel.h"

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
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>

namespace {

// Gleiche Ausgabe? (Auflösung über die Timeline aufgelöst, Qualität/Bitrate nur wo sie wirken)
bool sameOutput(const RenderSettings& a, const RenderSettings& b, QSize timeline)
{
    if (a.format != b.format) return false;
    const RenderFormatInfo& f = renderFormat(a.format);
    if (!a.audioOnly() && a.outputSize(timeline) != b.outputSize(timeline)) return false;
    if (f.hasQuality && a.quality != b.quality) return false;
    if (QLatin1String(f.audioCodec) == QLatin1String("aac") && a.audioBitrateK != b.audioBitrateK) return false;
    return a.subtitles == b.subtitles;
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

    m_name = new QLineEdit("Timeline 1");
    m_name->setObjectName("renderName");
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
    for (const RenderFormatInfo& f : renderFormats()) m_format->addItem(T(f.label), QString(f.id));

    m_resolution = new QComboBox;
    m_rate = new QLabel;

    m_quality = new QComboBox;
    m_quality->addItem(T("Hoch"));
    m_quality->addItem(T("Mittel"));
    m_quality->addItem(T("Klein"));

    m_audioBitrate = new QComboBox;
    for (int k : {128, 192, 256, 320}) m_audioBitrate->addItem(QString("%1 kbit/s").arg(k), k);

    // Wie DaVinci: "Render: Entire Timeline / In/Out Range"
    // Untertitel wie DaVinci „Subtitle Settings“: sichtbare Untertitelspur einbrennen oder als eigene Datei
    m_subtitles = new QComboBox;
    m_subtitles->addItem(T("Keine"), int(RenderSettings::NoSubtitles));
    m_subtitles->addItem(T("Ins Bild einbrennen"), int(RenderSettings::BurnSubtitles));
    m_subtitles->addItem(T("Als SRT-Datei daneben"), int(RenderSettings::SrtFile));
    m_subtitles->setToolTip(T("Gilt für die sichtbare Untertitelspur"));
    m_range = new QComboBox;
    m_range->addItem(T("Ganze Timeline"));
    m_range->addItem(T("In/Out-Bereich"));
    connect(project, &Project::timelineChanged, this, &DeliverPanel::updateRange);
    updateRange();
    connect(project, &Project::formatChanged, this, &DeliverPanel::updateFormat);
    updateFormat();

    // CPU-Kerne fürs Rendern: gilt pro Rechner (nicht in Vorlagen/Aufträgen), wirkt ab dem nächsten Auftrag
    m_cores = new QComboBox;
    m_cores->setObjectName("renderCores");
    const int available = Exporter::availableCores();
    m_cores->addItem(T("Alle (%1)").arg(available), 0);
    for (int n = available - 1; n >= 1; --n) m_cores->addItem(QString::number(n), n);
    m_cores->setCurrentIndex(std::max(0, m_cores->findData(Exporter::savedCores())));
    m_cores->setToolTip(T("Wie viele Prozessorkerne beim Rendern arbeiten. Weniger = der Rechner bleibt nebenbei "
                          "flüssiger. Mit Green Screen wird trotzdem nur ein Bild nach dem anderen berechnet."));
    connect(m_cores, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this] { Exporter::setSavedCores(m_cores->currentData().toInt()); });

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
    form->addRow(T("Audio-Bitrate"), m_audioBitrate);
    form->addRow(T("Untertitel"), m_subtitles);
    form->addRow(T("Bereich"), m_range);
    form->addRow(T("CPU-Kerne"), m_cores);

    m_addBtn = new QPushButton(T("Zur Render-Warteschlange hinzufügen"));
    m_addBtn->setMinimumHeight(30);
    m_addBtn->setStyleSheet(Theme::primaryButtonStyle());
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
    bottom->addWidget(m_addBtn);
    lay->addLayout(bottom);
    lay->addWidget(m_status);

    connect(m_preset, qOverload<int>(&QComboBox::activated), this, &DeliverPanel::applyPreset);
    for (QComboBox* c : {m_format, m_resolution, m_quality, m_audioBitrate, m_subtitles})
        connect(c, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
            updateControls();
            syncPresetToSettings();
        });
    connect(m_addBtn, &QPushButton::clicked, this, [this] { addToQueue(true); });
    // Doppelklick in der Warteschlange: Einstellungen des Auftrags übernehmen (wie DaVinci „Job bearbeiten“)
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

    reloadPresets();
    selectPreset(0); // YouTube 1080p
    updateControls();
}

// ---------- Vorlagen

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
    // Combo-Index -> Listenindex (Trennlinie beachten)
    const QVariant v = m_preset->itemData(comboIndex);
    if (!v.isValid()) return; // „Eigene Einstellungen“: nichts ändern
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
        if (p.name == name) return false; // eingebaute nicht überschreiben
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
    // Aktualisieren: eigene Vorlage mit geänderten Einstellungen ist gerade „Eigene Einstellungen“ -> letzte merken
    update->setEnabled(user);
    del->setEnabled(user);
    menu.exec(m_presetMenu->mapToGlobal(QPoint(0, m_presetMenu->height())));
}

void DeliverPanel::syncPresetToSettings()
{
    if (m_applying) return;
    const RenderSettings cur = settings();
    const QSize tl = m_project->format().size();
    // Aktuelle Vorlage passt noch -> bleibt; sonst die erste passende, sonst „Eigene Einstellungen“
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

// ---------- Einstellungen

RenderSettings DeliverPanel::settings() const
{
    RenderSettings s;
    s.format = m_format->currentData().toString();
    s.quality = m_quality->currentIndex();
    s.audioBitrateK = m_audioBitrate->currentData().toInt();
    s.subtitles = m_subtitles->currentData().toInt();
    if (s.audioOnly() && s.subtitles == RenderSettings::BurnSubtitles) s.subtitles = RenderSettings::NoSubtitles;
    const QSize size = m_resolution->currentData().toSize();
    const QSize tl = m_project->format().size();
    if (size.isValid() && size != tl) {
        const int shortSide = std::min(size.width(), size.height());
        if (sizeForShortSide(tl, shortSide) == size) s.shortSide = shortSide; // Seitenverhältnis der Timeline
        else s.size = size;
    }
    return s;
}

void DeliverPanel::setSettings(const RenderSettings& s)
{
    const bool wasApplying = m_applying;
    m_applying = true;
    m_format->setCurrentIndex(std::max(0, m_format->findData(renderFormat(s.format).id)));
    m_quality->setCurrentIndex(std::clamp(s.quality, 0, 2));
    const int br = m_audioBitrate->findData(s.audioBitrateK);
    m_audioBitrate->setCurrentIndex(br >= 0 ? br : m_audioBitrate->count() - 1);
    m_subtitles->setCurrentIndex(std::max(0, m_subtitles->findData(s.subtitles)));
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
    m_quality->setEnabled(f.hasQuality);
    m_quality->setToolTip(f.hasQuality ? QString() : T("Bei diesem Format fest"));
    m_audioBitrate->setEnabled(QLatin1String(f.audioCodec) == QLatin1String("aac"));
    // Nur Audio: nichts einzubrennen (Eintrag ausgegraut, SRT daneben geht weiter)
    if (auto* model = qobject_cast<QStandardItemModel*>(m_subtitles->model()))
        if (QStandardItem* item = model->item(1)) item->setEnabled(video);
    if (!video && m_subtitles->currentIndex() == 1) m_subtitles->setCurrentIndex(0);
}

void DeliverPanel::updateRange()
{
    // Bereich nur wählbar, wenn In oder Out gesetzt ist; dann automatisch vorgewählt
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
    // Wie DaVinci: Standard ist die Timeline-Auflösung; dazu gängige Größen im selben Seitenverhältnis
    // (Hochformat-Projekt -> 720 × 1280, 1080 × 1920 …) und das jeweils andere Format (16:9 bzw. 9:16)
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
    // War die Timeline-Auflösung gewählt, bleibt es die (neue) Timeline-Auflösung
    const int keep = previous.isValid() && previous != m_timelineSize ? m_resolution->findData(previous) : -1;
    m_timelineSize = timeline;
    m_resolution->setCurrentIndex(keep >= 0 ? keep : m_resolution->findData(timeline));
    m_applying = applying;
    m_rate->setText(QString("%1 fps").arg(f.rate.label()));
}

void DeliverPanel::browse()
{
    const QString dir = QFileDialog::getExistingDirectory(this, T("Speicherort wählen"), m_folder->text());
    if (!dir.isEmpty()) m_folder->setText(QDir::toNativeSeparators(dir));
}

// ---------- Warteschlange

RenderJob DeliverPanel::makeJob() const
{
    RenderJob job;
    job.settings = settings();
    const RenderFormatInfo& f = renderFormat(job.settings.format);
    QString name = m_name->text().trimmed();
    if (name.isEmpty()) name = "Export";
    const QString ext = QString(".") + f.extension;
    // Andere Endung (z. B. .mp4 nach Wechsel auf WAV) ersetzen
    for (const RenderFormatInfo& other : renderFormats())
        if (name.endsWith(QString(".") + other.extension, Qt::CaseInsensitive)) {
            name.chop(int(qstrlen(other.extension)) + 1);
            break;
        }
    job.path = QDir(QDir::fromNativeSeparators(m_folder->text().trimmed())).absoluteFilePath(name + ext);
    const int idx = currentPreset();
    if (idx >= 0 && idx < m_presets.size()) job.preset = m_presets[idx].name;
    if (!job.settings.audioOnly()) job.size = m_resolution->currentData().toSize();
    job.sequence = m_project->currentSequence(); // gerendert wird diese Timeline, auch wenn später eine andere offen ist
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
    QVector<RenderJob> q = m_project->renderQueue();
    const bool inQueue = std::any_of(q.cbegin(), q.cend(), [&](const RenderJob& j) {
        return j.path == job.path && j.status != RenderStatus::Done;
    });
    if (confirm && (inQueue || QFileInfo::exists(job.path))) {
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
