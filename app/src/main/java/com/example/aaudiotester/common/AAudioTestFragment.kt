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
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.Executor
import java.util.concurrent.Executors

/**
 * Abstract base class: shares all UI wiring (status/buttons/Spinner/info area/permissions/exclusivity).
 */
abstract class AAudioTestFragment : Fragment() {

    companion object {
        // One FIFO thread per section: each engine's native global is a process-wide singleton, so
        // its own old/new fragment instances must serialize on one thread — an old instance's
        // stop/release completes before the new instance's initialize/config/start. But g_player and
        // g_recorder are independent, so the two engines must not queue behind each other (one
        // engine's slow teardown would otherwise delay the other's start). The only cross-engine
        // shared resource (AssetExtractor) is @Synchronized. Intentionally never shut down: threads
        // live for the process lifetime.
        private val sectionExecutors = ConcurrentHashMap<String, Executor>()

        fun executorFor(section: String): Executor =
            sectionExecutors.getOrPut(section) { Executors.newSingleThreadExecutor() }
    }

    protected abstract fun createEngine(context: Context, engineExecutor: Executor): AAudioEngine
    protected abstract val section: String                       // "player" / "recorder"
    protected abstract val messages: AAudioMessages
    protected abstract fun requiredPermissions(): Array<String>
    protected abstract fun formatInfo(config: AAudioConfig): String
    protected abstract fun friendlyErrorMessage(raw: String): String

    private lateinit var engine: AAudioEngine
    private lateinit var engineExecutor: Executor  // this section's dedicated FIFO thread

    private lateinit var statusText: TextView
    private lateinit var infoText: TextView
    private lateinit var startButton: Button
    private lateinit var stopButton: Button
    private lateinit var configSpinner: Spinner

    private var availableConfigs: List<AAudioConfig> = emptyList()
    private var currentConfig: AAudioConfig? = null

    private val permissionLauncher =
        registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { result ->
            val denied = result.filterValues { !it }.keys
            if (denied.isEmpty()) {
                startInternal()
                return@registerForActivityResult
            }
            // Toasts are invisible on AAOS head units, so use a dialog; guide to system settings when permanently denied
            // minSdk 32 (API 30+ semantics): a denial leaves the rationale showable unless the user is
            // permanently denied (requires two denials), so rationale==false right after a denial is
            // the permanent-deny signal itself — no "requested at least once" state is needed
            val permanent = denied.any { !shouldShowRequestPermissionRationale(it) }
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
        engineExecutor = executorFor(section)
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
                    engineExecutor.execute { engine.stop() }
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
            loadConfigurations(reload = true)
            true
        }

        // Buttons first: the initial zero-config load disables Start, which must not be overwritten back to enabled
        updateButtons(false)
        loadConfigurations()
        return root
    }

    private fun startInternal() {
        if (engine.isActive()) {
            toast("Already active")
            return
        }
        statusText.text = messages.preparing
        startButton.isEnabled = false  // prevent double-taps; button state is restored by onStarted/onError callbacks
        // Lock the config too: native is busy mid-start until onStarted arrives, so a switch here
        // would be silently rejected by the engine (configSpinner stays locked in updateButtons(true))
        configSpinner.isEnabled = false
        engineExecutor.execute { engine.start() }
    }

    private fun loadConfigurations(reload: Boolean = false) {
        // A reload that finds nothing keeps the old list/adapter/selection intact: the last applied config still
        // works, and clearing here would leave the spinner adapter populated while availableConfigs is empty (crash on next tap)
        val loaded = AAudioConfig.loadConfigs(requireContext(), section)
        if (loaded.isEmpty()) {
            if (reload) toast("No valid configurations found") else startButton.isEnabled = false
            statusText.text = messages.failed
            return
        }
        // Restore by selected position rather than description: descriptions may repeat (custom configs), and position matches the UI selection state naturally
        val prevPosition = configSpinner.selectedItemPosition
        availableConfigs = loaded
        currentConfig = if (reload) {
            availableConfigs.getOrNull(prevPosition) ?: availableConfigs[0]
        } else {
            availableConfigs[0]
        }
        engineExecutor.execute { engine.setAudioConfig(currentConfig!!) }
        setupSpinner()
        updateInfo()
        if (reload) toast("Configuration reloaded successfully")
        statusText.text = messages.ready
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
        // FIFO on the shared executor: release runs after any queued stop; the executor itself is
        // process-wide and stays alive for the next fragment instance
        engineExecutor.execute { engine.release() }
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
