#if defined(ESP32) && defined(USE_WIFI_RANGE_EXTENDER)

#define XDRV_123 123

#include "esp_wifi.h"

static volatile bool apa_trigger = false;

static bool apa_started = false;

static uint8_t apa_ap_mac[6] = {0};

static uint32_t apa_retry = 0;
static uint32_t apa_last_trigger = 0;

static void ApAttemptPromiscCb(void *buf, wifi_promiscuous_pkt_type_t type)
{
  if (type != WIFI_PKT_MGMT) {
    return;
  }

  wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;

  if (pkt->rx_ctrl.sig_len < 24) {
    return;
  }

  const uint8_t *frame = pkt->payload;

  uint8_t subtype = (frame[0] >> 4) & 0x0F;

  if ((subtype != 0) && (subtype != 2)) {
    return;
  }

  if (memcmp(&frame[4], apa_ap_mac, 6) != 0) {
    return;
  }

  apa_trigger = true;
}

void ApAttemptStart(void)
{
  if (esp_wifi_get_mac(WIFI_IF_AP, apa_ap_mac) != ESP_OK) {
    return;
  }

  wifi_promiscuous_filter_t filter = {
    .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT
  };

  if (esp_wifi_set_promiscuous_filter(&filter) != ESP_OK) {
    return;
  }

  if (esp_wifi_set_promiscuous_rx_cb(ApAttemptPromiscCb) != ESP_OK) {
    return;
  }

  if (esp_wifi_set_promiscuous(true) != ESP_OK) {
    return;
  }

  apa_started = true;

  AddLog(
    LOG_LEVEL_INFO,
    PSTR("APA: Ready AP %02X:%02X:%02X:%02X:%02X:%02X"),
    apa_ap_mac[0],
    apa_ap_mac[1],
    apa_ap_mac[2],
    apa_ap_mac[3],
    apa_ap_mac[4],
    apa_ap_mac[5]
  );
}

void ApAttemptStop(void)
{
  if (apa_started) {
    esp_wifi_set_promiscuous(false);
  }

  apa_started = false;
  apa_trigger = false;
  apa_retry = 0;
  apa_last_trigger = 0;
}

bool ApAttemptRgxActive(void)
{
  return Settings->sbflag1.range_extender &&
         RgxApUp() &&
         (WiFi.getMode() == WIFI_AP_STA);
}

void ApAttemptLoop(void)
{
  if (!ApAttemptRgxActive()) {
    ApAttemptStop();
    return;
  }

  if (!apa_started) {

    if ((int32_t)(millis() - apa_retry) >= 0) {

      apa_retry = millis() + 2000;

      ApAttemptStart();
    }

    return;
  }

  if (apa_trigger) {

    apa_trigger = false;

    uint32_t now = millis();

    if ((apa_last_trigger == 0) ||
        ((now - apa_last_trigger) >= 1000)) {

      apa_last_trigger = now;

      AddLog(
        LOG_LEVEL_INFO,
        PSTR("APA: AP connection attempt")
      );

      ExecuteCommand(
        "Event ApAttempt=1",
        SRC_RULE
      );
    }
  }
}

bool Xdrv123(uint32_t function)
{
  switch (function) {
    case FUNC_EVERY_100_MSECOND:
      ApAttemptLoop();
      break;

    case FUNC_NETWORK_DOWN:
      ApAttemptStop();
      break;
  }

  return false;
}

#endif  // ESP32 && USE_WIFI_RANGE_EXTENDER
