#include "amg8833.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include <algorithm>
#include <cstdio>

namespace esphome {
namespace amg8833 {

static const char *const TAG = "amg8833";

static const uint8_t REG_PCTL = 0x00;
static const uint8_t REG_RST = 0x01;
static const uint8_t REG_FPSC = 0x02;
static const uint8_t REG_TTHL = 0x0E;
static const uint8_t REG_PIXEL = 0x80;
static const uint16_t WARMUP_FRAMES = 30;

void AMG8833Component::setup() {
  if (!this->write_byte(REG_PCTL, 0x00)) {  // Normal Mode
    ESP_LOGE(TAG, "AMG8833 not found");
    this->mark_failed();
    return;
  }
  delay(50);
  this->write_byte(REG_RST, 0x3F);  // Initial Reset
  delay(2);
  this->write_byte(REG_FPSC, 0x00);  // 10 fps
  delay(100);

  if (this->base_ != nullptr) {
    // init() is reference-counted: it starts the HTTP listener if no other component (web_server, ...) did yet.
    this->base_->init();
    this->base_->add_handler(new AMG8833Handler(this));  // NOLINT
  }
}

void AMG8833Component::dump_config() {
  ESP_LOGCONFIG(TAG, "AMG8833:");
  LOG_I2C_DEVICE(this);
  LOG_UPDATE_INTERVAL(this);
  ESP_LOGCONFIG(TAG, "  People counting: %s", YESNO(this->counting_));
  if (this->counting_) {
    ESP_LOGCONFIG(TAG, "  Axis: %s%s, warm_delta: %.1f, min_pixels: %d, min_travel: %.1f, release_frames: %d, background_rate: %.3f, auto_recalibrate_after: %u ms",
                  this->axis_ == AXIS_COLUMNS ? "columns" : "rows", this->invert_ ? " (inverted)" : "",
                  this->warm_delta_, this->min_pixels_, this->min_travel_, this->release_frames_, this->background_rate_,
                  (unsigned) this->auto_recalibrate_ms_);
  }
  ESP_LOGCONFIG(TAG, "  Image endpoint: bmp=%s svg=%s viewer=%s scale=%d", YESNO(this->bmp_), YESNO(this->svg_),
                YESNO(this->viewer_), this->scale_);
  LOG_SENSOR("  ", "Thermistor", this->thermistor_);
  LOG_SENSOR("  ", "Max. Temperatur", this->max_temp_);
}

bool AMG8833Component::read_frame_() {
  uint8_t buf[128];
  // read in 32-byte blocks (Wire buffer limit)
  for (uint8_t i = 0; i < 4; i++) {
    if (this->read_register(REG_PIXEL + 32 * i, buf + 32 * i, 32) != i2c::ERROR_OK)
      return false;
  }
  for (int i = 0; i < 64; i++) {
    int16_t raw = ((buf[2 * i + 1] & 0x0F) << 8) | buf[2 * i];
    if (raw & 0x800)
      raw -= 4096;  // 12-bit two's complement
    this->pixels_[i] = raw * 0.25f;
  }
  return true;
}

void AMG8833Component::update() {
  if (this->is_failed())
    return;
  if (!this->read_frame_()) {
    ESP_LOGW(TAG, "Reading frame failed");
    this->status_set_warning();
    return;
  }
  this->status_clear_warning();
  this->process_frame_();
  this->publish_frame_();

  uint32_t now = millis();
  if (now - this->last_thermistor_read_ > 10000 || this->last_thermistor_read_ == 0) {
    this->last_thermistor_read_ = now;
    uint8_t t[2];
    if (this->read_register(REG_TTHL, t, 2) == i2c::ERROR_OK) {
      int16_t raw = ((t[1] & 0x07) << 8) | t[0];
      if (t[1] & 0x08)
        raw = -raw;  // sign-magnitude
      this->thermistor_temp_ = raw * 0.0625f;
      if (this->thermistor_ != nullptr)
        this->thermistor_->publish_state(this->thermistor_temp_);
    }
  }
}

void AMG8833Component::process_frame_() {
  float max_t = -100;
  for (int i = 0; i < 64; i++)
    max_t = std::max(max_t, this->pixels_[i]);
  if (this->max_temp_ != nullptr)
    this->max_temp_->publish_state(max_t);

  // Warm-up: initialize the background as the mean of the first frames
  if (this->warmup_frames_ < WARMUP_FRAMES) {
    for (int i = 0; i < 64; i++)
      this->background_[i] = (this->warmup_frames_ == 0)
                                 ? this->pixels_[i]
                                 : this->background_[i] + (this->pixels_[i] - this->background_[i]) / (this->warmup_frames_ + 1);
    this->warmup_frames_++;
    return;
  }

  // Pixels warmer than the background, weighted centroid
  int n_warm = 0;
  float sum_w = 0, sum_pos = 0;
  for (int i = 0; i < 64; i++) {
    float diff = this->pixels_[i] - this->background_[i];
    if (diff > this->warm_delta_) {
      n_warm++;
      float pos = (this->axis_ == AXIS_COLUMNS) ? (i % 8) : (i / 8);
      sum_w += diff;
      sum_pos += diff * pos;
    }
  }
  bool present = n_warm >= this->min_pixels_;

  // Only track the background while nobody is in view (slowly, about 1/200 per frame)
  if (!present && !this->active_) {
    for (int i = 0; i < 64; i++)
      this->background_[i] += (this->pixels_[i] - this->background_[i]) * this->background_rate_;
  }

  // Presence that never ends (e.g. a heater or sunlight changed the scene): relearn the background
  if (present && this->auto_recalibrate_ms_ > 0) {
    uint32_t now = millis();
    if (this->present_since_ == 0)
      this->present_since_ = now;
    if (now - this->present_since_ > this->auto_recalibrate_ms_) {
      ESP_LOGW(TAG, "Presence lasted longer than %u ms, recalibrating background", (unsigned) this->auto_recalibrate_ms_);
      this->recalibrate();
      return;
    }
  } else if (!present) {
    this->present_since_ = 0;
  }

  if (this->presence_ != nullptr)
    this->presence_->publish_state(present);

  if (!this->counting_)
    return;

  if (present) {
    float pos = sum_pos / sum_w;
    if (this->invert_)
      pos = 7.0f - pos;
    if (!this->active_) {
      this->active_ = true;
      this->start_pos_ = pos;
    }
    this->last_pos_ = pos;
    this->lost_ = 0;
  } else if (this->active_) {
    if (++this->lost_ >= this->release_frames_) {
      float travel = this->last_pos_ - this->start_pos_;
      if (travel >= this->min_travel_) {
        this->entered_++;
        this->count_++;
        ESP_LOGI(TAG, "Person entered (travel %.1f), in room: %d", travel, this->count_);
        this->enter_callbacks_.call();
      } else if (travel <= -this->min_travel_) {
        this->exited_++;
        if (this->count_ > 0)
          this->count_--;
        ESP_LOGI(TAG, "Person left (travel %.1f), in room: %d", travel, this->count_);
        this->exit_callbacks_.call();
      } else {
        ESP_LOGD(TAG, "Movement ignored (travel %.1f < %.1f)", travel, this->min_travel_);
      }
      this->active_ = false;
      this->counts_dirty_ = true;
    }
  }
  this->publish_counts_();
}

void AMG8833Component::recalibrate() {
  ESP_LOGI(TAG, "Recalibrating background");
  this->warmup_frames_ = 0;
  this->active_ = false;
  this->lost_ = 0;
  this->present_since_ = 0;
  if (this->presence_ != nullptr)
    this->presence_->publish_state(false);
}

void AMG8833Component::set_count(int value) {
  this->count_ = std::max(0, value);
  this->counts_dirty_ = true;
  this->publish_counts_();
}

void AMG8833Component::publish_counts_() {
  if (!this->counts_dirty_)
    return;
  this->counts_dirty_ = false;
  if (this->entered_sensor_ != nullptr)
    this->entered_sensor_->publish_state(this->entered_);
  if (this->exited_sensor_ != nullptr)
    this->exited_sensor_->publish_state(this->exited_);
  if (this->count_sensor_ != nullptr)
    this->count_sensor_->publish_state(this->count_);
}

void AMG8833Component::publish_frame_() {
  if (this->frame_ == nullptr)
    return;
  uint32_t now = millis();
  if (now - this->last_frame_publish_ < this->frame_interval_)
    return;
  this->last_frame_publish_ = now;
  // 64 pixels, 1 byte each (0..255 between frame_min and frame_max), as 128 hex characters
  std::string s;
  s.reserve(128);
  char tmp[3];
  for (int i = 0; i < 64; i++) {
    float n = (this->pixels_[i] - this->frame_min_) / (this->frame_max_ - this->frame_min_);
    int v = (int) (std::min(1.0f, std::max(0.0f, n)) * 255.0f + 0.5f);
    snprintf(tmp, sizeof(tmp), "%02x", v);
    s += tmp;
  }
  this->frame_->publish_state(s);
}

struct Rgb {
  uint8_t r, g, b;
};

static Rgb heat_color(float n) {
  // blue -> cyan -> green -> yellow -> red
  n = std::min(1.0f, std::max(0.0f, n));
  float r, g, b;
  if (n < 0.25f) { r = 0; g = n * 4; b = 1; }
  else if (n < 0.5f) { r = 0; g = 1; b = 1 - (n - 0.25f) * 4; }
  else if (n < 0.75f) { r = (n - 0.5f) * 4; g = 1; b = 0; }
  else { r = 1; g = 1 - (n - 0.75f) * 4; b = 0; }
  return {(uint8_t) (r * 255), (uint8_t) (g * 255), (uint8_t) (b * 255)};
}

std::string AMG8833Component::build_svg() const {
  std::string s;
  s.reserve(4096);
  s += "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 8 8' width='480' height='480'>"
       "<defs><filter id='b' x='0' y='0' width='1' height='1'><feGaussianBlur stdDeviation='.3'/></filter></defs>"
       "<g filter='url(#b)'>";
  char buf[96];
  for (int i = 0; i < 64; i++) {
    Rgb c = heat_color((this->pixels_[i] - this->frame_min_) / (this->frame_max_ - this->frame_min_));
    snprintf(buf, sizeof(buf), "<rect x='%d' y='%d' width='1.02' height='1.02' fill='#%02x%02x%02x'/>", i % 8, i / 8,
             c.r, c.g, c.b);
    s += buf;
  }
  s += "</g></svg>";
  return s;
}

const std::vector<uint8_t> &AMG8833Component::build_bmp() {
  const int w = 8 * this->scale_, h = w;
  const int row = (w * 3 + 3) & ~3;
  const uint32_t size = 54 + row * h;
  auto &b = this->bmp_buf_;
  b.assign(size, 0);
  auto le32 = [&](int off, uint32_t v) {
    for (int i = 0; i < 4; i++)
      b[off + i] = (v >> (8 * i)) & 0xFF;
  };
  b[0] = 'B';
  b[1] = 'M';
  le32(2, size);
  le32(10, 54);
  le32(14, 40);  // BITMAPINFOHEADER
  le32(18, w);
  le32(22, h);  // positive = bottom-up row order
  b[26] = 1;
  b[28] = 24;
  le32(34, row * h);

  const float range = this->frame_max_ - this->frame_min_;
  for (int y = 0; y < h; y++) {
    float sy = std::min(7.0f, std::max(0.0f, (y + 0.5f) / this->scale_ - 0.5f));
    int y0 = (int) sy, y1 = std::min(7, y0 + 1);
    float fy = sy - y0;
    uint8_t *out = &b[54 + (h - 1 - y) * row];
    for (int x = 0; x < w; x++) {
      float sx = std::min(7.0f, std::max(0.0f, (x + 0.5f) / this->scale_ - 0.5f));
      int x0 = (int) sx, x1 = std::min(7, x0 + 1);
      float fx = sx - x0;
      float t = (this->pixels_[y0 * 8 + x0] * (1 - fx) + this->pixels_[y0 * 8 + x1] * fx) * (1 - fy) +
                (this->pixels_[y1 * 8 + x0] * (1 - fx) + this->pixels_[y1 * 8 + x1] * fx) * fy;
      Rgb c = heat_color((t - this->frame_min_) / range);
      out[x * 3] = c.b;
      out[x * 3 + 1] = c.g;
      out[x * 3 + 2] = c.r;
    }
  }
  return b;
}

// Newer ESPHome versions expose the URL via url_to(), older ones / Arduino via url().
template<typename R> static auto request_url(R *r, int) -> decltype(std::string(r->url().c_str())) {
  return std::string(r->url().c_str());
}
template<typename R> static std::string request_url(R *r, long) {
  char buf[R::URL_BUF_SIZE];
  return r->url_to(buf).str();
}

#if defined(ESPHOME_VERSION_CODE) && ESPHOME_VERSION_CODE >= VERSION_CODE(2025, 7, 0)
bool AMG8833Handler::canHandle(AsyncWebServerRequest *request) const {
#else
bool AMG8833Handler::canHandle(AsyncWebServerRequest *request) {
#endif
  if (request->method() != HTTP_GET)
    return false;
  std::string url = request_url(request, 0);
  return (this->parent_->bmp_enabled() && url == "/thermal.bmp") ||
         (this->parent_->svg_enabled() && url == "/thermal.svg") ||
         (this->parent_->viewer_enabled() && url == "/thermal");
}

void AMG8833Handler::handleRequest(AsyncWebServerRequest *request) {
  std::string url = request_url(request, 0);
  if (url == "/thermal.bmp") {
    const auto &bmp = this->parent_->build_bmp();
#ifdef USE_ARDUINO
    AsyncWebServerResponse *res = request->beginResponse_P(200, "image/bmp", bmp.data(), bmp.size());
#else
    AsyncWebServerResponse *res = request->beginResponse(200, "image/bmp", bmp.data(), bmp.size());
#endif
    res->addHeader("Cache-Control", "no-store");
    request->send(res);
    return;
  }
  if (url == "/thermal.svg") {
    std::string svg = this->parent_->build_svg();
    request->send(200, "image/svg+xml", svg.c_str());
    return;
  }
  std::string page =
      "<!doctype html><meta name=viewport content='width=device-width'>"
      "<body style='margin:0;background:#111'><img id=i style='width:100%;max-width:480px;image-rendering:auto'>"
      "<script>const u='";
  page += this->parent_->bmp_enabled() ? "/thermal.bmp" : "/thermal.svg";
  page += "';function r(){i.src=u+'?'+Date.now()}r();i.onload=i.onerror=()=>setTimeout(r,500)</script>";
  request->send(200, "text/html", page.c_str());
}

}  // namespace amg8833
}  // namespace esphome
