// SPDX-License-Identifier: AGPL-3.0-only
// Data rows for rendering a template many times (`templates` fill + rows): a table read from CSV (RFC 4180: quoted
// fields with commas, quotes and line breaks; "," or ";" as separator - German Excel writes ";"; UTF-8 with or without
// BOM, UTF-16, else Windows-1252 as Excel writes it; CRLF or LF; a header row) or JSON (an array of objects, or
// {rows:[…]}), and the file names of the outputs from a pattern ("{n:03}-{product}") made safe on every OS. Pure: no
// files but the one read, no engine.
#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>

namespace DataRows {

struct Table {
    QStringList columns;       // in the order of the header / of first appearance in the JSON objects
    QVector<QJsonObject> rows; // column -> value (CSV: strings; an empty cell is left out)
    QChar separator;           // CSV only: the separator found
};

// CSV text. Fails (nullopt + error with the line) for an empty file, a missing header, an empty or duplicate column
// name, a quote that is never closed or a row with more cells than the header.
std::optional<Table> parseCsv(const QByteArray& data, QString* error = nullptr);
// JSON text: an array of objects or {"rows":[…]}
std::optional<Table> parseJson(const QByteArray& data, QString* error = nullptr);
// Rows given inline (an array of objects or {"rows":[…]})
std::optional<Table> fromJson(const QJsonValue& value, QString* error = nullptr);
// A file: .json as JSON, anything else (.csv, .txt …) as CSV
std::optional<Table> readFile(const QString& path, QString* error = nullptr);

// A cell as text (names, text slots): numbers without exponent ("1234567", "2.5"), true/false, a {file}/{text}
// object's file or text
QString valueText(const QJsonValue& v);
// The bytes of a table file as text: UTF-16 with a byte order mark, UTF-8 (with or without BOM), else Windows-1252
// (what Excel writes as "CSV" on a Western Windows)
QString decodeText(const QByteArray& data);

inline constexpr int kMaxNumberWidth = 9; // {n:W}: W from 0 to 9
// The file name (without extension) of row `n` (1-based) from `pattern`: {n}, {n:03} (zero padded to 3 digits, 0..9),
// {<column>} (the row's value; `fileValues` columns hold files: their base name). Made safe as a file name on every OS
// (renderFileBaseName: no / \ : * ? " < > |, no Windows device names, no dot or space at the ends) and at most
// `maxLength` characters. An unknown {name} fails with the error (also checked once up front by patternError).
std::optional<QString> fileName(const QString& pattern, const QJsonObject& row, int n,
                                const QStringList& fileColumns = {}, int maxLength = 120, QString* error = nullptr);
// The first problem of a pattern against the columns ({unknown} column, unclosed brace), empty if none
QString patternError(const QString& pattern, const QStringList& columns);
// Names made unique (case-insensitively, as on Windows and macOS): the second "a" becomes "a-2", the third "a-3"
QStringList uniqueNames(const QStringList& names);

} // namespace DataRows
