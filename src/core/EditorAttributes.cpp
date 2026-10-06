// Attribute einfügen (Alt+V) wie DaVinci „Paste Attributes“, siehe Editor.h
#include "core/Editor.h"

#include "core/I18n.h"
#include "core/Keyframes.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"

#include <QUndoStack>
#include <optional>

namespace {

constexpr const char* kGrade = "grade"; // Effekt der Color-Seite (EffectRegistry)

void copyKeys(Clip& c, const Clip& src, std::initializer_list<AnimParam> params)
{
    for (AnimParam p : params) {
        if (src.keys.contains(p)) c.keys[p] = src.keys[p];
        else c.keys.remove(p);
    }
}

void copyKeyRange(Clip& c, const Clip& src, AnimParam first, AnimParam last)
{
    for (int p = int(first); p <= int(last); ++p) copyKeys(c, src, {AnimParam(p)});
}

std::optional<EffectInstance> gradeOf(const Clip& c, int* index = nullptr)
{
    for (int i = 0; i < c.effects.size(); ++i)
        if (c.effects[i].effectId == kGrade) {
            if (index) *index = i;
            return c.effects[i];
        }
    return std::nullopt;
}

// Effekte ohne die Farbkorrektur übernehmen; die eigene Farbkorrektur bleibt (an der Stelle der Quelle bzw. am Ende)
void pasteEffects(Clip& c, const Clip& src)
{
    const auto own = gradeOf(c);
    QVector<EffectInstance> out;
    bool placed = false;
    for (const EffectInstance& e : src.effects) {
        if (e.effectId != kGrade) out << e;
        else if (own) {
            out << *own;
            placed = true;
        }
    }
    if (own && !placed) out << *own;
    c.effects = out;
    copyKeyRange(c, src, AnimParam::FxBrightness, AnimParam::FxBlur);
    // dynamic effect parameters (frei0r) follow their effects
    c.keys.removeIf([](const auto& it) { return Keys::isEffectParam(it.key()); });
    for (auto it = src.keys.cbegin(); it != src.keys.cend(); ++it)
        if (Keys::isEffectParam(it.key())) c.keys.insert(it.key(), it.value());
}

// Farbkorrektur (Color-Seite) samt LUT übernehmen bzw. entfernen, übrige Effekte bleiben
void pasteColor(Clip& c, const Clip& src)
{
    int at = -1;
    gradeOf(c, &at);
    if (at >= 0) c.effects.removeAt(at);
    if (const auto g = gradeOf(src)) c.effects.insert(at >= 0 ? at : c.effects.size(), *g);
    copyKeyRange(c, src, AnimParam::GradeLiftY, AnimParam::GradeExposure);
}

} // namespace

const Clip* Editor::clipboardSource(const QVector<ClipboardItem>& items, TrackKind kind)
{
    const Clip* best = nullptr; // frühester Start
    for (const auto& it : items)
        if (it.ref.kind == kind && (!best || it.clip.start < best->start)) best = &it.clip;
    return best;
}

int Editor::pasteAttributesAvailable() const
{
    const Clip* v = clipboardSource(m_clipboard, TrackKind::Video);
    const Clip* a = clipboardSource(m_clipboard, TrackKind::Audio);
    int attrs = 0;
    if (v) attrs |= AttrVideoMask | AttrFades;
    if (a) attrs |= AttrAudioMask | AttrFades;
    if ((v && !v->isTitle()) || a) attrs |= AttrSpeed;
    return attrs;
}

QString Editor::pasteAttributesSource() const
{
    const Clip* c = clipboardSource(m_clipboard, TrackKind::Video);
    if (!c) c = clipboardSource(m_clipboard, TrackKind::Audio);
    return c ? m_project->clipName(*c) : QString();
}

void Editor::pasteAttributes(int attrs)
{
    attrs &= pasteAttributesAvailable();
    const QVector<int> ids = editable(clipIdsOf(m_selection->ids()));
    if (!attrs || ids.isEmpty()) return;
    // Kopie der Quellen: die Zwischenablage bleibt, aber Zeiger sollen nicht an ihr hängen
    const QVector<ClipboardItem> clipboard = m_clipboard;
    const Clip* vsrc = clipboardSource(clipboard, TrackKind::Video);
    const Clip* asrc = clipboardSource(clipboard, TrackKind::Audio);
    // Fades und Tempo gelten für beide Spurarten; fehlt eine Quelle dieser Art, kommt der Wert von der anderen
    const Clip* vAny = vsrc ? vsrc : asrc;
    const Clip* aAny = asrc ? asrc : vsrc;

    QSet<int> audioIds;
    QVector<int> videoTargets, audioTargets;
    for (int id : ids) {
        TrackRef ref;
        if (!TimelineOps::findClip(m_project->timeline(), id, &ref)) continue;
        if (ref.kind == TrackKind::Audio) {
            audioIds.insert(id);
            audioTargets << id;
        } else {
            videoTargets << id;
        }
    }

    QUndoStack* undo = m_project->undoStack();
    undo->beginMacro(T("Attribute einfügen"));
    modifyClips(ids, T("Attribute einfügen"), [&](Clip& c) {
        const bool audio = audioIds.contains(c.id);
        if (const Clip* s = audio ? nullptr : vsrc) {
            ClipTransform& t = c.transform;
            const ClipTransform& st = s->transform;
            if (attrs & (AttrZoom | AttrPosition | AttrRotation)) t.transformOn = st.transformOn;
            if (attrs & AttrZoom) {
                t.zoomX = st.zoomX;
                t.zoomY = st.zoomY;
                copyKeys(c, *s, {AnimParam::ZoomX, AnimParam::ZoomY});
            }
            if (attrs & AttrPosition) {
                t.posX = st.posX;
                t.posY = st.posY;
                copyKeys(c, *s, {AnimParam::PosX, AnimParam::PosY});
            }
            if (attrs & AttrRotation) {
                t.rotation = st.rotation;
                copyKeys(c, *s, {AnimParam::Rotation});
            }
            if (attrs & AttrCrop) {
                t.cropLeft = st.cropLeft;
                t.cropRight = st.cropRight;
                t.cropTop = st.cropTop;
                t.cropBottom = st.cropBottom;
                t.cropOn = st.cropOn;
                copyKeys(c, *s, {AnimParam::CropLeft, AnimParam::CropRight, AnimParam::CropTop, AnimParam::CropBottom});
            }
            if (attrs & AttrComposite) {
                t.opacity = st.opacity;
                t.compositeOn = st.compositeOn;
                copyKeys(c, *s, {AnimParam::Opacity});
            }
            if (attrs & AttrEffects) pasteEffects(c, *s);
            if (attrs & AttrColor) pasteColor(c, *s);
        }
        if (const Clip* s = audio ? asrc : nullptr) {
            if (attrs & AttrVolume) {
                c.volumeDb = s->volumeDb;
                copyKeys(c, *s, {AnimParam::Volume});
            }
            if (attrs & AttrPan) {
                c.pan = s->pan;
                copyKeys(c, *s, {AnimParam::Pan});
            }
        }
        if (const Clip* s = audio ? aAny : vAny; s && (attrs & AttrFades)) {
            c.fadeIn = std::min(s->fadeIn, c.length());
            c.fadeOut = std::min(s->fadeOut, c.length());
        }
    });
    if (attrs & AttrSpeed) { // Länge folgt dem Tempo, spätere Clips rücken nach (wie DaVinci)
        auto retime = [](const Clip& s) { return Retime{s.speed, s.reverse, s.freeze, s.keepPitch}; };
        if (!videoTargets.isEmpty() && vAny) setClipSpeed(videoTargets, retime(*vAny), true);
        if (!audioTargets.isEmpty() && aAny) setClipSpeed(audioTargets, retime(*aAny), true);
    }
    undo->endMacro();
}
