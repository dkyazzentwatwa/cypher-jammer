/*
  Cypher Jammer -- Serial Terminal Edition
  ----------------------------------------
  Fork of ../cypher-jammer/cypher-jammer.ino that removes the "jams the
  moment you power it on" behavior and replaces it with a line-based serial
  CLI. The intended workflow is:

    * Plug the ESP32 into a phone with a USB-OTG cable.
    * Open a serial terminal app (e.g. Serial USB Terminal by Kai Morich).
    * Connect at 115200 baud, 8N1, no flow control, line ending = newline.
    * Type "help" (or "?") to see everything the firmware exposes.

  Compared to the original single-purpose sketch, having a CLI lets you:
    * start/stop on demand instead of the moment power is applied,
    * pick jamming pattern at runtime (random / sweep / channel-list / fixed),
    * scope activity to Bluetooth, BLE, Wi-Fi, or full 2.4 GHz with presets,
    * change PA level and radio data rate without reflashing,
    * enable one radio at a time (HP-only or SP-only) instead of always both,
    * run for a bounded duration and auto-stop,
    * passively scan the 125-channel band with the nRF24's RPD detector
      before jamming, so you can see which channels are actually busy,
    * persist all of the above in NVS via `save`, and choose whether the
      device auto-starts on next boot with `autostart on`.

  WARNING: Radio jamming is illegal in most jurisdictions. Use this on RF-
  isolated benches, licensed test setups, or engagements where you have
  explicit written authorization. You are responsible for what you transmit.

  Hardware / firmware lineage:
    Cypher Jammer   -- https://github.com/dkyazzentwatwa/cypher-jammer
    Noisy Boy       -- https://github.com/smoochiee/Noisy-boy-esp32-Bluetooth-jammer
*/

#include "RF24.h"
#include <SPI.h>
#include <Preferences.h>
#include <stdarg.h>
#include "esp_bt.h"
#include "esp_wifi.h"

// Wiring is identical to the original board:
//   HSPI = SCK 14, MISO 12, MOSI 13, CS 15, CE 16   -> radio  (HP)
//   VSPI = SCK 18, MISO 19, MOSI 23, CS 21, CE 22   -> radio1 (SP)
SPIClass *spiHP = nullptr;
SPIClass *spiSP = nullptr;
RF24 radio (16, 15, 16000000);
RF24 radio1(22, 21, 16000000);

Preferences prefs;
bool hpOk = false;
bool spOk = false;

enum Mode : uint8_t { MODE_RANDOM = 0, MODE_SWEEP = 1, MODE_LIST = 2, MODE_FIXED = 3 };
enum RadioSel : uint8_t { RADIO_BOTH = 0, RADIO_HP = 1, RADIO_SP = 2 };

static const uint8_t MAX_LIST = 32;
static const uint8_t CH_MAX   = 125;   // nRF24L01+ tops out at 125

struct Config {
  uint8_t mode           = MODE_RANDOM;
  uint8_t radios         = RADIO_BOTH;
  uint8_t pa             = RF24_PA_MAX;
  uint8_t rate           = RF24_2MBPS;
  uint8_t fixedCh        = 45;
  uint8_t minCh          = 0;
  uint8_t maxCh          = 80;
  uint8_t chList[MAX_LIST] = { 2, 26, 80 };  // BLE advertising channels
  uint8_t chListLen      = 3;
  bool    bootAutoStart  = false;
  bool    echo           = true;
  bool    quietWhenJam   = false;
} cfg;

bool     running      = false;
uint32_t stopAtMs     = 0;    // 0 = run until told to stop
int16_t  sweepA       = 45;   // HP position
int16_t  sweepB       = 45;   // SP position
int8_t   sweepAdir    = +2;
int8_t   sweepBdir    = +4;
uint8_t  listIdx      = 0;
uint32_t hopsCount    = 0;
uint32_t startedAtMs  = 0;
String   cmdBuf;

// ---------------------------------------------------------------------------
// Output helpers. When `quietWhenJam` is on we suppress chatter while the
// carrier is up; some Android terminals will otherwise stream lines fast
// enough to visibly hitch the ISR / SPI activity.
// ---------------------------------------------------------------------------
static bool muted() { return cfg.quietWhenJam && running; }

static void say(const char *s) {
  if (!muted()) Serial.println(s);
}

static void sayf(const char *fmt, ...) {
  if (muted()) return;
  char buf[192];
  va_list ap; va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.println(buf);
}

static const char *modeName(uint8_t m) {
  switch (m) {
    case MODE_RANDOM: return "random";
    case MODE_SWEEP:  return "sweep";
    case MODE_LIST:   return "list";
    case MODE_FIXED:  return "fixed";
  }
  return "?";
}

static const char *radiosName(uint8_t r) {
  switch (r) {
    case RADIO_BOTH: return "BOTH";
    case RADIO_HP:   return "HP";
    case RADIO_SP:   return "SP";
  }
  return "?";
}

static const char *paName(uint8_t p) {
  switch (p) {
    case RF24_PA_MIN:  return "MIN";
    case RF24_PA_LOW:  return "LOW";
    case RF24_PA_HIGH: return "HIGH";
    case RF24_PA_MAX:  return "MAX";
  }
  return "?";
}

static const char *rateName(uint8_t r) {
  switch (r) {
    case RF24_1MBPS:   return "1M";
    case RF24_2MBPS:   return "2M";
    case RF24_250KBPS: return "250K";
  }
  return "?";
}

static bool parsePA(const String &s, uint8_t &out) {
  String v = s; v.trim(); v.toLowerCase();
  if (v == "min")  { out = RF24_PA_MIN;  return true; }
  if (v == "low")  { out = RF24_PA_LOW;  return true; }
  if (v == "high") { out = RF24_PA_HIGH; return true; }
  if (v == "max")  { out = RF24_PA_MAX;  return true; }
  return false;
}

static bool parseRate(const String &s, uint8_t &out) {
  String v = s; v.trim(); v.toLowerCase();
  if (v == "1m" || v == "1mbps")       { out = RF24_1MBPS;   return true; }
  if (v == "2m" || v == "2mbps")       { out = RF24_2MBPS;   return true; }
  if (v == "250k" || v == "250kbps")   { out = RF24_250KBPS; return true; }
  return false;
}

static bool parseRadios(const String &s, uint8_t &out) {
  String v = s; v.trim(); v.toLowerCase();
  if (v == "both")            { out = RADIO_BOTH; return true; }
  if (v == "hp" || v == "hspi") { out = RADIO_HP; return true; }
  if (v == "sp" || v == "vspi") { out = RADIO_SP; return true; }
  return false;
}

static bool parseMode(const String &s, uint8_t &out) {
  String v = s; v.trim(); v.toLowerCase();
  if (v == "random") { out = MODE_RANDOM; return true; }
  if (v == "sweep")  { out = MODE_SWEEP;  return true; }
  if (v == "list")   { out = MODE_LIST;   return true; }
  if (v == "fixed")  { out = MODE_FIXED;  return true; }
  return false;
}

static bool parseBool(const String &s, bool &out) {
  String v = s; v.trim(); v.toLowerCase();
  if (v == "on"  || v == "1" || v == "true"  || v == "yes") { out = true;  return true; }
  if (v == "off" || v == "0" || v == "false" || v == "no")  { out = false; return true; }
  return false;
}

static bool useHP() { return (cfg.radios == RADIO_BOTH || cfg.radios == RADIO_HP) && hpOk; }
static bool useSP() { return (cfg.radios == RADIO_BOTH || cfg.radios == RADIO_SP) && spOk; }

// ---------------------------------------------------------------------------
// Radio bring-up. Both radios stay initialized even if only one is in use.
// ---------------------------------------------------------------------------
static void applyCommonRadioSettings(RF24 &r) {
  r.setAutoAck(false);
  r.stopListening();
  r.setRetries(0, 0);
  r.setPALevel((rf24_pa_dbm_e)cfg.pa, true);
  r.setDataRate((rf24_datarate_e)cfg.rate);
  r.setCRCLength(RF24_CRC_DISABLED);
  r.setChannel(cfg.fixedCh);
}

static void initHP() {
  spiHP = new SPIClass(HSPI); spiHP->begin();
  hpOk = radio.begin(spiHP);
  if (hpOk) applyCommonRadioSettings(radio);
  sayf("HP (HSPI CE=16 CS=15) : %s", hpOk ? "ok" : "NOT FOUND");
}

static void initSP() {
  spiSP = new SPIClass(VSPI); spiSP->begin();
  spOk = radio1.begin(spiSP);
  if (spOk) applyCommonRadioSettings(radio1);
  sayf("SP (VSPI CE=22 CS=21) : %s", spOk ? "ok" : "NOT FOUND");
}

static void applyRadioSettingsLive() {
  if (hpOk) { radio.setPALevel((rf24_pa_dbm_e)cfg.pa, true);  radio.setDataRate((rf24_datarate_e)cfg.rate); }
  if (spOk) { radio1.setPALevel((rf24_pa_dbm_e)cfg.pa, true); radio1.setDataRate((rf24_datarate_e)cfg.rate); }
}

// ---------------------------------------------------------------------------
// Jam engine.
// ---------------------------------------------------------------------------
static void startJamming() {
  if (!hpOk && !spOk) { say("no radios detected -- check wiring / power"); return; }
  applyRadioSettingsLive();
  if (useHP()) {
    sweepA = cfg.fixedCh; sweepAdir = +2;
    radio.startConstCarrier((rf24_pa_dbm_e)cfg.pa, cfg.fixedCh);
  }
  if (useSP()) {
    sweepB = cfg.fixedCh; sweepBdir = +4;
    radio1.startConstCarrier((rf24_pa_dbm_e)cfg.pa, cfg.fixedCh);
  }
  listIdx     = 0;
  hopsCount   = 0;
  startedAtMs = millis();
  running     = true;
  sayf("[running] mode=%s radios=%s pa=%s rate=%s range=%u..%u fixed=%u",
       modeName(cfg.mode), radiosName(cfg.radios), paName(cfg.pa), rateName(cfg.rate),
       cfg.minCh, cfg.maxCh, cfg.fixedCh);
}

static void stopJamming() {
  if (hpOk) radio.stopConstCarrier();
  if (spOk) radio1.stopConstCarrier();
  running  = false;
  stopAtMs = 0;
  say("[stopped]");
}

static void jamStep() {
  const uint8_t lo = cfg.minCh;
  const uint8_t hi = cfg.maxCh > cfg.minCh ? cfg.maxCh : cfg.minCh;
  const uint8_t span = (uint8_t)(hi - lo + 1);

  switch (cfg.mode) {
    case MODE_RANDOM: {
      if (useHP()) radio.setChannel(lo + (uint8_t)random(span));
      if (useSP()) radio1.setChannel(lo + (uint8_t)random(span));
      delayMicroseconds(random(60));
      break;
    }
    case MODE_SWEEP: {
      if (useHP()) {
        sweepA += sweepAdir;
        if (sweepA >= hi) { sweepA = hi; sweepAdir = -2; }
        else if (sweepA <= lo) { sweepA = lo; sweepAdir = +2; }
        radio.setChannel((uint8_t)sweepA);
      }
      if (useSP()) {
        sweepB += sweepBdir;
        if (sweepB >= hi) { sweepB = hi; sweepBdir = -4; }
        else if (sweepB <= lo) { sweepB = lo; sweepBdir = +4; }
        radio1.setChannel((uint8_t)sweepB);
      }
      break;
    }
    case MODE_LIST: {
      if (cfg.chListLen == 0) break;
      const uint8_t c = cfg.chList[listIdx];
      if (useHP()) radio.setChannel(c);
      if (useSP()) radio1.setChannel(c);
      listIdx = (uint8_t)((listIdx + 1) % cfg.chListLen);
      break;
    }
    case MODE_FIXED:
    default:
      // startConstCarrier already parked us on cfg.fixedCh
      break;
  }
  hopsCount++;
}

// ---------------------------------------------------------------------------
// Passive scanner. Uses the nRF24's RPD (Received Power Detector) bit, which
// latches when in-band energy above ~-64 dBm was seen since the last read.
// Not a spectrum analyzer -- just a hint at which channels are busy.
// Any active jamming is stopped for the duration of the scan and restarted
// afterwards if it was running when scan started.
// ---------------------------------------------------------------------------
static void doScan(uint8_t passes) {
  if (!hpOk && !spOk) { say("no radios detected"); return; }
  if (passes == 0) passes = 5;

  const bool wasRunning = running;
  const uint8_t savedMode = cfg.mode;
  const uint8_t savedFixed = cfg.fixedCh;

  if (running) stopJamming();

  sayf("scan: %u passes across ch %u..%u (RPD, > -64 dBm)", passes, cfg.minCh, cfg.maxCh);

  uint16_t hits[CH_MAX + 1];
  for (uint16_t i = 0; i <= CH_MAX; i++) hits[i] = 0;

  for (uint8_t pass = 0; pass < passes; pass++) {
    for (uint16_t ch = cfg.minCh; ch <= cfg.maxCh; ch++) {
      bool hit = false;
      if (hpOk && useHP()) {
        radio.setChannel((uint8_t)ch);
        radio.startListening();
        delayMicroseconds(250);
        hit = hit || radio.testRPD();
        radio.stopListening();
      }
      if (spOk && useSP()) {
        radio1.setChannel((uint8_t)ch);
        radio1.startListening();
        delayMicroseconds(250);
        hit = hit || radio1.testRPD();
        radio1.stopListening();
      }
      if (hit) hits[ch]++;
    }
    // Yield to the RTOS between passes so the watchdog doesn't get grumpy.
    delay(1);
  }

  // Render a simple text histogram. Each '#' = one hit across all passes.
  say("ch   hits  bar");
  for (uint16_t ch = cfg.minCh; ch <= cfg.maxCh; ch++) {
    if (hits[ch] == 0) continue;
    char bar[64]; uint8_t n = hits[ch]; if (n > 60) n = 60;
    for (uint8_t i = 0; i < n; i++) bar[i] = '#';
    bar[n] = 0;
    sayf("%3u  %4u  %s", (unsigned)ch, hits[ch], bar);
  }
  say("scan complete");

  // Restore prior config values that scan didn't actually mutate but which
  // callers might expect to be untouched.
  cfg.mode = savedMode;
  cfg.fixedCh = savedFixed;
  if (wasRunning) startJamming();
}

// ---------------------------------------------------------------------------
// NVS persistence.
// ---------------------------------------------------------------------------
static void loadPrefs() {
  prefs.begin("cypherjam", true);
  cfg.mode          = prefs.getUChar("mode",   cfg.mode);
  cfg.radios        = prefs.getUChar("radios", cfg.radios);
  cfg.pa            = prefs.getUChar("pa",     cfg.pa);
  cfg.rate          = prefs.getUChar("rate",   cfg.rate);
  cfg.fixedCh       = prefs.getUChar("fixed",  cfg.fixedCh);
  cfg.minCh         = prefs.getUChar("minch",  cfg.minCh);
  cfg.maxCh         = prefs.getUChar("maxch",  cfg.maxCh);
  cfg.bootAutoStart = prefs.getBool ("boot",   cfg.bootAutoStart);
  cfg.echo          = prefs.getBool ("echo",   cfg.echo);
  cfg.quietWhenJam  = prefs.getBool ("quiet",  cfg.quietWhenJam);
  size_t n = prefs.getBytesLength("chlist");
  if (n > 0 && n <= MAX_LIST) {
    prefs.getBytes("chlist", cfg.chList, n);
    cfg.chListLen = (uint8_t)n;
  }
  prefs.end();
}

static void savePrefs() {
  prefs.begin("cypherjam", false);
  prefs.putUChar("mode",   cfg.mode);
  prefs.putUChar("radios", cfg.radios);
  prefs.putUChar("pa",     cfg.pa);
  prefs.putUChar("rate",   cfg.rate);
  prefs.putUChar("fixed",  cfg.fixedCh);
  prefs.putUChar("minch",  cfg.minCh);
  prefs.putUChar("maxch",  cfg.maxCh);
  prefs.putBool ("boot",   cfg.bootAutoStart);
  prefs.putBool ("echo",   cfg.echo);
  prefs.putBool ("quiet",  cfg.quietWhenJam);
  prefs.putBytes("chlist", cfg.chList, cfg.chListLen);
  prefs.end();
  say("settings saved to NVS");
}

static void clearPrefs() {
  prefs.begin("cypherjam", false);
  prefs.clear();
  prefs.end();
  say("NVS cleared -- defaults will apply on next boot");
}

// ---------------------------------------------------------------------------
// Command dispatch.
// ---------------------------------------------------------------------------
static void printPrompt() {
  if (!muted()) Serial.print(running ? "jam> " : "cj> ");
}

static void printStatus() {
  sayf("state    : %s%s", running ? "RUNNING" : "idle",
       (running && stopAtMs) ? " (timed)" : "");
  sayf("radios   : HP=%s SP=%s  active=%s",
       hpOk ? "ok" : "--", spOk ? "ok" : "--", radiosName(cfg.radios));
  sayf("mode     : %s", modeName(cfg.mode));
  sayf("pa/rate  : %s / %s", paName(cfg.pa), rateName(cfg.rate));
  sayf("range    : %u..%u   fixed=%u", cfg.minCh, cfg.maxCh, cfg.fixedCh);
  String cl; cl.reserve(cfg.chListLen * 4);
  for (uint8_t i = 0; i < cfg.chListLen; i++) {
    if (i) cl += ',';
    cl += String(cfg.chList[i]);
  }
  sayf("chlist   : %s", cl.c_str());
  sayf("autostart: %s   echo=%s   quiet=%s",
       cfg.bootAutoStart ? "on" : "off",
       cfg.echo ? "on" : "off",
       cfg.quietWhenJam ? "on" : "off");
  if (running) {
    uint32_t up = (millis() - startedAtMs) / 1000UL;
    sayf("uptime   : %lus  hops=%lu", (unsigned long)up, (unsigned long)hopsCount);
  }
}

static void printHelp() {
  say("commands:");
  say("  help | ?                    show this list");
  say("  status | info               dump current state");
  say("  start [seconds]             begin jamming (optional auto-stop timer)");
  say("  stop                        stop jamming");
  say("  mode <random|sweep|list|fixed>");
  say("  channel <0..125>            set fixed-mode channel & default start ch");
  say("  channels <c1,c2,...>        set channel list for list-mode (max 32)");
  say("  range <lo> <hi>             bound random/sweep to [lo..hi]");
  say("  preset <ble|bt|wifi|drone|all>");
  say("  pa <min|low|high|max>");
  say("  rate <1m|2m|250k>");
  say("  radios <both|hp|sp>");
  say("  scan [passes]               passive RPD sweep of range (default 5)");
  say("  autostart <on|off>          jam automatically at next boot");
  say("  quiet <on|off>              silence prints while jamming");
  say("  echo <on|off>               echo typed characters");
  say("  save                        persist config to NVS");
  say("  reset                       clear NVS (does not reboot)");
  say("  reboot                      software reset the ESP32");
}

static void applyPreset(const String &name) {
  String n = name; n.trim(); n.toLowerCase();
  if (n == "ble") {
    cfg.mode = MODE_LIST;
    cfg.chList[0] = 2; cfg.chList[1] = 26; cfg.chList[2] = 80;
    cfg.chListLen = 3;
    sayf("preset ble  -> mode=list channels=2,26,80");
  } else if (n == "bt") {
    cfg.mode = MODE_SWEEP; cfg.minCh = 0; cfg.maxCh = 78;
    sayf("preset bt   -> mode=sweep range=0..78");
  } else if (n == "wifi") {
    cfg.mode = MODE_SWEEP; cfg.minCh = 0; cfg.maxCh = 84;
    sayf("preset wifi -> mode=sweep range=0..84");
  } else if (n == "drone") {
    cfg.mode = MODE_RANDOM; cfg.minCh = 0; cfg.maxCh = 125;
    sayf("preset drone-> mode=random range=0..125");
  } else if (n == "all") {
    cfg.mode = MODE_RANDOM; cfg.minCh = 0; cfg.maxCh = 125;
    sayf("preset all  -> mode=random range=0..125");
  } else {
    say("unknown preset. try: ble, bt, wifi, drone, all");
  }
}

static void setChannelList(const String &arg) {
  uint8_t buf[MAX_LIST]; uint8_t n = 0;
  int i = 0, len = arg.length();
  while (i < len && n < MAX_LIST) {
    while (i < len && (arg[i] == ' ' || arg[i] == ',')) i++;
    int start = i;
    while (i < len && arg[i] != ',' && arg[i] != ' ') i++;
    if (i > start) {
      int v = arg.substring(start, i).toInt();
      if (v < 0 || v > CH_MAX) { sayf("channel %d out of range 0..%u", v, CH_MAX); return; }
      buf[n++] = (uint8_t)v;
    }
  }
  if (n == 0) { say("no channels parsed"); return; }
  memcpy(cfg.chList, buf, n);
  cfg.chListLen = n;
  String out; for (uint8_t j = 0; j < n; j++) { if (j) out += ','; out += String(buf[j]); }
  sayf("channel list (%u): %s", (unsigned)n, out.c_str());
}

static void handleCommand(String line) {
  line.trim();
  if (line.length() == 0) { printPrompt(); return; }

  int sp = line.indexOf(' ');
  String cmd  = (sp < 0) ? line : line.substring(0, sp);
  String args = (sp < 0) ? String("") : line.substring(sp + 1);
  cmd.toLowerCase();
  args.trim();

  if (cmd == "help" || cmd == "?") { printHelp(); }
  else if (cmd == "status" || cmd == "info") { printStatus(); }
  else if (cmd == "start") {
    stopAtMs = 0;
    if (args.length()) {
      long secs = args.toInt();
      if (secs > 0) stopAtMs = millis() + (uint32_t)secs * 1000UL;
    }
    startJamming();
    if (stopAtMs) sayf("will auto-stop in %s s", args.c_str());
  }
  else if (cmd == "stop") { if (running) stopJamming(); else say("already stopped"); }
  else if (cmd == "mode") {
    uint8_t m;
    if (!parseMode(args, m)) { say("mode: random|sweep|list|fixed"); }
    else { cfg.mode = m; sayf("mode = %s", modeName(m));
           if (running) { stopJamming(); startJamming(); } }
  }
  else if (cmd == "channel") {
    int v = args.toInt();
    if (v < 0 || v > CH_MAX) { sayf("channel out of range 0..%u", CH_MAX); }
    else { cfg.fixedCh = (uint8_t)v; sayf("fixed channel = %u", (unsigned)v);
           if (running && cfg.mode == MODE_FIXED) { stopJamming(); startJamming(); } }
  }
  else if (cmd == "channels") { setChannelList(args); }
  else if (cmd == "range") {
    int sp2 = args.indexOf(' ');
    if (sp2 < 0) { say("usage: range <lo> <hi>"); }
    else {
      int lo = args.substring(0, sp2).toInt();
      int hi = args.substring(sp2 + 1).toInt();
      if (lo < 0 || hi < 0 || lo > CH_MAX || hi > CH_MAX || lo > hi) {
        sayf("bad range; lo/hi must be 0..%u and lo<=hi", CH_MAX);
      } else {
        cfg.minCh = (uint8_t)lo; cfg.maxCh = (uint8_t)hi;
        sayf("range = %u..%u", (unsigned)lo, (unsigned)hi);
      }
    }
  }
  else if (cmd == "preset")    { applyPreset(args); }
  else if (cmd == "pa")        { uint8_t v; if (!parsePA(args, v)) say("pa: min|low|high|max");
                                  else { cfg.pa = v; applyRadioSettingsLive();
                                         sayf("pa = %s", paName(v));
                                         if (running) { stopJamming(); startJamming(); } } }
  else if (cmd == "rate")      { uint8_t v; if (!parseRate(args, v)) say("rate: 1m|2m|250k");
                                  else { cfg.rate = v; applyRadioSettingsLive();
                                         sayf("rate = %s", rateName(v));
                                         if (running) { stopJamming(); startJamming(); } } }
  else if (cmd == "radios")    { uint8_t v; if (!parseRadios(args, v)) say("radios: both|hp|sp");
                                  else { cfg.radios = v; sayf("radios = %s", radiosName(v));
                                         if (running) { stopJamming(); startJamming(); } } }
  else if (cmd == "scan")      { doScan((uint8_t)args.toInt()); }
  else if (cmd == "autostart") { bool b; if (!parseBool(args, b)) say("autostart: on|off");
                                  else { cfg.bootAutoStart = b; sayf("autostart = %s", b ? "on" : "off"); } }
  else if (cmd == "quiet")     { bool b; if (!parseBool(args, b)) say("quiet: on|off");
                                  else { cfg.quietWhenJam = b; sayf("quiet = %s", b ? "on" : "off"); } }
  else if (cmd == "echo")      { bool b; if (!parseBool(args, b)) say("echo: on|off");
                                  else { cfg.echo = b; sayf("echo = %s", b ? "on" : "off"); } }
  else if (cmd == "save")      { savePrefs(); }
  else if (cmd == "reset")     { clearPrefs(); }
  else if (cmd == "reboot")    { say("rebooting..."); delay(50); ESP.restart(); }
  else {
    sayf("unknown command: '%s' -- try 'help'", cmd.c_str());
  }
  printPrompt();
}

// ---------------------------------------------------------------------------
// Arduino entry points.
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(150);
  Serial.println();
  Serial.println("Cypher Jammer -- Serial Terminal Edition");
  Serial.println("type 'help' for commands.  jamming is illegal in most places -- know your local law.");

  // Shut the ESP32's own Wi-Fi/BT stacks down so they can't stomp on us.
  esp_bt_controller_deinit();
  esp_wifi_stop();
  esp_wifi_deinit();
  esp_wifi_disconnect();

  loadPrefs();
  initHP();
  initSP();
  printStatus();

  if (cfg.bootAutoStart) {
    say("autostart=on -- starting jamming");
    startJamming();
  }
  printPrompt();
}

void loop() {
  // Drain the UART. We do this unconditionally so a user can always stop
  // the jam by typing "stop\n" -- if `quietWhenJam` is on we just don't
  // echo prompts back at them.
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      String line = cmdBuf;
      cmdBuf = "";
      if (cfg.echo && !muted()) Serial.println();
      handleCommand(line);
      continue;
    }
    if (c == 8 || c == 127) {
      if (cmdBuf.length()) {
        cmdBuf.remove(cmdBuf.length() - 1);
        if (cfg.echo && !muted()) Serial.print("\b \b");
      }
      continue;
    }
    if (cmdBuf.length() < 200) {
      cmdBuf += c;
      if (cfg.echo && !muted()) Serial.write(c);
    }
  }

  if (running && stopAtMs && (int32_t)(millis() - stopAtMs) >= 0) {
    stopJamming();
    say("[timer expired]");
    printPrompt();
  }

  if (running) jamStep();
}
