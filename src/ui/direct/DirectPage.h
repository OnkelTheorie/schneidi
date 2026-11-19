// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// The Direct page "Regie" (0.25): material in, a recipe, suggestions to check, a timeline out - without a chat AI.
// Three columns: the material on the left (DirectMaterial), in the middle the shared viewer on top (MainWindow puts
// it into center() at index 0 when the page shows) and the recipe cards under it (DirectRecipes), the suggestions on
// the right (DirectSuggestions; above them a slot for the own wish line of the AI chat, package R5, empty and hidden
// for now). Along the bottom the result: the overview strip of the timeline the checked suggestions would give (only
// to look at), "Übernehmen", "In Schneiden öffnen" and "Exportieren…".
// The page itself never edits the project: MainWindow (MainWindowDirect.cpp) runs the DirectBackend on its requests.
// Built on the page's first showing (MainWindow::ensureDirectPage), never at the start.

#include "core/Types.h"

#include <QWidget>

class DirectMaterial;
class DirectRecipes;
class DirectSuggestions;
class QLabel;
class QPushButton;
class QSplitter;
class TimelineOverview;

class DirectPage : public QWidget {
    Q_OBJECT
public:
    explicit DirectPage(QWidget* parent = nullptr);

    QSplitter* root() const { return m_root; }     // columns | result row
    QSplitter* columns() const { return m_columns; } // material | middle | suggestions
    QSplitter* center() const { return m_center; }   // viewer | recipes
    DirectMaterial* material() const { return m_material; }
    DirectRecipes* recipes() const { return m_recipes; }
    DirectSuggestions* suggestions() const { return m_suggestions; }
    TimelineOverview* overview() const { return m_overview; }
    QPushButton* applyButton() const { return m_apply; }
    QPushButton* openEditButton() const { return m_openEdit; }
    QPushButton* exportButton() const { return m_export; }
    QWidget* wishSlot() const { return m_wish; } // R5: the own wish line (hidden until a chat provider is set up)

    // Frame rate of the result strip (the project's)
    void setFps(double fps);
    // A suggestion run: buttons off, the busy text in the list
    void setBusy(bool busy, const QString& text = {});
    bool busy() const { return m_busy; }
    // The result strip anew from the checked suggestions in their order
    void updateResult();
    // The timeline the checked suggestions give: one video track (files with a picture) and one audio track, back to
    // back in the list's order (only to look at; Übernehmen builds the real one through the backend)
    Timeline resultTimeline() const;

signals:
    void suggestRequested();
    void applyRequested();
    void openEditRequested();
    void exportRequested();

private:
    QSplitter* m_root;
    QSplitter* m_columns;
    QSplitter* m_center;
    DirectMaterial* m_material;
    DirectRecipes* m_recipes;
    DirectSuggestions* m_suggestions;
    TimelineOverview* m_overview;
    QLabel* m_resultText;
    QPushButton* m_apply;
    QPushButton* m_openEdit;
    QPushButton* m_export;
    QWidget* m_wish;
    double m_fps = 25;
    bool m_busy = false;
};
