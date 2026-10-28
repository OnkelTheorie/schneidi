// SPDX-License-Identifier: AGPL-3.0-only
// A fuzz target without libFuzzer: runs its code (with all its rules) once on every file given, or on every file in
// the folders given. The normal test suite builds each target with this and replays the seeds (corpus/<target>) and
// every input that once found a bug (regressions/<target>), see tests/CMakeLists.txt.
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QStringList>

#include <algorithm>
#include <cstdint>
#include <cstdio>

extern "C" int LLVMFuzzerInitialize(int* argc, char*** argv);
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

int main(int argc, char* argv[])
{
    LLVMFuzzerInitialize(&argc, &argv);
    QStringList files;
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (!QFileInfo::exists(arg)) continue; // e.g. no regressions/<target> yet
        if (!QFileInfo(arg).isDir()) {
            files << arg;
            continue;
        }
        QStringList inDir;
        for (QDirIterator it(arg, QDir::Files); it.hasNext();) inDir << it.next();
        std::sort(inDir.begin(), inDir.end());
        files << inDir;
    }
    for (const QString& path : files) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            std::printf("FEHLER: cannot read %s\n", qPrintable(path));
            return 1;
        }
        const QByteArray data = f.readAll();
        std::printf("%s\n", qPrintable(QFileInfo(path).fileName()));
        std::fflush(stdout);
        LLVMFuzzerTestOneInput(reinterpret_cast<const uint8_t*>(data.constData()), size_t(data.size()));
    }
    std::printf("ok: %d inputs\n", int(files.size()));
    return files.isEmpty() ? 1 : 0;
}
