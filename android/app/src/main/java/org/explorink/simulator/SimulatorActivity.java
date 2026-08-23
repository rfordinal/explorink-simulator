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
import android.util.Log;
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
     * only wired env is X4, 480x800 at 220 PPI (parent repo README.md:137,
     * EInkDisplay.h DISPLAY_WIDTH/HEIGHT).
     *
     * The 220 is the vendor's nominal figure and nobody has measured the panel
     * (parent docs/wallet-plan.md:103 marks it OPEN). REAL mode is therefore
     * accurate to whatever that figure is worth, and three phones agreeing with
     * each other cannot tell you: a wrong figure makes all of them wrong by the
     * same factor. The other two modes do not use it at all.
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
    private View sideKeys;
    private BleBridge bridge;
    /** What applyMode asked for, so a clipped panel can be reported. */
    private int wantedWidth;
    private int wantedHeight;

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
                    Log.i("ExplorInkSimulator", "waiting for Bluetooth permission");
                    requestPermissions(needed, REQ_BLUETOOTH);
                    return;
                }
            }
        }
        bridge = new BleBridge(this, port, this);
        bridge.start();
        Log.i("ExplorInkSimulator", "bridge starting on port " + port);
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
                Log.i("ExplorInkSimulator", "Bluetooth permission refused, no bridge");
                return;
            }
        }
        startBridgeIfConfigured();
    }

    /**
     * Messages are for logcat, not for the indicator. Matching on them was the
     * bug: the firmware asks for new connection parameters a few seconds after
     * every connect, and that message is not a state, so the token fell back to
     * unknown while a central was plainly connected.
     */
    @Override
    public void onBridgeStatus(String text) {
        Log.i("ExplorInkSimulator", text);
    }

    /**
     * A filled dot means a central is on the link, a hollow one advertising and
     * waiting, a dash attached to the simulator with no radio up yet.
     */
    @Override
    public void onBridgeState(BleBridge.State state) {
        if (bridgeLine == null) {
            return;
        }
        String token;
        switch (state) {
            case CONNECTED:   token = "BLE \u25CF"; break;
            case ADVERTISING: token = "BLE \u25CB"; break;
            case ATTACHED:    token = "BLE \u2014"; break;
            case OFF:
            default:          token = null; break;
        }
        if (token == null) {
            bridgeLine.setVisibility(View.GONE);
            return;
        }
        bridgeLine.setVisibility(View.VISIBLE);
        bridgeLine.setText(token);
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


        // The panel, with the page keys laid over its right edge rather than
        // beside it. Beside it they took width the panel needs: in real-size
        // mode there is a fixed amount to give, and a 46dp column left 906 px
        // where 908 were needed, so the panel was quietly clipped. Over the top
        // it costs nothing. The front row still sits underneath, mirroring
        // where the device carries them. X4 is the only wired device profile,
        // so this is X4's arrangement.
        panelHolder = new FrameLayout(this);
        panelHolder.setBackgroundColor(Color.parseColor("#0E0E0E"));
        root.addView(panelHolder, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));
        panelHolder.addView(mLayout);

        FrameLayout.LayoutParams sideParams = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT,
                FrameLayout.LayoutParams.WRAP_CONTENT);
        sideParams.gravity = Gravity.END | Gravity.CENTER_VERTICAL;
        sideKeys = buildSideKeys();
        panelHolder.addView(sideKeys, sideParams);

        root.addView(buildFrontKeys(), new LinearLayout.LayoutParams(
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

        // A token, not a sentence. The full messages go to logcat under
        // ExplorInkBridge; a status line wide enough to hold them was eating
        // screen the panel wants.
        bridgeLine = new TextView(this);
        bridgeLine.setTextColor(Color.parseColor("#9ECBFF"));
        bridgeLine.setTextSize(12f);
        bridgeLine.setVisibility(View.GONE);
        bridgeLine.setPadding(0, 0, dp(8), 0);
        bar.addView(bridgeLine, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));

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
        // Real buttons, rarely pressed. On screen they cost more room than they
        // earn.
        final String[][] rare = {
                { "Power", String.valueOf(KeyEvent.KEYCODE_P) },
                { "Sleep", String.valueOf(KeyEvent.KEYCODE_S) },
                { "Home key (X4 Pro)", String.valueOf(KeyEvent.KEYCODE_H) },
        };
        for (String[] spec : rare) {
            final int keycode = Integer.parseInt(spec[1]);
            popup.getMenu().add(spec[0]).setOnMenuItemClickListener(clicked -> {
                tapKey(keycode);
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
        // FIT centres in the space it is given. A fixed size is placed by the
        // margin computed after layout below, so it anchors left: centring
        // would overflow both edges once it is wider than the space.
        lp.gravity = (mode == Mode.FIT)
                ? Gravity.CENTER
                : (Gravity.START | Gravity.CENTER_VERTICAL);
        wantedWidth = lp.width;
        wantedHeight = lp.height;
        mLayout.setLayoutParams(lp);
        mLayout.requestLayout();
        final String text = mode.label + "  ·  " + note;
        subtitle.setText(text);
        mLayout.post(() -> placePanel(text));
    }

    /**
     * Runs once the panel has a real size, which is the only point at which the
     * space actually available is known.
     *
     * Two things it settles. Saying "real size" over a panel the layout had to
     * clip would be a lie, so a clip is named. And the side keys sit in the
     * strip to the panel's right rather than on top of it: the panel shifts
     * left into whatever room is left, but never past the left edge -- if a
     * fixed size is wider than the space, the keys overlap rather than the
     * panel running off screen.
     */
    private void placePanel(String text) {
        if (wantedWidth <= 0 || wantedHeight <= 0) {
            return;      // FIT: nothing fixed to place or to clip.
        }
        int gotW = mLayout.getWidth();
        int gotH = mLayout.getHeight();
        String suffix = "";
        if (gotW < wantedWidth || gotH < wantedHeight) {
            suffix = "  ·  clipped to " + gotW + "x" + gotH;
        }

        int keysWidth = (sideKeys != null) ? sideKeys.getWidth() : 0;
        int free = panelHolder.getWidth() - keysWidth - gotW;
        int leftMargin = Math.max(0, free / 2);
        if (free < 0) {
            suffix = "  ·  keys overlap, no room";
        }
        ViewGroup.LayoutParams raw = mLayout.getLayoutParams();
        if (raw instanceof FrameLayout.LayoutParams) {
            FrameLayout.LayoutParams panelLp = (FrameLayout.LayoutParams) raw;
            if (panelLp.leftMargin != leftMargin) {
                panelLp.leftMargin = leftMargin;
                mLayout.setLayoutParams(panelLp);
            }
        }
        subtitle.setText(text + suffix);
    }

    // --------------------------------------------------------------- buttons

    /**
     * The device's buttons. Each sends the Android keycode SDL translates into
     * the scancode HalGPIO already maps (src/HalGPIO.cpp:35-50), so no firmware
     * or HAL change is involved and a press is indistinguishable from a
     * keyboard press on the desktop simulator.
     *
     * The rarely-pressed ones -- power, sleep, the X4 Pro home key -- are in the
     * top bar's menu rather than on screen.
     */
    private View buildFrontKeys() {
        LinearLayout bar = new LinearLayout(this);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setBackgroundColor(Color.parseColor("#262626"));
        bar.setPadding(dp(6), dp(6), dp(6), dp(8));
        addKey(bar, "Back", KeyEvent.KEYCODE_ESCAPE, true);
        addKey(bar, "Select", KeyEvent.KEYCODE_ENTER, true);
        addKey(bar, "Left", KeyEvent.KEYCODE_DPAD_LEFT, true);
        addKey(bar, "Right", KeyEvent.KEYCODE_DPAD_RIGHT, true);
        return bar;
    }

    /**
     * The page keys, over the panel's right edge and centred vertically, in the
     * order the device has them: Up above Down. Translucent, because they sit
     * on top of the map rather than next to it.
     */
    private View buildSideKeys() {
        LinearLayout column = new LinearLayout(this);
        column.setOrientation(LinearLayout.VERTICAL);
        column.setGravity(Gravity.CENTER_VERTICAL);
        addKey(column, "Up", KeyEvent.KEYCODE_DPAD_UP, false);
        addKey(column, "Down", KeyEvent.KEYCODE_DPAD_DOWN, false);
        return column;
    }

    private void addKey(LinearLayout parent, String label, int keycode,
            boolean horizontal) {
        Button b = new Button(this);
        b.setText(label);
        b.setAllCaps(false);
        b.setTextSize(12f);
        b.setTextColor(Color.WHITE);
        // The side keys are translucent: they overlay the map, and an opaque
        // block there would hide part of what is being tested.
        b.setBackgroundColor(Color.parseColor(horizontal ? "#3C3C3C" : "#B4303030"));
        b.setPadding(0, 0, 0, 0);
        b.setOnClickListener(v -> tapKey(keycode));

        if (horizontal) {
            LinearLayout.LayoutParams lp =
                    new LinearLayout.LayoutParams(0, dp(38), 1f);
            lp.setMargins(dp(3), dp(3), dp(3), dp(3));
            parent.addView(b, lp);
            return;
        }

        // A side key reads along the edge it sits on, so the text turns with
        // it: rotated counter-clockwise, which puts the baseline against the
        // right edge of the panel.
        //
        // Rotation is applied to the view and turns its box with it, so the
        // button is laid out wide-and-short and wrapped in a holder of the
        // resulting tall-and-narrow size. Rotating the box alone would leave
        // the layout reserving the wrong rectangle.
        FrameLayout holder = new FrameLayout(this);
        FrameLayout.LayoutParams inner = new FrameLayout.LayoutParams(
                dp(72), dp(34));
        inner.gravity = Gravity.CENTER;
        b.setRotation(-90f);
        holder.addView(b, inner);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                dp(34), dp(72));
        lp.setMargins(dp(3), dp(3), dp(3), dp(3));
        parent.addView(holder, lp);
    }

    private void tapKey(int keycode) {
        SDLActivity.onNativeKeyDown(keycode);
        SDLActivity.onNativeKeyUp(keycode);
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
