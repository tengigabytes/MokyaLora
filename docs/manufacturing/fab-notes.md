# Fabrication Notes

**Project:** MokyaLora Rev A

---

## PCB Specification

| Parameter            | Value                         |
|----------------------|-------------------------------|
| Layer count          | 4                             |
| Stack-up             | Signal / GND / Power / Signal |
| Board thickness      | 1.6 mm                        |
| Copper weight        | 1 oz outer / 0.5 oz inner     |
| Surface finish       | ENIG (Electroless Nickel Immersion Gold) |
| Solder mask          | Green (both sides)            |
| Silkscreen           | White (both sides)            |
| Min. trace / space   | 0.1 mm / 0.1 mm               |
| Min. drill           | 0.2 mm (via), 0.3 mm (drill)  |
| Via fill             | Tented vias under BGA pads    |
| Impedance control    | 50 Ω single-ended on RF layers|

## Special Fabrication Requirements

1. **Impedance-controlled traces** — RF signal traces (LoRa, GNSS) require 50 Ω controlled impedance; specify dielectric constant to fab.
2. **Via-in-pad** — thermal vias under QFN/WQFN exposed pads must be filled and plated (or tented on back side).
3. **Microphone acoustic port** — PCB requires a through-hole opening aligned to IM69D130 bottom-port inlet; coordinate position with enclosure gasket design.
4. **RF shielding frame** — Wurth 3600213120S footprint requires precise placement; confirm fab tolerances with frame supplier.

## Gerber Outputs (in `production/rev-a/gerber/`)

| File                          | Layer              |
|-------------------------------|--------------------|
| MokyaLora-F_Cu.gbr            | Front copper       |
| MokyaLora-In1_Cu.gbr          | Inner layer 1 (GND)|
| MokyaLora-In2_Cu.gbr          | Inner layer 2 (PWR)|
| MokyaLora-B_Cu.gbr            | Back copper        |
| MokyaLora-F_Mask.gbr          | Front solder mask  |
| MokyaLora-B_Mask.gbr          | Back solder mask   |
| MokyaLora-F_Silkscreen.gbr    | Front silkscreen   |
| MokyaLora-B_Silkscreen.gbr    | Back silkscreen    |
| MokyaLora-F_Paste.gbr         | Front paste (SMT)  |
| MokyaLora-B_Paste.gbr         | Back paste (SMT)   |
| MokyaLora-Edge_Cuts.gbr       | Board outline      |
| MokyaLora.drl                 | Drill file (NC)    |
| MokyaLora-job.gbrjob          | Gerber job file    |

## Assembly Notes

- All SMT components are on the **front** side unless indicated in the BOM.
- Reflow profile: follow IPC-7711/7721 for lead-free (SAC305).
- LCD FPC connector (Molex 54132-4062): ZIF, 40-pin, 0.5 mm pitch — insert FPC with contacts facing **down** (verify orientation before locking).
- Battery connector (AVX 009155003301006): 3-pin pogo, 2.5 mm pitch — polarity marked on silkscreen.
- USB-C (GCT USB4105-GF-A): through-hole shell legs must be soldered for mechanical retention.
