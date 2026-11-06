// SPDX-License-Identifier: MIT
// TASK-180 (Linux plan L2, Android plan A2): the SDL3 form of the game window host (window.hpp). window.cpp is the Win32
// form and the reference: the frame pacing, the frame skip notes, the input recording and replay, the pad hand-off and
// the start-up settings below follow it line for line; only the window, its events and the keyboard are SDL3's.
//
// What is left out on purpose (later stages):
//   - CoreGS 1.1 and GSdx: CoreGS 2.0 (GT4_RENDERER=hw) is the only renderer outside Windows, so the window handle given
//     to the GS seam is the platform's native surface (ANativeWindow on Android);
//   - RetroAchievements (WinHTTP and GDI+ today), the cursor auto-hide, monitor choice, fullscreen toggles (Android is
//     always fullscreen), the Windows responsiveness timer and window watch;
//   - Android lifecycle (A6): in the background the game thread waits for the foreground; a surface replaced while away
//     is not re-attached yet.
// Duplicated glue with window.cpp (pacing release, input files) is meant to move into one shared file once the Win32
// host can be tested again in a windowed run (agents do not start windowed runs without the owner).
#include "window.hpp"

#include "display_aspect.hpp"
#include "frame_clock.hpp"
#include "frame_skip.hpp"
#include "gs/gs.hpp"
#include "host_io.hpp"
#include "input_recording.hpp"
#include "pacing.hpp"
#include "perf_probe.hpp"
#include "ps2/memory.hpp"
#include "ps2/system.hpp"
#include "race_display.hpp"
#include "race_widescreen.hpp"
#include "virtual_pad.hpp"
#include "window_mode.hpp"
#ifdef GT4_GS_STREAM_CLIENT
#include "gs_stream/gs_stream_client.hpp"
#endif
#ifdef __ANDROID__
#include "android_diagnostics.hpp"
#endif

#include <SDL3/SDL.h>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
SDL_Window *game_window = nullptr;
void *native_surface = nullptr;  // ANativeWindow* on Android
bool closing = false;
bool focused = true;
std::uint64_t input_vblank = 0;  // guest VBlanks so far (input recording, pacing, diagnostics)
std::uint32_t renderer_client_width = 0, renderer_client_height = 0;
ControllerInput *controller = nullptr;
gt4::pacing::Mode pacing_mode = gt4::pacing::Mode::paced;
std::unique_ptr<FrameClock> frame_clock;
std::unique_ptr<gt4::pacing::FramePacer> frame_pacer;
gt4::pacing::Diagnostics pacing_diagnostics;
gt4::pacing::Counters counters_at_last_report;
bool force_480p_enabled = false;
gt4::display::Force480pState force_480p_state;
AudioOutput *audio_output = nullptr;
gt4::controls::ControlMap active_controls;
gt4::pad::Backend input_backend = gt4::pad::Backend::sdl;
// The keyboard bindings as SDL scancodes (the controls table names Windows virtual-key codes).
struct ScancodeBinding {
  SDL_Scancode scancode;
  SDL_Scancode alternate;  // the right-hand Shift, Ctrl or Alt; SDL_SCANCODE_UNKNOWN otherwise
  std::uint16_t button_mask;
};
std::vector<ScancodeBinding> keyboard_keys;
// Keys pressed since the last VBlank: a tap shorter than a frame (an adb key event, a quick touch keyboard) still reaches
// the game for one VBlank instead of falling between two samples of the keyboard state.
std::vector<SDL_Scancode> keys_pressed_since_vblank;
gt4::input_recording::Recorder input_recorder;
std::unique_ptr<gt4::input_recording::Player> input_player;
const std::uint64_t paced_from_vblank = [] {
  const char *text = std::getenv("GT4_PACING_FROM_VBLANK");
  return text != nullptr ? std::strtoull(text, nullptr, 10) : 0ull;
}();

// Windows virtual-key code (controls.cpp resolve_key) to the SDL scancode of the same key on a US layout.
ScancodeBinding scancode_for(std::uint8_t virtual_key, std::uint16_t mask) {
  ScancodeBinding out{SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, mask};
  if (virtual_key >= 'A' && virtual_key <= 'Z') {
    out.scancode = static_cast<SDL_Scancode>(SDL_SCANCODE_A + (virtual_key - 'A'));
  } else if (virtual_key >= '1' && virtual_key <= '9') {
    out.scancode = static_cast<SDL_Scancode>(SDL_SCANCODE_1 + (virtual_key - '1'));
  } else if (virtual_key == '0') {
    out.scancode = SDL_SCANCODE_0;
  } else if (virtual_key >= 0x70 && virtual_key <= 0x7B) {  // F1..F12
    out.scancode = static_cast<SDL_Scancode>(SDL_SCANCODE_F1 + (virtual_key - 0x70));
  } else if (virtual_key >= 0x61 && virtual_key <= 0x69) {  // numpad 1..9
    out.scancode = static_cast<SDL_Scancode>(SDL_SCANCODE_KP_1 + (virtual_key - 0x61));
  } else {
    switch (virtual_key) {
      case 0x60: out.scancode = SDL_SCANCODE_KP_0; break;
      case 0x08: out.scancode = SDL_SCANCODE_BACKSPACE; break;
      case 0x09: out.scancode = SDL_SCANCODE_TAB; break;
      case 0x0D: out.scancode = SDL_SCANCODE_RETURN; break;
      case 0x10: out.scancode = SDL_SCANCODE_LSHIFT; out.alternate = SDL_SCANCODE_RSHIFT; break;
      case 0x11: out.scancode = SDL_SCANCODE_LCTRL; out.alternate = SDL_SCANCODE_RCTRL; break;
      case 0x12: out.scancode = SDL_SCANCODE_LALT; out.alternate = SDL_SCANCODE_RALT; break;
      case 0x1B: out.scancode = SDL_SCANCODE_ESCAPE; break;
      case 0x20: out.scancode = SDL_SCANCODE_SPACE; break;
      case 0x21: out.scancode = SDL_SCANCODE_PAGEUP; break;
      case 0x22: out.scancode = SDL_SCANCODE_PAGEDOWN; break;
      case 0x23: out.scancode = SDL_SCANCODE_END; break;
      case 0x24: out.scancode = SDL_SCANCODE_HOME; break;
      case 0x25: out.scancode = SDL_SCANCODE_LEFT; break;
      case 0x26: out.scancode = SDL_SCANCODE_UP; break;
      case 0x27: out.scancode = SDL_SCANCODE_RIGHT; break;
      case 0x28: out.scancode = SDL_SCANCODE_DOWN; break;
      case 0x2D: out.scancode = SDL_SCANCODE_INSERT; break;
      case 0x2E: out.scancode = SDL_SCANCODE_DELETE; break;
      default: break;
    }
  }
  return out;
}

void apply_pad(const PadState &pad) {
  ps2::set_pad_buttons(pad.buttons);
  for (int axis = 0; axis < 4; ++axis)
    ps2::set_pad_axis(axis, pad.axes[axis]);
  for (int button = 0; button < 16; ++button)
    ps2::set_pad_pressure(button, pad.pressures[button]);
}

gt4::pacing::Counters pacing_counters() {
  gt4::pacing::Counters counters;
  if (frame_pacer)
    counters.late_frames = frame_pacer->late_frames();
  if (audio_output) {
    counters.audio_underruns = audio_output->underruns();
    counters.ring_overflow_frames = audio_output->overflow_frames();
  }
  return counters;
}

// TASK-080 presentation sink: CoreGS 2.0 gets the race-view fill through the GS stream seam (in process).
void present_race_view(bool fill, double ratio) {
#ifdef GT4_GS_STREAM_CLIENT
  if (gt4::gs_stream::stream_renderer()) {
    gt4::gs_stream::set_display_aspect_mode(fill ? gt4::gs_stream::DisplayAspectMode::fill
                                                 : gt4::gs_stream::DisplayAspectMode::game_aspect,
                                            fill ? gt4::gs_stream::aspect_millionths(ratio) : 0);
  }
#else
  (void)fill;
  (void)ratio;
#endif
}

void handle_resize(std::uint32_t width, std::uint32_t height) {
  gt4::display_aspect::set_window_client_size(width, height);
  if (native_surface == nullptr || (width == renderer_client_width && height == renderer_client_height)) return;
  std::fprintf(stderr, "[window] client size %ux%u -> %ux%u at VBlank %llu\n", renderer_client_width,
               renderer_client_height, width, height, static_cast<unsigned long long>(input_vblank));
  renderer_client_width = width;
  renderer_client_height = height;
  ps2::gs::resize_window(width, height);
}

#ifdef __ANDROID__
// TASK-180 (A6): the player paused the game (Back: the pause dialog, Minimize) or the activity leaves the foreground
// (Home, the app switcher, the notification shade, Settings, screen off). GameActivity sets these from its UI thread.
std::atomic<bool> user_paused{false};
// The activity is going to the background: its surface is destroyed after onStop, so the game thread lets go of it
// first. GameActivity.onPause waits (briefly) for `surface_released`; onResume clears the request.
std::atomic<bool> background_wanted{false};
std::mutex park_mutex;
std::condition_variable park_changed;
bool surface_released = false;  // under park_mutex

void flush_cards_before_parking();

// The game thread parks here: the EE stops (so the GS and MTVU workers run dry and sleep on their doorbells), the
// memory card files are written out, audio stops, the VBlank count stands still. In the background the swapchain and
// the surface are let go (before the window is destroyed); on return the new window's surface is attached once it has
// a real size (0x0 is not presentable). Leaves when the activity is in front and not paused, or when it closes.
void park(bool background) {
  const std::uint64_t parked_at = input_vblank;
  std::fprintf(stderr, "[window] paused (%s) at VBlank %llu, surface %p %ux%u\n",
               background ? "background" : (background_wanted.load() ? "leaving the foreground" : "pause dialog"),
               static_cast<unsigned long long>(parked_at), native_surface, renderer_client_width, renderer_client_height);
  if (controller) controller->stop();
  flush_cards_before_parking();
  if (audio_output != nullptr) audio_output->set_paused(true);
  bool released = false;
  bool sdl_background = background;
  const auto let_go_of_surface = [&] {
    if (!released) {
      gt4::gs_stream::detach_window();
      released = true;
      std::fprintf(stderr, "[window] surface %p released before the window goes away\n", native_surface);
    }
    const std::lock_guard<std::mutex> lock(park_mutex);
    surface_released = true;
    park_changed.notify_all();
  };
  SDL_Event event;
  for (;;) {
    if (sdl_background || background_wanted.load(std::memory_order_acquire)) let_go_of_surface();
    // SDL does not block on pause here (SDL_HINT_ANDROID_BLOCK_ON_PAUSE=0): this thread waits on its own, idle.
    if (SDL_WaitEventTimeout(&event, 50)) {
      if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_TERMINATING) {
        input_recorder.finish(input_vblank);
        ps2::halt("window closed");
        closing = true;
        return;
      }
      if (event.type == SDL_EVENT_WILL_ENTER_BACKGROUND || event.type == SDL_EVENT_DID_ENTER_BACKGROUND) sdl_background = true;
      if (event.type == SDL_EVENT_DID_ENTER_FOREGROUND) sdl_background = false;
      continue;
    }
    if (sdl_background || background_wanted.load(std::memory_order_acquire) || user_paused.load(std::memory_order_acquire))
      continue;
    if (!released) break;
    // Back from the background: the new window, once it has a size.
    void *window = SDL_GetPointerProperty(SDL_GetWindowProperties(game_window), SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER,
                                          nullptr);
    int pixel_width = 0, pixel_height = 0;
    SDL_GetWindowSizeInPixels(game_window, &pixel_width, &pixel_height);
    if (window == nullptr || pixel_width <= 0 || pixel_height <= 0) continue;
    native_surface = window;
    renderer_client_width = static_cast<std::uint32_t>(pixel_width);
    renderer_client_height = static_cast<std::uint32_t>(pixel_height);
    const bool attached = gt4::gs_stream::attach_window(reinterpret_cast<std::uint64_t>(native_surface),
                                                        renderer_client_width, renderer_client_height);
    std::fprintf(stderr, "[window] new surface %p %ux%u %s\n", native_surface, renderer_client_width,
                 renderer_client_height, attached ? "attached" : "not attached yet (retrying)");
    if (!attached) continue;
    released = false;
    {
      const std::lock_guard<std::mutex> lock(park_mutex);
      surface_released = false;
    }
    break;
  }
  if (audio_output != nullptr) audio_output->set_paused(false);
  // TASK-180 test3: a focus change that arrived while the game was parked is not always repeated on return, and the pad
  // reads neutral without the window's focus (the owner's "nothing works after coming back"). Ask the window.
  focused = (SDL_GetWindowFlags(game_window) & SDL_WINDOW_INPUT_FOCUS) != 0;
  std::fprintf(stderr, "[window] resumed at VBlank %llu (paused at %llu), window focus %s\n",
               static_cast<unsigned long long>(input_vblank), static_cast<unsigned long long>(parked_at),
               focused ? "yes" : "NO");
  if (frame_pacer) frame_pacer->anchor(frame_clock->now());  // no burst of frames to catch up the time away
}
#endif

// F7 starts a performance interval, F8 writes its report (to stderr: the session's game.log), F9 toggles.
void perf_key(SDL_Scancode scancode) {
#ifdef GT4_PERF_PROBE
  if (scancode == SDL_SCANCODE_F7 || scancode == SDL_SCANCODE_F8 || scancode == SDL_SCANCODE_F9) {
    if (scancode == SDL_SCANCODE_F7) {
      counters_at_last_report = pacing_counters();
      pacing_diagnostics.reset();
#ifdef __ANDROID__
      gt4::android_diag::interval_start();
#endif
    }
    const auto request = scancode == SDL_SCANCODE_F7   ? gt4::perf::Request::start
                         : scancode == SDL_SCANCODE_F8 ? gt4::perf::Request::report
                                                       : gt4::perf::Request::toggle;
    gt4::perf::Probe::shared().request(request, false);
  }
#else
  (void)scancode;
#endif
}

#ifdef __ANDROID__
// TASK-180 test3: GameActivity forwards the pad's Android key and motion events (see ControllerInput::android_key).
// They arrive on the UI thread, so they wait in this queue for the game thread's next VBlank.
struct AndroidInputEvent {
  bool motion;
  int code;
  float value;
};
std::mutex android_input_mutex;
std::vector<AndroidInputEvent> android_input_events;

void drain_android_input() {
  std::vector<AndroidInputEvent> events;
  {
    const std::lock_guard<std::mutex> lock(android_input_mutex);
    events.swap(android_input_events);
  }
  for (const AndroidInputEvent &event : events) {
    if (event.motion)
      controller->android_motion(event.code, event.value);
    else
      controller->android_key(event.code, event.value != 0.0f);
  }
}

// TASK-180 (owner, 2026-11-05): a phone has no F7/F8. GameActivity draws a small touch button while the launcher's
// performance report is on (GT4_PERF=1); a tap asks for F7 (1) or F8 (2) here, from the UI thread, and the game thread
// runs it at its next event pump, exactly as the keys. The controller never sees it.
std::atomic<int> touch_report_request{0};

// TASK-185: the on-screen virtual pad. GameActivity installs the layout and forwards touches from its UI thread; the
// game thread reads its contribution at every VBlank, so the game keeps one pad (port 1) and never learns which of the
// two produced a value. The layout is the player's file (user/virtual-pad.json) or the APK's default; the activity says
// whether the pad is shown (auto with no controller, always, or off).
gt4::vpad::Pad &virtual_pad() {
  static gt4::vpad::Pad pad;
  return pad;
}

// True when the layout in use is the player's file: the activity's log line names the right source when a broken file
// fell back to the default.
std::atomic<bool> virtual_pad_from_player{false};

// The `[vpad]` lines of the game log: presses, releases, stick moves and the layout's own notes. Written on the game
// thread, where the pad state is read, so they interleave with the other `[input]` lines.
void flush_virtual_pad_log() {
  for (const std::string &line : virtual_pad().take_log())
    std::fprintf(stderr, "[vpad] %s\n", line.c_str());
}
#endif

bool pump_messages() {
#ifdef __ANDROID__
  if (user_paused.load(std::memory_order_acquire) || background_wanted.load(std::memory_order_acquire)) park(false);
  if (closing) return false;
  if (const int request = touch_report_request.exchange(0, std::memory_order_acq_rel); request != 0) {
    std::fprintf(stderr, "[perf] %s from the touch button\n", request == 1 ? "interval started (F7)" : "report (F8)");
    perf_key(request == 1 ? SDL_SCANCODE_F7 : SDL_SCANCODE_F8);
  }
#endif
  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    switch (event.type) {
      case SDL_EVENT_QUIT:
      case SDL_EVENT_TERMINATING:
      case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        input_recorder.finish(input_vblank);
        ps2::halt("window closed");
        closing = true;
        return false;
      case SDL_EVENT_WILL_ENTER_BACKGROUND:
#ifdef __ANDROID__
        park(true);
#endif
        if (closing) return false;
        break;
      case SDL_EVENT_WINDOW_FOCUS_GAINED:
        focused = true;
        // TASK-180 test3: the pad is read as neutral without the window's focus, so the log says when it comes and goes.
        std::fprintf(stderr, "[input] window focus gained at VBlank %llu: the pad reads again\n",
                     static_cast<unsigned long long>(input_vblank));
        if (controller) controller->log_devices("window focus gained", focused);
        break;
      case SDL_EVENT_WINDOW_FOCUS_LOST:
        focused = false;
        std::fprintf(stderr, "[input] window focus lost at VBlank %llu: the pad reads NEUTRAL until the focus comes back\n",
                     static_cast<unsigned long long>(input_vblank));
        if (controller) {
          controller->stop();
          if (ps2::g_options.presses.empty() && !input_player) apply_pad({});
          controller->log_devices("window focus lost", focused);
        }
#ifdef __ANDROID__
        // TASK-185: the pause dialog (or another window) took the touches: nothing may stay held behind it.
        virtual_pad().release_all();
        flush_virtual_pad_log();
#endif
        break;
      case SDL_EVENT_JOYSTICK_ADDED:
      case SDL_EVENT_JOYSTICK_REMOVED:
        // The `[input]` block is written by ControllerInput::poll when the device list changes (host_io.cpp).
        break;
      case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        handle_resize(static_cast<std::uint32_t>(event.window.data1), static_cast<std::uint32_t>(event.window.data2));
        break;
      case SDL_EVENT_KEY_DOWN:
        if (event.key.repeat) break;
        keys_pressed_since_vblank.push_back(event.key.scancode);
        if (input_recorder.active() && event.key.scancode == SDL_SCANCODE_F9) {
          const std::uint64_t changes = input_recorder.finish(input_vblank);
          std::fprintf(stderr, "[input] recording ended by F9 at VBlank %llu, %llu changes; closing the game\n",
                       static_cast<unsigned long long>(input_vblank), static_cast<unsigned long long>(changes));
          ps2::halt("window closed");
          closing = true;
          return false;
        }
        perf_key(event.key.scancode);
        break;
      default:
        break;
    }
  }
  return !closing;
}

std::int64_t last_release = 0;
std::int64_t ee_frame_ticks = 0;  // from the last release to this VBlank, before any wait for the pacer
std::int64_t last_gs_wait = 0;

// TASK-082 (window.cpp note_frame_skip_vblank): what the frame skip decides from.
void note_frame_skip_vblank() {
  if (!gt4::frame_skip::g_enabled) return;
  gt4::frame_skip::Controller &skip = gt4::frame_skip::controller();
  const std::int64_t gs_wait = gt4::pacing::ee_gs_wait_us.load(std::memory_order_relaxed);
  const std::int32_t ee_frame = static_cast<std::int32_t>(std::max<std::int64_t>(
      ee_frame_ticks * 1000000 / static_cast<std::int64_t>(frame_clock ? frame_clock->frequency() : 1) -
          (gs_wait - last_gs_wait),
      0));
  last_gs_wait = gs_wait;
  skip.note_vblank(input_vblank, gt4::pacing::ee_slack_us, gt4::pacing::gs_late_us.load(std::memory_order_relaxed),
                   gt4::pacing::gs_cost_us.load(std::memory_order_relaxed), ee_frame);
  gt4::pacing::frame_skip_engaged.store(skip.gate_open(), std::memory_order_relaxed);
  gt4::pacing::frame_skip_recent.store(skip.skipping_recently(), std::memory_order_relaxed);
  if (input_vblank % 600 == 0) gt4::frame_skip::print_report(stderr);
}

// window.cpp release_frame: paced mode waits on the wall-clock grid, so each VBlank comes at the donor's 60 Hz rate.
void release_frame() {
  if (pacing_mode == gt4::pacing::Mode::paced) {
    if (!frame_pacer->anchored())
      frame_pacer->anchor(frame_clock->now());
    if (const std::int32_t shift = gt4::pacing::grid_shift_us.exchange(0, std::memory_order_relaxed)) {
      frame_pacer->shift(static_cast<std::int64_t>(shift) * static_cast<std::int64_t>(frame_clock->frequency()) / 1000000);
    }
    GT4_PERF_SCOPE(gt4::perf::Category::pacing_wait);
    static const bool no_wait = std::getenv("GT4_PACING_NO_WAIT") != nullptr;
    ee_frame_ticks = last_release != 0 ? frame_clock->now() - last_release : 0;
    gt4::pacing::ee_slack_us = static_cast<std::int32_t>(std::clamp<std::int64_t>(
        (frame_pacer->deadline() - frame_clock->now()) * 1000000 / static_cast<std::int64_t>(frame_clock->frequency()),
        -100000, 100000));
    static const std::int64_t min_interval = [] {
      const char *text = std::getenv("GT4_PACING_MIN_INTERVAL_US");
      const std::int64_t microseconds = text != nullptr ? std::atoll(text) : 15700;
      return microseconds * static_cast<std::int64_t>(frame_clock->frequency()) / 1000000;
    }();
    if (input_vblank + 1 < paced_from_vblank) frame_pacer->anchor(frame_clock->now());
    if (!no_wait && input_vblank + 1 >= paced_from_vblank) {
      frame_pacer->set_resync_periods(gt4::pacing::frame_skip_engaged.load(std::memory_order_relaxed) ? 12 : 1);
      const std::int64_t earliest =
          last_release != 0 && !gt4::pacing::frame_skip_recent.load(std::memory_order_relaxed) ? last_release + min_interval
                                                                                              : 0;
      frame_clock->wait_until(std::max(frame_pacer->deadline(), earliest));
    }
  }
  const std::int64_t released = frame_clock->now();
  last_release = released;
  if (pacing_mode == gt4::pacing::Mode::paced)
    frame_pacer->release(released);
  pacing_diagnostics.record_release(released, frame_clock->frequency());
}

std::uint16_t keyboard_buttons() {
  if (!focused) {
    keys_pressed_since_vblank.clear();
    return 0;
  }
  const bool *keys = SDL_GetKeyboardState(nullptr);
  const bool alt_held = keys[SDL_SCANCODE_LALT] || keys[SDL_SCANCODE_RALT];
  const auto tapped = [](SDL_Scancode scancode) {
    return scancode != SDL_SCANCODE_UNKNOWN &&
           std::find(keys_pressed_since_vblank.begin(), keys_pressed_since_vblank.end(), scancode) !=
               keys_pressed_since_vblank.end();
  };
  std::uint16_t buttons = 0;
  for (const ScancodeBinding &binding : keyboard_keys) {
    if (binding.scancode == SDL_SCANCODE_RETURN && alt_held) continue;  // Alt+Enter is not Start (window.cpp rule)
    if (keys[binding.scancode] || (binding.alternate != SDL_SCANCODE_UNKNOWN && keys[binding.alternate]) ||
        tapped(binding.scancode) || tapped(binding.alternate))
      buttons |= binding.button_mask;
  }
  keys_pressed_since_vblank.clear();
  return buttons;
}

void on_vblank() {
#ifdef GT4_PERF_PROBE
  gt4::perf::Probe::shared().frame_boundary({ps2::now(), ps2::primitives_drawn(), ps2::vu1_interpreted_calls()});
#endif
  release_frame();
  ++input_vblank;
  note_frame_skip_vblank();
  GT4_PERF_SCOPE(gt4::perf::Category::host_vblank);
  if (!pump_messages())
    return;
  if (force_480p_enabled)
    gt4::display::force_race_480p_at_vblank(ps2::g_ram, ps2::kRamSize, force_480p_state);
  const std::uint16_t buttons = keyboard_buttons();
  if (input_player) {
    apply_pad(input_player->at(input_vblank));
  } else if (ps2::g_options.presses.empty()) {
#ifdef __ANDROID__
    if (controller) drain_android_input();
    flush_virtual_pad_log();
#endif
    const PadState physical = controller->poll(focused, buttons);
#ifdef __ANDROID__
    // TASK-185: the virtual pad's buttons and sticks combine with the physical pad's (buttons OR, a stick axis takes the
    // value further from rest). The recording keeps the merged state, so a replay drives exactly what the player saw.
    const PadState pad = gt4::vpad::merge(physical, virtual_pad().contribution());
#else
    const PadState pad = physical;
#endif
    apply_pad(pad);
    input_recorder.vblank(input_vblank, pad);
    if (focused)
      controller->rumble(ps2::pad_rumble());
  }
  // TASK-180 test3: the `[input]` block is written by controller->poll (a device appeared, went away, or the first read).
  if (audio_output) {
    audio_output->pump();
    pacing_diagnostics.record_queue_frames(audio_output->queued_frames());
    if (audio_output->mode() == gt4::pacing::Mode::paced)
      pacing_diagnostics.record_ratio(audio_output->resampler_ratio());
  }
  ps2::gs::present();
}

// A lock beside the memory card, so two game processes never share it (window.cpp takes an exclusive Win32 handle).
int lock_memory_card(const std::string &card) {
  const std::string lock_path = card + ".lock";
  const int file = open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (file < 0) throw std::runtime_error("Cannot open the memory card lock " + lock_path);
  if (flock(file, LOCK_EX | LOCK_NB) != 0) {
    close(file);
    throw std::runtime_error("Cannot exclusively lock game memory card: " + lock_path);
  }
  return file;
}
}  // namespace

// IN-520: the recording and replay files; false stops the start, since a bench must not run with half its input.
bool open_input_files() {
  const char *record = std::getenv("GT4_INPUT_RECORD");
  const char *replay = std::getenv("GT4_INPUT_REPLAY");
  std::string error;
  if (record != nullptr && replay != nullptr) {
    std::fprintf(stderr, "[input] GT4_INPUT_RECORD and GT4_INPUT_REPLAY together, game not started\n");
    return false;
  }
  if (record != nullptr && !input_recorder.open(record, error)) {
    std::fprintf(stderr, "[input] %s, game not started\n", error.c_str());
    return false;
  }
  if (record != nullptr) std::fprintf(stderr, "[input] recording to %s; F9 ends it and closes the game\n", record);
  if (replay != nullptr) {
    input_player = std::make_unique<gt4::input_recording::Player>();
    if (!input_player->load(replay, error)) {
      std::fprintf(stderr, "[input] %s: %s, game not started\n", replay, error.c_str());
      return false;
    }
    std::fprintf(stderr, "[input] replaying %s: %zu changes up to VBlank %llu\n", replay, input_player->changes(),
                 static_cast<unsigned long long>(input_player->end()));
    // TASK-180 test3 (Part A): make a bench session obvious in game.log, next to the launch contract's arguments.
    std::fprintf(stderr,
                 "[bench] session: the pad comes from %s for every guest VBlank; the memory card is the one --memcard "
                 "names (a bench session runs its own copy and never the player's card)\n",
                 replay);
  }
  return true;
}

bool install_input_replay_without_window() {
  if (std::getenv("GT4_INPUT_RECORD") != nullptr) {
    std::fprintf(stderr, "[input] GT4_INPUT_RECORD needs --window, game not started\n");
    return false;
  }
  if (std::getenv("GT4_INPUT_REPLAY") == nullptr) {
    if (gt4::frame_skip::g_enabled) {
      ps2::g_options.on_vblank = [] {
        ++input_vblank;
        note_frame_skip_vblank();
      };
    }
    return true;
  }
  if (!open_input_files()) return false;
  ps2::g_options.on_vblank = [] {
    apply_pad(input_player->at(++input_vblank));
    note_frame_skip_vblank();
  };
  return true;
}

bool open_window(bool fullscreen) {
  if (!open_input_files()) return false;
  try {
    (void)gt4::window_mode::parse_start_fullscreen(std::getenv("GT4_FULLSCREEN"));  // validated as on Windows
  } catch (const std::invalid_argument &error) {
    std::fprintf(stderr, "[window] %s, game not started\n", error.what());
    return false;
  }
  try {
    active_controls = gt4::controls::load_controls_from_environment();
  } catch (const gt4::controls::ConfigError &error) {
    std::fprintf(stderr, "[controls] invalid controls file, game not started: %s\n", error.what());
    return false;
  }
  keyboard_keys.clear();
  for (const auto &binding : gt4::controls::keyboard_bindings(active_controls)) {
    const ScancodeBinding mapped = scancode_for(binding.virtual_key, binding.button_mask);
    if (mapped.scancode != SDL_SCANCODE_UNKNOWN) keyboard_keys.push_back(mapped);
  }
  gt4::display_aspect::configured_setting();
  const gt4::display_aspect::OutputSize output = gt4::display_aspect::configured_output_size();
  try {
    input_backend = gt4::pad::parse_backend(std::getenv("GT4_INPUT"));
  } catch (const std::invalid_argument &error) {
    std::fprintf(stderr, "[input] %s, game not started\n", error.what());
    return false;
  }
  std::fprintf(stderr, "[input] backend %s\n", gt4::pad::backend_name(input_backend));
  const char *controls_file = std::getenv("GT4_CONTROLS_FILE");
  std::fprintf(stderr, "[controls] %s\n", controls_file && *controls_file ? controls_file : "default bindings");
  if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
    std::fprintf(stderr, "[window] SDL_InitSubSystem(VIDEO) failed: %s, game not started\n", SDL_GetError());
    return false;
  }
  // Android shows the game fullscreen and landscape (the activity declares it); a desktop gets a 960x540 window, or
  // the output size, as on Windows.
  const int width = output.known() ? static_cast<int>(output.width) : 960;
  const int height = output.known() ? static_cast<int>(output.height) : 540;
  SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#ifdef __ANDROID__
  // Fixed landscape: never portrait, no auto-rotation (owner, 2026-11-05). SDLActivity sets the activity's orientation
  // from this hint when the window is created ("LandscapeLeft" alone = SCREEN_ORIENTATION_LANDSCAPE); without it the
  // manifest's orientation is overridden and a phone held upright showed the game letterboxed in portrait.
  SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft");
  // TASK-180 test2: the game thread parks itself (park(), with the surface let go first); SDL must not block it inside
  // its event pump while paused (GameActivity also sets SDL_ANDROID_BLOCK_ON_PAUSE=0 before SDL starts).
  SDL_SetHint(SDL_HINT_ANDROID_BLOCK_ON_PAUSE, "0");
  flags |= SDL_WINDOW_FULLSCREEN;
  (void)fullscreen;
#else
  flags |= SDL_WINDOW_RESIZABLE;
  if (fullscreen) flags |= SDL_WINDOW_FULLSCREEN;
#endif
  game_window = SDL_CreateWindow("Gran Turismo 4", width, height, flags);
  if (game_window == nullptr) {
    std::fprintf(stderr, "[window] SDL_CreateWindow failed: %s, game not started\n", SDL_GetError());
    return false;
  }
#ifdef __ANDROID__
  native_surface = SDL_GetPointerProperty(SDL_GetWindowProperties(game_window), SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER,
                                          nullptr);
#endif
  if (native_surface == nullptr) {
    std::fprintf(stderr, "[window] no native surface for the SDL window on this platform, game not started\n");
    return false;
  }
#ifdef __ANDROID__
  gt4::android_diag::start();
#endif
  int pixel_width = 0, pixel_height = 0;
  SDL_GetWindowSizeInPixels(game_window, &pixel_width, &pixel_height);
  gt4::display_aspect::set_window_client_size(static_cast<std::uint32_t>(pixel_width),
                                              static_cast<std::uint32_t>(pixel_height));
  std::fprintf(stderr, "[window] SDL3 %s window %dx%d pixels\n", SDL_GetCurrentVideoDriver(), pixel_width, pixel_height);
  return true;
}

#ifdef GT4_MEMCARD_FLUSH_OVERLAY
namespace ps2::iop::sio2 {
void flush_cards_now();  // memcard capsule sio2.cpp (scripts/prepare_memcard_flush.py)
}
#endif

#ifdef __ANDROID__
namespace {
// TASK-180: a game minimized or in the background may be killed by Android; nothing written to the card is lost.
void flush_cards_before_parking() {
#ifdef GT4_MEMCARD_FLUSH_OVERLAY
  ps2::iop::sio2::flush_cards_now();
  std::fprintf(stderr, "[window] memory card files written out before parking\n");
#endif
}
}  // namespace
#endif

int run_window(const std::function<int()> &emulate) {
  ControllerInput input(active_controls, input_backend);
  controller = &input;
  int card_lock = -1;
  const auto cleanup = [&]() {
    input_recorder.finish(input_vblank);
#ifdef GT4_PERF_PROBE
    gt4::perf::Probe::shared().set_report_section({});
    gt4::perf::Probe::shared().finish();
#endif
    audio_output = nullptr;
    frame_pacer.reset();
    frame_clock.reset();
    ps2::set_audio_sink({});
    ps2::g_options.on_vblank = {};
    input.stop();
    controller = nullptr;
    if (card_lock >= 0) close(card_lock);
    if (game_window != nullptr) SDL_DestroyWindow(game_window);
    game_window = nullptr;
    native_surface = nullptr;
  };
  try {
    force_480p_enabled = gt4::display::parse_force_480p(std::getenv("GT4_FORCE_480P"));
    force_480p_state = {};
    if (force_480p_enabled)
      std::fprintf(stderr, "[display] GT4_FORCE_480P=1: race display index held at 480p (guest VBlank)\n");
    gt4::display::widescreen_hold_enabled();
    pacing_mode = gt4::pacing::parse_mode(std::getenv("GT4_PACING"));
    frame_clock = std::make_unique<FrameClock>();
    frame_pacer = std::make_unique<gt4::pacing::FramePacer>(frame_clock->frequency(), gt4::pacing::guest_vblank_rate);
    pacing_diagnostics.reset();
    counters_at_last_report = {};
    std::fprintf(stderr, "[pacing] mode=%s; guest VBlank %llu Hz; timer=%s\n", gt4::pacing::mode_name(pacing_mode),
                 static_cast<unsigned long long>(gt4::pacing::guest_vblank_rate),
                 frame_clock->high_resolution_timer() ? "high-resolution" : "1 ms");
    card_lock = lock_memory_card(ps2::g_options.memcard_path);
    ps2::init_graphics();
    int pixel_width = 0, pixel_height = 0;
    SDL_GetWindowSizeInPixels(game_window, &pixel_width, &pixel_height);
    // Kind 3 is the GS seam's native-window kind (the Win32 HWND on Windows); here it carries the ANativeWindow.
    if (!ps2::gs::attach_window(3, nullptr, reinterpret_cast<std::uint64_t>(native_surface), nullptr, nullptr,
                                static_cast<std::uint32_t>(pixel_width), static_cast<std::uint32_t>(pixel_height)))
      throw std::runtime_error("CoreGS 2.0 refused the window (GT4_RENDERER must be hw outside Windows)");
    renderer_client_width = static_cast<std::uint32_t>(pixel_width);
    renderer_client_height = static_cast<std::uint32_t>(pixel_height);
    gt4::display_aspect::set_presentation_sink(present_race_view);
    std::fprintf(stderr, "[window] real guest GPU output through CoreGS 2.0 (SDL3 window)\n");
    ps2::g_options.on_vblank = on_vblank;
    const char *audio_choice = std::getenv("GT4_AUDIO");
    AudioOutput audio(pump_messages, pacing_mode,
                      audio_choice != nullptr && std::strcmp(audio_choice, "none") == 0 ? AudioDevice::none
                                                                                       : AudioDevice::system);
    if (audio.silent() && pacing_mode == gt4::pacing::Mode::audio) {
      pacing_mode = gt4::pacing::Mode::paced;
      std::fprintf(stderr, "[pacing] mode=paced: no audio output to pace on\n");
    }
    audio_output = &audio;
#ifdef GT4_PERF_PROBE
    gt4::perf::Probe::shared().set_report_section([](std::FILE *output) {
      const gt4::pacing::Counters session = pacing_counters();
      pacing_diagnostics.format(output, pacing_mode, session.since(counters_at_last_report), session);
      counters_at_last_report = session;
      pacing_diagnostics.reset();
#ifdef __ANDROID__
      gt4::android_diag::report(output);
#endif
    });
#endif
    ps2::set_audio_sink([&audio](const std::int16_t *samples, std::size_t frames) {
      if (!audio.push(samples, frames))
        ps2::halt("window closed");
    });
    int result = 1;
    try {
      result = emulate();
    } catch (...) {
      ps2::set_audio_sink({});
      throw;
    }
#ifdef GT4_MEMCARD_FLUSH_OVERLAY
    ps2::iop::sio2::flush_cards_now();
#endif
    ps2::set_audio_sink({});
    audio.close(!closing && result == 0);
    audio_output = nullptr;
    cleanup();
    return result;
  } catch (const std::exception &error) {
    cleanup();
    std::fprintf(stderr, "[host] audio/input/window failure: %s\n", error.what());
    return 1;
  }
}

#ifdef __ANDROID__
#include <jni.h>
// GameActivity.nativeReportRequest(1 = start an interval, 2 = write the report). Any thread.
extern "C" JNIEXPORT void JNICALL Java_io_github_moesuito_gt4recomp_GameActivity_nativeReportRequest(JNIEnv *, jclass,
                                                                                                       jint request) {
  if (request == 1 || request == 2) touch_report_request.store(request, std::memory_order_release);
}

// GameActivity.onPause / onResume: the activity leaves or regains the foreground. Leaving waits (at most `timeout_ms`)
// until the game thread has let go of the surface, so the swapchain is gone before the window is destroyed.
extern "C" JNIEXPORT jboolean JNICALL Java_io_github_moesuito_gt4recomp_GameActivity_nativeEnterBackground(JNIEnv *, jclass,
                                                                                                         jint timeout_ms) {
  user_paused.store(true, std::memory_order_release);
  background_wanted.store(true, std::memory_order_release);
  std::unique_lock<std::mutex> lock(park_mutex);
  return park_changed.wait_for(lock, std::chrono::milliseconds(timeout_ms), [] { return surface_released; }) ? JNI_TRUE
                                                                                                            : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL Java_io_github_moesuito_gt4recomp_GameActivity_nativeLeaveBackground(JNIEnv *, jclass) {
  background_wanted.store(false, std::memory_order_release);
}

// GameActivity.dispatchKeyEvent: a pad key Android reports for a device SDL did not enumerate as a gamepad. `down` is
// the action; `keycode` is KeyEvent.KEYCODE_*. Any thread.
extern "C" JNIEXPORT void JNICALL Java_io_github_moesuito_gt4recomp_GameActivity_nativePadKey(JNIEnv *, jclass,
                                                                                           jint keycode, jboolean down) {
  const std::lock_guard<std::mutex> lock(android_input_mutex);
  if (android_input_events.size() < 256)
    android_input_events.push_back({false, keycode, down == JNI_TRUE ? 1.0f : 0.0f});
}

// GameActivity.dispatchGenericMotionEvent: a pad axis (MotionEvent.AXIS_*), -1..1 (triggers 0..1). Any thread.
extern "C" JNIEXPORT void JNICALL Java_io_github_moesuito_gt4recomp_GameActivity_nativePadMotion(JNIEnv *, jclass,
                                                                                               jint axis, jfloat value) {
  const std::lock_guard<std::mutex> lock(android_input_mutex);
  if (android_input_events.size() < 256)
    android_input_events.push_back({true, axis, value});
}

// GameActivity.nativeSetPaused: true parks the game at its next event pump (the pause dialog, Minimize, onPause);
// false lets it go on once the activity is in the foreground again. Any thread.
extern "C" JNIEXPORT void JNICALL Java_io_github_moesuito_gt4recomp_GameActivity_nativeSetPaused(JNIEnv *, jclass,
                                                                                                   jboolean paused) {
  user_paused.store(paused == JNI_TRUE, std::memory_order_release);
}

// TASK-185: the activity hands over both layout texts (the player's file, null when there is none, and the APK's default)
// and gets the lines to log. The pad is re-read at every game start, and this process is one game start.
extern "C" JNIEXPORT jstring JNICALL Java_io_github_moesuito_gt4recomp_GameActivity_nativeVirtualPadLoad(
    JNIEnv *environment, jclass, jstring player_text, jstring default_text) {
  const auto read = [environment](jstring value) -> std::optional<std::string> {
    if (value == nullptr)
      return std::nullopt;
    const char *characters = environment->GetStringUTFChars(value, nullptr);
    if (characters == nullptr)
      return std::nullopt;
    std::string copy(characters);
    environment->ReleaseStringUTFChars(value, characters);
    return copy;
  };
  const std::optional<std::string> player = read(player_text);
  const std::optional<std::string> fallback = read(default_text);
  std::string report;
  try {
    if (!fallback.has_value())
      throw gt4::vpad::LayoutError("the default layout asset is missing from the APK");
    const gt4::vpad::LoadResult loaded = gt4::vpad::load_layout(player.has_value() ? &*player : nullptr, *fallback);
    virtual_pad().set_layout(loaded.layout, loaded.notes);
    virtual_pad_from_player.store(loaded.from_player, std::memory_order_release);
    for (const std::string &line : loaded.notes)
      report += line + "\n";
  } catch (const std::exception &error) {
    // No usable layout at all: the pad stays inactive and every touch is ignored, which the activity points out.
    virtual_pad().set_layout({}, {});
    virtual_pad_from_player.store(false, std::memory_order_release);
    report = std::string("no usable layout, the virtual pad is off: ") + error.what() + "\n";
  }
  return environment->NewStringUTF(report.c_str());
}

// TASK-185: true when the layout in use is the player's file, false for the built-in default (including a broken file
// that fell back to it): the activity's log line then names the right source.
extern "C" JNIEXPORT jboolean JNICALL
Java_io_github_moesuito_gt4recomp_GameActivity_nativeVirtualPadFromPlayer(JNIEnv *, jclass) {
  return virtual_pad_from_player.load(std::memory_order_acquire) ? JNI_TRUE : JNI_FALSE;
}

// TASK-185: what the activity draws: 3 floats (element count, global opacity, haptics) then 8 per element (type, bind, x,
// y, width, height, opacity, visible), exactly gt4::vpad::Pad::geometry.
extern "C" JNIEXPORT jfloatArray JNICALL
Java_io_github_moesuito_gt4recomp_GameActivity_nativeVirtualPadGeometry(JNIEnv *environment, jclass) {
  const std::vector<float> geometry = virtual_pad().geometry();
  jfloatArray out = environment->NewFloatArray(static_cast<jsize>(geometry.size()));
  if (out != nullptr)
    environment->SetFloatArrayRegion(out, 0, static_cast<jsize>(geometry.size()), geometry.data());
  return out;
}

// TASK-185: the bind name of each element, in layout order ("cross", "left_stick", "dpad", ...): the activity picks the
// PlayStation symbol or the button's label from it.
extern "C" JNIEXPORT jobjectArray JNICALL
Java_io_github_moesuito_gt4recomp_GameActivity_nativeVirtualPadBinds(JNIEnv *environment, jclass) {
  const gt4::vpad::Layout layout = virtual_pad().layout();
  jclass string_class = environment->FindClass("java/lang/String");
  if (string_class == nullptr)
    return nullptr;
  jobjectArray out = environment->NewObjectArray(static_cast<jsize>(layout.elements.size()), string_class, nullptr);
  if (out == nullptr)
    return nullptr;
  for (std::size_t index = 0; index < layout.elements.size(); ++index) {
    const gt4::vpad::LayoutElement &element = layout.elements[index];
    jstring name = environment->NewStringUTF(gt4::vpad::bind_name(element.type, element.bind));
    environment->SetObjectArrayElement(out, static_cast<jsize>(index), name);
    environment->DeleteLocalRef(name);
  }
  return out;
}

// TASK-185: one touch on one element. dx and dy are in -1..1 with the screen's sign (a stick reads them from where the
// thumb landed, a d-pad from its centre); down false releases it. Any thread.
extern "C" JNIEXPORT void JNICALL Java_io_github_moesuito_gt4recomp_GameActivity_nativeVirtualPadTouch(
    JNIEnv *, jclass, jint element, jfloat dx, jfloat dy, jboolean down) {
  if (element < 0)
    return;
  virtual_pad().touch(static_cast<std::size_t>(element), dx, dy, down == JNI_TRUE);
}

// TASK-185: the pad is shown or hidden. `reason` is the line for the game log ("shown: no controller",
// "hidden: Xbox Wireless Controller connected"), empty for none. Hidden means no input at all, never a stuck button.
extern "C" JNIEXPORT void JNICALL Java_io_github_moesuito_gt4recomp_GameActivity_nativeVirtualPadActive(
    JNIEnv *environment, jclass, jboolean active, jstring reason) {
  virtual_pad().set_active(active == JNI_TRUE);
  if (reason != nullptr) {
    const char *characters = environment->GetStringUTFChars(reason, nullptr);
    if (characters != nullptr) {
      virtual_pad().note(std::string(characters));
      environment->ReleaseStringUTFChars(reason, characters);
    }
  }
  // The activity's own lines (a controller appeared or went away) are worth showing at once, not at the next VBlank.
  flush_virtual_pad_log();
}
#endif
