#include "frame_cap_pipeline.hpp"

#include "who_cam.hpp"

namespace smart_alarm {
namespace {
constexpr uint8_t PEDESTRIAN_MODEL_FRAME_LATENCY = 3;
} // namespace

who::frame_cap::WhoFrameCap *make_frame_cap_pipeline()
{
    framesize_t frame_size = who::cam::get_cam_frame_size_from_lcd_resolution();
    auto *cam = new who::cam::WhoS3Cam(PIXFORMAT_RGB565, frame_size, PEDESTRIAN_MODEL_FRAME_LATENCY + 2);
    auto *frame_cap = new who::frame_cap::WhoFrameCap();
    frame_cap->add_node<who::frame_cap::WhoFetchNode>("FrameCapFetch", cam);
    return frame_cap;
}
} // namespace smart_alarm
