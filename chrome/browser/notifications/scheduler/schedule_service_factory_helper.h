// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_NOTIFICATIONS_SCHEDULER_SCHEDULE_SERVICE_FACTORY_HELPER_H_
#define CHROME_BROWSER_NOTIFICATIONS_SCHEDULER_SCHEDULE_SERVICE_FACTORY_HELPER_H_

#include <memory>

#include "base/macros.h"

#include "components/keyed_service/core/keyed_service.h"

namespace notifications {

class NotificationBackgroundTaskScheduler;

// Creates the notification schedule service with all the embedder level
// dependencies. This layer is mainly to forbid the embedder to depend on
// notification scheduler internal code.
KeyedService* CreateNotificationScheduleService(
    std::unique_ptr<NotificationBackgroundTaskScheduler>
        background_task_scheduler);

}  // namespace notifications

#endif  // CHROME_BROWSER_NOTIFICATIONS_SCHEDULER_SCHEDULE_SERVICE_FACTORY_HELPER_H_
