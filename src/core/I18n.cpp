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

// Einfacher Übersetzer ohne Qt-Linguist: Wörterbuch aus einer JSON-Ressource
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

} // namespace

namespace I18n {

QString language()
{
    const QString lang = QSettings().value("ui/language").toString();
    if (lang == "de" || lang == "en") return lang;
    return QLocale::system().language() == QLocale::German ? "de" : "en";
}

void setLanguage(const QString& lang) { QSettings().setValue("ui/language", lang); }

void install(const QString& override)
{
    auto* app = QCoreApplication::instance();
    const QString lang = override == "de" || override == "en" ? override : language();
    if (lang == "en") {
        QFile f(":/i18n/en.json");
        if (!f.open(QIODevice::ReadOnly)) return;
        const QJsonObject obj = QJsonDocument::fromJson(f.readAll()).object();
        QHash<QString, QString> map;
        for (auto it = obj.begin(); it != obj.end(); ++it)
            if (!it.value().toString().isEmpty()) map.insert(it.key(), it.value().toString());
        QCoreApplication::installTranslator(new MapTranslator(std::move(map), app));
        return;
    }
    // Deutsch: Qt-eigene Texte (Dialogknöpfe, Dateidialog) ebenfalls deutsch, falls installiert
    auto* qt = new QTranslator(app);
    if (qt->load(QLocale(QLocale::German), "qtbase", "_", QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
        QCoreApplication::installTranslator(qt);
    else
        delete qt;
}

} // namespace I18n

QString T(const char* source) { return QCoreApplication::translate(kContext, source); }
QString T(const QString& source) { return T(source.toUtf8().constData()); }
