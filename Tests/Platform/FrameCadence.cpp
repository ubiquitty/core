// SPDX-FileCopyrightText: 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "FrameCadence.h"
#include "PAL/PAL.h"

#if defined(__APPLE__)
#include <pthread/qos.h>
#endif

namespace UbiquittyTest {

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
