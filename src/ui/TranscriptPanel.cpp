#include "ui/TranscriptPanel.h"

#include "app/Theme.h"
#include "core/Editor.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/Timecode.h"
#include "engine/Extensions.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QProgressBar>
#include <QScrollBar>
#include <QSettings>
#include <QTextBlock>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace {
// A pause this long (seconds) starts a new paragraph
constexpr double kParagraphPause = 1.5;
// Removing words also removes the pause after them up to the next word, if it is at most this long (seconds)
constexpr double kMaxCutPause = 1.0;
// "Remove pauses" leaves this much quiet between the words (seconds)
constexpr double kKeepPause = 0.25;
} // namespace

TranscriptPanel::TranscriptPanel(Project* project, Editor* editor, QWidget* parent)
    : QWidget(parent), m_project(project), m_editor(editor)
{
    setMinimumWidth(240);
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(6, 6, 6, 6);
    lay->setSpacing(6);

    auto* title = new QLabel(T("Transkript"));
    title->setStyleSheet("font-weight: 600;");
    m_track = new QComboBox;
    m_track->setToolTip(T("Welche Tonspuren das Transkript zeigt"));
    connect(m_track, &QComboBox::currentIndexChanged, this, [this] { scheduleRebuild(); });
    auto* head = new QHBoxLayout;
    head->addWidget(title);
    head->addStretch(1);
    head->addWidget(m_track);
    lay->addLayout(head);

    m_transcribe = new QToolButton;
    m_transcribe->setText(T("Transkribieren"));
    m_transcribe->setToolTip(T("Sprache der Clips ohne Transkript erkennen (Whisper, läuft im Hintergrund)"));
    connect(m_transcribe, &QToolButton::clicked, this, &TranscriptPanel::transcribe);
    m_pauses = new QToolButton;
    m_pauses->setText(T("Pausen entfernen"));
    m_pauses->setToolTip(T("Schneidet Pausen zwischen den Wörtern heraus, die länger sind als angegeben "
                           "(Ripple auf allen Spuren, ein Rückgängig-Schritt)"));
    connect(m_pauses, &QToolButton::clicked, this, &TranscriptPanel::removePauses);
    m_minPause = new QDoubleSpinBox;
    m_minPause->setRange(0.3, 10);
    m_minPause->setSingleStep(0.1);
    m_minPause->setDecimals(1);
    m_minPause->setSuffix(T(" s"));
    m_minPause->setValue(QSettings().value("transcript/minPause", 1.0).toDouble());
    m_minPause->setToolTip(T("Pausen ab dieser Länge"));
    connect(m_minPause, &QDoubleSpinBox::valueChanged, this, [](double v) { QSettings().setValue("transcript/minPause", v); });
    auto* tools = new QHBoxLayout;
    tools->addWidget(m_transcribe);
    tools->addStretch(1);
    tools->addWidget(m_pauses);
    tools->addWidget(m_minPause);
    lay->addLayout(tools);

    m_progress = new QProgressBar;
    m_progress->setRange(0, 1000);
    m_progress->setTextVisible(false);
    m_progress->setFixedHeight(6);
    m_cancel = new QToolButton;
    m_cancel->setText("✕");
    m_cancel->setToolTip(T("Abbrechen"));
    connect(m_cancel, &QToolButton::clicked, this, [this] {
        m_queue.clear();
        m_transcriber.cancel();
    });
    auto* progress = new QHBoxLayout;
    progress->addWidget(m_progress, 1);
    progress->addWidget(m_cancel);
    lay->addLayout(progress);

    m_hint = new QLabel;
    m_hint->setWordWrap(true);
    m_hint->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));
    lay->addWidget(m_hint);

    m_text = new QTextEdit;
    m_text->setReadOnly(true);
    m_text->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    m_text->setContextMenuPolicy(Qt::CustomContextMenu);
    m_text->setStyleSheet(QString("QTextEdit { background: %1; color: %2; border: none; font-size: 14px; }")
                              .arg(Theme::field.name(), Theme::text.name()));
    m_text->installEventFilter(this);
    m_text->viewport()->installEventFilter(this);
    connect(m_text, &QTextEdit::customContextMenuRequested, this, [this](const QPoint& pos) {
        QMenu menu;
        QAction* cut = menu.addAction(T("Ausgewählte Wörter herausschneiden"));
        cut->setEnabled(selectedWords().first >= 0);
        if (menu.exec(m_text->viewport()->mapToGlobal(pos)) == cut) cutSelection();
    });
    lay->addWidget(m_text, 1);

    connect(m_project, &Project::timelineChanged, this, &TranscriptPanel::scheduleRebuild);
    connect(m_project, &Project::transcriptChanged, this, &TranscriptPanel::scheduleRebuild);
    connect(m_project, &Project::mediaChanged, this, &TranscriptPanel::scheduleRebuild);
    connect(&m_transcriber, &Transcriber::progress, this, [this](double f) {
        const int done = m_queueTotal - int(m_queue.size()) - 1;
        m_progress->setValue(int((done + f) / std::max(1, m_queueTotal) * 1000));
    });
    connect(&m_transcriber, &Transcriber::finished, this, [this](bool ok, const QString& error) {
        if (ok && !m_queue.isEmpty()) {
            const auto [path, stream] = m_queue.takeFirst();
            const Transcript::Result& r = m_transcriber.result();
            MediaTranscript t;
            t.audioStream = stream;
            t.language = r.language;
            t.words = r.words;
            if (t.words.isEmpty()) t.words << TranscriptWord{0, 0, QString()}; // "no speech" is a result too
            m_project->setTranscript(path, t);
        } else if (!ok) {
            m_queue.clear();
            if (error != T("Abgebrochen")) QMessageBox::warning(this, T("Transkript"), error);
        }
        if (!m_queue.isEmpty()) startNext();
        updateState();
    });
    rebuildTracks();
    updateState();
}

TranscriptPanel::~TranscriptPanel() = default;

void TranscriptPanel::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    if (m_dirty) rebuild();
}

void TranscriptPanel::scheduleRebuild()
{
    m_dirty = true;
    if (!isVisible() || m_rebuildQueued) return;
    m_rebuildQueued = true;
    QTimer::singleShot(0, this, [this] {
        m_rebuildQueued = false;
        if (m_dirty && isVisible()) rebuild();
    });
}

void TranscriptPanel::rebuildTracks()
{
    const Timeline& tl = m_project->timeline();
    const QVariant current = m_track->currentData();
    QSignalBlocker block(m_track);
    m_track->clear();
    m_track->addItem(T("Alle Tonspuren"), -1);
    for (int i = 0; i < tl.audio.size(); ++i)
        if (!tl.audio[i].clips.isEmpty())
            m_track->addItem(trackShortName({TrackKind::Audio, i}) + "  " + trackDisplayName(tl.audio[i], {TrackKind::Audio, i}), i);
    const int at = m_track->findData(current.isValid() ? current : QVariant(-1));
    m_track->setCurrentIndex(std::max(0, at));
}

int TranscriptPanel::audioTrack() const { return m_track->currentData().toInt(); }

void TranscriptPanel::rebuild()
{
    m_dirty = false;
    rebuildTracks();
    const double fps = m_project->frameRate();
    m_words = TranscriptCut::timelineWords(m_project->timeline(), m_project->media(), fps, audioTrack());

    const int scroll = m_text->verticalScrollBar()->value();
    m_text->clear();
    m_chars.clear();
    QString text;
    const int paragraph = int(std::lround(kParagraphPause * fps));
    for (int i = 0; i < m_words.size(); ++i) {
        const TranscriptCut::Word& w = m_words[i];
        if (i > 0) {
            const TranscriptCut::Word& prev = m_words[i - 1];
            text += (w.from - prev.spokenTo >= paragraph || w.from < prev.from) ? QStringLiteral("\n\n") : QStringLiteral(" ");
        }
        m_chars << qMakePair(int(text.size()), int(text.size() + w.text.size()));
        text += w.text;
    }
    m_text->setPlainText(text);
    m_text->verticalScrollBar()->setValue(scroll);
    m_current = -1;
    updateHighlight();
    updateState();
}

int TranscriptPanel::wordAt(int pos) const
{
    // First word that ends after pos (a click into the space before a word counts for that word)
    const auto it = std::lower_bound(m_chars.begin(), m_chars.end(), pos,
                                     [](const QPair<int, int>& r, int p) { return r.second <= p; });
    return it == m_chars.end() ? -1 : int(it - m_chars.begin());
}

QPair<int, int> TranscriptPanel::selectedWords() const
{
    const QTextCursor c = m_text->textCursor();
    if (!c.hasSelection() || m_chars.isEmpty()) return {-1, -1};
    const int from = c.selectionStart(), to = c.selectionEnd();
    // Every word the selection touches
    int first = -1, last = -1;
    for (int i = 0; i < m_chars.size(); ++i)
        if (m_chars[i].first < to && m_chars[i].second > from) {
            if (first < 0) first = i;
            last = i;
        }
    return {first, last};
}

void TranscriptPanel::selectWords(int first, int last)
{
    if (m_dirty) rebuild();
    if (first < 0 || last >= m_chars.size() || first > last) return;
    QTextCursor c = m_text->textCursor();
    c.setPosition(m_chars[first].first);
    c.setPosition(m_chars[last].second, QTextCursor::KeepAnchor);
    m_text->setTextCursor(c);
}

bool TranscriptPanel::cutSelection()
{
    const auto [first, last] = selectedWords();
    if (first < 0) return false;
    // Contiguous runs inside the selection are one range each (a selection always is one run here)
    const int maxPause = int(std::lround(kMaxCutPause * m_project->frameRate()));
    const QPair<int, int> r = TranscriptCut::cutRange(m_words, first, last, maxPause);
    m_editor->deleteRanges({r}, true, T("Wörter herausschneiden"));
    emit seekRequested(r.first);
    return true;
}

void TranscriptPanel::removePauses()
{
    if (m_dirty) rebuild();
    const double fps = m_project->frameRate();
    const auto ranges = TranscriptCut::pauseRanges(m_words, int(std::lround(m_minPause->value() * fps)),
                                                   int(std::lround(kKeepPause * fps)));
    if (ranges.isEmpty()) {
        QMessageBox::information(this, T("Pausen entfernen"), T("Keine Pausen dieser Länge zwischen den Wörtern."));
        return;
    }
    m_editor->deleteRanges(ranges, true, T("Pausen entfernen"));
}

void TranscriptPanel::setPlayhead(int frame)
{
    m_playhead = frame;
    if (!isVisible() || m_dirty) return;
    updateHighlight();
}

void TranscriptPanel::updateHighlight()
{
    int current = -1;
    for (int i = 0; i < m_words.size(); ++i) {
        if (m_words[i].from > m_playhead) break;
        if (m_playhead < m_words[i].to) current = i;
    }
    if (current == m_current) return;
    m_current = current;
    QList<QTextEdit::ExtraSelection> extra;
    if (current >= 0) {
        QTextEdit::ExtraSelection s;
        s.cursor = QTextCursor(m_text->document());
        s.cursor.setPosition(m_chars[current].first);
        s.cursor.setPosition(m_chars[current].second, QTextCursor::KeepAnchor);
        s.format.setBackground(Theme::alpha(Theme::primary, 90));
        extra << s;
        // Follow the playhead unless the user is selecting
        if (!m_text->textCursor().hasSelection()) {
            const QRect r = m_text->cursorRect(s.cursor);
            if (!m_text->viewport()->rect().contains(r))
                m_text->verticalScrollBar()->setValue(m_text->verticalScrollBar()->value() + r.center().y()
                                                      - m_text->viewport()->height() / 2);
        }
    }
    m_text->setExtraSelections(extra);
}

void TranscriptPanel::updateState()
{
    const bool running = m_transcriber.isRunning();
    const auto missing = TranscriptCut::missingTranscripts(m_project->timeline(), m_project->media(), audioTrack());
    m_transcribe->setEnabled(!running && !missing.isEmpty());
    m_pauses->setEnabled(!running && m_words.size() > 1);
    m_progress->setVisible(running);
    m_cancel->setVisible(running);
    m_track->setEnabled(!running);
    QString hint;
    if (running)
        hint = T("Sprache wird erkannt … (%1 von %2)").arg(m_queueTotal - m_queue.size() + 1).arg(m_queueTotal);
    else if (m_words.isEmpty() && missing.isEmpty())
        hint = T("Keine gesprochenen Wörter in den Tonclips (Clips mit geändertem Tempo zählen nicht).");
    else if (m_words.isEmpty())
        hint = T("Noch kein Transkript. „Transkribieren“ erkennt, was in den Tonclips gesagt wird.");
    else if (!missing.isEmpty())
        hint = T("Clips ohne Transkript: %1").arg(missing.size());
    else
        hint = T("Wörter markieren und Entf drücken schneidet sie heraus. Klick = zum Wort springen.");
    m_hint->setText(hint);
}

void TranscriptPanel::transcribe()
{
    if (m_transcriber.isRunning()) return;
    if (Extensions::whisperProgram().isEmpty() || Extensions::installedWhisperModels().isEmpty()) {
        const auto answer = QMessageBox::question(
            this, T("Transkript"),
            T("Dafür wird die Erweiterung „Whisper“ mit einem Sprachmodell gebraucht (einmaliger Download). "
              "Erweiterungen öffnen?"));
        if (answer == QMessageBox::Yes) emit openExtensions();
        return;
    }
    m_queue = TranscriptCut::missingTranscripts(m_project->timeline(), m_project->media(), audioTrack());
    m_queueTotal = int(m_queue.size());
    if (m_queue.isEmpty()) return;
    startNext();
    updateState();
}

void TranscriptPanel::startNext()
{
    const auto [path, stream] = m_queue.first();
    TranscribeRequest req;
    req.media = path;
    req.audioStream = stream;
    req.format = m_project->format();
    QSettings settings;
    req.language = settings.value("transcribe/language", "auto").toString();
    req.model = Extensions::whisperModelPath(settings.value("transcribe/model").toString());
    QString error;
    if (!m_transcriber.start(req, &error)) {
        m_queue.clear();
        QMessageBox::warning(this, T("Transkript"), error);
    }
    m_progress->setValue(int(double(m_queueTotal - m_queue.size()) / std::max(1, m_queueTotal) * 1000));
}

bool TranscriptPanel::eventFilter(QObject* obj, QEvent* e)
{
    if (obj == m_text && e->type() == QEvent::KeyPress) {
        const auto* k = static_cast<QKeyEvent*>(e);
        if ((k->key() == Qt::Key_Delete || k->key() == Qt::Key_Backspace) && cutSelection()) return true;
    }
    if (obj == m_text && e->type() == QEvent::ShortcutOverride) {
        // Delete/Backspace belong to the text here (otherwise the timeline's delete action would take them)
        const auto* k = static_cast<QKeyEvent*>(e);
        if ((k->key() == Qt::Key_Delete || k->key() == Qt::Key_Backspace) && selectedWords().first >= 0) {
            e->accept();
            return true;
        }
    }
    if (obj == m_text->viewport() && e->type() == QEvent::MouseButtonRelease) {
        const auto* me = static_cast<QMouseEvent*>(e);
        if (me->button() == Qt::LeftButton && !m_text->textCursor().hasSelection()) {
            const int w = wordAt(m_text->cursorForPosition(me->position().toPoint()).position());
            if (w >= 0) emit seekRequested(m_words[w].from);
        }
    }
    return QWidget::eventFilter(obj, e);
}
