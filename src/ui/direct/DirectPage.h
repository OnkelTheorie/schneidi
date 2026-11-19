// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// The Direct page "Regie" (0.25): material in, a recipe, suggestions to check, a timeline out - without a chat AI.
// Three columns: the material on the left (DirectMaterial), in the middle the shared viewer on top (MainWindow puts
// it into center() at index 0 when the page shows) and the recipe cards under it (DirectRecipes), the suggestions on
// the right (DirectSuggestions; above them the own wish line for the AI chat, DirectWish, shown only with a chat
// provider). Along the bottom the result: the overview strip of the timeline the checked suggestions would give (only
// to look at), "Übernehmen", "In Schneiden öffnen" and "Exportieren…".
// The page itself never edits the project: MainWindow (MainWindowDirect.cpp) runs the DirectBackend on its requests.
// Built on the page's first showing (MainWindow::ensureDirectPage), never at the start.

#include "core/Types.h"

#include <QWidget>

class DirectMaterial;
class DirectRecipes;
class DirectSuggestions;
class DirectThumbs;
class DirectWish;
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
    QWidget* wishSlot() const; // the own wish line (hidden until a chat provider is set up)
    DirectWish* wish() const { return m_wish; }
    // What the chat's AI gets with a wish (AiChat::addNote, English, at most maxChars): the material (files with
    // their lengths, or the current timeline), the recipe and its options, the session id and its suggestions as
    // listed (n, kind, file, from-to, reason key, on/off; the rest counted when they do not fit) and how to answer:
    // by calling `direct`, a new suggestion list rather than an apply
    QString wishContext(int maxChars = kWishContextChars) const;
    static constexpr int kWishContextChars = 2000;

    // Frame rate of the result strip (the project's)
    void setFps(double fps);
    // A suggestion run: buttons off, the busy text in the list
    void setBusy(bool busy, const QString& text = {});
    bool busy() const { return m_busy; }
    // Übernehmen runs: its button off, the text beside it
    void setApplying(bool applying, const QString& text = {});
    // After a short's Übernehmen: the second button "Untertitel + Effektklänge" (finishRequested)
    void setFinishOffered(bool offered);
    QPushButton* finishButton() const { return m_finish; }
    // A template filled into a new project file: its name beside the buttons and "Öffnen" (empty = none)
    void setMadeProject(const QString& path);
    QPushButton* openProjectButton() const { return m_openProject; }
    bool applying() const { return m_applying; }
    // Pictures of both lists (MainWindow sets the decoder)
    DirectThumbs* thumbs() const { return m_thumbs; }
    // The result strip anew from the checked suggestions in their order
    void updateResult();
    // The timeline the checked suggestions give: back to back in the list's order with the user's In/Out, files with a
    // picture on V1 (+ their sound on A1), sound-only files on A1 (only to look at; Übernehmen builds the real one
    // through the backend)
    Timeline resultTimeline() const;

signals:
    void suggestRequested();
    void applyRequested();
    void openEditRequested();
    void exportRequested();
    void finishRequested();
    void openProjectRequested(const QString& path);

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
    QPushButton* m_finish;
    QPushButton* m_openProject;
    QString m_madeProject;
    QPushButton* m_openEdit;
    QPushButton* m_export;
    DirectWish* m_wish;
    DirectThumbs* m_thumbs;
    double m_fps = 25;
    bool m_busy = false;
    bool m_applying = false;
};
