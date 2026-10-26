#pragma once
// The editing guide for AIs, built into the program: the Claude skill of the repo (skills/schneidi: SKILL.md + one
// reference per task) as Qt resources. Served as the MCP tool `guide`, installed as a skill by `schneidi-cli setup`
// and read by the app's own AI chat, so every AI gets the same text in the version that matches the program.

#include <QString>
#include <QStringList>

namespace Cli::Guide {

// Task topics ("ad", "audio", "highlights" …) = the reference files
QStringList topics();
// First heading of a topic ("Ad, product or marketing clip")
QString title(const QString& topic);
// Empty topic: the main guide (SKILL.md without its front matter); otherwise the topic's text. Unknown: empty.
QString text(const QString& topic = {});

// Writes the skill as files into dir (dir/SKILL.md, dir/references/…), replacing an older copy there
bool installSkill(const QString& dir, QString* error);

} // namespace Cli::Guide
