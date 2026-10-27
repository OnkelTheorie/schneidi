#pragma once
// Still images (PNG, JPG, WebP, …) as an own MLT producer instead of MLT's qimage (Qt module, GPL). Reads the file
// with Qt's QImageReader (EXIF orientation applied), keeps the decoded picture as 8-bit RGBA with straight alpha and
// hands it out at the size MLT's rescaler asks for (scaled once per size with Qt's smooth scaling, then reused), so
// a still on the timeline costs nothing per frame. Registered with MLT's factory as "schneidi_still", and as "qimage"
// where MLT's Qt module is not loaded: the loader (loader.dict maps picture suffixes to qimage) then opens every
// picture with it, with its normalizing filters as before, and old graphs naming qimage keep working.

namespace Still {

constexpr const char* kService = "schneidi_still";

// After Mlt::Factory::init (Bundle::prepareMltCaches)
void registerService();
// mlt_service of a producer that shows a still picture (own, or MLT's qimage/pixbuf in old graphs)
bool isStillService(const char* service);

} // namespace Still
