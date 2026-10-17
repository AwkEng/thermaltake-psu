# USB dumps

Made by `tools/capture/capture.ps1` (USBPcap, PSU device only); each `.pcap`
has a matching `.txt` with timestamped labels. Decode with
`python tools/capture/pcap_summary.py usbdumps/<file>.pcap`.

| File | What was done |
|------|---------------|
| `01-app-startup` | TT RGB PLUS 3.0.8 started with no prior session, left idle ~55 s |
| `02-fan-rgb` | unlabelled exploration of the app; includes fan mode changes |
| `03-fan-modes` | silent → performance → zero fan → custom 30%..100% in 10% steps → silent, ~15-20 s per step |
