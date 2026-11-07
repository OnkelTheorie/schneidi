// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Inspector of the Comp page: the selected node's parameters (NodeType::params) - numbers with a keyframe diamond
// like the Edit page's Inspector, choices, switches, colors (the key color with a pipette), files. Each change is one
// Editor op: setCompNodeValuesAt (a drag of a number field = one undo step; a number with keyframes gets one at the
// playhead), setCompNodeKeyframe for the diamond. Widgets are rebuilt only when another node (or another set of
// parameters, a mask's shape) is shown, never during a drag. An fx node shows its effect's own values (e_<key>); a
// text value (the filter chain, the shader code) is edited in a multi-line field and set with "Anwenden" after the
// same check as the CLI's (the error shown under the field, nothing set).

#include <QScrollArea>
#include <QStringList>
#include <QVector>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>

#include "core/CompTrack.h"

class Editor;
namespace Picture::Comp {
struct NodeType;
}
class QLabel;
class QGridLayout;
class QToolButton;
struct Clip;

class CompInspector : public QScrollArea {
    Q_OBJECT
public:
    explicit CompInspector(Editor* editor, QWidget* parent = nullptr);
    ~CompInspector() override;

    void setTarget(int clipId, int node);
    void setPlayhead(int frame);
    void refresh(); // values (and rows, if the shown parameters changed) from the project
    int node() const { return m_node; }
    // Analysis of a cutout node ("Freistellen analysieren": Picture::Comp::cutoutNode over the whole clip, then
    // Editor::cutoutCompNode) or a tracker node ("Tracken": trackNode from the playhead to the clip's end, then
    // Editor::trackCompNode) - in the background with progress and cancel; the result is one undo step
    bool analyzing() const { return bool(m_job); }
    void startAnalysis();  // the button's act for the shown node
    void cancelAnalysis();

signals:
    // Pipette of a color parameter (the key color): pick it from the picture going into the node
    void keyPickRequested(int node, const QString& key);
    void outRequested(int node); // "Als Ausgang"
    void extensionsRequested();  // the cutout model is missing: open the extensions dialog

private:
    const Clip* clip() const;
    // The node's kind as shown: an fx node also gets the wrapped effect's own parameters as e_<key> (labels, ranges,
    // defaults of the effect's descriptor)
    std::shared_ptr<const Picture::Comp::NodeType> shownKind(const Clip& c) const;
    QStringList shownKeys(const Clip& c) const; // the parameters shown for the node now
    void rebuild(const Clip& c, const QStringList& keys);
    void set(const QString& key, const QVariant& value, bool drag);
    int addAnalysisRow(int row, const QString& kind); // cutout / tracker: button, progress, hint; returns the next row
    void updateAnalysisRow();
    void analysisDone(int clipId, int node, const QString& kind, const QString& cache,
                      const QVector<::Comp::TrackPoint>& points, const QString& error, const QString& note);

    Editor* m_editor;
    int m_clipId = 0, m_node = 0, m_playhead = 0;
    QString m_type;
    QStringList m_keys;
    QWidget* m_body;
    QLabel* m_title;
    QLabel* m_about;
    QToolButton* m_outButton;
    QGridLayout* m_grid = nullptr;
    QWidget* m_rows = nullptr;
    QVector<std::function<void(const Clip&)>> m_updates;
    struct Job {
        int clipId = 0, node = 0;
        QString kind;
        std::shared_ptr<std::atomic<bool>> cancel;
        int done = 0, total = 0;
    };
    std::optional<Job> m_job;
    QString m_analysisNote; // what the last analysis of the shown node said (error, result)
    int m_noteNode = 0;
    class QPushButton* m_analyzeButton = nullptr;
    QLabel* m_analyzeStatus = nullptr;
    class QPushButton* m_extensionsButton = nullptr;
};
