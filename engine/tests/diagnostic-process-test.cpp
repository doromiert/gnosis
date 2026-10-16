/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <initializer_list>
#include "diagnostic-process.hpp"
#include <sys/prctl.h>
#include <sys/wait.h>
int main() {
  for (int signal : {SIGABRT, SIGSEGV}) {
    auto child = fork();
    if (child < 0) return 1;
    if (!child) {
      int dumpable = prctl(PR_GET_DUMPABLE);
      disable_diagnostic_core_dumps();
      if (prctl(PR_GET_DUMPABLE) != dumpable) _exit(1);
      raise(signal);
      _exit(2);
    }
    int status;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 128 + signal || WCOREDUMP(status)) return 1;
  }
  std::puts("Fatal diagnostic signals exit without cores and preserve portal process access: PASS");
}
