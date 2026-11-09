// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/TrustDialog.h"

#include "core/I18n.h"
#include "core/ProjectFile.h"

#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace {

// The first lines of a code, for a look at what it does
QString firstLines(const QString& code, int lines = 4)
{
    const QStringList all = code.trimmed().split(QLatin1Char('\n'));
    QString out = all.mid(0, lines).join(QLatin1Char('\n'));
    if (all.size() > lines) out += QStringLiteral("\n…");
    return out;
}

} // namespace

TrustDialog::TrustDialog(const QVector<CodeTrust::Item>& items, QWidget* parent, bool recipe) : QDialog(parent)
{
    setWindowTitle(recipe ? T("Eigener Code im Rezept") : T("Eigener Code im Projekt"));
    int filters = 0, shaders = 0;
    for (const CodeTrust::Item& it : items) (it.effect == QLatin1String("shader") ? shaders : filters) += 1;
    auto* layout = new QVBoxLayout(this);
    const QString what = recipe ? T("Dieses Rezept enthält Code, den du hier nicht selbst geschrieben hast (Filterketten: "
                                    "%1, Shader: %2). Fremder Code kann die Grafikkarte oder den Prozessor lange "
                                    "beschäftigen. Führe ihn nur aus, wenn du der Quelle vertraust.")
                                : T("Dieses Projekt enthält Code, den du hier nicht selbst geschrieben hast (Filterketten: "
                                    "%1, Shader: %2). Fremder Code kann die Grafikkarte oder den Prozessor lange "
                                    "beschäftigen. Führe ihn nur aus, wenn du der Quelle vertraust.");
    auto* text = new QLabel(what.arg(filters).arg(shaders), this);
    text->setObjectName("trustText");
    text->setWordWrap(true);
    layout->addWidget(text);

    QStringList shown;
    for (const CodeTrust::Item& it : items.mid(0, 8)) {
        const QString where = it.node ? T("Clip %1, Knoten %2").arg(it.clip).arg(it.node) : T("Clip %1").arg(it.clip);
        shown << QStringLiteral("// %1 – %2\n%3").arg(it.effect == QLatin1String("shader") ? T("Shader") : T("Filterkette"),
                                                     where, firstLines(it.code));
    }
    if (items.size() > 8) shown << QStringLiteral("…");
    auto* code = new QPlainTextEdit(shown.join(QStringLiteral("\n\n")), this);
    code->setReadOnly(true);
    code->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    code->setMinimumSize(480, 160);
    layout->addWidget(code);
    auto* note = new QLabel(T("Nicht ausgeführte Effekte bleiben im Projekt; im Inspektor kannst du sie später erlauben."), this);
    note->setWordWrap(true);
    layout->addWidget(note);

    auto* buttons = new QDialogButtonBox(this);
    m_run = buttons->addButton(T("Ausführen"), QDialogButtonBox::AcceptRole);
    m_skip = buttons->addButton(T("Nicht ausführen"), QDialogButtonBox::RejectRole);
    m_run->setObjectName("trustRun");
    m_skip->setObjectName("trustSkip");
    m_skip->setDefault(true); // Enter never runs foreign code by accident
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

bool TrustDialog::askForProject(QWidget* parent, const ProjectData& data, const QString& path)
{
    const QVector<CodeTrust::Item> untrusted = CodeTrust::untrusted(data);
    if (untrusted.isEmpty()) return true;
    TrustDialog dialog(untrusted, parent);
    if (dialog.exec() != QDialog::Accepted) return false;
    CodeTrust::trust(CodeTrust::codes(data));
    if (!path.isEmpty()) CodeTrust::remember(path, CodeTrust::codes(data));
    return true;
}
