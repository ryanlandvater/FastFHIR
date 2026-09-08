/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#ifndef FF_CONCURRENCY_HPP
#define FF_CONCURRENCY_HPP

#include <thread>

#if defined(__APPLE__)
#  include <sys/sysctl.h>
#elif defined(_WIN32)
#  include <windows.h>
#  include <vector>
#endif

namespace FastFHIR
{
    /// Worker count for the ingest and build pools.
    ///
    /// Returns PERFORMANCE cores, not `hardware_concurrency()`. On a
    /// heterogeneous CPU those are different numbers and the difference is not
    /// a micro-optimization: measured 2026-09-05 on an M5 Pro (6 P + 12 E),
    /// building a 64 MB Bundle through the concurrent path,
    ///
    ///     workers   wall      user CPU
    ///      1        2.12 ms   2.07 ms
    ///      6        1.10 ms   4.41 ms     <- performance-core count
    ///      8        1.53 ms   7.91 ms
    ///     18        1.79 ms   8.05 ms     <- hardware_concurrency()
    ///
    /// The optimum sits exactly on the P-core count and regresses past it:
    /// spilling onto E-cores costs roughly double the CPU for the same bytes
    /// and makes wall time WORSE than six threads. Defaulting a pool to
    /// hardware_concurrency() is therefore not "use the whole machine", it is
    /// "use the slow half too, and pay for it twice".
    ///
    /// Platform branches are adjacent here on purpose: one audit point, one
    /// contract. Every branch falls back to hardware_concurrency(), so a
    /// platform without an efficiency-class API is never worse off than
    /// before, and a return of 0 is impossible.
    inline unsigned int performance_core_count()
    {
        unsigned int fallback = std::thread::hardware_concurrency();
        if (fallback == 0)
            fallback = 4; // no introspection at all; same floor the Ingestor used

#if defined(__APPLE__)
        // perflevel0 is the fastest level on Apple silicon; the key is absent
        // on Intel Macs, where every core is the same and the fallback is right.
        int32_t value = 0;
        size_t size = sizeof(value);
        if (sysctlbyname("hw.perflevel0.logicalcpu", &value, &size, nullptr, 0) == 0 && value > 0)
            return static_cast<unsigned int>(value);
        return fallback;

#elif defined(_WIN32)
        // EfficiencyClass is a relative ranking; the HIGHEST value is the
        // performance tier. A homogeneous machine reports one class for every
        // core, so this returns the full count and matches the fallback.
        DWORD bytes = 0;
        GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bytes);
        if (bytes == 0)
            return fallback;
        std::vector<unsigned char> buffer(bytes);
        auto *info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *>(buffer.data());
        if (!GetLogicalProcessorInformationEx(RelationProcessorCore, info, &bytes))
            return fallback;

        BYTE best = 0;
        unsigned int count = 0;
        for (DWORD offset = 0; offset < bytes;)
        {
            auto *entry = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *>(buffer.data() + offset);
            if (entry->Relationship == RelationProcessorCore)
            {
                const BYTE cls = entry->Processor.EfficiencyClass;
                if (cls > best)
                {
                    best = cls;
                    count = 0;
                }
                if (cls == best)
                    ++count;
            }
            offset += entry->Size;
        }
        return count ? count : fallback;

#else
        // Linux and the rest: no portable efficiency-class query. Reading
        // cpu_capacity from sysfs would work on big.LITTLE ARM but is absent on
        // x86, so the whole-machine count stays the contract here.
        return fallback;
#endif
    }
} // namespace FastFHIR

#endif // FF_CONCURRENCY_HPP
