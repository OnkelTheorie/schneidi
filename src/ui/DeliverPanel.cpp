#include "ui/DeliverPanel.h"

#include "core/I18n.h"
#include "core/Project.h"
#include "engine/Exporter.h"
#include <algorithm>
#include <cmath>
#include <QStandardItemModel>
#include "core/Timecode.h"

#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardPaths>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

struct Preset {
    const char* name;
    int codec;     // Index in m_codec
    int shortSide; // kürzere Bildkante (1080 = 1920 × 1080 bzw. Hochformat 1080 × 1920); 0 = Timeline-Auflösung
    int quality;   // Index in m_quality
};
const Preset kPresets[] = {
    {N_("Eigene Einstellungen"), 0, 0, 1},
    {"YouTube 1080p", 0, 1080, 0},
    {"YouTube 4K", 0, 2160, 0},
    {N_("Klein (zum Verschicken)"), 0, 720, 2},
    {"H.265 Master 1080p", 1, 1080, 0},
};

// Ausgabegröße mit dem Seitenverhältnis der Timeline und gegebener kürzerer Kante (gerade Maße)
QSize sizeForShortSide(QSize timeline, int shortSide)
{
    const int w = timeline.width(), h = timeline.height();
    if (w >= h) return {int(std::lround(double(shortSide) * w / h)) & ~1, shortSide};
    return {shortSide, int(std::lround(double(shortSide) * h / w)) & ~1};
}

} // namespace

DeliverPanel::DeliverPanel(Project* project, QWidget* parent)
    : QWidget(parent), m_project(project), m_exporter(new Exporter(this))
{
    setObjectName("Panel");
    setMinimumWidth(300);

    auto* title = new QLabel(T("Render-Einstellungen"));
    title->setObjectName("PanelTitle");

    m_preset = new QComboBox;
    for (const auto& p : kPresets) m_preset->addItem(T(p.name));

    m_name = new QLineEdit("Timeline 1");
    m_folder = new QLineEdit(QStandardPaths::writableLocation(QStandardPaths::MoviesLocation));
    auto* browseBtn = new QToolButton;
    browseBtn->setText(T("Durchsuchen…"));
    connect(browseBtn, &QToolButton::clicked, this, &DeliverPanel::browse);
    auto* folderRow = new QHBoxLayout;
    folderRow->addWidget(m_folder, 1);
    folderRow->addWidget(browseBtn);

    m_codec = new QComboBox;
    m_codec->addItem("H.264 (MP4)", "libx264");
    m_codec->addItem("H.265 (MP4)", "libx265");

    m_resolution = new QComboBox;
    m_rate = new QLabel;

    m_quality = new QComboBox;
    m_quality->addItem(T("Hoch"), 18);
    m_quality->addItem(T("Mittel"), 22);
    m_quality->addItem(T("Klein"), 28);

    // Wie DaVinci: "Render: Entire Timeline / In/Out Range"
    m_range = new QComboBox;
    m_range->addItem(T("Ganze Timeline"));
    m_range->addItem(T("In/Out-Bereich"));
    connect(project, &Project::timelineChanged, this, &DeliverPanel::updateRange);
    updateRange();
    connect(project, &Project::formatChanged, this, &DeliverPanel::updateFormat);
    updateFormat();

    auto* form = new QFormLayout;
    form->setContentsMargins(12, 12, 12, 12);
    form->setVerticalSpacing(8);
    form->addRow(T("Vorlage"), m_preset);
    form->addRow(T("Dateiname"), m_name);
    form->addRow(T("Ort"), folderRow);
    form->addRow(T("Codec"), m_codec);
    form->addRow(T("Auflösung"), m_resolution);
    form->addRow(T("Bildrate"), m_rate);
    form->addRow(T("Qualität"), m_quality);
    form->addRow(T("Bereich"), m_range);

    m_renderBtn = new QPushButton(T("Rendern"));
    m_renderBtn->setMinimumHeight(30);
    m_renderBtn->setStyleSheet("QPushButton { background: #e87a3a; color: black; font-weight: 600; border-radius: 3px; }"
                               "QPushButton:disabled { background: #5a4030; }");
    m_progress = new QProgressBar;
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    m_progress->setTextVisible(true);
    m_status = new QLabel;
    m_status->setWordWrap(true);
    m_status->setStyleSheet("color: #8c8c94; padding: 0 12px;");

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 12);
    lay->setSpacing(6);
    lay->addWidget(title);
    lay->addLayout(form);
    lay->addStretch(1);
    auto* bottom = new QVBoxLayout;
    bottom->setContentsMargins(12, 0, 12, 0);
    bottom->addWidget(m_progress);
    bottom->addWidget(m_renderBtn);
    lay->addLayout(bottom);
    lay->addWidget(m_status);

    connect(m_preset, qOverload<int>(&QComboBox::activated), this, &DeliverPanel::applyPreset);
    connect(m_renderBtn, &QPushButton::clicked, this, &DeliverPanel::startRender);
    connect(m_exporter, &Exporter::progress, m_progress, &QProgressBar::setValue);
    connect(m_exporter, &Exporter::finished, this, [this](bool ok, const QString& msg) {
        m_renderBtn->setText(T("Rendern"));
        m_status->setText(msg);
        if (!ok) m_progress->setValue(0);
    });
    applyPreset(1);
    m_preset->setCurrentIndex(1);
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
    // (Hochformat-Projekt -> 720 × 1280, 1080 × 1920 …)
    const ProjectFormat& f = m_project->format();
    const QSize timeline = f.size();
    const QSize previous = m_resolution->currentData().toSize();
    QVector<QSize> sizes{timeline};
    for (int side : {720, 1080, 1440, 2160}) {
        const QSize s = sizeForShortSide(timeline, side);
        if (!sizes.contains(s)) sizes << s;
    }
    std::sort(sizes.begin(), sizes.end(), [](QSize a, QSize b) { return a.width() * a.height() < b.width() * b.height(); });
    m_resolution->clear();
    for (const QSize& s : sizes) {
        QString label = resolutionLabel(s.width(), s.height());
        if (s == timeline) label += " " + T("(Timeline)");
        m_resolution->addItem(label, s);
    }
    // War die Timeline-Auflösung gewählt, bleibt es die (neue) Timeline-Auflösung
    const int keep = previous.isValid() && previous != m_timelineSize ? m_resolution->findData(previous) : -1;
    m_timelineSize = timeline;
    m_resolution->setCurrentIndex(keep >= 0 ? keep : m_resolution->findData(timeline));
    m_rate->setText(QString("%1 fps").arg(f.rate.label()));
}

void DeliverPanel::applyPreset(int index)
{
    const Preset& p = kPresets[index];
    m_codec->setCurrentIndex(p.codec);
    const QSize timeline = m_project->format().size();
    const int i = m_resolution->findData(p.shortSide ? sizeForShortSide(timeline, p.shortSide) : timeline);
    m_resolution->setCurrentIndex(i >= 0 ? i : m_resolution->findData(timeline));
    m_quality->setCurrentIndex(p.quality);
}

void DeliverPanel::browse()
{
    const QString dir = QFileDialog::getExistingDirectory(this, T("Speicherort wählen"), m_folder->text());
    if (!dir.isEmpty()) m_folder->setText(dir);
}

void DeliverPanel::startRender()
{
    if (m_exporter->isRunning()) {
        m_exporter->cancel();
        return;
    }
    QString name = m_name->text().trimmed();
    if (name.isEmpty()) name = "Export";
    if (!name.endsWith(".mp4", Qt::CaseInsensitive)) name += ".mp4";
    const QString path = QDir(m_folder->text()).filePath(name);

    if (QFileInfo::exists(path) &&
        QMessageBox::question(this, T("Rendern"), T("%1 existiert schon. Überschreiben?").arg(name)) != QMessageBox::Yes)
        return;

    ExportSettings s;
    s.path = path;
    s.videoCodec = m_codec->currentData().toString();
    s.format = m_project->format();
    s.size = m_resolution->currentData().toSize();
    s.crf = m_quality->currentData().toInt();
    if (m_range->currentIndex() == 1) {
        const Timeline& tl = m_project->timeline();
        s.from = std::max(0, tl.markIn);
        s.to = tl.markOut;
    }
    QString error;
    if (!m_exporter->start(m_project->timeline(), s, &error)) {
        m_status->setText(error);
        return;
    }
    m_renderBtn->setText(T("Abbrechen"));
    m_status->setText(T("Rendere nach %1 …").arg(path));
}
