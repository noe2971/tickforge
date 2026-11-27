#!/usr/bin/env bash
# Shared helpers for the benchmark scripts (source this file).
#
#   power_state       one line: power source and Low Power Mode, e.g.
#                     "AC Power; Low Power Mode off"
#   require_mains     exits with status 3 unless the machine is on AC power
#                     (PROGRESS.md: every benchmark runs on mains power)
#   machine_table     markdown table rows describing the machine and build

power_source() {
    if [[ "$(uname)" == "Darwin" ]]; then
        pmset -g batt 2>/dev/null | head -1 | sed "s/Now drawing from //; s/'//g"
    else
        echo "n/a"
    fi
}

low_power_mode() {
    if [[ "$(uname)" == "Darwin" ]]; then
        # The authoritative flag is ProcessInfo.isLowPowerModeEnabled.
        local v
        v="$(swift -e 'import Foundation; print(ProcessInfo.processInfo.isLowPowerModeEnabled ? "on" : "off")' 2>/dev/null)"
        if [[ -z "$v" ]]; then
            v="unknown (pmset powermode=$(pmset -g | awk '/powermode/ {print $2}'))"
        fi
        echo "$v"
    else
        echo "n/a"
    fi
}

power_state() { echo "$(power_source); Low Power Mode $(low_power_mode)"; }

require_mains() {
    local src
    src="$(power_source)"
    if [[ "$(uname)" == "Darwin" && "$src" != "AC Power" ]]; then
        echo "Refusing to benchmark: power source is '$src', not 'AC Power'. Plug the laptop in." >&2
        exit 3
    fi
}

machine_table() {
    local cpu cores ram os
    if [[ "$(uname)" == "Darwin" ]]; then
        cpu="$(sysctl -n machdep.cpu.brand_string)"
        cores="$(sysctl -n hw.ncpu) logical ($(sysctl -n hw.perflevel0.physicalcpu 2>/dev/null || echo ?) performance + $(sysctl -n hw.perflevel1.physicalcpu 2>/dev/null || echo ?) efficiency)"
        ram="$(( $(sysctl -n hw.memsize) / 1024 / 1024 / 1024 )) GiB"
        os="macOS $(sw_vers -productVersion) ($(uname -m))"
    else
        cpu="$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')"
        cores="$(nproc) logical"
        ram="$(awk '/MemTotal/ {printf "%d GiB", $2/1024/1024}' /proc/meminfo)"
        os="$(uname -sr) ($(uname -m))"
    fi
    echo "| CPU | $cpu |"
    echo "| Cores | $cores |"
    echo "| RAM | $ram |"
    echo "| OS | $os |"
    echo "| Compiler | $(c++ --version | head -1) |"
    echo "| Flags | CMake Release: \`$(grep -E '^CMAKE_CXX_FLAGS_RELEASE:' build/release/CMakeCache.txt | cut -d= -f2-)\` |"
}
