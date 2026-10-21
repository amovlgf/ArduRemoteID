#pragma once
#include "activation.h"
// Independent from PUBLIC_KEY1..5 (OTA and SecureCommand). No configured
// production key means deny every package. No transport no-key exception.
#if defined(RID_ACTIVATION_TEST_KEYS) && defined(RID_ACTIVATION_TRUST_HEADER)
#error "Test and production activation roots must not be combined"
#endif
#if defined(RID_ACTIVATION_TRUST_HEADER)
#include RID_ACTIVATION_TRUST_HEADER
#endif
namespace ActivationTrust {
#if defined(RID_ACTIVATION_TEST_KEYS)
// Public counterpart of the deliberately public TEST seed bytes(range(32)).
static const rid_activation::TrustKey keys[] = {{65535, {
0xf6,0x53,0x33,0xfa,0x63,0x03,0xb6,0xa2,0x3d,0xef,0xd7,0xde,0x2a,0xf8,0xaa,0x46,
0x1c,0xb0,0x47,0xcc,0xbf,0x12,0xd4,0xed,0xd2,0x9e,0xf3,0xb1,0xeb,0xa6,0x70,0x6b}}};
static constexpr size_t count=1;
static constexpr bool test_build=true;
#elif defined(RID_ACTIVATION_KEYS)
static const rid_activation::TrustKey keys[] = {RID_ACTIVATION_KEYS};
static constexpr size_t count=sizeof(keys)/sizeof(keys[0]);
static constexpr bool test_build=false;
#else
static const rid_activation::TrustKey *const keys=nullptr;
static constexpr size_t count=0;
static constexpr bool test_build=false;
#endif
}
