// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Texts of the Direct page: times, the reasons of suggestions and the notes of a session in the UI language. A
// reason comes as a fixed key with values (plan-0.25 › "Vertrag `direct`"): kills, loud, laughter, applause, cheering,
// scream, motion, scene_change, face, pause, filler, slot_match, take, fallback_end; an unknown key shows the
// command's English sentence (`text`).

#include <QJsonObject>
#include <QString>

namespace DirectText {
QString seconds(double s);           // "6,6 s" (decimal point of the UI language), whole from 10 s on: "42 s"
QString clock(double s);             // "0:12,4" / "1:02:03"
QString reason(const QJsonObject& reason);
// A session note in the UI language (its fixed key with values, Direction::noteText); empty for a key without a
// translation (the caller keeps its English `text` apart, never in the page's own text)
QString note(const QJsonObject& note);
// A failed run of `direct` ({code, message}) as a sentence for the user in the UI language: what to do. Known
// situations by code and message; empty = not known (the caller says something general, the message only as a
// tooltip). apply = the error came from Übernehmen
QString error(const QString& recipe, const QJsonObject& error, bool apply = false);
// What a recipe still needs before it can run (shown instead of running into an error), empty = ready
QString talkNeedsTimeline();
QString scriptNeedsText();
QString templateNeedsChoice();
QString templateNeedsFiles();
QString needsMaterial();
// The analysis points of one material entry: "Ton ✓ · Bild ✓ · Ereignisse … · Sprache –"
QString analysis(const QJsonObject& analysis);
} // namespace DirectText
