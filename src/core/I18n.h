#pragma once

#include <QString>

// Oberflächensprache: Deutsch ist die Quellsprache im Code, Englisch kommt aus i18n/en.json
// (Zuordnung deutscher Text -> englischer Text). Umschalten wirkt nach dem Neustart.
namespace I18n {

QString language();                    // "de" oder "en"
void setLanguage(const QString& lang); // speichert nur, wirkt beim nächsten Start
void install(const QString& override = {}); // nach dem Anlegen der QApplication; override = Sprache nur für diesen Lauf

} // namespace I18n

// Markiert Text in Tabellen (const char*) fürs Wörterbuch; übersetzt wird erst bei der Anzeige mit T()
#define N_(s) s

// Übersetzter Oberflächentext (Quelltext deutsch)
QString T(const char* source);
QString T(const QString& source);
