// SPDX-License-Identifier: MIT
// TASK-185: the on-screen virtual pad, drawn over the game picture in GameActivity.
//
// It draws the elements of the layout file as the native side parsed it (nativeVirtualPadGeometry/nativeVirtualPadBinds:
// buttons, d-pad and sticks, with the global and per-element opacity), takes multi-touch, and forwards each touch to the
// native pad (nativeVirtualPadTouch). It holds no PS2 knowledge: the dead zone, the stick range, the d-pad directions
// and the merge with a physical controller all live in native/virtual_pad.cpp, so a layout the editor writes and a
// layout the game reads obey one rule.
//
// The view fills the activity and consumes touches over the game picture (nothing else uses them); the F7 report button
// is added after this view, so it stays on top of its corner, and the default layout keeps that corner free.
package io.github.moesuito.gt4recomp

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
import android.graphics.Typeface
import android.view.HapticFeedbackConstants
import android.view.MotionEvent
import android.view.View
import kotlin.math.abs
import kotlin.math.hypot
import kotlin.math.min

class VirtualPadView(context: Context) : View(context) {
    companion object {
        // The layout model's element types, as gt4::vpad::Pad::geometry reports them.
        private const val TYPE_BUTTON = 0f
        private const val TYPE_DPAD = 1f
        private const val TYPE_STICK = 2f
        // A touch that moves less than this (fractions of the element's half size) is the same value: a still thumb does
        // not flood the native pad with events.
        private const val MOVE_EPSILON = 0.004f
    }

    private class Element(
        val index: Int,
        val type: Float,
        val bind: String,
        val x: Float,
        val y: Float,
        val width: Float,
        val height: Float,
        val opacity: Float,
        val visible: Boolean,
    )

    // One finger and the element it holds. A stick remembers where the thumb landed, so a touch off the stick's centre
    // still starts at rest and moves relatively (the contract's rule).
    private class Held(val element: Int, val startX: Float, val startY: Float) {
        var dx = 0.0f
        var dy = 0.0f
    }

    private val elements = ArrayList<Element>()
    private val pointerHeld = HashMap<Int, Held>()
    private var haptics = true

    private val outline = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = dp(2f)
        strokeCap = Paint.Cap.ROUND
        strokeJoin = Paint.Join.ROUND
    }
    private val fill = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL }
    private val label = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.FILL
        textAlign = Paint.Align.CENTER
        typeface = Typeface.DEFAULT_BOLD
    }
    private val path = Path()
    private val roundRect = RectF()

    /** The native touch sink: element index, offsets in -1..1 (screen sign) and whether this is a press. */
    var onTouchElement: ((index: Int, dx: Float, dy: Float, down: Boolean) -> Unit)? = null

    private fun dp(value: Float) = value * resources.displayMetrics.density

    /** Takes the layout the native side parsed: 3 header floats then 8 per element (nativeVirtualPadGeometry). */
    fun setLayout(geometry: FloatArray, binds: Array<String>) {
        elements.clear()
        if (geometry.size < 3) {
            invalidate()
            return
        }
        haptics = geometry[2] != 0.0f
        val count = geometry[0].toInt()
        for (index in 0 until count) {
            val base = 3 + index * 8
            if (base + 7 >= geometry.size) break
            elements.add(
                Element(
                    index = index,
                    type = geometry[base],
                    bind = binds.getOrElse(index) { "" },
                    x = geometry[base + 2],
                    y = geometry[base + 3],
                    width = geometry[base + 4],
                    height = geometry[base + 5],
                    opacity = geometry[base + 6],
                    visible = geometry[base + 7] != 0.0f,
                )
            )
        }
        invalidate()
    }

    /** Lets go of every element (the pad was hidden, the game paused, or the activity left): nothing stays pressed. */
    fun releaseAll() {
        if (pointerHeld.isEmpty()) return
        for (held in pointerHeld.values) onTouchElement?.invoke(held.element, 0.0f, 0.0f, false)
        pointerHeld.clear()
        invalidate()
    }

    // ---- touch --------------------------------------------------------------------------------------------------------

    override fun onTouchEvent(event: MotionEvent): Boolean {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> press(event, event.actionIndex)
            MotionEvent.ACTION_MOVE -> {
                for (index in 0 until event.pointerCount) move(event, index)
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> release(event.getPointerId(event.actionIndex))
            MotionEvent.ACTION_CANCEL -> releaseAll()
            else -> return true
        }
        return true
    }

    private fun press(event: MotionEvent, index: Int) {
        val x = event.getX(index)
        val y = event.getY(index)
        val element = elementAt(x, y) ?: return
        val held = Held(element.index, x, y)
        pointerHeld[event.getPointerId(index)] = held
        val offsets = offsetsFor(element, held, x, y)
        held.dx = offsets.first
        held.dy = offsets.second
        onTouchElement?.invoke(element.index, held.dx, held.dy, true)
        if (haptics) performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP)
        invalidate()
    }

    private fun move(event: MotionEvent, index: Int) {
        val held = pointerHeld[event.getPointerId(index)] ?: return
        val element = elements.getOrNull(held.element) ?: return
        val offsets = offsetsFor(element, held, event.getX(index), event.getY(index))
        if (abs(offsets.first - held.dx) < MOVE_EPSILON && abs(offsets.second - held.dy) < MOVE_EPSILON) return
        held.dx = offsets.first
        held.dy = offsets.second
        onTouchElement?.invoke(element.index, held.dx, held.dy, true)
        invalidate()
    }

    private fun release(pointerId: Int) {
        val held = pointerHeld.remove(pointerId) ?: return
        onTouchElement?.invoke(held.element, 0.0f, 0.0f, false)
        invalidate()
    }

    /** The topmost visible element under (x, y) that no other finger holds, or null. */
    private fun elementAt(x: Float, y: Float): Element? {
        if (width == 0 || height == 0) return null
        val taken = pointerHeld.values.map { it.element }.toHashSet()
        for (index in elements.indices.reversed()) {
            val element = elements[index]
            if (!element.visible || element.index in taken) continue
            val centreX = element.x * width
            val centreY = element.y * height
            val halfWidth = element.width * height / 2f
            val halfHeight = element.height * height / 2f
            if (element.type == TYPE_STICK) {
                val radius = min(halfWidth, halfHeight)
                if (hypot(x - centreX, y - centreY) <= radius) return element
            } else if (abs(x - centreX) <= halfWidth && abs(y - centreY) <= halfHeight) {
                return element
            }
        }
        return null
    }

    /**
     * The touch's offset from what the element measures against, in -1..1 with the screen's sign: a stick reads it from
     * where the thumb landed (and clamps it to the circle), a d-pad from its centre. A button has no offset.
     */
    private fun offsetsFor(element: Element, held: Held, x: Float, y: Float): Pair<Float, Float> {
        if (element.type == TYPE_BUTTON) return 0.0f to 0.0f
        val halfHeight = element.height * height / 2f
        val halfWidth = element.width * height / 2f
        if (element.type == TYPE_DPAD) {
            val dx = if (halfWidth > 0f) ((x - element.x * width) / halfWidth) else 0.0f
            val dy = if (halfHeight > 0f) ((y - element.y * height) / halfHeight) else 0.0f
            return dx.coerceIn(-1.0f, 1.0f) to dy.coerceIn(-1.0f, 1.0f)
        }
        val radius = min(halfWidth, halfHeight)
        if (radius <= 0f) return 0.0f to 0.0f
        var dx = (x - held.startX) / radius
        var dy = (y - held.startY) / radius
        val length = hypot(dx, dy)
        if (length > 1.0f) {
            dx /= length
            dy /= length
        }
        return dx to dy
    }

    // ---- drawing ------------------------------------------------------------------------------------------------------

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        if (width == 0 || height == 0) return
        for (element in elements) {
            if (!element.visible) continue
            val centreX = element.x * width
            val centreY = element.y * height
            val halfHeight = element.height * height / 2f
            val halfWidth = element.width * height / 2f
            val held = pointerHeld.values.firstOrNull { it.element == element.index }
            val pressed = held != null
            val alpha = (element.opacity.coerceIn(0.05f, 1f) * 255f).toInt()
            when (element.type) {
                TYPE_STICK -> drawStick(canvas, element, centreX, centreY, halfWidth, halfHeight, held, alpha)
                TYPE_DPAD -> drawDpad(canvas, centreX, centreY, halfWidth, halfHeight, held, alpha)
                else -> drawButton(canvas, element, centreX, centreY, halfWidth, halfHeight, pressed, alpha)
            }
        }
    }

    private fun drawStick(
        canvas: Canvas,
        element: Element,
        centreX: Float,
        centreY: Float,
        halfWidth: Float,
        halfHeight: Float,
        held: Held?,
        alpha: Int,
    ) {
        val radius = min(halfWidth, halfHeight)
        if (radius <= 0f) return
        fill.color = Color.WHITE
        fill.alpha = (alpha * 0.12f).toInt()
        canvas.drawCircle(centreX, centreY, radius, fill)
        outline.color = Color.WHITE
        outline.alpha = alpha
        canvas.drawCircle(centreX, centreY, radius, outline)
        // The knob follows the thumb inside the circle and sits at the centre otherwise.
        val dx = held?.dx ?: 0.0f
        val dy = held?.dy ?: 0.0f
        val knobX = centreX + dx * radius * 0.55f
        val knobY = centreY + dy * radius * 0.55f
        fill.alpha = (alpha * 0.35f).toInt()
        canvas.drawCircle(knobX, knobY, radius * 0.42f, fill)
        outline.alpha = alpha
        canvas.drawCircle(knobX, knobY, radius * 0.42f, outline)
        fill.alpha = (alpha * 0.6f).toInt()
        canvas.drawCircle(centreX, centreY, radius * 0.05f, fill)
    }

    private fun drawDpad(
        canvas: Canvas,
        centreX: Float,
        centreY: Float,
        halfWidth: Float,
        halfHeight: Float,
        held: Held?,
        alpha: Int,
    ) {
        val armX = halfWidth
        val armY = halfHeight
        if (armX <= 0f || armY <= 0f) return
        val thicknessX = armX * 0.34f
        val thicknessY = armY * 0.34f
        // A plus: four arms, so the pressed directions can be filled one by one.
        val arms = arrayOf(
            RectF(centreX - thicknessX, centreY - armY, centreX + thicknessX, centreY - thicknessY * 0.6f),
            RectF(centreX - thicknessX, centreY + thicknessY * 0.6f, centreX + thicknessX, centreY + armY),
            RectF(centreX - armX, centreY - thicknessY, centreX - thicknessX * 0.6f, centreY + thicknessY),
            RectF(centreX + thicknessX * 0.6f, centreY - thicknessY, centreX + armX, centreY + thicknessY),
        )
        val dx = held?.dx ?: 0.0f
        val dy = held?.dy ?: 0.0f
        val pressedArms = booleanArrayOf(dy < -1f / 3f, dy > 1f / 3f, dx < -1f / 3f, dx > 1f / 3f)
        path.reset()
        for (arm in arms) path.addRoundRect(arm, thicknessX * 0.3f, thicknessY * 0.3f, Path.Direction.CW)
        fill.color = Color.WHITE
        fill.alpha = (alpha * 0.12f).toInt()
        canvas.drawPath(path, fill)
        for (index in arms.indices) {
            if (!pressedArms[index]) continue
            fill.alpha = (alpha * 0.45f).toInt()
            canvas.drawRoundRect(arms[index], thicknessX * 0.3f, thicknessY * 0.3f, fill)
        }
        outline.color = Color.WHITE
        outline.alpha = alpha
        canvas.drawPath(path, outline)
        // The centre square, as on a DualShock.
        fill.alpha = (alpha * 0.3f).toInt()
        canvas.drawRect(centreX - thicknessX * 0.3f, centreY - thicknessY * 0.3f, centreX + thicknessX * 0.3f,
            centreY + thicknessY * 0.3f, fill)
    }

    private fun drawButton(
        canvas: Canvas,
        element: Element,
        centreX: Float,
        centreY: Float,
        halfWidth: Float,
        halfHeight: Float,
        pressed: Boolean,
        alpha: Int,
    ) {
        val face = element.bind == "cross" || element.bind == "circle" || element.bind == "square" ||
            element.bind == "triangle"
        val radius = min(halfWidth, halfHeight)
        if (face) {
            if (radius <= 0f) return
            fill.color = Color.WHITE
            fill.alpha = (alpha * if (pressed) 0.42f else 0.10f).toInt()
            canvas.drawCircle(centreX, centreY, radius, fill)
            outline.color = Color.WHITE
            outline.alpha = if (pressed) 255 else alpha
            outline.strokeWidth = dp(if (pressed) 3.4f else 2f)
            canvas.drawCircle(centreX, centreY, radius, outline)
            drawSymbol(canvas, element.bind, centreX, centreY, radius, if (pressed) 255 else alpha)
            outline.strokeWidth = dp(2f)
            return
        }
        roundRect.set(centreX - halfWidth, centreY - halfHeight, centreX + halfWidth, centreY + halfHeight)
        val corner = min(halfWidth, halfHeight) * 0.35f
        fill.color = Color.WHITE
        fill.alpha = (alpha * if (pressed) 0.42f else 0.10f).toInt()
        canvas.drawRoundRect(roundRect, corner, corner, fill)
        outline.color = Color.WHITE
        outline.alpha = if (pressed) 255 else alpha
        outline.strokeWidth = dp(if (pressed) 3.4f else 2f)
        canvas.drawRoundRect(roundRect, corner, corner, outline)
        val text = buttonLabel(element.bind)
        if (text.isEmpty()) return
        label.color = Color.WHITE
        label.alpha = if (pressed) 255 else alpha
        label.textSize = halfHeight * 0.8f
        outline.strokeWidth = dp(2f)
        val available = halfWidth * 1.7f
        val measured = label.measureText(text)
        if (measured > available && measured > 0f) label.textSize *= available / measured
        // Vertically centred: the baseline sits half the text's height below the centre.
        val metrics = label.fontMetrics
        canvas.drawText(text, centreX, centreY - (metrics.ascent + metrics.descent) / 2f, label)
    }

    /** The PlayStation symbols, drawn as outlines so every phone shows the same shape (no font dependency). */
    private fun drawSymbol(canvas: Canvas, bind: String, centreX: Float, centreY: Float, radius: Float, alpha: Int) {
        outline.color = Color.WHITE
        outline.alpha = alpha
        outline.strokeWidth = dp(2.2f)
        val reach = radius * 0.44f
        path.reset()
        when (bind) {
            "cross" -> {
                path.moveTo(centreX - reach, centreY - reach)
                path.lineTo(centreX + reach, centreY + reach)
                path.moveTo(centreX + reach, centreY - reach)
                path.lineTo(centreX - reach, centreY + reach)
                canvas.drawPath(path, outline)
            }
            "circle" -> canvas.drawCircle(centreX, centreY, reach * 0.92f, outline)
            "square" -> {
                roundRect.set(centreX - reach * 0.85f, centreY - reach * 0.85f, centreX + reach * 0.85f,
                    centreY + reach * 0.85f)
                canvas.drawRect(roundRect, outline)
            }
            "triangle" -> {
                val height = reach * 1.15f
                path.moveTo(centreX, centreY - height)
                path.lineTo(centreX + height * 0.95f, centreY + height * 0.62f)
                path.lineTo(centreX - height * 0.95f, centreY + height * 0.62f)
                path.close()
                canvas.drawPath(path, outline)
            }
        }
        outline.strokeWidth = dp(2f)
    }

    /** The label of a named button; the four face buttons draw their symbol instead. */
    private fun buttonLabel(bind: String): String = when (bind) {
        "select" -> "SELECT"
        "start" -> "START"
        "left_stick" -> ""
        else -> bind.uppercase()
    }
}
