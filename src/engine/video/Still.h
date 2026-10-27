#pragma once
// Still images (PNG, JPG, WebP, …) as an own MLT producer instead of MLT's qimage (Qt module, GPL). Reads the file
// with Qt's QImageReader (EXIF orientation applied) on first use, keeps the decoded picture as 8-bit RGBA with straight
// alpha and hands out a copy at its own size (the loader's normalizers scale it). Registered with MLT's factory as "schneidi_still", and as "qimage"
// where MLT's Qt module is not loaded: the loader (loader.dict maps picture suffixes to qimage) then opens every
// picture with it, with its normalizing filters as before, and old graphs naming qimage keep working.

namespace Still {

constexpr const char* kService = "schneidi_still";

// After Mlt::Factory::init (Bundle::prepareMltCaches)
void registerService();
// mlt_service of a producer that shows a still picture (own, or MLT's qimage/pixbuf in old graphs)
bool isStillService(const char* service);

} // namespace Still
