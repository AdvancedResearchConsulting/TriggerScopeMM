/*************************************************************
 * TriggerScope Mini — MicroManager-Compatible Firmware
 * Board:   STM32F072CB (48 MHz Cortex-M0, 16 KB RAM)
 * DAC:     AD5684RARUZ (4-ch, 12-bit, SPI)
 *
 * Protocol-compatible with the TriggerScope MM driver in
 * MicroManager.  Unsupported commands (DAC 5-16, pin group 1,
 * voltage range changes) return success as no-ops so the
 * driver never sees an error.
 *
 * Hardware:
 *   DAC A-D   via SPI1 (PA5=SCK, PA7=MOSI, PA4=CS)
 *   TTL 1-4   PB7, PB8, PB9, PB10
 *   TRIG 1    PB0 (input)
 *   READY LED PB11
 *   CONN LED  PB12
 *
 * Copyright (c) 2026, Advanced Research Consulting
 *************************************************************/

#include <SPI.h>

/* ==================== Pin Definitions ==================== */
#define READY_LED PB11
#define CONN_LED  PB12
#define TTLOUT_D  PB5      // Level shifter direction: MCU→External
#define TTLIN_D   PB6      // Level shifter direction: External→MCU
#define DAC_CS    PA4      // AD5684R SYNC pin (GPIOA pin 4)
#define DAC_GAIN  PB1      // GAIN pin: LOW=1x (0–VREF), HIGH=2x
#define DAC_RESET PB2      // *RESET pin: active low

#define TTL1 PB7
#define TTL2 PB8
#define TTL3 PB9
#define TTL4 PB10
#define TRIG1 PB0          // Trigger input

/* ==================== AD5684R Constants ==================== */
// Channel addresses (bitmask, NOT sequential)
#define DAC_CH_A   0x1
#define DAC_CH_B   0x2
#define DAC_CH_C   0x4
#define DAC_CH_D   0x8
#define DAC_CH_ALL 0xF

// Commands
#define DAC_CMD_NOP          0x0
#define DAC_CMD_WRITE_REG    0x1
#define DAC_CMD_UPDATE_DAC   0x2
#define DAC_CMD_WRITE_UPDATE 0x3
#define DAC_CMD_POWER        0x4
#define DAC_CMD_LDAC_MASK    0x5
#define DAC_CMD_RESET        0x6
#define DAC_CMD_REF_SETUP    0x7

// Fast CS via BSRR — single-cycle atomic set/clear on PA4
#define CS_LOW()   (GPIOA->BSRR = (1 << (4 + 16)))
#define CS_HIGH()  (GPIOA->BSRR = (1 << 4))

/* ==================== Protocol Constants ==================== */
#define NR_DACS       4       // AD5684R has 4 channels
#define NR_DAC_STATES 250     // Sequence depth (fits in 16 KB RAM)
#define NR_DO_STATES  250     // TTL sequence depth

static const char sep = '-';

// MicroManager DAC index (0-based) → AD5684R channel address
// MM DAC 1 (index 0) → Channel B, MM DAC 2 (index 1) → Channel A
// MM DAC 3 (index 2) → Channel C, MM DAC 4 (index 3) → Channel D
static const uint8_t dacChannelMap[NR_DACS] = { DAC_CH_B, DAC_CH_A, DAC_CH_C, DAC_CH_D };

// TTL pin array for pin-group 0 (only group on Mini)
static const uint8_t ttlPins[4] = { PB7, PB8, PB9, PB10 };

/* ==================== ID & Help Strings ==================== */
static const char* idname = "ARC TRIGGERSCOPE MINI v.100-MM";

static const char* helpString =
  "Available commands: \r\n"
  "SAOn-s - sets DAC. n=1-4, s=value 0-65535\r\n"
  "PANn - queries programmable analog output states, n=1-4\r\n"
  "PAOn-s-o1-o2-on - sends analog output sequence, n=1-4\r\n"
  "PACn - clears analog sequence, n=1-4\r\n"
  "PASn-s-t - start/stop analog sequence, n=1-4, s=0/1, t=0/1\r\n"
  "BAOn-s-t - analog blanking, n=1-4, s=0/1, t=0/1\r\n"
  "BADn-t - analog blank delay, n=1-4, t=microseconds\r\n"
  "BALn-t - analog blank duration, n=1-4, t=microseconds\r\n"
  "SARn-s - set range (no-op on Mini, fixed 0-5V)\r\n"
  "SDOn-s - set digital output, n=0, s=bitmask 0-255\r\n"
  "PDNn - queries programmable digital output states, n=0\r\n"
  "PDOn-s-o1-o2-on - sends digital output sequence, n=0\r\n"
  "PDCn - clears digital sequence, n=0\r\n"
  "PDSn-s-t - start/stop digital sequence, n=0, s=0/1, t=0/1\r\n"
  "BDOn-s-t - digital blanking, n=0, s=0/1, t=0/1\r\n"
  "SSLn - signal LEDs (no-op on Mini)\r\n"
  "\r\n";

/* ==================== Error Strings ==================== */
static const char* saoErrorString = "!ERROR_SAO: Format: SAOn-s n=1-16 (DAC1-16), s=value 0-65535";
static const char* sarErrorString = "!ERROR_SAR: Format: SARn-s n=1-16 (DAC=1-16), s=1:0-5V 2:0-10V 3:-5-+5V 4:-10-+10V 5:-2-+2V";
static const char* panErrorString = "!ERROR_PAN: Format: PANn  n=1-16 (DAC1-16)";
static const char* paoErrorString = "!ERROR_PAO: Format: PAOn-s-01-02-0n n=1-16 (DAC1-16), s= >=0 (position), 0n=values 0-65535";
static const char* pacErrorString = "!ERROR_PAC: Format: PACn n=1-16 (DAC1-16)";
static const char* pasErrorString = "!ERROR_PAS: Format: PASn-s-t n=1-16 (DAC1-16) s 0=stop 1=start, t=transition on falling(0) or rising(1) edge";
static const char* baoErrorString = "!ERROR_BAO: Format: BAOn-s-t n=1-16 (DAC1-16), s blank 0(off) or 1(on), t 0 (blank on low) or 1 (blank on high)";
static const char* badErrorString = "!ERROR_BAD: Format: BADn-t n=1-16 (DAC1-16), t = 0 - 2147483647";
static const char* balErrorString = "!ERROR_BAL: Format: BALn-t n=1-16 (DAC1-16), t = 0 - 2147483647";
static const char* sdoErrorString = "!ERROR_SDO: Format: SDOn-s n=pingroup 0-1, s=value 0-255";
static const char* pdnErrorString = "!ERROR_PDN: Format: PDNn n=pingroup 0-1";
static const char* pdoErrorString = "!ERROR_PDO: Format: PDOn-s-01-02-0n n=pingroup 0-1, s=position, 0n=values 0-255";
static const char* pdcErrorString = "!ERROR_PDC: Format: PDCn n=pinGroup(0/1)";
static const char* pdsErrorString = "!ERROR_PDS: Format: PDSn-s-t n=pinGroup(0/1) s 0=stop 1=start, t=transition on falling(0) or rising(1) edge";
static const char* bdoErrorString = "!ERROR_BDO: Format: BDOn-s-t n=pingroup 0-1, s blank 0(off) or 1(on), t 0 (blank on low) or 1 (blank on high)";
static const char* sslErrorString = "!ERROR_SSL: Format: SSLn n=0 (Off) or 1 (On)";
static const char* generalErrorString = "ERROR_UNKNOWN_COMMAND";

/* ==================== SPI Settings ==================== */
// 24 MHz = PCLK(48 MHz) / 2 — max for STM32F072CB, within AD5684R's 50 MHz
SPISettings dacSPI(24000000, MSBFIRST, SPI_MODE2);

/* ==================== State Variables ==================== */

// --- DAC state ---
uint16_t dacArray[NR_DAC_STATES][NR_DACS];
uint16_t dacState[NR_DACS];
uint16_t dacStoredState[NR_DACS];
int      dacArrayMaxIndex[NR_DACS];
int      dacArrayIndex[NR_DACS];
bool     dacSequencing[NR_DACS];
byte     dacSequenceMode[NR_DACS];       // 0=falling edge, 1=rising edge
bool     dacBlanking[NR_DACS];
bool     dacBlankOnLow[NR_DACS];
uint32_t dacBlankDelay[NR_DACS];
uint32_t dacBlankDuration[NR_DACS];

// --- TTL state (single pin group) ---
uint8_t  ttlArray[NR_DO_STATES];
byte     pinGroupState;
byte     pinGroupStoredState;
int      ttlArrayMaxIndex;
int      ttlArrayIndex;
bool     pinGroupSequencing;
byte     pinGroupSequenceMode;           // 0=falling, 1=rising
bool     pinGroupBlanking;
bool     pinGroupBlankOnLow;

// --- Trigger ---
bool     triggerPinState;

// --- Serial buffer ---
#define CMD_BUF_SIZE 256
char     cmdBuf[CMD_BUF_SIZE];
int      cmdBufIdx = 0;

// --- Misc ---
bool     error = false;

/* ==================== DAC Functions ==================== */

// Send raw 24-bit command: [CMD(4) | ADDR(4) | DATA(16)]
void dacSendCommand(uint8_t cmd, uint8_t addr, uint16_t data)
{
  uint8_t buf[3];
  buf[0] = (cmd << 4) | (addr & 0x0F);
  buf[1] = (data >> 8) & 0xFF;
  buf[2] = data & 0xFF;
  CS_LOW();
  SPI.transfer(buf, 3);
  CS_HIGH();
}

// Write 12-bit value to DAC channel (fast path)
// Data left-aligned: [D11:D0 | 0000].  LDAC=GND → immediate update.
inline void writeDAC(uint8_t channel, uint16_t value)
{
  uint16_t shifted = (value & 0x0FFF) << 4;
  uint8_t buf[3];
  buf[0] = (DAC_CMD_WRITE_REG << 4) | channel;
  buf[1] = (shifted >> 8);
  buf[2] = (shifted & 0xFF);
  CS_LOW();
  SPI.transfer(buf, 3);
  CS_HIGH();
}

// Set DAC by MicroManager index (0-based).
// MicroManager non-TS16 mode sends 0-4095 (12-bit) which maps
// directly to our 12-bit DAC.  Values >4095 (manual/TS16 testing)
// are scaled down via >>4.
void setDac(byte dacIdx, uint16_t value16)
{
  if (dacIdx >= NR_DACS) return;
  uint16_t value12 = (value16 > 4095) ? (value16 >> 4) : value16;
  writeDAC(dacChannelMap[dacIdx], value12);
}

// Set DAC respecting blanking state
void setDacCheckBlanking(byte dacIdx)
{
  if (dacIdx >= NR_DACS) return;
  if (dacBlanking[dacIdx])
  {
    triggerPinState = digitalRead(TRIG1);
    if (dacBlankOnLow[dacIdx] == triggerPinState)
      setDac(dacIdx, dacState[dacIdx]);
    else
      setDac(dacIdx, 0);
  }
  else
  {
    setDac(dacIdx, dacState[dacIdx]);
  }
}

/* ==================== TTL Functions ==================== */

// Set TTL1-4 from lower 4 bits of value
// Uses direct GPIOB BSRR for fast atomic writes on PB7-PB10
inline void setPinGroup(byte value)
{
  // Build BSRR value: bits 7-10 for set, bits 23-26 for reset
  uint32_t bsrr = 0;
  for (byte b = 0; b < 4; b++)
  {
    uint32_t pin_bit = (1 << (7 + b));  // PB7=bit7, PB8=bit8, PB9=bit9, PB10=bit10
    if (value & (1 << b))
      bsrr |= pin_bit;            // set
    else
      bsrr |= (pin_bit << 16);    // reset
  }
  GPIOB->BSRR = bsrr;
}

// Set TTL respecting blanking state
void setPinGroupCheckBlanking()
{
  if (pinGroupBlanking)
  {
    triggerPinState = digitalRead(TRIG1);
    if (pinGroupBlankOnLow == triggerPinState)
      setPinGroup(pinGroupState);
    else
      setPinGroup(0);
  }
  else
  {
    setPinGroup(pinGroupState);
  }
}

/* ==================== Utility Functions ==================== */

// Clear all DAC sequence data for one channel
void clearPAO(int dacIdx)
{
  for (int i = 0; i < NR_DAC_STATES; i++)
    dacArray[i][dacIdx] = 0;
  dacArrayMaxIndex[dacIdx] = 0;
}

// Clear all TTL sequence data
void clearPDO()
{
  for (int i = 0; i < NR_DO_STATES; i++)
    ttlArray[i] = 0;
  ttlArrayMaxIndex = 0;
}

// Clear everything
void clearTable()
{
  for (byte i = 0; i < NR_DACS; i++)
  {
    clearPAO(i);
    dacSequencing[i] = false;
    dacArrayIndex[i] = 0;
    dacBlanking[i] = false;
    dacBlankOnLow[i] = false;
    dacBlankDelay[i] = 0;
    dacBlankDuration[i] = 0;
    dacState[i] = 0;
    dacStoredState[i] = 0;
    setDac(i, 0);
  }
  clearPDO();
  pinGroupSequencing = false;
  ttlArrayIndex = 0;
  pinGroupBlanking = false;
  pinGroupBlankOnLow = false;
  pinGroupState = 0;
  pinGroupStoredState = 0;
  setPinGroup(0);
  Serial.println("!CLEAR_ALL");
}

// Clear one DAC's sequence (CLEAR_DAC command)
void clearDac(int dacNum)
{
  if (dacNum < 1 || dacNum > NR_DACS) return;
  int idx = dacNum - 1;
  clearPAO(idx);
  dacSequencing[idx] = false;
  dacArrayIndex[idx] = 0;
  Serial.print("!CLEAR_DAC,");
  Serial.println(dacNum);
}

/* ==================== Diagnostic Functions ==================== */

void debug()
{
  Serial.print("Input Trigger = ");
  Serial.println(digitalRead(TRIG1));

  Serial.print("TTL: ");
  for (int i = 0; i < 4; i++)
  {
    char sOut[16];
    sprintf(sOut, "%d=%d,", i + 1, digitalRead(ttlPins[i]));
    Serial.print(sOut);
  }
  Serial.println();

  Serial.print("DAC:");
  for (int i = 0; i < NR_DACS; i++)
  {
    char sOut[16];
    sprintf(sOut, "%d=%u,", i + 1, dacState[i]);
    Serial.print(sOut);
  }
  Serial.println();

  Serial.print("TTL buffer size: ");
  Serial.println(ttlArrayMaxIndex);
  for (int i = 0; i < ttlArrayMaxIndex; i++)
  {
    char sOut[8];
    sprintf(sOut, "%u ", ttlArray[i]);
    Serial.print(sOut);
  }
  if (ttlArrayMaxIndex > 0) Serial.println();

  Serial.println("DAC buffers:");
  for (int dac = 0; dac < NR_DACS; dac++)
  {
    char sOut[32];
    sprintf(sOut, "DAC %d buffer size: %d", dac + 1, dacArrayMaxIndex[dac]);
    Serial.println(sOut);
    for (int i = 0; i < dacArrayMaxIndex[dac]; i++)
    {
      sprintf(sOut, "%u ", dacArray[i][dac]);
      Serial.print(sOut);
    }
    if (dacArrayMaxIndex[dac] > 0) Serial.println();
  }
  Serial.println();
}

void diagTest()
{
  Serial.println("***** TRIGGERSCOPE MINI QC TEST *****");
  Serial.println("TEST 1: DAC all channels to 5V (full scale)");
  for (int i = 0; i < NR_DACS; i++)
  {
    dacState[i] = 65535;
    setDac(i, 65535);
  }
  Serial.println("  All DACs set to full scale. Measure ~5V on DAC outputs.");
  Serial.println("  Send any character to continue...");
  while (Serial.available() == 0) delay(10);
  while (Serial.available()) Serial.read();

  Serial.println("TEST 2: DAC all channels to 0V");
  for (int i = 0; i < NR_DACS; i++)
  {
    dacState[i] = 0;
    setDac(i, 0);
  }
  Serial.println("  All DACs set to zero. Measure ~0V on DAC outputs.");
  Serial.println("  Send any character to continue...");
  while (Serial.available() == 0) delay(10);
  while (Serial.available()) Serial.read();

  Serial.println("TEST 3: TTL all HIGH");
  setPinGroup(0x0F);
  Serial.println("  TTL 1-4 set HIGH. Measure 5V on TTL outputs.");
  Serial.println("  Send any character to continue...");
  while (Serial.available() == 0) delay(10);
  while (Serial.available()) Serial.read();

  Serial.println("TEST 4: TTL all LOW");
  setPinGroup(0x00);
  Serial.println("  TTL 1-4 set LOW. Measure 0V on TTL outputs.");
  Serial.println("  Send any character to continue...");
  while (Serial.available() == 0) delay(10);
  while (Serial.available()) Serial.read();

  Serial.println("TEST 5: Trigger input");
  Serial.println("  Apply 5V to TRIG1 input. Reading trigger...");
  for (int i = 0; i < 20; i++)
  {
    Serial.print("  TRIG1 = ");
    Serial.println(digitalRead(TRIG1));
    delay(250);
    if (Serial.available()) break;
  }
  while (Serial.available()) Serial.read();

  Serial.println("***** TEST COMPLETE *****");
  Serial.println();
}

/* ==================== Command Processing ==================== */

void processCommand(char* input, int len)
{
  // Strip trailing newline/CR
  while (len > 0 && (input[len - 1] == '\n' || input[len - 1] == '\r'))
    len--;
  input[len] = '\0';

  if (len == 0) return;

  digitalWrite(READY_LED, LOW);

  // Debug: echo raw command (remove this line once debugging is done)
  // Serial.print("DBG_RX:"); Serial.println(input);

  // Single-character commands
  if (len == 1 && input[0] == '?')
  {
    Serial.print(helpString);
    digitalWrite(READY_LED, HIGH);
    return;
  }
  if (len == 1 && input[0] == '*')
  {
    Serial.println(idname);
    digitalWrite(READY_LED, HIGH);
    return;
  }

  // Special multi-character commands
  if (strcmp(input, "STAT?") == 0)    { debug();      digitalWrite(READY_LED, HIGH); return; }
  if (strcmp(input, "TEST?") == 0)    { diagTest();    digitalWrite(READY_LED, HIGH); return; }
  if (strcmp(input, "CLEAR_ALL") == 0){ clearTable();  digitalWrite(READY_LED, HIGH); return; }
  if (strncmp(input, "CLEAR_DAC", 9) == 0)
  {
    int dacNum = atoi(&input[9]);
    // For unsupported DAC numbers, still respond success
    if (dacNum >= 1 && dacNum <= NR_DACS)
      clearDac(dacNum);
    else
    {
      Serial.print("!CLEAR_DAC,");
      Serial.println(dacNum);
    }
    digitalWrite(READY_LED, HIGH);
    return;
  }

  // Extract 3-character command prefix
  if (len < 3)
  {
    Serial.println(generalErrorString);
    digitalWrite(READY_LED, HIGH);
    return;
  }

  char cmd[4] = { input[0], input[1], input[2], '\0' };

  /* ---- SAO: Set Analog Output ---- */
  if (strcmp(cmd, "SAO") == 0)
  {
    error = false;
    int dacNum = 0;
    int offset = 5;
    if (len > 4 && input[4] == sep)
    {
      dacNum = atoi(&input[3]);
    }
    else if (len > 5 && input[5] == sep)
    {
      char tmp[3] = { input[3], input[4], '\0' };
      dacNum = atoi(tmp);
      offset = 6;
    }
    else
    {
      error = true;
    }
    if (dacNum < 1 || dacNum > 16) error = true;

    long value = atol(&input[offset]);
    if (value < 0 || value > 65535) error = true;

    if (!error)
    {
      Serial.print("!SAO");
      Serial.print(dacNum);
      Serial.print(sep);
      Serial.println((int)value);

      // Only physically set if within our 4 channels
      if (dacNum >= 1 && dacNum <= NR_DACS)
      {
        dacState[dacNum - 1] = (uint16_t)value;
        setDacCheckBlanking(dacNum - 1);
      }
      // DAC 5-16: no-op, success already printed
    }
    else
    {
      Serial.println(saoErrorString);
    }
  }

  /* ---- PAN: Query Analog Sequence Capacity ---- */
  else if (strcmp(cmd, "PAN") == 0)
  {
    int dac = atoi(&input[3]);
    if (dac < 1 || dac > 16)
    {
      Serial.println(panErrorString);
    }
    else
    {
      Serial.print("!PAN");
      Serial.print(dac);
      Serial.print(sep);
      Serial.println(NR_DAC_STATES);
    }
  }

  /* ---- PAO: Load Analog Sequence ---- */
  else if (strcmp(cmd, "PAO") == 0)
  {
    error = false;
    int n = 0;
    int s = 0;
    int dacNr = 0;
    unsigned int scp = 5;

    if (len > 4 && input[4] == sep) { dacNr = atoi(&input[3]); }
    else if (len > 5 && input[5] == sep)
    {
      char tmp[3] = { input[3], input[4], '\0' };
      dacNr = atoi(tmp);
      scp = 6;
    }
    else { error = true; }

    if (dacNr < 1 || dacNr > 16) error = true;
    bool isPhysical = (dacNr >= 1 && dacNr <= NR_DACS);

    unsigned int ecp = scp + 1;
    if (!error)
    {
      // Find start position
      while (ecp < (unsigned int)len && input[ecp - 1] != sep) ecp++;
      if (ecp < (unsigned int)len)
      {
        char tmp[12];
        int tmpLen = ecp - scp;
        if (tmpLen > 11) tmpLen = 11;
        memcpy(tmp, &input[scp], tmpLen);
        tmp[tmpLen] = '\0';
        s = atoi(tmp);
      }
      else { error = true; }

      // Parse values
      while (!error && ecp < (unsigned int)len)
      {
        scp = ecp;
        ecp++;
        while (ecp < (unsigned int)len && input[ecp - 1] != sep) ecp++;
        if ((ecp - scp) > 1)
        {
          char tmp[12];
          int tmpLen = ecp - scp;
          if (tmpLen > 11) tmpLen = 11;
          memcpy(tmp, &input[scp], tmpLen);
          tmp[tmpLen] = '\0';
          long val = atol(tmp);
          if (val < 0 || val > 65535)
          {
            error = true;
          }
          else
          {
            if (isPhysical && (n + s) < NR_DAC_STATES)
            {
              dacArray[n + s][dacNr - 1] = (uint16_t)val;
            }
            n++;
            int index = n + s;
            if (isPhysical && index > dacArrayMaxIndex[dacNr - 1])
            {
              dacArrayMaxIndex[dacNr - 1] = index;
            }
          }
        }
      }
    }

    if (!error)
    {
      char out[32];
      int maxIdx = isPhysical ? dacArrayMaxIndex[dacNr - 1] : (n + s);
      sprintf(out, "!PAO%d%c%d%c%d%c%d", dacNr, sep, s, sep, n, sep, maxIdx);
      Serial.println(out);
    }
    else
    {
      Serial.println(paoErrorString);
    }
  }

  /* ---- PAC: Clear Analog Sequence ---- */
  else if (strcmp(cmd, "PAC") == 0)
  {
    error = false;
    int dacNr = atoi(&input[3]);
    if (dacNr < 1 || dacNr > 16) error = true;

    if (!error)
    {
      if (dacNr >= 1 && dacNr <= NR_DACS)
      {
        dacSequencing[dacNr - 1] = false;
        dacArrayIndex[dacNr - 1] = 0;
        dacArrayMaxIndex[dacNr - 1] = 0;
        clearPAO(dacNr - 1);
      }
      char out[12];
      sprintf(out, "!PAC%d", dacNr);
      Serial.println(out);
    }
    else
    {
      Serial.println(pacErrorString);
    }
  }

  /* ---- PAS: Start/Stop Analog Sequencing ---- */
  else if (strcmp(cmd, "PAS") == 0)
  {
    error = false;
    int dacNr = 0;
    int scp = 4;

    if (len > 4 && input[4] == sep) { dacNr = atoi(&input[3]); scp = 4; }
    else if (len > 5 && input[5] == sep)
    {
      char tmp[3] = { input[3], input[4], '\0' };
      dacNr = atoi(tmp);
      scp = 5;
    }
    else { error = true; }

    if (dacNr < 1 || dacNr > 16) error = true;

    int state = 0, rising = 0;
    if (!error)
    {
      state = atoi(&input[scp + 1]);
      if (state < 0 || state > 1) error = true;
      rising = atoi(&input[scp + 3]);
      if (rising < 0 || rising > 1) error = true;
    }

    if (!error)
    {
      if (dacNr >= 1 && dacNr <= NR_DACS)
      {
        int idx = dacNr - 1;
        dacSequencing[idx] = (bool)state;
        dacSequenceMode[idx] = rising;
        if (state)
        {
          dacStoredState[idx] = dacState[idx];
          dacArrayIndex[idx] = 0;
          if (!rising)
          {
            // Falling edge trigger: set initial state now
            dacState[idx] = dacArray[0][idx];
            setDac(idx, dacState[idx]);
            dacArrayIndex[idx] = 1;
          }
        }
        else
        {
          dacState[idx] = dacStoredState[idx];
        }
      }
      char out[20];
      sprintf(out, "!PAS%d%c%d%c%d", dacNr, sep, state, sep, rising);
      Serial.println(out);
    }
    else
    {
      Serial.println(pasErrorString);
    }
  }

  /* ---- BAO: Analog Blanking On/Off ---- */
  else if (strcmp(cmd, "BAO") == 0)
  {
    error = false;
    int dacNr = 0;
    int scp = 5;

    if (len > 4 && input[4] == sep) { dacNr = atoi(&input[3]); scp = 5; }
    else if (len > 5 && input[5] == sep)
    {
      char tmp[3] = { input[3], input[4], '\0' };
      dacNr = atoi(tmp);
      scp = 6;
    }
    else { error = true; }

    if (dacNr < 1 || dacNr > 16) error = true;

    int state = 0, mode = 0;
    if (!error)
    {
      state = atoi(&input[scp]);
      if (state < 0 || state > 1) error = true;
      if (input[scp + 1] != sep) error = true;
      mode = atoi(&input[scp + 2]);
      if (mode < 0 || mode > 1) error = true;
    }

    if (!error)
    {
      if (dacNr >= 1 && dacNr <= NR_DACS)
      {
        dacBlanking[dacNr - 1] = (state == 1);
        dacBlankOnLow[dacNr - 1] = (mode == 0);
        setDacCheckBlanking(dacNr - 1);
      }
      char out[20];
      sprintf(out, "!BAO%d%c%d%c%d", dacNr, sep, state, sep, mode);
      Serial.println(out);
    }
    else
    {
      Serial.println(baoErrorString);
    }
  }

  /* ---- BAD: Analog Blank Delay ---- */
  else if (strcmp(cmd, "BAD") == 0)
  {
    error = false;
    int dacNr = 0;
    int scp = 5;

    if (len > 4 && input[4] == sep) { dacNr = atoi(&input[3]); scp = 5; }
    else if (len > 5 && input[5] == sep)
    {
      char tmp[3] = { input[3], input[4], '\0' };
      dacNr = atoi(tmp);
      scp = 6;
    }
    else { error = true; }

    if (dacNr < 1 || dacNr > 16) error = true;

    if (!error)
    {
      uint32_t duration = (uint32_t)atol(&input[scp]);
      if (dacNr >= 1 && dacNr <= NR_DACS)
      {
        dacBlankDelay[dacNr - 1] = duration;
      }
      Serial.print("!BAD");
      Serial.print(dacNr);
      Serial.print(sep);
      Serial.println(duration);
    }
    else
    {
      Serial.println(badErrorString);
    }
  }

  /* ---- BAL: Analog Blank Duration ---- */
  else if (strcmp(cmd, "BAL") == 0)
  {
    error = false;
    int dacNr = 0;
    int scp = 5;

    if (len > 4 && input[4] == sep) { dacNr = atoi(&input[3]); scp = 5; }
    else if (len > 5 && input[5] == sep)
    {
      char tmp[3] = { input[3], input[4], '\0' };
      dacNr = atoi(tmp);
      scp = 6;
    }
    else { error = true; }

    if (dacNr < 1 || dacNr > 16) error = true;

    if (!error)
    {
      uint32_t duration = (uint32_t)atol(&input[scp]);
      if (dacNr >= 1 && dacNr <= NR_DACS)
      {
        dacBlankDuration[dacNr - 1] = duration;
      }
      Serial.print("!BAL");
      Serial.print(dacNr);
      Serial.print(sep);
      Serial.println(duration);
    }
    else
    {
      Serial.println(balErrorString);
    }
  }

  /* ---- SAR: Set Analog Range (no-op on Mini, fixed 0-5V) ---- */
  else if (strcmp(cmd, "SAR") == 0)
  {
    error = false;
    int dacNum = atoi(&input[3]);
    int pp = 5;
    if (dacNum > 9) pp = 6;
    int rangeVal = atoi(&input[pp]);
    if (rangeVal < 1 || rangeVal > 5) error = true;
    if (dacNum < 1 || dacNum > 16) error = true;

    if (!error)
    {
      // No-op: Mini is fixed 0-5V, but echo success
      Serial.print("!SAR");
      Serial.print(dacNum);
      Serial.print(sep);
      Serial.println(rangeVal);
    }
    else
    {
      Serial.println(sarErrorString);
    }
  }

  /* ---- SDO: Set Digital Output ---- */
  else if (strcmp(cmd, "SDO") == 0)
  {
    int pinGroup = atoi(&input[3]);
    if (pinGroup < 0 || pinGroup > 1)
    {
      Serial.println(sdoErrorString);
    }
    else
    {
      int value = atoi(&input[5]);
      if (value < 0 || value > 255)
      {
        Serial.println(sdoErrorString);
      }
      else
      {
        Serial.print("!SDO");
        Serial.print(pinGroup);
        Serial.print(sep);
        Serial.println(value);

        if (pinGroup == 0)
        {
          pinGroupState = (byte)value;
          setPinGroupCheckBlanking();
        }
        // pinGroup 1: no-op, success already printed
      }
    }
  }

  /* ---- PDN: Query Digital Sequence Capacity ---- */
  else if (strcmp(cmd, "PDN") == 0)
  {
    int pinGroup = atoi(&input[3]);
    if (pinGroup < 0 || pinGroup > 1)
    {
      Serial.println(pdnErrorString);
    }
    else
    {
      Serial.print("!PDN");
      Serial.print(pinGroup);
      Serial.print(sep);
      Serial.println(NR_DO_STATES);
    }
  }

  /* ---- PDO: Load Digital Sequence ---- */
  else if (strcmp(cmd, "PDO") == 0)
  {
    error = false;
    int s = 0;
    int n = 0;
    int pinGroup = atoi(&input[3]);
    if (pinGroup < 0 || pinGroup > 1) error = true;
    if (len > 4 && input[4] != sep) error = true;

    bool isPhysical = (pinGroup == 0);
    unsigned int scp = 5;
    unsigned int ecp = 6;

    if (!error)
    {
      while (ecp < (unsigned int)len && input[ecp - 1] != sep) ecp++;
      if (ecp < (unsigned int)len)
      {
        char tmp[12];
        int tmpLen = ecp - scp;
        if (tmpLen > 11) tmpLen = 11;
        memcpy(tmp, &input[scp], tmpLen);
        tmp[tmpLen] = '\0';
        s = atoi(tmp);
      }
      else { error = true; }

      while (!error && ecp < (unsigned int)len)
      {
        scp = ecp;
        ecp++;
        while (ecp < (unsigned int)len && input[ecp - 1] != sep) ecp++;
        if ((ecp - scp) > 1)
        {
          char tmp[12];
          int tmpLen = ecp - scp;
          if (tmpLen > 11) tmpLen = 11;
          memcpy(tmp, &input[scp], tmpLen);
          tmp[tmpLen] = '\0';
          int val = atoi(tmp);
          if (val < 0 || val > 255)
          {
            error = true;
          }
          else
          {
            if (isPhysical && (n + s) < NR_DO_STATES)
            {
              ttlArray[n + s] = (uint8_t)val;
            }
            n++;
            int index = n + s;
            if (isPhysical && index > ttlArrayMaxIndex)
            {
              ttlArrayMaxIndex = index;
            }
          }
        }
      }
    }

    if (!error)
    {
      char out[32];
      int maxIdx = isPhysical ? ttlArrayMaxIndex : (n + s);
      sprintf(out, "!PDO%d%c%d%c%d%c%d", pinGroup, sep, s, sep, n, sep, maxIdx);
      Serial.println(out);
    }
    else
    {
      Serial.println(pdoErrorString);
    }
  }

  /* ---- PDC: Clear Digital Sequence ---- */
  else if (strcmp(cmd, "PDC") == 0)
  {
    error = false;
    int pinGroup = atoi(&input[3]);
    if (pinGroup < 0 || pinGroup > 1) error = true;

    if (!error)
    {
      if (pinGroup == 0)
      {
        pinGroupSequencing = false;
        ttlArrayIndex = 0;
        ttlArrayMaxIndex = 0;
        clearPDO();
      }
      char out[12];
      sprintf(out, "!PDC%d", pinGroup);
      Serial.println(out);
    }
    else
    {
      Serial.println(pdcErrorString);
    }
  }

  /* ---- PDS: Start/Stop Digital Sequencing ---- */
  else if (strcmp(cmd, "PDS") == 0)
  {
    error = false;
    int pinGroup = atoi(&input[3]);
    if (pinGroup < 0 || pinGroup > 1) error = true;
    if (len < 8) error = true;

    int state = 0, rising = 0;
    if (!error)
    {
      state = atoi(&input[5]);
      if (state < 0 || state > 1) error = true;
      rising = atoi(&input[7]);
      if (rising < 0 || rising > 1) error = true;
    }

    if (!error)
    {
      if (pinGroup == 0)
      {
        pinGroupSequencing = (bool)state;
        pinGroupSequenceMode = rising;
        if (state)
        {
          pinGroupStoredState = pinGroupState;
          ttlArrayIndex = 0;
          if (!rising)
          {
            // Falling edge: set initial state now
            pinGroupState = ttlArray[0];
            setPinGroup(pinGroupState);
            ttlArrayIndex = 1;
          }
        }
        else
        {
          pinGroupState = pinGroupStoredState;
        }
      }
      char out[20];
      sprintf(out, "!PDS%d%c%d%c%d", pinGroup, sep, state, sep, rising);
      Serial.println(out);
    }
    else
    {
      Serial.println(pdsErrorString);
    }
  }

  /* ---- BDO: Digital Blanking On/Off ---- */
  else if (strcmp(cmd, "BDO") == 0)
  {
    error = false;
    if (len < 8) error = true;

    int pinGroup = 0;
    int state = 0, mode = 0;
    if (!error)
    {
      pinGroup = atoi(&input[3]);
      if (pinGroup < 0 || pinGroup > 1) error = true;
      state = atoi(&input[5]);
      if (state < 0 || state > 1) error = true;
      mode = atoi(&input[7]);
      if (mode < 0 || mode > 1) error = true;
    }

    if (!error)
    {
      if (pinGroup == 0)
      {
        pinGroupBlanking = (state == 1);
        pinGroupBlankOnLow = (mode == 0);
        setPinGroupCheckBlanking();
      }
      char out[20];
      sprintf(out, "!BDO%d%c%d%c%d", pinGroup, sep, state, sep, mode);
      Serial.println(out);
    }
    else
    {
      Serial.println(bdoErrorString);
    }
  }

  /* ---- SSL: Signal LEDs (no-op on Mini) ---- */
  else if (strcmp(cmd, "SSL") == 0)
  {
    error = false;
    int result = atoi(&input[3]);
    if (result < 0 || result > 1) error = true;

    if (!error)
    {
      Serial.print("!SSL");
      Serial.println(result);
    }
    else
    {
      Serial.println(sslErrorString);
    }
  }

  /* ---- Unknown Command ---- */
  else
  {
    Serial.println(generalErrorString);
  }

  digitalWrite(READY_LED, HIGH);
}

/* ==================== Setup ==================== */

void setup()
{
  // Initialize all state to zero
  memset(dacArray, 0, sizeof(dacArray));
  memset(dacState, 0, sizeof(dacState));
  memset(dacStoredState, 0, sizeof(dacStoredState));
  memset(dacArrayMaxIndex, 0, sizeof(dacArrayMaxIndex));
  memset(dacArrayIndex, 0, sizeof(dacArrayIndex));
  memset(dacSequencing, 0, sizeof(dacSequencing));
  memset(dacSequenceMode, 0, sizeof(dacSequenceMode));
  memset(dacBlanking, 0, sizeof(dacBlanking));
  memset(dacBlankOnLow, 0, sizeof(dacBlankOnLow));
  memset(dacBlankDelay, 0, sizeof(dacBlankDelay));
  memset(dacBlankDuration, 0, sizeof(dacBlankDuration));
  memset(ttlArray, 0, sizeof(ttlArray));
  pinGroupState = 0;
  pinGroupStoredState = 0;
  ttlArrayMaxIndex = 0;
  ttlArrayIndex = 0;
  pinGroupSequencing = false;
  pinGroupSequenceMode = 0;
  pinGroupBlanking = false;
  pinGroupBlankOnLow = false;
  triggerPinState = false;

  // LEDs
  pinMode(READY_LED, OUTPUT);
  pinMode(CONN_LED, OUTPUT);
  digitalWrite(READY_LED, HIGH);
  digitalWrite(CONN_LED, HIGH);

  // TTL outputs
  pinMode(TTL1, OUTPUT);
  pinMode(TTL2, OUTPUT);
  pinMode(TTL3, OUTPUT);
  pinMode(TTL4, OUTPUT);
  digitalWrite(TTL1, LOW);
  digitalWrite(TTL2, LOW);
  digitalWrite(TTL3, LOW);
  digitalWrite(TTL4, LOW);

  // Trigger input
  pinMode(TRIG1, INPUT);

  // Level shifter direction
  pinMode(TTLOUT_D, OUTPUT);
  digitalWrite(TTLOUT_D, HIGH);    // MCU → External
  pinMode(TTLIN_D, OUTPUT);
  digitalWrite(TTLIN_D, LOW);      // Enable inputs

  // Serial — do NOT wait for it (no USB blocking)
  Serial.begin(115200);

  // SPI init
  pinMode(DAC_CS, OUTPUT);
  digitalWrite(DAC_CS, HIGH);
  SPI.begin();
  SPI.beginTransaction(dacSPI);    // Lock SPI settings for entire session

  // DAC hardware reset
  pinMode(DAC_RESET, OUTPUT);
  digitalWrite(DAC_RESET, LOW);
  delay(10);
  digitalWrite(DAC_RESET, HIGH);
  delay(10);

  // GAIN LOW = 1x gain: external 5V VREF × 1 = 0–5V full-scale
  pinMode(DAC_GAIN, OUTPUT);
  digitalWrite(DAC_GAIN, LOW);

  // Disable internal reference (using external 5V on VREF pin)
  dacSendCommand(DAC_CMD_REF_SETUP, 0x0, 0x0000);
  delay(1);

  // Zero all DAC outputs
  for (int i = 0; i < NR_DACS; i++)
    setDac(i, 0);

  // Read initial trigger state
  triggerPinState = digitalRead(TRIG1);

  delay(100);

  // Print ID on startup (matches original TriggerScope behavior)
  Serial.println(idname);

  digitalWrite(READY_LED, HIGH);
}

/* ==================== Main Loop ==================== */

void loop()
{
  // ---- 1. Poll trigger and advance sequences ----
  bool currentTrig = digitalRead(TRIG1);
  if (currentTrig != triggerPinState)
  {
    triggerPinState = currentTrig;

    // Advance DAC sequences
    for (byte i = 0; i < NR_DACS; i++)
    {
      if (dacSequencing[i])
      {
        if (dacSequenceMode[i] == triggerPinState)
        {
          dacState[i] = dacArray[dacArrayIndex[i]][i];
          dacArrayIndex[i]++;
          if (dacArrayIndex[i] >= dacArrayMaxIndex[i])
            dacArrayIndex[i] = 0;
          if (!dacBlanking[i])
            setDac(i, dacState[i]);
        }
      }
      if (dacBlanking[i])
      {
        if (dacBlankOnLow[i] == triggerPinState)
          setDac(i, dacState[i]);
        else
          setDac(i, 0);
      }
    }

    // Advance TTL sequence
    if (pinGroupSequencing)
    {
      if (pinGroupSequenceMode == triggerPinState)
      {
        pinGroupState = ttlArray[ttlArrayIndex];
        ttlArrayIndex++;
        if (ttlArrayIndex >= ttlArrayMaxIndex)
          ttlArrayIndex = 0;
        if (!pinGroupBlanking)
          setPinGroup(pinGroupState);
      }
    }
    if (pinGroupBlanking)
    {
      if (pinGroupBlankOnLow == triggerPinState)
        setPinGroup(pinGroupState);
      else
        setPinGroup(0);
    }
  }

  // ---- 2. Poll serial and accumulate commands ----
  while (Serial.available())
  {
    char c = (char)Serial.read();
    if (c == '\n')
    {
      cmdBuf[cmdBufIdx] = '\n';
      cmdBufIdx++;
      cmdBuf[cmdBufIdx] = '\0';
      processCommand(cmdBuf, cmdBufIdx);
      cmdBufIdx = 0;
    }
    else
    {
      if (cmdBufIdx < CMD_BUF_SIZE - 2)
      {
        cmdBuf[cmdBufIdx++] = c;
      }
      // else: overflow, silently drop character
    }
  }
}
