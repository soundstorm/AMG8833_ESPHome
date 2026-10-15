#pragma once

#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "esphome/core/helpers.h"
#include "esphome/core/version.h"
#include "esphome/components/i2c/i2c.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/web_server_base/web_server_base.h"

#include <string>
#include <vector>

namespace esphome {
namespace amg8833 {

enum Axis : uint8_t { AXIS_COLUMNS = 0, AXIS_ROWS = 1 };

class AMG8833Component;

class AMG8833Handler : public AsyncWebHandler {
 public:
  explicit AMG8833Handler(AMG8833Component *parent) : parent_(parent) {}
#if defined(ESPHOME_VERSION_CODE) && ESPHOME_VERSION_CODE >= VERSION_CODE(2025, 7, 0)
  bool canHandle(AsyncWebServerRequest *request) const override;
#else
  bool canHandle(AsyncWebServerRequest *request) override;
#endif
  void handleRequest(AsyncWebServerRequest *request) override;

 protected:
  AMG8833Component *parent_;
};

class AMG8833Component : public PollingComponent, public i2c::I2CDevice {
 public:
  void setup() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_thermistor_sensor(sensor::Sensor *s) { thermistor_ = s; }
  void set_max_temperature_sensor(sensor::Sensor *s) { max_temp_ = s; }
  void set_frame_text_sensor(text_sensor::TextSensor *s) { frame_ = s; }
  void set_presence_sensor(binary_sensor::BinarySensor *s) { presence_ = s; }
  void set_entered_sensor(sensor::Sensor *s) { entered_sensor_ = s; }
  void set_exited_sensor(sensor::Sensor *s) { exited_sensor_ = s; }
  void set_count_sensor(sensor::Sensor *s) { count_sensor_ = s; }
  void set_frame_range(float min, float max) { frame_min_ = min; frame_max_ = max; }
  void set_frame_interval(uint32_t ms) { frame_interval_ = ms; }
  void set_counting(Axis axis, bool invert, float warm_delta, int min_pixels, float min_travel, int release_frames,
                    float background_rate, uint32_t auto_recalibrate_ms) {
    background_rate_ = background_rate;
    auto_recalibrate_ms_ = auto_recalibrate_ms;
    counting_ = true;
    axis_ = axis;
    invert_ = invert;
    warm_delta_ = warm_delta;
    min_pixels_ = min_pixels;
    min_travel_ = min_travel;
    release_frames_ = release_frames;
  }
  void set_image_endpoint(bool bmp, bool svg, bool viewer, int scale) {
    bmp_ = bmp;
    svg_ = svg;
    viewer_ = viewer;
    scale_ = scale;
  }
  void add_on_enter_callback(std::function<void()> &&cb) { this->enter_callbacks_.add(std::move(cb)); }
  void add_on_exit_callback(std::function<void()> &&cb) { this->exit_callbacks_.add(std::move(cb)); }
  void set_log_frames(bool log_frames) { log_frames_ = log_frames; }
  void set_web_server_base(web_server_base::WebServerBase *base) { base_ = base; }

  /// Relearns the background (takes about 3 s, no counting meanwhile). The room should be empty.
  void recalibrate();
  /// Sets the number of people in the room (e.g. after a restart).
  void set_count(int value);

  const float *pixels() const { return pixels_; }
  bool bmp_enabled() const { return bmp_; }
  bool svg_enabled() const { return svg_; }
  bool viewer_enabled() const { return viewer_; }
  std::string build_svg() const;
  /// Builds the image (24-bit BMP, bilinearly upscaled) into an internal buffer that stays valid until the next call.
  const std::vector<uint8_t> &build_bmp();

 protected:
  bool read_frame_();
  void process_frame_();
  void publish_counts_();
  void publish_frame_();
  void log_frame_(int n_warm, float pos);

  sensor::Sensor *thermistor_{nullptr};
  sensor::Sensor *max_temp_{nullptr};
  text_sensor::TextSensor *frame_{nullptr};
  binary_sensor::BinarySensor *presence_{nullptr};
  sensor::Sensor *entered_sensor_{nullptr};
  sensor::Sensor *exited_sensor_{nullptr};
  sensor::Sensor *count_sensor_{nullptr};
  web_server_base::WebServerBase *base_{nullptr};

  CallbackManager<void()> enter_callbacks_;
  CallbackManager<void()> exit_callbacks_;
  bool bmp_{false}, svg_{false}, viewer_{false};
  int scale_{8};
  std::vector<uint8_t> bmp_buf_;

  bool log_frames_{false};
  int16_t raw_[64]{};
  uint32_t start_ms_{0};
  int active_frames_{0};
  int max_warm_{0};
  float pixels_[64]{};
  float background_[64]{};
  float thermistor_temp_{0};
  uint16_t warmup_frames_{0};

  float frame_min_{10}, frame_max_{40};
  uint32_t frame_interval_{1000};
  uint32_t last_frame_publish_{0};
  uint32_t last_thermistor_read_{0};

  // People counting
  bool counting_{false};
  Axis axis_{AXIS_COLUMNS};
  bool invert_{false};
  float warm_delta_{2.0f};
  int min_pixels_{3};
  float min_travel_{2.0f};
  int release_frames_{3};
  float background_rate_{0.005f};
  uint32_t auto_recalibrate_ms_{120000};
  uint32_t present_since_{0};
  bool active_{false};
  int lost_{0};
  float start_pos_{0}, last_pos_{0};
  int entered_{0}, exited_{0}, count_{0};
  bool counts_dirty_{true};
};

template<typename... Ts> class RecalibrateAction : public Action<Ts...>, public Parented<AMG8833Component> {
 public:
  void play(const Ts &...x) override { this->parent_->recalibrate(); }
};

class EnterTrigger : public Trigger<> {
 public:
  explicit EnterTrigger(AMG8833Component *parent) {
    parent->add_on_enter_callback([this]() { this->trigger(); });
  }
};

class ExitTrigger : public Trigger<> {
 public:
  explicit ExitTrigger(AMG8833Component *parent) {
    parent->add_on_exit_callback([this]() { this->trigger(); });
  }
};

}  // namespace amg8833
}  // namespace esphome
