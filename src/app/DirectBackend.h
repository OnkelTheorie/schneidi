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
// - apply(session, choice): choice = {on:[n…]|"all", adjust:[{n, from_s, to_s}], order:[n…], variant?}. Synchronous,
//   one undo step in the app (the user's own, not the AI's). Reply {"ok":true,"result":{timeline, length_s, …}} or
//   {"ok":false,"error":{code,message}}.
//
// The real backend (package R1) runs `direct` through Cli::Live::runInBackground (suggest) and Cli::Live::runForUser
// (apply); main.cpp hands it in (MainWindow::setDirectBackend), since the app library does not link the command layer.
// Until then MainWindow uses FakeDirectBackend.

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
    virtual QJsonObject apply(const QJsonObject& session, const QJsonObject& choice) = 0;
};

// FAKE - a stand-in until the `direct` command exists (package R1), so that the page can be clicked through and tested.
// suggest() makes suggestions in the contract's shape out of the material's lengths alone (no analysis: two moments
// per file for short/highlights, pauses for talk, slots for template, takes for script), with a short fake progress.
// apply() runs one `edit` through the user runner: a new timeline with the checked ranges appended in order (one
// undo step). Replace it by the real backend; nothing here is meant to stay.
class FakeDirectBackend : public DirectBackend
{
public:
    using UserRunner = std::function<QJsonObject(const QString& command, const QJsonObject& args, const QString& undoText)>;
    // lengthOf: a file's length in seconds (0 = unknown: the fake takes 30 s); stepMs: the fake progress' step
    FakeDirectBackend(UserRunner runner, std::function<double(const QString&)> lengthOf, int stepMs = 60);

    JobHandle suggest(const QString& recipe, const QJsonObject& material, const QJsonObject& options,
                      const Progress& onProgress, const Done& onDone, QObject* receiver) override;
    QJsonObject apply(const QJsonObject& session, const QJsonObject& choice) override;

    // The session suggest() would answer, at once (tests)
    QJsonObject makeSession(const QString& recipe, const QJsonObject& material, const QJsonObject& options) const;
    int applied() const { return m_applied; }

private:
    UserRunner m_runner;
    std::function<double(const QString&)> m_lengthOf;
    int m_stepMs;
    int m_sessions = 0;
    int m_applied = 0;
};
