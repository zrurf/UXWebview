/*
 * Minimal control channel between the Unreal plugin and UXWebviewHost.
 * Frames never travel here; only lifecycle and input-independent control.
 * Every message is a fixed-size UxIpcMessage with an optional small body.
 */

#ifndef UXWEBVIEW_IPC_H
#define UXWEBVIEW_IPC_H

#include <stdint.h>

#define UXWV_IPC_VERSION 1u

typedef enum UxIpcType
{
    UXWV_IPC_HELLO      = 1,  /* plugin -> host, carries ProtocolVersion */
    UXWV_IPC_HELLO_ACK  = 2,  /* host -> plugin, carries ProtocolVersion */
    UXWV_IPC_CREATE     = 3,  /* plugin -> host: A=width, B=height */
    UXWV_IPC_RESIZE     = 4,  /* plugin -> host: A=width, B=height */
    UXWV_IPC_BEGIN_FRAME = 5, /* plugin -> host: external begin frame request */
    UXWV_IPC_SHUTDOWN   = 6,  /* plugin -> host */
    UXWV_IPC_CRASH      = 7   /* host -> plugin: Value = error code */
} UxIpcType;

typedef struct UxIpcMessage
{
    uint32_t Type;
    uint32_t ProtocolVersion;
    uint32_t A;            /* Type-specific value */
    uint32_t B;            /* Type-specific value */
    uint32_t Instance;     /* WebView instance id */
    uint32_t RequestId;    /* 0 when not a request */
    uint32_t Value;        /* Type-specific value */
    uint32_t Reserved;
} UxIpcMessage;

#endif /* UXWEBVIEW_IPC_H */
