# Steps to generate certs for your WiFi device

## Work in the cert directory
Perform the following steps while in the ./cert directory. This is where the generated
header files will be looked for, and it contains convenient `.gitignore` entries to prevent
your certs from being leaked.

## Install dependencies
First, clone nRF Cloud utils into this (the cert) directory:

`git clone https://github.com/nRFCloud/utils.git cloud-utils``


Next, install MFW tool requirements:

`pip3 install -r cloud-utils/python/modem-firmware-1.3+/requirements.txt`


## Create self-signed CA certificate

Now, create a self-signed ca certificate with `create_ca_cert.py`:
`python3 cloud-utils/python/modem-firmware-1.3+/create_ca_cert.py -c US`

The `-c US` should be replaced with your two-letter country code. For instance, `-c GB` for users in the United Kingdom.
You only need to do this once, your CA cert can be re-used to generate certs for multiple devices
See https://github.com/nRFCloud/utils/tree/master/python/modem-firmware-1.3%2B for more details

Finally, rename the three generated files to just `ssca_ca.pem`, `ssca_prv.pem` and `ssca_pub.pem`.

## Create device credentials
You must perform this step for every device you wish to provision on nRF Cloud.

`python3 cloud-utils/python/modem-firmware-1.3+/create_device_credentials.py -ca ssca_ca.pem -ca_key ssca_prv.pem -c US Portland -o Nordic -ou Cloud -cn $DEVICE_ID -e $EMAIL -dv 2000`

Once again, `-c US` must be replaced with the appropriate country code.
Replace `-st OR` with your state or province's two letter code (if appropriate)
Replace `-l Portland` with your city
Replace `$DEVICE_ID` with your device ID (`nrf-xxxxxxxxxxxxx`) and `$EMAIL` with your `nrfcloud.net` user email.
Note that this certificate will expire after 2000 days, and you must generate new certs after that.
If you are going to be certifying more than one device, please save a copy of these files for each device.
Running this command multiple times will overwrite them.
See https://github.com/nRFCloud/utils/tree/master/python/modem-firmware-1.3%2B#create-device-credentials for more details.

You should now have three files resembling `device-id_crt.pem`, `device-id_prv.pem`, `device-id_pub.pem`.

## Inform nRF Cloud of the new certs
To do this, we must first create a `certs.csv` file containing our `$DEVICE_ID` and the contents of `device-id_crt.pem`:

```
   $DEVICE_ID,,,,"-----BEGIN CERTIFICATE-----
   (contents of $DEVICE_ID_crt.pem)
   -----END CERTIFICATE-----
   "
```

Now, this `certs.csv` file may be uploaded to nRF Cloud at https://nrfcloud.com/#/provision-devices.
Your device should now appear on the devices page: https://nrfcloud.com/#/devices

## Serialize device credentials
Now, we convert our credential files into binary arrays:

WE NEED TO WRITE WINDOWS VERSION OF THESE INSTRUCTIONS
Ideally, most of this could be automated with a single python script, maybe even one in the cloud utils repo.

"cat ssca_ca.pem | sed -e '1d;$d' | base64 -d -i |xxd -i > ca_cert.h"
"cat $DEVICE_ID_crt.pem | sed -e '1d;$d' | base64 -d -i |xxd -i > client_cert.h"
"cat $DEVICE_ID_prv.pem | sed -e '1d;$d' | base64 -d -i |xxd -i > private_key.h"


WE NEED SOME KIND OF NOTE HERE ABOUT SYNCING THIS UP WITH THE CONFIG.