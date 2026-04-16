/*******************************************************************************
 * CacheExplorer 0.15-LLM                  2026/03, based on :
 * CacheExplorer 0.14  dkk089@gmail.com    2019/03, based on :
 * CacheExplorer 0.9   spath@cdfreaks.com  2006/xx
 ******************************************************************************/

#ifdef _WIN32
#include "platform_win.h"
#elif defined(__linux__)
#include "platform_linux.h"
#elif defined(__NetBSD__)
#include "platform_netbsd.h"
#else
#include "result.h"
#include <array>
#include <vector>
#error "This platform is not supported. Please implement the functions below."
struct platform
{
  using device_handle = int;
  static device_handle open_volume(const char *) { return 0; }
  static bool handle_is_valid(device_handle) { return false; }
  static void close_handle(device_handle) {}
  static device_handle invalid_handle() { return 0; }
  static std::uint32_t monotonic_clock()
  {
    static std::uint32_t val = 0;
    return val++;
  }
  static void set_critical_priority() {}
  static void set_normal_priority() {}

  template <std::size_t CDBLength>
  static void exec_command(device_handle, CommandResult &,
                           const std::array<std::uint8_t, CDBLength> &)
  {
  }

  template <std::size_t CDBLength>
  static void send_data(device_handle, CommandResult &,
                        const std::array<std::uint8_t, CDBLength> &,
                        const std::vector<std::uint8_t> &)
  {
  }
};
#endif

#include <cstdint>
#include <cstring>

#include <algorithm>
#include <array>
#include <functional>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

constexpr int MAX_CACHE_LINES = 10;
constexpr int NB_IGNORE_MEASURES = 2;

// offset of first block descriptor = size of mode parameter header
constexpr int DESCRIPTOR_BLOCK_1 = 8;

constexpr std::uint8_t CACHING_MODE_PAGE = 0x08;
constexpr std::uint8_t CD_DVD_CAPABILITIES_PAGE = 0x2A;

constexpr std::uint8_t SCSI_OPCODE_REQUEST_SENSE = 0x03;
constexpr std::uint8_t SCSI_OPCODE_INQUIRY = 0x12;
constexpr std::uint8_t SCSI_OPCODE_READ_10 = 0x28;
constexpr std::uint8_t SCSI_OPCODE_PREFETCH_10 = 0x34;
constexpr std::uint8_t SCSI_OPCODE_SYNCHRONIZE_CACHE_10 = 0x35;
constexpr std::uint8_t SCSI_OPCODE_MODE_SELECT_10 = 0x55;
constexpr std::uint8_t SCSI_OPCODE_MODE_SENSE_10 = 0x5A;
constexpr std::uint8_t SCSI_OPCODE_READ_12 = 0xA8;
constexpr std::uint8_t SCSI_OPCODE_SET_CD_SPEED = 0xBB;
constexpr std::uint8_t SCSI_OPCODE_READ_CD = 0xBE;
constexpr std::uint8_t SCSI_OPCODE_READ_D4 = 0xD4;
constexpr std::uint8_t SCSI_OPCODE_READ_D5 = 0xD5;
constexpr std::uint8_t SCSI_OPCODE_READ_D8 = 0xD8;

constexpr std::uint8_t RCD_BIT = 1;
constexpr std::uint8_t RCD_READ_CACHE_ENABLED = 0;
constexpr std::uint8_t RCD_READ_CACHE_DISABLED = 1;

namespace
{

// ---------------------------------------------------------------------------
// DriveContext
//
// Bundles all mutable per-drive state that was previously spread across
// global variables. Pass by reference through every function that needs
// device access or configuration, so the tool can in principle operate on
// multiple drives without any shared mutable state.
// ---------------------------------------------------------------------------
struct DriveContext
{
  // Raw handle — lifetime managed by a DeviceHandle<platform> in main().
  // Never close this directly; let the RAII wrapper do it.
  platform::device_handle handle = platform::invalid_handle();
  int NbBurstReadSectors = 1;
  double ThresholdRatioMethod2 = 0.9;
  int CachedNonCachedSpeedFactor = 4;
  int MaxCacheSectors = 1000;
  bool PlextorFlushValidated =
      false; // set when -p confirms Plextor FUA flush works
  bool SyncCacheValidated =
      false; // set when -q confirms SYNCHRONIZE CACHE works
};

// g_ctx holds all mutable per-drive state. The accessors below return
// references into it so all call sites work without modification.
// To support multiple drives, replace g_ctx with a local DriveContext in
// main() and thread it through via function parameters instead.
DriveContext g_ctx;

// Convenience accessors — inlined by any optimising compiler.
inline platform::device_handle &hVolume() { return g_ctx.handle; }
inline int &NbBurstReadSectors() { return g_ctx.NbBurstReadSectors; }
inline double &ThresholdRatioMethod2() { return g_ctx.ThresholdRatioMethod2; }
inline int &CachedNonCachedSpeedFactor()
{
  return g_ctx.CachedNonCachedSpeedFactor;
}
inline int &MaxCacheSectors() { return g_ctx.MaxCacheSectors; }

struct debugstream
{
  debugstream() {}
  debugstream(const debugstream &) = delete;
  template <typename T> debugstream &operator<<(T val)
  {
    if (Enabled)
    {
      std::cerr << val;
    }
    return *this;
  }
  bool Enabled = false;
  operator bool() const { return Enabled; }
};

debugstream DEBUG;
debugstream SUPERDEBUG;

template <std::size_t N> using bytearray = std::array<std::uint8_t, N>;

namespace Command
{
bytearray<12> Read_A8h(long int TargetSector, int NbSectors, bool FUAbit)
{
  bytearray<12> rv = {SCSI_OPCODE_READ_12,
                      static_cast<std::uint8_t>(FUAbit << 3),
                      static_cast<std::uint8_t>(TargetSector >> 24),
                      static_cast<std::uint8_t>(TargetSector >> 16),
                      static_cast<std::uint8_t>(TargetSector >> 8),
                      static_cast<std::uint8_t>(TargetSector),
                      static_cast<std::uint8_t>(NbSectors >> 24),
                      static_cast<std::uint8_t>(NbSectors >> 16),
                      static_cast<std::uint8_t>(NbSectors >> 8),
                      static_cast<std::uint8_t>(NbSectors),
                      0,
                      0};
  return rv;
}

bytearray<10> Read_28h(long int TargetSector, int NbSectors, bool FUAbit)
{
  bytearray<10> rv = {SCSI_OPCODE_READ_10,
                      static_cast<std::uint8_t>(FUAbit << 3),
                      static_cast<std::uint8_t>(TargetSector >> 24),
                      static_cast<std::uint8_t>(TargetSector >> 16),
                      static_cast<std::uint8_t>(TargetSector >> 8),
                      static_cast<std::uint8_t>(TargetSector),
                      0,
                      static_cast<std::uint8_t>(NbSectors >> 8),
                      static_cast<std::uint8_t>(NbSectors),
                      0};
  return rv;
}

bytearray<12> Read_28h_12(long int TargetSector, int NbSectors, bool FUAbit)
{
  bytearray<12> rv = {SCSI_OPCODE_READ_10,
                      static_cast<std::uint8_t>(FUAbit << 3),
                      static_cast<std::uint8_t>(TargetSector >> 24),
                      static_cast<std::uint8_t>(TargetSector >> 16),
                      static_cast<std::uint8_t>(TargetSector >> 8),
                      static_cast<std::uint8_t>(TargetSector),
                      static_cast<std::uint8_t>(NbSectors >> 24),
                      static_cast<std::uint8_t>(NbSectors >> 16),
                      static_cast<std::uint8_t>(NbSectors >> 8),
                      static_cast<std::uint8_t>(NbSectors),
                      0,
                      0};
  return rv;
}

bytearray<12> Read_BEh(long int TargetSector, int NbSectors)
{
  bytearray<12> rv = {SCSI_OPCODE_READ_CD,
                      0x00, // 0x04 = audio data only, 0x00 = any type
                      static_cast<std::uint8_t>(TargetSector >> 24),
                      static_cast<std::uint8_t>(TargetSector >> 16),
                      static_cast<std::uint8_t>(TargetSector >> 8),
                      static_cast<std::uint8_t>(TargetSector),
                      static_cast<std::uint8_t>(NbSectors >> 16),
                      static_cast<std::uint8_t>(NbSectors >> 8),
                      static_cast<std::uint8_t>(NbSectors),
                      0x10, // just data
                      0,    // no subcode
                      0};
  return rv;
}

bytearray<10> Read_D4h(long int TargetSector, int NbSectors, bool FUAbit)
{
  bytearray<10> rv = {SCSI_OPCODE_READ_D4,
                      static_cast<std::uint8_t>(FUAbit << 3),
                      static_cast<std::uint8_t>(TargetSector >> 24),
                      static_cast<std::uint8_t>(TargetSector >> 16),
                      static_cast<std::uint8_t>(TargetSector >> 8),
                      static_cast<std::uint8_t>(TargetSector),
                      static_cast<std::uint8_t>(NbSectors >> 16),
                      static_cast<std::uint8_t>(NbSectors >> 8),
                      static_cast<std::uint8_t>(NbSectors),
                      0};
  return rv;
}

bytearray<10> Read_D5h(long int TargetSector, int NbSectors, bool FUAbit)
{
  bytearray<10> rv = {SCSI_OPCODE_READ_D5,
                      static_cast<std::uint8_t>(FUAbit << 3),
                      static_cast<std::uint8_t>(TargetSector >> 24),
                      static_cast<std::uint8_t>(TargetSector >> 16),
                      static_cast<std::uint8_t>(TargetSector >> 8),
                      static_cast<std::uint8_t>(TargetSector),
                      static_cast<std::uint8_t>(NbSectors >> 16),
                      static_cast<std::uint8_t>(NbSectors >> 8),
                      static_cast<std::uint8_t>(NbSectors),
                      0};
  return rv;
}

bytearray<12> Read_D8h(long int TargetSector, int NbSectors, bool FUAbit)
{
  bytearray<12> rv = {SCSI_OPCODE_READ_D8,
                      static_cast<std::uint8_t>(FUAbit << 3),
                      static_cast<std::uint8_t>(TargetSector >> 24),
                      static_cast<std::uint8_t>(TargetSector >> 16),
                      static_cast<std::uint8_t>(TargetSector >> 8),
                      static_cast<std::uint8_t>(TargetSector),
                      static_cast<std::uint8_t>(NbSectors >> 24),
                      static_cast<std::uint8_t>(NbSectors >> 16),
                      static_cast<std::uint8_t>(NbSectors >> 8),
                      static_cast<std::uint8_t>(NbSectors),
                      0,
                      0};
  return rv;
}

bytearray<12> PlextorFUAFlush(long int TargetSector)
{
  // size stays zero, that's how this command works
  // MMC spec specifies that "A Transfer Length of zero indicates that no
  // logical blocks shall be transferred. This condition shall not be
  // considered an error"
  return Read_28h_12(TargetSector, 0, 1);
}

// SYNCHRONIZE CACHE (10) — SBC-3 mandatory, opcode 0x35.
// Instructs the drive to flush its write cache and, when IMMED=0 (byte 1
// bit 1 = 0), to complete synchronisation before returning status.
// For cache-invalidation testing we issue it before a re-read to verify
// the drive actually re-fetches from disc rather than serving stale data.
bytearray<10> SynchronizeCache()
{
  bytearray<10> rv = {
      SCSI_OPCODE_SYNCHRONIZE_CACHE_10, // SYNCHRONIZE CACHE (10)
      0,                                // IMMED=0: wait until complete
      0,
      0,
      0,
      0, // LBA = 0 (sync entire cache)
      0,
      0,
      0, // number of blocks = 0 (sync entire cache)
      0};
  return rv;
}

bytearray<6> RequestSense(std::uint8_t AllocationLength)
{
  bytearray<6> rv = {SCSI_OPCODE_REQUEST_SENSE, // REQUEST SENSE
                     0,
                     0,
                     0,
                     AllocationLength, // allocation size
                     0};
  return rv;
}

bytearray<10> ModeSense(unsigned char PageCode, unsigned char SubPageCode,
                        int size)
{
  bytearray<10> rv = {SCSI_OPCODE_MODE_SENSE_10, // MODE SENSE(10)
                      0,
                      PageCode,
                      SubPageCode,
                      0,
                      0,
                      0,
                      uint8_t((size >> 8) & 0xFF), // size
                      uint8_t((size) & 0xFF),
                      0};
  return rv;
}

bytearray<10> ModeSelect(std::uint16_t size)
{
  bytearray<10> rv = {SCSI_OPCODE_MODE_SELECT_10, 0x10,          0, 0, 0, 0, 0,
                      uint8_t(size >> 8),         uint8_t(size), 0};
  return rv;
}

bytearray<10> Prefetch(long int TargetSector, unsigned int NbSectors)
{
  bytearray<10> rv = {SCSI_OPCODE_PREFETCH_10, // PREFETCH
                      0,
                      uint8_t((TargetSector >> 24) & 0xFF), // target sector
                      uint8_t((TargetSector >> 16) & 0xFF),
                      uint8_t((TargetSector >> 8) & 0xFF),
                      uint8_t((TargetSector) & 0xFF),
                      0,
                      uint8_t((NbSectors >> 8) & 0xFF), // size
                      uint8_t((NbSectors) & 0xFF),
                      0};
  return rv;
}

bytearray<6> Inquiry(std::uint8_t AllocationLength)
{
  bytearray<6> rv = {SCSI_OPCODE_INQUIRY, 0, 0, 0, AllocationLength, 0};
  return rv;
}

bytearray<12> SetCDSpeed(unsigned char ReadSpeedX, unsigned char WriteSpeedX)
{
  unsigned int ReadSpeedkB = 0xFFFF;
  if (ReadSpeedX != 0)
  {
    // don't ask me what this "+ 2" is doing here, MMC-4 doesn't mention
    // anything of this sort.
    ReadSpeedkB = (ReadSpeedX * 176) + 2; // 1x CD = 176kB/s
  }
  unsigned int WriteSpeedkB = (WriteSpeedX * 176);
  bytearray<12> rv = {SCSI_OPCODE_SET_CD_SPEED, // SET CD SPEED
                      0,
                      static_cast<std::uint8_t>(ReadSpeedkB >> 8),
                      static_cast<std::uint8_t>(ReadSpeedkB),
                      static_cast<std::uint8_t>(WriteSpeedkB >> 8),
                      static_cast<std::uint8_t>(WriteSpeedkB),
                      0,
                      0,
                      0,
                      0,
                      0,
                      0};
  return rv;
}
} // namespace Command

template <std::size_t CDBLength>
void ExecCommand(CommandResult &rv,
                 const std::array<std::uint8_t, CDBLength> &cdb)
{
  platform::exec_command(hVolume(), rv, cdb);
}

template <std::size_t CDBLength>
void ExecCommand(CommandResult &rv,
                 const std::array<std::uint8_t, CDBLength> &cdb,
                 const std::vector<std::uint8_t> &data)
{
  platform::send_data(hVolume(), rv, cdb, data);
}

template <std::size_t CDBLength>
CommandResult ExecSectorCommand(unsigned int NbSectors,
                                const std::array<std::uint8_t, CDBLength> &cdb)
{
  CommandResult rv(2448 * NbSectors);
  ExecCommand(rv, cdb);
  return rv;
}

template <std::size_t CDBLength>
CommandResult ExecBytesCommand(unsigned int NbBytes,
                               const std::array<std::uint8_t, CDBLength> &cdb)
{
  CommandResult rv(NbBytes);
  ExecCommand(rv, cdb);
  return rv;
}

template <std::size_t CDBLength>
CommandResult ExecBytesCommand(unsigned int NbBytes,
                               const std::array<std::uint8_t, CDBLength> &cdb,
                               const std::vector<std::uint8_t> &data)
{
  CommandResult rv(NbBytes);
  ExecCommand(rv, cdb, data);
  return rv;
}

CommandResult Read_A8h(long int TargetSector, int NbSectors, bool FUAbit)
{
  return ExecSectorCommand(NbSectors,
                           Command::Read_A8h(TargetSector, NbSectors, FUAbit));
}

CommandResult Read_28h(long int TargetSector, int NbSectors, bool FUAbit)
{
  return ExecSectorCommand(NbSectors,
                           Command::Read_28h(TargetSector, NbSectors, FUAbit));
}

CommandResult Read_28h_12(long int TargetSector, int NbSectors, bool FUAbit)
{
  return ExecSectorCommand(
      NbSectors, Command::Read_28h_12(TargetSector, NbSectors, FUAbit));
}

CommandResult Read_BEh(long int TargetSector, int NbSectors, bool)
{
  return ExecSectorCommand(NbSectors,
                           Command::Read_BEh(TargetSector, NbSectors));
}

CommandResult Read_D4h(long int TargetSector, int NbSectors, bool FUAbit)
{
  return ExecSectorCommand(NbSectors,
                           Command::Read_D4h(TargetSector, NbSectors, FUAbit));
}

CommandResult Read_D5h(long int TargetSector, int NbSectors, bool FUAbit)
{
  return ExecSectorCommand(NbSectors,
                           Command::Read_D5h(TargetSector, NbSectors, FUAbit));
}

CommandResult Read_D8h(long int TargetSector, int NbSectors, bool FUAbit)
{
  return ExecSectorCommand(NbSectors,
                           Command::Read_D8h(TargetSector, NbSectors, FUAbit));
}

struct sReadCommand
{
  sReadCommand(const char *name, CommandResult (*func)(long int, int, bool),
               bool fua)
      : Name(name), pFunc(func), Supported(false), FUAbitSupported(fua),
        DeclaredFUA(fua), FUAValidated(false)
  {
  }

  sReadCommand &operator=(const sReadCommand &) = delete;
  sReadCommand(const sReadCommand &) = delete;

  const char *const Name;
  CommandResult (*pFunc)(long int, int, bool);
  bool Supported;
  bool FUAbitSupported; // runtime value — may be cleared by -i probing
  bool DeclaredFUA;     // immutable table value — restored by -r override
  bool FUAValidated; // true only if -i probe confirmed FUA works on this drive
  bool operator==(const char *name) const { return strcmp(Name, name) == 0; }
};

std::array<sReadCommand, 7> Commands = {{{"BEh", &Read_BEh, false},
                                         {"A8h", &Read_A8h, true},
                                         {"28h", &Read_28h, true},
                                         {"28h_12", &Read_28h_12, true},
                                         {"D4h", &Read_D4h, true},
                                         {"D5h", &Read_D5h, true},
                                         {"D8h", &Read_D8h, true}}};

sReadCommand *TryGetSupportedCommand()
{
  auto it = std::find_if(std::begin(Commands), std::end(Commands),
                         [](auto &&cmd) { return cmd.Supported; });
  return it == std::end(Commands) ? nullptr : &(*it);
}

sReadCommand &GetSupportedCommand()
{
  auto *cmd = TryGetSupportedCommand();
  // Callers must ensure at least one command is supported before calling this.
  // Use -i or -r to initialise supported commands first.
  if (!cmd)
  {
    std::cerr << "\nError: no supported read command available. Run with -i "
                 "first or specify a command with -r.\n";
    std::exit(-1);
  }
  return *cmd;
}

sReadCommand *GetFUASupportedCommand()
{
  auto it =
      std::find_if(std::begin(Commands), std::end(Commands), [](auto &&cmd)
                   { return cmd.Supported && cmd.FUAbitSupported; });
  return it == std::end(Commands) ? nullptr : &(*it);
}

CommandResult PlextorFUAFlush(long int TargetSector)
{
  return ExecSectorCommand(0, Command::PlextorFUAFlush(TargetSector));
}

// SYNCHRONIZE CACHE (10): no data transfer, 0-byte result is correct.
CommandResult SynchronizeCache()
{
  return ExecBytesCommand(0, Command::SynchronizeCache());
}

CommandResult RequestSense()
{
  const std::uint8_t AllocationLength = 18;
  return ExecBytesCommand(AllocationLength,
                          Command::RequestSense(AllocationLength));
}

// LogSenseData
//
// Issues REQUEST SENSE and, if the response is valid, decodes and logs the
// sense key, ASC (additional sense code), and ASCQ (additional sense code
// qualifier) — the three values that together explain why a SCSI command
// returned CHECK CONDITION.
//
// Fixed-format sense data layout (SPC-4 §4.5.3):
//   Byte 0: response code (0x70 = current, 0x71 = deferred)
//   Byte 2: sense key (bits [3:0])
//   Byte 7: additional sense length
//   Byte 12: ASC
//   Byte 13: ASCQ
//
// Sense key policy:
//   0x0  NO SENSE        — silent (nothing wrong)
//   0x1  RECOVERED ERROR — silent (drive self-recovered)
//   0x5  ILLEGAL REQUEST — DEBUG only; expected during capability probing
//   all others           — always printed (NOT READY, MEDIUM ERROR, etc.)
//
// ILLEGAL REQUEST is suppressed outside debug mode because the tool
// intentionally probes many optional commands — most drives will reject
// several with ILLEGAL REQUEST and that is normal, not an alarm condition.
void LogSenseData()
{
  auto sense = RequestSense();
  if (!sense.Valid || sense.Data.size() < 14)
    return;

  const std::uint8_t responseCode = sense.Data[0] & 0x7F;
  // Only fixed-format responses (0x70, 0x71) are handled here
  if (responseCode != 0x70 && responseCode != 0x71)
    return;

  const std::uint8_t senseKey = sense.Data[2] & 0x0F;
  const std::uint8_t asc = sense.Data[12];
  const std::uint8_t ascq = sense.Data[13];

  // Always print full sense info in debug mode
  DEBUG << "\n  sense: key=0x" << std::hex << static_cast<int>(senseKey)
        << " ASC=0x" << static_cast<int>(asc) << " ASCQ=0x"
        << static_cast<int>(ascq) << std::dec;

  // Print unconditionally only for genuine fault conditions.
  // Skip NO SENSE (0x0), RECOVERED ERROR (0x1), and ILLEGAL REQUEST (0x5).
  const bool isFault = (senseKey != 0x0 && senseKey != 0x1 && senseKey != 0x5);
  if (isFault)
  {
    std::cerr << " [sense key=0x" << std::hex << static_cast<int>(senseKey)
              << " ASC=0x" << static_cast<int>(asc) << " ASCQ=0x"
              << static_cast<int>(ascq) << std::dec << "]";
  }
}

CommandResult ModeSense(unsigned char PageCode, unsigned char SubPageCode,
                        int size)
{
  return ExecBytesCommand(size,
                          Command::ModeSense(PageCode, SubPageCode, size));
}

CommandResult ModeSelect(const std::vector<std::uint8_t> &data)
{
  return ExecBytesCommand(
      data.size(), Command::ModeSelect(static_cast<std::uint16_t>(data.size())),
      data);
}

CommandResult Prefetch(long int TargetSector, unsigned int NbSectors)
{
  return ExecBytesCommand(18, Command::Prefetch(TargetSector, NbSectors));
}

void PrintIDString(unsigned char *dataChars, int dataLength)
{
  if (dataChars)
  {
    std::cerr << ' ';
    while (0 < dataLength--)
    {
      auto cc = *dataChars++;
      cc &= 0x7F;
      if (!((0x20 <= cc) && (cc <= 0x7E)))
      {
        cc ^= 0x40;
      }
      std::cerr << static_cast<char>(cc);
    }
  }
}

bool PrintDriveInfo()
{
  const std::uint8_t AllocationLength = 36;
  auto result =
      ExecBytesCommand(AllocationLength, Command::Inquiry(AllocationLength));

  if (!result || result.Data.size() < 36)
  {
    return false;
  }

  // print info
  PrintIDString(&result.Data[8], 8);       // vendor Id
  PrintIDString(&result.Data[0x10], 0x10); // product Id
  PrintIDString(&result.Data[0x20], 4);    // product RevisionLevel
  return true;
}

// bool ClearCache()
//
// fills the cache by reading backwards several areas at the beginning of the
// disc
//
bool ClearCache()
{
  auto &&cmd = GetSupportedCommand();
  for (int i = 0; i < MAX_CACHE_LINES; i++)
  {
    // old code added the original return value from these functions
    // but then assigned true in the next line anyway, so...
    cmd.pFunc((MAX_CACHE_LINES - i + 1) * 1000, 1, false);
  }
  return true;
}

bool SpinDrive(unsigned int Seconds)
{
  auto &&cmd = GetSupportedCommand();
  DEBUG << "\ninfo: spinning the drive... ";
  auto TimeStart = platform::monotonic_clock();
  int i = 0;
  while (platform::monotonic_clock() - TimeStart <= (Seconds * 1000))
  {
    cmd.pFunc((10000 + (i++)) % 50000, 1, false);
  }
  return true;
}

CommandResult SetDriveSpeed(unsigned char ReadSpeedX, unsigned char WriteSpeedX)
{
  return ExecBytesCommand(18, Command::SetCDSpeed(ReadSpeedX, WriteSpeedX));
}

void ShowCacheValues()
{
  auto result = ModeSense(CD_DVD_CAPABILITIES_PAGE, 0, 32);
  if (result && result.Data.size() >= DESCRIPTOR_BLOCK_1 + 14)
  {
    std::cerr << "\n[+] Buffer size: "
              << ((result.Data[DESCRIPTOR_BLOCK_1 + 12] << 8) |
                  result.Data[DESCRIPTOR_BLOCK_1 + 13])
              << " kB";
  }
  else
  {
    SUPERDEBUG << "\ninfo: cannot read CD/DVD Capabilities page";
    LogSenseData();
  }
  result = ModeSense(CACHING_MODE_PAGE, 0, 20);
  if (result && result.Data.size() >= DESCRIPTOR_BLOCK_1 + 3)
  {
    std::cerr << ", read cache is "
              << ((result.Data[DESCRIPTOR_BLOCK_1 + 2] & RCD_BIT) ? "disabled"
                                                                  : "enabled");
  }
  else
  {
    SUPERDEBUG << "\ninfo: cannot read Caching Mode page";
    LogSenseData();
  }
}

bool SetCacheRCDBit(bool RCDBitValue)
{
  bool retval = false;

  auto result = ModeSense(CACHING_MODE_PAGE, 0, 20);
  if (result && result.Data.size() >= DESCRIPTOR_BLOCK_1 + 3)
  {
    result.Data[DESCRIPTOR_BLOCK_1 + 2] =
        (result.Data[DESCRIPTOR_BLOCK_1 + 2] & 0xFE) |
        static_cast<std::uint8_t>(RCDBitValue);
    result = ModeSelect(result.Data);
    if (result)
    {
      result = ModeSense(CACHING_MODE_PAGE, 0, 20);
      if (result && result.Data.size() >= DESCRIPTOR_BLOCK_1 + 3 &&
          (result.Data[DESCRIPTOR_BLOCK_1 + 2] & RCD_BIT) == RCDBitValue)
      {
        retval = true;
      }
    }

    if (!retval)
    {
      DEBUG << "\ninfo: cannot write Caching Mode page";
      LogSenseData();
    }
  }
  else
  {
    DEBUG << "\ninfo: cannot read Caching Mode page";
    LogSenseData();
  }
  return (retval);
}

bool TestSupportedFlushCommands()
{
  std::cerr << "\n[+] Supported cache flush commands:";
  bool rv = false;
  for (auto &&cmd : Commands)
  {
    if (cmd.FUAbitSupported)
    {
      if (cmd.pFunc(9900, 0, true))
      {
        rv = true;
        std::cerr << ' ' << cmd.Name;
      }
      else
      {
        SUPERDEBUG << "\ncommand " << cmd.Name << " rejected";
        LogSenseData();
      }
    }
  }
  return rv;
}

//------------------------------------------------------------------------------
// void TestSupportedReadCommands(char DriveLetter)
//
// test and display which read commands are supported by the current drive
// and if any of these commands supports the FUA bit
//------------------------------------------------------------------------------
bool TestSupportedReadCommands()
{
  std::cerr << "\n[+] Supported read commands:";
  bool rv = false;
  for (auto &&cmd : Commands)
  {
    if (cmd.pFunc(10000, 1, false))
    {
      rv = true;
      std::cerr << ' ' << cmd.Name;
      cmd.Supported = true;
      if (cmd.FUAbitSupported)
      {
        if (cmd.pFunc(9900, 1, true))
        {
          std::cerr << "(FUA)";
          cmd.FUAValidated = true;
        }
        else
        {
          SUPERDEBUG << "\ncommand " << cmd.Name << " with FUA bit rejected";
          cmd.FUAbitSupported = false;
          LogSenseData();
        }
      }
    }
    else
    {
      SUPERDEBUG << "\ncommand " << cmd.Name << " rejected";
      LogSenseData();
    }
  }
  return rv;
}

//
// TestPlextorFUACommand
//
// test if Plextor's flushing command is supported
bool TestPlextorFUACommand()
{
  std::cerr << "\n[+] Plextor flush command: ";
  auto result = PlextorFUAFlush(100000);
  std::cerr << (result ? "accepted" : "rejected");
  DEBUG << " (status = " << static_cast<int>(result.ScsiStatusCode) << ")";
  return result;
}

//
// TestPlextorFUACommandWorks
//
// test if Plextor's flushing command actually works
int TestPlextorFUACommandWorks(sReadCommand &ReadCommand, long int TargetSector,
                               int NbTests)
{
  int InvalidationSuccess = 0;
  double InitDelay = 0.0;
  double InitDelay2 = 0.0;
  double Delay = 0.0;
  double Delay2 = 0.0;

  DEBUG << "\ninfo: " << NbTests
        << " test(s), c/nc ratio: " << CachedNonCachedSpeedFactor()
        << ", burst: " << NbBurstReadSectors()
        << ", max: " << MaxCacheSectors();

  for (int i = 0; i < NbTests; i++)
  {
    // first test : normal cache test
    ClearCache();
    auto result = ReadCommand.pFunc(TargetSector, NbBurstReadSectors(),
                                    false); // init read
    InitDelay = result.Duration;
    result = ReadCommand.pFunc(TargetSector + NbBurstReadSectors(),
                               NbBurstReadSectors(), false);
    Delay = result.Duration;

    // second test : add a Plextor FUA flush command in between
    ClearCache();
    result = ReadCommand.pFunc(TargetSector, NbBurstReadSectors(),
                               false); // init read
    InitDelay2 = result.Duration;
    PlextorFUAFlush(TargetSector);
    result = ReadCommand.pFunc(TargetSector + NbBurstReadSectors(),
                               NbBurstReadSectors(), false);
    Delay2 = result.Duration;
    DEBUG << "\n " << std::setprecision(2) << InitDelay << " ms / "
          << std::setprecision(2) << Delay << " ms -> " << std::setprecision(2)
          << InitDelay2 << " ms / " << std::setprecision(2) << Delay2 << " ms";

    // compare times
    if (Delay2 > (CachedNonCachedSpeedFactor() * Delay))
    {
      InvalidationSuccess++;
    }
  }
  DEBUG << "\nresult: ";
  return (InvalidationSuccess);
}

// wrapper for TestPlextorFUACommandWorks
int TestPlextorFUACommandWorksWrapper(long int TargetSector, int NbTests)
{
  auto &&cmd = GetSupportedCommand();
  DEBUG << "\ninfo: using command " << cmd.Name;
  return TestPlextorFUACommandWorks(cmd, TargetSector, NbTests);
}

//
// TestSyncCacheCommand
//
// Test whether the drive accepts the standard SYNCHRONIZE CACHE (10) command
// (opcode 0x35, mandatory in SBC-3). Unlike the Plextor vendor flush, this
// works on any standards-compliant drive.
bool TestSyncCacheCommand()
{
  std::cerr << "\n[+] SYNCHRONIZE CACHE (0x35): ";
  auto result = SynchronizeCache();
  std::cerr << (result ? "accepted" : "rejected");
  DEBUG << " (status = " << static_cast<int>(result.ScsiStatusCode) << ")";
  return static_cast<bool>(result);
}

//
// TestSyncCacheCommandWorks
//
// Verify that SYNCHRONIZE CACHE actually invalidates the drive's read cache
// by comparing read times before and after issuing it. Uses the same
// timing methodology as TestPlextorFUACommandWorks.
int TestSyncCacheCommandWorks(sReadCommand &ReadCommand, long int TargetSector,
                              int NbTests)
{
  int InvalidationSuccess = 0;
  double InitDelay2 = 0.0;

  DEBUG << "\ninfo: " << NbTests
        << " test(s), c/nc ratio: " << CachedNonCachedSpeedFactor()
        << ", burst: " << NbBurstReadSectors()
        << ", max: " << MaxCacheSectors();

  for (int i = 0; i < NbTests; i++)
  {
    double InitDelay = 0.0;
    double Delay = 0.0;
    double Delay2 = 0.0;

    // First test: normal cache test (no flush)
    ClearCache();
    auto result = ReadCommand.pFunc(TargetSector, NbBurstReadSectors(), false);
    InitDelay = result.Duration;
    result = ReadCommand.pFunc(TargetSector + NbBurstReadSectors(),
                               NbBurstReadSectors(), false);
    Delay = result.Duration;

    // Second test: issue SYNCHRONIZE CACHE between reads
    ClearCache();
    result = ReadCommand.pFunc(TargetSector, NbBurstReadSectors(), false);
    InitDelay2 = result.Duration;
    SynchronizeCache();
    result = ReadCommand.pFunc(TargetSector + NbBurstReadSectors(),
                               NbBurstReadSectors(), false);
    Delay2 = result.Duration;
    DEBUG << "\n " << std::setprecision(2) << InitDelay << " ms / "
          << std::setprecision(2) << Delay << " ms -> " << std::setprecision(2)
          << InitDelay2 << " ms / " << std::setprecision(2) << Delay2 << " ms";

    if (Delay2 > (CachedNonCachedSpeedFactor() * Delay))
    {
      InvalidationSuccess++;
    }
  }
  DEBUG << "\nresult: ";
  return InvalidationSuccess;
}

int TestSyncCacheCommandWorksWrapper(long int TargetSector, int NbTests)
{
  auto &&cmd = GetSupportedCommand();
  DEBUG << "\ninfo: using command " << cmd.Name;
  return TestSyncCacheCommandWorks(cmd, TargetSector, NbTests);
}

//
// TimeMultipleReads
//
double TimeMultipleReads(sReadCommand &cmd, long int TargetSector, int NbReads,
                         bool FUAbit)
{
  double AverageDelay = 0;

  for (int i = 0; i < NbReads; i++)
  {
    auto result = cmd.pFunc(TargetSector, NbBurstReadSectors(), FUAbit);
    double Delay = result.Duration;
    AverageDelay = (((AverageDelay * i) + Delay) / (i + 1));
  }
  return AverageDelay;
}

//
// TestCacheSpeedImpact
//
// Measures the speed difference between reading from cache vs. from disc,
// using the best available flush mechanism to force a cache miss.
//
// Strategy: read a sector (cached), flush, read the same sector again (cold).
// The flush is done using whatever was validated on this drive:
//   1. Plextor FUA flush (28h_12 with FUA=1, NbSectors=0) if available
//   2. SYNCHRONIZE CACHE (0x35) if available
//   3. FUA bit on the read command itself as a last resort
//
// This means -z is useful on any drive with any working flush method,
// not only drives where the FUA bit on a data read forces a cache bypass.
void TestCacheSpeedImpact(long int TargetSector, int NbReads)
{
  auto &readCmd = GetSupportedCommand();
  auto *fuaCmd = GetFUASupportedCommand(); // may be null

  // Determine best available flush function, in order of reliability:
  // 1. Plextor FUA flush — validated by -p tests
  // 2. SYNCHRONIZE CACHE — validated by -q tests
  // 3. FUA bit on a validated read command — last resort
  std::function<void()> flush;
  const char *flushName = nullptr;

  if (g_ctx.PlextorFlushValidated)
  {
    flush = [&]() { PlextorFUAFlush(TargetSector); };
    flushName = "Plextor FUA flush";
  }
  else if (g_ctx.SyncCacheValidated)
  {
    flush = []() { SynchronizeCache(); };
    flushName = "SYNCHRONIZE CACHE";
  }
  else if (fuaCmd && fuaCmd->FUAValidated)
  {
    flush = [&]() { fuaCmd->pFunc(TargetSector, NbBurstReadSectors(), true); };
    flushName = fuaCmd->Name;
  }
  else
  {
    std::cerr << "\n[+] Testing cache speed impact: skipped\n"
              << "    No validated flush mechanism available on this drive.\n"
              << "    Run -i -p or -i -q first to detect flush support.\n";
    return;
  }

  std::cerr << "\n[+] Testing cache speed impact (flush: " << flushName << ")";

  // Warm up: initial read to fill cache
  readCmd.pFunc(TargetSector, NbBurstReadSectors(), false);

  // Measure cached reads
  double cachedMs = TimeMultipleReads(readCmd, TargetSector, NbReads, false);

  // Measure post-flush (cold) reads
  double totalColdMs = 0.0;
  for (int i = 0; i < NbReads; i++)
  {
    ClearCache();
    flush();
    auto result = readCmd.pFunc(TargetSector, NbBurstReadSectors(), false);
    totalColdMs += result.Duration;
  }
  double coldMs = totalColdMs / NbReads;

  std::cerr << "\n[+] Read with " << readCmd.Name << ": "
            << std::setprecision(2) << cachedMs << " ms (cached)" << ", "
            << coldMs << " ms (post-flush)";

  if (coldMs > cachedMs * 2.0)
  {
    std::cerr << "\n    Cache impact confirmed: " << std::setprecision(1)
              << (coldMs / cachedMs) << "x slower after flush";
  }
  else
  {
    std::cerr
        << "\n    Cache impact unclear: post-flush timing similar to cached";
  }
}

//
// TestRCDBitWorks
//
// test if cache can be disabled via RCD bit
int TestRCDBitWorks(sReadCommand &ReadCommand, long int TargetSector,
                    int NbTests)
{
  int InvalidationSuccess = 0;
  double Delay = 0.0;
  double Delay2 = 0.0;
  double InitDelay = 0.0;

  DEBUG << "\ninfo: " << NbTests
        << " test(s), c/nc ratio: " << CachedNonCachedSpeedFactor()
        << ", burst: " << NbBurstReadSectors()
        << ", max: " << MaxCacheSectors();
  for (int i = 0; i < NbTests; i++)
  {
    // enable caching
    if (!SetCacheRCDBit(RCD_READ_CACHE_ENABLED))
    {
      break;
    }

    // first test : normal cache test
    ClearCache();
    auto result = ReadCommand.pFunc(TargetSector, NbBurstReadSectors(),
                                    false); // init read
    InitDelay = result.Duration;
    result = ReadCommand.pFunc(TargetSector + NbBurstReadSectors(),
                               NbBurstReadSectors(), false);
    Delay = result.Duration;
    DEBUG << "\n1) " << TargetSector << " : " << std::setprecision(2)
          << InitDelay << " ms / " << (TargetSector + NbBurstReadSectors())
          << " : " << std::setprecision(2) << Delay << " ms";

    // disable caching
    if (!SetCacheRCDBit(RCD_READ_CACHE_DISABLED))
    {
      break;
    }

    // second test : with cache disabled
    ClearCache();
    result = ReadCommand.pFunc(TargetSector, NbBurstReadSectors(),
                               false); // init read
    InitDelay = result.Duration;
    result = ReadCommand.pFunc(TargetSector + NbBurstReadSectors(),
                               NbBurstReadSectors(), false);
    Delay2 = result.Duration;
    DEBUG << "\n2) " << TargetSector << " : " << std::setprecision(2)
          << InitDelay << " ms / " << (TargetSector + NbBurstReadSectors())
          << " : " << std::setprecision(2) << Delay2 << " ms";

    // compare times
    if (Delay2 > (CachedNonCachedSpeedFactor() * Delay))
    {
      InvalidationSuccess++;
    }
  }
  DEBUG << "\nresult: " << InvalidationSuccess << '/' << NbTests << '\n';
  return (InvalidationSuccess);
}

// wrapper for TestRCDBit
int TestRCDBitWorksWrapper(long int TargetSector, int NbTests)
{
  auto &&cmd = GetSupportedCommand();
  DEBUG << "\ninfo: using command " << cmd.Name;
  return TestRCDBitWorks(cmd, TargetSector, NbTests);
}

//------------------------------------------------------------------------------
// TestCacheLineSize_Straight  (METHOD 1 : STRAIGHT)
//
// The initial read should fill in the cache. Thus, following ones should be
// read much faster until the end of the cache. Therefore, a sudden increase of
// durations of the read accesses should indicate the size of the cache line. We
// have to be careful though that the cache cannot be refilled while we try to
// find the limits of the cache, otherwise we will get a multiple of the cache
// line size and not the cache line size itself.
//
//------------------------------------------------------------------------------
int TestCacheLineSize_Straight(sReadCommand &ReadCommand, long int TargetSector,
                               int NbMeasures)
{
  int TargetSectorOffset = 0;
  int CacheLineSize = 0;
  int MaxCacheLineSize = 0;
  double PreviousDelay = 0.0;
  double InitialDelay = 0.0;

  DEBUG << "\ninfo: " << NbMeasures
        << " test(s), c/nc ratio: " << CachedNonCachedSpeedFactor()
        << ", burst: " << NbBurstReadSectors()
        << ", max: " << MaxCacheSectors();
  for (int i = 0; i < NbMeasures; i++)
  {
    ClearCache();
    PreviousDelay = 50;

    // initial read. After this the drive's cache should be filled
    // with a number of sectors following this one.
    auto result = ReadCommand.pFunc(TargetSector, NbBurstReadSectors(), false);
    InitialDelay = result.Duration;
    SUPERDEBUG << "\n init " << TargetSector << ": " << InitialDelay;

    // read 1 sector at a time and time the reads until one takes more
    // than [CachedNonCachedSpeedFactor()] times the delay taken by the
    // previous read
    double Delay = 0.0;
    for (TargetSectorOffset = 0; TargetSectorOffset < MaxCacheSectors();
         TargetSectorOffset += NbBurstReadSectors())
    {
      auto result = ReadCommand.pFunc(TargetSector + TargetSectorOffset,
                                      NbBurstReadSectors(), false);
      Delay = result.Duration;
      SUPERDEBUG << "\n init " << (TargetSector + TargetSectorOffset) << ": "
                 << Delay;

      if (Delay >= (CachedNonCachedSpeedFactor() * PreviousDelay))
      {
        break;
      }
      else
      {
        PreviousDelay = Delay;
      }
    }

    if (TargetSectorOffset < MaxCacheSectors())
    {
      CacheLineSize = TargetSectorOffset;
      std::cerr << "\n " << ((CacheLineSize * 2352) / 1024) << " kB / "
                << CacheLineSize << " sectors";
      DEBUG << " (" << std::setprecision(2) << InitialDelay << " .. "
            << std::setprecision(2) << PreviousDelay << " -> "
            << std::setprecision(2) << Delay << ")";

      if ((i > NB_IGNORE_MEASURES) && (CacheLineSize > MaxCacheLineSize))
      {
        MaxCacheLineSize = CacheLineSize;
      }
    }
    else
    {
      std::cerr << "\n test aborted.";
    }
  }
  return MaxCacheLineSize;
}

//------------------------------------------------------------------------------
// TestCacheLineSize_Wrap  (METHOD 2 : WRAPAROUND)
//
// The initial read should fill in the cache. Thus, following ones should be
// read much faster until the end of the cache. However, there's the risk that
// at each read new following sectors are cached in, thus showing an infinitely
// large cache with method 1.
// In this case, we detect the cache size by reading again the initial sector :
// when new sectors are read and cached in, the initial sector must be
// cached-out, thus reading it will be longer. Should work fine on Plextor
// drives.
//
// This method allows to avoid the "infinite cache" problem due to background
// reloading even in case of cache hits. However, cache reloading could be
// triggered when a given threshold is reached. So we might be measuring the
// threshold value and not really the cache size.
//
//------------------------------------------------------------------------------
int TestCacheLineSize_Wrap(sReadCommand &ReadCommand, long int TargetSector,
                           int NbMeasures)
{
  int TargetSectorOffset = 0;
  int CacheLineSize = 0;
  int MaxCacheLineSize = 0;
  double InitialDelay = 0.0;
  double PreviousInitDelay = 0.0;

  DEBUG << "\ninfo: " << NbMeasures
        << " test(s), c/nc ratio: " << CachedNonCachedSpeedFactor()
        << ", burst: " << NbBurstReadSectors()
        << ", max: " << MaxCacheSectors();
  for (int i = 0; i < NbMeasures; i++)
  {
    ClearCache();

    // initial read. After this the drive's cache should be filled
    // with a number of sectors following this one.
    auto result = ReadCommand.pFunc(TargetSector, NbBurstReadSectors(), false);
    InitialDelay = result.Duration;
    SUPERDEBUG << "\n init " << TargetSector << ": " << InitialDelay;
    result = ReadCommand.pFunc(TargetSector, NbBurstReadSectors(), false);
    PreviousInitDelay = result.Duration;
    SUPERDEBUG << "\n " << TargetSector << ": " << PreviousInitDelay;

    // read 1 sector forward and the initial sector. If the original sector
    // takes more than [CachedNonCachedSpeedFactor()] times the delay taken by
    // the previous read of, the initial sector, then we reached the limits
    // of the cache
    double Delay = 0.0;
    double Delay2 = 0.0;
    for (TargetSectorOffset = 1; TargetSectorOffset < MaxCacheSectors();
         TargetSectorOffset += NbBurstReadSectors())
    {
      result = ReadCommand.pFunc(TargetSector + TargetSectorOffset,
                                 NbBurstReadSectors(), false);
      Delay = result.Duration;
      SUPERDEBUG << "\n " << (TargetSector + TargetSectorOffset) << ": "
                 << Delay;

      result = ReadCommand.pFunc(TargetSector, NbBurstReadSectors(), false);
      Delay2 = result.Duration;
      SUPERDEBUG << "\n " << TargetSector << ": " << Delay2;

      if (Delay2 >= (CachedNonCachedSpeedFactor() * PreviousInitDelay))
      {
        break;
      }

      PreviousInitDelay = Delay2;
    }

    // did we find a timing drop within the expected limits ?
    if (TargetSectorOffset < MaxCacheSectors())
    {
      // sometimes the first sector can be read so much faster than the
      // next one that is incredibly fast, avoid this by increasing the
      // ratio
      if (TargetSectorOffset <= 1)
      {
        CachedNonCachedSpeedFactor()++;
        DEBUG << "\ninfo: increasing c/nc ratio to "
              << CachedNonCachedSpeedFactor();
        i--;
      }
      else
      {
        CacheLineSize = TargetSectorOffset;
        std::cerr << "\n " << ((CacheLineSize * 2352) / 1024) << " kB / "
                  << CacheLineSize << " sectors";
        DEBUG << " (" << std::setprecision(2) << InitialDelay << " .. "
              << std::setprecision(2) << PreviousInitDelay << " -> "
              << std::setprecision(2) << Delay << ")";

        if ((i > NB_IGNORE_MEASURES) && (CacheLineSize > MaxCacheLineSize))
        {
          MaxCacheLineSize = CacheLineSize;
        }
      }
    }
    else
    {
      std::cerr << "\n no cache detected";
    }
  }
  return MaxCacheLineSize;
}

//------------------------------------------------------------------------------
// TestCacheLineSize_Stat
//
// finds cache line size with a single long burst read of NbMeasures * BurstSize
// sectors, then try to find the cache size with statistical calculations
//------------------------------------------------------------------------------
int TestCacheLineSize_Stat(sReadCommand &ReadCommand, long int TargetSector,
                           int NbMeasures, int BurstSize)
{
  size_t NbPeakMeasures = 0;
  double Maxdelay = 0.0;
  double Threshold = 0.0;
  std::vector<double> Measures;
  Measures.reserve(NbMeasures);
  int CurrentDelta = 0;
  size_t MostFrequentDeltaIndex = 0;
  int MaxDeltaFrequency = 0;
  std::vector<int> PeakMeasuresIndexes;
  PeakMeasuresIndexes.reserve(100);

  struct sDelta
  {
    int delta = 0;
    int frequency = 0;
    short divider = 0;
  };
  // Use a vector so any number of unique deltas can be tracked without
  // silently discarding data when a drive produces erratic timing patterns.
  std::vector<sDelta> DeltaArray;

  // initial read.
  ClearCache();
  auto result = ReadCommand.pFunc(TargetSector, BurstSize, false);
  Measures.push_back(result.Duration);

  // fill in measures buffer
  for (int i = 1; i < NbMeasures; i++)
  {
    result = ReadCommand.pFunc(TargetSector + i * BurstSize, BurstSize, false);
    Measures.push_back(result.Duration);
  }

  // find max time
  Maxdelay = *(std::max_element(std::begin(Measures), std::end(Measures)));
  DEBUG << "\ninitial: " << std::setprecision(2) << Measures[0]
        << " ms, max: " << std::setprecision(2) << Maxdelay << " ms";

  // find all values above threshold% of max
  Threshold = Maxdelay * ThresholdRatioMethod2();
  for (int i = 1; i < NbMeasures; i++)
  {
    if (Measures[i] > Threshold)
      PeakMeasuresIndexes.push_back(i);
  }
  NbPeakMeasures = PeakMeasuresIndexes.size();
  DEBUG << "\nmeas: " << NbPeakMeasures << "/" << NbMeasures << " above "
        << std::setprecision(2) << Threshold << " ms ("
        << ThresholdRatioMethod2() << ")";

  // calculate stats on differences and keep max
  for (size_t i = 1; i < NbPeakMeasures; i++)
  {
    CurrentDelta = PeakMeasuresIndexes[i] - PeakMeasuresIndexes[i - 1];
    SUPERDEBUG << "\ndelta = " << CurrentDelta;

    // Search for this delta in the existing table.
    auto it = std::find_if(DeltaArray.begin(), DeltaArray.end(),
                           [CurrentDelta](const sDelta &entry)
                           { return entry.delta == CurrentDelta; });

    if (it != DeltaArray.end())
    {
      it->frequency++;
      if (it->frequency > MaxDeltaFrequency)
      {
        MaxDeltaFrequency = it->frequency;
        MostFrequentDeltaIndex = std::distance(DeltaArray.begin(), it);
      }
    }
    else
    {
      // New delta — append it. Vector grows as needed, no data is dropped.
      sDelta entry;
      entry.delta = CurrentDelta;
      entry.frequency = 1;
      DeltaArray.push_back(entry);
      if (1 > MaxDeltaFrequency)
      {
        MaxDeltaFrequency = 1;
        MostFrequentDeltaIndex = DeltaArray.size() - 1;
      }
    }
  }

  // find which sizes are multiples of others
  for (auto &deltaI : DeltaArray)
  {
    for (const auto &deltaJ : DeltaArray)
    {
      if ((deltaJ.delta % deltaI.delta == 0) && (&deltaI != &deltaJ))
      {
        deltaI.divider++;
      }
    }
  }

  if (DeltaArray.empty() || NbPeakMeasures == 0)
  {
    std::cerr << "\nno statistical data collected";
    return 0;
  }

  std::cerr << "\nsizes: ";
  size_t i = 0;
  for (const auto &deltaEntry : DeltaArray)
  {
    if (i % 5 == 0)
      std::cerr << '\n';
    std::cerr << " " << deltaEntry.delta << " ("
              << (100 * deltaEntry.frequency / NbPeakMeasures)
              << "%, div=" << deltaEntry.divider << ")";
    i++;
  }

  std::cerr
      << "\nfmax = " << DeltaArray[MostFrequentDeltaIndex].frequency << " ("
      << (100 * DeltaArray[MostFrequentDeltaIndex].frequency / NbPeakMeasures)
      << "%) : " << ((DeltaArray[MostFrequentDeltaIndex].delta * 2352) / 1024)
      << " kB, " << DeltaArray[MostFrequentDeltaIndex].delta << " sectors";
  return (DeltaArray[MostFrequentDeltaIndex].delta * BurstSize);
}

// wrapper for TestCacheLineSize
int TestCacheLineSizeWrapper(long int TargetSector, int NbMeasures,
                             int BurstSize, short method)
{
  int retval = -1;

  for (auto &&cmd : Commands)
  {
    if (cmd.Supported)
    {
      DEBUG << "\ninfo: using command " << cmd.Name;

      switch (method)
      {
      case 1:
        retval = TestCacheLineSize_Wrap(cmd, TargetSector, NbMeasures);
        break;
      case 2:
        retval = TestCacheLineSize_Straight(cmd, TargetSector, NbMeasures);
        break;
      case 3:
        retval =
            TestCacheLineSize_Stat(cmd, TargetSector, NbMeasures, BurstSize);
        break;
      default:
        std::cerr << "\nError: invalid method !!\n";
      }
      break;
    }
  }
  return retval;
}

//------------------------------------------------------------------------------
// TestCacheLineNumber
//
// finds number of cache lines by reading 1 sector at N (loading the cache),
// then another sector at M>>N, then at N+1 and N+2. If there are multiple cache
// lines, the read at N+1 should be done from the already loaded cache, so it
// will be very fast and the same time as the read at N+2. Otherwise, the read
// at N+1 will reload the cache and it will be much slower than the one at N+2.
// To find out the number of cache lines, we read multiple M sectors at various
// positions
//------------------------------------------------------------------------------
int TestCacheLineNumber(sReadCommand &ReadCommand, long int TargetSector,
                        int NbMeasures)
{
  int NbCacheLines = 1;
  double PreviousDelay = 0.0;
  long int LocalTargetSector = TargetSector;

  DEBUG << "\ninfo: using c/nc ratio : " << CachedNonCachedSpeedFactor();
  if (!DEBUG)
  {
    std::cerr << "\n";
  }

  for (int i = 0; i < NbMeasures; i++)
  {
    ClearCache();
    NbCacheLines = 1;

    // initial read. After this the drive's cache should be filled
    // with a number of sectors following this one.
    auto result = ReadCommand.pFunc(LocalTargetSector, 1, false);
    PreviousDelay = result.Duration;
    SUPERDEBUG << "\n first read at " << LocalTargetSector << ": "
               << std::setprecision(2) << PreviousDelay;

    for (int j = 1; j < MAX_CACHE_LINES; j++)
    {
      // second read to load another (?) cache line
      ReadCommand.pFunc(LocalTargetSector + 10000, 1, false);

      // read 1 sector next to the original one
      result = ReadCommand.pFunc(LocalTargetSector + 2 * j, 1, false);
      double Delay = result.Duration;
      SUPERDEBUG << "\n read at " << (LocalTargetSector + 2 * j) << ": "
                 << std::setprecision(2) << Delay;

      if (DEBUG || SUPERDEBUG)
      {
        std::cerr << "\n"
                  << std::setprecision(2) << PreviousDelay << " / "
                  << std::setprecision(2) << Delay << " -> ";
      }
      if (Delay <= (PreviousDelay / CachedNonCachedSpeedFactor()))
      {
        NbCacheLines++;
      }
      else
      {
        break;
      }
    }
    std::cerr << " " << NbCacheLines;
    LocalTargetSector += 2000;
  }
  return NbCacheLines;
}

// wrapper for TestCacheLineNumber
int TestCacheLineNumberWrapper(long int TargetSector, int NbMeasures)
{
  auto &&cmd = GetSupportedCommand();
  std::cerr << "\ninfo: using command " << cmd.Name;
  return TestCacheLineNumber(cmd, TargetSector, NbMeasures);
}

//------------------------------------------------------------------------------
// TestPlextorFUAInvalidationSize
//
// find size of cache invalidated by Plextor FUA command
//------------------------------------------------------------------------------
int TestPlextorFUAInvalidationSize(sReadCommand &ReadCommand,
                                   long int TargetSector, int NbMeasures)
{
  constexpr int CACHE_TEST_BLOCK = 20;

  int TargetSectorOffset = 0;
  int InvalidatedSize = 0;

  DEBUG << "\ninfo: using c/nc ratio : " << CachedNonCachedSpeedFactor();

  for (int i = 0; i < NbMeasures; i++)
  {
    for (TargetSectorOffset = 2000; TargetSectorOffset >= 0;
         TargetSectorOffset -= CACHE_TEST_BLOCK)
    {
      ClearCache();

      // initial read of 1 sector. After this the drive's cache should be
      // filled with a number of sectors following this one.
      auto result = ReadCommand.pFunc(TargetSector, 1, false);
      double InitialDelay = result.Duration;
      SUPERDEBUG << "\n(" << i << ") init = " << std::setprecision(2)
                 << InitialDelay << ", thr = " << std::setprecision(2)
                 << (InitialDelay / CachedNonCachedSpeedFactor());

      // invalidate cache with Plextor FUA command
      PlextorFUAFlush(TargetSector);

      // clang-format off
// now we should get this :
//
//  cache :             |-- invalidated --|--- still cached ---|
//  reading speeds :    |- slow (flushed)-|--- fast (cached) --|-- slow (not read yet) --|
//                                        ^
//                                        |
// read sectors backwards to find this ---|  spot
      // clang-format on

      result = ReadCommand.pFunc(TargetSector + TargetSectorOffset, 1, false);
      double Delay = result.Duration;
      SUPERDEBUG << " (" << i << ") " << (TargetSector + TargetSectorOffset)
                 << ": " << std::setprecision(2) << Delay;

      if (Delay <= (InitialDelay / CachedNonCachedSpeedFactor()))
      {
        InvalidatedSize = TargetSectorOffset;
        break;
      }
    }
  }
  return (InvalidatedSize - CACHE_TEST_BLOCK);
}

// wrapper for TestPlextorFUAInvalidationSize
int TestPlextorFUAInvalidationSizeWrapper(long int TargetSector, int NbMeasures)
{
  auto &&cmd = GetSupportedCommand();
  DEBUG << "\ninfo: using command " << cmd.Name;
  return TestPlextorFUAInvalidationSize(cmd, TargetSector, NbMeasures);
}

bool TestRCDBitSupport()
{
  bool retval = false;
  if (ModeSense(CACHING_MODE_PAGE, 0, 20))
  {
    retval = true;
  }
  else
  {
    std::cerr << "not supported";
  }
  return (retval);
}

//------------------------------------------------------------------------------
// TestCacheLineSizePrefetch
//
// This method is using only the Prefetch command, which is described as follows
// in SBC3 :
// - if the specified logical blocks were successfully transferred to the cache,
//   the device server shall return CONDITION MET
// - if the cache does not have sufficient capacity to accept all of the
//   specified logical blocks, the device server shall transfer to the cache as
//   many of the specified logical blocks that fit.
//   If these logical blocks are transferred successfully it shall return GOOD
//   status
//
//------------------------------------------------------------------------------
int TestCacheLineSizePrefetch(long int TargetSector)
{
  int NbSectors = 1;

  auto result = Prefetch(TargetSector, NbSectors);
  while (result.ScsiStatusCode == ScsiStatus::CONDITION_MET &&
         NbSectors <= MaxCacheSectors())
  {
    result = Prefetch(TargetSector, NbSectors++);
  }
  if ((result.ScsiStatusCode == ScsiStatus::GOOD) && (NbSectors > 1))
  {
    std::cerr << "\n-> cache size = " << ((NbSectors - 1) * 2352 / 1024)
              << " kB, " << (NbSectors - 1) << " sectors";
  }
  else
  {
    std::cerr << "\nError: this method does not seem to work on this drive";
  }
  return NbSectors - 1;
}

template <typename TestFn>
void RunTest(bool SpinDriveFlag, unsigned int SpinSeconds, TestFn &&fn)
{
  platform::set_critical_priority();
  if (SpinDriveFlag)
  {
    SpinDrive(SpinSeconds);
  }
  fn();
  platform::set_normal_priority();
}

void PrintUsage()
{
  std::cerr << "\nUsage:   cachex <commands> <options> <drive letter>\n";
  std::cerr << "\nCommands:  -i     : show drive info\n";
  std::cerr << "           -c     : test drive cache\n";
  //    std::cerr <<"           -c2    : test drive cache (method 2)\n";
  //    std::cerr <<"           -c3    : test drive cache (method 3)\n";
  std::cerr << "           -p     : test plextor FUA command\n";
  std::cerr << "           -q     : test standard SYNCHRONIZE CACHE (0x35)\n";
  std::cerr << "           -k     : test cache disabling\n";
  std::cerr << "           -w     : test cache line numbers\n";
  std::cerr << "           -z     : test cache speed impact\n";
  std::cerr << "\nOptions:   -d     : show debug info\n";
  std::cerr
      << "           -l xx  : spin drive for xx seconds before starting to "
         "measure\n";
  //    std::cerr <<"           -b xx  : use burst reads of size xx\n";
  //    std::cerr <<"           -t xx  : threshold at xx% for cache tests\n";
  std::cerr << "           -x xx  : use cached/non cached ratio xx\n";
  std::cerr << "           -r xx  : use read command xx (one of ";
  for (auto &&cmd : Commands)
  {
    std::cerr << cmd.Name;
    if (&cmd != &Commands.back())
    {
      std::cerr << ", ";
    }
  }
  std::cerr << ")\n";
  std::cerr << "           -s xx  : set read speed to xx (0=max)\n";
  std::cerr << "           -m xx  : look for cache size up to xx sectors\n";
  //    std::cerr << "           -y xx  : use xx sectors for cache test method
  //    2\n";
  std::cerr << "           -n xx  : perform xx tests\n";
}

} // namespace

int main(int argc, char **argv)
{
  std::cerr << std::fixed;
  const char *DrivePath = nullptr;
  int MaxReadSpeed = 0;
  bool SpinDriveFlag = false;
  bool ShowDriveInfos = false;
  bool TestPlextorFUA = false;
  bool TestSyncCache = false;
  bool SetMaxDriveSpeed = false;
  bool CacheMethod1 = false;
  bool CacheMethod2 = false;
  bool CacheMethod3 = false;
  bool CacheMethod4 = false;
  bool CacheNbTest = false;
  bool PFUAInvalidationSizeTest = false;
  bool TestRCDBit = false;
  bool TestSpeedImpact = false;
  int NbSecsDriveSpin = 10;
  int NbSectorsMethod2 = 1000;
  int InvalidatedSectors = 0;
  int Nbtests = 0;
  const char *UserReadCommand = nullptr;

  // --------------- setup ---------------------------
  std::cerr << "\nCacheExplorer 0.15-LLM - "
               "https://github.com/malvarenga123/cachex, based on";
  std::cerr
      << "\nCacheExplorer 0.14 - https://github.com/xavery/cachex, based on";
  std::cerr << "\nCacheExplorer 0.9 - spath@cdfreaks.com\n";

  // ------------ command line parsing --------------
  if (argc < 2)
  {
    PrintUsage();
    return (-1);
  }
  try
  {
    for (int i = 1; i < argc; i++)
    {
      if (argv[i][0] == '-')
      {
        switch (argv[i][1])
        {
        case 'c':
          if (argv[i][2] == '2')
          {
            CacheMethod2 = true;
          }
          else if (argv[i][2] == '3')
          {
            CacheMethod3 = true;
          }
          else if (argv[i][2] == '4')
          {
            CacheMethod4 = true;
          }
          else
          {
            CacheMethod1 = true; // default method
          }
          break;
        case 'p':
          TestPlextorFUA = true;
          break;
        case 'q':
          TestSyncCache = true;
          break;
        case 'k':
          TestRCDBit = true;
          break;
        case 'i':
          ShowDriveInfos = true;
          break;
        case 'd':
          DEBUG.Enabled = true;
          break;
        case 'l':
          if (++i >= argc)
          {
            std::cerr << "\nError: -l requires an argument\n";
            return -1;
          }
          NbSecsDriveSpin = std::stoi(argv[i]);
          SpinDriveFlag = true;
          break;
        case 'b':
          if (++i >= argc)
          {
            std::cerr << "\nError: -b requires an argument\n";
            return -1;
          }
          NbBurstReadSectors() = std::stoi(argv[i]);
          break;
        case 'r':
          if (++i >= argc)
          {
            std::cerr << "\nError: -r requires an argument\n";
            return -1;
          }
          UserReadCommand = argv[i];
          break;
        case 's':
          SetMaxDriveSpeed = true;
          if (++i >= argc)
          {
            std::cerr << "\nError: -s requires an argument\n";
            return -1;
          }
          MaxReadSpeed = std::stoi(argv[i]);
          break;
        case 'w':
          CacheNbTest = true;
          break;
        case 'm':
          if (++i >= argc)
          {
            std::cerr << "\nError: -m requires an argument\n";
            return -1;
          }
          MaxCacheSectors() = std::stoi(argv[i]);
          break;
        case 'y':
          if (++i >= argc)
          {
            std::cerr << "\nError: -y requires an argument\n";
            return -1;
          }
          NbSectorsMethod2 = std::stoi(argv[i]);
          break;
        case 't':
        {
          if (++i >= argc)
          {
            std::cerr << "\nError: -t requires an argument\n";
            return -1;
          }
          int v = std::stoi(argv[i]);
          ThresholdRatioMethod2() = v / 100.0;
          break;
        }
        case 'x':
          if (++i >= argc)
          {
            std::cerr << "\nError: -x requires an argument\n";
            return -1;
          }
          CachedNonCachedSpeedFactor() = std::stoi(argv[i]);
          break;
        case 'n':
          if (++i >= argc)
          {
            std::cerr << "\nError: -n requires an argument\n";
            return -1;
          }
          Nbtests = std::stoi(argv[i]);
          break;
        case 'z':
          TestSpeedImpact = true;
          break;

          // non documented options
        case '/':
          PFUAInvalidationSizeTest = true;
          break;
        case '.':
          SUPERDEBUG.Enabled = true;
          break;
        default:
          PrintUsage();
          return (-1);
        }
      }
      else // must be drive then
      {
        DrivePath = argv[i];
      }
    }
  }
  catch (const std::invalid_argument &)
  {
    std::cerr << "\nError: invalid numeric argument\n";
    return -1;
  }
  catch (const std::out_of_range &)
  {
    std::cerr << "\nError: numeric argument out of range\n";
    return -1;
  }

  if (!DrivePath)
  {
    std::cerr << "\nError: no drive selected\n";
    PrintUsage();
    return -1;
  }

  // ------------ actual stuff --------------

  // Initialise DriveContext from parsed command-line values.
  // All functions access these through the inline accessors above.
  g_ctx.NbBurstReadSectors = NbBurstReadSectors();
  g_ctx.MaxCacheSectors = MaxCacheSectors();
  g_ctx.ThresholdRatioMethod2 = ThresholdRatioMethod2();
  g_ctx.CachedNonCachedSpeedFactor = CachedNonCachedSpeedFactor();

  //
  // Open drive — RAII wrapper guarantees close on any exit path.
  //
  DeviceHandle<platform> deviceHandle(DrivePath);
  if (!deviceHandle.valid())
  {
    std::cerr << "\nError: cannot open drive " << DrivePath << "\n";
    return -1;
  }
  // Publish the raw handle into g_ctx so all helper functions can reach it
  // via the hVolume() accessor without needing to pass the wrapper around.
  hVolume() = deviceHandle.get();

  std::cerr << "\nDrive on " << DrivePath << " is ";
  if (!PrintDriveInfo())
  {
    std::cerr << "\nError: cannot read drive info";
    return -1;
  }
  std::cerr << '\n';

  //-------------------------------------------------------------------

  //
  // 0) Test all supported read commands / test user selected command
  //
  if (ShowDriveInfos)
  {
    ShowCacheValues();
    if (!TestSupportedFlushCommands())
    {
      std::cerr << "\nError: no supported flush commands found\n";
    }
    if (!TestSupportedReadCommands())
    {
      std::cerr << "\nError: no supported read commands found\n";
      return -1;
    }
  }

  if (UserReadCommand != nullptr)
  {
    auto chosen_cmd =
        std::find(std::begin(Commands), std::end(Commands), UserReadCommand);

    if (chosen_cmd == std::end(Commands))
    {
      std::cerr << "\nError: command " << UserReadCommand
                << " is not recognized\n";
      return -1;
    }

    if (chosen_cmd->pFunc(10000, 1, false))
    {
      for (auto &&cmd : Commands)
        cmd.Supported = false;
      chosen_cmd->Supported = true;
      // Restore FUAbitSupported to its declared value. Running -i before -r
      // can clear FUAbitSupported if the drive rejected a FUA probe read,
      // but when the user explicitly selects a command with -r we respect
      // the table declaration. The declared value is stored in DeclaredFUA.
      chosen_cmd->FUAbitSupported = chosen_cmd->DeclaredFUA;
    }
    else
    {
      std::cerr << "\nError: command " << UserReadCommand << " not supported\n";
      return -1;
    }
  }

  //
  // 1) Set drive speed
  //
  if (SetMaxDriveSpeed)
  {
    if (DEBUG)
    {
      std::cerr << "\n[+] Changing read speed to ";
      if (MaxReadSpeed == 0)
      {
        std::cerr << "max\n";
      }
      else
      {
        std::cerr << MaxReadSpeed << "x\n";
      }
    }
    SetDriveSpeed(MaxReadSpeed, 0);
  }

  //
  // 2) Test support for Plextor's FUA cache clearing command
  //
  if (TestPlextorFUA && TestPlextorFUACommand())
  {
    // The flush command is accepted — mark it available for -z regardless
    // of whether the effectiveness test passes. The test may return 0/5
    // when the read command and flush command are the same (e.g. -r 28h_12),
    // but the flush mechanism itself is still valid for use in other tests.
    g_ctx.PlextorFlushValidated = true;
    RunTest(SpinDriveFlag, NbSecsDriveSpin,
            [&]()
            {
              Nbtests = (Nbtests == 0) ? 5 : Nbtests;
              std::cerr << "\n[+] Plextor flush tests: ";
              int plexResult =
                  TestPlextorFUACommandWorksWrapper(15000, Nbtests);
              std::cerr << plexResult << '/' << Nbtests;
              if (plexResult == 0)
              {
                auto &rc = GetSupportedCommand();
                if (strcmp(rc.Name, "28h_12") == 0)
                  std::cerr << "\n    Note: flush test unreliable when read "
                               "command is also 28h_12 (same as flush command)."
                               " Try -r BEh or -r D8h for a cleaner test.";
              }

              if (PFUAInvalidationSizeTest)
              {
                // 4) Find the size of data invalidated  by Plextor FUA command
                std::cerr
                    << "\n[+] Testing invalidation of Plextor flush command: ";
                InvalidatedSectors =
                    TestPlextorFUAInvalidationSizeWrapper(15000, 1);
                std::cerr << "\nresult: "
                          << ((InvalidatedSectors > 0) ? "ok" : "not working")
                          << " (" << InvalidatedSectors << ')';
              }
            });
    Nbtests = 0; // reset so subsequent blocks get their own default
  }

  //
  // 2b) Test standard SYNCHRONIZE CACHE (0x35)
  //
  if (TestSyncCache && TestSyncCacheCommand())
  {
    RunTest(SpinDriveFlag, NbSecsDriveSpin,
            [&]()
            {
              Nbtests = (Nbtests == 0) ? 5 : Nbtests;
              std::cerr << "\n[+] SYNCHRONIZE CACHE flush tests: ";
              int syncResult = TestSyncCacheCommandWorksWrapper(15000, Nbtests);
              std::cerr << syncResult << '/' << Nbtests;
              if (syncResult > 0)
                g_ctx.SyncCacheValidated = true;
            });
    Nbtests = 0; // reset so subsequent blocks get their own default
  }

  //
  // 3) Explore cache structure
  //
  int CacheLineSizeSectors = 0;
  if (CacheMethod1)
  {
    // SIZE : method 1
    RunTest(SpinDriveFlag, NbSecsDriveSpin,
            [&]()
            {
              Nbtests = (Nbtests == 0) ? 10 : Nbtests;
              std::cerr << "\n[+] Testing cache line size:";
              CacheLineSizeSectors =
                  TestCacheLineSizeWrapper(15000, Nbtests, 0, 1);
              if (CacheLineSizeSectors > 0)
                std::cerr << "\n[+] Cache line size result: "
                          << CacheLineSizeSectors << " sectors ("
                          << (CacheLineSizeSectors * 2352 / 1024) << " kB)";
            });
  }

  if (CacheMethod2)
  {
    // SIZE : method 2
    RunTest(SpinDriveFlag, NbSecsDriveSpin,
            [&]()
            {
              Nbtests = (Nbtests == 0) ? 20 : Nbtests;
              std::cerr << "\n[+] Testing cache line size (method 2):";
              CacheLineSizeSectors =
                  TestCacheLineSizeWrapper(15000, Nbtests, 0, 2);
              if (CacheLineSizeSectors > 0)
                std::cerr << "\n[+] Cache line size result: "
                          << CacheLineSizeSectors << " sectors ("
                          << (CacheLineSizeSectors * 2352 / 1024) << " kB)";
            });
  }

  if (CacheNbTest)
  {
    // NUMBER
    RunTest(SpinDriveFlag, NbSecsDriveSpin,
            [&]()
            {
              Nbtests = (Nbtests == 0) ? 5 : Nbtests;
              std::cerr << "\n[+] Testing cache line numbers : "
                        << TestCacheLineNumberWrapper(15000, Nbtests);
            });
  }

  if (CacheMethod3)
  {
    // SIZE : method 3 (STATS)
    RunTest(SpinDriveFlag, NbSecsDriveSpin,
            [&]()
            {
              std::cerr << "\n[+] Testing cache line size (method 3):";
              TestCacheLineSizeWrapper(15000, NbSectorsMethod2,
                                       NbBurstReadSectors(), 3);
            });
  }

  if (CacheMethod4)
  {
    // SIZE : method 4 (PREFETCH)
    std::cerr << "\n[+] Testing cache line size (method 4):";
    TestCacheLineSizePrefetch(10000);
  }

  if (TestRCDBit)
  {
    std::cerr << "\n[+] Testing cache disabling: ";

    Nbtests = (Nbtests == 0) ? 3 : Nbtests;
    if (TestRCDBitSupport())
    {
      if (TestRCDBitWorksWrapper(15000, Nbtests) > 0)
      {
        std::cerr << "ok";
      }
      else
      {
        std::cerr << "not supported";
      }
    }
  }

  if (TestSpeedImpact)
  {
    RunTest(SpinDriveFlag, NbSecsDriveSpin,
            [&]()
            {
              Nbtests = (Nbtests == 0) ? 5 : Nbtests;
              TestCacheSpeedImpact(10000, Nbtests);
            });
  }

  std::cerr << '\n';
  // deviceHandle destructor closes the drive handle automatically.
  return 0;
}
