// SPDX-License-Identifier: AGPL-3.0-only
#include "app/WatchFoldersDialog.h"

#include "app/Theme.h"
#include "core/I18n.h"
#include "core/Project.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace {

// "Parent/Child" of a bin, "Master" for 0
QString binLabel(const Project& p, int id)
{
    QStringList parts;
    for (int guard = 0; id != 0 && guard <= p.bins().size(); ++guard) {
        const MediaBin* b = p.bin(id);
        if (!b) break;
        parts.prepend(b->name);
        id = b->parent;
    }
    return parts.isEmpty() ? p.binName(0) : parts.join(" / ");
}

QString folderName(const QString& path)
{
    const QString name = QDir(path).dirName();
    return name.isEmpty() ? QDir::toNativeSeparators(path) : name;
}

QString kindsText(const WatchFolder& w)
{
    QStringList kinds;
    if (w.video) kinds << T("Video");
    if (w.audio) kinds << T("Audio");
    if (w.images) kinds << T("Bilder");
    return kinds.join(", ");
}

QString binText(const Project& p, const WatchFolder& w)
{
    if (w.bin == WatchFolderDialog::kNewBin) return T("Neuer Bin „%1“").arg(folderName(w.path));
    return binLabel(p, p.bin(w.bin) ? w.bin : 0);
}

} // namespace

// ---------- one folder ----------

WatchFolderDialog::WatchFolderDialog(const Project& project, const WatchFolder& folder, bool adding, QWidget* parent)
    : QDialog(parent), m_project(project), m_folder(folder)
{
    setWindowTitle(adding ? T("Ordner überwachen") : T("Überwachten Ordner bearbeiten"));
    auto* layout = new QVBoxLayout(this);
    auto* intro = new QLabel(T("Neue Mediendateien in diesem Ordner kommen von selbst in den Media Pool, sobald sie fertig "
                               "geschrieben sind – z. B. Aufnahmen eines Bildschirmrekorders oder Clips eines "
                               "Spiele-Tools. Überwacht wird, solange das Projekt offen ist; was in der Zwischenzeit "
                               "dazukommt, wird beim Öffnen nachgeholt."));
    intro->setWordWrap(true);
    intro->setObjectName("watchIntro");
    intro->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));
    layout->addWidget(intro);

    auto* form = new QFormLayout;
    m_path = new QLineEdit(QDir::toNativeSeparators(folder.path));
    m_path->setObjectName("watchPath");
    auto* browse = new QPushButton(T("Durchsuchen…"));
    auto* pathRow = new QHBoxLayout;
    pathRow->addWidget(m_path, 1);
    pathRow->addWidget(browse);
    form->addRow(T("Ordner:"), pathRow);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(this, T("Ordner wählen"), QDir::fromNativeSeparators(m_path->text()));
        if (!dir.isEmpty()) m_path->setText(QDir::toNativeSeparators(dir));
    });

    m_subfolders = new QCheckBox(T("Auch Dateien in Unterordnern"));
    m_subfolders->setObjectName("watchSubfolders");
    m_subfolders->setChecked(folder.subfolders);
    form->addRow(T("Unterordner:"), m_subfolders);

    m_video = new QCheckBox(T("Video"));
    m_audio = new QCheckBox(T("Audio"));
    m_images = new QCheckBox(T("Bilder"));
    m_video->setChecked(folder.video);
    m_audio->setChecked(folder.audio);
    m_images->setChecked(folder.images);
    auto* kinds = new QHBoxLayout;
    for (QCheckBox* c : {m_video, m_audio, m_images}) {
        kinds->addWidget(c);
        connect(c, &QCheckBox::toggled, this, &WatchFolderDialog::updateOk);
    }
    kinds->addStretch();
    form->addRow(T("Medien:"), kinds);

    m_bin = new QComboBox;
    m_bin->setObjectName("watchBin");
    m_bin->setToolTip(T("Bin im Media Pool, in den die neuen Dateien kommen"));
    form->addRow(T("Ziel-Bin:"), m_bin);
    m_chosenBin = folder.bin;
    connect(m_bin, &QComboBox::activated, this, [this] { m_chosenBin = m_bin->currentData().toInt(); });
    fillBins();
    layout->addLayout(form);

    if (adding) {
        m_existing = new QCheckBox(T("Auch die Dateien importieren, die schon im Ordner sind"));
        m_existing->setObjectName("watchExisting");
        m_existing->setChecked(!folder.since.isValid() && !folder.path.isEmpty());
        layout->addWidget(m_existing);
    }

    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    m_ok = box->button(QDialogButtonBox::Ok);
    if (adding) m_ok->setText(T("Überwachen"));
    connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(box);
    connect(m_path, &QLineEdit::textChanged, this, [this] {
        fillBins(); // "New bin" carries the folder's name
        updateOk();
    });
    updateOk();
    // The height for this width: sizeHint() wraps the intro for the narrower hint width (English: one line more than it
    // takes at 560 px, shown as an empty gap above the text)
    const int width = std::max(560, sizeHint().width());
    resize(width, hasHeightForWidth() ? heightForWidth(width) : sizeHint().height());
}

void WatchFolderDialog::fillBins()
{
    const QString folder = QDir::fromNativeSeparators(m_path->text().trimmed());
    m_bin->clear();
    if (!folder.isEmpty()) m_bin->addItem(T("Neuer Bin „%1“").arg(folderName(folder)), kNewBin);
    m_bin->addItem(binLabel(m_project, 0), 0);
    QVector<std::pair<QString, int>> bins;
    for (const MediaBin& b : m_project.bins()) bins << std::pair{binLabel(m_project, b.id), b.id};
    std::sort(bins.begin(), bins.end(), [](const auto& a, const auto& b) { return I18n::compareNames(a.first, b.first) < 0; });
    for (const auto& [label, id] : bins) m_bin->addItem(label, id);
    const int index = m_bin->findData(m_chosenBin);
    m_bin->setCurrentIndex(index >= 0 ? index : m_bin->findData(0));
}

void WatchFolderDialog::updateOk()
{
    const QString folder = QDir::fromNativeSeparators(m_path->text().trimmed());
    m_ok->setEnabled(!folder.isEmpty() && QFileInfo(folder).isDir()
                     && (m_video->isChecked() || m_audio->isChecked() || m_images->isChecked()));
}

WatchFolder WatchFolderDialog::folder() const
{
    WatchFolder w = m_folder;
    const QString path = QDir::cleanPath(QFileInfo(QDir::fromNativeSeparators(m_path->text().trimmed())).absoluteFilePath());
    if (!WatchFolders::samePath(path, m_folder.path)) { // another folder: watched from now on
        w.since = QDateTime::currentDateTimeUtc();
        w.ignored.clear();
    }
    w.path = path;
    w.subfolders = m_subfolders->isChecked();
    w.video = m_video->isChecked();
    w.audio = m_audio->isChecked();
    w.images = m_images->isChecked();
    w.bin = m_bin->currentData().toInt();
    if (m_existing) w.since = m_existing->isChecked() ? QDateTime() : QDateTime::currentDateTimeUtc();
    return w;
}

// ---------- the list ----------

WatchFoldersDialog::WatchFoldersDialog(const Project& project, QWidget* parent)
    : QDialog(parent), m_project(project), m_list(project.watchFolders())
{
    setWindowTitle(T("Überwachte Ordner"));
    auto* layout = new QVBoxLayout(this);
    auto* intro = new QLabel(T("Neue Mediendateien dieser Ordner kommen von selbst in den Media Pool (ein Rückgängig-"
                               "Schritt je Schwung). Ohne Eintrag wird nichts überwacht."));
    intro->setWordWrap(true);
    intro->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));
    layout->addWidget(intro);

    m_table = new QTreeWidget;
    m_table->setObjectName("watchList");
    m_table->setRootIsDecorated(false);
    m_table->setTextElideMode(Qt::ElideMiddle); // a long path keeps its end: the folder's own name
    m_table->setHeaderLabels({T("Ordner"), T("Ziel-Bin"), T("Medien"), T("Unterordner")});
    m_table->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->header()->setStretchLastSection(false);
    layout->addWidget(m_table, 1);

    auto* buttons = new QHBoxLayout;
    auto* add = new QPushButton(T("Hinzufügen…"));
    add->setObjectName("watchAdd");
    m_editBtn = new QPushButton(T("Bearbeiten…"));
    m_removeBtn = new QPushButton(T("Entfernen"));
    m_removeBtn->setObjectName("watchRemove");
    buttons->addWidget(add);
    buttons->addWidget(m_editBtn);
    buttons->addWidget(m_removeBtn);
    buttons->addStretch();
    layout->addLayout(buttons);
    connect(add, &QPushButton::clicked, this, [this] { edit(-1); });
    connect(m_editBtn, &QPushButton::clicked, this, [this] { edit(m_table->indexOfTopLevelItem(m_table->currentItem())); });
    connect(m_table, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem* it) { edit(m_table->indexOfTopLevelItem(it)); });
    connect(m_removeBtn, &QPushButton::clicked, this, [this] {
        const int row = m_table->indexOfTopLevelItem(m_table->currentItem());
        if (row < 0 || row >= m_list.size()) return;
        m_list.removeAt(row);
        rebuild();
    });
    connect(m_table, &QTreeWidget::currentItemChanged, this, [this] {
        m_editBtn->setEnabled(m_table->currentItem());
        m_removeBtn->setEnabled(m_table->currentItem());
    });

    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(box);
    rebuild();
    resize(720, 340);
}

void WatchFoldersDialog::rebuild()
{
    const int row = std::max(0, m_table->indexOfTopLevelItem(m_table->currentItem()));
    m_table->clear();
    for (const WatchFolder& w : m_list) {
        auto* it = new QTreeWidgetItem({QDir::toNativeSeparators(w.path), binText(m_project, w), kindsText(w),
                                        w.subfolders ? T("ja") : T("nein")});
        if (!QFileInfo(w.path).isDir()) { // unplugged drive, network share away: watched again when it is back
            it->setForeground(0, Theme::textDim);
            it->setToolTip(0, T("Ordner gerade nicht erreichbar"));
        } else {
            it->setToolTip(0, QDir::toNativeSeparators(w.path));
        }
        m_table->addTopLevelItem(it);
    }
    for (int c = 1; c < m_table->columnCount(); ++c) m_table->resizeColumnToContents(c);
    if (m_table->topLevelItemCount() > 0)
        m_table->setCurrentItem(m_table->topLevelItem(std::min(row, m_table->topLevelItemCount() - 1)));
    m_editBtn->setEnabled(m_table->currentItem());
    m_removeBtn->setEnabled(m_table->currentItem());
}

void WatchFoldersDialog::edit(int row, const QString& newFolder)
{
    const bool adding = row < 0 || row >= m_list.size();
    WatchFolder w;
    if (adding) {
        w.path = newFolder;
        w.bin = WatchFolderDialog::kNewBin;
        w.since = QDateTime::currentDateTimeUtc();
    } else {
        w = m_list[row];
    }
    WatchFolderDialog dlg(m_project, w, adding, this);
    if (dlg.exec() != QDialog::Accepted) return;
    const WatchFolder n = dlg.folder();
    if (!adding) m_list.removeAt(row);
    const int same = WatchFolders::indexOf(m_list, n.path); // the folder is watched already: that entry changes
    if (same >= 0) m_list[same] = n;
    else m_list.insert(adding ? m_list.size() : row, n);
    rebuild();
}

void WatchFoldersDialog::apply(Project& project, QVector<WatchFolder> list)
{
    for (WatchFolder& w : list)
        if (w.bin == WatchFolderDialog::kNewBin) w.bin = project.addBin(0, folderName(w.path));
    project.setWatchFolders(list);
}

bool WatchFoldersDialog::addFolder(Project& project, const QString& folder, int bin, QWidget* parent)
{
    QVector<WatchFolder> list = project.watchFolders();
    const int index = WatchFolders::indexOf(list, folder);
    WatchFolder w;
    if (index >= 0) {
        w = list[index];
    } else {
        w.path = folder;
        w.bin = bin;
        w.since = QDateTime::currentDateTimeUtc();
    }
    WatchFolderDialog dlg(project, w, index < 0, parent);
    if (dlg.exec() != QDialog::Accepted) return false;
    const WatchFolder n = dlg.folder();
    if (index >= 0) list.removeAt(index);
    const int same = WatchFolders::indexOf(list, n.path);
    if (same >= 0) list[same] = n;
    else list.insert(index >= 0 ? index : list.size(), n);
    apply(project, list);
    return true;
}
