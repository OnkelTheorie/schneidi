// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/SlotText.h"

#include "core/I18n.h"

#include <QFileInfo>
#include <QStringList>

namespace SlotText {

QString kindName(SlotKind kind)
{
    switch (kind) {
    case SlotKind::Clip: return T("Clip");
    case SlotKind::Text: return T("Text");
    case SlotKind::Music: return T("Musik");
    case SlotKind::Logo: return T("Logo");
    }
    return {};
}

QString fitName(SlotFit fit)
{
    switch (fit) {
    case SlotFit::Trim: return T("Kürzer lassen");
    case SlotFit::Speed: return T("Verlangsamen");
    case SlotFit::Freeze: return T("Letztes Bild halten");
    case SlotFit::Loop: return T("Wiederholen");
    }
    return {};
}

QString seconds(double s)
{
    return T("%1 s").arg(QString::number(s, 'f', 1).replace('.', I18n::decimalPoint()));
}

QString tooltip(const ClipSlot& slot, double fps)
{
    fps = std::max(1.0, fps);
    QStringList lines;
    lines << QString("<b>%1</b>").arg(T("Platzhalter „%1“").arg((slot.label.isEmpty() ? slot.id : slot.label).toHtmlEscaped()));
    lines << T("Art: %1").arg(kindName(slot.kind)) + QString(" &nbsp;·&nbsp; id: %1").arg(slot.id.toHtmlEscaped());
    if (!slot.hint.isEmpty()) lines << QString("<i>%1</i>").arg(slot.hint.toHtmlEscaped());
    if (slot.kind == SlotKind::Text) {
        if (slot.maxChars > 0) lines << T("Höchstens %1 Zeichen").arg(slot.maxChars) + (slot.shrink ? ", " + T("längerer Text wird kleiner") : QString());
    } else {
        QString length = slot.length == SlotLength::Fixed ? T("Feste Länge") : T("Flexible Länge");
        if (slot.length == SlotLength::Flexible && (slot.minLen > 0 || slot.maxLen > 0))
            length += QString(" (%1 – %2)")
                          .arg(slot.minLen > 0 ? seconds(slot.minLen / fps) : QString("0"),
                               slot.maxLen > 0 ? seconds(slot.maxLen / fps) : QString("∞"));
        lines << length;
        if (slot.kind == SlotKind::Clip) {
            lines << T("Zu kurz: %1").arg(fitName(slot.fit));
            if (slot.reframe) lines << T("Andere Bildform wird angepasst");
        }
        if (slot.kind == SlotKind::Music && slot.fitMusic) lines << T("Musik wird an Phrasen auf die Länge geschnitten");
    }
    lines << QString("<span style='color:gray'>%1</span>").arg(T("Datei aus dem Medienpool hierher ziehen füllt den Platzhalter (Alt: normal einfügen)"));
    return lines.join("<br>");
}

QString problem(const TemplateFill::Problem& p)
{
    const QString& c = p.code;
    QString text;
    if (c == "UNKNOWN_SLOT") text = T("Diesen Platzhalter gibt es nicht");
    else if (c == "MISSING_VALUE") text = T("Nichts angegeben");
    else if (c == "FILE_MISSING") text = T("Datei nicht gefunden");
    else if (c == "UNREADABLE") text = T("Datei nicht lesbar");
    else if (c == "WRONG_TYPE") text = T("Falsche Art von Datei (Bild/Ton fehlt)");
    else if (c == "BAD_RANGE") text = T("Ungültiger Bereich");
    else if (c == "EMPTY_TEXT") text = T("Leerer Text");
    else if (c == "TOO_LONG") text = T("Text zu lang");
    else if (c == "SLOT_MISMATCH") text = T("Der Clip passt nicht zur Art des Platzhalters");
    else if (c == "LOCKED") text = T("Spur gesperrt");
    else text = p.message;
    return text;
}

QString report(const TemplateFill::Report& r, const QString& label)
{
    if (r.results.isEmpty() || !r.ok()) {
        const TemplateFill::Problem p = r.errors.value(0);
        return T("Platzhalter „%1“ nicht gefüllt: %2").arg(label, problem(p));
    }
    // The first instance tells what happened (the others got the same value)
    const TemplateFill::SlotReport& s = r.results.first();
    const auto num = [&](const char* key) { return s.details.value(key).toDouble(); };
    QStringList done;
    for (const QString& a : s.adjusted) {
        if (a == "trimmed") done << T("gekürzt %1 → %2").arg(seconds(num("value_s")), seconds(num("slot_s")));
        else if (a == "shortened") done << T("kürzer als der Platzhalter");
        else if (a == "slowed") done << T("verlangsamt auf %1 %").arg(QString::number(num("speed_pct"), 'f', 0));
        else if (a == "frozen") done << T("letztes Bild %1 gehalten").arg(seconds(num("frozen_s")));
        else if (a == "looped") done << T("%1× wiederholt").arg(int(num("repeats")));
        else if (a == "rippled") done << T("Rest verschoben um %1").arg(seconds(num("ripple_s")));
        else if (a == "reframed") done << T("Bildausschnitt angepasst");
        else if (a == "reshaped") done << T("Bildausschnitt angepasst");
        else if (a == "no_sound") done << T("ohne Ton");
        else if (a == "sound_added") done << T("mit Ton");
        else if (a == "compound") done << T("Verbundclips angepasst");
        else if (a == "shrunk") done << T("Schrift verkleinert");
        else if (a == "fit_music") done << T("an Phrasen geschnitten");
        else if (a == "faded") done << T("ausgeblendet");
    }
    done.removeDuplicates();
    QString text = T("Platzhalter „%1“ gefüllt").arg(label);
    if (!done.isEmpty()) text += ": " + done.join(", ");
    if (!s.warnings.isEmpty()) text += " ⚠";
    return text;
}

} // namespace SlotText
