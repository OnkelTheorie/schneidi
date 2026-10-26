#pragma once
// Settings of the AI features (Settings › AI), the same for all projects (QSettings "ai/…"). Read once at start and
// after the settings dialog; test runs change them only in memory (MainWindow does not save them then).
// The AI chat (plan 0.12, point 4) puts its provider settings next to these.

#include <QColor>

struct AiSettings {
    // AI indicator in the top bar: always (also to open the log), only while an AI is connected or works, never
    enum class Indicator { Always, WhileActive, Never };

    bool liveAllowed = true; // AIs may work on the project open in the app (schneidi-cli, MCP)
    bool highlight = true;   // light up what an AI changes in the timeline
    QColor color = defaultColor();
    int highlightMs = 2000;  // how long it stays lit (fades out)
    Indicator indicator = Indicator::WhileActive;

    static QColor defaultColor(); // violet, like the AI indicator
    static constexpr int kMinHighlightMs = 300;
    static constexpr int kMaxHighlightMs = 10000;

    static AiSettings load();
    void save() const;
    bool operator==(const AiSettings&) const = default;
};
