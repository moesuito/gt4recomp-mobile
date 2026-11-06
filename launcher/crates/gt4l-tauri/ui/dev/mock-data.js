/*
 * Development mock of the Rust command layer. Loaded only by ui/dev/index.html.
 * It answers the same command names and payloads as the Tauri build, so the
 * pages render in a plain browser. Values are sample values, not game data.
 *
 * Query options (ui/dev/index.html?...):
 *   gsdx=missing|offline                            (GSdx is not installed: the download works, or fails)
 *   gsdxfile=ok|cancel|bad                          (with gsdx=offline, "Install from a file": works, picker cancelled, or refused; default bad)
 *   vulkan=no                                       (the card has no Vulkan 1.2: Play does not offer CoreGS 2.0 for a missing GSdx)
 *   setup=done|needed|wrong|copying|checking|preparing|renderer|failed|missing|offline   (default done)
 *   pick=spec                                       (setup: the picker returns an unsupported image, refused)
 *   pick=other                                      (setup: the picker returns the other game's disc, a mismatch)
 *   pick=spec2                                      (setup: the picker returns a Spec II disc)
 *   games=gt4|spec2|both|none                       (which games are installed; default gt4: Spec II shows Install)
 *   selected=spec2                                  (the game the home screen starts on)
 *   running=1                                       (game running at start)
 *   focus=play                                      (home: focus the Play button, for screenshots)
 *   Controls: pad=A,LB,DpadRight lt=0.6 rt=0.2 lx=0.7 ly=-0.45 rx= ry= pads=2 nopad=1 (a still dev controller),
 *             select=circle mode=key capture=hold|conflict remap=1 locked=1
 *   Wheel (TASK-181): wheel=g27|unknown|both|none wheelcfg=g27|fresh wheelfile=migrated|refused|newer steer=90 thr=0.6
 *             brk=0.3 clu=0 wbtn=0,4 what=1 wheelmove=1 session=yes|no|unknown|off|none ffbtest=noffb capture=hold
 *             wizard=1 wizardstep=N wheelselect=throttle wheelpanel=ffb presets=1 (see the wheel block below)
 */
(function (root) {
  "use strict";

  var query = new URLSearchParams(root.location.search);
  // TASK-102: ?lang=pt-BR forces the language (the Settings value and the OS language); ?os=pt-BR sets only the OS
  // language, so Auto follows it. Without either, Auto follows the browser's language.
  root.GT4 = root.GT4 || {};
  var forcedLanguage = query.get("lang") || "";
  root.GT4.devOsLanguage = query.get("os") || forcedLanguage || "";
  var mockLanguage = forcedLanguage || "auto";
  var setupStart = query.get("setup") || "done";
  var IMAGE_BYTES = 5314478080;
  var SPEC_MESSAGE = "This disc is not supported yet. GT4Recomp supports Gran Turismo 4 (USA, SCUS-97328) and " +
    "Gran Turismo 4 Spec II v1.10 (SCUS-97436). This is the unmodified GT4 Online Public Beta (SCUS-97436), not Gran Turismo 4 Spec II.";

  // Two games (TASK-101). The mock keeps the same shapes as commands::game_row and app_state.
  var gamesParam = query.get("games") || "gt4";
  var GAME_TABLE = [
    { id: "gt4", name: "Gran Turismo 4", title: "Gran Turismo 4", variant: "USA v2.00", installed: gamesParam === "gt4" || gamesParam === "both",
      disc_name: "Gran Turismo 4 (USA v2.00)", size_bytes: 18000000, image_bytes: 5314478080 },
    { id: "spec2", name: "Gran Turismo 4 Spec II", title: "Gran Turismo 4 Spec II v1.10", variant: "", installed: gamesParam === "spec2" || gamesParam === "both",
      disc_name: "Gran Turismo 4 Spec II v1.10", size_bytes: 6520944, image_bytes: 3683418112 }
  ];
  // TASK-157: the textures folder choice of the mock (empty: the default folder).
  var texturesChoice = "";
  function texturesState() {
    return {
      path: texturesChoice,
      default: "C:\\Program Files\\GT4Recomp\\data\\Textures",
      custom: texturesChoice !== "",
      valid: !/Missing/.test(texturesChoice)
    };
  }
  var selectedGame = query.get("selected") ||
    (GAME_TABLE.filter(function (game) { return game.installed; })[0] || GAME_TABLE[0]).id;
  var setupGame = null; // the game of the setup that runs (set by setup_choose_disc)
  function gameById(id) {
    return GAME_TABLE.filter(function (game) { return game.id === id; })[0] || null;
  }
  function gamesView() {
    return {
      selected: selectedGame,
      any_installed: GAME_TABLE.some(function (game) { return game.installed; }),
      games: GAME_TABLE.map(function (game) {
        return { id: game.id, name: game.name, title: game.title, variant: game.installed ? game.variant : "",
          installed: game.installed, damaged: false, reason: "",
          selected: game.id === selectedGame, disc_name: game.installed ? game.disc_name : "",
          folder: "C:\\Program Files\\GT4Recomp\\data\\games\\" + game.id,
          linked: game.installed,
          image_path: game.installed ? "D:\\ISOs\\" + game.disc_name + ".iso" : "",
          image_bytes: game.installed ? game.image_bytes : 0,
          size_bytes: game.installed ? game.size_bytes : 0 };
      })
    };
  }

  var setup = { state: "done", step: "copying", progress: 0, bytes_done: 0, bytes_total: 0, renderer_step: "",
    disc_name: "Gran Turismo 4 (USA v2.00)", problem: "", note: "" };
  function running(step, progress) {
    setup.state = "checking";
    setup.step = step;
    setup.progress = progress;
    // As the Rust thread reports: the steps after the copy keep the finished copy's size (done == total).
    setup.bytes_total = step === "renderer" ? 353 : IMAGE_BYTES;
    setup.bytes_done = step === "copying" || step === "renderer" ? Math.round(setup.bytes_total * progress) : IMAGE_BYTES;
    setup.renderer_step = step === "renderer" ? "compiling" : "";
  }
  if (setupStart === "wrong") {
    setup.state = "wrong_disc";
    setup.disc_name = "Gran Turismo 4 Spec II v1.10.iso";
    setup.problem = SPEC_MESSAGE;
  } else if (setupStart === "needed") {
    setup.state = "needed";
    setup.disc_name = "";
  } else if (setupStart === "copying" || setupStart === "checking") {
    running("copying", 0.62);
  } else if (setupStart === "preparing") {
    running("preparing", 0.5);
  } else if (setupStart === "renderer") {
    running("renderer", 0.4);
  } else if (setupStart === "failed") {
    setup.state = "failed";
    setup.problem = "Not enough free space to copy the game files: 5.8 GB needed, 2.1 GB free. Free up space and try again.";
  } else if (setupStart === "missing") {
    setup.state = "files_missing";
    setup.disc_name = "";
    setup.problem = "Game files missing (SCUS-97328.iso is missing). Choose your disc image again.";
  } else if (setupStart === "offline") {
    setup.note = "GSdx could not be downloaded, so the game cannot start yet. GT4Recomp tries again at the next start.";
  }

  // setup_start: walk the steps on a timer, like the Rust thread.
  var PLAN = [["copying", 0], ["copying", 0.35], ["copying", 0.7], ["copying", 1], ["checking", 1], ["preparing", 0.5],
    ["renderer", 0.3], ["renderer", 0.8], ["renderer", 1]];
  var setupTimer = null;
  function startSetup() {
    var index = 0;
    clearInterval(setupTimer);
    setupTimer = setInterval(function () {
      if (index >= PLAN.length) {
        clearInterval(setupTimer);
        setup.state = "done";
        setup.renderer_step = "done";
        setup.renderer_choice = "coregs20";
        var installedGame = gameById(setupGame || selectedGame);
        if (installedGame) {
          installedGame.installed = true;
          selectedGame = installedGame.id;
          setup.disc_name = installedGame.disc_name;
        }
        return;
      }
      running(PLAN[index][0], PLAN[index][1]);
      index += 1;
    }, 500);
  }

  var gameRunning = query.get("running") === "1";

  // Read by pages/home.js: focuses Play on mount. Only the dev harness sets it.
  root.GT4 = root.GT4 || {};
  root.GT4.devFocus = query.get("focus") || "";
  // ?intro=1 plays the 1.3.0 intro on every load of Home (the real launcher shows it once per version).
  root.GT4.devIntro = query.get("intro") === "1";
  root.GT4.devMock = true;
  var maxUpscale = 4;

  // Settings rows, the same shape as the Rust table.
  var DEFAULTS = {};
  var rows = [
    { id: "renderer", label: "Renderer", help: "GSdx draws the game on your GPU in its own process; it is the renderer GT4Recomp uses. CoreGS 2.0 is in development.",
      page: "basic", group: "graphics", control: "dropdown",
      values: [
        { value: "pcsx2", label: "GSdx" },
        { value: "coregs20", label: "CoreGS 2.0 (recommended)" }
      ],
      default: "pcsx2", current: "pcsx2" },
    // TASK-103: the GSdx Graphics API (the table row graphics_api; the Renderer row above is the engine). Vulkan is the
    // default; Vulkan and Direct3D 12 are recommended, Direct3D 11 is for older PCs. OpenGL is not offered yet.
    { visible_when: { id: "renderer", equals: "pcsx2" }, id: "graphics_api", label: "Graphics API (GSdx)", help: "Vulkan is the default and the best tested; try Direct3D 11 on older graphics cards.",
      page: "basic", group: "graphics", control: "dropdown",
      values: [
        { value: "vulkan", label: "Vulkan (recommended)" },
        { value: "d3d12", label: "Direct3D 12 (recommended)" },
        { value: "d3d11", label: "Direct3D 11 (older PCs)" }
      ],
      default: "vulkan", current: "vulkan" },
    // TASK-080 addendum: no Aspect ratio and no Internal resolution row. Render scale (50% to 200%) with the
    // resulting internal resolution as its readout (labels are filled by tableView from the chosen resolution).
    { id: "render_scale", label: "Render scale", help: "How sharp the picture is drawn. 100% draws it at least at your resolution; 50% is faster and softer, 200% is sharper and needs a much faster GPU.",
      page: "basic", group: "graphics", control: "slider",
      values: { min: 50, max: 200, step: 25, unit: "%" },
      default: 100, current: 100 },
    { id: "fxaa", label: "Anti-aliasing (FXAA)", help: "Smooths jagged edges with a fast filter. It softens fine detail a little. Off by default.",
      page: "basic", group: "graphics", control: "toggle", values: null, default: false, current: false },
    // TASK-186: Performance Mode is gone; its items are effect toggles on by default, then the two GSdx-only settings of
    // the old bundle (blending accuracy and the readback mode).
    { id: "effect_flares", label: "Sun glare and light flares", help: "The sun's glare and the night lights' flares. Turning them off is the biggest speed gain of the old Performance Mode.",
      page: "basic", group: "graphics", control: "toggle", values: null, default: true, current: true },
    { visible_when: { id: "renderer", equals: "coregs20" }, id: "effect_road_sheen", label: "Road sheen", help: "The shine over the road that GT4 draws as a full-screen pass.",
      page: "basic", group: "graphics", control: "toggle", values: null, default: true, current: true },
    { visible_when: { id: "renderer", equals: "pcsx2" }, id: "blending", label: "Blending accuracy", help: "How exactly the graphics card combines colours when the game blends two layers.",
      page: "basic", group: "graphics", control: "dropdown",
      values: [
        { value: "high", label: "High (default)" },
        { value: "minimum", label: "Minimum (faster)" }
      ],
      default: "high", current: "high" },
    { visible_when: { id: "renderer", equals: "pcsx2" }, id: "hw_download_mode", label: "Readbacks", help: "How the graphics plugin reads images back from the graphics card.",
      page: "basic", group: "graphics", control: "dropdown",
      values: [
        { value: "accurate", label: "Accurate (default)" },
        { value: "enabled-force-full", label: "Enabled, force full" },
        { value: "no-readbacks", label: "No readbacks" },
        { value: "unsynchronized", label: "Unsynchronized (faster, may drop effects)" },
        { value: "disabled", label: "Disabled" }
      ],
      default: "accurate", current: "accurate" },
    { visible_when: { id: "renderer", equals: "pcsx2" }, id: "cas_mode", label: "Sharpening (CAS)", help: "FidelityFX Contrast Adaptive Sharpening. Off leaves the picture as the game drew it.",
      page: "basic", group: "graphics", control: "dropdown",
      values: [
        { value: "off", label: "Off" },
        { value: "sharpen-only", label: "Sharpen only" },
        { value: "sharpen-and-resize", label: "Sharpen and resize" }
      ],
      default: "off", current: "off" },
    { visible_when: { id: "renderer", equals: "pcsx2" }, id: "cas_sharpness", label: "Sharpening amount", help: "How strong the CAS sharpening is. Used when CAS is on.",
      page: "basic", group: "graphics", control: "slider",
      values: { min: 0, max: 100, step: 1, unit: "%" },
      default: 50, current: 50 },
    { id: "monitor", label: "Monitor", help: "The screen the game opens on. The primary monitor is used until you pick another one.",
      page: "basic", group: "display", control: "dropdown", values: [], default: "primary", current: "primary" },
    { id: "resolution", label: "Resolution", help: "The size of the picture, from the sizes your monitor supports. The shape of the screen follows it.",
      page: "basic", group: "display", control: "dropdown", values: [], default: "desktop", current: "desktop" },
    { id: "fullscreen", label: "Fullscreen", help: "Start in fullscreen. Alt+Enter or F11 switches while playing.",
      page: "basic", group: "display", control: "toggle", values: null, default: false, current: true },
    { id: "vsync", label: "VSync", help: "On keeps motion smooth with no tearing. Off lowers latency and may tear.",
      page: "basic", group: "display", control: "dropdown",
      values: [
        { value: "on", label: "On" },
        { value: "off", label: "Off" }
      ],
      default: "on", current: "on" },
    { visible_when: { id: "renderer", equals: "coregs20" }, id: "performance_hud", label: "Performance HUD", help: "Shows a panel in the corner while you play. FPS only shows just the frame rate. Full shows the names of your graphics card and processor, the frame rate and frame time with a live graph, the game's use of the processor, the graphics card and memory, and the state of the performance report when that is turned on.",
      page: "advanced", control: "dropdown", values: [{ value: "off", label: "Off" }, { value: "fps", label: "FPS only" }, { value: "full", label: "Full" }], default: "off", current: "off" },
    { id: "performance_report", label: "Performance report", help: "Records frame timing while you play for bug reports.",
      page: "advanced", control: "toggle", values: null, default: false, current: false },
    { visible_when: { id: "renderer", equals: "coregs20" }, id: "frame_rate", label: "Frame rate (experimental)", help: "60 FPS is the default. The 30 FPS mode is for weaker PCs and is still experimental: races draw 30 frames per second, with the game at full speed and the same physics. Menus stay at 60.", page: "basic", group: null, control: "dropdown", values: [{ value: "60", label: "60 FPS" }, { value: "30", label: "30 FPS" }], default: "60", current: "60", connected: true },
    // TASK-174: ?queues=2 is a card with two graphics queues (the row is shown but not available); the default is one queue.
    { visible_when: { id: "renderer", equals: "coregs20" }, id: "present_thread", label: "Presentation thread (one-queue GPUs)",
      help: "For the Steam Deck and other graphics cards that have a single queue for graphics work. The game shows finished frames from a separate thread, so it does not wait for the screen. On other graphics cards this changes nothing: they already do it.",
      page: "basic", group: "graphics", control: "toggle", values: null, default: true, current: true,
      disabled: query.get("queues") === "2", note_key: query.get("queues") === "2" ? "settings.already_active" : undefined },
    { visible_when: { id: "renderer", equals: "coregs20" }, id: "coregs_sharpening", label: "Sharpening (FSR1)",
      help: "AMD FidelityFX Super Resolution 1 sharpening of the final picture. Off leaves the picture as the game drew it.",
      page: "basic", group: "graphics", control: "dropdown",
      values: [
        { value: "off", label: "Off" },
        { value: "mild", label: "Mild" },
        { value: "medium", label: "Medium" },
        { value: "strong", label: "Strong" }
      ],
      default: "off", current: "off" },
    { visible_when: { id: "renderer", equals: "pcsx2" }, id: "perf_overlay", label: "Performance overlay", help: "Shows GT4Recomp's own overlay while you play (FPS, frame time, CPU and GPU use).",
      page: "advanced", control: "toggle", values: null, default: false, current: false },
    { visible_when: { id: "renderer", equals: "pcsx2" }, id: "perf_overlay_fps", label: "Overlay: FPS", help: "Shows the frame rate in the performance overlay.",
      page: "advanced", control: "toggle", values: null, default: true, current: true },
    { id: "debug_internal", label: "Internal debug", help: "Not shown to users.",
      page: "hidden", control: "toggle", values: null, default: false, current: true },
    // TASK-091 (1.2.1): the launcher's own row, appended by the Rust table (commands::get_settings_table).
    { id: "launcher_gpu", label: "Launcher GPU acceleration", help: "Off keeps the launcher window without GPU acceleration, after it failed to draw in WebView2. On takes effect at the next start of the launcher.",
      page: "advanced", control: "toggle", values: null, default: true, current: true }
  ];
  rows.forEach(function (row) {
    DEFAULTS[row.id] = row.default;
  });

  // Two sample monitors (the Rust table lists the connected ones; see model::settings_table_with_monitors).
  var MONITORS = [
    { id: "\\\\.\\DISPLAY1@1920x1080", label: "TV \u2013 1920x1080 (primary)", desktop: "1920x1080",
      modes: ["1920x1080", "1600x900", "1280x720", "1024x768", "800x600"] },
    { id: "\\\\.\\DISPLAY2@3440x1440", label: "DELL U3419W \u2013 3440x1440", desktop: "3440x1440",
      modes: ["3440x1440", "2560x1080", "1920x1080", "1680x1050", "1280x720"] }
  ];
  // The rule of gt4l_core::display::display_plan: k = ceil(max(W / 640, H / 448)), times the scale, ties up, 1..12.
  function renderLabels(output) {
    var size = output.split("x").map(Number);
    var k = Math.max(Math.ceil(size[0] / 640), Math.ceil(size[1] / 448));
    var labels = {};
    for (var step = 50; step <= 200; step += 25) {
      var m = Math.min(12, Math.max(1, Math.floor((2 * k * step + 100) / 200)));
      labels[String(step)] = step + "% \u00B7 " + m + "x \u00B7 " + (640 * m) + "x" + (448 * m);
    }
    return labels;
  }
  function fillDisplayRows() {
    var monitorRow = findRow("monitor");
    var chosen = MONITORS.filter(function (m) { return m.id === monitorRow.current; })[0] || MONITORS[0];
    monitorRow.current = chosen.id;
    monitorRow.values = MONITORS.map(function (m) { return { value: m.id, label: m.label }; });
    var resolutionRow = findRow("resolution");
    if (chosen.modes.indexOf(resolutionRow.current) < 0) {
      resolutionRow.current = chosen.desktop;
    }
    resolutionRow.default = chosen.desktop;
    resolutionRow.values = chosen.modes.map(function (mode) {
      return { value: mode, label: mode === chosen.desktop ? mode + " (desktop)" : mode };
    });
    findRow("render_scale").values.labels = renderLabels(resolutionRow.current);
  }

  // The Settings > General row (TASK-102): the launcher's language, as the table gives it.
  function languageRow() {
    return {
      id: "language", label: "Language", group: "general", page: "basic", control: "dropdown",
      help: "The language of the launcher.", default: "auto", current: mockLanguage,
      values: [
        { value: "auto", label: "Auto" },
        { value: "en-US", label: "English" },
        { value: "pt-BR", label: "Português (Brasil)" }
      ]
    };
  }

  // The Settings > General update channel row (TASK-109). The choice is kept in the mock like the real setting.
  var mockChannel = "stable";
  function channelRow() {
    return {
      id: "update_channel", label: "Channel", group: "general", page: "basic", control: "dropdown",
      help: "Stable gets the normal releases. Beta also gets the release candidates.", default: "stable", current: mockChannel,
      values: [{ value: "stable", label: "Stable" }, { value: "beta", label: "Beta" }]
    };
  }
  function discordRow() {
    return {
      id: "discord_rich_presence", label: "Discord Rich Presence", group: "general", page: "basic", control: "toggle",
      help: "Shows that you are playing in your Discord profile while a game runs.", default: true, current: true
    };
  }

  // TASK-109 version picker: a fake GitHub release list with stable releases, candidates and drafts that must not show.
  // The mock applies the same rules as gt4l-core version_list.rs (minimum 1.3.0-rc5, drafts out, dates for candidates,
  // SemVer for stables, a newer stable on top in Beta). devVersions.installed can be changed by the checks.
  var devVersions = root.GT4.devVersions = {
    installed: "1.3.0-rc6",
    installs: [],
    failList: false,
    releases: [
      { tag: "v1.3.2", published: "2026-10-30T10:00:00Z", draft: false },
      { tag: "v1.3.1-rc2", published: "2026-10-28T10:00:00Z", draft: false },
      { tag: "v1.3.1-rc3", published: null, draft: true },
      { tag: "v1.3.0", published: "2026-10-26T10:00:00Z", draft: false },
      { tag: "v1.3.0-rc6", published: "2026-10-25T09:00:00Z", draft: false },
      { tag: "v1.3.0-rc5", published: "2026-10-23T09:00:00Z", draft: false },
      { tag: "v1.3.0-rc4", published: "2026-10-22T09:00:00Z", draft: false },
      { tag: "v1.2.3", published: "2026-10-01T09:00:00Z", draft: false }
    ]
  };
  function parseRelease(text) {
    var match = /^v?(\d+)\.(\d+)\.(\d+)(?:-rc(\d+))?$/.exec(text);
    return match ? { nums: [+match[1], +match[2], +match[3]], rc: match[4] === undefined ? null : +match[4], text: String(text).replace(/^v/, "") } : null;
  }
  function compareRelease(a, b) {
    for (var i = 0; i < 3; i++) {
      if (a.nums[i] !== b.nums[i]) { return a.nums[i] - b.nums[i]; }
    }
    if (a.rc === b.rc) { return 0; }
    if (a.rc === null) { return 1; }
    if (b.rc === null) { return -1; }
    return a.rc - b.rc;
  }
  var PICKER_MINIMUM = parseRelease("1.3.0-rc5");
  function versionList() {
    var installed = parseRelease(devVersions.installed);
    var usable = devVersions.releases.filter(function (release) {
      return !release.draft && compareRelease(parseRelease(release.tag), PICKER_MINIMUM) >= 0;
    }).map(function (release) {
      var version = parseRelease(release.tag);
      return {
        tag: release.tag, version: version.text, date: (release.published || "").slice(0, 10), candidate: version.rc !== null,
        installed: compareRelease(version, installed) === 0, downgrade: compareRelease(version, installed) < 0,
        _v: version, _p: release.published || ""
      };
    });
    var stables = usable.filter(function (entry) { return !entry.candidate; }).sort(function (a, b) { return compareRelease(b._v, a._v); });
    var entries = stables;
    if (mockChannel === "beta") {
      var candidates = usable.filter(function (entry) { return entry.candidate; }).sort(function (a, b) {
        return a._p < b._p ? 1 : (a._p > b._p ? -1 : compareRelease(b._v, a._v));
      });
      var newest = candidates.reduce(function (best, entry) { return !best || compareRelease(entry._v, best) > 0 ? entry._v : best; }, null);
      entries = stables.filter(function (entry) { return !newest || compareRelease(entry._v, newest) > 0; }).concat(candidates);
    }
    var nothingNewer = entries.every(function (entry) { return compareRelease(entry._v, installed) < 0; }) &&
      !entries.some(function (entry) { return entry.installed; });
    return {
      channel: mockChannel, installed: devVersions.installed, nothingNewer: nothingNewer,
      entries: entries.map(function (entry) {
        return { tag: entry.tag, version: entry.version, date: entry.date, candidate: entry.candidate, installed: entry.installed, downgrade: entry.downgrade };
      })
    };
  }

  function tableView() {
    fillDisplayRows();
    return {
      max_upscale: maxUpscale,
      rows: [languageRow(), channelRow(), discordRow()].concat(rows.map(function (row) {
        return JSON.parse(JSON.stringify(row));
      }))
    };
  }

  function findRow(id) {
    for (var i = 0; i < rows.length; i++) {
      if (rows[i].id === id) {
        return rows[i];
      }
    }
    return null;
  }

  // Demo offers for the update popup (?update=1). Notes are sample text, not a real release.
  var DEMO_OFFERS = [
    { stream: "renderer", installed: "1.2.1", latest: "1.2.2", update: true,
      package: "gt4-gs-1.1.1-windows-x64.zip",
      notes: "## gt4-gs 1.1.1\n\n- **Faster** present on VSync Off.\n- Fixes a crash on exit when the game ends first.\n- Sample text for the launcher preview."},
    { stream: "launcher", installed: "1.3.0", latest: "1.3.1", update: true,
      package: "GT4Recomp-Setup-1.1.1.exe",
      notes: "GT4Recomp 1.1.1\n\n* Tray: the launcher comes back when the game ends.\n* Sample text for the launcher preview." }
  ];
  // Controls: the built-in mapping of docs/CONTROLS.md, with the same assign/swap rules as gt4controls (Rust).
  var CONTROL_ORDER = ["dpad_up", "dpad_right", "dpad_down", "dpad_left", "cross", "circle", "square", "triangle",
    "l1", "r1", "l2", "r2", "l3", "r3", "select", "start"];
  var CONTROL_NAMES = { dpad_up: "D-pad up", dpad_right: "D-pad right", dpad_down: "D-pad down", dpad_left: "D-pad left",
    cross: "Cross", circle: "Circle", square: "Square", triangle: "Triangle", l1: "L1", r1: "R1", l2: "L2", r2: "R2",
    l3: "L3", r3: "R3", select: "Select", start: "Start" };
  var DEFAULT_SOURCES = {
    dpad_up: ["xinput:DpadUp", "key:Up"], dpad_right: ["xinput:DpadRight", "key:Right"],
    dpad_down: ["xinput:DpadDown", "key:Down"], dpad_left: ["xinput:DpadLeft", "key:Left"],
    cross: ["xinput:A", "key:Z"], circle: ["xinput:B", "key:X"], square: ["xinput:X", "key:A"], triangle: ["xinput:Y", "key:S"],
    l1: ["xinput:LB", "key:Q"], r1: ["xinput:RB", "key:W"], l2: ["xinput:LT", "key:1"], r2: ["xinput:RT", "key:2"],
    l3: ["xinput:LThumb", "key:C"], r3: ["xinput:RThumb", "key:V"], select: ["xinput:Back", "key:Backspace"],
    start: ["xinput:Start", "key:Enter"]
  };
  var RESERVED = { F7: true, F8: true, F9: true, F11: true };
  var bindings = JSON.parse(JSON.stringify(DEFAULT_SOURCES));
  // ?remap=1: a few changed buttons, so the "Changed" state shows in screenshots.
  if (query.get("remap") === "1") {
    bindings.cross = ["xinput:B", "key:Z"];
    bindings.circle = ["xinput:A", "key:X"];
    bindings.select = ["xinput:Back"];
  }

  function analogDefaults() {
    var curve = [
      { value: "linear", label: "Linear" },
      { value: "s_curve", label: "S-Curve" },
      { value: "smooth_power", label: "Smooth Power" },
      { value: "quadratic", label: "Quadratic" },
      { value: "cubic", label: "Cubic" },
      { value: "sublinear", label: "Sublinear" },
      { value: "logarithmic", label: "Logarithmic" }
    ];
    function slider(id, label, def) {
      return { id: id, label: label, help: "How far it must travel before it counts, in percent of its travel.",
        page: "basic", group: "controls", control: "slider", values: { min: 0, max: 95, step: 1, unit: "%" },
        default: def, current: def };
    }
    function dropdown(id, label, values) {
      return { id: id, label: label, help: "How the pressure grows with the trigger travel.", page: "basic",
        group: "controls", control: "dropdown", values: values || curve, default: (values || curve)[0].value, current: (values || curve)[0].value };
    }
    // TASK-161: the full trigger range toggle, first of the Controls page's analog rows (model.rs), on by default.
    var fullRange = { id: "trigger_full_range", label: "Full trigger range",
      help: "Throttle and brake follow the whole travel of the triggers in finer steps.", page: "basic", group: "controls",
      control: "toggle", values: null, default: true, current: query.get("fullrange") !== "0" };
    return [fullRange, slider("l2_deadzone", "L2 deadzone", 11.8), dropdown("l2_curve", "L2 curve", curve),
      slider("r2_deadzone", "R2 deadzone", 11.8), dropdown("r2_curve", "R2 curve", curve),
      slider("lstick_deadzone", "Left stick deadzone", 24), slider("rstick_deadzone", "Right stick deadzone", 26.5)];
  }
  var analog = analogDefaults();

  function base(source) { return String(source).split("|")[0].trim(); }
  function familyOf(source) { return base(source).split(":")[0]; }
  function nameOf(source) { return base(source).split(":")[1] || ""; }

  function controlsView() {
    return {
      controls: CONTROL_ORDER.map(function (id) {
        var sources = bindings[id].slice();
        var pad = sources.filter(function (s) { return familyOf(s) === "xinput"; })[0];
        var key = sources.filter(function (s) { return familyOf(s) === "key"; })[0];
        return { id: id, name: CONTROL_NAMES[id], binding: sources.length ? nameOf(sources[0]) : "-", sources: sources,
          xinput: pad ? nameOf(pad) : null, key: key ? nameOf(key) : null,
          is_default: JSON.stringify(sources) === JSON.stringify(DEFAULT_SOURCES[id]) };
      }),
      triggers: JSON.parse(JSON.stringify(analog)),
      locked: query.get("locked") === "1"
    };
  }

  function ordered(list) {
    return list.filter(function (s) { return familyOf(s) === "xinput"; }).concat(list.filter(function (s) { return familyOf(s) !== "xinput"; }));
  }

  // The rules of gt4controls::Controls::assign: same family replaced, swap with the holder, refusal without swap.
  function assignBinding(id, source, swap) {
    var b = base(source);
    if (!bindings[id]) {
      throw new Error("'" + id + "' is not a button input");
    }
    if (familyOf(b) === "key" && RESERVED[nameOf(b)]) {
      throw new Error("key '" + nameOf(b) + "' is reserved by the launcher");
    }
    var current = bindings[id];
    if (current.some(function (s) { return base(s) === b; })) {
      var same = controlsView();
      same.swapped_with = null;
      same.unchanged = true;
      return same;
    }
    var previous = current.filter(function (s) { return familyOf(s) === familyOf(b); })[0] || null;
    var holder = CONTROL_ORDER.filter(function (other) {
      return other !== id && bindings[other].some(function (s) { return base(s) === b; });
    })[0] || null;
    if (holder) {
      if (!swap) {
        throw new Error("'" + b + "' is already bound to " + holder);
      }
      var replaced = [];
      bindings[holder].forEach(function (s) {
        var incoming = base(s) === b ? previous : s;
        if (incoming && !replaced.some(function (kept) { return base(kept) === base(incoming); })) {
          replaced.push(incoming);
        }
      });
      bindings[holder] = ordered(replaced);
    }
    bindings[id] = ordered(current.filter(function (s) { return familyOf(s) !== familyOf(b); }).concat([b]));
    var view = controlsView();
    view.swapped_with = holder;
    view.unchanged = false;
    return view;
  }

  // Dev-only live input for the Controls page: ?pad=A,LB,DpadRight&lt=0.6&rt=0&lx=0.7&ly=-0.45&rx=-0.3&ry=0.6&pads=2
  // builds a still Gamepad-like object (standard mapping). ?nopad=1 shows "No controller".
  var STANDARD = ["A", "B", "X", "Y", "LB", "RB", "LT", "RT", "Back", "Start", "LThumb", "RThumb", "DpadUp", "DpadDown",
    "DpadLeft", "DpadRight"];
  function devPad(index) {
    var pressed = (query.get("pad") || "").split(",").filter(Boolean);
    var buttons = STANDARD.map(function (name) {
      var down = pressed.indexOf(name) >= 0;
      var value = down ? 1 : 0;
      if (name === "LT" && query.get("lt")) { value = Number(query.get("lt")); }
      if (name === "RT" && query.get("rt")) { value = Number(query.get("rt")); }
      return { pressed: value > 0.12, value: value };
    });
    var axes = ["lx", "ly", "rx", "ry"].map(function (key) { return Number(query.get(key) || 0); });
    return { id: "Xbox Wireless Controller (XInput STANDARD GAMEPAD)", index: index, connected: true,
      mapping: "standard", buttons: buttons, axes: axes };
  }
  if (query.get("pad") !== null || query.get("lt") !== null || query.get("lx") !== null || query.get("pads") !== null) {
    var devPads = [devPad(0)];
    if (query.get("pads") === "2") {
      var second = devPad(1);
      second.id = "Controller (Gamepad for Xbox 360)";
      devPads.push(second);
    }
    root.GT4.devGamepads = function () { return devPads; };
  } else if (query.get("nopad") === "1") {
    root.GT4.devGamepads = function () { return []; };
  }
  // ?select=circle&mode=key&capture=hold|conflict (Controls screenshots; capture starts once the page is entered).
  root.GT4.devControls = {
    select: query.get("select") || "",
    mode: query.get("mode") || "",
    capture: query.get("capture") || ""
  };

  var pendingCapture = null;
  function captureBinding() {
    return new Promise(function (resolve, reject) {
      pendingCapture = reject;
      var mode = root.GT4.devControls.capture;
      if (mode === "hold") {
        return; // waits until cancel_capture
      }
      setTimeout(function () {
        if (pendingCapture === reject) {
          pendingCapture = null;
          resolve({ control: "", source: mode === "conflict" ? "xinput:LB" : "xinput:RB" });
        }
      }, mode === "conflict" ? 200 : 2000);
    });
  }

  // One library per game (TASK-101): saves and replays are never shared. A save is listed with its game id: `gt4:Save 01`.
  var libraries = {
    gt4: {
      saves: [
        { slot: "Save 01", name: "Save 01", modified: "2026-10-16T06:31:00", active: true },
        { slot: "Save 02", name: "Save 02", modified: "2026-10-12T22:14:00", active: false },
        { slot: "Save 03", name: "Save 03", modified: "2026-09-03T19:02:00", active: false }
      ],
      replays: [
        { id: "01", name: "Replay 01", modified: "2026-10-16T21:40:00" },
        { id: "02", name: "Replay 02", modified: "2026-10-16T20:55:00" },
        { id: "03", name: "Replay 03", modified: "2026-10-12T22:31:00" }
      ]
    },
    spec2: {
      saves: [
        { slot: "Save 01", name: "Save 01", modified: "2026-10-22T18:05:00", active: true }
      ],
      replays: []
    }
  };
  function libraryOf(id) { return libraries[id || selectedGame]; }
  // The Saves list of both games, in table order: each row carries its game id (the page shows the tag).
  function savesList() {
    var rows = [];
    ["gt4", "spec2"].forEach(function (gameId) {
      libraries[gameId].saves.forEach(function (save) {
        rows.push({ id: gameId + ":" + save.slot, slot: save.slot, name: save.name, modified: save.modified,
          active: !!save.active, renamed: !!save.renamed, game: gameId });
      });
    });
    return rows;
  }
  // "gt4:Save 02" -> { game: "gt4", slot: "Save 02" }.
  function parseSaveId(id) {
    var at = String(id).indexOf(":");
    return { game: String(id).slice(0, at), slot: String(id).slice(at + 1) };
  }
  function saveOf(id) {
    var parts = parseSaveId(id);
    return libraries[parts.game].saves.filter(function (save) { return save.slot === parts.slot; })[0];
  }
  function libraryView(kind) {
    var game = gameById(selectedGame);
    var items = kind === "saves" ? savesList() : JSON.parse(JSON.stringify(libraryOf()[kind]));
    return { items: items, game: game.id, gameName: game.name, status: "",
      folder: "C:\\Users\\Player\\AppData\\Roaming\\GT4Recomp\\games\\" + game.id + (kind === "saves" ? "\\Saves" : "\\Replays") };
  }

  function appState() {
    var view = gamesView();
    var snapshot = JSON.parse(JSON.stringify(setup));
    // Without a ?setup= override the setup section describes the selected game (installed: done; else: needed).
    if (setupStart === "done" && snapshot.state === "done" && !gameById(selectedGame).installed) {
      snapshot.state = "needed";
      snapshot.disc_name = "";
    }
    var target = setupGame || (view.any_installed ? selectedGame : null);
    snapshot.game = target ? { id: target, name: gameById(target).name } : null;
    return { setup: snapshot, game_running: gameRunning, selected_game: view.selected, language: mockLanguage,
      any_installed: view.any_installed, games: view.games,
      game_files: "C:\\Program Files\\GT4Recomp\\data\\games\\" + selectedGame + "\\disc",
      migration: { running: false, item: "", bytes_done: 0, bytes_total: 0, notice: "", problem: false } };
  }

  // Synthetic RetroAchievements data belongs only to this dev backend; it never contacts a server.
  var raGame = {
    game_id: 20580, title: "Gran Turismo 4", total_achievements: 3, unlocked_achievements: 2,
    total_points: 35, unlocked_points: 15,
    achievements: [
      { id: 1001, title: "National B License", description: "Acquire the National B License.", points: 10,
        unlocked: true, unlocked_hardcore: true, badge_name: "", type_label: "Progression" },
      { id: 1002, title: "First Victory", description: "Win your first race.", points: 5,
        unlocked: true, unlocked_hardcore: false, badge_name: "", type_label: "" },
      { id: 1003, title: "Championship Champion", description: "Win the championship.", points: 20,
        unlocked: false, unlocked_hardcore: false, badge_name: "", type_label: "" }
    ]
  };
  var raState = {
    enabled: true,
    hardcore: false,
    encore: false,
    signedIn: query.get("ra") === "signedin",
    username: "",
    displayName: "",
    score: 0,
    scoreSoftcore: 0,
    game: null,
    selectedGame: "gt4",
    supported: true
  };
  function raSignIn(username) {
    raState.signedIn = true;
    raState.username = raState.displayName = username;
    raState.score = 1250;
    raState.scoreSoftcore = 400;
    raState.game = raGame;
  }
  if (raState.signedIn) raSignIn("DemoDriver");

  function raView() {
    var cur = selectedGame || "gt4";
    var sup = cur === "gt4";
    var v = JSON.parse(JSON.stringify(raState));
    v.selectedGame = cur;
    v.supported = sup;
    if (!sup) {
      v.game = null;
    }
    return v;
  }

  function invoke(command, args) {
    args = args || {};
    if (command === "ra_login") {
      if (!args.username || !args.password) throw new Error("Enter a mock username and password.");
      raSignIn(args.username);
      return raView();
    } else if (command === "ra_logout") {
      raState.signedIn = false;
      raState.username = raState.displayName = "";
      raState.score = raState.scoreSoftcore = 0;
      raState.game = null;
      return raView();
    } else if (command === "ra_set_options") {
      raState.enabled = !!args.enabled;
      raState.hardcore = !!args.hardcore;
      raState.encore = !!args.encore;
      return raView();
    } else if (command === "ra_status") {
      return raView();
    }
    switch (command) {
      case "app_state":
        return appState();
      case "setup_start":
        running("copying", 0);
        startSetup();
        return null;
      case "setup_choose_disc": {
        if (query.get("pick") === "spec") {
          setup.state = "wrong_disc";
          setup.disc_name = "Gran Turismo 4 (USA) (Beta).iso";
          setup.problem = SPEC_MESSAGE;
          throw new Error(SPEC_MESSAGE);
        }
        // The disc decides the game; "pick=other" is always the one that was not asked for, "pick=spec2" is Spec II.
        var asked = args.game || null;
        var pick = query.get("pick");
        var discGame = pick === "spec2" ? "spec2" : (pick === "other" && asked ? (asked === "gt4" ? "spec2" : "gt4") : (asked || "gt4"));
        setupGame = discGame;
        setup.disc_name = discGame === "spec2" ? "Gran Turismo 4 Spec II v1.10.iso" : "Gran Turismo 4 (USA) (v2.00).iso";
        setup.problem = "";
        return { path: "D:\\Games\\" + setup.disc_name, serial: discGame === "spec2" ? "SCUS-97436" : "SCUS-97328",
          version: discGame === "spec2" ? "1.10" : "2.00", name: gameById(discGame).disc_name, game: discGame,
          gameName: gameById(discGame).name, mismatch: !!asked && asked !== discGame, requested: asked };
      }
      case "games_list":
        return gamesView();
      case "select_game":
        if (!gameById(args.id)) {
          throw new Error(args.id + " is not a game of this launcher.");
        }
        if (gameRunning) {
          throw new Error("Stop the game before choosing another one.");
        }
        selectedGame = args.id;
        return gamesView();
      case "games_remove":
        if (!gameById(args.id)) {
          throw new Error(args.id + " is not a game of this launcher.");
        }
        gameById(args.id).installed = false;
        return gamesView();
      case "setup_cancel":
        clearInterval(setupTimer);
        setup.state = "failed";
        setup.problem = "Setup was cancelled. Nothing was changed.";
        return null;
      case "prepare_shaders":
        // Demo: ?shaders=retry counts a prepare that stops twice (TASK-170b): attempts 1 and 2 end at 40 and 90 of 200.
        if (query.get("shaders") === "retry") {
          [[40, 1], [90, 2], [0, 3]].forEach(function (step, index) {
            setTimeout(function () { emit("shaders-progress", { done: step[0], total: 200, attempt: step[1] }); }, 400 * (index + 1));
          });
          return new Promise(function (resolve) { setTimeout(function () { resolve(true); }, 1800); });
        }
        return true;
      case "cancel_shader_prepare":
        return null;
      case "play":
        if (!gameById(selectedGame).installed) {
          throw new Error("Set up the game first.");
        }
        gameRunning = true;
        return null;
      case "first_start_hint":
        // TASK-164: ?firstStart=1 shows the first-start note on Play (no save yet).
        return query.get("firstStart") === "1" ? { game: "Gran Turismo 4", confirm: "W / Enter", pad: "A", right: "Right" } : null;
      case "stop_game":
        gameRunning = false;
        return null;
      case "get_settings_table":
        return tableView();
      case "gsdx_download_needed":
        return query.get("gsdx") === "missing" || query.get("gsdx") === "offline";
      case "coregs20_offer":
        // As launch::offers_coregs20: GSdx stored, not installed, and a card with Vulkan 1.2.
        return (query.get("gsdx") === "missing" || query.get("gsdx") === "offline") &&
          findRow("renderer").current === "pcsx2" && query.get("vulkan") !== "no";
      case "use_coregs20":
        if (query.get("vulkan") === "no") {
          throw new Error("CoreGS 2.0 could not be selected. Choose it in Settings > Graphics > Renderer.");
        }
        findRow("renderer").current = "coregs20";
        return null;
      case "download_gsdx":
        return new Promise(function (resolve, reject) {
          emit("gsdx-download", { step: "listing" });
          var done = 0;
          var timer = setInterval(function () {
            done += 12000000;
            emit("gsdx-download", { step: "downloading", bytesDone: done, bytesTotal: 48000000 });
            if (query.get("gsdx") === "offline") {
              clearInterval(timer);
              reject(new Error("GSdx could not be downloaded: no internet connection. Check your connection and try again."));
            } else if (done >= 48000000) {
              clearInterval(timer);
              emit("gsdx-download", { step: "installing" });
              setTimeout(resolve, 500);
            }
          }, 500);
        });
      case "install_gsdx_archive":
        // TASK-158: the file picker and the install; `gsdxfile` chooses the result (default: refused).
        return new Promise(function (resolve, reject) {
          var choice = query.get("gsdxfile") || "bad";
          if (choice === "cancel") {
            resolve(null);
            return;
          }
          emit("gsdx-download", { step: "installing" });
          setTimeout(function () {
            if (choice === "ok") {
              resolve(true);
            } else {
              reject(new Error("gt4-gs-1.2.3-windows-x64.zip does not match the checksum published with its release, so it was not installed. The file may be damaged or changed: download it again."));
            }
          }, 500);
        });
      case "set_setting": {
        if (args.id === "controller") {
          mockController = args.value; // TASK-148: the Controls page's controller row
          return null;
        }
        if (args.id === "language") {
          mockLanguage = args.value;
          return null;
        }
        if (args.id === "update_channel") {
          mockChannel = args.value;
          return null;
        }
        var row = findRow(args.id) || analog.filter(function (item) { return item.id === args.id; })[0];
        if (!row) {
          throw new Error("Unknown setting " + args.id);
        }
        row.current = args.value;
        if (args.id === "monitor") {
          // As the launcher: another monitor's Resolution goes back to its desktop mode.
          findRow("resolution").current = "desktop";
        }
        // The real set_setting returns nothing; the UI reads the table again.
        return null;
      }
      case "reset_gs_settings_to_defaults":
        rows.forEach(function (row) {
          if (row.page === "basic" || row.page === "advanced") {
            row.current = row.default;
          }
        });
        return tableView();
      case "pad_status":
        return padStatusPayload();
      case "get_controls":
        return controlsView();
      case "capture_binding":
        return captureBinding();
      case "cancel_capture":
        if (pendingCapture) {
          var rejectCapture = pendingCapture;
          pendingCapture = null;
          rejectCapture(new Error("capture cancelled"));
        }
        return null;
      case "assign_binding":
        return assignBinding(args.control, args.source, !!args.swap);
      case "reset_binding":
        bindings[args.control] = DEFAULT_SOURCES[args.control].slice();
        return null;
      case "reset_all_bindings":
        bindings = JSON.parse(JSON.stringify(DEFAULT_SOURCES));
        analog = analogDefaults();
        return controlsView();
      case "saves_list":
        return libraryView("saves");
      case "saves_import":
      case "replays_import":
        // Nothing is asked: each save names its game and goes to that game's library.
        return libraryView(command === "saves_import" ? "saves" : "replays");
      case "saves_activate": {
        var activeParts = parseSaveId(args.id);
        libraries[activeParts.game].saves.forEach(function (save) {
          save.active = save.slot === activeParts.slot;
        });
        return libraryView("saves");
      }
      case "saves_delete": {
        var deleteParts = parseSaveId(args.id);
        libraries[deleteParts.game].saves = libraries[deleteParts.game].saves.filter(function (save) {
          return save.slot !== deleteParts.slot;
        });
        return libraryView("saves");
      }
      case "saves_new": {
        // The active save of that game is parked as a slot; its next Play starts a new profile (the real launcher keeps it).
        var newGame = args.game || selectedGame;
        libraries[newGame].saves.forEach(function (save) {
          save.active = false;
        });
        var fresh = libraryView("saves");
        fresh.status = gameById(newGame).name + ": Started a new save. The old one is kept as a slot.";
        return fresh;
      }
      case "saves_rename": {
        var renamedSave = saveOf(args.id);
        if (!renamedSave) {
          throw new Error("That save is not in the library any more.");
        }
        // The same cleaning as the launcher: no characters a file name cannot hold, one space between words, 40 at most.
        var cleaned = String(args.name || "").replace(/[<>:"\/\\|?*\u0000-\u001f]/g, "").replace(/\s+/g, " ").trim().slice(0, 40).trim();
        if (!cleaned) {
          throw new Error("Type a name with letters or numbers.");
        }
        renamedSave.name = cleaned;
        renamedSave.renamed = true;
        var renamedView = libraryView("saves");
        renamedView.status = "Renamed to " + cleaned + ".";
        return renamedView;
      }
      case "saves_export": {
        var exported = libraryView("saves");
        exported.status = "Saved to C:\\Users\\Player\\Desktop\\save.psu.";
        return exported;
      }
      case "replays_list":
        return libraryView("replays");
      case "replays_delete":
        libraryOf().replays = libraryOf().replays.filter(function (replay) {
          return replay.id !== args.id;
        });
        return libraryView("replays");
      case "release_notes":
        // 1.3.0 is sample text; the older entries are copies of docs/release-notes/launcher/<version>.md (the real build
        // embeds every short notes file up to its own version, newest first).
        return {
          version: "1.3.0",
          releases: [
            { version: "1.3.0", markdown: "# GT4Recomp 1.3.0\n\n**Gran Turismo 4 Spec II comes to GT4Recomp.**\n\n## New\n- **Spec II 1.10** runs next to the normal game, with upscaling and texture packs.\n- Pick the version under **Play**: Normal or Spec II. Each keeps its own saves.\n- A **What's new** panel on the home screen, with every past update.\n- A portable version: carry the whole folder to another PC.\n\n## Good to know\n- Spec II needs your own Spec II 1.10 disc image (the Brazilian Portuguese edition works too).", translations: { "pt-BR": "# GT4Recomp 1.3.0\n\n**Gran Turismo 4 Spec II chega ao GT4Recomp.**\n\n## Novidades\n- **Spec II 1.10** roda ao lado do jogo normal, com upscaling e pacotes de texturas.\n- Escolha a versão em **Jogar**: Normal ou Spec II. Cada uma mantém os próprios saves.\n- Um painel **Novidades** na tela inicial, com todas as atualizações anteriores.\n- Uma versão portátil: leve a pasta inteira para outro PC.\n\n## Bom saber\n- O Spec II precisa da sua própria imagem de disco do Spec II 1.10 (a edição em português do Brasil também funciona)." } },
            { version: "1.2.3", markdown: "# GT4Recomp 1.2.3\n\n**Quick fixes and a desktop shortcut.**\n\n## Fixes\n- The game no longer crashes at start for players who were not on GSdx.\n\n## New\n- Texture packs can go straight into the Textures folder, with no extra subfolder.\n- The installer can create a **Gran Turismo 4** shortcut on your desktop.\n- GSdx is now the only renderer; our own renderer, CoreGS 2.0, is coming later.\n", translations: { "pt-BR": "# GT4Recomp 1.2.3\n\n**Correções rápidas e um atalho na área de trabalho.**\n\n## Correções\n- O jogo não trava mais ao iniciar para quem não estava usando o GSdx.\n\n## Novidades\n- Pacotes de texturas podem ir direto para a pasta Textures, sem subpasta extra.\n- O instalador pode criar um atalho **Gran Turismo 4** na sua área de trabalho.\n- O GSdx agora é o único renderizador; nosso próprio renderizador, o CoreGS 2.0, chega depois.\n" } },
            { version: "1.2.2", markdown: "# GT4Recomp 1.2.2\n\n**A lighter game on your processor.**\n\n## Performance\n- **About 15% less processor work** per frame in the heaviest races, so slower PCs keep 60 FPS more easily.\n- The picture is exactly the same as before.\n", translations: { "pt-BR": "# GT4Recomp 1.2.2\n\n**Um jogo mais leve para o seu processador.**\n\n## Desempenho\n- **Cerca de 15% menos trabalho do processador** por quadro nas corridas mais pesadas, então PCs mais lentos mantêm 60 FPS com mais facilidade.\n- A imagem continua exatamente igual à de antes.\n" } },
            { version: "1.2.1", markdown: "# GT4Recomp 1.2.1\n\n**A cleaner install and updates that just work.**\n\n## New\n- GT4Recomp now installs in Program Files, for every user of the PC, in any folder you pick.\n- Your settings and saves live in your Windows user folder; the big game files sit next to the program.\n- Updates install silently: one confirmation, and the launcher reopens by itself.\n- **Performance Mode** (Settings > Graphics): faster on weaker PCs, by skipping a few light effects.\n- Settings now sits next to Play on the home screen.\n\n## Fixes\n- The launcher no longer opens as a blank white window on some PCs.\n", translations: { "pt-BR": "# GT4Recomp 1.2.1\n\n**Uma instalação mais limpa e atualizações que simplesmente funcionam.**\n\n## Novidades\n- O GT4Recomp agora instala em Arquivos de Programas, para todos os usuários do PC, na pasta que você escolher.\n- Suas configurações e saves ficam na pasta de usuário do Windows; os arquivos grandes do jogo ficam ao lado do programa.\n- As atualizações são instaladas em silêncio: uma confirmação, e o launcher reabre sozinho.\n- **Modo de Desempenho** (Configurações > Gráficos): mais rápido em PCs mais fracos, pulando alguns efeitos leves.\n- Configurações agora fica ao lado de Jogar na tela inicial.\n\n## Correções\n- O launcher não abre mais como uma janela branca em alguns PCs.\n" } },
            { version: "1.2.0", markdown: "# GT4Recomp 1.2.0\n\n**Custom texture packs arrive.**\n\n## New\n- **HD texture packs**: drop a pack in the Textures folder (Settings > Graphics > Open textures folder) and play.\n- The renderer is now called **GSdx**, and every GSdx picture option is available in Settings.\n- The resolution follows your desktop automatically, and monitors show their real names.\n- The game window starts black instead of white.\n\n## Thanks\n- To **JulioHeroZ** for their contribution to this release.\n", translations: { "pt-BR": "# GT4Recomp 1.2.0\n\n**Chegam os pacotes de texturas HD.**\n\n## Novidades\n- **Pacotes de texturas HD**: coloque um pacote na pasta Textures (Configurações > Gráficos > Abrir pasta de texturas) e jogue.\n- O renderizador agora se chama **GSdx**, e todas as opções de imagem do GSdx estão disponíveis em Configurações.\n- A resolução segue a sua área de trabalho automaticamente, e os monitores mostram seus nomes reais.\n- A janela do jogo abre preta em vez de branca.\n\n## Agradecimentos\n- À **JulioHeroZ** pela contribuição nesta versão.\n" } },
            { version: "1.1.0", markdown: "# GT4Recomp 1.1.0\n\n**Ultrawide support: the game now fills any screen.**\n\n## New\n- **21:9, 32:9, 16:10 and more**: races fill your screen and show more to the sides, with the HUD kept undistorted.\n- Choose the monitor and resolution the game opens on.\n- The game's sharpness adjusts to your resolution automatically, with a Render scale slider for more speed.\n- All picture settings in one place: Settings > Graphics.\n\n## Fixes\n- Updating from inside the launcher works now.\n\n## Good to know\n- Menus, movies and photo mode stay 16:9.\n", translations: { "pt-BR": "# GT4Recomp 1.1.0\n\n**Suporte a ultrawide: o jogo agora preenche qualquer tela.**\n\n## Novidades\n- **21:9, 32:9, 16:10 e mais**: as corridas preenchem sua tela e mostram mais para os lados, com o HUD sem distorção.\n- Escolha o monitor e a resolução em que o jogo abre.\n- A nitidez do jogo se ajusta sozinha à sua resolução, com o controle Escala de renderização para ganhar velocidade.\n- Todas as configurações de imagem em um só lugar: Configurações > Gráficos.\n\n## Correções\n- Atualizar pelo próprio launcher funciona agora.\n\n## Bom saber\n- Menus, filmes e o modo foto continuam em 16:9.\n" } },
            { version: "1.0.0", markdown: "# GT4Recomp 1.0.0\n\n**The first release: Gran Turismo 4 running natively on your PC.**\n\n## New\n- One-click installer and a launcher that sets the game up from your own disc image.\n- **60 FPS** at full game speed in every scene we tested.\n- Any controller works out of the box (Xbox, DualShock 4, DualSense, Switch Pro), with button remapping.\n- Widescreen 16:9 and progressive 480p races, always on.\n- Your saves and replays are easy to find and back up, and survive a crash.\n\n## Good to know\n- Racing wheels and ultrawide screens are not supported yet.\n- Some license demos and menu backgrounds do not play.\n", translations: { "pt-BR": "# GT4Recomp 1.0.0\n\n**A primeira versão: Gran Turismo 4 rodando nativamente no seu PC.**\n\n## Novidades\n- Instalador de um clique e um launcher que prepara o jogo a partir da sua própria imagem de disco.\n- **60 FPS** em velocidade total do jogo em todas as cenas testadas.\n- Qualquer controle funciona logo de cara (Xbox, DualShock 4, DualSense, Switch Pro), com remapeamento de botões.\n- Widescreen 16:9 e corridas em 480p progressivo, sempre ativos.\n- Seus saves e replays são fáceis de achar e de fazer backup, e sobrevivem a uma queda do jogo.\n\n## Bom saber\n- Volantes de corrida e telas ultrawide ainda não são suportados.\n- Algumas demos de licença e fundos de menu não rodam.\n" } },
          ]
        };
      case "open_link":
        if (args.name !== "kofi" && args.name !== "youtube") {
          throw new Error(args.name + " is not a link of this launcher");
        }
        return null;
      case "about_info":
        return {
          version: "1.3.0",
          gt4recomp: "1.3.0",
          gt4gs: "1.2.1",
          disc_path: "D:\\Games\\PS2\\Gran Turismo 4 (USA) (v2.00).iso",
          game_files: "C:\\Program Files\\GT4Recomp\\data\\games\\" + selectedGame + "\\disc",
          data_folder: "C:\\Program Files\\GT4Recomp\\data",
          saves_folder: "C:\\Users\\Player\\AppData\\Roaming\\GT4Recomp",
          game_name: gameById(selectedGame).name,
          credits: "Gran Turismo 4 Spec II is a mod of the GT4 Online Public Beta made by [AUTHORS: names to be filled in by the owner, with their permission and a link]. GT4Recomp prepares it from your own disc image and includes none of its files. The Spec II logo on the home screen is a placeholder until the authors logo and credit are added."
        };
      case "open_log_folder":
      case "open_game_files_folder":
      case "open_player_folder":
      case "open_saves_folder":
      case "open_replays_folder":
      case "open_textures_folder":
      case "open_latest_log":
      case "open_export_folder":
        return null;
      case "textures_state":
        return texturesState();
      case "choose_textures_folder":
        // The mock "picks" a folder, as the folder picker would. ?textures=missing shows the missing-folder hint.
        texturesChoice = query.get("textures") === "missing" ? "D:\\Missing\\textures" : "D:\\PCSX2\\textures";
        return texturesState();
      case "reset_textures_folder":
        texturesChoice = "";
        return texturesState();
      case "export_logs":
        // The mock does not write a bundle: the player "cancelled" the folder choice.
        return null;
      case "check_updates":
        // Demo: ?update=1 offers both streams; otherwise nothing is new.
        if (query.get("update") === "1") {
          return {
            offers: DEMO_OFFERS.map(function (offer) { return JSON.parse(JSON.stringify(offer)); }),
            errors: [], upToDate: false, launcher: "1.3.0", renderer: "1.2.1"
          };
        }
        return { offers: [], errors: [], upToDate: true, launcher: "1.3.0", renderer: "1.2.1" };
      case "list_launcher_versions":
        if (devVersions.failList) {
          throw new Error("Could not read the version list (no connection). Your current version stays.");
        }
        return versionList();
      case "install_launcher_version":
        // The real command runs the update path (download, SHA256SUMS check, setup) and the launcher closes.
        devVersions.installs.push(args.tag);
        return { ok: true, message: "GT4Recomp setup started." };
      case "apply_update":
        return { ok: true, message: args.stream === "launcher" ? "GT4Recomp setup started." : "gt4-gs 1.1.1 is ready." };
      case "skip_update":
      case "confirm_close":
      case "ui_ready":
        return null;
      default: {
        // TASK-181: the Wheel page's commands (wheelCommand returns undefined for any other name).
        var wheelAnswer = wheelCommand(command, args);
        if (wheelAnswer !== undefined) {
          return wheelAnswer;
        }
        throw new Error("Mock has no command " + command);
      }
    }
  }

  // Events: pages listen through GT4.backend.listen; a demo offer is emitted after load with ?update=1.
  var eventHandlers = {};
  function listen(name, handler) {
    (eventHandlers[name] = eventHandlers[name] || []).push(handler);
  }
  function emit(name, payload) {
    (eventHandlers[name] || []).forEach(function (handler) { handler(payload); });
  }
  // Pad events in the shape of the Rust pad service: pad-status on change, pad-live every 50 ms while a pad is
  // connected. The source is the dev Gamepad-like object (?pad=, ?pads=, ?nopad=); the app itself never reads one.
  function devSummary() {
    var pads = root.GT4.devGamepads ? root.GT4.devGamepads() : [];
    return root.GT4.controlsModel.padSummary(pads);
  }
  // TASK-148: the dev pads with made-up GUIDs (the first an Xbox pad, the next DualSense pads with serials), and with
  // ?unmapped=1 an old joystick SDL3 has no mapping for. Like the pad service, a stored controller that is not connected
  // gives way to the pad read (controller 1), which is then stored.
  var mockController = "auto";
  var DEV_GUIDS = ["030000005e0400008e02000014010000", "050000004c050000e60c000000810000", "050000004c050000e60c000000810000"];
  var DEV_NAMES = ["Xbox Wireless Controller", "DualSense Wireless Controller", "DualSense Wireless Controller"];
  function padStatusPayload() {
    var summary = devSummary();
    var pads = [];
    for (var i = 0; i < summary.count; i++) {
      var guid = DEV_GUIDS[i % DEV_GUIDS.length];
      // Controller 1 keeps the dev pad's own name (focus-check's "Test Pad").
      pads.push({ name: i === 0 && summary.name ? summary.name : DEV_NAMES[i % DEV_NAMES.length], kind: i === 0 ? "xbox360" : "ps5",
        key: i === 0 ? guid : guid + "/a0:ab:51:00:00:0" + i, active: false });
    }
    // The pad read: the stored unit, else one of its model, else controller 1 (`pad::chosen_pad`).
    var activeIndex = pads.map(function (pad) { return pad.key; }).indexOf(mockController);
    if (activeIndex < 0) {
      activeIndex = pads.map(function (pad) { return pad.key.split("/")[0]; }).indexOf(mockController.split("/")[0]);
    }
    if (activeIndex < 0) {
      activeIndex = 0;
    }
    if (pads.length) {
      pads[activeIndex].active = true;
    }
    var chosen = mockController !== "auto" && pads.length > 0 && pads[activeIndex].key.split("/")[0] === mockController.split("/")[0];
    if (pads.length && !chosen) {
      mockController = pads[activeIndex].key;
      chosen = true;
    }
    return { available: true, connected: summary.connected, name: summary.connected ? pads[activeIndex].name : "",
      kind: summary.connected ? pads[activeIndex].kind : "", player_index: summary.connected ? 0 : null, count: summary.count,
      error: "", pads: pads, unrecognized: query.get("unmapped") === "1" ? [{ name: "USB Gamepad (old)", guid: "03000000aaaa0000bbbb000000000000" }] : [],
      choice: mockController, chosen: chosen };
  }
  var lastPadStatus = "";
  setInterval(function () {
    if (!root.GT4.controlsModel) {
      return;
    }
    var status = padStatusPayload();
    var key = JSON.stringify(status);
    if (key !== lastPadStatus) {
      lastPadStatus = key;
      emit("pad-status", status);
    }
    var summary = devSummary();
    if (summary.connected) {
      emit("pad-live", root.GT4.controlsModel.padLive(summary.pad));
    }
  }, 50);
  // ---- TASK-181: the Wheel page. The same shapes as wheel_page.rs and wheel_service.rs (sample devices, not game data).
  // ?wheel=g27|unknown|both|none (default g27), ?wheelcfg=g27|fresh (default g27: a configured G27; fresh: no file yet),
  // ?wheelfile=migrated|refused|newer, ?steer=<degrees> ?thr= ?brk= ?clu= (0..1 pedal travel), ?wbtn=0,4 (buttons
  // down), ?what=1 (hat bits), ?wheelmove=1 (the wheel and pedals move by themselves), ?session=yes|no|unknown|off|none,
  // ?ffbtest=noffb, ?capture=hold (a capture waits), ?wizard=1&wizardstep=N (the set-up wizard at step N),
  // ?wheelselect=throttle, ?wheelpanel=ffb (the panel scrolled to force feedback), ?presets=1 (one saved preset).
  var WHEEL_G27 = "030000006d0400009bc2000000000000";
  var WHEEL_UNKNOWN = "03000000341200007856000000000000";
  var WHEEL_PEDALS = "03000000b70e00003b18000000000000";
  var wheelParam = query.get("wheel") || "g27";
  var WHEEL_SLOTS = [
    { id: "dpad_up", dfp: "dpad_up" }, { id: "dpad_right", dfp: "dpad_right" }, { id: "dpad_down", dfp: "dpad_down" },
    { id: "dpad_left", dfp: "dpad_left" }, { id: "cross", dfp: "cross" }, { id: "circle", dfp: "circle" },
    { id: "square", dfp: "square" }, { id: "triangle", dfp: "triangle" }, { id: "l1", dfp: "l2" }, { id: "r1", dfp: "r2" },
    { id: "l2", dfp: "gear_down" }, { id: "r2", dfp: "gear_up" }, { id: "l3", dfp: "l3" }, { id: "r3", dfp: "r3" },
    { id: "select", dfp: "select" }, { id: "start", dfp: "start" }
  ];
  var WHEEL_FAMILIES = [
    { id: "logitech_legacy", name: "Logitech Driving Force / G25 / G27", software: "Logitech G HUB or Logitech Profiler", direct_drive: false, force_feedback: true },
    { id: "logitech_ps", name: "Logitech G29 / G923 (PlayStation)", software: "Logitech G HUB", direct_drive: false, force_feedback: true },
    { id: "generic", name: "Racing wheel", software: "", direct_drive: false, force_feedback: true }
  ];
  function wheelPedal(source, deadzone, saturation) {
    return { source: source, mode: "inverted", deadzone: deadzone, saturation: saturation, curve: "linear" };
  }
  function wheelDefaults() {
    return {
      version: 1, enabled: false, device_guid: "", device_name: "", preset: "generic",
      steering: { source: "axis:0", rotation_degrees: 900, invert: false },
      throttle: wheelPedal("axis:1", 0.02, 0.98), brake: wheelPedal("axis:2", 0.02, 0.98),
      handbrake: wheelPedal("", 0.05, 0.95), clutch: wheelPedal("axis:3", 0.05, 0.95),
      ffb: { enabled: true, strength: 1, damper: 0.15, invert: false },
      buttons: { dpad_up: ["hat:0:up"], dpad_right: ["hat:0:right"], dpad_down: ["hat:0:down"], dpad_left: ["hat:0:left"],
        cross: ["btn:0"], circle: ["btn:2"], square: ["btn:1"], triangle: ["btn:3"], l1: ["btn:7"], r1: ["btn:6"],
        l2: ["btn:5"], r2: ["btn:4"], l3: ["btn:10"], r3: ["btn:11"], select: ["btn:8"], start: ["btn:9"] }
    };
  }
  // The capture answers of the mock (a G27-like layout: Z throttle, Rz brake, Y clutch; sample indices).
  var WHEEL_CAPTURES = {
    throttle: { source: "axis:2", mode: "inverted", rest: 32767, value: -20000 },
    brake: { source: "axis:3", mode: "inverted", rest: 32767, value: -21000 },
    clutch: { source: "axis:1", mode: "inverted", rest: 32767, value: -18000 },
    handbrake: { source: "btn:22", mode: null },
    cross: "btn:0", square: "btn:1", circle: "btn:2", triangle: "btn:3", r2: "btn:4", l2: "btn:5", r1: "btn:6", l1: "btn:7",
    select: "btn:8", start: "btn:9", r3: "btn:10", l3: "btn:11",
    dpad_up: "hat:0:up", dpad_right: "hat:0:right", dpad_down: "hat:0:down", dpad_left: "hat:0:left"
  };
  function wheelG27Config() {
    var config = wheelDefaults();
    config.enabled = true;
    config.device_guid = WHEEL_G27;
    config.device_name = "Logitech G27 Racing Wheel USB";
    config.preset = "logitech_legacy";
    config.throttle.source = "axis:2";
    config.brake.source = "axis:3";
    config.clutch.source = "axis:1";
    config.ffb.strength = 0.8;
    WHEEL_SLOTS.forEach(function (slot) { config.buttons[slot.id] = [WHEEL_CAPTURES[slot.id]]; });
    return config;
  }
  var wheelConfig = query.get("wheelcfg") === "fresh" ? wheelDefaults() : wheelG27Config();
  var wheelFileState = query.get("wheelcfg") === "fresh" ? "missing" : (query.get("wheelfile") || "loaded");
  var wheelWatching = false;
  var wheelFeatures = null;
  var wheelPresets = query.get("presets") === "1"
    ? [{ name: "G27 (sample)", device_name: "Logitech G27 Racing Wheel USB", preset: "G27 (sample)", problem: null, config: wheelG27Config() }]
    : [];
  function wheelDevices() {
    var list = [];
    if (wheelParam === "g27" || wheelParam === "both") {
      list.push({ id: 11, guid: WHEEL_G27, name: "Logitech G27 Racing Wheel USB", vid_pid: "046D:C29B", sdl_type: "wheel", is_gamepad: false,
        axes: 4, buttons: 23, hats: 1, haptic: true, features: wheelFeatures, family: "logitech_legacy",
        family_name: "Logitech Driving Force / G25 / G27", model: "G27", matched_by: "vid_pid",
        software: "Logitech G HUB or Logitech Profiler", direct_drive: false, force_feedback: true, notes: [] });
    }
    if (wheelParam === "unknown" || wheelParam === "both") {
      list.push({ id: 12, guid: WHEEL_UNKNOWN, name: "USB Racing Wheel", vid_pid: "1234:5678", sdl_type: "other", is_gamepad: false,
        axes: 3, buttons: 12, hats: 1, haptic: false, features: null, family: null, family_name: null, model: null,
        matched_by: "none", software: "", direct_drive: false, force_feedback: true, notes: [] });
    }
    if (wheelParam === "both") {
      list.push({ id: 13, guid: WHEEL_PEDALS, name: "ClubSport Pedals", vid_pid: "0EB7:183B", sdl_type: "other", is_gamepad: false,
        axes: 3, buttons: 0, hats: 0, haptic: false, features: null, family: null, family_name: null, model: null,
        matched_by: "none", software: "", direct_drive: false, force_feedback: true, notes: [] });
    }
    return list;
  }
  function wheelPrimary(devices) {
    if (wheelConfig.device_guid) {
      var same = devices.filter(function (device) { return device.guid === wheelConfig.device_guid; });
      return same.length === 1 ? same[0].id : null;
    }
    var wheel = devices.filter(function (device) { return device.name.toLowerCase().indexOf("pedal") < 0; })[0];
    return wheel ? wheel.id : null;
  }
  function wheelStatus() {
    var devices = wheelDevices();
    var primaryId = wheelPrimary(devices);
    devices.forEach(function (device) {
      device.role = device.id === primaryId ? "wheel" : device.name.toLowerCase().indexOf("pedal") >= 0 ? "auxiliary" : "other_wheel";
    });
    return { available: true, error: "", game_running: gameRunning, watching: wheelWatching, primary: primaryId,
      configured: wheelConfig.device_guid,
      configured_connected: !!wheelConfig.device_guid && devices.some(function (device) { return device.guid === wheelConfig.device_guid; }),
      devices: devices };
  }
  function wheelView(taken) {
    return {
      config: JSON.parse(JSON.stringify(wheelConfig)),
      slots: WHEEL_SLOTS,
      families: WHEEL_FAMILIES,
      pedal_modes: ["inverted", "normal", "positive", "combined_negative", "combined_positive"],
      curves: ["linear", "quadratic", "cubic"],
      file: { state: wheelFileState, detail: wheelFileState === "refused" ? "line 3: mode must be \"dfp\"" : wheelFileState === "migrated" ? "a 1.2.x wheel file (no mode)" : "",
        exists: wheelFileState !== "missing", locked: wheelFileState === "newer" },
      status: wheelStatus(),
      presets: wheelPresets.map(function (preset) { return { name: preset.name, device_name: preset.device_name, preset: preset.preset, problem: preset.problem }; }),
      game_running: gameRunning,
      taken_from: taken || []
    };
  }
  function wheelTemplate(family) {
    var config = wheelDefaults();
    config.preset = family;
    ["throttle", "brake", "clutch", "handbrake"].forEach(function (id) { config[id].source = ""; });
    Object.keys(config.buttons).forEach(function (slot) { config.buttons[slot] = slot.indexOf("dpad_") === 0 ? config.buttons[slot] : []; });
    if (family === "logitech_ps") {
      [["cross", 0], ["square", 1], ["circle", 2], ["triangle", 3], ["r2", 4], ["l2", 5], ["r1", 6], ["l1", 7], ["select", 8],
        ["start", 9], ["r3", 10], ["l3", 11]].forEach(function (pair) { config.buttons[pair[0]] = ["btn:" + pair[1]]; });
    }
    return config;
  }
  function wheelSaved() {
    wheelFileState = wheelFileState === "newer" ? "newer" : "loaded";
  }
  function wheelSet(key, value) {
    if (wheelFileState === "newer") {
      throw new Error("The wheel file was written by a newer GT4Recomp and is kept as it is.");
    }
    var parts = String(key).split(".");
    var section = parts[0] === "wheel" ? wheelConfig : wheelConfig[parts[0]];
    if (parts.length !== 2 || !section || !Object.prototype.hasOwnProperty.call(section, parts[1])) {
      throw new Error("unknown wheel setting '" + key + "'");
    }
    if (key === "steering.rotation_degrees" && (value < 180 || value > 1080)) {
      throw new Error("rotation_degrees must be between 180 and 1080");
    }
    section[parts[1]] = value;
    if (parts[1] === "deadzone" && section.saturation <= section.deadzone + 0.05) {
      section.saturation = Math.min(1, Math.round((section.deadzone + 0.06) * 1000) / 1000);
    }
    if (parts[1] === "saturation" && section.saturation <= section.deadzone + 0.05) {
      section.deadzone = Math.max(0, Math.round((section.saturation - 0.06) * 1000) / 1000);
    }
    wheelSaved();
  }
  function wheelAssign(target, source, mode, add) {
    var taken = [];
    if (target === "steering") {
      wheelConfig.steering.source = source;
    } else if (["throttle", "brake", "clutch", "handbrake"].indexOf(target) >= 0) {
      wheelConfig[target].source = source;
      if (mode) {
        wheelConfig[target].mode = mode;
      }
    } else if (Object.prototype.hasOwnProperty.call(wheelConfig.buttons, target)) {
      Object.keys(wheelConfig.buttons).forEach(function (slot) {
        if (slot !== target && wheelConfig.buttons[slot].indexOf(source) >= 0) {
          wheelConfig.buttons[slot] = wheelConfig.buttons[slot].filter(function (one) { return one !== source; });
          taken.push(slot);
        }
      });
      wheelConfig.buttons[target] = add ? wheelConfig.buttons[target].concat([source]).slice(-3) : [source];
    } else {
      throw new Error("unknown wheel control '" + target + "'");
    }
    wheelSaved();
    return taken;
  }
  var wheelCapturePending = null;
  function wheelCapture(args) {
    return new Promise(function (resolve, reject) {
      if (gameRunning) {
        reject(new Error("The game is running: close it to change the wheel's controls or test the force."));
        return;
      }
      if (wheelCapturePending) {
        wheelCapturePending(new Error("capture cancelled"));
      }
      wheelCapturePending = reject;
      if (query.get("capture") === "hold") {
        return;
      }
      setTimeout(function () {
        if (wheelCapturePending !== reject) {
          return;
        }
        wheelCapturePending = null;
        var answer;
        if (args.target === "steering") {
          answer = { source: "axis:0", mode: null, rest: 0, value: args.phase === "right" ? 31000 : -31000 };
        } else {
          var known = WHEEL_CAPTURES[args.target];
          answer = typeof known === "string" ? { source: known, mode: null } : Object.assign({}, known);
        }
        answer.target = args.target;
        answer.device_name = "Logitech G27 Racing Wheel USB";
        answer.guid = WHEEL_G27;
        resolve(answer);
      }, 450);
    });
  }
  // The live raw state: still values from the query, or a slow sweep with ?wheelmove=1.
  function wheelLive(now) {
    if (root.GT4.devWheelLive) {
      return root.GT4.devWheelLive(); // the focus checks' still wheel
    }
    var moving = query.get("wheelmove") === "1";
    var phase = now / 1000;
    var steer = moving ? 210 * Math.sin(phase / 1.7) : Number(query.get("steer") || 0);
    function pedal(name, offset) {
      if (moving) {
        return Math.max(0, Math.sin(phase / 1.3 + offset));
      }
      return Number(query.get(name) || 0);
    }
    function inverted(travel) {
      return Math.round(32767 - travel * 65535);
    }
    var steerRaw = Math.max(-32768, Math.min(32767, Math.round(steer / 450 * 32767)));
    var pressed = (query.get("wbtn") || "").split(",").filter(Boolean).map(Number);
    var devices = [];
    if (wheelParam === "g27" || wheelParam === "both") {
      devices.push({ id: 11, guid: WHEEL_G27, axes: [steerRaw, inverted(pedal("clu", 2.1)), inverted(pedal("thr", 0)), inverted(pedal("brk", 3.3))],
        pressed: pressed, hats: [Number(query.get("what") || 0)] });
    }
    if (wheelParam === "unknown" || wheelParam === "both") {
      devices.push({ id: 12, guid: WHEEL_UNKNOWN, axes: [steerRaw, inverted(pedal("thr", 0)), inverted(pedal("brk", 3.3))], pressed: pressed, hats: [0] });
    }
    if (wheelParam === "both") {
      devices.push({ id: 13, guid: WHEEL_PEDALS, axes: [-32768, -32768, -32768], pressed: [], hats: [] });
    }
    return { devices: devices };
  }
  function wheelSession() {
    var kind = query.get("session") || "yes";
    if (kind === "none") {
      return { found: false, received: "unknown", reason: "no_session" };
    }
    var base = { found: true, session: "session-1793001234", modified: "2026-11-05T01:12:00Z", wheel_file: "found", enabled: true,
      preset: "logitech_legacy", refused: "", no_ffb: "", dfp_configured: true,
      devices: [{ name: "Logitech G27 Racing Wheel USB", vid_pid: "046d:c29b", sdl_type: "wheel", axes: 4, buttons: 23, hats: 1 }],
      ffb_device: { constant: true, spring: true, damper: true, friction: true, gain: true, autocenter: true }, summary: null };
    if (kind === "yes") {
      base.summary = { game_force_commands: 5120, effect_updates_sent: 4810, effects_started: 4, haptic_device_open: true,
        effects_supported: true, game_forces_played: 4790, reached_the_wheel: true };
      base.received = "yes";
      base.reason = "summary";
    } else if (kind === "no") {
      base.ffb_device = null;
      base.no_ffb = "Haptic: Joystick isn't a haptic device.";
      base.received = "no";
      base.reason = "no_haptics";
    } else if (kind === "off") {
      base.enabled = false;
      base.received = "no";
      base.reason = "wheel_off";
    } else {
      base.received = "unknown";
      base.reason = "ended_early";
    }
    return base;
  }
  function wheelCommand(command, args) {
    switch (command) {
      case "get_wheel_config":
        return wheelView();
      case "set_wheel_setting":
        wheelSet(args.key, args.value);
        return wheelView();
      case "apply_wheel_preset":
        if (args.saved) {
          var saved = wheelPresets.filter(function (preset) { return preset.name === args.name; })[0];
          if (!saved) {
            throw new Error("no preset named '" + args.name + "'");
          }
          var enabled = wheelConfig.enabled;
          wheelConfig = JSON.parse(JSON.stringify(saved.config));
          wheelConfig.enabled = enabled;
        } else {
          var keep = wheelConfig.enabled;
          wheelConfig = wheelTemplate(args.name);
          wheelConfig.enabled = keep;
          var device = wheelDevices().filter(function (one) { return one.guid === args.guid; })[0];
          if (device) {
            wheelConfig.device_guid = device.guid;
            wheelConfig.device_name = device.name;
          }
        }
        wheelSaved();
        return wheelView();
      case "assign_wheel_binding":
        return wheelView(wheelAssign(args.target, args.source, args.mode, args.add));
      case "reset_wheel_binding":
        wheelAssign(args.target, "", null, false);
        if (Object.prototype.hasOwnProperty.call(wheelConfig.buttons, args.target)) {
          wheelConfig.buttons[args.target] = [];
        }
        return wheelView();
      case "reset_wheel_config":
        wheelConfig = wheelDefaults();
        wheelSaved();
        return wheelView();
      case "capture_wheel_binding":
        return wheelCapture(args);
      case "cancel_wheel_capture":
        if (wheelCapturePending) {
          var cancel = wheelCapturePending;
          wheelCapturePending = null;
          cancel(new Error("capture cancelled"));
        }
        return null;
      case "test_wheel_ffb":
        if (gameRunning) {
          throw new Error("The game is running: close it to change the wheel's controls or test the force.");
        }
        return new Promise(function (resolve) {
          setTimeout(function () {
            if (query.get("ffbtest") === "noffb") {
              resolve({ device_name: "Logitech G27 Racing Wheel USB", has_ffb: false, features: { constant: false, spring: false, damper: false, friction: false, gain: false, autocenter: false },
                played: false, error: "Haptic: Joystick isn't a haptic device." });
              return;
            }
            wheelFeatures = { constant: true, spring: true, damper: true, friction: true, gain: true, autocenter: true };
            resolve({ device_name: "Logitech G27 Racing Wheel USB", has_ffb: true, features: wheelFeatures, played: true, error: "" });
          }, 900);
        });
      case "wheel_watch":
        wheelWatching = !!args.on;
        return wheelStatus();
      case "get_wheel_session_check":
        return wheelSession();
      case "list_wheel_presets":
        return wheelView().presets;
      case "save_wheel_preset": {
        var name = String(args.name || "").trim();
        if (!name || /[\\/:*?"<>|]/.test(name)) {
          throw new Error("a preset name may hold letters, digits, spaces and - _ ( ) . + only");
        }
        wheelPresets = wheelPresets.filter(function (preset) { return preset.name !== name; });
        wheelPresets.push({ name: name, device_name: wheelConfig.device_name, preset: name, problem: null, config: JSON.parse(JSON.stringify(wheelConfig)) });
        return wheelView().presets;
      }
      case "delete_wheel_preset":
        wheelPresets = wheelPresets.filter(function (preset) { return preset.name !== args.name; });
        return wheelView().presets;
      default:
        return undefined;
    }
  }
  root.GT4.devWheel = {
    select: query.get("wheelselect") || "",
    capture: query.get("capture") || "",
    wizard: query.get("wizard") === "1",
    wizardStep: Number(query.get("wizardstep") || 0),
    panel: query.get("wheelpanel") || ""
  };
  var lastWheelStatus = "";
  setInterval(function () {
    var status = wheelStatus();
    var key = JSON.stringify(status);
    if (key !== lastWheelStatus) {
      lastWheelStatus = key;
      emit("wheel-status", status);
    }
    if (wheelWatching && !gameRunning) {
      emit("wheel-live", wheelLive(Date.now()));
    }
  }, 50);
  root.GT4_MOCK = { invoke: invoke, listen: listen, emit: emit };
  if (query.get("update") === "1") {
    setTimeout(function () { emit("update-available", { offers: DEMO_OFFERS }); }, 900);
  }
})(window);
