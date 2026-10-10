// Test UI languages: every dictionary in i18n/ shows up in the language menu, restart hint in the new language,
// untranslated texts fall back to English
#include "check.h"

#include "core/I18n.h"

#include <QCoreApplication>

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
    return Check::result();
}
