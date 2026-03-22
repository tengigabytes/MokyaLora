# Compliance Notes

**Project:** MokyaLora Rev A

> This document is a placeholder. Regulatory compliance work is deferred until after Rev A hardware validation.

---

## Applicable Regulations (Target Markets)

| Region | Standard         | Scope                                |
|--------|------------------|--------------------------------------|
| EU     | CE — RED 2014/53/EU | Radio equipment (LoRa 868 MHz)    |
| US     | FCC Part 15      | Unlicensed intentional radiator      |
| TW     | NCC              | Radio type approval                  |

## LoRa Frequency Plan

| Region | Band     | Frequency     | Notes                        |
|--------|----------|---------------|------------------------------|
| EU     | EU868    | 863–870 MHz   | Duty-cycle limited per ETSI  |
| US     | US915    | 902–928 MHz   | FHSS, limited power          |

**Current design target:** EU868. US915 requires confirming SX1262 crystal / filter component values.

## Open Items

- [ ] Determine target frequency bands before PCB Rev A tape-out.
- [ ] Engage test lab for pre-compliance scan after Rev A build.
- [ ] Verify SX1262 TX power limits comply with local regulations.
- [ ] Add regulatory markings to silkscreen (CE / FCC ID placeholder).
