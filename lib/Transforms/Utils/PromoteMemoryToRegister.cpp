//===- PromoteMemoryToRegister.cpp - Convert memory refs to regs ----------===//
//
// This pass is used to promote memory references to be register references.  A
// simple example of the transformation performed by this pass is:
//
//        FROM CODE                           TO CODE
//   %X = alloca int, uint 1                 ret int 42
//   store int 42, int *%X
//   %Y = load int* %X
//   ret int %Y
//
// To do this transformation, a simple analysis is done to ensure it is safe.
// Currently this just loops over all alloca instructions, looking for
// instructions that are only used in simple load and stores.
//
// After this, the code is transformed by...something magical :)
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar/PromoteMemoryToRegister.h"
#include "llvm/Analysis/Dominators.h"
#include "llvm/iMemory.h"
#include "llvm/iPHINode.h"
#include "llvm/iTerminators.h"
#include "llvm/Pass.h"
#include "llvm/Function.h"
#include "llvm/BasicBlock.h"
#include "llvm/ConstantVals.h"

using namespace std;

namespace {

// instance of the promoter -- to keep all the local function data.
// gets re-created for each function processed
class PromoteInstance {
protected:
  vector<AllocaInst*>          Allocas;      // the alloca instruction..
  map<Instruction*, unsigned>  AllocaLookup; // reverse mapping of above
  
  vector<vector<BasicBlock*> > WriteSets;    // index corresponds to Allocas
  vector<vector<BasicBlock*> > PhiNodes;     // index corresponds to Allocas
  vector<vector<Value*> >      CurrentValue; // the current value stack
  
  //list of instructions to remove at end of pass :)
  vector<Instruction *>        KillList;

  set<BasicBlock*> visited;       // the basic blocks we've already visited
  map<BasicBlock*, vector<PHINode*> > NewPhiNodes; // the phinodes we're adding

  void traverse(BasicBlock *f, BasicBlock * predecessor);
  bool PromoteFunction(Function *F, DominanceFrontier &DF);
  bool QueuePhiNode(BasicBlock *bb, unsigned alloca_index);
  void findSafeAllocas(Function *M);
  bool didchange;
public:
  // I do this so that I can force the deconstruction of the local variables
  PromoteInstance(Function *F, DominanceFrontier &DF) {
    didchange = PromoteFunction(F, DF);
  }
  //This returns whether the pass changes anything
  operator bool () { return didchange; }
};

}  // end of anonymous namespace



// findSafeAllocas - Find allocas that are safe to promote
//
void PromoteInstance::findSafeAllocas(Function *F) {
  BasicBlock *BB = F->getEntryNode();  // Get the entry node for the function

  // Look at all instructions in the entry node
  for (BasicBlock::iterator I = BB->begin(), E = BB->end(); I != E; ++I)
    if (AllocaInst *AI = dyn_cast<AllocaInst>(*I))       // Is it an alloca?
      if (!AI->isArrayAllocation()) {
	bool isSafe = true;
	for (Value::use_iterator UI = AI->use_begin(), UE = AI->use_end();
	     UI != UE; ++UI) {   // Loop over all of the uses of the alloca

	  // Only allow nonindexed memory access instructions...
	  if (MemAccessInst *MAI = dyn_cast<MemAccessInst>(*UI)) {
	    if (MAI->hasIndices()) {  // indexed?
	      // Allow the access if there is only one index and the index is
              // zero.
	      if (*MAI->idx_begin() != ConstantUInt::get(Type::UIntTy, 0) ||
		  MAI->idx_begin()+1 != MAI->idx_end()) {
		isSafe = false;
                break;
	      }
	    }
	  } else {
	    isSafe = false; break;   // Not a load or store?
	  }
	}
	if (isSafe) {            // If all checks pass, add alloca to safe list
          AllocaLookup[AI] = Allocas.size();
          Allocas.push_back(AI);
        }
      }
}



bool PromoteInstance::PromoteFunction(Function *F, DominanceFrontier &DF) {
  // Calculate the set of safe allocas
  findSafeAllocas(F);

  // Add each alloca to the KillList.  Note: KillList is destroyed MOST recently
  // added to least recently.
  KillList.assign(Allocas.begin(), Allocas.end());

  // Calculate the set of write-locations for each alloca.  This is analogous to
  // counting the number of 'redefinitions' of each variable.
  WriteSets.resize(Allocas.size());
  for (unsigned i = 0; i != Allocas.size(); ++i) {
    AllocaInst *AI = Allocas[i];
    for (Value::use_iterator U =AI->use_begin(), E = AI->use_end(); U != E; ++U)
      if (StoreInst *SI = dyn_cast<StoreInst>(*U))
        // jot down the basic-block it came from
        WriteSets[i].push_back(SI->getParent());
  }

  // Compute the locations where PhiNodes need to be inserted.  Look at the
  // dominance frontier of EACH basic-block we have a write in
  //
  PhiNodes.resize(Allocas.size());
  for (unsigned i = 0; i != Allocas.size(); ++i) {
    for (unsigned j = 0; j != WriteSets[i].size(); j++) {
      // Look up the DF for this write, add it to PhiNodes
      DominanceFrontier::const_iterator it = DF.find(WriteSets[i][j]);
      DominanceFrontier::DomSetType     S = it->second;
      for (DominanceFrontier::DomSetType::iterator P = S.begin(), PE = S.end();
           P != PE; ++P)
        QueuePhiNode(*P, i);
    }
    
    // Perform iterative step
    for (unsigned k = 0; k != PhiNodes[i].size(); k++) {
      DominanceFrontier::const_iterator it = DF.find(PhiNodes[i][k]);
      DominanceFrontier::DomSetType     S = it->second;
      for (DominanceFrontier::DomSetType::iterator P = S.begin(), PE = S.end();
           P != PE; ++P)
        QueuePhiNode(*P, i);
    }
  }

  // Set the incoming values for the basic block to be null values for all of
  // the alloca's.  We do this in case there is a load of a value that has not
  // been stored yet.  In this case, it will get this null value.
  //
  CurrentValue.push_back(vector<Value *>(Allocas.size()));
  for (unsigned i = 0, e = Allocas.size(); i != e; ++i)
    CurrentValue[0][i] =
      Constant::getNullValue(Allocas[i]->getType()->getElementType());

  // Walks all basic blocks in the function performing the SSA rename algorithm
  // and inserting the phi nodes we marked as necessary
  //
  traverse(F->front(), 0);  // there is no predecessor of the root node

  // Remove all instructions marked by being placed in the KillList...
  //
  while (!KillList.empty()) {
    Instruction *I = KillList.back();
    KillList.pop_back();

    //now go find..
    I->getParent()->getInstList().remove(I);
    delete I;
  }

  return !Allocas.empty();
}


// QueuePhiNode - queues a phi-node to be added to a basic-block for a specific
// Alloca returns true if there wasn't already a phi-node for that variable
//
bool PromoteInstance::QueuePhiNode(BasicBlock *BB, unsigned i /*the alloca*/) {
  // Look up the basic-block in question
  vector<PHINode*> &BBPNs = NewPhiNodes[BB];
  if (BBPNs.empty()) BBPNs.resize(Allocas.size());

  // If the BB already has a phi node added for the i'th alloca then we're done!
  if (BBPNs[i]) return false;

  // Create a phi-node using the dereferenced type...
  PHINode *PN = new PHINode(Allocas[i]->getType()->getElementType(),
                            Allocas[i]->getName()+".mem2reg");
  BBPNs[i] = PN;

  // Add the phi-node to the basic-block
  BB->getInstList().push_front(PN);

  PhiNodes[i].push_back(BB);
  return true;
}

void PromoteInstance::traverse(BasicBlock *BB, BasicBlock *Pred) {
  vector<Value *> &TOS = CurrentValue.back(); // look at top

  // If this is a BB needing a phi node, lookup/create the phinode for each
  // variable we need phinodes for.
  vector<PHINode *> &BBPNs = NewPhiNodes[BB];
  for (unsigned k = 0; k != BBPNs.size(); ++k)
    if (PHINode *PN = BBPNs[k]) {
      // at this point we can assume that the array has phi nodes.. let's add
      // the incoming data
      PN->addIncoming(TOS[k], Pred);

      // also note that the active variable IS designated by the phi node
      TOS[k] = PN;
    }

  // don't revisit nodes
  if (visited.count(BB)) return;
  
  // mark as visited
  visited.insert(BB);

  // keep track of the value of each variable we're watching.. how?
  for (BasicBlock::iterator II = BB->begin(); II != BB->end(); ++II) {
    Instruction *I = *II; //get the instruction

    if (LoadInst *LI = dyn_cast<LoadInst>(I)) {
      Value *Ptr = LI->getPointerOperand();

      if (AllocaInst *Src = dyn_cast<AllocaInst>(Ptr)) {
        map<Instruction*, unsigned>::iterator ai = AllocaLookup.find(Src);
        if (ai != AllocaLookup.end()) {
          Value *V = TOS[ai->second];

          // walk the use list of this load and replace all uses with r
          LI->replaceAllUsesWith(V);
          KillList.push_back(LI); // Mark the load to be deleted
        }
      }
    } else if (StoreInst *SI = dyn_cast<StoreInst>(I)) {
      // delete this instruction and mark the name as the current holder of the
      // value
      Value *Ptr = SI->getPointerOperand();
      if (AllocaInst *Dest = dyn_cast<AllocaInst>(Ptr)) {
        map<Instruction *, unsigned>::iterator ai = AllocaLookup.find(Dest);
        if (ai != AllocaLookup.end()) {
          // what value were we writing?
          TOS[ai->second] = SI->getOperand(0);
          KillList.push_back(SI);  // Mark the store to be deleted
        }
      }
      
    } else if (TerminatorInst *TI = dyn_cast<TerminatorInst>(I)) {
      // Recurse across our successors
      for (unsigned i = 0; i != TI->getNumSuccessors(); i++) {
        CurrentValue.push_back(CurrentValue.back());
        traverse(TI->getSuccessor(i), BB); // This node becomes the predecessor
        CurrentValue.pop_back();
      }
    }
  }
}


namespace {
  struct PromotePass : public FunctionPass {

    // runOnFunction - To run this pass, first we calculate the alloca
    // instructions that are safe for promotion, then we promote each one.
    //
    virtual bool runOnFunction(Function *F) {
      return (bool)PromoteInstance(F, getAnalysis<DominanceFrontier>());
    }

    // getAnalysisUsage - We need dominance frontiers
    //
    virtual void getAnalysisUsage(AnalysisUsage &AU) const {
      AU.addRequired(DominanceFrontier::ID);
    }
  };
}
  

// createPromoteMemoryToRegister - Provide an entry point to create this pass.
//
Pass *createPromoteMemoryToRegister() {
  return new PromotePass();
}
