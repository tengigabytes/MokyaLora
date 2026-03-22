# Fabrication Notes

**Project:** MokyaLora Rev A

---

## Authoritative Reference

All PCB specifications (layer stack-up, board thickness, copper weights, surface finish,
impedance requirements, drill rules, via-in-pad requirements) and assembly instructions
(reflow profile, connector orientations, SMT placement side) are defined in the
**Assembly PDF**, which is generated directly from the KiCad design and is the single
source of truth:

- [`hardware/production/rev-a/pdf/MokyaLora__Assembly.pdf`](../../hardware/production/rev-a/pdf/MokyaLora__Assembly.pdf)

Do not duplicate those specifications here — update the KiCad design and regenerate
the PDF instead.

---

## Fabrication Status

| Milestone | Date | Notes |
|-----------|------|-------|
| Gerber files generated | 2026-03-22 | `hardware/production/rev-a/gerber/` |
| BOM finalised | 2026-03-22 | `hardware/production/rev-a/MokyaLora.csv` |
| Submitted to manufacturer | 2026-03-22 | PCB fabrication + SMT assembly |
| Board received | — | Pending |
| Bring-up started | — | Pending — see `docs/bringup/rev-a-bringup-log.md` |

---

## Fabrication Output Files

| File | Location |
|------|----------|
| Gerber + drill files | `hardware/production/rev-a/gerber/` |
| Bill of Materials | `hardware/production/rev-a/MokyaLora.csv` |
| Schematic PDF | `hardware/production/rev-a/pdf/MokyaLora.pdf` |
| Assembly drawing PDF | `hardware/production/rev-a/pdf/MokyaLora__Assembly.pdf` |
| Board 3D model (STEP) | `hardware/production/rev-a/MokyaLora.step` |
