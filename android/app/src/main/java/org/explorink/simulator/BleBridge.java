package org.explorink.simulator;

import android.annotation.SuppressLint;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattDescriptor;
import android.bluetooth.BluetoothGattServer;
import android.bluetooth.BluetoothGattServerCallback;
import android.bluetooth.BluetoothGattService;
import android.bluetooth.BluetoothManager;
import android.bluetooth.BluetoothProfile;
import android.bluetooth.le.AdvertiseCallback;
import android.bluetooth.le.AdvertiseData;
import android.bluetooth.le.AdvertiseSettings;
import android.bluetooth.le.BluetoothLeAdvertiser;
import android.content.Context;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelUuid;
import android.util.Log;
import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import java.util.Map;
import java.util.UUID;
import java.util.concurrent.atomic.AtomicBoolean;
import org.json.JSONArray;
import org.json.JSONObject;

/**
 * A real radio in front of the simulator, on the phone itself.
 *
 * The firmware's BLE code runs against the simulator's NimBLE shim, which is
 * driven over a loopback socket speaking newline-delimited JSON
 * (explorink-simulator, docs/ble-shim.md). This class is the other end of that
 * socket, and an Android GATT server on the far side, so a real central -- the
 * companion app on another phone -- talks to the firmware's own BLE code over
 * actual Bluetooth.
 *
 * It is a translation and nothing else. Every decision it makes for itself is
 * a decision the test stops testing. `tools/blebridge.py` in the parent repo is
 * the same translation against BlueZ on a laptop, and is the reference.
 *
 * Three places where Android is a better peer than BlueZ was:
 *   - the CCCD write arrives with its value, so the real subscribe bit is known
 *     rather than guessed from what the characteristic can do;
 *   - the negotiated MTU arrives in its own callback rather than only as a
 *     side effect of a write;
 *   - notify versus indicate is ours to pick from that CCCD value, which is
 *     what a real peripheral does.
 */
final class BleBridge {

    private static final String TAG = "ExplorInkBridge";

    /** Client Characteristic Configuration. Android does not create it for us. */
    private static final UUID CCCD =
            UUID.fromString("00002902-0000-1000-8000-00805f9b34fb");

    private static final int NIMBLE_READ = 0x0002;
    private static final int NIMBLE_WRITE_NO_RSP = 0x0004;
    private static final int NIMBLE_WRITE = 0x0008;
    private static final int NIMBLE_NOTIFY = 0x0010;
    private static final int NIMBLE_INDICATE = 0x0020;

    /**
     * What the bridge is, not what it last said. The two were the same thing
     * once and it was wrong: the indicator followed log messages, so the
     * firmware's "asked for new conn params" a few seconds after every connect
     * knocked it back to unknown.
     */
    enum State { OFF, ATTACHED, ADVERTISING, CONNECTED }

    interface StatusListener {
        /** Log-shaped, for people reading along. */
        void onBridgeStatus(String status);

        /** The actual state, for anything that draws it. */
        void onBridgeState(State state);
    }

    private final Context context;
    private final int port;
    private final StatusListener listener;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final AtomicBoolean running = new AtomicBoolean(false);

    private Thread thread;
    private Socket socket;
    private OutputStream out;
    private final Object writeLock = new Object();

    private BluetoothGattServer server;
    private BluetoothLeAdvertiser advertiser;
    private AdvertiseCallback advertiseCallback;
    private final Map<UUID, BluetoothGattCharacteristic> chars = new HashMap<>();
    private final Map<UUID, Integer> props = new HashMap<>();
    private final Map<UUID, byte[]> lastValue = new HashMap<>();
    private final Map<UUID, Integer> cccd = new HashMap<>();
    private BluetoothDevice peer;
    private int mtu;
    /**
     * Whether this server has started advertising yet. A central that turns up
     * before it has is not answering our advertisement: it is an ACL link that
     * outlived the previous process, and its GATT state is gone. See
     * dropStaleLink().
     */
    private boolean advertising;
    /**
     * Which characteristic the outstanding indication belongs to.
     * onNotificationSent does not say, and guessing from the subscription set
     * would send a confirm for a characteristic that was never indicated.
     */
    private UUID awaitingConfirm;

    BleBridge(Context context, int port, StatusListener listener) {
        this.context = context.getApplicationContext();
        this.port = port;
        this.listener = listener;
    }

    // ------------------------------------------------------------- lifecycle

    void start() {
        if (!running.compareAndSet(false, true)) {
            return;
        }
        thread = new Thread(this::run, "ble-bridge");
        thread.setDaemon(true);
        thread.start();
    }

    void stop() {
        running.set(false);
        closeSocket();
        main.post(this::teardownGatt);
        Thread t = thread;
        if (t != null) {
            t.interrupt();
        }
    }

    private void status(String text) {
        Log.i(TAG, text);
        if (listener != null) {
            main.post(() -> listener.onBridgeStatus(text));
        }
    }

    private State state = State.OFF;

    private void setState(State next) {
        if (state == next) {
            return;
        }
        state = next;
        if (listener != null) {
            main.post(() -> listener.onBridgeState(next));
        }
    }

    /**
     * The shim only starts listening when the firmware brings BLE up, which
     * happens when the map screen opens -- not at boot. So this retries instead
     * of failing once.
     */
    private void run() {
        while (running.get()) {
            try {
                Socket s = new Socket();
                s.connect(new InetSocketAddress("127.0.0.1", port), 2000);
                s.setTcpNoDelay(true);
                socket = s;
                out = s.getOutputStream();
                setState(State.ATTACHED);
                status("attached to the simulator on port " + port);
                // Before anything else: the shim confirms its own indications
                // by default, and that would make the real peer's confirm
                // timing -- the entire reason for a real radio -- unmeasurable.
                send(obj("op", "auto_confirm", "enabled", false));
                readLoop(s);
            } catch (Exception e) {
                // A refused connection is the normal case before the map opens.
            }
            closeSocket();
            main.post(this::teardownGatt);
            if (!running.get()) {
                break;
            }
            try {
                Thread.sleep(1000);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                break;
            }
        }
        setState(State.OFF);
        status("bridge stopped");
    }

    private void readLoop(Socket s) throws Exception {
        BufferedReader reader = new BufferedReader(
                new InputStreamReader(s.getInputStream(), StandardCharsets.UTF_8));
        String line;
        while (running.get() && (line = reader.readLine()) != null) {
            line = line.trim();
            if (line.isEmpty()) {
                continue;
            }
            try {
                onSimEvent(new JSONObject(line));
            } catch (Exception e) {
                Log.w(TAG, "undecodable line from the simulator: " + line, e);
            }
        }
    }

    private void closeSocket() {
        synchronized (writeLock) {
            out = null;
        }
        Socket s = socket;
        socket = null;
        if (s != null) {
            try {
                s.close();
            } catch (Exception ignored) {
            }
        }
    }

    /** One line, whole, under one lock. */
    private void send(JSONObject message) {
        synchronized (writeLock) {
            OutputStream stream = out;
            if (stream == null) {
                return;
            }
            try {
                stream.write((message.toString() + "\n")
                        .getBytes(StandardCharsets.UTF_8));
                stream.flush();
            } catch (Exception e) {
                Log.w(TAG, "write to the simulator failed", e);
            }
        }
    }

    // -------------------------------------------------- simulator -> phone

    private void onSimEvent(JSONObject ev) throws Exception {
        String kind = ev.optString("ev");
        switch (kind) {
            case "gatt":
                final JSONObject table = ev;
                main.post(() -> buildGatt(table));
                break;
            case "indicate": {
                UUID uuid = UUID.fromString(ev.getString("uuid"));
                byte[] payload = unhex(ev.optString("hex", ""));
                main.post(() -> push(uuid, payload));
                break;
            }
            case "clobber":
                // The firmware overwrote an unconfirmed indication. Real
                // behaviour; the peer never sees the dropped payload.
                status("the firmware dropped an unconfirmed indication");
                break;
            case "stack":
                if ("down".equals(ev.optString("state"))) {
                    main.post(this::teardownGatt);
                }
                break;
            case "connparams_request":
                // A real central decides these. Android gives a peripheral no
                // way to answer, exactly as BlueZ does not, so it is reported
                // and dropped rather than faked.
                status("the firmware asked for new conn params -- not forwardable");
                break;
            case "error":
                status("the simulator refused an op: " + ev.optString("msg"));
                break;
            default:
                break;
        }
    }

    @SuppressLint("MissingPermission")
    private void buildGatt(JSONObject ev) {
        if (server != null) {
            return;
        }
        String serviceUuid = ev.optString("service", "");
        JSONArray list = ev.optJSONArray("chars");
        if (serviceUuid.isEmpty() || list == null || list.length() == 0) {
            status("the simulator's gatt event had nothing usable");
            return;
        }

        BluetoothManager manager = context.getSystemService(BluetoothManager.class);
        BluetoothAdapter adapter = manager != null ? manager.getAdapter() : null;
        if (adapter == null || !adapter.isEnabled()) {
            status("Bluetooth is off -- turn it on");
            return;
        }

        server = manager.openGattServer(context, gattCallback);
        if (server == null) {
            status("could not open a GATT server");
            return;
        }

        BluetoothGattService service = new BluetoothGattService(
                UUID.fromString(serviceUuid), BluetoothGattService.SERVICE_TYPE_PRIMARY);

        for (int i = 0; i < list.length(); i++) {
            JSONObject c = list.optJSONObject(i);
            if (c == null) {
                continue;
            }
            UUID uuid;
            try {
                uuid = UUID.fromString(c.optString("uuid"));
            } catch (Exception e) {
                continue;
            }
            int nimble = c.optInt("props", 0);
            BluetoothGattCharacteristic ch = new BluetoothGattCharacteristic(
                    uuid, androidProps(nimble), androidPermissions(nimble));
            if ((nimble & (NIMBLE_NOTIFY | NIMBLE_INDICATE)) != 0) {
                // Android creates no CCCD of its own. Without it a central has
                // nothing to write and can never subscribe.
                ch.addDescriptor(new BluetoothGattDescriptor(CCCD,
                        BluetoothGattDescriptor.PERMISSION_READ
                                | BluetoothGattDescriptor.PERMISSION_WRITE));
            }
            service.addCharacteristic(ch);
            chars.put(uuid, ch);
            props.put(uuid, nimble);
            Log.i(TAG, "characteristic " + uuid + " nimble props " + nimble);
        }

        server.addService(service);
        startAdvertising(adapter, serviceUuid);
    }

    /** NimBLE's bitmask to Android's, read off the shim's own documented values. */
    private static int androidProps(int nimble) {
        int out = 0;
        if ((nimble & NIMBLE_READ) != 0) {
            out |= BluetoothGattCharacteristic.PROPERTY_READ;
        }
        if ((nimble & NIMBLE_WRITE) != 0) {
            out |= BluetoothGattCharacteristic.PROPERTY_WRITE;
        }
        if ((nimble & NIMBLE_WRITE_NO_RSP) != 0) {
            out |= BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE;
        }
        if ((nimble & NIMBLE_NOTIFY) != 0) {
            out |= BluetoothGattCharacteristic.PROPERTY_NOTIFY;
        }
        if ((nimble & NIMBLE_INDICATE) != 0) {
            out |= BluetoothGattCharacteristic.PROPERTY_INDICATE;
        }
        return out == 0 ? BluetoothGattCharacteristic.PROPERTY_READ : out;
    }

    private static int androidPermissions(int nimble) {
        int out = 0;
        if ((nimble & NIMBLE_READ) != 0) {
            out |= BluetoothGattCharacteristic.PERMISSION_READ;
        }
        if ((nimble & (NIMBLE_WRITE | NIMBLE_WRITE_NO_RSP)) != 0) {
            out |= BluetoothGattCharacteristic.PERMISSION_WRITE;
        }
        return out == 0 ? BluetoothGattCharacteristic.PERMISSION_READ : out;
    }

    @SuppressLint("MissingPermission")
    private void startAdvertising(BluetoothAdapter adapter, String serviceUuid) {
        advertiser = adapter.getBluetoothLeAdvertiser();
        if (advertiser == null) {
            status("this phone cannot advertise");
            return;
        }
        AdvertiseSettings settings = new AdvertiseSettings.Builder()
                .setAdvertiseMode(AdvertiseSettings.ADVERTISE_MODE_LOW_LATENCY)
                .setTxPowerLevel(AdvertiseSettings.ADVERTISE_TX_POWER_HIGH)
                .setConnectable(true)
                .setTimeout(0)
                .build();
        // The service UUID alone is enough: the companion app matches a scan
        // result on name OR advertised service UUID (parent repo
        // docs/ble-bridge.md, "Discovery does not depend on the advertised
        // name"). A 128-bit UUID takes 18 of the 31 advertisement bytes, so the
        // name goes in the scan response instead of overflowing the packet.
        AdvertiseData data = new AdvertiseData.Builder()
                .setIncludeDeviceName(false)
                .addServiceUuid(new ParcelUuid(UUID.fromString(serviceUuid)))
                .build();
        AdvertiseData scanResponse = new AdvertiseData.Builder()
                .setIncludeDeviceName(true)
                .build();
        advertiseCallback = new AdvertiseCallback() {
            @Override
            public void onStartSuccess(AdvertiseSettings settingsInEffect) {
                advertising = true;
                setState(peer != null ? State.CONNECTED : State.ADVERTISING);
                status("advertising " + serviceUuid + " -- the phone can connect");
            }

            @Override
            public void onStartFailure(int errorCode) {
                status("advertising failed, code " + errorCode);
            }
        };
        advertiser.startAdvertising(settings, data, scanResponse, advertiseCallback);
    }

    @SuppressLint("MissingPermission")
    private void teardownGatt() {
        if (advertiser != null && advertiseCallback != null) {
            try {
                advertiser.stopAdvertising(advertiseCallback);
            } catch (Exception ignored) {
            }
        }
        advertiseCallback = null;
        advertiser = null;
        if (server != null) {
            try {
                // Drop the link deliberately. A peer left hanging by a
                // disappearing peripheral wedged the companion app once
                // (parent repo docs/ble-bridge.md, "Traps").
                if (peer != null) {
                    server.cancelConnection(peer);
                }
                server.clearServices();
                server.close();
            } catch (Exception ignored) {
            }
        }
        server = null;
        peer = null;
        advertising = false;
        setState(socket != null ? State.ATTACHED : State.OFF);
        mtu = 0;
        awaitingConfirm = null;
        chars.clear();
        props.clear();
        lastValue.clear();
        cccd.clear();
    }

    /**
     * Killing the app does not drop the Bluetooth link. Both stacks keep the ACL
     * connection alive, so the next GATT server opened on this side is handed
     * the existing central immediately -- before advertising has even started,
     * which is the tell. Observed 2026-08-23: the same peer address came back
     * one millisecond before onStartSuccess, with no CCCD write and no MTU
     * exchange, leaving the firmware believing a central was connected while
     * that central believed it was still subscribed to a GATT table that no
     * longer existed. Indications would have gone nowhere.
     *
     * So a link that predates our advertising is dropped, which makes the peer
     * reconnect and renegotiate properly. Returns true when the link was
     * dropped and nothing should be forwarded for it.
     */
    @SuppressLint("MissingPermission")
    private boolean dropStaleLink(BluetoothDevice device) {
        if (advertising || server == null) {
            return false;
        }
        status("dropping a link left over from a previous run: "
                + safeAddress(device));
        try {
            server.cancelConnection(device);
        } catch (Exception e) {
            Log.w(TAG, "could not drop the stale link", e);
        }
        return true;
    }

    @SuppressLint("MissingPermission")
    private void push(UUID uuid, byte[] payload) {
        BluetoothGattCharacteristic ch = chars.get(uuid);
        if (ch == null || server == null || peer == null) {
            return;
        }
        lastValue.put(uuid, payload);
        ch.setValue(payload);
        // Which one is ours to choose, from the bit the central actually wrote
        // to the CCCD. 2 is indicate, and an indication is what carries a
        // confirm back.
        boolean confirm = (cccd.getOrDefault(uuid, 0) & 0x02) != 0;
        awaitingConfirm = confirm ? uuid : null;
        server.notifyCharacteristicChanged(peer, ch, confirm);
    }

    // -------------------------------------------------- phone -> simulator

    private final BluetoothGattServerCallback gattCallback =
            new BluetoothGattServerCallback() {

        @Override
        public void onConnectionStateChange(BluetoothDevice device, int st, int newState) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                if (dropStaleLink(device)) {
                    return;
                }
                peer = device;
                mtu = 23;
                // The negotiated MTU is not knowable yet, so open with the
                // contract's default and correct it when onMtuChanged fires.
                send(obj("op", "connect", "mtu", 23));
                // The address, because "a central connected" does not say
                // which one, and with several phones on a bench that is
                // exactly the question. Compare against `settings get secure
                // bluetooth_address` on the phone you think it is.
                setState(State.CONNECTED);
                status("a central connected: " + safeAddress(device));
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                setState(advertising ? State.ADVERTISING : State.ATTACHED);
                status("the central left: " + safeAddress(device));
                peer = null;
                cccd.clear();
                send(obj("op", "disconnect", "reason", 0x13));
            }
        }

        @Override
        public void onMtuChanged(BluetoothDevice device, int newMtu) {
            if (newMtu > 0 && newMtu != mtu) {
                mtu = newMtu;
                send(obj("op", "mtu", "mtu", newMtu));
                status("MTU " + newMtu);
            }
        }

        @SuppressLint("MissingPermission")
        @Override
        public void onCharacteristicWriteRequest(BluetoothDevice device, int requestId,
                BluetoothGattCharacteristic characteristic, boolean preparedWrite,
                boolean responseNeeded, int offset, byte[] value) {
            // A write with no response is a different ATT opcode and the
            // firmware's payload arithmetic cares, so pass it through instead
            // of assuming.
            send(obj("op", "write",
                    "uuid", characteristic.getUuid().toString(),
                    "hex", hex(value),
                    "response", responseNeeded));
            if (responseNeeded && server != null) {
                server.sendResponse(device, requestId, BluetoothGatt.GATT_SUCCESS,
                        offset, value);
            }
        }

        @SuppressLint("MissingPermission")
        @Override
        public void onCharacteristicReadRequest(BluetoothDevice device, int requestId,
                int offset, BluetoothGattCharacteristic characteristic) {
            // The wire protocol has no read op and the firmware exposes nothing
            // readable. Answer from the last pushed value rather than erroring.
            byte[] value = lastValue.get(characteristic.getUuid());
            if (value == null) {
                value = new byte[0];
            }
            if (server != null) {
                server.sendResponse(device, requestId, BluetoothGatt.GATT_SUCCESS,
                        offset, value);
            }
        }

        @SuppressLint("MissingPermission")
        @Override
        public void onDescriptorWriteRequest(BluetoothDevice device, int requestId,
                BluetoothGattDescriptor descriptor, boolean preparedWrite,
                boolean responseNeeded, int offset, byte[] value) {
            if (CCCD.equals(descriptor.getUuid())) {
                int bits = (value != null && value.length > 0) ? (value[0] & 0xFF) : 0;
                UUID uuid = descriptor.getCharacteristic().getUuid();
                cccd.put(uuid, bits);
                // The real bit the central wrote, not a guess from what the
                // characteristic can do. BlueZ could not tell us this.
                send(obj("op", "subscribe", "uuid", uuid.toString(),
                        "value", bits));
                status("subscribe " + shortUuid(uuid) + " value " + bits);
            }
            if (responseNeeded && server != null) {
                server.sendResponse(device, requestId, BluetoothGatt.GATT_SUCCESS,
                        offset, value);
            }
        }

        @Override
        public void onNotificationSent(BluetoothDevice device, int st) {
            // For an indication this fires when the peer's ATT confirm arrives.
            // Forwarding it is the whole reason a real radio is worth having:
            // the firmware waits 3000 ms on a semaphore for exactly this.
            UUID uuid = awaitingConfirm;
            awaitingConfirm = null;
            if (st != BluetoothGatt.GATT_SUCCESS || uuid == null) {
                // A plain notification also lands here and carries no confirm.
                return;
            }
            send(obj("op", "confirm", "uuid", uuid.toString()));
        }
    };

    // ------------------------------------------------------------- helpers

    /**
     * JSONObject.put throws a checked exception, which a Bluetooth callback
     * cannot declare. Build here and swallow: a failure would mean a null key,
     * which is a programming error, not a runtime condition.
     */
    private static JSONObject obj(Object... keyValues) {
        JSONObject out = new JSONObject();
        try {
            for (int i = 0; i + 1 < keyValues.length; i += 2) {
                out.put((String) keyValues[i], keyValues[i + 1]);
            }
        } catch (Exception e) {
            Log.w(TAG, "could not build a message", e);
        }
        return out;
    }

    private static String hex(byte[] bytes) {
        if (bytes == null) {
            return "";
        }
        StringBuilder sb = new StringBuilder(bytes.length * 2);
        for (byte b : bytes) {
            sb.append(Character.forDigit((b >> 4) & 0xF, 16));
            sb.append(Character.forDigit(b & 0xF, 16));
        }
        return sb.toString();
    }

    private static byte[] unhex(String s) {
        if (s == null || s.length() % 2 != 0) {
            return new byte[0];
        }
        byte[] out = new byte[s.length() / 2];
        for (int i = 0; i < out.length; i++) {
            out[i] = (byte) Integer.parseInt(s.substring(i * 2, i * 2 + 2), 16);
        }
        return out;
    }

    @SuppressLint("MissingPermission")
    private static String safeAddress(BluetoothDevice device) {
        if (device == null) {
            return "unknown";
        }
        try {
            return device.getAddress();
        } catch (SecurityException e) {
            return "unknown";
        }
    }

    private static String shortUuid(UUID uuid) {
        String s = uuid.toString();
        return s.substring(Math.max(0, s.length() - 4));
    }
}
