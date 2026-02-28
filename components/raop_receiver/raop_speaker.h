#pragma once

#include "esphome/core/component.h"
#include "esphome/components/speaker/speaker.h"
#include "esphome/components/i2s_audio/i2s_audio.h"
#include "esp_raop_receiver.h"

namespace esphome {
namespace raop_receiver {

class RAOPSpeaker : public i2s_audio::I2SAudioOut, public speaker::Speaker, public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override {
    return setup_priority::AFTER_WIFI - 1.0f;
  }

  // Configuration
  void set_device_name(const std::string &name) { this->device_name_ = name; }
  void set_dout_pin(uint8_t pin) { this->dout_pin_ = pin; }

  // Speaker interface
  size_t play(const uint8_t *data, size_t length) override;
  size_t play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) override;
  void start() override;
  void stop() override;
  void finish() override;
  bool has_buffered_data() const override;

 protected:
  // RAOP library callbacks (static, route to instance)
  static void audio_output_callback_(const uint8_t *data, size_t len, void *user_ctx);
  static void event_callback_(raop_event_t event, void *event_data, void *user_ctx);
  static void volume_callback_(float volume, void *user_ctx);

  // Instance methods called by static callbacks
  void handle_audio_output_(const uint8_t *data, size_t len);
  void handle_raop_event_(raop_event_t event, void *event_data);
  void handle_volume_change_(float volume);

  // I2S management
  bool start_i2s_();
  void stop_i2s_();

  // Setup RAOP receiver
  void setup_raop();
  void register_mdns_service_();

  raop_handle_t *raop_handle_{nullptr};
  i2s_chan_handle_t tx_handle_{nullptr};
  std::string device_name_{};
  uint8_t dout_pin_{0};
  bool raop_initialized_{false};
  float volume_{1.0f};
  bool i2s_locked_{false};
  bool stream_active_{false};
};

}  // namespace raop_receiver
}  // namespace esphome