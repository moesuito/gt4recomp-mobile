/*
 * Unit tests for GT4.spatial.pickNext (ui/js/spatial.js). Runs in any browser
 * through nav-test.html; prints PASS/FAIL per case and a summary.
 */
function startNavTest(root) {
  "use strict";
  var pick = root.GT4.spatial.pickNext;
  var cases = [];

  function rect(left, top, width, height) {
    return { left: left, top: top, width: width || 10, height: height || 10 };
  }

  function expect(name, actual, expected) {
    cases.push({ name: name, pass: actual === expected, detail: "got " + actual + ", expected " + expected });
  }

  // Row of three: right and left move along it.
  var row = [rect(0, 0), rect(20, 0), rect(40, 0)];
  expect("right moves to the next item in a row", pick(row, 0, "right"), 1);
  expect("right from the last item finds nothing", pick(row, 2, "right"), -1);
  expect("left moves back along the row", pick(row, 2, "left"), 1);
  expect("left from the first item finds nothing", pick(row, 0, "left"), -1);

  // Column: up and down.
  var column = [rect(0, 0), rect(0, 20), rect(0, 40)];
  expect("down moves to the item below", pick(column, 0, "down"), 1);
  expect("up from the top finds nothing", pick(column, 0, "up"), -1);
  expect("up moves to the item above", pick(column, 2, "up"), 1);

  // Items behind the origin are never chosen.
  var behind = [rect(100, 100), rect(0, 100), rect(100, 0)];
  expect("items to the left are ignored for right", pick(behind, 0, "right"), -1);
  expect("items above are ignored for down", pick(behind, 0, "down"), -1);

  // Sideways offset is penalised: a straight-below item beats a nearer diagonal one.
  var penalty = [rect(0, 0), rect(0, 100), rect(30, 60)];
  expect("straight below beats a nearer diagonal item", pick(penalty, 0, "down"), 1);

  // Grid 3x3, index = row * 3 + col.
  var grid = [];
  for (var r = 0; r < 3; r++) {
    for (var c = 0; c < 3; c++) {
      grid.push(rect(c * 50, r * 50));
    }
  }
  expect("grid centre right is the middle-right cell", pick(grid, 4, "right"), 5);
  expect("grid centre left is the middle-left cell", pick(grid, 4, "left"), 3);
  expect("grid centre down is the bottom-centre cell", pick(grid, 4, "down"), 7);
  expect("grid bottom-left up is the middle-left cell", pick(grid, 6, "up"), 3);
  expect("grid top-right down is the middle-right cell", pick(grid, 2, "down"), 5);

  // Different sizes: centres are compared, not edges.
  var sizes = [rect(0, 0, 100, 40), rect(120, 10, 20, 20)];
  expect("different-sized neighbours compare by centre", pick(sizes, 0, "right"), 1);

  // Invalid input.
  expect("bad origin index gives -1", pick(row, 9, "right"), -1);
  expect("unknown direction gives -1", pick(row, 0, "diagonal"), -1);
  expect("empty list gives -1", pick([], 0, "down"), -1);

  // Sideways items on the same line as the origin are not "ahead".
  var sameLine = [rect(0, 0), rect(0, 0)];
  expect("an item exactly on the origin is not ahead", pick(sameLine, 0, "down"), -1);

  // Swipe: a finger moving left asks for the next tab (+1); right for the previous one (-1).
  expect("swipe left (finger) gives the next tab", root.GT4.input.swipeStep(-80, 5), 1);
  expect("swipe right (finger) gives the previous tab", root.GT4.input.swipeStep(90, -10), -1);
  expect("a 59 px swipe is not a swipe", root.GT4.input.swipeStep(-59, 0), 0);
  expect("exactly 60 px counts as a swipe", root.GT4.input.swipeStep(-60, 0), 1);
  expect("a mostly vertical drag is not a swipe", root.GT4.input.swipeStep(-100, 80), 0);
  expect("a diagonal drag at 1.5 ratio is not a swipe", root.GT4.input.swipeStep(-90, 60), 0);
  expect("no movement is not a swipe", root.GT4.input.swipeStep(0, 0), 0);

  // Right stick: deadzone 0.2, full deflection 18 px per frame, sign follows the stick.
  expect("stick inside the deadzone does not scroll", root.GT4.input.stickScroll(0.15), 0);
  expect("stick at rest does not scroll", root.GT4.input.stickScroll(0), 0);
  expect("stick full down scrolls 18 px", root.GT4.input.stickScroll(1), 18);
  expect("stick full up scrolls -18 px", root.GT4.input.stickScroll(-1), -18);
  expect("stick just past the deadzone scrolls a little", root.GT4.input.stickScroll(0.25) > 0 && root.GT4.input.stickScroll(0.25) < 2, true);

  // A click from the keyboard, the pad or assistive technology has no click count; a mouse, finger or pen click has one.
  // An option row only steps on the first kind (TASK-193).
  expect("a keyboard or assistive click steps an option row", root.GT4.input.stepsFromActivation(0), true);
  expect("a click with no count at all steps an option row", root.GT4.input.stepsFromActivation(undefined), true);
  expect("a mouse click does not step an option row", root.GT4.input.stepsFromActivation(1), false);
  expect("a finger tap does not step an option row", root.GT4.input.stepsFromActivation(1), false);
  expect("a double click does not step an option row", root.GT4.input.stepsFromActivation(2), false);

  // Slider from a touch position: 7 px thumb inset is applied by the caller; here the track is plain.
  expect("touch at the left end gives min", root.GT4.input.valueFromPointer(0, 0, 100, 1, 12, 1), 1);
  expect("touch at the right end gives max", root.GT4.input.valueFromPointer(100, 0, 100, 1, 12, 1), 12);
  expect("touch past the right end clamps to max", root.GT4.input.valueFromPointer(300, 0, 100, 1, 12, 1), 12);
  expect("touch before the left end clamps to min", root.GT4.input.valueFromPointer(-40, 0, 100, 1, 12, 1), 1);
  expect("touch in the middle rounds to a step", root.GT4.input.valueFromPointer(50, 0, 100, 1, 12, 1), 7);
  expect("steps of 5 snap to the step grid", root.GT4.input.valueFromPointer(52, 0, 100, 0, 100, 5), 50);
  expect("a zero-width track gives min", root.GT4.input.valueFromPointer(10, 5, 0, 1, 12, 1), 1);

  // Face and shoulder buttons: which route each one takes in each state.
  var pageHooks = { x: true, y: true, start: true, tabs: true };
  var open = { hooks: pageHooks, dialogOpen: false, tipOpen: false, capturing: false };
  var route = root.GT4.input.padRoute;
  expect("A activates", route("a", open), "activate");
  expect("B goes back on a plain page", route("b", open), "back");
  expect("B closes an open tip first", route("b", { hooks: pageHooks, tipOpen: true }), "close-tip");
  expect("B closes a dialog", route("b", { hooks: pageHooks, dialogOpen: true }), "dialog-back");
  expect("X runs the page's x hook", route("x", open), "hook-x");
  expect("Y runs the page's y hook", route("y", open), "hook-y");
  expect("X does nothing on a page without an x hook", route("x", { hooks: { y: true } }), null);
  expect("Start runs the page's start hook", route("start", open), "hook-start");
  expect("LB goes to the previous tab", route("lb", open), "tab-prev");
  expect("RB goes to the next tab", route("rb", open), "tab-next");
  expect("LB does nothing on a page without tabs", route("lb", { hooks: {} }), null);
  expect("X is blocked by an open dialog", route("x", { hooks: pageHooks, dialogOpen: true }), null);
  expect("A still activates inside a dialog", route("a", { hooks: pageHooks, dialogOpen: true }), "activate");
  expect("while a rebind waits, B cancels it", route("b", { hooks: pageHooks, capturing: true }), "cancel-capture");
  expect("while a rebind waits, A does nothing", route("a", { hooks: pageHooks, capturing: true }), null);
  expect("while a rebind waits, Y does nothing", route("y", { hooks: pageHooks, capturing: true }), null);

  // Two-level focus (js/focus-levels.js): enter, B back, LB/RB keeping the level, the remembered row per tab.
  var FL = root.GT4.focusLevels;
  var TABS = ["graphics", "controls", "saves", "replays"];
  function same(name, actual, expected) {
    expect(name, JSON.stringify(actual), JSON.stringify(expected));
  }

  var opened = FL.start("graphics");
  same("opening Settings starts on the tab list", opened.level, FL.SIDE);
  same("opening Settings shows the current tab", opened.tab, "graphics");

  var entered = FL.enter(opened);
  same("A on a tab enters its content", entered.level, FL.CONTENT);
  same("entering keeps the tab", entered.tab, "graphics");
  same("entering does not change the input state", opened.level, FL.SIDE);

  var rowed = FL.remember(entered, "fxaa");
  same("focus on a row is remembered for its tab", FL.rowFor(rowed), "fxaa");
  var backed = FL.back(rowed);
  same("B in the content returns to the tab list", backed.level, FL.SIDE);
  same("B returns to the same tab", backed.tab, "graphics");
  same("B resets nothing: the row is still remembered", FL.rowFor(backed), "fxaa");
  same("entering again lands on the remembered row", FL.rowFor(FL.enter(backed)), "fxaa");

  var lbContent = FL.moveTab(entered, 1, TABS);
  same("LB/RB from content keeps the content level", lbContent.level, FL.CONTENT);
  same("LB/RB from content moves to the next tab", lbContent.tab, "controls");
  var lbSide = FL.moveTab(backed, 1, TABS);
  same("LB/RB from the tab list keeps the tab-list level", lbSide.level, FL.SIDE);
  same("the previous tab is remembered per tab", FL.rowFor(FL.moveTab(FL.moveTab(rowed, 1, TABS), -1, TABS)), "fxaa");
  same("another tab has no remembered row yet", FL.rowFor(lbContent), null);
  same("LB/RB wraps from the last tab to the first", FL.moveTab(FL.select(backed, "replays"), 1, TABS).tab, "graphics");
  same("RB/LB wraps back from the first tab", FL.moveTab(FL.select(backed, "graphics"), -1, TABS).tab, "replays");
  same("the preview (select) changes only the tab", FL.select(backed, "controls").level, FL.SIDE);

  var reopened = FL.open(backed, "controls");
  same("reopening Settings puts focus on the tab list", reopened.level, FL.SIDE);
  same("reopening keeps the remembered rows", FL.rowFor(FL.select(reopened, "graphics")), "fxaa");

  same("the tab-list footer shows A Enter, B Back, LB RB Tabs", FL.footer(FL.SIDE).map(function (item) { return item.key + " " + item.label; }).join("|"), "A Enter|B Back|LB RB Tabs");
  same("the content footer shows A Select, B Tabs", FL.footer(FL.CONTENT).map(function (item) { return item.key + " " + item.label; }).join("|"), "A Select|B Tabs");

  // Preview and focus (js/focus-levels.js onContentFocus): drawing a tab's content never moves focus.
  var sidePreview = FL.start("saves");
  same("a focus event from rendering the preview keeps the tab list", FL.onContentFocus(sidePreview, true).level, FL.SIDE);
  same("a focus event from the player moves to the rows", FL.onContentFocus(sidePreview, false).level, FL.CONTENT);
  same("a focus event in the rows changes nothing", FL.onContentFocus(entered, false) === entered, true);
  same("a preview render inside the rows changes nothing", FL.onContentFocus(entered, true) === entered, true);

  // Entry (js/settings-model.js): A on Saves or Replays focuses the first row, or Import when the list is empty.
  var SM = root.GT4.settingsModel;
  same("entering Saves with rows focuses the first row", SM.libraryEntry(3), "row");
  same("entering Replays with one row focuses that row", SM.libraryEntry(1), "row");
  same("entering Saves with an empty list focuses Import", SM.libraryEntry(0), "import");

  // Value changes (js/settings-model.js): the row model holds the written value and visibility follows it.
  var model = {
    max_upscale: 4,
    rows: [
      { id: "upscale", current: "1080p", control: "dropdown",
        values: [{ value: "1080p", label: "1080p (3x)" }, { value: "4K", label: "4K (6x)" }] },
      { id: "sharp", current: 50, control: "slider", page: "basic", visible_when: { id: "upscale", equals: "4K" } },
      { id: "fxaa", current: false, control: "toggle", page: "basic" }
    ]
  };
  same("a dropdown shows the label of the chosen option", SM.optionLabel(model.rows[0].values, "4K"), "4K (6x)");
  same("a value without an option shows the raw value", SM.optionLabel(model.rows[0].values, "8K"), "8K");
  same("a row hidden by visible_when stays hidden at the old value", SM.rowVisible(model, model.rows[1], false), false);
  SM.applyChange(model, "upscale", "4K");
  same("the row model holds the dropdown value after a change", SM.findRow(model, "upscale").current, "4K");
  same("a row that depends on the changed row shows at once", SM.rowVisible(model, model.rows[1], false), true);
  SM.applyChange(model, "upscale", "1080p");
  same("the dependent row hides again when the value goes back", SM.rowVisible(model, model.rows[1], false), false);
  SM.applyChange(model, "fxaa", true);
  same("a toggle holds a boolean after a change", SM.findRow(model, "fxaa").current, true);
  same("a change to an unknown row changes nothing", SM.applyChange(model, "nope", 1), null);
  same("mod rows stay hidden while the feature is off", SM.rowVisible(model, { id: "m", page: "mods" }, false), false);

  // Overlapping reads: only the newest answer is drawn.
  var gate = SM.readGate();
  var firstRead = gate.ticket();
  var secondRead = gate.ticket();
  same("an older read is not applied", gate.isLatest(firstRead), false);
  same("the newest read is applied", gate.isLatest(secondRead), true);

  // Controls page model (js/controls-model.js): live state from Gamepad values, keys, conflicts, trigger output.
  var CM = root.GT4.controlsModel;
  function fakePad(pressed, values, axes) {
    var buttons = CM.STANDARD_TO_XINPUT.map(function (name) {
      var value = values && values[name] !== undefined ? values[name] : (pressed.indexOf(name) >= 0 ? 1 : 0);
      return { pressed: value > 0.12, value: value };
    });
    return { id: "Xbox 360 Controller (XInput STANDARD GAMEPAD)", connected: true, buttons: buttons, axes: axes || [0, 0, 0, 0] };
  }
  var defaults = [
    { id: "cross", sources: ["xinput:A", "key:Z"] }, { id: "circle", sources: ["xinput:B", "key:X"] },
    { id: "l1", sources: ["xinput:LB", "key:Q"] }, { id: "l2", sources: ["xinput:LT", "key:1"] },
    { id: "r2", sources: ["xinput:RT | deadzone=0.000 curve=quadratic", "key:2"] },
    { id: "dpad_up", sources: ["xinput:DpadUp", "key:Up"] }, { id: "select", sources: ["xinput:Back"] }
  ];
  var live = CM.liveState(CM.padLive(fakePad(["A", "DpadUp"], { LT: 0.6 }, [0.5, -0.25, -1, 1])), defaults, {});
  same("pad A lights Cross", live.buttons.cross.pressed, true);
  same("pad B up leaves Circle dark", live.buttons.circle.pressed, false);
  same("D-pad up lights the D-pad up arm", live.buttons.dpad_up.pressed, true);
  same("LT at 60% fills L2 to 60%", Math.round(live.buttons.l2.value * 100), 60);
  same("L2 reads the game's pressure past the 30/255 deadzone: (153 - 30) * 255 / 225", Math.round(live.buttons.l2.output * 255), 139);
  same("sticks keep their raw position", [live.sticks.left.x, live.sticks.left.y, live.sticks.right.x, live.sticks.right.y], [0.5, -0.25, -1, 1]);
  var swapped = CM.liveState(CM.padLive(fakePad(["A"])), [{ id: "circle", sources: ["xinput:A"] }, { id: "cross", sources: ["xinput:B"] }], {});
  same("after a swap, pad A lights Circle, not Cross", [swapped.buttons.circle.pressed, swapped.buttons.cross.pressed], [true, false]);
  same("a key lights the button it drives", CM.liveState(null, defaults, { Q: true }).buttons.l1.pressed, true);
  same("no pad and no key: nothing is pressed", CM.liveState(null, defaults, {}).buttons.cross.pressed, false);
  var light = CM.liveState(CM.padLive(fakePad([], { LT: 0.1 })), defaults, {});
  same("a trigger inside its deadzone fills but does not press", [light.buttons.l2.value > 0, light.buttons.l2.pressed], [true, false]);
  same("R2 options (curve) shape the output", Math.round(CM.liveState(CM.padLive(fakePad([], { RT: 0.5 })), defaults, {}).buttons.r2.output * 100), 25);

  // Full parity verification against native C++ controls_fixture vectors across all 7 curves
  var curves7 = ["linear", "quadratic", "cubic", "s_curve", "smooth_power", "sublinear", "logarithmic"];

  // 1. Zero deadzone: rest at 0, half travel (raw 128) bit-exact to C++ fixture, and full travel at 255
  var midDz0 = { linear: 128, quadratic: 64, cubic: 32, s_curve: 128, smooth_power: 90, sublinear: 180, logarithmic: 189 };
  curves7.forEach(function (c) {
    same("trigger output dz=0 rest " + c, CM.triggerOutput(0, 0, c), 0);
    same("trigger output dz=0 mid " + c, CM.triggerOutput(128 / 255, 0, c), midDz0[c] / 255);
    same("trigger output dz=0 full " + c, CM.triggerOutput(1, 0, c), 1);
  });

  // 2. Default deadzone (30/255): rest at threshold (raw 30), mid travel (raw 142) bit-exact to C++, and full travel
  var midDzDefault = { linear: 126, quadratic: 63, cubic: 31, s_curve: 126, smooth_power: 89, sublinear: 179, logarithmic: 188 };
  curves7.forEach(function (c) {
    same("trigger output default dz rest " + c, CM.triggerOutput(30 / 255, 30 / 255, c), 0);
    same("trigger output default dz mid " + c, CM.triggerOutput(142 / 255, 30 / 255, c), midDzDefault[c] / 255);
    same("trigger output default dz full " + c, CM.triggerOutput(1, 30 / 255, c), 1);
  });

  // 3. Representative half deadzone (0.5): rest at raw 128, mid travel (raw 192) bit-exact to C++, and full travel
  var midDzHalf = { linear: 128, quadratic: 64, cubic: 32, s_curve: 129, smooth_power: 91, sublinear: 181, logarithmic: 189 };
  curves7.forEach(function (c) {
    same("trigger output 0.5 dz rest " + c, CM.triggerOutput(128 / 255, 0.5, c), 0);
    same("trigger output 0.5 dz mid " + c, CM.triggerOutput(192 / 255, 0.5, c), midDzHalf[c] / 255);
    same("trigger output 0.5 dz full " + c, CM.triggerOutput(1, 0.5, c), 1);
  });

  // TASK-161: the game's 100% at the end of a trigger's travel, on its own scale and with the full trigger range.
  same("R2's game 100% is 45", CM.triggerFull("r2", ["xinput:RT"], false), 45);
  same("L2's game 100% is 84", CM.triggerFull("l2", ["xinput:LT | deadzone=0.100 curve=linear"], false), 84);
  same("a trigger on another button keeps the byte", CM.triggerFull("cross", ["xinput:RT"], false), 255);
  same("range sets the game scale's full", CM.triggerFull("r2", ["key:R", "xinput:RT | range=84"], false), 84);
  same("full range: the whole byte", CM.triggerFull("r2", ["xinput:RT | range=45"], true), 255);
  same("full range: range_full sets it", CM.triggerFull("l2", ["xinput:LT | range=84 range_full=200"], true), 200);
  same("range_full is ignored on the game scale", CM.triggerFull("l2", ["xinput:LT | range_full=200"], false), 84);
  same("no trigger source: the defaults", CM.triggerFull("r2", ["key:R"], true), 255);

  same("KeyZ is Z", CM.keyFromEvent({ code: "KeyZ", key: "z" }), { name: "Z" });
  same("Digit1 is 1", CM.keyFromEvent({ code: "Digit1", key: "1" }), { name: "1" });
  same("Numpad4 is Num4", CM.keyFromEvent({ code: "Numpad4", key: "4" }), { name: "Num4" });
  same("ArrowUp is Up", CM.keyFromEvent({ code: "ArrowUp", key: "ArrowUp" }), { name: "Up" });
  same("right Shift is Shift", CM.keyFromEvent({ code: "ShiftRight", key: "Shift" }), { name: "Shift" });
  same("Esc cancels and is named Esc", CM.keyFromEvent({ code: "Escape", key: "Escape" }), { cancel: true, name: "Esc" });
  same("F9 is reserved for the game window", CM.keyFromEvent({ code: "F9", key: "F9" }), { reserved: "F9" });
  same("F5 is a key name", CM.keyFromEvent({ code: "F5", key: "F5" }), { name: "F5" });
  same("a key with no name is refused", CM.keyFromEvent({ code: "Semicolon", key: ";" }), { unsupported: ";" });

  same("a free source has no conflict", CM.conflict(defaults, "cross", "xinput:RB"), null);
  same("the button's own source is no change", CM.conflict(defaults, "cross", "xinput:A"), { same: true });
  same("a taken source names the holder and the swap", CM.conflict(defaults, "cross", "xinput:LB"), { holder: "l1", previous: "xinput:A" });
  same("a conflict ignores options", CM.conflict(defaults, "cross", "xinput:RT"), { holder: "r2", previous: "xinput:A" });
  same("a button without that family moves the source", CM.conflict(defaults, "select", "key:Q"), { holder: "l1", previous: null });

  same("XInput labels", [CM.label("xinput:LB").text, CM.label("xinput:LThumb").text, CM.label("xinput:DpadLeft").dir, CM.label("key:Backspace").text, CM.label(null).kind], ["LB", "LS", "left", "Bksp", "none"]);
  same("the controller name drops the API suffix", CM.cleanPadName("Xbox 360 Controller (XInput STANDARD GAMEPAD)"), "Xbox 360 Controller");
  var two = CM.padSummary([null, { id: "Pad One (XInput STANDARD GAMEPAD)", index: 1, connected: true }, { id: "Pad Two", index: 2, connected: true }]);
  same("two pads: the first connected one is shown and counted", [two.connected, two.name, two.index, two.count], [true, "Pad One", 1, 2]);
  same("no pad: not connected", CM.padSummary([null, null]).connected, false);
  // Fixed keyboard layout (TASK-076, read only) and the pad-live shape from the Rust service (SDL3).
  same("keyboard: W lights Cross", CM.liveState(null, defaults, { W: true }).buttons.cross.pressed, true);
  same("keyboard: Enter lights Cross too", CM.liveState(null, defaults, { Enter: true }).buttons.cross.pressed, true);
  same("keyboard: Space lights Circle", CM.liveState(null, defaults, { Space: true }).buttons.circle.pressed, true);
  same("keyboard: E lights D-pad up (ghost)", CM.liveState(null, defaults, { E: true }).buttons.dpad_up.pressed, true);
  same("keyboard: the fixed triangle keys are Backspace and Esc", CM.KEYBOARD_LAYOUT.triangle, ["Backspace", "Esc"]);
  same("keyboard: same table as the game (slice 210)", [CM.KEYBOARD_LAYOUT.r1, CM.KEYBOARD_LAYOUT.r2, CM.KEYBOARD_LAYOUT.l2,
    CM.KEYBOARD_LAYOUT.l1, CM.KEYBOARD_LAYOUT.select, CM.KEYBOARD_LAYOUT.start], [["Shift"], ["R"], ["F"], ["Q"], ["C", "Tab"], ["P"]]);
  same("keyboard: C lights Select (camera)", CM.liveState(null, defaults, { C: true }).buttons.select.pressed, true);
  same("keyboard: P lights Start", CM.liveState(null, [{ id: "start", sources: ["xinput:Start"] }], { P: true }).buttons.start.pressed, true);
  var sdlLive = CM.liveState({ sources: { "xinput:A": 1, "xinput:LT": 0.5 }, sticks: { left: { x: 0.5, y: -0.25 }, right: { x: 0, y: 1 } } }, defaults, {});
  same("pad-live: A presses Cross, LT fills L2, the left stick keeps its place", [sdlLive.buttons.cross.pressed, Math.round(sdlLive.buttons.l2.value * 100), sdlLive.sticks.left.x, sdlLive.sticks.left.y], [true, 50, 0.5, -0.25]);
  same("pad-live: no sources, nothing pressed", CM.liveState({ sources: {}, sticks: {} }, defaults, {}).buttons.cross.pressed, false);

  // Spatial navigation between DS2 buttons, on the drawing's own layout (js/controller-svg.js).
  var LAYOUT = root.GT4.controllerSvg.LAYOUT;
  var ids = Object.keys(LAYOUT);
  var padRects = ids.map(function (id) { return rect(LAYOUT[id].x - 20, LAYOUT[id].y - 20, 40, 40); });
  function padMove(from, dir) {
    var next = pick(padRects, ids.indexOf(from), dir);
    return next < 0 ? null : ids[next];
  }
  same("Cross up is Triangle", padMove("cross", "up"), "triangle");
  same("Cross left is Square", padMove("cross", "left"), "square");
  same("Cross right is Circle", padMove("cross", "right"), "circle");
  same("Square left is Start", padMove("square", "left"), "start");
  same("Start left is Select", padMove("start", "left"), "select");
  same("Select left is D-pad right", padMove("select", "left"), "dpad_right");
  same("D-pad up, up is L1", padMove("dpad_up", "up"), "l1");
  same("L1 up is L2", padMove("l1", "up"), "l2");
  same("D-pad down, down is L3", padMove("dpad_down", "down"), "l3");
  same("L3 right is R3", padMove("l3", "right"), "r3");
  same("Triangle up is R1", padMove("triangle", "up"), "r1");
  same("Circle right leaves the drawing (the panel is next)", padMove("circle", "right"), null);

  // TASK-181: the Wheel page's model (js/wheel-model.js), the game's own formulas.
  var wm = root.GT4.wheelModel;
  var G27 = "030000006d0400009bc2000000000000";
  var AUX = "03000000b70e00003b18000000000000";
  var wheel = { id: 5, guid: G27, axes: [6554, 32767, -32768, 0], pressed: [0, 4], hats: [3] };
  var aux = { id: 9, guid: AUX, axes: [-32768, 30000], pressed: [2], hats: [] };
  var live = [wheel, aux];
  var steering = { source: "axis:0", rotation_degrees: 900, invert: false };
  expect("a fifth of the travel is +90 degrees on a 900-degree wheel", Math.round(wm.steeringDegrees(steering, 6554)), 90);
  expect("full right is +450 degrees at 900", wm.steeringDegrees(steering, 32767), 450);
  expect("full left is -450 degrees at 900", wm.steeringDegrees(steering, -32768), -450);
  expect("a 1080-degree wheel turns to 540", wm.steeringDegrees({ source: "axis:0", rotation_degrees: 1080, invert: false }, 32767), 540);
  expect("invert reverses the angle", wm.steeringDegrees({ source: "axis:0", rotation_degrees: 900, invert: true }, 32767), -450);
  expect("the live steering reads the primary device", Math.round(wm.steeringLive(steering, live, 5).degrees), 90);
  expect("no primary device: the steering is not connected", wm.steeringLive(steering, live, null).connected, false);
  expect("the angle text uses a sign and a real minus", wm.angleText(-90.4) + " " + wm.angleText(90.6) + " " + wm.angleText(0.2), "−90° +91° 0°");
  var pedal = { source: "axis:2", mode: "inverted", deadzone: 0.02, saturation: 0.98, curve: "linear" };
  expect("an inverted pedal at rest gives 0", wm.pedalByte(pedal, 32767), 0);
  expect("an inverted pedal fully pressed gives 255", wm.pedalByte(pedal, -32768), 255);
  expect("a pedal inside the dead zone gives 0", wm.pedalByte(pedal, 32767 - 600), 0);
  expect("past the saturation the pedal is full", wm.pedalByte(pedal, -32000), 255);
  expect("a pressed pedal is at least 1", wm.pedalByte(pedal, 32767 - 1400) >= 1, true);
  expect("quadratic halves the middle", Math.abs(wm.pedalByte(Object.assign({}, pedal, { deadzone: 0, saturation: 1, curve: "quadratic" }), 0) - 64) <= 1, true);
  expect("the live pedal on the wheel is full", wm.pedalLive(pedal, live, 5).output, 1);
  expect("a dev: pedal reads its own device", wm.pedalLive({ source: "dev:" + AUX + ":axis:1", mode: "normal", deadzone: 0, saturation: 1, curve: "linear" }, live, 5).travel > 0.95, true);
  expect("a digital pedal is full while pressed", wm.pedalLive({ source: "btn:4", mode: "inverted", deadzone: 0, saturation: 1, curve: "linear" }, live, 5).output, 1);
  expect("an unbound pedal is not bound", wm.pedalLive({ source: "", mode: "inverted" }, live, 5).bound, false);
  expect("a missing device leaves the pedal at 0", wm.pedalLive(pedal, live, null).output, 0);
  expect("combined negative counts the lower half", wm.pedalTravel("combined_negative", -16384) > 0.49 && wm.pedalTravel("combined_negative", 100) === 0, true);
  expect("a button source is pressed", wm.sourcePressed("btn:4", live, 5), true);
  expect("a hat diagonal presses up and right", wm.sourcePressed("hat:0:up", live, 5) && wm.sourcePressed("hat:0:right", live, 5) && !wm.sourcePressed("hat:0:down", live, 5), true);
  expect("axis+ passes the game's +16384", wm.sourcePressed("dev:" + AUX + ":axis:1+", live, 5), true);
  expect("axis- needs below -16384", wm.sourcePressed("axis:2-", live, 5) && !wm.sourcePressed("axis:3-", live, 5), true);
  expect("a plain axis on a button: above 0 or past 20000", wm.sourcePressed("axis:1", live, 5) && !wm.sourcePressed("axis:3", live, 5), true);
  expect("a dev: source of a missing device is never pressed", wm.sourcePressed("dev:0300000000000000000000000000ffff:btn:0", live, 5), false);
  expect("two devices with one GUID name none", wm.sourcePressed("dev:" + G27 + ":btn:0", [wheel, Object.assign({}, wheel, { id: 6 })], 5), false);
  expect("the chips count buttons from 1", JSON.stringify(wm.describe("btn:0")), JSON.stringify({ kind: "button", number: 1, dir: "", sign: "", aux: false }));
  expect("an axis threshold keeps its sign", wm.describe("axis:3-").sign + wm.describe("axis:3+").sign, "-+");
  expect("a dev: source is marked as another device", wm.describe("dev:" + AUX + ":hat:0:left").aux, true);
  expect("an invalid source has no description", wm.describe("axis:40"), null);
  var config = { steering: steering, throttle: pedal, brake: { source: "" }, clutch: { source: "axis:1" }, handbrake: { source: "" },
    buttons: { cross: ["btn:0"], r2: ["btn:4", "btn:12"], l2: [], dpad_up: ["hat:0:up"] } };
  var down = wm.pressedSlots(config, live, 5);
  expect("pressed slots follow the bindings (gear up is r2)", down.cross && down.r2 && down.dpad_up && !down.l2, true);
  expect("the wizard has steering, three pedals and 16 buttons", wm.WIZARD.length, 21);
  expect("the wizard asks left then right first", wm.WIZARD[0].phase + " " + wm.WIZARD[1].phase, "left right");
  expect("gear up writes the r2 slot and gear down the l2 slot", wm.controlOfDfp("gear_up").slot + " " + wm.controlOfDfp("gear_down").slot, "r2 l2");
  expect("the DFP's R2 and L2 are the r1 and l1 slots", wm.controlOfDfp("r2").slot + " " + wm.controlOfDfp("l2").slot, "r1 l1");
  expect("bound count over the wizard's controls", JSON.stringify(wm.boundCount(config)), JSON.stringify({ bound: 6, total: 20 }));
  expect("turning left read positive: the steering is inverted", wm.steeringInvert(30000, -30000), true);
  expect("turning left read negative: the steering is not inverted", wm.steeringInvert(-30000, 30000), false);
  expect("900 degrees gives no warning, 1080 does", !wm.rotationWarning(900) && wm.rotationWarning(1080), true);
  expect("no session: unknown", wm.received(null).value + "/" + wm.received({ found: false }).reason, "unknown/no_session");
  expect("a summary that reached the wheel: yes", wm.received({ found: true, received: "yes", reason: "summary" }).value, "yes");
  expect("the main device is the one the game drives", wm.mainDevice({ primary: 9, devices: [{ id: 5, role: "other_wheel" }, { id: 9, role: "wheel" }] }).id, 9);

  // TASK-185: the virtual controller layout model (js/virtual-pad-model.js), in the contract's screen fractions.
  var VP = root.GT4.virtualPadModel;
  var VP_LAYOUT = { version: 1, name: "t", opacity: 0.6, haptics: true, elements: [
    { id: "lstick", type: "stick", binds: "left_stick", x: 0.13, y: 0.70, size: 0.32 },
    { id: "dpad", type: "dpad", binds: "dpad", x: 0.13, y: 0.30, size: 0.26 },
    { id: "cross", type: "button", binds: "cross", x: 0.88, y: 0.78, size: 0.15 },
    { id: "square", type: "button", binds: "square", x: 0.80, y: 0.66, size: 0.15, opacity: 0.4, visible: false },
    { id: "l1", type: "button", binds: "l1", x: 0.30, y: 0.10, size: 0.12, width: 0.16 },
    { id: "l2", type: "button", binds: "l2", x: 0.13, y: 0.10, size: 0.12, width: 0.16 },
    { id: "select", type: "button", binds: "select", x: 0.44, y: 0.92, size: 0.09, width: 0.11 }
  ] };
  function vp(id) { return VP.find(VP_LAYOUT, id); }
  function vpOf(layout, id) { return VP.find(layout, id); }
  function vpClose(a, b) { return Math.abs(a - b) < 0.0001; }
  expect("a square element is square on a square screen", vpClose(VP.rect(vp("cross"), 1).width, 0.15), true);
  expect("a square element is narrower in x on a wide screen", vpClose(VP.rect(vp("cross"), 20 / 9).width, 0.15 / (20 / 9)), true);
  expect("a wide button keeps its own width", vpClose(VP.rect(vp("l1"), 20 / 9).width, 0.16), true);
  var crossBox = VP.rect(vp("cross"), 20 / 9);
  expect("the rect is the element's box", vpClose(crossBox.y0, 0.78 - 0.075) && vpClose(crossBox.height, 0.15), true);
  expect("move keeps an element inside the left edge", vpClose(vpOf(VP.move(VP_LAYOUT, "lstick", -2, 0, 20 / 9), "lstick").x, 0.32 / (2 * 20 / 9)), true);
  expect("move keeps an element inside the right edge", vpClose(vpOf(VP.move(VP_LAYOUT, "cross", 2, 0, 20 / 9), "cross").x, 1 - 0.15 / (2 * 20 / 9)), true);
  expect("move keeps an element inside the bottom edge", vpClose(vpOf(VP.move(VP_LAYOUT, "lstick", 0, 2, 20 / 9), "lstick").y, 0.84), true);
  expect("a wide button is clamped by its own half width", vpClose(vpOf(VP.move(VP_LAYOUT, "l2", -2, 0, 20 / 9), "l2").x, 0.08), true);
  expect("resizing scales the size and the width of a wide button", vpClose(vpOf(VP.resize(VP_LAYOUT, "l1", 2, 20 / 9), "l1").size, 0.24) &&
    vpClose(vpOf(VP.resize(VP_LAYOUT, "l1", 2, 20 / 9), "l1").width, 0.32), true);
  expect("setSize keeps the shape of a wide button", vpClose(vpOf(VP.setSize(VP_LAYOUT, "l1", 0.24, 20 / 9), "l1").width, 0.32), true);
  expect("setWidth changes only the width", vpClose(vpOf(VP.setWidth(VP_LAYOUT, "l1", 0.5, 20 / 9), "l1").width, 0.5) &&
    vpClose(vpOf(VP.setWidth(VP_LAYOUT, "l1", 0.5, 20 / 9), "l1").size, 0.12), true);
  expect("a square button has no width to set", vpOf(VP.setWidth(VP_LAYOUT, "cross", 0.5, 20 / 9), "cross").width, undefined);
  expect("the size stays above its minimum", vpClose(vpOf(VP.setSize(VP_LAYOUT, "cross", 0.001, 20 / 9), "cross").size, VP.MIN_SIZE), true);
  expect("the size stays below its maximum", vpClose(vpOf(VP.setSize(VP_LAYOUT, "l1", 5, 20 / 9), "l1").size, VP.MAX_SIZE), true);
  same("opacity is clamped to 0..1", [VP.setOpacity(VP_LAYOUT, "square", 1.5).elements[3].opacity, VP.setOpacity(VP_LAYOUT, "square", -1).elements[3].opacity], [1, 0]);
  same("showing an element removes the hidden flag", "visible" in vpOf(VP.setVisible(VP_LAYOUT, "square", true), "square"), false);
  same("hiding an element writes the hidden flag", vpOf(VP.setVisible(VP_LAYOUT, "square", false), "square").visible, false);
  expect("the grid snaps to 2%", Math.round(VP.snapValue(0.147, VP.GRID) * 1000) / 1000 + "/" + Math.round(VP.snapValue(0.151, VP.GRID) * 1000) / 1000, "0.14/0.16");
  expect("the default layout's L2 touches but does not overlap the F7 corner", VP.overlapsReserved(VP_LAYOUT, 20 / 9).length, 0);
  expect("an element moved onto the corner is reported", JSON.stringify(VP.overlapsReserved(VP.move(VP_LAYOUT, "l2", -0.05, -0.05, 20 / 9), 20 / 9)), JSON.stringify(["l2"]));
  expect("the aspect list is 20:9, 16:9 and 21:9", VP.ASPECTS.map(function (a) { return a.id; }).join("|"), "20:9|16:9|21:9");
  expect("the ratio and the id of a shape agree", VP.ratioOf("16:9") + "|" + VP.aspectIdOf(21 / 9) + "|" + VP.aspectIdOf(1), (16 / 9) + "|21:9|20:9");
  expect("a valid layout has no problem", VP.problems(VP_LAYOUT).length, 0);
  var vpDuplicate = VP.clone(VP_LAYOUT);
  vpDuplicate.elements[1].id = "lstick";
  expect("a duplicate element id is a problem", VP.problems(vpDuplicate)[0].code, "duplicate_id");
  var vpBadBind = VP.clone(VP_LAYOUT);
  vpBadBind.elements[0].binds = "dpad";
  expect("a stick bound to the D-pad is a problem", VP.problems(vpBadBind)[0].code, "binds");
  var vpOut = VP.clone(VP_LAYOUT);
  vpOut.elements[0].x = 1.2;
  expect("an element off the screen is a problem", VP.problems(vpOut)[0].code, "x");
  expect("the element names come from the Controls page", VP.nameOf(vp("lstick")) + "/" + VP.nameOf(vp("cross")) + "/" + VP.nameOf(vp("dpad")), "Left stick/Cross/D-pad");
  var vpCopy = VP.clone(VP_LAYOUT);
  vpCopy.elements[0].x = 0.5;
  expect("clone is a deep copy", vp("lstick").x, 0.13);

  function run() {
    var list = document.getElementById("results");
    var passed = 0;
    cases.forEach(function (item) {
      if (item.pass) {
        passed += 1;
      }
      var li = document.createElement("li");
      li.className = "step";
      var label = document.createElement("span");
      label.className = "t";
      label.textContent = (item.pass ? "PASS  " : "FAIL  ") + item.name + (item.pass ? "" : " (" + item.detail + ")");
      li.appendChild(label);
      list.appendChild(li);
    });
    var summary = document.getElementById("summary");
    summary.textContent = passed + " of " + cases.length + " passed.";
    root.GT4_NAV_TEST_RESULT = { passed: passed, total: cases.length };
  }

  run();
}

// The footer labels come from the locale files (ui/locales): the cases run once they are loaded.
window.GT4.i18n.ready.then(function () {
  startNavTest(window);
});
