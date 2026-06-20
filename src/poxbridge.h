/* poxbridge.h — cross-process channel between a poxicle particle *producer*
 * (a Wayland client that runs its own sim, e.g. Chiguiro) and the poxicle-kwin
 * compositor effect, which draws the producer's instances on the producer's
 * window. The effect can't observe a client's internal triggers (terminal bell,
 * process spawn/exit, overscroll), so the client sims and streams ready-to-draw
 * instances; the effect only positions + draws them.
 *
 * Transport: one POSIX shared region (a memfd handed to the effect over D-Bus)
 * per producer toplevel. The producer writes each frame's PoxInstance[] into the
 * body and bumps a seqlock; the effect polls the seqlock every compositor frame
 * and copies the latest complete frame. No per-frame D-Bus — only Register /
 * Unregister (once each) and Wake (only on an idle->active edge, to un-park the
 * effect's repaint loop).
 *
 * This header is plain C and self-contained on purpose: the producer side can
 * copy it verbatim (Chiguiro's KgxParticleInstance is byte-identical to poxicle's
 * PoxInstance, so it maps the same body layout without depending on poxicle.h).
 *
 * VENDORED from poxicle-kwin/src/poxbridge.h — keep byte-for-byte in sync with
 * the effect's copy; the protocol is the contract between the two repos.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

/* D-Bus identity. The effect owns the name on the session bus; producers call
 * its methods. (KWin effects share the user session bus, so this is reachable
 * from a normal Wayland client.) */
#define POX_BRIDGE_SERVICE "org.ninez.PoxicleBridge"
#define POX_BRIDGE_PATH    "/PoxicleBridge"
#define POX_BRIDGE_IFACE   "org.ninez.PoxicleBridge"

/* 'P','O','X','1' (little-endian) — rejects a stale / wrong / zero-filled fd. */
#define POX_BRIDGE_MAGIC   0x31584F50u
#define POX_BRIDGE_VERSION 1u

/* Header at offset 0 of the shared region; the instance body follows it.
 * All fields are 32-bit so the layout is identical on both sides regardless of
 * struct-packing rules. `seq` is a seqlock written ONLY by the producer:
 *   - even & unchanged since last read  => no new frame
 *   - odd                               => producer is mid-write, skip this poll
 *   - even & advanced                   => a new complete frame is in the body
 * The producer bumps seq to odd, writes count + body, then bumps to even (with
 * release ordering); the reader re-reads seq after copying and retries if it
 * moved, so a torn frame is never drawn. */
typedef struct {
    uint32_t magic;      /* POX_BRIDGE_MAGIC                                    */
    uint32_t version;    /* POX_BRIDGE_VERSION                                  */
    uint32_t seq;        /* seqlock (see above)                                */
    uint32_t count;      /* live PoxInstance count in the body this frame      */
    uint32_t capacity;   /* PoxInstance slots the body can hold                */
    uint32_t inst_size;  /* sizeof(PoxInstance) — sanity-check vs the reader's */
    uint32_t width;      /* producer surface size, logical px (identity/HiDPI) */
    uint32_t height;
} PoxBridgeHeader;

/* Total bytes of the shared region for `capacity` instances of `inst_size`. The
 * header is 32 bytes (8 x uint32), 4-aligned, so the body — float-based, 4-byte
 * aligned PoxInstance — starts correctly aligned right after it. */
static inline uint64_t
pox_bridge_map_size (uint32_t capacity, uint32_t inst_size)
{
    return (uint64_t) sizeof (PoxBridgeHeader) + (uint64_t) capacity * inst_size;
}

/* Byte offset of instance `i` in the body. */
static inline uint64_t
pox_bridge_body_offset (uint32_t i, uint32_t inst_size)
{
    return (uint64_t) sizeof (PoxBridgeHeader) + (uint64_t) i * inst_size;
}
