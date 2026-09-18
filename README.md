# RHT e-paper sensor

Firmware ESP-IDF per la board Waveshare **ESP32-C6-ePaper-1.54**. Il dispositivo campiona il sensore SHTC3, aggiorna il display e-paper, conserva gli aggregati nella flash e pubblica lo stato su MQTT con auto-discovery per Home Assistant.

## Funzioni implementate

- campionamento di temperatura e umidità ogni 60-120 secondi;
- punto di rugiada calcolato con la formula di Magnus;
- media oraria e aggregati di calendario giornalieri, settimanali, mensili e annuali;
- minimo e massimo di temperatura dalla mezzanotte;
- log circolare con CRC nella partizione flash `stats` da 1 MiB;
- RTC PCF85063 mantenuto in UTC e sincronizzato periodicamente via NTP;
- wake-up da timer, tasto Power (GPIO2) e tasto BOOT (GPIO9);
- rotazione della pagina delta a ogni wake-up temporizzato oppure premendo BOOT;
- invio MQTT al primo campione di ogni ora o quando la temperatura cambia di oltre la soglia configurata;
- auto-discovery Home Assistant per temperatura, umidità, punto di rugiada, minimo/massimo giornalieri, batteria, tensione e RSSI;
- spegnimento delle alimentazioni commutate e deep sleep al termine del ciclo.

La misura SHTC3 viene eseguita prima di accendere la radio, così il calore prodotto dal Wi-Fi non altera il campione. L'offset iniziale è `-6,00 °C`, come nell'esempio Waveshare, ed è configurabile da menu.

## Configurazione

È richiesto ESP-IDF 5.5 o successivo; il progetto è stato compilato con ESP-IDF 6.1 per target ESP32-C6.

In una shell ESP-IDF:

```powershell
idf.py set-target esp32c6
idf.py menuconfig
```

Nel menu **RHT sensor configuration** impostare almeno:

- SSID e password Wi-Fi;
- URI, utente e password del broker MQTT;
- fuso orario POSIX e server NTP;
- intervallo di campionamento e soglia di trasmissione;
- eventuale correzione di temperatura del sensore.

Il valore predefinito del fuso è quello italiano con ora legale. Le credenziali finiscono nel file locale `sdkconfig`, escluso dal controllo versione.

Compilazione e programmazione:

```powershell
idf.py build
idf.py -p COMx flash monitor
```

Se sulla scheda era presente un firmware con una tabella partizioni diversa e il primo avvio segnala errori di partizione, eseguire una sola volta `idf.py erase-flash` prima del flash. Questa operazione cancella anche lo storico.

## Ciclo di funzionamento

1. Il wake-up legge RTC, SHTC3 e tensione batteria.
2. Se necessario, il Wi-Fi sincronizza NTP e aggiorna il PCF85063.
3. Il campione viene aggiunto all'accumulatore orario conservato nella memoria RTC dell'ESP32-C6.
4. Al cambio dell'ora l'aggregato viene scritto nella flash; da questo vengono prodotti gli aggregati di periodo più lunghi.
5. Se è scattato un nuovo intervallo orario o il delta supera la soglia, viene pubblicato lo stato MQTT.
6. Il display viene aggiornato e il dispositivo torna in deep sleep.

Gli aggregati seguono il calendario nel fuso configurato: la settimana comincia il lunedì. Un'interruzione completa dell'alimentazione può perdere soltanto i campioni dell'ora ancora aperta; tutti i periodi orari già chiusi restano in flash. La capacità attuale è di 32.768 record, circa tre anni e mezzo di storico completo con tutti i livelli di aggregazione.

## MQTT e Home Assistant

Con i valori predefiniti vengono usati:

- stato: `homeassistant/sensor/rht_epaper/state`;
- discovery: `homeassistant/sensor/rht_epaper/<sensore>/config`.

Il payload di stato è JSON e contiene `timestamp`, `temperature`, `humidity`, `dew_point`, `day_min`, `day_max`, `battery`, `battery_voltage` e `rssi`. I messaggi di stato e discovery sono retained con QoS 1; il discovery viene ripubblicato ogni 24 ore.

## Note hardware

La mappatura deriva dallo schema della board e dagli esempi ufficiali:

| Funzione | Pin / indirizzo |
|---|---:|
| I2C SDA / SCL | GPIO18 / GPIO8 |
| SHTC3 | `0x70` |
| PCF85063 RTC | `0x51` |
| TCA9554 | `0x20` |
| e-paper MOSI / SCLK / CS | GPIO5 / GPIO6 / GPIO7 |
| e-paper DC / BUSY / RESET | GPIO15 / GPIO10 / GPIO11 |
| misura batteria | ADC1 CH0, GPIO0, partitore 1:2 |
| tasto Power / BOOT | GPIO2 / GPIO9 |

Il caricabatterie ETA6098 presente sulla scheda gestisce autonomamente la carica Li-ion; non espone al microcontrollore una regolazione della corrente o il segnale `STAT`. Il firmware abilita solo il partitore di misura tramite TCA9554, calcola una percentuale indicativa dalla tensione a vuoto e disabilita il partitore prima del deep sleep.

Riferimenti: [documentazione Waveshare](https://docs.waveshare.com/ESP32-C6-ePaper-1.54/Resources-And-Documents), [esempi ESP-IDF Waveshare](https://docs.waveshare.com/ESP32-C6-ePaper-1.54/ESP-IDF), [repository ufficiale](https://github.com/waveshareteam/ESP32-C6-ePaper-1.54).
