package ssatr.device;

import java.beans.IntrospectionException;
import java.util.Locale;
import java.util.concurrent.BlockingQueue;

/**
 * TASK SPORADIC: tratează alarmele de temperatură.
 *
 * Evenimentele sosesc în coada alarmEvents (puse de taskul periodic).
 * Intervalul minim între sosiri este MIN_INTERARRIVAL_MS: între două eliberări
 * ale taskului (două alarme publicate) trebuie să treacă cel puțin atât.
 * Intervalul este impus de cod: evenimentele care sosesc mai des sunt
 * respinse, ca să nu fie inundat brokerul.
 */
public class SporadicAlarmTask implements Runnable {

    static final long MIN_INTERARRIVAL_MS = 5000;

    private final String deviceId;
    private final MqttConnection mqtt;
    private final BlockingQueue<Double> alarmEvents;

    public SporadicAlarmTask(String deviceId, MqttConnection mqtt, BlockingQueue<Double> alarmEvents) {
        this.deviceId = deviceId;
        this.mqtt = mqtt;
        this.alarmEvents = alarmEvents;
    }

    @Override
    public void run() {
        // TODO 2: implementați taskul sporadic:
        //   - așteptați un eveniment: alarmEvents.take()
        //   - dacă de la ultima eliberare au trecut mai puțin de
        //     MIN_INTERARRIVAL_MS, respingeți evenimentul (afișați un mesaj)
        //   - altfel publicați pe Config.alarmTopic(deviceId) mesajul
        //     "deviceId;timestamp;temperatura;prag"
        //     (pragul curent poate fi transmis împreună cu temperatura sau
        //      citit din PeriodicTelemetryTask; alegeți și justificați în raport)
        //
        // Vezi exemplul exemple/01-taskuri/.../SporadicTaskExample.java

        long lastRelease = 0;

        while(!Thread.currentThread().isInterrupted()){
            try{
                double temperature = alarmEvents.take();
                long now = System.nanoTime();
                if(lastRelease !=0 && now - lastRelease < MIN_INTERARRIVAL_MS * 1_000_000L) {
                    System.out.println("[sporadic] Alarma respinsa: interval prea scurt");
                    continue;
                }

                lastRelease = now;
                long timestamp = System.currentTimeMillis();
                double threshold = 28.0;
                String message = deviceId + ";" + timestamp + ";" + String.format(Locale.US, "%.2f", temperature) + ";" + String.format(Locale.US, "%.2f", threshold);
                mqtt.publish(Config.alarmTopic(deviceId), message);
                System.out.println("[sporadic] Alarma publicata: " + message);
            } catch (InterruptedException e){
                Thread.currentThread().interrupt();
                break;
            }
        }
    }
}
