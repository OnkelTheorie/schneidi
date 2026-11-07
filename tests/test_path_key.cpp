// SPDX-License-Identifier: AGPL-3.0-only
// Path keys (core/PathKey): one key per file for caches, notes, plan/backup folders and locks - absolute and clean,
// lower case on Windows (both ways tested on every platform through the parameter); the keys that use it.
#include "core/PathKey.h"
#include "core/ProjectFile.h"
#include "engine/MediaIndex.h"

#include "check.h"

#include <QCoreApplication>
#include <QDir>
#include <QTemporaryDir>

int main(int argc, char** argv)
{
    Check::initEnv();
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("schneidi");
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();

    // Case-insensitive (Windows): drive letter and names in any case, one key
    CHECK_EQ(PathKey::of("C:/Videos/Clip.MP4", true), PathKey::of("c:/videos/clip.mp4", true));
    CHECK_EQ(PathKey::of("C:/Videos/Clip.MP4", true), PathKey::of("C:/Videos/Clip.MP4", true).toLower());
    // Case-sensitive (Linux, macOS): the case stays, two files
    CHECK(PathKey::of("/videos/Clip.mp4", false) != PathKey::of("/videos/clip.mp4", false));
    CHECK_EQ(PathKey::of("/videos/Clip.mp4", false), QString("/videos/Clip.mp4"));
    // Clean and absolute in both ways: "." and ".." and double slashes do not make another key
    for (bool fold : {false, true}) {
        CHECK_EQ(PathKey::of("/a/./b/../c//d.mp4", fold), PathKey::of("/a/c/d.mp4", fold));
        CHECK_EQ(PathKey::of("d.mp4", fold), PathKey::of(QDir::currentPath() + "/d.mp4", fold));
        CHECK(PathKey::of("", fold).isEmpty());
    }
    // Inside a folder (template files): case-insensitive on Windows, the folder itself and its neighbours are not
    CHECK(PathKey::isInside("C:/Templates/Intro/media/a.png", "c:/templates/intro", true));
    CHECK(!PathKey::isInside("C:/Templates/Intro/media/a.png", "c:/templates/intro", false));
    for (bool fold : {false, true}) {
        CHECK(PathKey::isInside("/t/intro/a.png", "/t/intro/", fold));
        CHECK(PathKey::isInside("/t/intro/x/../a.png", "/t/intro", fold));
        CHECK(!PathKey::isInside("/t/intro", "/t/intro", fold));
        CHECK(!PathKey::isInside("/t/intro2/a.png", "/t/intro", fold));
        CHECK(!PathKey::isInside("/t/intro/../b.png", "/t/intro", fold));
    }
#ifdef Q_OS_WIN
    CHECK(PathKey::kFoldCase);
#else
    CHECK(!PathKey::kFoldCase);
#endif

    // The keys that use it: two spellings of one file give one cache key, one notes folder, one backup folder
    const QString file = tmp.filePath("clip.mp4");
    const QString other = tmp.path() + "/sub/../clip.mp4";
    CHECK_EQ(MediaIndex::cacheKey(file, "heuristic"), MediaIndex::cacheKey(other, "heuristic"));
    CHECK_EQ(ProjectFile::backupDir(tmp.filePath("film.schneidi")),
             ProjectFile::backupDir(tmp.path() + "/sub/../film.schneidi"));
    return Check::result();
}
