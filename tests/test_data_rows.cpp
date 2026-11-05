// SPDX-License-Identifier: AGPL-3.0-only
// core/DataRows: CSV (quotes, separators "," and ";", BOM, CRLF, empty cells and rows, errors), JSON (array, {rows}),
// file names from a pattern (row number, columns, file base names, unsafe characters, Windows device names, length)
// and unique names for duplicates.

#include "check.h"

#include "core/DataRows.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QTemporaryDir>

namespace {

void testCsv()
{
    std::printf("CSV\n");
    // Quotes: separators, quotes and line breaks inside, spaces around unquoted cells
    std::optional<DataRows::Table> t = DataRows::parseCsv("product,claim,clip\n"
                                                          "\"Mug, blue\",\"Say \"\"hi\"\"\", a.mp4\n"
                                                          "Pen,\"two\nlines\",b.mp4\n");
    if (!CHECK(t)) return;
    CHECK(t->separator == ',');
    CHECK_EQ(t->columns, (QStringList{"product", "claim", "clip"}));
    CHECK_EQ(t->rows.size(), 2);
    CHECK_EQ(t->rows[0]["product"].toString(), QString("Mug, blue"));
    CHECK_EQ(t->rows[0]["claim"].toString(), QString("Say \"hi\""));
    CHECK_EQ(t->rows[0]["clip"].toString(), QString("a.mp4"));
    CHECK_EQ(t->rows[1]["claim"].toString(), QString("two\nlines"));

    // German Excel: ";", BOM, CRLF, decimal commas stay in the cell, empty cells left out, empty rows skipped
    t = DataRows::parseCsv("\xEF\xBB\xBFprodukt;preis;clip\r\nTasse;4,99;a.mp4\r\nStift;;\r\n;;\r\n\r\nHeft;1,50;c.mp4");
    if (!CHECK(t)) return;
    CHECK(t->separator == ';');
    CHECK_EQ(t->columns.first(), QString("produkt")); // no BOM in the name
    CHECK_EQ(t->rows.size(), 3);
    CHECK_EQ(t->rows[0]["preis"].toString(), QString("4,99"));
    CHECK(!t->rows[1].contains("preis") && !t->rows[1].contains("clip"));
    CHECK_EQ(t->rows[1]["produkt"].toString(), QString("Stift"));
    CHECK_EQ(t->rows[2]["clip"].toString(), QString("c.mp4")); // last line without line break
    // A quoted ";" in a semicolon file, a CRLF inside quotes
    t = DataRows::parseCsv("a;b\r\n\"x;y\";\"1\r\n2\"\r\n");
    if (!CHECK(t)) return;
    CHECK_EQ(t->rows.size(), 1);
    CHECK_EQ(t->rows[0]["a"].toString(), QString("x;y"));
    CHECK_EQ(t->rows[0]["b"].toString(), QString("1\r\n2"));
    // Trailing separators of a spreadsheet: an empty header column is dropped, empty extra cells are fine
    t = DataRows::parseCsv("a,b,\n1,2,\n3,4,,\n");
    if (!CHECK(t)) return;
    CHECK_EQ(t->columns, (QStringList{"a", "b"}));
    CHECK_EQ(t->rows.size(), 2);
    // A single column
    t = DataRows::parseCsv("title\nHello\n");
    CHECK(t && t->rows.size() == 1 && t->rows[0]["title"].toString() == "Hello");

    // Errors with the line
    QString err;
    CHECK(!DataRows::parseCsv("", &err));
    CHECK(!DataRows::parseCsv("a,a\n1,2\n", &err));
    CHECK(err.contains("twice"));
    CHECK(!DataRows::parseCsv("a,b\n1,2\n\"open,3\n", &err));
    CHECK(err.contains("line 3"));
    CHECK(!DataRows::parseCsv("a,b\n1,2,3\n", &err));
    CHECK(err.contains("line 2"));
    CHECK(!DataRows::parseCsv("a,b\n\"x\"y,2\n", &err));
}

void testJson()
{
    std::printf("JSON\n");
    std::optional<DataRows::Table> t = DataRows::parseJson(R"([{"title":"A","n":3},{"title":"B","clip":{"file":"x.mp4"}}])");
    if (!CHECK(t)) return;
    CHECK_EQ(t->rows.size(), 2);
    CHECK(t->columns.contains("clip") && t->columns.contains("title") && t->columns.contains("n"));
    CHECK_EQ(t->rows[0]["n"].toInt(), 3);
    CHECK_EQ(t->rows[1]["clip"]["file"].toString(), QString("x.mp4"));
    t = DataRows::parseJson("\xEF\xBB\xBF{\"rows\":[{\"title\":\"A\",\"gone\":null}]}");
    CHECK(t && t->rows.size() == 1 && !t->rows[0].contains("gone"));
    QString err;
    CHECK(!DataRows::parseJson("[1,2]", &err));
    CHECK(err.contains("row 1"));
    CHECK(!DataRows::parseJson("{\"title\":\"A\"}", &err));
    CHECK(!DataRows::parseJson("[{", &err));
    CHECK(DataRows::fromJson(QJsonArray{QJsonObject{{"a", "b"}}}));

    // readFile: by extension
    QTemporaryDir dir;
    const auto write = [&](const QString& name, const QByteArray& data) {
        QFile f(dir.filePath(name));
        f.open(QIODevice::WriteOnly);
        f.write(data);
        return f.fileName();
    };
    t = DataRows::readFile(write("rows.json", R"([{"a":"1"}])"));
    CHECK(t && t->rows.size() == 1);
    t = DataRows::readFile(write("rows.csv", "a;b\n1;2\n"));
    CHECK(t && t->rows[0]["b"].toString() == "2");
    CHECK(!DataRows::readFile(dir.filePath("missing.csv"), &err));
    CHECK(!DataRows::readFile(write("bad.csv", "a,a\n"), &err));
    CHECK(err.startsWith("bad.csv"));
}

void testNames()
{
    std::printf("file names\n");
    const QJsonObject row{{"product", "Mug: blue/red?"}, {"clip", "/media/shots/front view.mp4"}, {"price", 4.5}};
    CHECK_EQ(*DataRows::fileName("{n:03}-{product}", row, 7), QString("007-Mug - blue-red"));
    CHECK_EQ(*DataRows::fileName("{n}_{price}", row, 12), QString("12_4.5"));
    CHECK_EQ(*DataRows::fileName("{clip}", row, 1, {"clip"}), QString("front view")); // a file column: its base name
    CHECK_EQ(*DataRows::fileName("{missing}x", QJsonObject{}, 1), QString("x")); // an empty cell adds nothing
    CHECK_EQ(*DataRows::fileName("{product}", QJsonObject{{"product", "NUL"}}, 1), QString("NUL_")); // Windows device
    CHECK_EQ(*DataRows::fileName("{product}", QJsonObject{{"product", "  .hidden. "}}, 1), QString("hidden"));
    CHECK_EQ(*DataRows::fileName("{product}", QJsonObject{{"product", "a<b>c|d*e\"f"}}, 1), QString("a_b_c_d_e'f"));
    CHECK_EQ(DataRows::fileName("{product}", QJsonObject{{"product", QString(300, 'x')}}, 1)->size(), 120);
    CHECK_EQ(*DataRows::fileName("{product}", QJsonObject{{"product", QString(119, 'x') + " y"}}, 1),
             QString(119, 'x')); // cut, no space at the end
    CHECK(!DataRows::fileName("{n", row, 1));
    // Pattern check up front
    const QStringList cols{"product", "clip"};
    CHECK(DataRows::patternError("{n:03}-{product}", cols).isEmpty());
    CHECK(DataRows::patternError("{prodcut}", cols).contains("prodcut"));
    CHECK(DataRows::patternError("{n:x}", cols).contains("{n:03}"));
    CHECK(!DataRows::patternError("{n", cols).isEmpty());
    // Duplicates: -2, -3, case-insensitively, without taking a name that is there already
    CHECK_EQ(DataRows::uniqueNames({"x", "x", "x"}), (QStringList{"x", "x-2", "x-3"}));
    CHECK_EQ(DataRows::uniqueNames({"a", "A", "b", "a", "a-2"}), (QStringList{"a", "A-3", "b", "a-4", "a-2"}));
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    testCsv();
    testJson();
    testNames();
    return Check::result();
}
