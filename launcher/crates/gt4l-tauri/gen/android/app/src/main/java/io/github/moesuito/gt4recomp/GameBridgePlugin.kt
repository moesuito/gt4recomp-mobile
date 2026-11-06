// SPDX-License-Identifier: MIT
// TASK-180 (docs/ANDROID-PORT-PLAN.md section 4): what the launcher (Rust, src/android.rs) needs from Android itself.
// - pickDocument: the Storage Access Framework picker. The disc image keeps a persistable read permission (never copied);
//   an import is copied into a file the launcher names.
// - openFd: a read descriptor for a document the app holds a permission to (gt4l_disc reads the image through it).
// - saveDocument: "save as" for exports (saves, replays, the log bundle).
// - startGame: Play. The game activity (GameActivity, its own ":game" process) gets the launch contract: the same
//   arguments and environment the Windows launcher gives gt4vulkan.exe.
// - openUrl: links and the Android update download, in the browser.
// - deviceInfo: the phone's facts for system.txt in the log bundle (model, Android version, SoC, RAM, display).
// - gameExits: how the game's (and the launcher's) process ended (ApplicationExitInfo, Android 11+), with its tombstone or ANR trace, so a
//   crash is recorded for the log bundle even though the game died.
package io.github.moesuito.gt4recomp

import android.app.Activity
import android.app.ActivityManager
import android.app.ApplicationExitInfo
import android.content.ActivityNotFoundException
import android.content.Intent
import android.content.Context
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.PowerManager
import android.provider.DocumentsContract
import android.provider.OpenableColumns
import androidx.activity.result.ActivityResult
import app.tauri.annotation.ActivityCallback
import app.tauri.annotation.Command
import app.tauri.annotation.InvokeArg
import app.tauri.annotation.TauriPlugin
import app.tauri.plugin.Invoke
import app.tauri.plugin.JSArray
import app.tauri.plugin.JSObject
import app.tauri.plugin.Plugin
import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream

@InvokeArg
class PickArgs {
    var mimeTypes: Array<String> = arrayOf("*/*")
    var persist: Boolean = false
    var copyTo: String? = null
}

@InvokeArg
class UriArgs {
    var uri: String = ""
}

@InvokeArg
class SaveArgs {
    var source: String = ""
    var name: String = ""
    var mimeType: String = "application/octet-stream"
}

@InvokeArg
class GameArgs {
    var arguments: Array<String> = arrayOf()
    var environment: Array<String> = arrayOf()
    var texts: Array<String> = arrayOf()
    /// TASK-180 test5: `gt4` (v2.00) or `spec2`; the game Activity loads the matching library.
    var game: String = "gt4"
}

@InvokeArg
class SyncTreeArgs {
    var uri: String = ""
    var destination: String = ""
}

@InvokeArg
class ShaderArgs {
    var environment: Array<String> = arrayOf()
    var log: String = ""
}

@InvokeArg
class UrlArgs {
    var url: String = ""
}

@InvokeArg
class ExitsArgs {
    var since: Long = 0
    var folder: String = ""
}

@TauriPlugin
class GameBridgePlugin(private val activity: Activity) : Plugin(activity) {

    @Command
    fun pickDocument(invoke: Invoke) {
        val args = invoke.parseArgs(PickArgs::class.java)
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"
            putExtra(Intent.EXTRA_MIME_TYPES, args.mimeTypes)
            var flags = Intent.FLAG_GRANT_READ_URI_PERMISSION
            if (args.persist) flags = flags or Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION
            addFlags(flags)
        }
        startActivityForResult(invoke, intent, "pickDocumentResult")
    }

    @ActivityCallback
    fun pickDocumentResult(invoke: Invoke, result: ActivityResult) {
        val uri = result.data?.data
        if (result.resultCode != Activity.RESULT_OK || uri == null) {
            invoke.resolve(JSObject())
            return
        }
        val args = invoke.parseArgs(PickArgs::class.java)
        // A copy can take a moment: off the main thread.
        Thread {
            try {
                if (args.persist) {
                    activity.contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
                }
                val target = args.copyTo
                if (target != null) {
                    val input = activity.contentResolver.openInputStream(uri)
                        ?: throw IllegalStateException("$uri cannot be read")
                    input.use { from -> FileOutputStream(File(target)).use { to -> from.copyTo(to) } }
                }
                val answer = JSObject()
                answer.put("uri", uri.toString())
                answer.put("name", displayName(uri))
                invoke.resolve(answer)
            } catch (error: Exception) {
                invoke.reject(error.message ?: error.toString())
            }
        }.start()
    }

    // Owner (2026-11-06): the texture folder picker on the phone. The player picks any folder with the system picker
    // (a tree the app keeps a persistable read permission to), and the launcher copies it once into the app's own
    // storage, because ARMSX2 reads plain file paths, never content URIs. Nothing is copied again on a later start.
    @Command
    fun pickTexturesFolder(invoke: Invoke) {
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT_TREE).apply {
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION)
        }
        startActivityForResult(invoke, intent, "pickTexturesFolderResult")
    }

    @ActivityCallback
    fun pickTexturesFolderResult(invoke: Invoke, result: ActivityResult) {
        val uri = result.data?.data
        if (result.resultCode != Activity.RESULT_OK || uri == null) {
            invoke.resolve(JSObject())
            return
        }
        try {
            activity.contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
        } catch (error: Exception) {
            // A provider that cannot grant a persistable permission still grants it for this session.
        }
        val answer = JSObject()
        answer.put("uri", uri.toString())
        answer.put("name", treeName(uri))
        invoke.resolve(answer)
    }

    // Copies one document tree into `destination` (app storage), returning how many files were written. Only the
    /// player's choice triggers it, so a large pack is copied once, not at every start.
    @Command
    fun syncTexturesTree(invoke: Invoke) {
        val args = invoke.parseArgs(SyncTreeArgs::class.java)
        Thread {
            try {
                val tree = Uri.parse(args.uri)
                val root = File(args.destination)
                root.mkdirs()
                var files = 0
                fun walk(parent: Uri, folder: File) {
                    val children = queryChildren(parent)
                    for ((name, mime, id) in children) {
                        val childUri = DocumentsContract.buildDocumentUriUsingTree(tree, id)
                        val target = File(folder, name)
                        if (mime == DocumentsContract.Document.MIME_TYPE_DIR) {
                            target.mkdirs()
                            walk(childUri, target)
                        } else {
                            activity.contentResolver.openInputStream(childUri)?.use { from ->
                                FileOutputStream(target).use { to -> from.copyTo(to) }
                            } ?: throw IllegalStateException("$childUri cannot be read")
                            files += 1
                        }
                    }
                }
                walk(tree, root)
                val answer = JSObject()
                answer.put("files", files)
                invoke.resolve(answer)
            } catch (error: Exception) {
                invoke.reject(error.message ?: error.toString())
            }
        }.start()
    }

    // The name the folder picker shows for the picked tree ("GT4 textures"), for the Settings page's plain words. A
    // tree URI is not a document: the provider throws UnsupportedOperationException for columns it does not serve, so
    // every query here is guarded and the last fallback is the document id.
    private fun treeName(uri: Uri): String? {
        try {
            activity.contentResolver.query(uri, arrayOf(DocumentsContract.Document.COLUMN_DISPLAY_NAME), null, null, null)?.use { cursor ->
                if (cursor.moveToFirst()) return cursor.getString(0)
            }
        } catch (error: Exception) {
            // A provider without that column: the question falls back to the document id below.
        }
        try {
            displayName(uri)?.let { return it }
        } catch (error: Exception) {
            // A tree URI does not answer OpenableColumns: the document id below names it.
        }
        val id = try { DocumentsContract.getTreeDocumentId(uri) } catch (error: Exception) { return null }
        return id.substringAfterLast('/').substringAfterLast(':').ifEmpty { null }
    }

    private fun queryChildren(parent: Uri): List<Triple<String, String, String>> {
        // The document id of THIS folder, not the tree's root: a child URI is tree/<root>/document/<id>, and using the
        // tree id there would list the root again on every level. About a tree URI itself getDocumentId throws (it only
        // takes document URIs), so the root's own id comes from getTreeDocumentId.
        val segments = parent.pathSegments
        val isDocumentUri = segments.size >= 4 && segments[2] == "document"
        val documentId = if (isDocumentUri) DocumentsContract.getDocumentId(parent) else DocumentsContract.getTreeDocumentId(parent)
        val childrenUri = DocumentsContract.buildChildDocumentsUriUsingTree(parent, documentId)
        val out = mutableListOf<Triple<String, String, String>>()
        activity.contentResolver.query(childrenUri, arrayOf(
            DocumentsContract.Document.COLUMN_DISPLAY_NAME,
            DocumentsContract.Document.COLUMN_MIME_TYPE,
            DocumentsContract.Document.COLUMN_DOCUMENT_ID,
        ), null, null, null)?.use { cursor ->
            while (cursor.moveToNext()) {
                out.add(Triple(cursor.getString(0), cursor.getString(1), cursor.getString(2)))
            }
        }
        return out
    }

    @Command
    fun openFd(invoke: Invoke) {
        val args = invoke.parseArgs(UriArgs::class.java)
        try {
            // The raw descriptor is detached and the ParcelFileDescriptor closed at once: an open one keeps a stable
            // reference to the documents provider, and when Android kills that provider's process (memory pressure, the
            // game is large) every process holding such a reference dies with it ("dependency died", owner's test1).
            val fd = activity.contentResolver.openFileDescriptor(Uri.parse(args.uri), "r")?.use { it.detachFd() } ?: -1
            val answer = JSObject()
            // Detached: the Rust side owns the descriptor from here and closes it.
            answer.put("fd", fd)
            invoke.resolve(answer)
        } catch (error: Exception) {
            invoke.reject(error.message ?: error.toString())
        }
    }

    @Command
    fun saveDocument(invoke: Invoke) {
        val args = invoke.parseArgs(SaveArgs::class.java)
        val intent = Intent(Intent.ACTION_CREATE_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = args.mimeType
            putExtra(Intent.EXTRA_TITLE, args.name)
        }
        startActivityForResult(invoke, intent, "saveDocumentResult")
    }

    @ActivityCallback
    fun saveDocumentResult(invoke: Invoke, result: ActivityResult) {
        val uri = result.data?.data
        if (result.resultCode != Activity.RESULT_OK || uri == null) {
            invoke.resolve(JSObject().put("saved", false))
            return
        }
        val args = invoke.parseArgs(SaveArgs::class.java)
        Thread {
            try {
                val output = activity.contentResolver.openOutputStream(uri, "wt")
                    ?: throw IllegalStateException("$uri cannot be written")
                output.use { to -> FileInputStream(File(args.source)).use { from -> from.copyTo(to) } }
                invoke.resolve(JSObject().put("saved", true))
            } catch (error: Exception) {
                invoke.reject(error.message ?: error.toString())
            }
        }.start()
    }

    @Command
    fun startGame(invoke: Invoke) {
        val args = invoke.parseArgs(GameArgs::class.java)
        // PRIVATE test build: the ARMSX2 GS reads GameIndex.yaml and the OpenGL ES shader sources from its own resources
        // folder under the app's external files folder. They ship as APK assets (staged by android/stage-armsx2gs.cmd)
        // and are copied out here, before the game process starts; the copy is cheap and idempotent, so it runs every Play.
        try {
            val external = activity.getExternalFilesDir(null)
            if (external != null) {
                copyAssetTree("armsx2gs/resources", File(external, "armsx2gs/resources"))
            }
        } catch (error: Exception) {
            android.util.Log.w("GT4Recomp-launcher", "ARMSX2 GS resources not extracted: ${error.message}")
        }
        val intent = Intent(activity, GameActivity::class.java).apply {
            putExtra(GameActivity.EXTRA_ARGUMENTS, args.arguments)
            putExtra(GameActivity.EXTRA_ENVIRONMENT, args.environment)
            putExtra(GameActivity.EXTRA_TEXTS, args.texts)
            putExtra(GameActivity.EXTRA_GAME, args.game)
            // TASK-180 test3: the game is another screen of this app, in this task and without an open animation.
            addFlags(Intent.FLAG_ACTIVITY_NO_ANIMATION)
        }
        activity.runOnUiThread {
            try {
                activity.startActivity(intent)
                noTransition(activity)
                invoke.resolve(JSObject())
            } catch (error: Exception) {
                invoke.reject(error.message ?: error.toString())
            }
        }
    }

    /// Copies one asset subtree out of the APK, creating folders as needed. A missing asset is skipped silently (a build
    /// without the private ARMSX2 GS has none).
    private fun copyAssetTree(assetPath: String, target: File) {
        val children = activity.assets.list(assetPath) ?: return
        if (children.isEmpty()) {
            target.parentFile?.mkdirs()
            activity.assets.open(assetPath).use { input ->
                FileOutputStream(target).use { output -> input.copyTo(output) }
            }
            return
        }
        target.mkdirs()
        for (child in children) copyAssetTree("$assetPath/$child", File(target, child))
    }

    /// TASK-180 test3: no activity transition. `overridePendingTransition` is deprecated from API 34 but still applies;
    /// the game's theme also sets no window animation, so the two together keep the change instant on every version.
    private fun noTransition(activity: Activity) {
        @Suppress("DEPRECATION")
        activity.overridePendingTransition(0, 0)
    }

    // TASK-180 test2: is the game's process alive (Minimize keeps it, paused)? The process list of this app (the
    // system's own record) decides; the game's heartbeat file adds whether it is paused and how fresh that is (a
    // minimized game may be frozen by the system, so a stale heartbeat alone does not mean it is gone).
    @Command
    fun gameState(invoke: Invoke) {
        val answer = JSObject()
        try {
            val manager = activity.getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
            val game = manager.runningAppProcesses?.firstOrNull { it.processName == activity.packageName + ":game" }
            answer.put("alive", game != null)
            answer.put("pid", game?.pid ?: 0)
            val shaders = manager.runningAppProcesses?.firstOrNull { it.processName == activity.packageName + ":shaders" }
            answer.put("shadersAlive", shaders != null)
            answer.put("shadersPid", shaders?.pid ?: 0)
            val heartbeat = File(activity.filesDir, GameActivity.HEARTBEAT_FILE)
            if (game != null && heartbeat.exists()) {
                val parts = heartbeat.readText().trim().split(" ")
                if (parts.size >= 3 && parts[0].toIntOrNull() == game.pid) {
                    answer.put("state", parts[1])
                    answer.put("heartbeatAgeMs", System.currentTimeMillis() - (parts[2].toLongOrNull() ?: 0L))
                }
            }
        } catch (error: Exception) {
            invoke.reject(error.message ?: error.toString())
            return
        }
        invoke.resolve(answer)
    }

    // TASK-180 test2: the shader prepare in its own process (ShaderPrepareService).
    @Command
    fun prepareShaders(invoke: Invoke) {
        val args = invoke.parseArgs(ShaderArgs::class.java)
        try {
            val intent = Intent(activity, ShaderPrepareService::class.java)
                .putExtra(ShaderPrepareService.EXTRA_ENVIRONMENT, args.environment)
                .putExtra(ShaderPrepareService.EXTRA_LOG, args.log)
            activity.startService(intent)
            invoke.resolve(JSObject())
        } catch (error: Exception) {
            invoke.reject(error.message ?: error.toString())
        }
    }

    // Cancel: the prepare's process ends (what it compiled stays in the cache).
    @Command
    fun stopShaders(invoke: Invoke) {
        val manager = activity.getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
        manager.runningAppProcesses?.firstOrNull { it.processName == activity.packageName + ":shaders" }?.let {
            android.os.Process.killProcess(it.pid)
        }
        invoke.resolve(JSObject())
    }

    // TASK-180 test2: Resume brings the minimized game's activity back; it unpauses itself in onResume.
    @Command
    fun resumeGame(invoke: Invoke) {
        // TASK-180 test3: the game lives in the launcher's task, below it after Minimize; this brings that same instance
        // back to the front instead of starting another one, with no animation.
        val intent = Intent(activity, GameActivity::class.java).apply {
            putExtra(GameActivity.EXTRA_RESUME, true)
            addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT or Intent.FLAG_ACTIVITY_NO_ANIMATION)
        }
        activity.runOnUiThread {
            try {
                activity.startActivity(intent)
                @Suppress("DEPRECATION")
                activity.overridePendingTransition(0, 0)
                invoke.resolve(JSObject())
            } catch (error: Exception) {
                invoke.reject(error.message ?: error.toString())
            }
        }
    }

    @Command
    fun openUrl(invoke: Invoke) {
        val args = invoke.parseArgs(UrlArgs::class.java)
        val url = Uri.parse(args.url)
        if (url.scheme != "https") {
            invoke.reject("only https links are opened")
            return
        }
        activity.runOnUiThread {
            try {
                activity.startActivity(Intent(Intent.ACTION_VIEW, url).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
                invoke.resolve(JSObject())
            } catch (error: ActivityNotFoundException) {
                invoke.reject("no browser to open ${args.url}")
            }
        }
    }

    // TASK-180 test2: the screen's current mode in landscape, the Render scale's output (100% = native, as a monitor).
    @Command
    fun displayInfo(invoke: Invoke) {
        val answer = JSObject()
        try {
            @Suppress("DEPRECATION")
            val display = activity.display ?: activity.windowManager.defaultDisplay
            val mode = display.mode
            answer.put("width", maxOf(mode.physicalWidth, mode.physicalHeight))
            answer.put("height", minOf(mode.physicalWidth, mode.physicalHeight))
            answer.put("refreshHz", mode.refreshRate.toDouble())
        } catch (error: Exception) {
            answer.put("width", 0)
            answer.put("height", 0)
            answer.put("refreshHz", 0.0)
        }
        invoke.resolve(answer)
    }

    // TASK-180 test2: the app's external files folder (adb can push debug.env there without root).
    @Command
    fun externalDir(invoke: Invoke) {
        invoke.resolve(JSObject().put("path", activity.getExternalFilesDir(null)?.path ?: ""))
    }

    @Command
    fun deviceInfo(invoke: Invoke) {
        val lines = mutableListOf<String>()
        try {
            lines.add("device: ${Build.MANUFACTURER} ${Build.MODEL} (${Build.DEVICE}, product ${Build.PRODUCT})")
            lines.add("android: ${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT}), security patch ${Build.VERSION.SECURITY_PATCH}")
            lines.add("build: ${Build.FINGERPRINT}")
            if (Build.VERSION.SDK_INT >= 31) {
                lines.add("soc: ${Build.SOC_MANUFACTURER} ${Build.SOC_MODEL}")
            }
            lines.add("hardware: ${Build.HARDWARE}, board ${Build.BOARD}, ABIs ${Build.SUPPORTED_ABIS.joinToString(",")}")
            lines.add("processors: ${Runtime.getRuntime().availableProcessors()}")
            val manager = activity.getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
            val memory = ActivityManager.MemoryInfo()
            manager.getMemoryInfo(memory)
            lines.add("memory: ${memory.totalMem / (1024 * 1024)} MB total, ${memory.availMem / (1024 * 1024)} MB available, " +
                "low-memory device ${manager.isLowRamDevice}, app heap limit ${manager.largeMemoryClass} MB")
            val features = activity.packageManager.systemAvailableFeatures
            fun feature(name: String): String {
                val found = features.firstOrNull { it.name == name } ?: return "none"
                return "0x" + Integer.toHexString(found.version)
            }
            lines.add("vulkan features: hardware version ${feature(PackageManager.FEATURE_VULKAN_HARDWARE_VERSION)}, " +
                "level ${feature(PackageManager.FEATURE_VULKAN_HARDWARE_LEVEL)}, " +
                "compute ${feature(PackageManager.FEATURE_VULKAN_HARDWARE_COMPUTE)}, " +
                "dEQP level ${feature(PackageManager.FEATURE_VULKAN_DEQP_LEVEL)}")
            val gles = manager.deviceConfigurationInfo.reqGlEsVersion
            lines.add("opengl es: ${gles shr 16}.${gles and 0xFFFF}")
            @Suppress("DEPRECATION")
            val display = activity.display ?: activity.windowManager.defaultDisplay
            val mode = display.mode
            val bounds = activity.windowManager.maximumWindowMetrics.bounds
            lines.add("display: ${mode.physicalWidth}x${mode.physicalHeight} at ${"%.2f".format(mode.refreshRate)} Hz, " +
                "window ${bounds.width()}x${bounds.height()}, density ${activity.resources.displayMetrics.densityDpi} dpi")
            lines.add("display modes: " + display.supportedModes.joinToString(", ") {
                "${it.physicalWidth}x${it.physicalHeight}@${"%.0f".format(it.refreshRate)}"
            })
            val power = activity.getSystemService(Context.POWER_SERVICE) as PowerManager
            lines.add("power: battery saver ${power.isPowerSaveMode}, thermal status ${power.currentThermalStatus}, " +
                "sustained performance ${power.isSustainedPerformanceModeSupported}")
            val app = activity.packageManager.getPackageInfo(activity.packageName, 0)
            lines.add("app: ${activity.packageName} ${app.versionName} (code ${app.longVersionCode})")
        } catch (error: Exception) {
            lines.add("device facts stopped: $error")
        }
        val answer = JSObject()
        answer.put("lines", JSArray(lines))
        invoke.resolve(answer)
    }

    // The ends of the app's processes (the launcher, and the game's ":game") after `since` (milliseconds since the epoch), oldest first. A native crash
    // leaves its tombstone (a protobuf, Android 12+) and an ANR its trace (text) in `folder`; the launcher turns the
    // tombstone into text for the log bundle.
    @Command
    fun gameExits(invoke: Invoke) {
        val args = invoke.parseArgs(ExitsArgs::class.java)
        val list = JSArray()
        if (Build.VERSION.SDK_INT < 30) {
            invoke.resolve(JSObject().put("exits", list))
            return
        }
        try {
            val manager = activity.getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
            val exits = manager.getHistoricalProcessExitReasons(activity.packageName, 0, 32)
                .filter { it.processName == activity.packageName || it.processName.startsWith(activity.packageName + ":") }
                .filter { it.timestamp > args.since }
                .sortedBy { it.timestamp }
            for (exit in exits) {
                val entry = JSObject()
                entry.put("time", exit.timestamp)
                entry.put("pid", exit.pid)
                entry.put("process", exit.processName)
                entry.put("reason", exit.reason)
                entry.put("reasonName", reasonName(exit.reason))
                entry.put("status", exit.status)
                entry.put("importance", exit.importance)
                entry.put("description", exit.description ?: "")
                entry.put("pssKb", exit.pss)
                entry.put("rssKb", exit.rss)
                if (args.folder.isNotEmpty()) {
                    val suffix = when (exit.reason) {
                        ApplicationExitInfo.REASON_CRASH_NATIVE -> "tombstone.pb"
                        ApplicationExitInfo.REASON_ANR -> "anr.txt"
                        else -> "trace.txt"
                    }
                    try {
                        exit.traceInputStream?.use { from ->
                            val file = File(args.folder, "game-exit-${exit.timestamp}.$suffix")
                            file.parentFile?.mkdirs()
                            FileOutputStream(file).use { to -> from.copyTo(to) }
                            entry.put("traceFile", file.absolutePath)
                        }
                    } catch (error: Exception) {
                        entry.put("traceError", error.toString())
                    }
                }
                list.put(entry)
            }
        } catch (error: Exception) {
            invoke.reject(error.message ?: error.toString())
            return
        }
        invoke.resolve(JSObject().put("exits", list))
    }

    private fun reasonName(reason: Int): String = when (reason) {
        ApplicationExitInfo.REASON_EXIT_SELF -> "exit"
        ApplicationExitInfo.REASON_SIGNALED -> "signaled"
        ApplicationExitInfo.REASON_LOW_MEMORY -> "low memory"
        ApplicationExitInfo.REASON_CRASH -> "crash (Java)"
        ApplicationExitInfo.REASON_CRASH_NATIVE -> "crash (native)"
        ApplicationExitInfo.REASON_ANR -> "not responding (ANR)"
        ApplicationExitInfo.REASON_INITIALIZATION_FAILURE -> "initialization failure"
        ApplicationExitInfo.REASON_PERMISSION_CHANGE -> "permission change"
        ApplicationExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE -> "excessive resource usage"
        ApplicationExitInfo.REASON_USER_REQUESTED -> "user requested"
        ApplicationExitInfo.REASON_USER_STOPPED -> "user stopped"
        ApplicationExitInfo.REASON_DEPENDENCY_DIED -> "dependency died"
        ApplicationExitInfo.REASON_OTHER -> "other"
        else -> "reason $reason"
    }

    private fun displayName(uri: Uri): String? {
        activity.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { cursor ->
            if (cursor.moveToFirst()) return cursor.getString(0)
        }
        return null
    }
}
