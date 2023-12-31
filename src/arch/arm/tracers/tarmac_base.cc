/*
 * Copyright (c) 2017-2019 ARM Limited
 * All rights reserved
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

#include "arch/arm/tracers/tarmac_base.hh"

#include <algorithm>
#include <string>

#include "arch/arm/regs/misc.hh"
#include "cpu/reg_class.hh"
#include "cpu/static_inst.hh"
#include "cpu/thread_context.hh"

namespace gem5
{

using namespace ArmISA;

namespace trace {

TarmacBaseRecord::TarmacBaseRecord(Tick _when, ThreadContext *_thread,
                                   const StaticInstPtr _staticInst,
                                   const PCStateBase &_pc,
                                   const StaticInstPtr _macroStaticInst)
    : InstRecord(_when, _thread, _staticInst, _pc, _macroStaticInst)
{
}

TarmacBaseRecord::InstEntry::InstEntry(
    ThreadContext* thread,
    const PCStateBase &pc,
    const StaticInstPtr staticInst,
    bool predicate)
        : taken(predicate) ,
          addr(pc.instAddr()) ,
          opcode(staticInst->getEMI() & 0xffffffff),
          isetstate(pcToISetState(pc)),
          mode(MODE_USER)
{

    // Operating mode gained by reading the architectural register (CPSR)
    const CPSR cpsr = thread->readMiscRegNoEffect(MISCREG_CPSR);
    mode = (OperatingMode) (uint8_t)cpsr.mode;
}

TarmacBaseRecord::RegEntry::RegEntry(const PCStateBase &pc)
  : isetstate(pcToISetState(pc)),
    values(2, 0)
{
    // values vector is constructed with size = 2, for
    // holding Lo and Hi values.
}

TarmacBaseRecord::MemEntry::MemEntry (
    uint8_t _size,
    Addr _addr,
    uint64_t _data)
      : size(_size), addr(_addr), data(_data)
{
}

TarmacBaseRecord::ISetState
TarmacBaseRecord::pcToISetState(const PCStateBase &pc)
{
    auto &apc = pc.as<ArmISA::PCState>();
    TarmacBaseRecord::ISetState isetstate;

    if (apc.aarch64())
        isetstate = TarmacBaseRecord::ISET_A64;
    else if (!apc.thumb())
        isetstate = TarmacBaseRecord::ISET_ARM;
    else if (apc.thumb())
        isetstate = TarmacBaseRecord::ISET_THUMB;
    else
        // Unsupported state in TARMAC
        isetstate = TarmacBaseRecord::ISET_UNSUPPORTED;

    return isetstate;
}

} // namespace trace
} // namespace gem5
