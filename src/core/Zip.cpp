// SPDX-License-Identifier: AGPL-3.0-only
// ZIP writing (stored) and reading (stored, deflate per RFC 1951) - see core/Zip.h
#include "core/Zip.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>

#include <algorithm>
#include <array>
#include <cstring>

namespace Zip {

namespace {

constexpr quint32 kLocalSig = 0x04034b50, kCentralSig = 0x02014b50, kEndSig = 0x06054b50;
constexpr quint16 kUtf8 = 1u << 11;
constexpr qint64 kChunk = 1 << 16;

bool failWith(QString* error, const QString& why)
{
    if (error) *error = why;
    return false;
}

const std::array<quint32, 256>& crcTable()
{
    static const std::array<quint32, 256> table = [] {
        std::array<quint32, 256> t{};
        for (quint32 i = 0; i < 256; ++i) {
            quint32 c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    return table;
}

// Running CRC-32 (start with 0)
quint32 crc32(quint32 crc, const char* data, qint64 n)
{
    const auto& t = crcTable();
    crc = ~crc;
    for (qint64 i = 0; i < n; ++i) crc = t[(crc ^ quint8(data[i])) & 0xff] ^ (crc >> 8);
    return ~crc;
}

void put16(QByteArray& b, quint16 v) { b.append(char(v & 0xff)).append(char(v >> 8)); }
void put32(QByteArray& b, quint32 v)
{
    for (int i = 0; i < 4; ++i) b.append(char((v >> (8 * i)) & 0xff));
}
quint16 get16(const uchar* p) { return quint16(p[0] | (p[1] << 8)); }
quint32 get32(const uchar* p) { return quint32(p[0]) | (quint32(p[1]) << 8) | (quint32(p[2]) << 16) | (quint32(p[3]) << 24); }

// MS-DOS time and date of a file's modification
void dosTime(const QDateTime& when, quint16* time, quint16* date)
{
    const QDateTime t = when.isValid() && when.date().year() >= 1980 ? when.toLocalTime() : QDateTime(QDate(1980, 1, 1), QTime(0, 0));
    *time = quint16((t.time().hour() << 11) | (t.time().minute() << 5) | (t.time().second() / 2));
    *date = quint16(((t.date().year() - 1980) << 9) | (t.date().month() << 5) | t.date().day());
}

// ---------- Inflate (RFC 1951): canonical Huffman codes decoded bit by bit, counts per length ----------

struct Huffman {
    std::array<quint16, 16> count{};
    std::array<quint16, 320> symbol{};
};

// false = the lengths over-subscribe the code (broken data); incomplete codes are allowed (a single distance code)
bool build(Huffman& h, const quint8* lengths, int n)
{
    h.count.fill(0);
    for (int i = 0; i < n; ++i) ++h.count[lengths[i]];
    h.count[0] = 0;
    int left = 1;
    for (int len = 1; len < 16; ++len) {
        left = (left << 1) - h.count[len];
        if (left < 0) return false;
    }
    std::array<quint16, 16> offs{};
    for (int len = 1; len < 15; ++len) offs[len + 1] = quint16(offs[len] + h.count[len]);
    for (int i = 0; i < n; ++i)
        if (lengths[i]) h.symbol[offs[lengths[i]]++] = quint16(i);
    return true;
}

class Inflater {
public:
    Inflater(QIODevice* in, quint64 inSize, QIODevice* out, quint64 outSize)
        : m_in(in), m_inLeft(inSize), m_out(out), m_outSize(outSize)
    {
    }

    // Empty = ok
    QString run()
    {
        quint32 last = 0, type = 0;
        do {
            if (!bits(1, &last) || !bits(2, &type)) return "truncated data";
            QString err;
            if (type == 0) err = stored();
            else if (type == 1) err = fixed();
            else if (type == 2) err = dynamic();
            else err = "bad block type";
            if (!err.isEmpty()) return err;
        } while (!last);
        if (!flush(true)) return "cannot write";
        if (m_written != m_outSize) return "wrong size";
        return {};
    }
    quint32 crc() const { return m_crc; }

private:
    QIODevice* m_in;
    quint64 m_inLeft;
    QIODevice* m_out;
    quint64 m_outSize;
    QByteArray m_buf; // input chunk
    qsizetype m_pos = 0;
    quint32 m_bitBuf = 0;
    int m_bitCount = 0;
    QByteArray m_window; // output not written yet plus the last 32 KiB (back references)
    qsizetype m_flushed = 0; // bytes at the start of m_window that were written already
    quint64 m_written = 0;
    quint32 m_crc = 0;

    bool byte(quint8* b)
    {
        if (m_pos >= m_buf.size()) {
            if (m_inLeft == 0) return false;
            m_buf = m_in->read(qint64(std::min<quint64>(m_inLeft, kChunk)));
            if (m_buf.isEmpty()) return false;
            m_inLeft -= quint64(m_buf.size());
            m_pos = 0;
        }
        *b = quint8(m_buf[m_pos++]);
        return true;
    }
    bool bits(int n, quint32* v)
    {
        while (m_bitCount < n) {
            quint8 b = 0;
            if (!byte(&b)) return false;
            m_bitBuf |= quint32(b) << m_bitCount;
            m_bitCount += 8;
        }
        *v = m_bitBuf & ((1u << n) - 1);
        m_bitBuf >>= n;
        m_bitCount -= n;
        return true;
    }
    // Writes what is not needed for back references any more (all at the end)
    bool flush(bool all)
    {
        const qsizetype keep = all ? 0 : 32768;
        const qsizetype pending = m_window.size() - m_flushed;
        if (!all && m_window.size() < (1 << 20) + keep) return true;
        if (pending > 0) {
            if (m_out->write(m_window.constData() + m_flushed, pending) != pending) return false;
            m_crc = crc32(m_crc, m_window.constData() + m_flushed, pending);
            m_written += quint64(pending);
        }
        const qsizetype drop = std::max<qsizetype>(0, m_window.size() - keep);
        m_window.remove(0, drop);
        m_flushed = m_window.size();
        return true;
    }
    bool output(char c)
    {
        if (m_written + quint64(m_window.size() - m_flushed) >= m_outSize) return false; // more than announced
        m_window.append(c);
        return flush(false);
    }
    int decode(const Huffman& h)
    {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; ++len) {
            quint32 b = 0;
            if (!bits(1, &b)) return -1;
            code |= int(b);
            const int count = h.count[len];
            if (code - count < first) return h.symbol[index + (code - first)];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        return -1;
    }
    QString stored()
    {
        m_bitBuf = 0; // to the byte boundary
        m_bitCount = 0;
        quint8 b[4];
        for (quint8& x : b)
            if (!byte(&x)) return "truncated data";
        const quint16 len = quint16(b[0] | (b[1] << 8)), nlen = quint16(b[2] | (b[3] << 8));
        if (len != quint16(~nlen)) return "bad stored block";
        for (int i = 0; i < len; ++i) {
            quint8 c = 0;
            if (!byte(&c)) return "truncated data";
            if (!output(char(c))) return "more data than announced";
        }
        return {};
    }
    QString codes(const Huffman& lit, const Huffman& dist)
    {
        static const quint16 lbase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                          31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static const quint8 lextra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static const quint16 dbase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                          193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static const quint8 dextra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        for (;;) {
            const int sym = decode(lit);
            if (sym < 0) return "bad code";
            if (sym < 256) {
                if (!output(char(sym))) return "more data than announced";
                continue;
            }
            if (sym == 256) return {};
            const int l = sym - 257;
            if (l >= 29) return "bad length";
            quint32 extra = 0;
            if (!bits(lextra[l], &extra)) return "truncated data";
            const int length = lbase[l] + int(extra);
            const int d = decode(dist);
            if (d < 0 || d >= 30) return "bad distance";
            if (!bits(dextra[d], &extra)) return "truncated data";
            const qsizetype distance = dbase[d] + qsizetype(extra);
            if (distance > m_window.size()) return "distance too far back";
            for (int i = 0; i < length; ++i)
                if (!output(m_window[m_window.size() - distance])) return "more data than announced";
        }
    }
    QString fixed()
    {
        static const std::pair<Huffman, Huffman> tables = [] {
            quint8 lengths[288 + 30];
            for (int i = 0; i < 144; ++i) lengths[i] = 8;
            for (int i = 144; i < 256; ++i) lengths[i] = 9;
            for (int i = 256; i < 280; ++i) lengths[i] = 7;
            for (int i = 280; i < 288; ++i) lengths[i] = 8;
            for (int i = 0; i < 30; ++i) lengths[288 + i] = 5;
            Huffman lit, dist;
            build(lit, lengths, 288);
            build(dist, lengths + 288, 30);
            return std::pair{lit, dist};
        }();
        return codes(tables.first, tables.second);
    }
    QString dynamic()
    {
        static const quint8 order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        quint32 hlit = 0, hdist = 0, hclen = 0;
        if (!bits(5, &hlit) || !bits(5, &hdist) || !bits(4, &hclen)) return "truncated data";
        hlit += 257;
        hdist += 1;
        hclen += 4;
        if (hlit > 286 || hdist > 30) return "bad code counts";
        quint8 lengths[320] = {};
        for (quint32 i = 0; i < hclen; ++i) {
            quint32 v = 0;
            if (!bits(3, &v)) return "truncated data";
            lengths[order[i]] = quint8(v);
        }
        Huffman lencode;
        if (!build(lencode, lengths, 19)) return "bad code lengths";
        std::memset(lengths, 0, sizeof lengths);
        quint32 index = 0;
        while (index < hlit + hdist) {
            const int sym = decode(lencode);
            if (sym < 0) return "bad code";
            if (sym < 16) {
                lengths[index++] = quint8(sym);
                continue;
            }
            quint8 len = 0;
            quint32 repeat = 0;
            if (sym == 16) {
                if (index == 0) return "repeat without a length";
                len = lengths[index - 1];
                if (!bits(2, &repeat)) return "truncated data";
                repeat += 3;
            } else if (sym == 17) {
                if (!bits(3, &repeat)) return "truncated data";
                repeat += 3;
            } else {
                if (!bits(7, &repeat)) return "truncated data";
                repeat += 11;
            }
            if (index + repeat > hlit + hdist) return "too many lengths";
            while (repeat--) lengths[index++] = len;
        }
        if (lengths[256] == 0) return "no end code";
        Huffman lit, dist;
        if (!build(lit, lengths, int(hlit)) || !build(dist, lengths + hlit, int(hdist))) return "bad code lengths";
        return codes(lit, dist);
    }
};

} // namespace

namespace {

// A Windows device name: no file there, also with an extension or spaces before it ("NUL.txt", "con .mp4", "COM1")
bool deviceName(const QString& part)
{
    const QString stem = part.section('.', 0, 0).trimmed().toUpper();
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" || stem == "CONIN$" || stem == "CONOUT$")
        return true;
    if (stem.size() != 4 || !(stem.startsWith("COM") || stem.startsWith("LPT"))) return false;
    const QChar d = stem[3];
    return (d >= '0' && d <= '9') || d == QChar(0x00b9) || d == QChar(0x00b2) || d == QChar(0x00b3); // also "COM¹"
}

// One part of an entry's path that is unsafe or names another file on some system: "..", a ':' (an NTFS stream, a
// drive), a dot or space at its end (Windows drops them: ".. " is "..", "a. " is "a"), a character Windows refuses
// (< > " | ? * and control characters), a device name. The same on every system, so an archive behaves alike
// everywhere.
bool unsafePart(const QString& p)
{
    if (p == ".") return false;
    if (p == ".." || p.endsWith('.') || p.endsWith(' ') || deviceName(p)) return true;
    for (const QChar c : p)
        if (c.unicode() < 0x20 || c == ':' || c == '<' || c == '>' || c == '"' || c == '|' || c == '?' || c == '*')
            return true;
    return false;
}

// An absolute path, a drive letter or an unsafe part (unsafePart)
bool unsafeName(const QString& name)
{
    QString n = name;
    n.replace('\\', '/');
    if (n.startsWith('/') || (n.size() >= 2 && n[1] == ':')) return true;
    for (const QString& p : n.split('/', Qt::SkipEmptyParts))
        if (unsafePart(p)) return true;
    return false;
}

} // namespace

QString safeName(const QString& name)
{
    if (unsafeName(name)) return {};
    QString n = name;
    n.replace('\\', '/');
    QStringList parts;
    for (const QString& p : n.split('/', Qt::SkipEmptyParts))
        if (p != ".") parts << p;
    return parts.join('/');
}

bool junk(const QString& name)
{
    if (name == "__MACOSX" || name.startsWith("__MACOSX/")) return true;
    const QString last = name.section('/', -1);
    return last == ".DS_Store" || last.startsWith("._") || last.compare("Thumbs.db", Qt::CaseInsensitive) == 0
           || last.compare("desktop.ini", Qt::CaseInsensitive) == 0;
}

bool write(const QString& zipPath, const QVector<Source>& files, QString* error)
{
    QFile out(zipPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) return failWith(error, "cannot write " + zipPath);
    QByteArray central;
    for (const Source& s : files) {
        QFile in(s.file);
        if (!in.open(QIODevice::ReadOnly)) return failWith(error, "cannot read " + s.file);
        if (in.size() >= qint64(0xFFFFFFFFLL)) return failWith(error, "file too big for a ZIP (4 GiB): " + s.file);
        const QByteArray name = s.name.toUtf8();
        quint16 time = 0, date = 0;
        dosTime(QFileInfo(s.file).lastModified(), &time, &date);
        const quint64 offset = quint64(out.pos());
        if (offset >= 0xFFFFFFFFull) return failWith(error, "archive too big for a ZIP (4 GiB)");
        // Local header with CRC and sizes patched in after the data
        QByteArray local;
        put32(local, kLocalSig);
        put16(local, 20);
        put16(local, kUtf8);
        put16(local, 0);
        put16(local, time);
        put16(local, date);
        put32(local, 0);
        put32(local, 0);
        put32(local, 0);
        put16(local, quint16(name.size()));
        put16(local, 0);
        local += name;
        if (out.write(local) != local.size()) return failWith(error, "cannot write " + zipPath);
        quint32 crc = 0;
        quint64 size = 0;
        while (!in.atEnd()) {
            const QByteArray chunk = in.read(kChunk);
            if (chunk.isEmpty()) break;
            crc = crc32(crc, chunk.constData(), chunk.size());
            size += quint64(chunk.size());
            if (out.write(chunk) != chunk.size()) return failWith(error, "cannot write " + zipPath);
        }
        const qint64 end = out.pos();
        QByteArray sizes;
        put32(sizes, crc);
        put32(sizes, quint32(size));
        put32(sizes, quint32(size));
        if (!out.seek(qint64(offset) + 14) || out.write(sizes) != sizes.size() || !out.seek(end))
            return failWith(error, "cannot write " + zipPath);
        put32(central, kCentralSig);
        put16(central, (3 << 8) | 20); // made by: Unix (the mode below), ZIP 2.0
        put16(central, 20);
        put16(central, kUtf8);
        put16(central, 0);
        put16(central, time);
        put16(central, date);
        put32(central, crc);
        put32(central, quint32(size));
        put32(central, quint32(size));
        put16(central, quint16(name.size()));
        put16(central, 0);
        put16(central, 0);
        put16(central, 0);
        put16(central, 0);
        put32(central, quint32(0100644) << 16); // a regular file, rw-r--r--
        put32(central, quint32(offset));
        central += name;
    }
    const quint64 cdOffset = quint64(out.pos());
    if (files.size() > 0xFFFF || cdOffset >= 0xFFFFFFFFull) return failWith(error, "too many files for a ZIP");
    QByteArray end;
    put32(end, kEndSig);
    put16(end, 0);
    put16(end, 0);
    put16(end, quint16(files.size()));
    put16(end, quint16(files.size()));
    put32(end, quint32(central.size()));
    put32(end, quint32(cdOffset));
    put16(end, 0);
    if (out.write(central) != central.size() || out.write(end) != end.size() || !out.flush())
        return failWith(error, "cannot write " + zipPath);
    return true;
}

std::optional<QVector<Entry>> entries(const QString& zipPath, QString* error)
{
    const auto bad = [&](const QString& why) -> std::optional<QVector<Entry>> {
        failWith(error, QString("%1: %2").arg(QFileInfo(zipPath).fileName(), why));
        return std::nullopt;
    };
    QFile f(zipPath);
    if (!f.open(QIODevice::ReadOnly)) return bad("cannot read");
    // End record: in the last 22 bytes plus at most a 64 KiB comment
    const qint64 tail = std::min<qint64>(f.size(), 22 + 0xFFFF);
    f.seek(f.size() - tail);
    const QByteArray t = f.read(tail);
    qsizetype at = -1;
    for (qsizetype i = t.size() - 22; i >= 0; --i)
        if (get32(reinterpret_cast<const uchar*>(t.constData() + i)) == kEndSig) {
            at = i;
            break;
        }
    if (at < 0) return bad("not a ZIP file");
    const uchar* e = reinterpret_cast<const uchar*>(t.constData() + at);
    const quint16 count = get16(e + 10);
    const quint32 cdSize = get32(e + 12), cdOffset = get32(e + 16);
    if (count == 0xFFFF || cdOffset == 0xFFFFFFFFu) return bad("ZIP64 archives are not supported");
    if (qint64(cdOffset) + qint64(cdSize) > f.size()) return bad("broken central directory");
    f.seek(cdOffset);
    const QByteArray cd = f.read(cdSize);
    QVector<Entry> list;
    qsizetype p = 0;
    for (int i = 0; i < count; ++i) {
        if (p + 46 > cd.size()) return bad("broken central directory");
        const uchar* h = reinterpret_cast<const uchar*>(cd.constData() + p);
        if (get32(h) != kCentralSig) return bad("broken central directory");
        Entry en;
        const quint16 madeBy = get16(h + 4);
        en.flags = get16(h + 8);
        en.method = get16(h + 10);
        en.crc = get32(h + 16);
        en.compressedSize = get32(h + 20);
        en.size = get32(h + 24);
        const quint16 nameLen = get16(h + 28), extraLen = get16(h + 30), commentLen = get16(h + 32);
        const quint32 external = get32(h + 38);
        en.offset = get32(h + 42);
        if (p + 46 + nameLen > cd.size()) return bad("broken central directory");
        const QByteArray raw = cd.mid(p + 46, nameLen);
        en.name = (en.flags & kUtf8) ? QString::fromUtf8(raw) : QString::fromLatin1(raw);
        en.dir = en.name.endsWith('/') || en.name.endsWith('\\');
        en.symlink = (madeBy >> 8) == 3 && ((external >> 16) & 0170000) == 0120000;
        if (en.size == 0xFFFFFFFFu || en.compressedSize == 0xFFFFFFFFu || en.offset == 0xFFFFFFFFu)
            return bad("ZIP64 entries are not supported");
        list << en;
        p += 46 + nameLen + extraLen + commentLen;
    }
    return list;
}

bool extract(const QString& zipPath, const QString& dir, const QString& prefix, QString* error, quint64 maxTotal)
{
    QString err;
    const std::optional<QVector<Entry>> list = entries(zipPath, &err);
    if (!list) return failWith(error, err);
    const QString name = QFileInfo(zipPath).fileName();
    // Everything checked before the first file is written
    quint64 total = 0;
    for (const Entry& e : *list) {
        if (unsafeName(e.name) || (!e.dir && safeName(e.name).isEmpty()))
            return failWith(error, QString("%1: unsafe entry '%2'").arg(name, e.name));
        if (e.symlink) return failWith(error, QString("%1: '%2' is a symbolic link").arg(name, e.name));
        if (e.flags & 1) return failWith(error, QString("%1: '%2' is encrypted").arg(name, e.name));
        if (!e.dir && e.method != 0 && e.method != 8)
            return failWith(error, QString("%1: '%2' uses an unsupported compression (%3)").arg(name, e.name).arg(e.method));
        total += e.size;
        if (total > maxTotal) return failWith(error, QString("%1: too big when unpacked").arg(name));
    }
    // One file per name on every file system: no duplicates, also not by case, no file where a folder is
    QHash<QString, QString> files; // case-folded -> as in the archive
    QSet<QString> folders;
    for (const Entry& e : *list) {
        const QString n = safeName(e.name);
        if (n.isEmpty() || junk(n)) continue;
        const QString folded = n.toCaseFolded();
        if (e.dir) {
            folders.insert(folded);
            continue;
        }
        if (files.contains(folded))
            return failWith(error, QString("%1: '%2' and '%3' are the same file on Windows and macOS")
                                       .arg(name, files.value(folded), e.name));
        files.insert(folded, e.name);
        for (qsizetype i = folded.indexOf('/'); i > 0; i = folded.indexOf('/', i + 1)) folders.insert(folded.left(i));
    }
    for (auto it = files.cbegin(); it != files.cend(); ++it)
        if (folders.contains(it.key()))
            return failWith(error, QString("%1: '%2' is a file and a folder").arg(name, it.value()));
    QFile f(zipPath);
    if (!f.open(QIODevice::ReadOnly)) return failWith(error, "cannot read " + zipPath);
    const QDir root(dir);
    if (!QDir().mkpath(dir)) return failWith(error, "cannot create " + dir);
    const QString inside = QDir::cleanPath(root.absolutePath()) + '/';
    const QString pre = prefix.isEmpty() ? QString() : safeName(prefix) + '/';
    for (const Entry& e : *list) {
        QString rel = safeName(e.name);
        if (rel.isEmpty() || junk(rel)) continue;
        if (!pre.isEmpty()) {
            if (!(rel + '/').startsWith(pre)) continue;
            rel = rel.mid(pre.size());
            if (rel.isEmpty()) continue;
        }
        const QString target = QDir::cleanPath(root.absoluteFilePath(rel));
        if (!target.startsWith(inside)) return failWith(error, QString("%1: unsafe entry '%2'").arg(name, e.name));
        if (e.dir) {
            QDir().mkpath(target);
            continue;
        }
        QDir().mkpath(QFileInfo(target).absolutePath());
        // The data start: after the local header's name and extra field (their lengths may differ from the central)
        if (!f.seek(qint64(e.offset))) return failWith(error, QString("%1: broken entry '%2'").arg(name, e.name));
        const QByteArray lh = f.read(30);
        const uchar* h = reinterpret_cast<const uchar*>(lh.constData());
        if (lh.size() != 30 || get32(h) != kLocalSig) return failWith(error, QString("%1: broken entry '%2'").arg(name, e.name));
        if (!f.seek(qint64(e.offset) + 30 + get16(h + 26) + get16(h + 28)))
            return failWith(error, QString("%1: broken entry '%2'").arg(name, e.name));
        QFile out(target);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) return failWith(error, "cannot write " + target);
        quint32 crc = 0;
        if (e.method == 0) {
            if (e.compressedSize != e.size) return failWith(error, QString("%1: broken entry '%2'").arg(name, e.name));
            quint64 left = e.size;
            while (left > 0) {
                const QByteArray chunk = f.read(qint64(std::min<quint64>(left, kChunk)));
                if (chunk.isEmpty()) return failWith(error, QString("%1: truncated entry '%2'").arg(name, e.name));
                crc = crc32(crc, chunk.constData(), chunk.size());
                if (out.write(chunk) != chunk.size()) return failWith(error, "cannot write " + target);
                left -= quint64(chunk.size());
            }
        } else {
            Inflater inf(&f, e.compressedSize, &out, e.size);
            if (const QString why = inf.run(); !why.isEmpty())
                return failWith(error, QString("%1: '%2': %3").arg(name, e.name, why));
            crc = inf.crc();
        }
        if (crc != e.crc) return failWith(error, QString("%1: '%2' is damaged (CRC)").arg(name, e.name));
    }
    return true;
}

} // namespace Zip
