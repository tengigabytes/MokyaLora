# Bringup TFT Screen Layouts

Reference layouts for diagnostic TFT screens added in Step 20.
All screens use scale=2 (12×16 px/char), 20 columns × variable rows.

---

## Bus A Diagnostic

```
Row 0:  " Bus A Diagnostic   "   (yellow on dark blue)
Row 2:  " IMU  6A  ID:70 OK  "   (green / red)
Row 3:  " Mag  1E  ID:40 OK  "
Row 4:  " Baro 5D  ID:B3 OK  "
Row 5:  " GNSS 3A  ACK OK    "
Row 7:  " Result: 4/4 pass   "
Row 9:  " BACK to return     "   (gray hint)
```

## Bus B Diagnostic

```
Row 0:  " Bus B Diagnostic   "   (yellow on dark blue)
Row 2:  " Chg  6B  PN:25622  "   (green / red)
Row 3:  " Fuel 55  DT:0421   "   (green / red)
Row 4:  " LED  36  GP:OK     "   (green / red)
Row 6:  " Result: 3/3 pass   "
Row 8:  " BACK to return     "   (gray hint)
```

## Charger Diagnostic (1 Hz live refresh)

```
Row 0:  " Charger Diag       "   (yellow on dark blue)
Row 2:  " VBUS:  5023 mV     "
Row 3:  " VBAT:  3842 mV     "
Row 4:  " VSYS:  3845 mV     "
Row 5:  " IBUS:  +102 mA     "
Row 6:  " IBAT:   +96 mA     "
Row 7:  " CHG:CC    VBUS:Adpt"   (green)
Row 9:  " BACK to return     "   (gray hint)
```

## Fuel Gauge Diagnostic (1 Hz live refresh)

```
Row 0:  " Fuel Gauge Diag    "   (yellow on dark blue)
Row 2:  " VBAT:  3842 mV     "
Row 3:  " Curr:   +96 mA     "
Row 4:  " SOC:    72 %       "   (green >20%, red ≤20%)
Row 5:  " Temp:  28.3 C      "
Row 6:  " Cap: 640/890 mAh   "
Row 7:  " SOH: 100% Full     "
Row 9:  " BACK to return     "   (gray hint)
```

## LED Control (interactive)

```
Row 0:  " LED Control        "   (yellow on dark blue)
Row 2:  ">TFT-BL   ON  16/31"   (selected row highlighted)
Row 3:  " Kbd+Grn  OFF 16/31"
Row 4:  " Red      OFF  1/ 3"
Row 6:  " UP/DN select       "   (gray hint)
Row 7:  " LT/RT duty OK=togg "   (gray hint)
Row 9:  " BACK to return     "   (gray hint)
```

## Memory Diagnostic

```
Row 0:  " Memory Diagnostic  "   (yellow on dark blue)
Row 2:  " SRAM 16KB  PASS    "   (green / red)
Row 3:  " JEDEC      PASS    "
Row 4:  " QE Bit     PASS    "
Row 5:  " PSRAM Init PASS    "
Row 6:  " PSRAM 4KB  PASS    "
Row 8:  " 5/5 PASS           "
Row 10: " BACK to return     "   (gray hint)
```

## PSRAM Speed Test

```
Row 0:  " PSRAM Speed Test   "   (yellow on dark blue)
Row 2:  " Wr: 4041ms 2027KB/s"
Row 3:  " Rd: 5019ms 1632KB/s"   (green if 0 errors, red otherwise)
Row 7:  " Wr:1926ns 288clk   "
Row 8:  " Rd:2393ns 358clk   "
Row 10: " PASS  err=0        "   (green / red)
Row 12: " BACK to return     "   (gray hint)
```
