// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Sharing content templates (core/TemplateLibrary.h) as ZIP (core/Zip.h): export writes a template's folder into an
// archive (root folder = the template's id), import unpacks one into the own templates. Used by the CLI (`templates`
// export/import) and the app's template browser.

#include "core/TemplateLibrary.h"

#include <QString>

#include <optional>

namespace TemplateLibrary {

// What import does when an own template of the archive's name exists already
enum class ZipClash {
    KeepBoth, // a name of its own: "Name 2", "Name 3" … (also in its project)
    Replace,  // the old one is deleted
    Fail,     // nothing is imported, error code "EXISTS" (the app then asks)
};

// Error codes (`code`, the CLI's codes): NOT_FOUND, BAD_ARGUMENT, EXISTS, SAVE_FAILED, BAD_ZIP, BAD_TEMPLATE

// Writes template `idOrName` (built-in or own, TemplateLibrary::find) into `zipPath` (must end in .zip; an existing
// file only with overwrite). Written through a .part file, so a failure leaves no half archive. fileCount = files in it.
bool exportZip(const QString& idOrName, const QString& zipPath, bool overwrite, QString* error = nullptr,
               QString* code = nullptr, int* fileCount = nullptr);

// Unpacks an archive (the template at its root or in its one top folder) into the own templates. The name comes from
// its project, else the folder, else the ZIP's name; media paths are fixed to the new folder. Unpacked into a hidden
// staging folder first (".import-…"), which is removed on any failure. Returns the imported template.
std::optional<ContentTemplate> importZip(const QString& zipPath, ZipClash clash = ZipClash::KeepBoth,
                                         QString* error = nullptr, QString* code = nullptr);

} // namespace TemplateLibrary
