// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Review render (0.17, `render review:"video"|"sheet"`): what an AI (or a person) needs to see to judge an edit
// without guessing - the beat grid of the music, cuts with clip names, hits and kill feed lines of the recordings,
// accents, open notes, markers and story sections - and the numbers behind it (a hit next to the nearest beat, cuts
// that miss the beat). Pure data and drawing; the caller (cli/RenderReview.cpp) collects the marks from the timeline
// and the analyses, engine/picture/ReviewOverlay burns them into each frame of a review video, sheet() draws the whole
// timeline as one picture. Nothing here touches the project: the marks only exist in the review render.

#include "core/ProjectFormat.h"

#include <QColor>
#include <QImage>
#include <QPainter>
#include <QRectF>
#include <QString>
#include <QVector>

#include <vector>

namespace ReviewSheet {

enum class MarkKind { Cut, Hit, Kill, Accent, Note, Marker };
struct Mark {
    MarkKind kind = MarkKind::Cut;
    int frame = 0;  // timeline frame
    QString label;  // clip name (cut, hit, kill, accent), note text, empty
    int clip = 0;   // clip id it belongs to (0 = none)
};

struct Section {
    QString kind; // hook, build, question, turn, resolution, cta, free (core/TimelineStory)
    QString name;
    int from = 0, to = 0; // timeline frames, [from, to)
};

// Loudest sample of an audio track's sound, kWaveRate values per second of the timeline (0..1)
constexpr int kWaveRate = 50;
struct Wave {
    QString track; // "A1"
    std::vector<float> peak;
};

struct Data {
    ProjectFormat format;
    int length = 0; // frames of the timeline (or the reviewed range's end)
    QString title;  // timeline name
    // Beat grid of the music track (timeline seconds), bar[k] = beat k starts a bar
    QString beatTrack; // "A2"; empty = no beat grid
    double bpm = 0;
    std::vector<double> beats;
    std::vector<char> bar;
    QVector<Mark> marks;       // sorted by frame
    QVector<Section> sections; // sorted by from
    QVector<Wave> waves;       // one per audio track looked at
    int musicWave = -1;        // index in waves of the beat track (drawn under the review video's strip)

    double fps() const { return format.rate.fps() > 0 ? format.rate.fps() : 25.0; }
    void sort(); // marks by frame (kind order for the same frame), sections by from
};

// Index of the beat nearest to `s` (seconds), -1 without beats
int nearestBeat(const Data& d, double s);

// A hit (or a cut) next to the nearest beat: off_ms > 0 = after the beat
struct Offset {
    int frame = 0;
    double at = 0;   // seconds (frame / fps)
    double beat = 0; // seconds of the nearest beat
    int offMs = 0;
    int clip = 0;
    QString label;
};
// Every mark of `kind` within the beat grid (between the first beat - one period and the last + one period)
QVector<Offset> offsets(const Data& d, MarkKind kind = MarkKind::Hit);
// Cuts that miss their nearest beat by more than half a frame (a frame closer to the beat exists)
QVector<Offset> cutsOffBeat(const Data& d);

// ---- The review video's lamps: a row of boxes that light up on the frame an event falls on

enum class Lamp { Beat, Bar, Cut, Hit, Kill, Count };
QColor lampColor(Lamp l);
QString lampName(Lamp l); // shown under the box
// How many frames a lamp stays lit from the event's frame on (about 0.1 s, at least 1)
int lampHold(double fps);
// Is the lamp lit at timeline frame `frame`? (the event's frame and the lampHold() - 1 after it)
bool lampLit(const Data& d, Lamp l, int frame);
// Where the lamp is drawn in a frame of `size` (top right, left to right in Lamp order)
QRect lampRect(Lamp l, QSize size);
// The strip at the bottom of a review frame (timeline window around the playhead)
QRect stripRect(QSize size);
// Seconds before and after the playhead the strip shows
constexpr double kStripSpan = 2.0;

QColor markColor(MarkKind k);
QColor sectionColor(const QString& kind);

// Draws the review overlay of timeline frame `frame` onto `p` (a frame of `size`): info box top left (timecode,
// frame number, section), the lamps, the strip (music waveform, beat grid with thicker bar lines, cuts with clip
// names, hits, kill feed lines, accents, notes, markers, sections, the playhead in the middle)
void drawOverlay(QPainter& p, QSize size, const Data& d, int frame);

// ---- The review sheet: one picture of the whole timeline

// One picture per shot (the stretch between two cuts) for the sheet: rendered by the caller at shotFrame()
struct Tile {
    int from = 0, to = 0; // shot [from, to)
    int frame = 0;        // the frame shown (middle of the shot)
    QString clip;         // top clip's name there
    QImage image;
    QRect rect;           // where sheet() put it (filled in by sheet())
};
// The shots of the timeline: [0, cut1), [cut1, cut2) … at most maxTiles (evenly picked when there are more)
QVector<Tile> shots(const Data& d, int maxTiles = 24);
constexpr int kSheetWidth = 1600;
constexpr int kSheetColumns = 6;
// The sheet: header, the shots as tiles (label: timecode + clip, count of hits/kills in the shot), then the timeline
// in rows of up to 20 s (more for long timelines, at most 15 rows): ruler, sections, marks over the beat grid and one
// waveform per audio track with the beat grid behind it. tiles: their rect is set.
QImage sheet(const Data& d, QVector<Tile>& tiles);

} // namespace ReviewSheet
