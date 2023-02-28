#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <time.h>
#include <vector>

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"

#include "full_trace.h"

using namespace llvm;
using namespace std;

raw_fd_ostream *logstream, *trace_id_steam, *inst_id_steam;

namespace {

static void getDebugLoc(Instruction *I, std::string &Filename, unsigned &Line,
                        unsigned &Column) {
#ifdef LLVM_OLD_DEBUG_API
  DebugLoc Loc = I->getDebugLoc();
  if (!Loc.isUnknown()) {
    DILocation cDILoc(Loc.getAsMDNode(M.getContext()));
    DILocation oDILoc = cDILoc.getOrigLocation();

    Line = oDILoc.getLineNumber();
    Filename = oDILoc.getFilename().str();
    Column = oDILoc.getColumnNumber();

    if (filename.empty()) {
      Line = cDILoc.getLineNumber();
      Filename = cDILoc.getFilename().str();
      Column = cDILoc.getColumnNumber();
    }
  }
#else
  if (DILocation *Loc = I->getDebugLoc()) {
    Line = Loc->getLine();
    Filename = Loc->getFilename().str();
    Column = Loc->getColumn();

    if (Filename.empty()) {
      DILocation *oDILoc = Loc->getInlinedAt();
      if (oDILoc) {
        Line = oDILoc->getLine();
        Filename = oDILoc->getFilename().str();
        Column = oDILoc->getColumn();
      }
    }
  }
#endif /* LLVM_OLD_DEBUG_API */
}

} // end of anonymous namespace

Tracer::Tracer() : ModulePass(ID) {}

bool Tracer::doInitialization(Module &M) {
  std::error_code ErrInfo;
  std::string filename = M.getSourceFileName();
  filename = filename.substr(filename.find_last_of("/\\") + 1);

  char *trace_id = alloc_printf("trace-id.log");
  trace_id_steam = new raw_fd_ostream(base_dir + "/" + std::string(trace_id),
                                      ErrInfo, sys::fs::OpenFlags::F_Append);
  if (trace_id == nullptr) {
    SAYF("failed to open %s\n", trace_id);
  }

  std::ifstream inst_id_file(base_dir + "/" + "inst_id");
  if (!inst_id_file.is_open()) {
    outs() << "failed to open inst_id file\n";
    inst_id = 0;
  } else {
    inst_id = 0;
    std::string inst_id_str;
    while (std::getline(inst_id_file, inst_id_str)) {
      inst_id = atoi(inst_id_str.c_str());
    }
    inst_id_file.close();
  }
  outs() << "initial inst_id: " << inst_id << "\n";

  auto &llvm_context = M.getContext();
  auto I1Ty = Type::getInt1Ty(llvm_context);
  auto I64Ty = Type::getInt64Ty(llvm_context);
  auto I8PtrTy = Type::getInt8PtrTy(llvm_context);
  auto VoidTy = Type::getVoidTy(llvm_context);
  auto DoubleTy = Type::getDoubleTy(llvm_context);

  // Add external trace_value function declarations.
  TL_trace_value = M.getOrInsertFunction("trace_value", I64Ty, I64Ty);

  return false;
}

void Tracer::dofinish() {
  trace_id_steam->close();
  std::error_code ErrInfo;
  char *inst_id_file = alloc_printf("inst_id");
  outs() << base_dir + "/" + std::string(inst_id_file) << "\n";
  inst_id_steam = new raw_fd_ostream(
      base_dir + "/" + std::string(inst_id_file), ErrInfo,
      sys::fs::OpenFlags::F_Append | sys::fs::OpenFlags::F_RW);
  outs() << "final inst_id: " << inst_id << "\n";
  *inst_id_steam << inst_id << '\n';
  inst_id_steam->close();
}

bool Tracer::runOnFunction(Function &F, std::vector<std::string> &target) {
  bool func_modified = false;
  std::string filename;
  unsigned line, column;
  for (auto bb_it = F.begin(); bb_it != F.end(); ++bb_it) {
    BasicBlock &bb = *bb_it;
    bool trace_bb = false;
    for (BasicBlock::iterator itr = bb.begin(); itr != bb.end(); ++itr) {
      getDebugLoc(cast<Instruction>(itr), filename, line, column);
      if (filename.empty() || line == 0)
        continue;
      std::size_t found = filename.find_last_of("/\\");
      if (found != std::string::npos)
        filename = filename.substr(found + 1);
      auto findline = find(begin(target), end(target),
                           filename + ':' + std::to_string(line));
      if (findline != std::end(target)) {
        trace_bb = true;
        std::cout << "found:" + filename + ':' + std::to_string(line) << "\n";
        target.erase(findline);
        break;
      }
    }
    if (trace_bb) {
      func_modified = runOnBasicBlock(bb, target);
    }
  }
  return func_modified;
}

bool Tracer::runOnBasicBlock(BasicBlock &BB, std::vector<std::string> &target) {
  // We have to get the first insertion point before we insert any
  // instrumentation!
  BasicBlock::iterator insertp = BB.getFirstInsertionPt();

  std::string filename;
  unsigned line, column;

  BasicBlock::iterator itr = BB.begin();
  if (isa<PHINode>(itr)) {
    getDebugLoc(cast<Instruction>(itr), filename, line, column);
    *trace_id_steam << "\n" << (filename + ':' + std::to_string(line)).c_str();
    handlePhiNodes(&BB);
  }

  // From this point onwards, nodes cannot be PHI nodes.
  BasicBlock::iterator nextitr;
  for (BasicBlock::iterator itr = insertp; itr != BB.end(); itr = nextitr) {

    nextitr = itr;
    nextitr++;

    getDebugLoc(cast<Instruction>(itr), filename, line, column);

    // Invoke instructions are used to call functions that may throw exceptions.
    // They are the only the terminator instruction that can also return a
    // value. 
    if (isa<InvokeInst>(*itr))
    {
      continue;
    }
    // Get static instruction ID: produce instid
    Instruction *currInst = cast<Instruction>(itr);
    *trace_id_steam << "\n" << (filename + ':' + std::to_string(line)).c_str();
    if (ZExtInst *Izext = dyn_cast<ZExtInst>(currInst)) {
      continue;
    }
    if (CallInst *I = dyn_cast<CallInst>(currInst)) {
      Function *called_func = I->getCalledFunction();
      // This is an indirect function  invocation (i.e. through called_fun
      // pointer). This cannot happen for code that we want to turn into
      // hardware, so skip it.
      if (!called_func || called_func->isIntrinsic()) {
        continue;
      }

      handleCallInstruction(currInst);
    } else {
      handleNonPhiNonCallInstruction(currInst);
    }
    if (!currInst->getType()->isVoidTy()) {
      Instruction *nextInst = cast<Instruction>(nextitr);
      handleInstructionResult(currInst, nextInst);
    }
  }

  // Conservatively assume that we changed the basic block.
  return true;
}

void Tracer::handleCallInstruction(Instruction *inst) {
  CallInst *CI = dyn_cast<CallInst>(inst);
  Function *fun = CI->getCalledFunction();
  int call_id = 0;
  Value *value, *curr_operand;
  for (auto arg_it = fun->arg_begin(); arg_it != fun->arg_end();
       ++arg_it, ++call_id) {
    curr_operand = inst->getOperand(call_id);
    if (Instruction *I = dyn_cast<Instruction>(curr_operand)) {
      value = curr_operand;
    } else {
      if (curr_operand->getType()->isLabelTy()) {
        continue;
      } else if (curr_operand->getValueID() == Value::FunctionVal) {
        continue;
      } else {
        value = curr_operand;
      }
    }
    if (isa<Constant>(*value))
      continue;

    ///*
    outs() << "handleCallInstruction: inst_id is " << inst_id << "\n";
    outs() << "inst      : ";
    value->print(outs());
    outs() << "\n";
    outs() << "value type: ";
    value->getType()->print(outs());
    outs() << "\n";
    //*/

    IRBuilder<> IRB(inst);
    Value *v_value;
    if (value->getType()->isMetadataTy())
      return;
    if (value->getType()->isPointerTy())
      v_value = IRB.CreatePtrToInt(value, IRB.getInt64Ty());
    else if (value->getType()->isFloatingPointTy())
      v_value = IRB.CreateFPExt(value, IRB.getDoubleTy());
    else
      v_value = IRB.CreateZExtOrTrunc(value, IRB.getInt64Ty());

    Value *v_inst_id = ConstantInt::get(IRB.getInt64Ty(), inst_id);
    *trace_id_steam << "\n" << inst_id;
    Value *args[] = {v_value, v_inst_id};
    IRB.CreateCall(TL_trace_value, args);
    inst_id = (inst_id + 1) % INST_SIZE;
  }
}

void Tracer::handleNonPhiNonCallInstruction(Instruction *inst) {
  int num_of_operands = inst->getNumOperands();
  if (num_of_operands > 0) {
    for (int i = num_of_operands - 1; i >= 0; i--) {
      Value *curr_operand = inst->getOperand(i);
      Value *value;

      if (Instruction *I = dyn_cast<Instruction>(curr_operand)) {
        value = curr_operand;
      } else {
        if (curr_operand->getType()->isVectorTy()) {
          value = curr_operand;
        } else if (curr_operand->getType()->isLabelTy()) {
          continue;
        } else if (curr_operand->getValueID() == Value::FunctionVal) {
          continue;
        } else {
          value = curr_operand;
        }
      }
      if (isa<Constant>(*value))
        continue;

      outs() << "handleNonPhiNonCallInstruction: inst_id is " << inst_id
             << "\n";
      outs() << "inst      : ";
      value->print(outs());
      outs() << "\n";
      outs() << "value type: ";
      value->getType()->print(outs());
      outs() << "\n";

      IRBuilder<> IRB(inst);
      Value *v_value;
      if (value->getType()->isMetadataTy())
        return;
      if (value->getType()->isPointerTy())
        v_value = IRB.CreatePtrToInt(value, IRB.getInt64Ty());
      else if (value->getType()->isFloatingPointTy())
        v_value = IRB.CreateFPExt(value, IRB.getDoubleTy());
      else
        v_value = IRB.CreateZExtOrTrunc(value, IRB.getInt64Ty());
      Value *v_inst_id = ConstantInt::get(IRB.getInt64Ty(), inst_id);
      *trace_id_steam << "\n" << inst_id;
      Value *args[] = {v_value, v_inst_id};
      IRB.CreateCall(TL_trace_value, args);
      inst_id = (inst_id + 1) % INST_SIZE;
    }
  }
}

// Handle all phi nodes at the beginning of a basic block.

void Tracer::handlePhiNodes(BasicBlock *BB) {
  BasicBlock::iterator insertp = BB->getFirstInsertionPt();
  Instruction *insertPointInst = cast<Instruction>(insertp);

  Value *v_value;
  Value *curr_operand = nullptr;
  for (BasicBlock::iterator itr = BB->begin(); isa<PHINode>(itr); itr++) {
    Instruction *currInst = cast<Instruction>(itr);
    if (ZExtInst *Izext = dyn_cast<ZExtInst>(currInst)) {
      continue;
    }
    // Print each operand.
    int num_of_operands = currInst->getNumOperands();
    if (num_of_operands > 0) {
      for (int i = num_of_operands - 1; i >= 0; i--) {
        curr_operand = currInst->getOperand(i);

        if (Instruction *I = dyn_cast<Instruction>(curr_operand)) {
          //
        } else {
          Value *value = curr_operand;
          if (isa<Constant>(*value))
            continue;

          outs() << "handlePhiNodes-operands: inst_id is " << inst_id << "\n";
          outs() << "inst      : ";
          value->print(outs());
          outs() << "\n";
          outs() << "value type: ";
          value->getType()->print(outs());
          outs() << "\n";

          IRBuilder<> IRB(insertPointInst);

          if (value->getType()->isMetadataTy())
            return;
          if (value->getType()->isPointerTy())
            v_value = IRB.CreatePtrToInt(value, IRB.getInt64Ty());
          else if (value->getType()->isFloatingPointTy())
            v_value = IRB.CreateFPExt(value, IRB.getDoubleTy());
          else
            v_value = IRB.CreateZExtOrTrunc(value, IRB.getInt64Ty());

          Value *v_inst_id = ConstantInt::get(IRB.getInt64Ty(), inst_id);
          *trace_id_steam << "\n" << inst_id;

          Value *args[] = {v_value, v_inst_id};
          IRB.CreateCall(TL_trace_value, args);
          inst_id = (inst_id + 1) % INST_SIZE;
        }
      }
    }

    // Print result line.

    if (!currInst->getType()->isVoidTy()) {
      if (currInst->isTerminator()) {
        assert(false && "It is terminator...\n");
      } else {
        Value *value = currInst;
        if (isa<Constant>(*value))
          continue;
        outs() << "handlePhiNodes-result: inst_id is " << inst_id << "\n";
        outs() << "inst      : ";

        value->print(outs());
        outs() << "\n";
        outs() << "value type: ";
        value->getType()->print(outs());
        outs() << "\n";

        IRBuilder<> IRB(insertPointInst);

        if (value->getType()->isMetadataTy())
          return;
        if (value->getType()->isPointerTy())
          v_value = IRB.CreatePtrToInt(value, IRB.getInt64Ty());
        else if (value->getType()->isFloatingPointTy())
          v_value = IRB.CreateFPExt(value, IRB.getDoubleTy());
        else
          v_value = IRB.CreateZExtOrTrunc(value, IRB.getInt64Ty());

        Value *v_inst_id = ConstantInt::get(IRB.getInt64Ty(), inst_id);
        *trace_id_steam << "\n" << inst_id;

        Value *args[] = {v_value, v_inst_id};
        IRB.CreateCall(TL_trace_value, args);
        inst_id = (inst_id + 1) % INST_SIZE;
      }
    }
  }
}

void Tracer::handleInstructionResult(Instruction *inst,
                                     Instruction *next_inst) {
  if (inst->isTerminator()) {
    assert(false);
  } else {
    Value *value = inst;

    if (isa<Constant>(*value))
      return;
    int num_of_operands = next_inst->getNumOperands();
    for (int i = 0; i < num_of_operands; i++) {
      Value *next_operand = next_inst->getOperand(i);
      if (next_operand == value)
        return;
    }

    outs() << "handleInstructionResult: inst_id is " << inst_id << "\n";
    outs() << "inst      : ";
    value->print(outs());
    outs() << "\n";
    outs() << "value type: ";
    value->getType()->print(outs());
    outs() << "\n";

    IRBuilder<> IRB(next_inst);
    Value *v_value;
    if (value->getType()->isMetadataTy())
      return;
    if (value->getType()->isPointerTy())
      v_value = IRB.CreatePtrToInt(value, IRB.getInt64Ty());
    else if (value->getType()->isFloatingPointTy())
      v_value = IRB.CreateFPExt(value, IRB.getDoubleTy());
    else
      v_value = IRB.CreateZExtOrTrunc(value, IRB.getInt64Ty());
    Value *v_inst_id = ConstantInt::get(IRB.getInt64Ty(), inst_id);
    *trace_id_steam << "\n" << inst_id;

    Value *args[] = {v_value, v_inst_id};
    IRB.CreateCall(TL_trace_value, args);
    inst_id = (inst_id + 1) % INST_SIZE;
  }
}

char Tracer::ID = 0;
