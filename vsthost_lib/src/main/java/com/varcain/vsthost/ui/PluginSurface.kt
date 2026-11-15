package com.varcain.vsthost.ui

import android.content.Context
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.InputType
import android.util.Log
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.ViewConfiguration
import android.view.inputmethod.BaseInputConnection
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputConnection
import android.view.inputmethod.InputMethodManager
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.requiredSize
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.SideEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.Saver
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import com.varcain.vsthost.NativeBridge
import com.varcain.vsthost.util.X11Keymap

/** Render one plugin's editor.
 *
 *  The SurfaceView's dp size is CHOSEN ONCE, at the first layout of a mount,
 *  and afterwards only re-centered — never resized. Resizing a surface that is
 *  already attached re-fires surfaceChanged → nativeAttachSurfaceToX11Display,
 *  which tears the EGL window surface down and builds a new one under the
 *  compositor's feet; on Adreno that races libGLESv2_adreno.so and has been
 *  observed to SIGSEGV inside eglSwapBuffers. GuitarRackCraft handles
 *  orientation changes without recreating the activity, so a rotation
 *  recomposes us with brand-new constraints while the very same SurfaceView
 *  stays attached: the rotation-safe answer is to keep the size and center it
 *  (letterbox) inside whatever box we now get. No size change ⇒ no
 *  surfaceChanged ⇒ no re-attach ⇒ no EGL teardown.
 *
 *  A mount whose first fit is degenerate (0×0) is postponed instead of being
 *  mounted at the 1 dp floor: the view only appears once a real box is chosen,
 *  and that box is frozen immediately, so deferring never turns into a resize
 *  either.
 *
 *  The X11 server's renderer letterboxes the plugin's framebuffer inside the
 *  surface and `injectTouch` reverses the same scale/offset, so any fixed
 *  surface size works — the plugin just gets centered with black bars when its
 *  native size doesn't match the viewport.
 *
 *  ACCEPTED LIMITATION (v1): a mounted inline editor keeps its mount-time
 *  size. After a rotation it is centered instead of re-fitted, so it is
 *  letterboxed when the frozen box is smaller than the viewport and overflows
 *  (cropped by the parent, if the parent clips) when it is wider/taller. The
 *  rack's scale handle behaves the same way: it re-sizes the box around the
 *  surface, not the surface itself; a new size takes effect on the next mount
 *  (mode switch, editor reopen). Fullscreen `VstEditorActivity` and the
 *  installer mount a fresh surface, so they still get the full available size.
 *
 *  The proper fix is deferred geometry: let vsthost's native side re-run
 *  ANativeWindow_setBuffersGeometry / recreate the EGL surface at a safe point
 *  (the LV2 path already does exactly that via X11Bridge.setSurfaceSize) —
 *  explicit follow-up, deliberately not done here because it is a native
 *  change. */
@Composable
fun PluginSurface(
    displayNumber: Int,
    pluginWidth: Int,
    pluginHeight: Int,
    modifier: Modifier = Modifier,
    /** True = destroy the X11 display when this composable disposes
     *  (vstpoc standalone — when the user closes the editor, the wine
     *  subprocess is also being torn down). False = keep the display
     *  alive across recompositions (GuitarRackCraft inline-in-rack —
     *  the wine subprocess outlives any single activity transition,
     *  and destroying the display would orphan its X11 client). */
    destroyOnDispose: Boolean = true,
) {
    DisposableEffect(displayNumber) {
        onDispose {
            EditorViewRegistry.unregister(displayNumber)
            if (destroyOnDispose) {
                NativeBridge.nativeDetachAndDestroyX11Display(displayNumber)
            }
        }
    }

    val aspect = pluginAspect(pluginWidth, pluginHeight)
    BoxWithConstraints(modifier = modifier, contentAlignment = Alignment.Center) {
        // Fit-inside (letterbox), never fill-and-crop: match whichever viewport
        // edge actually binds. A very wide editor (Anvil 892x210, aspect ~4.25)
        // in a narrow viewport is width-bound; a taller-relative editor is
        // height-bound. An unbounded axis (inline-in-a-scrollable-column sends
        // maxHeight = ∞) falls back to a width-bound fit instead of producing
        // an infinite or zero-sized surface.
        val fit = fitInsideSurface(
            aspect = aspect,
            maxWidth = maxWidth,
            widthBounded = constraints.hasBoundedWidth,
            maxHeight = maxHeight,
            heightBounded = constraints.hasBoundedHeight,
        )
        // Per displayNumber so re-entering the same editor reuses the size it
        // mounted with, and a different displayNumber starts from its own.
        // Keyed on the plugin's native size too: that only changes when the
        // editor itself is re-mounted (a resize would re-attach the surface
        // anyway). rememberSaveable, not key(), so neither the AndroidView slot
        // nor the SurfaceView identity changes when the value is restored.
        var frozen by rememberSaveable(
            displayNumber,
            pluginWidth,
            pluginHeight,
            stateSaver = FrozenSurfaceSizeSaver,
        ) {
            mutableStateOf(FrozenSurfaceSize.Unchosen)
        }
        SideEffect {
            if (!frozen.isChosen && fit.isChosen) frozen = fit
        }
        val surface = if (frozen.isChosen) frozen else fit
        // Mount only once a real size exists. A degenerate first layout (0×0,
        // e.g. the first frame of a freshly attached window) would otherwise
        // mount at MinSurfaceEdge and then jump to the real size, which is the
        // very surfaceChanged → re-attach this freeze exists to prevent. The
        // SideEffect above freezes this same first non-degenerate fit within
        // the frame, so mounted ⇒ frozen ⇒ never resized afterwards.
        if (surface.isChosen) {
            AndroidView(
                // requiredSize, not size: an already-frozen box must not be
                // coerced by a smaller parent, because shrinking it is exactly
                // the re-attach that crashes. An oversized box is centered by
                // the parent and overflows.
                modifier = Modifier.requiredSize(
                    surface.width.coerceAtLeast(MinSurfaceEdge),
                    surface.height.coerceAtLeast(MinSurfaceEdge),
                ),
                factory = { ctx ->
                    EditorSurfaceView(ctx, displayNumber).also {
                        EditorViewRegistry.register(displayNumber, it)
                    }
                },
            )
        }
    }
}

/** dp box chosen for a mounted surface. 0×0 means "not chosen yet". */
private data class FrozenSurfaceSize(val width: Dp, val height: Dp) {
    val isChosen: Boolean get() = width > 0.dp && height > 0.dp

    companion object {
        val Unchosen = FrozenSurfaceSize(0.dp, 0.dp)
    }
}

private val FrozenSurfaceSizeSaver: Saver<FrozenSurfaceSize, List<Float>> = Saver(
    save = { size -> if (size.isChosen) listOf(size.width.value, size.height.value) else null },
    restore = { values -> FrozenSurfaceSize(values[0].dp, values[1].dp) },
)

/** Smallest edge we ever hand to a surface, so a degenerate constraint can
 *  never produce a 0-pixel window surface. */
private val MinSurfaceEdge = 1.dp

/** Width used when the parent gives no bounded width (e.g. inside a
 *  horizontally scrolling row). It only sizes the letterbox box, never the
 *  plugin's framebuffer. */
private val UnboundedWidthFallback = 360.dp

private fun pluginAspect(pluginWidth: Int, pluginHeight: Int): Float =
    if (pluginWidth > 0 && pluginHeight > 0) pluginWidth.toFloat() / pluginHeight.toFloat() else 1f

private fun fitInsideSurface(
    aspect: Float,
    maxWidth: Dp,
    widthBounded: Boolean,
    maxHeight: Dp,
    heightBounded: Boolean,
): FrozenSurfaceSize {
    val width = if (widthBounded) maxWidth else UnboundedWidthFallback
    val heightForFullWidth = width / aspect
    return when {
        !heightBounded -> FrozenSurfaceSize(width, heightForFullWidth)
        heightForFullWidth <= maxHeight -> FrozenSurfaceSize(width, heightForFullWidth)
        else -> FrozenSurfaceSize(maxHeight * aspect, maxHeight)
    }
}

/** Process-wide map of `displayNumber → EditorSurfaceView` so external code
 *  (e.g. GuitarRackCraft's rack chrome) can request soft-IME against the
 *  inline editor without knowing how PluginSurface is composed. The view
 *  registers itself in PluginSurface's factory and unregisters on dispose.
 *
 *  Lives in the vsthost_lib package so it stays scoped to the in-process
 *  X11 server's display numbers (which are owned by vsthost, not :app's
 *  X11DisplayManager). */
object EditorViewRegistry {
    private val views = mutableMapOf<Int, EditorSurfaceView>()

    @Synchronized
    fun register(displayNumber: Int, view: EditorSurfaceView) {
        views[displayNumber] = view
    }

    @Synchronized
    fun unregister(displayNumber: Int) {
        views.remove(displayNumber)
    }

    @Synchronized
    fun showKeyboard(displayNumber: Int): Boolean {
        val v = views[displayNumber] ?: return false
        v.requestFocus()
        val imm = v.context.getSystemService(InputMethodManager::class.java)
        imm?.showSoftInput(v, InputMethodManager.SHOW_IMPLICIT)
        return true
    }
}

/** SurfaceView that:
 *  - Forwards touch events to the X11 server display.
 *  - Captures hardware/soft-IME key events and forwards them via
 *    NativeBridge.nativeInjectX11Key + the X11Keymap translation table.
 *
 *  The IME path: Android shows the soft keyboard against this view when
 *  someone calls showSoftInput. The IME asks the view for an
 *  InputConnection via onCreateInputConnection; we hand back a
 *  BaseInputConnection whose commitText / sendKeyEvent / deleteSurroundingText
 *  routes characters to nativeInjectX11Key. Most Android soft IMEs use
 *  commitText (rather than synthetic key events) so the BaseInputConnection
 *  overrides are essential — onKeyDown alone wouldn't see soft-keyboard input. */
class EditorSurfaceView(
    context: Context,
    private val displayNumber: Int,
) : SurfaceView(context) {

    init {
        isFocusable = true
        isFocusableInTouchMode = true
        // Forward touches to the X server. Single-finger short tap/drag maps
        // to left button (action 0/1/2). A LONG-PRESS without moving past
        // the touch slop converts the gesture into a right-click (action=3
        // — atomic right press+release) at the original press location, then
        // swallows the rest of the gesture so no spurious drag/release events
        // follow.
        //
        // Long-press timeout uses Android's ViewConfiguration default
        // (~500 ms). Touch slop also comes from ViewConfiguration so we
        // cancel the long-press if the finger drifts more than the slop
        // distance (signaling a drag, not a press-and-hold).
        val longPressMs = ViewConfiguration.getLongPressTimeout().toLong()
        val touchSlop = ViewConfiguration.get(context).scaledTouchSlop
        val handler = Handler(Looper.getMainLooper())
        var rightClickConsumed = false
        var downX = 0
        var downY = 0
        var leftButtonDownSent = false
        var pendingLongPress: Runnable? = null
        setOnTouchListener { view, e ->
            when (e.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    rightClickConsumed = false
                    leftButtonDownSent = false
                    downX = e.x.toInt()
                    downY = e.y.toInt()
                    requestFocus()
                    // Block the parent (LazyColumn in GuitarRackCraft's rack)
                    // from intercepting drags as scroll gestures — otherwise
                    // turning a knob just scrolls the rack instead. Harmless
                    // when the parent doesn't intercept (vstpoc standalone).
                    view.parent?.requestDisallowInterceptTouchEvent(true)
                    // DEFER the left ButtonPress — we don't know yet whether
                    // this becomes a tap (send left) or a long-press (send
                    // right instead). If long-press fires we want to avoid
                    // ever telling wine a left button went down.
                    val pressX = downX
                    val pressY = downY
                    pendingLongPress = Runnable {
                        rightClickConsumed = true
                        pendingLongPress = null
                        NativeBridge.nativeInjectX11Touch(displayNumber, 3, pressX, pressY)
                    }
                    handler.postDelayed(pendingLongPress!!, longPressMs)
                }
                MotionEvent.ACTION_MOVE -> {
                    val dx = (e.x.toInt() - downX)
                    val dy = (e.y.toInt() - downY)
                    val moved = dx * dx + dy * dy > touchSlop * touchSlop
                    if (rightClickConsumed) {
                        // Right-click already fired; swallow further motion.
                    } else if (moved) {
                        // It's a drag, not a long-press. Cancel the pending
                        // right-click and start the deferred left button if
                        // not started yet.
                        pendingLongPress?.let { handler.removeCallbacks(it) }
                        pendingLongPress = null
                        if (!leftButtonDownSent) {
                            leftButtonDownSent = true
                            NativeBridge.nativeInjectX11Touch(displayNumber, 0, downX, downY)
                        }
                        NativeBridge.nativeInjectX11Touch(displayNumber, 2, e.x.toInt(), e.y.toInt())
                    }
                    // else: still within slop, keep waiting for long-press.
                }
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                    pendingLongPress?.let { handler.removeCallbacks(it) }
                    pendingLongPress = null
                    if (rightClickConsumed) {
                        // Right-click already fired atomically; nothing to do.
                    } else if (leftButtonDownSent) {
                        // We were dragging — close the gesture with a left UP.
                        NativeBridge.nativeInjectX11Touch(displayNumber, 1, e.x.toInt(), e.y.toInt())
                    } else if (e.actionMasked == MotionEvent.ACTION_UP) {
                        // Short tap — never sent the DOWN yet. Send press+release
                        // at the press location. Post the release one UI-loop
                        // tick later so the X11 server can drain ButtonPress
                        // separately; X50II's activation dialog stops producing
                        // UI requests when DOWN+UP arrive in the same drain.
                        NativeBridge.nativeInjectX11Touch(displayNumber, 0, downX, downY)
                        handler.post {
                            NativeBridge.nativeInjectX11Touch(displayNumber, 1, downX, downY)
                        }
                    }
                    leftButtonDownSent = false
                    rightClickConsumed = false
                }
                MotionEvent.ACTION_POINTER_DOWN,
                MotionEvent.ACTION_POINTER_UP -> {
                    // Ignore multi-touch — two-finger right-click is replaced
                    // by long-press above.
                }
                else -> return@setOnTouchListener false
            }
            true
        }
        holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(h: SurfaceHolder) {}
            override fun surfaceChanged(h: SurfaceHolder, fmt: Int, w: Int, hh: Int) {
                NativeBridge.nativeAttachSurfaceToX11Display(displayNumber, h.surface, w, hh)
                maybeRunAhbEditorTest()
                maybeRunAhbChannelTest()
            }
            override fun surfaceDestroyed(h: SurfaceHolder) {}
        })
    }

    // Phase 1 GPU-present validation (synthetic). When cache/ahb_editor_test
    // exists, ~1.5 s after attach register a CPU-filled gradient AHardwareBuffer
    // as the editor's GPU source — the compositor should sample it via EGLImage
    // in place of the (plugin or empty) CPU framebuffer. ~6 s later clear it to
    // prove the CPU path returns. One-shot per view. Drop the marker with:
    //   adb shell run-as <pkg> sh -c 'printf x > cache/ahb_editor_test'
    private var ahbTestDone = false
    private fun maybeRunAhbEditorTest() {
        if (ahbTestDone) return
        val marker = java.io.File(context.cacheDir, "ahb_editor_test")
        if (!marker.exists()) return
        ahbTestDone = true
        val h = Handler(Looper.getMainLooper())
        h.postDelayed({
            val ok = NativeBridge.nativeDebugEditorAhbGradient(displayNumber, true)
            Log.i("AhbEditorTest", "register gradient display=$displayNumber ok=$ok")
            h.postDelayed({
                val off = NativeBridge.nativeDebugEditorAhbGradient(displayNumber, false)
                Log.i("AhbEditorTest", "clear gradient display=$displayNumber ok=$off")
            }, 6000)
        }, 1500)
    }

    // Phase 2 side-channel validation (synthetic). When cache/ahb_channel_test
    // exists, ~1.5 s after attach run the synthetic client on a background
    // thread: it connects to the X server's abstract AHB socket, REGISTERs +
    // PRESENTs a gradient AHB over SCM_RIGHTS, holds it 6 s (gradient visible),
    // then UNREGISTERs (editor reverts). Distinct gradient from the Phase 1
    // hook (red-x/blue-y vs red-x/green-y) so the socket path is identifiable.
    //   adb shell run-as <pkg> toybox touch cache/ahb_channel_test
    private var ahbChannelTestDone = false
    private fun maybeRunAhbChannelTest() {
        if (ahbChannelTestDone) return
        val marker = java.io.File(context.cacheDir, "ahb_channel_test")
        if (!marker.exists()) return
        ahbChannelTestDone = true
        val logPath = java.io.File(context.cacheDir, "ahb_channel_test.log").absolutePath
        Handler(Looper.getMainLooper()).postDelayed({
            Thread {
                val ok = NativeBridge.nativeAhbChannelTest(displayNumber, 6000, logPath)
                Log.i("AhbChannelTest", "side-channel test display=$displayNumber ok=$ok")
            }.start()
        }, 1500)
    }

    override fun onCheckIsTextEditor(): Boolean = true

    override fun onCreateInputConnection(outAttrs: EditorInfo): InputConnection {
        // TYPE_CLASS_TEXT with TYPE_TEXT_FLAG_NO_SUGGESTIONS: regular
        // keyboard with clipboard bar available, autocomplete/suggestions
        // disabled. Gboard still uses composing-text for some characters,
        // but our setComposingText/commitText track the composing buffer
        // and forward only the delta (added chars + backspaces) to X11.
        outAttrs.inputType = InputType.TYPE_CLASS_TEXT or
            InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
        outAttrs.imeOptions = EditorInfo.IME_ACTION_DONE or
            EditorInfo.IME_FLAG_NO_FULLSCREEN or
            EditorInfo.IME_FLAG_NO_EXTRACT_UI or
            EditorInfo.IME_FLAG_NO_PERSONALIZED_LEARNING
        return KeyForwardingInputConnection(this, displayNumber)
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        val mapping = X11Keymap.fromAndroidKey(keyCode, event.metaState)
            ?: return super.onKeyDown(keyCode, event)
        NativeBridge.nativeInjectX11Key(displayNumber, 0, mapping.keycode, mapping.state)
        return true
    }

    override fun onKeyUp(keyCode: Int, event: KeyEvent): Boolean {
        val mapping = X11Keymap.fromAndroidKey(keyCode, event.metaState)
            ?: return super.onKeyUp(keyCode, event)
        NativeBridge.nativeInjectX11Key(displayNumber, 1, mapping.keycode, mapping.state)
        return true
    }
}

/** InputConnection wired to nativeInjectX11Key. Soft-keyboard input
 *  arrives here via commitText (most common), sendKeyEvent (some IMEs),
 *  or deleteSurroundingText (backspace). Each gets translated to a
 *  KeyPress + KeyRelease pair on the X11 server.
 *
 *  Inherits BaseInputConnection's plumbing for selection, composing text,
 *  etc. — we don't pretend to actually edit a text buffer, we just turn
 *  the IME's intents into key events. */
private class KeyForwardingInputConnection(
    targetView: EditorSurfaceView,
    private val displayNumber: Int,
) : BaseInputConnection(targetView, /*fullEditor=*/false) {

    // Tracks the current composing buffer (Gboard's in-flight composition).
    // setComposingText replaces this entirely; we send the diff to X11.
    private var composing: String = ""

    // Gboard's autofill / clipboard suggestion can fire commitText with the
    // same value repeatedly when the user taps Login or moves focus. Each
    // commitText is APPEND to the target text field (the field doesn't get
    // cleared between commits), so a "1234" password becomes "12341234..."
    // after a few clicks. Suppress duplicate commits within a short window.
    private var lastCommit: String = ""
    private var lastCommitAt: Long = 0
    private val dedupeWindowMs: Long = 1500

    /** Send X11 BackSpace N times. */
    private fun sendBackspaces(n: Int) {
        repeat(n) {
            NativeBridge.nativeInjectX11Key(displayNumber, 0, 53, 0)
            NativeBridge.nativeInjectX11Key(displayNumber, 1, 53, 0)
        }
    }

    /** Send characters as KeyPress + KeyRelease pairs. */
    private fun sendChars(s: CharSequence) {
        s.forEach { ch ->
            val m = X11Keymap.lookup(ch)
            if (m != null) {
                NativeBridge.nativeInjectX11Key(displayNumber, 0, m.keycode, m.state)
                NativeBridge.nativeInjectX11Key(displayNumber, 1, m.keycode, m.state)
            } else {
                Log.i("IMEForward", "  no mapping for char=${ch.code} '${ch}'")
            }
        }
    }

    /** Replace `composing` with `newText` on the X11 side by sending the diff. */
    private fun replaceComposingWith(newText: String) {
        val common = newText.commonPrefixWith(composing).length
        sendBackspaces(composing.length - common)
        sendChars(newText.substring(common))
    }

    override fun commitText(text: CharSequence?, newCursorPosition: Int): Boolean {
        val newText = text?.toString() ?: ""
        val now = System.currentTimeMillis()
        // Suppress duplicate Gboard autofill: if the same text was committed
        // within the dedupe window, the field probably already has it (Gboard
        // re-fires commitText on focus changes / Login button taps). Skip the
        // resend so the field doesn't get the password concatenated multiple
        // times.
        if (newText.length >= 2 && newText == lastCommit && (now - lastCommitAt) < dedupeWindowMs) {
            Log.i("IMEForward", "commitText DEDUPED len=${newText.length} text='${newText}' Δt=${now - lastCommitAt}ms")
            // Still clear composing so IME state is consistent.
            composing = ""
            return true
        }
        Log.i("IMEForward", "commitText len=${newText.length} text='${newText}' composing.len=${composing.length}")
        // commitText finalizes whatever the user typed. If we had composing
        // text in flight, the committed text REPLACES it. Compute the diff
        // from current composing to commit-text, then clear composing.
        replaceComposingWith(newText)
        composing = ""
        lastCommit = newText
        lastCommitAt = now
        return true
    }

    override fun setComposingText(text: CharSequence?, newCursorPosition: Int): Boolean {
        val newText = text?.toString() ?: ""
        Log.i("IMEForward", "setComposingText len=${newText.length} text='${newText}' composing.len=${composing.length}")
        // Gboard sends growing/shrinking composing strings as user types.
        // Diff-send to X11 and remember the new composing buffer.
        replaceComposingWith(newText)
        composing = newText
        return true
    }

    override fun finishComposingText(): Boolean {
        Log.i("IMEForward", "finishComposingText composing.len=${composing.length}")
        // IME closes composition without changing text. The chars in
        // `composing` were already sent to X11; just clear local state.
        composing = ""
        return true
    }

    override fun sendKeyEvent(event: KeyEvent): Boolean {
        val mapping = X11Keymap.fromAndroidKey(event.keyCode, event.metaState)
        Log.i("IMEForward", "sendKeyEvent keyCode=${event.keyCode} action=${event.action} mapping=${mapping}")
        if (mapping == null) return false
        val action = if (event.action == KeyEvent.ACTION_UP) 1 else 0
        NativeBridge.nativeInjectX11Key(displayNumber, action, mapping.keycode, mapping.state)
        return true
    }

    override fun deleteSurroundingText(beforeLength: Int, afterLength: Int): Boolean {
        // Most IMEs use this for the Backspace button rather than
        // sending a KEYCODE_DEL. Translate to N BackSpace events.
        // Also shrink our composing buffer if it was in flight.
        Log.i("IMEForward", "deleteSurroundingText before=$beforeLength after=$afterLength composing.len=${composing.length}")
        sendBackspaces(beforeLength)
        if (composing.isNotEmpty()) {
            composing = composing.dropLast(beforeLength.coerceAtMost(composing.length))
        }
        return true
    }

    override fun performEditorAction(actionCode: Int): Boolean {
        // IME's "Done" → Enter on the plugin.
        NativeBridge.nativeInjectX11Key(displayNumber, 0, 52, 0)  // 52 = Return
        NativeBridge.nativeInjectX11Key(displayNumber, 1, 52, 0)
        return true
    }

    override fun performContextMenuAction(id: Int): Boolean = false
    override fun performPrivateCommand(action: String?, data: Bundle?): Boolean = false
}
