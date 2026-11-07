// SPDX-License-Identifier: AGPL-3.0-only
// Trust records of files (core/CodeTrust.h, 0.19): an effect preset the user saved from trusted custom code keeps it
// trusted when applied in a later run, someone else's preset does not; saving a template leaves no record for its
// temporary staging file; records of deleted files are pruned (at most once a day).
#include "core/CodeTrust.h"
#include "core/EffectRegistry.h"
#include "core/Presets.h"
#include "core/Project.h"
#include "core/TemplateLibrary.h"

#include "check.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>

namespace {

Clip filterClip(const QString& chain)
{
    Clip c;
    c.id = 1;
    c.mediaPath = "/x/a.mp4";
    c.out = 99;
    EffectInstance e;
    e.effectId = "ffmpeg_filter";
    e.params["chain"] = chain;
    c.effects << e;
    return c;
}

// The files of all trust records
QStringList recordFiles()
{
    QSettings s;
    s.beginGroup("trustfile");
    QStringList out;
    for (const QString& k : s.childKeys()) out << s.value(k).toString();
    return out;
}
int recordCount()
{
    QSettings s;
    s.beginGroup("trust");
    return int(s.childKeys().size());
}
void clearRecords()
{
    QSettings().remove("trust");
    QSettings().remove("trustfile");
    QSettings().remove("trust_pruned");
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QCoreApplication app(argc, argv);
    Check::initApp("trust_records");
    clearRecords();
    QTemporaryDir dir;

    // A preset saved from the user's own (trusted) code: trusted again when loaded in a later run
    {
        const QString mine = dir.filePath("mine.xml");
        CodeTrust::resetForTests();
        CodeTrust::trust("negate");
        QString err;
        CHECK(Presets::save(filterClip("negate"), "Mine", mine, &err));
        CHECK(CodeTrust::remembered(mine, {"negate"}));
        CodeTrust::resetForTests(); // a new run
        CHECK(!CodeTrust::isTrusted("negate"));
        const Presets::Preset p = Presets::load(mine);
        const Presets::Mapped m = Presets::map(p);
        CHECK_EQ(m.effects.size(), 1);
        CHECK_EQ(m.effects.value(0).params.value("chain").toString(), QString("negate"));
        CHECK(CodeTrust::isTrusted("negate"));
    }
    // Someone else's preset (no record; or code the user never trusted): stays untrusted when loaded
    {
        const QString other = dir.filePath("other.xml");
        CodeTrust::resetForTests();
        CHECK(Presets::save(filterClip("hue=s=0"), "Other", other, nullptr));
        CHECK(!CodeTrust::remembered(other, {"hue=s=0"}));
        Presets::load(other);
        CHECK(!CodeTrust::isTrusted("hue=s=0"));
        CHECK(!CodeTrust::trustWritten("hue=s=0", {})); // seen untrusted in a file: not the caller's own
    }

    // A template with trusted code: a record for the template's project only, none for the staging file
    {
        clearRecords();
        CodeTrust::resetForTests();
        CodeTrust::trust("negate");
        ProjectData d;
        d.timeline = emptyTimeline();
        d.timeline.video[0].clips << filterClip("negate");
        QString err, code;
        const auto saved = TemplateLibrary::save(d, "Trust record test", {}, &err, &code);
        if (CHECK(saved.has_value())) {
            CHECK(CodeTrust::remembered(saved->project, {"negate"}));
            CHECK_EQ(recordCount(), 1);
            for (const QString& f : recordFiles())
                if (!CHECK(QFileInfo::exists(f))) std::printf("       record of a missing file %s\n", qPrintable(f));
            TemplateLibrary::remove(saved->id);
        }
    }

    // Prune: records of deleted files go, others stay; once a day unless forced
    {
        clearRecords();
        const QString kept = dir.filePath("kept.schneidi"), gone = dir.filePath("gone.schneidi");
        for (const QString& f : {kept, gone}) {
            QFile file(f);
            CHECK(file.open(QIODevice::WriteOnly));
            CodeTrust::remember(f, {"negate"});
        }
        CHECK_EQ(recordCount(), 2);
        CHECK_EQ(CodeTrust::prune(), 0); // all files there
        QFile::remove(gone);
        CHECK_EQ(CodeTrust::prune(), 0); // done today already
        CHECK(CodeTrust::remembered(gone, {"negate"}));
        CHECK_EQ(CodeTrust::prune(true), 1);
        CHECK(!CodeTrust::remembered(gone, {"negate"}));
        CHECK(CodeTrust::remembered(kept, {"negate"}));
        CHECK_EQ(recordCount(), 1);
        CHECK_EQ(recordFiles().size(), 1);
        CodeTrust::forget(kept);
        CHECK(!CodeTrust::remembered(kept, {"negate"}));
        CHECK_EQ(recordCount(), 0);
    }
    clearRecords();
    return Check::result();
}
