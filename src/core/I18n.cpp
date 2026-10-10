#include "core/I18n.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLibraryInfo>
#include <QLocale>
#include <QSettings>
#include <QTranslator>

namespace {

constexpr const char* kContext = "schneidi";

// Simple translator without Qt Linguist: dictionary from a JSON resource
class MapTranslator : public QTranslator {
public:
    explicit MapTranslator(QHash<QString, QString> map, QObject* parent) : QTranslator(parent), m_map(std::move(map)) {}
    bool isEmpty() const override { return m_map.isEmpty(); }
    QString translate(const char* context, const char* source, const char*, int) const override
    {
        if (qstrcmp(context, kContext) != 0) return {};
        return m_map.value(QString::fromUtf8(source));
    }

private:
    QHash<QString, QString> m_map;
};

// Reads i18n/<lang>.json into map; empty values (not translated yet) are skipped
void readDictionary(const QString& lang, QHash<QString, QString>& map)
{
    QFile f(":/i18n/" + lang + ".json");
    if (!f.open(QIODevice::ReadOnly)) return;
    const QJsonObject obj = QJsonDocument::fromJson(f.readAll()).object();
    for (auto it = obj.begin(); it != obj.end(); ++it)
        if (!it.value().toString().isEmpty()) map.insert(it.key(), it.value().toString());
}

bool known(const QString& lang)
{
    return lang == "de" || QFile::exists(":/i18n/" + lang + ".json");
}

} // namespace

namespace I18n {

QList<Language> languages()
{
    QList<Language> list = {{"de", "Deutsch"}};
    for (const QString& file : QDir(":/i18n").entryList({"*.json"}, QDir::Files, QDir::Name)) {
        const QString code = file.chopped(5);
        // Qt calls English "American English"; otherwise the native name, capitalised ("polski" -> "Polski")
        QString name = code == "en" ? QString("English") : QLocale(code).nativeLanguageName();
        if (name.isEmpty()) name = code;
        name[0] = name[0].toUpper();
        list.append({code, name});
    }
    return list;
}

QString language()
{
    const QString lang = QSettings().value("ui/language").toString();
    if (known(lang)) return lang;
    const QString system = QLocale::system().name().left(2);
    return known(system) ? system : "en";
}

void setLanguage(const QString& lang) { QSettings().setValue("ui/language", lang); }

void install(const QString& override)
{
    auto* app = QCoreApplication::instance();
    const QString lang = known(override) ? override : language();
    if (lang != "de") {
        // English first, so texts not translated yet show in English rather than German
        QHash<QString, QString> map;
        readDictionary("en", map);
        if (lang != "en") readDictionary(lang, map);
        QCoreApplication::installTranslator(new MapTranslator(std::move(map), app));
        if (lang == "en") return;
    }
    // Qt's own texts (dialog buttons, file dialog) in the same language, if installed
    auto* qt = new QTranslator(app);
    if (qt->load(QLocale(lang), "qtbase", "_", QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
        QCoreApplication::installTranslator(qt);
    else
        delete qt;
}

QString translate(const QString& lang, const char* source)
{
    QHash<QString, QString> map;
    if (lang != "de") readDictionary("en", map);
    if (lang != "de" && lang != "en") readDictionary(lang, map);
    return map.value(QString::fromUtf8(source), QString::fromUtf8(source));
}

QChar decimalPoint() { return language() == "en" ? QChar('.') : QChar(','); }

} // namespace I18n

QString T(const char* source) { return QCoreApplication::translate(kContext, source); }
QString T(const QString& source) { return T(source.toUtf8().constData()); }
