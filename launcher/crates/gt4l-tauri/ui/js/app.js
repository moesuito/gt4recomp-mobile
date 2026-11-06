/*
 * Router and boot. Routes (location.hash):
 *   #/setup                  first run, or a game being installed (also shown until a game is set up)
 *   #/home                   Play and Settings
 *   #/settings/<tab>         graphics | controls | saves | replays | about (mods only when GT4.features.mods;
 *                            Display and Advanced are sections of Graphics, so #/settings/display and
 *                            #/settings/advanced fall back to graphics)
 * Each page mounts into #app and sets GT4.hooks for the input layer.
 */
(function (root) {
  "use strict";
  var GT4 = (root.GT4 = root.GT4 || {});
  var app = document.getElementById("app");
  var current = null;

  function parseRoute() {
    var raw = root.location.hash.replace(/^#\/?/, "");
    var parts = raw.split("/");
    return { name: parts[0] || "", tab: parts[1] || "" };
  }

  function go(hash) {
    if (root.location.hash === hash) {
      render();
    } else {
      root.location.hash = hash;
    }
  }
  GT4.go = go;

  function render() {
    if (current && current.destroy) {
      current.destroy();
    }
    current = null;
    GT4.ui.clear(app);
    GT4.hooks = {};
    GT4.ui.setHints([]);
    GT4.capturing = false;

    var route = parseRoute();
    var name = route.name === "setup" || route.name === "settings" ? route.name : "home";
    document.body.classList.toggle("is-home", name === "home");

    var page = GT4.pages[name];
    if (name === "settings") {
      current = page.mount(app, { tab: route.tab || "graphics" });
    } else {
      current = page.mount(app);
    }
    if (name === "home") {
      // TASK-114 rc5: once the home screen is up, the Settings data is read in the background (once per session), so
      // the Settings tabs open from memory.
      root.setTimeout(function () {
        if (GT4.prefetchSettings) {
          GT4.prefetchSettings();
        }
      }, 0);
    }
  }

  root.addEventListener("hashchange", render);

  // Events from Rust (see ui/dev/README.md): update offers at launch, close confirmation, install progress,
  // and the end of a game session (the Home page shows the message).
  GT4.backend.listen("update-available", function (payload) {
    GT4.updates.offer(payload && payload.offers);
  });
  GT4.backend.listen("close-confirm", function (payload) {
    GT4.updates.confirmClose(payload && payload.message);
  });
  GT4.backend.listen("update-finished", function (payload) {
    GT4.ui.toast(payload && payload.message ? payload.message : GT4.t("app.update_finished"), !(payload && payload.ok));
  });
  // IN-507: Play found shaders to compile. The window stays until Play goes on (GT4.closeShaderWindow, js/pages/home.js).
  // The payload is {done, total, attempt}; `attempt` above 1 is a retry after the game program stopped (TASK-170b).
  var shaderWindow = null;
  var shaderBar = null;
  var shaderCount = null;
  var shaderRetry = null;
  GT4.closeShaderWindow = function () {
    if (shaderWindow) {
      shaderWindow.close();
      shaderWindow = null;
    }
  };
  GT4.backend.listen("shaders-progress", function (payload) {
    var done = (payload && payload.done) || 0;
    var total = (payload && payload.total) || 0;
    if (!shaderWindow) {
      shaderBar = GT4.ui.h("div", { class: "shader-bar-fill" });
      shaderCount = GT4.ui.h("p", { class: "shader-count" });
      shaderRetry = GT4.ui.h("p", { class: "shader-retry" });
      shaderWindow = GT4.ui.openDialog({
        title: GT4.t("shaders.title"),
        text: GT4.t("shaders.text"),
        body: GT4.ui.h("div", {}, [GT4.ui.h("div", { class: "shader-bar" }, [shaderBar]), shaderCount, shaderRetry]),
        dismissable: false,
        // TASK-180 test3 (owner): nothing focused. The only button is Cancel, and one A press used to cancel the prepare.
        focus: "none",
        buttons: [{
          label: GT4.t("common.cancel"),
          closes: false,
          run: function () {
            GT4.backend.call("cancel_shader_prepare").then(null, function () {});
          }
        }]
      });
    }
    shaderBar.style.width = (total > 0 ? Math.round(done * 100 / total) : 0) + "%";
    shaderCount.textContent = GT4.t("shaders.count", { done: done, total: total });
    // TASK-170b: the game program stopped inside a compile and the launcher runs it again; attempt 1 is the plain first run.
    var attempt = (payload && payload.attempt) || 1;
    shaderRetry.textContent = attempt > 1 ? GT4.t("shaders.retry", { attempt: attempt }) : "";
  });
  GT4.backend.listen("renderer-progress", function (payload) {
    // The setup page draws its own renderer row; these toasts are for the retry at a later launch and updates.
    if (parseRoute().name === "setup") {
      return;
    }
    var step = payload && payload.step;
    // TASK-089 extension (1.2.0): a switch in Settings shows its progress inline, under the Renderer row.
    var labels = { listing: GT4.t("app.gsdx_listing"), downloading: GT4.t("app.gsdx_downloading"), installing: GT4.t("app.gsdx_installing") };
    if (GT4.setRendererStatus && labels[step]) {
      GT4.setRendererStatus(labels[step]);
    }
    if (step === "failed") {
      GT4.ui.toast(GT4.t("app.gsdx_failed"), true);
    } else if (step === "done") {
      GT4.ui.toast(GT4.t("app.gsdx_ready"));
    }
  });
  GT4.backend.listen("renderer-switched", function (payload) {
    // Choosing GSdx in Settings finished: the notice, and the page shows the saved choice again.
    if (GT4.setRendererStatus) {
      GT4.setRendererStatus("");
    }
    GT4.ui.toast(payload && payload.message ? payload.message : GT4.t("app.gsdx_name"), !(payload && payload.ok));
    if (parseRoute().name === "settings") {
      GT4.go(root.location.hash);
    }
  });
  // Export logs (TASK-111, rc4): one zip in a folder the player picks; then its path and two next steps.
  GT4.exportLogs = function () {
    return GT4.backend.call("export_logs", {}).then(function (path) {
      if (!path) {
        return;
      }
      GT4.ui.openDialog({
        title: GT4.t("about.logs_exported_title"),
        text: GT4.t("about.logs_exported_text", { path: path }),
        buttons: [
          { label: GT4.t("common.cancel") },
          {
            label: GT4.t("about.report_problem"),
            run: function () {
              GT4.backend.call("open_link", { name: "report_problem" }).catch(function (err) {
                GT4.ui.toast(GT4.errorText(err), true);
              });
            }
          },
          {
            label: GT4.t("about.open_export_folder"),
            primary: true,
            run: function () {
              GT4.backend.call("open_export_folder", { path: path }).catch(function (err) {
                GT4.ui.toast(GT4.errorText(err), true);
              });
            }
          }
        ].filter(function (button) {
          // TASK-180: on Android the zip went where the player saved it; there is no folder to open.
          return GT4.platform !== "android" || button.label !== GT4.t("about.open_export_folder");
        })
      });
    }, function (err) {
      GT4.ui.toast(GT4.errorText(err), true);
    });
  };
  // A crash or a lost graphics card: the player is offered the log bundle for a report.
  GT4.backend.listen("logs-offer", function () {
    GT4.ui.openDialog({
      title: GT4.t("logs.offer_title"),
      text: GT4.t("logs.offer_text"),
      buttons: [
        { label: GT4.t("common.cancel") },
        { label: GT4.t("about.export_logs"), primary: true, run: function () { GT4.exportLogs(); } }
      ]
    });
  });
  // TASK-118 (owner, 2026-10-26): the six background layers animate for 72 s, forever, and kept compositing while the
  // game ran: up to ~20% of a 2-core laptop (the game's own EE CPU went from 6-12 ms to ~19.6 ms). Pause every infinite
  // animation while the page is hidden, the window is unfocused or a game is running, and resume when the player is back.
  var gameIsRunning = false;
  var animationsIdle = null;
  function refreshAnimations() {
    var idle = document.hidden || !document.hasFocus() || gameIsRunning;
    if (idle === animationsIdle) return;
    animationsIdle = idle;
    document.body.classList.toggle("is-idle", idle);
  }
  document.addEventListener("visibilitychange", refreshAnimations);
  root.addEventListener("focus", refreshAnimations);
  root.addEventListener("blur", refreshAnimations);
  refreshAnimations();

  GT4.backend.listen("game-state", function (payload) {
    gameIsRunning = !!(payload && payload.running);
    refreshAnimations();
    if (payload && payload.message) {
      // TASK-108: a notice (a run on Direct3D 11 without Vulkan 1.2) is not an error.
      GT4.ui.toast(payload.message, !payload.notice);
    }
    if (payload && payload.running === false && root.location.hash !== "#/home") {
      GT4.go("#/home");
    }
  });

  // TASK-091 (1.2.1): the first start moves the files of GT4Recomp 1.2.0 to the new folders. While the game files
  // move (in the background), a small status line shows the progress; Play and setup wait. The result is a toast,
  // shown once per notice text.
  var migrationBox = null;
  var shownNotice = "";
  // The status box: a label with the percentage on the right and a bar under it. Without a byte count (a move
  // inside one drive) the bar slides and no percentage is shown.
  function renderMigration(box, migration) {
    var done = migration.bytes_done || 0;
    var total = migration.bytes_total || 0;
    var percent = total > 0 ? Math.min(100, Math.floor((done * 100) / total)) : -1;
    if (!box.firstChild) {
      var head = document.createElement("div");
      head.className = "migration-head";
      head.appendChild(document.createElement("span"));
      head.appendChild(document.createElement("span"));
      var bar = document.createElement("div");
      bar.className = "migration-bar";
      var fill = document.createElement("div");
      fill.className = "migration-fill";
      bar.appendChild(fill);
      box.appendChild(head);
      box.appendChild(bar);
    }
    var label = box.firstChild.firstChild;
    var number = box.firstChild.lastChild;
    var barNode = box.lastChild;
    label.textContent = GT4.t("app.migration_moving", { item: migration.item || GT4.t("app.migration_items") });
    number.textContent = percent >= 0 ? percent + "%" : "";
    barNode.classList.toggle("indeterminate", percent < 0);
    barNode.firstChild.style.width = percent >= 0 ? percent + "%" : "";
  }
  function showMigration(appState) {
    var migration = appState && appState.migration;
    if (!migration) {
      return;
    }
    if (migration.running) {
      if (!migrationBox) {
        migrationBox = document.createElement("div");
        migrationBox.className = "migration";
        migrationBox.setAttribute("role", "status");
        migrationBox.setAttribute("aria-live", "polite");
        document.body.appendChild(migrationBox);
      }
      renderMigration(migrationBox, migration);
      root.setTimeout(function () {
        GT4.backend.call("app_state").then(showMigration, function () {});
      }, 700);
      return;
    }
    if (migrationBox) {
      migrationBox.parentNode.removeChild(migrationBox);
      migrationBox = null;
    }
    if (migration.notice && migration.notice !== shownNotice) {
      shownNotice = migration.notice;
      GT4.ui.toast(migration.notice, !!migration.problem);
    }
  }
  GT4.showMigration = showMigration;

  // TASK-091 (1.2.1, WebView2 guard): the window stays hidden until the first screen has drawn. ui_ready is sent after
  // the next animation frame, or after a short timer when the window is still hidden (a hidden web view may not run
  // animation frames). The backend shows the window; a second call does nothing.
  var readySent = false;
  function signalReady() {
    function send() {
      if (readySent) {
        return;
      }
      readySent = true;
      GT4.backend.call("ui_ready").catch(function () {});
    }
    root.requestAnimationFrame(send);
    root.setTimeout(send, 250);
  }

  // TASK-102: the locale files load first (js/i18n.js); the language then comes from the Settings value that
  // app_state reports. A change of language (Settings, or the same value read again) re-draws the page.
  var booted = false;
  GT4.i18n.onChange(function () {
    if (booted) {
      render();
    }
  });
  GT4.i18n.ready.then(function () {
    return GT4.backend.call("app_state");
  }).then(function (appState) {
    GT4.i18n.setSetting(appState.language || "auto");
    // TASK-180: "windows" or "android" (the backend's platform rules; the page only adapts its wording).
    GT4.platform = appState.platform || "windows";
    document.body.classList.toggle("platform-android", GT4.platform === "android");
    // TASK-180 test3 (owner): racing wheels are not supported on Android; the Wheel page, its tab and its commands are
    // not offered there (the backend says so, so the page has no second rule to keep in step).
    if (appState.wheel === false) {
      GT4.features.wheel = false;
    }
    // TASK-185: the on-screen pad and its layout editor exist where the game draws them (Android); the backend says so.
    if (appState.virtual_pad === true) {
      GT4.features.virtualPad = true;
    }
    booted = true;
    showMigration(appState);
    var route = parseRoute();
    if (!route.name) {
      // Home as soon as one game is installed (the other game then offers Install there); Setup otherwise.
      go(appState.any_installed || (appState.setup && appState.setup.state === "done") ? "#/home" : "#/setup");
    } else {
      render();
    }
    signalReady();
  }, function (err) {
    booted = true;
    render();
    GT4.ui.toast(GT4.errorText(err), true);
    signalReady();
  });
})(window);
