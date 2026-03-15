package com.example.sensor

import android.Manifest
import android.bluetooth.*
import android.bluetooth.le.*
import android.content.Context
import android.content.pm.PackageManager
import android.graphics.Color
import android.graphics.drawable.GradientDrawable
import android.os.*
import android.widget.*
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import com.google.android.material.textfield.TextInputEditText
import java.util.*

class MainActivity : AppCompatActivity() {

    private val SERVICE_UUID        = UUID.fromString("12345678-1234-1234-1234-123456789abc")
    private val CHAR_GAS_UUID       = UUID.fromString("12345678-1234-1234-1234-123456789ab1")
    private val CHAR_TEMP_UUID      = UUID.fromString("12345678-1234-1234-1234-123456789ab2")
    private val CHAR_HUM_UUID       = UUID.fromString("12345678-1234-1234-1234-123456789ab3")
    private val CHAR_ALERT_UUID     = UUID.fromString("12345678-1234-1234-1234-123456789ab4")
    private val CHAR_THRESHOLD_UUID = UUID.fromString("12345678-1234-1234-1234-123456789ab5")
    private val CHAR_CALIBRATE_UUID = UUID.fromString("12345678-1234-1234-1234-123456789ab6")
    private val CHAR_PHONE_UUID     = UUID.fromString("12345678-1234-1234-1234-123456789ab8")
    private val CCCD_UUID           = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")

    private var bluetoothGatt: BluetoothGatt? = null
    private val handler = Handler(Looper.getMainLooper())
    private val operationQueue: Queue<Runnable> = LinkedList()
    private var isOperationPending = false

    private lateinit var tvStatus: TextView
    private lateinit var tvGas: TextView
    private lateinit var tvTemp: TextView
    private lateinit var tvHum: TextView
    private lateinit var tvAlert: TextView
    private lateinit var etThreshold: EditText
    private lateinit var etSimNumber: TextInputEditText
    private lateinit var btnConnect: Button
    private lateinit var btnSaveSim: Button
    private lateinit var btnSetThreshold: Button
    private lateinit var btnCalibrate: Button

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        tvStatus = findViewById(R.id.tvStatus)
        tvGas = findViewById(R.id.tvGas)
        tvTemp = findViewById(R.id.tvTemp)
        tvHum = findViewById(R.id.tvHum)
        tvAlert = findViewById(R.id.tvAlert)
        etThreshold = findViewById(R.id.etThreshold)
        etSimNumber = findViewById(R.id.etSimNumber)
        btnConnect = findViewById(R.id.btnConnect)
        btnSaveSim = findViewById(R.id.btnSaveSim)
        btnSetThreshold = findViewById(R.id.btnSetThreshold)
        btnCalibrate = findViewById(R.id.btnCalibrate)

        val prefs = getSharedPreferences("SensorPrefs", Context.MODE_PRIVATE)
        etSimNumber.setText(prefs.getString("num", ""))

        btnSaveSim.setOnClickListener {
            val num = etSimNumber.text.toString().trim()
            if (num.isNotEmpty()) {
                enqueueWrite(CHAR_PHONE_UUID, num)
                prefs.edit().putString("num", num).apply()
                Toast.makeText(this, "Number Syncing...", Toast.LENGTH_SHORT).show()
            }
        }

        btnConnect.setOnClickListener { if (bluetoothGatt == null) startScan() else disconnect() }
        btnSetThreshold.setOnClickListener {
            val v = etThreshold.text.toString().trim()
            if (v.isNotEmpty()) enqueueWrite(CHAR_THRESHOLD_UUID, v)
        }
        btnCalibrate.setOnClickListener { enqueueWrite(CHAR_CALIBRATE_UUID, "CAL") }
    }

    private fun startScan() {
        val adapter = (getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager).adapter ?: return
        val scanner = adapter.bluetoothLeScanner
        val filter = ScanFilter.Builder().setDeviceName("AmmoniaMonitor").build()
        val settings = ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build()
        if (ActivityCompat.checkSelfPermission(this, Manifest.permission.BLUETOOTH_SCAN) == PackageManager.PERMISSION_GRANTED) {
            scanner.startScan(listOf(filter), settings, scanCallback)
            btnConnect.text = "Searching..."
        }
    }

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(ct: Int, res: ScanResult) {
            if (ActivityCompat.checkSelfPermission(this@MainActivity, Manifest.permission.BLUETOOTH_SCAN) == PackageManager.PERMISSION_GRANTED) {
                (getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager).adapter.bluetoothLeScanner.stopScan(this)
                res.device.connectGatt(this@MainActivity, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
            }
        }
    }

    private val gattCallback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, s: Int, ns: Int) {
            if (ns == BluetoothProfile.STATE_CONNECTED) {
                bluetoothGatt = g
                handler.postDelayed({ if (ActivityCompat.checkSelfPermission(this@MainActivity, Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED) g.discoverServices() }, 1000)
            } else if (ns == BluetoothProfile.STATE_DISCONNECTED) {
                runOnUiThread { tvStatus.text = "Disconnected"; btnConnect.text = "Connect" }
                bluetoothGatt?.close(); bluetoothGatt = null
            }
        }

        override fun onServicesDiscovered(g: BluetoothGatt, s: Int) {
            val svc = g.getService(SERVICE_UUID) ?: return
            enqueueNotify(svc, CHAR_GAS_UUID); enqueueNotify(svc, CHAR_TEMP_UUID)
            enqueueNotify(svc, CHAR_HUM_UUID); enqueueNotify(svc, CHAR_ALERT_UUID)
            runOnUiThread { tvStatus.text = "Connected"; btnConnect.text = "Disconnect" }
        }

        override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic, v: ByteArray) {
            val str = String(v)
            runOnUiThread {
                when(c.uuid) {
                    CHAR_GAS_UUID -> tvGas.text = str
                    CHAR_TEMP_UUID -> tvTemp.text = "$str°C"
                    CHAR_HUM_UUID -> tvHum.text = "$str%"
                    CHAR_ALERT_UUID -> {
                        val isDanger = str == "1"
                        tvAlert.text = if (isDanger) "STATUS: DANGER" else "STATUS: NORMAL"
                        val drawable = tvAlert.background as GradientDrawable
                        drawable.setColor(if (isDanger) Color.parseColor("#E74C3C") else Color.parseColor("#27AE60"))
                    }
                }
            }
        }

        override fun onCharacteristicWrite(g: BluetoothGatt, c: BluetoothGattCharacteristic, s: Int) { nextOp() }
        override fun onDescriptorWrite(g: BluetoothGatt, d: BluetoothGattDescriptor, s: Int) { nextOp() }
    }

    private fun enqueueNotify(s: BluetoothGattService, u: UUID) {
        val c = s.getCharacteristic(u) ?: return
        operationQueue.add(Runnable {
            if (ActivityCompat.checkSelfPermission(this, Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED) {
                bluetoothGatt?.setCharacteristicNotification(c, true)
                val d = c.getDescriptor(CCCD_UUID)
                d?.let {
                    if (Build.VERSION.SDK_INT >= 33) bluetoothGatt?.writeDescriptor(it, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)
                    else { it.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE; bluetoothGatt?.writeDescriptor(it) }
                }
            }
        })
        process()
    }

    private fun enqueueWrite(u: UUID, v: String) {
        val s = bluetoothGatt?.getService(SERVICE_UUID) ?: return
        val c = s.getCharacteristic(u) ?: return
        operationQueue.add(Runnable {
            if (ActivityCompat.checkSelfPermission(this, Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED) {
                if (Build.VERSION.SDK_INT >= 33) {
                    bluetoothGatt?.writeCharacteristic(
                        c,
                        v.toByteArray(),
                        BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
                    )
                } else {
                    c.value = v.toByteArray()
                    c.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
                    bluetoothGatt?.writeCharacteristic(c)
                }
            }
        })
        process()
    }

    private fun process() { if (!isOperationPending && operationQueue.isNotEmpty()) { isOperationPending = true; operationQueue.poll()?.run() } }
    private fun nextOp() { isOperationPending = false; process() }
    private fun disconnect() { if (ActivityCompat.checkSelfPermission(this, Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED) bluetoothGatt?.disconnect() }
    private fun hasPermissions() = true
    private fun requestPermissions() {}
}