// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/direct/DirectText.h"

#include "core/Direction.h"
#include "core/I18n.h"

#include <QStringList>
#include <cmath>

namespace DirectText {

namespace {
QString decimal(double v, int digits)
{
    return QString::number(v, 'f', digits).replace('.', I18n::decimalPoint());
}
} // namespace

QString seconds(double s)
{
    return T("%1 s").arg(s >= 10 ? QString::number(qRound(s)) : decimal(s, 1));
}

QString clock(double s)
{
    s = std::max(0.0, s);
    const int tenths = int(std::lround(s * 10));
    const int whole = tenths / 10;
    const int h = whole / 3600, m = (whole / 60) % 60, sec = whole % 60;
    const QString secs = QString("%1%2%3").arg(sec, 2, 10, QChar('0')).arg(I18n::decimalPoint()).arg(tenths % 10);
    return h > 0 ? QString("%1:%2:%3").arg(h).arg(m, 2, 10, QChar('0')).arg(secs) : QString("%1:%2").arg(m).arg(secs);
}

QString reason(const QJsonObject& r)
{
    const QString key = r.value("key").toString();
    const QJsonObject v = r.value("values").toObject();
    const int n = v.value("n").toInt();
    const double s = v.value("s").toDouble();
    if (key == "kills") {
        QString t = n == 1 ? T("1 Kill") : T("%1 Kills").arg(n);
        if (v.value("extra").toString() == "multi") t += " " + T("(Mehrfach-Kill)");
        return t;
    }
    if (key == "loud") return n > 1 ? T("%1 laute Momente").arg(n) : T("Laut");
    if (key == "laughter") return s > 0 ? T("Lachen (%1)").arg(seconds(s)) : T("Lachen");
    if (key == "applause") return T("Applaus");
    if (key == "cheering") return T("Jubel");
    if (key == "scream") return T("Schrei");
    if (key == "motion") return T("Viel Bewegung");
    if (key == "scene_change") return T("Bildwechsel");
    if (key == "face") return T("Gesicht im Bild");
    if (key == "pause") return T("Pause (%1)").arg(seconds(s));
    if (key == "filler") return T("Füllwort „%1“").arg(v.value("word").toString());
    if (key == "slot_match") {
        const QJsonValue sv = v.value("slot"); // the slot's id ("clip1"; a number in older replies)
        const QString slot = sv.isDouble() ? QString::number(sv.toInt()) : sv.toString();
        return slot.isEmpty() ? T("Passt zum Feld") : T("Passt zum Feld „%1“").arg(slot);
    }
    if (key == "take") return T("Bester Take");
    if (key == "fallback_end") return T("Ende der Aufnahme");
    return r.value("text").toString();
}

QString note(const QJsonObject& note)
{
    const QString key = note.value("key").toString();
    if (key == "fake") return T("Platzhalter-Vorschläge ohne Analyse (Vorabversion).");
    return Direction::noteText(key, note.value("values").toObject(), Direction::Lang::Ui);
}

QString talkNeedsTimeline()
{
    return T("„Gespräch ohne Pausen“ arbeitet auf einer Timeline: Die Aufnahmen in Schneiden auf eine Timeline legen und "
             "links „Aktuelle Timeline als Material“ wählen.");
}

QString scriptNeedsText() { return T("Für „Aus Skript“ erst den Text einfügen oder eine Textdatei wählen."); }

QString templateNeedsChoice() { return T("Erst eine Vorlage wählen („Wählen…“)."); }

QString templateNeedsFiles()
{
    return T("„Vorlage füllen“ wählt aus Clips: links Clips hineinziehen statt der aktuellen Timeline.");
}

QString needsMaterial() { return T("Erst Material links hineinziehen (oder „Aktuelle Timeline als Material“ wählen)."); }

QString error(const QString& recipe, const QJsonObject& error, bool apply)
{
    const QString code = error.value("code").toString();
    const QString m = error.value("message").toString();
    const auto has = [&m](const char* part) { return m.contains(QLatin1String(part), Qt::CaseInsensitive); };
    if (code == "NO_TRANSCRIPT" || has("transcript"))
        return T("Dafür braucht schneidi eine Abschrift der Aufnahmen: Whisper nachladen (Hilfe › Erweiterungen) oder "
                 "die Clips im Transkript-Panel transkribieren.");
    if (has("no sentence of the script"))
        return T("Kein Satz des Skripts kommt in den Aufnahmen vor (andere Sprache oder andere Clips?).");
    if (has("missing 'script'") || has("script has no words") || has("needs options.script")) return scriptNeedsText();
    if (has("cannot read script_file")) return T("Die Skript-Datei lässt sich nicht lesen.");
    if (has("not on files") || (recipe == "talk" && code == "NO_MATERIAL")) return talkNeedsTimeline();
    if (code == "NO_MATERIAL") return needsMaterial();
    if (code == "TRACK_LOCKED")
        return T("Eine gesperrte Spur hat Ton in einer Pause: die Spur entsperren oder unter „Spuren“ weglassen.");
    if (recipe == "template" && (has("no slots") || has("no video") || has("no template") || has("slot")))
        return T("Die Vorlage lässt sich mit diesem Material nicht füllen: eine Vorlage wählen und Clips mit Bild "
                 "hineinziehen.");
    if (code == "NOT_FOUND" && has("no such file")) return T("Eine Datei des Materials fehlt.");
    if (code == "SAVE_FAILED")
        return apply ? T("Das Projekt ließ sich nicht speichern (Ordner für Projekte in den Einstellungen prüfen).")
                     : T("Die Vorschläge ließen sich nicht speichern.");
    if (code == "SESSION_STALE") return T("Das Material hat sich geändert: Die Vorschläge passen nicht mehr.");
    if (code == "SESSION_EXPIRED") return T("Die Vorschläge sind abgelaufen.");
    if (code == "CANCELED") return T("Abgebrochen.");
    return {};
}

QString analysis(const QJsonObject& a)
{
    const auto mark = [](const QString& state) {
        if (state == "done") return QString("✓");
        if (state == "running") return QString("…");
        return QString("–");
    };
    const struct { const char* key; const char* name; } parts[] = {
        {"sound", N_("Ton")}, {"picture", N_("Bild")}, {"events", N_("Ereignisse")}, {"speech", N_("Sprache")}};
    QStringList out;
    for (const auto& p : parts) out << T(p.name) + " " + mark(a.value(p.key).toString());
    return out.join(" · ");
}

} // namespace DirectText
