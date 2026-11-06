/*
 * Dev only: DOM-level checks on the dev mock, run by dev/focus-check.html in headless Edge (FLOW-AUDIT 7.7).
 * Arrow keys stand in for the pad (nav.js handles both). The results are written into a <pre> at the end of the
 * page and body[data-focus-check] is "pass" or "fail".
 *
 *   (no query)          the sidebar walk over every tab, Saves and Replays entry, B back, value changes, the
 *                       Display, Graphics and Advanced sections inside Graphics (no Display or Advanced tab; the
 *                       D-pad walks through all three), and the
 *                       Controls page (entry on the DS2, spatial moves, rebind and cancel, conflict and swap, live
 *                       input from a still dev pad, Y reset, X reset all), and the Wheel page (TASK-181: entry, chips, the
 *                       real-angle wheel and pedals, bind, clear, calibration, the force test, the wizard, presets)
 *   ?scenario=4k        only sets the state for docs/launcher/shots/graphics-4k-updated.png (4K chosen, row shown)
 *   ?scenario=advanced  only sets the state for docs/launcher/shots/graphics-advanced-section.png (pad focus on the
 *                       first Advanced row inside Graphics)
 *   ?scenario=render    only sets the state for docs/launcher/shots/graphics-sections.png (pad focus on Render scale,
 *                       the Graphics section in view)
 *   ?scenario=general, ?scenario=graphics  only open that Settings tab (screenshots at 1280x720; no checks run)
 *   Option values (rc6): no value of a .row selector or dropdown on General or Graphics wraps, in every language
 */
(function (root) {
  "use strict";
  var GT4 = root.GT4;
  var TABS = ["graphics", "controls", "wheel", "games", "saves", "replays", "achievements", "about"];
  var lines = [];
  var failures = 0;

  // The text of a locale key in the current language. The checks compare against keys, so the same checks run in
  // every language (ui/locales/*.json).
  function T(key, params) {
    return GT4.t(key, params);
  }

  function wait(ms) {
    return new Promise(function (resolve) {
      root.setTimeout(resolve, ms);
    });
  }

  // A key press: down and up, so the page's held-key state (keysDown) clears as it does with a real keyboard.
  function press(name) {
    document.dispatchEvent(new KeyboardEvent("keydown", { key: name, bubbles: true, cancelable: true }));
    document.dispatchEvent(new KeyboardEvent("keyup", { key: name, bubbles: true, cancelable: true }));
  }

  function check(name, ok, detail) {
    lines.push((ok ? "PASS  " : "FAIL  ") + name + (ok ? "" : " (" + detail + ")"));
    if (!ok) {
      failures += 1;
    }
  }

  function sideTab() {
    var active = document.activeElement;
    if (!active || !active.closest || !active.closest("nav.side")) {
      return null;
    }
    return active.getAttribute("data-tab") || "(back)";
  }

  function shownTab() {
    var current = document.querySelector("nav.side [aria-current=\"page\"]");
    return current ? current.getAttribute("data-tab") : null;
  }

  function focusInContent() {
    var content = document.querySelector(".stage .content");
    return !!content && content.contains(document.activeElement);
  }

  function sideLevel() {
    var stage = document.querySelector(".stage");
    return !!stage && stage.classList.contains("is-side-level");
  }

  function footerText() {
    return Array.prototype.map.call(document.querySelectorAll("#hints .hint"), function (item) {
      return item.textContent;
    }).join("|");
  }

  function sideButton(tab) {
    return document.querySelector("nav.side [data-tab=\"" + tab + "\"]");
  }

  function setHash(hash) {
    root.location.hash = hash;
    return wait(250);
  }

  // TASK-114 rc5: a tab read before opens from memory, so its rows are there at once and no loading line shows (the
  // loading line waits 300 ms and only shows with no data). The check reads the page 40 ms after the hash changes.
  async function loadedTabChecks() {
    await setHash("#/settings/graphics");
    root.location.hash = "#/settings/general";
    await wait(40);
    var rows = document.querySelectorAll(".stage .content .row").length;
    check("a loaded tab shows its rows at once, with no loading line", !document.querySelector(".stage .loading") && rows > 0, "rows " + rows);
    // The Saves list (TASK-114 rc5, item 8 extension): read before, so its header is drawn at once.
    root.location.hash = "#/settings/saves";
    await wait(40);
    check("a loaded Saves tab shows its list at once, with no loading line", !document.querySelector(".stage .loading") && !!document.querySelector(".stage .content .lib-head"), "no list header");
    await wait(250);
  }

  // The tab list has focus, the shown tab is the highlighted one, and nothing in the rows has focus.
  function sideStepOk(tab) {
    return sideTab() === tab && shownTab() === tab && !focusInContent() && sideLevel();
  }

  function sideDetail() {
    return "focus " + sideTab() + ", shown " + shownTab() + ", in rows " + focusInContent() + ", side level " + sideLevel();
  }

  // Rendering each tab through its route never moves focus off the tab list.
  // IN-507: the "Compiling shaders" window opens on the first progress, counts, holds on Back and closes when Play goes on.
  async function shaderWindowChecks() {
    await setHash("#/");
    await wait(100);
    check("no shader window before a compile", !document.querySelector(".shader-bar"), "open");
    GT4_MOCK.emit("shaders-progress", { done: 0, total: 200 });
    GT4_MOCK.emit("shaders-progress", { done: 50, total: 200 });
    await wait(50);
    var bar = document.querySelector(".shader-bar-fill");
    check("the window shows the title and the count", !!bar && document.querySelector(".modal h3").textContent === T("shaders.title") &&
      document.querySelector(".shader-count").textContent === T("shaders.count", { done: 50, total: 200 }), bar ? bar.style.width : "none");
    check("the bar follows the count", !!bar && bar.style.width === "25%", bar ? bar.style.width : "none");
    // TASK-170b: a retry names its attempt under the count; the first attempt shows nothing there.
    var retry = document.querySelector(".shader-retry");
    check("the first attempt shows no retry line", !!retry && retry.textContent === "", retry ? retry.textContent : "missing");
    GT4_MOCK.emit("shaders-progress", { done: 0, total: 150, attempt: 3 });
    await wait(50);
    check("a retry says its attempt number", !!retry && retry.textContent === T("shaders.retry", { attempt: 3 }) && retry.textContent.indexOf("3") >= 0 &&
      retry.textContent.indexOf("{") < 0, retry ? retry.textContent : "missing");
    check("the count restarts with the retry", document.querySelector(".shader-count").textContent === T("shaders.count", { done: 0, total: 150 }), document.querySelector(".shader-count").textContent);
    GT4_MOCK.emit("shaders-progress", { done: 50, total: 200, attempt: 1 });
    await wait(50);
    // TASK-180 test3 (owner): nothing focused. The only button is Cancel, and one pad A press used to cancel the prepare;
    // the dialog box takes the focus instead, so cancelling needs a move to the button first.
    check("nothing has the focus (Cancel cannot be pressed by accident)",
      !!document.activeElement && document.activeElement.classList.contains("modal-box") &&
      !document.activeElement.matches("button"), document.activeElement && document.activeElement.className);
    document.querySelector(".modal").__dialog.back();
    check("Back does not close it", !!document.querySelector(".shader-bar"), "closed");
    GT4.closeShaderWindow();
    check("it closes when Play goes on", !document.querySelector(".shader-bar"), "still open");
  }

  async function routeChecks() {
    for (var i = 0; i < TABS.length; i++) {
      await setHash("#/settings/" + TABS[i]);
      check("opening #/settings/" + TABS[i] + " keeps focus on its tab", sideStepOk(TABS[i]), sideDetail());
      check("the footer on the tab list has no Save (" + TABS[i] + ")", footerText().indexOf("Save") < 0, footerText());
    }
  }

  // Arrow keys move the highlight through every tab and down again: focus stays in the tab list each time.
  async function sideWalk() {
    await setHash("#/settings/graphics");
    for (var i = 1; i < TABS.length; i++) {
      press("ArrowDown");
      await wait(150);
      check("ArrowDown highlights " + TABS[i] + "; focus stays in the tab list", sideStepOk(TABS[i]), sideDetail());
    }
    for (var j = TABS.length - 2; j >= 0; j--) {
      press("ArrowUp");
      await wait(150);
      check("ArrowUp highlights " + TABS[j] + "; focus stays in the tab list", sideStepOk(TABS[j]), sideDetail());
    }
  }

  // From the tab list, D-pad right (A) enters a tab; B goes back to the tab list on the same tab.
  async function enterChecks() {
    await setHash("#/settings/graphics");
    var expected = {
      saves: "gt4:Save 01",
      replays: "01"
    };
    for (var i = 0; i < TABS.length; i++) {
      var tab = TABS[i];
      await setHash("#/settings/" + tab);
      press("ArrowRight");
      await wait(200);
      var active = document.activeElement;
      if (expected[tab]) {
        var first = active && active.getAttribute && active.getAttribute("data-item-id") === expected[tab];
        check("entering " + tab + " focuses its first row", !!first, "focus " + (active && active.outerHTML ? active.outerHTML.slice(0, 80) : active));
      } else if (tab === "controls") {
        var control = active && active.getAttribute ? active.getAttribute("data-control") : null;
        check("entering controls focuses Cross on the drawing", control === "cross", "focus " + control);
      } else {
        check("entering " + tab + " puts focus in its rows", focusInContent() && !sideLevel(), sideDetail());
      }
      check("entering " + tab + " shows no Save in the footer", footerText().indexOf("Save") < 0, footerText());
      press("Escape");
      await wait(150);
      check("B returns " + tab + " to the tab list on the same tab", sideStepOk(tab), sideDetail());
    }
  }

  // Saves with an empty list would focus Import; the mock has three saves, so the first row is the entry.
  async function importEntryCheck() {
    await setHash("#/settings/saves");
    press("ArrowRight");
    await wait(200);
    var active = document.activeElement;
    check("Saves never auto-selects Import", !(active && active.textContent && active.textContent.indexOf(T("library.import_saves")) >= 0), active ? active.textContent : "none");
  }

  // New save, Rename and Export (TASK-104), in the page's language: the buttons exist with their texts; New save asks
  // first (Cancel closes it); Rename opens a text box of up to 40 characters that takes the focus, and Escape closes it.
  async function libraryActionChecks() {
    await setHash("#/settings/saves");
    await wait(400);
    var newButton = Array.prototype.filter.call(document.querySelectorAll(".lib-actions button"), function (b) {
      return b.textContent === T("library.new_save");
    })[0];
    check("Saves has a New save button", !!newButton, "no New save button");
    var renameButton = document.querySelector(".slot-row .iconbtn[title=\"" + T("library.rename") + "\"]");
    var exportButton = document.querySelector(".slot-row .iconbtn[title=\"" + T("library.export") + "\"]");
    check("each save row has Rename and Export buttons", !!renameButton && !!exportButton, "rename " + !!renameButton + " export " + !!exportButton);
    if (newButton) {
      newButton.click();
      await wait(150);
      var askDialog = document.querySelector(".modal");
      check("New save asks first with its confirm button", !!askDialog && askDialog.textContent.indexOf(T("library.new_save_confirm")) >= 0,
        askDialog ? askDialog.textContent : "no dialog");
      if (askDialog) {
        GT4.ui.topDialog().close();
      }
      await wait(100);
      check("Cancel leaves the save list as it was", !document.querySelector(".modal"), "a dialog is still open");
    }
    if (renameButton) {
      renameButton.click();
      await wait(150);
      var box = document.querySelector(".modal input.modal-input");
      check("Rename opens a text box of up to 40 characters", !!box && box.getAttribute("maxlength") === "40",
        box ? "maxlength " + box.getAttribute("maxlength") : "no text box");
      check("the text box takes the focus", !!box && document.activeElement === box, "focus " + (document.activeElement && document.activeElement.tagName));
      press("Escape");
      await wait(150);
      check("Escape closes the rename box", !document.querySelector(".modal"), "a dialog is still open");
    }
    await setHash("#/settings/saves");
    await wait(200);
  }

  // Changed values show on screen at once and stay after the table is read again.
  async function valueChecks() {
    await setHash("#/settings/graphics");
    // TASK-080 addendum: no Internal resolution or Aspect ratio row; Render scale shows the resulting resolution.
    var gone = document.querySelector(".content [data-setting=\"upscale\"], .content [data-setting=\"aspect_ratio\"], " +
      ".content [data-setting=\"aspect_custom\"]");
    check("no internal resolution or aspect row is shown", !gone, gone ? gone.getAttribute("data-setting") : "");
    var scale = document.querySelector("input[type=range][data-setting=\"render_scale\"]");
    check("render scale is a slider on the Graphics tab", !!scale, "no control");
    var readout = scale && scale.parentNode.querySelector(".val");
    check("the render scale readout shows the resulting resolution", !!readout && readout.textContent === "100% \u00B7 3x \u00B7 1920x1344",
      readout ? readout.textContent : "none");
    if (scale) {
      scale.focus({ preventScroll: true });
      scale.dispatchEvent(new CustomEvent("gt4-adjust", { detail: { delta: 1 } }));
      await wait(300);
      var after = document.querySelector("input[type=range][data-setting=\"render_scale\"]");
      var afterReadout = after && after.parentNode.querySelector(".val");
      check("one step right gives 125% and 4x", !!afterReadout && afterReadout.textContent === "125% \u00B7 4x \u00B7 2560x1792",
        afterReadout ? afterReadout.textContent : "none");
      var scaleFocus = document.activeElement && document.activeElement.getAttribute("data-setting");
      check("focus stays on the render scale slider", scaleFocus === "render_scale", "focus " + scaleFocus);
      after.dispatchEvent(new CustomEvent("gt4-adjust", { detail: { delta: -1 } }));
      await wait(300);
    }

    await setHash("#/settings/graphics");
    var select = document.querySelector("[data-section=\"display\"] .selector[data-setting=\"resolution\"]");
    check("resolution is in the Display section of Graphics", !!select, "no control");
    check("monitor is in the Display section of Graphics",
      !!document.querySelector("[data-section=\"display\"] .selector[data-setting=\"monitor\"]"), "no control");
    if (!select) {
      return;
    }
    var reached = await stepSelectorUntil("resolution", function () { return selectorText("resolution") === "1280x720"; });
    check("the resolution selector goes through the monitor's modes", reached, selectorText("resolution"));
    await wait(300);
    var label = document.querySelector(".selector[data-setting=\"resolution\"] .selector-value:not(.is-leaving)");
    check("the resolution row shows 1280x720 after the change", !!label && label.textContent === "1280x720", label ? label.textContent : "none");
    var table = await GT4.backend.call("get_settings_table");
    var row = GT4.settingsModel.findRow(table, "resolution");
    check("the table holds 1280x720 after the change", !!row && row.current === "1280x720", row ? String(row.current) : "no row");
    var scaleRow = GT4.settingsModel.findRow(table, "render_scale");
    check("the render scale readout follows the resolution", scaleRow.values.labels["100"] === "100% \u00B7 2x \u00B7 1280x896",
      scaleRow.values.labels["100"]);
    var focused = document.activeElement && document.activeElement.getAttribute("data-setting");
    check("focus stays on the changed control", focused === "resolution", "focus " + focused);
    await GT4.backend.call("set_setting", { id: "resolution", value: "1920x1080" });
    await setHash("#/settings/graphics");

    var toggle = selectorNode("fxaa");
    check("anti-aliasing is an Off and On selector on Graphics", !!toggle, "no control");
    if (toggle) {
      toggle.click();
      await wait(300);
      check("the selector shows On after the change", selectorText("fxaa") === T("settings.on"), selectorText("fxaa"));
      var fxaa = GT4.settingsModel.findRow(await GT4.backend.call("get_settings_table"), "fxaa");
      check("the table holds the selector's value", !!fxaa && fxaa.current === true, fxaa ? String(fxaa.current) : "no row");
    }
    await selectorLoopChecks();

    var slider = document.querySelector("input[type=range][data-setting=\"cas_sharpness\"]");
    check("sharpening amount is a slider on Graphics", !!slider, "no control");
    if (slider) {
      var before = Number(slider.value);
      slider.dispatchEvent(new CustomEvent("gt4-adjust", { detail: { delta: 1 } }));
      await wait(300);
      var after = document.querySelector("input[type=range][data-setting=\"cas_sharpness\"]");
      check("the slider moves one step and shows the new value", !!after && Number(after.value) === before + 1, after ? after.value : "none");
      var sharp = GT4.settingsModel.findRow(await GT4.backend.call("get_settings_table"), "cas_sharpness");
      check("the table holds the slider value", !!sharp && sharp.current === before + 1, sharp ? String(sharp.current) : "no row");
    }
  }

  // Advanced is a section at the bottom of Graphics (owner, 2026-10-21), not a tab: the old route falls back to
  // Graphics, the D-pad walks from the Graphics rows into the Advanced rows and the reset button, Y is Reset, and B
  // returns to the tab list from inside the section.
  function focusedSetting() {
    var active = document.activeElement;
    return active && active.getAttribute ? active.getAttribute("data-setting") || active.getAttribute("data-action") : null;
  }

  // The renderer defines the graphics options (owner, 2026-10-30): what each renderer's Graphics tab shows.
  async function rendererRowChecks() {
    function shown() {
      return Array.prototype.map.call(document.querySelectorAll(".content .row[data-row]"), function (item) {
        return item.getAttribute("data-row");
      });
    }
    function holds(list, ids) {
      return ids.every(function (id) { return list.indexOf(id) >= 0; });
    }
    function lacks(list, ids) {
      return ids.every(function (id) { return list.indexOf(id) < 0; });
    }
    var both = ["renderer", "render_scale", "monitor", "resolution", "fullscreen", "vsync", "fxaa", "effect_flares"];
    var gsdxOnly = ["graphics_api", "cas_mode", "cas_sharpness", "perf_overlay", "blending", "hw_download_mode"];
    await setHash("#/settings/about");
    await setHash("#/settings/graphics");
    await wait(200);
    var gsdx = shown();
    function menusAbove(list, menus, switches) {
      return menus.every(function (menu) {
        return switches.every(function (item) { return list.indexOf(menu) < list.indexOf(item); });
      });
    }
    check("GSdx shows its Graphics menus above its switches", menusAbove(gsdx, ["renderer", "render_scale", "graphics_api", "cas_mode"], ["effect_flares", "fxaa"]), gsdx.join("|"));
    check("GSdx shows its rows and no FSR1", holds(gsdx, both) && holds(gsdx, gsdxOnly) && lacks(gsdx, ["frame_rate", "coregs_sharpening", "effect_road_sheen"]), gsdx.join("|"));
    await GT4.backend.call("set_setting", { id: "renderer", value: "coregs20" });
    await setHash("#/settings/about");
    await setHash("#/settings/graphics");
    await wait(200);
    var coregs = shown();
    check("CoreGS 2.0 shows its Graphics menus above its switches", menusAbove(coregs, ["renderer", "render_scale", "frame_rate", "coregs_sharpening"], ["effect_flares", "fxaa"]), coregs.join("|"));
    check("CoreGS 2.0 shows FSR1 and no API, CAS or overlay", holds(coregs, both) && holds(coregs, ["frame_rate", "coregs_sharpening", "effect_road_sheen"]) && lacks(coregs, gsdxOnly), coregs.join("|"));
    // TASK-175: the Performance HUD is a three-way choice (Off, FPS only, Full), Off by default, in the Advanced section.
    function hudNode() { return document.querySelector(".selector[data-setting=\"performance_hud\"]"); }
    var hudWords = ["off", "fps", "full"].map(function (value) { return T("choice.performance_hud." + value); });
    check("the Performance HUD choices are text in this language, all different", hudWords.every(function (word) { return word.indexOf("choice.") !== 0; }) &&
      hudWords[0] !== hudWords[1] && hudWords[1] !== hudWords[2] && hudWords[0] !== hudWords[2], hudWords.join("|"));
    check("the Performance HUD help is text in this language", T("setting.performance_hud.help").indexOf("setting.") !== 0, T("setting.performance_hud.help"));
    check("CoreGS 2.0 shows the Performance HUD as a choice in Advanced, Off by default", !!hudNode() && !!hudNode().closest("[data-section=\"advanced\"]") &&
      hudNode().getAttribute("aria-valuetext") === hudWords[0], hudNode() ? hudNode().getAttribute("aria-valuetext") : "no selector");
    for (var hudStep = 1; hudStep <= 2; hudStep++) {
      await GT4.backend.call("set_setting", { id: "performance_hud", value: ["off", "fps", "full"][hudStep] });
      await setHash("#/settings/about");
      await setHash("#/settings/graphics");
      await wait(200);
      check("the Performance HUD selector shows " + hudWords[hudStep], !!hudNode() && hudNode().getAttribute("aria-valuetext") === hudWords[hudStep],
        hudNode() ? hudNode().getAttribute("aria-valuetext") : "no selector");
    }
    await GT4.backend.call("set_setting", { id: "performance_hud", value: "off" });
    await GT4.backend.call("set_setting", { id: "renderer", value: "pcsx2" });
    await setHash("#/settings/about");
    await setHash("#/settings/graphics");
    await wait(200);
  }

  async function advancedSectionChecks() {
    check("the tab list has no Advanced tab", !sideButton("advanced"), "an Advanced tab is shown");
    check("the tab list has no Display tab", !sideButton("display"), "a Display tab is shown");
    await setHash("#/settings/advanced");
    check("#/settings/advanced falls back to Graphics", shownTab() === "graphics", "shown " + shownTab());
    await setHash("#/settings/display");
    check("#/settings/display falls back to Graphics", shownTab() === "graphics", "shown " + shownTab());
    await setHash("#/settings/graphics");
    var heads = Array.prototype.map.call(document.querySelectorAll(".content .section-head"), function (item) {
      return item.textContent;
    }).join("|");
    check("Graphics shows the Display, Graphics and Advanced sections in order", heads === T("settings.section_display") + "|" + T("settings.section_graphics") + "|" + T("settings.section_advanced"), heads);
    var displayIds = Array.prototype.map.call(document.querySelectorAll("[data-section=\"display\"] .row"), function (item) {
      return item.getAttribute("data-row");
    }).join("|");
    check("the Display section holds Monitor, Resolution, Fullscreen and VSync", displayIds === "monitor|resolution|fullscreen|vsync", displayIds);
    var graphicsSection = document.querySelector("[data-section=\"graphics\"]");
    var inGraphics = !!graphicsSection && ["renderer", "graphics_api", "render_scale", "fxaa"].every(function (id) {
      return !!graphicsSection.querySelector("[data-row=\"" + id + "\"]");
    });
    check("the Graphics section holds Renderer, Graphics API, Render scale and FXAA", inGraphics, "missing rows");
    var looseRows = document.querySelectorAll(".content > .row").length;
    check("every Graphics row sits in a section", looseRows === 0, looseRows + " loose rows");
    var firstAdvanced = document.querySelector("[data-section=\"advanced\"] [data-setting]");
    var firstId = firstAdvanced ? firstAdvanced.getAttribute("data-setting") : null;
    check("the Advanced section holds the advanced rows", firstId === "performance_report", "first " + firstId);
    // TASK-180 test3 fix (owner, 2026-11-05): the Performance report row is what ships the F7/F8 touch button in the game;
    // it must exist as an enabled toggle on both platforms.
    var reportToggle = document.querySelector("[data-row=\"performance_report\"] [data-setting=\"performance_report\"]");
    var reportValue = reportToggle ? reportToggle.getAttribute("aria-valuetext") : null;
    check("the Performance report row is a focusable On/Off toggle",
      !!reportToggle && reportToggle.getAttribute("role") === "spinbutton" && reportToggle.getAttribute("data-focusable") === "1" &&
        (reportValue === T("settings.on") || reportValue === T("settings.off")),
      reportToggle ? "role " + reportToggle.getAttribute("role") + ", value " + reportValue : "missing");
    press("ArrowRight");
    await wait(200);
    // Without a remembered row, A enters on the first control of the tab (settings.js focusRow): Monitor, at the top of
    // Display. Earlier checks left a remembered row, so the walk starts from Monitor explicitly.
    var firstControl = document.querySelector(".content .ctl [data-setting]");
    var firstControlId = firstControl ? firstControl.getAttribute("data-setting") : null;
    check("the first control of Graphics is Monitor, at the top of Display", firstControlId === "monitor", "first " + firstControlId);
    if (firstControl) {
      firstControl.focus({ preventScroll: true });
    }
    var reached = false;
    var reset = false;
    var passedGraphics = false;
    var walk = [];
    for (var i = 0; i < 60 && !reset; i++) {
      press("ArrowDown");
      await wait(80);
      var id = focusedSetting();
      walk.push(id || (document.activeElement ? document.activeElement.className : "none"));
      var active = document.activeElement;
      if (active && active.closest && active.closest("[data-section=\"graphics\"] .row")) {
        passedGraphics = true; // a control or the "i" button of a Graphics row
      }
      if (active && active.closest && active.closest("[data-section=\"advanced\"] .row")) {
        reached = true; // a control or the "i" button of an Advanced row
      }
      if (id === "reset-defaults") {
        reset = true;
      }
    }
    check("D-pad down from the Display rows passes through the Graphics rows", passedGraphics, "walk " + walk.join(" > "));
    check("D-pad down from the Graphics rows reaches the Advanced rows", reached, "walk " + walk.join(" > "));
    check("D-pad down reaches Reset to defaults at the end of the section", reset, "focus " + focusedSetting());
    check("the Graphics content footer offers Y Reset", new RegExp(T("hint.reset")).test(footerText()), footerText());
    var inSection = document.querySelector("[data-section=\"advanced\"] [data-setting=\"perf_overlay\"]");
    check("our performance overlay row is in the Advanced section", !!inSection, "no perf_overlay row");
    var osd = document.querySelector(".content [data-setting=\"osd_fps\"], .content [data-setting=\"aspect\"], " +
      ".content [data-setting=\"input_resolution\"], .content [data-setting=\"widescreen\"], " +
      ".content [data-setting=\"aspect_ratio\"], .content [data-setting=\"upscale\"]");
    check("no PCSX2 OSD, aspect, input resolution or widescreen row is shown", !osd, osd ? osd.getAttribute("data-setting") : "");
    press("ArrowUp");
    await wait(80);
    press("Escape");
    await wait(150);
    check("B inside the Advanced section returns to the tab list on Graphics", sideStepOk("graphics"), sideDetail());
  }

  async function scenarioAdvanced() {
    GT4.nav.setMode("pad");
    await setHash("#/settings/graphics");
    press("ArrowRight");
    await wait(200);
    var first = document.querySelector("[data-section=\"advanced\"] [data-setting]");
    if (first) {
      first.focus();
      first.scrollIntoView({ block: "center" });
    }
    await wait(300);
  }

  // The screenshot state of docs/launcher/shots/graphics-sections.png: pad focus on Render scale, centred, so the end
  // of the Display section, the Graphics section and the start of Advanced are in view.
  async function scenarioRender() {
    GT4.nav.setMode("pad");
    await setHash("#/settings/graphics");
    press("ArrowRight");
    await wait(200);
    var scale = document.querySelector("input[type=range][data-setting=\"render_scale\"]");
    if (scale) {
      scale.focus();
      scale.scrollIntoView({ block: "center" });
    }
    await wait(300);
  }

  // Controls (js/pages/controls.js): entry on the DS2, spatial moves, rebind with cancel, conflicts, live input, reset.
  function key(name, code) {
    document.dispatchEvent(new KeyboardEvent("keydown", { key: name, code: code || name, bubbles: true, cancelable: true }));
    document.dispatchEvent(new KeyboardEvent("keyup", { key: name, code: code || name, bubbles: true, cancelable: true }));
  }

  function focusedControl() {
    var active = document.activeElement;
    var group = active && active.closest ? active.closest(".pad-ctl") : null;
    return group ? group.getAttribute("data-control") : null;
  }

  function cardName() {
    var name = document.querySelector(".cx-card .cx-name");
    return name ? name.textContent : null;
  }

  function badgeLabel(id) {
    var badge = document.querySelector(".badge[data-for=\"" + id + "\"]");
    return badge ? badge.getAttribute("data-label") : null;
  }

  async function controlsOf(id) {
    var view = await GT4.backend.call("get_controls");
    return view.controls.filter(function (control) { return control.id === id; })[0];
  }

  async function controlsChecks() {
    GT4.devGamepads = function () { return []; };
    await setHash("#/settings/controls");
    await wait(400);
    check("the Controls preview with live input running keeps focus on the tab list", sideStepOk("controls"), sideDetail());
    press("ArrowRight");
    await wait(200);
    check("entering Controls focuses Cross on the drawing", focusedControl() === "cross", "focus " + focusedControl());
    check("the Controls footer offers Rebind, Reset button and Reset all", footerText().indexOf(T("controls.rebind")) >= 0 && footerText().indexOf(T("hint.reset_button")) >= 0 && footerText().indexOf(T("hint.reset_all")) >= 0, footerText());

    press("ArrowUp");
    await wait(120);
    check("D-pad up from Cross reaches Triangle", focusedControl() === "triangle", "focus " + focusedControl());
    press("ArrowDown");
    await wait(120);
    press("ArrowLeft");
    await wait(120);
    check("D-pad left from Cross reaches Square", focusedControl() === "square", "focus " + focusedControl());
    check("the selection follows focus (panel shows Square)", cardName() === T("controls.button.square"), cardName());
    press("ArrowLeft");
    await wait(120);
    check("D-pad left from Square reaches Start", focusedControl() === "start", "focus " + focusedControl());
    press("ArrowRight");
    await wait(120);

    // Rebind, then cancel with Esc (B on the pad goes through the same GT4.cancelCapture).
    GT4.nav.activate();
    await wait(200);
    check("A on a DS2 button opens the rebind overlay", !!document.querySelector(".modal.is-capture") && GT4.capturing === true, "capturing " + GT4.capturing);
    check("the overlay names the button", new RegExp(T("controls.button.square")).test((document.querySelector(".modal.is-capture h3") || {}).textContent || ""), "no title");
    check("the rebind footer shows only B Cancel", footerText() === "BCancel", footerText());
    press("Escape");
    await wait(200);
    check("Esc cancels the rebind and closes the overlay", !document.querySelector(".modal.is-capture") && GT4.capturing === false, "capturing " + GT4.capturing);
    check("after a cancel, focus is back on the button", focusedControl() === "square", "focus " + focusedControl());
    check("a cancelled rebind changes nothing", (await controlsOf("square")).key === "A", "key " + (await controlsOf("square")).key);

    // Enter on a focused button starts a rebind; that same Enter is not taken as the new key.
    var squareGroup = document.querySelector(".pad-ctl[data-control=\"square\"]");
    squareGroup.dispatchEvent(new KeyboardEvent("keydown", { key: "Enter", code: "Enter", bubbles: true, cancelable: true }));
    squareGroup.dispatchEvent(new KeyboardEvent("keyup", { key: "Enter", code: "Enter", bubbles: true, cancelable: true }));
    await wait(200);
    check("Enter on a DS2 button opens the rebind and keeps waiting", !!document.querySelector(".modal.is-capture") && GT4.capturing === true, "capturing " + GT4.capturing);
    press("Escape");
    await wait(200);
    check("Enter did not become the binding", (await controlsOf("square")).key === "A", "key " + (await controlsOf("square")).key);

    // A controller button already on another DS2 button: the dev mock's capture answers RB (R1's); the conflict
    // dialog opens. Esc keeps everything. (Keyboard rebinding is off: a key only cancels.)
    GT4.nav.activate();
    await wait(2600);
    var dialogTitle = (document.querySelector(".modal h3") || {}).textContent || "";
    check("a pad button already on R1 opens the conflict dialog", /already on R1|já está em R1/.test(dialogTitle), dialogTitle);
    press("Escape");
    await wait(200);
    check("cancelling the conflict changes nothing", (await controlsOf("square")).xinput === "X" && (await controlsOf("r1")).xinput === "RB", "square/r1 changed");

    // Swap: Square takes RB from R1, R1 gets X (Square's previous controller button). The chips follow at once.
    GT4.nav.activate();
    await wait(2600);
    var swap = Array.prototype.filter.call(document.querySelectorAll(".modal button"), function (button) {
      return button.textContent === T("controls.swap");
    })[0];
    check("the conflict dialog offers Swap", !!swap, "no Swap button");
    if (swap) {
      swap.click();
      await wait(300);
    }
    var square = await controlsOf("square");
    var r1 = await controlsOf("r1");
    check("Swap gives Square RB and R1 X", square.xinput === "RB" && r1.xinput === "X", square.xinput + "/" + r1.xinput);
    check("the chips show the swap (controller view)", badgeLabel("square") === "RB" && badgeLabel("r1") === "X", badgeLabel("square") + "/" + badgeLabel("r1"));
    check("a swapped button reads Changed in the panel", (document.querySelector(".cx-card .cx-state") || {}).textContent === T("controls.changed"), "state");

    // Live input from the pad events (a still dev pad, sent by the mock like the Rust service): Cross lights from A,
    // L2 fills to 60%.
    GT4.devGamepads = function () {
      var buttons = GT4.controlsModel.STANDARD_TO_XINPUT.map(function (name) {
        var value = name === "A" ? 1 : (name === "LT" ? 0.6 : 0);
        return { pressed: value > 0.12, value: value };
      });
      return [{ id: "Test Pad (XInput STANDARD GAMEPAD)", index: 0, connected: true, buttons: buttons, axes: [1, 0, 0, -1] }];
    };
    await wait(200);
    var cross = document.querySelector(".pad-ctl[data-control=\"cross\"]");
    check("pad A lights Cross on the drawing", !!cross && cross.classList.contains("is-down"), "no is-down");
    var level = document.querySelector(".pad-ctl[data-control=\"l2\"] .level");
    check("LT at 60% fills L2 to 60%", !!level && Math.abs(Number(level.getAttribute("height")) - 42 * 0.6) < 0.2, level ? level.getAttribute("height") : "none");
    var capL = document.querySelector(".pad-ctl[data-control=\"l3\"] .stick-cap");
    check("the left stick moves to its real position", !!capL && capL.getAttribute("transform") === "translate(24 0)", capL ? capL.getAttribute("transform") : "none");
    check("the status names the controller", /Test Pad/.test((document.querySelector(".cx-status") || {}).textContent || ""), "status");
    check("live input never moves focus", focusedControl() === "square", "focus " + focusedControl());
    // TASK-161: the full trigger range toggle sits above L2 and R2, on by default; the L2 trigger meter shows the whole byte.
    var fullRange = document.querySelector(".cx-analog .selector[data-setting=\"trigger_full_range\"]");
    check("the full trigger range toggle is on the Controls page, On", !!fullRange && selectorText("trigger_full_range") === T("settings.on"),
      fullRange ? selectorText("trigger_full_range") : "none");
    var meterL2 = document.querySelectorAll(".cx-meter-v")[0];
    var barL2 = document.querySelectorAll(".cx-meter")[0];
    if (meterL2 && barL2) {
      // The bar keeps its width whatever the number says (the widest text is reserved).
      var shown = meterL2.textContent;
      meterL2.textContent = T("controls.meter_value", { percent: 0, pressure: 0, full: 255 });
      var narrow = barL2.getBoundingClientRect().width;
      meterL2.textContent = T("controls.meter_value", { percent: 100, pressure: 255, full: 255 });
      var wide = barL2.getBoundingClientRect().width;
      meterL2.textContent = shown;
      check("the trigger bar keeps its width from 0% to 100%", narrow > 0 && Math.abs(narrow - wide) < 0.5, narrow + " / " + wide);
    }
    check("the L2 trigger meter shows the whole byte (of 255)", !!meterL2 && /\/255\)$/.test(meterL2.textContent), meterL2 ? meterL2.textContent : "none");
    if (fullRange) {
      fullRange.click();
      await wait(400);
      check("the full trigger range toggle turns Off", selectorText("trigger_full_range") === T("settings.off"), selectorText("trigger_full_range"));
      meterL2 = document.querySelectorAll(".cx-meter-v")[0];
      check("without the full trigger range the L2 trigger meter shows the game scale (of 84)", !!meterL2 && /\/84\)$/.test(meterL2.textContent),
        meterL2 ? meterL2.textContent : "none");
      selectorNode("trigger_full_range").click();
      await wait(400);
      check("the full trigger range toggle turns back On", selectorText("trigger_full_range") === T("settings.on"), selectorText("trigger_full_range"));
      // Back to Square on the drawing, where the checks below expect the selection.
      var squareNode = document.querySelector(".pad-ctl[data-control=\"square\"]");
      if (squareNode) {
        squareNode.focus();
        await wait(100);
      }
    }
    GT4.devGamepads = function () { return []; };
    await wait(150);
    check("released buttons go dark", !cross.classList.contains("is-down"), "still down");

    // Y resets the selected button; X resets everything after a confirmation.
    GT4.hooks.y();
    await wait(300);
    check("Y resets Square to its default key", (await controlsOf("square")).key === "A", (await controlsOf("square")).key);
    GT4.hooks.x();
    await wait(150);
    var resetAll = Array.prototype.filter.call(document.querySelectorAll(".modal button"), function (button) {
      return button.textContent === T("controls.reset_all_hint");
    })[0];
    check("X asks before resetting everything", !!resetAll, "no confirmation");
    if (resetAll) {
      resetAll.click();
      await wait(300);
    }
    check("Reset all restores L1", (await controlsOf("l1")).key === "Q" && (await controlsOf("l1")).is_default, "l1 " + (await controlsOf("l1")).key);
    press("Escape");
    await wait(150);
    check("B returns Controls to the tab list", sideStepOk("controls"), sideDetail());
    delete GT4.devGamepads;
  }

  // The screenshot state (scenario "4k", name kept for the screenshot list): Graphics with Render scale at 200%, the
  // slider focused in pad mode (TASK-080 addendum: there is no Internal resolution row any more).
  async function scenario4k() {
    GT4.nav.setMode("pad");
    await setHash("#/settings/graphics");
    press("ArrowRight");
    await wait(200);
    var scale = document.querySelector("input[type=range][data-setting=\"render_scale\"]");
    if (scale) {
      scale.focus({ preventScroll: true });
      for (var i = 0; i < 4; i++) {
        var current = document.querySelector("input[type=range][data-setting=\"render_scale\"]");
        current.focus({ preventScroll: true });
        current.dispatchEvent(new CustomEvent("gt4-adjust", { detail: { delta: 1 } }));
        await wait(250);
      }
    }
    await wait(400);
  }

  // Two games (TASK-101): the version switch under Play (the name between two arrows, an endless loop; owner, 2026-10-23),
  // Play or Install, Settings > Games and the game on the Saves title. The mock starts with Gran Turismo 4 installed and
  // Spec II not (?games=gt4).
  async function achievementsChecks() {
    await setHash("#/settings/achievements");
    await wait(100);
    var page = document.querySelector(".ra-page");
    check("Achievements has a signed-out account state", !!page && page.textContent.indexOf(T("ra.not_signed_in")) >= 0, "no signed-out state");
    var signIn = page.querySelector("button.primary");
    check("Achievements has a Sign in button", !!signIn, "no sign-in button");
    signIn.click();
    await wait(60);
    var modal = document.querySelector(".modal");
    check("Achievements opens the account login dialog", !!modal && !!modal.querySelector("input[type=password]"), "no password field");
    modal.querySelector("input[type=text]").value = "TestDriver";
    modal.querySelector("input[type=password]").value = "test-only-password";
    modal.querySelector("button.primary").click();
    await wait(150);
    page = document.querySelector(".ra-page");
    check("Achievements paints the signed-in account", page.textContent.indexOf("TestDriver") >= 0, "no account name");
    check("Achievements paints the synthetic progress list", page.querySelectorAll(".ra-ach-row").length === 3, "missing sample rows");
    var hcBtn = page.querySelector(".selector[aria-label=\"" + T("ra.opt_hardcore") + "\"]");
    if (hcBtn) {
      hcBtn.click();
      await wait(100);
      var saved = await GT4.backend.call("ra_status", {});
      check("Achievements saves the Hardcore toggle", saved.hardcore === true, "option was not saved");
    }

    // Hold an old signed-in status response while a real UI logout completes.
    var originalCall = GT4.backend.call;
    var release = null;
    var deferStatus = true;
    var currentSaved = await originalCall("ra_status", {});
    GT4.backend.call = function (command, args) {
      if (command === "ra_status" && deferStatus) {
        deferStatus = false;
        return new Promise(function (resolve) { release = function () { resolve(currentSaved); }; });
      }
      return originalCall(command, args);
    };
    try {
      page.querySelector("button.danger").click();
      await wait(40);
      document.querySelector(".modal button.danger").click();
      await wait(100);
      check("Achievements logout clears the account", !(await originalCall("ra_status", {})).signedIn, "mock still signed in");
      if (release) release();
      await wait(100);
      check("a late status response cannot undo UI logout", document.querySelector(".ra-page").textContent.indexOf(T("ra.not_signed_in")) >= 0, "old account reappeared");
    } finally {
      GT4.backend.call = originalCall;
    }

    // Target isolation check: when Spec II is selected, achievement set is NOT shown and unsupported banner is present
    await setHash("#/settings/games");
    await GT4.games.select("spec2");
    await wait(100);
    await setHash("#/settings/achievements");
    await wait(150);
    page = document.querySelector(".ra-page");
    check("Spec II shows unsupported banner", !!page && !!page.querySelector(".ra-unsupported-banner"), "no unsupported banner");
    check("Spec II does not show GT4 achievements list", page.querySelectorAll(".ra-ach-row").length === 0, "achievements list shown on Spec II");

    // Restore GT4 target
    await GT4.games.select("gt4");
    await wait(100);
  }

  async function gamesChecks() {
    await setHash("#/home");
    await wait(500);
    var arrows = document.querySelectorAll(".version-arrow");
    // The name that is shown (a leaving name may still be sliding out beside it).
    var currentName = function () {
      var names = document.querySelectorAll(".version-name:not(.is-leaving)");
      return names[names.length - 1];
    };
    var nameText = function () {
      var names = document.querySelectorAll(".version-name:not(.is-leaving)");
      return names.length ? names[names.length - 1].textContent : "";
    };
    var playText = function () { return document.querySelector(".play").textContent; };
    check("Home has the version switch: two arrows around Normal", arrows.length === 2 && nameText() === T("home.normal"),
      arrows.length + " " + nameText());
    check("Gran Turismo 4 is selected first and shows Play", playText() === T("common.play"), playText());
    check("the logo is the Gran Turismo 4 logo, with no Spec II line", !document.querySelector(".home-logo-spec2") &&
      !!document.querySelector(".home-logo"), "");
    arrows[1].click();
    await wait(400);
    check("the right arrow switches to Spec II and shows Install (no other not-installed mark)", nameText() === "Spec II" &&
      playText() === T("common.install") && !document.querySelector(".version-note"), nameText() + " " + playText());
    check("the new name came in from the right", currentName().classList.contains("enter-from-right"), "");
    arrows[1].click();
    await wait(400);
    check("the loop is endless: right again comes back to Normal", nameText() === T("home.normal") && playText() === T("common.play"), nameText());
    arrows[0].click();
    await wait(400);
    check("the left arrow switches too, sliding in from the left", nameText() === "Spec II" &&
      currentName().classList.contains("enter-from-left"), nameText());
    GT4.hooks.tabs(-1);
    await wait(400);
    check("LB / RB switches back to Normal and Play", playText() === T("common.play") && nameText() === T("home.normal"), nameText());
    var notes = document.querySelector(".home-notes");
    check("the patch notes card shows this version's notes (TASK-099)", !!notes && !notes.hidden &&
      !!notes.querySelector(".home-notes-body .md-h2") && notes.querySelectorAll(".home-notes-body li").length > 0 &&
      document.querySelector(".home-notes-version").textContent.length > 0, notes ? "card" : "none");
    check("the notes title line is not repeated", !notes.querySelector(".md-h1"), "");
    // Safety of js/markdown.js on its own (not shown in the mock): HTML in the notes stays plain text.
    var sample = document.createElement("div");
    sample.appendChild(GT4.markdown.render("- <b>raw HTML</b> and <img src=x> stay text", {}));
    check("HTML written in release notes shows as plain text", !sample.querySelector("b") && !sample.querySelector("img") &&
      /<b>raw HTML<\/b>/.test(sample.textContent), sample.textContent);
    check("the notes card has no footer link", !notes.querySelector(".home-notes-more"), "");
    // The 1.3.0 intro (js/intro.js): it covers the screen, a key skips it without reaching the page, and it shows once
    // per version.
    GT4.intro.play();
    check("the intro covers the screen with spec-II", !!document.querySelector(".intro") &&
      document.querySelector(".intro-spec").textContent === "spec-II" && GT4.intro.isPlaying(), "");
    document.dispatchEvent(new KeyboardEvent("keydown", { key: "Enter", bubbles: true }));
    await wait(800);
    check("a key skips the intro and does not press Play", !GT4.intro.isPlaying() && !document.querySelector(".intro") &&
      document.querySelector(".play").textContent === T("common.play"), "");
    try { localStorage.removeItem("gt4recomp.intro.seen"); } catch (error) { /* storage off */ }
    var first = GT4.intro.maybePlay("1.3.0");
    GT4.intro.skip();
    var second = GT4.intro.maybePlay("1.3.0");
    var other = GT4.intro.maybePlay("1.2.3");
    await wait(800);
    check("the intro plays once for 1.3.0 and never for a version without one", first && !second && !other, [first, second, other].join(" "));
    var support = document.querySelector(".support");
    check("Support the project with the Ko-fi cup sits on the home screen", !!support && !!support.querySelector(".support-cup") &&
      new RegExp(T("home.support")).test(support.textContent), "");
    var credit = document.querySelector(".home-version");
    check("the version line credits Moesuito as a link", /GT4Recomp .* - Made with passion, by Moesuito$/.test(credit.textContent) &&
      !!credit.querySelector(".home-author"), credit.textContent);
    var older = notes.querySelectorAll(".notes-arrow")[0];
    var newer = notes.querySelectorAll(".notes-arrow")[1];
    var shownVersion = function () { return document.querySelector(".home-notes-version").textContent; };
    check("the notes open on the newest release; newer is disabled at the start", shownVersion() === "1.3.0" &&
      newer.disabled && !older.disabled, shownVersion());
    older.click();
    await wait(100);
    check("the left arrow shows an older release, sliding in from the left", shownVersion() === "1.2.3" &&
      !!notes.querySelector(".home-notes-page.enter-from-left") && !newer.disabled, shownVersion());
    for (var step = 0; step < 10 && !older.disabled; step++) {
      older.click();
    }
    await wait(100);
    check("no loop: the oldest release disables the left arrow", shownVersion() === "1.0.0" && older.disabled, shownVersion());
    older.click();
    await wait(100);
    check("an extra click at the oldest stays there", shownVersion() === "1.0.0", shownVersion());
    newer.click();
    await wait(100);
    check("the right arrow goes back to a newer release", shownVersion() === "1.1.0" &&
      !!notes.querySelector(".home-notes-page.enter-from-right"), shownVersion());
    check("the pad hints name the game switch", /LB \/ RB/.test(document.getElementById("hints").textContent), document.getElementById("hints").textContent);

    await setHash("#/settings/games");
    await wait(400);
    var rows = document.querySelectorAll(".game-row");
    check("Settings > Games lists both games", rows.length === 2, String(rows.length));
    var labels = Array.prototype.map.call(rows[0].querySelectorAll("button"), function (b) { return b.textContent; });
    var imageLine = rows[0] && rows[0].querySelector(".game-image");
    check("an installed game names its disc image and where it is", !!imageLine && imageLine.textContent === T("games.image_at", { path: "D:\\ISOs\\Gran Turismo 4 (USA v2.00).iso" }), imageLine ? imageLine.textContent : "no line");
    check("the size shown is the disc image's, not 0.0 GB", !!rows[0] && rows[0].querySelector(".val").textContent.indexOf(GT4.games.size(5314478080)) >= 0, rows[0] ? rows[0].querySelector(".val").textContent : "no row");
    check("a small size reads in megabytes", GT4.games.size(8100000) === T("common.mb", { size: (8).toLocaleString(GT4.i18n.intlLocale()) }), GT4.games.size(8100000));
    check("an installed game offers Update and Remove", labels.join("|") === T("games.update") + "|" + T("common.remove"), labels.join("|"));
    var installedDetail = rows[0].querySelector(".val").textContent;
    check("an installed game names its copy: status, USA v2.00 and size, not the title again",
      installedDetail.indexOf(T("games.installed") + " · USA v2.00 · ") === 0 && installedDetail.indexOf("Gran Turismo") < 0, installedDetail);
    check("Spec II is titled with its supported version", rows[1].querySelector(".k span").textContent === "Gran Turismo 4 Spec II v1.10",
      rows[1].querySelector(".k span").textContent);
    var other = Array.prototype.map.call(rows[1].querySelectorAll("button"), function (b) { return b.textContent; });
    check("a game that is not installed offers Install", other.join("|") === T("common.install"), other.join("|"));
    rows[0].querySelectorAll("button")[1].click();
    await wait(200);
    var dialog = document.querySelector(".modal");
    check("Remove asks first and says the saves are kept", !!dialog && /saves and replays are kept|saves e replays são mantidos/i.test(dialog.textContent), dialog ? dialog.textContent : "no dialog");
    if (dialog) {
      GT4.ui.topDialog().close();
    }

    await setHash("#/settings/saves");
    await wait(400);
    // One Saves list for both games (TASK-101 item 2): each row carries its game's tag, and the list does not change
    // with the game shown; the Replays title still names the game.
    var rowTag = document.querySelector(".slot-row .game-tag");
    check("the Saves rows carry the game tag", !!rowTag && rowTag.textContent === T("home.normal"), rowTag ? rowTag.textContent : "no tag");
    check("the Saves list shows the Spec II save too", !!document.querySelector(".slot-row[data-item-id^=\"spec2:\"]"), "no spec2 row");
    await GT4.games.select("spec2");
    await setHash("#/settings/replays");
    await wait(400);
    var tag = document.querySelector(".game-tag");
    check("the Replays title names the game", !!tag && tag.textContent === "Gran Turismo 4 Spec II", tag ? tag.textContent : "no tag");
    await setHash("#/settings/saves");
    await wait(400);
    check("the Spec II save row is tagged Spec II", !!document.querySelector(".slot-row[data-item-id^=\"spec2:\"] .game-tag") &&
      document.querySelector(".slot-row[data-item-id^=\"spec2:\"] .game-tag").textContent === T("home.spec2"), "spec2 tag");
    await GT4.games.select("gt4");
  }


  // ---------- Locales (TASK-102): every language has the keys of en-US; index.json lists only files that exist ----------
  function fetchJson(name) {
    return fetch("locales/" + name, { cache: "no-cache" }).then(function (response) {
      return response.ok ? response.json() : null;
    }, function () {
      return null;
    });
  }

  function placeholdersOf(text) {
    return (String(text).match(/\{[A-Za-z0-9_]+\}/g) || []).slice().sort().join(",");
  }

  async function localeChecks() {
    var index = await fetchJson("index.json");
    check("index.json lists the languages", Array.isArray(index) && index.length >= 2, String(index && index.length));
    var english = await fetchJson("en-US.json");
    check("en-US.json exists (the fallback)", !!english, "missing");
    if (!Array.isArray(index) || !english) {
      return;
    }
    for (var i = 0; i < index.length; i++) {
      var tag = index[i].tag;
      var file = await fetchJson(tag + ".json");
      check("index.json lists " + tag + " and its file exists", !!file, "no locales/" + tag + ".json");
      if (!file || tag === "en-US") {
        continue;
      }
      var missing = Object.keys(english).filter(function (key) { return !Object.prototype.hasOwnProperty.call(file, key); });
      var extra = Object.keys(file).filter(function (key) { return !Object.prototype.hasOwnProperty.call(english, key); });
      check(tag + " has every key of en-US", missing.length === 0, "missing " + missing.slice(0, 5).join(", "));
      check(tag + " has no keys that en-US lacks", extra.length === 0, "extra " + extra.slice(0, 5).join(", "));
      var badPlaceholder = Object.keys(english).filter(function (key) {
        return typeof file[key] === "string" && placeholdersOf(file[key]) !== placeholdersOf(english[key]);
      });
      check(tag + " keeps the {placeholders} of en-US", badPlaceholder.length === 0, badPlaceholder.slice(0, 5).join(", "));
      var empty = Object.keys(file).filter(function (key) { return typeof file[key] !== "string" || !file[key].trim(); });
      check(tag + " has no empty texts", empty.length === 0, empty.slice(0, 5).join(", "));
    }
    check("the Auto rule picks pt-BR for pt-BR, pt_BR and pt", GT4.i18n.resolve("auto", "pt-BR") === "pt-BR" &&
      GT4.i18n.resolve("auto", "pt_BR") === "pt-BR" && GT4.i18n.resolve("auto", "pt") === "pt-BR", "rule");
    check("the Auto rule picks en-US for es-ES, fr and empty text", GT4.i18n.resolve("auto", "es-ES") === "en-US" &&
      GT4.i18n.resolve("auto", "fr") === "en-US" && GT4.i18n.resolve("auto", "") === "en-US", "rule");
    check("an explicit language wins over the OS", GT4.i18n.resolve("en-US", "pt-BR") === "en-US" &&
      GT4.i18n.resolve("pt-BR", "en-US") === "pt-BR", "rule");
  }

  // The option selector (owner, 2026-11-02): no start or end, the arrows step both ways, and a step that changes the tab's rows
  // animates them and does not stop the value's slide.
  function selectorNode(id) {
    return document.querySelector(".selector[data-setting=\"" + id + "\"]");
  }

  function selectorText(id) {
    var node = selectorNode(id);
    return node ? node.getAttribute("aria-valuetext") : null;
  }

  async function stepSelector(id, direction) {
    var node = selectorNode(id);
    node.focus({ preventScroll: true });
    node.dispatchEvent(new CustomEvent("gt4-adjust", { detail: { delta: direction } }));
    await wait(150);
  }

  async function stepSelectorUntil(id, reached) {
    for (var count = 0; count < 24; count++) {
      if (reached()) {
        return true;
      }
      if (!selectorNode(id)) {
        return false;
      }
      await stepSelector(id, 1);
    }
    return reached();
  }

  async function selectorLoopChecks() {
    var vsync = selectorNode("vsync");
    vsync.click();
    await wait(200);
    check("a step that moves nothing else keeps the control on screen", selectorNode("vsync") === vsync, "the tab was drawn again");
    vsync.click();
    await wait(200);

    var api = GT4.settingsModel.findRow(await GT4.backend.call("get_settings_table"), "graphics_api");
    var choices = api.values.filter(function (option) { return !option.disabled; }).length;
    var first = selectorText("graphics_api");
    for (var count = 0; count < choices; count++) {
      await stepSelector("graphics_api", 1);
    }
    check("going right through every option comes back to the first", choices > 1 && selectorText("graphics_api") === first,
      selectorText("graphics_api") + " after " + choices + " steps");
    await stepSelector("graphics_api", -1);
    var last = selectorText("graphics_api");
    check("going left from the first option reaches the last", last !== first, last);
    selectorNode("graphics_api").querySelectorAll(".selector-arrow")[1].click();
    await wait(150);
    check("the right arrow steps the selector", selectorText("graphics_api") === first, selectorText("graphics_api"));
    selectorNode("graphics_api").querySelectorAll(".selector-arrow")[0].click();
    await wait(150);
    check("the left arrow steps the selector", selectorText("graphics_api") === last, selectorText("graphics_api"));
    selectorNode("graphics_api").querySelectorAll(".selector-arrow")[1].click();
    await wait(150);

    var renderer = GT4.settingsModel.findRow(await GT4.backend.call("get_settings_table"), "renderer").current;
    await GT4.backend.call("set_setting", { id: "renderer", value: "pcsx2" });
    await setHash("#/settings/general");
    await setHash("#/settings/graphics");
    selectorNode("renderer").focus({ preventScroll: true });
    selectorNode("renderer").dispatchEvent(new CustomEvent("gt4-adjust", { detail: { delta: 1 } }));
    await wait(120);
    var drawn = selectorNode("renderer");
    check("a renderer step draws its own rows", !!selectorNode("frame_rate") && !selectorNode("graphics_api"), "rows not changed");
    check("a renderer step keeps the value's slide going", !!drawn && drawn.querySelectorAll(".selector-value.is-leaving").length === 1,
      drawn ? drawn.innerHTML.slice(0, 120) : "no control");
    var newRow = document.querySelector("[data-row=\"frame_rate\"]");
    check("a new row comes in with an animation", !!newRow && newRow.getAnimations().length > 0, "no animation");
    check("a row going out fades over the page", document.querySelectorAll(".row-departing").length > 0, "no departing row");
    check("the focus stays on the renderer through the redraw", document.activeElement === drawn, "focus elsewhere");
    await wait(500);
    check("the rows that went out are gone after the animation", document.querySelectorAll(".row-departing").length === 0, "left behind");
    await GT4.backend.call("set_setting", { id: "renderer", value: renderer });
    await setHash("#/settings/general");
    await setHash("#/settings/graphics");
  }

  // TASK-174: the Presentation thread row. CoreGS 2.0 only; works on a one-queue card (and when the queue count is unknown);
  // shown but not available, with a note, on a card with two or more graphics queues.
  async function presentThreadChecks() {
    function shown() {
      return Array.prototype.map.call(document.querySelectorAll("[data-row]"), function (node) { return node.getAttribute("data-row"); });
    }
    function valueOf(table) {
      return GT4.settingsModel.findRow(table, "present_thread");
    }
    var renderer = GT4.settingsModel.findRow(await GT4.backend.call("get_settings_table"), "renderer").current;
    await GT4.backend.call("set_setting", { id: "renderer", value: "pcsx2" });
    await setHash("#/settings/general");
    await setHash("#/settings/graphics");
    await wait(200);
    check("the Presentation thread row is not shown under GSdx", shown().indexOf("present_thread") < 0, shown().join("|"));
    await GT4.backend.call("set_setting", { id: "renderer", value: "coregs20" });
    await setHash("#/settings/general");
    await setHash("#/settings/graphics");
    await wait(200);
    var order = shown();
    check("the Presentation thread row is shown under CoreGS 2.0, after the frame rate", order.indexOf("frame_rate") >= 0 && order.indexOf("present_thread") > order.indexOf("frame_rate"), order.join("|"));
    var node = document.querySelector("[data-row=\"present_thread\"]");
    var label = node && node.querySelector(".k span");
    check("the row's label is the translated one", !!label && label.textContent === T("setting.present_thread.label") && label.textContent.length > 10, label ? label.textContent : "no row");
    var tip = node && node.querySelector(".tip");
    check("the info text is the translated one and explains the single queue", !!tip && tip.textContent.indexOf(T("setting.present_thread.help")) >= 0 && T("setting.present_thread.help").length > 60,
      tip ? tip.textContent.slice(0, 80) : "no tip");
    check("the row is on by default and works (not disabled, no note)", selectorText("present_thread") === T("settings.on") && !node.classList.contains("is-disabled") && !node.querySelector(".hint-text"),
      node ? node.className : "no row");
    await stepSelector("present_thread", 1);
    check("a step turns it off and the table holds the value", valueOf(await GT4.backend.call("get_settings_table")).current === false && selectorText("present_thread") === T("settings.off"),
      String(selectorText("present_thread")));
    await stepSelector("present_thread", -1);
    check("a step back turns it on again", valueOf(await GT4.backend.call("get_settings_table")).current === true && selectorText("present_thread") === T("settings.on"), String(selectorText("present_thread")));

    // A card with two or more graphics queues: the launcher sends the row disabled with a note key.
    var real = GT4.backend.call;
    GT4.backend.call = function (command, args) {
      var result = real.call(GT4.backend, command, args);
      if (command !== "get_settings_table") {
        return result;
      }
      return result.then(function (table) {
        var row = valueOf(table);
        row.disabled = true;
        row.note_key = "settings.already_active";
        return table;
      });
    };
    try {
      await setHash("#/settings/general");
      await setHash("#/settings/graphics");
      await wait(250);
      var off = document.querySelector("[data-row=\"present_thread\"]");
      var hint = off && off.querySelector(".hint-text");
      check("a two-queue card shows the row disabled", !!off && off.classList.contains("is-disabled") && off.querySelector(".selector[aria-disabled=\"true\"]") !== null, off ? off.className : "no row");
      check("a two-queue card says it is already active, in this language", !!hint && hint.textContent === T("settings.already_active") && hint.textContent.length > 8, hint ? hint.textContent : "no note");
      check("the disabled row keeps its info text", !!off && off.querySelector(".tip").textContent.indexOf(T("setting.present_thread.help")) >= 0, "no tip");
    } finally {
      GT4.backend.call = real;
    }
    await GT4.backend.call("set_setting", { id: "renderer", value: renderer });
    await setHash("#/settings/general");
    await setHash("#/settings/graphics");
  }

  // Owner, 2026-11-02: choosing GSdx when it is missing asks first. The dialog shows the download and closes by itself; declining,
  // or a failed download, goes back to CoreGS 2.0, and the choice is stored only after the install.
  async function gsdxDialogChecks() {
    var real = GT4.backend.call;
    var needed = true;
    var download = null;
    var calls = [];
    GT4.backend.call = function (command, args) {
      if (command === "gsdx_download_needed") {
        return Promise.resolve(needed);
      }
      if (command === "download_gsdx") {
        calls.push(command);
        return new Promise(function (resolve, reject) { download = { resolve: resolve, reject: reject }; });
      }
      if (command === "install_gsdx_archive") {
        calls.push(command);
        return new Promise(function (resolve, reject) { download = { resolve: resolve, reject: reject }; });
      }
      return real(command, args);
    };
    var storedRenderer = async function () {
      return GT4.settingsModel.findRow(await real("get_settings_table"), "renderer").current;
    };
    var openFromCoregs = async function () {
      await real("set_setting", { id: "renderer", value: "coregs20" });
      await setHash("#/settings/general");
      await setHash("#/settings/graphics");
      await stepSelector("renderer", 1);
      await wait(150);
    };
    var modal = function () { return document.querySelector(".modal"); };
    var buttons = function () {  // the buttons shown (TASK-158 keeps Install from a file hidden until a failure)
      return Array.prototype.filter.call(modal().querySelectorAll(".modal-actions button"), function (button) { return button.style.display !== "none"; });
    };
    var before = await storedRenderer();
    try {
      await openFromCoregs();
      check("choosing GSdx when it is missing opens the dialog", !!modal() && modal().textContent.indexOf(T("settings.gsdx_title")) >= 0, "no dialog");
      check("nothing is stored while the dialog asks", (await storedRenderer()) === "coregs20", await storedRenderer());
      if (!modal()) {
        return;
      }
      buttons()[1].click();
      await wait(450);
      check("cancelling closes the dialog", !modal(), "still open");
      check("cancelling goes back to CoreGS 2.0", selectorText("renderer") === T("choice.renderer.coregs20") && (await storedRenderer()) === "coregs20", selectorText("renderer"));
      check("after cancelling the focus is on the renderer", document.activeElement === selectorNode("renderer"), "focus elsewhere");

      await openFromCoregs();
      GT4.ui.topDialog().back();
      await wait(450);
      check("Back also cancels", !modal() && selectorText("renderer") === T("choice.renderer.coregs20"), selectorText("renderer"));

      await openFromCoregs();
      buttons()[0].click();
      await wait(100);
      check("accepting shows the progress in the dialog", !!modal() && !modal().querySelector(".gsdx-progress").hidden && modal().querySelector(".gsdx-status").textContent !== "", "no progress");
      root.GT4_MOCK.emit("gsdx-download", { step: "downloading", bytesDone: 24000000, bytesTotal: 48000000 });
      await wait(50);
      check("the dialog shows the download's figures", modal().querySelector(".gsdx-figures").textContent === T("setup.renderer_count", { done: "24.0 MB", total: "48.0 MB" }) && modal().querySelector(".bar i").style.width === "50%", modal().querySelector(".gsdx-figures").textContent);
      GT4.ui.topDialog().back();
      check("the download cannot be dismissed", !!modal(), "closed");
      check("the choice waits for the install", (await storedRenderer()) === "coregs20", await storedRenderer());
      download.resolve(null);
      await wait(450);
      check("the dialog closes by itself after the install", !modal(), "still open");
      check("after the install GSdx is stored and shown", (await storedRenderer()) === "pcsx2" && selectorText("renderer") === T("app.gsdx_name"), selectorText("renderer"));

      calls.length = 0;
      await openFromCoregs();
      buttons()[0].click();
      await wait(100);
      download.reject("no internet");
      await wait(150);
      check("a failed download says so in the dialog", !!modal() && modal().querySelector(".modal-text").textContent.indexOf("no internet") >= 0, modal() ? modal().textContent : "closed");
      var shown = function (index) { return buttons()[index].style.display !== "none"; };
      check("a failed download offers Try again, Install from a file and Close, with the hint", shown(0) && shown(1) && shown(2) &&
        buttons()[0].textContent === T("settings.gsdx_retry") && buttons()[1].textContent === T("settings.gsdx_from_file") &&
        buttons()[2].textContent === T("settings.gsdx_close") && !modal().querySelector(".gsdx-hint").hidden && modal().querySelector(".gsdx-hint").textContent.indexOf("windows-x64.zip") >= 0,
        modal() ? modal().textContent : "closed");
      buttons()[0].click();
      await wait(100);
      check("Try again runs the download again", calls.join(",") === "download_gsdx,download_gsdx" && !modal().querySelector(".gsdx-progress").hidden, calls.join(","));
      download.reject("no internet");
      await wait(150);
      buttons()[1].click();
      await wait(100);
      download.resolve(null);
      await wait(150);
      check("a cancelled file picker goes back to the failure view", !!modal() && modal().querySelector(".modal-text").textContent.indexOf("no internet") >= 0 &&
        shown(0) && shown(1) && calls.join(",") === "download_gsdx,download_gsdx,install_gsdx_archive", calls.join(","));
      buttons()[2].click();
      await wait(450);
      check("after a failed download CoreGS 2.0 stays", !modal() && selectorText("renderer") === T("choice.renderer.coregs20") && (await storedRenderer()) === "coregs20", selectorText("renderer"));

      needed = false;
      await openFromCoregs();
      check("with GSdx installed there is no dialog, and the choice is stored", !modal() && (await storedRenderer()) === "pcsx2", "dialog or not stored");
    } finally {
      GT4.backend.call = real;
      GT4.ui.topDialog() && GT4.ui.topDialog().close();
      await real("set_setting", { id: "renderer", value: before });
      await setHash("#/settings/general");
      await setHash("#/settings/graphics");
    }
  }

  // Issues #139 and #179: Play with GSdx stored but not installed, on a card with Vulkan 1.2, offers CoreGS 2.0 before anything
  // starts. "Use CoreGS 2.0" stores it and goes on; "Download GSdx" runs the Settings download dialog's download inside the same
  // dialog; Cancel and Back start nothing. The commands are stubbed; `calls` is what Play asked of Rust, in order.
  async function playOfferChecks() {
    var real = GT4.backend.call;
    var calls = [];
    var offer = true;
    var useWorks = true;
    var download = null;
    GT4.backend.call = function (command, args) {
      if (command === "coregs20_offer") {
        calls.push(command);
        return offer === "fail" ? Promise.reject(new Error("the probe failed")) : Promise.resolve(offer);
      }
      if (command === "use_coregs20") {
        calls.push(command);
        return useWorks ? Promise.resolve(null) : Promise.reject(new Error("could not select CoreGS 2.0"));
      }
      if (command === "download_gsdx") {
        calls.push(command);
        return new Promise(function (resolve, reject) { download = { resolve: resolve, reject: reject }; });
      }
      if (command === "prepare_shaders" || command === "play") {
        calls.push(command);
        return Promise.resolve(command === "prepare_shaders" ? true : null);
      }
      return real(command, args);
    };
    var modal = function () { return document.querySelector(".modal"); };
    var buttons = function () {  // the buttons shown (TASK-158 keeps Install from a file hidden until a failure)
      return Array.prototype.filter.call(modal().querySelectorAll(".modal-actions button"), function (button) { return button.style.display !== "none"; });
    };
    var started = function () { return document.querySelector(".stage.home").classList.contains("is-running"); };
    var freshHome = async function () {
      await setHash("#/settings/general");
      await setHash("#/home");
      await wait(300);
      calls.length = 0;
    };
    var clickPlay = async function () {
      document.querySelector(".play").click();
      await wait(150);
    };
    var offerLabels = [T("home.gsdx_missing_use"), T("home.gsdx_missing_download"), T("common.cancel")].join("|");
    try {
      check("the dev mock offers nothing without ?gsdx=missing", (await real("coregs20_offer")) === false, "offered");

      offer = false;
      await freshHome();
      await clickPlay();
      check("with nothing to offer, Play prepares the shaders and starts at once", !modal() && calls.join(",") === "coregs20_offer,prepare_shaders,play", calls.join(","));

      offer = "fail";
      await freshHome();
      await clickPlay();
      check("when the offer check itself fails, Play goes on", !modal() && calls.join(",") === "coregs20_offer,prepare_shaders,play", calls.join(","));

      offer = true;
      await freshHome();
      await clickPlay();
      var labels = modal() ? Array.prototype.map.call(buttons(), function (button) { return button.textContent; }).join("|") : "no dialog";
      check("a missing GSdx opens the offer before anything starts", !!modal() && modal().querySelector("h3").textContent === T("home.gsdx_missing_title") &&
        calls.join(",") === "coregs20_offer" && !started(), calls.join(","));
      check("it offers CoreGS 2.0 (the default), the GSdx download and Cancel", labels === offerLabels && document.activeElement === buttons()[0], labels);
      check("its text names CoreGS 2.0 and GSdx", modal().querySelector(".modal-text").textContent === T("home.gsdx_missing_text"), "");
      buttons()[2].click();
      await wait(450);
      check("Cancel closes the offer and starts nothing", !modal() && calls.join(",") === "coregs20_offer" && !started(), calls.join(","));
      await clickPlay();
      check("after Cancel, Play offers again", !!modal() && calls.join(",") === "coregs20_offer,coregs20_offer", calls.join(","));
      GT4.ui.topDialog().back();
      await wait(450);
      check("Back also cancels the offer", !modal() && !started() && calls.indexOf("play") < 0, calls.join(","));

      await freshHome();
      await clickPlay();
      buttons()[0].click();
      await wait(300);
      check("Use CoreGS 2.0 stores it, then prepares the shaders and starts", !modal() && calls.join(",") === "coregs20_offer,use_coregs20,prepare_shaders,play", calls.join(","));

      useWorks = false;
      await freshHome();
      await clickPlay();
      buttons()[0].click();
      await wait(300);
      var toast = document.getElementById("toast");
      check("a refused switch starts nothing and says why", !modal() && !started() && calls.join(",") === "coregs20_offer,use_coregs20" &&
        toast.classList.contains("is-shown") && toast.textContent.indexOf("could not select CoreGS 2.0") >= 0, calls.join(",") + " / " + toast.textContent);
      useWorks = true;

      await freshHome();
      await clickPlay();
      buttons()[1].click();
      await wait(100);
      check("Download GSdx shows the download inside the same dialog", !!modal() && !modal().querySelector(".gsdx-progress").hidden &&
        modal().querySelector("h3").textContent === T("app.gsdx_name") && modal().querySelector(".gsdx-status").textContent !== "", "no progress");
      GT4.ui.topDialog().back();
      check("the download cannot be dismissed", !!modal(), "closed");
      download.resolve(null);
      await wait(450);
      check("after the download the dialog closes and Play starts with GSdx, no switch", !modal() && calls.join(",") === "coregs20_offer,download_gsdx,prepare_shaders,play", calls.join(","));

      await freshHome();
      await clickPlay();
      buttons()[1].click();
      await wait(100);
      download.reject("no internet");
      await wait(150);
      var visible = function (index) { return buttons()[index].style.display !== "none"; };
      check("a failed download says so and keeps CoreGS 2.0 on offer", !!modal() && modal().querySelector(".modal-text").textContent.indexOf("no internet") >= 0 &&
        visible(0) && visible(1) && visible(2) && visible(3) && buttons()[1].textContent === T("settings.gsdx_retry") &&
        buttons()[3].textContent === T("settings.gsdx_close") && document.activeElement === buttons()[0],
        modal() ? modal().textContent : "closed");
      buttons()[0].click();
      await wait(300);
      check("CoreGS 2.0 from there stores it and starts", !modal() && calls.join(",") === "coregs20_offer,download_gsdx,use_coregs20,prepare_shaders,play", calls.join(","));

      await freshHome();
      await clickPlay();
      buttons()[1].click();
      await wait(100);
      download.reject("no internet");
      await wait(150);
      buttons()[3].click();
      await wait(450);
      check("Close after a failed download starts nothing", !modal() && !started() && calls.indexOf("play") < 0, calls.join(","));
    } finally {
      GT4.backend.call = real;
      GT4.ui.topDialog() && GT4.ui.topDialog().close();
      await setHash("#/settings/general");
      await setHash("#/settings/graphics");
    }
  }

  // TASK-164 (issue #160): on a start with no save yet, Play shows which keys answer the game's first-start questions, after the
  // shaders and before the start. Play in the note starts; Cancel and Back start nothing; a failed check starts at once; a
  // controls set without a controller button names only the keys. The commands are stubbed; `calls` is what Play asked of Rust.
  async function firstStartHintChecks() {
    var real = GT4.backend.call;
    var calls = [];
    var hint = null;
    GT4.backend.call = function (command, args) {
      if (command === "first_start_hint") {
        calls.push(command);
        return hint === "fail" ? Promise.reject(new Error("the library could not be read")) : Promise.resolve(hint);
      }
      if (command === "coregs20_offer") {
        calls.push(command);
        return Promise.resolve(false);
      }
      if (command === "prepare_shaders" || command === "play") {
        calls.push(command);
        return Promise.resolve(command === "prepare_shaders" ? true : null);
      }
      return real(command, args);
    };
    var modal = function () { return document.querySelector(".modal"); };
    var started = function () { return document.querySelector(".stage.home").classList.contains("is-running"); };
    var freshHome = async function () {
      await setHash("#/settings/general");
      await setHash("#/home");
      await wait(300);
      calls.length = 0;
    };
    var clickPlay = async function () {
      document.querySelector(".play").click();
      await wait(150);
    };
    var full = { game: "Gran Turismo 4", confirm: "W / Enter", pad: "A", right: "Right" };
    var all = "coregs20_offer,prepare_shaders,first_start_hint,play";
    try {
      hint = null;
      await freshHome();
      await clickPlay();
      check("with a save, Play starts without a note", !modal() && calls.join(",") === all, calls.join(","));

      hint = full;
      await freshHome();
      await clickPlay();
      var box = modal();
      check("without a save, the note opens after the shaders and before the start",
        !!box && calls.join(",") === "coregs20_offer,prepare_shaders,first_start_hint", calls.join(","));
      if (box) {
        var text = box.textContent;
        check("the note has the title", box.querySelector("h3").textContent === T("home.first_start_title"), box.querySelector("h3").textContent);
        check("the note names the game, the Cross keys, the controller button and the right key",
          text.indexOf(T("home.first_start_text", { game: full.game })) >= 0 &&
          text.indexOf(T("home.first_start_confirm_pad", { keys: full.confirm, pad: full.pad })) >= 0 &&
          text.indexOf(T("home.first_start_cancel_first", { right: full.right })) >= 0, text);
        check("Play is the default button of the note", document.activeElement && document.activeElement.textContent === T("common.play"),
          document.activeElement && document.activeElement.textContent);
        document.activeElement.click();
        await wait(150);
        check("Play in the note starts the game", !modal() && calls.join(",") === all, calls.join(","));
      }

      await freshHome();
      await clickPlay();
      if (modal()) {
        Array.prototype.filter.call(modal().querySelectorAll(".modal-actions button"), function (button) {
          return button.textContent === T("common.cancel");
        })[0].click();
        await wait(150);
      }
      check("Cancel in the note starts nothing", !modal() && !started() && calls.indexOf("play") < 0, calls.join(","));

      await freshHome();
      await clickPlay();
      if (modal()) {
        GT4.ui.topDialog().back();
        await wait(150);
      }
      check("Back closes the note and starts nothing", !modal() && !started() && calls.indexOf("play") < 0, calls.join(","));

      hint = "fail";
      await freshHome();
      await clickPlay();
      check("when the check itself fails, Play goes on", !modal() && calls.join(",") === all, calls.join(","));

      hint = { game: "Gran Turismo 4", confirm: "W / Enter", pad: "", right: "Right" };
      await freshHome();
      await clickPlay();
      var keysOnly = modal() ? modal().textContent : "";
      check("without a controller button the note names only the keys",
        keysOnly.indexOf(T("home.first_start_confirm", { keys: "W / Enter" })) >= 0, keysOnly);
    } finally {
      GT4.backend.call = real;
      GT4.ui.topDialog() && GT4.ui.topDialog().close();
      await setHash("#/settings/general");
      await setHash("#/settings/graphics");
    }
  }

  // The live switch (Settings > General > Language): the whole UI changes at once, in both directions.
  async function languageSwitchChecks() {
    var before = GT4.i18n.current();
    var beforeSetting = GT4.i18n.setting();
    var other = before === "pt-BR" ? "en-US" : "pt-BR";
    await routeTo("#/settings/general");
    await wait(400);
    var select = document.querySelector("[data-setting=\"language\"]");
    check("Settings > General shows the Language row", !!select, "no row");
    if (!select) {
      return;
    }
    var reached = await stepSelectorUntil("language", function () { return GT4.i18n.setting() === other; });
    check("the Language selector offers " + other, reached, GT4.i18n.setting());
    await wait(900);
    var languageFocus = document.activeElement && document.activeElement.getAttribute("data-setting");
    check("the focus stays on the Language selector after the switch", languageFocus === "language", "focus " + languageFocus);
    check("the page is in " + other + " after the choice (no restart)", GT4.i18n.current() === other &&
      document.documentElement.getAttribute("lang") === other, GT4.i18n.current());
    var sideGeneral = document.querySelector("nav.side [data-tab=\"general\"]");
    check("the tab list names the General tab in " + other, !!sideGeneral && sideGeneral.textContent === GT4.t("tab.general"),
      sideGeneral ? sideGeneral.textContent : "no tab");
    // Back to the language the run started in.
    await GT4.backend.call("set_setting", { id: "language", value: beforeSetting });
    GT4.i18n.setSetting(beforeSetting);
    await wait(400);
  }

  async function routeTo(hash) {
    root.location.hash = hash;
    await wait(500);
  }

  // Portuguese checks (run in pt-BR): the fixed terms, the notes card and the English credit line.
  async function portugueseChecks() {
    await routeTo("#/home");
    var play = document.querySelector(".play");
    check("pt-BR: the home button reads Jogar", !!play && play.textContent === "Jogar", play ? play.textContent : "no play");
    var support = document.querySelector(".support");
    check("pt-BR: the support button reads Ajude o projeto", !!support && support.textContent.indexOf("Ajude o projeto") >= 0,
      support ? support.textContent : "no support");
    var credit = document.querySelector(".home-version");
    check("pt-BR: the credit line stays in English", !!credit && credit.textContent.indexOf("Made with passion, by Moesuito") >= 0,
      credit ? credit.textContent : "no credit");
    var notesTitle = document.querySelector(".home-notes-title");
    check("pt-BR: the What's new card reads Novidades", !!notesTitle && notesTitle.textContent === "Novidades",
      notesTitle ? notesTitle.textContent : "no card");
    var notesBody = document.querySelector(".home-notes-body");
    check("pt-BR: the release notes come in Portuguese", !!notesBody && notesBody.textContent.indexOf("Novidades") >= 0,
      notesBody ? notesBody.textContent.slice(0, 60) : "no notes");
    await routeTo("#/settings/general");
    await wait(400);
    var row = document.querySelector("[data-row=\"language\"] .k span");
    check("pt-BR: the General tab row reads Idioma", !!row && row.textContent === "Idioma", row ? row.textContent : "no row");
    var tableNow = await GT4.backend.call("get_settings_table");
    var languageRow = (tableNow.rows || []).filter(function (item) { return item.id === "language"; })[0];
    var ctl = document.querySelector("[data-setting=\"language\"]");
    check("pt-BR: the language choice shows Português (Brasil)", !!ctl && ctl.textContent.indexOf("Português (Brasil)") >= 0,
      (ctl ? ctl.textContent : "no choice") + " table " + (languageRow && languageRow.current) + " setting " + GT4.i18n.setting());
  }

  // ---------- Update channel and version picker (TASK-109), in the page's language ----------
  function versionOptionTags() {
    return Array.prototype.map.call(document.querySelectorAll(".modal .option-list .option"), function (button) {
      return button.getAttribute("data-tag");
    });
  }

  function topModalTitle() {
    var modals = document.querySelectorAll(".modal");
    var top = modals[modals.length - 1];
    var title = top && top.querySelector("h3");
    return title ? title.textContent : "";
  }

  function closeModals() {
    var dialog = GT4.ui.topDialog();
    while (dialog) {
      dialog.close();
      dialog = GT4.ui.topDialog();
    }
  }

  async function openVersionList() {
    var select = document.querySelector("[data-setting=\"launcher_version\"]");
    select.focus({ preventScroll: true });
    select.click();
    await wait(150);
    return versionOptionTags();
  }

  async function chooseVersion(tag) {
    var button = document.querySelector(".modal .option[data-tag=\"" + tag + "\"]");
    if (button) {
      button.click();
    }
    await wait(150);
    return !!button;
  }

  async function chooseChannel(value) {
    var wanted = T("choice.update_channel." + value);
    var reached = await stepSelectorUntil("update_channel", function () { return selectorText("update_channel") === wanted; });
    await wait(600);
    return reached;
  }

  async function versionChecks() {
    var dev = GT4.devVersions;
    dev.installed = "1.3.0-rc6";
    dev.installs.length = 0;
    dev.failList = false;
    await GT4.backend.call("set_setting", { id: "update_channel", value: "stable" });
    await routeTo("#/settings/general");
    await wait(500);

    // The rows: Language, Channel, Version, in that order.
    var order = Array.prototype.map.call(document.querySelectorAll(".content .row"), function (row) {
      return row.getAttribute("data-row");
    });
    check("General lists Language, Channel, Discord Rich Presence, Version and GPU acceleration in that order", order.join(",") === "language,update_channel,discord_rich_presence,launcher_version,launcher_gpu", order.join(","));
    var channelSelect = document.querySelector("[data-setting=\"update_channel\"]");
    check("the Channel row shows Stable", !!channelSelect && channelSelect.textContent.indexOf(T("choice.update_channel.stable")) === 0,
      channelSelect ? channelSelect.textContent : "no row");
    var versionSelect = document.querySelector("[data-setting=\"launcher_version\"]");
    check("the Version row shows the installed version", !!versionSelect && versionSelect.textContent.indexOf("1.3.0-rc6") === 0,
      versionSelect ? versionSelect.textContent : "no row");
    var note = document.querySelector("[data-action=\"versions-note\"]");
    check("the Version row says older versions are not listed", !!note && note.textContent.indexOf(T("versions.older_note")) >= 0,
      note ? note.textContent : "no note");
    check("the Version row has a refresh button", !!document.querySelector("[data-action=\"versions-refresh\"]"), "none");

    var tags = await openVersionList();
    // The running test build (1.3.0-rc6) is not a Stable release, so it comes first, marked Installed (rc5 owner report).
    check("Stable lists the running build, then the stable releases by version, without a draft or a version below rc5",
      tags.join(",") === "v1.3.0-rc6,v1.3.2,v1.3.0", tags.join(","));
    closeModals();

    // Choosing Beta applies at once and refreshes the list.
    check("the Beta choice is in the Channel list", await chooseChannel("beta"), "no Beta option");
    var saved = (await GT4.backend.call("get_settings_table")).rows.filter(function (row) { return row.id === "update_channel"; })[0];
    check("the channel is saved at once", !!saved && saved.current === "beta", saved ? saved.current : "no row");
    tags = await openVersionList();
    check("Beta lists a newer stable on top, then candidates by date, no draft", tags.join(",") === "v1.3.2,v1.3.1-rc2,v1.3.0-rc6,v1.3.0-rc5", tags.join(","));
    var installedOption = document.querySelector(".modal .option[data-tag=\"v1.3.0-rc6\"]");
    check("the installed version is marked", !!installedOption && installedOption.textContent.indexOf(T("versions.installed")) >= 0 &&
      installedOption.getAttribute("aria-selected") === "true", installedOption ? installedOption.textContent : "none");

    // An older version: the strong warning, Cancel first, nothing changes on No.
    await chooseVersion("v1.3.0-rc5");
    var warning = document.querySelector(".modal .version-warning");
    check("an older version opens the downgrade warning", topModalTitle() === T("versions.downgrade_title") && !!warning &&
      warning.textContent.indexOf(T("versions.downgrade_warning")) >= 0, topModalTitle());
    var active = document.activeElement;
    check("the downgrade dialog does not focus the install button", !!active && active.textContent === T("versions.cancel"),
      active ? active.textContent : "none");
    var olderButton = Array.prototype.filter.call(document.querySelectorAll(".modal-actions .btn"), function (button) {
      return button.textContent === T("versions.install_older");
    })[0];
    check("the downgrade dialog has an explicit Install older version button and a saves folder button", !!olderButton &&
      Array.prototype.some.call(document.querySelectorAll(".modal-actions .btn"), function (button) {
        return button.textContent === T("versions.open_saves_folder");
      }), "missing");
    versionSelect = document.querySelector("[data-setting=\"launcher_version\"]");
    check("the dropdown shows the picked version while the question is open", versionSelect.textContent.indexOf("1.3.0-rc5") === 0, versionSelect.textContent);
    active.click();
    await wait(150);
    versionSelect = document.querySelector("[data-setting=\"launcher_version\"]");
    check("No puts the dropdown back on the installed version and installs nothing", versionSelect.textContent.indexOf("1.3.0-rc6") === 0 &&
      dev.installs.length === 0, versionSelect.textContent + " installs " + dev.installs.join(","));

    // A newer version: the plain question, Install is the default; No restores.
    await openVersionList();
    await chooseVersion("v1.3.2");
    check("a newer version opens the plain question", topModalTitle() === T("versions.confirm_title", { version: "1.3.2" }) &&
      !document.querySelector(".modal .version-warning"), topModalTitle());
    active = document.activeElement;
    check("the plain question focuses Install", !!active && active.textContent === T("versions.install"), active ? active.textContent : "none");
    var cancel = Array.prototype.filter.call(document.querySelectorAll(".modal-actions .btn"), function (button) {
      return button.textContent === T("versions.cancel");
    })[0];
    cancel.click();
    await wait(150);
    versionSelect = document.querySelector("[data-setting=\"launcher_version\"]");
    check("Cancel on the plain question restores the dropdown", versionSelect.textContent.indexOf("1.3.0-rc6") === 0 && dev.installs.length === 0,
      versionSelect.textContent);

    // Yes calls the update path with the exact tag.
    await openVersionList();
    await chooseVersion("v1.3.2");
    document.activeElement.click();
    await wait(200);
    check("Yes calls the install path for that exact version", dev.installs.join(",") === "v1.3.2", dev.installs.join(","));
    await openVersionList();
    await chooseVersion("v1.3.0-rc5");
    olderButton = Array.prototype.filter.call(document.querySelectorAll(".modal-actions .btn"), function (button) {
      return button.textContent === T("versions.install_older");
    })[0];
    olderButton.click();
    await wait(200);
    check("Install older version calls the same path", dev.installs.join(",") === "v1.3.2,v1.3.0-rc5", dev.installs.join(","));
    closeModals();

    // Keyboard: Enter on the focused dropdown opens the list.
    versionSelect = document.querySelector("[data-setting=\"launcher_version\"]");
    versionSelect.focus({ preventScroll: true });
    versionSelect.click();
    await wait(150);
    check("the version list opens from the keyboard focus and holds the focus", !!document.querySelector(".modal .option-list") &&
      document.activeElement.classList.contains("option"), "none");
    closeModals();

    // Offline or a GitHub error: a plain message, the current version stays.
    dev.failList = true;
    document.querySelector("[data-action=\"versions-refresh\"]").click();
    await wait(400);
    versionSelect = document.querySelector("[data-setting=\"launcher_version\"]");
    note = document.querySelector("[data-action=\"versions-note\"]");
    check("a failed list read shows a plain message and keeps the current version", versionSelect.textContent.indexOf("1.3.0") === 0 &&
      !!note && note.classList.contains("is-warning") && note.textContent.length > 0, (versionSelect.textContent) + " / " + (note ? note.textContent : "none"));
    dev.failList = false;

    // Stable while a newer candidate runs: nothing to list above it, said plainly.
    await GT4.backend.call("set_setting", { id: "update_channel", value: "stable" });
    dev.installed = "1.3.3-rc1";
    await routeTo("#/home");
    await routeTo("#/settings/general");
    await wait(500);
    note = document.querySelector("[data-action=\"versions-note\"]");
    var expected = T("versions.nothing_newer", { channel: T("choice.update_channel.stable") });
    check("a newer build than any Stable release says updates resume later", !!note && note.textContent.indexOf(expected) >= 0,
      note ? note.textContent : "no note");

    dev.installed = "1.3.0-rc6";
    dev.installs.length = 0;
    await GT4.backend.call("set_setting", { id: "update_channel", value: "stable" });
  }

  // TASK-114 rc6 (pt-BR owner screenshot): no option value on General or Graphics wraps. A value is one line when it
  // is nowrap and its box is no taller than its line height (app.css fixes the line height). The Language
  // value is also measured with the longest text it can show, "Automático (Português (Brasil))", in place of its own.
  function selectValueInfo(select) {
    var value = select.querySelector(".selector-value") || select.firstElementChild;
    var style = getComputedStyle(value);
    return {
      text: value.textContent,
      height: value.getBoundingClientRect().height,
      line: parseFloat(style.lineHeight),
      nowrap: style.whiteSpace === "nowrap"
    };
  }

  function oneLineProblems(selects) {
    var problems = [];
    selects.forEach(function (select) {
      var info = selectValueInfo(select);
      if (!info.nowrap || !(info.line > 0) || info.height > info.line + 0.5) {
        problems.push(info.text + " (" + info.height + "px, line " + info.line + "px)");
      }
    });
    return problems;
  }

  async function selectValueChecks() {
    var tabs = ["general", "graphics"];
    for (var i = 0; i < tabs.length; i++) {
      await setHash("#/settings/" + tabs[i]);
      await wait(300);
      var selects = Array.prototype.slice.call(document.querySelectorAll(".row .select, .row .selector"));
      var problems = oneLineProblems(selects);
      check(tabs[i] + " tab: every option value is one line (" + selects.length + " controls)",
        selects.length > 0 && problems.length === 0, problems.join("; ") || "no controls");
      if (tabs[i] === "general") {
        var language = document.querySelector(".row .selector[data-setting=\"language\"]");
        if (language) {
          var span = language.querySelector(".selector-value");
          var saved = span.textContent;
          span.textContent = T("choice.language.auto", { language: T("choice.language.pt-BR") });
          await wait(50);
          var longProblems = oneLineProblems([language]);
          span.textContent = saved;
          check("the Language value stays on one line with its longest text", longProblems.length === 0,
            longProblems.join("; "));
        }
      }
    }
  }

  async function languageChecks() {
    await localeChecks();
    await languageSwitchChecks();
    if (GT4.i18n.current() === "pt-BR") {
      await portugueseChecks();
    }
  }

  // The setup page against a stubbed backend: the first-run page names no game, and the copy step ends at a full bar.
  async function setupChecks() {
    var realCall = GT4.backend.call;
    var current = null;
    GT4.backend.call = function (command) {
      return command === "app_state" ? Promise.resolve({ setup: current, any_installed: false, games: [] }) : realCall.apply(null, arguments);
    };
    var scratch = document.createElement("div");
    document.body.appendChild(scratch);
    var handle = null;
    try {
      current = { state: "needed", step: "copying", progress: 0, bytes_done: 0, bytes_total: 0, renderer_step: "", disc_name: "", problem: "", note: "", game: null };
      handle = GT4.pages.setup.mount(scratch);
      await wait(700);
      var title = scratch.querySelector("h1").textContent;
      var sub = scratch.querySelector(".sub").textContent;
      check("the first-run setup page is generic: no game is named before the disc is read",
        title === T("setup.set_up") && sub === T("setup.choose_text") && title.indexOf("Spec") < 0, title + " / " + sub);
      current = { state: "checking", step: "checking", progress: 0, bytes_done: 3683418112, bytes_total: 3683418112, renderer_step: "",
        disc_name: "Gran Turismo 4 Spec II v1.10", problem: "", note: "", game: { id: "spec2", name: "Gran Turismo 4 Spec II" } };
      await wait(900);
      var named = scratch.querySelector("h1").textContent;
      check("after the disc is identified the steps name the game", named === T("setup.checking_title_named", { game: "Gran Turismo 4 Spec II" }), named);
      var discStep = scratch.querySelector(".steps li .d");
      check("step 1 says what the disc was recognised as", !!discStep && discStep.textContent === T("setup.recognised", { name: "Gran Turismo 4 Spec II v1.10" }),
        discStep ? discStep.textContent : "no detail");
      var stepLabels = Array.prototype.map.call(scratch.querySelectorAll(".steps li .t"), function (node) { return node.textContent; });
      check("the copy step says it also checks the image, and the step after it is the final check",
        stepLabels[1] === T("setup.step_copying") && stepLabels[2] === T("setup.step_checking"), stepLabels.join(" | "));
      var copyBar = scratch.querySelector(".steps li .bar i");
      check("the copy step ends at a full bar after the page moved on to checking",
        !!copyBar && copyBar.style.width === "100%" && copyBar.style.transition === "none", copyBar ? copyBar.style.width + " " + copyBar.style.transition : "no bar");
      // Owner, 2026-11-02: setup checks the graphics card and prepares CoreGS 2.0; GSdx only when the card needs it.
      var lastStep = function () {
        var rows = scratch.querySelectorAll(".steps li");
        return rows[rows.length - 1].textContent;
      };
      current = { state: "checking", step: "renderer", progress: 0.5, bytes_done: 100, bytes_total: 200, renderer_step: "compiling", renderer_choice: "",
        disc_name: "Gran Turismo 4 Spec II v1.10", problem: "", note: "", game: { id: "spec2", name: "Gran Turismo 4 Spec II" } };
      await wait(900);
      check("the last step counts the shaders while it prepares CoreGS 2.0",
        lastStep().indexOf(T("setup.renderer_compiling")) >= 0 && lastStep().indexOf(T("setup.renderer_count", { done: 100, total: 200 })) >= 0, lastStep());
      current = Object.assign({}, current, { state: "done", renderer_step: "done", renderer_choice: "coregs20" });
      await wait(900);
      check("the last step says CoreGS 2.0 is ready, with nothing about GSdx", lastStep().indexOf(T("setup.renderer_ready_coregs20")) >= 0 && lastStep().indexOf("GSdx") < 0, lastStep());
      current = Object.assign({}, current, { renderer_choice: "gsdx_fallback" });
      await wait(900);
      check("the last step says why GSdx was needed", lastStep().indexOf(T("setup.renderer_ready_gsdx")) >= 0, lastStep());
    } finally {
      if (handle) {
        handle.destroy();
      }
      scratch.remove();
      GT4.backend.call = realCall;
    }
  }

  // The 1.3.0 intro is looked up by the base version: 1.3.0-rc5 plays it, once, and 1.3.0 then does not.
  async function introChecks() {
    var key = "gt4recomp.intro.seen";
    var before = null;
    try { before = root.localStorage.getItem(key); root.localStorage.removeItem(key); } catch (error) { return; }
    try {
      check("the intro base version drops the rc suffix", GT4.intro.baseVersion("1.3.0-rc5") === "1.3.0" && GT4.intro.baseVersion("1.3.0") === "1.3.0", "");
      if (document.hidden) {
        check("a hidden page does not start the intro", GT4.intro.maybePlay("1.3.0-rc5") === false && !GT4.intro.isPlaying(), "");
        return;
      }
      var first = GT4.intro.maybePlay("1.3.0-rc5");
      var playing = GT4.intro.isPlaying();
      GT4.intro.skip();
      await wait(800);
      var second = GT4.intro.maybePlay("1.3.0-rc6");
      var third = GT4.intro.maybePlay("1.3.0");
      check("1.3.0-rc5 plays the 1.3.0 intro once", first === true && playing && second === false && third === false && !GT4.intro.isPlaying(),
        first + " " + playing + " " + second + " " + third);
      check("a version without an intro plays nothing", GT4.intro.maybePlay("1.2.9") === false, "");
    } finally {
      try { if (before === null) { root.localStorage.removeItem(key); } else { root.localStorage.setItem(key, before); } } catch (error) { /* none */ }
    }
  }

  // TASK-181: the Wheel page (js/pages/wheel.js): entry on the steering chip, D-pad moves between the chips with the
  // panel following, the real-angle wheel and the pedals from the live event, bind and cancel, clear and bind again, a
  // pedal's dead zone, the 900-degree warning, the force test and "received", the set-up wizard from start to end,
  // presets, and no raw locale key on the page.
  function wheelFocused() {
    var active = document.activeElement;
    var item = active && active.closest ? active.closest(".wh-item") : null;
    return item ? item.getAttribute("data-control") : null;
  }

  function wheelChipText(id) {
    var item = document.querySelector(".wh-item[data-control=\"" + id + "\"] .chip-text");
    return item ? item.textContent : null;
  }

  function wheelCardName() {
    var name = document.querySelector(".wh-control .cx-name");
    return name ? name.textContent : null;
  }

  async function wheelConfig() {
    return (await GT4.backend.call("get_wheel_config")).config;
  }

  async function wheelFocus(id) {
    var item = document.querySelector(".wh-item[data-control=\"" + id + "\"]");
    if (item) {
      item.focus();
    }
    await wait(120);
  }

  async function wheelChecks() {
    // A still G27 for the live checks: +90 degrees (a fifth of the travel), the accelerator at 60%, button 1 down.
    GT4.devWheelLive = function () {
      return { devices: [{ id: 11, guid: "030000006d0400009bc2000000000000", axes: [6554, 32767, Math.round(32767 - 0.6 * 65535), 32767], pressed: [0], hats: [0] }] };
    };
    await setHash("#/settings/wheel");
    await wait(500);
    check("the Wheel preview with live input running keeps focus on the tab list", sideStepOk("wheel"), sideDetail());
    press("ArrowRight");
    await wait(250);
    check("entering Wheel focuses the steering chip", wheelFocused() === "steering", "focus " + wheelFocused());
    check("the Wheel footer offers Bind, Clear and Set up", footerText().indexOf(T("wheel.hint.bind")) >= 0 && footerText().indexOf(T("wheel.hint.clear")) >= 0 &&
      footerText().indexOf(T("wheel.hint.setup")) >= 0, footerText());
    check("the panel shows the steering with its rotation", wheelCardName() === T("wheel.steering") && !!document.querySelector("input[data-setting=\"wheel-rotation\"]"), wheelCardName());

    // The wheel turns at its real angle and the pedals show what the game gets.
    var turning = document.querySelector(".wheel-svg .wh-turning");
    check("the drawn wheel turns to +90 degrees", !!turning && /^rotate\(90\.0\d 500 225\)$/.test(turning.getAttribute("transform") || ""), turning ? turning.getAttribute("transform") : "none");
    check("the angle reads +90°", (document.querySelector(".wheel-svg .wh-angle") || {}).textContent === "+90°", (document.querySelector(".wheel-svg .wh-angle") || {}).textContent);
    var throttleValue = document.querySelector(".wh-item[data-control=\"throttle\"] .wh-pedal-value");
    check("the accelerator shows the game's 60%", !!throttleValue && throttleValue.textContent === "60%", throttleValue ? throttleValue.textContent : "none");
    var crossChip = document.querySelector(".wh-item[data-control=\"cross\"]");
    check("button 1 lights Cross", !!crossChip && crossChip.classList.contains("is-live"), "no is-live");
    check("live input never moves focus", wheelFocused() === "steering", "focus " + wheelFocused());

    // D-pad between the chips; the panel follows.
    await wheelFocus("cross");
    press("ArrowUp");
    await wait(150);
    check("D-pad up from Cross reaches Circle", wheelFocused() === "circle", "focus " + wheelFocused());
    check("the panel follows the selection (Circle)", wheelCardName() === T("wheel.control.circle"), wheelCardName());
    press("ArrowDown");
    await wait(150);
    press("ArrowDown");
    await wait(150);
    check("D-pad down from Cross reaches R3", wheelFocused() === "r3", "focus " + wheelFocused());

    // Bind, then cancel with Esc (B on the pad goes through the same GT4.cancelCapture).
    await wheelFocus("cross");
    GT4.nav.activate();
    await wait(200);
    check("A on a chip opens the bind overlay", !!document.querySelector(".modal.is-capture") && GT4.capturing === true, "capturing " + GT4.capturing);
    check("the overlay names the control", ((document.querySelector(".modal.is-capture h3") || {}).textContent || "").indexOf(T("wheel.control.cross")) >= 0, "title");
    check("the bind footer shows only B Cancel", footerText() === "B" + T("common.cancel"), footerText());
    press("Escape");
    await wait(200);
    check("Esc cancels the bind and closes the overlay", !document.querySelector(".modal.is-capture") && GT4.capturing === false, "capturing " + GT4.capturing);
    check("after a cancel, focus is back on the chip", wheelFocused() === "cross", "focus " + wheelFocused());

    // Y clears Cross; A binds it again (the mock answers button 1).
    GT4.hooks.y();
    await wait(300);
    check("Y clears Cross", (await wheelConfig()).buttons.cross.length === 0 && wheelChipText("cross") === T("wheel.unbound"), wheelChipText("cross"));
    GT4.nav.activate();
    await wait(1200);
    check("a capture binds Cross to button 1", JSON.stringify((await wheelConfig()).buttons.cross) === "[\"btn:0\"]" &&
      wheelChipText("cross") === T("wheel.source.button", { n: 1 }), wheelChipText("cross"));

    // A pedal: its calibration rows; the dead zone moves by one step.
    await wheelFocus("throttle");
    check("the accelerator's card has its direction, dead zone, saturation and curve", wheelCardName() === T("wheel.pedal.throttle") &&
      !!document.querySelector("[data-setting=\"wheel-throttle-mode\"]") && !!document.querySelector("input[data-setting=\"wheel-throttle-deadzone\"]") &&
      !!document.querySelector("input[data-setting=\"wheel-throttle-saturation\"]") && !!document.querySelector("[data-setting=\"wheel-throttle-curve\"]"), wheelCardName());
    var deadzone = document.querySelector("input[data-setting=\"wheel-throttle-deadzone\"]");
    if (deadzone) {
      deadzone.focus();
      press("ArrowRight");
      await wait(400);
    }
    check("the dead zone moves by one step (2% to 3%)", (await wheelConfig()).throttle.deadzone === 0.03, String((await wheelConfig()).throttle.deadzone));
    var focusedSetting = document.activeElement && document.activeElement.getAttribute ? document.activeElement.getAttribute("data-setting") : null;
    check("the dead zone keeps the focus after the change", focusedSetting === "wheel-throttle-deadzone", "focus " + focusedSetting);

    // The rotation: 900 has no warning; another value says so.
    await wheelFocus("steering");
    check("900 degrees shows no warning", (document.querySelector(".row[data-row=\"wheel-rotation\"]") || { textContent: "" }).textContent.indexOf(T("wheel.rotation_warning")) < 0, "warning");
    var rotation = document.querySelector("input[data-setting=\"wheel-rotation\"]");
    if (rotation) {
      rotation.focus();
      press("ArrowRight");
      await wait(400);
    }
    check("910 degrees warns that the car steers differently", (await wheelConfig()).steering.rotation_degrees === 910 &&
      (document.querySelector(".row[data-row=\"wheel-rotation\"]") || { textContent: "" }).textContent.indexOf(T("wheel.rotation_warning")) >= 0, "rotation");
    await GT4.backend.call("set_wheel_setting", { key: "steering.rotation_degrees", value: 900 });

    // Force feedback: the test plays and lists the effects; the last game's line says Received.
    var test = document.querySelector(".wh-test");
    check("the force test is enabled while the game is not running", !!test && !test.disabled, "disabled");
    if (test) {
      test.click();
      await wait(1300);
    }
    check("the force test reports the effects it found", ((document.querySelector(".wh-test-result") || {}).textContent || "").indexOf(T("wheel.effect.constant")) >= 0, (document.querySelector(".wh-test-result") || {}).textContent);
    check("the last game's force feedback reads Received", (document.querySelector(".wh-received-v") || {}).textContent === T("wheel.received.yes"), (document.querySelector(".wh-received-v") || {}).textContent);

    // The wizard, from the intro to the summary: every step answered by the mock, the wheel turned on at the end.
    await GT4.backend.call("set_wheel_setting", { key: "wheel.enabled", value: false });
    await wheelFocus("steering");
    GT4.hooks.x();
    await wait(200);
    var start = Array.prototype.filter.call(document.querySelectorAll(".modal button"), function (button) { return button.textContent === T("wheel.wizard.start"); })[0];
    check("X opens the set-up wizard's intro", !!start, "no Start");
    if (start) {
      start.click();
      await wait(300);
    }
    check("the wizard starts at step 1 of 21 with the steering", ((document.querySelector(".wh-step") || {}).textContent || "") === T("wheel.wizard.step", { n: 1, total: 21 }) &&
      ((document.querySelector(".wh-prompt") || {}).textContent || "") === T("wheel.prompt.steer_left"), (document.querySelector(".wh-step") || {}).textContent);
    check("the wizard footer offers B Skip", footerText().indexOf("B" + T("wheel.wizard.skip")) === 0, footerText());
    for (var tries = 0; tries < 60 && document.querySelector(".modal.is-wizard"); tries++) {
      await wait(500);
    }
    // The summary opens after the wheel is switched on (a command round trip), so it can come a moment after the wizard.
    var finished = document.querySelector(".modal h3");
    for (var waits = 0; waits < 20 && !(finished && finished.textContent === T("wheel.wizard.done_title")); waits++) {
      await wait(150);
      finished = document.querySelector(".modal h3");
    }
    check("the wizard ends with the summary", !!finished && finished.textContent === T("wheel.wizard.done_title"), finished ? finished.textContent : "none");
    var after = await wheelConfig();
    check("the wizard bound the steering, the pedals and the buttons", after.steering.source === "axis:0" && after.throttle.source === "axis:2" &&
      after.brake.source === "axis:3" && after.clutch.source === "axis:1" && JSON.stringify(after.buttons.r2) === "[\"btn:4\"]" &&
      JSON.stringify(after.buttons.dpad_left) === "[\"hat:0:left\"]", JSON.stringify(after.buttons));
    check("turning left read negative: the steering is not reversed", after.steering.invert === false, String(after.steering.invert));
    check("the wizard turned the wheel on", after.enabled === true, String(after.enabled));
    var done = Array.prototype.filter.call(document.querySelectorAll(".modal button"), function (button) { return button.textContent === T("wheel.wizard.done"); })[0];
    if (done) {
      done.click();
      await wait(200);
    }

    // Presets: save one, see it in the list.
    var saveButton = Array.prototype.filter.call(document.querySelectorAll(".wh-presets button"), function (button) { return button.textContent === T("wheel.presets.save"); })[0];
    if (saveButton) {
      saveButton.click();
      await wait(200);
    }
    var nameInput = document.querySelector(".modal .modal-input");
    check("Save as preset asks for a name with one filled in", !!nameInput && nameInput.value.length > 0, nameInput ? nameInput.value : "none");
    var saveIt = Array.prototype.filter.call(document.querySelectorAll(".modal button"), function (button) { return button.textContent === T("wheel.presets.save_button"); })[0];
    if (saveIt) {
      saveIt.click();
      await wait(300);
    }
    var presets = await GT4.backend.call("list_wheel_presets");
    check("the preset is saved", presets.length === 1 && presets[0].name === (nameInput ? nameInput.value.trim() : ""), JSON.stringify(presets));

    var rawKey = /wheel\.(?!toml)[a-z_]+/.exec(document.querySelector(".stage .content").textContent);
    check("no raw locale key shows on the Wheel page", !rawKey, rawKey ? rawKey[0] : "");
    await wheelFocus("steering");
    press("Escape");
    await wait(200);
    check("B returns Wheel to the tab list", sideStepOk("wheel"), sideDetail());
    delete GT4.devWheelLive;
  }

  async function run() {
    await GT4.i18n.ready;
    await wait(300);
    var scenario = new URLSearchParams(root.location.search).get("scenario") || "";
    try {
      if (scenario === "4k") {
        await scenario4k();
        return;
      }
      if (scenario === "advanced") {
        await scenarioAdvanced();
        return;
      }
      if (scenario === "render") {
        await scenarioRender();
        return;
      }
      if (scenario === "general" || scenario === "graphics") {
        // The screenshot states of slice 295 (TASK-114 rc6): the tab at 1280x720, nothing focused.
        await setHash("#/settings/" + scenario);
        await wait(400);
        return;
      }
      await routeChecks();
      await loadedTabChecks();
      await sideWalk();
      await enterChecks();
      await importEntryCheck();
      await libraryActionChecks();
      await valueChecks();
      await rendererRowChecks();
      await presentThreadChecks();
      await gsdxDialogChecks();
      await playOfferChecks();
      await firstStartHintChecks();
      await advancedSectionChecks();
      await controlsChecks();
      await wheelChecks();
      await gamesChecks();
      await achievementsChecks();
      await shaderWindowChecks();
      await setupChecks();
      await introChecks();
      await versionChecks();
      await languageChecks();
      await selectValueChecks();
    } catch (err) {
      check("the checks ran to the end", false, String(err && err.message ? err.message : err));
    }
    var summary = lines.length - failures + " of " + lines.length + " passed";
    var pre = document.createElement("pre");
    pre.id = "focus-check-results";
    pre.textContent = summary + "\n" + lines.join("\n");
    document.body.appendChild(pre);
    document.body.setAttribute("data-focus-check", failures ? "fail" : "pass");
  }

  run();
})(window);
