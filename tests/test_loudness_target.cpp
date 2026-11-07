// SPDX-License-Identifier: AGPL-3.0-only
// Loudness targets with the master limiter on, on peaky game-like sound (synthetic: quiet noise floor and sparse
// shots up to 0 dBFS): `mixer` target_lufs and `normalize` mode lufs reach -14 LUFS within 0.5 LU after the limiter
// (an AI test run 0.19 stopped at -15.7 LUFS: the gains were capped at +12 dB and the limiter ate the rest; the AI
// then built compressor loops by hand), and say so with how far they got when the target cannot be reached.
#include "cli/Commands.h"

#include "check.h"

#include <QCoreApplication>
#include <QDataStream>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <QRegularExpression>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace {

const QJsonObject run(const QString& name, const QJsonObject& raw)
{
    const Cli::Command* cmd = Cli::find(name);
    Cli::Context ctx;
    try {
        return cmd->run(Cli::normalize(*cmd, raw), ctx);
    } catch (const Cli::Error& e) {
        qWarning().noquote() << "unexpected error" << e.code << e.message;
        throw;
    }
}

// 30 s stereo 48 kHz 16 bit: white noise at about -35 dBFS RMS, every 6 s a shot (noise burst at 0 dBFS, 2 ms decay)
// - about -27 LUFS with peaks at 0 dBFS, so -14 needs more than the +12 dB of one fader and the limiter works hard
bool writePeaky(const QString& path)
{
    constexpr int rate = 48000, seconds = 30, every = 6 * rate;
    constexpr double tau = 0.002 * rate;
    const double floor = std::pow(10.0, -35.0 / 20) * std::sqrt(3.0);
    const int n = rate * seconds;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    QDataStream out(&f);
    out.setByteOrder(QDataStream::LittleEndian);
    const quint32 data = quint32(n) * 4;
    out.writeRawData("RIFF", 4);
    out << quint32(36 + data);
    out.writeRawData("WAVEfmt ", 8);
    out << quint32(16) << quint16(1) << quint16(2) << quint32(rate) << quint32(rate * 4) << quint16(4) << quint16(16);
    out.writeRawData("data", 4);
    out << data;
    uint32_t seed = 3;
    const auto random = [&seed] { // -1..1, deterministic
        seed = seed * 1664525u + 1013904223u;
        return double(seed >> 8) / double(1u << 23) - 1.0;
    };
    for (int i = 0; i < n; ++i) {
        double v = random() * floor;
        if (i >= every / 2) {
            const int k = (i - every / 2) % every;
            if (k < 8 * tau) v += (random() < 0 ? -1.0 : 1.0) * std::exp(-k / tau);
        }
        const qint16 sample = qint16(std::lround(std::clamp(v, -1.0, 1.0) * 32767));
        out << sample << sample;
    }
    return out.status() == QDataStream::Ok;
}

double mixLufs(const QString& project)
{
    return run("loudness", {{"source", project}})["loudness"]["integrated_lufs"].toDouble();
}

QString notesOf(const QJsonObject& r) { return QString::fromUtf8(QJsonDocument(r["notes"].toArray()).toJson()); }

// The first number before `after` in the notes (e.g. "-16.9" of "-16.9 LUFS measured"); nan = none
double numberBefore(const QString& notes, const QString& after)
{
    const QRegularExpressionMatch m = QRegularExpression("(-?\\d+(?:\\.\\d+)?) " + QRegularExpression::escape(after)).match(notes);
    return m.hasMatch() ? m.captured(1).toDouble() : std::nan("");
}

// track -> volume_db of the track (0 when not shown); clip id -> its volume_db
QHash<QString, double> trackVolumes(const QString& project, QHash<int, double>* clips = nullptr)
{
    QHash<QString, double> out;
    for (const QJsonValue& t : run("info", {{"project", project}})["timeline"]["tracks"].toArray()) {
        out[t["track"].toString()] = t["volume_db"].toDouble();
        if (clips)
            for (const QJsonValue& c : t["clips"].toArray()) (*clips)[c["id"].toInt()] = c["volume_db"].toDouble();
    }
    return out;
}

// A new project with the file on an audio track (unmuted) and the master limiter on; the clip's id
int project(const QString& path, const QString& wav)
{
    run("new", {{"project", path}, {"overwrite", true}});
    run("edit", {{"project", path}, {"ops", QString(R"([{"op":"append","media":"%1"}])").arg(wav)}});
    int id = 0;
    QString track;
    for (const QJsonValue& t : run("info", {{"project", path}})["timeline"]["tracks"].toArray())
        if (t["track"].toString().startsWith('A') && !t["clips"].toArray().isEmpty())
            id = t["clips"][0]["id"].toInt(), track = t["track"].toString();
    if (id)
        run("edit", {{"project", path},
                     {"ops", QString(R"([{"op":"mixer","track":"%1","mute":false},{"op":"mixer","track":"master","limiter":true}])").arg(track)}});
    return id;
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QCoreApplication app(argc, argv);
    Check::initApp("loudness_target");
    Check::initEngine();
    QTemporaryDir dir;
    const QString wav = dir.filePath("peaky.wav");
    if (!CHECK(writePeaky(wav))) return Check::result();
    const double file = run("loudness", {{"source", wav}})["loudness"]["integrated_lufs"].toDouble();
    if (!CHECK(file < -24 && file > -32)) std::printf("       file %.1f LUFS\n", file);

    const QString p = dir.filePath("mix.schneidi");
    // The master to -14 with the limiter on: reached after the limiter, beyond the master's +12 dB on the track
    {
        if (!CHECK(project(p, wav))) return Check::result();
        const QJsonObject r = run("edit", {{"project", p}, {"ops", R"([{"op":"mixer","track":"master","target_lufs":-14}])"}});
        const double got = mixLufs(p);
        if (!CHECK(std::abs(got + 14) <= 0.5)) std::printf("       mix %.2f LUFS, notes %s\n", got, qPrintable(notesOf(r)));
        CHECK(notesOf(r).contains("after the master limiter"));
        CHECK(!notesOf(r).contains("not reachable"));
        const QJsonObject master = run("info", {{"project", p}})["timeline"]["master"].toObject();
        CHECK(master["limiter"].toBool());
        CHECK(master["volume_db"].toDouble() <= 12.0);
    }
    // Lower than now works too (the fader goes down), quickly
    {
        project(p, wav);
        run("edit", {{"project", p}, {"ops", R"([{"op":"mixer","track":"master","target_lufs":-20}])"}});
        CHECK(std::abs(mixLufs(p) + 20) <= 0.5);
    }
    // Not reachable (0 LUFS): says so and how far it got, the number is what the mix measures
    {
        project(p, wav);
        const QJsonObject r = run("edit", {{"project", p}, {"ops", R"([{"op":"mixer","track":"master","target_lufs":0}])"}});
        const QString notes = notesOf(r);
        if (!CHECK(notes.contains("not reachable") && notes.contains("LU short"))) std::printf("       %s\n", qPrintable(notes));
        const double got = mixLufs(p), said = numberBefore(notes, "LUFS (target");
        if (!CHECK(std::abs(got - said) <= 0.3)) std::printf("       mix %.2f, notes say %.2f\n", got, said);
    }

    // normalize lufs -14 with the limiter on
    {
        const int clip = project(p, wav);
        const QJsonObject r = run("edit", {{"project", p}, {"ops", QString(R"([{"op":"normalize","clips":[%1],"mode":"lufs","target":-14,"allow_track_gain":true}])").arg(clip)}});
        const double got = mixLufs(p);
        if (!CHECK(std::abs(got + 14) <= 0.5)) std::printf("       mix %.2f LUFS, notes %s\n", got, qPrintable(notesOf(r)));
        CHECK(notesOf(r).contains("after the master limiter"));
        CHECK(!notesOf(r).contains("turn on the master limiter")); // it is on
    }
    {
        const int clip = project(p, wav);
        run("edit", {{"project", p}, {"ops", QString(R"([{"op":"normalize","clips":[%1],"mode":"lufs","target":-20}])").arg(clip)}});
        CHECK(std::abs(mixLufs(p) + 20) <= 0.5);
    }
    // Beyond the clip volume's +12 dB without allow_track_gain: not reached, the answer says how far and what helps
    {
        const int clip = project(p, wav);
        const QJsonObject r = run("edit", {{"project", p}, {"ops", QString(R"([{"op":"normalize","clips":[%1],"mode":"lufs","target":-14}])").arg(clip)}});
        const QString notes = notesOf(r);
        const double got = mixLufs(p), said = numberBefore(notes, "LUFS measured after the master limiter");
        if (!CHECK(notes.contains("not reached") && notes.contains("allow_track_gain") && std::abs(got - said) <= 0.3))
            std::printf("       mix %.2f, %s\n", got, qPrintable(notes));
    }
    // The clip volume normalize gives one clip to -20 (the reference for the cases below)
    QHash<int, double> volumes;
    double single = 0;
    {
        const int clip = project(p, wav);
        run("edit", {{"project", p}, {"ops", QString(R"([{"op":"normalize","clips":[%1],"mode":"lufs","target":-20}])").arg(clip)}});
        trackVolumes(p, &volumes);
        single = volumes.value(clip);
        CHECK(single > 7 && single < 12);
    }
    // After the master went to -14 (master +12, the track up too), normalize aims at the clip's own target as without
    // a limiter (faders counted as 0 dB): the master stays, the track gets only what the clip needs beyond +12 dB
    // (review: it aimed at -14 + 15 dB of faders, pushed clip and track to +12 and lowered the track's other clips)
    {
        const int clip = project(p, wav);
        run("edit", {{"project", p}, {"ops", R"([{"op":"mixer","track":"master","target_lufs":-14}])"}});
        const QHash<QString, double> tracksBefore = trackVolumes(p);
        const double masterBefore = run("info", {{"project", p}})["timeline"]["master"]["volume_db"].toDouble();
        const QJsonObject r = run("edit", {{"project", p}, {"ops", QString(R"([{"op":"normalize","clips":[%1],"mode":"lufs","target":-14,"allow_track_gain":true}])").arg(clip)}});
        const QHash<QString, double> tracksAfter = trackVolumes(p);
        CHECK_EQ(run("info", {{"project", p}})["timeline"]["master"]["volume_db"].toDouble(), masterBefore);
        const double added = tracksAfter.value("A1") - tracksBefore.value("A1");
        if (!CHECK(added > 1.5 && added < 4.5)) // the clip alone needs about +3 dB beyond its +12 (see above)
            std::printf("       A1 %.2f -> %.2f dB, %s\n", tracksBefore.value("A1"), tracksAfter.value("A1"), qPrintable(notesOf(r)));
        CHECK(!notesOf(r).contains("not reached"));
    }
    // relative: two clips at the same time on A1 and A2 (A2's fader -6) are measured each alone, together as one
    // (as without a limiter), not summed: both get the volume one clip alone gets
    {
        const int clip = project(p, wav);
        const QString copy = dir.filePath("peaky2.wav"); // the same file at the same time would be a disabled take
        QFile::copy(wav, copy);
        run("edit", {{"project", p}, {"ops", QString(R"([{"op":"place","media":"%1","at":0,"track":"A2"},{"op":"mixer","track":"A2","volume_db":-6,"mute":false}])").arg(copy)}});
        volumes.clear();
        trackVolumes(p, &volumes);
        int other = 0;
        for (auto it = volumes.begin(); it != volumes.end(); ++it)
            if (it.key() != clip) other = it.key();
        const QJsonObject r = run("edit", {{"project", p}, {"ops", QString(R"([{"op":"normalize","clips":[%1,%2],"mode":"lufs","target":-20,"relative":true}])").arg(clip).arg(other)}});
        volumes.clear();
        trackVolumes(p, &volumes);
        if (!CHECK(std::abs(volumes.value(clip) - single) <= 0.5 && std::abs(volumes.value(other) - single) <= 0.5))
            std::printf("       %.2f / %.2f dB, alone %.2f dB, %s\n", volumes.value(clip), volumes.value(other), single,
                        qPrintable(notesOf(r)));
    }
    // A clip on a muted track is measured too (unmuted in the measurement), not skipped silently
    {
        const int clip = project(p, wav);
        run("edit", {{"project", p}, {"ops", R"([{"op":"mixer","track":"A1","mute":true}])"}});
        const QJsonObject r = run("edit", {{"project", p}, {"ops", QString(R"([{"op":"normalize","clips":[%1],"mode":"lufs","target":-20}])").arg(clip)}});
        CHECK(notesOf(r).contains(QString("clip %1: ").arg(clip)));
        volumes.clear();
        trackVolumes(p, &volumes);
        CHECK(std::abs(volumes.value(clip) - single) <= 0.5);
    }
    return Check::result();
}
