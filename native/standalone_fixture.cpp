// TASK-097: tests of the standalone start rules (native/standalone.cpp) over a fake file system and environment.

#include "standalone.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <string>

namespace sa = gt4::standalone;
namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(bool condition, const char *what) {
  if (!condition) {
    std::fprintf(stderr, "standalone_fixture: FAILED %s\n", what);
    ++failures;
  }
}

std::string read_sample(const char *name) {
  std::ifstream file(fs::path(GT4_STANDALONE_SAMPLE_DIR) / name, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

std::string key(const fs::path &path) {
  std::string text = path.generic_string();
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  while (text.size() > 1 && text.back() == '/') text.pop_back();
  return text;
}

struct Entry {
  bool directory = false;
  std::string text;
  sa::FileInfo info;
  std::string sha256;
};

// A fake file system and environment behind sa::Access.
struct World {
  std::map<std::string, Entry> entries;
  std::map<std::string, std::string> environment;

  void file(const fs::path &path, const std::string &text, std::uint64_t size = 0, std::uint64_t mtime = 0, std::string sha = "") {
    Entry entry;
    entry.text = text;
    entry.info.size_bytes = size ? size : text.size();
    entry.info.modified_unix_nanos = mtime;
    entry.sha256 = std::move(sha);
    entries[key(path)] = entry;
    for (fs::path parent = path.parent_path(); !parent.empty() && parent != parent.parent_path(); parent = parent.parent_path()) {
      Entry folder;
      folder.directory = true;
      entries.emplace(key(parent), folder);
    }
  }

  sa::Access access() const {
    sa::Access out;
    out.stat_file = [this](const fs::path &path) -> std::optional<sa::FileInfo> {
      const auto it = entries.find(key(path));
      if (it == entries.end() || it->second.directory) return std::nullopt;
      return it->second.info;
    };
    out.is_directory = [this](const fs::path &path) {
      const auto it = entries.find(key(path));
      return it != entries.end() && it->second.directory;
    };
    out.read_text = [this](const fs::path &path) -> std::optional<std::string> {
      const auto it = entries.find(key(path));
      if (it == entries.end() || it->second.directory) return std::nullopt;
      return it->second.text;
    };
    out.sha256_file = [this](const fs::path &path) -> std::optional<std::string> {
      const auto it = entries.find(key(path));
      if (it == entries.end() || it->second.directory) return std::nullopt;
      return it->second.sha256;
    };
    out.environment = [this](const std::string &name) -> std::optional<std::string> {
      const auto it = environment.find(name);
      if (it == environment.end()) return std::nullopt;
      return it->second;
    };
    return out;
  }
};

const fs::path kInstall = "C:/Program Files/GT4Recomp";
const std::string kHash(64, 'a');

// An installed game with everything in place: receipt (the sample), ELF, disc copy, GSdx 1.2.3.
World ready_world(const char *id = "gt4", const char *elf = "SCUS_973.28") {
  World world;
  const fs::path game_root = kInstall / "data" / "games" / id;
  world.file(game_root / "prepared" / "prepare.json", read_sample("prepare.json"));
  world.file(game_root / "prepared" / elf, "elf", 5555555);
  world.file(game_root / "disc" / "SCUS-97328.iso", "iso", 4294967296ull, 1761200000123456700ull);
  world.file(kInstall / "data" / "gs" / "current.txt", "1.2.3\r\n");
  world.file(kInstall / "data" / "gs" / "1.2.3" / "gt4-gs.exe", "gs", 0, 0, kHash);
  world.file(kInstall / "data" / "gs" / "1.2.3" / "sha256.txt", kHash + "\n");
  world.file(kInstall / "game" / "gt4vulkan.exe", "game");
  return world;
}

bool has(const sa::Environment &environment, const std::string &name, const std::string &value) {
  return std::any_of(environment.begin(), environment.end(), [&](const auto &entry) { return entry.first == name && entry.second == value; });
}

bool lacks(const sa::Environment &environment, const std::string &name) {
  return std::none_of(environment.begin(), environment.end(), [&](const auto &entry) { return entry.first == name; });
}

void check_game_ids_and_mode() {
  check(sa::game_for_executable("gt4vulkan.exe") && std::string(sa::game_for_executable("gt4vulkan.exe")->id) == "gt4", "gt4vulkan.exe is gt4");
  check(sa::game_for_executable("GT4Vulkan-Spec2.EXE") && std::string(sa::game_for_executable("GT4Vulkan-Spec2.EXE")->elf_name) == "SCUS_974.36",
        "gt4vulkan-spec2.exe is spec2, any case");
  check(sa::game_for_executable("gt4.exe") && std::string(sa::game_for_executable("gt4.exe")->id) == "gt4", "a release's name of the game");
  check(sa::game_for_executable("GT4-Spec2.exe") && std::string(sa::game_for_executable("GT4-Spec2.exe")->id) == "spec2", "a release's name of Spec II");
  check(!sa::game_for_executable("gt4vulkan_fixture.exe") && !sa::game_for_executable("gt4vulkan"), "other names are no game");

  const char *none[] = {"gt4vulkan.exe"};
  check(sa::is_standalone_start(1, none), "no argument: standalone");
  const char *options[] = {"gt4vulkan.exe", "--frames", "600"};
  check(sa::is_standalone_start(3, options), "only options: standalone");
  const char *fullscreen[] = {"gt4vulkan.exe", "--fullscreen"};
  check(sa::is_standalone_start(2, fullscreen), "--fullscreen alone: standalone");
  const char *explicit_elf[] = {"gt4vulkan.exe", "C:/x/SCUS_973.28", "--ntsc"};
  check(!sa::is_standalone_start(3, explicit_elf), "an ELF argument is the explicit mode of the launcher and the harnesses");
  const char *portable_check[] = {"gt4vulkan.exe", "--portable-check"};
  check(!sa::is_standalone_start(2, portable_check), "--portable-check is a donor command, not a start");
}

void check_layout() {
  World world = ready_world();
  const sa::Access access = world.access();
  sa::Check why;
  auto layout = sa::find_layout(kInstall / "game" / "gt4vulkan.exe", "C:/Users/p/AppData/Local", "C:/Users/p/AppData/Roaming", access, why);
  check(layout.has_value() && why.problem == sa::Problem::None, "an install is found from the executable path");
  if (!layout) return;
  check(std::string(layout->game.id) == "gt4", "the game follows the file name");
  check(layout->install_root == kInstall, "the install folder is the parent of game\\");
  check(layout->data_root == kInstall / "data", "the data folder is beside the program");
  check(layout->game_root == kInstall / "data" / "games" / "gt4", "files are in games\\gt4");
  check(layout->card == kInstall / "data" / "games" / "gt4" / "runtime" / "card.ps2", "the runtime card");
  check(layout->gs_root == kInstall / "data" / "gs", "the renderer root");
  check(layout->sessions == kInstall / "data" / "sessions", "the sessions folder");
  check(layout->user_root == fs::path("C:/Users/p/AppData/Roaming") / "GT4Recomp", "the user folder is in AppData without a user\\ folder");
  check(layout->config_dir == layout->user_root / "config", "the config folder");

  world.file(kInstall / "user" / "config" / "game.ini", "version=1\n");
  layout = sa::find_layout(kInstall / "game" / "gt4vulkan.exe", "C:/Users/p/AppData/Local", "C:/Users/p/AppData/Roaming", world.access(), why);
  check(layout && layout->user_root == kInstall / "user", "a user\\ folder beside the program (portable) holds the settings");

  // Spec II: its own folder under games.
  World spec2 = ready_world("spec2", "SCUS_974.36");
  spec2.file(kInstall / "game" / "gt4vulkan-spec2.exe", "game");
  layout = sa::find_layout(kInstall / "game" / "gt4vulkan-spec2.exe", "", "", spec2.access(), why);
  check(layout && std::string(layout->game.id) == "spec2" && layout->game_root == kInstall / "data" / "games" / "spec2", "Spec II uses games\\spec2");
  check(layout && layout->user_root == kInstall / "user", "no AppData known: user\\ beside the program");

  // Not an install: the executable is not in a game\ folder, or is not a game executable.
  layout = sa::find_layout("C:/build/gt4vulkan.exe", "", "", access, why);
  check(!layout && why.problem == sa::Problem::NotInstalled, "a build folder is not an install");
  layout = sa::find_layout(kInstall / "game" / "vulkan_fixture.exe", "", "", access, why);
  check(!layout && why.problem == sa::Problem::NotInstalled, "another program is not a game");

  // The data folder of a read-only copy: the %LOCALAPPDATA% fallback holds the prepared files.
  World fallback;
  fallback.file(fs::path("C:/Users/p/AppData/Local/GT4Recomp/games/gt4/prepared/prepare.json"), "{}");
  fallback.file(fs::path("C:/Users/p/AppData/Local/GT4Recomp/gs/current.txt"), "1.2.3");
  fallback.file(kInstall / "data" / "gs" / "readme.txt", "x");
  layout = sa::find_layout(kInstall / "game" / "gt4vulkan.exe", "C:/Users/p/AppData/Local", "C:/Users/p/AppData/Roaming", fallback.access(), why);
  check(layout && layout->data_root == fs::path("C:/Users/p/AppData/Local/GT4Recomp"), "the fallback data folder is used when it holds the files");
  check(layout && layout->gs_root == fs::path("C:/Users/p/AppData/Local/GT4Recomp/gs"), "and its renderer");
}

void check_receipt() {
  const auto receipt = sa::parse_receipt(read_sample("prepare.json"));
  check(receipt.has_value(), "the sample receipt is read");
  if (receipt) {
    check(receipt->image_file == "SCUS-97328.iso", "image file");
    check(receipt->image_size == 4294967296ull, "image size");
    check(receipt->image_modified_unix_nanos == 1761200000123456700ull, "image modified time");
    check(receipt->elf_size == 5555555, "elf size");
  }
  check(!sa::parse_receipt("not json"), "garbage is refused");
  check(!sa::parse_receipt("{\"schema_version\":1,\"elf\":{\"size_bytes\":1}}"), "a receipt without the image copy (schema 1) is refused");
  check(!sa::parse_receipt("{\"image\":null,\"elf\":{\"size_bytes\":1}}"), "a null image is refused");
  // A key text inside a string value must not be taken for a key, and braces inside strings must not end an object.
  const std::string tricky =
      "{\"disc\":{\"path\":\"C:\\\\x\\\\\\\"image\\\\\\\": {} } \"},\"image\":{\"file\":\"a b.iso\",\"size_bytes\":7,\"modified_unix_nanos\":9,"
      "\"sha256\":\"x\"},\"elf\":{\"size_bytes\":3}}";
  const auto parsed = sa::parse_receipt(tricky);
  check(parsed && parsed->image_file == "a b.iso" && parsed->image_size == 7 && parsed->elf_size == 3, "strings with keys and braces are skipped");
  check(!sa::parse_receipt("{\"image\":{\"file\":\"a\\u00e9.iso\",\"size_bytes\":1,\"modified_unix_nanos\":1},\"elf\":{\"size_bytes\":1}}"),
        "an escape a plain file name never has is refused");
}

void check_linked_image() {
  // Decision 0075: the receipt names the image where the player keeps it; backslashes come escaped.
  const std::string json =
      "{\"image\":{\"file\":\"gt4.iso\",\"linked_path\":\"D:\\\\ISOs\\\\gt4.iso\",\"size_bytes\":7,\"modified_unix_nanos\":9},\"elf\":{\"size_bytes\":3}}";
  const auto linked = sa::parse_receipt(json);
  check(linked && linked->image_linked_path == "D:\\ISOs\\gt4.iso", "the receipt's link is read");
  const auto copied = sa::parse_receipt(read_sample("prepare.json"));
  check(copied && copied->image_linked_path.empty(), "a receipt from before the link has none, and uses its copy");

  World world = ready_world();
  const auto game_root = kInstall / "data" / "games" / "gt4";
  world.file(game_root / "prepared" / "prepare.json", json);
  world.file(game_root / "prepared" / "SCUS_973.28", "elf");
  sa::Check why;
  const auto layout = sa::find_layout(kInstall / "game" / "gt4vulkan.exe", "C:/Users/p/AppData/Local", "C:/Users/p/AppData/Roaming", world.access(), why);
  check(layout.has_value(), "a layout for the linked image");
  if (!layout) return;
  fs::path disc, elf;
  check(sa::check_game_files(*layout, world.access(), disc, elf).problem == sa::Problem::GameFilesChanged, "an image that is not there is reported");
  check(disc == fs::path("D:\\ISOs\\gt4.iso"), "the game looks for it where the player keeps it");
  world.file(fs::path("D:\\ISOs\\gt4.iso"), "iso", 7, 9);
  check(sa::check_game_files(*layout, world.access(), disc, elf).problem == sa::Problem::None, "the linked image passes");
  sa::Access with_short_names = world.access();
  with_short_names.openable_path = [](const fs::path &) { return fs::path("D:\\ISOS~1\\GT4~1.ISO"); };
  check(sa::check_game_files(*layout, with_short_names, disc, elf).problem == sa::Problem::None && disc == fs::path("D:\\ISOS~1\\GT4~1.ISO"),
        "the game is given the name it can open");
  world.file(fs::path("D:\\ISOs\\gt4.iso"), "iso", 8, 9);
  check(sa::check_game_files(*layout, world.access(), disc, elf).problem == sa::Problem::GameFilesChanged, "a changed image does not");
  world.file(fs::path("D:\\ISOs\\gt4.iso"), "iso", 7, 1500000009ull);
  check(sa::check_game_files(*layout, world.access(), disc, elf).problem == sa::Problem::None, "a time a file system rounded is not a change");
  world.file(fs::path("D:\\ISOs\\gt4.iso"), "iso", 7, 9000000009ull);
  check(sa::check_game_files(*layout, world.access(), disc, elf).problem == sa::Problem::GameFilesChanged, "a later time is");

  // An image inside the launcher's folder is saved relative to it, so a portable folder can move with it.
  world.file(game_root / "prepared" / "prepare.json",
             "{\"image\":{\"file\":\"gt4.iso\",\"linked_path\":\"ISOs\\\\gt4.iso\",\"size_bytes\":7,\"modified_unix_nanos\":9},\"elf\":{\"size_bytes\":3}}");
  world.file(kInstall / "ISOs" / "gt4.iso", "iso", 7, 9);
  check(sa::check_game_files(*layout, world.access(), disc, elf).problem == sa::Problem::None && disc == kInstall / "ISOs\\gt4.iso",
        "a relative link is found inside the install folder");
}

void check_game_files() {
  World world = ready_world();
  sa::Check why;
  const auto layout = sa::find_layout(kInstall / "game" / "gt4vulkan.exe", "", "", world.access(), why);
  fs::path disc, elf;
  sa::Check result = sa::check_game_files(*layout, world.access(), disc, elf);
  check(result.problem == sa::Problem::None, "ready game files pass");
  check(disc == kInstall / "data" / "games" / "gt4" / "disc" / "SCUS-97328.iso", "the disc copy is named by the receipt");
  check(elf == kInstall / "data" / "games" / "gt4" / "prepared" / "SCUS_973.28", "the ELF keeps its file name (argv0 rule)");

  World no_receipt = ready_world();
  no_receipt.entries.erase(key(kInstall / "data" / "games" / "gt4" / "prepared" / "prepare.json"));
  check(sa::check_game_files(*layout, no_receipt.access(), disc, elf).problem == sa::Problem::NoGameFiles, "no receipt: not prepared yet");

  World bad_elf = ready_world();
  bad_elf.file(kInstall / "data" / "games" / "gt4" / "prepared" / "SCUS_973.28", "elf", 5555556);
  check(sa::check_game_files(*layout, bad_elf.access(), disc, elf).problem == sa::Problem::GameFilesChanged, "an ELF of another size");

  World no_elf = ready_world();
  no_elf.entries.erase(key(kInstall / "data" / "games" / "gt4" / "prepared" / "SCUS_973.28"));
  check(sa::check_game_files(*layout, no_elf.access(), disc, elf).problem == sa::Problem::GameFilesChanged, "a missing ELF");

  World no_disc = ready_world();
  no_disc.entries.erase(key(kInstall / "data" / "games" / "gt4" / "disc" / "SCUS-97328.iso"));
  result = sa::check_game_files(*layout, no_disc.access(), disc, elf);
  check(result.problem == sa::Problem::GameFilesChanged && result.detail.find("missing") != std::string::npos, "a missing disc copy");

  World bad_size = ready_world();
  bad_size.file(kInstall / "data" / "games" / "gt4" / "disc" / "SCUS-97328.iso", "iso", 4294967295ull, 1761200000123456700ull);
  check(sa::check_game_files(*layout, bad_size.access(), disc, elf).problem == sa::Problem::GameFilesChanged, "a disc copy of another size");

  World bad_time = ready_world();
  bad_time.file(kInstall / "data" / "games" / "gt4" / "disc" / "SCUS-97328.iso", "iso", 4294967296ull, 1761200000123456800ull);
  result = sa::check_game_files(*layout, bad_time.access(), disc, elf);
  check(result.problem == sa::Problem::GameFilesChanged && result.detail.find("changed") != std::string::npos, "a disc copy with another modified time");

  World traversal = ready_world();
  std::string receipt = read_sample("prepare.json");
  receipt.replace(receipt.find("SCUS-97328.iso"), 14, "..\\\\evil.iso");
  traversal.file(kInstall / "data" / "games" / "gt4" / "prepared" / "prepare.json", receipt);
  check(sa::check_game_files(*layout, traversal.access(), disc, elf).problem == sa::Problem::GameFilesChanged, "a disc name with a path is refused");
}

void check_renderer() {
  World world = ready_world();
  sa::Check why;
  const auto layout = sa::find_layout(kInstall / "game" / "gt4vulkan.exe", "", "", world.access(), why);
  sa::Renderer renderer;
  check(sa::find_renderer(*layout, world.access(), renderer).problem == sa::Problem::None, "an installed GSdx passes");
  check(renderer.executable == kInstall / "data" / "gs" / "1.2.3" / "gt4-gs.exe" && renderer.recorded_sha256 == kHash, "its path and recorded hash");

  World none;
  none.file(kInstall / "game" / "gt4vulkan.exe", "game");
  const auto empty_layout = sa::find_layout(kInstall / "game" / "gt4vulkan.exe", "", "", none.access(), why);
  check(sa::find_renderer(*empty_layout, none.access(), renderer).problem == sa::Problem::NoRenderer, "no current.txt: not installed");

  World bad_version = ready_world();
  bad_version.file(kInstall / "data" / "gs" / "current.txt", "..\\evil");
  check(sa::find_renderer(*layout, bad_version.access(), renderer).problem == sa::Problem::NoRenderer, "a version that is not x.y.z is refused");

  World no_exe = ready_world();
  no_exe.entries.erase(key(kInstall / "data" / "gs" / "1.2.3" / "gt4-gs.exe"));
  check(sa::find_renderer(*layout, no_exe.access(), renderer).problem == sa::Problem::NoRenderer, "a missing gt4-gs.exe");

  World no_hash = ready_world();
  no_hash.entries.erase(key(kInstall / "data" / "gs" / "1.2.3" / "sha256.txt"));
  check(sa::find_renderer(*layout, no_hash.access(), renderer).problem == sa::Problem::NoRenderer, "no recorded hash");

  World changed = ready_world();
  changed.file(kInstall / "data" / "gs" / "1.2.3" / "gt4-gs.exe", "gs", 0, 0, std::string(64, 'b'));
  check(sa::find_renderer(*layout, changed.access(), renderer).problem == sa::Problem::RendererChanged, "a gt4-gs.exe that does not match its recorded hash");

  World upper = ready_world();
  upper.file(kInstall / "data" / "gs" / "1.2.3" / "sha256.txt", std::string(64, 'A') + "\r\n");
  check(sa::find_renderer(*layout, upper.access(), renderer).problem == sa::Problem::None, "the recorded hash is read case-insensitively");
}

void check_config() {
  const sa::Config sample = sa::parse_config(read_sample("game.ini"));
  check(sample.warnings.empty(), "the launcher's sample has no warning");
  check(sample.values.size() == 7, "seven keys in the sample");
  const sa::Environment environment = sa::config_environment(sample);
  check(has(environment, "GT4_FULLSCREEN", "1") && has(environment, "GT4_PRESENT_MODE", "fifo"), "fullscreen and present mode");
  check(has(environment, "GT4_MONITOR", "\\\\.\\DISPLAY2") && has(environment, "GT4_OUTPUT_SIZE", "3440x1440"), "monitor and output size");
  check(lacks(environment, "GT4_GS_SYNC") && lacks(environment, "GT4_PERF") && lacks(environment, "GT4_VU1_VERIFY"), "the default switches add nothing");
  check(has(environment, "GT4_TRIGGER_FULL_RANGE", "1"), "TASK-161: the launcher states the full trigger range (on by default)");

  const sa::Config changed = sa::parse_config("fullscreen=0\npresent_mode=mailbox\nasync_gs=0\nperformance_report=1\ncheck_vu_code=100\n");
  const sa::Environment changed_environment = sa::config_environment(changed);
  check(has(changed_environment, "GT4_FULLSCREEN", "0") && has(changed_environment, "GT4_PRESENT_MODE", "mailbox"), "windowed, mailbox");
  check(has(changed_environment, "GT4_GS_SYNC", "1") && has(changed_environment, "GT4_PERF", "1"), "synchronous GS and the report");
  check(has(changed_environment, "GT4_VU1_VERIFY", "100") && has(changed_environment, "GT4_VU0_VERIFY", "100"), "VU checks");
  const sa::Config wheel = sa::parse_config("wheel_file=C:\\Users\\P\\AppData\\Roaming\\GT4Recomp\\wheel.toml\nwheel_file=\n");
  check(has(sa::config_environment(wheel), "GT4_WHEEL_FILE", "C:\\Users\\P\\AppData\\Roaming\\GT4Recomp\\wheel.toml") &&
            wheel.warnings.size() == 1,
        "TASK-181: wheel_file becomes GT4_WHEEL_FILE; an empty value is refused");

  const sa::Config bad = sa::parse_config("fullscreen=yes\nmonitor=DISPLAY1\noutput_size=axb\npresent_mode=slow\nunknown=1\nno equals\n  # comment\n\n");
  check(bad.values.empty() && bad.warnings.size() == 6, "unknown keys, a line without = and refused values are dropped with a warning");
  check(sa::config_environment(bad).empty(), "a damaged config stands for nothing");
  check(sa::parse_config("FULLSCREEN = 1 \r\nPresent_Mode=fifo\r\n").values.size() == 2, "keys are case-insensitive and values are trimmed");
}

sa::Inputs inputs_for(const sa::Layout &layout, const sa::Renderer *renderer) {
  sa::Inputs inputs;
  inputs.layout = &layout;
  inputs.elf = layout.prepared / layout.game.elf_name;
  inputs.disc = layout.disc_folder / "SCUS-97328.iso";
  inputs.session_dir = layout.sessions / "standalone-1761200000";
  inputs.renderer = renderer;
  return inputs;
}

void check_resolve() {
  World world = ready_world();
  sa::Check why;
  const auto layout = sa::find_layout(kInstall / "game" / "gt4vulkan.exe", "", "C:/Users/p/AppData/Roaming", world.access(), why);
  sa::Renderer renderer;
  sa::find_renderer(*layout, world.access(), renderer);

  // Nothing but the built-in defaults.
  sa::Inputs inputs = inputs_for(*layout, &renderer);
  sa::Resolved resolved = sa::resolve(inputs, world.access());
  check(resolved.needs_renderer, "GSdx is needed by default");
  check(has(resolved.set, "GT4_RENDERER", "pcsx2") && has(resolved.set, "PS2_GS", "gpu"), "GSdx is the default renderer");
  check(has(resolved.set, "GT4_FORCE_480P", "1") && has(resolved.set, "GT4_FORCE_WIDESCREEN", "1") && has(resolved.set, "GT4_DISPLAY_ASPECT", "auto"),
        "480p, the 16:9 hold and the automatic ultrawide are default");
  check(has(resolved.set, "GT4_FULLSCREEN", "1") && has(resolved.set, "GT4_PRESENT_MODE", "fifo"), "fullscreen and vsync are default");
  check(has(resolved.set, "GT4_GS_PROCESS", renderer.executable.string()) && has(resolved.set, "GT4_GS_PROCESS_SHA256", kHash), "the renderer and its recorded hash");
  check(lacks(resolved.set, "GT4_GS_CONFIG") && lacks(resolved.set, "GT4_CONTROLS_FILE"), "no ini and no controls file: not set");
  check(lacks(resolved.set, "GT4_MONITOR") && lacks(resolved.set, "GT4_OUTPUT_SIZE"), "no config: the monitor the window opens on");
  const std::vector<std::string> expected = {inputs.elf.string(), "--ntsc", "--disc", inputs.disc.string(), "--frames", "0", "--memcard",
                                             layout->card.string(), "--misses", (inputs.session_dir / "misses.txt").string(), "--window"};
  check(resolved.arguments == expected, "the command line the launcher builds");

  // The config file and the files it names.
  inputs.config = sa::parse_config(read_sample("game.ini") + "fullscreen=0\n");
  inputs.gs_ini_exists = true;
  inputs.controls_exists = true;
  resolved = sa::resolve(inputs, world.access());
  check(has(resolved.set, "GT4_FULLSCREEN", "0") && lacks(resolved.set, "GT4_PRESENT_MODE") == false, "the config wins over the default");
  check(std::count_if(resolved.set.begin(), resolved.set.end(), [](const auto &entry) { return entry.first == "GT4_FULLSCREEN"; }) == 1, "each variable once");
  check(has(resolved.set, "GT4_MONITOR", "\\\\.\\DISPLAY2") && has(resolved.set, "GT4_OUTPUT_SIZE", "3440x1440"), "the display from the config");
  check(has(resolved.set, "GT4_GS_CONFIG", (layout->config_dir / "gt4-gs.ini").string()), "the gt4-gs.ini of the launcher");
  check(has(resolved.set, "GT4_CONTROLS_FILE", (layout->user_root / "controls.toml").string()), "the controls file of the launcher");
  // TASK-117: Performance mode on in that ini limits the flares (GT4 v2.00 only), like the launcher's GT4_FLARE_LIMITS.
  World accurate = ready_world();
  accurate.file(layout->config_dir / "gt4-gs.ini", "[EmuCore/GS]\nHWDownloadMode = 0\n");
  check(lacks(sa::resolve(inputs, accurate.access()).set, "GT4_FLARE_LIMITS"), "Accurate mode keeps the original flares");
  World performance = ready_world();
  performance.file(layout->config_dir / "gt4-gs.ini", "; gt4-gs.ini\n[EmuCore/GS]\nRenderer = 14\nHWDownloadMode = 2\n\n[GT4GS]\nSurfaceless = true\n");
  check(has(sa::resolve(inputs, performance.access()).set, "GT4_FLARE_LIMITS", "1"),
        "Performance mode on in the gt4-gs.ini sends the flare limits");
  // A GT4_FLARE_LIMITS the user set himself is never overwritten.
  World inherited = ready_world();
  inherited.file(layout->config_dir / "gt4-gs.ini", "[EmuCore/GS]\nHWDownloadMode = 2\n");
  inherited.environment["GT4_FLARE_LIMITS"] = "0";
  check(lacks(sa::resolve(inputs, inherited.access()).set, "GT4_FLARE_LIMITS"), "a set variable is never overwritten");

  // The process environment wins over the config and the defaults; an empty value counts as unset.
  world.environment["GT4_FULLSCREEN"] = "1";
  world.environment["GT4_FORCE_480P"] = "0";
  world.environment["GT4_PRESENT_MODE"] = "";
  resolved = sa::resolve(inputs, world.access());
  check(lacks(resolved.set, "GT4_FULLSCREEN") && lacks(resolved.set, "GT4_FORCE_480P"), "a variable that is set is never overwritten");
  check(has(resolved.set, "GT4_PRESENT_MODE", "fifo"), "an empty variable counts as unset");

  // CoreGS 2.0 chosen in game.ini: the game runs it with its variables, no GSdx files; a renderer named by hand still wins.
  {
    World chosen = ready_world();
    sa::Inputs choice = inputs_for(*layout, nullptr);
    choice.config = sa::parse_config("version=1\nrenderer=coregs20\nhw_scale=4\nhw_effect_flares=0\nhw_effect_road_sheen=1\nhw_frame_skip=2\nhw_hud=1\nhw_present_shared_queue=0\nhw_textures=D:/T\nhw_adapter=Card\n");
    check(choice.config->warnings.empty(), "the launcher's CoreGS 2.0 keys are accepted");
    check(!sa::parse_config("renderer=donor\nhw_scale=13\nhw_effect_flares=2\n").warnings.empty(), "other values are refused");
    // TASK-174: the one-queue presentation key is accepted only as 0, the launcher's Presentation thread off.
    check(!sa::parse_config("hw_present_shared_queue=1\n").warnings.empty(), "hw_present_shared_queue=1 is refused");
    check(!sa::renderer_needed(choice, chosen.access()), "CoreGS 2.0 needs no GSdx files");
    const sa::Resolved coregs = sa::resolve(choice, chosen.access());
    check(!coregs.needs_renderer && has(coregs.set, "GT4_RENDERER", "hw") && has(coregs.set, "GT4_HW_CORE", "2") &&
              has(coregs.set, "GT4_HW_SCALE", "4") && has(coregs.set, "GT4_CORE2_PERF_FLARES", "0") &&
              has(coregs.set, "GT4_CORE2_PERF_ROAD_SHEEN", "1") && has(coregs.set, "GT4_FRAME_SKIP_FORCE", "2") &&
              has(coregs.set, "GT4_HW_HUD", "1") && has(coregs.set, "GT4_HW_PRESENT_SHARED_QUEUE", "0") && has(coregs.set, "GT4_HW_TEXTURES", "D:/T") &&
              has(coregs.set, "GT4_HW_ADAPTER", "Card"),
          "game.ini's CoreGS 2.0 keys become its variables");
    check(lacks(coregs.set, "PS2_GS") && lacks(coregs.set, "GT4_GS_PROCESS") && lacks(coregs.set, "GT4_GS_CONFIG"),
          "nothing of GSdx goes with it");
    check(has(coregs.set, "GT4_FORCE_480P", "1") && has(coregs.set, "GT4_FORCE_WIDESCREEN", "1"), "the fixed holds stay");
    // TASK-175: the HUD's FPS only mode.
    sa::Inputs fps_hud = inputs_for(*layout, nullptr);
    fps_hud.config = sa::parse_config("version=1\nrenderer=coregs20\nhw_hud=fps\n");
    check(fps_hud.config->warnings.empty() && has(sa::resolve(fps_hud, chosen.access()).set, "GT4_HW_HUD", "fps"), "game.ini's hw_hud=fps becomes GT4_HW_HUD=fps");
    check(!sa::parse_config("hw_hud=full\n").warnings.empty() && !sa::parse_config("hw_hud=0\n").warnings.empty(), "other HUD values are refused");
    // TASK-146: the 60 FPS mode's frame skipping when behind, and nothing else for it.
    sa::Inputs sixty = inputs_for(*layout, nullptr);
    sixty.config = sa::parse_config("version=1\nrenderer=coregs20\nhw_frame_skip_adaptive=1\n");
    check(sixty.config->warnings.empty(), "the 60 FPS mode's key is accepted");
    check(!sa::parse_config("hw_frame_skip_adaptive=2\n").warnings.empty(), "only 1 turns it on");
    const sa::Resolved adaptive = sa::resolve(sixty, chosen.access());
    check(has(adaptive.set, "GT4_FRAME_SKIP", "1") && lacks(adaptive.set, "GT4_FRAME_SKIP_FORCE"),
          "game.ini's 60 FPS mode becomes GT4_FRAME_SKIP=1 without a fixed pattern");
    // TASK-148: the controller chosen in the launcher, for any renderer.
    sa::Inputs pad = inputs_for(*layout, nullptr);
    pad.config = sa::parse_config("version=1\npad=050000004c050000e60c000000810000/a0:ab:51:12:34:56\n");
    check(pad.config->warnings.empty(), "the chosen controller's key is accepted");
    check(!sa::parse_config("pad=auto\n").warnings.empty() && !sa::parse_config("pad=1234\n").warnings.empty(),
          "pad takes a controller, not auto or other text");
    check(has(sa::resolve(pad, chosen.access()).set, "GT4_PAD", "050000004c050000e60c000000810000/a0:ab:51:12:34:56"),
          "game.ini's pad becomes GT4_PAD");
    // TASK-159: the 360-degree chase camera, for any renderer; only 0 and 1, on (1) when the key is absent.
    sa::Inputs camera = inputs_for(*layout, nullptr);
    camera.config = sa::parse_config("version=1\ncamera_360=0\n");
    check(camera.config->warnings.empty(), "the 360 camera key is accepted");
    check(has(sa::resolve(camera, chosen.access()).set, "GT4_CAMERA_360", "0"), "game.ini's camera_360 becomes GT4_CAMERA_360");
    check(!sa::parse_config("camera_360=2\n").warnings.empty() && !sa::parse_config("camera_360=on\n").warnings.empty(),
          "camera_360 takes only 0 or 1");
    check(lacks(sa::resolve(inputs_for(*layout, nullptr), chosen.access()).set, "GT4_CAMERA_360"),
          "no key, no variable: the game's default, on");
    // TASK-160: the field of view adjustment, the same rule.
    sa::Inputs fov = inputs_for(*layout, nullptr);
    fov.config = sa::parse_config("version=1\nfov_adjust=0\n");
    check(fov.config->warnings.empty(), "the field of view key is accepted");
    check(has(sa::resolve(fov, chosen.access()).set, "GT4_FOV_ADJUST", "0"), "game.ini's fov_adjust becomes GT4_FOV_ADJUST");
    check(!sa::parse_config("fov_adjust=2\n").warnings.empty() && !sa::parse_config("fov_adjust=yes\n").warnings.empty(),
          "fov_adjust takes only 0 or 1");
    check(lacks(sa::resolve(inputs_for(*layout, nullptr), chosen.access()).set, "GT4_FOV_ADJUST"),
          "no key, no variable: the game's default, on");
    // TASK-161: the full trigger range, the same rule (the game's default is off).
    sa::Inputs triggers = inputs_for(*layout, nullptr);
    triggers.config = sa::parse_config("version=1\ntrigger_full_range=1\n");
    check(triggers.config->warnings.empty(), "the full trigger range key is accepted");
    check(has(sa::resolve(triggers, chosen.access()).set, "GT4_TRIGGER_FULL_RANGE", "1"),
          "game.ini's trigger_full_range becomes GT4_TRIGGER_FULL_RANGE");
    check(!sa::parse_config("trigger_full_range=on\n").warnings.empty(), "trigger_full_range takes only 0 or 1");
    sa::Inputs fast = inputs_for(*layout, nullptr);
    fast.config = sa::parse_config(std::string("present_mode=mailbox") + char(10) + "renderer=coregs20" + char(10) + "hw_present_mode=fifo" + char(10));
    check(has(sa::resolve(fast, chosen.access()).set, "GT4_PRESENT_MODE", "fifo"), "a stored Fast vsync draws as On under CoreGS 2.0");
    World named = ready_world();
    named.environment["GT4_RENDERER"] = "pcsx2";
    check(sa::renderer_needed(choice, named.access()), "GT4_RENDERER in the environment wins over game.ini");
    World spec2_world = ready_world("spec2", "SCUS_974.36");
    spec2_world.file(kInstall / "game" / "gt4vulkan-spec2.exe", "game");
    const auto spec2_layout = sa::find_layout(kInstall / "game" / "gt4vulkan-spec2.exe", "", "", spec2_world.access(), why);
    check(spec2_layout.has_value(), "the Spec II layout is found");
    sa::Inputs spec2_choice = inputs_for(*spec2_layout, nullptr);
    spec2_choice.config = choice.config;
    const sa::Resolved spec2_coregs = sa::resolve(spec2_choice, spec2_world.access());
    check(!spec2_coregs.needs_renderer && has(spec2_coregs.set, "GT4_RENDERER", "hw") && has(spec2_coregs.set, "GT4_HW_SCALE", "4"),
          "Spec II runs CoreGS 2.0 from the same choice");
  }

  // Another renderer, or a GS process given by hand: the installed one is not needed.
  World donor = ready_world();
  donor.environment["GT4_RENDERER"] = "donor";
  resolved = sa::resolve(inputs_for(*layout, nullptr), donor.access());
  check(!resolved.needs_renderer && lacks(resolved.set, "GT4_GS_PROCESS") && lacks(resolved.set, "GT4_RENDERER"), "GT4_RENDERER=donor needs no GSdx");
  World manual = ready_world();
  manual.environment["GT4_GS_PROCESS"] = "D:/mine/gt4-gs.exe";
  resolved = sa::resolve(inputs_for(*layout, nullptr), manual.access());
  check(!resolved.needs_renderer && lacks(resolved.set, "GT4_GS_PROCESS") && lacks(resolved.set, "GT4_GS_PROCESS_SHA256"), "a GS process given by hand wins");

  // The user's options replace their default; the rest is kept in order.
  inputs = inputs_for(*layout, &renderer);
  inputs.user_arguments = {"--frames", "600", "--fullscreen", "--disc", "D:/other.iso", "--pal", "--trace-io", "--press", "5:cross"};
  resolved = sa::resolve(inputs, ready_world().access());
  const std::vector<std::string> overridden = {inputs.elf.string(), "--pal", "--disc", "D:/other.iso", "--frames", "600", "--memcard", layout->card.string(),
                                               "--misses", (inputs.session_dir / "misses.txt").string(), "--fullscreen", "--trace-io", "--press", "5:cross"};
  check(resolved.arguments == overridden, "user options replace their default and the others are appended");

  // Headless: no window flag at all, and the option itself is not passed to the donor.
  inputs.user_arguments = {"--no-window", "--frames", "60"};
  resolved = sa::resolve(inputs, ready_world().access());
  check(std::find(resolved.arguments.begin(), resolved.arguments.end(), "--window") == resolved.arguments.end() &&
            std::find(resolved.arguments.begin(), resolved.arguments.end(), "--no-window") == resolved.arguments.end() &&
            resolved.arguments.back() == (inputs.session_dir / "misses.txt").string(),
        "--no-window leaves the window flag out");
}

void check_sessions_and_messages() {
  check(sa::session_name(1761200000) == "standalone-1761200000", "session name");
  std::vector<std::string> names = {"session-100", "standalone-9", "standalone-10", "standalone-2", "standalone-100", "other"};
  const std::vector<std::string> removed = sa::sessions_to_remove(names, 2);
  check(removed.size() == 2 && removed[0] == "standalone-2" && removed[1] == "standalone-9", "the oldest standalone sessions go first, by number");
  check(sa::sessions_to_remove(names, 20).empty(), "nothing goes below the limit");
  check(sa::sessions_to_remove({"session-1", "session-2"}, 0).empty(), "launcher sessions are never touched");

  for (sa::Problem problem : {sa::Problem::NotInstalled, sa::Problem::NoGameFiles, sa::Problem::GameFilesChanged, sa::Problem::NoRenderer,
                              sa::Problem::RendererChanged}) {
    const std::string text = sa::message_for(sa::Check{problem, "detail text"}, "Gran Turismo 4");
    check(text.find("GT4Recomp") != std::string::npos, "every message names GT4Recomp");
    check(problem == sa::Problem::NotInstalled || text.find("once") != std::string::npos, "and says to open it once");
    check(text.find("detail text") != std::string::npos, "and carries the detail");
  }
  check(sa::message_for(sa::Check{}, "Gran Turismo 4").empty(), "no problem, no message");
}

} // namespace

int main() {
  check_game_ids_and_mode();
  check_layout();
  check_receipt();
  check_linked_image();
  check_game_files();
  check_renderer();
  check_config();
  check_resolve();
  check_sessions_and_messages();
  if (failures != 0) {
    std::fprintf(stderr, "standalone_fixture: %d check(s) failed\n", failures);
    return 1;
  }
  std::fprintf(stderr, "PASS: standalone start rules (layout, receipt, renderer, config, defaults, precedence, command line)\n");
  return 0;
}
