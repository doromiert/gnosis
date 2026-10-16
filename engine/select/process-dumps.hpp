/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>

inline bool prevent_memory_core_dumps() {
  // Keep /proc access available to portals while excluding Chromium's large
  // address space from systemd's piped core handler.
  int filter = open("/proc/self/coredump_filter", O_WRONLY | O_CLOEXEC);
  bool filtered = filter >= 0 && write(filter, "0", 1) == 1;
  if (filter >= 0) close(filter);
  const rlimit limit{0, 0};
  return filtered && setrlimit(RLIMIT_CORE, &limit) == 0;
}
