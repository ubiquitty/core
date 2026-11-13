// SPDX-FileCopyrightText: 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "FrameCadence.h"
#include "PAL/PAL.h"

#if defined(__APPLE__)
#include <pthread/qos.h>
#include <sys/resource.h>
#endif

namespace UbiquittyTest {

bool preparePresentationProcess()
{
#if defined(__APPLE__)
    // A headless runner can pass its process background policy to the test.
    // That policy coalesces timed wakes even at user-interactive thread QoS;
    // a foreground native view does not inherit it.
    return ::setpriority(PRIO_DARWIN_PROCESS, 0, 0) == 0;
#else
    return true;
#endif
}

bool preparePresentationThread()
{
#if defined(__APPLE__)
    // The native frontend's main thread is user-interactive. Ingest's
    // user-initiated band lets its work compete with other ingest work;
    // a display tick must run ahead of that work, as it does in the app.
    return ::pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0) == 0;
#else
    Ubiquitty::PAL::raiseCurrentThreadToInteractivePriority();
    return true;
#endif
}

}
