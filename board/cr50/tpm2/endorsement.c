/* Copyright 2016 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "TPM_Types.h"
#include "TpmBuildSwitches.h"
#include "CryptoEngine.h"
#include "CpriECC_fp.h"
#include "CpriRSA_fp.h"
#include "tpm_types.h"

#include "Global.h"
#include "Hierarchy_fp.h"
#include "InternalRoutines.h"
#include "Manufacture_fp.h"
#include "NV_Write_fp.h"
#include "NV_DefineSpace_fp.h"

#include "console.h"
#include "extension.h"
#include "flash.h"
#include "flash_config.h"
#include "flash_info.h"
#include "printf.h"
#include "registers.h"
#include "tpm_manufacture.h"
#include "tpm_registers.h"

#include "dcrypto.h"

#include <cryptoc/sha256.h>

#include <endian.h>
#include <string.h>

#define CPRINTF(format, args...) cprintf(CC_EXTENSION, format, ## args)

#define EK_CERT_NV_START_INDEX             0x01C00000
#define INFO1_EPS_SIZE                     PRIMARY_SEED_SIZE
#define INFO1_EPS_OFFSET                   FLASH_INFO_MANUFACTURE_STATE_OFFSET
#define AES256_BLOCK_CIPHER_KEY_SIZE       32

#define RO_CERTS_START_ADDR                 0x43800
#define RO_CERTS_REGION_SIZE                0x0800

enum cros_perso_component_type {
	CROS_PERSO_COMPONENT_TYPE_EPS       = 128,
	CROS_PERSO_COMPONENT_TYPE_RSA_CERT  = 129,
	CROS_PERSO_COMPONENT_TYPE_P256_CERT = 130
};

struct cros_perso_response_component_info_v0 {
	uint16_t component_size;
	uint8_t  component_type;
	uint8_t  reserved[5];
} __packed;                                             /* Size: 8B */

/* key_id: key for which this is the certificate */
/* cert_len: length of the following certificate */
/* cert: the certificate bytes */
struct cros_perso_certificate_response_v0 {
	uint8_t key_id[4];
	uint32_t cert_len;
	uint8_t cert[0];
} __packed;                                             /* Size: 8B */

/* Personalization response. */
BUILD_ASSERT(sizeof(struct cros_perso_response_component_info_v0) == 8);
BUILD_ASSERT(sizeof(struct cros_perso_certificate_response_v0) == 8);

/* This is a fixed seed (and corresponding certificates) for use in a
 * developer environment.  Use of this fixed seed will be triggered if
 * the HMAC on the certificate region (i.e. read-only certificates
 * written at manufacture) fails to verify.
 *
 * The HMAC verification failure itself only occurs in the event that
 * RO & RW are signed in a mode that does correspond to the
 * manufacture process, i.e. a PRODUCTION mode chip installed with DEV
 * signed RO/RW (or vice-versa) or a PRODUCTION signed RO and DEV
 * signed RW (or vice-versa).
 *
 * The fixed seed and its corresponding certificates are not trusted
 * by production infrastructure, and are hence useful for development
 * and testing.
 */
const uint8_t FIXED_ENDORSEMENT_SEED[PRIMARY_SEED_SIZE] = {
	0x1c, 0xb0, 0xde, 0x0e, 0x96, 0xe5, 0x58, 0xb0,
	0xad, 0x1d, 0x3a, 0x08, 0x22, 0x41, 0x7f, 0x45,
	0x37, 0xe7, 0x17, 0x42, 0x5d, 0x87, 0xc4, 0x77,
	0xf2, 0x97, 0xf8, 0xdd, 0xb9, 0xa0, 0xe5, 0x3a
};

const uint8_t FIXED_RSA_ENDORSEMENT_CERT[1007] = {
	0x30, 0x82, 0x03, 0xeb, 0x30, 0x82, 0x02, 0xd3, 0xa0, 0x03, 0x02, 0x01,
	0x02, 0x02, 0x10, 0x57, 0xd7, 0x5a, 0xbc, 0x74, 0xa8, 0x2e, 0x11, 0x9c,
	0x73, 0x70, 0x2d, 0x3e, 0x15, 0xdf, 0x4e, 0x30, 0x0d, 0x06, 0x09, 0x2a,
	0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0b, 0x05, 0x00, 0x30, 0x81,
	0x80, 0x31, 0x0b, 0x30, 0x09, 0x06, 0x03, 0x55, 0x04, 0x06, 0x13, 0x02,
	0x55, 0x53, 0x31, 0x13, 0x30, 0x11, 0x06, 0x03, 0x55, 0x04, 0x08, 0x0c,
	0x0a, 0x43, 0x61, 0x6c, 0x69, 0x66, 0x6f, 0x72, 0x6e, 0x69, 0x61, 0x31,
	0x14, 0x30, 0x12, 0x06, 0x03, 0x55, 0x04, 0x0a, 0x0c, 0x0b, 0x47, 0x6f,
	0x6f, 0x67, 0x6c, 0x65, 0x20, 0x49, 0x6e, 0x63, 0x2e, 0x31, 0x24, 0x30,
	0x22, 0x06, 0x03, 0x55, 0x04, 0x0b, 0x0c, 0x1b, 0x45, 0x6e, 0x67, 0x69,
	0x6e, 0x65, 0x65, 0x72, 0x69, 0x6e, 0x67, 0x20, 0x61, 0x6e, 0x64, 0x20,
	0x44, 0x65, 0x76, 0x65, 0x6c, 0x6f, 0x70, 0x6d, 0x65, 0x6e, 0x74, 0x31,
	0x20, 0x30, 0x1e, 0x06, 0x03, 0x55, 0x04, 0x03, 0x0c, 0x17, 0x43, 0x52,
	0x4f, 0x53, 0x20, 0x54, 0x50, 0x4d, 0x20, 0x44, 0x45, 0x56, 0x20, 0x45,
	0x4b, 0x20, 0x52, 0x4f, 0x4f, 0x54, 0x20, 0x43, 0x41, 0x30, 0x1e, 0x17,
	0x0d, 0x31, 0x36, 0x31, 0x30, 0x32, 0x30, 0x30, 0x30, 0x34, 0x39, 0x33,
	0x36, 0x5a, 0x17, 0x0d, 0x32, 0x36, 0x31, 0x30, 0x31, 0x38, 0x30, 0x30,
	0x34, 0x39, 0x33, 0x36, 0x5a, 0x30, 0x00, 0x30, 0x82, 0x01, 0x22, 0x30,
	0x0d, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01,
	0x05, 0x00, 0x03, 0x82, 0x01, 0x0f, 0x00, 0x30, 0x82, 0x01, 0x0a, 0x02,
	0x82, 0x01, 0x01, 0x00, 0xae, 0x3f, 0x7e, 0x66, 0x78, 0x26, 0x7a, 0x38,
	0x93, 0xaf, 0x9c, 0xe4, 0x2c, 0x3c, 0x9e, 0x11, 0xb7, 0xae, 0x2f, 0x71,
	0x8d, 0x4f, 0x2e, 0x3f, 0xd2, 0x35, 0x18, 0xb0, 0x27, 0x04, 0x4e, 0x04,
	0x66, 0xb2, 0x16, 0xd4, 0xa8, 0xfc, 0x51, 0x60, 0x1b, 0x05, 0x1c, 0x02,
	0xb5, 0x77, 0x1b, 0xf6, 0x40, 0xc4, 0x0e, 0x01, 0xbf, 0x70, 0xc1, 0x68,
	0x53, 0x8b, 0x20, 0x4c, 0xa3, 0x39, 0x09, 0xd4, 0x4e, 0x28, 0x7c, 0x1d,
	0xda, 0x57, 0x5c, 0x41, 0xae, 0x9b, 0xf3, 0xd5, 0xd3, 0x46, 0x12, 0x3d,
	0x43, 0xcc, 0x39, 0x29, 0x79, 0x9d, 0xe5, 0x87, 0x84, 0x22, 0x85, 0x4b,
	0x49, 0x35, 0x16, 0x4f, 0x3b, 0xdd, 0xd8, 0xaf, 0xe3, 0x99, 0xfa, 0x37,
	0xaf, 0xbd, 0xa9, 0x38, 0xb4, 0x47, 0x58, 0x1e, 0x71, 0xb2, 0x46, 0xf2,
	0x14, 0x85, 0x43, 0x12, 0x55, 0x8b, 0xc3, 0x5b, 0x78, 0x86, 0xd0, 0x0b,
	0x08, 0x87, 0x1d, 0xf7, 0x4c, 0x69, 0x47, 0x91, 0xd1, 0x16, 0x5c, 0x0e,
	0xf7, 0x0d, 0xad, 0x4a, 0x2d, 0xd8, 0x74, 0xe2, 0x89, 0xe1, 0xaf, 0xd7,
	0x54, 0xb6, 0xe0, 0x36, 0x76, 0x7b, 0xd4, 0x6d, 0x50, 0x64, 0x13, 0x5b,
	0x86, 0xa8, 0xa7, 0xee, 0xed, 0xf9, 0x50, 0x4d, 0xac, 0x1d, 0x1f, 0x9c,
	0x1b, 0x58, 0x19, 0xa5, 0x20, 0x19, 0x75, 0xb7, 0xcf, 0xf6, 0x37, 0x59,
	0x2a, 0xc7, 0x5b, 0x14, 0x51, 0xe6, 0x64, 0x70, 0xcc, 0x0e, 0x90, 0x9f,
	0xe8, 0xf3, 0xc5, 0x95, 0x41, 0x74, 0x24, 0xb4, 0x6d, 0x37, 0x4a, 0x90,
	0x17, 0x0e, 0x11, 0xea, 0xde, 0x74, 0x0e, 0x05, 0x4d, 0x1f, 0x9c, 0x11,
	0xea, 0x06, 0xbd, 0x90, 0x9a, 0x9f, 0x44, 0x55, 0x0f, 0x93, 0x82, 0x96,
	0xfc, 0x29, 0xb7, 0x26, 0x5e, 0x01, 0x25, 0x55, 0x4b, 0x80, 0xda, 0xd6,
	0x2d, 0xe0, 0xd9, 0x65, 0xcf, 0xcb, 0x7a, 0x2b, 0x02, 0x03, 0x01, 0x00,
	0x01, 0xa3, 0x81, 0xdf, 0x30, 0x81, 0xdc, 0x30, 0x0e, 0x06, 0x03, 0x55,
	0x1d, 0x0f, 0x01, 0x01, 0xff, 0x04, 0x04, 0x03, 0x02, 0x00, 0x20, 0x30,
	0x51, 0x06, 0x03, 0x55, 0x1d, 0x11, 0x01, 0x01, 0xff, 0x04, 0x47, 0x30,
	0x45, 0xa4, 0x43, 0x30, 0x41, 0x31, 0x16, 0x30, 0x14, 0x06, 0x05, 0x67,
	0x81, 0x05, 0x02, 0x01, 0x0c, 0x0b, 0x69, 0x64, 0x3a, 0x34, 0x37, 0x34,
	0x46, 0x34, 0x46, 0x34, 0x37, 0x31, 0x0f, 0x30, 0x0d, 0x06, 0x05, 0x67,
	0x81, 0x05, 0x02, 0x02, 0x0c, 0x04, 0x48, 0x31, 0x42, 0x32, 0x31, 0x16,
	0x30, 0x14, 0x06, 0x05, 0x67, 0x81, 0x05, 0x02, 0x03, 0x0c, 0x0b, 0x69,
	0x64, 0x3a, 0x30, 0x30, 0x31, 0x33, 0x30, 0x30, 0x33, 0x37, 0x30, 0x0c,
	0x06, 0x03, 0x55, 0x1d, 0x13, 0x01, 0x01, 0xff, 0x04, 0x02, 0x30, 0x00,
	0x30, 0x13, 0x06, 0x03, 0x55, 0x1d, 0x20, 0x04, 0x0c, 0x30, 0x0a, 0x30,
	0x08, 0x06, 0x06, 0x67, 0x81, 0x0c, 0x01, 0x02, 0x02, 0x30, 0x1f, 0x06,
	0x03, 0x55, 0x1d, 0x23, 0x04, 0x18, 0x30, 0x16, 0x80, 0x14, 0xd5, 0xfd,
	0x4b, 0xf1, 0xbe, 0x05, 0xfb, 0x13, 0x28, 0xe2, 0x5f, 0x39, 0xd3, 0x9d,
	0x70, 0x4a, 0x48, 0x91, 0x6b, 0xb0, 0x30, 0x10, 0x06, 0x03, 0x55, 0x1d,
	0x25, 0x04, 0x09, 0x30, 0x07, 0x06, 0x05, 0x67, 0x81, 0x05, 0x08, 0x01,
	0x30, 0x21, 0x06, 0x03, 0x55, 0x1d, 0x09, 0x04, 0x1a, 0x30, 0x18, 0x30,
	0x16, 0x06, 0x05, 0x67, 0x81, 0x05, 0x02, 0x10, 0x31, 0x0d, 0x30, 0x0b,
	0x0c, 0x03, 0x32, 0x2e, 0x30, 0x02, 0x01, 0x00, 0x02, 0x01, 0x10, 0x30,
	0x0d, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0b,
	0x05, 0x00, 0x03, 0x82, 0x01, 0x01, 0x00, 0x4c, 0x65, 0x3f, 0x58, 0x73,
	0xb6, 0x21, 0x72, 0xb3, 0x2c, 0xc3, 0x94, 0xf4, 0xb3, 0xe0, 0x74, 0xa3,
	0x2e, 0x47, 0xa7, 0x63, 0x12, 0xa3, 0x0f, 0xc5, 0x18, 0x45, 0x06, 0xab,
	0xa9, 0xba, 0x64, 0xf0, 0xeb, 0x18, 0x7c, 0xba, 0x57, 0x09, 0xd0, 0x11,
	0x60, 0x6f, 0xbd, 0x52, 0x73, 0xab, 0x39, 0x81, 0x29, 0xab, 0x78, 0x84,
	0xec, 0x00, 0xe3, 0x87, 0xec, 0xf1, 0x7d, 0x2e, 0x15, 0x3f, 0xad, 0x1b,
	0x3a, 0x3f, 0x03, 0x53, 0x91, 0xee, 0x72, 0x7a, 0x87, 0x74, 0xa8, 0x09,
	0x7d, 0x83, 0x37, 0x0d, 0x46, 0x22, 0x12, 0xf3, 0x79, 0x61, 0xaf, 0x80,
	0xf3, 0xf4, 0x76, 0x7d, 0xbd, 0xb3, 0x1f, 0x87, 0xb8, 0x66, 0xc9, 0x24,
	0x15, 0xe9, 0xc7, 0x5b, 0x19, 0xdf, 0x04, 0x0a, 0x47, 0xec, 0x88, 0x46,
	0x7f, 0x20, 0x6c, 0x4b, 0x23, 0xdb, 0x65, 0x67, 0x54, 0xde, 0x3a, 0xc3,
	0x64, 0xbb, 0x77, 0x4d, 0x6d, 0x4b, 0x1e, 0x43, 0x9a, 0x35, 0x20, 0x7e,
	0x28, 0xce, 0x4e, 0xe5, 0xb7, 0x0b, 0xae, 0xd0, 0x26, 0xc0, 0xac, 0x2f,
	0x79, 0x35, 0x71, 0xbd, 0x74, 0x68, 0x8d, 0x51, 0x6f, 0x84, 0x4d, 0xaa,
	0xca, 0x0d, 0xf0, 0xa8, 0x41, 0x5c, 0xa9, 0x6e, 0x3b, 0x70, 0x15, 0x73,
	0x8d, 0xf0, 0x70, 0xd3, 0xb3, 0x0e, 0xa7, 0x3a, 0x34, 0x12, 0xd2, 0x1e,
	0xa4, 0x18, 0x4c, 0x31, 0xee, 0x26, 0x44, 0x24, 0xe0, 0xa5, 0xca, 0x56,
	0x5d, 0x76, 0x9e, 0xf4, 0x9a, 0x6e, 0x2b, 0xd6, 0x4a, 0xe9, 0x47, 0xd9,
	0x29, 0x94, 0x2d, 0x23, 0xf7, 0xbb, 0x13, 0x0c, 0x48, 0x73, 0x93, 0xe3,
	0x49, 0xc7, 0xd8, 0xca, 0x5d, 0x63, 0xf5, 0x68, 0xb2, 0xe9, 0x1a, 0xe6,
	0x87, 0x39, 0xf8, 0x12, 0xa7, 0x5c, 0xb2, 0x6e, 0x04, 0xd0, 0x73, 0x3a,
	0x05, 0x77, 0xc0, 0x9f, 0x23, 0xa7, 0x1a, 0x71, 0x38, 0x55, 0x70
};

const uint8_t FIXED_ECC_ENDORSEMENT_CERT[804] = {
	0x30, 0x82, 0x03, 0x20, 0x30, 0x82, 0x02, 0x08, 0xa0, 0x03, 0x02, 0x01,
	0x02, 0x02, 0x10, 0x67, 0x02, 0x3f, 0x35, 0xc3, 0x17, 0xad, 0xcf, 0x0a,
	0x76, 0xed, 0x50, 0x17, 0xd8, 0x4e, 0x50, 0x30, 0x0d, 0x06, 0x09, 0x2a,
	0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0b, 0x05, 0x00, 0x30, 0x81,
	0x80, 0x31, 0x0b, 0x30, 0x09, 0x06, 0x03, 0x55, 0x04, 0x06, 0x13, 0x02,
	0x55, 0x53, 0x31, 0x13, 0x30, 0x11, 0x06, 0x03, 0x55, 0x04, 0x08, 0x0c,
	0x0a, 0x43, 0x61, 0x6c, 0x69, 0x66, 0x6f, 0x72, 0x6e, 0x69, 0x61, 0x31,
	0x14, 0x30, 0x12, 0x06, 0x03, 0x55, 0x04, 0x0a, 0x0c, 0x0b, 0x47, 0x6f,
	0x6f, 0x67, 0x6c, 0x65, 0x20, 0x49, 0x6e, 0x63, 0x2e, 0x31, 0x24, 0x30,
	0x22, 0x06, 0x03, 0x55, 0x04, 0x0b, 0x0c, 0x1b, 0x45, 0x6e, 0x67, 0x69,
	0x6e, 0x65, 0x65, 0x72, 0x69, 0x6e, 0x67, 0x20, 0x61, 0x6e, 0x64, 0x20,
	0x44, 0x65, 0x76, 0x65, 0x6c, 0x6f, 0x70, 0x6d, 0x65, 0x6e, 0x74, 0x31,
	0x20, 0x30, 0x1e, 0x06, 0x03, 0x55, 0x04, 0x03, 0x0c, 0x17, 0x43, 0x52,
	0x4f, 0x53, 0x20, 0x54, 0x50, 0x4d, 0x20, 0x44, 0x45, 0x56, 0x20, 0x45,
	0x4b, 0x20, 0x52, 0x4f, 0x4f, 0x54, 0x20, 0x43, 0x41, 0x30, 0x1e, 0x17,
	0x0d, 0x31, 0x36, 0x31, 0x30, 0x32, 0x30, 0x30, 0x30, 0x34, 0x39, 0x33,
	0x36, 0x5a, 0x17, 0x0d, 0x32, 0x36, 0x31, 0x30, 0x31, 0x38, 0x30, 0x30,
	0x34, 0x39, 0x33, 0x36, 0x5a, 0x30, 0x00, 0x30, 0x59, 0x30, 0x13, 0x06,
	0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01, 0x06, 0x08, 0x2a, 0x86,
	0x48, 0xce, 0x3d, 0x03, 0x01, 0x07, 0x03, 0x42, 0x00, 0x04, 0x6e, 0xcc,
	0xf0, 0x96, 0x69, 0x9b, 0x3f, 0xea, 0x95, 0xb7, 0xd5, 0x00, 0x27, 0x20,
	0x81, 0x8e, 0x57, 0x00, 0x6f, 0x67, 0x98, 0xce, 0x8e, 0xdf, 0xc7, 0xda,
	0xae, 0xa8, 0xa3, 0xed, 0x3e, 0x7a, 0xb3, 0x27, 0xbf, 0x92, 0xee, 0xb2,
	0xa2, 0x76, 0x81, 0xc1, 0x71, 0x4d, 0x8c, 0xa8, 0x9d, 0xfd, 0x8e, 0xd0,
	0x29, 0xb5, 0x01, 0x20, 0xec, 0x78, 0xc0, 0x17, 0x8f, 0xf6, 0xf8, 0x67,
	0x5f, 0xe8, 0xa3, 0x81, 0xdf, 0x30, 0x81, 0xdc, 0x30, 0x0e, 0x06, 0x03,
	0x55, 0x1d, 0x0f, 0x01, 0x01, 0xff, 0x04, 0x04, 0x03, 0x02, 0x00, 0x20,
	0x30, 0x51, 0x06, 0x03, 0x55, 0x1d, 0x11, 0x01, 0x01, 0xff, 0x04, 0x47,
	0x30, 0x45, 0xa4, 0x43, 0x30, 0x41, 0x31, 0x16, 0x30, 0x14, 0x06, 0x05,
	0x67, 0x81, 0x05, 0x02, 0x01, 0x0c, 0x0b, 0x69, 0x64, 0x3a, 0x34, 0x37,
	0x34, 0x46, 0x34, 0x46, 0x34, 0x37, 0x31, 0x0f, 0x30, 0x0d, 0x06, 0x05,
	0x67, 0x81, 0x05, 0x02, 0x02, 0x0c, 0x04, 0x48, 0x31, 0x42, 0x32, 0x31,
	0x16, 0x30, 0x14, 0x06, 0x05, 0x67, 0x81, 0x05, 0x02, 0x03, 0x0c, 0x0b,
	0x69, 0x64, 0x3a, 0x30, 0x30, 0x31, 0x33, 0x30, 0x30, 0x33, 0x37, 0x30,
	0x0c, 0x06, 0x03, 0x55, 0x1d, 0x13, 0x01, 0x01, 0xff, 0x04, 0x02, 0x30,
	0x00, 0x30, 0x13, 0x06, 0x03, 0x55, 0x1d, 0x20, 0x04, 0x0c, 0x30, 0x0a,
	0x30, 0x08, 0x06, 0x06, 0x67, 0x81, 0x0c, 0x01, 0x02, 0x02, 0x30, 0x1f,
	0x06, 0x03, 0x55, 0x1d, 0x23, 0x04, 0x18, 0x30, 0x16, 0x80, 0x14, 0xd5,
	0xfd, 0x4b, 0xf1, 0xbe, 0x05, 0xfb, 0x13, 0x28, 0xe2, 0x5f, 0x39, 0xd3,
	0x9d, 0x70, 0x4a, 0x48, 0x91, 0x6b, 0xb0, 0x30, 0x10, 0x06, 0x03, 0x55,
	0x1d, 0x25, 0x04, 0x09, 0x30, 0x07, 0x06, 0x05, 0x67, 0x81, 0x05, 0x08,
	0x01, 0x30, 0x21, 0x06, 0x03, 0x55, 0x1d, 0x09, 0x04, 0x1a, 0x30, 0x18,
	0x30, 0x16, 0x06, 0x05, 0x67, 0x81, 0x05, 0x02, 0x10, 0x31, 0x0d, 0x30,
	0x0b, 0x0c, 0x03, 0x32, 0x2e, 0x30, 0x02, 0x01, 0x00, 0x02, 0x01, 0x10,
	0x30, 0x0d, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01,
	0x0b, 0x05, 0x00, 0x03, 0x82, 0x01, 0x01, 0x00, 0x21, 0xab, 0x9e, 0x92,
	0x4d, 0xb0, 0x50, 0x04, 0xeb, 0x2b, 0xb6, 0xcc, 0x87, 0x8c, 0xa8, 0x27,
	0xe3, 0x5a, 0xbf, 0x03, 0x5d, 0xb1, 0x4d, 0x24, 0xda, 0xdf, 0x44, 0xdb,
	0x4a, 0x37, 0x5c, 0x3e, 0x70, 0xf3, 0x35, 0x5d, 0x26, 0x2e, 0xaa, 0x85,
	0xc6, 0xbe, 0x1c, 0x9d, 0x1e, 0x5f, 0xf6, 0x6c, 0xb8, 0x94, 0x41, 0x25,
	0x20, 0x55, 0x28, 0x53, 0x55, 0x67, 0x9a, 0xb5, 0xfb, 0x6b, 0x57, 0x09,
	0xf0, 0x5b, 0xe2, 0x66, 0xc5, 0xe8, 0xd1, 0x9e, 0xb8, 0xb7, 0xed, 0xd8,
	0x41, 0xb5, 0xbd, 0x44, 0xd9, 0x53, 0xab, 0x2d, 0x17, 0x4c, 0x73, 0x05,
	0x19, 0x2c, 0x9d, 0x18, 0x98, 0xd8, 0x55, 0xbe, 0xbd, 0xb6, 0xa5, 0xf6,
	0x5f, 0x3d, 0x70, 0x98, 0xd6, 0xd0, 0xcf, 0x1c, 0x0d, 0xc6, 0x78, 0x6d,
	0x2e, 0x9c, 0x44, 0xf6, 0x9e, 0x0a, 0x80, 0x12, 0xcd, 0x9b, 0x4b, 0x1f,
	0xbc, 0xfe, 0xe7, 0x3f, 0x45, 0x81, 0x78, 0x43, 0x40, 0xf2, 0xb0, 0x6b,
	0x2c, 0x23, 0xc8, 0xc8, 0x57, 0xc6, 0x33, 0x08, 0x3e, 0x17, 0x43, 0x16,
	0xf0, 0x3f, 0xbf, 0x24, 0x54, 0xba, 0xe6, 0x85, 0x4c, 0xc8, 0x2e, 0x7f,
	0x88, 0x41, 0x6c, 0x4e, 0x03, 0xa6, 0x35, 0x00, 0x4d, 0xdb, 0x65, 0x68,
	0x78, 0x01, 0x40, 0xc6, 0xa0, 0x95, 0xd9, 0xe9, 0x27, 0xe1, 0x90, 0x20,
	0xc8, 0xe6, 0xa7, 0x7c, 0x4d, 0x9c, 0x1c, 0x44, 0x47, 0xfe, 0x9e, 0xc9,
	0x25, 0x7a, 0x07, 0xa9, 0x86, 0x60, 0x58, 0x18, 0x1c, 0x16, 0x18, 0x7e,
	0x04, 0xd6, 0x5a, 0xb6, 0xcb, 0xb6, 0xa6, 0x0f, 0xd9, 0x42, 0xf3, 0x19,
	0x8c, 0xbe, 0x26, 0x98, 0xdd, 0x07, 0x05, 0x76, 0xc0, 0xf9, 0xa4, 0xeb,
	0x53, 0xff, 0x13, 0x27, 0x61, 0x87, 0x66, 0x99, 0x76, 0x9c, 0x5f, 0x03,
	0x52, 0x95, 0x13, 0x6e, 0xb7, 0x33, 0x1f, 0x8d, 0xc6, 0x22, 0xd8, 0xe4
};

/* Test endorsement CA root. */
static const uint32_t TEST_ENDORSEMENT_CA_RSA_N[64] = {
	0xfa3b34ed, 0x3c59ad05, 0x912d6623, 0x83302402,
	0xd43b6755, 0x5777021a, 0xaf37e9a1, 0x45c0e8ad,
	0x9728f946, 0x4391523d, 0xdf7a9164, 0x88f1a9ae,
	0x036c557e, 0x5d9df43e, 0x3e65de68, 0xe172008a,
	0x709dc81f, 0x27a75fe0, 0x3e77f89e, 0x4f400ecc,
	0x51a17dae, 0x2ff9c652, 0xd1d83cdb, 0x20d26349,
	0xbbad71dd, 0x30051b2b, 0x276b2459, 0x809bb8e1,
	0xb8737049, 0xdbe94466, 0x8287072b, 0x070ef311,
	0x6e2a26de, 0x29d69f11, 0x96463d95, 0xb4dc6950,
	0x097d4dfe, 0x1b4a88cc, 0xbd6b50c8, 0x9f7a5b34,
	0xda22c199, 0x9d1ac04b, 0x136af5e5, 0xb1a0e824,
	0x4a065b34, 0x1f67fb46, 0xa1f91ab1, 0x27bb769f,
	0xb704c992, 0xb669cbf4, 0x9299bb6c, 0xcb1b2208,
	0x2dc0d9db, 0xe1513e13, 0xc7f24923, 0xa74c6bcc,
	0xca1a9a69, 0x1b994244, 0x4f64b0d9, 0x78607fd6,
	0x486fb315, 0xa1098c31, 0x5dc50dd6, 0xcdc10874
};

/* Production endorsement CA root. */
static const uint32_t PROD_ENDORSEMENT_CA_RSA_N[64] = {
	0xeb6a07bf, 0x6cf8eca6, 0x4756e85e, 0x2fc3874c,
	0xa4c23e87, 0xc364dffe, 0x2a2ddb95, 0x2f7f0e1e,
	0xdb485bd8, 0xce8aa808, 0xe062001b, 0x187811c3,
	0x0e400462, 0xb7097a01, 0xb988152b, 0xba9d058a,
	0x814b6691, 0xc70a694f, 0x8108c7f0, 0x4c7a1f33,
	0x5cfda48e, 0xef303dbc, 0x84f5a3ea, 0x14607435,
	0xc72f1e60, 0x345d0b38, 0x0ac16927, 0xbdf903c7,
	0x11b660ed, 0x21ebfe0e, 0x8c8b303c, 0xd6eff6cb,
	0x76156bf7, 0x57735ce4, 0x8b7a87ed, 0x7a757188,
	0xd4fb3eb0, 0xc67fa05d, 0x163f0cf5, 0x69d8abf3,
	0xec105749, 0x1de78f37, 0xb885a62f, 0x81344a82,
	0x390df2b7, 0x58a7c56a, 0xa938f471, 0x506ee7d4,
	0x2ca0f2a3, 0x2aa5392c, 0x39052797, 0x199e837c,
	0x0d367b81, 0xb7bbff6f, 0x0ea99f5f, 0xfbac0d2a,
	0x7bbe018d, 0x265fc995, 0x34f73008, 0x5e2cd747,
	0x42096e33, 0x0c15f816, 0xffa7f7d2, 0xbd6f0198
};

static const struct RSA TEST_ENDORSEMENT_CA_RSA_PUB = {
	.e = RSA_F4,
	.N = {
		.dmax = sizeof(TEST_ENDORSEMENT_CA_RSA_N) / sizeof(uint32_t),
		.d = (struct access_helper *) TEST_ENDORSEMENT_CA_RSA_N,
	},
	.d = {
		.dmax = 0,
		.d = NULL,
	},
};

static const struct RSA PROD_ENDORSEMENT_CA_RSA_PUB = {
	.e = RSA_F4,
	.N = {
		.dmax = sizeof(PROD_ENDORSEMENT_CA_RSA_N) / sizeof(uint32_t),
		.d = (struct access_helper *) PROD_ENDORSEMENT_CA_RSA_N,
	},
	.d = {
		.dmax = 0,
		.d = NULL,
	},
};

static int validate_cert(
	const struct cros_perso_response_component_info_v0 *cert_info,
	const struct cros_perso_certificate_response_v0 *cert,
	const uint8_t eps[PRIMARY_SEED_SIZE])
{
	if (cert_info->component_type != CROS_PERSO_COMPONENT_TYPE_RSA_CERT &&
	    cert_info->component_type !=
	    CROS_PERSO_COMPONENT_TYPE_P256_CERT)
		return 0;  /* Invalid component type. */

	/* TODO(ngm): verify key_id against HIK/FRK0. */
	if (cert->cert_len > MAX_NV_BUFFER_SIZE)
		return 0;

	/* Verify certificate signature; accept either root CA.
	 * Getting here implies that the previous mac check on the
	 * endorsement seed passed, and that one of these two CA
	 * certificates serve as roots for the installed endorsement
	 * certificate.
	 */
	return DCRYPTO_x509_verify(cert->cert, cert->cert_len,
				&PROD_ENDORSEMENT_CA_RSA_PUB) ||
		DCRYPTO_x509_verify(cert->cert, cert->cert_len,
				&TEST_ENDORSEMENT_CA_RSA_PUB);
}

static int store_cert(enum cros_perso_component_type component_type,
		const uint8_t *cert, size_t cert_len)
{
	const uint32_t rsa_ek_nv_index = EK_CERT_NV_START_INDEX;
	const uint32_t ecc_ek_nv_index = EK_CERT_NV_START_INDEX + 1;
	uint32_t nv_index;
	NV_DefineSpace_In define_space;
	TPMA_NV space_attributes;
	NV_Write_In in;

	/* Clear up structures potentially uszed only partially. */
	memset(&define_space, 0, sizeof(define_space));
	memset(&space_attributes, 0, sizeof(space_attributes));
	memset(&in, 0, sizeof(in));

	/* Indicate that a system reset has occurred, and currently
	 * running with Platform auth.
	 */
	HierarchyStartup(SU_RESET);

	if (component_type == CROS_PERSO_COMPONENT_TYPE_RSA_CERT)
		nv_index = rsa_ek_nv_index;
	else   /* P256 certificate. */
		nv_index = ecc_ek_nv_index;

	/* EK Credential attributes specified in the "TCG PC Client
	 * Platform, TPM Profile (PTP) Specification" document.
	 */
	/* REQUIRED: Writeable under platform auth. */
	space_attributes.TPMA_NV_PPWRITE = 1;
	/* OPTIONAL: Write-once; space must be deleted to be re-written. */
	space_attributes.TPMA_NV_WRITEDEFINE = 1;
	/* REQUIRED: Space created with platform auth. */
	space_attributes.TPMA_NV_PLATFORMCREATE = 1;
	/* REQUIRED: Readable under empty password? */
	space_attributes.TPMA_NV_AUTHREAD = 1;
	/* REQUIRED: Disable dictionary attack protection. */
	space_attributes.TPMA_NV_NO_DA = 1;

	define_space.authHandle = TPM_RH_PLATFORM;
	define_space.auth.t.size = 0;
	define_space.publicInfo.t.size = sizeof(
		define_space.publicInfo.t.nvPublic);
	define_space.publicInfo.t.nvPublic.nvIndex = nv_index;
	define_space.publicInfo.t.nvPublic.nameAlg = TPM_ALG_SHA256;
	define_space.publicInfo.t.nvPublic.attributes = space_attributes;
	define_space.publicInfo.t.nvPublic.authPolicy.t.size = 0;
	define_space.publicInfo.t.nvPublic.dataSize = cert_len;

	/* Define the required space first. */
	if (TPM2_NV_DefineSpace(&define_space) != TPM_RC_SUCCESS)
		return 0;

	/* TODO(ngm): call TPM2_NV_WriteLock(nvIndex) on tpm_init();
	 * this prevents delete?
	 */

	in.nvIndex = nv_index;
	in.authHandle = TPM_RH_PLATFORM;
	in.data.t.size = cert_len;
	memcpy(in.data.t.buffer, cert, cert_len);
	in.offset = 0;

	if (TPM2_NV_Write(&in) != TPM_RC_SUCCESS)
		return 0;
	if (NvCommit())
		return 1;
	return 0;
}

static uint32_t hw_key_ladder_step(uint32_t cert)
{
	uint32_t itop;

	GREG32(KEYMGR, SHA_ITOP) = 0;  /* clear status */

	GREG32(KEYMGR, SHA_USE_CERT_INDEX) =
		(cert << GC_KEYMGR_SHA_USE_CERT_INDEX_LSB) |
		GC_KEYMGR_SHA_USE_CERT_ENABLE_MASK;

	GREG32(KEYMGR, SHA_CFG_EN) =
		GC_KEYMGR_SHA_CFG_EN_INT_EN_DONE_MASK;
	GREG32(KEYMGR, SHA_TRIG) =
		GC_KEYMGR_SHA_TRIG_TRIG_GO_MASK;

	do {
		itop = GREG32(KEYMGR, SHA_ITOP);
	} while (!itop);

	GREG32(KEYMGR, SHA_ITOP) = 0;  /* clear status */

	return !!GREG32(KEYMGR, HKEY_ERR_FLAGS);
}


#define KEYMGR_CERT_0 0
#define KEYMGR_CERT_3 3
#define KEYMGR_CERT_4 4
#define KEYMGR_CERT_5 5
#define KEYMGR_CERT_7 7
#define KEYMGR_CERT_15 15
#define KEYMGR_CERT_20 20
#define KEYMGR_CERT_25 25
#define KEYMGR_CERT_26 26

#define K_CROS_FW_MAJOR_VERSION 0
static const uint8_t k_cr50_max_fw_major_version = 254;

static int compute_frk2(uint8_t frk2[AES256_BLOCK_CIPHER_KEY_SIZE])
{
	int i;

	/* TODO(ngm): reading ITOP in hw_key_ladder_step hangs on
	 * second run of this function (i.e. install of ECC cert,
	 * which re-generates FRK2) unless the SHA engine is reset.
	 */
	GREG32(KEYMGR, SHA_TRIG) =
		GC_KEYMGR_SHA_TRIG_TRIG_RESET_MASK;

	if (hw_key_ladder_step(KEYMGR_CERT_0))
		return 0;

	/* Derive HC_PHIK --> Deposited into ISR0 */
	if (hw_key_ladder_step(KEYMGR_CERT_3))
		return 0;

	/* Cryptographically mix OBS-FBS --> Deposited into ISR1 */
	if (hw_key_ladder_step(KEYMGR_CERT_4))
		return 0;

	/* Derive HIK_RT --> Deposited into ISR0 */
	if (hw_key_ladder_step(KEYMGR_CERT_5))
		return 0;

	/* Derive BL_HIK --> Deposited into ISR0 */
	if (hw_key_ladder_step(KEYMGR_CERT_7))
		return 0;

	/* Generate FRK2 by executing certs 15, 20, 25, and 26 */
	if (hw_key_ladder_step(KEYMGR_CERT_15))
		return 0;

	if (hw_key_ladder_step(KEYMGR_CERT_20))
		return 0;

	for (i = 0; i < k_cr50_max_fw_major_version -
			K_CROS_FW_MAJOR_VERSION; i++) {
		if (hw_key_ladder_step(KEYMGR_CERT_25))
			return 0;
	}
	if (hw_key_ladder_step(KEYMGR_CERT_26))
		return 0;
	memcpy(frk2, (void *) GREG32_ADDR(KEYMGR, HKEY_FRR0),
		AES256_BLOCK_CIPHER_KEY_SIZE);
	return 1;
}

static void flash_info_read_enable(void)
{
	/* Enable R access to INFO. */
	GREG32(GLOBALSEC, FLASH_REGION7_BASE_ADDR) = FLASH_INFO_MEMORY_BASE +
		FLASH_INFO_MANUFACTURE_STATE_OFFSET;
	GREG32(GLOBALSEC, FLASH_REGION7_SIZE) =
		FLASH_INFO_MANUFACTURE_STATE_SIZE - 1;
	GREG32(GLOBALSEC, FLASH_REGION7_CTRL) =
		GC_GLOBALSEC_FLASH_REGION7_CTRL_EN_MASK |
		GC_GLOBALSEC_FLASH_REGION7_CTRL_RD_EN_MASK;
}

static void flash_info_read_disable(void)
{
	GREG32(GLOBALSEC, FLASH_REGION7_CTRL) = 0;
}

static void flash_cert_region_enable(void)
{
	/* Enable R access to CERT block. */
	GREG32(GLOBALSEC, FLASH_REGION6_BASE_ADDR) = RO_CERTS_START_ADDR;
	GREG32(GLOBALSEC, FLASH_REGION6_SIZE) =
		RO_CERTS_REGION_SIZE - 1;
	GREG32(GLOBALSEC, FLASH_REGION6_CTRL) =
		GC_GLOBALSEC_FLASH_REGION6_CTRL_EN_MASK |
		GC_GLOBALSEC_FLASH_REGION6_CTRL_RD_EN_MASK;
}

/* EPS is stored XOR'd with FRK2, so make sure that the sizes match. */
BUILD_ASSERT(AES256_BLOCK_CIPHER_KEY_SIZE == PRIMARY_SEED_SIZE);
static int get_decrypted_eps(uint8_t eps[PRIMARY_SEED_SIZE])
{
	int i;
	uint8_t frk2[AES256_BLOCK_CIPHER_KEY_SIZE];

	CPRINTF("%s: getting eps\n", __func__);
	if (!compute_frk2(frk2))
		return 0;

	/* Setup flash region mapping. */
	flash_info_read_enable();

	for (i = 0; i < INFO1_EPS_SIZE; i += sizeof(uint32_t)) {
		uint32_t word;

		if (flash_physical_info_read_word(
				INFO1_EPS_OFFSET + i, &word) != EC_SUCCESS) {
			memset(frk2, 0, sizeof(frk2));
			return 0;     /* Flash read INFO1 failed. */
		}
		memcpy(eps + i, &word, sizeof(word));
	}

	/* Remove flash region mapping. */
	flash_info_read_disable();

	/* One-time-pad decrypt EPS. */
	for (i = 0; i < PRIMARY_SEED_SIZE; i++)
		eps[i] ^= frk2[i];

	memset(frk2, 0, sizeof(frk2));
	return 1;
}

static int store_eps(const uint8_t eps[PRIMARY_SEED_SIZE])
{
	/* gp is a TPM global state structure, declared in Global.h. */
	memcpy(gp.EPSeed.t.buffer, eps, PRIMARY_SEED_SIZE);

	/* Persist the seed to flash. */
	NvWriteReserved(NV_EP_SEED, &gp.EPSeed);
	return NvCommit();
}

static void endorsement_complete(void)
{
	CPRINTF("%s(): SUCCESS\n", __func__);
}

static int install_fixed_certs(void)
{
	if (!store_eps(FIXED_ENDORSEMENT_SEED))
		return 0;

	if (!store_cert(CROS_PERSO_COMPONENT_TYPE_RSA_CERT,
				FIXED_RSA_ENDORSEMENT_CERT,
				sizeof(FIXED_RSA_ENDORSEMENT_CERT)))
		return 0;

	if (!store_cert(CROS_PERSO_COMPONENT_TYPE_P256_CERT,
				FIXED_ECC_ENDORSEMENT_CERT,
				sizeof(FIXED_ECC_ENDORSEMENT_CERT)))
		return 0;

	return 1;
}

static int handle_cert(
	const struct cros_perso_response_component_info_v0 *cert_info,
	const struct cros_perso_certificate_response_v0 *cert,
	const uint8_t *eps)
{

	/* Write RSA / P256 endorsement certificate. */
	if (!validate_cert(cert_info, cert, eps))
		return 0;

	/* TODO(ngm): verify that storage succeeded. */
	if (!store_cert(cert_info->component_type, cert->cert,
				cert->cert_len)) {
		CPRINTF("%s(): cert storage failed, type: %d\n", __func__,
			cert_info->component_type);
		return 0;  /* Internal failure. */
	}

	return 1;
}

int tpm_endorse(void)
{
	struct ro_cert_response {
		uint8_t key_id[4];
		uint32_t cert_len;
		uint8_t cert[0];
	} __packed;

	struct ro_cert {
		const struct cros_perso_response_component_info_v0 cert_info;
		const struct ro_cert_response cert_response;
	} __packed;

	/* 2-kB RO cert region is setup like so:
	 *
	 *   | struct ro_cert | rsa_cert | struct ro_cert | ecc_cert |
	 *
	 *   last 32 bytes is hmac over (2048 - 32) preceding bytes.
	 *   using hmac(eps, "RSA", 4) as key
	 */
	const uint8_t *p = (const uint8_t *) RO_CERTS_START_ADDR;
	const uint32_t *c = (const uint32_t *) RO_CERTS_START_ADDR;
	const struct ro_cert *rsa_cert;
	const struct ro_cert *ecc_cert;
	int result = 0;
	uint8_t eps[PRIMARY_SEED_SIZE];

	LITE_HMAC_CTX hmac;

	flash_cert_region_enable();

	/* First boot, certs not yet installed. */
	if (*c == 0xFFFFFFFF)
		return 0;

	if (!get_decrypted_eps(eps)) {
		CPRINTF("%s(): failed to read eps\n", __func__);
		return 0;
	}

	/* Unpack rsa cert struct. */
	rsa_cert = (const struct ro_cert *) p;
	/* Sanity check cert region contents. */
	if ((2 * sizeof(struct ro_cert)) +
		rsa_cert->cert_response.cert_len > RO_CERTS_REGION_SIZE)
		return 0;

	/* Unpack ecc cert struct. */
	ecc_cert = (const struct ro_cert *) (p + sizeof(struct ro_cert) +
					rsa_cert->cert_response.cert_len);
	/* Sanity check cert region contents. */
	if ((2 * sizeof(struct ro_cert)) +
		rsa_cert->cert_response.cert_len +
		ecc_cert->cert_response.cert_len > RO_CERTS_REGION_SIZE)
		return 0;

	/* Verify expected component types. */
	if (rsa_cert->cert_info.component_type !=
		CROS_PERSO_COMPONENT_TYPE_RSA_CERT) {
		return 0;
	}
	if (ecc_cert->cert_info.component_type !=
		CROS_PERSO_COMPONENT_TYPE_P256_CERT) {
		return 0;
	}

	do {
		/* Check cert region hmac.
		 *
		 * This will fail if we are not running w/ expected keyladder.
		 */
		DCRYPTO_HMAC_SHA256_init(&hmac, eps, sizeof(eps));
		HASH_update(&hmac.hash, "RSA", 4);
		DCRYPTO_HMAC_SHA256_init(&hmac, DCRYPTO_HMAC_final(&hmac), 32);
		HASH_update(&hmac.hash, p, RO_CERTS_REGION_SIZE - 32);
		if (memcmp(p + RO_CERTS_REGION_SIZE - 32,
				DCRYPTO_HMAC_final(&hmac), 32) != 0) {
			CPRINTF("%s: bad cert region hmac; falling back\n"
				"    to fixed endorsement\n", __func__);

			/* HMAC verification failure indicates either
			 * a manufacture fault, or mis-match in
			 * production mode and currently running
			 * firmware (e.g. PRODUCTION mode chip, now
			 * flashed with DEV mode firmware.
			 *
			 * In either case, fall back to a fixed
			 * endorsement seed, which will not be trusted
			 * by production infrastructure.
			 */
			if (!install_fixed_certs()) {
				CPRINTF("%s: failed to install fixed "
					"endorsement certs; \n"
					"    unknown endorsement state\n",
					__func__);
			}

			/* TODO(ngm): is this state considered
			 * endorsement failure?
			 */
			break;
		}

		if (!handle_cert(
				&rsa_cert->cert_info,
				(struct cros_perso_certificate_response_v0 *)
				&rsa_cert->cert_response, eps)) {
			CPRINTF("%s: Failed to process RSA cert\n", __func__);
			break;
		}
		CPRINTF("%s: RSA cert install success\n", __func__);

		if (!handle_cert(
				&ecc_cert->cert_info,
				(struct cros_perso_certificate_response_v0 *)
				&ecc_cert->cert_response, eps)) {
			CPRINTF("%s: Failed to process ECC cert\n", __func__);
			break;
		}
		CPRINTF("%s: ECC cert install success\n", __func__);

		/* Copy EPS from INFO1 to flash data region. */
		if (!store_eps(eps)) {
			CPRINTF("%s(): eps storage failed\n", __func__);
			break;
		}

		/* Mark as endorsed. */
		endorsement_complete();

		/* Chip has been marked as manufactured. */
		result = 1;
	} while (0);

	memset(eps, 0, sizeof(eps));
	return result;
}
