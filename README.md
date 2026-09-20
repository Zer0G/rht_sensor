# RHT e-paper sensor

Firmware ESP-IDF per la board Waveshare **ESP32-C6-ePaper-1.54**. Il dispositivo campiona il sensore SHTC3, aggiorna il display e-paper, conserva gli aggregati nella flash e pubblica lo stato su MQTT con auto-discovery per Home Assistant.

## Funzioni implementate

- campionamento di temperatura e umidità ogni `t_sample` secondi (900 predefiniti);
- punto di rugiada calcolato con la formula di Magnus;
- media oraria e aggregati di calendario giornalieri, settimanali, mensili e annuali;
- minimo e massimo di temperatura dalla mezzanotte;
- log circolare con CRC nella partizione flash `stats` da 1 MiB;
- RTC PCF85063 mantenuto in UTC e sincronizzato periodicamente via NTP;
- wake-up da timer o tasto Power (GPIO2);
- pressione di Power durante il funzionamento per entrare subito in deep sleep;
- cambio della pagina storica soltanto tramite pressione breve di BOOT (GPIO9);
- provisioning Wi-Fi e MQTT tramite pressione di 5 secondi su BOOT, SoftAP, QR code e captive portal;
- invio e ricezione MQTT ogni `update_freq * t_sample`, con 0 e 1 equivalenti a ogni campione;
- auto-discovery Home Assistant per temperatura, umidità, punto di rugiada, minimo/massimo giornalieri, batteria, tensione e RSSI;
- finestra console di 30 secondi con un host USB collegato, poi deep sleep;
- console comandi persistente tramite USB Serial/JTAG;
- LED verde acceso durante il ciclo attivo e spento tramite EXIO4 prima del deep sleep.

La misura SHTC3 viene eseguita prima di accendere la radio, così il calore prodotto dal Wi-Fi non altera il campione. L'offset predefinito è `0,00 °C` ed è configurabile da menu. Al primo avvio di questa versione, i record storici della versione precedente vengono migrati una sola volta aggiungendo `6,00 °C` a media, minimo e massimo.

## Configurazione

È richiesto ESP-IDF 5.5 o successivo; il progetto è stato compilato con ESP-IDF 6.1 per target ESP32-C6.

In una shell ESP-IDF:

```powershell
idf.py set-target esp32c6
idf.py menuconfig
```

Nel menu **RHT sensor configuration** impostare:

- URI, utente e password del broker MQTT;
- fuso orario POSIX e server NTP;
- `t_sample` in secondi e moltiplicatore MQTT `update_freq`;
- eventuale correzione di temperatura del sensore.

Il valore predefinito del fuso è quello italiano con ora legale.

SSID e password Wi-Fi possono essere preconfigurati da menu oppure acquisiti dal
portale di provisioning e salvati nella NVS. Per avviare il provisioning tenere
premuto BOOT/GP9 per 5 secondi, scansionare il QR mostrato sull'e-paper e accettare
la connessione alla rete `RHT-xxxxxx`. Il captive portal consente di inserire le
credenziali della rete definitiva e le salva soltanto dopo aver verificato la
connessione. La modalità termina dopo il salvataggio o dopo cinque minuti.

I valori impostati da menu finiscono nel file locale `sdkconfig`, escluso dal
controllo versione. Le credenziali ricevute dal portale hanno precedenza.

### Console USB

Quando il dispositivo e collegato a un computer, aprire la porta seriale a
115200 baud (per esempio con `idf.py monitor`). La console accetta:

```text
show
wifi set "Nome rete" "password wifi"
mqtt set mqtt://192.168.1.10:1883 utente password
wifi clear
mqtt clear
reboot
help
```

I valori tra virgolette possono contenere spazi. Le password non vengono mai
mostrate da `show` e l'input dei comandi non viene ripetuto sul terminale. I
comandi `clear` rimuovono i valori salvati in NVS, facendo
riemergere eventuali valori predefiniti compilati in `sdkconfig`. Le nuove
impostazioni vengono usate dal ciclo di rete successivo e restano memorizzate
dopo spegnimenti e riavvii.

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
5. Ogni `update_freq` campioni viene pubblicato lo stato MQTT e ascoltato brevemente il topic comandi.
6. Il display viene aggiornato e il dispositivo entra in deep sleep; con un host USB collegato lascia prima una finestra console di 30 secondi.

Gli aggregati seguono il calendario nel fuso configurato: la settimana comincia il lunedì. Un'interruzione completa dell'alimentazione può perdere soltanto i campioni dell'ora ancora aperta; tutti i periodi orari già chiusi restano in flash. La capacità attuale è di 32.768 record, circa tre anni e mezzo di storico completo con tutti i livelli di aggregazione.

## MQTT e Home Assistant

Con i valori predefiniti vengono usati:

- stato: `homeassistant/sensor/rht_epaper/state`;
- discovery: `homeassistant/sensor/rht_epaper/<sensore>/config`.
- comandi in ingresso: `homeassistant/sensor/rht_epaper/command`.

Il payload di stato è JSON e contiene `timestamp`, `temperature`, `humidity`, `dew_point`, `day_min`, `day_max`, `battery`, `battery_voltage` espresso in mV, `rssi` e i valori/delta di ora precedente, stessa ora del giorno precedente, settimana, mese e anno precedenti. I messaggi di stato e discovery sono retained con QoS 1; il discovery viene ripubblicato ogni 24 ore e subito dopo un nuovo provisioning.

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
| LED verde | TCA9554 EXIO4, attivo basso (`GP4` non montato) |

Il caricabatterie ETA6098 presente sulla scheda gestisce autonomamente la carica Li-ion. Il firmware calcola una percentuale indicativa dalla tensione a vuoto, con il 100% impostato a 4180 mV. Il pin `STAT` pilota soltanto la sezione rossa del LED bicolore e non è collegato al microcontrollore; per questo il firmware non può distinguere in modo affidabile `charging` e `discharging`. La sezione verde è invece pilotata da EXIO4: segnala il ciclo attivo e viene spenta prima del deep sleep.

Lo schema non porta VBUS a un ingresso del microcontrollore. I pacchetti SOF USB Serial/JTAG permettono di rilevare un host e offrire una finestra console di 30 secondi; scaduta la finestra il dispositivo entra comunque in deep sleep. Un caricatore o power bank privo di dati USB non viene considerato connesso.

Riferimenti: [documentazione Waveshare](https://docs.waveshare.com/ESP32-C6-ePaper-1.54/Resources-And-Documents), [esempi ESP-IDF Waveshare](https://docs.waveshare.com/ESP32-C6-ePaper-1.54/ESP-IDF), [repository ufficiale](https://github.com/waveshareteam/ESP32-C6-ePaper-1.54).
