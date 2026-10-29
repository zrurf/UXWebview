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
#define UXWV_FRAME_VERSION   1u
#define UXWV_FRAME_POOL_SIZE 3

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

    uint32_t Seq;          /* Monotonic; the only change signal */
    uint32_t Reserved;
    uint64_t PublishTicks; /* Host QPC at publish, diagnostics only */
} UxFrameHeader;

#endif /* UXWEBVIEW_FRAME_H */
