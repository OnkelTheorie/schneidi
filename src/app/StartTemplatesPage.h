#pragma once
#include "core/StartTemplates.h"

#include <QDialog>
#include <QVector>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

// Settings › Start Templates (core/StartTemplates.h): which template new projects start with ("Neutral" = as
// always), whether File › New Project asks each time, and the templates themselves: built-in ones are read-only
// (copying makes an own one), own ones can be renamed and deleted. Copy, rename and delete change the files right
// away like a file manager (with their own questions); the default and the question count on OK (defaultId(),
// askOnNew(); the caller saves them).
class StartTemplatesPage : public QWidget {
    Q_OBJECT
public:
    explicit StartTemplatesPage(QWidget* parent = nullptr);
    QString defaultId() const;
    bool askOnNew() const;
    // The list anew (a template was saved meanwhile); the default stays, or becomes `defaultId` if given
    void reload(const QString& defaultId = {});

signals:
    void saveRequested(); // "From the Project…": the main window saves the open project as template

private:
    void fill(const QString& defaultId, const QString& select);
    void showDetails();
    const StartTemplate* current() const;
    void copyCurrent();
    void renameCurrent();
    void removeCurrent();

    QVector<StartTemplate> m_templates;
    QComboBox* m_default = nullptr;
    QCheckBox* m_ask = nullptr;
    QListWidget* m_list = nullptr;
    QLabel* m_details = nullptr;
    QPushButton* m_rename = nullptr;
    QPushButton* m_remove = nullptr;
};

// File › Save as Start Template…: name, description and whether new projects start with it; shows what the template
// takes from the project (StartTemplates::summary)
class SaveStartTemplateDialog : public QDialog {
    Q_OBJECT
public:
    // t: StartTemplates::fromProject with a suggested name
    explicit SaveStartTemplateDialog(const StartTemplate& t, QWidget* parent = nullptr);
    QString name() const;
    QString description() const;
    bool makeDefault() const;

private:
    QLineEdit* m_name = nullptr;
    QLineEdit* m_description = nullptr;
    QCheckBox* m_default = nullptr;
};
