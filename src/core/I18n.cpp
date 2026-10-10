#include "core/I18n.h"

#include <QCoreApplication>
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
    for (const I18n::Language& l : I18n::languages())
        if (lang == l.code) return true;
    return false;
}

} // namespace

namespace I18n {

const QList<Language>& languages()
{
    static const QList<Language> list = {
        {"de", "Deutsch", "Die Sprache ändert sich nach dem Neustart von schneidi.", "Jetzt neu starten", "Später"},
        {"en", "English", "The language changes after restarting schneidi.", "Restart Now", "Later"},
        {"es", "Español", "El idioma cambiará al reiniciar schneidi.", "Reiniciar ahora", "Más tarde"},
        {"fr", "Français", "La langue changera au redémarrage de schneidi.", "Redémarrer maintenant", "Plus tard"},
        {"pl", "Polski", "Język zmieni się po ponownym uruchomieniu schneidi.", "Uruchom ponownie teraz", "Później"},
        {"ru", "Русский", "Язык изменится после перезапуска schneidi.", "Перезапустить сейчас", "Позже"},
    };
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

QChar decimalPoint() { return language() == "en" ? QChar('.') : QChar(','); }

} // namespace I18n

QString T(const char* source) { return QCoreApplication::translate(kContext, source); }
QString T(const QString& source) { return T(source.toUtf8().constData()); }
