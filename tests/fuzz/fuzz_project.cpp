// SPDX-License-Identifier: AGPL-3.0-only
// Fuzz target: project files (.schneidi, core/ProjectFile.h) as others or an AI may write them. A file is read
// (migrations included), opened as a project (Project::load sorts out broken references), checked like the server
// does for its work folder (Sandbox::checkProject), saved and read again, and every timeline is written in the
// exchange formats (EDL, FCP7 XML, OTIO).
//
// Rule checked: saving is stable - what a saved file loads as is saved the same way again (nothing drifts or gets
// lost with every save).
#include "FuzzCommon.h"
#include "FuzzJson.h"

#include "cli/Commands.h"
#include "cli/Sandbox.h"
#include "core/Interchange.h"
#include "core/Project.h"
#include "core/ProjectFile.h"

#include <QDir>
#include <QTemporaryDir>

namespace {

QString& projectPath()
{
    static QString path;
    return path;
}

QByteArray saveLoaded(const QByteArray& json)
{
    ProjectData d;
    QString error;
    if (!ProjectFile::fromJson(json, projectPath(), &d, &error)) return {};
    Project p;
    p.load(d);
    return ProjectFile::toJson(p.data(), projectPath());
}

} // namespace

FUZZ_JSON_MUTATOR

extern "C" int LLVMFuzzerInitialize(int*, char***)
{
    Fuzz::init("project");
    static QTemporaryDir dir;
    if (!dir.isValid()) std::abort();
    QDir(dir.path()).mkpath("work/media");
    projectPath() = QDir(dir.path()).filePath("work/project.schneidi");
    Cli::Sandbox::configure({QDir(dir.path()).filePath("work"), {}, false});
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    const QByteArray json(reinterpret_cast<const char*>(data), qsizetype(size));
    ProjectData d;
    QString error;
    if (!ProjectFile::fromJson(json, projectPath(), &d, &error)) return 0;
    (void)ProjectFile::missingMedia(d);
    (void)ProjectFile::missingEffects(d);
    Project p;
    p.load(d);
    try {
        Cli::Sandbox::checkProject(p);
    } catch (const Cli::Error&) {
    }

    const QByteArray once = ProjectFile::toJson(p.data(), projectPath());
    FUZZ_ASSERT(Fuzz::sameText(once, saveLoaded(once)));

    for (const Sequence& s : p.sequences())
        for (const Interchange::Format f : {Interchange::Format::Edl, Interchange::Format::Fcp7, Interchange::Format::Otio}) {
            Interchange::Sequence seq = Interchange::fromTimeline(p, s.id);
            (void)Interchange::write(seq, f);
        }
    return 0;
}
