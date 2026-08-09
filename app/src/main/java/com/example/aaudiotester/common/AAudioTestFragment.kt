package com.example.aaudiotester.common

import android.content.Context
import android.content.pm.PackageManager
import android.os.Bundle
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
import androidx.fragment.app.Fragment
import com.example.aaudiotester.R

/**
 * 抽象基类：共享全部 UI 接线（状态/按钮/Spinner/信息区/权限/互斥）。
 */
abstract class AAudioTestFragment : Fragment() {

    protected abstract fun createEngine(context: Context): AAudioEngine
    protected abstract val section: String                       // "player" / "recorder"
    protected abstract val messages: AAudioMessages
    protected abstract fun requiredPermissions(): Array<String>
    protected abstract fun formatInfo(config: AAudioConfig): String
    protected abstract fun friendlyErrorMessage(raw: String): String

    private lateinit var engine: AAudioEngine
    private lateinit var statusText: TextView
    private lateinit var infoText: TextView
    private lateinit var startButton: Button
    private lateinit var stopButton: Button
    private lateinit var configSpinner: Spinner

    private var availableConfigs: List<AAudioConfig> = emptyList()
    private var currentConfig: AAudioConfig? = null
    private var spinnerInitialized = false

    private val permissionLauncher =
        registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { result ->
            val denied = result.filterValues { !it }.keys
            if (denied.isEmpty()) {
                startInternal()
            } else {
                statusText.text = messages.failed
                Toast.makeText(requireContext(), "Permission denied: ${denied.joinToString()}", Toast.LENGTH_SHORT).show()
            }
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

        engine = createEngine(requireActivity().applicationContext)
        engine.setListener(object : AAudioEngine.Listener {
            override fun onStarted() {
                activity?.runOnUiThread { updateButtons(true); statusText.text = messages.active; updateInfo() }
            }
            override fun onStopped() {
                activity?.runOnUiThread { updateButtons(false); statusText.text = messages.stopped; updateInfo() }
            }
            override fun onError(error: String) {
                activity?.runOnUiThread { updateButtons(false); showError(error) }
            }
        })

        startButton.setOnClickListener {
            val missing = requiredPermissions().filter {
                ContextCompat.checkSelfPermission(requireContext(), it) != PackageManager.PERMISSION_GRANTED
            }
            if (missing.isNotEmpty()) permissionLauncher.launch(missing.toTypedArray())
            else startInternal()
        }
        stopButton.setOnClickListener { engine.stop() }
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
        val ok = engine.start()
        if (ok) updateButtons(true)
    }

    private fun loadConfigurations() {
        availableConfigs = try {
            AAudioConfig.loadConfigs(requireContext(), section)
        } catch (e: Exception) {
            android.util.Log.e("AAudioTestFragment", "Failed to load $section configurations", e)
            emptyList()
        }
        if (availableConfigs.isNotEmpty()) {
            currentConfig = availableConfigs[0]
            engine.setAudioConfig(currentConfig!!)
            setupSpinner()
            updateInfo()
            statusText.text = messages.ready
        } else {
            statusText.text = messages.failed
            startButton.isEnabled = false
        }
    }

    private fun reloadConfigurations() {
        val prevDesc = currentConfig?.description
        availableConfigs = try {
            AAudioConfig.reloadConfigs(requireContext(), section)
        } catch (e: Exception) {
            android.util.Log.e("AAudioTestFragment", "Failed to reload $section configurations", e)
            emptyList()
        }
        if (availableConfigs.isNotEmpty()) {
            currentConfig = prevDesc?.let { d -> availableConfigs.find { it.description == d } }
                ?: availableConfigs[0]
            engine.setAudioConfig(currentConfig!!)
            spinnerInitialized = false
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

        currentConfig?.let {
            val index = availableConfigs.indexOfFirst { c -> c.description == it.description }
            if (index >= 0) configSpinner.setSelection(index)
        }

        configSpinner.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: AdapterView<*>?, view: View?, position: Int, id: Long) {
                if (!spinnerInitialized) {
                    spinnerInitialized = true
                    return
                }
                val selected = availableConfigs[position]
                currentConfig = selected
                engine.setAudioConfig(selected)
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

    private fun updateInfo() {
        currentConfig?.let { infoText.text = formatInfo(it) } ?: run { infoText.text = "Information" }
    }

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

    override fun onPause() {
        super.onPause()
        engine.stop()
    }

    override fun onDestroy() {
        super.onDestroy()
        if (::engine.isInitialized) engine.release()
    }
}
