// USB host link to the Beat Gateway (XIAO ESP32-S3) on the Tab5's USB-A port.
// A small CDC-ACM host: finds the gateway's CDC data interface (it is a CDC + MIDI composite device), raises DTR
// so it starts talking, reads its text lines and writes commands back. Hot-plug safe.
#pragma once
#include <Arduino.h>
#include "usb/usb_host.h"

namespace gw {

typedef void (*LineFn)(const char* line);

static usb_host_client_handle_t client;
static usb_device_handle_t dev = nullptr;
static usb_transfer_t *xIn = nullptr, *xOut = nullptr, *xCtl = nullptr;
static uint8_t ifComm = 0xFF, ifData = 0xFF, epIn = 0, epOut = 0;
static uint16_t mpsIn = 64;
static volatile uint8_t newAddr = 0;
static volatile bool gone = false, ready = false, outBusy = false, inBusy = false;
static char name[40];
static char lineBuf[160]; static int lineLen = 0;
static LineFn onLine = nullptr;
static QueueHandle_t outQ;                       // pending command lines (copied, up to 48 chars)
struct Out { char s[48]; };

static void inDone(usb_transfer_t* t) {
  inBusy = false;
  if (t->status == USB_TRANSFER_STATUS_COMPLETED) {
    for (int i = 0; i < t->actual_num_bytes; i++) {
      char c = (char)t->data_buffer[i];
      if (c == '\n' || c == '\r') { if (lineLen) { lineBuf[lineLen] = 0; if (onLine) onLine(lineBuf); lineLen = 0; } }
      else if (lineLen < (int)sizeof(lineBuf) - 1) lineBuf[lineLen++] = c;
    }
  }
  if (ready && !gone && (t->status == USB_TRANSFER_STATUS_COMPLETED || t->status == USB_TRANSFER_STATUS_TIMED_OUT)) {
    t->num_bytes = mpsIn * 4;
    if (usb_host_transfer_submit(t) == ESP_OK) inBusy = true;
  }
}
static void outDone(usb_transfer_t*) { outBusy = false; }
static void ctlDone(usb_transfer_t*) {}

static void clientEvent(const usb_host_client_event_msg_t* m, void*) {
  if (m->event == USB_HOST_CLIENT_EVENT_NEW_DEV) newAddr = m->new_dev.address;
  else if (m->event == USB_HOST_CLIENT_EVENT_DEV_GONE) gone = true;
}

static void strDesc(const usb_str_desc_t* d, char* out, int n) {
  int k = 0;
  if (d) for (int i = 0; i < (d->bLength - 2) / 2 && k < n - 1; i++) { uint16_t c = d->wData[i]; out[k++] = c < 128 ? (char)c : '?'; }
  out[k] = 0;
}

// find the CDC comm (class 2) and data (class 10) interfaces and the data interface's bulk endpoints
static bool parseConfig(const usb_config_desc_t* cfg) {
  const uint8_t* p = (const uint8_t*)cfg; int total = cfg->wTotalLength, off = 0;
  uint8_t curIf = 0xFF, curClass = 0;
  ifComm = ifData = 0xFF; epIn = epOut = 0;
  while (off + 2 <= total) {
    uint8_t len = p[off], type = p[off + 1];
    if (len < 2) break;
    if (type == 0x04 && len >= 9) {                  // interface
      curIf = p[off + 2]; curClass = p[off + 5];
      if (curClass == 0x02 && ifComm == 0xFF) ifComm = curIf;
    } else if (type == 0x05 && len >= 7 && curClass == 0x0A) {   // endpoint inside a CDC data interface
      uint8_t addr = p[off + 2], attr = p[off + 3]; uint16_t mps = p[off + 4] | (p[off + 5] << 8);
      if ((attr & 3) == 2) {
        if ((addr & 0x80) && !epIn) { epIn = addr; mpsIn = mps; ifData = curIf; }
        else if (!(addr & 0x80) && !epOut) epOut = addr;
      }
    }
    off += len;
  }
  return ifData != 0xFF && epIn && epOut;
}

static void openDevice(uint8_t addr) {
  if (dev) return;                                   // one gateway at a time
  usb_device_handle_t d;
  if (usb_host_device_open(client, addr, &d) != ESP_OK) return;
  const usb_config_desc_t* cfg;
  if (usb_host_get_active_config_descriptor(d, &cfg) != ESP_OK || !parseConfig(cfg)) {
    Serial.printf("# usb: device %u has no CDC serial, ignored\n", addr);
    usb_host_device_close(client, d); return;
  }
  if (ifComm != 0xFF) usb_host_interface_claim(client, d, ifComm, 0);
  if (usb_host_interface_claim(client, d, ifData, 0) != ESP_OK) {
    if (ifComm != 0xFF) usb_host_interface_release(client, d, ifComm);
    usb_host_device_close(client, d); return;
  }
  usb_device_info_t info; name[0] = 0;
  if (usb_host_device_info(d, &info) == ESP_OK) strDesc(info.str_desc_product, name, sizeof(name));
  dev = d; gone = false;
  // SET_CONTROL_LINE_STATE: DTR + RTS on, so the gateway starts writing
  usb_setup_packet_t* sp = (usb_setup_packet_t*)xCtl->data_buffer;
  sp->bmRequestType = 0x21; sp->bRequest = 0x22; sp->wValue = 0x03; sp->wIndex = ifComm != 0xFF ? ifComm : 0; sp->wLength = 0;
  xCtl->num_bytes = sizeof(usb_setup_packet_t); xCtl->device_handle = dev; xCtl->bEndpointAddress = 0;
  xCtl->callback = ctlDone;
  usb_host_transfer_submit_control(client, xCtl);
  xIn->device_handle = dev; xIn->bEndpointAddress = epIn; xIn->callback = inDone; xIn->num_bytes = mpsIn * 4;
  xOut->device_handle = dev; xOut->bEndpointAddress = epOut; xOut->callback = outDone;
  ready = true; lineLen = 0;
  if (usb_host_transfer_submit(xIn) == ESP_OK) inBusy = true;
  Serial.printf("# usb: gateway \"%s\" on address %u (data if %u, in 0x%02X out 0x%02X)\n", name, addr, ifData, epIn, epOut);
}

static void closeDevice() {
  ready = false;
  if (!dev) return;
  uint32_t t = millis();
  while ((inBusy || outBusy) && millis() - t < 300) { usb_host_client_handle_events(client, 10); }
  usb_host_interface_release(client, dev, ifData);
  if (ifComm != 0xFF) usb_host_interface_release(client, dev, ifComm);
  usb_host_device_close(client, dev);
  dev = nullptr; inBusy = outBusy = false; name[0] = 0;
  Serial.println("# usb: gateway unplugged");
}

static void libTask(void*) {
  for (;;) { uint32_t f; usb_host_lib_handle_events(portMAX_DELAY, &f); if (f & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all(); }
}

static void clientTask(void*) {
  for (;;) {
    usb_host_client_handle_events(client, pdMS_TO_TICKS(5));
    if (newAddr) { uint8_t a = newAddr; newAddr = 0; openDevice(a); }
    if (gone) { gone = false; closeDevice(); }
    Out o;
    if (ready && !outBusy && xQueueReceive(outQ, &o, 0) == pdTRUE) {
      int n = strlen(o.s); memcpy(xOut->data_buffer, o.s, n); xOut->num_bytes = n;
      outBusy = true;
      if (usb_host_transfer_submit(xOut) != ESP_OK) outBusy = false;
    }
  }
}

// ---------------- public ----------------
bool begin(LineFn fn) {
  onLine = fn;
  outQ = xQueueCreate(8, sizeof(Out));
  usb_host_config_t hc = {}; hc.intr_flags = ESP_INTR_FLAG_LEVEL1;
  if (usb_host_install(&hc) != ESP_OK) { Serial.println("# usb: host install failed"); return false; }
  xTaskCreatePinnedToCore(libTask, "usblib", 4096, nullptr, 6, nullptr, 0);
  usb_host_client_config_t cc = {}; cc.is_synchronous = false; cc.max_num_event_msg = 5;
  cc.async.client_event_callback = clientEvent; cc.async.callback_arg = nullptr;
  if (usb_host_client_register(&cc, &client) != ESP_OK) return false;
  usb_host_transfer_alloc(512, 0, &xIn);
  usb_host_transfer_alloc(64, 0, &xOut);
  usb_host_transfer_alloc(64, 0, &xCtl);
  xTaskCreatePinnedToCore(clientTask, "usbgw", 6144, nullptr, 7, nullptr, 0);
  return true;
}
bool connected() { return ready && dev != nullptr; }
const char* product() { return name; }
// queue one command line ("F,1280,0,200\n"); returns false if not connected or the queue is full
bool send(const char* s) {
  if (!connected()) return false;
  Out o; strncpy(o.s, s, sizeof(o.s) - 1); o.s[sizeof(o.s) - 1] = 0;
  return xQueueSend(outQ, &o, 0) == pdTRUE;
}

}  // namespace gw
