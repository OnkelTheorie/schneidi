// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Content templates in the app (core/TemplateLibrary.h, core/TemplateFill.h): File › New from Template… (the browser
// with content and start templates), File › Save as Template…, and "Check Fields" of the open project.

#include "core/StartTemplates.h"
#include "core/TemplateLibrary.h"

#include <QDialog>
#include <QVector>

#include <functional>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTreeWidget;

// File › Save as Template…: name, description, tags (comma separated), whether the media are copied into the template
// (on by default: it then works on every machine and in a ZIP) and whether the viewer's picture is its preview
class SaveTemplateDialog : public QDialog {
    Q_OBJECT
public:
    // info: the open project's template description (a template opened for editing), else empty with a suggested name
    SaveTemplateDialog(const TemplateInfo& info, QWidget* parent = nullptr);
    QString name() const;
    QString description() const;
    QStringList tags() const;
    bool copyMedia() const;
    bool preview() const;

private:
    QLineEdit* m_name = nullptr;
    QLineEdit* m_description = nullptr;
    QLineEdit* m_tags = nullptr;
    QCheckBox* m_copy = nullptr;
    QCheckBox* m_preview = nullptr;
};

// File › New from Template…: tiles of the content templates (built-in, then own) and the start templates (format and
// tracks only), with preview picture, format, length and number of fields; filter by picture shape and tag.
// "New" makes an untitled project from the chosen one, "Edit" opens an own content template itself; own ones can be
// renamed and deleted here right away (like a file manager). ZIP import/export: buttons in place (see .cpp).
class TemplateBrowserDialog : public QDialog {
    Q_OBJECT
public:
    // thumbnail: first frame of a preview.mp4 (the main window's engine); empty = only preview.png pictures
    explicit TemplateBrowserDialog(QWidget* parent = nullptr,
                                   std::function<QImage(const QString& video, const QSize& size)> thumbnail = {});

    enum class Choice { None, NewContent, NewStart, Edit };
    Choice choice() const { return m_choice; }
    ContentTemplate content() const { return m_chosenContent; }
    StartTemplate start() const { return m_chosenStart; }

    // The lists anew (tests: after changing the folders)
    void reload();
    // Tests: the shown tiles' names in order
    QStringList shownNames() const;
    // Tests: select the tile of this name (true = found)
    bool select(const QString& name);

private:
    struct Item {
        bool content = true;
        ContentTemplate t;
        StartTemplate s;
        QString name, aspect;
        QStringList tags;
    };
    void fill();
    void updateButtons();
    const Item* current() const;
    void choose(Choice c);
    void renameCurrent();
    void removeCurrent();
    QIcon tileIcon(const Item& item) const;

    std::function<QImage(const QString&, const QSize&)> m_thumbnail;
    QVector<Item> m_items;
    QComboBox* m_aspect = nullptr;
    QComboBox* m_tag = nullptr;
    QListWidget* m_list = nullptr;
    QLabel* m_details = nullptr;
    QPushButton* m_new = nullptr;
    QPushButton* m_edit = nullptr;
    QPushButton* m_rename = nullptr;
    QPushButton* m_remove = nullptr;
    QPushButton* m_import = nullptr;
    QPushButton* m_export = nullptr;
    Choice m_choice = Choice::None;
    ContentTemplate m_chosenContent;
    StartTemplate m_chosenStart;
};

// "Check Fields": every placeholder of the project with its kind, place and content, and what is wrong with it now
// (TemplateFill::check with what it holds)
class SlotCheckDialog : public QDialog {
    Q_OBJECT
public:
    SlotCheckDialog(const ProjectData& project, QWidget* parent = nullptr);
    int problemCount() const { return m_problems; }

private:
    QTreeWidget* m_tree = nullptr;
    int m_problems = 0;
};
