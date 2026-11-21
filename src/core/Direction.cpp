// SPDX-License-Identifier: AGPL-3.0-only
#include "core/Direction.h"

#include "core/I18n.h"


#include <algorithm>
#include <cmath>

namespace Direction {

namespace {

// One text per key: English for the AI, the German key for the UI (T() at display time); "many" variants where the
// count n is not 1. Placeholders {n}, {s}, {db}, {word}, {slot}, {sentence}; " ({extra})" parts are added in code.
struct Text {
    const char* key;
    const char* en;
    const char* enMany; // nullptr = en
    const char* de;     // N_() key
    const char* deMany;
};

const Text kReasons[] = {
    {"kills", "1 kill", "{n} kills", N_("1 Abschuss"), N_("{n} Abschüsse")},
    {"loud", "1 loud moment", "{n} loud moments", N_("1 laute Stelle"), N_("{n} laute Stellen")},
    {"laughter", "laughter ({s} s)", nullptr, N_("Lachen ({s} s)"), nullptr},
    {"applause", "applause ({s} s)", nullptr, N_("Applaus ({s} s)"), nullptr},
    {"cheering", "cheering ({s} s)", nullptr, N_("Jubel ({s} s)"), nullptr},
    {"scream", "a scream ({s} s)", nullptr, N_("Schrei ({s} s)"), nullptr},
    {"motion", "lots of movement", nullptr, N_("viel Bewegung"), nullptr},
    {"scene_change", "the picture changes", nullptr, N_("Bildwechsel"), nullptr},
    {"face", "a face in the picture", nullptr, N_("Gesicht im Bild"), nullptr},
    {"pause", "pause of {s} s", nullptr, N_("Pause von {s} s"), nullptr},
    {"filler", "filler word \"{word}\"", nullptr, N_("Füllwort „{word}“"), nullptr},
    {"slot_match", "fits the field \"{slot}\"", nullptr, N_("passt ins Feld „{slot}“"), nullptr},
    {"take", "best take of sentence {sentence}", nullptr, N_("bester Take von Satz {sentence}"), nullptr},
    {"alt_take", "another take of sentence {sentence}", nullptr, N_("anderer Take von Satz {sentence}"), nullptr},
    {"fallback_end", "end of the recording (nothing found)", nullptr, N_("Ende der Aufnahme (nichts gefunden)"), nullptr},
};

const Text kNotes[] = {
    {"no_whisper", "Whisper is not installed: no captions and no filler words (load it under Help > Extensions).",
     nullptr, N_("Whisper ist nicht installiert: keine Untertitel und keine Füllwörter (unter Hilfe › Erweiterungen "
                 "nachladen)."),
     nullptr},
    {"running", "Analysis still running for {n} file(s): the suggestions use what is there so far.", nullptr,
     N_("Die Analyse läuft noch für {n} Datei(en): die Vorschläge nutzen, was schon da ist."), nullptr},
    {"no_moments", "Nothing stood out: the suggestions are the end of each recording.", nullptr,
     N_("Nichts ist aufgefallen: die Vorschläge sind das Ende jeder Aufnahme."), nullptr},
    {"fallback", "No highlight found: moments from sound events and picture changes instead.", nullptr,
     N_("Kein Highlight gefunden: stattdessen Momente aus Ton-Ereignissen und Bildwechseln."), nullptr},
    {"music_missing", "The music bed (op music_bed) is not in this build: no music added.", nullptr,
     N_("Das Musikbett (Op music_bed) fehlt in dieser Version: keine Musik eingefügt."), nullptr},
    {"not_yet", "This recipe comes in a later step.", nullptr, N_("Dieses Rezept kommt in einem späteren Schritt."),
     nullptr},
    {"few_moments",
     "Only {s} s stood out of the {target} s wanted: the end of each recording is in the list too (switched off), to "
     "fill up with.",
     nullptr,
     N_("Nur {s} s sind aufgefallen, gewünscht sind {target} s: Das Ende jeder Aufnahme steht ausgeschaltet in der "
        "Liste, zum Auffüllen."),
     nullptr},
    {"moments_grown",
     "Only {found} s stood out of the {target} s wanted: each moment shows more of its scene (up to 6 s before, 4 s "
     "after), {s} s in all.",
     nullptr,
     N_("Nur {found} s sind aufgefallen, gewünscht sind {target} s: Jeder Moment zeigt mehr von seiner Szene (bis 6 s "
        "davor, 4 s danach), zusammen {s} s."),
     nullptr},
    {"fewer_variants", "Only {n} variant(s) instead of {asked}: the material has too few moments for more.", nullptr,
     N_("Nur {n} Variante(n) statt {asked}: Für mehr hat das Material zu wenige Momente."), nullptr},
    {"no_shot", "No shot fits the field \"{slot}\".", nullptr, N_("Für das Feld „{slot}“ passt kein Clip."), nullptr},
    {"few_slot_moments",
     "Fewer moments than clip fields: {n} field(s) get the best other shot of the material (sharp, well exposed).",
     nullptr, N_("Weniger Momente als Clip-Felder: {n} Feld(er) bekommen die beste andere Einstellung des Materials."),
     nullptr},
    {"fields_kept",
     "Not filled here, the template's content stays: {fields} - give them with apply values {field: text or file}.",
     nullptr, N_("Hier nicht gefüllt, der Inhalt der Vorlage bleibt: {fields}"), nullptr},
    {"not_said", "Not found in what was said: \"{text}\"", nullptr, N_("Nicht im Gesagten gefunden: „{text}“"), nullptr},
    {"no_pause",
     "No pause long enough where every chosen track is quiet (try a shorter min_pause_s, a threshold in dBFS, or fewer "
     "tracks).",
     nullptr,
     N_("Keine Pause lang genug, in der alle gewählten Spuren still sind (kürzere Pausen versuchen)."), nullptr},
    {"never_quiet",
     "Never quiet: {tracks} - its noise stays above the threshold, so no pause is common to all tracks; give threshold "
     "(dBFS, above that floor) or leave the track out with tracks.",
     nullptr,
     N_("Nie still: {tracks} – das Rauschen liegt über der Schwelle, keine Pause ist allen Spuren gemeinsam (die Spur "
        "unter „Spuren“ weglassen)."),
     nullptr},
    {"no_transcript_fillers", "No transcript on these tracks: only pauses were found, no filler words (transcribe first).",
     nullptr,
     N_("Keine Abschrift auf diesen Spuren: nur Pausen, keine Füllwörter (erst transkribieren)."), nullptr},
    {"fillers_transcript", "Filler words: only those the transcript contains.", nullptr,
     N_("Füllwörter: nur, was die Abschrift enthält."), nullptr},
    {"room_tone", "Room tone is not laid under the cuts yet.", nullptr,
     N_("Raumton unter den Schnitten kommt noch nicht."), nullptr},
};

QString number(const QJsonValue& v, Lang lang)
{
    if (!v.isDouble()) return v.toVariant().toString();
    const double d = v.toDouble();
    QString s = std::abs(d - std::round(d)) < 1e-9 ? QString::number(qint64(std::llround(d)))
                                                   : QString::number(d, 'f', 1);
    if (lang == Lang::Ui) s.replace('.', I18n::decimalPoint());
    return s;
}

QString fill(QString text, const QJsonObject& values, Lang lang)
{
    for (auto it = values.begin(); it != values.end(); ++it) text.replace("{" + it.key() + "}", number(it.value(), lang));
    return text;
}

const Text* findIn(const Text* begin, const Text* end, const QString& key)
{
    const Text* t = std::find_if(begin, end, [&](const Text& x) { return key == QLatin1String(x.key); });
    return t == end ? nullptr : t;
}

QString textOf(const Text& t, const QJsonObject& values, Lang lang)
{
    const bool many = values.contains("n") && values["n"].toDouble() != 1;
    const char* en = many && t.enMany ? t.enMany : t.en;
    const char* de = many && t.deMany ? t.deMany : t.de;
    return fill(lang == Lang::Ui ? T(de) : QString::fromUtf8(en), values, lang);
}

double round3(double v) { return std::round(v * 1000) / 1000; }

} // namespace

const QStringList& reasonKeys()
{
    static const QStringList k = [] {
        QStringList l;
        for (const Text& t : kReasons) l << t.key;
        return l;
    }();
    return k;
}

const QStringList& noteKeys()
{
    static const QStringList k = [] {
        QStringList l;
        for (const Text& t : kNotes) l << t.key;
        return l;
    }();
    return k;
}

QString reasonText(const Reason& r, Lang lang)
{
    const Text* t = findIn(std::begin(kReasons), std::end(kReasons), r.key);
    if (!t) return r.key;
    QString s = textOf(*t, r.values, lang);
    if (r.key == QLatin1String("kills") && r.values["extra"].toString() == QLatin1String("multi"))
        s += lang == Lang::Ui ? " (" + T("Mehrfach-Abschuss") + ")" : QString(" (multi kill)");
    // What made the material index choose the shot (English, for the AI): "sharp, well exposed, face"
    if (r.key == QLatin1String("slot_match") && lang == Lang::English && !r.values["why"].toString().isEmpty())
        s += " (" + r.values["why"].toString() + ")";
    return s;
}

QString noteText(const QString& key, const QJsonObject& values, Lang lang)
{
    const Text* t = findIn(std::begin(kNotes), std::end(kNotes), key);
    return t ? textOf(*t, values, lang) : QString();
}

Note makeNote(const QString& key, const QJsonObject& values) { return {key, noteText(key, values), values}; }

const Suggestion* Session::find(int n) const
{
    for (const Suggestion& s : suggestions)
        if (s.n == n) return &s;
    return nullptr;
}

double Session::length() const
{
    double sum = 0;
    for (const Suggestion& s : suggestions)
        if (s.on) sum += std::max(0.0, s.to - s.from);
    return sum;
}

QJsonObject toJson(const Suggestion& s)
{
    QJsonObject reason{{"key", s.reason.key}, {"values", s.reason.values}, {"text", reasonText(s.reason)}};
    QJsonObject o{{"n", s.n},
                  {"kind", s.kind},
                  {"file", s.file},
                  {"stream", s.stream},
                  {"from_s", round3(s.from)},
                  {"to_s", round3(s.to)},
                  {"score", std::round(s.score * 10) / 10},
                  {"reason", reason},
                  {"labels", QJsonArray::fromStringList(s.labels)},
                  {"on", s.on}};
    if (s.variant > 0) o["variant"] = s.variant;
    if (!s.track.isEmpty()) o["track"] = s.track;
    if (!s.slot.isEmpty()) o["slot"] = s.slot;
    if (s.shot > 0) o["shot"] = s.shot;
    if (s.sentence > 0) o["sentence"] = s.sentence;
    return o;
}

QJsonObject toJson(const Material& m)
{
    QJsonObject o{{"file", m.file},       {"length_s", round3(m.length)}, {"has_video", m.hasVideo},
                  {"has_audio", m.hasAudio}, {"analysis", m.analysis}};
    if (m.size >= 0) {
        o["size"] = m.size;
        o["modified"] = m.modified;
    }
    if (m.fps > 0) o["fps"] = m.fps;
    if (m.width > 0) {
        o["width"] = m.width;
        o["height"] = m.height;
    }
    if (!m.audioStreams.isEmpty()) o["audio_streams"] = QJsonArray::fromStringList(m.audioStreams);
    o["moments"] = m.moments;
    return o;
}

QJsonObject toJson(const Session& s)
{
    QJsonArray material, suggestions, order, notes;
    for (const Material& m : s.material) material << toJson(m);
    for (const Suggestion& x : s.suggestions) suggestions << toJson(x);
    for (int n : s.order) order << n;
    for (const Note& n : s.notes) {
        QJsonObject note{{"key", n.key}, {"text", n.text}};
        if (!n.values.isEmpty()) note["values"] = n.values;
        notes << note;
    }
    QJsonObject o{{"id", s.id},           {"recipe", s.recipe},   {"made", s.made},
                  {"options", s.options}, {"material", material}, {"suggestions", suggestions},
                  {"order", order},       {"length_s", round3(s.length())}, {"notes", notes}};
    if (s.target > 0) o["target_s"] = round3(s.target);
    if (!s.project.isEmpty()) o["project"] = s.project;
    if (!s.timeline.isEmpty()) o["timeline"] = s.timeline;
    return o;
}

std::optional<Session> fromJson(const QJsonObject& o, QString* error)
{
    const auto bad = [error](const QString& why) -> std::optional<Session> {
        if (error) *error = why;
        return std::nullopt;
    };
    Session s;
    s.recipe = o["recipe"].toString();
    if (s.recipe.isEmpty()) return bad("no recipe");
    s.id = o["id"].toString();
    s.made = o["made"].toString();
    s.options = o["options"].toObject();
    s.project = o["project"].toString();
    s.timeline = o["timeline"].toString();
    s.target = o["target_s"].toDouble();
    for (const QJsonValue& v : o["material"].toArray()) {
        Material m;
        m.file = v["file"].toString();
        m.length = v["length_s"].toDouble();
        m.hasVideo = v["has_video"].toBool();
        m.hasAudio = v["has_audio"].toBool();
        m.analysis = v["analysis"].toObject();
        m.size = v["size"].toInteger(-1);
        m.modified = v["modified"].toInteger();
        m.fps = v["fps"].toDouble();
        m.width = v["width"].toInt();
        m.height = v["height"].toInt();
        for (const QJsonValue& a : v["audio_streams"].toArray()) m.audioStreams << a.toString();
        m.moments = v["moments"].toInt();
        s.material << m;
    }
    for (const QJsonValue& v : o["suggestions"].toArray()) {
        Suggestion x;
        x.n = v["n"].toInt();
        x.kind = v["kind"].toString();
        if (x.n <= 0 || x.kind.isEmpty()) return bad("a suggestion without n or kind");
        x.file = v["file"].toString();
        x.stream = v["stream"].toInt();
        x.from = v["from_s"].toDouble();
        x.to = v["to_s"].toDouble();
        x.score = v["score"].toDouble();
        x.reason = {v["reason"]["key"].toString(), v["reason"]["values"].toObject()};
        for (const QJsonValue& l : v["labels"].toArray()) x.labels << l.toString();
        x.on = v["on"].toBool(true);
        x.variant = v["variant"].toInt();
        x.track = v["track"].toString();
        x.slot = v["slot"].toString();
        x.shot = v["shot"].toInt();
        x.sentence = v["sentence"].toInt();
        s.suggestions << x;
    }
    for (const QJsonValue& v : o["order"].toArray()) s.order << v.toInt();
    for (const QJsonValue& v : o["notes"].toArray())
        s.notes << Note{v["key"].toString(), v["text"].toString(), v["values"].toObject()};
    return s;
}

QVector<int> resolvedOrder(const Session& s)
{
    QVector<int> out;
    for (int n : s.order)
        if (const Suggestion* x = s.find(n); x && x->on && !out.contains(n)) out << n;
    QVector<int> rest;
    for (const Suggestion& x : s.suggestions)
        if (x.on && !out.contains(x.n)) rest << x.n;
    std::sort(rest.begin(), rest.end());
    // A take of a script sentence switched on (another take than the suggested one) goes to its sentence's place:
    // after the takes of its own sentence, before those of later sentences - not to the end
    for (int n : rest) {
        const Suggestion* x = s.find(n);
        int at = int(out.size());
        if (x->kind == QLatin1String("take") && x->sentence > 0)
            for (int k = 0; k < out.size(); ++k)
                if (const Suggestion* y = s.find(out[k]); y->kind == QLatin1String("take") && y->sentence > x->sentence) {
                    at = k;
                    break;
                }
        out.insert(at, n);
    }
    return out;
}

} // namespace Direction
