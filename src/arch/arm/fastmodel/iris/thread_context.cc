/*
 * Copyright (c) 2020 ARM Limited
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
 * Copyright 2019 Google, Inc.
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

#include "arch/arm/fastmodel/iris/thread_context.hh"

#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "arch/arm/fastmodel/iris/cpu.hh"
#include "arch/arm/fastmodel/iris/memory_spaces.hh"
#include "arch/arm/regs/vec.hh"
#include "arch/arm/system.hh"
#include "arch/arm/utility.hh"
#include "base/logging.hh"
#include "iris/detail/IrisCppAdapter.h"
#include "iris/detail/IrisObjects.h"
#include "mem/se_translating_port_proxy.hh"
#include "mem/translating_port_proxy.hh"
#include "sim/pseudo_inst.hh"

namespace gem5
{

namespace Iris
{

void
ThreadContext::initFromIrisInstance(const ResourceMap &resources)
{
    bool enabled = false;
    call().perInstanceExecution_getState(_instId, enabled);
    _status = enabled ? Active : Suspended;

    suspend();

    call().memory_getMemorySpaces(_instId, memorySpaces);
    for (const auto &space: memorySpaces) {
        memorySpaceIds.emplace(
            Iris::CanonicalMsn(space.canonicalMsn), space.spaceId);
    }
    call().memory_getUsefulAddressTranslations(_instId, translations);

    typedef ThreadContext Self;
    iris::EventSourceInfo evSrcInfo;

    client.registerEventCallback<Self, &Self::breakpointHit>(
            this, "ec_IRIS_BREAKPOINT_HIT",
            "Handle hitting a breakpoint", "Iris::ThreadContext");
    call().event_getEventSource(_instId, evSrcInfo, "IRIS_BREAKPOINT_HIT");
    call().eventStream_create(_instId, breakpointEventStreamId,
            evSrcInfo.evSrcId, client.getInstId());

    for (auto it = bps.begin(); it != bps.end(); it++)
        installBp(it);

    client.registerEventCallback<Self, &Self::semihostingEvent>(
            this, "ec_IRIS_SEMIHOSTING_CALL_EXTENSION",
            "Handle a semihosting call", "Iris::ThreadContext");
    call().event_getEventSource(_instId, evSrcInfo,
            "IRIS_SEMIHOSTING_CALL_EXTENSION");
    call().eventStream_create(_instId, semihostingEventStreamId,
            evSrcInfo.evSrcId, client.getInstId(),
            // Set all arguments to their defaults, except syncEc which is
            // changed to true.
            nullptr, "", false, 0, nullptr, false, false, true);
}

iris::ResourceId
ThreadContext::extractResourceId(
        const ResourceMap &resources, const std::string &name)
{
    return resources.at(name).rscId;
}

void
ThreadContext::extractResourceMap(
        ResourceIds &ids, const ResourceMap &resources,
        const IdxNameMap &idx_names)
{
    for (const auto &idx_name: idx_names) {
        int idx = idx_name.first;
        const std::string &name = idx_name.second;

        if (idx >= ids.size())
            ids.resize(idx + 1, iris::IRIS_UINT64_MAX);

        ids[idx] = extractResourceId(resources, name);
    }
}

iris::MemorySpaceId
ThreadContext::getMemorySpaceId(const Iris::CanonicalMsn& msn) const
{
    auto it = memorySpaceIds.find(msn);
    return it == memorySpaceIds.end() ? iris::IRIS_UINT64_MAX : it->second;
}

void
ThreadContext::maintainStepping()
{
    Tick now = 0;

    while (true) {
        if (comInstEventQueue.empty()) {
            // Set to 0 to deactivate stepping.
            call().step_setup(_instId, 0, "instruction");
            break;
        }

        Tick next = comInstEventQueue.nextTick();
        if (!now)
            now = getCurrentInstCount();

        if (next <= now) {
            comInstEventQueue.serviceEvents(now);
            // Start over now that comInstEventQueue has likely changed.
            continue;
        }

        // Set to the number of instructions still to step through.
        Tick remaining = next - now;
        call().step_setup(_instId, remaining, "instruction");
        break;
    }
}

ThreadContext::BpInfoIt
ThreadContext::getOrAllocBp(Addr pc)
{
    auto pc_it = bps.find(pc);

    if (pc_it != bps.end())
        return pc_it;

    auto res = bps.emplace(std::make_pair(pc, new BpInfo(pc)));
    panic_if(!res.second, "Inserting breakpoint failed.");
    return res.first;
}

void
ThreadContext::installBp(BpInfoIt it)
{
    Addr pc = it->second->pc;
    const auto &space_ids = getBpSpaceIds();
    for (auto sid: space_ids) {
        BpId id;
        call().breakpoint_set_code(_instId, id, pc, sid, 0);
        it->second->ids.push_back(id);
    }
}

void
ThreadContext::uninstallBp(BpInfoIt it)
{
    for (auto id: it->second->ids)
        call().breakpoint_delete(_instId, id);
    it->second->clearIds();
}

void
ThreadContext::delBp(BpInfoIt it)
{
    panic_if(!it->second->empty(),
             "BP info still had events associated with it.");

    if (it->second->validIds())
        uninstallBp(it);

    bps.erase(it);
}

iris::IrisErrorCode
ThreadContext::instanceRegistryChanged(
        uint64_t esId, const iris::IrisValueMap &fields, uint64_t time,
        uint64_t sInstId, bool syncEc, std::string &error_message_out)
{
    const std::string &event = fields.at("EVENT").getString();
    const iris::InstanceId id = fields.at("INST_ID").getU64();
    const std::string &name = fields.at("INST_NAME").getString();

    if (name != "component." + _irisPath)
        return iris::E_ok;

    if (event == "added")
        _instId = id;
    else if (event == "removed")
        _instId = iris::IRIS_UINT64_MAX;
    else
        panic("Unrecognized event type %s", event);

    return iris::E_ok;
}

iris::IrisErrorCode
ThreadContext::phaseInitLeave(
        uint64_t esId, const iris::IrisValueMap &fields, uint64_t time,
        uint64_t sInstId, bool syncEc, std::string &error_message_out)
{
    std::vector<iris::ResourceInfo> resources;
    call().resource_getList(_instId, resources);

    std::map<iris::ResourceId, const iris::ResourceInfo *>
        idToResource;
    for (const auto &resource: resources) {
        idToResource[resource.rscId] = &resource;
    }
    ResourceMap resourceMap;
    for (const auto &resource: resources) {
        std::string name = resource.name;
        iris::ResourceId parentId = resource.parentRscId;
        while (parentId != iris::IRIS_UINT64_MAX) {
            const auto *parent = idToResource[parentId];
            name = parent->name + "." + name;
            parentId = parent->parentRscId;
        }
        resourceMap[name] = resource;
    }

    initFromIrisInstance(resourceMap);

    return iris::E_ok;
}

iris::IrisErrorCode
ThreadContext::simulationTimeEvent(
        uint64_t esId, const iris::IrisValueMap &fields, uint64_t time,
        uint64_t sInstId, bool syncEc, std::string &error_message_out)
{
    if (fields.at("RUNNING").getAsBool()) {
        // If this is just simulation time starting up, don't do anything.
        return iris::E_ok;
    }

    // Handle the breakpoint event at simulation is stopped if needed.
    if (bpAddr.has_value()) {
        auto it = getOrAllocBp(bpAddr.value());

        std::shared_ptr<BpInfo::EventList> events = it->second->events;
        auto e_it = events->begin();
        while (e_it != events->end()) {
            PCEvent *e = *e_it;
            // Advance e_it here since e might remove itself from the list.
            e_it++;
            e->process(this);
        }

        bpAddr.reset();
    }

    // If simulation time has stopped for any reason, IRIS helpfully clears
    // all stepping counters and we need to set them back. We might also need
    // to service events based on the current number of executed instructions.
    maintainStepping();

    // Restart simulation time to make sure things progress once we give
    // control back.
    call().simulationTime_run(iris::IrisInstIdSimulationEngine);

    return iris::E_ok;
}

iris::IrisErrorCode
ThreadContext::breakpointHit(
        uint64_t esId, const iris::IrisValueMap &fields, uint64_t time,
        uint64_t sInstId, bool syncEc, std::string &error_message_out)
{
    // Handle the breakpoint event later when the fastmodel simulation is
    // stopped.
    Addr pc = fields.at("PC").getU64();
    bpAddr = pc;
    return iris::E_ok;
}

iris::IrisErrorCode
ThreadContext::semihostingEvent(
        uint64_t esId, const iris::IrisValueMap &fields, uint64_t time,
        uint64_t sInstId, bool syncEc, std::string &error_message_out)
{
    if (ArmSystem::callSemihosting(this, true)) {
        // Stop execution in case an exit of the sim loop was scheduled. We
        // don't want to keep executing instructions in the mean time.
        call().perInstanceExecution_setState(_instId, false);

        // Schedule an event to resume execution right after any exit has
        // had a chance to happen.
        if (!enableAfterPseudoEvent->scheduled())
            getCpuPtr()->schedule(enableAfterPseudoEvent, curTick());

        call().semihosting_return(_instId, readIntReg(0));
    } else {
        call().semihosting_notImplemented(_instId);
    }
    return iris::E_ok;
}

ThreadContext::ThreadContext(
        gem5::BaseCPU *cpu, int id, System *system, gem5::BaseMMU *mmu,
        BaseISA *isa, iris::IrisConnectionInterface *iris_if,
        const std::string &iris_path) :
    _cpu(cpu), _threadId(id), _system(system), _mmu(mmu), _isa(isa),
    _irisPath(iris_path), vecRegs(ArmISA::NumVecRegs),
    vecPredRegs(ArmISA::NumVecPredRegs),
    comInstEventQueue("instruction-based event queue"),
    client(iris_if, "client." + iris_path)
{
    iris::InstanceInfo info;
    auto ret_code = noThrow().instanceRegistry_getInstanceInfoByName(
                info, "component." + iris_path);
    if (ret_code == iris::E_ok) {
        // The iris instance registry already new about this path.
        _instId = info.instId;
    } else {
        // This path doesn't (yet) exist. Set the ID to something invalid.
        _instId = iris::IRIS_UINT64_MAX;
    }

    typedef ThreadContext Self;
    iris::EventSourceInfo evSrcInfo;

    client.registerEventCallback<Self, &Self::instanceRegistryChanged>(
            this, "ec_IRIS_INSTANCE_REGISTRY_CHANGED",
            "Install the iris instance ID", "Iris::ThreadContext");
    call().event_getEventSource(iris::IrisInstIdGlobalInstance, evSrcInfo,
            "IRIS_INSTANCE_REGISTRY_CHANGED");
    regEventStreamId = iris::IRIS_UINT64_MAX;
    static const std::vector<std::string> fields =
        { "EVENT", "INST_ID", "INST_NAME" };
    call().eventStream_create(iris::IrisInstIdGlobalInstance, regEventStreamId,
            evSrcInfo.evSrcId, client.getInstId(), &fields);

    client.registerEventCallback<Self, &Self::phaseInitLeave>(
            this, "ec_IRIS_SIM_PHASE_INIT_LEAVE",
            "Initialize register contexts", "Iris::ThreadContext");
    call().event_getEventSource(iris::IrisInstIdSimulationEngine, evSrcInfo,
            "IRIS_SIM_PHASE_INIT_LEAVE");
    initEventStreamId = iris::IRIS_UINT64_MAX;
    call().eventStream_create(
            iris::IrisInstIdSimulationEngine, initEventStreamId,
            evSrcInfo.evSrcId, client.getInstId());

    client.registerEventCallback<Self, &Self::simulationTimeEvent>(
            this, "ec_IRIS_SIMULATION_TIME_EVENT",
            "Handle simulation time stopping for breakpoints or stepping",
            "Iris::ThreadContext");
    call().event_getEventSource(iris::IrisInstIdSimulationEngine, evSrcInfo,
            "IRIS_SIMULATION_TIME_EVENT");
    timeEventStreamId = iris::IRIS_UINT64_MAX;
    call().eventStream_create(
            iris::IrisInstIdSimulationEngine, timeEventStreamId,
            evSrcInfo.evSrcId, client.getInstId());

    breakpointEventStreamId = iris::IRIS_UINT64_MAX;
    semihostingEventStreamId = iris::IRIS_UINT64_MAX;

    auto enable_lambda = [this]{
        call().perInstanceExecution_setState(_instId, true);
    };
    enableAfterPseudoEvent = new EventFunctionWrapper(
            enable_lambda, "resume after pseudo inst",
            false, Event::Sim_Exit_Pri + 1);
}

ThreadContext::~ThreadContext()
{
    call().eventStream_destroy(
            iris::IrisInstIdSimulationEngine, initEventStreamId);
    initEventStreamId = iris::IRIS_UINT64_MAX;
    client.unregisterEventCallback("ec_IRIS_SIM_PHASE_INIT_LEAVE");

    call().eventStream_destroy(
            iris::IrisInstIdGlobalInstance, regEventStreamId);
    regEventStreamId = iris::IRIS_UINT64_MAX;
    client.unregisterEventCallback("ec_IRIS_INSTANCE_REGISTRY_CHANGED");

    call().eventStream_destroy(
            iris::IrisInstIdGlobalInstance, timeEventStreamId);
    timeEventStreamId = iris::IRIS_UINT64_MAX;
    client.unregisterEventCallback("ec_IRIS_SIMULATION_TIME_EVENT");

    if (enableAfterPseudoEvent->scheduled())
        getCpuPtr()->deschedule(enableAfterPseudoEvent);
    delete enableAfterPseudoEvent;
}

bool
ThreadContext::schedule(PCEvent *e)
{
    auto it = getOrAllocBp(e->pc());
    it->second->events->push_back(e);

    if (_instId != iris::IRIS_UINT64_MAX && !it->second->validIds())
        installBp(it);

    return true;
}

bool
ThreadContext::remove(PCEvent *e)
{
    auto it = getOrAllocBp(e->pc());
    it->second->events->remove(e);

    if (it->second->empty())
        delBp(it);

    return true;
}

void
ThreadContext::readMem(
    iris::MemorySpaceId space, Addr addr, void *p, size_t size)
{
    iris::MemoryReadResult r;
    auto err = call().memory_read(_instId, r, space, addr, 1, size);
    panic_if(err != iris::E_ok, "readMem failed.");
    std::memcpy(p, r.data.data(), size);
}

void
ThreadContext::writeMem(
    iris::MemorySpaceId space, Addr addr, const void *p, size_t size)
{
    std::vector<uint64_t> data((size + 7) / 8);
    std::memcpy(data.data(), p, size);
    iris::MemoryWriteResult r;
    auto err = call().memory_write(_instId, r, space, addr, 1, size, data);
    panic_if(err != iris::E_ok, "writeMem failed.");
}

bool
ThreadContext::translateAddress(Addr &paddr, iris::MemorySpaceId p_space,
                                Addr vaddr, iris::MemorySpaceId v_space)
{
    iris::MemoryAddressTranslationResult result;
    auto ret = noThrow().memory_translateAddress(
            _instId, result, v_space, vaddr, p_space);

    if (ret != iris::E_ok) {
        // Check if there was  a legal translation between these two spaces.
        // If so, something else went wrong.
        for (auto &trans: translations)
            if (trans.inSpaceId == v_space && trans.outSpaceId == p_space)
                return false;

        panic("No legal translation IRIS address translation found.");
    }

    if (result.address.empty())
        return false;

    if (result.address.size() > 1) {
        warn("Multiple mappings for address %#x.", vaddr);
        return false;
    }

    paddr = result.address[0];
    return true;
}

void
ThreadContext::scheduleInstCountEvent(Event *event, Tick count)
{
    Tick now = getCurrentInstCount();
    comInstEventQueue.schedule(event, count);
    if (count <= now)
        call().simulationTime_stop(iris::IrisInstIdSimulationEngine);
    else
        maintainStepping();
}

void
ThreadContext::descheduleInstCountEvent(Event *event)
{
    comInstEventQueue.deschedule(event);
    maintainStepping();
}

Tick
ThreadContext::getCurrentInstCount()
{
    uint64_t count;
    auto ret = call().step_getStepCounterValue(_instId, count, "instruction");
    panic_if(ret != iris::E_ok, "Failed to get instruction count.");
    return count;
}

void
ThreadContext::sendFunctional(PacketPtr pkt)
{
    auto msn = ArmISA::isSecure(this) ?
        Iris::PhysicalMemorySecureMsn : Iris::PhysicalMemoryNonSecureMsn;
    auto id = getMemorySpaceId(msn);

    auto addr = pkt->getAddr();
    auto size = pkt->getSize();
    auto data = pkt->getPtr<uint8_t>();

    pkt->makeResponse();
    if (pkt->isRead())
        readMem(id, addr, data, size);
    else
        writeMem(id, addr, data, size);
}

void
ThreadContext::readMemWithCurrentMsn(Addr vaddr, size_t size, char *data)
{
    readMem(getMemorySpaceId(Iris::CurrentMsn), vaddr, data, size);
}

void
ThreadContext::writeMemWithCurrentMsn(Addr vaddr, size_t size,
                                      const char *data)
{
    writeMem(getMemorySpaceId(Iris::CurrentMsn), vaddr, data, size);
}

ThreadContext::Status
ThreadContext::status() const
{
    return _status;
}

void
ThreadContext::setStatus(Status new_status)
{
    if (enableAfterPseudoEvent->scheduled())
        getCpuPtr()->deschedule(enableAfterPseudoEvent);
    if (new_status == Active) {
        if (_status != Active)
            call().perInstanceExecution_setState(_instId, true);
    } else {
        if (_status == Active)
            call().perInstanceExecution_setState(_instId, false);
    }
    _status = new_status;
}

const PCStateBase &
ThreadContext::pcState() const
{
    ArmISA::CPSR cpsr = readMiscRegNoEffect(ArmISA::MISCREG_CPSR);

    pc.thumb(cpsr.t);
    pc.nextThumb(pc.thumb());
    pc.aarch64(!cpsr.width);
    pc.nextAArch64(!cpsr.width);
    pc.illegalExec(false);
    pc.itstate(ArmISA::itState(cpsr));
    pc.nextItstate(0);

    iris::ResourceReadResult result;
    call().resource_read(_instId, result, pcRscId);
    Addr addr = result.data.at(0);
    if (cpsr.width && cpsr.t)
        addr = addr & ~0x1;
    pc.set(addr);

    return pc;
}
void
ThreadContext::pcState(const PCStateBase &val)
{
    Addr pc = val.instAddr();

    ArmISA::CPSR cpsr = readMiscRegNoEffect(ArmISA::MISCREG_CPSR);
    if (cpsr.width && cpsr.t)
        pc = pc | 0x1;

    iris::ResourceWriteResult result;
    call().resource_write(_instId, result, pcRscId, pc);
}

iris::ResourceId
ThreadContext::getMiscRegRscId(RegIndex misc_reg) const
{
    iris::ResourceId rsc_id = iris::IRIS_UINT64_MAX;
    if (misc_reg < miscRegIds.size())
        rsc_id = miscRegIds.at(misc_reg);

    panic_if(rsc_id == iris::IRIS_UINT64_MAX,
             "Misc reg %s is not supported by fast model.",
             ArmISA::miscRegClass[misc_reg]);
    return rsc_id;
}

RegVal
ThreadContext::readMiscRegNoEffect(RegIndex misc_reg) const
{
    iris::ResourceReadResult result;
    call().resource_read(_instId, result, getMiscRegRscId(misc_reg));
    return result.data.at(0);
}

void
ThreadContext::setMiscRegNoEffect(RegIndex misc_reg, const RegVal val)
{
    iris::ResourceWriteResult result;
    call().resource_write(_instId, result, getMiscRegRscId(misc_reg), val);
}

RegVal
ThreadContext::getReg(const RegId &reg) const
{
    RegVal val;
    getReg(reg, &val);
    return val;
}

void
ThreadContext::setReg(const RegId &reg, RegVal val)
{
    setReg(reg, &val);
}

void
ThreadContext::getReg(const RegId &reg, void *val) const
{
    const RegIndex idx = reg.index();
    const bool flat = reg.regClass().isFlat();
    const RegClassType type = reg.classValue();
    if (flat) {
        switch (type) {
          case IntRegClass:
            *(RegVal *)val = readIntRegFlat(idx);
            break;
          case VecRegClass:
            *(ArmISA::VecRegContainer *)val = readVecRegFlat(idx);
            break;
          case VecElemClass:
            *(RegVal *)val = readVecElemFlat(idx);
            break;
          case VecPredRegClass:
            *(ArmISA::VecPredRegContainer *)val = readVecPredRegFlat(idx);
            break;
          case CCRegClass:
            *(RegVal *)val = readCCRegFlat(idx);
            break;
          case MiscRegClass:
            panic("MiscRegs should not be read with getReg.");
          default:
            panic("Unrecognized register class type %d.", type);
        }
    } else {
        switch (type) {
          case IntRegClass:
            *(RegVal *)val = readIntReg(idx);
            break;
          case VecRegClass:
            *(ArmISA::VecRegContainer *)val = readVecReg(reg);
            break;
          case VecElemClass:
            *(RegVal *)val = readVecElem(reg);
            break;
          case VecPredRegClass:
            *(ArmISA::VecPredRegContainer *)val = readVecPredReg(reg);
            break;
          case CCRegClass:
            *(RegVal *)val = readCCReg(idx);
            break;
          case MiscRegClass:
            panic("MiscRegs should not be read with getReg.");
          default:
            panic("Unrecognized register class type %d.", type);
        }
    }
}

void
ThreadContext::setReg(const RegId &reg, const void *val)
{
    const RegIndex idx = reg.index();
    const bool flat = reg.regClass().isFlat();
    const RegClassType type = reg.classValue();
    if (flat) {
        switch (type) {
          case IntRegClass:
            setIntRegFlat(idx, *(RegVal *)val);
            break;
          case VecRegClass:
            setVecRegFlat(idx, *(ArmISA::VecRegContainer *)val);
            break;
          case VecElemClass:
            setVecElemFlat(idx, *(RegVal *)val);
            break;
          case VecPredRegClass:
            setVecPredRegFlat(idx, *(ArmISA::VecPredRegContainer *)val);
            break;
          case CCRegClass:
            setCCRegFlat(idx, *(RegVal *)val);
            break;
          case MiscRegClass:
            panic("MiscRegs should not be read with getReg.");
          default:
            panic("Unrecognized register class type %d.", type);
        }
    } else {
        switch (type) {
          case IntRegClass:
            setIntReg(idx, *(RegVal *)val);
            break;
          case VecRegClass:
            setVecReg(reg, *(ArmISA::VecRegContainer *)val);
            break;
          case VecElemClass:
            setVecElem(reg, *(RegVal *)val);
            break;
          case VecPredRegClass:
            setVecPredReg(reg, *(ArmISA::VecPredRegContainer *)val);
            break;
          case CCRegClass:
            setCCReg(idx, *(RegVal *)val);
            break;
          case MiscRegClass:
            panic("MiscRegs should not be read with getReg.");
          default:
            panic("Unrecognized register class type %d.", type);
        }
    }
}

void *
ThreadContext::getWritableReg(const RegId &reg)
{
    const RegIndex idx = reg.index();
    const bool flat = reg.regClass().isFlat();
    const RegClassType type = reg.classValue();
    if (flat) {
        switch (type) {
          case VecRegClass:
            return &getWritableVecRegFlat(idx);
          case VecPredRegClass:
            return &getWritableVecPredRegFlat(idx);
          default:
            panic("Unrecognized register class type %d.", type);
        }
    } else {
        switch (type) {
          case VecRegClass:
            return &getWritableVecReg(reg);
          case VecPredRegClass:
            return &getWritableVecPredReg(reg);
          default:
            panic("Unrecognized register class type %d.", type);
        }
    }
}

iris::ResourceId
ThreadContext::getIntRegRscId(RegIndex int_reg) const
{
    ArmISA::CPSR cpsr = readMiscRegNoEffect(ArmISA::MISCREG_CPSR);
    auto &regIds = cpsr.width ? intReg32Ids : intReg64Ids;
    iris::ResourceId rsc_id = iris::IRIS_UINT64_MAX;
    if (int_reg < regIds.size())
        rsc_id = regIds.at(int_reg);

    panic_if(rsc_id == iris::IRIS_UINT64_MAX,
             "Int reg %s is not supported by fast model.",
             ArmISA::intRegClass[int_reg]);
    return rsc_id;
}

RegVal
ThreadContext::readIntReg(RegIndex reg_idx) const
{
    iris::ResourceReadResult result;
    call().resource_read(_instId, result, getIntRegRscId(reg_idx));
    return result.data.at(0);
}

void
ThreadContext::setIntReg(RegIndex reg_idx, RegVal val)
{
    iris::ResourceWriteResult result;
    call().resource_write(_instId, result, getIntRegRscId(reg_idx), val);
}

iris::ResourceId
ThreadContext::getIntRegFlatRscId(RegIndex int_reg) const
{
    iris::ResourceId rsc_id = iris::IRIS_UINT64_MAX;
    if (int_reg < flattenedIntIds.size())
        rsc_id = flattenedIntIds.at(int_reg);
    return rsc_id;
}

/*
 * The 64 bit version of registers gives us a pre-flattened view of the reg
 * file, no matter what mode we're in or if we're currently 32 or 64 bit.
 */
RegVal
ThreadContext::readIntRegFlat(RegIndex idx) const
{
    auto rsc_id = getIntRegFlatRscId(idx);
    if (rsc_id == iris::IRIS_UINT64_MAX)
        return 0;
    iris::ResourceReadResult result;
    call().resource_read(_instId, result, rsc_id);
    return result.data.at(0);
}

void
ThreadContext::setIntRegFlat(RegIndex idx, uint64_t val)
{
    auto rsc_id = getIntRegFlatRscId(idx);
    panic_if(rsc_id == iris::IRIS_UINT64_MAX,
             "Int reg %s is not supported by fast model.",
             ArmISA::intRegClass[idx]);
    iris::ResourceWriteResult result;
    call().resource_write(_instId, result, rsc_id, val);
}

iris::ResourceId
ThreadContext::getCCRegFlatRscId(RegIndex cc_reg) const
{
    iris::ResourceId rsc_id = iris::IRIS_UINT64_MAX;
    if (cc_reg < ccRegIds.size())
        rsc_id = ccRegIds.at(cc_reg);
    return rsc_id;
}

RegVal
ThreadContext::readCCRegFlat(RegIndex idx) const
{
    auto rsc_id = getCCRegFlatRscId(idx);
    if (rsc_id == iris::IRIS_UINT64_MAX)
        return 0;
    iris::ResourceReadResult result;
    call().resource_read(_instId, result, rsc_id);
    return result.data.at(0);
}

void
ThreadContext::setCCRegFlat(RegIndex idx, RegVal val)
{
    auto rsc_id = getCCRegFlatRscId(idx);
    panic_if(rsc_id == iris::IRIS_UINT64_MAX,
             "CC reg %s is not supported by fast model.",
             ArmISA::ccRegClass[idx]);
    iris::ResourceWriteResult result;
    call().resource_write(_instId, result, rsc_id, val);
}

iris::ResourceId
ThreadContext::getVecRegRscId(RegIndex vec_reg) const
{
    iris::ResourceId rsc_id = iris::IRIS_UINT64_MAX;
    if (vec_reg < vecRegIds.size())
        rsc_id = vecRegIds.at(vec_reg);
    return rsc_id;
}

const ArmISA::VecRegContainer &
ThreadContext::readVecReg(const RegId &reg_id) const
{
    const RegIndex idx = reg_id.index();
    ArmISA::VecRegContainer &reg = vecRegs.at(idx);
    reg.zero();

    // Ignore accesses to registers which aren't architected. gem5 defines a
    // few extra registers which it uses internally in the implementation of
    // some instructions.
    auto rsc_id = getVecRegRscId(reg_id);
    if (rsc_id == iris::IRIS_UINT64_MAX)
        return reg;

    iris::ResourceReadResult result;
    call().resource_read(_instId, result, rsc_id);
    size_t data_size = result.data.size() * (sizeof(*result.data.data()));
    size_t size = std::min(data_size, reg.size());
    memcpy(reg.as<uint8_t>(), (void *)result.data.data(), size);

    return reg;
}

const ArmISA::VecRegContainer &
ThreadContext::readVecRegFlat(RegIndex idx) const
{
    return readVecReg(ArmISA::vecRegClass[idx]);
}

iris::ResourceId
ThreadContext::getVecPredRegRscId(RegIndex vec_reg) const
{
    iris::ResourceId rsc_id = iris::IRIS_UINT64_MAX;
    if (vec_reg < vecPredRegIds.size())
        rsc_id = vecPredRegIds.at(vec_reg);
    return rsc_id;
}

const ArmISA::VecPredRegContainer &
ThreadContext::readVecPredReg(const RegId &reg_id) const
{
    RegIndex idx = reg_id.index();

    ArmISA::VecPredRegContainer &reg = vecPredRegs.at(idx);
    reg.reset();

    auto rsc_id = getVecPredRegRscId(reg_id);
    if (rsc_id == iris::IRIS_UINT64_MAX)
        return reg;

    iris::ResourceReadResult result;
    call().resource_read(_instId, result, rsc_id);

    size_t offset = 0;
    size_t num_bits = reg.NUM_BITS;
    uint8_t *bytes = (uint8_t *)result.data.data();
    while (num_bits > 8) {
        reg.setBits(offset, 8, *bytes);
        offset += 8;
        num_bits -= 8;
        bytes++;
    }
    if (num_bits)
        reg.setBits(offset, num_bits, *bytes);

    return reg;
}

ArmISA::VecPredRegContainer
ThreadContext::readVecPredRegFlat(RegIndex idx) const
{
    return readVecPredReg(ArmISA::vecPredRegClass[idx]);
}

} // namespace Iris
} // namespace gem5
