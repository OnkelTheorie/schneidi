// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// A fake piper for tests: the test program itself, started with SCHNEIDI_PIPER = its own path and SCHNEIDI_FAKE_PIPER =
// a log file, answers like piper with a sine WAV (main(): `if (qEnvironmentVariableIsSet("SCHNEIDI_FAKE_PIPER"))
// return fakePiper(...)` before anything else). No network, no real piper.

#include <QByteArray>
#include <QFile>
#include <QString>
#include <QStringList>
#include <QThread>

#include <cmath>
#include <cstdio>

namespace {

void writeWav(const QString& path, int rate, double silence, double tone)
{
    QByteArray pcm;
    const auto add = [&](double seconds, bool sound) {
        const int n = int(seconds * rate);
        for (int i = 0; i < n; ++i) {
            const qint16 v = sound ? qint16(16000 * std::sin(2 * 3.14159265358979323846 * 440 * i / rate)) : 0;
            pcm.append(char(v & 0xff)).append(char((v >> 8) & 0xff));
        }
    };
    add(silence, false);
    add(tone, true);
    add(silence, false);
    QByteArray h;
    const auto u32 = [&](quint32 v) { for (int i = 0; i < 4; ++i) h.append(char((v >> (8 * i)) & 0xff)); };
    const auto u16 = [&](quint16 v) { h.append(char(v & 0xff)).append(char(v >> 8)); };
    h += "RIFF";
    u32(quint32(36 + pcm.size()));
    h += "WAVEfmt ";
    u32(16);
    u16(1);
    u16(1);
    u32(quint32(rate));
    u32(quint32(rate * 2));
    u16(2);
    u16(16);
    h += "data";
    u32(quint32(pcm.size()));
    QFile f(path);
    if (f.open(QIODevice::WriteOnly)) f.write(h + pcm);
}

// Like piper: --output_file, --length_scale; 60 ms per character of the text (stdin) at length_scale 1. Appends its
// arguments and the text to the log the variable names.
int fakePiper(const QStringList& args)
{
    QString out;
    double scale = 1;
    for (int i = 1; i + 1 < args.size(); ++i) {
        if (args[i] == "--output_file") out = args[i + 1];
        if (args[i] == "--length_scale") scale = args[i + 1].toDouble();
    }
    QFile in;
    if (!in.open(stdin, QIODevice::ReadOnly)) return 2;
    const QString text = QString::fromUtf8(in.readAll());
    QFile log(qEnvironmentVariable("SCHNEIDI_FAKE_PIPER"));
    if (log.open(QIODevice::Append)) log.write((args.mid(1).join('|') + "\n" + text).toUtf8());
    if (out.isEmpty() || text.trimmed() == "fail") return 1;
    if (text.trimmed() == "hang") QThread::sleep(60); // a piper that never answers
    writeWav(out, 22050, 0.2, 0.06 * text.trimmed().size() * scale);
    return 0;
}

} // namespace
