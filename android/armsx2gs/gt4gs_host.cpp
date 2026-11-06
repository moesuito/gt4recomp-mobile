// SPDX-License-Identifier: GPL-3.0+
//
// GT4Recomp private Android test build (PCSX2-tainted, GPL). NOT distributed and never published.
//
// libgt4gs.so: the ARMSX2 (PCSX2-family) GS running inside the GT4Recomp Android game process. This file is the host
// around it: the frontend stubs PCSX2 expects (a port of the owner's fork frontend, gt4recomp-gs pcsx2-gt4gs), the
// Android window (SDL's ANativeWindow, given by the game), the GS settings, the boot of the GS-only VM and the C ABI
// declared in gt4gs_api.h.

#include "gt4gs_host.h"

#include "common/Assertions.h"
#include "common/Console.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/MemorySettingsInterface.h"
#include "common/Path.h"
#include "common/ProgressCallback.h"
#include "common/StringUtil.h"
#include "common/Timer.h"

#include "pcsx2/PrecompiledHeader.h"

#include "pcsx2/Config.h"
#include "pcsx2/GS.h"
#include "pcsx2/GS/GS.h"
#include "pcsx2/GS/Renderers/Common/GSDevice.h"
#include "pcsx2/GSDumpReplayer.h"
#include "pcsx2/Host.h"
#include "pcsx2/PerformanceMetrics.h"
#include "pcsx2/Achievements.h"
#include "pcsx2/ImGui/FullscreenUI.h"
#include "pcsx2/ImGui/ImGuiFullscreen.h"
#include "pcsx2/ImGui/ImGuiManager.h"
#include "pcsx2/INISettingsInterface.h"
#include "pcsx2/MTGS.h"
#include "pcsx2/PerformanceMetrics.h"
#include "pcsx2/Achievements.h"
#include "pcsx2/ImGui/FullscreenUI.h"
#include "pcsx2/ImGui/ImGuiFullscreen.h"
#include "pcsx2/ImGui/ImGuiManager.h"
#include "pcsx2/SIO/Pad/Pad.h"
#include "pcsx2/VMManager.h"

#include "gt4gs_api.h"
#include "gt4gs_queue.h"
#include "gt4gs_replayer.h"

#include <android/log.h>
#include <android/native_window.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace gt4gs;

namespace {

constexpr const char *kLogTag = "GT4Recomp-gs";
constexpr const char *kVersion = "gt4gs 0.1 (ARMSX2 1049db37fb, in-process private test build)";
constexpr u32 kBootTimeoutSeconds = 60;
// PCSX2 tracks the display rectangle (0,0,1,2 crops the one column and two rows GT4's display leaves outside the
// picture), as the Windows gt4-gs build does.
constexpr int kDisplayCrop[4] = {0, 0, 1, 2};

MemorySettingsInterface s_settings;

std::unique_ptr<QueueSource> s_source;
std::string s_serial = "SCUS-97328";
std::string s_error_text;
std::string s_data_root;
std::string s_config_path;
std::string s_overrides_path;
std::string s_renderer_name = "OpenGL";
bool s_log_performance = false;

std::thread s_cpu_thread;
std::mutex s_boot_mutex;
std::condition_variable s_boot_cv;
bool s_boot_finished = false;
bool s_boot_ok = false;
std::string s_boot_error;
std::atomic<bool> s_booted{false};
std::atomic<bool> s_started{false};
std::atomic<bool> s_boot_deferred{false};
std::atomic<bool> s_finished{false};
std::atomic<bool> s_stopping{false};
int s_preferred_backend = -1;

// --- the window (ANativeWindow from SDL) -------------------------------------------------------------
std::mutex s_window_mutex;
std::condition_variable s_window_cv;
ANativeWindow *s_window = nullptr;         // the game's surface, borrowed with a reference
ANativeWindow *s_acquired_window = nullptr;  // the GS thread's own reference for the live swap chain
u32 s_window_width = 0;
u32 s_window_height = 0;
float s_window_refresh_rate = 0.0f;

// --- tasks queued for the CPU thread (Host::RunOnCPUThread) ------------------------------------------
struct HostTask {
  void (*function)(void *);
  void *argument;
};
std::mutex s_tasks_mutex;
std::vector<HostTask> s_tasks;

void LogLine(const char *format, ...) {
  std::va_list args;
  va_start(args, format);
  std::fprintf(stderr, "[gt4gs] ");
  std::vfprintf(stderr, format, args);
  std::fprintf(stderr, "\n");
  std::fflush(stderr);
  va_end(args);
}

// Copies every key of the sections PCSX2's GS reads from `path` into the in-memory settings layer. The Windows
// launcher writes exactly these sections, and the owner's own file is applied on top of them, so a line he adds wins.
bool ApplyIniFile(const char *path, bool report_missing) {
  if (path == nullptr || *path == '\0') return true;
  if (!FileSystem::FileExists(path)) {
    if (report_missing) LogLine("settings file %s not found; defaults kept", path);
    return false;
  }
  INISettingsInterface ini{std::string(path)};
  if (!ini.Load()) {
    LogLine("settings file %s could not be read", path);
    return false;
  }
  u32 applied = 0;
  for (const char *section : {"EmuCore/GS", "EmuCore", "Folders", "GT4GS"}) {
    for (const auto &[key, value] : ini.GetKeyValueList(section)) {
      s_settings.SetStringValue(section, key.c_str(), value.c_str());
      ++applied;
    }
  }
  LogLine("settings: %u values from %s", applied, path);
  return true;
}

void ApplyBaseSettings() {
  // The Windows launcher's own values for the PCSX2 GS (docs/GS-SETTINGS.md), then the game's session file, then the
  // owner's extra lines.
  s_settings.SetIntValue("EmuCore/GS", "Renderer", static_cast<int>(GSRendererType::OGL));
  s_settings.SetIntValue("EmuCore/GS", "accurate_blending_unit", static_cast<int>(AccBlendLevel::High));
  s_settings.SetIntValue("EmuCore/GS", "deinterlace_mode", static_cast<int>(GSInterlaceMode::Automatic));
  s_settings.SetStringValue("EmuCore/GS", "AspectRatio", "16:9");
  s_settings.SetIntValue("EmuCore/GS", "CropLeft", kDisplayCrop[0]);
  s_settings.SetIntValue("EmuCore/GS", "CropTop", kDisplayCrop[1]);
  s_settings.SetIntValue("EmuCore/GS", "CropRight", kDisplayCrop[2]);
  s_settings.SetIntValue("EmuCore/GS", "CropBottom", kDisplayCrop[3]);
  // Texture loading only, as on Windows; dumping is never taken from a settings file.
  s_settings.SetBoolValue("EmuCore/GS", "DumpReplaceableTextures", false);
  s_settings.SetBoolValue("EmuCore/GS", "DumpReplaceableMipmaps", false);
  s_settings.SetBoolValue("EmuCore/GS", "DumpTexturesWithFMVActive", false);
  // No frame limiter: the game's own pacing clock drives the stream, as on Windows.
  s_settings.SetBoolValue("EmuCore/GS", "FrameLimitEnable", false);
  s_settings.SetBoolValue("EmuCore/GS", "VsyncEnable", false);
  s_settings.SetBoolValue("EmuCore/GS", "OsdShowFPS", false);
  s_settings.SetBoolValue("EmuCore/GS", "OsdShowGPU", false);
  s_settings.SetBoolValue("EmuCore/GS", "OsdShowResolution", false);
  s_settings.SetIntValue("EmuCore/GS", "OsdMessagesPos", static_cast<int>(OsdOverlayPos::None));

  // Nothing else of the emulator runs: no inputs, no sound backend, no memory cards, no achievements.
  s_settings.SetBoolValue("InputSources", "SDL", false);
  s_settings.SetBoolValue("InputSources", "XInput", false);
  s_settings.SetStringValue("SPU2/Output", "Backend", "Null");
  Pad::ClearPortBindings(s_settings, 0);
  s_settings.ClearSection("Hotkeys");
  s_settings.SetBoolValue("Achievements", "Enabled", false);
  for (u32 slot = 0; slot < 2; slot++) {
    s_settings.SetBoolValue("MemoryCards", fmt::format("Slot{}_Enable", slot + 1).c_str(), false);
    s_settings.SetStringValue("MemoryCards", fmt::format("Slot{}_Filename", slot + 1).c_str(), "");
  }

  // Logging: the game already sends this process's stderr to the session's game.log (and to logcat).
  s_settings.SetBoolValue("Logging", "EnableSystemConsole", true);
  s_settings.SetBoolValue("Logging", "EnableFileLogging", false);
  s_settings.SetBoolValue("Logging", "EnableTimestamps", true);
  s_settings.SetBoolValue("Logging", "EnableVerbose", true);
}

bool CreateSessionFolders() {
  if (s_data_root.empty()) {
    s_error_text = "no data root given";
    return false;
  }
  for (const char *folder : {"", "cache", "textures", "logs", "resources", "snapshots"}) {
    const std::string path = *folder == '\0' ? s_data_root : Path::Combine(s_data_root, folder);
    if (!FileSystem::DirectoryExists(path.c_str()) && !FileSystem::CreateDirectoryPath(path.c_str(), false)) {
      s_error_text = "cannot create " + path;
      return false;
    }
  }
  return true;
}

// Boots the GS-only VM: no BIOS, no disc, no game code -- the CPU provider is the stream replayer, exactly as the
// Windows gt4-gs frontend does it.
void CpuThreadMain() {
  bool ok = false;
  std::string why = "CPU thread did not start";
  if (VMManager::Internal::CPUThreadInitialize()) {
    VMManager::ApplySettings();
    GSDumpReplayer::SetIsDumpRunner(true);

    VMBootParameters params;
    params.filename = "gt4gs-inprocess";  // only a name: the VMManager hook routes it to the stream replayer
    params.disable_achievements_hardcore_mode = true;

    Error boot_error;
    const VMBootResult result = VMManager::Initialize(params, &boot_error);
    if (result == VMBootResult::StartupSuccess) {
      ok = true;
      s_booted.store(true);
      {
        std::lock_guard lock(s_boot_mutex);
        s_boot_finished = true;
      }
      s_boot_cv.notify_all();

      VMManager::SetState(VMState::Running);
      if (g_gs_device) g_gs_device->SetGPUTimingEnabled(true);
      while (VMManager::GetState() == VMState::Running) VMManager::Execute();
      VMManager::Shutdown(false);
      LogLine("GS session ended: %llu frames", static_cast<unsigned long long>(GetFrameNumber()));
    } else {
      why = "GS boot failed: " + boot_error.GetDescription();
      ReportFatal(5, why.c_str());
    }
  } else {
    why = "CPU thread initialization failed";
  }

  VMManager::Internal::CPUThreadShutdown();
  if (!ok) {
    {
      std::lock_guard lock(s_boot_mutex);
      s_boot_error = why;
      s_boot_finished = true;
    }
    s_boot_cv.notify_all();
  }
  s_finished.store(true);
}

// Boots the GS VM once the game's window has arrived (see gt4gs_start).
void BootIfWaitingForWindow() {
  if (!s_started.load()) return;  // gt4gs_start decides for the window it was given
  bool expected = false;
  if (!s_boot_deferred.compare_exchange_strong(expected, true)) return;
  LogLine("window attached: opening the GS");
  s_cpu_thread = std::thread(CpuThreadMain);
}

// Waits for the boot that gt4gs_start started (or the deferred one).
bool WaitForBoot() {
  std::unique_lock lock(s_boot_mutex);
  const bool finished = s_boot_cv.wait_for(lock, std::chrono::seconds(kBootTimeoutSeconds),
                                           [&] { return s_boot_finished; });
  if (!finished) {
    s_error_text = "the GS did not finish booting within 60 s";
    LogLine("%s", s_error_text.c_str());
    return false;
  }
  if (!s_boot_error.empty()) {
    s_error_text = s_boot_error;
    LogLine("%s", s_error_text.c_str());
    return false;
  }
  return true;
}

// The host stubs below are the ones the owner's fork frontend provides, minus the Windows windowing.
}  // namespace

// --- window ------------------------------------------------------------------------------------------
namespace gt4gs::host {

void SetNativeWindow(void *native_window, std::uint32_t width, std::uint32_t height) {
  {
    std::lock_guard lock(s_window_mutex);
    ANativeWindow *window = static_cast<ANativeWindow *>(native_window);
    if (s_window != nullptr) ANativeWindow_release(s_window);
    s_window = window;
    if (window != nullptr) ANativeWindow_acquire(window);
    if (width > 0 && height > 0) {
      s_window_width = width;
      s_window_height = height;
    }
  }
  s_window_cv.notify_all();
  BootIfWaitingForWindow();
  if (MTGS::IsOpen()) {
    RunOnCpuThread(
        [](void *) {
          if (MTGS::IsOpen()) MTGS::UpdateDisplayWindow();
        },
        nullptr);
  }
}

void DetachNativeWindow() {
  {
    std::lock_guard lock(s_window_mutex);
    if (s_window != nullptr) ANativeWindow_release(s_window);
    s_window = nullptr;
  }
  if (MTGS::IsOpen()) {
    RunOnCpuThread(
        [](void *) {
          if (MTGS::IsOpen()) MTGS::UpdateDisplayWindow();
        },
        nullptr);
  }
}

void ApplyWindowSize(std::uint32_t width, std::uint32_t height) {
  {
    std::lock_guard lock(s_window_mutex);
    if (s_window_width == width && s_window_height == height) return;
    s_window_width = width;
    s_window_height = height;
  }
  if (MTGS::IsOpen()) {
    // CPU thread: called from the CONTROL records, which the replayer processes there.
    MTGS::UpdateDisplayWindow();
    MTGS::ResizeDisplayWindow(width, height, 1.0f);
  }
}

std::optional<WindowInfo> AcquireWindow(bool recreate_window) {
  WindowInfo info;
  std::memset(&info, 0, sizeof(info));
  std::lock_guard lock(s_window_mutex);
  if (s_acquired_window != nullptr) {
    ANativeWindow_release(s_acquired_window);
    s_acquired_window = nullptr;
  }
  if (s_window == nullptr) {
    // Surfaceless: GSDevice handles it by skipping swap-chain creation, and the next attach triggers UpdateDisplayWindow.
    info.type = WindowInfo::Type::Surfaceless;
    return info;
  }
  ANativeWindow_acquire(s_window);
  s_acquired_window = s_window;
  info.type = WindowInfo::Type::Android;
  info.surface_width = s_window_width;
  info.surface_height = s_window_height;
  info.surface_scale = 1.0f;
  info.surface_refresh_rate = s_window_refresh_rate;
  info.window_handle = s_window;
  return info;
}

void ReleaseWindow() {
  std::lock_guard lock(s_window_mutex);
  if (s_acquired_window != nullptr) {
    ANativeWindow_release(s_acquired_window);
    s_acquired_window = nullptr;
  }
}

void RunOnCpuThread(void (*task)(void *), void *argument) {
  if (task == nullptr) return;
  {
    std::lock_guard lock(s_tasks_mutex);
    s_tasks.push_back(HostTask{task, argument});
  }
}

void DrainQueuedTasks() {
  std::vector<HostTask> tasks;
  {
    std::lock_guard lock(s_tasks_mutex);
    tasks.swap(s_tasks);
  }
  for (const HostTask &task : tasks) task.function(task.argument);
}

bool Running() { return s_booted.load(); }

const char *DataRoot() { return s_data_root.c_str(); }

}  // namespace gt4gs::host

// --- Host --------------------------------------------------------------------------------------------
void Host::CommitBaseSettingChanges() {}

void Host::LoadSettings(SettingsInterface &si, std::unique_lock<std::mutex> &lock) {}

void Host::CheckForSettingsChanges(const Pcsx2Config &old_config) {}

bool Host::RequestResetSettings(bool folders, bool core, bool controllers, bool hotkeys, bool ui) { return false; }

void Host::SetDefaultUISettings(SettingsInterface &si) {}

bool Host::LocaleCircleConfirm() { return false; }

std::unique_ptr<ProgressCallback> Host::CreateHostProgressCallback() {
  return ProgressCallback::CreateNullProgressCallback();
}

void Host::ReportInfoAsync(const std::string_view title, const std::string_view message) {
  if (!message.empty()) LogLine("%s: %s", std::string(title).c_str(), std::string(message).c_str());
}

void Host::ReportErrorAsync(const std::string_view title, const std::string_view message) {
  if (!message.empty()) LogLine("ERROR %s: %s", std::string(title).c_str(), std::string(message).c_str());
}

void Host::OpenURL(const std::string_view url) {}

bool Host::CopyTextToClipboard(const std::string_view text) { return false; }

std::string Host::GetTextFromClipboard() { return std::string(); }

void Host::BeginTextInput() {}

void Host::EndTextInput() {}

std::optional<WindowInfo> Host::GetTopLevelWindowInfo() { return gt4gs::host::AcquireWindow(false); }

void Host::OnInputDeviceConnected(const std::string_view identifier, const std::string_view device_name) {}

void Host::OnInputDeviceDisconnected(const InputBindingKey key, const std::string_view identifier) {}

void Host::SetMouseMode(bool relative_mode, bool hide_cursor) {}

void Host::SetMouseLock(bool state) {}

std::optional<WindowInfo> Host::AcquireRenderWindow(bool recreate_window) {
  return gt4gs::host::AcquireWindow(recreate_window);
}

void Host::ReleaseRenderWindow() { gt4gs::host::ReleaseWindow(); }

void Host::BeginPresentFrame() {}

void Host::RequestResizeHostDisplay(s32 width, s32 height) {}

void Host::OnVMStarting() {}

void Host::OnVMStarted() {}

void Host::OnVMDestroyed() {}

void Host::OnVMPaused() {}

void Host::OnVMResumed() {}

void Host::OnGameChanged(const std::string &title, const std::string &elf_override, const std::string &disc_path,
                         const std::string &disc_serial, u32 disc_crc, u32 current_crc) {
  LogLine("game: title '%s', serial '%s'", title.c_str(), disc_serial.c_str());
}

void Host::OnPerformanceMetricsUpdated() {}

void Host::OnSaveStateLoading(const std::string_view filename) {}

void Host::OnSaveStateLoaded(const std::string_view filename, bool was_successful) {}

void Host::OnSaveStateSaved(const std::string_view filename) {}

void Host::RunOnCPUThread(std::function<void()> function, bool block) {
  // std::function cannot cross the C-style task queue; the frontend is the only caller and it uses a lambda with no
  // captures for the window refresh, so a heap copy with a trampoline is used instead.
  struct Task {
    std::function<void()> function;
    static void Call(void *argument) { static_cast<Task *>(argument)->function(); }
  };
  auto *task = new Task{std::move(function)};
  gt4gs::host::RunOnCpuThread(&Task::Call, task);
}

void Host::RefreshGameListAsync(bool invalidate_cache) {}

void Host::CancelGameListRefresh() {}

bool Host::IsFullscreen() { return true; }

void Host::SetFullscreen(bool enabled) {}

void Host::RequestExitApplication(bool allow_confirm) {}

void Host::RequestExitBigPicture() {}

void Host::RequestVMShutdown(bool allow_confirm, bool allow_save_state, bool default_save_state) {
  VMManager::SetState(VMState::Stopping);
}

void Host::OnAchievementsLoginSuccess(const char *username, u32 points, u32 sc_points, u32 unread_messages) {}

void Host::OnAchievementsLoginRequested(Achievements::LoginRequestReason reason) {}

void Host::OnAchievementsHardcoreModeChanged(bool enabled) {}

void Host::OnAchievementsRefreshed() {}

bool Host::HasNativeAchievementNotifications() { return false; }

void Host::OnAchievementNotification(const char *key, float duration, const char *title, const char *message,
                                     const char *badge_path) {}

bool Host::InBatchMode() { return false; }

bool Host::InNoGUIMode() { return false; }

bool Host::ShouldPreferHostFileSelector() { return false; }

void Host::OpenHostFileSelectorAsync(std::string_view title, bool select_directory, FileSelectorCallback callback,
                                      FileSelectorFilters filters, std::string_view initial_directory) {
  callback(std::string());
}

int Host::LocaleSensitiveCompare(std::string_view lhs, std::string_view rhs) {
  const int res = std::strncmp(lhs.data(), rhs.data(), std::min(lhs.size(), rhs.size()));
  if (res != 0) return res;
  return lhs.size() > rhs.size() ? 1 : (lhs.size() < rhs.size() ? -1 : 0);
}

std::optional<u32> InputManager::ConvertHostKeyboardStringToCode(const std::string_view str) { return std::nullopt; }

std::optional<std::string> InputManager::ConvertHostKeyboardCodeToString(u32 code) { return std::nullopt; }

const char *InputManager::ConvertHostKeyboardCodeToIcon(u32 code) { return nullptr; }

BEGIN_HOTKEY_LIST(g_host_hotkeys)
END_HOTKEY_LIST()

void Host::PumpMessagesOnCPUThread() { gt4gs::host::DrainQueuedTasks(); }

s32 Host::Internal::GetTranslatedStringImpl(const std::string_view context, const std::string_view msg, char *tbuf,
                                            size_t tbuf_space) {
  if (msg.size() > tbuf_space) return -1;
  if (msg.empty()) return 0;
  std::memcpy(tbuf, msg.data(), msg.size());
  return static_cast<s32>(msg.size());
}

std::string Host::TranslatePluralToString(const char *context, const char *msg, const char *disambiguation, int count) {
  std::string ret(msg);
  const std::string replacement = std::to_string(count);
  for (;;) {
    const std::string::size_type pos = ret.find("%n");
    if (pos == std::string::npos) break;
    ret.replace(pos, 2, replacement);
  }
  return ret;
}

// --- the C ABI (gt4gs_api.h) -------------------------------------------------------------------------
extern "C" {

int gt4gs_start(const Gt4gsStartInfo *info) {
  if (s_started.load()) return 0;
  if (info == nullptr) {
    s_error_text = "gt4gs_start: no info";
    return -1;
  }
  s_data_root = info->data_root != nullptr ? info->data_root : "";
  s_config_path = info->config_path != nullptr ? info->config_path : "";
  s_overrides_path = info->overrides_path != nullptr ? info->overrides_path : "";
  s_serial = info->serial != nullptr && *info->serial != '\0' ? info->serial : "SCUS-97328";
  s_preferred_backend = info->preferred_backend;
  s_log_performance = info->log_performance != 0;
  s_error_text.clear();
  s_started.store(true);

  LogLine("%s starting; data root %s", kVersion, s_data_root.c_str());
  if (!CreateSessionFolders()) return -1;

  EmuFolders::AppRoot = s_data_root;
  EmuFolders::DataRoot = s_data_root;
  EmuFolders::Resources = Path::Combine(s_data_root, "resources");
  EmuFolders::UserResources = EmuFolders::Resources;
  if (!FileSystem::FileExists(Path::Combine(EmuFolders::Resources, "GameIndex.yaml").c_str()))
    LogLine("no GameIndex.yaml under %s: the GameDB GS fixes for %s do not apply", EmuFolders::Resources.c_str(),
            s_serial.c_str());

  Host::Internal::SetBaseSettingsLayer(&s_settings);
  VMManager::SetDefaultSettings(s_settings, true, true, true, true, true);
  ApplyBaseSettings();

  // Folders first (they create cache/, textures/ and logs/ under the data root), then the two settings files.
  EmuFolders::LoadConfig(s_settings);
  EmuFolders::EnsureFoldersExist();
  VMManager::Internal::LoadStartupSettings();

  ApplyIniFile(s_config_path.c_str(), false);
  ApplyIniFile(s_overrides_path.c_str(), false);

  // A build without Vulkan always runs the GLES back end (ARMSX2's own Android default); the setting is written so the
  // library's choice is visible in the log and in the shared settings file.
  const bool have_vulkan = gt4gs_has_vulkan() != 0;
  GSRendererType renderer = have_vulkan ? GSRendererType::VK : GSRendererType::OGL;
  switch (s_preferred_backend) {
    case 0: renderer = GSRendererType::OGL; break;
    case 1: renderer = have_vulkan ? GSRendererType::VK : GSRendererType::OGL; break;
    case 2: renderer = GSRendererType::Null; break;  // deviceless: no GPU context and no picture, a diagnosis path
    default: break;
  }
  s_settings.SetIntValue("EmuCore/GS", "Renderer", static_cast<int>(renderer));
  switch (renderer) {
    case GSRendererType::VK: s_renderer_name = "Vulkan"; break;
    case GSRendererType::Null: s_renderer_name = "Null (deviceless)"; break;
    default: s_renderer_name = "OpenGL ES"; break;
  }

  s_source = std::make_unique<QueueSource>(s_serial);
  SetStream(s_source.get(), s_renderer_name.c_str(), kVersion);

  gt4gs::host::SetNativeWindow(info->native_window, info->surface_width, info->surface_height);

  // The GL context is created with the game's window, never on a surfaceless one. The game starts the renderer at its
  // first GS hook and attaches the window immediately after, in the same call, so the boot is deferred to the attach.
  {
    std::lock_guard lock(s_window_mutex);
    if (s_window == nullptr) {
      LogLine("waiting for the game window before opening the GS");
      return 0;
    }
  }

  {
    std::lock_guard lock(s_boot_mutex);
    s_boot_finished = false;
    s_boot_ok = false;
    s_boot_error.clear();
  }
  {
    std::lock_guard lock(s_window_mutex);
    if (s_window != nullptr) {
      s_boot_deferred.store(true);
      s_cpu_thread = std::thread(CpuThreadMain);
    } else {
      s_boot_deferred.store(false);
    }
  }
  if (!s_boot_deferred.load()) {
    LogLine("GS set up; it opens with the game window");
    return 0;
  }
  if (!WaitForBoot()) return -1;
  LogLine("GS ready: %s, serial %s", s_renderer_name.c_str(), s_serial.c_str());
  return 0;
}

void gt4gs_attach_window(void *native_window, uint32_t width, uint32_t height) {
  if (!s_started.load()) return;
  LogLine("window %p %ux%u", native_window, width, height);
  gt4gs::host::SetNativeWindow(native_window, width, height);
}

void gt4gs_resize(uint32_t width, uint32_t height) {
  if (!s_started.load()) return;
  gt4gs::host::ApplyWindowSize(width, height);
}

void gt4gs_detach_window(void) {
  if (!s_started.load()) return;
  LogLine("window detached");
  gt4gs::host::DetachNativeWindow();
}

void gt4gs_gif(int path, const void *data, uint32_t bytes) {
  if (s_source == nullptr || data == nullptr || bytes == 0) return;
  s_source->PushGif(path, data, bytes);
}

void gt4gs_privileged(uint32_t address, uint64_t value) {
  if (s_source == nullptr) return;
  s_source->PushPrivileged(address, value);
}

void gt4gs_vsync(int odd_field) {
  if (s_source == nullptr) return;
  s_source->PushVsync(odd_field != 0);
}

int gt4gs_read_fifo(void *data, uint32_t qwords) {
  if (s_source == nullptr) {
    if (data != nullptr) std::memset(data, 0, static_cast<std::size_t>(qwords) * 16u);
    return 0;
  }
  return s_source->PushReadFifo(static_cast<std::uint8_t *>(data), qwords) ? 1 : 0;
}

void gt4gs_reset(int kind) {
  if (s_source == nullptr) return;
  s_source->PushReset(static_cast<std::uint32_t>(kind), kind == 2 ? 0xeu : 0u);
}

void gt4gs_gif_reset(void) {
  if (s_source == nullptr) return;
  s_source->PushReset(2, 0xeu);
}

void gt4gs_display_aspect(int fill, uint32_t ratio_millionths) {
  if (s_source == nullptr) return;
  (void)ratio_millionths;  // the ARMSX2 presenter fills the window at the configured aspect ratio
  gs_stream::ControlPayload control{};
  control.kind = 7;  // protocol 1.3 display_aspect
  control.args[0] = fill != 0 ? 1u : 0u;
  s_source->PushControl(control);
}

void gt4gs_finish(void) {
  if (!s_started.load() || s_finished.load()) {
    s_started.store(false);
    return;
  }
  LogLine("finishing");
  if (s_source != nullptr) s_source->PushShutdown();
  if (s_cpu_thread.joinable()) s_cpu_thread.join();
  s_started.store(false);
  LogLine("finished");
}

const char *gt4gs_last_error(void) { return s_error_text.c_str(); }

const char *gt4gs_version(void) { return kVersion; }

const char *gt4gs_renderer_name(void) { return s_renderer_name.c_str(); }

int gt4gs_has_vulkan(void) { return 0; }  // this build has the OpenGL ES back end only (no shaderc sources locally)

}  // extern "C"
