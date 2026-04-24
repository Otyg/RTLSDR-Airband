/*
 * output.cpp
 * Output related routines
 *
 * Copyright (c) 2015-2021 Tomasz Lemiech <szpajder@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <https://www.gnu.org/licenses/>.
 */
#include <math.h>
#include <ogg/ogg.h>
#include <shout/shout.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <vorbis/vorbisenc.h>

// SHOUTERR_RETRY is available since libshout 2.4.0.
// Set it to an impossible value if it's not there.
#ifndef SHOUTERR_RETRY
#define SHOUTERR_RETRY (-255)
#endif /* SHOUTERR_RETRY */

#include <lame/lame.h>

#ifdef WITH_PULSEAUDIO
#include <pulse/pulseaudio.h>
#endif /* WITH_PULSEAUDIO */

#include <syslog.h>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>
#include "config.h"
#ifdef WITH_FLAC_FILE_OUTPUT
#include <FLAC/stream_encoder.h>
#endif /* WITH_FLAC_FILE_OUTPUT */
#include "helper_functions.h"
#include "input-common.h"
#include "rtl_airband.h"

namespace {
struct DecodedMessage {
    std::string modulation;
    std::string msg_type;
    std::string payload;
    bool crc_ok;
    int mmsi;
};

static uint16_t crc16_x25(uint8_t const* data, size_t len) {
    uint16_t crc = 0xffff;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0x8408;
            } else {
                crc >>= 1;
            }
        }
    }
    return (uint16_t)(~crc);
}

static uint32_t get_bits_be(uint8_t const* data, int bit_offset, int bit_count) {
    uint32_t value = 0;
    for (int i = 0; i < bit_count; ++i) {
        int const abs_bit = bit_offset + i;
        uint8_t const byte = data[abs_bit / 8];
        int const shift = 7 - (abs_bit % 8);
        value = (value << 1) | ((byte >> shift) & 0x1);
    }
    return value;
}

static std::string bytes_to_hex(uint8_t const* data, size_t len) {
    static char const* hexdigits = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        unsigned char b = data[i];
        out.push_back(hexdigits[(b >> 4) & 0x0f]);
        out.push_back(hexdigits[b & 0x0f]);
    }
    return out;
}

static void append_json_escaped(std::string* dst, std::string const& src) {
    for (size_t i = 0; i < src.size(); ++i) {
        char const c = src[i];
        switch (c) {
            case '\"':
                *dst += "\\\"";
                break;
            case '\\':
                *dst += "\\\\";
                break;
            case '\b':
                *dst += "\\b";
                break;
            case '\f':
                *dst += "\\f";
                break;
            case '\n':
                *dst += "\\n";
                break;
            case '\r':
                *dst += "\\r";
                break;
            case '\t':
                *dst += "\\t";
                break;
            default:
                *dst += c;
                break;
        }
    }
}

static int32_t get_bits_be_signed(uint8_t const* data, int bit_offset, int bit_count) {
    uint32_t const raw = get_bits_be(data, bit_offset, bit_count);
    uint32_t const sign_mask = 1u << (bit_count - 1);
    if ((raw & sign_mask) == 0) {
        return (int32_t)raw;
    }
    uint32_t const full_mask = (bit_count == 32) ? 0xffffffffu : ((1u << bit_count) - 1u);
    return (int32_t)(raw | (~full_mask));
}

static std::string ais_sixbit_to_text(uint8_t const* data, int bit_offset, int char_count) {
    std::string out;
    out.reserve((size_t)char_count);
    for (int i = 0; i < char_count; ++i) {
        uint8_t const v = (uint8_t)get_bits_be(data, bit_offset + i * 6, 6);
        char c;
        if (v == 0) {
            c = '@';
        } else if (v >= 1 && v <= 26) {
            c = (char)('A' + (v - 1));
        } else if (v == 32) {
            c = ' ';
        } else if (v >= 48 && v <= 57) {
            c = (char)('0' + (v - 48));
        } else {
            c = ' ';
        }
        out.push_back(c);
    }

    while (!out.empty() && (out.back() == '@' || out.back() == ' ')) {
        out.pop_back();
    }
    return out;
}

static bool ais_valid_lon(double lon) {
    return lon >= -180.0 && lon <= 180.0;
}

static bool ais_valid_lat(double lat) {
    return lat >= -90.0 && lat <= 90.0;
}

static std::string ais_decoded_payload_json(uint8_t const* bytes, int payload_len, int ais_type, int mmsi) {
    std::string raw_hex = bytes_to_hex(bytes, (size_t)payload_len);
    std::ostringstream os;
    os << "{";
    os << "\"type\":" << ais_type << ",";
    os << "\"mmsi\":" << mmsi << ",";
    os << "\"raw_hex\":\"" << raw_hex << "\"";

    if (ais_type >= 1 && ais_type <= 3) {
        int const nav_status = (int)get_bits_be(bytes, 38, 4);
        int const rot_raw = (int)get_bits_be_signed(bytes, 42, 8);
        int const sog_raw = (int)get_bits_be(bytes, 50, 10);
        int32_t const lon_raw = get_bits_be_signed(bytes, 61, 28);
        int32_t const lat_raw = get_bits_be_signed(bytes, 89, 27);
        int const cog_raw = (int)get_bits_be(bytes, 116, 12);
        int const heading = (int)get_bits_be(bytes, 128, 9);
        int const timestamp = (int)get_bits_be(bytes, 137, 6);

        double const lon = lon_raw / 600000.0;
        double const lat = lat_raw / 600000.0;

        os << ",\"nav_status\":" << nav_status;
        if (rot_raw == -128) {
            os << ",\"rot\":null";
        } else {
            os << ",\"rot\":" << rot_raw;
        }
        if (sog_raw >= 1023) {
            os << ",\"sog_kn\":null";
        } else {
            os << std::fixed << std::setprecision(1);
            os << ",\"sog_kn\":" << (sog_raw / 10.0);
        }
        if (ais_valid_lon(lon) && ais_valid_lat(lat)) {
            os << std::fixed << std::setprecision(6);
            os << ",\"lon\":" << lon << ",\"lat\":" << lat;
        } else {
            os << ",\"lon\":null,\"lat\":null";
        }
        if (cog_raw >= 3600) {
            os << ",\"cog_deg\":null";
        } else {
            os << std::fixed << std::setprecision(1);
            os << ",\"cog_deg\":" << (cog_raw / 10.0);
        }
        os << ",\"heading\":" << (heading >= 511 ? -1 : heading);
        os << ",\"timestamp\":" << timestamp;
    } else if (ais_type == 18 || ais_type == 19) {
        int const sog_raw = (int)get_bits_be(bytes, 46, 10);
        int32_t const lon_raw = get_bits_be_signed(bytes, 57, 28);
        int32_t const lat_raw = get_bits_be_signed(bytes, 85, 27);
        int const cog_raw = (int)get_bits_be(bytes, 112, 12);
        int const heading = (int)get_bits_be(bytes, 124, 9);
        int const timestamp = (int)get_bits_be(bytes, 133, 6);
        double const lon = lon_raw / 600000.0;
        double const lat = lat_raw / 600000.0;

        if (sog_raw >= 1023) {
            os << ",\"sog_kn\":null";
        } else {
            os << std::fixed << std::setprecision(1);
            os << ",\"sog_kn\":" << (sog_raw / 10.0);
        }
        if (ais_valid_lon(lon) && ais_valid_lat(lat)) {
            os << std::fixed << std::setprecision(6);
            os << ",\"lon\":" << lon << ",\"lat\":" << lat;
        } else {
            os << ",\"lon\":null,\"lat\":null";
        }
        if (cog_raw >= 3600) {
            os << ",\"cog_deg\":null";
        } else {
            os << std::fixed << std::setprecision(1);
            os << ",\"cog_deg\":" << (cog_raw / 10.0);
        }
        os << ",\"heading\":" << (heading >= 511 ? -1 : heading);
        os << ",\"timestamp\":" << timestamp;
    } else if (ais_type == 5) {
        int const imo = (int)get_bits_be(bytes, 40, 30);
        std::string const callsign = ais_sixbit_to_text(bytes, 70, 7);
        std::string const shipname = ais_sixbit_to_text(bytes, 112, 20);
        int const ship_type = (int)get_bits_be(bytes, 232, 8);
        int const to_bow = (int)get_bits_be(bytes, 240, 9);
        int const to_stern = (int)get_bits_be(bytes, 249, 9);
        int const to_port = (int)get_bits_be(bytes, 258, 6);
        int const to_starboard = (int)get_bits_be(bytes, 264, 6);

        std::string esc_callsign;
        std::string esc_shipname;
        append_json_escaped(&esc_callsign, callsign);
        append_json_escaped(&esc_shipname, shipname);

        os << ",\"imo\":" << imo;
        os << ",\"callsign\":\"" << esc_callsign << "\"";
        os << ",\"shipname\":\"" << esc_shipname << "\"";
        os << ",\"ship_type\":" << ship_type;
        os << ",\"dim_to_bow\":" << to_bow;
        os << ",\"dim_to_stern\":" << to_stern;
        os << ",\"dim_to_port\":" << to_port;
        os << ",\"dim_to_starboard\":" << to_starboard;
    } else if (ais_type == 24) {
        int const part_no = (int)get_bits_be(bytes, 38, 2);
        os << ",\"part_no\":" << part_no;
        if (part_no == 0) {
            std::string const shipname = ais_sixbit_to_text(bytes, 40, 20);
            std::string esc_shipname;
            append_json_escaped(&esc_shipname, shipname);
            os << ",\"shipname\":\"" << esc_shipname << "\"";
        } else if (part_no == 1) {
            int const ship_type = (int)get_bits_be(bytes, 40, 8);
            std::string const vendor = ais_sixbit_to_text(bytes, 48, 3);
            std::string const callsign = ais_sixbit_to_text(bytes, 66, 7);
            int const to_bow = (int)get_bits_be(bytes, 108, 9);
            int const to_stern = (int)get_bits_be(bytes, 117, 9);
            int const to_port = (int)get_bits_be(bytes, 126, 6);
            int const to_starboard = (int)get_bits_be(bytes, 132, 6);

            std::string esc_vendor;
            std::string esc_callsign;
            append_json_escaped(&esc_vendor, vendor);
            append_json_escaped(&esc_callsign, callsign);
            os << ",\"ship_type\":" << ship_type;
            os << ",\"vendor\":\"" << esc_vendor << "\"";
            os << ",\"callsign\":\"" << esc_callsign << "\"";
            os << ",\"dim_to_bow\":" << to_bow;
            os << ",\"dim_to_stern\":" << to_stern;
            os << ",\"dim_to_port\":" << to_port;
            os << ",\"dim_to_starboard\":" << to_starboard;
        }
    }

    os << "}";
    return os.str();
}

static int ais_pack_bytes(uint8_t const* bits, int bit_len, int bit_offset, bool invert_bits, uint8_t* out_bytes, int out_capacity) {
    if (bit_len <= bit_offset) {
        return 0;
    }
    int const usable_bits = bit_len - bit_offset;
    int const byte_count = usable_bits / 8;
    if (byte_count < 5 || byte_count > out_capacity) {
        return 0;
    }

    memset(out_bytes, 0, (size_t)out_capacity);
    for (int i = 0; i < byte_count; ++i) {
        uint8_t v = 0;
        for (int bit = 0; bit < 8; ++bit) {
            int const idx = bit_offset + i * 8 + bit;
            uint8_t b = bits[idx] & 0x1;
            if (invert_bits) {
                b ^= 0x1;
            }
            v |= (uint8_t)(b << bit);
        }
        out_bytes[i] = v;
    }
    return byte_count;
}

static bool ais_build_message_from_bytes(uint8_t const* bytes, int byte_count, bool require_crc_ok, DecodedMessage* out_msg) {
    if (byte_count < 5) {
        return false;
    }
    int const payload_len = byte_count - 2;
    if (payload_len < 5) {
        return false;
    }

    uint16_t const rx_fcs = (uint16_t)bytes[payload_len] | ((uint16_t)bytes[payload_len + 1] << 8);
    uint16_t const calc_fcs = crc16_x25(bytes, (size_t)payload_len);
    bool const crc_ok = (rx_fcs == calc_fcs);
    if (require_crc_ok && !crc_ok) {
        return false;
    }

    int const ais_type = (int)get_bits_be(bytes, 0, 6);
    if (ais_type < 1 || ais_type > 27) {
        return false;
    }
    int const mmsi = (int)get_bits_be(bytes, 8, 30);

    out_msg->modulation = "ais";
    out_msg->msg_type = "ais_frame";
    out_msg->payload = ais_decoded_payload_json(bytes, payload_len, ais_type, mmsi);
    out_msg->crc_ok = crc_ok;
    out_msg->mmsi = mmsi;
    return true;
}

static void try_decode_ais_frame(freq_t* fparms, std::vector<DecodedMessage>* out) {
    if (fparms->ais_debit_len < 40) {
        return;
    }

    int const nominal_byte_count = fparms->ais_debit_len / 8;
    if (nominal_byte_count < 5 || nominal_byte_count > 256) {
        return;
    }

    uint8_t bytes[256];
    DecodedMessage msg;

    // First try nominal framing.
    int const byte_count = ais_pack_bytes(fparms->ais_debit_buf, fparms->ais_debit_len, 0, false, bytes, (int)sizeof(bytes));
    if (byte_count > 0 && ais_build_message_from_bytes(bytes, byte_count, true, &msg)) {
        out->push_back(msg);
        return;
    }

    // Recovery pass: try alternate bit alignments and inverted bit polarity.
    for (int bit_offset = 0; bit_offset < 8; ++bit_offset) {
        for (int invert = 0; invert < 2; ++invert) {
            if (bit_offset == 0 && invert == 0) {
                continue;
            }
            int const recovered_bytes = ais_pack_bytes(fparms->ais_debit_buf, fparms->ais_debit_len, bit_offset, invert != 0, bytes, (int)sizeof(bytes));
            if (recovered_bytes <= 0) {
                continue;
            }
            if (ais_build_message_from_bytes(bytes, recovered_bytes, true, &msg)) {
                out->push_back(msg);
                return;
            }
        }
    }

    // Debug fallback: emit best-effort decode even when CRC fails.
    if (byte_count > 0 && ais_build_message_from_bytes(bytes, byte_count, false, &msg)) {
        out->push_back(msg);
    }
}

static void ais_decode_batch(freq_t* fparms, float const* samples, size_t sample_count, std::vector<DecodedMessage>* out) {
    // Symbol clock at 9600 symbols/sec from 16k discriminator samples.
    constexpr uint32_t kSymbolRate = 9600;

    for (size_t i = 0; i < sample_count; ++i) {
        fparms->ais_resample_phase += kSymbolRate;
        if (fparms->ais_resample_phase < WAVE_RATE) {
            continue;
        }
        fparms->ais_resample_phase -= WAVE_RATE;

        uint8_t const nrzi_level = samples[i] >= 0.0f ? 1 : 0;
        if (!fparms->ais_nrzi_prev_valid) {
            fparms->ais_nrzi_prev = nrzi_level;
            fparms->ais_nrzi_prev_valid = 1;
            continue;
        }

        // NRZI decode: transition=0, no transition=1
        uint8_t const bit = (nrzi_level == fparms->ais_nrzi_prev) ? 1 : 0;
        fparms->ais_nrzi_prev = nrzi_level;

        if (bit) {
            if (fparms->ais_pending_ones < 31) {
                fparms->ais_pending_ones++;
            }
            continue;
        }

        // bit == 0
        if (fparms->ais_pending_ones == 6) {
            if (fparms->ais_in_frame) {
                try_decode_ais_frame(fparms, out);
            }
            fparms->ais_in_frame = 1;
            fparms->ais_debit_len = 0;
        } else if (fparms->ais_pending_ones == 5) {
            if (fparms->ais_in_frame) {
                for (int k = 0; k < 5; ++k) {
                    if (fparms->ais_debit_len >= (int)sizeof(fparms->ais_debit_buf)) {
                        fparms->ais_in_frame = 0;
                        fparms->ais_debit_len = 0;
                        break;
                    }
                    fparms->ais_debit_buf[fparms->ais_debit_len++] = 1;
                }
            }
        } else if (fparms->ais_pending_ones > 6) {
            fparms->ais_in_frame = 0;
            fparms->ais_debit_len = 0;
        } else if (fparms->ais_in_frame) {
            for (int k = 0; k < fparms->ais_pending_ones; ++k) {
                if (fparms->ais_debit_len >= (int)sizeof(fparms->ais_debit_buf)) {
                    fparms->ais_in_frame = 0;
                    fparms->ais_debit_len = 0;
                    break;
                }
                fparms->ais_debit_buf[fparms->ais_debit_len++] = 1;
            }
            if (fparms->ais_in_frame) {
                if (fparms->ais_debit_len < (int)sizeof(fparms->ais_debit_buf)) {
                    fparms->ais_debit_buf[fparms->ais_debit_len++] = 0;
                } else {
                    fparms->ais_in_frame = 0;
                    fparms->ais_debit_len = 0;
                }
            }
        }
        fparms->ais_pending_ones = 0;
    }
}

static bool dsc_is_format(uint8_t symbol) {
    return symbol == 112 || symbol == 114 || symbol == 116 || symbol == 120 || symbol == 123 || symbol == 102;
}

static bool dsc_is_eos(uint8_t symbol) {
    return symbol == 117 || symbol == 122 || symbol == 127;
}

static const char* dsc_format_name(uint8_t symbol) {
    switch (symbol) {
        case 112:
            return "distress";
        case 114:
            return "group";
        case 116:
            return "all_ships";
        case 120:
            return "individual";
        case 123:
            return "semi_auto";
        case 102:
            return "geo_area";
        default:
            return "unknown";
    }
}

static const char* dsc_category_name(uint8_t symbol) {
    switch (symbol) {
        case 112:
            return "distress";
        case 110:
            return "urgency";
        case 108:
            return "safety";
        case 100:
            return "routine";
        default:
            return "unknown";
    }
}

static const char* dsc_telecommand_name(uint8_t symbol) {
    switch (symbol) {
        case 100:
            return "f3e_g3e_simplex";
        case 101:
            return "f3e_g3e_duplex";
        case 109:
            return "j3e_rt";
        case 126:
            return "no_information";
        default:
            return "unknown";
    }
}

static const char* dsc_eos_name(uint8_t symbol) {
    switch (symbol) {
        case 117:
            return "ack_rq";
        case 122:
            return "ack_bq";
        case 127:
            return "eos";
        default:
            return "unknown";
    }
}

static const char* dsc_distress_nature_name(uint8_t symbol) {
    switch (symbol) {
        case 100:
            return "fire_explosion";
        case 101:
            return "flooding";
        case 102:
            return "collision";
        case 103:
            return "grounding";
        case 104:
            return "listing_capsizing";
        case 105:
            return "sinking";
        case 106:
            return "disabled_adrift";
        case 107:
            return "undesignated_distress";
        case 108:
            return "abandoning_ship";
        case 109:
            return "piracy_attack";
        case 110:
            return "man_overboard";
        case 112:
            return "epirb_emission";
        default:
            return "unknown";
    }
}

static bool dsc_symbol_to_digits(uint8_t symbol, char* out_a, char* out_b) {
    if (symbol > 99) {
        return false;
    }
    *out_a = (char)('0' + (symbol / 10));
    *out_b = (char)('0' + (symbol % 10));
    return true;
}

static std::string dsc_symbols_to_digit_string(uint8_t const* symbols, size_t count) {
    std::string out;
    out.reserve(count * 2);
    for (size_t i = 0; i < count; ++i) {
        char a = 0;
        char b = 0;
        if (!dsc_symbol_to_digits(symbols[i], &a, &b)) {
            return std::string();
        }
        out.push_back(a);
        out.push_back(b);
    }
    return out;
}

static int dsc_mmsi_to_int(std::string const& digits10) {
    if (digits10.size() < 9) {
        return -1;
    }
    int mmsi = 0;
    for (size_t i = 0; i < 9; ++i) {
        if (digits10[i] < '0' || digits10[i] > '9') {
            return -1;
        }
        mmsi = mmsi * 10 + (digits10[i] - '0');
    }
    return mmsi;
}

static bool dsc_word_to_symbol(uint16_t word10, uint8_t* symbol_out) {
    uint8_t const info = (uint8_t)(word10 & 0x7f);
    uint8_t const ones = (uint8_t)__builtin_popcount((unsigned int)info);
    uint8_t const zeros = (uint8_t)(7 - ones);
    uint8_t const check = (uint8_t)((((word10 >> 7) & 0x1) << 2) | (((word10 >> 8) & 0x1) << 1) | ((word10 >> 9) & 0x1));
    if (check != zeros) {
        return false;
    }
    *symbol_out = info;
    return true;
}

static uint8_t dsc_calc_ecc(uint8_t format, uint8_t const* address, size_t address_len, uint8_t category, uint8_t const* self_id, size_t self_len, uint8_t const* message,
                            size_t message_len, uint8_t eos) {
    uint8_t ecc = format;
    for (size_t i = 0; i < address_len; ++i) {
        ecc ^= address[i];
    }
    ecc ^= category;
    for (size_t i = 0; i < self_len; ++i) {
        ecc ^= self_id[i];
    }
    for (size_t i = 0; i < message_len; ++i) {
        ecc ^= message[i];
    }
    ecc ^= eos;
    return ecc;
}

static bool dsc_parse_logical(uint8_t const* symbols, size_t n, DecodedMessage* out_msg, size_t* consumed) {
    if (n < 14) {
        return false;
    }
    size_t i = 0;
    uint8_t const format = symbols[i];
    if (!dsc_is_format(format)) {
        return false;
    }
    i++;
    if (i < n && symbols[i] == format) {
        i++;  // tolerate duplicate format character
    }

    size_t const address_len = (format == 112 || format == 116) ? 0u : 5u;
    if (i + address_len + 1 + 5 + 2 > n) {
        return false;
    }

    uint8_t address[5] = {0};
    for (size_t a = 0; a < address_len; ++a) {
        address[a] = symbols[i++];
    }

    uint8_t const category = symbols[i++];
    uint8_t self_id[5] = {0};
    for (size_t s = 0; s < 5; ++s) {
        self_id[s] = symbols[i++];
    }

    uint8_t message[16] = {0};
    size_t message_len = 0;
    while (i < n && !dsc_is_eos(symbols[i]) && message_len < 16) {
        message[message_len++] = symbols[i++];
    }
    if (i >= n || !dsc_is_eos(symbols[i])) {
        return false;
    }
    uint8_t const eos = symbols[i++];
    if (i >= n) {
        return false;
    }
    uint8_t const ecc = symbols[i++];
    if (i < n && dsc_is_eos(symbols[i])) {
        i++;
    }

    uint8_t const calc_ecc = dsc_calc_ecc(format, address, address_len, category, self_id, 5, message, message_len, eos);
    if (ecc != calc_ecc) {
        return false;
    }

    std::string const address_digits = dsc_symbols_to_digit_string(address, address_len);
    std::string const self_digits = dsc_symbols_to_digit_string(self_id, 5);
    if ((address_len > 0 && address_digits.empty()) || self_digits.empty()) {
        return false;
    }

    std::ostringstream payload;
    payload << "{";
    payload << "\"format_code\":" << (unsigned int)format << ",";
    payload << "\"format\":\"" << dsc_format_name(format) << "\",";
    if (address_len > 0) {
        payload << "\"address\":\"" << address_digits << "\",";
    } else {
        payload << "\"address\":\"\",";
    }
    payload << "\"category_code\":" << (unsigned int)category << ",";
    payload << "\"category\":\"" << dsc_category_name(category) << "\",";
    payload << "\"self_id\":\"" << self_digits << "\",";
    payload << "\"eos_code\":" << (unsigned int)eos << ",";
    payload << "\"eos\":\"" << dsc_eos_name(eos) << "\",";
    payload << "\"ecc\":" << (unsigned int)ecc << ",";
    payload << "\"telecommand1_code\":" << (message_len > 0 ? (unsigned int)message[0] : 0) << ",";
    payload << "\"telecommand1\":\"" << (message_len > 0 ? dsc_telecommand_name(message[0]) : "none") << "\",";
    payload << "\"telecommand2_code\":" << (message_len > 1 ? (unsigned int)message[1] : 0) << ",";
    payload << "\"telecommand2\":\"" << (message_len > 1 ? dsc_telecommand_name(message[1]) : "none") << "\"";

    if (format == 112) {
        payload << ",\"distress_nature_code\":" << (message_len > 0 ? (unsigned int)message[0] : 0);
        payload << ",\"distress_nature\":\"" << (message_len > 0 ? dsc_distress_nature_name(message[0]) : "none") << "\"";
        if (message_len >= 6) {
            std::string const pos_digits = dsc_symbols_to_digit_string(message + 1, 5);
            if (!pos_digits.empty()) {
                payload << ",\"distress_position\":\"" << pos_digits << "\"";
            }
        }
        if (message_len >= 8) {
            std::string const utc_digits = dsc_symbols_to_digit_string(message + 6, 2);
            if (!utc_digits.empty()) {
                payload << ",\"distress_utc\":\"" << utc_digits << "\"";
            }
        }
    } else if (message_len > 2) {
        std::string const msg_digits = dsc_symbols_to_digit_string(message + 2, message_len - 2);
        if (!msg_digits.empty()) {
            payload << ",\"message_digits\":\"" << msg_digits << "\"";
        }
    }

    payload << "}";

    out_msg->modulation = "dsc";
    out_msg->msg_type = "dsc_message";
    out_msg->payload = payload.str();
    out_msg->crc_ok = true;
    out_msg->mmsi = dsc_mmsi_to_int(self_digits);
    *consumed = i;
    return true;
}

static bool dsc_try_parse_from_symbol_stream(std::vector<uint8_t> const& symbols, DecodedMessage* out_msg) {
    if (symbols.size() < 14) {
        return false;
    }

    for (size_t start = 0; start + 14 <= symbols.size(); ++start) {
        if (!dsc_is_format(symbols[start])) {
            continue;
        }
        size_t consumed = 0;
        if (dsc_parse_logical(symbols.data() + start, symbols.size() - start, out_msg, &consumed)) {
            return true;
        }

        if (start + 22 > symbols.size()) {
            continue;
        }
        size_t const max_logical = std::min((size_t)40, symbols.size() - start - 5);
        std::vector<uint8_t> logical;
        logical.reserve(max_logical);
        for (size_t i = 0; i < max_logical; ++i) {
            uint8_t const dx = symbols[start + i];
            uint8_t const rx = symbols[start + i + 5];
            logical.push_back(dx == rx ? dx : dx);
        }
        consumed = 0;
        if (dsc_parse_logical(logical.data(), logical.size(), out_msg, &consumed)) {
            return true;
        }
    }
    return false;
}

static void dsc_try_emit_from_buffer(freq_t* fparms, std::vector<DecodedMessage>* out) {
    if (fparms->dsc_word_count < 24) {
        return;
    }

    std::vector<uint8_t> symbols;
    symbols.reserve(fparms->dsc_word_count);
    for (uint8_t i = 0; i < fparms->dsc_word_count; ++i) {
        uint8_t symbol = 0;
        if (dsc_word_to_symbol(fparms->dsc_words[i], &symbol)) {
            symbols.push_back(symbol);
        }
    }

    DecodedMessage msg;
    if (dsc_try_parse_from_symbol_stream(symbols, &msg)) {
        uint32_t const hash = (uint32_t)std::hash<std::string>{}(msg.payload);
        if (hash != 0 && hash != fparms->dsc_last_hash) {
            out->push_back(msg);
            fparms->dsc_last_hash = hash;
        }
        fparms->dsc_word_count = 0;
        return;
    }

    if (fparms->dsc_word_count >= 90) {
        memmove(fparms->dsc_words, fparms->dsc_words + 45, 45 * sizeof(fparms->dsc_words[0]));
        fparms->dsc_word_count = 45;
    }
}

static void dsc_decode_batch(freq_t* fparms, float const* samples, size_t sample_count, std::vector<DecodedMessage>* out) {
    // DSC over FM discriminator: 1200 baud symbol clock.
    constexpr uint32_t kSymbolRate = 1200;

    for (size_t i = 0; i < sample_count; ++i) {
        fparms->dsc_phase += kSymbolRate;
        fparms->dsc_accum += samples[i];
        if (fparms->dsc_phase < WAVE_RATE) {
            continue;
        }
        fparms->dsc_phase -= WAVE_RATE;

        uint16_t const bit = fparms->dsc_accum >= 0.0f ? 1u : 0u;
        fparms->dsc_accum = 0.0f;
        fparms->dsc_word |= (uint16_t)(bit << fparms->dsc_word_bits);
        fparms->dsc_word_bits++;

        if (fparms->dsc_word_bits == 10) {
            if (fparms->dsc_word_count < (uint8_t)(sizeof(fparms->dsc_words) / sizeof(fparms->dsc_words[0]))) {
                fparms->dsc_words[fparms->dsc_word_count++] = fparms->dsc_word;
            }
            fparms->dsc_word = 0;
            fparms->dsc_word_bits = 0;
            dsc_try_emit_from_buffer(fparms, out);
        }
    }
}

static void decode_digital_messages(freq_t* fparms, float const* samples, size_t sample_count, std::vector<DecodedMessage>* out) {
#ifdef NFM
    if (fparms->modulation == MOD_AIS) {
        ais_decode_batch(fparms, samples, sample_count, out);
    } else if (fparms->modulation == MOD_DSC) {
        dsc_decode_batch(fparms, samples, sample_count, out);
    }
#else
    (void)fparms;
    (void)samples;
    (void)sample_count;
    (void)out;
#endif /* NFM */
}
}  // namespace

void shout_setup(icecast_data* icecast, mix_modes mixmode) {
    int ret;
    shout_t* shouttemp = shout_new();
    if (shouttemp == NULL) {
        printf("cannot allocate\n");
    }
    if (shout_set_host(shouttemp, icecast->hostname) != SHOUTERR_SUCCESS) {
        shout_free(shouttemp);
        return;
    }
    if (shout_set_protocol(shouttemp, SHOUT_PROTOCOL_HTTP) != SHOUTERR_SUCCESS) {
        shout_free(shouttemp);
        return;
    }
    if (shout_set_port(shouttemp, icecast->port) != SHOUTERR_SUCCESS) {
        shout_free(shouttemp);
        return;
    }
#ifdef LIBSHOUT_HAS_TLS
    if (shout_set_tls(shouttemp, icecast->tls_mode) != SHOUTERR_SUCCESS) {
        shout_free(shouttemp);
        return;
    }
#endif /* LIBSHOUT_HAS_TLS */
    char mp[100];
    sprintf(mp, "/%s", icecast->mountpoint);
    if (shout_set_mount(shouttemp, mp) != SHOUTERR_SUCCESS) {
        shout_free(shouttemp);
        return;
    }
    if (shout_set_user(shouttemp, icecast->username) != SHOUTERR_SUCCESS) {
        shout_free(shouttemp);
        return;
    }
    if (shout_set_password(shouttemp, icecast->password) != SHOUTERR_SUCCESS) {
        shout_free(shouttemp);
        return;
    }
#ifdef LIBSHOUT_HAS_CONTENT_FORMAT
    if (shout_set_content_format(shouttemp, SHOUT_FORMAT_MP3, SHOUT_USAGE_AUDIO, NULL) != SHOUTERR_SUCCESS) {
#else
    if (shout_set_format(shouttemp, SHOUT_FORMAT_MP3) != SHOUTERR_SUCCESS) {
#endif /* LIBSHOUT_HAS_CONTENT_FORMAT */
        shout_free(shouttemp);
        return;
    }
    if (icecast->name && shout_set_meta(shouttemp, SHOUT_META_NAME, icecast->name) != SHOUTERR_SUCCESS) {
        shout_free(shouttemp);
        return;
    }
    if (icecast->genre && shout_set_meta(shouttemp, SHOUT_META_GENRE, icecast->genre) != SHOUTERR_SUCCESS) {
        shout_free(shouttemp);
        return;
    }
    if (icecast->description && shout_set_meta(shouttemp, SHOUT_META_DESCRIPTION, icecast->description) != SHOUTERR_SUCCESS) {
        shout_free(shouttemp);
        return;
    }
    char samplerates[20];
    sprintf(samplerates, "%d", MP3_RATE);
    shout_set_audio_info(shouttemp, SHOUT_AI_SAMPLERATE, samplerates);
    shout_set_audio_info(shouttemp, SHOUT_AI_CHANNELS, (mixmode == MM_STEREO ? "2" : "1"));

    if (shout_set_nonblocking(shouttemp, 1) != SHOUTERR_SUCCESS) {
        log(LOG_ERR, "Error setting non-blocking mode: %s\n", shout_get_error(shouttemp));
        return;
    }
    ret = shout_open(shouttemp);
    if (ret == SHOUTERR_SUCCESS)
        ret = SHOUTERR_CONNECTED;

    if (ret == SHOUTERR_BUSY || ret == SHOUTERR_RETRY)
        log(LOG_NOTICE, "Connecting to %s:%d/%s...\n", icecast->hostname, icecast->port, icecast->mountpoint);

    int shout_timeout = 30 * 5;  // 30 * 5 * 200ms = 30s
    while ((ret == SHOUTERR_BUSY || ret == SHOUTERR_RETRY) && shout_timeout-- > 0) {
        SLEEP(200);
        ret = shout_get_connected(shouttemp);
    }

    if (ret == SHOUTERR_CONNECTED) {
        log(LOG_NOTICE, "Connected to %s:%d/%s\n", icecast->hostname, icecast->port, icecast->mountpoint);
        SLEEP(100);
        icecast->shout = shouttemp;
    } else {
        log(LOG_WARNING, "Could not connect to %s:%d/%s: %s\n", icecast->hostname, icecast->port, icecast->mountpoint, shout_get_error(shouttemp));
        shout_close(shouttemp);
        shout_free(shouttemp);
        return;
    }
}

lame_t airlame_init(mix_modes mixmode, int highpass, int lowpass) {
    lame_t lame = lame_init();
    if (!lame) {
        log(LOG_WARNING, "lame_init failed\n");
        return NULL;
    }

    lame_set_in_samplerate(lame, WAVE_RATE);
    lame_set_VBR(lame, vbr_mtrh);
    lame_set_brate(lame, 16);
    lame_set_quality(lame, 7);
    lame_set_lowpassfreq(lame, lowpass);
    lame_set_highpassfreq(lame, highpass);
    lame_set_out_samplerate(lame, MP3_RATE);
    if (mixmode == MM_STEREO) {
        lame_set_num_channels(lame, 2);
        lame_set_mode(lame, JOINT_STEREO);
    } else {
        lame_set_num_channels(lame, 1);
        lame_set_mode(lame, MONO);
    }
    debug_print("lame init with mixmode=%s\n", mixmode == MM_STEREO ? "MM_STEREO" : "MM_MONO");
    lame_init_params(lame);
    return lame;
}

#ifdef WITH_FLAC_FILE_OUTPUT
FLAC__StreamEncoder* flac_encoder_init(mix_modes mixmode) {
    FLAC__StreamEncoder* encoder = FLAC__stream_encoder_new();
    if (!encoder) {
        log(LOG_WARNING, "FLAC__stream_encoder_new failed\n");
        return NULL;
    }

    if (!FLAC__stream_encoder_set_channels(encoder, mixmode == MM_STEREO ? 2 : 1) || !FLAC__stream_encoder_set_bits_per_sample(encoder, 16) ||
        !FLAC__stream_encoder_set_sample_rate(encoder, WAVE_RATE) || !FLAC__stream_encoder_set_compression_level(encoder, 5)) {
        log(LOG_WARNING, "Failed to configure FLAC encoder\n");
        FLAC__stream_encoder_delete(encoder);
        return NULL;
    }

    return encoder;
}

static int32_t float_to_pcm16(float sample) {
    if (isnan(sample)) {
        return 0;
    }
    if (sample > 1.0f) {
        sample = 1.0f;
    } else if (sample < -1.0f) {
        sample = -1.0f;
    }
    return (int32_t)(sample * 32767.0f);
}
#else
FLAC__StreamEncoder* flac_encoder_init(mix_modes) {
    return NULL;
}
#endif /* WITH_FLAC_FILE_OUTPUT */

class LameTone {
    unsigned char* _data;
    int _bytes;

   public:
    LameTone(mix_modes mixmode, int msec, unsigned int hz = 0) : _data(NULL), _bytes(0) {
        _data = (unsigned char*)XCALLOC(1, LAMEBUF_SIZE);

        int samples = (msec * WAVE_RATE) / 1000;
        float* buf = (float*)XCALLOC(samples, sizeof(float));

        debug_print("LameTone with mixmode=%s msec=%d hz=%u\n", mixmode == MM_STEREO ? "MM_STEREO" : "MM_MONO", msec, hz);
        if (hz > 0) {
            const float period = 1.0 / (float)hz;
            const float sample_time = 1.0 / (float)WAVE_RATE;
            float t = 0;
            for (int i = 0; i < samples; ++i, t += sample_time) {
                buf[i] = 0.9 * sinf(t * 2.0 * M_PI / period);
            }
        } else
            memset(buf, 0, samples * sizeof(float));
        lame_t lame = airlame_init(mixmode, 0, 0);
        if (lame) {
            _bytes = lame_encode_buffer_ieee_float(lame, buf, (mixmode == MM_STEREO ? buf : NULL), samples, _data, LAMEBUF_SIZE);
            if (_bytes > 0) {
                int flush_ofs = _bytes;
                if (flush_ofs & 0x1f)
                    flush_ofs += 0x20 - (flush_ofs & 0x1f);
                if (flush_ofs < LAMEBUF_SIZE) {
                    int flush_bytes = lame_encode_flush(lame, _data + flush_ofs, LAMEBUF_SIZE - flush_ofs);
                    if (flush_bytes > 0) {
                        memmove(_data + _bytes, _data + flush_ofs, flush_bytes);
                        _bytes += flush_bytes;
                    }
                }
            } else
                log(LOG_WARNING, "lame_encode_buffer_ieee_float: %d\n", _bytes);
            lame_close(lame);
        }
        free(buf);
    }

    ~LameTone() {
        if (_data)
            free(_data);
    }

    int write(FILE* f) {
        if (!_data || _bytes <= 0)
            return 1;

        if (fwrite(_data, 1, _bytes, f) != (unsigned int)_bytes) {
            log(LOG_WARNING, "LameTone: failed to write %d bytes\n", _bytes);
            return -1;
        }

        return 0;
    }
};

int rename_if_exists(char const* oldpath, char const* newpath) {
    int ret = rename(oldpath, newpath);
    if (ret < 0) {
        if (errno == ENOENT) {
            return 0;
        } else {
            log(LOG_ERR, "Could not rename %s to %s: %s\n", oldpath, newpath, strerror(errno));
        }
    }
    return ret;
}

/*
 * Open output file (mp3 or raw IQ) for append or initial write.
 * If appending to an audio file, insert discontinuity indictor tones
 * as well as the appropriate amount of silence when in continuous mode.
 */
static int open_file(file_data* fdata, mix_modes mixmode, int is_audio) {
    int rename_result = rename_if_exists(fdata->file_path.c_str(), fdata->file_path_tmp.c_str());
    fdata->f = fopen(fdata->file_path_tmp.c_str(), fdata->append ? "a+" : "w");
    if (fdata->f == NULL) {
        return -1;
    }

    struct stat st = {};
    if (!fdata->append || fstat(fileno(fdata->f), &st) != 0 || st.st_size == 0) {
        if (!fdata->split_on_transmission) {
            log(LOG_INFO, "Writing to %s\n", fdata->file_path.c_str());
        } else {
            debug_print("Writing to %s\n", fdata->file_path_tmp.c_str());
        }
        return 0;
    }
    if (rename_result < 0) {
        log(LOG_INFO, "Writing to %s\n", fdata->file_path.c_str());
        debug_print("Writing to %s\n", fdata->file_path_tmp.c_str());
    } else {
        log(LOG_INFO, "Appending from pos %llu to %s\n", (unsigned long long)st.st_size, fdata->file_path.c_str());
        debug_print("Appending from pos %llu to %s\n", (unsigned long long)st.st_size, fdata->file_path_tmp.c_str());
    }

    if (is_audio) {
        // fill missing space with marker tones
        LameTone lt_a(mixmode, 120, 2222);
        LameTone lt_b(mixmode, 120, 1111);
        LameTone lt_c(mixmode, 120, 555);

        int r = lt_a.write(fdata->f);
        if (r == 0)
            r = lt_b.write(fdata->f);
        if (r == 0)
            r = lt_c.write(fdata->f);

        // fill in time delta with silence if continuous output mode
        if (fdata->continuous) {
            time_t now = time(NULL);
            if (now > st.st_mtime) {
                time_t delta = now - st.st_mtime;
                if (delta > 3600) {
                    log(LOG_WARNING, "Too big time difference: %llu sec, limiting to one hour\n", (unsigned long long)delta);
                    delta = 3600;
                }
                LameTone lt_silence(mixmode, 1000);
                for (; (r == 0 && delta > 1); --delta)
                    r = lt_silence.write(fdata->f);
            }
        }

        if (r == 0)
            r = lt_c.write(fdata->f);
        if (r == 0)
            r = lt_b.write(fdata->f);
        if (r == 0)
            r = lt_a.write(fdata->f);

        if (r < 0)
            fseek(fdata->f, st.st_size, SEEK_SET);
    }
    return 0;
}

static void close_file(output_t* output) {
    file_data* fdata = (file_data*)(output->data);
    if (!fdata) {
        return;
    }

    // close all mp3 files for every output that has a lame context
    if (fdata->type == O_FILE && fdata->f && output->lame) {
        int encoded = lame_encode_flush_nogap(output->lame, output->lamebuf, LAMEBUF_SIZE);
        debug_print("closing file %s flushed %d\n", fdata->file_path.c_str(), encoded);

        if (encoded > 0) {
            size_t written = fwrite((void*)output->lamebuf, 1, (size_t)encoded, fdata->f);
            if (written == 0 || written < (size_t)encoded)
                log(LOG_WARNING, "Problem writing %s (%s)\n", fdata->file_path.c_str(), strerror(errno));
        }

        // write the lametag to the beginning of the file
        const int lametag_size = lame_get_lametag_frame(output->lame, output->lamebuf, LAMEBUF_SIZE);
        fseek(fdata->f, 0, SEEK_SET);
        fwrite(output->lamebuf, 1, lametag_size, fdata->f);
    }
#ifdef WITH_FLAC_FILE_OUTPUT
    if (fdata->type == O_FLAC_FILE && fdata->f && output->flac) {
        if (!FLAC__stream_encoder_finish(output->flac)) {
            log(LOG_WARNING, "Failed to finalize FLAC file %s\n", fdata->file_path.c_str());
        }
        // init_FILE() gives FILE* ownership to libFLAC; finish() closes it.
        fdata->f = NULL;
    }
#endif /* WITH_FLAC_FILE_OUTPUT */

    if (fdata->f) {
        fclose(fdata->f);
        fdata->f = NULL;
    }
    if (!fdata->file_path_tmp.empty()) {
        rename_if_exists(fdata->file_path_tmp.c_str(), fdata->file_path.c_str());
    }
    fdata->file_path.clear();
    fdata->file_path_tmp.clear();
}

/*
 * Close current output file based on certain conditions:
 * If "split_on_transmission" mode is true check:
 *   If current duration too long, or we've been idle too long
 * else (append or continuous) check:
 *   if hour is different.
 */
static void close_if_necessary(output_t* output) {
    file_data* fdata = (file_data*)(output->data);

    static const double MIN_TRANSMISSION_TIME_SEC = 1.0;
    static const double MAX_TRANSMISSION_TIME_SEC = 60.0 * 60.0;
    static const double MAX_TRANSMISSION_IDLE_SEC = 0.5;

    if (!fdata || !fdata->f) {
        return;
    }

    timeval current_time;
    gettimeofday(&current_time, NULL);

    if (fdata->split_on_transmission) {
        double duration_sec = delta_sec(&fdata->open_time, &current_time);
        double idle_sec = delta_sec(&fdata->last_write_time, &current_time);

        if (duration_sec > MAX_TRANSMISSION_TIME_SEC || (duration_sec > MIN_TRANSMISSION_TIME_SEC && idle_sec > MAX_TRANSMISSION_IDLE_SEC)) {
            debug_print("closing file %s, duration %f sec, idle %f sec\n", fdata->file_path.c_str(), duration_sec, idle_sec);
            close_file(output);
        }
        return;
    }

    // Check if the hour boundary was just crossed.  NOTE: Actual hour number doesn't matter but still
    // need to use localtime if enabled (some timezones have partial hour offsets)
    int start_hour;
    int current_hour;
    if (use_localtime) {
        start_hour = localtime(&(fdata->open_time.tv_sec))->tm_hour;
        current_hour = localtime(&current_time.tv_sec)->tm_hour;
    } else {
        start_hour = gmtime(&(fdata->open_time.tv_sec))->tm_hour;
        current_hour = gmtime(&current_time.tv_sec)->tm_hour;
    }

    if (start_hour != current_hour) {
        debug_print("closing file %s after crossing hour boundary\n", fdata->file_path.c_str());
        close_file(output);
    }
}

/*
 * For a particular channel file output, check if there is a file currently open.
 * If so, that file may need to be flushed and closed.
 *
 * If the existing open file is good for continued use, return true.
 * Otherwise, create a file name based on the current timestamp and
 * open that new file.  If that file open succeeded, return true.
 */
static bool output_file_ready(channel_t* channel, output_t* output) {
    file_data* fdata = (file_data*)(output->data);
    if (!fdata) {
        return false;
    }

    close_if_necessary(output);

    if (fdata->f) {  // still open
        return true;
    }

    timeval current_time;
    gettimeofday(&current_time, NULL);
    struct tm* time;
    if (use_localtime) {
        time = localtime(&current_time.tv_sec);
    } else {
        time = gmtime(&current_time.tv_sec);
    }

    char timestamp[32];
    if (strftime(timestamp, sizeof(timestamp), fdata->split_on_transmission ? "_%Y%m%d_%H%M%S" : "_%Y%m%d_%H", time) == 0) {
        log(LOG_NOTICE, "strftime returned 0\n");
        return false;
    }

    std::string output_dir;
    if (fdata->dated_subdirectories) {
        output_dir = make_dated_subdirs(fdata->basedir, time);
        if (output_dir.empty()) {
            log(LOG_ERR, "Failed to create dated subdirectory\n");
            return false;
        }
    } else {
        output_dir = fdata->basedir;
        make_dir(output_dir);
    }

    // use a string stream to build the output filepath
    std::stringstream ss;
    ss << output_dir << '/' << fdata->basename << timestamp;
    if (fdata->include_freq) {
        ss << '_' << channel->freqlist[channel->freq_idx].frequency;
    }
    ss << fdata->suffix;
    fdata->file_path = ss.str();

    fdata->file_path_tmp = fdata->file_path + ".tmp";

    fdata->open_time = fdata->last_write_time = current_time;

    const int is_audio = output->type == O_FILE ? 1 : 0;
    if (open_file(fdata, channel->mode, is_audio) < 0) {
        log(LOG_WARNING, "Cannot open output file %s (%s)\n", fdata->file_path_tmp.c_str(), strerror(errno));
        return false;
    }
#ifdef WITH_FLAC_FILE_OUTPUT
    if (output->type == O_FLAC_FILE) {
        if (!output->flac) {
            log(LOG_WARNING, "FLAC encoder is not initialized for %s\n", fdata->file_path.c_str());
            return false;
        }
        FLAC__StreamEncoderInitStatus init_status = FLAC__stream_encoder_init_FILE(output->flac, fdata->f, NULL, NULL);
        if (init_status != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
            log(LOG_WARNING, "Cannot initialize FLAC encoder for %s: %s\n", fdata->file_path.c_str(), FLAC__StreamEncoderInitStatusString[init_status]);
            fclose(fdata->f);
            fdata->f = NULL;
            return false;
        }
    }
#endif /* WITH_FLAC_FILE_OUTPUT */

    return true;
}

static bool is_udp_stream_overridden_by_file_playback(channel_t* channel, udp_stream_data* udp_data) {
    for (int i = 0; i < channel->output_count; ++i) {
        output_t* output = channel->outputs + i;
        if (output->type != O_FILE_CMD_TCP_SERVER) {
            continue;
        }
        file_cmd_tcp_server_data* cdata = (file_cmd_tcp_server_data*)output->data;
        if (cdata && cdata->playback_active && cdata->playback_udp_stream == udp_data) {
            return true;
        }
    }
    return false;
}

// Create all the output for a particular channel.
void process_outputs(channel_t* channel, int cur_scan_freq) {
    int meta_freq_idx = -1;
    if (cur_scan_freq >= 0 && cur_scan_freq < channel->freq_count) {
        meta_freq_idx = cur_scan_freq;
    } else {
        meta_freq_idx = channel->freq_idx;
    }

    freq_t* meta_fparms = (meta_freq_idx >= 0 && meta_freq_idx < channel->freq_count) ? (channel->freqlist + meta_freq_idx) : NULL;
    bool const has_signal = (channel->axcindicate != NO_SIGNAL);
    bool suppress_audio_output = false;
#ifdef NFM
    if (meta_fparms != NULL && (meta_fparms->modulation == MOD_AIS || meta_fparms->modulation == MOD_DSC)) {
        suppress_audio_output = true;
    }
#endif /* NFM */
    bool const has_audio_signal = has_signal && !suppress_audio_output;

    std::vector<DecodedMessage> decoded_messages;
#ifdef NFM
    if (meta_fparms != NULL && (meta_fparms->modulation == MOD_AIS || meta_fparms->modulation == MOD_DSC)) {
        decode_digital_messages(meta_fparms, channel->waveout, (size_t)WAVE_BATCH, &decoded_messages);
        for (size_t m = 0; m < decoded_messages.size(); ++m) {
            meta_fparms->decoded_counter++;
            if (decoded_messages[m].crc_ok) {
                meta_fparms->decoded_crc_ok_counter++;
            } else {
                meta_fparms->decoded_crc_bad_counter++;
            }
        }
    }
#endif /* NFM */

    for (int k = 0; k < channel->output_count; k++) {
        if (channel->outputs[k].enabled == false)
            continue;
        if (channel->outputs[k].type == O_ICECAST) {
            if (suppress_audio_output) {
                continue;
            }
            icecast_data* icecast = (icecast_data*)(channel->outputs[k].data);
            if (icecast->shout == NULL)
                continue;

            // encode and send mp3 to shoutcast output
            const auto& lame = channel->outputs[k].lame;
            const auto& lamebuf = channel->outputs[k].lamebuf;
            int mp3_bytes = lame_encode_buffer_ieee_float(lame, channel->waveout, (channel->mode == MM_STEREO ? channel->waveout_r : NULL), WAVE_BATCH, lamebuf, LAMEBUF_SIZE);
            if (mp3_bytes < 0) {
                log(LOG_WARNING, "lame_encode_buffer_ieee_float: %d\n", mp3_bytes);
            }

            if (mp3_bytes == 0) {
                continue;
            }

            int ret = shout_send(icecast->shout, channel->outputs[k].lamebuf, mp3_bytes);

            if (ret != SHOUTERR_SUCCESS || shout_queuelen(icecast->shout) > MAX_SHOUT_QUEUELEN) {
                if (shout_queuelen(icecast->shout) > MAX_SHOUT_QUEUELEN)
                    log(LOG_WARNING, "Exceeded max backlog for %s:%d/%s, disconnecting\n", icecast->hostname, icecast->port, icecast->mountpoint);
                // reset connection
                log(LOG_WARNING, "Lost connection to %s:%d/%s\n", icecast->hostname, icecast->port, icecast->mountpoint);
                shout_close(icecast->shout);
                shout_free(icecast->shout);
                icecast->shout = NULL;
            } else if (icecast->send_scan_freq_tags && cur_scan_freq >= 0) {
                shout_metadata_t* meta = shout_metadata_new();
                char description[32];
                if (channel->freqlist[channel->freq_idx].label != NULL) {
                    if (shout_metadata_add(meta, "song", channel->freqlist[channel->freq_idx].label) != SHOUTERR_SUCCESS) {
                        log(LOG_WARNING, "Failed to add shout metadata\n");
                    }
                } else {
                    snprintf(description, sizeof(description), "%.3f MHz", channel->freqlist[channel->freq_idx].frequency / 1000000.0);
                    if (shout_metadata_add(meta, "song", description) != SHOUTERR_SUCCESS) {
                        log(LOG_WARNING, "Failed to add shout metadata\n");
                    }
                }
                if (SHOUT_SET_METADATA(icecast->shout, meta) != SHOUTERR_SUCCESS) {
                    log(LOG_WARNING, "Failed to add shout metadata\n");
                }
                shout_metadata_free(meta);
            }
        } else if (channel->outputs[k].type == O_FILE || channel->outputs[k].type == O_RAWFILE
#ifdef WITH_FLAC_FILE_OUTPUT
                   || channel->outputs[k].type == O_FLAC_FILE
#endif /* WITH_FLAC_FILE_OUTPUT */
        ) {
            file_data* fdata = (file_data*)(channel->outputs[k].data);
            bool const is_audio_file = (channel->outputs[k].type == O_FILE
#ifdef WITH_FLAC_FILE_OUTPUT
                                        || channel->outputs[k].type == O_FLAC_FILE
#endif /* WITH_FLAC_FILE_OUTPUT */
            );
            bool const has_output_signal = is_audio_file ? has_audio_signal : has_signal;

            if (fdata->continuous == false && !has_output_signal && channel->outputs[k].active == false) {
                close_if_necessary(&channel->outputs[k]);
                continue;
            }

            if (!output_file_ready(channel, &channel->outputs[k])) {
                log(LOG_WARNING, "Output disabled\n");
                channel->outputs[k].enabled = false;
                continue;
            };

            // encode mp3 bytes if O_FILE
            const auto& lame = channel->outputs[k].lame;
            const auto& lamebuf = channel->outputs[k].lamebuf;
            int mp3_bytes = 0;
            if (channel->outputs[k].type == O_FILE) {
                if (suppress_audio_output) {
                    channel->outputs[k].active = false;
                    close_if_necessary(&channel->outputs[k]);
                    continue;
                }
                mp3_bytes = lame_encode_buffer_ieee_float(lame, channel->waveout, (channel->mode == MM_STEREO ? channel->waveout_r : NULL), WAVE_BATCH, lamebuf, LAMEBUF_SIZE);
                if (mp3_bytes < 0) {
                    log(LOG_WARNING, "lame_encode_buffer_ieee_float: %d\n", mp3_bytes);
                }

                if (mp3_bytes <= 0) {
                    continue;
                }
            }

            size_t buflen = 0, written = 0;
            if (channel->outputs[k].type == O_FILE) {
                buflen = (size_t)mp3_bytes;
                written = fwrite(lamebuf, 1, buflen, fdata->f);
            } else if (channel->outputs[k].type == O_RAWFILE) {
                buflen = 2 * sizeof(float) * WAVE_BATCH;
                written = fwrite(channel->iq_out, 1, buflen, fdata->f);
#ifdef WITH_FLAC_FILE_OUTPUT
            } else if (channel->outputs[k].type == O_FLAC_FILE) {
                if (suppress_audio_output) {
                    channel->outputs[k].active = false;
                    close_if_necessary(&channel->outputs[k]);
                    continue;
                }
                output_t* output = &channel->outputs[k];
                if (!output->flac || !output->flacbuf) {
                    log(LOG_WARNING, "FLAC encoder resources missing for %s\n", fdata->file_path.c_str());
                    buflen = 1;
                    written = 0;
                } else {
                    if (channel->mode == MM_STEREO) {
                        for (int i = 0; i < WAVE_BATCH; i++) {
                            output->flacbuf[2 * i] = float_to_pcm16(channel->waveout[i]);
                            output->flacbuf[2 * i + 1] = float_to_pcm16(channel->waveout_r[i]);
                        }
                    } else {
                        for (int i = 0; i < WAVE_BATCH; i++) {
                            output->flacbuf[i] = float_to_pcm16(channel->waveout[i]);
                        }
                    }
                    if (FLAC__stream_encoder_process_interleaved(output->flac, output->flacbuf, WAVE_BATCH)) {
                        buflen = written = 1;
                    } else {
                        buflen = 1;
                        written = 0;
                        log(LOG_WARNING, "Failed to encode FLAC block to %s\n", fdata->file_path.c_str());
                    }
                }
#endif /* WITH_FLAC_FILE_OUTPUT */
            }
            if (written < buflen) {
                if (ferror(fdata->f))
                    log(LOG_WARNING, "Cannot write to %s (%s), output disabled\n", fdata->file_path.c_str(), strerror(errno));
                else
                    log(LOG_WARNING, "Short write on %s, output disabled\n", fdata->file_path.c_str());
                close_file(&channel->outputs[k]);
                channel->outputs[k].enabled = false;
            }
            channel->outputs[k].active = has_output_signal;
            gettimeofday(&fdata->last_write_time, NULL);
        } else if (channel->outputs[k].type == O_MIXER) {
            if (suppress_audio_output) {
                continue;
            }
            mixer_data* mdata = (mixer_data*)(channel->outputs[k].data);
            mixer_put_samples(mdata->mixer, mdata->input, channel->waveout, has_audio_signal, WAVE_BATCH);
        } else if (channel->outputs[k].type == O_FILE_CMD_TCP_SERVER) {
            file_cmd_tcp_server_data* sdata = (file_cmd_tcp_server_data*)channel->outputs[k].data;
            file_cmd_tcp_server_poll(sdata);
        } else if (channel->outputs[k].type == O_SCAN_META_UDP) {
            scan_meta_udp_data* sdata = (scan_meta_udp_data*)channel->outputs[k].data;
            int out_meta_idx = -1;
            if (cur_scan_freq >= 0 && cur_scan_freq < channel->freq_count) {
                out_meta_idx = cur_scan_freq;
            } else if (sdata->continuous) {
                out_meta_idx = channel->freq_idx;
            }

            if (out_meta_idx >= 0 && out_meta_idx < channel->freq_count) {
                struct freq_t const* fparms = channel->freqlist + out_meta_idx;
                scan_meta_udp_write(sdata, -1, fparms->frequency, fparms->label, has_signal);
                for (size_t m = 0; m < decoded_messages.size(); ++m) {
                    scan_meta_udp_write_decoded(sdata, -1, fparms->frequency, fparms->label, decoded_messages[m].modulation.c_str(), decoded_messages[m].msg_type.c_str(),
                                                decoded_messages[m].payload.c_str(), decoded_messages[m].crc_ok, decoded_messages[m].mmsi);
                }
            }
        } else if (channel->outputs[k].type == O_UDP_STREAM) {
            udp_stream_data* sdata = (udp_stream_data*)channel->outputs[k].data;
            if (suppress_audio_output) {
                continue;
            }
            if (is_udp_stream_overridden_by_file_playback(channel, sdata)) {
                continue;
            }

            if (sdata->continuous == false && !has_audio_signal) {
                continue;
            }

            if (channel->mode == MM_MONO) {
                udp_stream_write(sdata, channel->waveout, (size_t)WAVE_BATCH * sizeof(float));
            } else {
                udp_stream_write(sdata, channel->waveout, channel->waveout_r, (size_t)WAVE_BATCH * sizeof(float));
            }
        } else if (channel->outputs[k].type == O_UDP_STREAM_SERVER) {
            udp_stream_server_data* sdata = (udp_stream_server_data*)channel->outputs[k].data;
            if (suppress_audio_output) {
                continue;
            }

            if (sdata->continuous == false && !has_audio_signal) {
                continue;
            }

            if (channel->mode == MM_MONO) {
                udp_stream_server_write(sdata, channel->waveout, (size_t)WAVE_BATCH * sizeof(float));
            } else {
                udp_stream_server_write(sdata, channel->waveout, channel->waveout_r, (size_t)WAVE_BATCH * sizeof(float));
            }
        } else if (channel->outputs[k].type == O_SCAN_META_TCP_SERVER) {
            scan_meta_tcp_server_data* sdata = (scan_meta_tcp_server_data*)channel->outputs[k].data;
            int out_meta_idx = -1;
            if (cur_scan_freq >= 0 && cur_scan_freq < channel->freq_count) {
                out_meta_idx = cur_scan_freq;
            } else if (sdata->continuous) {
                out_meta_idx = channel->freq_idx;
            }

            if (out_meta_idx >= 0 && out_meta_idx < channel->freq_count) {
                struct freq_t const* fparms = channel->freqlist + out_meta_idx;
                scan_meta_tcp_server_write(sdata, -1, fparms->frequency, fparms->label, has_signal);
                for (size_t m = 0; m < decoded_messages.size(); ++m) {
                    scan_meta_tcp_server_write_decoded(sdata, -1, fparms->frequency, fparms->label, decoded_messages[m].modulation.c_str(), decoded_messages[m].msg_type.c_str(),
                                                       decoded_messages[m].payload.c_str(), decoded_messages[m].crc_ok, decoded_messages[m].mmsi);
                }
            }
        } else if (channel->outputs[k].type == O_TCP_STREAM_SERVER) {
            tcp_stream_server_data* sdata = (tcp_stream_server_data*)channel->outputs[k].data;
            if (suppress_audio_output) {
                continue;
            }

            if (sdata->continuous == false && !has_audio_signal) {
                continue;
            }

            if (channel->mode == MM_MONO) {
                tcp_stream_server_write(sdata, channel->waveout, (size_t)WAVE_BATCH * sizeof(float));
            } else {
                tcp_stream_server_write(sdata, channel->waveout, channel->waveout_r, (size_t)WAVE_BATCH * sizeof(float));
            }

#ifdef WITH_PULSEAUDIO
        } else if (channel->outputs[k].type == O_PULSE) {
            if (suppress_audio_output) {
                continue;
            }
            pulse_data* pdata = (pulse_data*)(channel->outputs[k].data);
            if (pdata->continuous == false && !has_audio_signal)
                continue;

            pulse_write_stream(pdata, channel->mode, channel->waveout, channel->waveout_r, (size_t)WAVE_BATCH * sizeof(float));
#endif /* WITH_PULSEAUDIO */
        }
    }
}

void disable_channel_outputs(channel_t* channel) {
    for (int k = 0; k < channel->output_count; k++) {
        output_t* output = channel->outputs + k;
        output->enabled = false;
        if (output->type == O_ICECAST) {
            icecast_data* icecast = (icecast_data*)(channel->outputs[k].data);
            if (icecast->shout == NULL)
                continue;
            log(LOG_WARNING, "Closing connection to %s:%d/%s\n", icecast->hostname, icecast->port, icecast->mountpoint);
            shout_close(icecast->shout);
            shout_free(icecast->shout);
            icecast->shout = NULL;
        } else if (output->type == O_FILE || output->type == O_RAWFILE
#ifdef WITH_FLAC_FILE_OUTPUT
                   || output->type == O_FLAC_FILE
#endif /* WITH_FLAC_FILE_OUTPUT */
        ) {
            close_file(&channel->outputs[k]);
        } else if (output->type == O_MIXER) {
            mixer_data* mdata = (mixer_data*)(output->data);
            mixer_disable_input(mdata->mixer, mdata->input);
        } else if (output->type == O_SCAN_META_UDP) {
            scan_meta_udp_data* sdata = (scan_meta_udp_data*)output->data;
            scan_meta_udp_shutdown(sdata);
        } else if (output->type == O_UDP_STREAM) {
            udp_stream_data* sdata = (udp_stream_data*)output->data;
            udp_stream_shutdown(sdata);
        } else if (output->type == O_UDP_STREAM_SERVER) {
            udp_stream_server_data* sdata = (udp_stream_server_data*)output->data;
            udp_stream_server_shutdown(sdata);
        } else if (output->type == O_SCAN_META_TCP_SERVER) {
            scan_meta_tcp_server_data* sdata = (scan_meta_tcp_server_data*)output->data;
            scan_meta_tcp_server_shutdown(sdata);
        } else if (output->type == O_FILE_CMD_TCP_SERVER) {
            file_cmd_tcp_server_data* sdata = (file_cmd_tcp_server_data*)output->data;
            file_cmd_tcp_server_shutdown(sdata);
        } else if (output->type == O_TCP_STREAM_SERVER) {
            tcp_stream_server_data* sdata = (tcp_stream_server_data*)output->data;
            tcp_stream_server_shutdown(sdata);
#ifdef WITH_PULSEAUDIO
        } else if (output->type == O_PULSE) {
            pulse_data* pdata = (pulse_data*)(output->data);
            pulse_shutdown(pdata);
#endif /* WITH_PULSEAUDIO */
        }
    }
}

void disable_device_outputs(device_t* dev) {
    log(LOG_INFO, "Disabling device outputs\n");
    for (int j = 0; j < dev->channel_count; j++) {
        disable_channel_outputs(dev->channels + j);
    }
}

static void print_channel_metric(FILE* f, char const* name, float freq, char* label) {
    fprintf(f, "%s{freq=\"%.3f\"", name, freq / 1000000.0);
    if (label != NULL) {
        fprintf(f, ",label=\"%s\"", label);
    }
    fprintf(f, "}");
}

static void output_channel_noise_levels(FILE* f) {
    fprintf(f,
            "# HELP channel_noise_level Raw squelch noise_level.\n"
            "# TYPE channel_noise_level gauge\n");

    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        for (int j = 0; j < dev->channel_count; j++) {
            channel_t* channel = devices[i].channels + j;
            for (int k = 0; k < channel->freq_count; k++) {
                print_channel_metric(f, "channel_noise_level", channel->freqlist[k].frequency, channel->freqlist[k].label);
                fprintf(f, "\t%.3f\n", channel->freqlist[k].squelch.noise_level());
            }
        }
    }
    fprintf(f, "\n");
}

static void output_channel_dbfs_noise_levels(FILE* f) {
    fprintf(f,
            "# HELP channel_dbfs_noise_level Squelch noise_level as dBFS.\n"
            "# TYPE channel_dbfs_noise_level gauge\n");

    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        for (int j = 0; j < dev->channel_count; j++) {
            channel_t* channel = devices[i].channels + j;
            for (int k = 0; k < channel->freq_count; k++) {
                print_channel_metric(f, "channel_dbfs_noise_level", channel->freqlist[k].frequency, channel->freqlist[k].label);
                fprintf(f, "\t%.3f\n", level_to_dBFS(channel->freqlist[k].squelch.noise_level()));
            }
        }
    }
    fprintf(f, "\n");
}

static void output_channel_signal_levels(FILE* f) {
    fprintf(f,
            "# HELP channel_signal_level Raw squelch signal_level.\n"
            "# TYPE channel_signal_level gauge\n");

    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        for (int j = 0; j < dev->channel_count; j++) {
            channel_t* channel = devices[i].channels + j;
            for (int k = 0; k < channel->freq_count; k++) {
                print_channel_metric(f, "channel_signal_level", channel->freqlist[k].frequency, channel->freqlist[k].label);
                fprintf(f, "\t%.3f\n", channel->freqlist[k].squelch.signal_level());
            }
        }
    }
    fprintf(f, "\n");
}

static void output_channel_dbfs_signal_levels(FILE* f) {
    fprintf(f,
            "# HELP channel_dbfs_signal_level Squelch signal_level as dBFS.\n"
            "# TYPE channel_dbfs_signal_level gauge\n");

    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        for (int j = 0; j < dev->channel_count; j++) {
            channel_t* channel = devices[i].channels + j;
            for (int k = 0; k < channel->freq_count; k++) {
                print_channel_metric(f, "channel_dbfs_signal_level", channel->freqlist[k].frequency, channel->freqlist[k].label);
                fprintf(f, "\t%.3f\n", level_to_dBFS(channel->freqlist[k].squelch.signal_level()));
            }
        }
    }
    fprintf(f, "\n");
}

static void output_channel_squelch_levels(FILE* f) {
    fprintf(f,
            "# HELP channel_squelch_level Squelch squelch_level.\n"
            "# TYPE channel_squelch_level gauge\n");

    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        for (int j = 0; j < dev->channel_count; j++) {
            channel_t* channel = devices[i].channels + j;
            for (int k = 0; k < channel->freq_count; k++) {
                print_channel_metric(f, "channel_squelch_level", channel->freqlist[k].frequency, channel->freqlist[k].label);
                fprintf(f, "\t%.3f\n", channel->freqlist[k].squelch.squelch_level());
            }
        }
    }
    fprintf(f, "\n");
}

static void output_channel_squelch_counter(FILE* f) {
    fprintf(f,
            "# HELP channel_squelch_counter Squelch open_count.\n"
            "# TYPE channel_squelch_counter counter\n");

    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        for (int j = 0; j < dev->channel_count; j++) {
            channel_t* channel = devices[i].channels + j;
            for (int k = 0; k < channel->freq_count; k++) {
                print_channel_metric(f, "channel_squelch_counter", channel->freqlist[k].frequency, channel->freqlist[k].label);
                fprintf(f, "\t%zu\n", channel->freqlist[k].squelch.open_count());
            }
        }
    }
    fprintf(f, "\n");
}

static void output_channel_flappy_counter(FILE* f) {
    fprintf(f,
            "# HELP channel_flappy_counter Squelch flappy_count.\n"
            "# TYPE channel_flappy_counter counter\n");

    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        for (int j = 0; j < dev->channel_count; j++) {
            channel_t* channel = devices[i].channels + j;
            for (int k = 0; k < channel->freq_count; k++) {
                print_channel_metric(f, "channel_flappy_counter", channel->freqlist[k].frequency, channel->freqlist[k].label);
                fprintf(f, "\t%zu\n", channel->freqlist[k].squelch.flappy_count());
            }
        }
    }
    fprintf(f, "\n");
}

static void output_channel_ctcss_counter(FILE* f) {
    fprintf(f,
            "# HELP channel_ctcss_counter count of windows with CTCSS detected.\n"
            "# TYPE channel_ctcss_counter counter\n");

    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        for (int j = 0; j < dev->channel_count; j++) {
            channel_t* channel = devices[i].channels + j;
            for (int k = 0; k < channel->freq_count; k++) {
                print_channel_metric(f, "channel_ctcss_counter", channel->freqlist[k].frequency, channel->freqlist[k].label);
                fprintf(f, "\t%zu\n", channel->freqlist[k].squelch.ctcss_count());
            }
        }
    }
    fprintf(f, "\n");
}

static void output_channel_no_ctcss_counter(FILE* f) {
    fprintf(f,
            "# HELP channel_no_ctcss_counter count of windows without CTCSS detected.\n"
            "# TYPE channel_no_ctcss_counter counter\n");

    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        for (int j = 0; j < dev->channel_count; j++) {
            channel_t* channel = devices[i].channels + j;
            for (int k = 0; k < channel->freq_count; k++) {
                print_channel_metric(f, "channel_no_ctcss_counter", channel->freqlist[k].frequency, channel->freqlist[k].label);
                fprintf(f, "\t%zu\n", channel->freqlist[k].squelch.no_ctcss_count());
            }
        }
    }
    fprintf(f, "\n");
}

static void output_channel_activity_counters(FILE* f) {
    fprintf(f,
            "# HELP channel_activity_counter Loops of output_thread with frequency active.\n"
            "# TYPE channel_activity_counter counter\n");

    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        for (int j = 0; j < dev->channel_count; j++) {
            channel_t* channel = devices[i].channels + j;
            for (int k = 0; k < channel->freq_count; k++) {
                print_channel_metric(f, "channel_activity_counter", channel->freqlist[k].frequency, channel->freqlist[k].label);
                fprintf(f, "\t%zu\n", channel->freqlist[k].active_counter);
            }
        }
    }
    fprintf(f, "\n");
}

static void output_channel_decoded_counters(FILE* f) {
    fprintf(f,
            "# HELP channel_decoded_counter Count of decoded digital messages.\n"
            "# TYPE channel_decoded_counter counter\n");

    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        for (int j = 0; j < dev->channel_count; j++) {
            channel_t* channel = devices[i].channels + j;
            for (int k = 0; k < channel->freq_count; k++) {
                print_channel_metric(f, "channel_decoded_counter", channel->freqlist[k].frequency, channel->freqlist[k].label);
                fprintf(f, "\t%zu\n", channel->freqlist[k].decoded_counter);
            }
        }
    }
    fprintf(f, "\n");
}

static void output_channel_decoded_crc_ok_counters(FILE* f) {
    fprintf(f,
            "# HELP channel_decoded_crc_ok_counter Count of decoded digital messages with valid CRC.\n"
            "# TYPE channel_decoded_crc_ok_counter counter\n");

    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        for (int j = 0; j < dev->channel_count; j++) {
            channel_t* channel = devices[i].channels + j;
            for (int k = 0; k < channel->freq_count; k++) {
                print_channel_metric(f, "channel_decoded_crc_ok_counter", channel->freqlist[k].frequency, channel->freqlist[k].label);
                fprintf(f, "\t%zu\n", channel->freqlist[k].decoded_crc_ok_counter);
            }
        }
    }
    fprintf(f, "\n");
}

static void output_channel_decoded_crc_bad_counters(FILE* f) {
    fprintf(f,
            "# HELP channel_decoded_crc_bad_counter Count of decoded digital messages with invalid CRC.\n"
            "# TYPE channel_decoded_crc_bad_counter counter\n");

    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        for (int j = 0; j < dev->channel_count; j++) {
            channel_t* channel = devices[i].channels + j;
            for (int k = 0; k < channel->freq_count; k++) {
                print_channel_metric(f, "channel_decoded_crc_bad_counter", channel->freqlist[k].frequency, channel->freqlist[k].label);
                fprintf(f, "\t%zu\n", channel->freqlist[k].decoded_crc_bad_counter);
            }
        }
    }
    fprintf(f, "\n");
}

static void output_device_buffer_overflows(FILE* f) {
    fprintf(f,
            "# HELP buffer_overflow_count Number of times a device's buffer has overflowed.\n"
            "# TYPE buffer_overflow_count counter\n");

    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        fprintf(f, "buffer_overflow_count{device=\"%d\"}\t%zu\n", i, dev->input->overflow_count);
    }
    fprintf(f, "\n");
}

static void output_output_overruns(FILE* f) {
    fprintf(f,
            "# HELP output_overrun_count Number of times a device or mixer output has overrun.\n"
            "# TYPE output_overrun_count counter\n");

    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        fprintf(f, "output_overrun_count{device=\"%d\"}\t%zu\n", i, dev->output_overrun_count);
    }
    for (int i = 0; i < mixer_count; i++) {
        mixer_t* mixer = mixers + i;
        fprintf(f, "output_overrun_count{mixer=\"%d\"}\t%zu\n", i, mixer->output_overrun_count);
    }
    fprintf(f, "\n");
}

static void output_input_overruns(FILE* f) {
    if (mixer_count == 0) {
        return;
    }

    fprintf(f,
            "# HELP input_overrun_count Number of times mixer input has overrun.\n"
            "# TYPE input_overrun_count counter\n");

    for (int i = 0; i < mixer_count; i++) {
        mixer_t* mixer = mixers + i;
        for (int j = 0; j < mixer->input_count; j++) {
            mixinput_t* input = mixer->inputs + j;
            fprintf(f, "input_overrun_count{mixer=\"%d\",input=\"%d\"}\t%zu\n", i, j, input->input_overrun_count);
        }
    }
    fprintf(f, "\n");
}

void write_stats_file(timeval* last_stats_write) {
    if (!stats_filepath) {
        return;
    }

    timeval current_time;
    gettimeofday(&current_time, NULL);

    static const double STATS_FILE_TIMING = 15.0;
    if (!do_exit && delta_sec(last_stats_write, &current_time) < STATS_FILE_TIMING) {
        return;
    }

    *last_stats_write = current_time;

    FILE* file = fopen(stats_filepath, "w");
    if (!file) {
        log(LOG_WARNING, "Cannot open output file %s (%s)\n", stats_filepath, strerror(errno));
        return;
    }

    output_channel_activity_counters(file);
    output_channel_noise_levels(file);
    output_channel_dbfs_noise_levels(file);
    output_channel_signal_levels(file);
    output_channel_dbfs_signal_levels(file);
    output_channel_squelch_counter(file);
    output_channel_squelch_levels(file);
    output_channel_flappy_counter(file);
    output_channel_ctcss_counter(file);
    output_channel_no_ctcss_counter(file);
    output_channel_decoded_counters(file);
    output_channel_decoded_crc_ok_counters(file);
    output_channel_decoded_crc_bad_counters(file);
    output_device_buffer_overflows(file);
    output_output_overruns(file);
    output_input_overruns(file);

    fclose(file);
}

void* output_thread(void* param) {
    assert(param != NULL);
    output_params_t* output_param = (output_params_t*)param;
    struct freq_tag tag;
    struct timeval tv;
    int new_freq = -1;
    timeval last_stats_write = {0, 0};

    debug_print("Starting output thread, devices %d:%d, mixers %d:%d, signal %p\n", output_param->device_start, output_param->device_end, output_param->mixer_start, output_param->mixer_end,
                output_param->mp3_signal);

#ifdef DEBUG
    timeval ts, te;
    gettimeofday(&ts, NULL);
#endif /* DEBUG */
    while (!do_exit) {
        output_param->mp3_signal->wait();
        for (int i = output_param->mixer_start; i < output_param->mixer_end; i++) {
            if (mixers[i].enabled == false)
                continue;
            channel_t* channel = &mixers[i].channel;
            if (channel->state == CH_READY) {
                process_outputs(channel, -1);
                channel->state = CH_DIRTY;
            }
        }
#ifdef DEBUG
        gettimeofday(&te, NULL);
        debug_bulk_print("mixeroutput: %lu.%lu %lu\n", te.tv_sec, (unsigned long)te.tv_usec, (te.tv_sec - ts.tv_sec) * 1000000UL + te.tv_usec - ts.tv_usec);
        ts.tv_sec = te.tv_sec;
        ts.tv_usec = te.tv_usec;
#endif /* DEBUG */
        for (int i = output_param->device_start; i < output_param->device_end; i++) {
            device_t* dev = devices + i;
            if (dev->input->state == INPUT_RUNNING && dev->waveavail) {
                if (dev->mode == R_SCAN) {
                    tag_queue_get(dev, &tag);
                    if (tag.freq >= 0) {
                        tag.tv.tv_sec += shout_metadata_delay;
                        gettimeofday(&tv, NULL);
                        if (tag.tv.tv_sec < tv.tv_sec || (tag.tv.tv_sec == tv.tv_sec && tag.tv.tv_usec <= tv.tv_usec)) {
                            new_freq = tag.freq;
                            tag_queue_advance(dev);
                        }
                    }
                }
                for (int j = 0; j < dev->channel_count; j++) {
                    channel_t* channel = devices[i].channels + j;
                    process_outputs(channel, new_freq);
                    memcpy(channel->waveout, channel->waveout + WAVE_BATCH, AGC_EXTRA * 4);
                }
                dev->waveavail = 0;
            }
            // make sure we don't carry new_freq value to the next receiver which might be working
            // in multichannel mode
            new_freq = -1;
        }
        if (output_param->device_start == 0) {
            write_stats_file(&last_stats_write);
        }
    }
    return 0;
}

// reconnect as required
void* output_check_thread(void*) {
    while (!do_exit) {
        SLEEP(10000);
        for (int i = 0; i < device_count; i++) {
            device_t* dev = devices + i;
            for (int j = 0; j < dev->channel_count; j++) {
                for (int k = 0; k < dev->channels[j].output_count; k++) {
                    if (dev->channels[j].outputs[k].type == O_ICECAST) {
                        icecast_data* icecast = (icecast_data*)(dev->channels[j].outputs[k].data);
                        if (dev->input->state == INPUT_FAILED) {
                            if (icecast->shout) {
                                log(LOG_WARNING, "Device #%d failed, disconnecting stream %s:%d/%s\n", i, icecast->hostname, icecast->port, icecast->mountpoint);
                                shout_close(icecast->shout);
                                shout_free(icecast->shout);
                                icecast->shout = NULL;
                            }
                        } else if (dev->input->state == INPUT_RUNNING) {
                            if (icecast->shout == NULL) {
                                log(LOG_NOTICE, "Trying to reconnect to %s:%d/%s...\n", icecast->hostname, icecast->port, icecast->mountpoint);
                                shout_setup(icecast, dev->channels[j].mode);
                            }
                        }
                    } else if (dev->channels[j].outputs[k].type == O_UDP_STREAM) {
                        udp_stream_data* sdata = (udp_stream_data*)dev->channels[j].outputs[k].data;

                        if (dev->input->state == INPUT_FAILED) {
                            udp_stream_shutdown(sdata);
                        } else if (dev->input->state == INPUT_RUNNING && sdata->send_socket == -1) {
                            udp_stream_init(sdata, dev->channels[j].mode, (size_t)WAVE_BATCH * sizeof(float));
                        }
                    } else if (dev->channels[j].outputs[k].type == O_SCAN_META_UDP) {
                        scan_meta_udp_data* sdata = (scan_meta_udp_data*)dev->channels[j].outputs[k].data;

                        if (dev->input->state == INPUT_FAILED) {
                            scan_meta_udp_shutdown(sdata);
                        } else if (dev->input->state == INPUT_RUNNING && sdata->send_socket == -1) {
                            scan_meta_udp_init(sdata);
                        }
                    } else if (dev->channels[j].outputs[k].type == O_UDP_STREAM_SERVER) {
                        udp_stream_server_data* sdata = (udp_stream_server_data*)dev->channels[j].outputs[k].data;

                        if (dev->input->state == INPUT_FAILED) {
                            udp_stream_server_shutdown(sdata);
                        } else if (dev->input->state == INPUT_RUNNING && sdata->socket_fd == -1) {
                            udp_stream_server_init(sdata, dev->channels[j].mode, (size_t)WAVE_BATCH * sizeof(float));
                        }
                    } else if (dev->channels[j].outputs[k].type == O_TCP_STREAM_SERVER) {
                        tcp_stream_server_data* sdata = (tcp_stream_server_data*)dev->channels[j].outputs[k].data;

                        if (dev->input->state == INPUT_FAILED) {
                            tcp_stream_server_shutdown(sdata);
                        } else if (dev->input->state == INPUT_RUNNING && sdata->listen_socket == -1) {
                            tcp_stream_server_init(sdata, dev->channels[j].mode, (size_t)WAVE_BATCH * sizeof(float));
                        }
                    } else if (dev->channels[j].outputs[k].type == O_SCAN_META_TCP_SERVER) {
                        scan_meta_tcp_server_data* sdata = (scan_meta_tcp_server_data*)dev->channels[j].outputs[k].data;

                        if (dev->input->state == INPUT_FAILED) {
                            scan_meta_tcp_server_shutdown(sdata);
                        } else if (dev->input->state == INPUT_RUNNING && sdata->listen_socket == -1) {
                            scan_meta_tcp_server_init(sdata);
                        }
                    } else if (dev->channels[j].outputs[k].type == O_FILE_CMD_TCP_SERVER) {
                        file_cmd_tcp_server_data* sdata = (file_cmd_tcp_server_data*)dev->channels[j].outputs[k].data;

                        if (dev->input->state == INPUT_FAILED) {
                            file_cmd_tcp_server_shutdown(sdata);
                        } else if (dev->input->state == INPUT_RUNNING && sdata->listen_socket == -1) {
                            file_cmd_tcp_server_set_dirs(sdata, dev->channels + j);
                            file_cmd_tcp_server_init(sdata);
                        }
#ifdef WITH_PULSEAUDIO
                    } else if (dev->channels[j].outputs[k].type == O_PULSE) {
                        pulse_data* pdata = (pulse_data*)(dev->channels[j].outputs[k].data);
                        if (dev->input->state == INPUT_FAILED) {
                            if (pdata->context) {
                                pulse_shutdown(pdata);
                            }
                        } else if (dev->input->state == INPUT_RUNNING) {
                            if (pdata->context == NULL) {
                                pulse_setup(pdata, dev->channels[j].mode);
                            }
                        }
#endif /* WITH_PULSEAUDIO */
                    }
                }
            }
        }
        for (int i = 0; i < mixer_count; i++) {
            if (mixers[i].enabled == false)
                continue;
            for (int k = 0; k < mixers[i].channel.output_count; k++) {
                if (mixers[i].channel.outputs[k].enabled == false)
                    continue;
                if (mixers[i].channel.outputs[k].type == O_ICECAST) {
                    icecast_data* icecast = (icecast_data*)(mixers[i].channel.outputs[k].data);
                    if (icecast->shout == NULL) {
                        log(LOG_NOTICE, "Trying to reconnect to %s:%d/%s...\n", icecast->hostname, icecast->port, icecast->mountpoint);
                        shout_setup(icecast, mixers[i].channel.mode);
                    }
                } else if (mixers[i].channel.outputs[k].type == O_TCP_STREAM_SERVER) {
                    tcp_stream_server_data* sdata = (tcp_stream_server_data*)(mixers[i].channel.outputs[k].data);
                    if (sdata->listen_socket == -1) {
                        tcp_stream_server_init(sdata, mixers[i].channel.mode, (size_t)WAVE_BATCH * sizeof(float));
                    }
                } else if (mixers[i].channel.outputs[k].type == O_UDP_STREAM_SERVER) {
                    udp_stream_server_data* sdata = (udp_stream_server_data*)(mixers[i].channel.outputs[k].data);
                    if (sdata->socket_fd == -1) {
                        udp_stream_server_init(sdata, mixers[i].channel.mode, (size_t)WAVE_BATCH * sizeof(float));
                    }
#ifdef WITH_PULSEAUDIO
                } else if (mixers[i].channel.outputs[k].type == O_PULSE) {
                    pulse_data* pdata = (pulse_data*)(mixers[i].channel.outputs[k].data);
                    if (pdata->context == NULL) {
                        pulse_setup(pdata, mixers[i].channel.mode);
                    }
#endif /* WITH_PULSEAUDIO */
                }
            }
        }
    }
    return 0;
}
