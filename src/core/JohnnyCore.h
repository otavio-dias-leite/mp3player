#pragma once
#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace johnny {

enum class Resource : uint8_t { Display, Audio, Microphone, SdCard, WiFi, Bluetooth, SpiBus, I2cBus, Gpio, Count };
constexpr uint8_t kResourceCount = static_cast<uint8_t>(Resource::Count);
constexpr uint32_t resourceBit(Resource r) { return 1UL << static_cast<uint8_t>(r); }

class ResourceManager {
public:
  bool acquire(uint8_t owner, Resource resource);
  bool acquireMask(uint8_t owner, uint32_t mask);
  void release(uint8_t owner, Resource resource);
  void releaseOwner(uint8_t owner);
  bool ownedBy(uint8_t owner, Resource resource) const;
  uint32_t ownedMask(uint8_t owner) const;
private:
  uint8_t owners_[kResourceCount]{};
  mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
};

enum class AppState : uint8_t { Idle, Starting, Running, Stopping, Fault };
class AppModule {
public:
  virtual ~AppModule() = default;
  virtual uint8_t id() const = 0;
  virtual uint32_t requiredResources() const = 0;
  virtual bool begin() = 0;
  virtual void update() = 0;
  virtual void requestStop() = 0;
  virtual bool stopped() const = 0;
};

class AppManager {
public:
  explicit AppManager(ResourceManager& resources) : resources_(resources) {}
  bool launch(AppModule& app);
  void service();
  void stop();
  AppState state() const { return state_; }
  uint8_t activeAppId() const { return app_ ? app_->id() : 0; }
private:
  ResourceManager& resources_;
  AppModule* app_ = nullptr;
  AppState state_ = AppState::Idle;
};

constexpr size_t kManagedTaskLimit = 12;
struct ProcessInfo {
  char name[configMAX_TASK_NAME_LEN]{};
  uint8_t owner = 0;
  eTaskState state = eInvalid;
  UBaseType_t priority = 0;
  UBaseType_t stackHighWaterWords = 0;
};
class ProcessRegistry {
public:
  bool add(TaskHandle_t handle, uint8_t owner);
  void remove(TaskHandle_t handle);
  size_t snapshot(ProcessInfo* output, size_t capacity) const;
private:
  struct Entry { TaskHandle_t handle = nullptr; uint8_t owner = 0; };
  Entry entries_[kManagedTaskLimit]{};
  mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
};

namespace board {
constexpr int kSdCs=12, kSpiSck=40, kSpiMiso=39, kSpiMosi=14;
constexpr int kExtDisplayCs=5, kExtI2cSda=8, kExtI2cScl=9;
constexpr int kRgbLedData=21, kDisplayLedPower=38;
constexpr bool isReservedPin(int gpio) {
  return gpio==12 || gpio==21 || gpio==38 || gpio==40 || gpio==39 ||
         gpio==14 || gpio==8 || gpio==9;
}
}

} // namespace johnny
