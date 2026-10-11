package ssatr.service;

import java.util.HashMap;
import java.util.Locale;
import java.util.Map;

/**
 * Ultima stare cunoscută a fiecărui device: ultima temperatură, momentul
 * ultimului mesaj, numărul de alarme etc.
 *
 * Este actualizată de thread-urile MessageProcessor și citită de CommandConsole,
 * deci accesul trebuie sincronizat.
 */
public class Dashboard {

    // TODO 5: alegeți structura de date pentru starea device-urilor
    //   (de exemplu un Map<String, ...> cu cheia deviceId)
    //   și adăugați metodele de actualizare apelate din MessageProcessor.


    private static class DeviceState {
        double temperature = Double.NaN;
        long lastMessage = 0;
        int alarmCount = 0;
    }

    private final Map<String, DeviceState> devices = new HashMap<>();

    // Actualizarea temperaturii
    public synchronized void updateTelemetry(
            String deviceId, double temperature, long timestamp) {

        DeviceState state = devices.computeIfAbsent(
                deviceId, id -> new DeviceState());

        state.temperature = temperature;
        state.lastMessage = timestamp;
    }

    // Actualizarea numarului de alarme
    public synchronized void updateAlarm(
            String deviceId, long timestamp) {

        DeviceState state = devices.computeIfAbsent(
                deviceId, id -> new DeviceState());

        state.alarmCount++;
        state.lastMessage = timestamp;
    }

    // Actualizarea ultimului mesaj de status
    public synchronized void updateStatus(
            String deviceId, long timestamp) {

        DeviceState state = devices.computeIfAbsent(
                deviceId, id -> new DeviceState());

        state.lastMessage = timestamp;
    }

    /** Afișează un tabel cu starea tuturor device-urilor. */
    public void print() {
        // TODO 5: afișați câte un rând pentru fiecare device, de exemplu:
        //   DEVICE     TEMP    ULTIMUL MESAJ   ALARME
        //   device-1   23.41   acum 1 s        2
        System.out.printf("%-15s %-10s %-20s %-10s%n",
                "DEVICE", "TEMP", "ULTIMUL MESAJ", "ALARME");

        for (Map.Entry<String, DeviceState> entry : devices.entrySet()) {

            String deviceId = entry.getKey();
            DeviceState state = entry.getValue();

            String temp = Double.isNaN(state.temperature)
                    ? "-"
                    : String.format(Locale.US, "%.2f", state.temperature);

            long seconds = (System.currentTimeMillis()
                    - state.lastMessage) / 1000;

            String lastMessage = "acum " + seconds + " s";

            System.out.printf("%-15s %-10s %-20s %-10d%n",
                    deviceId, temp, lastMessage, state.alarmCount);
        }
    }
}
