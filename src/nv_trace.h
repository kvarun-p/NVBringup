// nv_trace.h: the kdebug trace points of NVBringup, read with tools/nvtrace next to NVMetal's os_signpost points
// (subsystem io.github.kvarun-p.nvmetal, category frames).
//
// Each point is one kernel_debug() call: class DBG_DRIVERS, subclass NV_TRACE_SUBCLASS ('N'), a code from the table
// below, and a func qualifier: DBG_FUNC_START / DBG_FUNC_END bracket an interval (same code), DBG_FUNC_NONE is an
// event. The kdebug stamp is mach_absolute_time() at the point (the clock of NVPipeTiming). kernel_debug() returns
// at once while no trace is armed, so the points stay in the code and cost a call when nothing records.
//
// Ids that tie points together: a pipe copy's start and end carry its copy id (the low 24 bits of the copy's
// semaphore payload); a flip carries its flip number (pipeFlips after the flip). NVMetal's IOSurface IDs are not
// visible in the kernel, so tools/nvtrace lines a frame up by time, not by id.
//
// code  name        func         arg1           arg2                   arg3              arg4
// 0x01  EXEC        NONE         context        seqno                  pushes            waits
// 0x10  PIPE_COPY   START        copy id        source slot            target (3 = fb)   rows
// 0x10  PIPE_COPY   END          copy id        ok (1 = copied)        copy us           flipping (1 = yes)
// 0x12  PIPE_FLIP   NONE         flip number    target buffer          lines to vblank   wait for a free buffer, ticks
// 0x20  FLIP_LATCH  START        buffer         -                      -                 -
// 0x20  FLIP_LATCH  END          how (see below) wait, ticks           latch raster line -
//
// FLIP_LATCH how: 0 timeout, 1 notifier left NOT_BEGUN, 2 armed state is the new buffer, 3 armed with no vblank seen.
// Arguments are raw uintptr_t values; unused ones are 0.

#ifndef NV_TRACE_H
#define NV_TRACE_H

#include <sys/kdebug.h>
#include <sys/kdebug_kernel.h>

#define NV_TRACE_SUBCLASS   0x4e                       // 'N'

#define NV_TRACE_EXEC       0x01
#define NV_TRACE_PIPE_COPY  0x10                       // START and END
#define NV_TRACE_PIPE_FLIP  0x12
#define NV_TRACE_FLIP_LATCH 0x20                       // START and END

#define NV_TRACE_ID(code) KDBG_EVENTID(DBG_DRIVERS, NV_TRACE_SUBCLASS, (code))

// NV_TRACE(code, func, a1, a2, a3, a4): func is DBG_FUNC_START, DBG_FUNC_END or DBG_FUNC_NONE
#define NV_TRACE(code, func, a1, a2, a3, a4)                                                              \
    kernel_debug(NV_TRACE_ID(code) | (func), (uintptr_t)(a1), (uintptr_t)(a2), (uintptr_t)(a3),          \
                 (uintptr_t)(a4), 0)

#endif /* NV_TRACE_H */
