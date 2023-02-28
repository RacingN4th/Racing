#include <fstream>
#include <map>
#include <set>
#include <string>

#include "../config.h"
#include "../debug.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/ModuleSlotTracker.h"
#include "llvm/Pass.h"

#define alloc_printf(_str...)                                                  \
  ({                                                                           \
    char *_tmp;                                                                \
    s32 _len = snprintf(NULL, 0, _str);                                        \
    if (_len < 0)                                                              \
      FATAL("Whoa, snprintf() fails?!");                                       \
    _tmp = (char *)malloc(_len + 1);                                           \
    snprintf((char *)_tmp, _len + 1, _str);                                    \
    _tmp;                                                                      \
  })

extern char s_phi[];

using namespace llvm;

class Tracer : public ModulePass {
public:
  Tracer();
  ~Tracer(){}

  virtual bool doInitialization(Module &M);
  void dofinish();
  bool runOnFunction(Function &F,std::vector<std::string> &target);
  bool runOnBasicBlock(BasicBlock &BB,std::vector<std::string> &target);

  // private:
  // Instrumentation functions for different types of nodes.
  void handlePhiNodes(BasicBlock *BB);
  void handleInstructionResult(Instruction *inst, Instruction *next_inst);
  void handleCallInstruction(Instruction *inst);
  void handleNonPhiNonCallInstruction(Instruction *inst);

  // References to the logging functions.
  Value *TL_trace_value;
  uint32_t inst_id;
  std::string base_dir;

  static char ID;
  
};
