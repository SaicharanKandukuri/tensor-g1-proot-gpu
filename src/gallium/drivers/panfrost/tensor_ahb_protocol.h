#ifndef TENSOR_AHB_PROTOCOL_H
#define TENSOR_AHB_PROTOCOL_H

#include <stdint.h>

#define TENSOR_AHB_MAGIC   0x31424841u
#define TENSOR_AHB_VERSION 1u
#define TENSOR_AHB_SOCKET_DEFAULT                                              \
   "/data/data/com.termux/files/usr/tmp/tensor-ahb.sock"
#define TENSOR_AHB_DRI3_MODIFIER 1255u

enum tensor_ahb_message_type {
   TENSOR_AHB_ALLOCATE = 1,
   TENSOR_AHB_PRESENT = 2,
   TENSOR_AHB_RELEASE = 3,
   TENSOR_AHB_PING = 4,
   TENSOR_AHB_RESPONSE = 101,
};

struct tensor_ahb_message {
   uint32_t magic;
   uint16_t version;
   uint16_t type;
   int32_t status;
   uint32_t id;
   uint32_t width;
   uint32_t height;
   uint32_t format;
   uint32_t stride;
   uint32_t data_size;
   uint32_t flags;
};

_Static_assert(sizeof(struct tensor_ahb_message) == 40,
               "AHardwareBuffer bridge protocol must remain stable");

#endif
