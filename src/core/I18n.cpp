// SPDX-License-Identifier: AGPL-3.0-only
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
QString s_installed; // language of this run (install), may differ from the setting (--lang)

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

// Qt's own texts (shortcut names, dialog buttons) default to their English source: the platform theme (KDE) installs
// Qt's translation for the system language, which showed "Strg+Z" and "Ja/Nein" in an English UI on a German system.
// Installed first, so Qt's translation for the UI language and the dictionary come before it
class SourceTranslator : public QTranslator {
public:
    using QTranslator::QTranslator;
    bool isEmpty() const override { return false; }
    QString translate(const char* context, const char* source, const char*, int) const override
    {
        return qstrcmp(context, kContext) != 0 ? QString::fromUtf8(source) : QString();
    }
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

QString englishName(const QString& code)
{
    if (code == "pt") return "Portuguese, Brazil";
    if (code == "zh") return "Chinese, Simplified";
    if (code == "de") return "German";
    return QLocale::languageToString(QLocale(code).language());
}

QString displayName(const QString& code)
{
    if (code == "en") return "English";
    QString native = code == "pt"   ? QStringLiteral("Português")
                     : code == "zh" ? QStringLiteral("简体中文")
                                    : QLocale(code).nativeLanguageName();
    if (native.isEmpty()) native = code;
    native[0] = native[0].toUpper(); // "polski" -> "Polski"
    return native + " (" + englishName(code) + ")";
}

QList<Language> languages()
{
    // The name in the language itself, then in English, so everyone finds their own even in a script they can't
    // read: "日本語 (Japanese)". English alone; "pt" is Brazilian Portuguese, "zh" Simplified Chinese
    QList<Language> list = {{"de", "Deutsch (German)"}};
    for (const QString& file : QDir(":/i18n").entryList({"*.json"}, QDir::Files, QDir::Name)) {
        const QString code = file.chopped(5);
        list.append({code, displayName(code)});
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

void prepareEnvironment(const QString& override)
{
    // The platform theme (KDE) translates its own texts, e.g. Yes/No in message boxes, by LANGUAGE (gettext order),
    // not by Qt's translators -> same language as the UI
    const QString lang = known(override) ? override : language();
    qputenv("LANGUAGE", lang.toUtf8());
}

void install(const QString& override)
{
    auto* app = QCoreApplication::instance();
    const QString lang = known(override) ? override : language();
    s_installed = lang;
    // Numbers in spin boxes, dates and sorting follow the UI language, not the system (English UI on a German
    // system showed "1,0 s" next to "170.0"); no thousands separator, number fields read back what they show
    QLocale locale(lang);
    locale.setNumberOptions(QLocale::OmitGroupSeparator);
    QLocale::setDefault(locale);
    // Only against a platform theme in another language: in the system language its texts are right (KDE
    // translates e.g. "Cancel" itself, which the source translator would hide)
    if (lang != QLocale::system().name().left(2)) QCoreApplication::installTranslator(new SourceTranslator(app));
    if (lang != "de") {
        // English first, so texts not translated yet show in English rather than German
        QHash<QString, QString> map;
        readDictionary("en", map);
        if (lang != "en") readDictionary(lang, map);
        QCoreApplication::installTranslator(new MapTranslator(std::move(map), app));
        if (lang == "en") return;
    }
    // Qt's own texts (dialog buttons like Save/Discard, the context menu of text fields) in the same language: where
    // Qt keeps them, else next to the program (Windows package: windeployqt's Qt has none of its own)
    auto* qt = new QTranslator(app);
    if (qt->load(QLocale(lang), "qtbase", "_", QLibraryInfo::path(QLibraryInfo::TranslationsPath))
        || qt->load(QLocale(lang), "qtbase", "_", QCoreApplication::applicationDirPath() + "/translations"))
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

int compareNames(const QString& a, const QString& b)
{
    const int c = QString::localeAwareCompare(a.toCaseFolded(), b.toCaseFolded());
    return c != 0 ? c : QString::localeAwareCompare(a, b);
}

QChar decimalPointOf(const QString& lang) { return lang == "en" ? QChar('.') : QChar(','); }

QChar groupSeparatorOf(const QString& lang)
{
    if (lang == "en") return QChar(',');
    if (lang == "de" || lang == "es") return QChar('.');
    return QChar(0x00a0);
}

QChar decimalPoint() { return decimalPointOf(s_installed.isEmpty() ? language() : s_installed); }

QChar groupSeparator() { return groupSeparatorOf(s_installed.isEmpty() ? language() : s_installed); }

} // namespace I18n

QString T(const char* source) { return QCoreApplication::translate(kContext, source); }
QString T(const QString& source) { return T(source.toUtf8().constData()); }
