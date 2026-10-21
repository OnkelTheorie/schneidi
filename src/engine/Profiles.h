#pragma once
// Detect the MLT profile from the project settings (preview, export, cache) and the format of a clip
// (for "adapt project to the first clip" like DaVinci).

#include "core/ProjectFormat.h"
#include "core/Types.h"

#include <QString>

#include <map>
#include <memory>
#include <mutex>

namespace Mlt {
class Producer;
class Profile;
}

// Audio streams of an opened file (from the MLT metadata, in file order)
QVector<AudioStreamInfo> audioStreamsOf(Mlt::Producer& p);
// Plays the n-th audio stream (0 = first) instead of the default stream; if it is missing (file replaced), it stays silent.
// Also works with timewarp producers (they pass audio_index on to the file).
void selectAudioStream(Mlt::Producer& p, int stream);

// Square pixels, progressive, BT.709
std::unique_ptr<Mlt::Profile> makeProfile(const ProjectFormat& format);

// Opens media files for images. Workaround for an MLT bug (7.30, filter_swscale): when MLT scales a BT.601 source
// in a 709 profile, it converts the colors to 709 but leaves the tag at 601 -> wrong colors at any size other than the
// original (preview, 720p/4K export, thumbnails). The scaler takes the target standard from the
// profile of the producer, so such sources get an otherwise identical profile with their own color standard.
// The profiles belong to the factory: it must outlive all producers opened with it.
class ProducerFactory {
public:
    explicit ProducerFactory(Mlt::Profile& base);
    ~ProducerFactory();
    // Thread-safe (TimelineBuilder::prefetch opens several files at once)
    std::unique_ptr<Mlt::Producer> open(const QString& resource);
    // Threads per video decoder (MLT "threads"); 0 = MLT default (preview)
    void setThreads(int threads) { m_threads = threads; }

private:
    Mlt::Profile& m_base;
    int m_threads = 0;
    std::map<int, std::unique_ptr<Mlt::Profile>> m_profiles; // color standard -> profile
    std::mutex m_mutex;                                       // guards m_profiles
};

struct ClipFormat {
    bool ok = false;      // video (no still image/audio) with a readable format
    int width = 0, height = 0; // as displayed (rotation from the metadata taken into account)
    FrameRate rate;       // frame rate according to the file (the highest with variable frame rate)
    double averageFps = 0; // average frame rate (0 = unknown)
    bool variable = false; // variable frame rate (phone)
    FrameRate suggested;  // nearest standard frame rate for the project
};
ClipFormat detectClipFormat(const QString& path);
