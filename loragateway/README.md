# LoRa gateway installer

`lora_gateway-github-py3-20261127.zip` is the current gateway distribution.
It targets a Raspberry Pi 2 or 3 with an SX127x radio (tested with a Dragino
LoRa Shield 1.4) on Raspberry Pi OS Buster or Debian 13 (Trixie).

On the Pi, logged in as the account that will own the gateway:

    cd ~
    unzip lora_gateway-github-py3-20261127.zip
    cd lora_gateway
    sudo ./install.sh

The installer compiles the radio binary for the board and OS it finds, installs
Caddy and php-fpm for the web panel, and writes the systemd units without
starting them. To start the services:

    sudo scripts/gw_web_ctl.sh restart_services

The web panel is at `http://<pi-ip>/web/dashboard/`. The default login is
`admin` / `admin`; change it on first login.

The zip contains placeholders only: `key_LBS.py` has a TTN LBS API key
placeholder and `gateway_conf.json` has an all-zero gateway ID. Set both for
your gateway before starting the services.

The full documentation is inside the zip. `lora_gateway/README.md` covers the
installer and panel, `README-LBS.md` covers the TTN Basics Station connection,
and `README-TTN.md` covers the legacy MQTT and UDP path.

Upstream project: https://github.com/CongducPham/LowCostLoRaGw
