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
import java.util.concurrent.Executor
import java.util.concurrent.Executors

/**
 * 抽象基类：共享全部 UI 接线（状态/按钮/Spinner/信息区/权限/互斥）。
 */
abstract class AAudioTestFragment : Fragment() {

    protected abstract fun createEngine(context: Context, engineExecutor: Executor): AAudioEngine
    protected abstract val section: String                       // "player" / "recorder"
    protected abstract val messages: AAudioMessages
    protected abstract fun requiredPermissions(): Array<String>
    protected abstract fun formatInfo(config: AAudioConfig): String
    protected abstract fun friendlyErrorMessage(raw: String): String

    private lateinit var engine: AAudioEngine

    // native 层为全局单例状态：所有触碰 native 的调用（setAudioConfig/start/stop/release）
    // 经此单线程串行执行，既消除并发竞争，也避免阻塞主线程
    private val engineExecutor = Executors.newSingleThreadExecutor()
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

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // engine 绑定 Fragment 生命周期：view 重建不换实例，原生全局态无新旧实例交叠
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
                // 错误后主动清理残留资源（ERROR 态 stop 可进入：关流、join 线程、回填 WAV 头）；
                // 会话仍健康时（如 "Already active" 提示）不清理
                if (!engine.isActive()) engineExecutor.execute { engine.stop() }
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
        startButton.isEnabled = false  // 防双击；按钮状态由 onStarted/onError 回调恢复
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
        val prevDesc = currentConfig?.description
        availableConfigs = AAudioConfig.loadConfigs(requireContext(), section)
        if (availableConfigs.isNotEmpty()) {
            currentConfig = prevDesc?.let { d -> availableConfigs.find { it.description == d } }
                ?: availableConfigs[0]
            engineExecutor.execute { engine.setAudioConfig(currentConfig!!) }
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
        engineExecutor.execute { engine.stop() }
    }

    override fun onDestroy() {
        super.onDestroy()
        // FIFO：排在未完成的 stop 之后，原生资源（GlobalRef 等）最终释放
        engineExecutor.execute { engine.release() }
        engineExecutor.shutdown()
    }
}

/** 各特性的状态文案。 */
data class AAudioMessages(
    val ready: String,
    val preparing: String,
    val active: String,
    val stopped: String,
    val failed: String,
)
