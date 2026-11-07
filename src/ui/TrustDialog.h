// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "core/CodeTrust.h"

#include <QDialog>

class QPushButton;

// Opening a project with custom code (filter chains, shaders) the user did not write or confirm here
// (core/CodeTrust.h): what it holds (counts, the first lines of each code) and "Ausführen" (trusted and remembered
// for the file) / "Nicht ausführen" (kept in the project, skipped when rendering; the effect can be allowed later).
class TrustDialog : public QDialog {
    Q_OBJECT
public:
    TrustDialog(const QVector<CodeTrust::Item>& items, QWidget* parent = nullptr);
    QPushButton* runButton() const { return m_run; }
    QPushButton* skipButton() const { return m_skip; }

    // Asks when the project has untrusted code; true = it is trusted now (or there was none to ask about)
    static bool askForProject(QWidget* parent, const ProjectData& data, const QString& path);

private:
    QPushButton* m_run;
    QPushButton* m_skip;
};
