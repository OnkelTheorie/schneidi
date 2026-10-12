#pragma once
// Single frames without a preview (schneidi-cli: lets an AI look at the material). Own profile and producers,
// blocking; transparent areas are black like in the viewer.

#include "core/ProjectFormat.h"
#include "core/Types.h"

#include <QImage>
#include <QString>

#include <memory>

namespace Mlt {
class Producer;
class Profile;
} // namespace Mlt
class ProducerFactory;
class TimelineBuilder;

namespace Snapshot {

// Several frames of one source: opens the file or builds the timeline once (opening a long 60 fps capture or
// building an edit with speed ramps takes seconds, too long to repeat for every frame of a contact sheet)
class Source {
public:
    static std::unique_ptr<Source> media(const ProjectFormat& format, const QString& path);
    // Only the picture is built (the sound's producers are not needed for a frame)
    static std::unique_ptr<Source> timeline(const ProjectFormat& format, const Timeline& tl);
    ~Source();
    QImage frame(int frame, int maxEdge = 0);

private:
    Source();
    ProjectFormat m_format;
    std::unique_ptr<Mlt::Profile> m_profile;
    std::unique_ptr<ProducerFactory> m_factory;
    std::unique_ptr<TimelineBuilder> m_builder;
    std::unique_ptr<Mlt::Producer> m_producer;
};

// Frame `frame` (in the rate of `format`) of a media file, fitted into the project aspect ratio
QImage media(const ProjectFormat& format, const QString& path, int frame, int maxEdge = 0);
// Frame of a timeline (use Project::renderTimeline so compound clips render)
QImage timeline(const ProjectFormat& format, const Timeline& tl, int frame, int maxEdge = 0);

} // namespace Snapshot
