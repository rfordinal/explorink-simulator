package org.explorink.simulator;

import android.Manifest;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.graphics.Insets;
import android.os.Build;
import android.os.Bundle;
import android.util.DisplayMetrics;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.MenuItem;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.PopupMenu;
import android.widget.TextView;
import java.util.concurrent.CountDownLatch;
import org.libsdl.app.SDLActivity;

/**
 * Hosts the ExplorInk firmware on a phone.
 *
 * This is not the companion app. org.explorink.gpsbridge talks to a reader over
 * BLE; this package runs the reader's own firmware, compiled for arm64 with the
 * simulator's SDL2 HAL in place of lib/hal.
 *
 * Everything here is chrome around SDL's surface: a size menu, the device's
 * buttons, and the relaunch a firmware wake needs. Nothing touches the
 * firmware or the HAL -- buttons go in as key events through SDL's own
 * onNativeKeyDown, so HalGPIO sees exactly what a keyboard would send.
 */
public class SimulatorActivity extends SDLActivity
        implements BleBridge.StatusListener {

    /** Runtime Bluetooth permissions, Android 12 and up. */
    private static final int REQ_BLUETOOTH = 41;

    /**
     * Panel geometry. Must track the simulator's compiled device profile: the
     * only wired env is X4, 480x800 at 220 PPI (parent repo README.md:130,
     * EInkDisplay.h DISPLAY_WIDTH/HEIGHT). Wrong numbers only make "real size"
     * wrong; the other two modes do not use them.
     */
    private static final int PANEL_W = 480;
    private static final int PANEL_H = 800;
    private static final float PANEL_PPI = 220f;

    private static final String PREFS = "simulator";
    private static final String KEY_MODE = "display_mode";

    /** How big the panel is drawn. SDL letterboxes inside whatever it gets. */
    private enum Mode {
        FIT("Fit the screen"),
        ONE_TO_ONE("1:1 (pixel perfect)"),
        REAL("Real size");

        final String label;
        Mode(String label) { this.label = label; }
    }

    private Mode mode = Mode.FIT;
    private FrameLayout panelHolder;
    private TextView subtitle;
    private TextView bridgeLine;
    private BleBridge bridge;

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "main" };
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        SharedPreferences prefs = getSharedPreferences(PREFS, MODE_PRIVATE);
        try {
            mode = Mode.valueOf(prefs.getString(KEY_MODE, Mode.FIT.name()));
        } catch (IllegalArgumentException ignored) {
            mode = Mode.FIT;
        }

        buildChrome();
        applyMode();
        startBridgeIfConfigured();
    }

    @Override
    protected void onDestroy() {
        if (bridge != null) {
            bridge.stop();
            bridge = null;
        }
        super.onDestroy();
    }

    // ------------------------------------------------------------ ble bridge

    /**
     * The bridge only exists when the shim does: it is off unless
     * CROSSPOINT_SIM_BLE_PORT is set, and that comes from the same sim-env file
     * the native side reads.
     */
    private void startBridgeIfConfigured() {
        int port = SimEnv.blePort(this);
        if (port == 0) {
            return;
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            String[] needed = {
                    Manifest.permission.BLUETOOTH_ADVERTISE,
                    Manifest.permission.BLUETOOTH_CONNECT,
            };
            for (String permission : needed) {
                if (checkSelfPermission(permission) != PackageManager.PERMISSION_GRANTED) {
                    onBridgeStatus("waiting for Bluetooth permission");
                    requestPermissions(needed, REQ_BLUETOOTH);
                    return;
                }
            }
        }
        bridge = new BleBridge(this, port, this);
        bridge.start();
        onBridgeStatus("bridge starting on port " + port);
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions,
            int[] results) {
        super.onRequestPermissionsResult(requestCode, permissions, results);
        if (requestCode != REQ_BLUETOOTH) {
            return;
        }
        for (int result : results) {
            if (result != PackageManager.PERMISSION_GRANTED) {
                onBridgeStatus("Bluetooth permission refused, no bridge");
                return;
            }
        }
        startBridgeIfConfigured();
    }

    @Override
    public void onBridgeStatus(String text) {
        if (bridgeLine == null) {
            return;
        }
        bridgeLine.setVisibility(View.VISIBLE);
        bridgeLine.setText("BLE: " + text);
    }

    // ---------------------------------------------------------------- chrome

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    /**
     * SDLActivity has already called setContentView(mLayout). Lift that layout
     * out and rebuild the screen around it: a bar on top, the panel in the
     * middle, the device's buttons underneath.
     */
    private void buildChrome() {
        if (mLayout.getParent() instanceof ViewGroup) {
            ((ViewGroup) mLayout.getParent()).removeView(mLayout);
        }

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Color.parseColor("#1A1A1A"));

        root.addView(buildTopBar(), new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(44)));

        bridgeLine = new TextView(this);
        bridgeLine.setTextColor(Color.parseColor("#9ECBFF"));
        bridgeLine.setTextSize(11f);
        bridgeLine.setBackgroundColor(Color.parseColor("#1F2A36"));
        bridgeLine.setPadding(dp(12), dp(3), dp(12), dp(3));
        bridgeLine.setVisibility(View.GONE);
        root.addView(bridgeLine, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));

        panelHolder = new FrameLayout(this);
        panelHolder.setBackgroundColor(Color.parseColor("#0E0E0E"));
        LinearLayout.LayoutParams holderParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f);
        root.addView(panelHolder, holderParams);
        panelHolder.addView(mLayout);

        root.addView(buildButtonBar(), new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));

        // Android 15 enforces edge-to-edge for targetSdk 35 and up, so without
        // this the top bar hides behind the status bar and the bottom row of
        // buttons sits under the navigation bar.
        root.setOnApplyWindowInsetsListener((view, insets) -> {
            int top, bottom, left, right;
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                Insets bars = insets.getInsets(WindowInsets.Type.systemBars());
                left = bars.left;
                top = bars.top;
                right = bars.right;
                bottom = bars.bottom;
            } else {
                left = insets.getSystemWindowInsetLeft();
                top = insets.getSystemWindowInsetTop();
                right = insets.getSystemWindowInsetRight();
                bottom = insets.getSystemWindowInsetBottom();
            }
            view.setPadding(left, top, right, bottom);
            return insets;
        });

        setContentView(root);
    }

    private View buildTopBar() {
        LinearLayout bar = new LinearLayout(this);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setGravity(Gravity.CENTER_VERTICAL);
        bar.setBackgroundColor(Color.parseColor("#262626"));
        bar.setPadding(dp(12), 0, dp(4), 0);

        subtitle = new TextView(this);
        subtitle.setTextColor(Color.parseColor("#E0E0E0"));
        subtitle.setTextSize(13f);
        bar.addView(subtitle, new LinearLayout.LayoutParams(
                0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));

        Button menu = new Button(this);
        menu.setText("⋮");
        menu.setTextSize(18f);
        menu.setTextColor(Color.WHITE);
        menu.setBackgroundColor(Color.TRANSPARENT);
        menu.setOnClickListener(this::showMenu);
        bar.addView(menu, new LinearLayout.LayoutParams(dp(48), dp(44)));
        return bar;
    }

    private void showMenu(View anchor) {
        PopupMenu popup = new PopupMenu(this, anchor);
        for (Mode m : Mode.values()) {
            MenuItem item = popup.getMenu().add(m.label);
            item.setCheckable(true);
            item.setChecked(m == mode);
            item.setOnMenuItemClickListener(clicked -> {
                mode = m;
                getSharedPreferences(PREFS, MODE_PRIVATE).edit()
                        .putString(KEY_MODE, m.name()).apply();
                applyMode();
                return true;
            });
        }
        popup.show();
    }

    // ----------------------------------------------------------------- modes

    private void applyMode() {
        DisplayMetrics dm = getResources().getDisplayMetrics();
        FrameLayout.LayoutParams lp;
        String note;

        switch (mode) {
            case ONE_TO_ONE:
                // One panel pixel to one screen pixel. On a dense phone this is
                // physically SMALLER than the device, which is the point of
                // having REAL as well.
                lp = new FrameLayout.LayoutParams(PANEL_W, PANEL_H);
                note = PANEL_W + "x" + PANEL_H + " px";
                break;
            case REAL:
                // Match the panel's physical size. xdpi/ydpi are the screen's
                // real densities, so this only holds as far as the phone
                // reports them honestly -- some do round them.
                int w = Math.round(PANEL_W / PANEL_PPI * dm.xdpi);
                int h = Math.round(PANEL_H / PANEL_PPI * dm.ydpi);
                lp = new FrameLayout.LayoutParams(w, h);
                note = String.format("%.0fx%.0f mm at %.0f dpi",
                        PANEL_W / PANEL_PPI * 25.4f, PANEL_H / PANEL_PPI * 25.4f,
                        dm.xdpi);
                break;
            case FIT:
            default:
                lp = new FrameLayout.LayoutParams(
                        FrameLayout.LayoutParams.MATCH_PARENT,
                        FrameLayout.LayoutParams.MATCH_PARENT);
                note = "fit";
                break;
        }
        lp.gravity = Gravity.CENTER;
        mLayout.setLayoutParams(lp);
        mLayout.requestLayout();
        subtitle.setText(mode.label + "  ·  " + note);
    }

    // --------------------------------------------------------------- buttons

    /**
     * The device's buttons. Each sends the Android keycode SDL translates into
     * the scancode HalGPIO already maps (src/HalGPIO.cpp:35-50), so no firmware
     * or HAL change is involved and a press is indistinguishable from a
     * keyboard press on the desktop simulator.
     */
    private View buildButtonBar() {
        LinearLayout bar = new LinearLayout(this);
        bar.setOrientation(LinearLayout.VERTICAL);
        bar.setBackgroundColor(Color.parseColor("#262626"));
        bar.setPadding(dp(6), dp(6), dp(6), dp(8));

        bar.addView(row(new String[][] {
                { "Back", String.valueOf(KeyEvent.KEYCODE_ESCAPE) },
                { "Left", String.valueOf(KeyEvent.KEYCODE_DPAD_LEFT) },
                { "Right", String.valueOf(KeyEvent.KEYCODE_DPAD_RIGHT) },
                { "Select", String.valueOf(KeyEvent.KEYCODE_ENTER) },
        }));
        bar.addView(row(new String[][] {
                { "Up", String.valueOf(KeyEvent.KEYCODE_DPAD_UP) },
                { "Down", String.valueOf(KeyEvent.KEYCODE_DPAD_DOWN) },
                { "Power", String.valueOf(KeyEvent.KEYCODE_P) },
                { "Sleep", String.valueOf(KeyEvent.KEYCODE_S) },
                { "Home", String.valueOf(KeyEvent.KEYCODE_H) },
        }));
        return bar;
    }

    private View row(String[][] buttons) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        for (String[] spec : buttons) {
            Button b = new Button(this);
            b.setText(spec[0]);
            b.setAllCaps(false);
            b.setTextSize(12f);
            b.setTextColor(Color.WHITE);
            b.setBackgroundColor(Color.parseColor("#3C3C3C"));
            final int keycode = Integer.parseInt(spec[1]);
            b.setOnClickListener(v -> {
                SDLActivity.onNativeKeyDown(keycode);
                SDLActivity.onNativeKeyUp(keycode);
            });
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                    0, dp(44), 1f);
            lp.setMargins(dp(3), dp(3), dp(3), dp(3));
            row.addView(b, lp);
        }
        return row;
    }

    // -------------------------------------------------------------- wake

    /**
     * Called from native when the firmware wakes from its sleep screen.
     *
     * The desktop simulator relaunches itself with execvp, because a native
     * build has no ESP deep-sleep resume path. That cannot work here: SDL sets
     * argv[0] to "app_process", so execvp replaced the app with a bare system
     * binary that died immediately -- and the firmware sleeps on an idle timer,
     * so the app killed itself unattended.
     *
     * Blocks until the intent has been submitted, because the caller calls
     * _exit(0) as soon as this returns.
     */
    @SuppressWarnings("unused") // called through JNI
    public void relaunchForWake() {
        final CountDownLatch submitted = new CountDownLatch(1);
        runOnUiThread(() -> {
            try {
                Intent intent = new Intent(this, SimulatorActivity.class);
                intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK
                        | Intent.FLAG_ACTIVITY_CLEAR_TASK);
                startActivity(intent);
            } finally {
                submitted.countDown();
            }
        });
        try {
            submitted.await();
        } catch (InterruptedException ignored) {
            Thread.currentThread().interrupt();
        }
    }
}
