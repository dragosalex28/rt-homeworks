// Semafor pentru pietoni, cu buton, pe ESP32-C3 Super Mini.
//
// Placa este controlerul semaforului; lămpile sunt desenate de dashboard, care
// le aprinde după mesajele "lights" publicate de placă. Patru activități:
//
//   ISR buton BOOT ----+
//                      +--(coada butonului)--> task sporadic
//   command_received --+   (BUTTON)                 |
//                                                   v
//                                         [ starea semaforului ] <-- mutex
//                                            ^          |
//                                            |          v
//                         task periodic de control   task periodic de ieșire
//                         (fazele, T = 100 ms)       (lămpile, T = 500 ms) --> lights
//
// Gata implementate (nu trebuie modificate):
//   config.h         setările: Wi-Fi, broker, STUDENT, pinii, perioadele, duratele fazelor
//   connection.h/cpp Wi-Fi, ora exactă (SNTP), clientul MQTT, mqtt_publish(), epoch_ms()
//
// Așa cum este, schița se conectează la Wi-Fi și la broker și afișează pe
// Serial comenzile primite. Restul îl scrieți voi: TODO 0 ... TODO 3.
//
// Placa: ESP32C3 Dev Module, USB CDC On Boot: Enabled. Serial Monitor: 115200 baud.

#include "config.h"
#include "connection.h"

// Fazele semaforului (vezi tabelul și diagrama din README.md). Numele din
// PHASE_NAMES sunt cele din mesajul "lights", în aceeași ordine ca în enum.
enum Phase { CAR_GREEN, CAR_YELLOW, ALL_RED, PED_GREEN };
const char* const PHASE_NAMES[] = {"CAR_GREEN", "CAR_YELLOW", "ALL_RED", "PED_GREEN"};

// Culorile unei lămpi, cu numele din mesajul "lights".
enum Light { RED, YELLOW, GREEN };
const char* const LIGHT_NAMES[] = {"RED", "YELLOW", "GREEN"};

// TODO 0 (c): STAREA SEMAFORULUI, partajată de taskuri și protejată de un
//   mutex (xSemaphoreCreateMutex). De exemplu un struct cu:
//   - faza curentă
//   - momentul intrării în faza curentă (millis()), din care se calculează
//     cât a durat faza și cât mai are
//   - cererea pietonului (lampa AȘTEPTAȚI): setată de taskul sporadic,
//     ștearsă de taskul de control la intrarea în PED_GREEN
//   Mutexul se ține cât mai puțin: în el doar citiți sau scrieți câmpurile
//   (eventual copiați starea într-o variabilă locală). Fără mqtt_publish(),
//   Serial sau așteptări cât timp țineți mutexul.
//starea partajata a semaforului
struct TrafficState{
    Phase phase;
    uint32_t phaseStartMs;
    bool pedestrianRequest;
};

//faza initiala - verde pt masini
TrafficState trafficState = {CAR_GREEN, 0, false};

//mutex pt protejarea starii comune
SemaphoreHandle_t stateMutex = nullptr;

//Coada evenimentelor de apasare
QueueHandle_t buttonQueue = nullptr;

//fiecare eveniment pastreaza momentul sosirii
struct ButtonEvent{
    uint32_t arrivalMs;
};

//functiile FreeRTOS
void controlTask(void* parameter);
void outputTask(void* parameter);
void buttonTask(void* parameter);

//rutina de intrerupere
void IRAM_ATTR button_isr(void);

// TODO 1: TASK PERIODIC DE CONTROL, cu perioada CONTROL_PERIOD_MS.
//   La fiecare job, cu mutexul luat, decide dacă faza curentă s-a terminat și
//   trece în faza următoare, după tabelul din README.md:
//     CAR_GREEN  -> CAR_YELLOW  după CAR_GREEN_MIN_MS, doar dacă există o cerere
//     CAR_YELLOW -> ALL_RED     după CAR_YELLOW_MS
//     ALL_RED    -> PED_GREEN   după ALL_RED_MS; aici se șterge cererea pietonului
//     PED_GREEN  -> CAR_GREEN   după PED_GREEN_MS
//   La fiecare schimbare de fază notați momentul intrării în faza nouă.
//   Taskul nu publică nimic: doar actualizează starea. După ce eliberează
//   mutexul, afișează pe Serial faza nouă, dacă s-a schimbat.
//   Eliberările respectă grila r_k = r_0 + kT: așteptare absolută (xTaskDelayUntil).
//   Durata unei faze se măsoară cu un timp monoton (millis()), nu cu epoch_ms().
void controlTask(void* parameter){
    TickType_t lastWakeTime = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(CONTROL_PERIOD_MS);

    while(true){
        uint32_t now = millis();
        bool phaseChanged = false;
        Phase newPhase = CAR_GREEN;
    

    //protejez starea partajata
    if(xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE){
        uint32_t elapsed = now - trafficState.phaseStartMs;
        switch(trafficState.phase){
            case CAR_GREEN:
            if(elapsed >= CAR_GREEN_MIN_MS && trafficState.pedestrianRequest){
                trafficState.phase = CAR_YELLOW;
                phaseChanged = true;
            }
            break;

            case CAR_YELLOW:
            if(elapsed >= CAR_YELLOW_MS) {
                trafficState.phase = ALL_RED;
                phaseChanged = true;
            }
            break;

            case ALL_RED:
            if(elapsed >= ALL_RED_MS){
                trafficState.phase = PED_GREEN;
                trafficState.pedestrianRequest = false;
                phaseChanged = true;
            }
            break;

            case PED_GREEN:
            if(elapsed >= PED_GREEN_MS){
                trafficState.phase = CAR_GREEN;
                phaseChanged = true;
            }
            break;
        }

        if(phaseChanged){
            trafficState.phaseStartMs = now;
            newPhase = trafficState.phase;
        }

        xSemaphoreGive(stateMutex);
    }

    //afisez doar cand se schimba faza
    if(phaseChanged){
        Serial.printf("[CONTROL] Faza noua: %s\n", PHASE_NAMES[newPhase]);
    }

    //asteptare absoluta - perioada constanta de 100ms
    xTaskDelayUntil(&lastWakeTime, period);
  }
}

// TODO 2: TASK PERIODIC DE IEȘIRE, cu perioada OUTPUT_PERIOD_MS.
//   La fiecare job:
//   - copiază starea (cu mutexul luat, cât mai scurt)
//   - stabilește culorile celor două lămpi din faza curentă (tabelul din README.md)
//   - calculează secundele rămase din verdele pietonilor: în PED_GREEN,
//     rotunjite în sus (7,2 s rămase -> 8); 0 în celelalte faze
//   - publică: mqtt_publish("lights", "deviceId;timestamp;faza;masini;pietoni;asteptati;secunde")
//     de exemplu "esp32-a1b2c3;1727600000000;PED_GREEN;RED;GREEN;0;8"
//   - aprinde LED-ul de pe placă (LED_PIN, aprins pe LOW) cât timp lampa
//     AȘTEPTAȚI este aprinsă
//   Așteptare absolută (xTaskDelayUntil), ca și la taskul de control.
void outputTask(void *parameter){
    TickType_t lastWakeTime = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(OUTPUT_PERIOD_MS);

    while(true){
        TrafficState currentState = {CAR_GREEN, 0, false};

        //copiez starea partajata folosind mutexul
        if(xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE){
            currentState = trafficState;
            xSemaphoreGive(stateMutex);
        }

        //determin culorile semaforului
        Light carLight = RED;
        Light pedestrianLight = RED;

        switch(currentState.phase){
            case CAR_GREEN:
            carLight = GREEN;
            pedestrianLight = RED;
            break;

            case CAR_YELLOW:
            carLight = YELLOW;
            pedestrianLight = RED;
            break;

            case ALL_RED:
            carLight = RED;
            pedestrianLight = RED;
            break;

            case PED_GREEN:
            carLight = RED;
            pedestrianLight = GREEN;
            break;
        }

        //calculez secundele ramase pt pietoni
        uint32_t secondsRemaining = 0;

        if(currentState.phase == PED_GREEN){
            uint32_t elapsed = millis() - currentState.phaseStartMs;

            if(elapsed < PED_GREEN_MS){
                uint32_t remaining = PED_GREEN_MS - elapsed;
                secondsRemaining = (remaining + 999) / 1000;
            }
        }

        //indicatorul ASTEPTATI
        int waiting = currentState.pedestrianRequest ? 1 : 0;

        //LEDul ESP32 este activ pe LOW
        digitalWrite(LED_PIN, waiting ? LOW : HIGH);
        
        //construiesc mesajul MQTT
        char payload[160];
        snprintf(payload, sizeof(payload), "%s;%llu;%s;%s;%s;%d;%lu", DEVICE_ID, (unsigned long long)epoch_ms(), PHASE_NAMES[currentState.phase], LIGHT_NAMES[carLight], LIGHT_NAMES[pedestrianLight], waiting, (unsigned long)secondsRemaining);

        //public starea semaforului
        mqtt_publish("lights", payload);

        //asteptare absoluta - perioada de 500ms
        xTaskDelayUntil(&lastWakeTime, period);
    }
}

// TODO 3: TASK SPORADIC, tratează apăsările butonului pentru pietoni.
//   Apăsările vin din două surse, în aceeași coadă (coada butonului):
//   - butonul BOOT de pe placă, printr-o rutină de întrerupere (ISR, mai jos)
//   - butonul virtual din dashboard: comanda "BUTTON", pusă în coadă de
//     command_received()
//   Taskul:
//   - așteaptă o apăsare din coada butonului
//   - dacă de la ultima apăsare acceptată au trecut mai puțin de
//     MIN_INTERARRIVAL_MS, o respinge (mesaj pe Serial). Așa se filtrează și
//     contactele imperfecte ale butonului (bounce), care produc mai multe
//     întreruperi la o singură apăsare.
//   - altfel înregistrează cererea pietonului în starea semaforului (cu mutexul)
//     și afișează pe Serial că a acceptat-o
//   Momentul sosirii se ia când apăsarea intră în coadă (în ISR sau în
//   command_received()), nu când o preia taskul, și se pune în elementul cozii.
void buttonTask(void* parameter){
    ButtonEvent event;
    uint32_t lastAcceptedMs = 0;
    bool hasAccepted = false;

    while(true){
        //astept un eveniment din coada
        if(xQueueReceive(buttonQueue, &event, portMAX_DELAY) == pdTRUE) {
            //verific intervalul de la ultima apasare acceptata
            if(hasAccepted && (uint32_t)(event.arrivalMs - lastAcceptedMs) < MIN_INTERARRIVAL_MS) {
                Serial.println("[BUTTON] Apasare respinsa (sub 1000 ms)");
                continue;
            }

            lastAcceptedMs = event.arrivalMs;
            hasAccepted = true;

            //inregistrez cererea pietonului
            if(xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE){
                trafficState.pedestrianRequest = true;
                xSemaphoreGive(stateMutex);
            }

            Serial.println("[BUTTON] Cerere pieton acceptata");
        }
    }
}

// TODO 3: RUTINA DE ÎNTRERUPERE a butonului BOOT, înregistrată în setup() cu
//   attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), button_isr, FALLING).
//   Scurtă: pune o apăsare în coada butonului cu xQueueSendFromISR() și atât.
//   Fără Serial, fără mqtt_publish(), fără mutex. Se declară cu IRAM_ATTR:
//
//   void IRAM_ATTR button_isr(void) { ... }
void IRAM_ATTR button_isr(void){
    ButtonEvent event;
    event.arrivalMs = millis();
    BaseType_t higherPriorityTaskWoken = pdFALSE;
    xQueueSendFromISR(buttonQueue, &event, &higherPriorityTaskWoken);
    if(higherPriorityTaskWoken == pdTRUE){
        portYIELD_FROM_ISR();
    }
}

// Apelată de clientul MQTT pentru fiecare comandă primită (vezi connection.h).
void command_received(const char* text) {
    Serial.printf("[mqtt] comandă primită: %s\n", text);
    // TODO 3: "BUTTON" merge în coada butonului (o apăsare, la fel ca cea din
    //   ISR), fără să vă blocați (timeout 0). Celelalte comenzi se ignoră:
    //   în această temă placa nu are alte comenzi.
    //accept doar comanda BUTTON
    if (strcmp(text, "BUTTON") == 0){
        ButtonEvent event;
        event.arrivalMs = millis();

        //trimit evenimentul in coada fara blocare
        if(xQueueSend(buttonQueue, &event, 0) != pdTRUE){
            Serial.println("[MQTT] Coada butonului este plina!");
        }
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
    Serial.printf("=== tema 03: STUDENT %s, DEVICE_ID %s ===\n", STUDENT, DEVICE_ID);

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH);  // LED stins
    pinMode(BUTTON_PIN, INPUT_PULLUP);

    wifi_connect();
    time_sync();

    // TODO 0 (a): creați mutexul stării (xSemaphoreCreateMutex) și coada
    //   butonului (xQueueCreate). Înainte de mqtt_start(): de acolo încolo pot
    //   sosi comenzi, iar command_received() pune "BUTTON" în coadă.
    //   Notați și momentul intrării în faza inițială (CAR_GREEN).
    stateMutex = xSemaphoreCreateMutex();
    buttonQueue = xQueueCreate(10, sizeof(ButtonEvent));

    if(stateMutex == nullptr || buttonQueue == nullptr){
        Serial.println("[ERROR] Nu s au putut crea resursele FreeRTOS!");
        while(true){
            delay(1000);
        }
    }

    //memorez momentul inceperii fazei initiale
    trafficState.phase = CAR_GREEN;
    trafficState.phaseStartMs = millis();
    trafficState.pedestrianRequest = false;
    Serial.println("[FreeRTOS] Mutex si coada create.");

    mqtt_start();

    // TODO 0 (b): porniți cele trei taskuri (xTaskCreate), apoi înregistrați
    //   întreruperea butonului (attachInterrupt). Alegeți prioritățile între
    //   2 și 19 (peste loopTask, sub taskurile sistemului); pentru cele două
    //   taskuri periodice, după regula Rate Monotonic. Stiva, în octeți: cel puțin 4096.\
    
    //task periodic de control - 100ms
    xTaskCreate(controlTask, "ControlTask", 4096, nullptr, 4, nullptr);
    //task periodic de iesire - 500ms
    xTaskCreate(outputTask, "OutputTask", 4096, nullptr, 3, nullptr);
    //task sporadic pentru buton
    xTaskCreate(buttonTask, "ButtonTask", 4096, nullptr, 5, nullptr);

    //activez intreruperea pe front descendent
    attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), button_isr, FALLING);
}

void loop() {
    // Totul se face în taskuri: loop() nu are nimic de făcut.
    delay(1000);
}
