#include <Bonezegei_DHT11.h>
#include <SPI.h>
#include <SD.h>
#include <math.h>
#include <RTClib.h>
#include <avr/wdt.h>
#include <avr/io.h>

// ================= PIN =================

#define DHTPIN1 7
#define DHTPIN2 8

#define PUMP_PIN 6

#define WATER_PIN A0

#define SD_CS 10



// ================= TEMPI =================

#define ENV_INTERVAL 1800000UL   //30 minuti: ogni quanto si rilegge temperatura/umidita e si ricalcola la frequenza
#define LOG_INTERVAL 60000UL     //1 minuto

// Sicurezza indipendente dalla configurazione: pausa minima assoluta
// tra la fine di un'irrigazione e l'inizio della successiva.
// Non e' modificabile da config.txt: protegge la pompa e le radici
// anche in caso di parametri errati nel file di configurazione.
#define MIN_OFF_TIME 60000UL      //1 minuto



// ================= DEFAULT (sovrascrivibili da config.txt) =================

float VPD_START = 1.0;            // soglia di VPD oltre la quale si aumenta la frequenza

float PUMP_ON_MIN = 3.0;          // durata fissa di ogni irrigazione, in minuti

float IRRIG_MIN_PER_HOUR = 7.0;   // frequenza minima (VPD sotto soglia)
float IRRIG_MAX_PER_HOUR = 15.0;  // frequenza massima desiderata (poi comunque limitata da MIN_OFF_TIME)
float IRRIG_SCALE = 3.0;          // quanto aggressivamente la frequenza cresce con il VPD

int WATER_STOP = 250;
int WATER_OK = 320;

unsigned long lastSDCheck = 0;

bool dht1Fault = false;
bool dht2Fault = false;



// ================= SENSORI =================

Bonezegei_DHT11 dht1(DHTPIN1);
Bonezegei_DHT11 dht2(DHTPIN2);

RTC_DS1307 rtc;
bool rtcOK = false;



// ================= VARIABILI =================

float temperature = 25;
float humidity = 60;
float vpd = 1;

float currentIrrigPerHour = 7.0;  // frequenza attualmente in vigore, solo per log

unsigned long pumpOnDuration = 180000UL;  // ms, calcolata da PUMP_ON_MIN
unsigned long cycleInterval = 0;          // ms tra un avvio e il successivo, aggiornata dal VPD
unsigned long nextPumpStart = 0;          // istante (millis) del prossimo avvio pianificato


int waterLevel = 0;


bool pumpON = false;
bool waterBlocked = false;

bool sdOK = false;

bool lastDHTError = false;


unsigned long lastEnv = 0;
unsigned long lastLog = 0;
unsigned long pumpTimer = 0;



// ================= SETUP =================

void setup()
{
  byte resetCause = MCUSR;
  MCUSR = 0;

  Serial.begin(9600);

  if (!rtc.begin())
  {
    Serial.println("RTC NON PRESENTE");
    rtcOK = false;
  }
  else
  {
    rtcOK = true;

    if (!rtc.isrunning())
    {
      Serial.println("RTC AVVIATO");
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }
  }

  pinMode(PUMP_PIN, OUTPUT);
  digitalWrite(PUMP_PIN, LOW);

  dht1.begin();
  dht2.begin();

  inizializzaSD();

  if (sdOK)
  {
    caricaConfig();
    creaFile();
  }

  pumpOnDuration = (unsigned long)(PUMP_ON_MIN * 60000.0);

  if (resetCause & (1 << WDRF))
  {
    evento("WATCHDOG_RESET");
  }

  evento("SYSTEM_START");

  if (!rtcOK)
  {
    evento("RTC_NON_PRESENTE");
  }

  // primo calcolo ambientale: imposta cycleInterval prima di programmare
  // il primo avvio della pompa
  aggiornaAmbiente();

  // il primo ciclo parte dopo una pausa piena, cosi' il sistema si comporta
  // fin da subito come a regime (niente irrigazione "a raffica" all'avvio)
  if (cycleInterval > pumpOnDuration)
    nextPumpStart = millis() + (cycleInterval - pumpOnDuration);
  else
    nextPumpStart = millis() + pumpOnDuration;

  wdt_enable(WDTO_8S);
}



// ================= LOOP =================

void loop()
{
  wdt_reset();

  unsigned long now = millis();

  // aggiornamento ambiente e ricalcolo della frequenza di irrigazione
  if (now - lastEnv >= ENV_INTERVAL)
  {
    aggiornaAmbiente();
    lastEnv = now;
  }

  // controllo SD ogni minuto
  if (now - lastSDCheck >= 60000UL)
  {
    controlloSD();
    lastSDCheck = now;
  }

  controllaAcqua();

  gestionePompa();

  if (now - lastLog >= LOG_INTERVAL)
  {
    salvaLog();
    lastLog = now;
  }
}



// =================================================
// SD
// =================================================


void inizializzaSD()
{

  if (SD.begin(SD_CS))
  {
    sdOK = true;
  }
  else
  {
    sdOK = false;
    Serial.println("SD NON PRESENTE");
  }

}



void controlloSD()
{
  if (sdOK)
    return;

  if (SD.begin(SD_CS))
  {
    sdOK = true;

    creaFile();

    evento("SD_RECOVERED");
  }
}



void creaFile()
{

  if (!sdOK)
    return;



  if (!SD.exists("serra.csv"))
  {

    File f = SD.open("serra.csv", FILE_WRITE);

    f.println("DATE,TIME,TEMP,HUM,VPD,WATER,PUMP,IRRIG_PER_HOUR");

    f.close();

  }



  if (!SD.exists("eventi.csv"))
  {

    File f = SD.open("eventi.csv", FILE_WRITE);

    f.println("DATE,TIME,EVENT");

    f.close();

  }

}



// =================================================
// CONFIG
// =================================================


void caricaConfig()
{

  if (!sdOK)
    return;


  File f = SD.open("config.txt");


  if (!f)
    return;



  while (f.available())
  {

    char buffer[40];

    int i = 0;



    while (f.available())
    {

      char c = f.read();


      if (c == '\n')
        break;


      if (i < 39)
        buffer[i++] = c;

    }


    buffer[i] = '\0';



    if (strncmp(buffer, "VPD_START=", 10) == 0)
      VPD_START = atof(buffer + 10);


    if (strncmp(buffer, "PUMP_ON_MIN=", 12) == 0)
      PUMP_ON_MIN = atof(buffer + 12);


    if (strncmp(buffer, "IRRIG_MIN_PER_HOUR=", 19) == 0)
      IRRIG_MIN_PER_HOUR = atof(buffer + 19);


    if (strncmp(buffer, "IRRIG_MAX_PER_HOUR=", 19) == 0)
      IRRIG_MAX_PER_HOUR = atof(buffer + 19);


    if (strncmp(buffer, "IRRIG_SCALE=", 12) == 0)
      IRRIG_SCALE = atof(buffer + 12);


    if (strncmp(buffer, "WATER_STOP=", 11) == 0)
      WATER_STOP = atoi(buffer + 11);


    if (strncmp(buffer, "WATER_OK=", 9) == 0)
      WATER_OK = atoi(buffer + 9);

  }


  f.close();

}



// =================================================
// AMBIENTE
// =================================================


void aggiornaAmbiente()
{
  bool ok1 = dht1.getData();
  bool ok2 = dht2.getData();

  if (ok1 && ok2)
  {
    if (dht1Fault)
    {
      evento("DHT1_RECOVERED");
      dht1Fault = false;
    }

    if (dht2Fault)
    {
      evento("DHT2_RECOVERED");
      dht2Fault = false;
    }

    temperature =
      (dht1.getTemperature() +
       dht2.getTemperature()) / 2.0;

    humidity =
      (dht1.getHumidity() +
       dht2.getHumidity()) / 2.0;

    lastDHTError = false;
  }

  else if (ok1)
  {
    if (!dht2Fault)
    {
      evento("DHT2_ERROR");
      dht2Fault = true;
    }

    temperature = dht1.getTemperature();
    humidity = dht1.getHumidity();

    lastDHTError = false;
  }

  else if (ok2)
  {
    if (!dht1Fault)
    {
      evento("DHT1_ERROR");
      dht1Fault = true;
    }

    temperature = dht2.getTemperature();
    humidity = dht2.getHumidity();

    lastDHTError = false;
  }

  else
  {
    if (!lastDHTError)
    {
      evento("DHT_BOTH_ERROR");
    }

    dht1Fault = true;
    dht2Fault = true;
    lastDHTError = true;

    return;   // niente sensori validi: si tiene la frequenza gia' in vigore
  }

  float svp =
    0.6108 *
    exp((17.27 * temperature) /
    (temperature + 237.3));

  vpd =
    svp *
    (1 - humidity / 100.0);

  // ---------------------------------------------------------
  // Calcolo della frequenza di irrigazione (irrigazioni/ora)
  // in base al VPD. La durata di ogni irrigazione resta SEMPRE
  // fissa (pumpOnDuration): a variare e' solo quanto spesso parte.
  // ---------------------------------------------------------

  float irrigPerHour;

  if (vpd < VPD_START)
  {
    irrigPerHour = IRRIG_MIN_PER_HOUR;
  }
  else
  {
    irrigPerHour =
      IRRIG_MIN_PER_HOUR +
      IRRIG_SCALE *
      pow(vpd - VPD_START, 1.4);
  }

  // Limite di sicurezza assoluto: la pausa tra due cicli non puo'
  // mai scendere sotto MIN_OFF_TIME, indipendentemente da come e'
  // configurato IRRIG_MAX_PER_HOUR.
  float safetyMaxPerHour =
    3600000.0 / (float)(pumpOnDuration + MIN_OFF_TIME);

  float effectiveMax = IRRIG_MAX_PER_HOUR;
  if (effectiveMax > safetyMaxPerHour)
    effectiveMax = safetyMaxPerHour;

  float effectiveMin = IRRIG_MIN_PER_HOUR;
  if (effectiveMin > effectiveMax)
    effectiveMin = effectiveMax;

  irrigPerHour = constrain(irrigPerHour, effectiveMin, effectiveMax);

  currentIrrigPerHour = irrigPerHour;

  // NOTA IMPORTANTE: qui si aggiorna SOLO cycleInterval.
  // pumpON, pumpTimer e nextPumpStart non vengono mai toccati:
  // se la pompa e' accesa, finisce il suo ciclo regolarmente e
  // il PROSSIMO avvio verra' calcolato con la frequenza aggiornata.
  // Questo elimina qualunque conflitto tra aggiornamento ambientale
  // e gestione della pompa.
  cycleInterval = (unsigned long)(3600000.0 / irrigPerHour);
}
// =================================================
// ACQUA
// =================================================


void controllaAcqua()
{

  static unsigned long timer = 0;

  static long sum = 0;

  static byte samples = 0;



  if (millis() - timer < 50)
    return;



  timer = millis();



  sum += analogRead(WATER_PIN);

  samples++;



  if (samples >= 10)
  {

    waterLevel = sum / 10;


    sum = 0;

    samples = 0;



    // sensore scollegato

    if (waterLevel < 5)
    {

      waterBlocked = true;

      spegniPompa();

      evento("WATER_SENSOR_ERROR");

      return;

    }



    if (waterLevel < WATER_STOP)
    {

      if (!waterBlocked)
        evento("WATER_LOW");


      waterBlocked = true;

      spegniPompa();

    }



    if (waterLevel > WATER_OK)
    {

      if (waterBlocked)
        evento("WATER_OK");


      waterBlocked = false;

    }

  }

}



// =================================================
// POMPA — scheduler indipendente a frequenza variabile
// =================================================


void gestionePompa()
{
  unsigned long now = millis();

  if (waterBlocked)
  {
    // La pompa resta bloccata indipendentemente dalla programmazione.
    // nextPumpStart NON viene modificato: quando l'acqua torna
    // disponibile, il ciclo riparte in base alla programmazione
    // corrente (se il momento previsto e' gia' passato, parte subito).
    return;
  }

  if (pumpON)
  {
    if (now - pumpTimer >= pumpOnDuration)
    {
      spegniPompa();

      // Programma il prossimo avvio usando la frequenza PIU' RECENTE
      // calcolata da aggiornaAmbiente(): se nel frattempo il VPD e'
      // cambiato, il nuovo intervallo si applica subito al ciclo
      // successivo, senza alcun conflitto con quello appena concluso.
      if (cycleInterval > pumpOnDuration)
        nextPumpStart = now + (cycleInterval - pumpOnDuration);
      else
        nextPumpStart = now + pumpOnDuration;
    }

    return;
  }

  if (now >= nextPumpStart)
  {
    accendiPompa();
  }
}


void accendiPompa()
{

  digitalWrite(PUMP_PIN, HIGH);


  pumpON = true;


  pumpTimer = millis();


  evento("PUMP_ON");

}



void spegniPompa()
{

  if (pumpON)
    evento("PUMP_OFF");



  digitalWrite(PUMP_PIN, LOW);


  pumpON = false;

}



// =================================================
// LOG
// =================================================


void stampaDataOra(File &f)
{
  if (!rtcOK)
  {
    f.print("NO_RTC,");
    f.print(millis() / 1000UL);
    return;
  }

  DateTime now = rtc.now();



  if (now.day() < 10)
    f.print("0");

  f.print(now.day());

  f.print("/");



  if (now.month() < 10)
    f.print("0");

  f.print(now.month());

  f.print("/");



  f.print(now.year());


  f.print(",");



  if (now.hour() < 10)
    f.print("0");

  f.print(now.hour());


  f.print(":");



  if (now.minute() < 10)
    f.print("0");

  f.print(now.minute());


  f.print(":");



  if (now.second() < 10)
    f.print("0");

  f.print(now.second());

}


void salvaLog()
{
  if (!sdOK)
    return;

  File f = SD.open("serra.csv", FILE_WRITE);

  if (!f)
  {
    sdOK = false;
    return;
  }

  if (f)
  {

    stampaDataOra(f);

    f.print(",");



    f.print(temperature);

    f.print(",");



    f.print(humidity);

    f.print(",");



    f.print(vpd);

    f.print(",");



    f.print(waterLevel);

    f.print(",");



    f.print(pumpON);

    f.print(",");



    f.println(currentIrrigPerHour);



    f.close();

  }

}



// =================================================
// EVENTI
// =================================================


void evento(const char *msg)
{
  Serial.println(msg);

  if (!sdOK)
    return;

  File f = SD.open("eventi.csv", FILE_WRITE);

  if (!f)
  {
    sdOK = false;
    return;
  }

  stampaDataOra(f);
  f.print(",");
  f.println(msg);

  f.close();
}
