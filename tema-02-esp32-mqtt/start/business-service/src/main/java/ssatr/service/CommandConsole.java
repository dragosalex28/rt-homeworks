package ssatr.service;

import java.util.Scanner;

/**
 * Consola operatorului: citește comenzi de la tastatură.
 *
 *   show                               afișează dashboard-ul
 *   <deviceId> <COMANDA> [parametru]   trimite o comandă unui device, de exemplu
 *                                      device-1 SET_INTERVAL 1000
 *   help                               afișează comenzile
 *   exit                               oprește serviciul
 */
public class CommandConsole implements Runnable {

    private final MqttConnection mqtt;
    private final Dashboard dashboard;

    public CommandConsole(MqttConnection mqtt, Dashboard dashboard) {
        this.mqtt = mqtt;
        this.dashboard = dashboard;
    }

    @Override
    public void run() {
        Scanner scanner = new Scanner(System.in);
        printHelp();

        while (scanner.hasNextLine()) {
            String line = scanner.nextLine().trim();

            if (line.isEmpty()) {
                continue;
            } else if (line.equals("exit")) {
                break;
            } else if (line.equals("help")) {
                printHelp();
            } else if (line.equals("show")) {
                dashboard.print();
            } else {
                sendCommand(line);
            }
        }
    }

    private void sendCommand(String line) {
        // TODO 6: separați deviceId de restul liniei
        //   ("device-1 SET_INTERVAL 1000" -> "device-1" și "SET_INTERVAL 1000")
        //   și publicați comanda pe Config.commandTopic(deviceId).
        //   Afișați un mesaj de eroare dacă linia nu are formatul corect.

        // Separarea deviceId de comanda
        String[] parts = line.trim().split("\\s+", 2);

        // Verificarea formatului
        if (parts.length != 2 || parts[1].isEmpty()) {
            System.out.println("ERROR: Format invalid!");
            return;
        }

        String deviceId = parts[0];
        String command = parts[1];

        // Verificarea comenzii
        String[] commandParts = command.split("\\s+");
        String commandName = commandParts[0];

        switch (commandName) {
            case "SET_INTERVAL":
            case "SET_THRESHOLD":
                if (commandParts.length != 2) {
                    System.out.println("ERROR: Parametru invalid!");
                    return;
                }
                break;

            case "RESET":
            case "STATUS":
                if (commandParts.length != 1) {
                    System.out.println("ERROR: Comanda nu necesita parametru!");
                    return;
                }
                break;

            default:
                System.out.println("ERROR: Comanda necunoscuta!");
                return;
        }

        // Publicarea comenzii prin MQTT
        mqtt.publish(Config.commandTopic(deviceId), command);

        System.out.println("Comanda trimisa catre "
                + deviceId + ": " + command);
    }

    private void printHelp() {
        System.out.println("Comenzi: show | <deviceId> SET_INTERVAL <ms> | <deviceId> SET_THRESHOLD <valoare>");
        System.out.println("         <deviceId> RESET | <deviceId> STATUS | help | exit");
    }
}
