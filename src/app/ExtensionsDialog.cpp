#include "app/ExtensionsDialog.h"

#include "core/I18n.h"

#include <QDesktopServices>
#include <QDir>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

namespace {
QString megabytes(qint64 bytes) { return T("%1 MB").arg(bytes / 1e6, 0, 'f', bytes < 10e6 ? 1 : 0); }
} // namespace

ExtensionsDialog::ExtensionsDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle(T("Erweiterungen"));
    auto* lay = new QVBoxLayout(this);
    auto* intro = new QLabel(T("Zusätze, die schneidi nur bei Bedarf herunterlädt. Sie laufen ganz auf diesem Rechner "
                               "(keine Daten gehen ins Internet) und lassen sich jederzeit wieder entfernen."));
    intro->setWordWrap(true);
    lay->addWidget(intro);

    auto* grid = new QGridLayout;
    grid->setColumnStretch(0, 1);
    grid->setHorizontalSpacing(16);
    int r = 0;
    for (const Extensions::Item& item : Extensions::catalog()) {
        if (item.kind == Extensions::Kind::WhisperVad) continue; // comes with whisper
        auto* name = new QLabel(QString("<b>%1</b><br><small>%2</small>").arg(T(item.name).toHtmlEscaped(),
                                                                             T(item.description).toHtmlEscaped()));
        name->setWordWrap(true);
        auto* status = new QLabel;
        auto* button = new QPushButton;
        grid->addWidget(name, r, 0);
        grid->addWidget(new QLabel(megabytes(item.size)), r, 1, Qt::AlignRight);
        grid->addWidget(status, r, 2);
        grid->addWidget(button, r, 3);
        m_rows << Row{&item, status, button};
        connect(button, &QPushButton::clicked, this, [this, &item] {
            if (Extensions::isInstalled(item)) {
                for (const Extensions::Item* i : Extensions::withDependencies({&item}))
                    if (i->kind == item.kind || i->kind == Extensions::Kind::WhisperVad) Extensions::remove(*i);
                refresh();
            } else {
                install(item);
            }
        });
        ++r;
    }
    lay->addLayout(grid);
    if (!Extensions::find("whisper")) {
        // No ready-made whisper for this system (e.g. Linux on ARM): an own whisper-cli on PATH works too
        m_programNote = new QLabel(T("Für dieses System gibt es kein fertiges Whisper-Programm. Ein selbst gebautes "
                                     "<i>whisper-cli</i> im Suchpfad (PATH) wird verwendet."));
        m_programNote->setWordWrap(true);
        lay->addWidget(m_programNote);
    }

    m_progressLabel = new QLabel;
    m_progress = new QProgressBar;
    m_cancel = new QPushButton(T("Abbrechen"));
    auto* progressRow = new QHBoxLayout;
    progressRow->addWidget(m_progress, 1);
    progressRow->addWidget(m_cancel);
    lay->addWidget(m_progressLabel);
    lay->addLayout(progressRow);
    connect(m_cancel, &QPushButton::clicked, this, [this] {
        m_queue.clear();
        m_installer.cancel();
    });

    lay->addStretch(1); // extra height goes below the list, not between the rows
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    auto* folder = buttons->addButton(T("Ordner öffnen"), QDialogButtonBox::ActionRole);
    connect(folder, &QPushButton::clicked, this, [] {
        QDir().mkpath(Extensions::rootDir());
        QDesktopServices::openUrl(QUrl::fromLocalFile(Extensions::rootDir()));
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &ExtensionsDialog::reject);
    lay->addWidget(buttons);

    connect(&m_installer, &Extensions::Installer::progress, this, [this](qint64 received, qint64 total) {
        m_progress->setRange(0, 1000);
        m_progress->setValue(total > 0 ? int(1000 * received / total) : 0);
        m_progressLabel->setText(T("Lade %1 … %2 von %3").arg(T(m_current->name), megabytes(received), megabytes(total)));
    });
    connect(&m_installer, &Extensions::Installer::finished, this, [this](bool ok, const QString& message) {
        const Extensions::Item* item = m_current;
        m_current = nullptr;
        if (!ok) {
            m_queue.clear();
            refresh();
            if (message != T("Abgebrochen"))
                QMessageBox::warning(this, T("Erweiterungen"), T("%1 konnte nicht installiert werden:\n%2").arg(T(item->name), message));
            return;
        }
        startNext();
    });
    setMinimumWidth(640);
    refresh();
}

void ExtensionsDialog::refresh()
{
    const bool busy = m_current != nullptr;
    for (const Row& row : m_rows) {
        const bool installed = Extensions::isInstalled(*row.item);
        row.status->setText(installed ? T("installiert") : QString());
        row.button->setText(installed ? T("Entfernen") : T("Installieren"));
        row.button->setEnabled(!busy);
    }
    m_progress->setVisible(busy);
    m_progressLabel->setVisible(busy);
    m_cancel->setVisible(busy);
}

void ExtensionsDialog::install(const Extensions::Item& item)
{
    m_queue.clear();
    for (const Extensions::Item* i : Extensions::withDependencies({&item}))
        if (!Extensions::isInstalled(*i)) m_queue << i;
    startNext();
}

void ExtensionsDialog::startNext()
{
    if (m_queue.isEmpty()) {
        refresh();
        return;
    }
    m_current = m_queue.takeFirst();
    m_progress->setRange(0, 0);
    m_progressLabel->setText(T("Lade %1 …").arg(T(m_current->name)));
    refresh();
    QString error;
    if (!m_installer.start(*m_current, &error)) {
        const Extensions::Item* item = m_current;
        m_current = nullptr;
        m_queue.clear();
        refresh();
        QMessageBox::warning(this, T("Erweiterungen"), T("%1 konnte nicht installiert werden:\n%2").arg(T(item->name), error));
    }
}

void ExtensionsDialog::reject()
{
    m_queue.clear();
    if (m_installer.isRunning()) m_installer.cancel();
    QDialog::reject();
}
