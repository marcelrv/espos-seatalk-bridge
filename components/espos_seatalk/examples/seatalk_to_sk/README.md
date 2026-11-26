# seatalk_to_sk

Minimal standalone SeaTalk 1 <-> Signal K bridge example consuming
[espOS](https://github.com/signalk-espOS/espOS) from the ESP Component Registry
and `espos_seatalk`.

## Build

Generate an app-signing key once before configuring:

```sh
espsecure generate-signing-key --version 2 --scheme rsa3072 secure_boot_signing_key.pem
idf.py set-target esp32s3   # or esp32
idf.py build flash monitor
```
