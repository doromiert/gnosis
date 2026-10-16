/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <cstdio>
#include <cstdlib>
#include "../select/process-dumps.hpp"
#include <initializer_list>
#include <signal.h>
#include <unistd.h>

inline void disable_diagnostic_core_dumps() {
  if (!prevent_memory_core_dumps()) {
    std::fputs("Could not disable diagnostic memory dumps; refusing to run.\n", stderr);
    std::_Exit(1);
  }
  struct sigaction action{};
  sigemptyset(&action.sa_mask);
  action.sa_handler = +[](int signal) {
    constexpr char message[] = "Diagnostic process failed; exiting without a core dump.\n";
    auto written = write(STDERR_FILENO, message, sizeof(message) - 1);
    (void)written;
    _exit(128 + signal);
  };
  for (int signal : {SIGABRT, SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGSYS}) {
    if (sigaction(signal, &action, nullptr)) {
      std::fputs("Could not install diagnostic failure handler; refusing to run.\n", stderr);
      std::_Exit(1);
    }
  }
}
