// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <QString>

// Media files outside schneidi: open a file with the program the system has for it, or show it in the file manager
// with the file selected (Windows Explorer, macOS Finder, on Linux the file manager over its D-Bus interface; where
// none answers, the folder is opened).
namespace FileManager {

void open(const QString& path);
void reveal(const QString& path);

} // namespace FileManager
