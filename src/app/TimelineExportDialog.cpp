#include "app/TimelineExportDialog.h"

#include "core/I18n.h"

#include <QButtonGroup>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QVBoxLayout>

namespace {

struct Target {
    const char* id;
    const char* program;
    const char* how; // file type and where to import it in that program
    Interchange::Format format;
};

// Order = how often wanted. FCP7 XML is what Premiere writes and reads; Resolve reads OTIO since 18.5, older
// versions only the XML. Final Cut Pro X (FCPXML) and Shotcut/Kdenlive (MLT XML) are not written yet.
const Target kTargets[] = {
    {"premiere", N_("Adobe Premiere Pro"), N_("Final Cut Pro XML (.xml) – in Premiere: Datei › Importieren"),
     Interchange::Format::Fcp7},
    {"resolve", N_("DaVinci Resolve 18.5 oder neuer"),
     N_("OpenTimelineIO (.otio) – in Resolve: Datei › Importieren › Timeline"), Interchange::Format::Otio},
    {"resolve-xml", N_("DaVinci Resolve (ältere Versionen)"),
     N_("Final Cut Pro XML (.xml) – in Resolve: Datei › Importieren › Timeline"), Interchange::Format::Fcp7},
    {"edl", N_("EDL – fast jedes Schnittprogramm"), N_("CMX 3600 (.edl), einfachstes Austauschformat"),
     Interchange::Format::Edl},
};

const Target& target(int index)
{
    return kTargets[std::clamp(index, 0, int(std::size(kTargets)) - 1)];
}

} // namespace

TimelineExportDialog::TimelineExportDialog(const QString& name, const QString& dir, QWidget* parent) : QDialog(parent)
{
    setWindowTitle(T("Timeline exportieren"));
    auto* root = new QVBoxLayout(this);
    auto* intro = new QLabel(T("Für welches Programm?"));
    root->addWidget(intro);
    m_group = new QButtonGroup(this);
    for (int i = 0; i < int(std::size(kTargets)); ++i) {
        const Target& t = kTargets[i];
        auto* radio = new QRadioButton(T(t.program));
        radio->setObjectName(QString("target_%1").arg(t.id));
        m_group->addButton(radio, i);
        auto* how = new QLabel(T(t.how));
        how->setForegroundRole(QPalette::PlaceholderText);
        how->setContentsMargins(24, 0, 0, 0); // under the text, next to the indicator
        auto* entry = new QVBoxLayout;
        entry->setSpacing(0);
        entry->addWidget(radio);
        entry->addWidget(how);
        root->addLayout(entry);
    }
    m_carries = new QLabel;
    m_carries->setWordWrap(true);
    m_carries->setContentsMargins(0, 6, 0, 6);
    root->addWidget(m_carries);

    auto* row = new QHBoxLayout;
    row->addWidget(new QLabel(T("Datei:")));
    m_path = new QLineEdit;
    m_path->setObjectName("path");
    m_path->setMinimumWidth(380);
    row->addWidget(m_path, 1);
    auto* browse = new QPushButton(T("Durchsuchen…"));
    row->addWidget(browse);
    root->addLayout(row);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    m_ok = buttons->button(QDialogButtonBox::Ok);
    m_ok->setText(T("Exportieren"));
    buttons->button(QDialogButtonBox::Cancel)->setText(T("Abbrechen"));
    root->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(browse, &QPushButton::clicked, this, &TimelineExportDialog::browse);
    connect(m_path, &QLineEdit::textChanged, this, [this](const QString& t) { m_ok->setEnabled(!t.trimmed().isEmpty()); });

    QSettings settings;
    const QString folder = settings.value("interchange/dir", dir).toString();
    m_path->setText(QDir(folder.isEmpty() ? QDir::homePath() : folder).filePath(name.isEmpty() ? QString("Timeline") : name));
    selectTarget(settings.value("interchange/target", "premiere").toString());
    connect(m_group, &QButtonGroup::idToggled, this, [this](int, bool on) {
        if (on) updateTarget();
    });
}

void TimelineExportDialog::selectTarget(const QString& id)
{
    int index = 0;
    for (int i = 0; i < int(std::size(kTargets)); ++i)
        if (id == QLatin1String(kTargets[i].id)) index = i;
    m_group->button(index)->setChecked(true);
    updateTarget();
}

QString TimelineExportDialog::targetId() const
{
    return QString::fromLatin1(target(m_group->checkedId()).id);
}

Interchange::Format TimelineExportDialog::format() const
{
    return target(m_group->checkedId()).format;
}

void TimelineExportDialog::updateTarget()
{
    m_carries->setText(format() == Interchange::Format::Edl
        ? T("Kommt mit: Schnitte und Blenden der ersten Videospur und der Audiospuren A1–A4. Nicht: weitere "
            "Videospuren, Lautstärke, Marker, Effekte, Farbkorrektur, Titel.")
        : T("Kommt mit: alle Spuren und Schnitte, Ton, Blenden, Geschwindigkeit, Lautstärke, Deckkraft, Marker. "
            "Nicht: Effekte, Farbkorrektur, Titel (das Programm zeigt dafür Lücken oder Hinweise)."));
    // Extension follows the format
    QString p = m_path->text().trimmed();
    if (p.isEmpty()) return;
    if (const auto f = Interchange::formatOf(p)) p.chop(Interchange::extension(*f).size() + 1);
    m_path->setText(p + "." + Interchange::extension(format()));
}

QString TimelineExportDialog::path() const
{
    QString p = m_path->text().trimmed();
    if (Interchange::formatOf(p) != format()) p += "." + Interchange::extension(format());
    return QFileInfo(p).absoluteFilePath();
}

void TimelineExportDialog::browse()
{
    const QString ext = Interchange::extension(format());
    const QString p = QFileDialog::getSaveFileName(this, T("Timeline exportieren"), path(),
                                                   QString("%1 (*.%2)").arg(T(target(m_group->checkedId()).program), ext));
    if (!p.isEmpty()) m_path->setText(p);
    updateTarget(); // extension
}

void TimelineExportDialog::accept()
{
    if (m_path->text().trimmed().isEmpty()) return;
    QSettings settings;
    settings.setValue("interchange/target", targetId());
    settings.setValue("interchange/dir", QFileInfo(path()).absolutePath());
    QDialog::accept();
}
