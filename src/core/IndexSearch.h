// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Searching the material index (engine/MediaIndex) locally, without a model: the words of a question against the
// shots' descriptions, tags, speech and hearable events, plus their measured quality ("ruhig" = little motion,
// "dunkel" = dark). A small German/English synonym table bridges the two languages and the event ids ("lacht" ->
// laughter, "Applaus" -> applause), part-word matches catch compounds ("Ladentheke" for "Laden") and inflections.
// Pure functions.

#include <QString>
#include <QStringList>
#include <QVector>

namespace IndexSearch {

struct Event {
    QString kind; // AudioEvents ids: speech, music, silence, laughter, applause, cheering, shot, scream
    double from = 0, to = 0, conf = 0;
};

// One shot as the search sees it (seconds in its file)
struct Shot {
    QString file;
    int n = 0; // 1-based
    double from = 0, to = 0;
    QString desc;
    QStringList tags;
    QString speech;
    QVector<Event> events;
    double motion = 0;     // mean change between pictures (MediaIndex::Quality::motion); < 0 = no picture
    bool dark = false, overexposed = false, blurry = false, sharp = false, pan = false, face = false;
};

struct Hit {
    QString file;
    int n = 0;
    double from = 0, to = 0; // the shot, or the event that matched best
    double score = 0;
    QString why; // "event laughter 0.80", "tag 'laden'", "desc 'totale'", "speech 'laden'", "quality calm"
};

// Lower case, umlauts folded (ä -> a, ß -> ss), split into words; filler words of a question ("wo", "der", "the")
// dropped
QStringList words(const QString& text);
QStringList queryWords(const QString& query);

// Best first; at most `top` (0 = all). Shots matching more of the question's words come first.
QVector<Hit> search(const QString& query, const QVector<Shot>& shots, int top = 10);

} // namespace IndexSearch
