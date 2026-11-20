#ifndef BLUETOOTH_HELPER_H
#define BLUETOOTH_HELPER_H

// The display is a BLE client: the phone (ParkingPermitSync) runs a GATT server and
// advertises all the time, so a short scan on each wake finds it when it's in range.

#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEClient.h>
#include <ArduinoJson.h>
#include "permit_store.h"

#define BLE_SERVICE_UUID        "0000ff00-0000-1000-8000-00805f9b34fb"
#define BLE_PERMIT_CHAR_UUID    "0000ff01-0000-1000-8000-00805f9b34fb"  // read: permit JSON
#define BLE_SYNC_TYPE_CHAR_UUID "0000ff02-0000-1000-8000-00805f9b34fb"  // write: why we're syncing
#define BLE_STATUS_CHAR_UUID    "0000ff03-0000-1000-8000-00805f9b34fb"  // write: what we show + battery

// Sync types - the phone notifies on manual/force syncs, and on auto syncs only for a new permit
#define SYNC_TYPE_AUTO   1  // timer wake or power-on
#define SYNC_TYPE_MANUAL 2  // button press
#define SYNC_TYPE_FORCE  3  // long press

#define FW_VERSION 2

enum SyncResult { SYNC_NOT_FOUND, SYNC_FAILED, SYNC_OK };

static BLEAdvertisedDevice *targetDevice = nullptr;

class PermitScanCallback : public BLEAdvertisedDeviceCallbacks
{
    void onResult(BLEAdvertisedDevice advertisedDevice)
    {
        if (advertisedDevice.haveServiceUUID() &&
            advertisedDevice.isAdvertisingService(BLEUUID(BLE_SERVICE_UUID)))
        {
            Serial.println("Found phone");
            targetDevice = new BLEAdvertisedDevice(advertisedDevice);
            BLEDevice::getScan()->stop();
        }
    }
};

static void copyField(char *dst, size_t size, JsonVariantConst v)
{
    strlcpy(dst, v | "", size);
}

// Fill a permit from a JSON object; false if any field the display draws is missing
static bool parsePermit(JsonVariantConst o, PermitData &p)
{
    memset(&p, 0, sizeof(p));
    copyField(p.permitNumber, sizeof(p.permitNumber), o["permitNumber"]);
    copyField(p.plateNumber, sizeof(p.plateNumber), o["plateNumber"]);
    copyField(p.validFrom, sizeof(p.validFrom), o["validFrom"]);
    copyField(p.validTo, sizeof(p.validTo), o["validTo"]);
    copyField(p.barcodeValue, sizeof(p.barcodeValue), o["barcodeValue"]);
    copyField(p.barcodeLabel, sizeof(p.barcodeLabel), o["barcodeLabel"]);
    return p.permitNumber[0] && p.plateNumber[0] && p.validFrom[0] && p.validTo[0] &&
           p.barcodeValue[0] && p.barcodeLabel[0];
}

static void bleShutdown(BLEClient *client)
{
    if (client) {
        if (client->isConnected()) client->disconnect();
        delete client;
    }
    if (targetDevice) {
        delete targetDevice;
        targetDevice = nullptr;
    }
    BLEDevice::deinit(false);
}

// Scan for the phone, read the permits and the time, write back our status.
// On SYNC_OK, store holds the phone's data (time is set too if the phone sent it).
SyncResult syncWithPhone(uint32_t scanSeconds, uint8_t syncType, PermitStore &store, int batteryMv)
{
    Serial.printf("BLE: scanning %u s\n", scanSeconds);
    BLEDevice::init("ParkingDisplay");
    BLEDevice::setMTU(256);  // whole permit JSON in few packets, status write in one

    BLEScan *scan = BLEDevice::getScan();
    scan->setAdvertisedDeviceCallbacks(new PermitScanCallback());
    scan->setActiveScan(true);
    scan->setInterval(100);
    scan->setWindow(99);
    scan->start(scanSeconds, false);

    if (!targetDevice) {
        Serial.println("Phone not in range");
        bleShutdown(nullptr);
        return SYNC_NOT_FOUND;
    }

    BLEClient *client = BLEDevice::createClient();
    bool connected = false;
    for (int attempt = 0; attempt < 2 && !connected; attempt++) {
        connected = client->connect(targetDevice);
        if (!connected) delay(300);
    }
    if (!connected) {
        Serial.println("Connect failed");
        bleShutdown(client);
        return SYNC_FAILED;
    }

    BLERemoteService *service = client->getService(BLEUUID(BLE_SERVICE_UUID));
    BLERemoteCharacteristic *permitChar = service ? service->getCharacteristic(BLEUUID(BLE_PERMIT_CHAR_UUID)) : nullptr;
    if (!permitChar) {
        Serial.println("Permit characteristic not found");
        bleShutdown(client);
        return SYNC_FAILED;
    }

    BLERemoteCharacteristic *syncTypeChar = service->getCharacteristic(BLEUUID(BLE_SYNC_TYPE_CHAR_UUID));
    if (syncTypeChar) syncTypeChar->writeValue(&syncType, 1, false);

    String json = permitChar->readValue().c_str();
    Serial.println(json);

    JsonDocument doc;
    PermitStore incoming = {};
    if (deserializeJson(doc, json) || !parsePermit(doc.as<JsonVariantConst>(), incoming.slot[0])) {
        Serial.println("Bad or incomplete permit JSON");
        bleShutdown(client);
        return SYNC_FAILED;
    }
    if (!doc["next"].isNull()) parsePermit(doc["next"], incoming.slot[1]);
    incoming.displayFlipped = doc["displayFlipped"] | false;
    if (doc["now"].is<long long>()) setLocalTime((time_t)doc["now"].as<long long>());
    store = incoming;

    // Tell the phone what we'll show and what we're holding, plus the battery level
    BLERemoteCharacteristic *statusChar = service->getCharacteristic(BLEUUID(BLE_STATUS_CHAR_UUID));
    if (statusChar) {
        int shown = chooseSlot(store);
        int cached = shown == 0 ? 1 : 0;
        char status[64];
        snprintf(status, sizeof(status), "%s,%s,%d,%d",
                 shown >= 0 ? store.slot[shown].permitNumber : "",
                 shown >= 0 ? store.slot[cached].permitNumber : "",
                 batteryMv, FW_VERSION);
        statusChar->writeValue((uint8_t *)status, strlen(status), true);
    }

    bleShutdown(client);
    return SYNC_OK;
}

#endif
