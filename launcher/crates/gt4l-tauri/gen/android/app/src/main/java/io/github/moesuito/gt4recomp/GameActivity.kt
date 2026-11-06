// SPDX-License-Identifier: MIT
// TASK-180 (docs/ANDROID-PORT-PLAN.md section 4): the game. SDL3's SDLActivity creates the surface, forwards input, audio
// and the lifecycle, and calls SDL_main in libgt4.so (native/android_main.cpp) on its own thread. It runs in its own
// process (":game", AndroidManifest.xml), started only by the launcher's Play (GameBridgePlugin.startGame) with the
// launch contract:
// - EXTRA_ENVIRONMENT: NAME=VALUE entries, set in the process environment before SDL_main runs (game.ini values, paths);
// - EXTRA_ARGUMENTS: the command line. A content:// argument (the disc image, a document the app holds a permission to)
//   is opened here and passed as fd:<descriptor>; the game reads the image through it.
// The process ends with the activity, so every start gets fresh native state.
package io.github.moesuito.gt4recomp

import android.app.AlertDialog
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.hardware.input.InputManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.Process
import android.util.Log
import android.util.TypedValue
import android.view.Gravity
import android.view.InputDevice
import android.view.View
import android.widget.FrameLayout
import android.widget.TextView
import android.window.OnBackInvokedDispatcher
import java.io.File
import org.libsdl.app.SDLActivity

class GameActivity : SDLActivity() {
    companion object {
        const val EXTRA_ARGUMENTS = "io.github.moesuito.gt4recomp.ARGUMENTS"
        const val EXTRA_ENVIRONMENT = "io.github.moesuito.gt4recomp.ENVIRONMENT"
        // The pause dialog's texts in the launcher's language: title, keep playing, minimize, close game.
        const val EXTRA_TEXTS = "io.github.moesuito.gt4recomp.TEXTS"
        // Set by the launcher's Resume: the activity comes back and the game goes on.
        const val EXTRA_RESUME = "io.github.moesuito.gt4recomp.RESUME"
        // TASK-180 test5: which game library this session loads (gt4 = v2.00, spec2 = Spec II).
        const val EXTRA_GAME = "io.github.moesuito.gt4recomp.GAME"
        // "<pid> <running|paused> <epoch ms>", rewritten every 2 s (GameBridgePlugin.gameState reads it).
        const val HEARTBEAT_FILE = "game-heartbeat"
        private const val TAG = "GT4Recomp-game"

        // native/window_sdl.cpp: 1 starts a performance interval (F7), 2 writes its report (F8) to the game log.
        @JvmStatic
        external fun nativeReportRequest(request: Int)

        // native/window_sdl.cpp: true parks the game (EE stopped, card files written, audio paused), false lets it go on.
        @JvmStatic
        external fun nativeSetPaused(paused: Boolean)

        // native/window_sdl.cpp: the activity leaves the foreground; waits until the game thread has let go of the surface.
        @JvmStatic
        external fun nativeEnterBackground(timeoutMs: Int): Boolean

        @JvmStatic
        external fun nativeLeaveBackground()

        // native/window_sdl.cpp (TASK-180 test3): a pad that Android reports as keys and motion but that SDL does not
        // enumerate as a gamepad (some USB controllers). The game reads these only while SDL has no gamepad open.
        @JvmStatic
        external fun nativePadKey(keycode: Int, down: Boolean)

        @JvmStatic
        external fun nativePadMotion(axis: Int, value: Float)

        // TASK-185: the virtual pad. Load parses the player's layout text (null when there is no file) and the APK's
        // default and returns the lines to log; Geometry and Binds are what the overlay draws; Touch is one element and
        // its offsets; Active says whether the pad is shown at all (hidden means no input, never a stuck button).
        @JvmStatic
        external fun nativeVirtualPadLoad(player: String?, fallback: String): String

        @JvmStatic
        external fun nativeVirtualPadGeometry(): FloatArray?

        @JvmStatic
        external fun nativeVirtualPadFromPlayer(): Boolean

        @JvmStatic
        external fun nativeVirtualPadBinds(): Array<String>?

        @JvmStatic
        external fun nativeVirtualPadTouch(element: Int, dx: Float, dy: Float, down: Boolean)

        @JvmStatic
        external fun nativeVirtualPadActive(active: Boolean, reason: String)

        // The Android key codes of a gamepad's buttons (KeyEvent.KEYCODE_*): the ones own_pad_event forwards. A pad key
        // event that SDL does understand reaches SDL too; the native side ignores these while SDL has a gamepad open.
        private val PAD_KEYS = intArrayOf(
            96, 97, 99, 100,      // BUTTON_A, B, X, Y
            102, 103, 104, 105,   // BUTTON_L1, R1, L2, R2
            106, 107,             // BUTTON_THUMBL, THUMBR
            108, 109,             // BUTTON_START, SELECT
            19, 20, 21, 22,       // DPAD_UP, DOWN, LEFT, RIGHT
        )
        // The MotionEvent axes of a gamepad (MotionEvent.AXIS_*): sticks, hat and analog triggers.
        private val PAD_AXES = intArrayOf(0, 1, 2, 11, 14, 15, 17, 18, 22, 23)
    }

    private var recording = false
    private var pauseDialog: AlertDialog? = null
    private var paused = false
    private val heartbeat = Handler(Looper.getMainLooper())
    private val beat = object : Runnable {
        override fun run() {
            try {
                File(filesDir, HEARTBEAT_FILE).writeText("${Process.myPid()} ${if (paused) "paused" else "running"} ${System.currentTimeMillis()}")
            } catch (error: Exception) {
                Log.w(TAG, "heartbeat not written: $error")
            }
            heartbeat.postDelayed(this, 2000)
        }
    }

    // Load order: SDL3, then the game (the C++ runtime is linked into it); the last one holds SDL_main.
    // TASK-180 test5: one library per game, chosen by the launcher (Extra GAME): libgt4.so (v2.00) or libgt4spec2.so
    // (Spec II, SCUS-97436). Both are packaged; the Activity loads only its own, so the memory of the other is never mapped.
    override fun getLibraries(): Array<String> {
        val game = intent?.getStringExtra(EXTRA_GAME) ?: "gt4"
        val library = if (game == "spec2") "gt4spec2" else "gt4"
        Log.i(TAG, "game library: lib$library.so (game $game)")
        return arrayOf("SDL3", library)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // TASK-185: the overlay is added before the F7 button, so the button stays on top of its corner in the view order
        // (and the layout keeps that corner free anyway).
        createVirtualPad()
        val environment = intent.getStringArrayExtra(EXTRA_ENVIRONMENT) ?: arrayOf()
        if (environment.contains("GT4_PERF=1")) addReportButton()
        // TASK-180 test2 (owner): Back pauses the game and asks what to do. Android 13+ (and every app targeting 36)
        // delivers Back through the dispatcher; older versions call onBackPressed.
        if (Build.VERSION.SDK_INT >= 33) {
            onBackInvokedDispatcher.registerOnBackInvokedCallback(OnBackInvokedDispatcher.PRIORITY_DEFAULT) { onBack() }
        }
        // TASK-185: a controller plugged in, unplugged or changed while the game runs shows or hides the virtual pad.
        getSystemService(InputManager::class.java)?.registerInputDeviceListener(controllerListener, heartbeat)
        heartbeat.post(beat)
    }

    @Deprecated("Android 12 and older")
    override fun onBackPressed() {
        onBack()
    }

    private fun texts(): Array<String> {
        val given = intent.getStringArrayExtra(EXTRA_TEXTS) ?: arrayOf()
        return if (given.size >= 4) given else arrayOf("Game paused", "Keep playing", "Minimize", "Close game")
    }

    private fun setPaused(value: Boolean, why: String) {
        if (paused == value) return
        paused = value
        nativeSetPaused(value)
        Log.i(TAG, (if (value) "paused: " else "resumed: ") + why)
    }

    private fun onBack() {
        if (pauseDialog?.isShowing == true) {
            pauseDialog?.cancel()
            return
        }
        showPauseDialog()
    }

    private fun showPauseDialog() {
        setPaused(true, "Back")
        val (title, keep, minimize, close) = texts()
        val dialog = AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
            .setTitle(title)
            .setPositiveButton(keep) { _, _ -> setPaused(false, "keep playing") }
            .setNeutralButton(minimize) { _, _ -> minimize() }
            .setNegativeButton(close) { _, _ -> closeGame() }
            .setOnCancelListener { setPaused(false, "Back again") }
            .create()
        dialog.setOnDismissListener { pauseDialog = null }
        pauseDialog = dialog
        dialog.show()
        // TASK-180 test3 (owner): centred on the screen (it sat at the bottom) and driven by the pad. The three buttons are
        // focusable and Keep playing takes the focus, so A keeps the game and the d-pad/stick move between the buttons.
        dialog.window?.let { window ->
            window.setGravity(Gravity.CENTER)
            val attributes = window.attributes
            attributes.width = android.view.ViewGroup.LayoutParams.WRAP_CONTENT
            attributes.height = android.view.ViewGroup.LayoutParams.WRAP_CONTENT
            window.attributes = attributes
        }
        for (which in listOf(AlertDialog.BUTTON_POSITIVE, AlertDialog.BUTTON_NEUTRAL, AlertDialog.BUTTON_NEGATIVE)) {
            dialog.getButton(which)?.apply {
                isFocusable = true
                isFocusableInTouchMode = true
            }
        }
        dialog.getButton(AlertDialog.BUTTON_POSITIVE)?.requestFocus()
    }

    // Minimize: the game stays alive and paused; the launcher comes back (its Play button reads Resume).
    // Minimize: the game stays alive and paused in the launcher's task, with the launcher brought back over it (TASK-180
    // test3: no separate task, no animation). The launcher's Resume brings the game's own instance back to the front.
    private fun minimize() {
        Log.i(TAG, "minimize: the launcher comes to the front, the game stays paused below it")
        startActivity(
            Intent(this, MainActivity::class.java)
                .addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT or Intent.FLAG_ACTIVITY_NO_ANIMATION)
        )
        @Suppress("DEPRECATION")
        overridePendingTransition(0, 0)
    }

    // Close: the normal end (SDL asks the game to quit; it writes its cards and ends the process).
    private fun closeGame() {
        Log.i(TAG, "close: the game ends")
        setPaused(false, "closing")
        finish()
        @Suppress("DEPRECATION")
        overridePendingTransition(0, 0)
    }

    // The same pause path for Home, the app switcher, the notification shade, Settings and screen off (A6): the game
    // parks and lets go of its surface before the window is destroyed (owner's test1: black screen after Settings).
    override fun onPause() {
        paused = true
        // TASK-185: a pad left pressed while the activity goes away would stay pressed in the game.
        virtualPad?.releaseAll()
        if (libraryLoaded) {
            val released = nativeEnterBackground(1500)
            Log.i(TAG, "paused: the activity left the foreground (surface ${if (released) "released" else "NOT released in 1.5 s"})")
        }
        super.onPause()
    }

    override fun onResume() {
        super.onResume()
        // TASK-185: the game libraries are loaded by now (onCreate may run before SDLActivity has them), so this is where
        // the layout is read when it was not ready yet. The text is read again at every start, never cached between
        // sessions: the player's file may have changed in the launcher.
        loadVirtualPad()
        refreshVirtualPadVisibility()
        if (libraryLoaded) {
            nativeLeaveBackground()
            if (pauseDialog?.isShowing != true) setPaused(false, "the activity is back in the foreground")
            else Log.i(TAG, "back in the foreground; the pause dialog stays")
        }
    }

    // The native methods exist once SDLActivity has loaded the libraries (onCreate).
    private val libraryLoaded: Boolean
        get() = SDLActivity.mBrokenLibraries == false

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        // The launch contract stays the first intent's; a Resume only brings the activity back.
        if (intent.getBooleanExtra(EXTRA_RESUME, false)) Log.i(TAG, "resume from the launcher")
    }

    // TASK-185 (owner, 2026-11-06): the on-screen virtual pad, for a phone with no controller. The layout is the file the
    // launcher's editor writes (`<user>/virtual-pad.json`: the same user folder as the launcher's settings) or the
    // built-in default; this activity draws it, forwards touches and decides when it shows:
    //   auto   only while no physical controller is connected (the default);
    //   always even with one (the virtual buttons combine with the pad's);
    //   off    never.
    // The contract with the editor and the native side is TASK-185's layout file (android/virtual-pad/).
    private var virtualPad: VirtualPadView? = null
    private var virtualPadLoaded = false
    private var virtualPadShown: Boolean? = null
    private var virtualPadMode = "auto"
    // TASK-185 evidence on an emulator, which attaches no controller: GT4_VIRTUAL_PAD_TEST_CONTROLLER=<name>, set through
    // debug.env, reports a controller plugged in eight seconds into the session, through the same refresh the Android
    // device listener calls. It is never in a player's launch plan; with it unset nothing changes.
    private val injectedControllers = ArrayList<String>()
    private var injectedControllerPosted = false

    // The pad's visibility follows Android's input devices, hot-plug included.
    private val controllerListener = object : InputManager.InputDeviceListener {
        override fun onInputDeviceAdded(deviceId: Int) = refreshVirtualPadVisibility()

        override fun onInputDeviceRemoved(deviceId: Int) = refreshVirtualPadVisibility()

        override fun onInputDeviceChanged(deviceId: Int) = refreshVirtualPadVisibility()
    }

    private fun createVirtualPad() {
        if (virtualPad != null) return
        virtualPadMode = when (val mode = launchValue("GT4_VIRTUAL_PAD")) {
            "always" -> "always"
            "off" -> "off"
            else -> if (mode == null || mode == "auto") "auto" else {
                Log.w(TAG, "virtual pad: unknown GT4_VIRTUAL_PAD value '$mode', using auto")
                "auto"
            }
        }
        if (virtualPadMode == "off") {
            Log.i(TAG, "virtual pad off: GT4_VIRTUAL_PAD=off")
            return
        }
        Log.i(TAG, "virtual pad mode: $virtualPadMode (GT4_VIRTUAL_PAD=${launchValue("GT4_VIRTUAL_PAD") ?: "unset"})")
        val view = VirtualPadView(this)
        view.visibility = View.GONE
        view.onTouchElement = { element, dx, dy, down ->
            if (libraryLoaded) nativeVirtualPadTouch(element, dx, dy, down)
        }
        addContentView(view, FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT,
            FrameLayout.LayoutParams.MATCH_PARENT,
        ))
        virtualPad = view
        loadVirtualPad()
    }

    // Reads the layout, once per process (one game start): the player's file, else the APK's default. Both texts are
    // parsed by the native pad, which also logs what it made of them (an unknown binds, a clamped coordinate, a broken
    // file that falls back to the default).
    private fun loadVirtualPad() {
        val view = virtualPad ?: return
        if (virtualPadLoaded || !libraryLoaded) return
        val fallback = readAsset("default-layout.json")
        if (fallback == null) {
            Log.w(TAG, "virtual pad: the APK has no default layout, the pad stays off")
            return
        }
        val player = readPlayerLayout()
        val report = nativeVirtualPadLoad(player?.first, fallback)
        for (line in report.split('\n')) {
            if (line.isNotBlank()) Log.i(TAG, "virtual pad layout: $line")
        }
        val geometry = nativeVirtualPadGeometry()
        if (geometry == null || geometry.size < 3) {
            Log.w(TAG, "virtual pad: the native pad returned no geometry, the pad stays off")
            return
        }
        view.setLayout(geometry, nativeVirtualPadBinds() ?: arrayOf())
        virtualPadLoaded = true
        val source = when {
            !nativeVirtualPadFromPlayer() -> "the built-in default"
            player != null -> player.second
            else -> "the player layout"
        }
        Log.i(TAG, "virtual pad: ${geometry[0].toInt()} elements, layout from $source")
        injectTestController()
        refreshVirtualPadVisibility()
    }

    // The evidence hook of the class comment: one injected device event, eight seconds in, so a session on an emulator
    // shows the `auto` rule hiding the pad exactly as a Bluetooth pad being switched on would.
    private fun injectTestController() {
        if (injectedControllerPosted) return
        val name = launchValue("GT4_VIRTUAL_PAD_TEST_CONTROLLER") ?: return
        injectedControllerPosted = true
        heartbeat.postDelayed({
            Log.i(TAG, "virtual pad: test hook, reporting '$name' as a controller that was just connected")
            injectedControllers.add(name)
            refreshVirtualPadVisibility()
        }, 8000)
    }

    // The player's file: the launcher's user folder first (the one that holds config/settings.toml), then the app's
    // external files folder as the contract names it, so a layout dropped there by hand is found too.
    private fun readPlayerLayout(): Pair<String, String>? {
        val candidates = listOfNotNull(
            File(filesDir.parentFile, "user/virtual-pad.json"),
            getExternalFilesDir(null)?.let { File(it, "user/virtual-pad.json") },
        )
        for (file in candidates) {
            if (!file.isFile) continue
            try {
                Log.i(TAG, "virtual pad: player layout ${file.absolutePath} (${file.length()} bytes)")
                return file.readText() to file.absolutePath
            } catch (error: Exception) {
                Log.w(TAG, "virtual pad: cannot read ${file.absolutePath}: $error")
            }
        }
        Log.i(TAG, "virtual pad: no player layout at ${candidates.joinToString(" or ") { it.absolutePath }}")
        return null
    }

    private fun readAsset(name: String): String? = try {
        assets.open(name).use { stream -> stream.readBytes().toString(Charsets.UTF_8) }
    } catch (error: Exception) {
        Log.w(TAG, "virtual pad: cannot read the asset $name: $error")
        null
    }

    // Shows or hides the overlay and tells the native pad, which stops contributing input while it is hidden. Every
    // change is logged with its reason ("virtual pad shown: no controller").
    private fun refreshVirtualPadVisibility() {
        val view = virtualPad ?: return
        val controllers = connectedControllers()
        val wanted = virtualPadMode == "always" || controllers.isEmpty()
        val description = when {
            virtualPadMode == "always" -> "always (GT4_VIRTUAL_PAD=always)"
            wanted -> "no controller"
            else -> "${controllers.first()} connected"
        }
        val line = (if (wanted) "shown: " else "hidden: ") + description
        val changed = virtualPadShown != wanted
        if (changed) {
            virtualPadShown = wanted
            Log.i(TAG, "virtual pad $line")
        }
        // A layout that failed to load draws nothing, so the overlay never takes touches for nothing.
        view.visibility = if (wanted && virtualPadLoaded) View.VISIBLE else View.GONE
        if (!wanted) view.releaseAll()
        if (libraryLoaded) nativeVirtualPadActive(wanted, if (changed) line else "")
    }

    // Every input device Android reports as a controller: a gamepad or a joystick, Bluetooth, USB or built in. The
    // emulator's touchscreen and keyboards offer neither source, so `auto` shows the pad on a phone with none attached.
    private fun connectedControllers(): List<String> {
        val manager = getSystemService(InputManager::class.java) ?: return injectedControllers
        val names = ArrayList<String>(injectedControllers)
        for (id in manager.inputDeviceIds) {
            val device = InputDevice.getDevice(id) ?: continue
            val controller = device.supportsSource(InputDevice.SOURCE_GAMEPAD) ||
                device.supportsSource(InputDevice.SOURCE_JOYSTICK)
            if (controller) names.add(device.name)
        }
        return names
    }

    // A NAME=VALUE entry of the launch contract (the game.ini values the launcher passes), or null.
    private fun launchValue(name: String): String? {
        val prefix = "$name="
        return intent.getStringArrayExtra(EXTRA_ENVIRONMENT)
            ?.firstOrNull { it.startsWith(prefix) }
            ?.substring(prefix.length)
    }

    // TASK-180 (owner, 2026-11-05): the F7/F8 performance report without a keyboard. Only while the launcher's
    // performance report setting is on (GT4_PERF=1). A small, faint button in the TOP-LEFT corner (owner, 2026-11-05
    // 17:53: the middle of the left edge sat right over his phone's front camera), kept clear of the display cutout and
    // the system bars by the window's insets: a tap starts an interval, the next writes the report. It never takes
    // focus, so the controller keeps all its input.
    private fun addReportButton() {
        val size = TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, 44f, resources.displayMetrics).toInt()
        val margin = TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, 8f, resources.displayMetrics).toInt()
        val button = TextView(this)
        button.gravity = Gravity.CENTER
        button.setTextColor(Color.WHITE)
        button.setTextSize(TypedValue.COMPLEX_UNIT_DIP, 11f)
        button.isFocusable = false
        button.isFocusableInTouchMode = false
        button.contentDescription = "Performance report"
        fun paint() {
            val shape = GradientDrawable()
            shape.shape = GradientDrawable.OVAL
            shape.setColor(if (recording) Color.rgb(200, 30, 30) else Color.rgb(60, 60, 60))
            shape.setStroke(2, Color.WHITE)
            button.background = shape
            button.text = if (recording) "F8" else "F7"
            button.alpha = if (recording) 0.7f else 0.35f
        }
        paint()
        button.setOnClickListener {
            recording = !recording
            nativeReportRequest(if (recording) 1 else 2)
            Log.i(TAG, if (recording) "touch button: interval started (F7)" else "touch button: report written (F8)")
            paint()
        }
        val layout = FrameLayout.LayoutParams(size, size, Gravity.START or Gravity.TOP)
        layout.leftMargin = margin
        layout.topMargin = margin
        addContentView(button, layout)
        button.visibility = View.VISIBLE
        // The top-left corner is under the display cutout on phones whose front camera sits there, and under the status
        // bar in many orientations. The window's insets tell both; the button adds them to its own margin, on every
        // change and once the view is attached. The insets are passed on untouched, so SDL and the game see them too.
        val keepClear = { insets: android.view.WindowInsets ->
            val left = if (Build.VERSION.SDK_INT >= 30) {
                maxOf(
                    insets.getInsets(android.view.WindowInsets.Type.displayCutout()).left,
                    insets.getInsets(android.view.WindowInsets.Type.systemBars()).left,
                )
            } else {
                @Suppress("DEPRECATION")
                maxOf(insets.systemWindowInsetLeft, insets.displayCutout?.safeInsetLeft ?: 0)
            }
            val top = if (Build.VERSION.SDK_INT >= 30) {
                maxOf(
                    insets.getInsets(android.view.WindowInsets.Type.displayCutout()).top,
                    insets.getInsets(android.view.WindowInsets.Type.systemBars()).top,
                )
            } else {
                @Suppress("DEPRECATION")
                maxOf(insets.systemWindowInsetTop, insets.displayCutout?.safeInsetTop ?: 0)
            }
            layout.leftMargin = margin + left
            layout.topMargin = margin + top
            button.layoutParams = layout
            Log.i(TAG, "report button: top-left corner, clear of $left x $top of insets")
        }
        button.setOnApplyWindowInsetsListener { _, insets ->
            keepClear(insets)
            insets
        }
        button.requestApplyInsets()
        button.post { window.decorView.rootWindowInsets?.let(keepClear) }
    }

    // TASK-180 test2: the performance HUD's glyphs (native/hw_gs/hud/hw_hud_system_posix.cpp calls this through JNI).
    // The 95 printable ASCII glyphs of the system's monospace font, side by side in one row of cells:
    // {cell width, cell height, coverage 0-255 row-major over the whole strip}.
    @Suppress("unused")
    fun glyphAtlas(pixelHeight: Int): IntArray {
        val paint = Paint(Paint.ANTI_ALIAS_FLAG)
        paint.typeface = Typeface.MONOSPACE
        paint.textSize = pixelHeight.toFloat()
        paint.color = Color.WHITE
        val metrics = paint.fontMetricsInt
        val cellHeight = maxOf(1, metrics.descent - metrics.ascent)
        val cellWidth = maxOf(1, kotlin.math.ceil(paint.measureText("M").toDouble()).toInt())
        val glyphs = 126 - 32 + 1
        val bitmap = Bitmap.createBitmap(cellWidth * glyphs, cellHeight, Bitmap.Config.ARGB_8888)
        val canvas = Canvas(bitmap)
        for (index in 0 until glyphs) {
            canvas.drawText((32 + index).toChar().toString(), (index * cellWidth).toFloat(), (-metrics.ascent).toFloat(), paint)
        }
        val pixels = IntArray(bitmap.width * bitmap.height)
        bitmap.getPixels(pixels, 0, bitmap.width, 0, 0, bitmap.width, bitmap.height)
        bitmap.recycle()
        val out = IntArray(2 + pixels.size)
        out[0] = cellWidth
        out[1] = cellHeight
        for (i in pixels.indices) out[2 + i] = (pixels[i] ushr 24) and 0xFF
        return out
    }

    // The display's current refresh rate (phones change it on the fly), for the HUD.
    @Suppress("unused")
    fun displayRefreshHz(): Float {
        @Suppress("DEPRECATION")
        val current = (if (android.os.Build.VERSION.SDK_INT >= 30) display else null) ?: windowManager.defaultDisplay
        return current?.refreshRate ?: 0f
    }

    // TASK-180 test3 (owner's phone): the controller must work in the game. SDL enumerates most pads, but a USB pad
    // Android reports only as keys and motion never reaches the game at all, because SDL builds no gamepad for it. Both
    // dispatchers below forward the pad's events to the game (nativePadKey/nativePadMotion) and then let SDL have them
    // as well; the game reads the forwarded state only while SDL has no gamepad open, so nothing is ever read twice.
    override fun dispatchKeyEvent(event: android.view.KeyEvent): Boolean {
        forwardPadKey(event)
        return super.dispatchKeyEvent(event)
    }

    private fun forwardPadKey(event: android.view.KeyEvent) {
        if (!libraryLoaded || !PAD_KEYS.contains(event.keyCode)) return
        val sources = event.device?.sources ?: 0
        val pad = (sources and android.view.InputDevice.SOURCE_JOYSTICK) != 0 ||
            (sources and android.view.InputDevice.SOURCE_GAMEPAD) != 0 ||
            (sources and android.view.InputDevice.SOURCE_DPAD) != 0
        if (!pad) return  // a keyboard key that happens to share a pad code (Enter is not in the list)
        when (event.action) {
            android.view.KeyEvent.ACTION_DOWN -> nativePadKey(event.keyCode, true)
            android.view.KeyEvent.ACTION_UP -> nativePadKey(event.keyCode, false)
        }
    }

    override fun dispatchGenericMotionEvent(event: android.view.MotionEvent): Boolean {
        if (libraryLoaded && event.isFromSource(android.view.InputDevice.SOURCE_JOYSTICK)) {
            // Every standard gamepad axis; a pad without one reports nothing for it.
            for (axis in PAD_AXES) nativePadMotion(axis, event.getAxisValue(axis))
        }
        return super.dispatchGenericMotionEvent(event)
    }

    // Called on SDL's thread, after the libraries are loaded and before SDL_main.
    override fun getArguments(): Array<String> {
        // The game thread parks itself while paused (window_sdl.cpp park()); SDL must not block it in its event pump.
        SDLActivity.nativeSetenv("SDL_ANDROID_BLOCK_ON_PAUSE", "0")
        for (entry in intent.getStringArrayExtra(EXTRA_ENVIRONMENT) ?: arrayOf()) {
            val equals = entry.indexOf('=')
            if (equals > 0) SDLActivity.nativeSetenv(entry.substring(0, equals), entry.substring(equals + 1))
        }
        Log.i(TAG, "launch contract: ${intent.getStringArrayExtra(EXTRA_ARGUMENTS)?.size ?: 0} arguments, " +
            "${intent.getStringArrayExtra(EXTRA_ENVIRONMENT)?.size ?: 0} environment entries")
        return (intent.getStringArrayExtra(EXTRA_ARGUMENTS) ?: arrayOf()).map { argument ->
            if (argument.startsWith("content://")) descriptorArgument(argument) else argument
        }.toTypedArray()
    }

    private fun descriptorArgument(uri: String): String {
        return try {
            // Detached and closed at once: no stable provider reference stays (see GameBridgePlugin.openFd).
            "fd:" + (contentResolver.openFileDescriptor(Uri.parse(uri), "r")?.use { it.detachFd() } ?: -1)
        } catch (error: Exception) {
            Log.e(TAG, "cannot open $uri: $error")
            "fd:-1"
        }
    }

    override fun onDestroy() {
        heartbeat.removeCallbacks(beat)
        getSystemService(InputManager::class.java)?.unregisterInputDeviceListener(controllerListener)
        File(filesDir, HEARTBEAT_FILE).delete()
        Log.i(TAG, "game activity closed; the game process ends")
        super.onDestroy()
        // Fresh native state at every start: the game's globals live as long as this process.
        Process.killProcess(Process.myPid())
    }
}
