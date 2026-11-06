#include "standalone.hpp"

#include "pad_mapping.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>

namespace gt4::standalone {
namespace {

std::string lower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

std::string trim(std::string_view text) {
  std::size_t begin = 0, end = text.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
  return std::string(text.substr(begin, end - begin));
}

bool starts_with(std::string_view text, std::string_view prefix) {
  return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool is_set(const Access &access, const std::string &name) {
  if (!access.environment) return false;
  const std::optional<std::string> value = access.environment(name);
  return value && !value->empty();
}

std::string value_of(const Access &access, const std::string &name) {
  if (!access.environment) return {};
  return access.environment(name).value_or(std::string());
}

// ---- A very small reader for the receipt JSON (written by serde_json) ----------------------------------------------------

// Position just after the string that starts at `pos` (text[pos] == '"'); -1 when it is not terminated.
std::ptrdiff_t skip_string(std::string_view text, std::size_t pos) {
  for (std::size_t i = pos + 1; i < text.size(); ++i) {
    if (text[i] == '\\') ++i;
    else if (text[i] == '"') return static_cast<std::ptrdiff_t>(i + 1);
  }
  return -1;
}

// The text between the braces of the object that is the value of top-level-or-nested `"key"`. The key is looked up outside
// of strings only, so a path with the same text in a value is not taken for it.
std::optional<std::string_view> object_slice(std::string_view text, std::string_view key) {
  const std::string quoted = "\"" + std::string(key) + "\"";
  std::size_t i = 0;
  while (i < text.size()) {
    if (text[i] == '"') {
      const std::ptrdiff_t end = skip_string(text, i);
      if (end < 0) return std::nullopt;
      const std::size_t next = static_cast<std::size_t>(end);
      if (text.substr(i, next - i) == quoted) {
        std::size_t j = next;
        while (j < text.size() && std::isspace(static_cast<unsigned char>(text[j]))) ++j;
        if (j < text.size() && text[j] == ':') {
          ++j;
          while (j < text.size() && std::isspace(static_cast<unsigned char>(text[j]))) ++j;
          if (j < text.size() && text[j] == '{') {
            // The matching brace, skipping strings.
            int depth = 0;
            for (std::size_t k = j; k < text.size(); ++k) {
              if (text[k] == '"') {
                const std::ptrdiff_t string_end = skip_string(text, k);
                if (string_end < 0) return std::nullopt;
                k = static_cast<std::size_t>(string_end) - 1;
              } else if (text[k] == '{') {
                ++depth;
              } else if (text[k] == '}') {
                if (--depth == 0) return text.substr(j + 1, k - j - 1);
              }
            }
            return std::nullopt;
          }
        }
      }
      i = next;
    } else {
      ++i;
    }
  }
  return std::nullopt;
}

// The raw value text of `"key"` inside an object slice (no nested objects expected): a number's digits, or a string's
// unescaped content.
std::optional<std::string> field_value(std::string_view slice, std::string_view key, bool want_string) {
  const std::string quoted = "\"" + std::string(key) + "\"";
  std::size_t i = 0;
  while (i < slice.size()) {
    if (slice[i] != '"') {
      ++i;
      continue;
    }
    const std::ptrdiff_t end = skip_string(slice, i);
    if (end < 0) return std::nullopt;
    const std::size_t next = static_cast<std::size_t>(end);
    if (slice.substr(i, next - i) == quoted) {
      std::size_t j = next;
      while (j < slice.size() && std::isspace(static_cast<unsigned char>(slice[j]))) ++j;
      if (j < slice.size() && slice[j] == ':') {
        ++j;
        while (j < slice.size() && std::isspace(static_cast<unsigned char>(slice[j]))) ++j;
        if (want_string) {
          if (j >= slice.size() || slice[j] != '"') return std::nullopt;
          const std::ptrdiff_t value_end = skip_string(slice, j);
          if (value_end < 0) return std::nullopt;
          std::string out;
          for (std::size_t k = j + 1; k + 1 < static_cast<std::size_t>(value_end); ++k) {
            if (slice[k] == '\\') {
              ++k;
              switch (slice[k]) {
              case '\\': out += '\\'; break;
              case '"': out += '"'; break;
              case '/': out += '/'; break;
              default: return std::nullopt; // an escape a plain file name never has
              }
            } else {
              out += slice[k];
            }
          }
          return out;
        }
        std::size_t k = j;
        while (k < slice.size() && std::isdigit(static_cast<unsigned char>(slice[k]))) ++k;
        if (k == j) return std::nullopt;
        return std::string(slice.substr(j, k - j));
      }
    }
    i = next;
  }
  return std::nullopt;
}

std::optional<std::uint64_t> parse_u64(const std::string &text) {
  std::uint64_t value = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc() || result.ptr != text.data() + text.size()) return std::nullopt;
  return value;
}

bool plain_file_name(const std::string &name) {
  if (name.empty() || name == "." || name == "..") return false;
  return name.find_first_of("\\/:") == std::string::npos;
}

// "1.2.3": three groups of digits.
bool is_version(const std::string &text) {
  int groups = 0;
  std::size_t digits = 0;
  for (char c : text) {
    if (std::isdigit(static_cast<unsigned char>(c))) {
      ++digits;
    } else if (c == '.' && digits > 0) {
      ++groups;
      digits = 0;
    } else {
      return false;
    }
  }
  return groups == 2 && digits > 0;
}

bool is_sha256(const std::string &text) {
  return text.size() == 64 && std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}

Check fail(Problem problem, std::string detail) { return Check{problem, std::move(detail)}; }

} // namespace

std::optional<GameId> game_for_executable(std::string_view file_name) {
  const std::string name = lower(file_name);
  // The names of the build, and of a release (scripts/make_distribution.py): a direct start of a release was refused without them.
  if (name == "gt4vulkan.exe" || name == "gt4.exe") return GameId{"gt4", "SCUS_973.28"};
  if (name == "gt4vulkan-spec2.exe" || name == "gt4-spec2.exe") return GameId{"spec2", "SCUS_974.36"};
  return std::nullopt;
}

bool is_standalone_start(int argc, const char *const *argv) {
  if (argc <= 1) return true;
  const char *first = argv[1];
  if (first == nullptr) return true;
  if (std::strcmp(first, "--portable-check") == 0) return false;
  return std::strncmp(first, "--", 2) == 0;
}

std::optional<Layout> find_layout(const std::filesystem::path &executable, const std::filesystem::path &local_app_data,
                                  const std::filesystem::path &app_data, const Access &access, Check &why) {
  why = Check{};
  const std::optional<GameId> game = game_for_executable(executable.filename().string());
  const std::filesystem::path game_dir = executable.parent_path();
  if (!game || lower(game_dir.filename().string()) != "game" || game_dir.parent_path().empty()) {
    why = fail(Problem::NotInstalled, executable.string());
    return std::nullopt;
  }
  Layout layout;
  layout.game = *game;
  layout.executable = executable;
  layout.install_root = game_dir.parent_path();

  // The data folders in the launcher's order: beside the program, then the write fallback of a read-only copy.
  std::vector<std::filesystem::path> roots{layout.install_root / "data"};
  if (!local_app_data.empty()) roots.push_back(local_app_data / "GT4Recomp");
  const std::filesystem::path relative = std::filesystem::path("games") / game->id;
  layout.data_root = roots.front();
  bool found = false;
  for (const auto &root : roots) {
    if (access.stat_file && access.stat_file(root / relative / "prepared" / "prepare.json")) {
      layout.data_root = root;
      found = true;
      break;
    }
  }
  if (!found) {
    for (const auto &root : roots) {
      if (access.is_directory && access.is_directory(root / relative / "prepared")) {
        layout.data_root = root;
        break;
      }
    }
  }
  layout.game_root = layout.data_root / relative;
  layout.prepared = layout.game_root / "prepared";
  layout.disc_folder = layout.game_root / "disc";
  layout.card = layout.game_root / "runtime" / "card.ps2";
  layout.sessions = layout.data_root / "sessions";
  // The renderer: the first data folder that has a gs\current.txt (DataDirs::renderer_root).
  layout.gs_root = layout.data_root / "gs";
  for (const auto &root : roots) {
    if (access.stat_file && access.stat_file(root / "gs" / "current.txt")) {
      layout.gs_root = root / "gs";
      break;
    }
  }
  // The user folder: beside the program when it has a `user` folder (portable build), else the roaming profile.
  if (access.is_directory && access.is_directory(layout.install_root / "user")) layout.user_root = layout.install_root / "user";
  else if (!app_data.empty()) layout.user_root = app_data / "GT4Recomp";
  else layout.user_root = layout.install_root / "user";
  layout.config_dir = layout.user_root / "config";
  return layout;
}

std::optional<Receipt> parse_receipt(std::string_view json) {
  const std::optional<std::string_view> image = object_slice(json, "image");
  const std::optional<std::string_view> elf = object_slice(json, "elf");
  if (!image || !elf) return std::nullopt;
  const std::optional<std::string> file = field_value(*image, "file", true);
  const std::optional<std::string> image_size = field_value(*image, "size_bytes", false);
  const std::optional<std::string> image_modified = field_value(*image, "modified_unix_nanos", false);
  const std::optional<std::string> elf_size = field_value(*elf, "size_bytes", false);
  if (!file || !image_size || !image_modified || !elf_size) return std::nullopt;
  const auto size = parse_u64(*image_size);
  const auto modified = parse_u64(*image_modified);
  const auto elf_bytes = parse_u64(*elf_size);
  if (!size || !modified || !elf_bytes) return std::nullopt;
  Receipt receipt;
  receipt.image_file = *file;
  receipt.image_linked_path = field_value(*image, "linked_path", true).value_or("");
  receipt.image_size = *size;
  receipt.image_modified_unix_nanos = *modified;
  receipt.elf_size = *elf_bytes;
  return receipt;
}

Check check_game_files(const Layout &layout, const Access &access, std::filesystem::path &disc, std::filesystem::path &elf) {
  const std::filesystem::path receipt_path = layout.prepared / "prepare.json";
  const std::optional<std::string> text = access.read_text ? access.read_text(receipt_path) : std::nullopt;
  if (!text) return fail(Problem::NoGameFiles, receipt_path.string());
  const std::optional<Receipt> receipt = parse_receipt(*text);
  if (!receipt) return fail(Problem::GameFilesChanged, "the setup record cannot be read");
  elf = layout.prepared / layout.game.elf_name;
  const std::optional<FileInfo> elf_info = access.stat_file ? access.stat_file(elf) : std::nullopt;
  if (!elf_info) return fail(Problem::GameFilesChanged, std::string(layout.game.elf_name) + " is missing");
  if (elf_info->size_bytes != receipt->elf_size)
    return fail(Problem::GameFilesChanged, std::string(layout.game.elf_name) + " has changed");
  if (!receipt->image_linked_path.empty()) {
    // Decision 0075: the image stays where the player keeps it.
    const std::string &linked = receipt->image_linked_path;
    disc = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(linked.data()), linked.size()));
    if (disc.is_relative()) disc = layout.install_root / disc;
  } else {
    if (!plain_file_name(receipt->image_file)) return fail(Problem::GameFilesChanged, "the setup record names an invalid disc copy");
    disc = layout.disc_folder / receipt->image_file;
  }
  const std::optional<FileInfo> disc_info = access.stat_file ? access.stat_file(disc) : std::nullopt;
  if (!disc_info) return fail(Problem::GameFilesChanged, receipt->image_file + " is missing");
  // A linked image may have been carried to a file system that rounds the time (launcher: LINKED_TIME_TOLERANCE_NANOS).
  const std::uint64_t tolerance = receipt->image_linked_path.empty() ? 0 : 2000000000ull;
  const std::uint64_t recorded = receipt->image_modified_unix_nanos, found = disc_info->modified_unix_nanos;
  if (disc_info->size_bytes != receipt->image_size || (found > recorded ? found - recorded : recorded - found) > tolerance)
    return fail(Problem::GameFilesChanged, receipt->image_file + " has changed");
  if (!receipt->image_linked_path.empty() && access.openable_path) disc = access.openable_path(disc);
  return Check{};
}

Check find_renderer(const Layout &layout, const Access &access, Renderer &renderer) {
  const std::optional<std::string> current = access.read_text ? access.read_text(layout.gs_root / "current.txt") : std::nullopt;
  if (!current) return fail(Problem::NoRenderer, (layout.gs_root / "current.txt").string());
  const std::string version = trim(*current);
  if (!is_version(version)) return fail(Problem::NoRenderer, "current.txt names no version");
  const std::filesystem::path folder = layout.gs_root / version;
  const std::filesystem::path executable = folder / "gt4-gs.exe";
  if (!access.stat_file || !access.stat_file(executable)) return fail(Problem::NoRenderer, executable.string() + " is missing");
  const std::optional<std::string> recorded_text = access.read_text(folder / "sha256.txt");
  if (!recorded_text) return fail(Problem::NoRenderer, (folder / "sha256.txt").string() + " is missing");
  const std::string recorded = lower(trim(*recorded_text));
  if (!is_sha256(recorded)) return fail(Problem::NoRenderer, "sha256.txt holds no hash");
  const std::optional<std::string> actual = access.sha256_file ? access.sha256_file(executable) : std::nullopt;
  if (!actual) return fail(Problem::RendererChanged, "gt4-gs.exe cannot be read");
  if (lower(*actual) != recorded) return fail(Problem::RendererChanged, "gt4-gs.exe does not match its recorded hash");
  renderer.executable = executable;
  renderer.recorded_sha256 = recorded;
  return Check{};
}

Config parse_config(std::string_view text) {
  Config config;
  std::size_t position = 0;
  while (position <= text.size()) {
    std::size_t end = text.find('\n', position);
    if (end == std::string_view::npos) end = text.size();
    const std::string line = trim(text.substr(position, end - position));
    position = end + 1;
    if (line.empty() || line[0] == '#') continue;
    const std::size_t equals = line.find('=');
    if (equals == std::string::npos) {
      config.warnings.push_back("game.ini: ignored a line without '=': " + line);
      continue;
    }
    const std::string key = lower(trim(std::string_view(line).substr(0, equals)));
    const std::string value = trim(std::string_view(line).substr(equals + 1));
    bool valid = false;
    if (key == "version") {
      continue;
    } else if (key == "fullscreen" || key == "async_gs" || key == "performance_report") {
      valid = value == "0" || value == "1";
    } else if (key == "present_mode") {
      valid = value == "fifo" || value == "mailbox" || value == "immediate";
    } else if (key == "pad") {
      gt4::pad::PadChoice choice;  // TASK-148: the controller chosen in the launcher
      valid = gt4::pad::parse_pad_choice(value.c_str(), choice);
    } else if (key == "monitor") {
      valid = value.size() >= 5 && value.size() <= 31 && starts_with(value, "\\\\.\\") &&
              std::all_of(value.begin(), value.end(), [](unsigned char c) { return c > 0x20 && c < 0x7F; });
    } else if (key == "output_size") {
      const std::size_t x = value.find('x');
      valid = x != std::string::npos && x >= 3 && x <= 5 && value.size() - x - 1 >= 3 && value.size() - x - 1 <= 5 &&
              std::all_of(value.begin(), value.begin() + static_cast<std::ptrdiff_t>(x), [](unsigned char c) { return std::isdigit(c) != 0; }) &&
              std::all_of(value.begin() + static_cast<std::ptrdiff_t>(x) + 1, value.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
    } else if (key == "check_vu_code") {
      valid = value == "0" || value == "100" || value == "1000";
    } else if (key == "renderer") {
      valid = value == "coregs20";  // TASK-027: GSdx, the default, is not written
    } else if (key == "hw_scale") {
      valid = (value.size() == 1 && value[0] >= '1' && value[0] <= '9') || value == "10" || value == "11" || value == "12";
    } else if (key == "hw_performance") {
      valid = value == "1";
    } else if (key == "hw_effect_flares" || key == "hw_effect_road_sheen") {
      valid = value == "0" || value == "1";  // TASK-186: the effect toggles (1 = drawn, the launcher's default)
    } else if (key == "hw_frame_skip") {
      valid = value == "2";  // the 30 FPS mode: one frame drawn in two
    } else if (key == "hw_frame_skip_adaptive") {
      valid = value == "1";  // TASK-146: the 60 FPS mode leaves frames out in a race when the PC falls behind
    } else if (key == "camera_360") {
      valid = value == "0" || value == "1";  // TASK-159: the 360-degree chase camera on the right stick (absent = 1)
    } else if (key == "fov_adjust") {
      valid = value == "0" || value == "1";  // TASK-160: the field of view adjustment on R3 + D-pad (absent = 1)
    } else if (key == "trigger_full_range") {
      valid = value == "0" || value == "1";  // TASK-161: the full trigger range (absent = 0, the launcher's toggle)
    } else if (key == "hw_hud") {
      valid = value == "1" || value == "fps";  // TASK-135, TASK-175: the performance HUD, the full panel or the frame rate alone
    } else if (key == "hw_present_shared_queue") {
      valid = value == "0";  // TASK-174: the launcher writes it only when the one-queue presentation path is off
    } else if (key == "hw_postfx") {
      valid = value == "fxaa";
    } else if (key == "hw_present_mode") {
      valid = value == "fifo";  // CoreGS 2.0 draws a stored "Fast" vsync as On
    } else if (key == "hw_sharpening") {
      valid = value == "0" || value == "0.2" || value == "2";
    } else if (key == "hw_textures" || key == "hw_adapter") {
      valid = !value.empty();  // a folder or a card name: any text the launcher wrote
    } else if (key == "wheel_file") {
      valid = !value.empty();  // TASK-181: the launcher's wheel.toml (the Wheel page), as GT4_WHEEL_FILE at Play
    } else {
      config.warnings.push_back("game.ini: unknown key '" + key + "' ignored");
      continue;
    }
    if (!valid) {
      config.warnings.push_back("game.ini: refused the value of '" + key + "': " + value);
      continue;
    }
    config.values[key] = value;
  }
  return config;
}

// TASK-117: a direct start with Performance mode on gets the flare limits too. At Play the launcher sets
// GT4_FLARE_LIMITS; here the gt4-gs.ini of the config folder (the same file the game hands to the GS process) says
// whether Performance mode is on: HWDownloadMode = 2 in [EmuCore/GS] (docs/launcher/gs-settings.toml). Only GT4 v2.00
// has the derived ELF with the helpers; Spec II's ELF has no such code.
bool gs_ini_performance_mode(const Access &access, const std::filesystem::path &ini) {
  const std::optional<std::string> text = access.read_text ? access.read_text(ini) : std::nullopt;
  if (!text) return false;
  bool section = false;
  std::size_t position = 0;
  while (position <= text->size()) {
    std::size_t end = text->find('\n', position);
    if (end == std::string::npos) end = text->size();
    const std::string line = trim(std::string_view(*text).substr(position, end - position));
    position = end + 1;
    if (line.empty() || line[0] == ';' || line[0] == '#') continue;
    if (line[0] == '[') {
      section = line == "[EmuCore/GS]";
      continue;
    }
    if (!section) continue;
    const std::size_t equals = line.find('=');
    if (equals == std::string::npos) continue;
    if (trim(std::string_view(line).substr(0, equals)) != "HWDownloadMode") continue;
    return trim(std::string_view(line).substr(equals + 1)) == "2";
  }
  return false;
}

Environment config_environment(const Config &config) {  Environment out;
  const auto get = [&](const char *key) -> const std::string * {
    const auto it = config.values.find(key);
    return it == config.values.end() ? nullptr : &it->second;
  };
  if (const auto *value = get("fullscreen")) out.emplace_back("GT4_FULLSCREEN", *value);
  if (const auto *value = get("present_mode")) out.emplace_back("GT4_PRESENT_MODE", *value);
  if (const auto *value = get("monitor")) out.emplace_back("GT4_MONITOR", *value);
  if (const auto *value = get("output_size")) out.emplace_back("GT4_OUTPUT_SIZE", *value);
  if (const auto *value = get("pad")) out.emplace_back("GT4_PAD", *value);
  if (const auto *value = get("camera_360")) out.emplace_back("GT4_CAMERA_360", *value);
  if (const auto *value = get("fov_adjust")) out.emplace_back("GT4_FOV_ADJUST", *value);
  if (const auto *value = get("trigger_full_range")) out.emplace_back("GT4_TRIGGER_FULL_RANGE", *value);
  if (const auto *value = get("wheel_file")) out.emplace_back("GT4_WHEEL_FILE", *value);
  if (const auto *value = get("async_gs"); value && *value == "0") out.emplace_back("GT4_GS_SYNC", "1");
  if (const auto *value = get("performance_report"); value && *value == "1") out.emplace_back("GT4_PERF", "1");
  if (const auto *value = get("check_vu_code"); value && *value != "0") {
    out.emplace_back("GT4_VU1_VERIFY", *value);
    out.emplace_back("GT4_VU0_VERIFY", *value);
  }
  return out;
}

Environment default_environment() {
  return {
      {"GT4_RENDERER", "pcsx2"},          // GSdx
      {"PS2_GS", "gpu"},                  // what the launcher sets with it
      {"GT4_FORCE_480P", "1"},            // the race display at 480p
      {"GT4_FORCE_WIDESCREEN", "1"},      // 16:9 hold
      {"GT4_DISPLAY_ASPECT", "auto"},     // automatic ultrawide correction
      {"GT4_FULLSCREEN", "1"},            // fullscreen
      {"GT4_PRESENT_MODE", "fifo"},       // vsync
  };
}

// TASK-027: game.ini says CoreGS 2.0 (renderer=coregs20; the launcher wrote it) and the process environment names no
// renderer.
bool coregs20_chosen(const Inputs &inputs, const Access &access) {
  if (!inputs.config || !value_of(access, "GT4_RENDERER").empty()) return false;
  const auto it = inputs.config->values.find("renderer");
  return it != inputs.config->values.end() && it->second == "coregs20";
}

// The variables the launcher sets for a CoreGS 2.0 session (launch.rs coregs20_environment).
Environment coregs20_environment(const Config &config) {
  Environment out = {{"GT4_RENDERER", "hw"}, {"GT4_HW_CORE", "2"}};
  for (const auto &[key, name] : {std::pair<const char *, const char *>{"hw_scale", "GT4_HW_SCALE"},
                                  {"hw_performance", "GT4_CORE2_PERFORMANCE"},
                                  {"hw_effect_flares", "GT4_CORE2_PERF_FLARES"},
                                  {"hw_effect_road_sheen", "GT4_CORE2_PERF_ROAD_SHEEN"},
                                  {"hw_frame_skip", "GT4_FRAME_SKIP_FORCE"},
                                  {"hw_frame_skip_adaptive", "GT4_FRAME_SKIP"},
                                  {"hw_hud", "GT4_HW_HUD"},
                                  {"hw_textures", "GT4_HW_TEXTURES"},
                                  {"hw_postfx", "GT4_HW_POSTFX"},
                                  {"hw_sharpening", "GT4_CORE2_FSR_SHARPNESS"},
                                  {"hw_present_mode", "GT4_PRESENT_MODE"},
                                  {"hw_present_shared_queue", "GT4_HW_PRESENT_SHARED_QUEUE"},
                                  {"hw_adapter", "GT4_HW_ADAPTER"}}) {
    const auto it = config.values.find(key);
    if (it != config.values.end()) out.emplace_back(name, it->second);
  }
  return out;
}

bool renderer_needed(const Inputs &inputs, const Access &access) {
  const std::string renderer = value_of(access, "GT4_RENDERER");
  return (renderer.empty() || renderer == "pcsx2") && !is_set(access, "GT4_GS_PROCESS") && !coregs20_chosen(inputs, access);
}

Resolved resolve(const Inputs &inputs, const Access &access) {
  Resolved out;
  const Layout &layout = *inputs.layout;
  out.needs_renderer = renderer_needed(inputs, access);
  const std::string renderer = value_of(access, "GT4_RENDERER");
  const bool coregs20 = coregs20_chosen(inputs, access);
  const bool pcsx2 = (renderer.empty() || renderer == "pcsx2") && !coregs20;

  // Layers, highest first. A name already set in the process environment, or by a higher layer, is left alone.
  Environment layers;
  if (inputs.config) {
    // CoreGS 2.0's keys first: hw_present_mode wins over present_mode.
    if (coregs20) {
      Environment from_choice = coregs20_environment(*inputs.config);
      from_choice.emplace_back("GT4_HW_SHADER_CACHE", (layout.user_root / "shader-cache").string());  // IN-489
      layers.insert(layers.end(), from_choice.begin(), from_choice.end());
    }
    const Environment from_config = config_environment(*inputs.config);
    layers.insert(layers.end(), from_config.begin(), from_config.end());
  }
  if (out.needs_renderer && inputs.renderer) {
    layers.emplace_back("GT4_GS_PROCESS", inputs.renderer->executable.string());
    layers.emplace_back("GT4_GS_PROCESS_SHA256", inputs.renderer->recorded_sha256);
  }
  if (pcsx2 && inputs.gs_ini_exists) {
    const std::filesystem::path ini = layout.config_dir / kGsConfigFileName;
    layers.emplace_back("GT4_GS_CONFIG", ini.string());
    // TASK-117: Performance mode on also limits the flares in a direct start (GT4 v2.00 only).
    if (std::string_view(layout.game.id) == "gt4" && gs_ini_performance_mode(access, ini))
      layers.emplace_back("GT4_FLARE_LIMITS", "1");
  }
  if (inputs.controls_exists) layers.emplace_back("GT4_CONTROLS_FILE", (layout.user_root / "controls.toml").string());
  const Environment defaults = default_environment();
  for (const auto &entry : defaults) {
    if (!(coregs20 && entry.first == "PS2_GS")) layers.push_back(entry);  // it goes with GSdx
  }
  for (const auto &[name, value] : layers) {
    if (is_set(access, name)) continue;
    const bool known = std::any_of(out.set.begin(), out.set.end(), [&](const auto &entry) { return entry.first == name; });
    if (!known) out.set.emplace_back(name, value);
  }

  // The command line the launcher passes (launch.rs plan_with), with the user's options replacing their default.
  std::string region = "--ntsc", disc = inputs.disc.string(), frames = "0", card = layout.card.string();
  std::string misses = (inputs.session_dir / "misses.txt").string(), window = "--window";
  std::vector<std::string> extra;
  const auto &user = inputs.user_arguments;
  for (std::size_t i = 0; i < user.size(); ++i) {
    const std::string &argument = user[i];
    const bool has_value = i + 1 < user.size();
    if (argument == "--disc" && has_value) disc = user[++i];
    else if (argument == "--memcard" && has_value) card = user[++i];
    else if (argument == "--frames" && has_value) frames = user[++i];
    else if (argument == "--misses" && has_value) misses = user[++i];
    else if (argument == "--ntsc" || argument == "--pal") region = argument;
    else if (argument == "--window") window = "--window";
    else if (argument == "--fullscreen") window = "--fullscreen";
    else if (argument == "--no-window") window.clear(); // headless: tests and automation (not a donor option)
    else extra.push_back(argument);
  }
  out.arguments = {inputs.elf.string(), region, "--disc", disc, "--frames", frames, "--memcard", card, "--misses", misses};
  if (!window.empty()) out.arguments.push_back(window);
  out.arguments.insert(out.arguments.end(), extra.begin(), extra.end());
  return out;
}

std::string session_name(std::uint64_t unix_seconds) { return "standalone-" + std::to_string(unix_seconds); }

std::vector<std::string> sessions_to_remove(std::vector<std::string> names, std::size_t keep) {
  names.erase(std::remove_if(names.begin(), names.end(), [](const std::string &name) { return !starts_with(name, "standalone-"); }),
              names.end());
  // Oldest first: by the number after the prefix, then by name.
  const auto number = [](const std::string &name) {
    return parse_u64(name.substr(std::strlen("standalone-"))).value_or(0);
  };
  std::sort(names.begin(), names.end(), [&](const std::string &a, const std::string &b) {
    const auto na = number(a), nb = number(b);
    return na != nb ? na < nb : a < b;
  });
  if (names.size() <= keep) return {};
  names.resize(names.size() - keep);
  return names;
}

std::string message_for(const Check &check, std::string_view display_name) {
  const std::string name(display_name);
  std::string text;
  switch (check.problem) {
  case Problem::NotInstalled:
    text = name + " has to be started from the game folder of its GT4Recomp install. Open GT4Recomp, or start the game from the "
                  "folder it was installed in.";
    break;
  case Problem::NoGameFiles:
    text = "The game files of " + name + " are not prepared yet. Open GT4Recomp once and choose your disc image. After that, "
           "this program starts on its own.";
    break;
  case Problem::GameFilesChanged:
    text = "The game files of " + name + " need to be set up again. Open GT4Recomp once and choose your disc image again. "
           "After that, this program starts on its own.";
    break;
  case Problem::NoRenderer:
    text = "The graphics renderer (GSdx) is not installed yet. Open GT4Recomp once: it installs the renderer. After that, this "
           "program starts on its own.";
    break;
  case Problem::RendererChanged:
    text = "The graphics renderer (GSdx) did not pass its check. Open GT4Recomp once: it installs the renderer again. After "
           "that, this program starts on its own.";
    break;
  case Problem::None:
    return {};
  }
  if (!check.detail.empty()) text += "\n\nDetails: " + check.detail;
  return text;
}

} // namespace gt4::standalone
