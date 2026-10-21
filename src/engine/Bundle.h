#pragma once
// Bundled runtime (AppImage on Linux, later program folder on Windows): MLT modules/data, frei0r and
// ffmpeg/ffprobe live with the program instead of in the system. Same layout on both systems:
// <prefix>/lib/mlt-7, <prefix>/share/mlt-7, <prefix>/lib/frei0r-1, programs in the program folder
// with <prefix> = program folder (Windows) resp. its parent folder (…/usr/bin -> …/usr, AppImage).

#include <QByteArray>
#include <QString>

namespace Bundle {

// Folder with the bundled MLT modules, empty = use system MLT
QString mltModuleDir();
// Sets MLT_DATA, MLT_PROFILES_PATH and FREI0R_PATH to the bundled folders (before Mlt::Factory::init);
// does not load the unused jackrack module (MLT_REPOSITORY_DENY)
void prepareMltEnvironment();
// Verbose messages? Environment variable SCHNEIDI_VERBOSE (main sets it with --verbose)
bool verboseLogging();
// Right after Mlt::Factory::init: log levels of MLT and FFmpeg. Normal: MLT from warning, FFmpeg errors only;
// verbose: MLT from info, FFmpeg from warning
void configureMltLogging();
// Directly after Mlt::Factory::init: MLT's qt module (Qt 5) creates its own QApplication the first time one of its
// services is made, which calls setlocale(LC_ALL, "") and puts LC_NUMERIC back to e.g. German. MLT modules read
// numbers with sscanf/atof (vid.stab results, timewarp speed) -> create it now and keep LC_NUMERIC at "C"
void pinNumericLocale();
// Directly after Mlt::Factory::init, in one thread: MLT creates its service caches (one per producer kind) lazily and
// without a lock (mlt_service.c get_cache). The first producers opened in parallel (Media Pool thumbnails, parallel
// opening before an export) raced on it and crashed – about 1 in 10 starts with a project (2026-10-21)
void prepareMltCaches();
// Path as MLT expects it for the module folder/environment variables (Windows: ANSI code page resp. 8.3 path)
QByteArray pathForMlt(const QString& path);
// Path of an external program (e.g. "ffmpeg"): first next to the program, then in PATH; otherwise the bare name
QString tool(const QString& name);

} // namespace Bundle
