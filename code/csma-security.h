

#ifndef CSMA_SECURITY_H_
#define CSMA_SECURITY_H_


#ifdef CSMA_CONF_LLSEC_DEFAULT_KEY0
#define CSMA_LLSEC_DEFAULT_KEY0 CSMA_CONF_LLSEC_DEFAULT_KEY0
#else
#define CSMA_LLSEC_DEFAULT_KEY0 {0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f}
#endif

#ifdef CSMA_CONF_LLSEC_SECURITY_LEVEL
#define CSMA_LLSEC_SECURITY_LEVEL   CSMA_CONF_LLSEC_SECURITY_LEVEL
#else
#define CSMA_LLSEC_SECURITY_LEVEL   5
#endif /* CSMA_CONF_LLSEC_SECURITY_LEVEL */

#ifdef CSMA_CONF_LLSEC_KEY_ID_MODE
#define CSMA_LLSEC_KEY_ID_MODE   CSMA_CONF_LLSEC_KEY_ID_MODE
#else
#define CSMA_LLSEC_KEY_ID_MODE   FRAME802154_IMPLICIT_KEY
#endif /* CSMA_CONF_LLSEC_KEY_ID_MODE */

#ifdef CSMA_CONF_LLSEC_KEY_INDEX
#define CSMA_LLSEC_KEY_INDEX   CSMA_CONF_LLSEC_KEY_INDEX
#else
#define CSMA_LLSEC_KEY_INDEX   0
#endif /* CSMA_CONF_LLSEC_KEY_INDEX */

#ifdef CSMA_CONF_LLSEC_MAXKEYS
#define CSMA_LLSEC_MAXKEYS CSMA_CONF_LLSEC_MAXKEYS
#else
#define CSMA_LLSEC_MAXKEYS 1
#endif

#endif /* CSMA_SECURITY_H_ */
