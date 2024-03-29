/*
 * This file belongs to the Galois project, a C++ library for exploiting
 * parallelism. The code is being released under the terms of the 3-Clause BSD
 * License (a copy is located in LICENSE.txt at the top-level directory).
 *
 * Copyright (C) 2018, The University of Texas at Austin. All rights reserved.
 * UNIVERSITY EXPRESSLY DISCLAIMS ANY AND ALL WARRANTIES CONCERNING THIS
 * SOFTWARE AND DOCUMENTATION, INCLUDING ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR ANY PARTICULAR PURPOSE, NON-INFRINGEMENT AND WARRANTIES OF
 * PERFORMANCE, AND ANY WARRANTY THAT MIGHT OTHERWISE ARISE FROM COURSE OF
 * DEALING OR USAGE OF TRADE.  NO WARRANTY IS EITHER EXPRESS OR IMPLIED WITH
 * RESPECT TO THE USE OF THE SOFTWARE OR DOCUMENTATION. Under no circumstances
 * shall University be liable for incidental, special, indirect, direct or
 * consequential damages or loss of profits, interruption of business, or
 * related expenses which may arise from use of Software or Documentation,
 * including but not limited to those resulting from defects in Software and/or
 * Documentation, or loss or inaccuracy of data of any kind.
 */

#ifndef KATANA_LIBGALOIS_KATANA_PROFILE_H_
#define KATANA_LIBGALOIS_KATANA_PROFILE_H_

#include <cstdlib>

#ifdef KATANA_USE_VTUNE
#include "ittnotify.h"
#endif

#ifdef KATANA_USE_PAPI
extern "C" {
#include <papi.h>
#include <papiStdEventDefs.h>
}
#endif

#include "katana/Galois.h"
#include "katana/Timer.h"
#include "katana/config.h"
#include "katana/gIO.h"

namespace katana {

#ifdef KATANA_USE_VTUNE

template <typename F>
void
profileVtune(const F& func, const char* region) {
  region = region ? region : "(NULL)";

  KATANA_LOG_ASSERT(
      katana::ThreadPool::getTID() == 0,
      "profileVtune can only be called from master thread (thread 0)");

  __itt_resume();

  {
    katana::StatTimer timer(region);
    katana::TimerGuard timer_guard(timer);
    func();
  }

  __itt_pause();
}

#else

template <typename F>
void
profileVtune(const F& func, const char* region) {
  region = region ? region : "(NULL)";
  katana::gWarn("Vtune not enabled or found");

  katana::StatTimer timer(region);
  katana::TimerGuard timer_guard(timer);
  func();
}

#endif

#ifdef KATANA_USE_PAPI

namespace internal {

unsigned long papiGetTID(void);

template <typename __T = void>
void
papiInit() {
  /* Initialize the PAPI library */
  int retval = PAPI_library_init(PAPI_VER_CURRENT);

  if (retval != PAPI_VER_CURRENT && retval > 0) {
    KATANA_DIE(
        "PAPI library version mismatch: ", retval, " != ", PAPI_VER_CURRENT);
  }

  if (retval < 0) {
    KATANA_DIE("initialization error!");
  }

  if ((retval = PAPI_thread_init(&papiGetTID)) != PAPI_OK) {
    KATANA_DIE("PAPI thread init failed");
  }
}

template <typename V1, typename V2>
void
decodePapiEvents(const V1& eventNames, V2& papiEvents) {
  for (size_t i = 0; i < eventNames.size(); ++i) {
    char buf[256];
    std::strcpy(buf, eventNames[i].c_str());
    if (PAPI_event_name_to_code(buf, &papiEvents[i]) != PAPI_OK) {
      KATANA_DIE(
          "failed to recognize eventName = ", eventNames[i],
          ", event code: ", papiEvents[i]);
    }
  }
}

template <typename V1, typename V2, typename V3>
void
papiStart(V1& eventSets, V2& papiResults, V3& papiEvents) {
  katana::on_each([&](const unsigned tid, const unsigned numT) {
    if (PAPI_register_thread() != PAPI_OK) {
      KATANA_DIE("failed to register thread with PAPI");
    }

    int& eventSet = *eventSets.getLocal();

    eventSet = PAPI_NULL;
    papiResults.getLocal()->resize(papiEvents.size());

    if (PAPI_create_eventset(&eventSet) != PAPI_OK) {
      KATANA_DIE("failed to init event set");
    }
    if (PAPI_add_events(eventSet, papiEvents.data(), papiEvents.size()) !=
        PAPI_OK) {
      KATANA_DIE("failed to add events");
    }

    if (PAPI_start(eventSet) != PAPI_OK) {
      KATANA_DIE("failed to start PAPI");
    }
  });
}

template <typename V1, typename V2, typename V3>
void
papiStop(V1& eventSets, V2& papiResults, V3& eventNames, const char* region) {
  katana::on_each([&](const unsigned tid, const unsigned numT) {
    int& eventSet = *eventSets.getLocal();

    if (PAPI_stop(eventSet, papiResults.getLocal()->data()) != PAPI_OK) {
      KATANA_DIE("PAPI_stop failed");
    }

    if (PAPI_cleanup_eventset(eventSet) != PAPI_OK) {
      KATANA_DIE("PAPI_cleanup_eventset failed");
    }

    if (PAPI_destroy_eventset(&eventSet) != PAPI_OK) {
      KATANA_DIE("PAPI_destroy_eventset failed");
    }

    assert(
        eventNames.size() == papiResults.getLocal()->size() &&
        "Both vectors should be of equal length");
    for (size_t i = 0; i < eventNames.size(); ++i) {
      katana::ReportStatSum(
          region, eventNames[i], (*papiResults.getLocal())[i]);
    }

    if (PAPI_unregister_thread() != PAPI_OK) {
      KATANA_DIE("failed to un-register thread with PAPI");
    }
  });
}

template <typename C>
void
splitCSVstr(const std::string& inputStr, C& output, const char delim = ',') {
  std::stringstream ss(inputStr);

  for (std::string item; std::getline(ss, item, delim);) {
    output.push_back(item);
  }
}

}  // end namespace internal

template <typename F>
void
profilePapi(const F& func, const char* region) {
  region = region ? region : "(NULL)";

  std::string eventNamesCSV;

  if (!katana::GetEnv("KATANA_PAPI_EVENTS", &eventNamesCSV) ||
      eventNamesCSV.empty()) {
    katana::gWarn(
        "No Events specified. Set environment variable KATANA_PAPI_EVENTS");
    katana::StatTimer timer(region);
    katana::TimerGuard timer_guard(timer);
    func();
    return;
  }

  internal::papiInit();

  std::vector<std::string> eventNames;

  internal::splitCSVstr(eventNamesCSV, eventNames);

  std::vector<int> papiEvents(eventNames.size());

  internal::decodePapiEvents(eventNames, papiEvents);

  katana::PerThreadStorage<int> eventSets;
  katana::PerThreadStorage<std::vector<long_long>> papiResults;

  internal::papiStart(eventSets, papiResults, papiEvents);

  {
    katana::StatTimer timer(region);
    katana::TimerGuard timer_guard(timer);
    func();
  }

  internal::papiStop(eventSets, papiResults, eventNames, region);
}

#else

template <typename F>
void
profilePapi(const F& func, const char* region) {
  region = region ? region : "(NULL)";
  katana::gWarn("PAPI not enabled or found");

  katana::StatTimer timer(region);
  katana::TimerGuard timer_guard(timer);
  func();
}

#endif

}  // namespace katana

#endif
