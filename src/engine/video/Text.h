#pragma once
// Text (titles, subtitles) as an own MLT filter instead of MLT's qtext (Qt module, GPL): draws lines of text with
// Qt's QPainter (LGPL) onto the frame's picture. Same properties as qtext, so the timeline code sets them unchanged:
// "argument" (text, lines split at \n), "geometry" (profile pixels "x y w h", keyframed: the area the text block is
// aligned in), "family", "size" (pixels), "weight" (400/700), "style" (normal/italic), "halign" (left/center/right:
// the lines within the block and the block in the area), "valign" (top/middle/bottom), "fgcolour" (keyframed),
// "bgcolour" (box behind the block), "pad" (box margin), "olcolour", "outline" (pen width, centered on the glyph
// edges). Own addition: "layout_outline" (the outline width the layout makes room for, default "outline": stacked
// layers of one title share it and so the scale and origin). Font, outline and margin scale with the requested size (a preview frame looks like the export one).

namespace Mlt {
class Filter;
class Profile;
} // namespace Mlt

namespace Text {

Mlt::Filter filter(Mlt::Profile& profile);
// With MLT's factory: "schneidi_text", and as "qtext" where MLT's Qt module is not loaded
void registerService();

} // namespace Text
