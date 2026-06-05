/**
 * @file FF_Recovery.hpp
 * @author Ryan Landvater (ryanlandvater[at]gmail[dot]com)
 * @copyright (c) 2026 Ryan Landvater. All rights reserved.
 * @version 0.1
 * @brief FastFHIR Recovery Tags
 * @license FastFHIR Shared Source License (FF-SSL) — see LICENSE file in the project root for terms.
 *
 * This header defines the recovery tags for the FastFHIR format
 * Recovery tags are used to identify the type of data block during validation and error handling, enabling robust recovery strategies when parsing or processing FastFHIR streams.
 * The tags are organized into blocks based on their general category (e.g., core primitives, inline scalars, data types, resources, sub-elements) and are assigned unique values to facilitate efficient lookups and type identification.
 *
 * Each structure includes validation methods to ensure data integrity and recovery tags for error handling.
 * The primitives are designed for high performance and low overhead, enabling zero-copy parsing
 * and efficient serialization of FHIR resources in the FastFHIR format.
 *
 */

// ============================================================
// Convention:
//   Core Primitives   0x0000 – 0x00FF
//   Inline Scalars    0x0100 – 0x01FF
//   Data Types        0x0200 – 0x02FF
//   Resources         0x0300 – 0x03FF
//   Sub-elements      0x0400 – 0x04FF
// ============================================================
#pragma once

#include <cstdint>

// =====================================================================
// RECOVERY TAG REGISTRY
// =====================================================================
enum RECOVERY_TAG : uint16_t {
    // Undefined / Sentinel
    FF_RECOVER_UNDEFINED                  = 0x0000,

    // Core Primitives (0x0001 range)
    RECOVER_FF_HEADER                     = 0x0001,
    RECOVER_FF_STRING                     = 0x0002,
    RECOVER_FF_CODE                       = 0x0003,
    RECOVER_FF_RESOURCE                   = 0x0004,
    RECOVER_FF_CHECKSUM                   = 0x0005,
    RECOVER_FF_URL_DIRECTORY              = 0x0006, // Stream-level URL intern table
    RECOVER_FF_MODULE_REGISTRY            = 0x0007, // WASM extension module registry
    RECOVER_FF_OPAQUE_JSON                = 0x0008, // Path B passive raw-JSON extension blob
    RECOVER_FF_WASM_PAYLOAD               = 0x0009, // Path A WASM-encoded extension payload

    // --- Inline Scalars (0x0100 Block) ---
    RECOVER_FF_SCALAR_BLOCK               = 0x0100,
    RECOVER_FF_BOOL                       = 0x0101,
    RECOVER_FF_INT32                      = 0x0102,
    RECOVER_FF_UINT32                     = 0x0103,
    RECOVER_FF_INT64                      = 0x0104,
    RECOVER_FF_UINT64                     = 0x0105,
    RECOVER_FF_FLOAT64                    = 0x0106,
    RECOVER_FF_DATE                       = 0x0107, // Reserved for bit-packing
    RECOVER_FF_DATETIME                   = 0x0108, // Reserved for bit-packing
    RECOVER_FF_TIME                       = 0x0109, // Reserved for bit-packing
    RECOVER_FF_INSTANT                    = 0x010A, // Reserved for bit-packing

    // Data Types (0x0200 Block)
    RECOVER_FF_DATA_TYPE_BLOCK            = 0x0200,
    RECOVER_FF_EXTENSION                        = 0x0201,
    RECOVER_FF_CODING                           = 0x0202,
    RECOVER_FF_CODEABLECONCEPT                  = 0x0203,
    RECOVER_FF_QUANTITY                         = 0x0204,
    RECOVER_FF_IDENTIFIER                       = 0x0205,
    RECOVER_FF_AGE                              = 0x0206,
    RECOVER_FF_COUNT                            = 0x0207,
    RECOVER_FF_DISTANCE                         = 0x0208,
    RECOVER_FF_RANGE                            = 0x0209,
    RECOVER_FF_PERIOD                           = 0x020A,
    RECOVER_FF_REFERENCE                        = 0x020B,
    RECOVER_FF_META                             = 0x020C,
    RECOVER_FF_NARRATIVE                        = 0x020D,
    RECOVER_FF_ANNOTATION                       = 0x020E,
    RECOVER_FF_HUMANNAME                        = 0x020F,
    RECOVER_FF_ADDRESS                          = 0x0210,
    RECOVER_FF_CONTACTPOINT                     = 0x0211,
    RECOVER_FF_ATTACHMENT                       = 0x0212,
    RECOVER_FF_RATIO                            = 0x0213,
    RECOVER_FF_SAMPLEDDATA                      = 0x0214,
    RECOVER_FF_DURATION                         = 0x0215,
    RECOVER_FF_AVAILABILITY                     = 0x0216,
    RECOVER_FF_EXTENDEDCONTACTDETAIL            = 0x0217,
    RECOVER_FF_TIMING                           = 0x0218,
    RECOVER_FF_DOSAGE                           = 0x0219,
    RECOVER_FF_SIGNATURE                        = 0x021A,
    RECOVER_FF_CODEABLEREFERENCE                = 0x021B,
    RECOVER_FF_VIRTUALSERVICEDETAIL             = 0x021C,

    // Resources (0x0300 Block)
    // Append new resources at the end. Never reorder.
    RECOVER_FF_RESOURCE_BLOCK            = 0x0300,
    RECOVER_FF_ALLERGYINTOLERANCE               = 0x0301,
    RECOVER_FF_BUNDLE                           = 0x0302,
    RECOVER_FF_CAREPLAN                         = 0x0303,
    RECOVER_FF_CARETEAM                         = 0x0304,
    RECOVER_FF_CONDITION                        = 0x0305,
    RECOVER_FF_COVERAGE                         = 0x0306,
    RECOVER_FF_DEVICE                           = 0x0307,
    RECOVER_FF_DIAGNOSTICREPORT                 = 0x0308,
    RECOVER_FF_DOCUMENTREFERENCE                = 0x0309,
    RECOVER_FF_ENCOUNTER                        = 0x030A,
    RECOVER_FF_GOAL                             = 0x030B,
    RECOVER_FF_IMMUNIZATION                     = 0x030C,
    RECOVER_FF_LOCATION                         = 0x030D,
    RECOVER_FF_MEDICATION                       = 0x030E,
    RECOVER_FF_MEDICATIONDISPENSE               = 0x030F,
    RECOVER_FF_MEDICATIONREQUEST                = 0x0310,
    RECOVER_FF_MEDICATIONSTATEMENT              = 0x0311,
    RECOVER_FF_OBSERVATION                      = 0x0312,
    RECOVER_FF_ORGANIZATION                     = 0x0313,
    RECOVER_FF_PATIENT                          = 0x0314,
    RECOVER_FF_PRACTITIONER                     = 0x0315,
    RECOVER_FF_PRACTITIONERROLE                 = 0x0316,
    RECOVER_FF_PROCEDURE                        = 0x0317,
    RECOVER_FF_PROVENANCE                       = 0x0318,
    RECOVER_FF_QUESTIONNAIRERESPONSE            = 0x0319,
    RECOVER_FF_RELATEDPERSON                    = 0x031A,
    RECOVER_FF_SERVICEREQUEST                   = 0x031B,
    RECOVER_FF_SPECIMEN                         = 0x031C,

    // Sub-elements / BackboneElements (0x0400 Block)
    // Append new sub-elements at the end. Never reorder.
    RECOVER_FF_BACKBONE_BLOCK             = 0x0400,
    RECOVER_FF_ALLERGYINTOLERANCE_PARTICIPANT   = 0x0401,
    RECOVER_FF_ALLERGYINTOLERANCE_REACTION      = 0x0402,
    RECOVER_FF_AVAILABILITY_AVAILABLETIME       = 0x0403,
    RECOVER_FF_AVAILABILITY_NOTAVAILABLETIME    = 0x0404,
    RECOVER_FF_BUNDLE_ENTRY                     = 0x0405,
    RECOVER_FF_BUNDLE_ENTRY_REQUEST             = 0x0406,
    RECOVER_FF_BUNDLE_ENTRY_RESPONSE            = 0x0407,
    RECOVER_FF_BUNDLE_ENTRY_SEARCH              = 0x0408,
    RECOVER_FF_BUNDLE_LINK                      = 0x0409,
    RECOVER_FF_CAREPLAN_ACTIVITY                = 0x040A,
    RECOVER_FF_CAREPLAN_ACTIVITY_DETAIL         = 0x040B,
    RECOVER_FF_CARETEAM_PARTICIPANT             = 0x040C,
    RECOVER_FF_CONDITION_EVIDENCE               = 0x040D,
    RECOVER_FF_CONDITION_PARTICIPANT            = 0x040E,
    RECOVER_FF_CONDITION_STAGE                  = 0x040F,
    RECOVER_FF_COVERAGE_CLASS                   = 0x0410,
    RECOVER_FF_COVERAGE_COSTTOBENEFICIARY       = 0x0411,
    RECOVER_FF_COVERAGE_COSTTOBENEFICIARY_EXCEPTION= 0x0412,
    RECOVER_FF_COVERAGE_PAYMENTBY               = 0x0413,
    RECOVER_FF_DEVICE_CONFORMSTO                = 0x0414,
    RECOVER_FF_DEVICE_DEVICENAME                = 0x0415,
    RECOVER_FF_DEVICE_NAME                      = 0x0416,
    RECOVER_FF_DEVICE_PROPERTY                  = 0x0417,
    RECOVER_FF_DEVICE_SPECIALIZATION            = 0x0418,
    RECOVER_FF_DEVICE_UDICARRIER                = 0x0419,
    RECOVER_FF_DEVICE_VERSION                   = 0x041A,
    RECOVER_FF_DIAGNOSTICREPORT_MEDIA           = 0x041B,
    RECOVER_FF_DIAGNOSTICREPORT_SUPPORTINGINFO  = 0x041C,
    RECOVER_FF_DOCUMENTREFERENCE_ATTESTER       = 0x041D,
    RECOVER_FF_DOCUMENTREFERENCE_CONTENT        = 0x041E,
    RECOVER_FF_DOCUMENTREFERENCE_CONTENT_PROFILE= 0x041F,
    RECOVER_FF_DOCUMENTREFERENCE_CONTEXT        = 0x0420,
    RECOVER_FF_DOCUMENTREFERENCE_RELATESTO      = 0x0421,
    RECOVER_FF_DOSAGE_DOSEANDRATE               = 0x0422,
    RECOVER_FF_ENCOUNTER_ADMISSION              = 0x0423,
    RECOVER_FF_ENCOUNTER_CLASSHISTORY           = 0x0424,
    RECOVER_FF_ENCOUNTER_DIAGNOSIS              = 0x0425,
    RECOVER_FF_ENCOUNTER_HOSPITALIZATION        = 0x0426,
    RECOVER_FF_ENCOUNTER_LOCATION               = 0x0427,
    RECOVER_FF_ENCOUNTER_PARTICIPANT            = 0x0428,
    RECOVER_FF_ENCOUNTER_REASON                 = 0x0429,
    RECOVER_FF_ENCOUNTER_STATUSHISTORY          = 0x042A,
    RECOVER_FF_GOAL_TARGET                      = 0x042B,
    RECOVER_FF_IMMUNIZATION_EDUCATION           = 0x042C,
    RECOVER_FF_IMMUNIZATION_PERFORMER           = 0x042D,
    RECOVER_FF_IMMUNIZATION_PROGRAMELIGIBILITY  = 0x042E,
    RECOVER_FF_IMMUNIZATION_PROTOCOLAPPLIED     = 0x042F,
    RECOVER_FF_IMMUNIZATION_REACTION            = 0x0430,
    RECOVER_FF_LOCATION_HOURSOFOPERATION        = 0x0431,
    RECOVER_FF_LOCATION_POSITION                = 0x0432,
    RECOVER_FF_MEDICATION_BATCH                 = 0x0433,
    RECOVER_FF_MEDICATION_INGREDIENT            = 0x0434,
    RECOVER_FF_MEDICATIONDISPENSE_PERFORMER     = 0x0435,
    RECOVER_FF_MEDICATIONDISPENSE_SUBSTITUTION  = 0x0436,
    RECOVER_FF_MEDICATIONREQUEST_DISPENSEREQUEST= 0x0437,
    RECOVER_FF_MEDICATIONREQUEST_DISPENSEREQUEST_INITIALFILL= 0x0438,
    RECOVER_FF_MEDICATIONREQUEST_SUBSTITUTION   = 0x0439,
    RECOVER_FF_MEDICATIONSTATEMENT_ADHERENCE    = 0x043A,
    RECOVER_FF_OBSERVATION_COMPONENT            = 0x043B,
    RECOVER_FF_OBSERVATION_REFERENCERANGE       = 0x043C,
    RECOVER_FF_OBSERVATION_TRIGGEREDBY          = 0x043D,
    RECOVER_FF_ORGANIZATION_CONTACT             = 0x043E,
    RECOVER_FF_ORGANIZATION_QUALIFICATION       = 0x043F,
    RECOVER_FF_PATIENT_COMMUNICATION            = 0x0440,
    RECOVER_FF_PATIENT_CONTACT                  = 0x0441,
    RECOVER_FF_PATIENT_LINK                     = 0x0442,
    RECOVER_FF_PRACTITIONER_COMMUNICATION       = 0x0443,
    RECOVER_FF_PRACTITIONER_QUALIFICATION       = 0x0444,
    RECOVER_FF_PRACTITIONERROLE_AVAILABLETIME   = 0x0445,
    RECOVER_FF_PRACTITIONERROLE_NOTAVAILABLE    = 0x0446,
    RECOVER_FF_PROCEDURE_FOCALDEVICE            = 0x0447,
    RECOVER_FF_PROCEDURE_PERFORMER              = 0x0448,
    RECOVER_FF_PROVENANCE_AGENT                 = 0x0449,
    RECOVER_FF_PROVENANCE_ENTITY                = 0x044A,
    RECOVER_FF_QUESTIONNAIRERESPONSE_ITEM       = 0x044B,
    RECOVER_FF_QUESTIONNAIRERESPONSE_ITEM_ANSWER= 0x044C,
    RECOVER_FF_RELATEDPERSON_COMMUNICATION      = 0x044D,
    RECOVER_FF_SERVICEREQUEST_ORDERDETAIL       = 0x044E,
    RECOVER_FF_SERVICEREQUEST_ORDERDETAIL_PARAMETER= 0x044F,
    RECOVER_FF_SERVICEREQUEST_PATIENTINSTRUCTION= 0x0450,
    RECOVER_FF_SPECIMEN_COLLECTION              = 0x0451,
    RECOVER_FF_SPECIMEN_CONTAINER               = 0x0452,
    RECOVER_FF_SPECIMEN_FEATURE                 = 0x0453,
    RECOVER_FF_SPECIMEN_PROCESSING              = 0x0454,
    RECOVER_FF_TIMING_REPEAT                    = 0x0455,
};

constexpr uint16_t RECOVER_ARRAY_BIT = 0x8000;
constexpr uint16_t RECOVER_TYPE_MASK = 0x7FFF;
inline constexpr bool IsArrayTag(RECOVERY_TAG tag) {return(tag & RECOVER_ARRAY_BIT)!= 0;}
inline constexpr RECOVERY_TAG GetTypeFromTag(RECOVERY_TAG tag) {return static_cast<RECOVERY_TAG>(tag & RECOVER_TYPE_MASK);}
inline constexpr RECOVERY_TAG ToArrayTag(RECOVERY_TAG base_tag) {return static_cast<RECOVERY_TAG>(base_tag | RECOVER_ARRAY_BIT);}
