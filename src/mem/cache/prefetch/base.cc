/*
 * Copyright (c) 2013-2014 ARM Limited
 * All rights reserved.
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Copyright (c) 2005 The Regents of The University of Michigan
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * @file
 * Hardware Prefetcher Definition.
 */

#include "mem/cache/prefetch/base.hh"

#include <cassert>

#include "base/intmath.hh"
#include "mem/cache/base.hh"
#include "params/BasePrefetcher.hh"
#include "sim/system.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

Base::PrefetchInfo::PrefetchInfo(PacketPtr pkt, Addr addr, bool miss)
  : address(addr), pc(pkt->req->hasPC() ? pkt->req->getPC() : 0),
    requestorId(pkt->req->requestorId()), validPC(pkt->req->hasPC()),
    secure(pkt->isSecure()), size(pkt->req->getSize()), write(pkt->isWrite()),
    paddress(pkt->req->getPaddr()), cacheMiss(miss)
{
    unsigned int req_size = pkt->req->getSize();
    if (!write && miss) {
        data = nullptr;
    } else {
        data = new uint8_t[req_size];
        Addr offset = pkt->req->getPaddr() - pkt->getAddr();
        std::memcpy(data, &(pkt->getConstPtr<uint8_t>()[offset]), req_size);
    }
}

Base::PrefetchInfo::PrefetchInfo(PrefetchInfo const &pfi, Addr addr)
  : address(addr), pc(pfi.pc), requestorId(pfi.requestorId),
    validPC(pfi.validPC), secure(pfi.secure), size(pfi.size),
    write(pfi.write), paddress(pfi.paddress), cacheMiss(pfi.cacheMiss),
    data(nullptr)
{
}

Base::PrefetchInfo::PrefetchInfo(Addr addr, Addr pc, RequestorID requestorID)
  : address(addr), pc(pc), requestorId(requestorId), validPC(true),
    secure(false), size(0), write(false), paddress(0x0), cacheMiss(false),
    data(nullptr)
{
}

void
Base::PrefetchListener::notify(const PacketPtr &pkt)
{
    if (l1_req) {
        parent.notifyL1Req(pkt);
    } else if (l1_resp) {
        parent.notifyL1Resp(pkt);
    } else if (isFill) {
        parent.notifyFill(pkt);
    } else {
        parent.probeNotify(pkt, miss);
    }
}

Base::Base(const BasePrefetcherParams &p)
    : ClockedObject(p), listeners(), cache(nullptr), blkSize(p.block_size),
      lBlkSize(floorLog2(blkSize)), onMiss(p.on_miss), onRead(p.on_read),
      onWrite(p.on_write), onData(p.on_data), onInst(p.on_inst),
      requestorId(p.sys->getRequestorId(this)),
      pageBytes(p.page_bytes),
      prefetchOnAccess(p.prefetch_on_access),
      prefetchOnPfHit(p.prefetch_on_pf_hit),
      useVirtualAddresses(p.use_virtual_addresses),
      prefetchStats(this), issuedPrefetches(0),
      usefulPrefetches(0), tlb(nullptr)
{
    if (!p.stats_pc_list.empty()) {
        prefetchStats.regStatsPerPC(p.stats_pc_list);
        stats_pc_list = p.stats_pc_list;
    }
}

void
Base::setCache(BaseCache *_cache)
{
    assert(!cache);
    cache = _cache;

    // If the cache has a different block size from the system's, save it
    blkSize = cache->getBlockSize();
    lBlkSize = floorLog2(blkSize);
}

Base::StatGroup::StatGroup(statistics::Group *parent)
  : statistics::Group(parent),
    ADD_STAT(demandMshrMisses, statistics::units::Count::get(),
        "demands not covered by prefetchs"),
    ADD_STAT(demandMshrMissesPerPC, statistics::units::Count::get(),
        "demands not covered by prefetchs"),
    ADD_STAT(pfIssued, statistics::units::Count::get(),
        "number of hwpf issued"),
    ADD_STAT(pfIssuedPerPC, statistics::units::Count::get(),
        "number of hwpf issued"),
    ADD_STAT(pfUnused, statistics::units::Count::get(),
             "number of HardPF blocks evicted w/o reference"),
    ADD_STAT(pfUnusedPerPC, statistics::units::Count::get(),
             "number of HardPF blocks evicted w/o reference"),
    ADD_STAT(pfUseful, statistics::units::Count::get(),
        "number of useful prefetch"),
    ADD_STAT(pfUsefulPerPC, statistics::units::Count::get(),
        "number of useful prefetch"),
    ADD_STAT(pfUsefulButMiss, statistics::units::Count::get(),
        "number of hit on prefetch but cache block is not in an usable "
        "state"),
    ADD_STAT(accuracy, statistics::units::Count::get(),
        "accuracy of the prefetcher"),
    ADD_STAT(accuracyPerPC, statistics::units::Count::get(),
        "accuracy of the prefetcher"),
    ADD_STAT(timely_accuracy, statistics::units::Count::get(),
        "timely accuracy of the prefetcher"),
    ADD_STAT(timely_accuracy_perPC, statistics::units::Count::get(),
        "timely accuracy of the prefetcher"),
    ADD_STAT(coverage, statistics::units::Count::get(),
    "coverage brought by this prefetcher"),
    ADD_STAT(coveragePerPC, statistics::units::Count::get(),
    "coverage brought by this prefetcher"),
    ADD_STAT(pfHitInCache, statistics::units::Count::get(),
        "number of prefetches hitting in cache"),
    ADD_STAT(pfHitInCachePerPC, statistics::units::Count::get(),
        "number of prefetches hitting in cache"),
    ADD_STAT(pfHitInMSHR, statistics::units::Count::get(),
        "number of prefetches hitting in a MSHR"),
    ADD_STAT(pfHitInMSHRPerPC, statistics::units::Count::get(),
        "number of prefetches hitting in a MSHR"),
    ADD_STAT(pfHitInWB, statistics::units::Count::get(),
        "number of prefetches hit in the Write Buffer"),
    ADD_STAT(pfHitInWBPerPC, statistics::units::Count::get(),
        "number of prefetches hit in the Write Buffer"),
    ADD_STAT(pfLate, statistics::units::Count::get(),
        "number of late prefetches (hitting in cache, MSHR or WB)"),
    ADD_STAT(pfLatePerPC, statistics::units::Count::get(),
        "number of late prefetches (hitting in cache, MSHR or WB)")
{
    using namespace statistics;

    pfUnused.flags(nozero);

    timely_accuracy.flags(total);
    timely_accuracy = pfUseful / pfIssued;

    coverage.flags(total | nonan );
    coverage = pfUseful / (pfUseful + demandMshrMisses);

    pfLate = pfHitInCache + pfHitInMSHR + pfHitInWB;

    accuracy.flags(total | nonan);
    accuracy = pfUseful / (pfIssued - pfHitInCache - pfHitInMSHR - pfHitInWB);

    int max_per_pc = 32;

    demandMshrMissesPerPC
        .init(max_per_pc)
        .flags(total | nozero | nonan)
        ;
    pfIssuedPerPC
        .init(max_per_pc)
        .flags(total | nozero | nonan)
        ;
    pfUnusedPerPC
        .init(max_per_pc)
        .flags(total | nozero | nonan)
        ;
    pfUsefulPerPC
        .init(max_per_pc)
        .flags(total | nozero | nonan)
        ;
    pfHitInCachePerPC
        .init(max_per_pc)
        .flags(total | nozero | nonan)
        ;
    pfHitInMSHRPerPC
        .init(max_per_pc)
        .flags(total | nozero | nonan)
        ;
    pfHitInWBPerPC
        .init(max_per_pc)
        .flags(total | nozero | nonan)
        ;
    accuracyPerPC.flags(nozero | nonan);
    accuracyPerPC = pfUsefulPerPC / 
        (pfIssuedPerPC - pfHitInCachePerPC - pfHitInMSHRPerPC - pfHitInWBPerPC);

    timely_accuracy_perPC.flags(nozero | nonan);
    timely_accuracy_perPC = pfUsefulPerPC / pfIssuedPerPC;

    coveragePerPC.flags(nozero | nonan);
    coveragePerPC = pfUsefulPerPC / (pfUsefulPerPC + demandMshrMissesPerPC);

    pfLatePerPC.flags(total | nozero | nonan);
    pfLatePerPC = pfHitInCachePerPC + pfHitInMSHRPerPC + pfHitInWBPerPC;

}

void 
Base::StatGroup::regStatsPerPC(const std::vector<Addr> &stats_pc_list)
{
    using namespace statistics;

    int max_per_pc = 32;
    assert(stats_pc_list.size() < max_per_pc);
    
    for (int i = 0; i < stats_pc_list.size(); i++) {

        std::stringstream stream;
        stream << std::hex << stats_pc_list[i];
        std::string pc_name = stream.str();

        demandMshrMissesPerPC.subname(i, pc_name);
        pfIssuedPerPC.subname(i, pc_name);
        pfUnusedPerPC.subname(i, pc_name); 
        pfUsefulPerPC.subname(i, pc_name);
        pfHitInCachePerPC.subname(i, pc_name);
        pfHitInMSHRPerPC.subname(i, pc_name);
        pfHitInWBPerPC.subname(i, pc_name);

        accuracyPerPC.subname(i, pc_name);
        timely_accuracy_perPC.subname(i, pc_name);
        coveragePerPC.subname(i, pc_name);
        pfLatePerPC.subname(i, pc_name);
    }
}

bool
Base::observeAccess(const PacketPtr &pkt, bool miss) const
{
    bool fetch = pkt->req->isInstFetch();
    bool read = pkt->isRead();
    bool inv = pkt->isInvalidate();

    if (!miss) {
        if (prefetchOnPfHit)
            return hasBeenPrefetched(pkt->getAddr(), pkt->isSecure());
        if (!prefetchOnAccess)
            return false;
    }
    if (pkt->req->isUncacheable()) return false;
    if (fetch && !onInst) return false;
    if (!fetch && !onData) return false;
    if (!fetch && read && !onRead) return false;
    if (!fetch && !read && !onWrite) return false;
    if (!fetch && !read && inv) return false;
    if (pkt->cmd == MemCmd::CleanEvict) return false;

    if (onMiss) {
        return miss;
    }

    return true;
}

bool
Base::inCache(Addr addr, bool is_secure) const
{
    return cache->inCache(addr, is_secure);
}

bool
Base::inMissQueue(Addr addr, bool is_secure) const
{
    return cache->inMissQueue(addr, is_secure);
}

bool
Base::hasBeenPrefetched(Addr addr, bool is_secure) const
{
    return cache->hasBeenPrefetched(addr, is_secure);
}

bool
Base::samePage(Addr a, Addr b) const
{
    return roundDown(a, pageBytes) == roundDown(b, pageBytes);
}

Addr
Base::blockAddress(Addr a) const
{
    return a & ~((Addr)blkSize-1);
}

Addr
Base::blockIndex(Addr a) const
{
    return a >> lBlkSize;
}

Addr
Base::pageAddress(Addr a) const
{
    return roundDown(a, pageBytes);
}

Addr
Base::pageOffset(Addr a) const
{
    return a & (pageBytes - 1);
}

Addr
Base::pageIthBlockAddress(Addr page, uint32_t blockIndex) const
{
    return page + (blockIndex << lBlkSize);
}

void
Base::probeNotify(const PacketPtr &pkt, bool miss)
{
    // Don't notify prefetcher on SWPrefetch, cache maintenance
    // operations or for writes that we are coaslescing.
    if (pkt->cmd.isSWPrefetch()) return;
    if (pkt->req->isCacheMaintenance()) return;
    if (pkt->isWrite() && cache != nullptr && cache->coalesce()) return;
    if (!pkt->req->hasPaddr()) {
        panic("Request must have a physical address");
    }

    if (hasBeenPrefetched(pkt->getAddr(), pkt->isSecure())) {
        usefulPrefetches += 1;
        prefetchStats.pfUseful++;

        if (pkt->req->hasPC()) {
            Addr req_pc = pkt->req->getPC();
            for (int i = 0; i < stats_pc_list.size(); i++) {
                if (req_pc == stats_pc_list[i]) {
                    prefetchStats.pfUsefulPerPC[i]++;
                    break;
                }
            }
        }

        if (miss)
            // This case happens when a demand hits on a prefetched line
            // that's not in the requested coherency state.
            prefetchStats.pfUsefulButMiss++;
    }

    // Verify this access type is observed by prefetcher
    if (observeAccess(pkt, miss)) {
        if (useVirtualAddresses && pkt->req->hasVaddr()) {
            PrefetchInfo pfi(pkt, pkt->req->getVaddr(), miss);
            notify(pkt, pfi);
        } else if (!useVirtualAddresses) {
            PrefetchInfo pfi(pkt, pkt->req->getPaddr(), miss);
            notify(pkt, pfi);
        }
    }
}

void
Base::regProbeListeners()
{
    /**
     * If no probes were added by the configuration scripts, connect to the
     * parent cache using the probe "Miss". Also connect to "Hit", if the
     * cache is configured to prefetch on accesses.
     */
    if (listeners.empty() && cache != nullptr) {
        ProbeManager *pm(cache->getProbeManager());
        listeners.push_back(new PrefetchListener(*this, pm, "Miss", false,
                                                true));
        listeners.push_back(new PrefetchListener(*this, pm, "Fill", true,
                                                 false));
        listeners.push_back(new PrefetchListener(*this, pm, "Hit", false,
                                                 false));
    }
}

// void
// Base::addEventProbe(SimObject *obj, const char *name)
// {
//     ProbeManager *pm(obj->getProbeManager());
//     listeners.push_back(new PrefetchListener(*this, pm, name));
// }

void
Base::addEventProbe(SimObject *obj, const char *name,
                    bool isFill=false, bool isMiss=false, bool l1_req=false, bool l1_resp=false)
{
    ProbeManager *pm(obj->getProbeManager());
    listeners.push_back(new PrefetchListener(*this, pm, name, isFill, isMiss, l1_req, l1_resp));
}

void
Base::addTLB(BaseTLB *t)
{
    fatal_if(tlb != nullptr, "Only one TLB can be registered");
    tlb = t;
}

} // namespace prefetch
} // namespace gem5
