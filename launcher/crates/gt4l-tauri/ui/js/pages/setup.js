/*
 * Setup page (first launch, or after the game files went missing). Setup cannot be skipped: Home is shown only
 * when app_state says "done". The page polls app_state while it is open and draws the state it reports.
 *
 * app_state().setup = {
 *   game: {id, name} | null,            (the game being set up; null on the very first start: the disc decides)
 *   state: "needed" | "wrong_disc" | "checking" | "failed" | "files_missing" | "done",
 *   step: "copying" | "checking" | "preparing" | "renderer",   (while checking)
 *   progress: 0..1,                     (of the current step)
 *   bytes_done, bytes_total,            (copying, the renderer download, the shaders compiled)
 *   renderer_step: "checking" | "compiling" | "listing" | "downloading" | "installing" | "done" | "failed",
 *   renderer_choice: "coregs20" | "gsdx_fallback" | "",  (what setup decided for this PC)
 *   disc_name, problem, note            (note: GSdx could not be downloaded; the game cannot start yet)
 * }
 * Two games (TASK-101): a disc of the other game than the one asked for offers to install it as that game (js/games.js).
 * When another game is already installed the page also has "Back" to Home.
 * Flow: "Choose disc image" -> setup_choose_disc (Rust file picker and fast check) -> setup_start (copy, check,
 * prepare, renderer). A refused file shows its reason with "Choose another file". When setup is done the page
 * shows "Finish", which opens Home (it never starts the game). Commands: setup_choose_disc, setup_start, setup_cancel.
 */
(function (root) {
  "use strict";
  var GT4 = (root.GT4 = root.GT4 || {});
  var h = GT4.ui.h;

  // The step names are keys: read when the page is drawn, so a language change applies.
  var STEPS = [
    { id: "copying", label: "setup.step_copying", detail: "setup.step_copying_detail" },
    { id: "checking", label: "setup.step_checking", detail: null },
    { id: "preparing", label: "setup.step_preparing", detail: null },
    { id: "renderer", label: "setup.step_renderer", detail: "setup.step_renderer_detail" }
  ];
  var RENDERER_KEYS = {
    checking: "setup.renderer_checking",
    compiling: "setup.renderer_compiling",
    listing: "setup.renderer_listing",
    downloading: "setup.renderer_downloading",
    installing: "setup.renderer_installing",
    done: "setup.renderer_done",
    failed: "setup.renderer_failed"
  };
  var POLL_MS = 400;

  // TASK-180 (owner, 2026-11-06): the last step prepares the renderer, which on Android means nothing at all: CoreGS 2.0 is
  // inside the game and the PC GSdx is never downloaded there. The row is left out instead of sitting greyed for ever.
  function stepsHere() {
    if (GT4.platform !== "android") {
      return STEPS;
    }
    return STEPS.filter(function (step) { return step.id !== "renderer"; });
  }

  function call(command, args) {
    return GT4.backend.call(command, args);
  }

  function gigabytes(bytes) {
    return (bytes / 1e9).toFixed(1) + " GB";
  }

  function megabytes(bytes) {
    return (bytes / 1e6).toFixed(1) + " MB";
  }

  function barNode(pct) {
    var fill = h("i");
    // A finished step never animates: the bar is placed at once, so no transition is left half-way when the step changes.
    if (pct >= 100) {
      fill.style.transition = "none";
    }
    fill.style.width = pct + "%";
    return h("span", { class: "bar" }, [fill]);
  }

  // One row: number, label (+ detail, bytes line, bar), value on the right.
  function row(number, label, detail, status, pct, bytesText) {
    var value = "";
    var valueClass = "v";
    if (status === "ok") {
      value = "✓";
      valueClass += " ok";
    } else if (status === "bad") {
      value = GT4.t("setup.not_supported");
      valueClass += " bad";
    } else if (status === "active" && pct !== null) {
      value = pct + "%";
    } else if (status === "active") {
      value = "…";
    }
    return h("li", { class: "step" + (status === "wait" ? " wait" : "") }, [
      h("span", { class: "n", text: number }),
      h("span", null, [
        h("span", { class: "t", text: label }),
        detail ? h("span", { class: "d", text: detail }) : null,
        bytesText ? h("span", { class: "bytes", text: bytesText }) : null,
        (status === "active" || status === "ok") && pct !== null ? barNode(pct) : null
      ]),
      h("span", { class: valueClass, text: value })
    ]);
  }

  // Signature decides whether the page has to be rebuilt.
  function signature(setup) {
    return [setup.state, setup.step, Math.round((setup.progress || 0) * 100), setup.renderer_step || "", setup.renderer_choice || "",
      Math.round((setup.bytes_done || 0) / 1e6), setup.problem || "", setup.note || "", setup.disc_name || ""].join("|");
  }

  function gameName(setup) {
    return setup.game && setup.game.name ? setup.game.name : "";
  }

  function discHint(setup) {
    var name = gameName(setup);
    if (!name) {
      return GT4.t("setup.disc_hint");
    }
    return name === "Gran Turismo 4" ? GT4.t("setup.disc_hint_gt4") : GT4.t("setup.disc_hint_named", { game: name });
  }

  function texts(state, setup) {
    var name = gameName(setup);
    switch (state) {
      case "done":
        return [name ? GT4.t("setup.done_title_named", { game: name }) : GT4.t("setup.done_title"), GT4.t("setup.done_text")];
      case "files_missing":
        return [GT4.t("setup.missing_title"), GT4.t("setup.missing_text")];
      case "checking":
        return [name ? GT4.t("setup.checking_title_named", { game: name }) : GT4.t("setup.checking_title"), GT4.t("setup.checking_text")];
      default:
        return [name ? GT4.t("setup.set_up_named", { game: name }) : GT4.t("setup.set_up"),
          name ? GT4.t("setup.choose_named_text", { game: name }) : GT4.t("setup.choose_text")];
    }
  }

  function build(setup, parts) {
    var state = setup.state;
    var heading = texts(state, setup);
    parts.title.textContent = heading[0];
    parts.sub.textContent = heading[1];

    var steps = [];
    var buttons = [];
    var hints = [];
    var shown = stepsHere();
    var order = shown.map(function (s) { return s.id; }).indexOf(setup.step);
    var running = state === "checking";
    var done = state === "done";
    var discName = setup.disc_name || "";
    // Step 1 says what the disc was recognised as (the title and the variant, from the disc itself).
    var recognised = discName ? GT4.t("setup.recognised", { name: discName }) : null;

    if (state === "wrong_disc") {
      steps.push(row("01", GT4.t("setup.step_disc"), discName || null, "bad", null, null));
    } else if (running || done) {
      steps.push(row("01", GT4.t("setup.step_disc"), recognised, "ok", null, null));
    } else {
      // Not chosen yet ("..." waits for the pick), or the file of a stopped run (no mark).
      steps.push(discName && state === "failed"
        ? row("01", GT4.t("setup.step_disc"), recognised, "none", null, null)
        : row("01", GT4.t("setup.step_disc"), discHint(setup), "active", null, null));
    }
    // A refused file, a stopped run and missing game files show the Disc row and the reason only, so the buttons
    // stay on screen at 1280 x 720.
    if (state === "needed" || running || done) {
      shown.forEach(function (step, index) {
        var status = "wait";
        var pct = null;
        var bytesText = null;
        var detail = step.detail ? GT4.t(step.detail) : null;
        if (done || (running && index < order)) {
          status = "ok";
          if (step.id === "copying" && running && setup.step !== "renderer" && setup.bytes_total) {
            // The copy ended: its bar is drawn full with the final figure, even when the poll never saw the last
            // 100% value (rc5 owner report: the Spec II copy bar stopped short).
            pct = 100;
            bytesText = gigabytes(setup.bytes_total) + " of " + gigabytes(setup.bytes_total);
          }
        } else if (running && index === order) {
          status = "active";
          pct = Math.round((setup.progress || 0) * 100);
          if (step.id === "copying" && setup.bytes_total) {
            // The bar and the figure of the copy use the same total (the size of the disc file being copied), so the bar
            // reaches 100% when the text says "x of x" (rc3 owner report: the bar stopped at about 85%).
            bytesText = gigabytes(setup.bytes_done || 0) + " of " + gigabytes(setup.bytes_total);
            pct = Math.min(100, Math.floor(((setup.bytes_done || 0) * 100) / setup.bytes_total));
          }
          if (step.id === "renderer") {
            detail = RENDERER_KEYS[setup.renderer_step] ? GT4.t(RENDERER_KEYS[setup.renderer_step]) : detail;
            if (setup.renderer_step === "compiling") {
              bytesText = GT4.t("setup.renderer_count", { done: setup.bytes_done || 0, total: setup.bytes_total || 0 });
            } else if (setup.renderer_step !== "downloading") {
              pct = null;
            } else if (setup.bytes_total) {
              bytesText = megabytes(setup.bytes_done || 0) + " of " + megabytes(setup.bytes_total);
            }
          }
        }
        if (done && step.id === "renderer") {
          if (setup.note) {
            detail = GT4.t("setup.renderer_failed");
          } else if (setup.renderer_choice === "coregs20") {
            detail = GT4.t("setup.renderer_ready_coregs20");
          } else if (setup.renderer_choice === "gsdx_fallback") {
            detail = GT4.t("setup.renderer_ready_gsdx");
          }
        }
        steps.push(row("0" + (index + 2), GT4.t(step.label), detail, status, pct, bytesText));
      });
    }

    var problem = setup.problem || "";
    parts.problem.textContent = problem;
    parts.problem.hidden = !problem || running || done;
    parts.notice.textContent = setup.note || "";
    parts.notice.hidden = !(done && setup.note);

    if (done) {
      buttons.push({ label: GT4.t("setup.finish"), primary: true, run: finishSetup });
      hints.push({ key: "A", label: GT4.t("hint.finish"), run: finishSetup });
    } else if (running) {
      if (setup.step !== "renderer") {
        buttons.push({ label: GT4.t("common.cancel"), run: cancelSetup });
        hints.push({ key: "B", label: GT4.t("hint.cancel"), run: cancelSetup });
      }
      buttons.push({ label: GT4.t("common.working"), primary: true, disabled: true });
    } else if (state === "failed") {
      if (discName) {
        buttons.push({ label: GT4.t("setup.try_again"), run: startSetup });
      }
      buttons.push({ label: GT4.t("common.choose_another_file"), primary: true, run: chooseDisc });
      hints.push({ key: "A", label: GT4.t("hint.select") });
    } else {
      buttons.push({ label: state === "wrong_disc" ? GT4.t("common.choose_another_file") : GT4.t("games.choose_disc"), primary: true, run: chooseDisc });
      hints.push({ key: "A", label: GT4.t("hint.choose_disc"), run: chooseDisc });
    }
    // Another game is installed: Setup is not a dead end. Back returns to Home (nothing is changed).
    if (!running && !done && active && active.anyInstalled) {
      buttons.push({ label: GT4.t("common.back"), run: backHome });
      hints.push({ key: "B", label: GT4.t("hint.back"), run: backHome });
      GT4.hooks.back = backHome;
    }

    GT4.ui.clear(parts.steps);
    steps.forEach(function (node) {
      parts.steps.appendChild(node);
    });
    GT4.ui.clear(parts.foot);
    buttons.forEach(function (button) {
      parts.foot.appendChild(h("button", {
        class: "btn" + (button.primary ? " primary" : ""),
        type: "button",
        disabled: !!button.disabled,
        on: { click: function () { if (button.run) { button.run(); } } }
      }, button.label));
    });
    GT4.ui.setHints(hints);
    if (running || done) {
      GT4.hooks.back = running && setup.step !== "renderer" ? cancelSetup : finishSetup;
    } else if (!(active && active.anyInstalled)) {
      GT4.hooks.back = null;
    }
  }

  function backHome() {
    GT4.go("#/home");
  }

  var busyChoosing = false;

  // Rust opens the file picker and checks the file. A refusal comes back as the error and app_state then says
  // wrong_disc with the reason, so the page shows it in the problem box (no toast needed).
  function chooseDisc() {
    if (busyChoosing) {
      return;
    }
    busyChoosing = true;
    var target = active && active.game ? active.game : null;
    call("setup_choose_disc", target ? { game: target } : {}).then(function (info) {
      busyChoosing = false;
      if (info && info.mismatch) {
        // The disc is the other game: install it as that game, or pick again.
        GT4.games.offerOtherGame(info, startSetup, chooseDisc);
      } else if (info) {
        startSetup();
      } else {
        poll();
      }
    }, function () {
      busyChoosing = false;
      poll();
    });
  }

  function startSetup() {
    call("setup_start").then(function () {
      poll();
    }, function (err) {
      GT4.ui.toast(GT4.errorText(err), true);
      poll();
    });
  }

  function cancelSetup() {
    call("setup_cancel").then(function () {
      poll();
    }, function (err) {
      GT4.ui.toast(GT4.errorText(err), true);
    });
  }

  // Setup is done: open Home. Play stays on Home; nothing starts the game here.
  function finishSetup() {
    GT4.go("#/home");
  }

  var active = null;

  function poll() {
    if (!active) {
      return;
    }
    call("app_state").then(function (appState) {
      if (!active) {
        return;
      }
      var setup = appState.setup || { state: "needed" };
      active.game = setup.game && setup.game.id ? setup.game.id : null;
      active.anyInstalled = !!appState.any_installed;
      var sig = signature(setup) + "|" + (active.game || "") + "|" + active.anyInstalled;
      if (sig === active.last) {
        return;
      }
      var stateChanged = !active.lastState || active.lastState !== setup.state || active.lastStep !== setup.step;
      active.last = sig;
      active.lastState = setup.state;
      active.lastStep = setup.step;
      var focusedText = document.activeElement && active.root.contains(document.activeElement) ? document.activeElement.textContent : null;
      build(setup, active.parts);
      var buttons = Array.prototype.slice.call(active.parts.foot.querySelectorAll("button:not([disabled])"));
      var again = buttons.filter(function (b) { return b.textContent === focusedText; })[0];
      var primary = active.parts.foot.querySelector("button.primary:not([disabled])") || buttons[0];
      if (again && !stateChanged) {
        again.focus({ preventScroll: true });
      } else if (primary && (stateChanged || focusedText)) {
        primary.focus({ preventScroll: true });
      }
    }, function (err) {
      GT4.ui.toast(GT4.errorText(err), true);
    });
  }

  GT4.pages = GT4.pages || {};
  GT4.pages.setup = {
    mount: function (root) {
      var cover = h("img", { class: "cover", src: "assets/cover-front.png", alt: "Gran Turismo 4 cover" });
      var title = h("h1", { text: "" });
      var sub = h("p", { class: "sub", text: "" });
      var steps = h("ul", { class: "steps" });
      var problem = h("p", { class: "problem", role: "alert", hidden: true });
      var notice = h("p", { class: "notice", hidden: true });
      var foot = h("div", { class: "foot" });
      var parts = { title: title, sub: sub, steps: steps, problem: problem, notice: notice, foot: foot };
      var page = h("div", { class: "stage setup" }, [
        h("div", { class: "cover-col" }, [cover]),
        h("div", { class: "setup-main" }, [
          h("img", { class: "wordmark", src: "assets/logo-gt4-white.png", alt: "Gran Turismo 4" }),
          title,
          sub,
          steps,
          problem,
          notice,
          foot
        ])
      ]);
      root.appendChild(page);
      active = { root: page, parts: parts, last: null, lastState: null, lastStep: null, game: null, anyInstalled: false };
      poll();
      var timer = setInterval(poll, POLL_MS);
      return {
        destroy: function () {
          active = null;
          clearInterval(timer);
        }
      };
    }
  };
})(window);
