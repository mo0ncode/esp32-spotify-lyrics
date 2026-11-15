# Spotify PCB placement draft

Status: preliminary component placement only. No routed PCB, Gerbers, or fabrication approval.

Proposed carrier outline: 65 x 65 mm. This is an initial allocation, not a validated minimum size.
ESP32 body: 52 x 29 mm. Header rows: 22.86 mm apart. Header pitch provisionally 2.54 mm, 15 pins per row. The body-to-header offsets in the drawing are placeholders and MUST be measured before footprints or traces are finalized.

The gold RF area is a reservation, not a verified antenna keepout. Exact antenna extent and clearance must be resolved, potentially by moving the module, adding a carrier cutout, or enlarging the carrier. Keep LCD metal away from this area as well.

## Wiring intent
- Previous module VCC/OUT/GND: 3.3V/GPIO25/GND.
- Play module VCC/OUT/GND: 3.3V/GPIO26/GND.
- Next module VCC/OUT/GND: 3.3V/GPIO27/GND.
- LCD connector GND/VCC/SDA/SCL: GND/USB-derived 5V/translated GPIO21/translated GPIO22.
- Two bidirectional I2C level-shifting circuits replace the eight-channel converter. Proposed BSS138 MOSFETs, gates to 3.3V, sources to ESP32 I2C, drains to LCD I2C. Pull-ups on both voltage domains, values and existing backpack pull-ups to be checked during schematic design.
- Power through the existing ESP32 USB socket only. Verify the board's USB-to-VIN/5V path before finalizing its footprint and power connection.

## Reused hardware
ESP32 CP2102 development board, LCD2004 with I2C backpack, three existing 22 x 11 x 20 mm button modules with 12 mm caps.

## Before manufacturing
Measure actual ESP32 pin locations relative to body and USB, actual LCD bezel and screw-hole diameters, LCD backpack position, and button supports. Complete schematic, exact connector/part selection, PCB routing, electrical/design-rule checks, 1:1 paper fit test, and enclosure interference/antenna clearance checks. Mounting holes are intentionally not fixed in this placement draft.

References:
https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32/pcb-layout-design.html
https://www.onsemi.com/pdf/datasheet/bss138-d.pdf
