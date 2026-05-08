#include "havok_xml.hpp"
#include "hklib/hka_animation.hpp"
#include "hklib/hka_animationbinding.hpp"
#include "hklib/hka_animationcontainer.hpp"
#include "hklib/hka_annotationtrack.hpp"
#include "hklib/hka_skeleton.hpp"
#include "internal/hka_animatedreferenceframe.hpp"
#include "internal/hka_animation.hpp"
#include "internal/hka_deltaanimation.hpp"
#include "internal/hka_interleavedanimation.hpp"
#include "internal/hka_splineanimation.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#define HEAT_API extern "C" __declspec(dllexport)
#else
#define HEAT_API extern "C" __attribute__((visibility("default")))
#endif

namespace {

thread_local std::string g_lastError;

struct Binding : hkaAnimationBindingInternalInterface {
  DECLARE_XMLCLASS(Binding, hkaAnimationBinding)

  hkaAnimation *anim{};
  std::string skelName;
  std::vector<int16> trackMap;

  std::string_view GetSkeletonName() const override { return skelName; }
  const hkaAnimation *GetAnimation() const override { return anim; }
  BlendHint GetBlendHint() const override { return NORMAL; }
  size_t GetNumTransformTrackToBoneIndices() const override {
    return trackMap.size();
  }
  int16 GetTransformTrackToBoneIndex(size_t id) const override {
    return trackMap[id];
  }
  size_t GetNumFloatTrackToFloatSlotIndices() const override { return 0; }
  int16 GetFloatTrackToFloatSlotIndex(size_t) const override { return -1; }
  size_t GetNumPartitionIndices() const override { return 0; }
  int16 GetPartitionIndex(size_t) const override { return -1; }
};

struct Preset {
  hkToolset toolset;
  uint32 rule;
};

enum GameType {
  GENERATIONS_PC_GAME_TYPE = 0,
  UNLEASHED_360_GAME_TYPE = 1,
  GENERATIONS_360_GAME_TYPE = 2,
  GENERATIONS_PS3_GAME_TYPE = 3,
  UNLEASHED_PS3_GAME_TYPE = 4,
  LOST_WORLD_PC_GAME_TYPE = 5,
  LOST_WORLD_WIIU_GAME_TYPE = 6,
};

constexpr Preset GENERATIONS_PC{HK2010_2, 0x4101};
constexpr Preset GENERATIONS_360{HK2010_2, 0x4001};
constexpr Preset GENERATIONS_PS3{HK2010_2, 0x4011};
constexpr Preset UNLEASHED_360{HK550, 0x4001};
constexpr Preset UNLEASHED_PS3{HK550, 0x4011};
constexpr Preset LOST_WORLD_PC{HK2012_2, 0x4101};
constexpr Preset LOST_WORLD_WIIU{HK2012_2, 0x4001};

Preset GetPreset(int preset) {
  switch (preset) {
  case GENERATIONS_360_GAME_TYPE:
    return GENERATIONS_360;
  case GENERATIONS_PS3_GAME_TYPE:
    return GENERATIONS_PS3;
  case UNLEASHED_PS3_GAME_TYPE:
    return UNLEASHED_PS3;
  case LOST_WORLD_PC_GAME_TYPE:
    return LOST_WORLD_PC;
  case LOST_WORLD_WIIU_GAME_TYPE:
    return LOST_WORLD_WIIU;
  case UNLEASHED_360_GAME_TYPE:
    return UNLEASHED_360;
  default:
    return GENERATIONS_PC;
  }
}

hkaSplineCompressionSettings GetSplineSettings(int preset) {
  hkaSplineCompressionSettings settings;
  settings.maxFramesPerBlock = 256;
  settings.keyStep = 7;
  if (preset == UNLEASHED_360_GAME_TYPE || preset == UNLEASHED_PS3_GAME_TYPE) {
    settings.keyStep = 2;
  }
  return settings;
}

void SetError(std::string message) { g_lastError = std::move(message); }

template <class Fn> int Guard(Fn &&fn) {
  g_lastError.clear();
  return fn();
}

void AppendVector(std::string &out, const Vector4A16 &v, int count) {
  out += '[';
  out += std::to_string(v.X);
  if (count > 1) {
    out += ',';
    out += std::to_string(v.Y);
  }
  if (count > 2) {
    out += ',';
    out += std::to_string(v.Z);
  }
  if (count > 3) {
    out += ',';
    out += std::to_string(v.W);
  }
  out += ']';
}

void AppendRootMotionJson(
    std::string &out,
    const hkaAnimatedReferenceFrameInternalInterface *motion) {
  if (!motion) {
    return;
  }

  out += ",\"rootMotion\":{";
  out += "\"duration\":";
  out += std::to_string(motion->GetDuration());
  out += ",\"up\":";
  AppendVector(out, motion->GetUp(), 4);
  out += ",\"forward\":";
  AppendVector(out, motion->GetForward(), 4);
  out += ",\"samples\":[";

  const size_t numFrames = motion->GetNumFrames();
  for (size_t i = 0; i < numFrames; ++i) {
    if (i) {
      out += ',';
    }
    AppendVector(out, motion->GetRefFrame(i), 4);
  }

  out += "]}";
}

int CopyResult(const std::string &payload, char *out, uint32_t capacity,
               uint32_t *required) {
  const auto need = static_cast<uint32_t>(payload.size() + 1);
  if (required) {
    *required = need;
  }

  if (!out || capacity == 0) {
    return 1;
  }

  if (capacity < need) {
    SetError("Output buffer is too small");
    return 0;
  }

  std::copy(payload.begin(), payload.end(), out);
  out[payload.size()] = '\0';
  return 1;
}

template <class T> const T *FirstClass(IhkPackFile &pack, JenHash hash) {
  for (auto *cls : pack.GetClasses(hash)) {
    if (auto *typed = safe_deref_cast<const T>(cls)) {
      return typed;
    }
  }

  return nullptr;
}

const hkaSkeleton *FindSkeleton(IhkPackFile &pack) {
  if (auto *container =
          FirstClass<hkaAnimationContainer>(pack, hkaAnimationContainer::GetHash())) {
    if (container->GetNumSkeletons()) {
      return container->GetSkeleton(0);
    }
  }

  return FirstClass<hkaSkeleton>(pack, hkaSkeleton::GetHash());
}

const hkaAnimationContainer *FindContainer(IhkPackFile &pack) {
  return FirstClass<hkaAnimationContainer>(pack, hkaAnimationContainer::GetHash());
}

std::string SkeletonToJson(const hkaSkeleton &skeleton) {
  std::string out = "{\"type\":\"skeleton\",\"name\":\"";
  out += skeleton.Name();
  out += "\",\"bones\":[";

  for (size_t i = 0; i < skeleton.GetNumBones(); ++i) {
    const hkQTransform *tm = skeleton.GetBoneTM(i);
    if (i) {
      out += ',';
    }

    out += "{\"name\":\"";
    out += skeleton.GetBoneName(i);
    out += "\",\"parent\":";
    out += std::to_string(skeleton.GetBoneParentID(i));
    out += ",\"translation\":";
    AppendVector(out, tm->translation, 3);
    out += ",\"rotation\":";
    AppendVector(out, tm->rotation, 4);
    out += ",\"scale\":";
    AppendVector(out, tm->scale, 3);
    out += '}';
  }

  out += "]}";
  return out;
}

const hkaAnimationBinding *FindBindingForAnimation(const hkaAnimationContainer &c,
                                                  const hkaAnimation *anim) {
  for (size_t i = 0; i < c.GetNumBindings(); ++i) {
    auto *binding = c.GetBinding(i);
    if (binding && (!anim || binding->GetAnimation() == anim)) {
      return binding;
    }
  }

  return nullptr;
}

std::string NormalizeBoneName(std::string_view name) {
  std::string out{name};
  const size_t lt = out.size() >= 3 ? out.size() - 3 : std::string::npos;
  if (lt != std::string::npos && out.substr(lt) == "@LT") {
    out.resize(lt);
  }

  for (char &ch : out) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }

  return out;
}

std::vector<int16> BuildTrackMap(const hkaAnimation &anim,
                                 const hkaAnimationBinding *binding,
                                 const hkaSkeleton *skeleton) {
  const size_t numTracks = anim.GetNumOfTransformTracks();
  std::vector<int16> map(numTracks, -1);

  if (binding && binding->GetNumTransformTrackToBoneIndices()) {
    const size_t count =
        std::min(numTracks, binding->GetNumTransformTrackToBoneIndices());
    for (size_t i = 0; i < count; ++i) {
      map[i] = binding->GetTransformTrackToBoneIndex(i);
    }
    return map;
  }

  if (skeleton && anim.GetNumAnnotations() == numTracks) {
    std::unordered_map<std::string, int16> bones;
    bones.reserve(skeleton->GetNumBones());

    for (size_t bone = 0; bone < skeleton->GetNumBones(); ++bone) {
      std::string name = NormalizeBoneName(skeleton->GetBoneName(bone));
      if (!name.empty() && bones.find(name) == bones.end()) {
        bones.emplace(std::move(name), static_cast<int16>(bone));
      }
    }

    for (size_t track = 0; track < numTracks; ++track) {
      auto annot = anim.GetAnnotation(track);
      if (!annot) {
        continue;
      }

      const auto it = bones.find(NormalizeBoneName(annot->GetName()));
      if (it != bones.end()) {
        map[track] = it->second;
      }
    }

    if (std::any_of(map.begin(), map.end(), [](int16 item) { return item >= 0; })) {
      return map;
    }
  }

  for (size_t i = 0; i < numTracks; ++i) {
    map[i] = static_cast<int16>(i);
  }

  return map;
}

size_t AnimationFrameCount(const hkaAnimation *animation) {
  if (!animation) {
    return 0;
  }

  const auto *internal =
      dynamic_cast<const hkaAnimationInternalInterface *>(animation);
  if (!internal) {
    return 0;
  }

  if (const auto *interleaved =
          dynamic_cast<const hkaInterleavedAnimationInternalInterface *>(
              internal)) {
    const size_t tracks = interleaved->GetNumOfTransformTracks();
    return tracks ? interleaved->GetNumTransforms() / tracks : 0;
  }

  if (const auto *spline =
          dynamic_cast<const hkaSplineCompressedAnimationInternalInterface *>(
              internal)) {
    return spline->GetNumFrames();
  }

  if (const auto *delta =
          dynamic_cast<const hkaDeltaCompressedAnimationInternalInterface *>(
              internal)) {
    return delta->GetNumOfPoses();
  }

  if (const auto *lerp = dynamic_cast<const hkaAnimationLerpSampler *>(internal)) {
    return lerp->numFrames;
  }

  const uint32 frameRate = animation->FrameRate();
  return frameRate
             ? static_cast<size_t>(
                   std::round(animation->Duration() *
                              static_cast<float>(frameRate))) +
                   1
             : 0;
}

std::string AnimationToJson(IhkPackFile &animPack, const hkaSkeleton *skeleton) {
  const hkaAnimationContainer *container = FindContainer(animPack);
  const hkaAnimation *animation = nullptr;

  if (container && container->GetNumAnimations()) {
    animation = container->GetAnimation(0);
    if (!skeleton && container->GetNumSkeletons()) {
      skeleton = container->GetSkeleton(0);
    }
  }

  if (!animation) {
    animation = FirstClass<hkaAnimation>(animPack, hkaAnimation::GetHash());
  }

  if (!animation) {
    SetError("No hkaAnimation was found");
    return {};
  }

  if (skeleton) {
    const_cast<hkaAnimation *>(animation)->SetReferenceSkeleton(skeleton);
  }

  const auto *internal =
      dynamic_cast<const hkaAnimationInternalInterface *>(animation);
  if (!internal) {
    SetError("Unexpected error, report to developer(s).");
    return {};
  }

  const size_t sourceFrameCount = AnimationFrameCount(animation);
  const float duration = animation->Duration();
  uint32_t fps = animation->FrameRate();
  if (!fps && duration > 0.0f && sourceFrameCount > 1) {
    fps = static_cast<uint32_t>(
        std::round(static_cast<float>(sourceFrameCount - 1) / duration));
  }
  if (!fps) {
    fps = 30;
  }

  size_t frameCount =
      std::max<size_t>(1, static_cast<size_t>(duration * fps + 0.5f) + 1);

  if (frameCount <= 1 && sourceFrameCount > 1) {
    frameCount = sourceFrameCount;
  }

  const auto *binding =
      container ? FindBindingForAnimation(*container, animation) : nullptr;
  const auto trackMap = BuildTrackMap(*animation, binding, skeleton);

  std::string out = "{\"type\":\"animation\",\"duration\":";
  out += std::to_string(duration);
  out += ",\"fps\":";
  out += std::to_string(fps);
  out += ",\"frames\":";
  out += std::to_string(frameCount);
  AppendRootMotionJson(
      out, dynamic_cast<const hkaAnimatedReferenceFrameInternalInterface *>(
               animation->GetExtractedMotion()));
  const size_t outputTrackCount =
      skeleton ? skeleton->GetNumBones() : animation->GetNumOfTransformTracks();
  std::vector<std::vector<uni::RTSValue>> sampledFrames(
      frameCount, std::vector<uni::RTSValue>(outputTrackCount));

  for (uint32_t frame = 0; frame < frameCount; ++frame) {
    auto &sampled = sampledFrames[frame];

    for (size_t bone = 0; bone < outputTrackCount; ++bone) {
      if (skeleton) {
        if (const hkQTransform *ref = skeleton->GetBoneTM(bone)) {
          sampled[bone] =
              uni::RTSValue(ref->translation, ref->rotation, ref->scale);
        } else {
          sampled[bone] = uni::RTSValue();
        }
      } else {
        sampled[bone] = uni::RTSValue();
      }
    }

    const float time = frame / static_cast<float>(fps);
    const size_t trackCount = animation->GetNumOfTransformTracks();
    for (size_t track = 0; track < trackCount; ++track) {
      size_t boneIndex = track;
      if (track < trackMap.size() && trackMap[track] >= 0) {
        boneIndex = static_cast<size_t>(trackMap[track]);
      }

      if (boneIndex >= sampled.size()) {
        continue;
      }

      internal->GetValue(sampled[boneIndex], time, track);
    }
  }

  out += ",\"tracks\":[";

  for (size_t track = 0; track < outputTrackCount; ++track) {
    if (track) {
      out += ',';
    }

    const int16 boneIndex = static_cast<int16>(track);
    out += "{\"bone\":";
    out += std::to_string(boneIndex);
    out += ",\"name\":\"";
    if (skeleton && boneIndex >= 0 &&
        static_cast<size_t>(boneIndex) < skeleton->GetNumBones()) {
      out += skeleton->GetBoneName(static_cast<size_t>(boneIndex));
    } else if (track < animation->GetNumAnnotations()) {
      auto annot = animation->GetAnnotation(track);
      if (annot) {
        out += annot->GetName();
      }
    }
    out += "\",\"samples\":[";

    for (uint32_t frame = 0; frame < frameCount; ++frame) {
      if (frame) {
        out += ',';
      }

      const uni::RTSValue &value = sampledFrames[frame][track];
      out += '[';
      out += std::to_string(value.translation.X) + "," +
             std::to_string(value.translation.Y) + "," +
             std::to_string(value.translation.Z) + ",";
      out += std::to_string(value.rotation.X) + "," +
             std::to_string(value.rotation.Y) + "," +
             std::to_string(value.rotation.Z) + "," +
             std::to_string(value.rotation.W) + ",";
      out += std::to_string(value.scale.X) + "," + std::to_string(value.scale.Y) +
             "," + std::to_string(value.scale.Z);
      out += ']';
    }

    out += "]}";
  }

  out += "]}";
  return out;
}

std::vector<std::string_view> SplitTabs(std::string_view line) {
  std::vector<std::string_view> parts;
  while (true) {
    const size_t pos = line.find('\t');
    parts.emplace_back(line.substr(0, pos));
    if (pos == std::string_view::npos) {
      break;
    }
    line.remove_prefix(pos + 1);
  }
  return parts;
}

float ParseFloat(std::string_view value, float fallback = 0.0f) {
  float out = fallback;
  std::from_chars(value.data(), value.data() + value.size(), out);
  return out;
}

int ParseInt(std::string_view value, int fallback = 0) {
  int out = fallback;
  std::from_chars(value.data(), value.data() + value.size(), out);
  return out;
}

void AddRootAndContainer(xmlRootLevelContainer *root,
                         xmlAnimationContainer *container,
                         std::string_view variant) {
  root->AddVariant(container, variant);
}

xmlSkeleton *ParseSkeleton(xmlHavokFile &file, std::string_view text) {
  auto *skeleton = file.NewClass<xmlSkeleton>();
  std::vector<int> parents;

  std::stringstream stream{std::string(text)};
  std::string line;

  while (std::getline(stream, line)) {
    auto cols = SplitTabs(line);
    if (cols.empty()) {
      continue;
    }

    if (cols[0] == "skeleton" && cols.size() > 1) {
      skeleton->name = std::string(cols[1]);
      continue;
    }

    if (cols[0] != "bone" || cols.size() < 12) {
      continue;
    }

    auto bone = std::make_unique<xmlBone>();
    bone->ID = static_cast<int16>(skeleton->bones.size());
    parents.push_back(ParseInt(cols[1], -1));
    bone->name = std::string(cols[2]);
    bone->transform.translation =
        Vector4A16(ParseFloat(cols[3]), ParseFloat(cols[4]), ParseFloat(cols[5]), 0.0f);
    bone->transform.rotation =
        Vector4A16(ParseFloat(cols[6]), ParseFloat(cols[7]), ParseFloat(cols[8]),
                   ParseFloat(cols[9], 1.0f));
    bone->transform.scale =
        Vector4A16(ParseFloat(cols[10], 1.0f), ParseFloat(cols[11], 1.0f),
                   ParseFloat(cols.size() > 12 ? cols[12] : "1", 1.0f), 0.0f);
    skeleton->bones.emplace_back(std::move(bone));
  }

  for (size_t i = 0; i < skeleton->bones.size(); ++i) {
    const int parent = i < parents.size() ? parents[i] : -1;
    if (parent >= 0 && static_cast<size_t>(parent) < skeleton->bones.size()) {
      skeleton->bones[i]->parent = skeleton->bones[parent].get();
    }
  }

  return skeleton;
}

struct AnimLine {
  int track = 0;
  int frame = 0;
  hkQTransform value;
};

struct RootMotionData {
  bool has = false;
  float duration = 0.0f;
  Vector4A16 up = Vector4A16(0.0f, 1.0f, 0.0f, 0.0f);
  Vector4A16 forward = Vector4A16(0.0f, 0.0f, 1.0f, 0.0f);
  std::vector<Vector4A16> samples;
};

void ParseAnimationData(xmlHavokFile &file, std::string_view text,
                        xmlAnimationContainer *container,
                        std::string skeletonName, int preset, bool compress) {
  std::unique_ptr<xmlInterleavedAnimation> tempAnim;
  xmlInterleavedAnimation *anim = nullptr;

  if (compress) {
    tempAnim = std::make_unique<xmlInterleavedAnimation>();
    anim = tempAnim.get();
  } else {
    anim = file.NewClass<xmlInterleavedAnimation>();
  }

  anim->animType = HK_INTERLEAVED_ANIMATION;
  anim->frameRate = 30;
  anim->duration = 0.0f;
  anim->numFrames = 1;

  std::vector<int16> trackBones;
  std::vector<AnimLine> lines;
  RootMotionData rootMotion;

  std::stringstream stream{std::string(text)};
  std::string line;

  while (std::getline(stream, line)) {
    auto cols = SplitTabs(line);
    if (cols.empty()) {
      continue;
    }

    if (cols[0] == "animation" && cols.size() >= 4) {
      anim->frameRate = static_cast<uint32>(std::max(1, ParseInt(cols[1], 30)));
      anim->duration = ParseFloat(cols[2], 0.0f);
      const int numFrames = std::max(1, ParseInt(cols[3], 1));
      anim->numFrames = static_cast<size_t>(numFrames);
      continue;
    }

    if (cols[0] == "track" && cols.size() >= 3) {
      trackBones.push_back(static_cast<int16>(ParseInt(cols[1], static_cast<int>(trackBones.size()))));
      xmlAnnotationTrack annotation;
      annotation.name = std::string(cols[2]);
      anim->annotations.emplace_back(std::move(annotation));
      continue;
    }

    if (cols[0] == "rootmotion" && cols.size() >= 10) {
      rootMotion.has = true;
      rootMotion.duration = ParseFloat(cols[1], 0.0f);
      rootMotion.up =
          Vector4A16(ParseFloat(cols[2]), ParseFloat(cols[3]), ParseFloat(cols[4]),
                     ParseFloat(cols[5]));
      rootMotion.forward =
          Vector4A16(ParseFloat(cols[6]), ParseFloat(cols[7]), ParseFloat(cols[8]),
                     ParseFloat(cols[9]));
      continue;
    }

    if (cols[0] == "rootframe" && cols.size() >= 6) {
      const int frame = ParseInt(cols[1], 0);
      if (frame >= 0) {
        if (rootMotion.samples.size() <= static_cast<size_t>(frame)) {
          rootMotion.samples.resize(static_cast<size_t>(frame) + 1);
        }
        rootMotion.samples[static_cast<size_t>(frame)] =
            Vector4A16(ParseFloat(cols[2]), ParseFloat(cols[3]), ParseFloat(cols[4]),
                       ParseFloat(cols[5]));
      }
      continue;
    }

    if (cols[0] == "frame" && cols.size() >= 13) {
      AnimLine f;
      f.track = ParseInt(cols[1]);
      f.frame = ParseInt(cols[2]);
      f.value.translation =
          Vector4A16(ParseFloat(cols[3]), ParseFloat(cols[4]), ParseFloat(cols[5]), 0.0f);
      f.value.rotation =
          Vector4A16(ParseFloat(cols[6]), ParseFloat(cols[7]), ParseFloat(cols[8]),
                     ParseFloat(cols[9], 1.0f));
      f.value.scale =
          Vector4A16(ParseFloat(cols[10], 1.0f), ParseFloat(cols[11], 1.0f),
                     ParseFloat(cols[12], 1.0f), 0.0f);
      lines.emplace_back(f);
    }
  }

  const size_t numTracks = trackBones.size();
  if (!numTracks) {
    SetError("Animation export needs at least one track");
    return;
  }

  anim->transforms.reserve(numTracks);
  for (size_t i = 0; i < numTracks; ++i) {
    auto *track = new xmlInterleavedAnimation::transform_container();
    track->assign(anim->numFrames, hkQTransform{});
    anim->transforms.emplace_back(track);
  }

  for (const AnimLine &f : lines) {
    if (f.track >= 0 && static_cast<size_t>(f.track) < numTracks && f.frame >= 0 &&
        static_cast<size_t>(f.frame) < anim->numFrames) {
      anim->transforms[f.track]->at(static_cast<size_t>(f.frame)) = f.value;
    }
  }

  if (anim->duration <= 0.0f && anim->frameRate) {
    anim->duration = static_cast<float>(anim->numFrames - 1) /
                     static_cast<float>(anim->frameRate);
  }

  hkaAnimation *outputAnim = anim;
  if (compress) {
    auto *spline = file.NewClass<xmlSplineCompressedAnimation>();
    hkaSplineCompressionSettings settings = GetSplineSettings(preset);
    std::string compressionError;
    if (!spline->CompressFromInterleaved(*anim, settings, &compressionError)) {
      SetError(compressionError.empty() ? "Spline animation compression failed"
                                        : std::move(compressionError));
      return;
    }
    if (preset == GENERATIONS_PC_GAME_TYPE) {
      const auto extraTail = 16u * (static_cast<uint32>(numTracks) + 11u);
      spline->compressed.dataBuffer.resize(
          spline->compressed.dataBuffer.size() + extraTail, 0);
    }

    if (rootMotion.has && !rootMotion.samples.empty()) {
      auto *motion = file.NewClass<xmlDefaultAnimatedReferenceFrame>();
      motion->duration =
          rootMotion.duration > 0.0f ? rootMotion.duration : anim->duration;
      motion->up = rootMotion.up;
      motion->forward = rootMotion.forward;
      motion->referenceFrames = std::move(rootMotion.samples);
      spline->extractedMotion = motion;
    } else if (preset == GENERATIONS_PC_GAME_TYPE) {
      auto *motion = file.NewClass<xmlDefaultAnimatedReferenceFrame>();
      motion->duration = anim->duration;
      motion->up = Vector4A16(0.0f, 1.0f, 0.0f, 0.0f);
      motion->forward = Vector4A16(0.0f, 0.0f, 1.0f, 0.0f);
      motion->referenceFrames = {
          Vector4A16(0.0f, 0.0f, 0.0f, 0.0f),
          Vector4A16(0.0f, 0.0f, 0.0f, 0.0f),
      };
      spline->extractedMotion = motion;
    }

    outputAnim = spline;
  } else if (rootMotion.has && !rootMotion.samples.empty()) {
    auto *motion = file.NewClass<xmlDefaultAnimatedReferenceFrame>();
    motion->duration =
        rootMotion.duration > 0.0f ? rootMotion.duration : anim->duration;
    motion->up = rootMotion.up;
    motion->forward = rootMotion.forward;
    motion->referenceFrames = std::move(rootMotion.samples);
    anim->extractedMotion = motion;
  }

  auto *binding = file.NewClass<Binding>();
  binding->anim = outputAnim;
  binding->skelName = std::move(skeletonName);
  binding->trackMap = std::move(trackBones);

  container->animations.push_back(outputAnim);
  container->bindings.push_back(binding);
}

int ExportSkeleton(const char *path, const char *text, int preset) {
  if (!path || !text) {
    SetError("Missing export path or skeleton payload");
    return 0;
  }

  xmlHavokFile file;
  auto *root = file.NewClass<xmlRootLevelContainer>();
  auto *container = file.NewClass<xmlAnimationContainer>();
  auto *skeleton = ParseSkeleton(file, text);
  container->skeletons.push_back(skeleton);
  AddRootAndContainer(root, container, "Animation Container");
  const Preset p = GetPreset(preset);
  file.ToPackFile(path, p.toolset, p.rule);
  return 1;
}

std::string ParseSkeletonName(std::string_view text) {
  std::stringstream stream{std::string(text)};
  std::string line;

  while (std::getline(stream, line)) {
    auto cols = SplitTabs(line);
    if (cols.size() > 1 && cols[0] == "skeleton") {
      return std::string(cols[1]);
    }
  }

  return {};
}

int ExportAnimation(const char *path, const char *skeletonText,
                    const char *animationText, int preset, bool compress) {
  if (!path || !animationText) {
    SetError("Missing export path or animation payload");
    return 0;
  }

  xmlHavokFile file;
  auto *root = file.NewClass<xmlRootLevelContainer>();
  auto *container = file.NewClass<xmlAnimationContainer>();
  std::string skeletonName;
  if (skeletonText && skeletonText[0]) {
    skeletonName = ParseSkeletonName(skeletonText);
  }
  ParseAnimationData(file, animationText, container, std::move(skeletonName),
                     preset, compress);
  if (!g_lastError.empty()) {
    return 0;
  }
  AddRootAndContainer(root, container, "Merged Animation Container");
  const Preset p = GetPreset(preset);
  file.ToPackFile(path, p.toolset, p.rule);
  return 1;
}

} 

HEAT_API const char *HEAT_last_error() {
  return g_lastError.c_str();
}

HEAT_API int HEAT_import_skeleton(const char *path, char *out,
                                  uint32_t capacity, uint32_t *required) {
  return Guard([&]() {
    if (!path) {
      SetError("Missing skeleton path");
      return 0;
    }

    auto pack = IhkPackFile::Create(path);
    const hkaSkeleton *skeleton = FindSkeleton(*pack);
    if (!skeleton) {
      SetError("No hkaSkeleton was found");
      return 0;
    }

    return CopyResult(SkeletonToJson(*skeleton), out, capacity, required);
  });
}

HEAT_API int HEAT_import_animation(const char *animationPath,
                                   const char *skeletonPath, char *out,
                                   uint32_t capacity, uint32_t *required) {
  return Guard([&]() {
    if (!animationPath) {
      SetError("Missing animation path");
      return 0;
    }

    auto animPack = IhkPackFile::Create(animationPath);
    IhkPackFile::Ptr skeletonPack;
    const hkaSkeleton *skeleton = nullptr;

    if (skeletonPath && skeletonPath[0]) {
      skeletonPack = IhkPackFile::Create(skeletonPath);
      skeleton = FindSkeleton(*skeletonPack);
    }

    if (!skeleton) {
      skeleton = FindSkeleton(*animPack);
    }

    std::string json = AnimationToJson(*animPack, skeleton);
    if (!g_lastError.empty()) {
      return 0;
    }

    return CopyResult(json, out, capacity, required);
  });
}

HEAT_API int HEAT_export_skeleton(const char *path, const char *skeletonText,
                                  int preset) {
  return Guard([&]() { return ExportSkeleton(path, skeletonText, preset); });
}

HEAT_API int HEAT_export_animation(const char *path, const char *skeletonText,
                                   const char *animationText, int preset) {
  return Guard([&]() {
    return ExportAnimation(path, skeletonText, animationText, preset, false);
  });
}

HEAT_API int HEAT_export_compressed_animation(const char *path,
                                              const char *skeletonText,
                                              const char *animationText,
                                              int preset) {
  return Guard([&]() {
    return ExportAnimation(path, skeletonText, animationText, preset, true);
  });
}
