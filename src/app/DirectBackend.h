// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// The work behind the Direct page ("Regie", ui/direct/DirectPage.h): suggestions for a recipe on some material, and
// applying the checked ones as a timeline. The page never edits the project itself - it shows what the backend
// returns and hands the user's choice back.
//
// JSON shapes = the `direct` command's contract (plan-0.25 › "Vertrag `direct`"):
// - suggest(recipe, material, options): material is {files:[path…]} or {timeline:"@current"}; options the recipe
//   card's settings (short: for, min_s, max_s, aspect, captions, variants, sfx; highlights: length_s|"all", aspect,
//   music, order, transitions; talk: min_pause_s, pad_s, fillers, room_tone, target_lufs; template: template, auto;
//   script: script|script_file). Runs in the background: onProgress(fraction 0..1 or -1, stage) and one onDone(reply)
//   with {"ok":true,"result":{session:{id, recipe, options, material[], suggestions[], order[], length_s, target_s,
//   notes[]}, next}} or {"ok":false,"error":{code,message}} (CANCELED after cancel()). Both callbacks run in the UI
//   thread, never after `receiver` is gone.
// - apply(session, choice): choice = {on:[n…]|"all", order:[n…] (the checked ones as listed), adjust:[{n, from_s,
//   to_s}] (only changed ranges), variant? (short: 1-3)}. Also in the background (the command builds the timeline,
//   then `check` runs): onProgress / one onDone(reply), one undo step in the app (the user's own, not the AI's).
//   Reply {"ok":true,"result":{timeline, length_s, …}} or {"ok":false,"error":{code,message}}: SESSION_STALE (the
//   material changed) and SESSION_EXPIRED (older than 24 h) ask for new suggestions, CANCELED after cancel(). The
//   result is always a NEW timeline (talk: a copy of the current one without the pauses), never the open one
//   changed.
//
// The real backend (package R1) runs `direct` through Cli::Live::runInBackground (suggest) and Cli::Live::runForUser
// (apply); main.cpp hands it in (MainWindow::setDirectBackend), since the app library does not link the command layer.
// Until then MainWindow uses FakeDirectBackend.

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <functional>

class QObject;

class DirectBackend
{
public:
    using Progress = std::function<void(double fraction, const QString& stage)>;
    using Done = std::function<void(const QJsonObject& reply)>;
    // A running suggestion job: cancel() ends it (its onDone then says CANCELED, or nothing comes when the receiver
    // is gone); calling it after the end does nothing
    struct JobHandle {
        std::function<void()> cancel;
    };

    virtual ~DirectBackend() = default;
    virtual JobHandle suggest(const QString& recipe, const QJsonObject& material, const QJsonObject& options,
                              const Progress& onProgress, const Done& onDone, QObject* receiver) = 0;
    virtual JobHandle apply(const QJsonObject& session, const QJsonObject& choice, const Progress& onProgress,
                            const Done& onDone, QObject* receiver) = 0;
};

// FAKE - a stand-in until the `direct` command exists (package R1), so that the page can be clicked through and tested.
// suggest() makes suggestions in the contract's shape out of the material's lengths alone (no analysis: two moments
// per file for short/highlights, pauses for talk, slots for template, takes for script), with a short fake progress.
// apply() runs one `edit` through the user runner (one undo step): a new timeline with the checked ranges appended
// in order; talk: a copy of the current timeline with the checked pauses deleted. Sessions it did not make answer
// SESSION_EXPIRED, sessions whose material files changed since SESSION_STALE. Replace it by the real backend;
// nothing here is meant to stay.
class FakeDirectBackend : public DirectBackend
{
public:
    using UserRunner = std::function<QJsonObject(const QString& command, const QJsonObject& args, const QString& undoText)>;
    // lengthOf: a file's length in seconds (0 = unknown: the fake takes 30 s); currentTimeline: the open timeline's id
    // for `new_timeline from`; stepMs: the fake progress' step
    FakeDirectBackend(UserRunner runner, std::function<double(const QString&)> lengthOf,
                      std::function<QString()> currentTimeline = {}, int stepMs = 60);

    JobHandle suggest(const QString& recipe, const QJsonObject& material, const QJsonObject& options,
                      const Progress& onProgress, const Done& onDone, QObject* receiver) override;
    JobHandle apply(const QJsonObject& session, const QJsonObject& choice, const Progress& onProgress,
                    const Done& onDone, QObject* receiver) override;
    // The edit apply() runs, at once (tests)
    QJsonObject applyNow(const QJsonObject& session, const QJsonObject& choice);
    // The ops apply() would run (tests)
    QJsonArray opsFor(const QJsonObject& session, const QJsonObject& choice) const;

    // The session suggest() would answer, at once (tests)
    QJsonObject makeSession(const QString& recipe, const QJsonObject& material, const QJsonObject& options) const;
    int applied() const { return m_applied; }

private:
    UserRunner m_runner;
    std::function<double(const QString&)> m_lengthOf;
    std::function<QString()> m_currentTimeline;
    QHash<QString, qint64> m_made; // session id -> the material's stamp when it was made (stale check)
    int m_stepMs;
    int m_sessions = 0;
    int m_applied = 0;
};
