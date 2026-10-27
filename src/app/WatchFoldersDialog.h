#pragma once
// Watch folders of the project (core/WatchFolders.h, app/FolderWatch.h): one folder (WatchFolderDialog: folder,
// subfolders, media kinds, bin, files already there) and the list of all of them (WatchFoldersDialog: add, edit,
// remove). Both only edit a list; WatchFoldersDialog::apply() puts it into the project.

#include "core/WatchFolders.h"

#include <QDialog>

class Project;
class QCheckBox;
class QComboBox;
class QLineEdit;
class QPushButton;
class QTreeWidget;

class WatchFolderDialog : public QDialog {
    Q_OBJECT
public:
    // Bin -1 = a new bin named like the folder (made by WatchFoldersDialog::apply)
    static constexpr int kNewBin = -1;
    // adding = a folder not watched yet: offers to take the files already there too
    WatchFolderDialog(const Project& project, const WatchFolder& folder, bool adding, QWidget* parent = nullptr);
    WatchFolder folder() const;

private:
    void fillBins();
    void updateOk();

    const Project& m_project;
    WatchFolder m_folder;
    QLineEdit* m_path;
    QCheckBox* m_subfolders;
    QCheckBox* m_video;
    QCheckBox* m_audio;
    QCheckBox* m_images;
    QComboBox* m_bin;
    int m_chosenBin = 0; // the user's choice (kept when the list is filled anew for another folder name)
    QCheckBox* m_existing = nullptr;
    QPushButton* m_ok;
};

class WatchFoldersDialog : public QDialog {
    Q_OBJECT
public:
    WatchFoldersDialog(const Project& project, QWidget* parent = nullptr);
    QVector<WatchFolder> folders() const { return m_list; }
    // Into the project: new bins for kNewBin, then the list (not in the undo history; the bins are)
    static void apply(Project& project, QVector<WatchFolder> list);
    // Adds or edits the watch of `folder` (WatchFolderDialog), `bin` preset for a new one (kNewBin = like the folder)
    static bool addFolder(Project& project, const QString& folder, int bin, QWidget* parent);

private:
    void rebuild();
    void edit(int row, const QString& newFolder = {});

    const Project& m_project;
    QVector<WatchFolder> m_list;
    QTreeWidget* m_table;
    QPushButton* m_editBtn;
    QPushButton* m_removeBtn;
};
