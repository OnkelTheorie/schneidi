// SPDX-License-Identifier: AGPL-3.0-only
// Test UI languages: every dictionary in i18n/ shows up in the language menu, restart hint in the new language,
// untranslated texts fall back to English, numbers and bold/italic letters follow the UI language
#include "check.h"

#include "core/I18n.h"

#include <QCoreApplication>
#include <QLocale>

int main(int argc, char** argv)
{
    Check::initEnv();
    QCoreApplication app(argc, argv);
    Check::initApp("i18n");

    QStringList codes, names;
    for (const I18n::Language& l : I18n::languages()) {
        codes << l.code;
        names << l.name;
    }
    CHECK_EQ(codes.value(0), QString("de"));
    for (const char* code : {"en", "es", "fr", "pl", "ru"}) CHECK(codes.contains(code));
    CHECK(names.contains("English"));
    CHECK(names.contains("Polski"));
    CHECK(names.contains("Русский"));

    const char* hint = "Die Sprache ändert sich nach dem Neustart von schneidi.";
    CHECK_EQ(I18n::translate("de", hint), QString(hint));
    CHECK_EQ(I18n::translate("en", hint), QString("The language changes after restarting schneidi."));
    CHECK(I18n::translate("fr", hint) != I18n::translate("en", hint));
    // Unknown source text stays as it is
    CHECK_EQ(I18n::translate("fr", "gibt es nicht"), QString("gibt es nicht"));

    // Numbers follow the UI language, not the system (CI runs with LANG=de_DE): English UI shows "1.5", not "1,5"
    I18n::install("en");
    CHECK_EQ(QLocale().toString(1234.5, 'f', 1), QString("1234.5"));
    CHECK_EQ(I18n::decimalPoint(), QChar('.'));
    // Bold/italic buttons show the letter of the UI language, not German F/K
    CHECK_EQ(T("F"), QString("B"));
    CHECK_EQ(T("K"), QString("I"));
    CHECK_EQ(I18n::translate("ru", "F"), QString("Ж"));
    return Check::result();
}
