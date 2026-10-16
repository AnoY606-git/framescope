// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "telemetry.hpp"

#include <memory>

namespace framescope::intel {

class SysmanBackend {
  public:
    explicit SysmanBackend(bool legacy);
    ~SysmanBackend();
    SysmanBackend(const SysmanBackend&) = delete;
    SysmanBackend& operator=(const SysmanBackend&) = delete;
    Snapshot sample();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace framescope::intel
