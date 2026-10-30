/*
 * Control channel between the Unreal plugin and UXWebviewHost.
 * Frames never travel here; they use the shared memory described in
 * uxwebview_frame.h. Every message is a fixed UxIpcMessage followed by an
 * optional UTF-8 JSON (control/events/script) or binary (input) payload.
 *
 * Plain C so both the host (C++17) and the plugin (UE) can include it.
 */

#ifndef UXWEBVIEW_IPC_H
#define UXWEBVIEW_IPC_H

#include <stdint.h>

#define UXWV_IPC_VERSION 2u
/* A single message above this size disconnects the client on purpose. */
#define UXWV_IPC_MAX_PAYLOAD (2u * 1024u * 1024u)
/* UTF-8 bytes carried after UxInputEvent (key character / IME text). */
#define UXWV_IPC_MAX_INPUT_TEXT 256u

typedef enum UxIpcType
{
    /* Auth and version negotiation. */
    UXWV_IPC_HELLO          = 1,  /* plugin -> host: payload {"token": "..."} */
    UXWV_IPC_HELLO_ACK      = 2,  /* host -> plugin: A = protocol version */

    /* Instance lifecycle. */
    UXWV_IPC_CREATE         = 3,  /* plugin -> host: payload CreateJson */
    UXWV_IPC_CREATE_ACK     = 4,  /* host -> plugin */
    UXWV_IPC_DESTROY        = 5,  /* plugin -> host */

    /* Navigation. */
    UXWV_IPC_NAVIGATE       = 6,  /* payload {"url": "..."} */
    UXWV_IPC_LOAD_HTML      = 7,  /* payload {"html": "...", "base": "..."} */
    UXWV_IPC_RELOAD         = 8,
    UXWV_IPC_STOP           = 9,

    /* Instance properties. */
    UXWV_IPC_RESIZE         = 10, /* A = width px, B = height px, payload {"scale": float} */
    UXWV_IPC_SET_VISIBLE    = 11, /* A = 0/1 */
    UXWV_IPC_SET_FRAME_RATE = 12, /* A = frames per second, 0 disables begin frame */
    UXWV_IPC_FOCUS          = 13, /* A = 0/1 */
    UXWV_IPC_BEGIN_FRAME    = 14, /* request one external begin frame */

    /* Process lifecycle. */
    UXWV_IPC_SHUTDOWN       = 15,
    UXWV_IPC_SHUTDOWN_ACK   = 16,
    UXWV_IPC_PING           = 17,
    UXWV_IPC_PONG           = 18,

    /* Input (binary payload, see UxInputEvent). */
    UXWV_IPC_INPUT          = 19,

    /* Script execution. */
    UXWV_IPC_EXEC           = 20, /* payload {"script": "..."} */
    UXWV_IPC_EXEC_RESULT    = 21, /* payload {"success": bool, "result": "..."} */

    /* Debugging. */
    UXWV_IPC_REQUEST_DEVTOOLS = 22,
    UXWV_IPC_EVENT_DEVTOOLS   = 23,

    /* Script bridge: page -> game queries and their replies. */
    UXWV_IPC_EMIT           = 24, /* plugin -> host: payload {"event": "...", "payload": "..."} */
    UXWV_IPC_BRIDGE_RESULT  = 25, /* plugin -> host: payload {"request": n, "success": bool, "result": "..."} */

    /* Host -> plugin events. Event names are the message type. */
    UXWV_IPC_EVENT_READY      = 30, /* browser created */
    UXWV_IPC_EVENT_LOAD_START = 31,
    UXWV_IPC_EVENT_LOAD_END   = 32, /* payload {"url": "...", "code": int} */
    UXWV_IPC_EVENT_TITLE      = 33, /* payload {"title": "..."} */
    UXWV_IPC_EVENT_URL        = 34, /* payload {"url": "..."} */
    UXWV_IPC_EVENT_CRASH      = 35, /* payload {"reason": "..."} */
    UXWV_IPC_EVENT_BRIDGE     = 36  /* payload {"name": "...", "payload": "...", "request": n} */
} UxIpcType;

typedef enum UxIpcFlags
{
    UXWV_IPC_FLAG_RESPONSE = 1u << 0
} UxIpcFlags;

/* 32 bytes, little endian. Size counts the payload that follows. */
typedef struct UxIpcMessage
{
    uint32_t Size;            /* payload bytes */
    uint32_t Type;            /* UxIpcType */
    uint32_t ProtocolVersion; /* UXWV_IPC_VERSION */
    uint32_t Instance;        /* webview instance id, 0 when global */
    uint32_t RequestId;       /* echoed by responses, 0 otherwise */
    uint32_t A;               /* type specific numeric parameter */
    uint32_t B;               /* type specific numeric parameter */
    uint32_t Flags;           /* UxIpcFlags */
} UxIpcMessage;

typedef enum UxInputType
{
    UXWV_INPUT_MOUSE_MOVE  = 0,
    UXWV_INPUT_MOUSE_DOWN  = 1,
    UXWV_INPUT_MOUSE_UP    = 2,
    UXWV_INPUT_MOUSE_WHEEL = 3,
    UXWV_INPUT_KEY_DOWN    = 4,
    UXWV_INPUT_KEY_UP      = 5,
    UXWV_INPUT_CHAR        = 6
} UxInputType;

typedef enum UxInputModifier
{
    UXWV_MOD_SHIFT = 1u << 0,
    UXWV_MOD_CTRL  = 1u << 1,
    UXWV_MOD_ALT   = 1u << 2
} UxInputModifier;

/* 32 bytes; TextBytes UTF-8 bytes follow the struct inside the payload. */
typedef struct UxInputEvent
{
    uint32_t Type;       /* UxInputType */
    uint32_t Button;     /* 0 left, 1 right, 2 middle */
    uint32_t KeyCode;    /* platform key code */
    uint32_t Modifiers;  /* UxInputModifier bits */
    float X;             /* CSS pixels */
    float Y;             /* CSS pixels */
    float WheelDelta;
    uint32_t TextBytes;
} UxInputEvent;

#endif /* UXWEBVIEW_IPC_H */
