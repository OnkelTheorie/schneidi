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

#include <cmath>
#include <cstdint>

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
        const double got = mixLufs(p);
        CHECK(notes.contains(QString("now %1 LUFS").arg(got, 0, 'f', 1)));
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
        const double got = mixLufs(p);
        if (!CHECK(notes.contains("not reached") && notes.contains("allow_track_gain")
                   && notes.contains(QString("%1 LUFS measured after the master limiter").arg(got, 0, 'f', 1))))
            std::printf("       mix %.2f, %s\n", got, qPrintable(notes));
    }
    return Check::result();
}
