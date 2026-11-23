// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/timeline/PlayheadMove.h"

#include "engine/Engine.h"

#include <QSettings>
#include <utility>

namespace PlayheadMove {

namespace {
constexpr const char* kKey = "timeline/afterPlayheadMove";
}

Mode mode()
{
    const QString v = QSettings().value(kKey).toString();
    if (v == "continue") return Mode::Continue;
    if (v == "stop") return Mode::Stop;
    if (v == "play") return Mode::Play;
    return Mode::Standard;
}

void setMode(Mode mode)
{
    const char* v = mode == Mode::Continue ? "continue" : mode == Mode::Stop ? "stop" : mode == Mode::Play ? "play" : nullptr;
    if (v) QSettings().setValue(kKey, v);
    else QSettings().remove(kKey); // Standard: nothing stored (a later default change reaches it)
}

void Gesture::begin(Engine* engine, Source source) { begin(engine, source, mode()); }

void Gesture::begin(Engine* engine, Source source, Mode mode)
{
    const bool playing = engine->speed() != 0.0;
    const bool after = playsAfter(mode, source, playing);
    m_active = true;
    m_startAfter = after && !playing;
    if (playing && !after) engine->pause(); // before the seek: no sound from the new position
}

void Gesture::end(Engine* engine)
{
    if (!std::exchange(m_active, false)) return;
    if (std::exchange(m_startAfter, false) && engine->speed() == 0.0 && !engine->atPlayEnd()) engine->play();
}

void jump(Engine* engine, Source source, const std::function<void()>& seek)
{
    Gesture g;
    g.begin(engine, source);
    seek();
    g.end(engine);
}

} // namespace PlayheadMove
