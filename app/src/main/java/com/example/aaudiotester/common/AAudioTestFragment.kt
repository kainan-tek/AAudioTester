package com.example.aaudiotester.common

import android.annotation.SuppressLint
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Bundle
import android.provider.Settings
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.Spinner
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AlertDialog
import androidx.core.content.ContextCompat
import androidx.core.net.toUri
import androidx.fragment.app.Fragment
import com.example.aaudiotester.R
import java.util.concurrent.Executor
import java.util.concurrent.Executors
import java.util.concurrent.RejectedExecutionException

/**
 * Abstract base class: shares all UI wiring (status/buttons/Spinner/info area/permissions/exclusivity).
 */
abstract class AAudioTestFragment : Fragment() {

    protected abstract fun createEngine(context: Context, engineExecutor: Executor): AAudioEngine
    protected abstract val section: String                       // "player" / "recorder"
    protected abstract val messages: AAudioMessages
    protected abstract fun requiredPermissions(): Array<String>
    protected abstract fun formatInfo(config: AAudioConfig): String
    protected abstract fun friendlyErrorMessage(raw: String): String

    private lateinit var engine: AAudioEngine

    // The native layer is a global singleton: all native-touching calls (setAudioConfig/start/stop/release)
    // are serialized through this single-threaded executor, eliminating races and avoiding main-thread blocking
    private val engineExecutor = Executors.newSingleThreadExecutor()
    private lateinit var statusText: TextView
    private lateinit var infoText: TextView
    private lateinit var startButton: Button
    private lateinit var stopButton: Button
    private lateinit var configSpinner: Spinner

    private var availableConfigs: List<AAudioConfig> = emptyList()
    private var currentConfig: AAudioConfig? = null

    // "Permanently denied" can only be determined after requesting at least once: on the first denial the
    // rationale isn't showable yet, so don't mislead users to the settings page
    private var permissionRequestedOnce = false

    private val permissionLauncher =
        registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { result ->
            val denied = result.filterValues { !it }.keys
            if (denied.isEmpty()) {
                startInternal()
                return@registerForActivityResult
            }
            // Toasts are invisible on AAOS head units, so use a dialog; guide to system settings when permanently denied
            val permanent = permissionRequestedOnce &&
                denied.any { !shouldShowRequestPermissionRationale(it) }
            permissionRequestedOnce = true
            val builder = AlertDialog.Builder(requireContext())
                .setTitle("Permission Required")
                .setMessage(
                    if (permanent) "Permission permanently denied. Please grant it in system settings."
                    else "Permission needed to continue."
                )
                .setPositiveButton("OK") { dialog, _ -> dialog.dismiss() }
            if (permanent) {
                builder.setNegativeButton("Open Settings") { _, _ -> openAppSettings() }
            }
            builder.show()
            statusText.text = messages.failed
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // The engine is bound to the Fragment lifecycle: view recreation doesn't swap instances, so native global state never overlaps between old/new instances
        engine = createEngine(requireActivity().applicationContext, engineExecutor)
        engine.setListener(object : AAudioEngine.Listener {
            override fun onStarted() {
                activity?.runOnUiThread {
                    if (!isAdded) return@runOnUiThread
                    updateButtons(true); statusText.text = messages.active; updateInfo()
                }
            }
            override fun onStopped() {
                activity?.runOnUiThread {
                    if (!isAdded) return@runOnUiThread
                    updateButtons(false); statusText.text = messages.stopped; updateInfo()
                }
            }
            override fun onError(error: String) {
                // Actively clean up leftover resources after an error (stop can be entered from ERROR state: close stream, join threads, backfill WAV header);
                // skip cleanup while the session is still healthy (e.g. "Already active" notice)
                if (!engine.isActive()) {
                    try {
                        engineExecutor.execute { engine.stop() }
                    } catch (_: RejectedExecutionException) {
                        // Lost the race with onDestroy's shutdown: release() already did everything stop() would
                    }
                }
                activity?.runOnUiThread {
                    if (!isAdded) return@runOnUiThread
                    updateButtons(false); showError(error)
                }
            }
        })
    }

    override fun onCreateView(
        inflater: LayoutInflater, container: ViewGroup?, savedInstanceState: Bundle?
    ): View {
        val root = inflater.inflate(R.layout.fragment_audio_test, container, false)
        statusText = root.findViewById(R.id.statusTextView)
        infoText = root.findViewById(R.id.infoTextView)
        startButton = root.findViewById(R.id.startButton)
        stopButton = root.findViewById(R.id.stopButton)
        configSpinner = root.findViewById(R.id.configSpinner)

        startButton.setOnClickListener {
            val missing = requiredPermissions().filter {
                ContextCompat.checkSelfPermission(requireContext(), it) != PackageManager.PERMISSION_GRANTED
            }
            if (missing.isNotEmpty()) permissionLauncher.launch(missing.toTypedArray())
            else startInternal()
        }
        stopButton.setOnClickListener { engineExecutor.execute { engine.stop() } }
        configSpinner.setOnLongClickListener {
            reloadConfigurations()
            true
        }

        loadConfigurations()
        updateButtons(false)
        return root
    }

    private fun startInternal() {
        if (engine.isActive()) {
            toast("Already active")
            return
        }
        statusText.text = messages.preparing
        startButton.isEnabled = false  // prevent double-taps; button state is restored by onStarted/onError callbacks
        engineExecutor.execute { engine.start() }
    }

    private fun loadConfigurations() {
        availableConfigs = AAudioConfig.loadConfigs(requireContext(), section)
        if (availableConfigs.isNotEmpty()) {
            currentConfig = availableConfigs[0]
            engineExecutor.execute { engine.setAudioConfig(currentConfig!!) }
            setupSpinner()
            updateInfo()
            statusText.text = messages.ready
        } else {
            statusText.text = messages.failed
            startButton.isEnabled = false
        }
    }

    private fun reloadConfigurations() {
        // Restore by selected position rather than description: descriptions may repeat (custom configs), and position matches the UI selection state naturally
        val prevPosition = configSpinner.selectedItemPosition
        availableConfigs = AAudioConfig.loadConfigs(requireContext(), section)
        if (availableConfigs.isNotEmpty()) {
            currentConfig = availableConfigs.getOrNull(prevPosition) ?: availableConfigs[0]
            engineExecutor.execute { engine.setAudioConfig(currentConfig!!) }
            setupSpinner()
            updateInfo()
            toast("Configuration reloaded successfully")
            statusText.text = messages.ready
        } else {
            toast("No valid configurations found")
            statusText.text = messages.failed
        }
    }

    private fun setupSpinner() {
        val adapter = ArrayAdapter(
            requireContext(), android.R.layout.simple_spinner_item, availableConfigs.map { it.description }
        )
        adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item)
        configSpinner.adapter = adapter

        currentConfig?.let { config ->
            // Locate by object position directly: avoids indexOfFirst ambiguity when descriptions repeat
            val index = availableConfigs.indexOf(config)
            if (index >= 0) configSpinner.setSelection(index)
        }

        configSpinner.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: AdapterView<*>?, view: View?, position: Int, id: Long) {
                val selected = availableConfigs[position]
                // During programmatic restore (setSelection in load/reload), currentConfig is already the same config → equal means ignore;
                // a real user switch always selects a different config (selecting the same position doesn't fire the callback), so no extra flag is needed
                if (selected == currentConfig) return
                currentConfig = selected
                engineExecutor.execute { engine.setAudioConfig(selected) }
                updateInfo()
                toast("Switched to: ${selected.description}")
            }
            override fun onNothingSelected(parent: AdapterView<*>?) {}
        }
    }

    private fun updateButtons(active: Boolean) {
        startButton.isEnabled = !active
        stopButton.isEnabled = active
        configSpinner.isEnabled = !active
    }

    @SuppressLint("SetTextI18n")
    private fun updateInfo() {
        currentConfig?.let { infoText.text = formatInfo(it) } ?: run { infoText.text = "Information" }
    }

    @SuppressLint("SetTextI18n")
    private fun showError(raw: String) {
        val userMessage = friendlyErrorMessage(raw)
        AlertDialog.Builder(requireContext())
            .setTitle("Error")
            .setMessage(userMessage)
            .setPositiveButton("OK") { d, _ -> d.dismiss(); statusText.text = messages.ready }
            .setCancelable(true)
            .setOnCancelListener { statusText.text = messages.ready }
            .show()
        statusText.text = "Error: $userMessage"
    }

    private fun toast(msg: String) {
        Toast.makeText(requireContext(), msg, Toast.LENGTH_SHORT).show()
    }

    /** Opens this app's system settings page (guidance when permission is permanently denied) */
    private fun openAppSettings() {
        val intent = Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS)
            .setData("package:${requireContext().packageName}".toUri())
        startActivity(intent)
    }

    override fun onPause() {
        super.onPause()
        engineExecutor.execute { engine.stop() }
    }

    override fun onDestroy() {
        super.onDestroy()
        // FIFO: queued after any in-flight stop, native resources (GlobalRef etc.) are eventually released;
        // release shuts down the executor itself when done — async callbacks arriving meanwhile (onError/focus loss) are still
        // accepted and safely executed, not interrupted by RejectedExecutionException (stop after release is a no-op)
        engineExecutor.execute {
            try {
                engine.release()
            } finally {
                engineExecutor.shutdown()
            }
        }
    }
}

/** Status strings for each state. */
data class AAudioMessages(
    val ready: String,
    val preparing: String,
    val active: String,
    val stopped: String,
    val failed: String,
)
