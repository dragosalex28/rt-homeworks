package ssatr.service;

import java.util.concurrent.BlockingQueue;

/**
 * Preia mesajele de un anumit tip ("telemetry", "alarm" sau "status") din coada
 * inbox, le salvează și actualizează dashboard-ul.
 *
 * Formatul mesajelor (câmpuri separate prin ";"):
 *   telemetry:  deviceId;timestamp;temperatura
 *   alarm:      deviceId;timestamp;temperatura;prag
 *   status:     deviceId;timestamp;text
 */
public class MessageProcessor implements Runnable {

    private final String kind;
    private final BlockingQueue<String> inbox;
    private final CsvStorage storage;
    private final Dashboard dashboard;

    public MessageProcessor(String kind, BlockingQueue<String> inbox, CsvStorage storage, Dashboard dashboard) {
        this.kind = kind;
        this.inbox = inbox;
        this.storage = storage;
        this.dashboard = dashboard;
    }

    @Override
    public void run() {
        // TODO 4: într-o buclă:
        //   - preluați un mesaj: inbox.take()
        //   - salvați mesajul în fișierul CSV: storage.append(kind, mesaj)
        //     (CsvStorage este gata implementată)
        //   - actualizați dashboard-ul, în funcție de kind
        //   - alarmele și confirmările se afișează imediat în consolă
        //   - un mesaj greșit (format invalid) nu trebuie să oprească thread-ul

        while (!Thread.currentThread().isInterrupted()) {
            try {
                // Preluam mesajul din coada
                String message = inbox.take();

                try {
                    // Separarea campurilor din mesaj
                    String[] parts = message.split(";", -1);

                    // Verificarea campurilor comune
                    if (parts.length < 3 || parts[0].isEmpty()) {
                        throw new IllegalArgumentException("Format invalid");
                    }

                    String deviceId = parts[0];
                    long timestamp = Long.parseLong(parts[1]);

                    // Verificam tipul mesajului
                    switch (kind) {

                        case "telemetry": {
                            if (parts.length != 3) {
                                throw new IllegalArgumentException("Telemetrie invalida");
                            }

                            double temperature = Double.parseDouble(parts[2]);

                            if (!Double.isFinite(temperature)) {
                                throw new IllegalArgumentException("Temperatura invalida");
                            }

                            storage.append(kind, message);
                            dashboard.updateTelemetry(deviceId, temperature, timestamp);
                            break;
                        }

                        case "alarm": {
                            if (parts.length != 4) {
                                throw new IllegalArgumentException("Alarma invalida");
                            }

                            double temperature = Double.parseDouble(parts[2]);
                            double threshold = Double.parseDouble(parts[3]);

                            if (!Double.isFinite(temperature) || !Double.isFinite(threshold)) {
                                throw new IllegalArgumentException("Valori invalide");
                            }

                            storage.append(kind, message);
                            dashboard.updateAlarm(deviceId, timestamp);

                            System.out.println("[ALARM] " + message);
                            break;
                        }

                        case "status": {
                            if (parts.length != 3 || parts[2].isEmpty()) {
                                throw new IllegalArgumentException("Status invalid");
                            }

                            storage.append(kind, message);
                            dashboard.updateStatus(deviceId, timestamp);

                            System.out.println("[STATUS] " + message);
                            break;
                        }

                        default:
                            throw new IllegalArgumentException("Tip mesaj necunoscut");
                    }

                } catch (IllegalArgumentException e) {
                    System.out.println("[ERROR] Mesaj invalid: "
                            + message + " (" + e.getMessage() + ")");
                }

            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                break;
            }
        }
    }
}
