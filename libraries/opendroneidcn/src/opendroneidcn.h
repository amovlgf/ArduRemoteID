/*
Copyright (C) 2026 Jun Zhang

SPDX-License-Identifier: Apache-2.0

Open Drone ID C Library — GB 46750-2025

Maintainer:
Jun Zhang
zhangjun.sole@qq.com

GB 46750-2025 — Civil Unmanned Aircraft System Operational Identification
(中华人民共和国民用无人驾驶航空器系统运营识别规范)

Broadcast-mode RID packet parser for the Chinese national standard.
See opendroneidcn.c for the encoding details.

Reference: GB 46750-2025, section 5.2, Tables 1-3.
Modified for ArduRemoteID: explicit unknown values and strict input validation.
*/

#ifndef OPENDRONEID_CN46750_H
#define OPENDRONEID_CN46750_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===================== Error Code Definition ===================== */
typedef enum {
    RID_OK = 0,                // Operation succeeded, all known fields parsed
    RID_OK_EXTENSION = 1,      // Success, but packet contains additional flag
                                //   bytes beyond the 3 currently defined —
                                //   future CAAC extension fields were skipped
    RID_ERR_PARAM = -1,        // Invalid or null input parameter
    RID_ERR_INVALID_TYPE = -3, // Packet type mismatch (not 0xFF)
    RID_ERR_INVALID_LEN = -4,  // Data length mismatch
    RID_ERR_PARSE_FAIL = -5    // Field parsing failed (mandatory field missing)
} RID_Status_t;

/* ===================== Protocol Enumerations ===================== */
// 004 Civil UAV classification
typedef enum {
    DRONE_CLASS_MICRO  = 0,  // Micro UAV
    DRONE_CLASS_LIGHT  = 1,  // Light UAV
    DRONE_CLASS_SMALL  = 2,  // Small UAV
    DRONE_CLASS_MEDIUM = 3,  // Medium UAV
    DRONE_CLASS_LARGE  = 4   // Large UAV
} DroneClass_t;

// 003 Operation category
typedef enum {
    OP_CATEGORY_UNDEFINED = 0, // Undefined
    OP_CATEGORY_OPEN      = 1, // Open category
    OP_CATEGORY_SPECIFIC  = 2, // Specific category
    OP_CATEGORY_CERTIFIED = 3  // Certified category
} OperationCategory_t;

// 005 GCS position type
typedef enum {
    POS_TYPE_TAKEOFF = 0,  // Takeoff point position
    POS_TYPE_GCS     = 1   // Ground control station position
} PositionType_t;

// 015 Operation status
typedef enum {
    OP_STATUS_UNKNOWN        = 0, // Not reported
    OP_STATUS_GROUND         = 1, // On ground
    OP_STATUS_AIR            = 2, // In flight
    OP_STATUS_EMERGENCY      = 3, // Emergency state
    OP_STATUS_FAIL_NORMAL    = 4, // RID function failure (non-emergency)
    OP_STATUS_FAIL_EMERGENCY = 5  // RID function failure (emergency)
} OperationStatus_t;

// 016 Coordinate system type
typedef enum {
    COORD_WGS84    = 0,  // WGS-84 coordinate system
    COORD_CGCS2000 = 1   // CGCS2000 coordinate system
} CoordSystem_t;

/* Unknown/unavailable markers for fields with a specified wire sentinel.
 * Position markers apply to the complete longitude/latitude pair. */
#define CN46750_FIELD_UNKNOWN(id) (UINT32_C(1) << ((id) - 1))
#define CN46750_UNKNOWN_GCS_POSITION CN46750_FIELD_UNKNOWN(6)
#define CN46750_UNKNOWN_GCS_ALTITUDE CN46750_FIELD_UNKNOWN(7)
#define CN46750_UNKNOWN_UAV_POSITION CN46750_FIELD_UNKNOWN(8)
#define CN46750_UNKNOWN_TRACK_ANGLE CN46750_FIELD_UNKNOWN(9)
#define CN46750_UNKNOWN_GROUND_SPEED CN46750_FIELD_UNKNOWN(10)
#define CN46750_UNKNOWN_REL_ALTITUDE CN46750_FIELD_UNKNOWN(11)
#define CN46750_UNKNOWN_VERTICAL_SPEED CN46750_FIELD_UNKNOWN(12)
#define CN46750_UNKNOWN_UAV_ALTITUDE CN46750_FIELD_UNKNOWN(13)
#define CN46750_UNKNOWN_PRESSURE_ALTITUDE CN46750_FIELD_UNKNOWN(14)
#define CN46750_UNKNOWN_TIMESTAMP CN46750_FIELD_UNKNOWN(20)
#define CN46750_UNKNOWN_ALL (UINT32_C(0x83FE0))
#define CN46750_MAX_TIMESTAMP UINT64_C(0xFFFFFFFFFFFF)
#define CN46750_MIN_PACKET_SIZE 72U
#define CN46750_MAX_PACKET_SIZE 78U

/* ===================== RID Data Structure ===================== */
// Stores physical values (lat/lon, altitude, speed, etc.)
typedef struct {
    /* 001 Mandatory: Unique product identification code, ASCII, max 20 bytes */
    char sn[21];

    /* 002 Mandatory: Last 8 chars of real-name registration mark, ASCII */
    char uin[9];

    /* 003 Optional: Operation category */
    bool                has_op_category;
    OperationCategory_t op_category;

    /* 004 Mandatory: UAV class */
    DroneClass_t drone_class;

    /* 005 Mandatory: GCS position type */
    PositionType_t gcs_pos_type;

    /* 006 Mandatory: GCS longitude & latitude, unit: degree
       Positive value = east longitude / north latitude */
    double gcs_lon;
    double gcs_lat;

    /* 007 Mandatory: GCS geodetic altitude, unit: meter */
    float gcs_alt;

    /* 008 Mandatory: UAV longitude & latitude, unit: degree */
    double drone_lon;
    double drone_lat;

    /* 009 Mandatory: Track angle, unit: degree, 0 <= value < 360 */
    float track_angle;

    /* 010 Mandatory: Ground speed, unit: m/s */
    float ground_speed;

    /* 011 Optional: Altitude relative to takeoff point, unit: meter */
    bool  has_rel_alt;
    float rel_alt;

    /* 012 Optional: Vertical speed, unit: m/s
       Positive = ascending, negative = descending */
    bool  has_v_speed;
    float vertical_speed;

    /* 013 Mandatory: UAV geodetic altitude, unit: meter */
    float drone_alt;

    /* 014 Optional: Barometric altitude, unit: meter */
    bool  has_pressure_alt;
    float pressure_alt;

    /* 015 Mandatory: Operation status */
    OperationStatus_t op_status;

    /* 016 Mandatory: Coordinate system */
    CoordSystem_t coord_system;

    /* 017~019: defined accuracy levels, respectively 0~12, 0~6, 0~4. */
    uint8_t horizontal_accuracy;
    uint8_t vertical_accuracy;
    uint8_t speed_accuracy;

    /* 020 Mandatory: Unix timestamp, unit: millisecond */
    uint64_t timestamp_ms;

    /* 021 Mandatory: Timestamp accuracy level, range 0~8 */
    uint8_t timestamp_accuracy;

    /* A set bit requests the field's specified unknown wire value. Physical
     * values for marked fields are ignored. Zero-initialize this structure.
     * Decode sets these bits, preserving unknown versus a real zero value.
     * Optional fields still require their has_* flag to be transmitted. */
    uint32_t unknown_fields;

} DroneRIDData_t;

/* ===================== Encode / Decode API ===================== */

/**
 * @brief  Decode GB 46750-2025 byte stream into physical value structure
 * @param  buf      [IN]  Raw byte stream
 * @param  buf_len  [IN]  Length of byte stream
 * @param  data     [OUT] Parsed RID data
 * @param  version  [OUT] Protocol sub-version, can be NULL
 * @return Error code, RID_OK on success
 */
RID_Status_t CN46750_RID_Decode(const uint8_t *buf,
                                size_t buf_len,
                                DroneRIDData_t *data,
                                uint8_t *version);

/**
 * @brief  Encode DroneRIDData_t structure into GB 46750-2025 byte stream
 * @param  data      [IN]  RID data structure to encode
 * @param  buf       [OUT] Output buffer for encoded byte stream
 * @param  buf_size  [IN]  Size of output buffer (recommend >= 128 bytes)
 * @param  out_len   [OUT] Actual encoded byte length, can be NULL
 * @return Error code, RID_OK on success
 */
RID_Status_t CN46750_RID_Encode(const DroneRIDData_t *data,
                                uint8_t *buf, size_t buf_size,
                                size_t *out_len);

/* ===================== Beacon Scanner ===================== */

/**
 * @brief  Locate a GB 46750-2025 RID packet inside raw beacon frame data
 * @param  beacon_data [IN]  Raw 802.11 beacon frame body (after radiotap header)
 * @param  beacon_len  [IN]  Length of beacon data in bytes
 * @param  rid_offset  [OUT] Byte offset where the RID packet starts (may be NULL)
 * @param  rid_len     [OUT] Total byte length of the RID packet (may be NULL)
 * @return true if a valid GB46750 packet header was found
 */
bool CN46750_FindPacket(const uint8_t *beacon_data, size_t beacon_len,
                        size_t *rid_offset, size_t *rid_len);

#ifdef __cplusplus
}
#endif

#endif // OPENDRONEID_CN46750_H
