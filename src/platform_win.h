#ifndef CACHEX_WIN_H
#define CACHEX_WIN_H

// Target Windows Vista (0x0600) or later to expose GetTickCount64
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include "result.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// winioctl.h MUST be included before ntddscsi.h in MinGW to define 
// CTL_CODE, FILE_DEVICE_CONTROLLER, METHOD_BUFFERED, etc.
#include <winioctl.h>

// ntddscsi.h provides the canonical SCSI_PASS_THROUGH_DIRECT struct,
// IOCTL_SCSI_PASS_THROUGH_DIRECT, and the SCSI_IOCTL_DATA_* constants.
//
// Critically, using the system-provided definition (rather than a hand-rolled
// one) ensures the WOW64 thunk layer activates correctly when a 32-bit build
// runs on a 64-bit OS. The kernel's thunking for IOCTL_SCSI_PASS_THROUGH_DIRECT
// is keyed to the canonical struct layout: a custom redefinition bypasses it
// entirely, causing the kernel to misread the DataBuffer pointer offset and
// reject or misinterpret the IOCTL.
//
// It also ensures sptd.Length = sizeof(sptd) produces the value the kernel
// driver expects (56 bytes on 64-bit, 44 bytes on 32-bit), rather than always
// producing the wrong size when the build architecture doesn't match the OS.
#include <ntddscsi.h>

namespace windows_detail
{

void sptd_exec(HANDLE handle, SCSI_PASS_THROUGH_DIRECT &sptd, CommandResult &rv)
{
  DWORD dwBytesReturned;

  // Use steady_clock so both timestamps are taken from the same monotonic
  // source regardless of which core the thread happens to be running on.
  // This replaces the old QPC + SetThreadAffinityMask approach, which could
  // still produce skewed timestamps because the thread was unlocked during
  // DeviceIoControl and could migrate to a different core between the start
  // and end measurements.
  auto t_start = std::chrono::steady_clock::now();
  auto io_ok = DeviceIoControl(handle, IOCTL_SCSI_PASS_THROUGH_DIRECT, &sptd,
                               sizeof(sptd), &sptd, sizeof(sptd),
                               &dwBytesReturned, NULL);
  auto t_end = std::chrono::steady_clock::now();

  if (io_ok && dwBytesReturned == sizeof(sptd))
  {
    rv.Valid = true;
    rv.Duration = std::chrono::duration<double, std::milli>(t_end - t_start).count();
    // DataTransferLength is updated by the kernel to the actual transfer count.
    // Clamp to capacity so a misbehaving driver can't cause a reallocation
    // that would invalidate the pre-allocated DMA buffer pointer.
    const auto actual = std::min(
        static_cast<std::size_t>(sptd.DataTransferLength), rv.Data.capacity());
    rv.Data.resize(actual);
    rv.ScsiStatusCode = sptd.ScsiStatus;
  }
  else
  {
    rv.Valid = false;
  }
}

template <std::size_t CDBLength>
SCSI_PASS_THROUGH_DIRECT
sptd_common(const std::array<std::uint8_t, CDBLength> &cdb)
{
  auto sptd = SCSI_PASS_THROUGH_DIRECT{};
  sptd.Length = sizeof(sptd);
  sptd.CdbLength = CDBLength; // CDB size
  sptd.TimeOutValue = 60;     // SCSI timeout value
  std::copy(std::begin(cdb), std::end(cdb), sptd.Cdb);
  return sptd;
}

template <std::size_t CDBLength>
SCSI_PASS_THROUGH_DIRECT
sptd_for_read(CommandResult &rv, const std::array<std::uint8_t, CDBLength> &cdb)
{
  auto sptd = sptd_common(cdb);
  sptd.DataIn = SCSI_IOCTL_DATA_IN;
  sptd.DataTransferLength = static_cast<ULONG>(rv.Data.size());
  sptd.DataBuffer = rv.Data.data();
  return sptd;
}

template <std::size_t CDBLength>
SCSI_PASS_THROUGH_DIRECT
sptd_for_write(const std::vector<std::uint8_t> &data,
               const std::array<std::uint8_t, CDBLength> &cdb)
{
  auto sptd = sptd_common(cdb);
  sptd.DataIn = SCSI_IOCTL_DATA_OUT;
  sptd.DataTransferLength = static_cast<ULONG>(data.size());
  sptd.DataBuffer = const_cast<std::uint8_t *>(data.data());
  return sptd;
}
} // namespace windows_detail

struct platform_windows
{
  using device_handle = HANDLE;

  static std::uint32_t monotonic_clock() { return static_cast<std::uint32_t>(GetTickCount64()); }

  static device_handle open_volume(const char *DrivePath)
  {
    HANDLE hVolume;
    UINT uDriveType;
    char szVolumeName[8];
    char szRootName[5];
    DWORD dwAccessFlags;
    char DriveLetter = DrivePath[0];

    szRootName[0] = DriveLetter;
    szRootName[1] = ':';
    szRootName[2] = '\\';
    szRootName[3] = '\0';

    uDriveType = GetDriveType(szRootName);

    switch (uDriveType)
    {
    case DRIVE_CDROM:
      dwAccessFlags = GENERIC_READ | GENERIC_WRITE;
      break;

    default:
      std::cerr << "\nError: invalid drive type\n";
      return INVALID_HANDLE_VALUE;
    }

    szVolumeName[0] = '\\';
    szVolumeName[1] = '\\';
    szVolumeName[2] = '.';
    szVolumeName[3] = '\\';
    szVolumeName[4] = DriveLetter;
    szVolumeName[5] = ':';
    szVolumeName[6] = '\0';

    hVolume = CreateFile(szVolumeName, dwAccessFlags,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

    // Some drives or OS configurations reject GENERIC_WRITE on optical media.
    // Fall back to read-only access; commands that require write access
    // (SET CD SPEED, MODE SELECT) will fail gracefully at the IOCTL level.
    if (hVolume == INVALID_HANDLE_VALUE && dwAccessFlags == (GENERIC_READ | GENERIC_WRITE))
    {
      hVolume = CreateFile(szVolumeName, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
      if (hVolume != INVALID_HANDLE_VALUE)
        std::cerr << "\nWarning: opened read-only (write access denied); "
                     "SET CD SPEED and MODE SELECT may fail\n";
    }

    if (hVolume == INVALID_HANDLE_VALUE)
      std::cerr << "\nError: invalid handle";

    return hVolume;
  }

  static void close_handle(device_handle h) { CloseHandle(h); }
  static bool handle_is_valid(device_handle h) { return h != INVALID_HANDLE_VALUE; }
  static device_handle invalid_handle() { return INVALID_HANDLE_VALUE; }

  static void set_critical_priority()
  {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
  }

  static void set_normal_priority()
  {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
  }

  template <std::size_t CDBLength>
  static void exec_command(device_handle handle, CommandResult &rv,
                           const std::array<std::uint8_t, CDBLength> &cdb)
  {
    using namespace windows_detail;
    auto sptd = sptd_for_read(rv, cdb);
    sptd_exec(handle, sptd, rv);
  }

  template <std::size_t CDBLength>
  static void send_data(device_handle handle, CommandResult &rv,
                        const std::array<std::uint8_t, CDBLength> &cdb,
                        const std::vector<std::uint8_t> &data)
  {
    using namespace windows_detail;
    auto sptd = sptd_for_write(data, cdb);
    windows_detail::sptd_exec(handle, sptd, rv);
  }
};

using platform = platform_windows;

#endif
