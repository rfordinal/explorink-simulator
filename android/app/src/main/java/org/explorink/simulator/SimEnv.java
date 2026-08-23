package org.explorink.simulator;

import android.content.Context;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.util.HashMap;
import java.util.Map;

/**
 * Reads the same {@code files/sim-env} file the native side reads
 * (src/SimulatorAndroidEnv.cpp).
 *
 * One file, two readers: the native side turns the lines into environment
 * variables for the firmware, and Java reads whichever it needs for itself --
 * the BLE port, so the bridge knows where the shim is listening. Keeping one
 * source means the port cannot disagree between the two halves.
 */
final class SimEnv {

    private SimEnv() {}

    static Map<String, String> read(Context context) {
        Map<String, String> out = new HashMap<>();
        File file = new File(context.getFilesDir(), "sim-env");
        if (!file.isFile()) {
            return out;
        }
        try (BufferedReader reader = new BufferedReader(new FileReader(file))) {
            String line;
            while ((line = reader.readLine()) != null) {
                line = line.trim();
                if (line.isEmpty() || line.charAt(0) == '#') {
                    continue;
                }
                int eq = line.indexOf('=');
                if (eq <= 0) {
                    continue;
                }
                out.put(line.substring(0, eq), line.substring(eq + 1));
            }
        } catch (Exception ignored) {
            // A missing or unreadable file means "no knobs set", same as the
            // native side treats it.
        }
        return out;
    }

    /** Returns the configured port, or 0 when BLE is off. */
    static int blePort(Context context) {
        String value = read(context).get("CROSSPOINT_SIM_BLE_PORT");
        if (value == null) {
            return 0;
        }
        try {
            int port = Integer.parseInt(value.trim());
            return (port > 0 && port < 65536) ? port : 0;
        } catch (NumberFormatException e) {
            return 0;
        }
    }
}
