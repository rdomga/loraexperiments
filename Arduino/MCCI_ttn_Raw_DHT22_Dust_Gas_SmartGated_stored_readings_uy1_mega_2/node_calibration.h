#ifndef NODE_CALIBRATION_H
#define NODE_CALIBRATION_H
// Per-board sleep calibration for uy1-mega-2 (CREDENTIALS_REFERENCE.md Option D), measured 2026-11-01 on the bench
// (USB 5 V, room temperature). Report: Reports/Firmware/SmartGated/Millis_Clock_Diagnosis_Report_2026-11-01.html
// Change these only after re-measuring this chip (Millis_Diag_Tools/calib_node.sh), then re-run make_node_firmware.py.
#define NODE_NAME                 "uy1-mega-2"
#define WDT_SCALE_PERMILLE_NODE   1147   // real ms per 1000 nominal watchdog ms: sleep sweep slope k = 1.1473, rounded half up
#define WDT_AWAKE_PERMILLE_REF    1157   // awake watchdog measurement at that time (informational; the boot calibration measures it again)
#endif
