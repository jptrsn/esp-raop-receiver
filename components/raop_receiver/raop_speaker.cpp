#include "raop_speaker.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/components/network/util.h"

#include <mdns.h>
#include <driver/i2s_std.h>

extern "C" {
#include "esp_raop_receiver.h"
}







namespace esphome {
namespace raop_receiver {

static const char *const TAG = "raop_receiver";

void RAOPSpeaker::setup() {
  ESP_LOGCONFIG(TAG, "RAOP Speaker component registered");
}

void RAOPSpeaker::setup_raop() {
  ESP_LOGCONFIG(TAG, "Setting up RAOP Speaker...");

  // Initialize RAOP library
  raop_config_t config = {};

  // Device name
  if (!this->device_name_.empty()) {
    config.device_name = this->device_name_.c_str();
  } else {
    config.device_name = nullptr;  // Library auto-generates
  }

  // Network (library auto-detects)
  config.mac_address = nullptr;
  config.ip_address = 0;

  // Audio configuration
  config.latency_frames = 0;  // Use library default
  config.volume_mode = RAOP_VOLUME_HARDWARE;  // We get volume callbacks

  // mDNS - ESPHome manages it
  config.mdns_mode = RAOP_MDNS_EXTERNAL;
  config.mdns_hostname = nullptr;

  // Callbacks
  config.audio_output_cb = &RAOPSpeaker::audio_output_callback_;
  config.event_cb = &RAOPSpeaker::event_callback_;
  config.volume_cb = &RAOPSpeaker::volume_callback_;
  config.user_ctx = this;

  // Initialize library
  ESP_LOGCONFIG(TAG, "Initializing RAOP with device name: '%s'",
    config.device_name ? config.device_name : "(null - auto)");
  esp_err_t err = raop_init(&config, &this->raop_handle_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize RAOP receiver: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }

  ESP_LOGCONFIG(TAG, "RAOP receiver initialized");
  ESP_LOGCONFIG(TAG, "Device name: %s", raop_get_device_name(this->raop_handle_));

  register_mdns_service_();
}

void RAOPSpeaker::register_mdns_service_() {
  // Get MAC address and format as AABBCCDDEEFF@Device Name
  std::string mac = get_mac_address_pretty();
  mac.erase(std::remove(mac.begin(), mac.end(), ':'), mac.end());
  std::string instance = mac + "@" + std::string(raop_get_device_name(this->raop_handle_));

  mdns_txt_item_t txt[] = {
    {"txtvers", "1"},
    {"ch", "2"},
    {"cn", "0,1"},
    {"ek", "1"},
    {"et", "0,1"},
    {"md", "0,1,2"},
    {"sm", "false"},
    {"sr", "44100"},
    {"ss", "16"},
    {"sv", "false"},
    {"tp", "UDP"},
    {"vn", "3"},
    {"am", "esp32air"}
  };

  esp_err_t err = mdns_service_add(instance.c_str(), "_raop", "_tcp", 5000, txt, 12);

  if (err == ESP_OK) {
    ESP_LOGI(TAG, "Registered RAOP service: %s", instance.c_str());
  } else {
    ESP_LOGE(TAG, "Failed to register RAOP service: %s", esp_err_to_name(err));
  }
}

void RAOPSpeaker::loop() {
  // RAOP library runs in its own tasks
  if (!this->raop_initialized_ && network::is_connected()) {
    this->setup_raop();
    this->raop_initialized_ = true;
  }
}

void RAOPSpeaker::dump_config() {
  ESP_LOGCONFIG(TAG, "RAOP Receiver Speaker:");
  ESP_LOGCONFIG(TAG, "  Device Name: %s",
                this->device_name_.empty() ? "auto" : this->device_name_.c_str());
  ESP_LOGCONFIG(TAG, "  I2S DOUT Pin: GPIO%d", this->dout_pin_);
  if (this->raop_handle_) {
    ESP_LOGCONFIG(TAG, "  Active Name: %s", raop_get_device_name(this->raop_handle_));
  }
}

// Speaker interface
size_t RAOPSpeaker::play(const uint8_t *data, size_t length) {
  // RAOP receiver doesn't accept input from ESPHome
  ESP_LOGW(TAG, "play() called but RAOP is receive-only");
  return 0;
}

size_t RAOPSpeaker::play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) {
  return this->play(data, length);
}

void RAOPSpeaker::start() {
  ESP_LOGD(TAG, "start() called");
  // RAOP stream starts when sender connects
}

void RAOPSpeaker::stop() {
  ESP_LOGD(TAG, "stop() called");
  if (this->i2s_locked_) {
    this->stop_i2s_();
  }
}

void RAOPSpeaker::finish() {
  this->stop();
}

bool RAOPSpeaker::has_buffered_data() const {
  return this->stream_active_;
}

// I2S management
bool RAOPSpeaker::start_i2s_() {
  if (this->i2s_locked_) {
    return true;
  }

  if (!this->parent_->try_lock()) {
    ESP_LOGW(TAG, "Cannot start I2S - port is locked");
    return false;
  }

  // Get pin config from parent
  i2s_std_gpio_config_t pin_config = this->parent_->get_pin_config();
  pin_config.dout = (gpio_num_t) this->dout_pin_;

  // Configure I2S channel
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(this->parent_->get_port(), I2S_ROLE_MASTER);
  esp_err_t err = i2s_new_channel(&chan_cfg, &this->tx_handle_, nullptr);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to create I2S channel: %s", esp_err_to_name(err));
    this->parent_->unlock();
    return false;
  }

  // Configure standard mode (stereo, 16-bit, 44.1kHz)
  i2s_std_config_t std_cfg = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
    .gpio_cfg = pin_config,
  };

  err = i2s_channel_init_std_mode(this->tx_handle_, &std_cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to init I2S standard mode: %s", esp_err_to_name(err));
    i2s_del_channel(this->tx_handle_);
    this->parent_->unlock();
    return false;
  }

  err = i2s_channel_enable(this->tx_handle_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to enable I2S channel: %s", esp_err_to_name(err));
    i2s_del_channel(this->tx_handle_);
    this->parent_->unlock();
    return false;
  }

  this->i2s_locked_ = true;
  ESP_LOGI(TAG, "I2S started");
  return true;
}

void RAOPSpeaker::stop_i2s_() {
  if (!this->i2s_locked_) {
    return;
  }

  i2s_channel_disable(this->tx_handle_);
  i2s_del_channel(this->tx_handle_);
  this->tx_handle_ = nullptr;
  this->parent_->unlock();
  this->i2s_locked_ = false;
  ESP_LOGI(TAG, "I2S stopped");
}

// RAOP callbacks
void RAOPSpeaker::audio_output_callback_(const uint8_t *data, size_t len, void *user_ctx) {
  auto *speaker = static_cast<RAOPSpeaker *>(user_ctx);
  speaker->handle_audio_output_(data, len);
}

void RAOPSpeaker::event_callback_(raop_event_t event, void *event_data, void *user_ctx) {
  auto *speaker = static_cast<RAOPSpeaker *>(user_ctx);
  speaker->handle_raop_event_(event, event_data);
}

void RAOPSpeaker::volume_callback_(float volume, void *user_ctx) {
  auto *speaker = static_cast<RAOPSpeaker *>(user_ctx);
  speaker->handle_volume_change_(volume);
}

void RAOPSpeaker::handle_audio_output_(const uint8_t *data, size_t len) {
  if (!this->i2s_locked_) {
    ESP_LOGW(TAG, "Audio callback but I2S not started");
    return;
  }

  size_t bytes_written = 0;

  // Apply volume scaling if needed
  if (this->volume_ < 0.99f) {
    // Scale 16-bit samples
    std::vector<int16_t> scaled(len / 2);
    const int16_t *samples = reinterpret_cast<const int16_t *>(data);

    for (size_t i = 0; i < len / 2; i++) {
      int32_t sample = static_cast<int32_t>(samples[i] * this->volume_);
      // Clamp to prevent overflow
      if (sample > 32767) sample = 32767;
      if (sample < -32768) sample = -32768;
      scaled[i] = static_cast<int16_t>(sample);
    }

    esp_err_t err = i2s_channel_write(this->tx_handle_, scaled.data(), len, &bytes_written, portMAX_DELAY);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "I2S write failed: %s", esp_err_to_name(err));
    }
  } else {
    // Full volume - write directly
    esp_err_t err = i2s_channel_write(this->tx_handle_, data, len, &bytes_written, portMAX_DELAY);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "I2S write failed: %s", esp_err_to_name(err));
    }
  }
}

void RAOPSpeaker::handle_raop_event_(raop_event_t event, void *event_data) {
  switch (event) {
    case RAOP_EVENT_CONNECTED:
      ESP_LOGI(TAG, "AirPlay client connected");
      break;

    case RAOP_EVENT_DISCONNECTED:
      ESP_LOGI(TAG, "AirPlay client disconnected");
      this->stream_active_ = false;
      this->stop_i2s_();
      break;

    case RAOP_EVENT_BUFFERING:
      ESP_LOGI(TAG, "Buffering audio...");
      // Try to start I2S
      if (!this->start_i2s_()) {
        ESP_LOGW(TAG, "Failed to start I2S - stream may fail");
      }
      break;

    case RAOP_EVENT_PLAYING:
      ESP_LOGI(TAG, "Playback started");
      this->stream_active_ = true;
      if (!this->i2s_locked_) {
        this->start_i2s_();
      }
      break;

    case RAOP_EVENT_STOPPED:
    case RAOP_EVENT_PAUSED:
      ESP_LOGI(TAG, "Playback paused");
      this->stream_active_ = false;
      this->stop_i2s_();
      break;

    case RAOP_EVENT_METADATA: {
      auto *meta = static_cast<raop_metadata_t *>(event_data);
      ESP_LOGI(TAG, "Now playing: %s - %s",
               meta->artist ? meta->artist : "Unknown",
               meta->title ? meta->title : "Unknown");
      break;
    }

    default:
      break;
  }
}

void RAOPSpeaker::handle_volume_change_(float volume) {
  ESP_LOGD(TAG, "Volume changed to %.2f", volume);
  this->volume_ = volume;
  // TODO: Apply volume - either via DAC or software scaling
}

}  // namespace raop_receiver
}  // namespace esphome