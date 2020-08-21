// Copyright 2019 The MediaPipe Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <emscripten/bind.h>
#include <emscripten/html5.h>

#include <string>
#include <vector>

#include "apps/jspb/jspb_format.h"
#include "research/drishti/framework/calculator_framework.h"
#include "third_party/absl/strings/str_cat.h"
#include "third_party/libyuv/files/include/libyuv/convert.h"
#include "third_party/mediapipe/examples/desktop/autoflip/autoflip_messages.proto.h"
#include "third_party/mediapipe/framework/formats/image_frame.h"
#include "third_party/mediapipe/framework/formats/video_stream_header.h"
#include "third_party/mediapipe/framework/formats/yuv_image.h"
#include "third_party/mediapipe/framework/packet.h"

// For full-proto parsing
#include "third_party/easyexif/exif.h"
#include "third_party/mediapipe/framework/port/core_proto_inc.h"

namespace drishti {
namespace wasm {

// Make these available externally for text support.
std::vector<CalculatorGraphConfig> graph_configs_;
void CycleGraph();

namespace {

constexpr char kInputRawDataStream[] = "input_image_raw_data";
constexpr char kInputGpuBufferStream[] = "input_frames_gpu";
constexpr char kInputRawYuvStream[] = "input_yuv_raw_data";
constexpr char kOutputGpuBufferStream[] = "output_frames_gpu";

bool started_graph_ = false;
std::unique_ptr<CalculatorGraph> graph_;
std::vector<std::function<void()>> action_list_;
int64 timestamp_ = 0;
std::string aspect_ratio = "1:1";

struct SizeRect {
  int x;
  int y;
};

struct PacketListener {
  virtual ~PacketListener() {}
  virtual void onSize(const std::string &stream, const SizeRect &data) const {}
  virtual void onNumber(const std::string &stream, double number) const {}
  virtual void onShot(const std::string &stream, bool shotChanged,
                      double timestamp_seconds) const {}
  virtual void onExternalRendering(const std::string &stream,
                                   const std::string &frame_proto_str) const {}
  virtual void onFeature(const std::string &stream,
                         const std::string &feature_proto_str,
                         double timestamp) const {}
  virtual void onBorder(const std::string &stream,
                        const std::string &border_proto_str,
                        double timestamp) const {}
};

void AttachListener(const std::string &stream_name,
                    const PacketListener &listener) {
  LOG(ERROR) << "Attaching listener";
  action_list_.push_back([stream_name, &listener]() {
    std::cout << "Attaching listener to " << stream_name << std::endl;
    if (!graph_
             ->ObserveOutputStream(
                 stream_name,
                 [stream_name, &listener](const Packet &packet) {
                   if (packet.ValidateAsType<float>().ok()) {
                     const auto result = packet.Get<float>();
                     listener.onNumber(stream_name, result);
                   } else if (packet.ValidateAsType<std::pair<int, int>>()
                                  .ok()) {
                     const auto &pair = packet.Get<std::pair<int, int>>();
                     SizeRect result;
                     result.x = pair.first;
                     result.y = pair.second;
                     listener.onSize(stream_name, result);
                   } else if (packet.ValidateAsType<bool>().ok()) {
                     const auto result = packet.Get<bool>();
                     const auto timestamp = packet.Timestamp();
                     const double seconds = timestamp.Microseconds();
                     listener.onShot(stream_name, result, seconds);
                   } else if (packet
                                  .ValidateAsType<mediapipe::autoflip::
                                                      ExternalRenderFrame>()
                                  .ok()) {
                     const auto result =
                         packet.Get<mediapipe::autoflip::ExternalRenderFrame>();
                     apps::jspb::JspbFormat jspb_format;
                     const auto string_or = jspb_format.PrintToString(result);
                     listener.onExternalRendering(stream_name,
                                                  string_or.value_or(""));
                   } else if (packet
                                  .ValidateAsType<
                                      mediapipe::autoflip::DetectionSet>()
                                  .ok()) {
                     const auto result =
                         packet.Get<mediapipe::autoflip::DetectionSet>();
                     const double time = packet.Timestamp().Microseconds();
                     LOG(ERROR) << result.DebugString();
                     apps::jspb::JspbFormat jspb_format;
                     const auto string_or = jspb_format.PrintToString(result);
                     listener.onFeature(stream_name, string_or.value_or(""),
                                        time);
                   } else if (packet
                                  .ValidateAsType<
                                      mediapipe::autoflip::StaticFeatures>()
                                  .ok()) {
                     const auto result =
                         packet.Get<mediapipe::autoflip::StaticFeatures>();
                     const double time = packet.Timestamp().Microseconds();
                     LOG(ERROR) << result.DebugString();
                     apps::jspb::JspbFormat jspb_format;
                     const auto string_or = jspb_format.PrintToString(result);
                     listener.onBorder(stream_name, string_or.value_or(""),
                                       time);
                   }
                   return OkStatus();
                 })
             .ok()) {
      std::cout << "Could not observe '" << stream_name << "'" << std::endl;
      for (const auto &node : graph_->Config().node()) {
        for (const auto &output_stream : node.output_stream()) {
          std::cout << "s: " << output_stream << std::endl;
        }
      }
    }
  });
}

// Small helper function for passing our additional graph setup to StartGraph.
void AdditionalGraphSetup() {
  // We ignore error status, since it will break and be clearly logged in any
  // case.
  // SetupRenderToScreenFromOutput(graph_.get(), kOutputGpuBufferStream);
  for (const auto &fn : action_list_) {
    fn();
  }
}

void CppClearGraphs() { graph_configs_.clear(); }

void CppPushBinaryGraph(const std::string &data) {
  CalculatorGraphConfig graph_config;
  // We grab the graph from the compiled binary data string.
  if (!graph_config.ParseFromArray(data.c_str(), data.length())) {
    printf("Subgraph config failed to parse from binary string!\n");
    return;
  }
  graph_configs_.push_back(graph_config);
  CycleGraph();
}

void CloseGraphInternal() {
  CHECK_OK(graph_->CloseAllInputStreams());
  CHECK_OK(graph_->WaitUntilDone());
}

void StartGraphInternal() {
  CHECK_OK(graph_->Initialize(graph_configs_, {}));
  AdditionalGraphSetup();
  CHECK_OK(graph_->StartRun(
      {{"aspect_ratio",
        mediapipe::Adopt(
            absl::make_unique<std::string>(aspect_ratio).release())}}));
}

std::unique_ptr<easyexif::EXIFInfo> GetExifInfo(const std::string &data) {
  auto info = absl::make_unique<easyexif::EXIFInfo>();
  info->parseFrom(data);
  return info;
}

bool CppSetAspectRatio(int aspect_left, int aspect_right) {
  aspect_ratio = absl::StrCat(aspect_left, ":", aspect_right);
  return true;
}

bool CppProcessPreAllocatedRawYuvBytes(int32 raw_yuv_bytes_ptr, int image_width,
                                       int image_height) {
  uint8 *data = reinterpret_cast<uint8 *>(raw_yuv_bytes_ptr);

  const int src_width = image_width;
  const int src_height = image_height;
  const size_t y_size = src_width * src_height;
  const size_t uv_size = src_width * src_height / 4;

  auto y = absl::make_unique<uint8[]>(y_size);
  auto u = absl::make_unique<uint8[]>(uv_size);
  auto v = absl::make_unique<uint8[]>(uv_size);

  const auto y_ptr = data;
  const auto u_ptr = y_ptr + y_size;
  const auto v_ptr = u_ptr + uv_size;

  libyuv::I420Copy(y_ptr, src_width, u_ptr, src_width / 2, v_ptr, src_width / 2,
                   y.get(), src_width, u.get(), src_width / 2, v.get(),
                   src_width / 2, src_width, src_height);

  auto header = absl::make_unique<mediapipe::VideoHeader>();
  header->height = src_height;
  header->width = src_width;
  header->format = drishti::ImageFormat::YCBCR420P;

  auto yuv_image = absl::make_unique<mediapipe::YUVImage>(
      libyuv::FOURCC_I420, std::move(y), src_width, std::move(u), src_width / 2,
      std::move(v), src_width / 2, src_width, src_height);
  auto video_size =
      absl::make_unique<std::pair<int, int>>(src_width, src_height);
  CHECK_OK(graph_->AddPacketToInputStream(
      "video_header",
      drishti::Adopt(header.release()).At(drishti::Timestamp(timestamp_))));
  CHECK_OK(graph_->AddPacketToInputStream(
      "input_yuv_raw_data",
      drishti::Adopt(yuv_image.release()).At(drishti::Timestamp(timestamp_))));
  CHECK_OK(graph_->AddPacketToInputStream(
      "video_size",
      drishti::Adopt(video_size.release()).At(drishti::Timestamp(timestamp_))));

  timestamp_ +=
      static_cast<int64>(drishti::Timestamp::kTimestampUnitsPerSecond / 15);
  return true;
}

bool CppProcessUpdatetimestamp() {
  timestamp_ +=
      static_cast<int64>(drishti::Timestamp::kTimestampUnitsPerSecond / 15);
  return true;
}

bool CppProcessPreAllocatedSize(int image_width, int image_height) {
  const int src_width = image_width;
  const int src_height = image_height;
  auto video_size =
      absl::make_unique<std::pair<int, int>>(src_width, src_height);
  CHECK_OK(graph_->AddPacketToInputStream(
      "video_size",
      drishti::Adopt(video_size.release()).At(drishti::Timestamp(timestamp_))));
  return true;
}

bool CppProcessPreAllocatedSalientRegions(
    const std::string &input_salient_regions) {
  auto salient_regions_proto =
      absl::make_unique<mediapipe::autoflip::DetectionSet>();
  apps::jspb::JspbFormat jspb_format;
  CHECK(jspb_format.ParseFromString(input_salient_regions,
                                    salient_regions_proto.get()));
  CHECK_OK(graph_->AddPacketToInputStream(
      "salient_regions_external",
      drishti::Adopt(salient_regions_proto.release())
          .At(drishti::Timestamp(timestamp_))));
  return true;
}

bool CppProcessPreAllocatedBorders(const std::string &input_borders) {
  auto borders_proto = absl::make_unique<mediapipe::autoflip::StaticFeatures>();
  apps::jspb::JspbFormat jspb_format;
  CHECK(jspb_format.ParseFromString(input_borders, borders_proto.get()));
  CHECK_OK(graph_->AddPacketToInputStream(
      "borders_external", drishti::Adopt(borders_proto.release())
                              .At(drishti::Timestamp(timestamp_))));
  return true;
}

bool CppProcessPreAllocatedShots(bool input_shot_change) {
  auto shot_change_proto = std::make_unique<bool>(input_shot_change);
  CHECK_OK(graph_->AddPacketToInputStream(
      "shot_change_external", drishti::Adopt(shot_change_proto.release())
                                  .At(drishti::Timestamp(timestamp_))));
  return true;
}

bool CppRunTillIdle() { return graph_->WaitUntilIdle().ok(); }

}  // namespace

void CycleGraph() {
  // Finish and close out the old graph, if any.
  if (started_graph_) {
    CloseGraphInternal();
  }

  // Graphs can only be initialized once, so need a new one.
  graph_.reset(new CalculatorGraph());

  // Start the graph running, with all pieces.
  StartGraphInternal();

  // if (!started_graph_) {
  //   SetupPassthroughShader();
  // }
  started_graph_ = true;
  timestamp_ = 0;
}

struct PacketListenerWrapper : public emscripten::wrapper<PacketListener> {
  EMSCRIPTEN_WRAPPER(PacketListenerWrapper);
  void onSize(const std::string &stream, const SizeRect &data) const {
    return call<void>("onSize", stream, data);
  }
  void onNumber(const std::string &stream, double number) const {
    return call<void>("onNumber", stream, number);
  }
  void onShot(const std::string &stream, bool shot_changed,
              double timestamp_seconds) const {
    return call<void>("onShot", stream, shot_changed, timestamp_seconds);
  }
  void onExternalRendering(const std::string &stream,
                           const std::string &frame_proto_str) const {
    return call<void>("onExternalRendering", stream, frame_proto_str);
  }
  void onFeature(const std::string &stream,
                 const std::string &feature_proto_str, double timestamp) const {
    return call<void>("onFeature", stream, feature_proto_str, timestamp);
  }
  void onBorder(const std::string &stream, const std::string &border_proto_str,
                double timestamp) const {
    return call<void>("onBorder", stream, border_proto_str, timestamp);
  }
};

EMSCRIPTEN_BINDINGS(graph_runner) {
  emscripten::function("clearGraphs", &CppClearGraphs);
  emscripten::function("pushBinarySubgraph", &CppPushBinaryGraph);
  emscripten::function("processRawYuvBytes",
                       &CppProcessPreAllocatedRawYuvBytes);
  emscripten::function("processSalientRegions",
                       &CppProcessPreAllocatedSalientRegions);
  emscripten::function("processBorders", &CppProcessPreAllocatedBorders);
  emscripten::function("processShots", &CppProcessPreAllocatedShots);
  emscripten::function("processSize", &CppProcessPreAllocatedSize);
  emscripten::function("processTimeStamp", &CppProcessUpdatetimestamp);
  emscripten::function("setAspectRatio", &CppSetAspectRatio);
  emscripten::function("attachListener", &AttachListener);
  emscripten::function("getExifInfo", &GetExifInfo);
  emscripten::function("runTillIdle", &CppRunTillIdle);
  emscripten::function("closeGraphInternal", &CloseGraphInternal);
  emscripten::function("cycleGraph", &CycleGraph);

  emscripten::class_<easyexif::EXIFInfo>("EXIFInfo")
      .property("orientation", &easyexif::EXIFInfo::Orientation)
      .property("imageWidth", &easyexif::EXIFInfo::ImageWidth)
      .property("imageHeight", &easyexif::EXIFInfo::ImageHeight);

  emscripten::function("changeBinaryGraph", &CppPushBinaryGraph);

  emscripten::value_object<SizeRect>("SizeRect")
      .field("x", &SizeRect::x)
      .field("y", &SizeRect::y);
  emscripten::class_<PacketListener>("PacketListener")
      .allow_subclass<PacketListenerWrapper>("PacketListenerWrapper")
      .function("onSize",
                emscripten::optional_override([](PacketListener &self,
                                                 const std::string &stream,
                                                 const SizeRect &data) {
                  return self.PacketListener::onSize(stream, data);
                }))
      .function("onShot",
                emscripten::optional_override(
                    [](PacketListener &self, const std::string &stream,
                       const bool &shotChanged, double timestamp_seconds) {
                      return self.PacketListener::onShot(stream, shotChanged,
                                                         timestamp_seconds);
                    }))
      .function("onExternalRendering",
                emscripten::optional_override(
                    [](PacketListener &self, const std::string &stream,
                       const std::string &frame_proto_str) {
                      return self.PacketListener::onExternalRendering(
                          stream, frame_proto_str);
                    }))
      .function("onNumber",
                emscripten::optional_override([](PacketListener &self,
                                                 const std::string &stream,
                                                 double number) {
                  return self.PacketListener::onNumber(stream, number);
                }))
      .function("onFeature",
                emscripten::optional_override(
                    [](PacketListener &self, const std::string &stream,
                       const std::string &feature_proto_str, double timestamp) {
                      return self.PacketListener::onFeature(
                          stream, feature_proto_str, timestamp);
                    }))
      .function("onBorder",
                emscripten::optional_override(
                    [](PacketListener &self, const std::string &stream,
                       const std::string &border_proto_str, double timestamp) {
                      return self.PacketListener::onBorder(
                          stream, border_proto_str, timestamp);
                    }));
}

}  // namespace wasm
}  // namespace drishti