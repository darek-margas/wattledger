# Emporia Vue 3: backup, flashing and the RTC

Notes from converting one Vue 3 (board P/N 501-1-02649_B3, ESP32-WROVER-IE,
8 MB flash) in November 2026. Photos are in [images/vue3-rtc](images/vue3-rtc).

Work on the board only with mains disconnected. Never connect a serial
adapter or bench supply while it is on mains.

## Serial connection

The UART pads are on the underside, next to TP5. Their labels are from the
adapter's side, so the wiring is straight through: pad TXD to adapter TX, pad
RXD to adapter RX. IO0 is a plated hole; tie it to GND before power-up to
enter download mode.

Ground **TP6** as well. TP6 is the SAMD09's reset (the driver drives it from
GPIO23). With the stock SAMD09 firmware running, reads over the UART failed
with single flipped bits every 20-90 KB, and the esptool stub never came up.
With TP6 held low, a full 8 MB read passed every block checksum first time.
Remove the TP6 wire before ESPHome boots, since the firmware drives that line.

A CH340G adapter's 3.3 V pin ran the board but sagged under it. A separate
3.3 V source (an ESP32-C3 Super Mini held in BOOT mode, its 3V3 pin to the
Vue's 3.3 V pad) with the adapter's 3V3 pin left unconnected was more stable.

## Backup before flashing

```powershell
esptool --chip esp32 -p COM5 flash-id
esptool --chip esp32 -p COM5 read-flash 0 ALL vue3-stock-1.bin
esptool --chip esp32 -p COM5 read-flash 0 ALL vue3-stock-2.bin
Get-FileHash vue3-stock-*.bin
```

The two hashes must match. The image contains your Wi-Fi credentials and the
unit's Emporia identity, so keep it private. The stock layout is nvs,
otadata, phy_init, ota_0 (3 MB, the app), coredump, storage and ota_1.
`write-flash 0 vue3-stock-1.bin` restores it.

No per-unit calibration could be recovered from the stock image; calibrate
voltage and CT gain against a meter after installation.

## First flash

WattLedger adds its own partition, which OTA can't create, so the first
install is over serial. Use the factory image from the ESPHome dashboard
(Install, Manual download, Factory format):

```powershell
esptool --chip esp32 -p COM5 write-flash --erase-all 0x0 emporia-vue-grid.factory.bin
```

After that, updates go over OTA.

## RTC (optional)

A [PiicoDev RV-3028](https://core-electronics.com.au/piicodev-real-time-clock-rv3028.html)
on the I2C bus the ESP32 already shares with the SAMD09 (0x64). The RV-3028
answers at 0x52.

| PiicoDev | Vue 3 |
|---|---|
| GND | GND pad next to TP5 (underside) |
| 3.3V | 3.3 V pad next to TP5 (underside); confirm with a meter |
| SDA | ESP32-WROVER-IE pin 29 (GPIO5) |
| SCL | ESP32-WROVER-IE pin 30 (GPIO18) |
| EVI, CLK, INT | not connected |

There is no test point for I2C: TP9 and TP10 are other signals. Solder to the
module's castellations (photo 05). The WROVER has 19 pins per long side: 1-19
along the terminal-block edge starting at the antenna end, 20-38 back along
the transformer edge, so pin 38 is at the antenna end. Pins 29 and 30 are the
10th and 9th from the antenna end on the transformer edge. Each reads about
4.7 kOhm to 3.3 V through the bus pull-up. The SAMD09 (U6, underside) is a
QFN with nothing to solder to; its ends of the bus are PA22 (SDA) and PA23
(SCL).

On the PiicoDev, cut both traces of the three-pad I2C PU jumper (photo 03):
the Vue has its own pull-ups. Taking the JST connectors off saves height.

The power wires go through to the top side (photo 08). The board sits on
Kapton over the module's shield (photos 09, 10); the WROVER-IE has no PCB
antenna, so this doesn't affect Wi-Fi. Keep every wire on the low-voltage
side of the yellow isolation line, away from the transformer and the
Y-capacitors CY1/CY2, and glue them down before closing the lid.

Add [packages/vue3-rtc.yaml](../packages/vue3-rtc.yaml). Testing it:

- The boot log no longer reports `rv3028.time is marked FAILED`.
- A software restart proves nothing: the ESP32 keeps its clock through a
  reset. Cut the power instead.
- The 0.47 F supercapacitor charges through the RV-3028's 3 kOhm trickle
  resistor, a time constant of about 23 minutes. After 17 minutes of charge
  the time didn't survive a power cut; after 45 minutes it did.
- `/wattledger/v1/rows?tier=events` shows it. Rows are
  `[seq, type, boot, seconds_since_boot, unix_time, a, b]`. The ANCHOR (type 2)
  of a cold boot comes at 0 s when the RTC supplied the time, and 4 s or more
  when it came from SNTP or Home Assistant.
