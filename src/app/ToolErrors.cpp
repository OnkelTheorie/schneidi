// SPDX-License-Identifier: AGPL-3.0-only
#include "app/ToolErrors.h"

#include "core/I18n.h"

namespace ToolErrors {

QString text(const QString& code, const QString& message)
{
    // Messages of fit_music (cli/EditOpsMusic.cpp) that need the user to act on the timeline
    if (code == "FAILED" && message.contains("would cover clip"))
        return T("Der gekürzte Song würde andere Clips auf seiner Spur überdecken – den Clip daneben verschieben oder "
                 "eine freie Spur nehmen.");
    if (code == "BAD_ARGUMENT" && message.contains("linked to a picture"))
        return T("Der Clip ist mit einem Bild verknüpft – erst die Verknüpfung lösen.");
    if (code == "BAD_ARGUMENT" && message.contains("has another speed"))
        return T("Der Clip läuft mit anderer Geschwindigkeit – erst auf 100 % zurücksetzen.");
    if (code == "BAD_ARGUMENT" && message.contains("is no audio clip"))
        return T("Das geht nur mit einem Audioclip einer Datei (der Musik auf einer A-Spur).");
    if (code == "APP_BUSY" || code == "APP_WAITING" || code == "PROJECT_LOCKED")
        return T("Die App führt gerade einen anderen Befehl aus – bitte gleich noch einmal versuchen.");
    if (code == "TOO_SHORT") return T("Der Song ist kürzer als die gewünschte Länge.");
    if (code == "NO_FIT")
        return T("Für diese Länge passt keine Kürzung an Phrasen- und Taktgrenzen (Anfang und Ende des Songs "
                 "zusammen sind schon länger).");
    if (code == "NO_BEAT") return T("Im Song wurde kein Takt gefunden (Stille, oder kürzer als vier Schläge).");
    if (code == "NO_AUDIO") return T("Die Datei hat keinen Ton, oder er ließ sich nicht lesen.");
    if (code == "NO_VIDEO") return T("Die Datei hat kein Bild.");
    if (code == "NO_TRANSCRIPT") return T("Dafür braucht es zuerst ein Transkript.");
    if (code == "NO_SPEECH") return T("In der Aufnahme wurde keine Sprache gefunden.");
    if (code == "EMPTY") return T("Es gibt nichts, worauf der Schritt wirken kann (leere Timeline oder Auswahl).");
    if (code == "EXISTS") return T("Die Zieldatei gibt es schon.");
    if (code == "DECODE_FAILED" || code == "BAD_MEDIA" || code == "OPEN_FAILED")
        return T("Eine Datei ließ sich nicht lesen.");
    if (code == "WRITE_FAILED" || code == "SAVE_FAILED") return T("Eine Datei ließ sich nicht schreiben.");
    if (code == "NOT_INSTALLED" || code == "TOOL_MISSING")
        return T("Dafür fehlt eine Erweiterung (Einstellungen › Erweiterungen).");
    // The rest: the tool's own words (English, for AIs) inside a sentence of the UI language
    const QString detail = message.trimmed().isEmpty() ? code : message.trimmed();
    if (code == "NOT_FOUND") return T("Etwas, worauf sich der Schritt bezieht, gibt es nicht (mehr). Meldung: „%1“").arg(detail);
    if (code == "BAD_ARGUMENT") return T("Der Schritt hat ungültige Angaben. Meldung: „%1“").arg(detail);
    return T("Der Schritt ist fehlgeschlagen. Meldung: „%1“").arg(detail);
}

} // namespace ToolErrors
