/* vim: set expandtab ts=4 sw=4: */

/*
 * Copyright 2017 The bin2llvm Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "ReplaceConstantLoads.h"

#include "FixOverlappedBBs.h"
#include "JumpTableInfo.h"

#include <llvm/Bitcode/ReaderWriter.h>
#include <llvm/Function.h>
#include <llvm/Instructions.h>
#include <llvm/LLVMContext.h>
#include <llvm/Module.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Utils/ValueMapper.h>

#include <list>
#include <fstream>
#include <cstring>

using namespace llvm;

char ReplaceConstantLoads::ID = 0;
static RegisterPass<ReplaceConstantLoads> X("replaceconstantloads",
        "Replace __ldl_mmu(cst) with the actual value.", false, false);

static cl::opt<std::string> JumpTableInfoFile("jump-table-info",
        cl::desc("Json file with jump tables. Generated by jump_table.py"),
        cl::value_desc("filename"));

static cl::list<std::string> MemoryDescriptors(
        "memory",
        cl::desc("path-to-file@0xL0ADADD7"));

static cl::opt<bool> IsBigEndian("is-big-endian",
        cl::desc("Set big endian."));

ReplaceConstantLoads::MemoryPool::MemoryPool(std::string path,
        uint64_t start)
{
    mp_start = start;

    // open file at end
    mp_file.open(path, std::ifstream::ate |
            std::ifstream::in |
            std::ifstream::binary);
    mp_end = mp_start+(uint64_t)mp_file.tellg();

    // seek to beg (just in case)
    mp_file.seekg(0, mp_file.beg);

    assert(this->mp_file.good());
}

bool
ReplaceConstantLoads::MemoryPool::inside(uint64_t addr)
{
    return addr >= mp_start && addr < mp_end;
}

uint64_t
ReplaceConstantLoads::MemoryPool::read(uint64_t addr,
        uint8_t byteCnt,
        bool isBigEndian = false)
{
    uint64_t ret = 0;

    assert(inside(addr));
    assert(byteCnt);
    assert(byteCnt <= 8);
    assert(inside(addr+byteCnt-1));

    for (int i = 0; i < byteCnt; ++i) {
        uint64_t pos;
        if (isBigEndian) {
            pos = addr+i-mp_start;
        } else {
            pos = addr+byteCnt-i-1-mp_start;
        }
        mp_file.seekg(pos, mp_file.beg);
        assert(mp_file.good());
        assert(!mp_file.eof());
        unsigned char b;
        mp_file.read((char *)&b, 1);
        //outs() << "[ReplaceConstantLoads] Y [" << FixOverlappedBBs::hex(addr)
        //    << "] got " << i << " - " << FixOverlappedBBs::hex(b) << "\n";
        ret = ((uint64_t)ret << 8) | ((uint64_t)b);
    }
    outs() << "[ReplaceConstantLoads] load cst [" << FixOverlappedBBs::hex(addr)
        << "] = " << FixOverlappedBBs::hex(ret) << "\n";

    return ret;
}

ReplaceConstantLoads::MemoryPool *
ReplaceConstantLoads::getMemoryPool(std::string desc)
{
    std::string::size_type atPos = desc.rfind('@');

    assert(atPos != std::string::npos);
    std::string path = desc.substr(0, atPos);
    uint64_t addr = strtoll(desc.substr(atPos+1).c_str(), NULL, 16);

    outs() << "[ReplaceConstantLoads] adding pool from file " <<
        path << "@" << FixOverlappedBBs::hex(addr) << "\n";

    return new MemoryPool(path, addr);
}

void ReplaceConstantLoads::initialize()
{
    for (auto desci = MemoryDescriptors.begin(), descie = MemoryDescriptors.end();
            desci != descie;
            ++desci) {
        MemoryPool *mp = getMemoryPool(*desci);
        m_memoryPools.push_back(mp);
    }

    if (JumpTableInfoFile != "") {
        jumpTableInfoList = JumpTableInfoFactory::loadFromFile(JumpTableInfoFile);
        std::list<JumpTableInfo *>::iterator it = jumpTableInfoList.begin();
        std::list<JumpTableInfo *>::iterator ite = jumpTableInfoList.end();
        for (; it != ite; ++it)
            jumpTableInfoMap[(*it)->indirect_jmp_pc] = *it;

        hasJumpTableInfo = true;
        outs() << "[ReplaceConstantLoads] load jump table from " <<
            JumpTableInfoFile << ", cnt: " << jumpTableInfoList.size() << "\n";
    } else {
        hasJumpTableInfo = false;
    }

    outs() << "[ReplaceConstantLoads] loaded " << m_memoryPools.size()
        << " constant pools\n";
    outs() << "[ReplaceConstantLoads] endianness " << IsBigEndian << "\n";
}

bool ReplaceConstantLoads::runOnFunction(Function &F)
{
    std::list<llvm::Instruction *> eraseIns;

    /* replace load_mmu */
    for (auto bbi = F.begin(), bbie = F.end();
            bbi != bbie;
            ++bbi) {
        for (auto insi = bbi->begin(), insie = bbi->end();
                insi != insie;
                ++insi) {
            CallInst* callInst = dyn_cast<CallInst>(insi);
            if (!callInst)
                continue;
            if (callInst->getNumArgOperands() != 2)
                continue;
            ConstantInt* address =
                dyn_cast<ConstantInt>(callInst->getArgOperand(0));
            if (!address)
                continue;

            IntegerType *valueType = NULL;

            if (callInst->getCalledFunction()->getName() ==
                    "__ldl_mmu") {
                valueType = llvm::Type::getInt32Ty(bbi->getContext());
            } else if (callInst->getCalledFunction()->getName() ==
                    "__lds_mmu") {
                valueType = llvm::Type::getInt16Ty(bbi->getContext());
            } else if (callInst->getCalledFunction()->getName() ==
                    "__ldb_mmu") {
                valueType = llvm::Type::getInt8Ty(bbi->getContext());
            }
            if (valueType)  {
                Value* value = getMemoryValue(address->getZExtValue(),
                        valueType, IsBigEndian);
                if (!value) {
                    outs() << "[ReplaceConstantLoads] skip load from: " <<
                        address->getZExtValue() << " -- " << *callInst <<
                        "\n";
                    continue;
                }
                callInst->replaceAllUsesWith(value);
                eraseIns.push_back(callInst);
            }
        }
    }

    /* annotate jump tables */
    LLVMContext &ctx = F.getContext();
    for (auto bbi = F.begin(), bbie = F.end();
            bbi != bbie;
            ++bbi) {
        for (auto insi = bbi->begin(), insie = bbi->end();
                insi != insie;
                ++insi) {
            StoreInst *storeInst = dyn_cast<StoreInst>(insi);
            if (!storeInst)
                continue;
            GlobalVariable *gv = dyn_cast<GlobalVariable>(storeInst->getOperand(1));
            ConstantInt* val = dyn_cast<ConstantInt>(storeInst->getValueOperand());
            if (!(gv && gv->getName() == "PC"))
                continue;
            if (!insi->getMetadata("INS_currPC"))
                continue;

            uint64_t currPC =
                FixOverlappedBBs::getHexMetadataFromIns(insi,
                        "INS_currPC");

            if (jumpTableInfoMap.find(currPC) == jumpTableInfoMap.end())
                continue;

            JumpTableInfo *info = jumpTableInfoMap[currPC];
            int cnt_entries = 1+info->idx_stop-info->idx_start;
            /* XXX this has to go into the json */
            int mul = 4;
            assert(cnt_entries >= 1);
            /* we have to load cnt_entries entries and push them as new
             * PCs
             */
            MDNode *m = MDNode::get(ctx, MDString::get(ctx,
                        FixOverlappedBBs::hex(cnt_entries)));
            insi->setMetadata("INS_switch_cnt", m);
            m = MDNode::get(ctx, MDString::get(ctx,
                        FixOverlappedBBs::hex(info->default_case_pc)));
            insi->setMetadata("INS_switch_default", m);
            m = MDNode::get(ctx, MDString::get(ctx,
                        FixOverlappedBBs::hex(info->idx_start)));
            insi->setMetadata("INS_switch_idx_start", m);
            for (int i = 0; i < cnt_entries; ++i) {
                uint64_t loadedPC = getMemoryValue(info->base_table+mul*i,
                        4, IsBigEndian);
                MDNode *m = MDNode::get(ctx, MDString::get(ctx,
                            FixOverlappedBBs::hex(
                                loadedPC)));
                char buf[512];
                snprintf(buf, sizeof buf, "INS_switch_case%d", i);
                insi->setMetadata(std::string(buf), m);
                /* OK, we've got a sovled jumptable */
            }
        }
    }

    for (auto insi = eraseIns.begin(), insie = eraseIns.end();
            insi != insie;
            ++insi) {
        (*insi)->eraseFromParent();
    }
    outs() << "[ReplaceConstantLoads] erased " << eraseIns.size() <<
        " instructions\n";
    if (eraseIns.size() > 0) {
        return true;
    }
    return false;
}

uint64_t
ReplaceConstantLoads::getMemoryValue(uint64_t address,
        uint64_t size,
        bool isBigEndian = false)
{
    for (auto mpi = m_memoryPools.begin(), mpie = m_memoryPools.end();
            mpi !=  mpie;
            ++mpi) {
        if ((*mpi)->inside(address)) {
            uint64_t value = (*mpi)->read(address,
                    size, isBigEndian);
            return value;
        }
    }
    return 0xdeadbeef;
}

llvm::Value *
ReplaceConstantLoads::getMemoryValue(uint64_t address,
        llvm::IntegerType *type,
        bool isBigEndian = false)
{
    for (auto mpi = m_memoryPools.begin(), mpie = m_memoryPools.end();
            mpi !=  mpie;
            ++mpi) {
        if ((*mpi)->inside(address)) {
            uint64_t value = (*mpi)->read(address,
                    type->getBitWidth() / 8, isBigEndian);
            return ConstantInt::get(type, value, false);
        }
    }
    return NULL;
}

ReplaceConstantLoads::
~ReplaceConstantLoads()
{
    for (auto p = m_memoryPools.begin(), pe = m_memoryPools.end();
            p != pe;
            ++p) {
        (*p)->mp_file.close();
        delete *p;
    }
    outs() << "[ReplaceConstantLoads] destroy\n";
}
