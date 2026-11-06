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

// Error codes (`code`, the CLI's codes): NOT_FOUND, BAD_ARGUMENT, EXISTS, SAVE_FAILED, BAD_ZIP, BAD_TEMPLATE, BUSY

// Writes template `idOrName` (built-in or own, TemplateLibrary::find) into `zipPath` (must end in .zip; an existing
// file only with overwrite). Written through a .part file, so a failure leaves no half archive. fileCount = files in it.
// A file name that is not valid on every system ("a:b.mp4", "nul.png", "Clip.mp4" next to "clip.mp4") gets a portable
// one in the archive ("a_b.mp4", "nul_.png", "clip-2.mp4"), and the project in it points at that.
bool exportZip(const QString& idOrName, const QString& zipPath, bool overwrite, QString* error = nullptr,
               QString* code = nullptr, int* fileCount = nullptr);

// Unpacks an archive (the template at its root or in its one top folder) into the own templates. The name comes from
// its project, else the folder, else the ZIP's name; media paths are fixed to the new folder. What macOS/Windows add
// ("__MACOSX/", ".DS_Store", "Thumbs.db") is left out. Unpacked into a hidden staging folder first (".import-…"), which
// is removed on any failure; Replace keeps the old template until the new one is in place. Refused before anything
// is written: unsafe names (core/Zip.h), more than importLimit unpacked, not enough free disk space.
// Returns the imported template.
// The most an archive of `zipSize` bytes may unpack to: 20 times its size plus 64 MiB (media hardly compress, a
// project about 10 times), at most 4 GiB
quint64 importLimit(qint64 zipSize);

// waitMs: how long to wait for the library lock (lockLibrary; -1 = a minute, the app: AppWaitMs), else code BUSY
std::optional<ContentTemplate> importZip(const QString& zipPath, ZipClash clash = ZipClash::KeepBoth,
                                         QString* error = nullptr, QString* code = nullptr, int waitMs = -1);

} // namespace TemplateLibrary
