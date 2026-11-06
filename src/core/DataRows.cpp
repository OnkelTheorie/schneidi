// SPDX-License-Identifier: AGPL-3.0-only
#include "core/DataRows.h"

#include "core/RenderJob.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QSet>
#include <QStringDecoder>

#include <algorithm>
#include <cmath>

namespace DataRows {

namespace {

void setError(QString* error, const QString& text)
{
    if (error) *error = text;
}

// The separator of the header line: the one of , ; tab that appears most often outside quotes ("," when none does)
QChar detectSeparator(const QString& text)
{
    int comma = 0, semicolon = 0, tab = 0;
    bool quoted = false;
    for (const QChar c : text) {
        if (c == '"') quoted = !quoted;
        else if (quoted) continue;
        else if (c == '\n' || c == '\r') break;
        else if (c == ',') ++comma;
        else if (c == ';') ++semicolon;
        else if (c == '\t') ++tab;
    }
    if (semicolon > comma && semicolon >= tab) return ';';
    if (tab > comma && tab > semicolon) return '\t';
    return ',';
}

struct Cell {
    QString text;
    bool quoted = false;
};

// RFC 4180 records: a quoted field may hold the separator, line breaks and "" (one quote). Line numbers (1-based) of
// each record's start for the errors.
std::optional<QVector<QVector<Cell>>> records(const QString& text, QChar sep, QVector<int>* lines, QString* error)
{
    QVector<QVector<Cell>> out;
    QVector<Cell> record;
    Cell cell;
    bool inQuotes = false, afterQuote = false, any = false;
    int line = 1, start = 1;
    const auto endCell = [&] {
        if (!cell.quoted) cell.text = cell.text.trimmed();
        record << cell;
        cell = {};
        afterQuote = false;
    };
    const auto endRecord = [&] {
        endCell();
        out << record;
        if (lines) *lines << start;
        record.clear();
        any = false;
    };
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text[i];
        if (inQuotes) {
            if (c == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') {
                    cell.text += '"';
                    ++i;
                } else {
                    inQuotes = false;
                    afterQuote = true;
                }
            } else {
                if (c == '\n') ++line;
                cell.text += c;
            }
            continue;
        }
        if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') continue; // CRLF: the LF ends it
        if (c == '\n' || c == '\r') {
            if (any || !record.isEmpty() || !cell.text.isEmpty() || cell.quoted) endRecord();
            ++line;
            start = line;
            continue;
        }
        any = true;
        if (c == sep) {
            endCell();
        } else if (c == '"' && !afterQuote && cell.text.trimmed().isEmpty()) {
            inQuotes = true;
            cell.quoted = true;
            cell.text.clear();
        } else if (afterQuote) {
            if (c.isSpace()) continue; // "a" ;b
            setError(error, QString("line %1: text after a closing quote (a quote inside a field is written \"\")").arg(line));
            return std::nullopt;
        } else {
            cell.text += c;
        }
    }
    if (inQuotes) {
        setError(error, QString("line %1: a quote is never closed").arg(start));
        return std::nullopt;
    }
    if (any || !record.isEmpty() || !cell.text.isEmpty() || cell.quoted) endRecord();
    return out;
}

// Windows-1252 (Excel's "CSV" on a Western Windows): Latin-1 but 0x80..0x9F, which hold € „ “ ” – — … there.
// By hand, not QStringDecoder: without ICU (Windows builds) Qt knows no Windows-1252.
QString fromWindows1252(const QByteArray& data)
{
    static const char16_t high[32] = {0x20AC, 0x81,   0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
                                      0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x8D,   0x017D, 0x8F,
                                      0x90,   0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                                      0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D,   0x017E, 0x0178};
    QString out(data.size(), Qt::Uninitialized);
    for (qsizetype i = 0; i < data.size(); ++i) {
        const uchar b = uchar(data[i]);
        out[i] = QChar(b >= 0x80 && b < 0xA0 ? high[b - 0x80] : char16_t(b));
    }
    return out;
}

} // namespace

QString valueText(const QJsonValue& v)
{
    if (v.isString()) return v.toString();
    if (v.isDouble()) {
        const double d = v.toDouble();
        // Whole numbers as such (an order number 1234567, not 1.23457e+06), others as short as they stay exact
        if (std::abs(d) < 9e15 && d == std::floor(d)) return QString::number(qint64(d));
        return QString::number(d, 'g', QLocale::FloatingPointShortest);
    }
    if (v.isBool()) return v.toBool() ? "true" : "false";
    if (v.isObject()) return v.toObject().contains("file") ? v["file"].toString() : v["text"].toString();
    return {};
}

QString decodeText(const QByteArray& data)
{
    // UTF-16 with its byte order mark (Excel's "Unicode text"), UTF-8 with or without one, else Windows-1252
    if (data.startsWith("\xFF\xFE") || data.startsWith("\xFE\xFF")) {
        QStringDecoder utf16(QStringDecoder::Utf16); // reads the mark: either byte order
        return utf16.decode(data);
    }
    // Without the mark: the separators, line ends and digits (also of a Cyrillic or Greek table) have a zero byte in
    // UTF-16, all on the odd positions (little endian) or all on the even ones (big endian); UTF-8 and Windows-1252
    // text has none
    if (data.size() >= 4 && data.size() % 2 == 0) {
        const int n = int(std::min<qsizetype>(data.size(), 512));
        int even = 0, odd = 0;
        for (int i = 0; i < n; ++i)
            if (data[i] == 0) ++(i % 2 ? odd : even);
        const int most = std::max(even, odd), least = std::min(even, odd);
        if (most * 20 >= n / 2 && least * 4 < most) {
            QStringDecoder utf16(odd > even ? QStringDecoder::Utf16LE : QStringDecoder::Utf16BE);
            return utf16.decode(data);
        }
    }
    QStringDecoder utf8(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    QString text = utf8.decode(data);
    if (utf8.hasError()) text = fromWindows1252(data);
    if (text.startsWith(QChar(0xFEFF))) text.remove(0, 1);
    return text;
}

std::optional<Table> parseCsv(const QByteArray& data, QString* error)
{
    QString text = decodeText(data);
    Table t;
    t.separator = detectSeparator(text);
    QVector<int> lines;
    const std::optional<QVector<QVector<Cell>>> recs = records(text, t.separator, &lines, error);
    if (!recs) return std::nullopt;
    if (recs->isEmpty()) {
        setError(error, "empty table (the first line names the columns)");
        return std::nullopt;
    }
    // Header: an empty name (Excel's separators at the end) drops that column
    const QVector<Cell>& header = recs->first();
    QVector<int> used; // cell index of each column
    QSet<QString> seen;
    for (int i = 0; i < header.size(); ++i) {
        const QString name = header[i].text.trimmed();
        if (name.isEmpty()) continue;
        if (seen.contains(name)) {
            setError(error, QString("line %1: the column '%2' is there twice").arg(lines.first()).arg(name));
            return std::nullopt;
        }
        seen.insert(name);
        t.columns << name;
        used << i;
    }
    if (t.columns.isEmpty()) {
        setError(error, "the first line names no columns");
        return std::nullopt;
    }
    for (int r = 1; r < recs->size(); ++r) {
        const QVector<Cell>& rec = (*recs)[r];
        bool empty = true;
        for (const Cell& c : rec) empty &= c.text.isEmpty();
        if (empty) continue; // ";;;" of an empty spreadsheet row
        for (int i = int(header.size()); i < rec.size(); ++i)
            if (!rec[i].text.isEmpty()) {
                setError(error, QString("line %1: %2 cells, the header has %3 columns")
                                    .arg(lines[r]).arg(rec.size()).arg(header.size()));
                return std::nullopt;
            }
        QJsonObject row;
        for (int c = 0; c < t.columns.size(); ++c) {
            const int i = used[c];
            if (i < rec.size() && !rec[i].text.isEmpty()) row[t.columns[c]] = rec[i].text;
        }
        t.rows << row;
    }
    return t;
}

std::optional<Table> fromJson(const QJsonValue& value, QString* error)
{
    QJsonValue v = value;
    if (v.isObject() && v.toObject().contains("rows")) v = v.toObject().value("rows");
    if (!v.isArray()) {
        setError(error, "rows: an array of objects [{column: value}, …] or {\"rows\": […]}");
        return std::nullopt;
    }
    Table t;
    QSet<QString> seen;
    const QJsonArray list = v.toArray();
    for (int i = 0; i < list.size(); ++i) {
        if (!list[i].isObject()) {
            setError(error, QString("row %1 is no object {column: value}").arg(i + 1));
            return std::nullopt;
        }
        QJsonObject row;
        const QJsonObject o = list[i].toObject();
        for (auto it = o.begin(); it != o.end(); ++it) {
            if (it.value().isNull() || it.value().isUndefined()) continue;
            if (!seen.contains(it.key())) {
                seen.insert(it.key());
                t.columns << it.key();
            }
            row[it.key()] = it.value();
        }
        t.rows << row;
    }
    return t;
}

std::optional<Table> parseJson(const QByteArray& data, QString* error)
{
    QJsonParseError err;
    QByteArray bytes = data;
    if (bytes.startsWith("\xEF\xBB\xBF")) bytes.remove(0, 3);
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &err);
    if (err.error != QJsonParseError::NoError) {
        setError(error, QString("invalid JSON at %1: %2").arg(err.offset).arg(err.errorString()));
        return std::nullopt;
    }
    return fromJson(doc.isArray() ? QJsonValue(doc.array()) : QJsonValue(doc.object()), error);
}

std::optional<Table> readFile(const QString& path, QString* error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        setError(error, "cannot read " + path);
        return std::nullopt;
    }
    const QByteArray data = f.readAll();
    QString err;
    std::optional<Table> t = QFileInfo(path).suffix().compare("json", Qt::CaseInsensitive) == 0 ? parseJson(data, &err)
                                                                                                  : parseCsv(data, &err);
    if (!t) setError(error, QString("%1: %2").arg(QFileInfo(path).fileName(), err));
    return t;
}

QString patternError(const QString& pattern, const QStringList& columns)
{
    for (qsizetype i = 0; i < pattern.size(); ++i) {
        if (pattern[i] != '{') continue;
        const qsizetype end = pattern.indexOf('}', i);
        if (end < 0) return QString("name: '{' without '}' in \"%1\"").arg(pattern);
        const QString key = pattern.mid(i + 1, end - i - 1).trimmed();
        const QString name = key.section(':', 0, 0).trimmed();
        if (name == "n") {
            bool ok = true;
            const QString spec = key.section(':', 1).trimmed();
            const int width = spec.isEmpty() ? 0 : spec.toInt(&ok);
            if (!ok || width < 0 || spec.startsWith('-'))
                return QString("name: {n:03} pads the row number, not {%1}").arg(key);
        } else if (!columns.contains(name)) {
            return QString("name: no column '%1' (columns: %2; {n} = the row number)").arg(name, columns.join(", "));
        }
        i = end;
    }
    return {};
}

std::optional<QString> fileName(const QString& pattern, const QJsonObject& row, int n, const QStringList& fileColumns,
                                int maxLength, QString* error)
{
    QString out;
    for (qsizetype i = 0; i < pattern.size(); ++i) {
        if (pattern[i] != '{') {
            out += pattern[i];
            continue;
        }
        const qsizetype end = pattern.indexOf('}', i);
        if (end < 0) {
            setError(error, QString("name: '{' without '}' in \"%1\"").arg(pattern));
            return std::nullopt;
        }
        const QString key = pattern.mid(i + 1, end - i - 1).trimmed();
        const QString name = key.section(':', 0, 0).trimmed();
        if (name == "n") {
            const QString spec = key.section(':', 1).trimmed();
            // Padded to at most kMaxNumberWidth digits (a negative width would pad on the right)
            const int width = std::clamp(spec.isEmpty() ? 0 : spec.toInt(), 0, kMaxNumberWidth);
            out += QString("%1").arg(n, width, 10, QChar('0'));
        } else if (row.contains(name)) { // (a column this row leaves empty adds nothing)
            const QString v = valueText(row[name]);
            out += fileColumns.contains(name) ? QFileInfo(v).completeBaseName() : v;
        }
        i = end;
    }
    QString safe = renderFileBaseName(out);
    if (maxLength > 0 && safe.size() > maxLength) safe = renderFileBaseName(safe.left(maxLength));
    return safe;
}

QStringList uniqueNames(const QStringList& names)
{
    QSet<QString> taken;
    for (const QString& n : names) taken.insert(n.toLower());
    QSet<QString> used;
    QStringList out;
    for (const QString& n : names) {
        QString name = n;
        if (used.contains(name.toLower()))
            for (int k = 2;; ++k) {
                name = QString("%1-%2").arg(n).arg(k);
                if (!used.contains(name.toLower()) && !taken.contains(name.toLower())) break;
            }
        used.insert(name.toLower());
        out << name;
    }
    return out;
}

} // namespace DataRows
