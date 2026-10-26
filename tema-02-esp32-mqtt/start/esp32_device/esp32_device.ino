// Device-ul IoT din tema 01, de data aceasta pe un ESP32-C3 Super Mini.
//
// Aceeași logică și același protocol ca device-simulator din tema 01, cu trei
// taskuri FreeRTOS în loc de trei thread-uri Java:
//
//   task periodic --(coada de alarme)--> task sporadic
//        |                                     |
//        +---- telemetry ---> MQTT <--- alarm -+
//                              |
//                          commands --> command_received() --(coada de comenzi)--> task aperiodic ---> status
//
// Gata implementate (nu trebuie modificate):
//   config.h         setările: Wi-Fi, broker, STUDENT, valorile inițiale; DEVICE_ID
//                    se citește din adresa MAC a plăcii
//   connection.h/cpp Wi-Fi, ora exactă (SNTP), clientul MQTT, mqtt_publish(), epoch_ms()
//
// Așa cum este, schița se conectează la Wi-Fi și la broker și afișează pe
// Serial comenzile primite. Taskurile le scrieți voi: TODO 0 ... TODO 3.
// Exemplele de pornire sunt în laboratorul 0 din rt-labs-esp32c3; vezi
// exemple/README.md din folderul temei.
//
// Placa: ESP32C3 Dev Module, USB CDC On Boot: Enabled. Serial Monitor: 115200 baud.

#include "config.h"
#include "connection.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

//structura pt alarme
struct AlarmEvent {
    float temperature;
    float threshold;
};

//structura pt comenzile broker ului MQTT
struct CommandEvent {
    char text[128];
};

//cozile folosite pt comunicarea dintre taskuri
QueueHandle_t alarmQueue = NULL;
QueueHandle_t commandQueue = NULL;

//configuratia curenta a dispozitivului
volatile uint32_t periodMs = PERIOD_MS_INITIAL;
volatile float threshold = THRESHOLD_INITIAL;

// Temperatura cipului, în °C, de la senzorul intern al ESP32-C3.
// Este temperatura siliciului, nu a camerei: imediat după pornirea Wi-Fi-ului
// poate fi în jur de 50 °C, apoi scade în câteva minute spre 35-40 °C.
// Rezoluția este de aproximativ 1 °C. Ca să provocați alarme, puneți pragul
// cu SET_THRESHOLD cu un grad sub valoarea citită.
float read_temperature(void) {
    return temperatureRead();
}


// TODO 1: TASK PERIODIC, cu perioada T (inițial PERIOD_MS_INITIAL).
//   Fiecare job:
//   - citește temperatura: read_temperature()
//   - publică telemetria: mqtt_publish("telemetry", "deviceId;timestamp;temperatura")
//     (timestamp = epoch_ms(), tipărit cu %lld; temperatura cu "%.2f")
//   - dacă temperatura depășește pragul (inițial THRESHOLD_INITIAL), pune
//     un eveniment în coada de alarme, fără să se blocheze (timeout 0).
//     Taskul periodic NU tratează alarma: o tratează taskul sporadic.
//   Eliberările respectă grila r_k = r_0 + kT: folosiți așteptare absolută
//   (xTaskDelayUntil), nu relativă (delay, vTaskDelay), ca să nu apară derivă.
//   Perioada și pragul se schimbă prin comenzi (taskul aperiodic), deci sunt
//   variabile globale volatile, citite la fiecare job.
void periodicTask(void *parameter){
    (void)parameter;
    Serial.println("[FreeRTOS] Task periodic pornit");
    TickType_t lastWakeTime = xTaskGetTickCount();

    for (;;) {
        //citesc configuratia curenta
        uint32_t currentPeriod = periodMs;
        float currentThreshold = threshold;
        //citesc temperatura cipului
        float temperature = read_temperature();
        //obtin timestamp-ul in milisecunde
        int64_t timestamp = epoch_ms();
        //construiesc mesajul de telemetrie
        char message[128];
        snprintf(message, sizeof(message), "%s;%lld;%.2f", DEVICE_ID, (long long)timestamp, temperature);
        //public telemetria prin MQTT
        mqtt_publish("telemetry", message);
        Serial.printf("[Periodic] Temperatura: %.2f C, Prag: %.2f C, Perioada: %lu ms\n", temperature, currentThreshold, (unsigned long)currentPeriod);
        //verific depasirea pragului
        if (temperature > currentThreshold) {
            AlarmEvent event;
            event.temperature = temperature;
            event.threshold = currentThreshold;

            //trimit evenimentul fara blocare
            if (xQueueSend(alarmQueue, &event, 0) != pdPASS) {
                Serial.println("[Periodic] Coada de alarme este plina!");
            }
        }

        //asteptare absoluta pana la urmatoarea eliberare
        TickType_t periodTicks = pdMS_TO_TICKS(currentPeriod);

        xTaskDelayUntil(&lastWakeTime, periodTicks);
    }
}


// TODO 2: TASK SPORADIC, tratează alarmele de temperatură.
//   - așteaptă un eveniment din coada de alarme
//   - dacă de la ultima alarmă publicată au trecut mai puțin de
//     MIN_INTERARRIVAL_MS, respinge evenimentul (afișați un mesaj pe Serial)
//   - altfel publică: mqtt_publish("alarm", "deviceId;timestamp;temperatura;prag")
//   Pentru intervale folosiți un timp monoton, millis(), nu epoch_ms().
void sporadicTask(void *parameter) {
    (void)parameter;
    Serial.println("[FreeRTOS] Task sporadic pornit");
    AlarmEvent event;

    //timpul ultimei alarme publicate
    uint32_t lastAlarmTime = 0;
    bool firstAlarm = true;

    for (;;) {
        //astept un eveniment de alarma
        if (xQueueReceive(alarmQueue, &event, portMAX_DELAY) == pdTRUE) {
            uint32_t currentTime = millis();

            //verific intervalul minim dintre alarme
            if(!firstAlarm && (uint32_t)(currentTime - lastAlarmTime) < MIN_INTERARRIVAL_MS){
                Serial.printf("[Sporadic] Alarma respinsa: interval minim %lu ms\n", (unsigned long)MIN_INTERARRIVAL_MS);
                continue;
            }
            //eveniment acceptat
            firstAlarm = false;
            lastAlarmTime = currentTime;
            //construiesc mesajul de alarma
            char message[128];
            snprintf(message,sizeof(message),"%s;%lld;%.2f;%.2f",DEVICE_ID,(long long)epoch_ms(),event.temperature,event.threshold);
            //public alarma prin broker ul MQTT
            mqtt_publish("alarm",message);

            Serial.printf("[Sporadic] ALARMA publicata: T=%.2f C, prag=%.2f C\n", event.temperature, event.threshold);
        }
    }
}

// TODO 3: TASK APERIODIC, execută comenzile primite de la business service.
//   Comenzile ajung în command_received() (mai jos), care le pune în coada de
//   comenzi; fiecare comandă eliberează un job al taskului:
//     SET_INTERVAL <ms>       schimbă perioada T a taskului periodic
//     SET_THRESHOLD <valoare> schimbă pragul de alarmă
//     RESET                   readuce perioada și pragul la valorile inițiale
//     STATUS                  raportează perioada și pragul curente
//   După fiecare comandă publică o confirmare:
//     mqtt_publish("status", "deviceId;timestamp;OK SET_INTERVAL 1000")
//   O comandă necunoscută sau un parametru invalid nu oprește taskul: se
//   publică "ERROR <motiv>".
void aperiodicTask(void *parameter) {
    (void)parameter;
    Serial.println("[FreeRTOS] Task aperiodic pornit");
    CommandEvent command;
    char response[128];
    char message[192];

    for (;;) {
        //astept o comanda MQTT
        if (xQueueReceive(commandQueue, &command, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        const char *text = command.text;
        Serial.printf("[Aperiodic] Execut: %s\n", text);

        //SET_INTERVAL <ms>
        if(strncmp(text, "SET_INTERVAL ", 13) == 0) {
            const char *valueText = text + 13;
            char *end;
            long value = strtol(valueText, &end, 10);

            //validez parametrul
            if(end == valueText || *end != '\0' || value <=0 || value > 60000 || pdMS_TO_TICKS(value) == 0){
                snprintf(response, sizeof(response), "ERROR SET_INTERVAL invalid");
            } else{
                periodMs = (uint32_t)value;
                snprintf(response, sizeof(response), "OK SET_INTERVAL %lu", (unsigned long)periodMs);
            }
        }

        //SET_THRESHOLD <valoare>
        else if (strncmp(text, "SET_THRESHOLD ", 14) == 0){
            const char *valueText = text + 14;
            char *end;
            float value = strtof(valueText, &end);
            if (end == valueText || *end != '\0' || !isfinite(value)) {
                snprintf(response, sizeof(response), "ERROR SET_THRESHOLD invalid");
        } else{
                threshold = value;
                snprintf(response, sizeof(response), "OK SET_THRESHOLD %.2f", (double)threshold);
            }
        }

        //RESET
        else if (strcmp(text, "RESET") == 0){
            periodMs = PERIOD_MS_INITIAL;
            threshold = THRESHOLD_INITIAL;
            snprintf(response, sizeof(response), "OK RESET");
        }

        //STATUS
        else if (strcmp(text, "STATUS") == 0){
            snprintf(response, sizeof(response), "OK STATUS interval=%lu threshold=%.2f",
            (unsigned long)periodMs,
            (double)threshold);
        }

        //comanda necunoscuta
        else {
            snprintf(response, sizeof(response), "ERROR unknown command");
        }

        //construiesc confirmarea broker ului MQTT
        snprintf(message, sizeof(message), "%s;%lld;%s", DEVICE_ID, (long long)epoch_ms(), response);
        //public confirmarea
        mqtt_publish("status", message);
        Serial.printf("[Aperiodic] Raspuns: %s\n", response);
    }
}

// Apelată de clientul MQTT pentru fiecare comandă primită (vezi connection.h).
void command_received(const char* text) {
    Serial.printf("[mqtt] comandă primită: %s\n", text);
    // TODO 3: puneți comanda în coada de comenzi, fără să vă blocați
    //   (timeout 0). text este valid doar în timpul acestui apel: elementul
    //   cozii trebuie să conțină textul însuși (de exemplu un struct cu
    //   char text[COMMAND_MAX_LEN]), nu un pointer.
    if(commandQueue == NULL || text == NULL){
        Serial.println("[Aperiodic] Coada indisponibila!");
        return;
    }

    CommandEvent command;
    //comanda trebuie sa incapa integral in structura
    size_t length = strlen(text);
    if(length >= sizeof(command.text)){
        Serial.println("[Aperiodic] Comanda prea lunga!");
        return;
    }

    //copiez textul, nu pointerul
    memcpy(command.text, text, length + 1);
    //introduc comanda in coada fara blocare
    if(xQueueSend(commandQueue, &command, 0) != pdPASS){
        Serial.println("[Aperiodic] Coada de comenzi este plina!");
    }
}

void setup() {
    Serial.begin(115200);
    // Fără așteptare la scriere: cu placa la USB, dar fără Serial Monitor deschis,
    // fiecare Serial.printf() ar aștepta până la 2 s să fie citit și ar întârzia
    // taskul care scrie. Așa, textul pe care nu-l citește nimeni se pierde.
    Serial.setTxTimeoutMs(0);
    uint32_t t = millis();
    while (!Serial && millis() - t < 3000) delay(10);  // așteptăm Serial Monitor, cel mult 3 s
    delay(200);
    Serial.println();
    Serial.printf("=== tema 02: STUDENT %s, DEVICE_ID %s ===\n", STUDENT, DEVICE_ID);

    wifi_connect();
    time_sync();

    // TODO 0 (a): creați cele două cozi (xQueueCreate): coada de alarme și
    //   coada de comenzi. Înainte de mqtt_start(): de acolo încolo pot sosi
    //   comenzi, iar command_received() le pune în coada de comenzi.
    alarmQueue = xQueueCreate(10, sizeof(AlarmEvent));
    commandQueue = xQueueCreate(10, sizeof(CommandEvent));
    //verific daca a reusit alocarea memoriei
    if(alarmQueue == NULL || commandQueue == NULL)
    {
        Serial.print("\n[FreeRTOS] Eroare la crearea cozilor!\n");
        while(true) {
        delay(1000);
    }
    }
    Serial.print("\n[FreeRTOS] Cozile au fost create cu succes!\n");

    mqtt_start();

    // TODO 0 (b): porniți cele trei taskuri (xTaskCreate). După mqtt_start(),
    //   ca primele mesaje publicate să nu se piardă. Alegeți prioritățile
    //   între 2 și 19: peste loopTask (1), sub taskurile sistemului pentru
    //   evenimente și Wi-Fi (20, 23). Stiva, în octeți: cel puțin 4096.
    BaseType_t periodicResult = xTaskCreate(periodicTask, "Periodic", 4096, NULL, 5, NULL);
    BaseType_t sporadicResult = xTaskCreate(sporadicTask, "Sporadic", 4096, NULL, 4, NULL);
    BaseType_t aperiodicResult = xTaskCreate(aperiodicTask, "Aperiodic", 4096, NULL, 3, NULL);

    if (periodicResult != pdPASS || sporadicResult != pdPASS || aperiodicResult != pdPASS) {
        Serial.println("[FreeRTOS] Eroare la crearea taskurilor!");
        ESP.restart();
    }

    Serial.println("[FreeRTOS] Toate taskurile au fost create!");
}

void loop() {
    // Totul se face în taskuri: loop() nu are nimic de făcut.
    delay(1000);
}
