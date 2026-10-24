#pragma once

#include "esphome.h"
#include <SPI.h>

namespace quntis {

// nRF24L01+ SPI Commands & Registers
static const uint8_t NRF_R_REGISTER    = 0x00;
static const uint8_t NRF_W_REGISTER    = 0x20;
static const uint8_t NRF_R_RX_PAYLOAD  = 0x61;
static const uint8_t NRF_W_TX_PAYLOAD  = 0xA0;
static const uint8_t NRF_FLUSH_TX      = 0xE1;
static const uint8_t NRF_FLUSH_RX      = 0xE2;
static const uint8_t NRF_NOP           = 0xFF;

static const uint8_t REG_CONFIG        = 0x00;
static const uint8_t REG_EN_AA         = 0x01;
static const uint8_t REG_EN_RXADDR     = 0x02;
static const uint8_t REG_SETUP_AW      = 0x03;
static const uint8_t REG_SETUP_RETR    = 0x04;
static const uint8_t REG_RF_CH         = 0x05;
static const uint8_t REG_RF_SETUP      = 0x06;
static const uint8_t REG_STATUS        = 0x07;
static const uint8_t REG_RX_ADDR_P0    = 0x0A;
static const uint8_t REG_TX_ADDR       = 0x10;
static const uint8_t REG_RX_PW_P0      = 0x11;
static const uint8_t REG_FIFO_STATUS   = 0x17;

static const uint8_t XN297_SCRAMBLE[16] = {
    0xE3, 0xB1, 0x4B, 0xEA, 0x85, 0xBC, 0xE5, 0x66,
    0x0D, 0xAE, 0x8C, 0x88, 0x12, 0x69, 0xEE, 0x1F
};

// Precomputed XN297 scrambled CRC-16 XOR-out for 16-byte body (index 16 - 3 = 13)
static const uint16_t XN297_CRC_XOROUT_16B = 0xF494;

// Quntis Command IDs (p[4])
static const uint8_t CMD_POWER      = 0x31;
static const uint8_t CMD_RGB_ON     = 0x32;
static const uint8_t CMD_BRI_UP     = 0x35;
static const uint8_t CMD_BRI_DOWN   = 0x36;
static const uint8_t CMD_COLOR_TEMP = 0x39;
static const uint8_t CMD_RGB_OFF    = 0x3D;

class QuntisRF {
 public:
  // Hardware pins
  uint8_t ce_pin_;
  uint8_t csn_pin_;
  uint8_t sck_pin_;
  uint8_t miso_pin_;
  uint8_t mosi_pin_;

  // Device Pipe Address (default: AA 41 5F 98 E7)
  uint8_t addr_pipe_[5] = {0xAA, 0x41, 0x5F, 0x98, 0xE7};
  // Constant 4-byte payload header (default: 83 18 5B 01)
  uint8_t header_[4] = {0x83, 0x18, 0x5B, 0x01};

  // Tracked Lightbar State
  bool front_on_ = true;
  uint8_t brightness_ = 0x55;   // 0x1A (10%) .. 0xFF (100%)
  uint8_t color_temp_ = 0xA2;   // 0x1A (Warm) .. 0xFF (Cool)
  bool rgb_on_ = false;
  uint8_t rgb_mode_ = 14;       // 1 .. 15
  uint8_t seq_counter_ = 1;

  // Sniffer / Pairing Mode (captures any Quntis remote's 5B address + 4B header)
  bool sniff_mode_ = false;

  // Non-blocking TX burst state machine
  // Physical remote sends triplets (3 packets ~3.8ms apart, 28ms between triplets)
  bool tx_active_ = false;
  uint8_t tx_frame_[18];        // 16B scrambled body + 2B CRC
  uint8_t tx_triplets_left_ = 0;
  uint8_t tx_in_triplet_ = 0;
  uint32_t tx_next_ms_ = 0;

  // RX deduplication
  uint8_t last_rx_seq_ = 0xFF;
  uint32_t last_rx_ms_ = 0;
  bool updating_from_rf_ = false;

  QuntisRF(uint8_t ce_pin, uint8_t csn_pin, uint8_t sck_pin, uint8_t miso_pin, uint8_t mosi_pin)
      : ce_pin_(ce_pin), csn_pin_(csn_pin), sck_pin_(sck_pin), miso_pin_(miso_pin), mosi_pin_(mosi_pin) {}

  void set_address(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3, uint8_t b4) {
    addr_pipe_[0] = b0;
    addr_pipe_[1] = b1;
    addr_pipe_[2] = b2;
    addr_pipe_[3] = b3;
    addr_pipe_[4] = b4;
  }

  void set_header(uint8_t h0, uint8_t h1, uint8_t h2, uint8_t h3) {
    header_[0] = h0;
    header_[1] = h1;
    header_[2] = h2;
    header_[3] = h3;
  }

  void set_sniff_mode(bool enable) {
    sniff_mode_ = enable;
    if (!tx_active_) {
      enter_rx_mode();
    }
    if (enable) {
      ESP_LOGW("quntis_rf", "Sniffer/Pairing Mode ENABLED: Hold your physical Quntis remote near the nRF24L01+ antenna and press any button.");
    } else {
      ESP_LOGI("quntis_rf", "Sniffer/Pairing Mode disabled; listening for configured address %02X:%02X:%02X:%02X:%02X",
               addr_pipe_[0], addr_pipe_[1], addr_pipe_[2], addr_pipe_[3], addr_pipe_[4]);
    }
  }

  static uint8_t bit_reverse(uint8_t b) {
    b = ((b & 0xF0) >> 4) | ((b & 0x0F) << 4);
    b = ((b & 0xCC) >> 2) | ((b & 0x33) << 2);
    b = ((b & 0xAA) >> 1) | ((b & 0x55) << 1);
    return b;
  }

  static uint16_t crc16_update(uint16_t crc, uint8_t b) {
    crc ^= (uint16_t)b << 8;
    for (uint8_t i = 0; i < 8; i++) {
      if (crc & 0x8000)
        crc = (crc << 1) ^ 0x1021;
      else
        crc <<= 1;
    }
    return crc;
  }

  static bool is_valid_cmd(uint8_t cmd) {
    return cmd == CMD_POWER || cmd == CMD_RGB_ON || cmd == CMD_BRI_UP ||
           cmd == CMD_BRI_DOWN || cmd == CMD_COLOR_TEMP || cmd == CMD_RGB_OFF;
  }

  void write_reg(uint8_t reg, uint8_t val) {
    digitalWrite(csn_pin_, LOW);
    SPI.transfer(NRF_W_REGISTER | (reg & 0x1F));
    SPI.transfer(val);
    digitalWrite(csn_pin_, HIGH);
  }

  void write_reg_multi(uint8_t reg, const uint8_t *buf, uint8_t len) {
    digitalWrite(csn_pin_, LOW);
    SPI.transfer(NRF_W_REGISTER | (reg & 0x1F));
    for (uint8_t i = 0; i < len; i++) {
      SPI.transfer(buf[i]);
    }
    digitalWrite(csn_pin_, HIGH);
  }

  uint8_t read_reg(uint8_t reg) {
    digitalWrite(csn_pin_, LOW);
    SPI.transfer(NRF_R_REGISTER | (reg & 0x1F));
    uint8_t val = SPI.transfer(NRF_NOP);
    digitalWrite(csn_pin_, HIGH);
    return val;
  }

  void flush_rx() {
    digitalWrite(csn_pin_, LOW);
    SPI.transfer(NRF_FLUSH_RX);
    digitalWrite(csn_pin_, HIGH);
  }

  void flush_tx() {
    digitalWrite(csn_pin_, LOW);
    SPI.transfer(NRF_FLUSH_TX);
    digitalWrite(csn_pin_, HIGH);
  }

  void begin() {
    pinMode(ce_pin_, OUTPUT);
    pinMode(csn_pin_, OUTPUT);
    digitalWrite(ce_pin_, LOW);
    digitalWrite(csn_pin_, HIGH);

    SPI.begin(sck_pin_, miso_pin_, mosi_pin_, csn_pin_);
    SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));

    delay(5);
    write_reg(REG_CONFIG, 0x00);      // Power down, CRC disabled
    write_reg(REG_EN_AA, 0x00);       // Disable ShockBurst Auto-ACK
    write_reg(REG_EN_RXADDR, 0x01);   // Enable Pipe 0
    write_reg(REG_SETUP_RETR, 0x00);  // Disable retransmissions
    write_reg(REG_RF_CH, 19);         // Channel 19 = 2419 MHz
    write_reg(REG_RF_SETUP, 0x06);    // 1 Mbps, 0 dBm max power
    flush_rx();
    flush_tx();
    write_reg(REG_STATUS, 0x70);      // Clear status flags

    enter_rx_mode();
    ESP_LOGI("quntis_rf", "nRF24L01+ initialized on 2419 MHz (CH 19)");
  }

  void enter_rx_mode() {
    digitalWrite(ce_pin_, LOW);

    if (sniff_mode_) {
      // In Sniffer Mode, match the 3-byte XN297 preamble (0x71 0x0F 0x55) directly
      // and capture all 18 bytes (5B scrambled address + 11B payload + 2B CRC).
      static const uint8_t preamble_rx_addr[3] = {0x55, 0x0F, 0x71};
      write_reg(REG_SETUP_AW, 0x01);  // 3-byte address width
      write_reg_multi(REG_RX_ADDR_P0, preamble_rx_addr, 3);
      write_reg(REG_RX_PW_P0, 18);    // 16B body + 2B CRC
    } else {
      // In normal RX mode, the nRF24L01+ uses the 0x55 byte at the end of the XN297
      // preamble (0x71 0x0F 0x55) as its 1-byte preamble, and matches the 5-byte
      // scrambled address in Pipe 0, placing the remaining 13 bytes into the RX FIFO.
      uint8_t rx_addr_lsb_first[5];
      for (int i = 0; i < 5; i++) {
        uint8_t wire_byte = addr_pipe_[4 - i] ^ XN297_SCRAMBLE[i];
        rx_addr_lsb_first[4 - i] = wire_byte;
      }
      write_reg(REG_SETUP_AW, 0x03);  // 5-byte address width
      write_reg_multi(REG_RX_ADDR_P0, rx_addr_lsb_first, 5);
      write_reg(REG_RX_PW_P0, 13);    // 11B payload + 2B CRC
    }

    flush_rx();
    write_reg(REG_STATUS, 0x70);
    write_reg(REG_CONFIG, 0x03);      // PWR_UP=1, PRIM_RX=1, CRC=0
    delayMicroseconds(150);
    digitalWrite(ce_pin_, HIGH);
  }

  // In TX mode, we set the 3-byte TX_ADDR to the XN297 preamble {0x55, 0x0F, 0x71}
  // (transmitted on air as 0x71, 0x0F, 0x55) followed by the 18-byte payload
  // (5B scrambled address + 11B scrambled payload + 2B CRC).
  void send_single_burst() {
    digitalWrite(ce_pin_, LOW);

    static const uint8_t tx_preamble_addr[3] = {0x55, 0x0F, 0x71};
    write_reg(REG_SETUP_AW, 0x01);    // 3-byte address width
    write_reg_multi(REG_TX_ADDR, tx_preamble_addr, 3);
    write_reg(REG_CONFIG, 0x02);      // PWR_UP=1, PRIM_RX=0, CRC=0
    flush_tx();
    write_reg(REG_STATUS, 0x70);

    digitalWrite(csn_pin_, LOW);
    SPI.transfer(NRF_W_TX_PAYLOAD);
    for (uint8_t i = 0; i < 18; i++) {
      SPI.transfer(tx_frame_[i]);
    }
    digitalWrite(csn_pin_, HIGH);

    digitalWrite(ce_pin_, HIGH);
    delayMicroseconds(450);           // Wait for 18+4 bytes @ 1Mbps (~180us + PLL startup)
    digitalWrite(ce_pin_, LOW);
  }

  void prepare_and_start_tx(uint8_t cmd_id, uint8_t p5_bri, uint8_t p6_ct, uint8_t p7_en, uint8_t p8_rgb, uint8_t triplets = 12) {
    seq_counter_++;
    uint8_t p[11];
    p[0] = header_[0];
    p[1] = header_[1];
    p[2] = header_[2];
    p[3] = header_[3];
    p[4] = cmd_id;
    p[5] = p5_bri;
    p[6] = p6_ct;
    p[7] = p7_en;
    p[8] = p8_rgb;
    p[9] = seq_counter_;

    uint16_t sum = 0;
    for (int i = 0; i < 10; i++) sum += p[i];
    p[10] = sum & 0xFF;

    // Build 16-byte scrambled body
    for (int i = 0; i < 5; i++) {
      uint8_t wire_addr = addr_pipe_[4 - i];
      tx_frame_[i] = wire_addr ^ XN297_SCRAMBLE[i];
    }
    for (int i = 0; i < 11; i++) {
      tx_frame_[5 + i] = bit_reverse(p[i]) ^ XN297_SCRAMBLE[5 + i];
    }

    // Compute XN297 scrambled CRC-16
    uint16_t crc = 0xB5D2;
    for (int i = 0; i < 16; i++) {
      crc = crc16_update(crc, tx_frame_[i]);
    }
    crc ^= XN297_CRC_XOROUT_16B;
    tx_frame_[16] = (crc >> 8) & 0xFF;
    tx_frame_[17] = crc & 0xFF;

    tx_triplets_left_ = triplets;
    tx_in_triplet_ = 0;
    tx_active_ = true;
    tx_next_ms_ = millis();

    ESP_LOGI("quntis_rf", "TX Cmd=0x%02X Bri=%u CT=%u En=0x%02X RGB=0x%02X Seq=%u Chk=0x%02X CRC=0x%04X",
             cmd_id, p5_bri, p6_ct, p7_en, p8_rgb, seq_counter_, p[10], crc);
  }

  // High-level control methods called from ESPHome entities
  void set_front_light(bool state, uint8_t bri, uint8_t ct) {
    if (updating_from_rf_) return;

    uint8_t clamped_bri = bri < 0x1A ? 0x1A : bri;
    uint8_t clamped_ct  = ct < 0x1A ? 0x1A : ct;

    if (!state) {
      front_on_ = false;
      prepare_and_start_tx(CMD_POWER, 0x00, color_temp_, 0x00, 0x80 | (rgb_mode_ & 0x0F));
      return;
    }

    bool was_off = !front_on_;
    bool bri_changed = (clamped_bri != brightness_);
    bool ct_changed  = (clamped_ct != color_temp_);
    uint8_t cmd = CMD_POWER;

    if (!was_off) {
      if (ct_changed && !bri_changed) {
        cmd = CMD_COLOR_TEMP;
      } else if (clamped_bri >= brightness_) {
        cmd = CMD_BRI_UP;
      } else {
        cmd = CMD_BRI_DOWN;
      }
    }

    front_on_ = true;
    brightness_ = clamped_bri;
    color_temp_ = clamped_ct;
    prepare_and_start_tx(cmd, brightness_, color_temp_, 0xFF, 0x80 | (rgb_mode_ & 0x0F));
  }

  void set_rgb_backlight(bool state, uint8_t mode) {
    if (updating_from_rf_) return;
    if (mode >= 1 && mode <= 15) rgb_mode_ = mode;
    rgb_on_ = state;
    uint8_t p5 = front_on_ ? brightness_ : 0x00;
    if (state) {
      prepare_and_start_tx(CMD_RGB_ON, p5, color_temp_, 0xFF, 0x80 | (rgb_mode_ & 0x0F));
    } else {
      prepare_and_start_tx(CMD_RGB_OFF, p5, color_temp_, 0x00, 0x80 | (rgb_mode_ & 0x0F));
    }
  }

  // Poll loop (call every 1-2 ms)
  // Returns true when a new valid packet from the physical remote was decoded
  bool loop() {
    uint32_t now = millis();

    // Handle active TX burst state machine (3 bursts ~4ms apart, then 28ms gap)
    if (tx_active_) {
      if ((int32_t)(now - tx_next_ms_) >= 0) {
        send_single_burst();
        tx_in_triplet_++;
        if (tx_in_triplet_ < 3) {
          tx_next_ms_ = now + 4;
        } else {
          tx_in_triplet_ = 0;
          tx_triplets_left_--;
          if (tx_triplets_left_ == 0) {
            tx_active_ = false;
            enter_rx_mode();
          } else {
            tx_next_ms_ = now + 28;
            enter_rx_mode();
          }
        }
      }
      return false;
    }

    // Check RX FIFO for packets from the physical remote
    uint8_t status = read_reg(REG_STATUS);
    if (status & 0x40) {  // RX_DR bit set
      uint8_t body[16];
      uint16_t rx_crc = 0;

      if (sniff_mode_) {
        uint8_t rx_buf[18];
        digitalWrite(csn_pin_, LOW);
        SPI.transfer(NRF_R_RX_PAYLOAD);
        for (int i = 0; i < 18; i++) rx_buf[i] = SPI.transfer(NRF_NOP);
        digitalWrite(csn_pin_, HIGH);
        write_reg(REG_STATUS, 0x40);

        for (int i = 0; i < 16; i++) body[i] = rx_buf[i];
        rx_crc = ((uint16_t)rx_buf[16] << 8) | rx_buf[17];
      } else {
        uint8_t rx_buf[13];
        digitalWrite(csn_pin_, LOW);
        SPI.transfer(NRF_R_RX_PAYLOAD);
        for (int i = 0; i < 13; i++) rx_buf[i] = SPI.transfer(NRF_NOP);
        digitalWrite(csn_pin_, HIGH);
        write_reg(REG_STATUS, 0x40);

        for (int i = 0; i < 5; i++) body[i] = addr_pipe_[4 - i] ^ XN297_SCRAMBLE[i];
        for (int i = 0; i < 11; i++) body[5 + i] = rx_buf[i];
        rx_crc = ((uint16_t)rx_buf[11] << 8) | rx_buf[12];
      }

      // 1. Verify XN297 16-bit scrambled CRC
      uint16_t crc = 0xB5D2;
      for (int i = 0; i < 16; i++) {
        crc = crc16_update(crc, body[i]);
      }
      crc ^= XN297_CRC_XOROUT_16B;
      if (crc != rx_crc) {
        return false;  // Reject RF noise
      }

      // 2. Unscramble 11-byte LSB-first payload and verify 8-bit sum checksum p[10]
      uint8_t p[11];
      uint16_t sum = 0;
      for (int i = 0; i < 11; i++) {
        p[i] = bit_reverse(body[5 + i] ^ XN297_SCRAMBLE[5 + i]);
        if (i < 10) sum += p[i];
      }
      if ((sum & 0xFF) != p[10] || !is_valid_cmd(p[4])) {
        return false;
      }

      // 3. If in Sniffer Mode (or if header differs on same pipe), report & adopt ID!
      if (sniff_mode_) {
        for (int i = 0; i < 5; i++) {
          addr_pipe_[4 - i] = body[i] ^ XN297_SCRAMBLE[i];
        }
        for (int i = 0; i < 4; i++) {
          header_[i] = p[i];
        }
        ESP_LOGW("quntis_rf", "=== DISCOVERED QUNTIS REMOTE ===");
        ESP_LOGW("quntis_rf", "  Address (pipe): 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X",
                 addr_pipe_[0], addr_pipe_[1], addr_pipe_[2], addr_pipe_[3], addr_pipe_[4]);
        ESP_LOGW("quntis_rf", "  Header p[0..3]: 0x%02X, 0x%02X, 0x%02X, 0x%02X",
                 header_[0], header_[1], header_[2], header_[3]);
        set_sniff_mode(false);
      } else if (p[0] != header_[0] || p[1] != header_[1] || p[2] != header_[2] || p[3] != header_[3]) {
        ESP_LOGW("quntis_rf", "Received valid Quntis packet with different header: 0x%02X, 0x%02X, 0x%02X, 0x%02X (expected 0x%02X, 0x%02X, 0x%02X, 0x%02X)",
                 p[0], p[1], p[2], p[3], header_[0], header_[1], header_[2], header_[3]);
        return false;
      }

      // 4. Deduplicate repeated bursts from the same button press
      if (p[9] == last_rx_seq_ && (now - last_rx_ms_) < 1200) {
        last_rx_ms_ = now;
        return false;
      }
      last_rx_seq_ = p[9];
      last_rx_ms_ = now;
      seq_counter_ = p[9];

      // 5. Update tracked state from physical remote packet
      uint8_t cmd = p[4];
      color_temp_ = p[6];
      rgb_mode_ = (p[8] & 0x0F);
      if (rgb_mode_ == 0) rgb_mode_ = 1;

      if (cmd == CMD_POWER) {
        front_on_ = (p[7] != 0x00) && (p[5] != 0x00);
        if (front_on_) brightness_ = p[5];
      } else if (cmd == CMD_BRI_UP || cmd == CMD_BRI_DOWN || cmd == CMD_COLOR_TEMP) {
        front_on_ = true;
        if (p[5] > 0) brightness_ = p[5];
      } else if (cmd == CMD_RGB_ON) {
        rgb_on_ = true;
        if (p[5] > 0) brightness_ = p[5];
      } else if (cmd == CMD_RGB_OFF) {
        rgb_on_ = false;
      }

      ESP_LOGI("quntis_rf", "RX Remote Cmd=0x%02X Front=%s Bri=%u CT=%u RGB=%s Mode=%u Seq=%u",
               cmd, front_on_ ? "ON" : "OFF", brightness_, color_temp_,
               rgb_on_ ? "ON" : "OFF", rgb_mode_, seq_counter_);
      return true;
    }
    return false;
  }
};

// Global pointer initialized in ESPHome on_boot
static QuntisRF *rf = nullptr;

}  // namespace quntis
