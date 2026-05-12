#include "acl/compression/compress.h"
#include "acl/compression/compression_settings.h"
#include "acl/compression/output_stats.h"
#include "acl/compression/track_array.h"
#include "acl/core/ansi_allocator.h"
#include "acl/decompression/decompress.h"
#include "acl/decompression/decompression_settings.h"
#include "nlohmann/json.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#define HEAT_PXD_API extern "C" __declspec(dllexport)
#else
#define HEAT_PXD_API extern "C" __attribute__((visibility("default")))
#endif

namespace {

using json = nlohmann::json;

thread_local std::string g_lastError;

struct Bone {
  std::string name;
  int16_t parent = -1;
  float t[3]{};
  float r[4]{0.0f, 0.0f, 0.0f, 1.0f};
  float s[3]{1.0f, 1.0f, 1.0f};
};

struct Track {
  int index = 0;
  std::string name;
};

struct Sample {
  int track = 0;
  int frame = 0;
  float t[3]{};
  float r[4]{0.0f, 0.0f, 0.0f, 1.0f};
  float s[3]{1.0f, 1.0f, 1.0f};
  float translationW = 0.0f;
  float scaleW = 1.0f;
};

struct RootFrame {
  int frame = 0;
  float t[3]{};
  float r[4]{0.0f, 0.0f, 0.0f, 1.0f};
  float s[3]{1.0f, 1.0f, 1.0f};
  float translationW = 0.0f;
  float scaleW = 1.0f;
};

struct Animation {
  float sampleRate = 30.0f;
  float duration = 0.0f;
  int frames = 1;
  bool additive = false;
  bool rootMotion = true;
  float rootDuration = 0.0f;
  std::vector<Track> tracks;
  std::vector<Sample> samples;
  std::vector<RootFrame> rootFrames;
};

void SetError(std::string message) { g_lastError = std::move(message); }

uint16_t Read16(const std::vector<uint8_t> &buf, size_t off) {
  uint16_t v = 0;
  if (off + sizeof(v) <= buf.size()) {
    std::memcpy(&v, buf.data() + off, sizeof(v));
  }
  return v;
}

uint32_t Read32(const std::vector<uint8_t> &buf, size_t off) {
  uint32_t v = 0;
  if (off + sizeof(v) <= buf.size()) {
    std::memcpy(&v, buf.data() + off, sizeof(v));
  }
  return v;
}

float ReadF32(const std::vector<uint8_t> &buf, size_t off) {
  float v = 0.0f;
  if (off + sizeof(v) <= buf.size()) {
    std::memcpy(&v, buf.data() + off, sizeof(v));
  }
  return v;
}

void Write16(std::vector<uint8_t> &buf, size_t off, int16_t v) {
  std::memcpy(buf.data() + off, &v, sizeof(v));
}

void Write32(std::vector<uint8_t> &buf, size_t off, uint32_t v) {
  std::memcpy(buf.data() + off, &v, sizeof(v));
}

void WriteOffsetTableValue(std::vector<uint8_t> &buf, size_t &cursor, uint32_t offset) {
  uint32_t shifted = offset >> 2;
  int bytes = 1;
  uint32_t value = 0x40 | shifted;
  if (offset > 16384) {
    bytes = 4;
    value = 0xC0000000 | shifted;
  } else if (offset > 64) {
    bytes = 2;
    value = 0x8000 | shifted;
  }

  for (int byte = bytes - 1; byte >= 0; --byte) {
    buf[cursor++] = static_cast<uint8_t>(value >> (byte * 8));
  }
}

void WriteF32(std::vector<uint8_t> &buf, size_t off, float v) {
  std::memcpy(buf.data() + off, &v, sizeof(v));
}

bool ReadFile(const char *path, std::vector<uint8_t> &out) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    SetError("Could not open file");
    return false;
  }
  file.seekg(0, std::ios::end);
  const auto size = file.tellg();
  file.seekg(0, std::ios::beg);
  out.resize(static_cast<size_t>(size));
  if (!out.empty()) {
    file.read(reinterpret_cast<char *>(out.data()), size);
  }
  return true;
}

bool WriteFile(const char *path, const std::vector<uint8_t> &data) {
  std::ofstream file(path, std::ios::binary);
  if (!file) {
    SetError("Could not write file");
    return false;
  }
  file.write(reinterpret_cast<const char *>(data.data()),
             static_cast<std::streamsize>(data.size()));
  return true;
}

std::string ReadString(const std::vector<uint8_t> &buf, size_t off) {
  std::string out;
  while (off < buf.size() && buf[off] != 0) {
    out.push_back(static_cast<char>(buf[off++]));
  }
  return out;
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

std::vector<std::string> SplitTabs(const std::string &line) {
  std::vector<std::string> cols;
  size_t start = 0;
  while (start <= line.size()) {
    const size_t pos = line.find('\t', start);
    cols.emplace_back(line.substr(start, pos == std::string::npos ? pos : pos - start));
    if (pos == std::string::npos) {
      break;
    }
    start = pos + 1;
  }
  return cols;
}

int ToInt(const std::string &s, int fallback = 0) {
  char *end = nullptr;
  const long value = std::strtol(s.c_str(), &end, 10);
  return end == s.c_str() ? fallback : static_cast<int>(value);
}

float ToFloat(const std::string &s, float fallback = 0.0f) {
  char *end = nullptr;
  const float value = std::strtof(s.c_str(), &end);
  return end == s.c_str() ? fallback : value;
}

std::vector<Bone> ParseSkeletonText(const char *text) {
  std::vector<Bone> bones;
  std::stringstream stream(text ? text : "");
  std::string line;
  while (std::getline(stream, line)) {
    auto cols = SplitTabs(line);
    if (cols.size() < 13 || cols[0] != "bone") {
      continue;
    }
    Bone b;
    b.parent = static_cast<int16_t>(ToInt(cols[1], -1));
    b.name = cols[2];
    b.t[0] = ToFloat(cols[3]);
    b.t[1] = ToFloat(cols[4]);
    b.t[2] = ToFloat(cols[5]);
    b.r[0] = ToFloat(cols[6]);
    b.r[1] = ToFloat(cols[7]);
    b.r[2] = ToFloat(cols[8]);
    b.r[3] = ToFloat(cols[9], 1.0f);
    b.s[0] = ToFloat(cols[10], 1.0f);
    b.s[1] = ToFloat(cols[11], 1.0f);
    b.s[2] = ToFloat(cols[12], 1.0f);
    bones.emplace_back(std::move(b));
  }
  return bones;
}

Animation ParseAnimationText(const char *text) {
  Animation anim;
  std::stringstream stream(text ? text : "");
  std::string line;
  while (std::getline(stream, line)) {
    auto cols = SplitTabs(line);
    if (cols.empty()) {
      continue;
    }
    if (cols[0] == "animation" && cols.size() >= 4) {
      anim.sampleRate = std::max(1.0f, ToFloat(cols[1], 30.0f));
      anim.duration = ToFloat(cols[2]);
      anim.frames = std::max(1, ToInt(cols[3], 1));
    } else if (cols[0] == "pxdflags" && cols.size() >= 3) {
      anim.additive = ToInt(cols[1]) != 0;
      anim.rootMotion = ToInt(cols[2], 1) != 0;
    } else if (cols[0] == "rootmotion" && cols.size() >= 2) {
      anim.rootDuration = ToFloat(cols[1], anim.duration);
    } else if (cols[0] == "rootframe" && cols.size() >= 12) {
      RootFrame f;
      f.frame = ToInt(cols[1]);
      f.t[0] = ToFloat(cols[2]);
      f.t[1] = ToFloat(cols[3]);
      f.t[2] = ToFloat(cols[4]);
      f.r[0] = ToFloat(cols[5]);
      f.r[1] = ToFloat(cols[6]);
      f.r[2] = ToFloat(cols[7]);
      f.r[3] = ToFloat(cols[8], 1.0f);
      f.s[0] = ToFloat(cols[9], 1.0f);
      f.s[1] = ToFloat(cols[10], 1.0f);
      f.s[2] = ToFloat(cols[11], 1.0f);
      if (cols.size() >= 14) {
        f.translationW = ToFloat(cols[12]);
        f.scaleW = ToFloat(cols[13], 1.0f);
      }
      anim.rootFrames.emplace_back(f);
    } else if (cols[0] == "rootframe" && cols.size() >= 6) {
      RootFrame f;
      f.frame = ToInt(cols[1]);
      f.t[0] = ToFloat(cols[2]);
      f.t[1] = ToFloat(cols[3]);
      f.t[2] = ToFloat(cols[4]);
      anim.rootFrames.emplace_back(f);
    } else if (cols[0] == "track" && cols.size() >= 3) {
      anim.tracks.push_back({ToInt(cols[1]), cols[2]});
    } else if (cols[0] == "frame" && cols.size() >= 13) {
      Sample s;
      s.track = ToInt(cols[1]);
      s.frame = ToInt(cols[2]);
      s.t[0] = ToFloat(cols[3]);
      s.t[1] = ToFloat(cols[4]);
      s.t[2] = ToFloat(cols[5]);
      s.r[0] = ToFloat(cols[6]);
      s.r[1] = ToFloat(cols[7]);
      s.r[2] = ToFloat(cols[8]);
      s.r[3] = ToFloat(cols[9], 1.0f);
      s.s[0] = ToFloat(cols[10], 1.0f);
      s.s[1] = ToFloat(cols[11], 1.0f);
      s.s[2] = ToFloat(cols[12], 1.0f);
      if (cols.size() >= 15) {
        s.translationW = ToFloat(cols[13]);
        s.scaleW = ToFloat(cols[14], 1.0f);
      }
      anim.samples.emplace_back(s);
    }
  }
  return anim;
}

void WriteBinaHeaderBase(std::vector<uint8_t> &buf, uint32_t fileSize,
                         uint32_t dataValue) {
  buf[0] = 'B';
  buf[1] = 'I';
  buf[2] = 'N';
  buf[3] = 'A';
  buf[4] = '2';
  buf[5] = '1';
  buf[6] = '0';
  buf[7] = 'L';
  Write32(buf, 0x08, fileSize);
  Write32(buf, 0x0C, 1);
  buf[0x10] = 'D';
  buf[0x11] = 'A';
  buf[0x12] = 'T';
  buf[0x13] = 'A';
  Write32(buf, 0x14, fileSize - 0x10);
  Write32(buf, 0x18, dataValue);
  Write32(buf, 0x1C, 0);
  buf[0x24] = 0x18;
}

void WriteSkeletonBinaHeader(std::vector<uint8_t> &buf, uint32_t fileSize,
                             uint32_t stringTableOffset, uint32_t stringTableSize,
                             uint32_t offsetTableSize) {
  WriteBinaHeaderBase(buf, fileSize, stringTableOffset);
  Write32(buf, 0x1C, stringTableSize);
  Write32(buf, 0x20, offsetTableSize);
}

void WriteAnimationBinaHeader(std::vector<uint8_t> &buf, uint32_t fileSize,
                              uint32_t stringTableOffset) {
  WriteBinaHeaderBase(buf, fileSize, stringTableOffset);
  Write32(buf, 0x20, 4);
}

std::string SkeletonJson(const std::vector<Bone> &bones, std::string_view name) {
  json root;
  root["type"] = "skeleton";
  root["name"] = name;
  root["bones"] = json::array();
  for (const auto &b : bones) {
    root["bones"].push_back({
        {"name", b.name},
        {"parent", b.parent},
        {"translation", {b.t[0], b.t[1], b.t[2]}},
        {"rotation", {b.r[0], b.r[1], b.r[2], b.r[3]}},
        {"scale", {b.s[0], b.s[1], b.s[2]}},
    });
  }
  return root.dump();
}

int ImportSkeletonImpl(const char *path, char *out, uint32_t capacity,
                       uint32_t *required) {
  std::vector<uint8_t> buf;
  if (!ReadFile(path, buf)) {
    return 0;
  }
  if (buf.size() < 0xA0 || std::memcmp(buf.data() + 0x40, "KSXP", 4) != 0) {
    SetError("Not a valid PXD skeleton");
    return 0;
  }
  const size_t ds = 0x40;
  const uint32_t version = Read32(buf, ds + 0x04);
  if (version != 512) {
    SetError("Unsupported PXD skeleton version");
    return 0;
  }
  const size_t parentOff = ds + Read32(buf, ds + 0x08);
  const uint32_t count = Read32(buf, ds + 0x10);
  const size_t nameOff = ds + Read32(buf, ds + 0x28);
  const size_t transformOff = ds + Read32(buf, ds + 0x48);

  std::vector<Bone> bones(count);
  for (uint32_t i = 0; i < count; ++i) {
    Bone &b = bones[i];
    b.parent = static_cast<int16_t>(Read16(buf, parentOff + i * 2));
    b.name = ReadString(buf, ds + Read32(buf, nameOff + i * 0x10));
    const size_t base = transformOff + i * 0x30;
    b.t[0] = ReadF32(buf, base + 0x00);
    b.t[1] = ReadF32(buf, base + 0x04);
    b.t[2] = ReadF32(buf, base + 0x08);
    b.r[0] = ReadF32(buf, base + 0x10);
    b.r[1] = ReadF32(buf, base + 0x14);
    b.r[2] = ReadF32(buf, base + 0x18);
    b.r[3] = ReadF32(buf, base + 0x1C);
    b.s[0] = ReadF32(buf, base + 0x20);
    b.s[1] = ReadF32(buf, base + 0x24);
    b.s[2] = ReadF32(buf, base + 0x28);
  }
  return CopyResult(SkeletonJson(bones, path ? path : "PxdSkeleton"), out, capacity,
                    required);
}

int ExportSkeletonImpl(const char *path, const char *text) {
  auto bones = ParseSkeletonText(text);
  if (bones.empty()) {
    SetError("PXD skeleton export needs at least one bone");
    return 0;
  }

  const size_t dataStart = 0x40;
  const size_t headerSize = 0x68;
  const size_t parentOff = headerSize;
  const size_t nameOff = (parentOff + bones.size() * 2 + 7) & ~size_t(7);
  const size_t nameTableSize = bones.size() * 0x10;
  const size_t transformOff = (nameOff + nameTableSize + 0xF) & ~size_t(0xF);
  const size_t stringBase = transformOff + bones.size() * 0x30;
  std::vector<size_t> stringOffsets(bones.size());
  size_t cursor = 0;
  for (size_t i = 0; i < bones.size(); ++i) {
    stringOffsets[i] = stringBase + cursor;
    cursor += bones[i].name.size() + 1;
  }
  const size_t stringTableSize = (cursor + 3) & ~size_t(3);
  const size_t offsetTableOff = stringBase + stringTableSize;
  const size_t nameOffsetTableSize = (nameOff - parentOff + 0x20) > 64 ? 2 : 1;
  const size_t offsetTableSize = (3 + nameOffsetTableSize + bones.size() + 3) & ~size_t(3);
  const size_t dataSize = offsetTableOff + offsetTableSize;
  const size_t fileSize = dataStart + dataSize;
  std::vector<uint8_t> buf(fileSize, 0);
  WriteSkeletonBinaHeader(buf, static_cast<uint32_t>(fileSize),
                          static_cast<uint32_t>(stringBase),
                          static_cast<uint32_t>(stringTableSize),
                          static_cast<uint32_t>(offsetTableSize));

  const size_t ds = dataStart;
  std::memcpy(buf.data() + ds, "KSXP", 4);
  Write32(buf, ds + 0x04, 512);
  Write32(buf, ds + 0x08, static_cast<uint32_t>(parentOff));
  Write32(buf, ds + 0x10, static_cast<uint32_t>(bones.size()));
  Write32(buf, ds + 0x18, static_cast<uint32_t>(bones.size()));
  Write32(buf, ds + 0x28, static_cast<uint32_t>(nameOff));
  Write32(buf, ds + 0x30, static_cast<uint32_t>(bones.size()));
  Write32(buf, ds + 0x38, static_cast<uint32_t>(bones.size()));
  Write32(buf, ds + 0x48, static_cast<uint32_t>(transformOff));
  Write32(buf, ds + 0x50, static_cast<uint32_t>(bones.size()));
  Write32(buf, ds + 0x58, static_cast<uint32_t>(bones.size()));

  for (size_t i = 0; i < bones.size(); ++i) {
    Write16(buf, ds + parentOff + i * 2, bones[i].parent);
    Write32(buf, ds + nameOff + i * 0x10, static_cast<uint32_t>(stringOffsets[i]));
    std::memcpy(buf.data() + ds + stringOffsets[i], bones[i].name.c_str(),
                bones[i].name.size() + 1);
    const size_t base = ds + transformOff + i * 0x30;
    WriteF32(buf, base + 0x00, bones[i].t[0]);
    WriteF32(buf, base + 0x04, bones[i].t[1]);
    WriteF32(buf, base + 0x08, bones[i].t[2]);
    WriteF32(buf, base + 0x10, bones[i].r[0]);
    WriteF32(buf, base + 0x14, bones[i].r[1]);
    WriteF32(buf, base + 0x18, bones[i].r[2]);
    WriteF32(buf, base + 0x1C, bones[i].r[3]);
    WriteF32(buf, base + 0x20, bones[i].s[0]);
    WriteF32(buf, base + 0x24, bones[i].s[1]);
    WriteF32(buf, base + 0x28, bones[i].s[2]);
  }

  size_t offsetCursor = ds + offsetTableOff;
  buf[offsetCursor++] = 0x42;
  buf[offsetCursor++] = 0x48;
  buf[offsetCursor++] = 0x48;
  WriteOffsetTableValue(buf, offsetCursor,
                        static_cast<uint32_t>(nameOff - parentOff + 0x20));
  for (size_t i = 1; i < bones.size(); ++i) {
    buf[offsetCursor++] = 0x44;
  }
  return WriteFile(path, buf) ? 1 : 0;
}

std::vector<uint8_t> DecompressAcl(const uint8_t *data, size_t size, float &duration,
                                   float &sampleRate, uint32_t &frames,
                                   uint32_t &tracks) {
  acl::error_result result;
  const auto *compressed =
      acl::make_compressed_tracks(reinterpret_cast<const char *>(data), &result);
  if (!compressed || !result.empty()) {
    SetError(result.empty() ? "Could not read ACL data" : result.c_str());
    return {};
  }

  acl::decompression_context<acl::default_transform_decompression_settings> context;
  if (!context.initialize(*compressed)) {
    SetError("Could not initialize ACL decompressor");
    return {};
  }

  struct Writer final : acl::track_writer {
    std::vector<rtm::qvvf> values;
    explicit Writer(uint32_t count) : values(count) {}
    void RTM_SIMD_CALL write_rotation(uint32_t i, rtm::quatf_arg0 v) {
      values[i].rotation = v;
    }
    void RTM_SIMD_CALL write_translation(uint32_t i, rtm::vector4f_arg0 v) {
      values[i].translation = v;
    }
    void RTM_SIMD_CALL write_scale(uint32_t i, rtm::vector4f_arg0 v) {
      values[i].scale = v;
    }
  };

  duration = compressed->get_duration();
  sampleRate = compressed->get_sample_rate();
  frames = compressed->get_num_samples_per_track();
  tracks = compressed->get_num_tracks();
  Writer writer(tracks);
  std::vector<uint8_t> raw(frames * tracks * sizeof(rtm::qvvf));
  for (uint32_t f = 0; f < frames; ++f) {
    const float t =
        std::min(static_cast<float>(f) / sampleRate, compressed->get_duration());
    context.seek(t, acl::sample_rounding_policy::none);
    context.decompress_tracks(writer);
    std::memcpy(raw.data() + f * tracks * sizeof(rtm::qvvf), writer.values.data(),
                tracks * sizeof(rtm::qvvf));
  }
  return raw;
}

std::vector<uint8_t> CompressAcl(const Animation &anim) {
  acl::ansi_allocator allocator;
  const uint32_t trackCount = static_cast<uint32_t>(anim.tracks.size());
  const uint32_t frameCount = static_cast<uint32_t>(std::max(1, anim.frames));
  acl::track_array_qvvf rawTracks(allocator, trackCount);
  std::vector<rtm::qvvf> values(frameCount * trackCount);
  for (auto &v : values) {
    v.rotation = rtm::quat_identity();
    v.translation = rtm::vector_zero();
    v.scale = rtm::vector_set(1.0f);
  }
  for (const auto &s : anim.samples) {
    if (s.track < 0 || s.frame < 0 || static_cast<uint32_t>(s.track) >= trackCount ||
        static_cast<uint32_t>(s.frame) >= frameCount) {
      continue;
    }
    auto &v = values[static_cast<size_t>(s.frame) * trackCount + s.track];
    v.rotation = rtm::quat_set(s.r[0], s.r[1], s.r[2], s.r[3]);
    v.translation = rtm::vector_set(s.t[0], s.t[1], s.t[2], s.translationW);
    v.scale = rtm::vector_set(s.s[0], s.s[1], s.s[2], s.scaleW);
  }

  for (uint32_t track = 0; track < trackCount; ++track) {
    acl::track_desc_transformf desc;
    desc.output_index = track;
    desc.precision = 0.001f;
    desc.shell_distance = 3.0f;
    auto rawTrack = acl::track_qvvf::make_reserve(
        desc, allocator, std::max(1u, frameCount), std::max(1.0f, anim.sampleRate));
    for (uint32_t frame = 0; frame < frameCount; ++frame) {
      rawTrack[frame] = values[static_cast<size_t>(frame) * trackCount + track];
    }
    rawTracks[track] = std::move(rawTrack);
  }

  acl::compression_settings settings;
  settings.level = acl::compression_level8::highest;
  settings.rotation_format = acl::rotation_format8::quatf_drop_w_variable;
  settings.translation_format = acl::vector_format8::vector3f_variable;
  settings.scale_format = acl::vector_format8::vector3f_variable;
  acl::qvvf_transform_error_metric metric;
  settings.error_metric = &metric;

  acl::output_stats stats;
  acl::compressed_tracks *compressed = nullptr;
  acl::error_result result =
      acl::compress_track_list(allocator, rawTracks, settings, compressed, stats);
  if (!compressed || !result.empty()) {
    SetError(result.empty() ? "ACL compression failed" : result.c_str());
    return {};
  }
  std::vector<uint8_t> out(compressed->get_size());
  std::memcpy(out.data(), compressed, out.size());
  allocator.deallocate(compressed, compressed->get_size());
  return out;
}

int ImportAnimationImpl(const char *path, char *out, uint32_t capacity,
                        uint32_t *required) {
  std::vector<uint8_t> buf;
  if (!ReadFile(path, buf)) {
    return 0;
  }
  if (buf.size() < 0x80 || std::memcmp(buf.data() + 0x40, "NAXP", 4) != 0) {
    SetError("Not a valid PXD animation");
    return 0;
  }
  const size_t ds = 0x40;
  if (Read32(buf, ds + 0x04) != 512 || buf[ds + 0x09] != 8) {
    SetError("Only compressed PXD animations are supported");
    return 0;
  }
  const float headerDuration = ReadF32(buf, ds + 0x18);
  const uint32_t headerFrames = Read32(buf, ds + 0x1C);
  const uint32_t headerTracks = Read32(buf, ds + 0x20);
  const size_t mainOff = ds + Read32(buf, ds + 0x28);
  const size_t rootOff = ds + Read32(buf, ds + 0x30);
  const uint32_t chunkSize = Read32(buf, mainOff);

  float duration = headerDuration;
  float sampleRate = 30.0f;
  uint32_t frames = headerFrames;
  uint32_t tracks = headerTracks;
  auto raw = DecompressAcl(buf.data() + mainOff, chunkSize, duration, sampleRate, frames, tracks);
  if (raw.empty()) {
    return 0;
  }

  json root;
  root["type"] = "animation";
  root["name"] = path ? path : "PxdAnimation";
  root["fps"] = static_cast<int>(sampleRate + 0.5f);
  root["duration"] = duration;
  root["frames"] = frames;
  root["tracks"] = json::array();

  if (rootOff > ds && rootOff < buf.size()) {
    const uint32_t rootChunkSize = Read32(buf, rootOff);
    float rootDuration = duration;
    float rootSampleRate = sampleRate;
    uint32_t rootFrames = frames;
    uint32_t rootTracks = 1;
    auto rootRaw = DecompressAcl(buf.data() + rootOff, rootChunkSize, rootDuration,
                                 rootSampleRate, rootFrames, rootTracks);
    if (!rootRaw.empty() && rootTracks) {
      json rootMotion;
      rootMotion["duration"] = rootDuration;
      rootMotion["up"] = {0.0f, 1.0f, 0.0f, 0.0f};
      rootMotion["forward"] = {0.0f, 0.0f, 1.0f, 0.0f};
      rootMotion["samples"] = json::array();
      for (uint32_t frame = 0; frame < rootFrames; ++frame) {
        const auto *qvv = reinterpret_cast<const rtm::qvvf *>(
            rootRaw.data() + static_cast<size_t>(frame) * rootTracks * sizeof(rtm::qvvf));
        float t[4];
        rtm::vector_store(qvv->translation, t);
        rootMotion["samples"].push_back({t[0], t[1], t[2], 0.0f});
      }
      root["rootMotion"] = std::move(rootMotion);
    }
  }
  for (uint32_t track = 0; track < tracks; ++track) {
    json trackJson;
    trackJson["bone"] = track;
    trackJson["name"] = "Bone_" + std::to_string(track);
    trackJson["samples"] = json::array();
    for (uint32_t frame = 0; frame < frames; ++frame) {
      const auto *qvv =
          reinterpret_cast<const rtm::qvvf *>(raw.data() +
                                              (static_cast<size_t>(frame) * tracks + track) *
                                                  sizeof(rtm::qvvf));
      float q[4];
      float t[4];
      float s[4];
      rtm::quat_store(qvv->rotation, q);
      rtm::vector_store(qvv->translation, t);
      rtm::vector_store(qvv->scale, s);
      trackJson["samples"].push_back(
          {t[0], t[1], t[2], q[0], q[1], q[2], q[3], s[0], s[1], s[2]});
    }
    root["tracks"].push_back(std::move(trackJson));
  }
  return CopyResult(root.dump(), out, capacity, required);
}

int ExportAnimationImpl(const char *path, const char *text) {
  Animation anim = ParseAnimationText(text);
  if (anim.tracks.empty()) {
    SetError("PXD animation export needs at least one track");
    return 0;
  }
  auto aclData = CompressAcl(anim);
  if (aclData.empty()) {
    return 0;
  }

  std::vector<uint8_t> rootData;
  if (anim.rootMotion && !anim.rootFrames.empty()) {
    Animation rootAnim;
    rootAnim.sampleRate = anim.sampleRate;
    rootAnim.duration = anim.rootDuration > 0.0f ? anim.rootDuration : anim.duration;
    rootAnim.frames = anim.frames;
    rootAnim.tracks.push_back({0, "root"});
    for (const RootFrame &f : anim.rootFrames) {
      Sample s;
      s.track = 0;
      s.frame = f.frame;
      s.t[0] = f.t[0];
      s.t[1] = f.t[1];
      s.t[2] = f.t[2];
      s.r[0] = f.r[0];
      s.r[1] = f.r[1];
      s.r[2] = f.r[2];
      s.r[3] = f.r[3];
      s.s[0] = f.s[0];
      s.s[1] = f.s[1];
      s.s[2] = f.s[2];
      s.translationW = f.translationW;
      s.scaleW = f.scaleW;
      rootAnim.samples.emplace_back(s);
    }
    rootData = CompressAcl(rootAnim);
    if (rootData.empty()) {
      return 0;
    }
  }

  const size_t dataStart = 0x40;
  const size_t naxpSize = 0x40;
  const size_t aclOff = dataStart + naxpSize;
  const size_t mainPad = rootData.empty() ? 4 - (aclData.size() % 4)
                                          : 0x10 - (aclData.size() % 0x10);
  const size_t rootPad = rootData.empty() ? 0 : 4 - (rootData.size() % 4);
  const size_t fileSize =
      aclOff + aclData.size() + mainPad + rootData.size() + rootPad + 4;
  std::vector<uint8_t> buf(fileSize, 0);
  WriteAnimationBinaHeader(
      buf, static_cast<uint32_t>(fileSize),
      static_cast<uint32_t>(fileSize - 0x44));

  const size_t ds = dataStart;
  std::memcpy(buf.data() + ds, "NAXP", 4);
  Write32(buf, ds + 0x04, 512);
  buf[ds + 0x08] = anim.additive ? 1 : 0;
  buf[ds + 0x09] = 8;
  Write32(buf, ds + 0x10, 0x18);
  WriteF32(buf, ds + 0x18, anim.duration);
  Write32(buf, ds + 0x1C, static_cast<uint32_t>(anim.frames));
  Write32(buf, ds + 0x20, static_cast<uint32_t>(anim.tracks.size()));
  Write32(buf, ds + 0x28, static_cast<uint32_t>(naxpSize));
  if (!rootData.empty()) {
    Write32(buf, ds + 0x30, static_cast<uint32_t>(naxpSize + aclData.size() + mainPad));
  }
  std::memcpy(buf.data() + aclOff, aclData.data(), aclData.size());
  size_t cursor = aclOff + aclData.size() + mainPad;
  if (!rootData.empty()) {
    std::memcpy(buf.data() + cursor, rootData.data(), rootData.size());
    cursor += rootData.size() + rootPad;
    buf[cursor] = 0x44;
    buf[cursor + 1] = 0x46;
    buf[cursor + 2] = 0x42;
  } else {
    buf[cursor] = 0x44;
    buf[cursor + 1] = 0x46;
  }
  return WriteFile(path, buf) ? 1 : 0;
}

} // namespace

HEAT_PXD_API const char *HEAT_PXD_last_error() { return g_lastError.c_str(); }

HEAT_PXD_API int HEAT_PXD_import_skeleton(const char *path, char *out,
                                          uint32_t capacity, uint32_t *required) {
  g_lastError.clear();
  return ImportSkeletonImpl(path, out, capacity, required);
}

HEAT_PXD_API int HEAT_PXD_import_animation(const char *path, char *out,
                                           uint32_t capacity, uint32_t *required) {
  g_lastError.clear();
  return ImportAnimationImpl(path, out, capacity, required);
}

HEAT_PXD_API int HEAT_PXD_export_skeleton(const char *path, const char *text) {
  g_lastError.clear();
  return ExportSkeletonImpl(path, text);
}

HEAT_PXD_API int HEAT_PXD_export_animation(const char *path, const char *text) {
  g_lastError.clear();
  return ExportAnimationImpl(path, text);
}
