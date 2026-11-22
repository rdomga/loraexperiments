# ThingsBoard and TTN formatters

These files belong to the data path of the deployed node firmware,
`Arduino/MCCI_ttn_Raw_DHT22_Dust_Gas_SmartGated_stored_readings_uy1_mega_2`.

| File | Where it goes |
|---|---|
| `public_dashboard.json` | ThingsBoard dashboard import |
| `ttn_uplink_formatter.js` | TTN application uplink payload formatter |
| `ttn_downlink_formatter.js` | TTN application downlink payload formatter |

## Install ThingsBoard

Use the official installation guide for your platform:
https://thingsboard.io/docs/user-guide/install/

The dashboard export comes from ThingsBoard CE 4.3.1.8, installed with the
Docker Compose and PostgreSQL option.

## Import the dashboard

1. Sign in as a tenant administrator.
2. Open Dashboards, click the plus button and choose Import dashboard.
3. Select `public_dashboard.json`.
4. Open the dashboard, click Share and confirm that access is set to Public.

Older ThingsBoard releases may drop widgets that use fields from 4.3.

## Upload the TTN formatters

In the TTN console, open the application and go to Payload formatters.

1. Uplink: paste `ttn_uplink_formatter.js` and save.
2. Downlink: paste `ttn_downlink_formatter.js` and save.

The uplink formatter decodes port 1 (14-byte reading), port 2 (status, up to
46 bytes) and port 3 (stored readings, v1 and v2). The downlink formatter
encodes the `/@...#` command set. Unknown keys return an error and queue
nothing.
