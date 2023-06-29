.. _lib_nrf_cloud_coap:

nRF Cloud CoAP
##############

.. contents::
   :local:
   :depth: 2

This library is an enhancement to the :ref:`lib_nrf_cloud` library.
It enables applications to communicate with nRF Cloud using the nRF Cloud CoAP service.
This service uses UDP network packets, encrypted using DTLS, containing compact data encoded using CBOR.
This service is lighter weight than MQTT or REST, and so consumes less power on the device and consumes less data bandwidth.

Overview
********

This library provides an API for CoAP-based applications to send requests to nRF Cloud and receive responses back.
Like the REST API for nRF Cloud, CoAP is client-driven.
The server cannot initiate transfers to the device.
Instead, the device must periodically poll for information it cares about.

Polling functions
=================

Functions that poll for externally created information:

#. :c:func:`nrf_cloud_coap_shadow_get` -- Retrieve any pending shadow delta (change)
#. :c:func:`nrf_cloud_coap_fota_job_get` -- Retrieve any pending FOTA job

The function :c:func:`nrf_cloud_coap_shadow_get` returns 0 whether there is a delta or not.
Set the delta parameter true to request the delta.
The underlying CoAP result code 2.05 and an empty payload indicating there is no delta.
If there is a pending delta, the function returns result code 2.05 and a payload in JSON format.
When the delta parameter is set false, the whole current delta state section is returned, which can be quite large.

If there is a pending job, the function :c:func:`nrf_cloud_coap_fota_job_get` returns 0 and updates the job structure.
If there is no pending job, the function returns -ENOMSG.

Supported features
==================

Requirements
************

The device must be preprovisoned on nRF Cloud.
Preprovision the device using the `device_credentials_installer.py`_ script and the `nrf_cloud_provision.py`_ script.
You must specify the ``--coap`` option to ``device_credentials_installer.py`` so that the proper root CA certificates are installed in the device.

Call the :c:func:`nrf_cloud_coap_init` function once to initialize the library.
Connect the device to LTE before calling the :c:func:`nrf_cloud_coap_connect` function.

Configuration
*************

The following Kconfig options are available:

* :kconfig:option:`CONFIG_NRF_CLOUD_COAP`
* :kconfig:option:`CONFIG_NRF_CLOUD_COAP_SERVER_HOSTNAME`
* :kconfig:option:`CONFIG_NRF_CLOUD_COAP_SEC_TAG`
* :kconfig:option:`CONFIG_NRF_CLOUD_COAP_THREAD_STACK_SIZE`
* :kconfig:option:`CONFIG_NRF_CLOUD_COAP_RESPONSE_TIMEOUT_MS`

Finally, configure these recommended additional options:

* :kconfig:option:`CONFIG_COAP_CLIENT_BLOCK_SIZE` set to ``1024``.
* :kconfig:option:`CONFIG_COAP_CLIENT_STACK_SIZE` set to ``6144``..`
* :kconfig:option:`CONFIG_COAP_CLIENT_THREAD_PRIORITY` set to ``0``.
* :kconfig:option:`CONFIG_COAP_EXTENDED_OPTIONS_LEN_VALUE` set to ``32``.

Usage
*****

To use this library, complete the following steps:

1. Include the :file:`nrf_cloud_coap.h` file.
#. Call the :c:func:`nrf_cloud_coap_init` function once to initialize the library.
#. Connect the device to an LTE network.
#. Call the :c:func:`nrf_cloud_coap_connect` function to connect to nRF Cloud and obtain authorization to access services.
#. If that is successful, call any of the other functions declared in the header file to access services.
#. Disconnect from LTE when your device will be not needing cloud services for a long period (for example, most of a day).
#. Call the :c:func:`nrf_cloud_coap_disconnect` function to close the network socket, which frees resources in the modem.

Samples using the library
*************************

The following |NCS| samples use this library:

* :ref:`modem_shell_application`

Limitations
***********

For CoAP-based applications, communications will not be as reliable for all nRF Cloud services as when using MQTT or REST.
This is a fundamental aspect of the way CoAP works over UDP compared to TCP.

The loss of the LTE connection or closing of the network socket will result in loss of the session information for DTLS inside the modem.
The device must call :c:func:`nrf_cloud_coap_disconnect`, then call :c:func:`nrf_cloud_coap_connect` once the LTE connection has been restored.
This will result in a new full handshake of the DTLS connection and the need to reestablish authentication with the server.

Due to the same limitations in the modem, a call to :c:func:`nrf_cloud_coap_disconnect` followed by a subsequent call to :c:func:`nrf_cloud_coap_connect` will necessitate a full DTLS handshake and reauthentication.
This is true whether or not the LTE connection is intact.

Dependencies
************

This library uses the following |NCS| libraries:

* :ref:`lib_nrf_cloud`

API documentation
*****************

| Header file: :file:`include/net/nrf_cloud_coap.h`
| Source files: :file:`subsys/net/lib/nrf_cloud/src/nrf_cloud_coap.c`

.. doxygengroup:: nrf_cloud_coap
   :project: nrf
   :members:
