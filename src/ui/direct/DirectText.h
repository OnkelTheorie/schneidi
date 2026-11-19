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
QString note(const QJsonObject& note);
// The analysis points of one material entry: "Ton ✓ · Bild ✓ · Ereignisse … · Sprache –"
QString analysis(const QJsonObject& analysis);
} // namespace DirectText
