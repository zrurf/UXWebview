/*
 * Shared frame state between UXWebviewHost and the Unreal plugin.
 * Plain C, no dependency on CEF or engine headers.
 *
 * The host publishes frames into a pool of shared textures and updates
 * this header. Readers must: read Seq, read the remaining fields, read
 * Seq again, and accept the sample only when both Seq values match.
 * A mismatch only means a newer frame arrived; the next tick retries.
 */

#ifndef UXWEBVIEW_FRAME_H
#define UXWEBVIEW_FRAME_H

#include <stdint.h>

#define UXWV_FRAME_MAGIC     0x55585746u /* 'UXWF' */
#define UXWV_FRAME_VERSION   2u
#define UXWV_FRAME_POOL_SIZE 4

/** ClientHeld value for "no buffer": all bits set in both halves. */
#define UXWV_BUFFER_NONE     0xFFFFFFFFu

typedef enum UxFrameFormat
{
    UXWV_FORMAT_BGRA8 = 0,
    UXWV_FORMAT_RGBA8 = 1
} UxFrameFormat;

typedef struct UxFrameHeader
{
    uint32_t Magic;
    uint32_t Version;

    /* Latest published frame. Written before Seq is incremented. */
    uint32_t Width;
    uint32_t Height;
    uint32_t Format;       /* UxFrameFormat */
    uint32_t Generation;   /* Bumped whenever the shared handles change */
    uint32_t PoolSize;
    uint32_t BufferIndex;  /* Valid index into SharedHandles */

    /* Windows: NT handle value of each pool texture.
     * macOS: IOSurfaceID. Linux: unused (CPU path). */
    uint64_t SharedHandles[UXWV_FRAME_POOL_SIZE];

    /* Client side handshake for direct sampling (no copy). When HoldBuffers
     * is 1 the client keeps the buffers in ClientHeld on screen and the host
     * must not copy into them. ClientHeld packs the current buffer in the
     * low 32 bits and the previous one in the high 32 bits, written as one
     * 64-bit value so a reader never sees a half updated pair.
     * UXWV_BUFFER_NONE marks an empty slot. Both fields are written by the
     * client at publish time, so the host polls them before picking a buffer. */
    uint32_t HoldBuffers;
    uint32_t Reserved0;
    uint64_t ClientHeld;

    uint32_t Seq;          /* Monotonic; the only change signal */
    uint32_t Reserved;
    uint64_t PublishTicks; /* Host QPC at publish, diagnostics only */
} UxFrameHeader;

#endif /* UXWEBVIEW_FRAME_H */
