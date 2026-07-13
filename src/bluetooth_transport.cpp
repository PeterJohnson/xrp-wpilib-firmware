#include "bluetooth_transport.h"

#include <Arduino.h>
#include <BTstackLib.h>

#include <stdint.h>
#include <string.h>

extern "C" {
#include "ble/att_server.h"
#include "bluetooth_data_types.h"
#include "btstack_event.h"
#include "btstack_util.h"
#include "gap.h"
#include "hci.h"
#include "l2cap.h"
}

namespace bluetooth_transport {
namespace {

constexpr uint8_t kRxQueueDepth = 4;
constexpr uint16_t kInitialCredits = L2CAP_LE_AUTOMATIC_CREDITS;
constexpr uint16_t kDefaultGattPayloadMtu = 20;
constexpr uint16_t kConnectionSupervisionTimeout = 200;  // 2 seconds.

enum class Transport : uint8_t {
  kNone,
  kL2cap,
  kGatt,
};

struct PacketSlot {
  uint16_t size = 0;
  uint8_t data[kMaxPacketSize];
};

PacketSlot rxQueue[kRxQueueDepth];
volatile uint8_t rxWriteIndex = 0;
volatile uint8_t rxReadIndex = 0;
volatile bool rxOverflow = false;

uint8_t l2capReceiveBuffer[kMaxPacketSize];
uint16_t l2capChannelId = 0;
hci_con_handle_t l2capConnectionHandle = HCI_CON_HANDLE_INVALID;
uint16_t l2capRemoteMtu = kMaxPacketSize;

hci_con_handle_t gattConnectionHandle = HCI_CON_HANDLE_INVALID;
hci_con_handle_t connectionParameterUpdateHandle = HCI_CON_HANDLE_INVALID;
uint16_t gattControlValueHandle = 0;
uint16_t gattStatusValueHandle = 0;
uint16_t gattStatusCccHandle = 0;
uint16_t gattPayloadMtu = kDefaultGattPayloadMtu;
bool gattNotificationsEnabled = false;

uint8_t txBuffer[kMaxPacketSize];
uint16_t txSize = 0;
volatile bool txPending = false;
volatile bool txCanSendRequested = false;
Transport txTransport = Transport::kNone;
Transport activeTransport = Transport::kNone;

btstack_packet_callback_registration_t hciEventRegistration;
btstack_packet_callback_registration_t l2capEventRegistration;
btstack_context_callback_registration_t gattNotifyRegistration;

uint8_t nextQueueIndex(uint8_t index) {
  return static_cast<uint8_t>((index + 1) % kRxQueueDepth);
}

uint16_t gattNotificationConfiguration() {
  return gattNotificationsEnabled
             ? GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION
             : 0;
}

uint16_t currentGattPayloadMtu() {
  return gattPayloadMtu;
}

void clearPendingTxFor(Transport transport) {
  if (txPending && txTransport == transport) {
    txPending = false;
    txCanSendRequested = false;
    txTransport = Transport::kNone;
    txSize = 0;
  }
}

void finishPendingTx() {
  txPending = false;
  txCanSendRequested = false;
  txTransport = Transport::kNone;
  txSize = 0;
}

bool queueIncomingPacket(const uint8_t* packet, uint16_t size, Transport transport) {
  if (size == 0 || size > kMaxPacketSize) {
    return false;
  }

  uint8_t nextWrite = nextQueueIndex(rxWriteIndex);
  if (nextWrite == rxReadIndex) {
    rxOverflow = true;
    return false;
  }

  PacketSlot& slot = rxQueue[rxWriteIndex];
  memcpy(slot.data, packet, size);
  slot.size = size;
  rxWriteIndex = nextWrite;
  activeTransport = transport;

  return true;
}

void clearL2capConnection() {
  l2capChannelId = 0;
  l2capConnectionHandle = HCI_CON_HANDLE_INVALID;
  l2capRemoteMtu = kMaxPacketSize;
  clearPendingTxFor(Transport::kL2cap);
  if (activeTransport == Transport::kL2cap) {
    activeTransport = Transport::kNone;
  }
}

void clearGattConnection() {
  gattConnectionHandle = HCI_CON_HANDLE_INVALID;
  gattNotificationsEnabled = false;
  gattPayloadMtu = kDefaultGattPayloadMtu;
  clearPendingTxFor(Transport::kGatt);
  if (activeTransport == Transport::kGatt) {
    activeTransport = Transport::kNone;
  }
}

bool canSendL2cap(size_t packetSize) {
  return l2capChannelId != 0 && packetSize <= l2capRemoteMtu;
}

bool canSendGatt(size_t packetSize) {
  return gattConnectionHandle != HCI_CON_HANDLE_INVALID &&
         gattNotificationsEnabled && packetSize <= currentGattPayloadMtu();
}

Transport selectTxTransport(size_t packetSize) {
  if (activeTransport == Transport::kL2cap) {
    return canSendL2cap(packetSize) ? Transport::kL2cap : Transport::kNone;
  }

  if (activeTransport == Transport::kGatt) {
    return canSendGatt(packetSize) ? Transport::kGatt : Transport::kNone;
  }

  if (canSendL2cap(packetSize)) {
    return Transport::kL2cap;
  }

  if (canSendGatt(packetSize)) {
    return Transport::kGatt;
  }

  return Transport::kNone;
}

void requestPreferredConnectionParameters(hci_con_handle_t handle) {
  if (handle == HCI_CON_HANDLE_INVALID || connectionParameterUpdateHandle == handle) {
    return;
  }

  int result = gap_request_connection_parameter_update(
      handle, kPreferredConnectionIntervalMin, kPreferredConnectionIntervalMax,
      kPreferredSlaveLatency, kConnectionSupervisionTimeout);
  if (result == ERROR_CODE_SUCCESS) {
    connectionParameterUpdateHandle = handle;
    Serial.printf("[BT] Requested connection interval %u-%u units, latency %u\n",
                  kPreferredConnectionIntervalMin,
                  kPreferredConnectionIntervalMax,
                  kPreferredSlaveLatency);
  } else {
    Serial.printf("[BT] Connection parameter request failed: 0x%02x\n", result);
  }
}

void updateGattPayloadMtu(uint16_t attMtu) {
  if (attMtu > 3) {
    gattPayloadMtu = attMtu - 3;
  } else {
    gattPayloadMtu = 0;
  }
}

void handleL2capCanSendNow(uint16_t eventChannelId) {
  txCanSendRequested = false;

  if (!txPending || txTransport != Transport::kL2cap) {
    return;
  }

  if (l2capChannelId == 0 || eventChannelId != l2capChannelId) {
    return;
  }

  uint8_t result = l2cap_send(l2capChannelId, txBuffer, txSize);
  (void)result;
  finishPendingTx();
}

void handleGattCanSendNow(void* context) {
  (void)context;
  txCanSendRequested = false;

  if (!txPending || txTransport != Transport::kGatt) {
    return;
  }

  if (gattConnectionHandle == HCI_CON_HANDLE_INVALID ||
      !gattNotificationsEnabled || txSize > currentGattPayloadMtu()) {
    finishPendingTx();
    return;
  }

  uint8_t result =
      att_server_notify(gattConnectionHandle, gattStatusValueHandle, txBuffer, txSize);
  (void)result;
  finishPendingTx();
}

void requestCanSend() {
  if (!txPending || txCanSendRequested) {
    return;
  }

  if (txTransport == Transport::kL2cap) {
    if (l2capChannelId == 0) {
      return;
    }

    uint8_t result = l2cap_request_can_send_now_event(l2capChannelId);
    txCanSendRequested =
        result == ERROR_CODE_SUCCESS || result == ERROR_CODE_COMMAND_DISALLOWED;
    return;
  }

  if (txTransport == Transport::kGatt) {
    if (gattConnectionHandle == HCI_CON_HANDLE_INVALID ||
        !gattNotificationsEnabled) {
      return;
    }

    gattNotifyRegistration.callback = &handleGattCanSendNow;
    gattNotifyRegistration.context = nullptr;
    uint8_t result = att_server_request_to_send_notification(
        &gattNotifyRegistration, gattConnectionHandle);
    txCanSendRequested =
        result == ERROR_CODE_SUCCESS || result == ERROR_CODE_COMMAND_DISALLOWED;
  }
}

bool appendAdField(uint8_t* data, uint8_t& pos, uint8_t type,
                   const uint8_t* value, uint8_t valueSize) {
  if (pos + valueSize + 2 > 31) {
    return false;
  }

  data[pos++] = valueSize + 1;
  data[pos++] = type;
  memcpy(&data[pos], value, valueSize);
  pos += valueSize;

  return true;
}

void configureAdvertisement(const char* deviceName) {
  uint8_t advData[31];
  uint8_t advPos = 0;

  uint8_t flags = 0x06;  // LE General Discoverable, BR/EDR not supported.
  appendAdField(advData, advPos, BLUETOOTH_DATA_TYPE_FLAGS, &flags, sizeof(flags));

  UUID serviceUuid(kGattServiceUuid);
  uint8_t serviceUuidLe[16];
  reverse_128(serviceUuid.getUuid(), serviceUuidLe);
  appendAdField(advData, advPos,
                BLUETOOTH_DATA_TYPE_COMPLETE_LIST_OF_128_BIT_SERVICE_CLASS_UUIDS,
                serviceUuidLe, sizeof(serviceUuidLe));

  uint8_t intervalRange[4];
  little_endian_store_16(intervalRange, 0, kPreferredConnectionIntervalMin);
  little_endian_store_16(intervalRange, 2, kPreferredConnectionIntervalMax);
  appendAdField(advData, advPos,
                BLUETOOTH_DATA_TYPE_SLAVE_CONNECTION_INTERVAL_RANGE,
                intervalRange, sizeof(intervalRange));

  BTstack.setAdvData(advPos, advData);

  uint8_t scanData[31];
  uint8_t scanPos = 0;
  size_t deviceNameLength = strlen(deviceName);
  uint8_t advertisedNameLength =
      static_cast<uint8_t>(deviceNameLength > 29 ? 29 : deviceNameLength);
  uint8_t nameType = advertisedNameLength == deviceNameLength
                         ? BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME
                         : BLUETOOTH_DATA_TYPE_SHORTENED_LOCAL_NAME;
  appendAdField(scanData, scanPos, nameType,
                reinterpret_cast<const uint8_t*>(deviceName), advertisedNameLength);

  BTstack.setScanData(scanPos, scanData);
}

uint16_t handleGattRead(uint16_t characteristicId, uint8_t* buffer,
                        uint16_t bufferSize) {
  if (characteristicId != gattStatusCccHandle) {
    return 0;
  }

  uint8_t configuration[2];
  little_endian_store_16(configuration, 0, gattNotificationConfiguration());
  if (buffer == nullptr) {
    return sizeof(configuration);
  }

  uint16_t bytesToCopy = bufferSize < sizeof(configuration) ? bufferSize
                                                            : sizeof(configuration);
  memcpy(buffer, configuration, bytesToCopy);
  return bytesToCopy;
}

int handleGattWrite(uint16_t characteristicId, uint8_t* buffer,
                    uint16_t bufferSize) {
  if (characteristicId == gattControlValueHandle) {
    queueIncomingPacket(buffer, bufferSize, Transport::kGatt);
    return 0;
  }

  if (characteristicId == gattStatusCccHandle) {
    if (bufferSize >= 2) {
      uint16_t configuration = little_endian_read_16(buffer, 0);
      gattNotificationsEnabled =
          (configuration & GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION) != 0;
      Serial.printf("[BT] GATT status notifications %s\n",
                    gattNotificationsEnabled ? "enabled" : "disabled");
      if (!gattNotificationsEnabled) {
        clearPendingTxFor(Transport::kGatt);
      }
    }
    return 0;
  }

  return 0;
}

void addGattService() {
  static UUID serviceUuid(kGattServiceUuid);
  static UUID controlUuid(kGattControlCharacteristicUuid);
  static UUID statusUuid(kGattStatusCharacteristicUuid);

  BTstack.addGATTService(&serviceUuid);
  gattControlValueHandle = BTstack.addGATTCharacteristicDynamic(
      &controlUuid, ATT_PROPERTY_WRITE_WITHOUT_RESPONSE, 0);
  gattStatusValueHandle =
      BTstack.addGATTCharacteristicDynamic(&statusUuid, ATT_PROPERTY_NOTIFY, 0);
  gattStatusCccHandle = gattStatusValueHandle + 1;

  BTstack.setGATTCharacteristicRead(&handleGattRead);
  BTstack.setGATTCharacteristicWrite(&handleGattWrite);
}

void packetHandler(uint8_t packetType, uint16_t channel, uint8_t* packet,
                   uint16_t size) {
  switch (packetType) {
    case HCI_EVENT_PACKET:
      switch (hci_event_packet_get_type(packet)) {
        case HCI_EVENT_DISCONNECTION_COMPLETE: {
          hci_con_handle_t handle =
              hci_event_disconnection_complete_get_connection_handle(packet);
          if (handle == l2capConnectionHandle) {
            clearL2capConnection();
          }
          if (handle == gattConnectionHandle) {
            clearGattConnection();
          }
          if (handle == connectionParameterUpdateHandle) {
            connectionParameterUpdateHandle = HCI_CON_HANDLE_INVALID;
          }
          break;
        }

        case HCI_EVENT_LE_META:
          switch (hci_event_le_meta_get_subevent_code(packet)) {
            case HCI_SUBEVENT_LE_CONNECTION_COMPLETE:
              if (hci_subevent_le_connection_complete_get_status(packet) ==
                  ERROR_CODE_SUCCESS) {
                requestPreferredConnectionParameters(
                    hci_subevent_le_connection_complete_get_connection_handle(packet));
              }
              break;

            case HCI_SUBEVENT_LE_CONNECTION_UPDATE_COMPLETE:
              if (hci_subevent_le_connection_update_complete_get_status(packet) ==
                  ERROR_CODE_SUCCESS) {
                Serial.printf("[BT] Connection interval now %u units, latency %u\n",
                              hci_subevent_le_connection_update_complete_get_conn_interval(
                                  packet),
                              hci_subevent_le_connection_update_complete_get_conn_latency(
                                  packet));
              }
              break;

            default:
              break;
          }
          break;

        case ATT_EVENT_CONNECTED:
          gattConnectionHandle = att_event_connected_get_handle(packet);
          updateGattPayloadMtu(att_server_get_mtu(gattConnectionHandle));
          Serial.printf("[BT] GATT connected handle=0x%04x mtu_payload=%u\n",
                        gattConnectionHandle, currentGattPayloadMtu());
          requestPreferredConnectionParameters(gattConnectionHandle);
          break;

        case ATT_EVENT_DISCONNECTED: {
          hci_con_handle_t handle = att_event_disconnected_get_handle(packet);
          if (handle == gattConnectionHandle) {
            Serial.println("[BT] GATT disconnected");
            clearGattConnection();
          }
          if (handle == connectionParameterUpdateHandle) {
            connectionParameterUpdateHandle = HCI_CON_HANDLE_INVALID;
          }
          break;
        }

        case ATT_EVENT_MTU_EXCHANGE_COMPLETE: {
          hci_con_handle_t handle =
              att_event_mtu_exchange_complete_get_handle(packet);
          if (handle == gattConnectionHandle) {
            updateGattPayloadMtu(att_event_mtu_exchange_complete_get_MTU(packet));
            Serial.printf("[BT] GATT MTU payload=%u\n", currentGattPayloadMtu());
          }
          break;
        }

        case L2CAP_EVENT_CBM_INCOMING_CONNECTION: {
          uint16_t psm = l2cap_event_cbm_incoming_connection_get_psm(packet);
          uint16_t localCid =
              l2cap_event_cbm_incoming_connection_get_local_cid(packet);
          if (psm != kLePsm) {
            break;
          }

          Serial.printf("[BT] Accepting L2CAP CBM connection cid=0x%04x psm=0x%04x\n",
                        localCid, psm);
          l2cap_cbm_accept_connection(localCid, l2capReceiveBuffer,
                                      sizeof(l2capReceiveBuffer), kInitialCredits);
          break;
        }

        case L2CAP_EVENT_CBM_CHANNEL_OPENED: {
          uint8_t status = l2cap_event_cbm_channel_opened_get_status(packet);
          if (status != ERROR_CODE_SUCCESS) {
            Serial.printf("[BT] L2CAP CBM open failed: 0x%02x\n", status);
            clearL2capConnection();
            break;
          }

          l2capConnectionHandle =
              l2cap_event_cbm_channel_opened_get_handle(packet);
          l2capChannelId = l2cap_event_cbm_channel_opened_get_local_cid(packet);
          l2capRemoteMtu =
              l2cap_event_cbm_channel_opened_get_remote_mtu(packet);

          Serial.printf("[BT] L2CAP CBM channel open cid=0x%04x remote_mtu=%u\n",
                        l2capChannelId, l2capRemoteMtu);
          requestPreferredConnectionParameters(l2capConnectionHandle);
          requestCanSend();
          break;
        }

        case L2CAP_EVENT_CHANNEL_CLOSED:
          Serial.println("[BT] L2CAP channel closed");
          clearL2capConnection();
          break;

        case L2CAP_EVENT_CAN_SEND_NOW:
          handleL2capCanSendNow(l2cap_event_can_send_now_get_local_cid(packet));
          break;

        default:
          break;
      }
      break;

    case L2CAP_DATA_PACKET:
      if (channel == l2capChannelId) {
        queueIncomingPacket(packet, size, Transport::kL2cap);
      }
      break;

    default:
      break;
  }
}

}  // namespace

void begin(const char* deviceName) {
  clearL2capConnection();
  clearGattConnection();
  connectionParameterUpdateHandle = HCI_CON_HANDLE_INVALID;
  activeTransport = Transport::kNone;
  finishPendingTx();
  rxWriteIndex = 0;
  rxReadIndex = 0;
  rxOverflow = false;

  addGattService();
  BTstack.setup(deviceName);
  configureAdvertisement(deviceName);

  hciEventRegistration.callback = &packetHandler;
  hci_add_event_handler(&hciEventRegistration);

  att_server_register_packet_handler(&packetHandler);

  l2capEventRegistration.callback = &packetHandler;
  l2cap_add_event_handler(&l2capEventRegistration);

  uint8_t result = l2cap_cbm_register_service(&packetHandler, kLePsm, LEVEL_0);
  if (result != ERROR_CODE_SUCCESS) {
    Serial.printf("[BT] Failed to register L2CAP CBM service: 0x%02x\n", result);
  }

  BTstack.startAdvertising();

  Serial.printf("[BT] Advertising %s, GATT %s, L2CAP LE PSM 0x%04x\n",
                deviceName, kGattServiceUuid, kLePsm);
}

bool connected() {
  return l2capChannelId != 0 || gattConnectionHandle != HCI_CON_HANDLE_INVALID;
}

bool readPacket(char* buffer, size_t bufferSize, size_t* packetSize) {
  if (rxReadIndex == rxWriteIndex) {
    return false;
  }

  PacketSlot& slot = rxQueue[rxReadIndex];
  if (slot.size > bufferSize) {
    rxReadIndex = nextQueueIndex(rxReadIndex);
    return false;
  }

  memcpy(buffer, slot.data, slot.size);
  *packetSize = slot.size;
  rxReadIndex = nextQueueIndex(rxReadIndex);

  return true;
}

bool sendPacket(const char* buffer, size_t packetSize) {
  if (txPending || packetSize == 0 || packetSize > kMaxPacketSize) {
    return false;
  }

  Transport transport = selectTxTransport(packetSize);
  if (transport == Transport::kNone) {
    return false;
  }

  memcpy(txBuffer, buffer, packetSize);
  txSize = packetSize;
  txTransport = transport;
  txPending = true;
  txCanSendRequested = false;
  requestCanSend();

  return true;
}

}  // namespace bluetooth_transport
