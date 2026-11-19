// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/direct/DirectText.h"

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
    if (key == "no_whisper") return T("Whisper fehlt: keine Untertitel und Füllwörter (Hilfe › Erweiterungen).");
    if (key == "fake") return T("Platzhalter-Vorschläge ohne Analyse (Vorabversion).");
    if (key == "running") return T("Ein Teil der Analyse läuft noch.");
    return note.value("text").toString();
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
