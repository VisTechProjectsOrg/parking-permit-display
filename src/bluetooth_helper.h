#ifndef BLUETOOTH_HELPER_H
#define BLUETOOTH_HELPER_H

// The display is a BLE client: the phone (ParkingPermitSync) runs a GATT server and
// advertises all the time, so a short scan on each wake finds it when it's in range.
// A paired display only connects to the phone advertising its owner ID (see permit_store.h).

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

#define FW_VERSION 4

// Check-ins that failed since the last good one, sent with the next good one. RTC memory:
// survives deep sleep, lost on a restart (which the restart count shows instead).
RTC_DATA_ATTR uint16_t missNotFound = 0;
RTC_DATA_ATTR uint16_t missFailed = 0;

// SYNC_UNPAIRED: the phone forgot this display, pairing and permits are cleared
enum SyncResult { SYNC_NOT_FOUND, SYNC_FAILED, SYNC_OK, SYNC_UNPAIRED };

static BLEAdvertisedDevice *targetDevice = nullptr;
static bool scanPaired = false;                 // only accept the phone below
static uint8_t scanOwner[OWNER_ID_LEN];
static bool foundHasOwner = false;              // the phone we found advertises an owner ID
static uint8_t foundOwner[OWNER_ID_LEN];

// Owner ID from the 0xFF00 service data, false if the phone doesn't send one (older app)
static bool readOwnerId(BLEAdvertisedDevice &d, uint8_t out[OWNER_ID_LEN])
{
    for (int i = 0; i < d.getServiceDataCount(); i++) {
        if (!d.getServiceDataUUID(i).equals(BLEUUID(BLE_SERVICE_UUID))) continue;
        std::string data = d.getServiceData(i);
        if (data.length() != OWNER_ID_LEN) return false;
        memcpy(out, data.data(), OWNER_ID_LEN);
        return true;
    }
    return false;
}

class PermitScanCallback : public BLEAdvertisedDeviceCallbacks
{
    void onResult(BLEAdvertisedDevice advertisedDevice)
    {
        if (targetDevice || !advertisedDevice.haveServiceUUID() ||
            !advertisedDevice.isAdvertisingService(BLEUUID(BLE_SERVICE_UUID)))
            return;

        uint8_t owner[OWNER_ID_LEN];
        bool hasOwner = readOwnerId(advertisedDevice, owner);
        if (scanPaired && !(hasOwner && memcmp(owner, scanOwner, OWNER_ID_LEN) == 0)) {
            Serial.println("Ignoring a phone that isn't ours");
            return;
        }
        Serial.println(hasOwner ? "Found phone" : "Found phone (no owner ID)");
        foundHasOwner = hasOwner;
        if (hasOwner) memcpy(foundOwner, owner, OWNER_ID_LEN);
        targetDevice = new BLEAdvertisedDevice(advertisedDevice);
        BLEDevice::getScan()->stop();
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
// Unpaired, it takes any phone, and pairs with it if allowClaim (someone pressed the button
// or just powered it on, so the phone next to it is the owner's).
SyncResult syncWithPhone(uint32_t scanSeconds, uint8_t syncType, PermitStore &store, int batteryMv,
                         bool allowClaim)
{
    scanPaired = loadOwner(scanOwner);
    foundHasOwner = false;
    Serial.printf("BLE: scanning %u s (%s)\n", scanSeconds, scanPaired ? "paired" : "unpaired");
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

    // Ask for bigger packets now: the library's own request on connect is skipped (it checks a
    // connection ID that isn't set yet), which left the link at 20 bytes per write. The permit
    // read below gives the exchange time to settle before the status write.
    client->setMTU(256);

    BLERemoteService *service = client->getService(BLEUUID(BLE_SERVICE_UUID));
    BLERemoteCharacteristic *permitChar = service ? service->getCharacteristic(BLEUUID(BLE_PERMIT_CHAR_UUID)) : nullptr;
    if (!permitChar) {
        Serial.println("Permit characteristic not found");
        bleShutdown(client);
        return SYNC_FAILED;
    }

    // Sync type, then our ID, so the phone knows which display is asking before the read
    BLERemoteCharacteristic *syncTypeChar = service->getCharacteristic(BLEUUID(BLE_SYNC_TYPE_CHAR_UUID));
    if (syncTypeChar) {
        uint8_t hello[13];
        hello[0] = syncType;
        char id[13];
        getDisplayId(id);
        memcpy(hello + 1, id, 12);
        syncTypeChar->writeValue(hello, sizeof(hello), true);
    }

    String json = permitChar->readValue().c_str();
    Serial.println(json);

    JsonDocument doc;
    PermitStore incoming = {};
    bool parsed = !deserializeJson(doc, json);
    // Pairing changes are written to flash only after BLE is shut down: NVS writes while
    // connected crashed the BLE stack on teardown
    if (parsed && (doc["unpair"] | false)) {
        Serial.println("Phone forgot this display: unpairing");
        bleShutdown(client);
        clearOwner();
        clearPermits();
        memset(&store, 0, sizeof(store));
        return SYNC_UNPAIRED;
    }
    if (!parsed || !parsePermit(doc.as<JsonVariantConst>(), incoming.slot[0])) {
        Serial.println("Bad or incomplete permit JSON");
        bleShutdown(client);
        return SYNC_FAILED;
    }
    if (!doc["next"].isNull()) parsePermit(doc["next"], incoming.slot[1]);
    incoming.displayFlipped = doc["displayFlipped"] | false;
    if (doc["now"].is<long long>()) setLocalTime((time_t)doc["now"].as<long long>());
    store = incoming;

    // Tell the phone what we'll show and what we're holding, the battery level, and dev info:
    // "shown,cached,mv,fw,missNotFound,missFailed,lastRestartReason,restarts"
    BLERemoteCharacteristic *statusChar = service->getCharacteristic(BLEUUID(BLE_STATUS_CHAR_UUID));
    if (statusChar) {
        int shown = chooseSlot(store);
        int cached = shown == 0 ? 1 : 0;
        uint32_t boots;
        int reason;
        loadRestartStats(boots, reason);
        char status[96];
        snprintf(status, sizeof(status), "%s,%s,%d,%d,%u,%u,%d,%u",
                 shown >= 0 ? store.slot[shown].permitNumber : "",
                 shown >= 0 ? store.slot[cached].permitNumber : "",
                 batteryMv, FW_VERSION, missNotFound, missFailed, reason, (unsigned)boots);
        // A write longer than the link's packet size gets cut off. The bigger MTU is requested
        // on connect but settles asynchronously, so give it a moment; if it's still the 23-byte
        // default, send just the first four fields (they fit) rather than a chopped string.
        for (int i = 0; i < 100 && client->getMTU() <= 23; i++) delay(10);
        if ((int)strlen(status) > client->getMTU() - 3) {
            Serial.printf("MTU %u: sending the short status\n", client->getMTU());
            snprintf(status, sizeof(status), "%s,%s,%d,%d",
                     shown >= 0 ? store.slot[shown].permitNumber : "",
                     shown >= 0 ? store.slot[cached].permitNumber : "",
                     batteryMv, FW_VERSION);
        }
        statusChar->writeValue((uint8_t *)status, strlen(status), true);
    }

    bleShutdown(client);
    if (!scanPaired && allowClaim && foundHasOwner) {
        saveOwner(foundOwner);
        Serial.println("Paired with this phone");
    }
    return SYNC_OK;
}

#endif
