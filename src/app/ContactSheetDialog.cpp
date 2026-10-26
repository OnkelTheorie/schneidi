#include "app/ContactSheetDialog.h"

#include "core/I18n.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSpinBox>

ContactSheetDialog::ContactSheetDialog(const QString& name, const QString& dir, bool hasInOut, QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(T("Kontaktbogen speichern"));
    const QSettings s;
    auto* form = new QFormLayout(this);

    m_count = new QSpinBox;
    m_count->setRange(2, 100);
    m_count->setValue(s.value("sheet/count", 12).toInt());
    form->addRow(T("Bilder"), m_count);

    m_columns = new QComboBox;
    m_columns->addItem(T("Automatisch"), 0);
    for (int c = 2; c <= 8; ++c) m_columns->addItem(QString::number(c), c);
    m_columns->setCurrentIndex(std::max(0, m_columns->findData(s.value("sheet/columns", 0).toInt())));
    form->addRow(T("Spalten"), m_columns);

    m_width = new QComboBox;
    for (int w : {240, 320, 480, 640, 960}) m_width->addItem(T("%1 Pixel breit").arg(w), w);
    m_width->setCurrentIndex(std::max(0, m_width->findData(s.value("sheet/width", 480).toInt())));
    form->addRow(T("Bildgröße"), m_width);

    m_range = new QComboBox;
    m_range->addItem(T("Ganze Timeline"));
    if (hasInOut) m_range->addItem(T("In/Out-Bereich"));
    form->addRow(T("Bereich"), m_range);

    auto* row = new QHBoxLayout;
    m_path = new QLineEdit;
    m_path->setMinimumWidth(360);
    const QString folder = s.value("sheet/dir", dir).toString();
    QString file = name + " – " + T("Kontaktbogen") + ".jpg";
    file.replace(QRegularExpression(R"([/\\:*?"<>|])"), "-"); // a timeline "Intro/Outro" is no folder
    m_path->setText(QDir::toNativeSeparators(QDir(folder).filePath(file)));
    row->addWidget(m_path, 1);
    auto* browse = new QPushButton(T("Durchsuchen…"));
    row->addWidget(browse);
    form->addRow(T("Datei"), row);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    form->addRow(buttons);
    connect(browse, &QPushButton::clicked, this, &ContactSheetDialog::browse);
    connect(buttons, &QDialogButtonBox::accepted, this, &ContactSheetDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &ContactSheetDialog::reject);
}

int ContactSheetDialog::count() const { return m_count->value(); }
int ContactSheetDialog::columns() const { return m_columns->currentData().toInt(); }
int ContactSheetDialog::tileWidth() const { return m_width->currentData().toInt(); }
bool ContactSheetDialog::inOut() const { return m_range->currentIndex() == 1; }

QString ContactSheetDialog::path() const
{
    QString p = QDir::fromNativeSeparators(m_path->text().trimmed());
    const QString suffix = QFileInfo(p).suffix().toLower();
    if (suffix != "jpg" && suffix != "jpeg" && suffix != "png") p += ".jpg";
    return p;
}

void ContactSheetDialog::browse()
{
    const QString file = QFileDialog::getSaveFileName(this, T("Kontaktbogen speichern"), path(),
                                                      T("Bilder (*.jpg *.png)"));
    if (!file.isEmpty()) m_path->setText(QDir::toNativeSeparators(file));
}

void ContactSheetDialog::accept()
{
    const QFileInfo fi(path());
    if (!fi.absoluteDir().exists()) {
        QMessageBox::warning(this, windowTitle(), T("Den Ordner %1 gibt es nicht.").arg(QDir::toNativeSeparators(fi.absolutePath())));
        return;
    }
    if (fi.exists()
        && QMessageBox::question(this, windowTitle(), T("%1 gibt es schon. Ersetzen?").arg(fi.fileName()))
               != QMessageBox::Yes)
        return;
    QSettings s;
    s.setValue("sheet/count", count());
    s.setValue("sheet/columns", columns());
    s.setValue("sheet/width", tileWidth());
    s.setValue("sheet/dir", fi.absolutePath());
    QDialog::accept();
}
