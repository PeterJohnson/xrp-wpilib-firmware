#include "bluetooth_transport.h"

#include <Arduino.h>
#include <BTstackLib.h>

#include <stdint.h>
#include <string.h>

extern "C" {
#include "btstack_event.h"
#include "btstack_util.h"
#include "hci.h"
#include "l2cap.h"
}

namespace bluetooth_transport {
namespace {

constexpr uint8_t kRxQueueDepth = 4;
constexpr uint16_t kInitialCredits = L2CAP_LE_AUTOMATIC_CREDITS;

struct PacketSlot {
  uint16_t size = 0;
  uint8_t data[kMaxPacketSize];
};

PacketSlot rxQueue[kRxQueueDepth];
volatile uint8_t rxWriteIndex = 0;
volatile uint8_t rxReadIndex = 0;
volatile bool rxOverflow = false;

uint8_t l2capReceiveBuffer[kMaxPacketSize];
uint16_t channelId = 0;
uint16_t remoteMtu = kMaxPacketSize;

uint8_t txBuffer[kMaxPacketSize];
uint16_t txSize = 0;
volatile bool txPending = false;
volatile bool txCanSendRequested = false;

btstack_packet_callback_registration_t l2capEventRegistration;

uint8_t nextQueueIndex(uint8_t index) {
  return static_cast<uint8_t>((index + 1) % kRxQueueDepth);
}

void queueIncomingPacket(const uint8_t* packet, uint16_t size) {
  if (size == 0 || size > kMaxPacketSize) {
    return;
  }

  uint8_t nextWrite = nextQueueIndex(rxWriteIndex);
  if (nextWrite == rxReadIndex) {
    rxOverflow = true;
    return;
  }

  PacketSlot& slot = rxQueue[rxWriteIndex];
  memcpy(slot.data, packet, size);
  slot.size = size;
  rxWriteIndex = nextWrite;
}

void clearConnection() {
  channelId = 0;
  remoteMtu = kMaxPacketSize;
  txPending = false;
  txCanSendRequested = false;
}

void requestCanSend() {
  if (channelId == 0 || !txPending || txCanSendRequested) {
    return;
  }

  txCanSendRequested = l2cap_request_can_send_now_event(channelId) == ERROR_CODE_SUCCESS;
}

void handleCanSendNow(uint16_t eventChannelId) {
  txCanSendRequested = false;

  if (channelId == 0 || eventChannelId != channelId || !txPending) {
    return;
  }

  uint8_t result = l2cap_send(channelId, txBuffer, txSize);
  if (result == ERROR_CODE_SUCCESS) {
    txPending = false;
    txSize = 0;
  }
}

void packetHandler(uint8_t packetType, uint16_t channel, uint8_t* packet, uint16_t size) {
  (void)channel;

  switch (packetType) {
    case HCI_EVENT_PACKET:
      switch (hci_event_packet_get_type(packet)) {
        case L2CAP_EVENT_CBM_INCOMING_CONNECTION: {
          uint16_t psm = l2cap_event_cbm_incoming_connection_get_psm(packet);
          uint16_t localCid = l2cap_event_cbm_incoming_connection_get_local_cid(packet);
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
            clearConnection();
            break;
          }

          channelId = l2cap_event_cbm_channel_opened_get_local_cid(packet);
          remoteMtu = l2cap_event_cbm_channel_opened_get_remote_mtu(packet);

          Serial.printf("[BT] L2CAP CBM channel open cid=0x%04x remote_mtu=%u\n",
                        channelId, remoteMtu);
          requestCanSend();
          break;
        }

        case L2CAP_EVENT_CHANNEL_CLOSED:
          Serial.println("[BT] L2CAP channel closed");
          clearConnection();
          break;

        case L2CAP_EVENT_CAN_SEND_NOW:
          handleCanSendNow(l2cap_event_can_send_now_get_local_cid(packet));
          break;

        default:
          break;
      }
      break;

    case L2CAP_DATA_PACKET:
      queueIncomingPacket(packet, size);
      break;

    default:
      break;
  }
}

}  // namespace

void begin(const char* deviceName) {
  clearConnection();
  rxWriteIndex = 0;
  rxReadIndex = 0;
  rxOverflow = false;

  BTstack.setup(deviceName);

  l2capEventRegistration.callback = &packetHandler;
  l2cap_add_event_handler(&l2capEventRegistration);

  uint8_t result = l2cap_cbm_register_service(&packetHandler, kLePsm, LEVEL_0);
  if (result != ERROR_CODE_SUCCESS) {
    Serial.printf("[BT] Failed to register L2CAP CBM service: 0x%02x\n", result);
  }

  BTstack.startAdvertising();

  Serial.printf("[BT] Advertising %s, L2CAP LE PSM 0x%04x\n", deviceName, kLePsm);
}

bool connected() {
  return channelId != 0;
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
  if (!connected() || txPending || packetSize == 0 || packetSize > kMaxPacketSize ||
      packetSize > remoteMtu) {
    return false;
  }

  memcpy(txBuffer, buffer, packetSize);
  txSize = packetSize;
  txPending = true;
  requestCanSend();

  return true;
}

}  // namespace bluetooth_transport
