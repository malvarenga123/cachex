#ifndef CACHEX_RESULT_H
#define CACHEX_RESULT_H

#include <cstdint>
#include <utility>
#include <vector>

#include "scsi_status.h"

struct CommandResult
{
  // MaxCapacity reserves the DMA buffer upfront so the platform layer can
  // pass Data.data() directly as the transfer target. After a successful
  // transfer the platform layer resizes Data to the actual bytes received.
  // Data is intentionally left empty until then — never pre-filled — so
  // callers don't accidentally read uninitialised bytes as valid results.
  explicit CommandResult(unsigned int MaxCapacity)
      : Duration(0.0), Valid(false), ScsiStatusCode(0xff)
  {
    Data.reserve(MaxCapacity);
    Data.resize(MaxCapacity); // platform layers write into Data.data()
  }

  std::vector<std::uint8_t> Data;
  double Duration;
  bool Valid;
  std::uint8_t ScsiStatusCode;

  operator bool() const { return Valid && ScsiStatusCode == ScsiStatus::GOOD; }
};

// ---------------------------------------------------------------------------
// DeviceHandle<Platform>
//
// RAII wrapper around a platform device handle. Opens on construction,
// closes on destruction. Non-copyable; movable.
//
// Usage:
//   DeviceHandle<platform> h(path);
//   if (!h.valid()) { ... }
//   platform::exec_command(h.get(), rv, cdb);
// ---------------------------------------------------------------------------
template <typename Platform>
class DeviceHandle
{
public:
  using handle_type = typename Platform::device_handle;

  explicit DeviceHandle(const char *path)
      : handle_(Platform::open_volume(path))
  {
  }

  ~DeviceHandle()
  {
    if (Platform::handle_is_valid(handle_))
      Platform::close_handle(handle_);
  }

  // Non-copyable
  DeviceHandle(const DeviceHandle &) = delete;
  DeviceHandle &operator=(const DeviceHandle &) = delete;

  // Movable
  DeviceHandle(DeviceHandle &&other) noexcept
      : handle_(std::move(other.handle_))
  {
    // Leave other in a known-invalid state so its destructor skips close.
    other.invalidate();
  }
  DeviceHandle &operator=(DeviceHandle &&other) noexcept
  {
    if (this != &other)
    {
      if (Platform::handle_is_valid(handle_))
        Platform::close_handle(handle_);
      handle_ = std::move(other.handle_);
      other.invalidate();
    }
    return *this;
  }

  bool valid() const { return Platform::handle_is_valid(handle_); }
  handle_type get() const { return handle_; }

private:
  // Each platform's invalid sentinel is whatever open_volume returns on
  // failure: -1 for POSIX fd, INVALID_HANDLE_VALUE for Windows HANDLE.
  // Platform::invalid_handle() must return that value.
  void invalidate() { handle_ = Platform::invalid_handle(); }

  handle_type handle_;
};

#endif
