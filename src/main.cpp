#include "havok_xml.hpp"
#include "hklib/hka_animation.hpp"
#include "hklib/hka_animationbinding.hpp"
#include "hklib/hka_animationcontainer.hpp"
#include "hklib/hka_annotationtrack.hpp"
#include "hklib/hka_skeleton.hpp"
#include "internal/hka_animation.hpp"
#include "internal/hka_deltaanimation.hpp"
#include "internal/hka_interleavedanimation.hpp"
#include "internal/hka_splineanimation.hpp"

#include <algorithm>
#include <charconv>
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

struct SkeletonImport {
  IhkPackFile::Ptr pack;
  const hkaSkeleton *skeleton = nullptr;
};

struct AnimationImport {
  IhkPackFile::Ptr animPack;
  IhkPackFile::Ptr skeletonPack;
  const hkaSkeleton *skeleton = nullptr;
  const hkaAnimationContainer *container = nullptr;
  const hkaAnimation *animation = nullptr;
  const hkaAnimationInternalInterface *internal = nullptr;
  std::vector<int16> trackMap;
  float duration = 0.0f;
  uint32_t fps = 30;
  size_t frameCount = 1;
  size_t outputTrackCount = 0;
};

enum GameType {
  GENERATIONS_PC_GAME_TYPE = 0,
  UNLEASHED_360_GAME_TYPE = 1,
  GENERATIONS_360_GAME_TYPE = 2,
  GENERATIONS_PS3_GAME_TYPE = 3,
  UNLEASHED_PS3_GAME_TYPE = 4,
  LOST_WORLD_PC_GAME_TYPE = 5,
  LOST_WORLD_WIIU_GAME_TYPE = 6,
  FORCES_GAME_TYPE = 7,
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

const char *TempString(std::string_view value) {
  thread_local std::string temp;
  temp.assign(value);
  return temp.c_str();
}

template <class Fn> int Guard(Fn &&fn) {
  g_lastError.clear();
  return fn();
}

bool AlmostEqual(float lhs, float rhs) {
  return std::fabs(lhs - rhs) <= 0.0001f;
}

Vector4A16 ScaleForImport(const Vector4A16 &scale) {
  if (!AlmostEqual(scale.X, scale.Y) || !AlmostEqual(scale.X, scale.Z)) {
    return scale;
  }

  if (AlmostEqual(scale.X, 1.0f)) {
    return scale;
  }

  Vector4A16 out = scale;
  out.Y = 1.0f;
  out.Z = 1.0f;
  return out;
}

void CopyVector(const Vector4A16 &v, float *out, int count) {
  if (count > 0) {
    out[0] = v.X;
  }
  if (count > 1) {
    out[1] = v.Y;
  }
  if (count > 2) {
    out[2] = v.Z;
  }
  if (count > 3) {
    out[3] = v.W;
  }
}

void CopyTransform(const uni::RTSValue &value, float *out) {
  CopyVector(value.translation, out, 3);
  CopyVector(value.rotation, out + 3, 4);
  CopyVector(ScaleForImport(value.scale), out + 7, 3);
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

std::vector<int16> BuildTrackMap(const hkaAnimation &anim,
                                 const hkaAnimationBinding *binding,
                                 const hkaSkeleton *skeleton) {
  const size_t numTracks = anim.GetNumOfTransformTracks();
  std::vector<int16> map(numTracks, -1);
  std::unordered_map<std::string, int16> boneNameToIndex;
  int16 rootBoneIndex = -1;

  if (skeleton) {
    boneNameToIndex.reserve(skeleton->GetNumBones());
    for (size_t bone = 0; bone < skeleton->GetNumBones(); ++bone) {
      std::string name{skeleton->GetBoneName(bone)};
      if (!name.empty() && boneNameToIndex.find(name) == boneNameToIndex.end()) {
        boneNameToIndex.emplace(std::move(name), static_cast<int16>(bone));
      }
      if (skeleton->GetBoneParentID(bone) < 0) {
        rootBoneIndex = static_cast<int16>(bone);
      }
    }
  }

  for (size_t track = 0; track < numTracks; ++track) {
    if (track < anim.GetNumAnnotations()) {
      if (auto annot = anim.GetAnnotation(track)) {
        std::string trackName{annot->GetName()};

        const auto it = boneNameToIndex.find(trackName);
        if (it != boneNameToIndex.end()) {
          map[track] = it->second;
          continue;
        }

        if (trackName == "Root" && rootBoneIndex >= 0) {
          map[track] = rootBoneIndex;
          continue;
        }
      }
    }

    if (binding && track < binding->GetNumTransformTrackToBoneIndices()) {
      const int16 mapped = binding->GetTransformTrackToBoneIndex(track);
      if (mapped >= 0) {
        map[track] = mapped;
        continue;
      }
    }

    map[track] = static_cast<int16>(track);
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
  if (preset == FORCES_GAME_TYPE) {
    file.ToTagFile(path, HK2016_1);
    return 1;
  }
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
  if (preset == FORCES_GAME_TYPE) {
    file.ToTagFile(path, HK2016_1);
    return 1;
  }
  const Preset p = GetPreset(preset);
  file.ToPackFile(path, p.toolset, p.rule);
  return 1;
}

int OpenSkeleton(const char *path, SkeletonImport **out) {
  if (!path || !out) {
    SetError("Missing skeleton path or output handle");
    return 0;
  }

  auto import = std::make_unique<SkeletonImport>();
  import->pack = IhkPackFile::Create(path);
  import->skeleton = FindSkeleton(*import->pack);
  if (!import->skeleton) {
    SetError("No hkaSkeleton was found");
    return 0;
  }

  *out = import.release();
  return 1;
}

int OpenAnimation(const char *animationPath, const char *skeletonPath,
                  AnimationImport **out) {
  if (!animationPath || !out) {
    SetError("Missing animation path or output handle");
    return 0;
  }

  auto import = std::make_unique<AnimationImport>();
  import->animPack = IhkPackFile::Create(animationPath);

  if (skeletonPath && skeletonPath[0]) {
    import->skeletonPack = IhkPackFile::Create(skeletonPath);
    import->skeleton = FindSkeleton(*import->skeletonPack);
  }

  import->container = FindContainer(*import->animPack);
  if (import->container && import->container->GetNumAnimations()) {
    import->animation = import->container->GetAnimation(0);
    if (!import->skeleton && import->container->GetNumSkeletons()) {
      import->skeleton = import->container->GetSkeleton(0);
    }
  }

  if (!import->animation) {
    import->animation =
        FirstClass<hkaAnimation>(*import->animPack, hkaAnimation::GetHash());
  }

  if (!import->animation) {
    SetError("No hkaAnimation was found");
    return 0;
  }

  if (!import->skeleton) {
    import->skeleton = FindSkeleton(*import->animPack);
  }

  if (import->skeleton) {
    const_cast<hkaAnimation *>(import->animation)
        ->SetReferenceSkeleton(import->skeleton);
  }

  import->internal =
      dynamic_cast<const hkaAnimationInternalInterface *>(import->animation);
  if (!import->internal) {
    SetError("Unexpected error, report to developer(s).");
    return 0;
  }

  const size_t sourceFrameCount = AnimationFrameCount(import->animation);
  import->duration = import->animation->Duration();
  import->fps = import->animation->FrameRate();
  if (!import->fps && import->duration > 0.0f && sourceFrameCount > 1) {
    import->fps = static_cast<uint32_t>(
        std::round(static_cast<float>(sourceFrameCount - 1) / import->duration));
  }
  if (!import->fps) {
    import->fps = 30;
  }

  import->frameCount = std::max<size_t>(
      1, static_cast<size_t>(import->duration * import->fps + 0.5f) + 1);
  if (import->frameCount <= 1 && sourceFrameCount > 1) {
    import->frameCount = sourceFrameCount;
  }

  const auto *binding = import->container
                            ? FindBindingForAnimation(*import->container,
                                                      import->animation)
                            : nullptr;
  import->trackMap = BuildTrackMap(*import->animation, binding, import->skeleton);
  import->outputTrackCount = import->skeleton
                                 ? import->skeleton->GetNumBones()
                                 : import->animation->GetNumOfTransformTracks();

  *out = import.release();
  return 1;
}

void SampleFrame(const AnimationImport &import, uint32_t frame, float *out) {
  for (size_t bone = 0; bone < import.outputTrackCount; ++bone) {
    uni::RTSValue value;
    if (import.skeleton) {
      if (const hkQTransform *ref = import.skeleton->GetBoneTM(bone)) {
        value = uni::RTSValue(ref->translation, ref->rotation, ref->scale);
      }
    }
    CopyTransform(value, out + bone * 10);
  }

  float time = 0.0f;
  if (import.frameCount > 1) {
    time = frame / static_cast<float>(import.fps);
  }
  if (import.duration > 0.0f) {
    time = std::min(time, import.duration);
  }

  const size_t trackCount = import.animation->GetNumOfTransformTracks();
  for (size_t track = 0; track < trackCount; ++track) {
    size_t boneIndex = track;
    if (track < import.trackMap.size() && import.trackMap[track] >= 0) {
      boneIndex = static_cast<size_t>(import.trackMap[track]);
    }

    if (boneIndex >= import.outputTrackCount) {
      continue;
    }

    uni::RTSValue value;
    import.internal->GetValue(value, time, track);
    CopyTransform(value, out + boneIndex * 10);
  }
}

} 

HEAT_API const char *HEAT_last_error() {
  return g_lastError.c_str();
}

HEAT_API int HEAT_open_skeleton(const char *path, void **out) {
  return Guard([&]() {
    auto *handle = static_cast<SkeletonImport *>(nullptr);
    const int result = OpenSkeleton(path, &handle);
    if (out) {
      *out = handle;
    }
    return result;
  });
}

HEAT_API void HEAT_close_skeleton(void *handle) {
  delete static_cast<SkeletonImport *>(handle);
}

HEAT_API const char *HEAT_skeleton_name(void *handle) {
  auto *import = static_cast<SkeletonImport *>(handle);
  return import && import->skeleton ? TempString(import->skeleton->Name()) : "";
}

HEAT_API uint32_t HEAT_skeleton_bone_count(void *handle) {
  auto *import = static_cast<SkeletonImport *>(handle);
  return import && import->skeleton
             ? static_cast<uint32_t>(import->skeleton->GetNumBones())
             : 0;
}

HEAT_API const char *HEAT_skeleton_bone_name(void *handle, uint32_t bone) {
  auto *import = static_cast<SkeletonImport *>(handle);
  if (!import || !import->skeleton || bone >= import->skeleton->GetNumBones()) {
    return "";
  }
  return TempString(import->skeleton->GetBoneName(bone));
}

HEAT_API int32_t HEAT_skeleton_bone_parent(void *handle, uint32_t bone) {
  auto *import = static_cast<SkeletonImport *>(handle);
  if (!import || !import->skeleton || bone >= import->skeleton->GetNumBones()) {
    return -1;
  }
  return import->skeleton->GetBoneParentID(bone);
}

HEAT_API int HEAT_skeleton_bone_transform(void *handle, uint32_t bone,
                                          float *out10) {
  auto *import = static_cast<SkeletonImport *>(handle);
  if (!import || !import->skeleton || !out10 ||
      bone >= import->skeleton->GetNumBones()) {
    return 0;
  }
  if (const hkQTransform *tm = import->skeleton->GetBoneTM(bone)) {
    CopyTransform(*tm, out10);
    return 1;
  }
  return 0;
}

HEAT_API int HEAT_open_animation(const char *animationPath,
                                 const char *skeletonPath, void **out) {
  return Guard([&]() {
    auto *handle = static_cast<AnimationImport *>(nullptr);
    const int result = OpenAnimation(animationPath, skeletonPath, &handle);
    if (out) {
      *out = handle;
    }
    return result;
  });
}

HEAT_API void HEAT_close_animation(void *handle) {
  delete static_cast<AnimationImport *>(handle);
}

HEAT_API uint32_t HEAT_animation_fps(void *handle) {
  auto *import = static_cast<AnimationImport *>(handle);
  return import ? import->fps : 0;
}

HEAT_API uint32_t HEAT_animation_frame_count(void *handle) {
  auto *import = static_cast<AnimationImport *>(handle);
  return import ? static_cast<uint32_t>(import->frameCount) : 0;
}

HEAT_API uint32_t HEAT_animation_track_count(void *handle) {
  auto *import = static_cast<AnimationImport *>(handle);
  return import ? static_cast<uint32_t>(import->outputTrackCount) : 0;
}

HEAT_API const char *HEAT_animation_track_name(void *handle, uint32_t track) {
  auto *import = static_cast<AnimationImport *>(handle);
  if (!import || track >= import->outputTrackCount) {
    return "";
  }
  if (import->skeleton && track < import->skeleton->GetNumBones()) {
    return TempString(import->skeleton->GetBoneName(track));
  }
  if (track < import->animation->GetNumAnnotations()) {
    if (auto annot = import->animation->GetAnnotation(track)) {
      return TempString(annot->GetName());
    }
  }
  return "";
}

HEAT_API int HEAT_animation_sample_frame(void *handle, uint32_t frame,
                                         float *out, uint32_t capacity) {
  auto *import = static_cast<AnimationImport *>(handle);
  if (!import || !out || frame >= import->frameCount ||
      capacity < import->outputTrackCount * 10) {
    return 0;
  }
  SampleFrame(*import, frame, out);
  return 1;
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
