package com.example.aaudiotester.common

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Session state machine of [AAudioEngineBase]: the started announcement must lose to a terminal
 * notice delivered from a native thread while start is in flight (the check-then-act race fixed here),
 * and the IDLE/ACTIVE/ERROR/CLOSED transitions must survive restart/stop/release.
 */
class AAudioEngineBaseTest {

    private class FakeEngine(bindResult: Boolean = true) : AAudioEngineBase(Runnable::run) {
        override val logTag = "FakeEngine"
        override val sessionLabel = "fake"
        override val alreadyActiveNotice = "Already fake"

        var bindResult = bindResult
        var beforeStartError: String? = null
        @Volatile var startNativeResult = true
        @Volatile var stopNativeResult = true

        var startNativeCalls = 0; private set
        var stopNativeCalls = 0; private set
        var releaseNativeCalls = 0; private set
        var terminatedCalls = 0; private set

        // Hold startNative while the test injects a racing native notice
        val holdStartNative = AtomicBoolean(false)
        val startNativeEntered = CountDownLatch(1)
        val releaseStartNative = CountDownLatch(1)

        override fun initializeNative() = bindResult
        override fun beforeStartNative(): String? = beforeStartError
        override fun startNative(): Boolean {
            startNativeCalls++
            if (holdStartNative.get()) {
                startNativeEntered.countDown()
                assertTrue(releaseStartNative.await(5, TimeUnit.SECONDS))
            }
            return startNativeResult
        }
        override fun stopNative(): Boolean { stopNativeCalls++; return stopNativeResult }
        override fun releaseNative() { releaseNativeCalls++ }
        override fun onSessionTerminated() { terminatedCalls++ }
        override fun setAudioConfig(config: AAudioConfig) {}  // engine-specific: not under test here

        // Test-only bridges to the JNI-invoked callback entry points
        fun simulateNativeError(error: String) = onNativeError(error)
        fun simulateNativeStopped() = onNativeStopped()
    }

    private class RecordingListener : AAudioEngine.Listener {
        val events = mutableListOf<String>()
        override fun onStarted() { events += "started" }
        override fun onStopped() { events += "stopped" }
        override fun onError(error: String) { events += "error:$error" }
    }

    private fun newEngine(bindResult: Boolean = true): Pair<FakeEngine, RecordingListener> {
        val engine = FakeEngine(bindResult)
        val listener = RecordingListener()
        engine.setListener(listener)
        return engine to listener
    }

    private fun startAsync(engine: FakeEngine): Thread =
        Thread { engine.start() }.apply { start() }

    @Test
    fun `start announces started and becomes active`() {
        val (engine, listener) = newEngine()
        assertTrue(engine.start())
        assertTrue(engine.isActive())
        assertEquals(listOf("started"), listener.events)
    }

    @Test
    fun `error delivered during start wins over started`() {
        val (engine, listener) = newEngine()
        engine.holdStartNative.set(true)
        val startThread = startAsync(engine)
        assertTrue(engine.startNativeEntered.await(5, TimeUnit.SECONDS))
        engine.simulateNativeError("[TEST] raced error")
        engine.releaseStartNative.countDown()
        startThread.join(1000)
        assertFalse(engine.isActive())
        assertEquals(listOf("error:[TEST] raced error"), listener.events)
    }

    @Test
    fun `stopped delivered during start wins over started`() {
        val (engine, listener) = newEngine()
        engine.holdStartNative.set(true)
        val startThread = startAsync(engine)
        assertTrue(engine.startNativeEntered.await(5, TimeUnit.SECONDS))
        engine.simulateNativeStopped()
        engine.releaseStartNative.countDown()
        startThread.join(1000)
        assertFalse(engine.isActive())
        assertEquals(listOf("stopped"), listener.events)
    }

    @Test
    fun `late error after started still tears the session down`() {
        val (engine, listener) = newEngine()
        assertTrue(engine.start())
        engine.simulateNativeError("[TEST] late error")
        assertFalse(engine.isActive())
        assertEquals(listOf("started", "error:[TEST] late error"), listener.events)
    }

    @Test
    fun `startNative false without a callback falls back to an error`() {
        val (engine, listener) = newEngine()
        engine.startNativeResult = false
        assertFalse(engine.start())
        assertFalse(engine.isActive())
        assertEquals(
            listOf("error:${AAudioConstants.ErrorTypes.STREAM} Native start failed without error callback"),
            listener.events
        )
    }

    @Test
    fun `bind failure reports error and never calls startNative`() {
        val (engine, listener) = newEngine(bindResult = false)
        assertFalse(engine.start())
        assertEquals(0, engine.startNativeCalls)
        assertTrue(listener.events.single().contains("Native initialization failed"))
    }

    @Test
    fun `beforeStartNative rejection reports its error and never calls startNative`() {
        val (engine, listener) = newEngine()
        engine.beforeStartError = "[PARAM] bad config"
        assertFalse(engine.start())
        assertEquals(0, engine.startNativeCalls)
        assertEquals(listOf("error:[PARAM] bad config"), listener.events)
    }

    @Test
    fun `start while active reports already active`() {
        val (engine, listener) = newEngine()
        assertTrue(engine.start())
        assertFalse(engine.start())
        assertEquals(1, engine.startNativeCalls)
        assertEquals(listOf("started", "error:Already fake"), listener.events)
    }

    @Test
    fun `stop with failed native delivery self-runs stopped`() {
        val (engine, listener) = newEngine()
        assertTrue(engine.start())
        engine.stopNativeResult = false
        engine.stop()
        assertFalse(engine.isActive())
        assertEquals(listOf("started", "stopped"), listener.events)
    }

    @Test
    fun `stop after native delivery does not double-notify`() {
        val (engine, listener) = newEngine()
        assertTrue(engine.start())
        engine.stop()
        assertFalse(engine.isActive())
        assertEquals(listOf("started"), listener.events)
    }

    @Test
    fun `error then stop cleans up and returns to idle`() {
        val (engine, listener) = newEngine()
        assertTrue(engine.start())
        engine.simulateNativeError("[TEST] error")
        engine.stop()
        assertFalse(engine.isActive())
        assertEquals(1, engine.stopNativeCalls)  // cleanup ran
        // A late queued stop hits the IDLE gate and touches native no more
        engine.stop()
        assertEquals(1, engine.stopNativeCalls)
    }

    @Test
    fun `restart directly after error needs no stop first`() {
        val (engine, listener) = newEngine()
        assertTrue(engine.start())
        engine.simulateNativeError("[TEST] error")
        assertTrue(engine.start())
        assertTrue(engine.isActive())
        assertEquals(listOf("started", "error:[TEST] error", "started"), listener.events)
    }

    @Test
    fun `restart directly after a closed session succeeds`() {
        val (engine, listener) = newEngine()
        assertTrue(engine.start())
        engine.simulateNativeStopped()
        assertFalse(engine.isActive())
        assertTrue(engine.start())
        assertTrue(engine.isActive())
        assertEquals(listOf("started", "stopped", "started"), listener.events)
    }

    @Test
    fun `release from error state settles to idle without native stop`() {
        val (engine, listener) = newEngine()
        assertTrue(engine.start())
        engine.simulateNativeError("[TEST] error")
        engine.release()
        assertFalse(engine.isActive())
        assertEquals(0, engine.stopNativeCalls)
        assertEquals(1, engine.releaseNativeCalls)
        // A late queued stop after release hits the IDLE gate
        engine.stop()
        assertEquals(0, engine.stopNativeCalls)
        // Listener is detached by release: no further notifications
        engine.simulateNativeError("[TEST] after release")
        assertEquals(listOf("started", "error:[TEST] error"), listener.events)
    }

    @Test
    fun `onSessionTerminated runs on error and stopped callbacks`() {
        val (engine, _) = newEngine()
        engine.start()
        engine.simulateNativeError("[TEST] error")
        engine.start()
        engine.simulateNativeStopped()
        assertEquals(2, engine.terminatedCalls)
    }
}
