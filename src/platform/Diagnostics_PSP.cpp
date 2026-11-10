#include "platform/Diagnostics.h"

#ifdef PSP_PLATFORM

#include "platform/Log.h"
#include "platform/WorldLoadTrace.h"

#include <pspkernel.h>
#include <pspsysmem.h>
#include <pspthreadman.h>
#include <malloc.h>
#include <cstdio>
#include <cstring>

namespace
{

// --- heap --------------------------------------------------------------------
//
// Two different views matter on the PSP and neither is complete alone:
//
//   - sceKernelTotalFreeMemSize()/sceKernelMaxFreeMemSize() report the free
//     bytes of the user partition itself (the ~24 MB partition a MEMSIZE 0
//     game runs in). TotalFree is what an allocation of any kind competes for;
//     MaxFreeMemSize is the largest contiguous block, which catches the
//     fragmentation case where plenty of memory exists but nothing big fits.
//
//   - mallinfo() reports newlib heap accounting only. With PSP_HEAP_SIZE_KB(-1)
//     the heap is carved from the same partition, so heapUsed roughly tracks
//     what the game has actually touched, and heapFree is the slack inside the
//     carved area. The difference between sysFree and heapFree is memory the
//     partition still owes the heap.
//
// Every line naming both makes the difference between "the game is out of
// memory" and "the game fragmented itself out of memory" visible in debug.log.

unsigned long pspSysFreeKb()
{
    return (unsigned long)(sceKernelTotalFreeMemSize() / 1024u);
}

unsigned long pspMaxBlockKb()
{
    return (unsigned long)(sceKernelMaxFreeMemSize() / 1024u);
}

struct PspHeapSnapshot
{
    unsigned long sysFreeKb = 0;
    unsigned long maxBlockKb = 0;
    unsigned long heapUsedKb = 0;
    unsigned long heapFreeKb = 0;
};

PspHeapSnapshot pspHeapSnapshot()
{
    PspHeapSnapshot snapshot;
    snapshot.sysFreeKb = pspSysFreeKb();
    snapshot.maxBlockKb = pspMaxBlockKb();

    struct mallinfo mi = mallinfo();
    snapshot.heapUsedKb = (unsigned long)((mi.uordblks + mi.hblkhd) / 1024);
    snapshot.heapFreeKb = (unsigned long)(mi.fordblks / 1024);
    return snapshot;
}

// --- bad_alloc capture -------------------------------------------------------

char s_oomLines[5][120];
bool s_oomCaptured = false;

// --- watchdog ----------------------------------------------------------------
//
// The failure on real hardware is "loading animation, LED blink, console
// frozen" with no log tail, because the last buffered line never reached the
// Memory Stick. The watchdog is a kernel thread that wakes every 10 s and
// checks whether the frame loop advanced a heartbeat counter. If it has not
// moved for 30 s the watchdog writes the stall to debug.log itself: the
// WorldLoadTrace stage, elapsed time, and a full heap snapshot. If the frame
// loop is still running but wedged in engine code, the last logged stage names
// it; if the partition leaked away, the heap snapshot shows it.
//
// It uses the kernel thread API directly rather than std::thread so it works
// with any toolchain and links no pthread dependency the rest of the build
// does not have. Priority 17 sits above the game's main thread (pspsdk boots
// user_main at 0x20/32), which is what lets it run at all while the main
// thread burns CPU in a hang; it sleeps 10 s between checks, so it holds the
// CPU only for the microseconds a log line takes, and only when a stall
// triggered it.

volatile unsigned s_beat = 0;
bool s_watchdogStarted = false;
SceUID s_mainThreadId = -1;

constexpr SceUInt WATCHDOG_INTERVAL_US = 10u * 1000 * 1000;   // wake every 10 s
constexpr SceUInt WATCHDOG_ALERT_US = 30u * 1000 * 1000;      // alert after 30 s

const char* pspThreadStatusText(int status)
{
    // Bit tests, lowest bit first: RUNNING outranks WAITING only when the SDK
    // encodes both, which it does not, but this ordering is safe either way.
    if (status & PSP_THREAD_RUNNING) return "RUNNING";   // on CPU
    if (status & PSP_THREAD_WAITING) return "WAITING";   // blocked on an event/sema/wait
    if (status & PSP_THREAD_READY)   return "READY";     // runnable, waiting for its slice
    if (status & PSP_THREAD_SUSPEND) return "SUSPENDED";
    if (status & PSP_THREAD_STOPPED) return "STOPPED";
    if (status & PSP_THREAD_KILLED)  return "KILLED";
    return "?";
}

const char* pspMainThreadStatusText()
{
    SceKernelThreadInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (s_mainThreadId < 0 || sceKernelReferThreadStatus(s_mainThreadId, &info) < 0)
        return "?";
    return pspThreadStatusText(info.status);
}

int pspWatchdogEntry(SceSize /*args*/, void* /*argp*/)
{
    unsigned lastSeenBeat = s_beat;
    unsigned long long lastChangeUs = static_cast<unsigned long long>(sceKernelGetSystemTimeWide());
    bool stallLogged = false;

    for (;;)
    {
        sceKernelDelayThread(WATCHDOG_INTERVAL_US);

        const unsigned beat = s_beat;
        const unsigned long long nowUs = static_cast<unsigned long long>(sceKernelGetSystemTimeWide());
        if (beat != lastSeenBeat)
        {
            if (stallLogged)
            {
                MC_LOG_INFO("watchdog", "[watchdog] heart resumed after %.1f s stall",
                            (double)(nowUs - lastChangeUs) / 1000000.0);
            }
            lastSeenBeat = beat;
            lastChangeUs = nowUs;
            stallLogged = false;
            continue;
        }

        const unsigned long long stallUs = nowUs - lastChangeUs;
        if (stallUs < WATCHDOG_ALERT_US)
            continue;

        // One line per stall, not per wake: the Memory Stick is already the
        // slowest thing in the system, and the stall either ends or the
        // console does not give the watchdog back anyway.
        const PspHeapSnapshot snapshot = pspHeapSnapshot();
        MC_LOG_ERROR("watchdog",
                     "[watchdog] no frame progress for %.1f s: main=%s stage='%s' "
                     "loadElapsedMs=%ld sysFree=%luKB maxBlock=%luKB heapUsed=%luKB heapFree=%luKB",
                     (double)stallUs / 1000000.0,
                     pspMainThreadStatusText(),
                     WorldLoadTrace::currentStage(),
                     WorldLoadTrace::elapsedMs(),
                     snapshot.sysFreeKb, snapshot.maxBlockKb,
                     snapshot.heapUsedKb, snapshot.heapFreeKb);
        stallLogged = true;
    }

    // The PSP thread entry must return an int; the loop never exits, so this
    // return is only there to satisfy the SceKernelThreadEntry signature.
    return 0;
}

void pspStartWatchdogIfNeeded()
{
    if (s_watchdogStarted)
        return;
    s_watchdogStarted = true; // start at most once, even on failure

    s_mainThreadId = sceKernelGetThreadId();

    PspHeapSnapshot snapshot = pspHeapSnapshot();
    MC_LOG_INFO("watchdog", "[watchdog] arming: interval=10s alert=30s sysFree=%luKB maxBlock=%luKB",
                snapshot.sysFreeKb, snapshot.maxBlockKb);

    // 16 KB stack is ample for one log line's vsnprintf; every kernel-allocated
    // thread stack comes out of the same partition the game does, so it is
    // accounted for in the snapshot above.
    const SceUID threadId = sceKernelCreateThread("mc_watchdog",
                                                  pspWatchdogEntry,
                                                  17, 16 * 1024, 0, nullptr);
    if (threadId < 0)
    {
        MC_LOG_ERROR("watchdog", "[watchdog] create failed: 0x%08x - diagnostics disabled",
                     static_cast<unsigned>(threadId));
        return;
    }

    if (sceKernelStartThread(threadId, 0, nullptr) < 0)
    {
        MC_LOG_ERROR("watchdog", "[watchdog] start failed: 0x%08x - diagnostics disabled",
                     static_cast<unsigned>(threadId));
        return;
    }
}

} // namespace

const char* platformOomDiagnosticLine(int index)
{
    if (index < 0 || index > 4 || !s_oomCaptured)
        return "";
    return s_oomLines[index];
}

void platformMemoryCheckpoint(const char* tag)
{
    const PspHeapSnapshot snapshot = pspHeapSnapshot();
    MC_LOG_INFO("memory",
                "[MEM] %-26s sysFree=%luKB maxBlock=%luKB heapUsed=%luKB heapFree=%luKB",
                tag != nullptr ? tag : "(null)",
                snapshot.sysFreeKb, snapshot.maxBlockKb,
                snapshot.heapUsedKb, snapshot.heapFreeKb);
}

void platformHardwareCheckpoint(const char* tag)
{
    // No GE debugger hook exists here; this records that execution reached the
    // named hardware-sensitive site, with the heap state around it.
    MC_LOG_INFO("hardware", "[HW] %s sysFree=%luKB maxBlock=%luKB",
                tag != nullptr ? tag : "(null)",
                pspSysFreeKb(), pspMaxBlockKb());
}

long platformHeapFreeKb()
{
    return static_cast<long>(pspSysFreeKb());
}

void platformCaptureBadAlloc()
{
    const PspHeapSnapshot snapshot = pspHeapSnapshot();
    s_oomCaptured = true;
    snprintf(s_oomLines[0], sizeof(s_oomLines[0]), "stage: %s",
             WorldLoadTrace::currentStage());
    snprintf(s_oomLines[1], sizeof(s_oomLines[1]), "load: %ld ms elapsed",
             WorldLoadTrace::elapsedMs());
    snprintf(s_oomLines[2], sizeof(s_oomLines[2]), "free: %lu KB, max block %lu KB",
             snapshot.sysFreeKb, snapshot.maxBlockKb);
    snprintf(s_oomLines[3], sizeof(s_oomLines[3]), "heap used: %lu KB",
             snapshot.heapUsedKb);
    snprintf(s_oomLines[4], sizeof(s_oomLines[4]), "heap slack: %lu KB",
             snapshot.heapFreeKb);

    MC_LOG_ERROR("memory",
                 "bad_alloc: stage='%s' loadElapsedMs=%ld sysFree=%luKB maxBlock=%luKB "
                 "heapUsed=%luKB heapFree=%luKB",
                 WorldLoadTrace::currentStage(),
                 WorldLoadTrace::elapsedMs(),
                 snapshot.sysFreeKb, snapshot.maxBlockKb,
                 snapshot.heapUsedKb, snapshot.heapFreeKb);
}

void platformWatchdogHeartbeat()
{
    ++s_beat;
    pspStartWatchdogIfNeeded();
}

#endif // PSP_PLATFORM
