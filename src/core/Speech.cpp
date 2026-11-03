// SPDX-License-Identifier: AGPL-3.0-only
#include "core/Speech.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>

namespace Speech {

namespace {
QString number(double v) { return QString::number(v, 'f', 3); } // C locale: the same key everywhere
} // namespace

namespace {
// Emoji and other pictographs: espeak reads some of them by their name ("Gesicht mit Freudentränen"), others not at
// all; neither belongs in a voice-over or its captions. Signs below the arrows block (°, ©, €) stay.
bool isPictograph(char32_t c)
{
    if (c == 0x200D || c == 0xFE0E || c == 0xFE0F || c == 0x20E3) return true; // joiner, variation selectors, keycap
    if (c >= 0x1F3FB && c <= 0x1F3FF) return true;                           // skin tones
    if (c >= 0xE0020 && c <= 0xE007F) return true;                           // tag characters (flags)
    return c >= 0x2190 && QChar::category(c) == QChar::Symbol_Other;
}
} // namespace

QString normalizedText(const QString& text)
{
    // One line per text: piper speaks each line as its own utterance into the same file. Line breaks of a list or
    // between paragraphs end a sentence (else "- Tempo\n- Preis" runs together as "Tempo Preis"); a line break inside
    // a sentence (wrapped text) is a space. List markers are not spoken.
    static const QRegularExpression marker(QStringLiteral("^[-*•–—·]\\s+"));
    struct Line {
        QString text;
        bool item = false, paragraphEnd = false;
    };
    QVector<Line> lines;
    for (const QString& raw : text.split('\n')) {
        QString clean;
        for (const char32_t c : raw.toUcs4())
            if (!isPictograph(c)) clean += QString::fromUcs4(&c, 1);
        Line l{clean.simplified()};
        if (l.text.contains(marker)) {
            l.text = l.text.remove(marker).trimmed();
            l.item = true;
        }
        if (l.text.isEmpty()) {
            if (!lines.isEmpty()) lines.last().paragraphEnd = true;
            continue;
        }
        if (l.item && !lines.isEmpty()) lines.last().paragraphEnd = true; // the line before a list item ends too
        lines << l;
    }
    QStringList out;
    for (int i = 0; i < lines.size(); ++i) {
        QString t = lines[i].text;
        static const QString ends = QStringLiteral(".!?…:;,");
        const bool last = i + 1 == lines.size();
        if (!last && (lines[i].item || lines[i].paragraphEnd) && !ends.contains(t.back())) t += '.';
        out << t;
    }
    return out.join(' ');
}

QString cacheKey(const QString& text, const Options& o)
{
    // v1: piper 2023.11.14-2; a new piper build (other sound) gets a new version here
    const QByteArray data = QStringList{"v1", o.voice, o.voiceVersion, number(std::clamp(o.speed, kMinSpeed, kMaxSpeed)),
                                        number(std::clamp(o.pause, 0.0, kMaxPause)), normalizedText(text)}
                                .join('\n')
                                .toUtf8();
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex().left(16));
}

QString fileName(const QString& text, const Options& options)
{
    // The first words as a hint for people looking into the folder; letters outside ASCII become their base letter
    // ("Grüße" -> "gruse") or are left out
    const QString plain = normalizedText(text).normalized(QString::NormalizationForm_KD).toLower();
    QString slug;
    for (const QChar c : plain) {
        if (c.isLetterOrNumber() && c.unicode() < 128) slug += c;
        else if (c.isSpace() && !slug.endsWith('-')) slug += '-';
        if (slug.size() >= 24) break;
    }
    while (slug.endsWith('-')) slug.chop(1);
    return "speech-" + (slug.isEmpty() ? QString() : slug + "-") + cacheKey(text, options) + ".wav";
}

QString folder(const QString& projectPath)
{
    if (projectPath.isEmpty() || projectPath.startsWith('@'))
        return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath("tts");
    const QFileInfo fi(projectPath);
    return fi.absoluteDir().filePath(fi.completeBaseName() + "-tts");
}

QStringList piperArguments(const QString& model, const Options& o, const QString& outFile, const QString& config)
{
    QStringList args{"--model", model};
    if (!config.isEmpty()) args << "--config" << config;
    return args << "--output_file" << outFile << "--length_scale" << number(1.0 / std::clamp(o.speed, kMinSpeed, kMaxSpeed))
                << "--sentence_silence" << number(std::clamp(o.pause, 0.0, kMaxPause)) << "--quiet";
}

QByteArray stdinText(const QString& text) { return normalizedText(text).toUtf8() + '\n'; }

QString voiceLanguage(const QString& voice)
{
    static const QRegularExpression re("(?:^|[/\\\\-])([a-z]{2,3})_[A-Z]{2}-");
    const QString name = QFileInfo(voice).fileName();
    const QRegularExpressionMatch m = re.match(name.startsWith("piper-voice-") ? name.mid(12) : name);
    return m.hasMatch() ? m.captured(1) : QString();
}

QVector<TranscriptWord> estimateWords(const QString& text, qint64 from, qint64 to, qint64 pauseMs)
{
    const QStringList words = normalizedText(text).split(' ', Qt::SkipEmptyParts);
    QVector<TranscriptWord> out;
    if (words.isEmpty() || to <= from) return out;
    // Weight of each word: its letters (at least 2: short words still take time) plus the pause after a sentence
    // end, measured in characters at the pace of the whole text
    QVector<double> chars;
    QVector<bool> sentenceEnd;
    double total = 0;
    for (const QString& w : words) {
        int n = 0;
        for (const QChar c : w) n += c.isLetterOrNumber() ? 1 : 0;
        chars << std::max(2, n);
        total += chars.last();
        const QChar last = w.back();
        sentenceEnd << (last == '.' || last == '!' || last == '?' || last == ':' || last == ';');
    }
    int pauses = 0;
    for (int i = 0; i + 1 < words.size(); ++i) pauses += sentenceEnd[i] ? 1 : 0;
    const qint64 span = to - from;
    const qint64 pauseTotal = std::min<qint64>(span / 2, pauseMs * pauses); // never more than half the time
    const double msPerChar = (span - pauseTotal) / total;
    const double pause = pauses ? double(pauseTotal) / pauses : 0;
    double t = from;
    for (int i = 0; i < words.size(); ++i) {
        TranscriptWord w;
        w.from = qint64(std::llround(t));
        t += chars[i] * msPerChar;
        if (i + 1 < words.size() && sentenceEnd[i]) t += pause;
        w.to = i + 1 < words.size() ? qint64(std::llround(t)) : to;
        w.text = words[i];
        out << w;
    }
    return out;
}

int freeAudioTrack(const Timeline& tl, int from, int to, int preferred)
{
    const auto isFree = [&](int i) {
        if (i < 0 || i >= tl.audio.size()) return false;
        const Track& t = tl.audio[i];
        if (t.locked) return false;
        return std::none_of(t.clips.begin(), t.clips.end(), [&](const Clip& c) { return c.start < to && c.end() > from; });
    };
    if (preferred >= 0 && (isFree(preferred) || preferred >= tl.audio.size())) return preferred;
    for (int i = 0; i < tl.audio.size(); ++i)
        if (isFree(i)) return i;
    return int(tl.audio.size());
}

} // namespace Speech
