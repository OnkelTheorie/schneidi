#include "app/MulticamDialog.h"

#include "core/I18n.h"
#include "core/Project.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QSettings>
#include <QVBoxLayout>
#include <algorithm>

using Method = Multicam::SyncMethod;

MulticamDialog::MulticamDialog(const Project* project, const QStringList& files, const QString& defaultName,
                               QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(T("Neuer Multicam-Clip"));
    QSettings settings;
    auto* form = new QFormLayout;

    m_name = new QLineEdit(defaultName);
    m_name->setObjectName("name");
    m_name->selectAll();
    form->addRow(T("Name"), m_name);

    auto* angles = new QListWidget;
    angles->setObjectName("angles");
    angles->setSelectionMode(QAbstractItemView::NoSelection);
    angles->setFocusPolicy(Qt::NoFocus);
    for (int i = 0; i < files.size(); ++i)
        angles->addItem(QString("%1  %2").arg(i + 1).arg(Multicam::defaultAngleName(files[i])));
    angles->setMaximumHeight(std::min(8, int(files.size()) + 1) * angles->fontMetrics().height() + 12);
    form->addRow(T("Winkel"), angles);

    m_method = new QComboBox;
    m_method->setObjectName("syncMethod");
    m_method->addItem(T("Ton"), int(Method::Sound));
    m_method->addItem(T("Erster Quell-Marker"), int(Method::Marker));
    m_method->addItem(T("In-Punkt"), int(Method::InPoint));
    m_method->addItem(T("Anfang"), int(Method::Start));
    m_method->setToolTip(T("Ton: gemeinsame Geräusche (Rundenstart, Ansagen) finden. Marker: die ersten Quell-Marker "
                           "(M im Quell-Viewer) liegen übereinander. In-Punkt: die Quell-In-Punkte liegen übereinander."));
    m_method->setCurrentIndex(std::max(0, m_method->findData(settings.value("multicam/syncMethod", 0).toInt())));
    form->addRow(T("Winkel synchronisieren"), m_method);

    // Audio streams: as many as the file with the most has, titles from the first file that has the stream
    m_stream = new QComboBox;
    m_stream->setObjectName("syncStream");
    int streams = 1;
    for (const QString& f : files)
        if (const MediaInfo* m = project->mediaInfo(f)) streams = std::max(streams, m->audioStreamCount());
    for (int k = 0; k < streams; ++k) {
        QString title = T("Spur %1").arg(k + 1);
        for (const QString& f : files)
            if (const MediaInfo* m = project->mediaInfo(f); m && k < m->audioStreamCount()) {
                if (m->audioStreamCount() > 1) title = QString("%1 – %2").arg(k + 1).arg(m->audioStreamName(k));
                break;
            }
        m_stream->addItem(title);
    }
    m_stream->setToolTip(T("Ton-Stream, nach dem synchronisiert wird (OBS: meist der Mix mit dem Spielton). "
                           "Dateien mit weniger Streams nehmen den ersten."));
    m_stream->setCurrentIndex(std::clamp(settings.value("multicam/syncStream", 0).toInt(), 0, streams - 1));
    form->addRow(T("Ton-Stream"), m_stream);
    auto updateStream = [this] { m_stream->setEnabled(method() == Method::Sound); };
    connect(m_method, &QComboBox::currentIndexChanged, this, updateStream);
    updateStream();

    auto* hint = new QLabel(T("Was sich nicht synchronisieren lässt, kommt an den Anfang und wird gemeldet. "
                              "Winkel lassen sich nachträglich in der Multicam-Timeline verschieben."));
    hint->setWordWrap(true);
    hint->setObjectName("hint");

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* lay = new QVBoxLayout(this);
    lay->addLayout(form);
    lay->addWidget(hint);
    lay->addWidget(buttons);
    resize(460, sizeHint().height());
}

QString MulticamDialog::name() const
{
    return m_name->text().trimmed();
}

Multicam::SyncMethod MulticamDialog::method() const
{
    return Method(m_method->currentData().toInt());
}

int MulticamDialog::stream() const
{
    return std::max(0, m_stream->currentIndex());
}

void MulticamDialog::accept()
{
    QSettings settings;
    settings.setValue("multicam/syncMethod", int(method()));
    settings.setValue("multicam/syncStream", stream());
    QDialog::accept();
}
