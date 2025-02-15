//===- Inliner.cpp - Code common to all inliners --------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the mechanics required to implement inlining without
// missing any calls and updating the call graph.  The decisions of which calls
// are profitable to inline are implemented elsewhere.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/IPO/Inliner.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/BasicAliasAnalysis.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/Analysis/InlineCost.h"
#include "llvm/Analysis/OptimizationDiagnosticInfo.h"
#include "llvm/Analysis/ProfileSummaryInfo.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/IR/CallSite.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
using namespace llvm;

#define DEBUG_TYPE "inline"

STATISTIC(NumInlined, "Number of functions inlined");
STATISTIC(NumCallsDeleted, "Number of call sites deleted, not inlined");
STATISTIC(NumDeleted, "Number of functions deleted because all callers found");
STATISTIC(NumMergedAllocas, "Number of allocas merged together");

// This weirdly named statistic tracks the number of times that, when attempting
// to inline a function A into B, we analyze the callers of B in order to see
// if those would be more profitable and blocked inline steps.
STATISTIC(NumCallerCallersAnalyzed, "Number of caller-callers analyzed");

/// Flag to disable manual alloca merging.
///
/// Merging of allocas was originally done as a stack-size saving technique
/// prior to LLVM's code generator having support for stack coloring based on
/// lifetime markers. It is now in the process of being removed. To experiment
/// with disabling it and relying fully on lifetime marker based stack
/// coloring, you can pass this flag to LLVM.
static cl::opt<bool>
    DisableInlinedAllocaMerging("disable-inlined-alloca-merging",
                                cl::init(false), cl::Hidden);

namespace {
enum class InlinerFunctionImportStatsOpts {
  No = 0,
  Basic = 1,
  Verbose = 2,
};

cl::opt<InlinerFunctionImportStatsOpts> InlinerFunctionImportStats(
    "inliner-function-import-stats",
    cl::init(InlinerFunctionImportStatsOpts::No),
    cl::values(clEnumValN(InlinerFunctionImportStatsOpts::Basic, "basic",
                          "basic statistics"),
               clEnumValN(InlinerFunctionImportStatsOpts::Verbose, "verbose",
                          "printing of statistics for each inlined function")),
    cl::Hidden, cl::desc("Enable inliner stats for imported functions"));
} // namespace

LegacyInlinerBase::LegacyInlinerBase(char &ID)
    : CallGraphSCCPass(ID), InsertLifetime(true) {}

LegacyInlinerBase::LegacyInlinerBase(char &ID, bool InsertLifetime)
    : CallGraphSCCPass(ID), InsertLifetime(InsertLifetime) {}

/// For this class, we declare that we require and preserve the call graph.
/// If the derived class implements this method, it should
/// always explicitly call the implementation here.
void LegacyInlinerBase::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<AssumptionCacheTracker>();
  AU.addRequired<ProfileSummaryInfoWrapperPass>();
  AU.addRequired<TargetLibraryInfoWrapperPass>();
  getAAResultsAnalysisUsage(AU);
  CallGraphSCCPass::getAnalysisUsage(AU);
}

typedef DenseMap<ArrayType *, std::vector<AllocaInst *>> InlinedArrayAllocasTy;

/// Look at all of the allocas that we inlined through this call site.  If we
/// have already inlined other allocas through other calls into this function,
/// then we know that they have disjoint lifetimes and that we can merge them.
///
/// There are many heuristics possible for merging these allocas, and the
/// different options have different tradeoffs.  One thing that we *really*
/// don't want to hurt is SRoA: once inlining happens, often allocas are no
/// longer address taken and so they can be promoted.
///
/// Our "solution" for that is to only merge allocas whose outermost type is an
/// array type.  These are usually not promoted because someone is using a
/// variable index into them.  These are also often the most important ones to
/// merge.
///
/// A better solution would be to have real memory lifetime markers in the IR
/// and not have the inliner do any merging of allocas at all.  This would
/// allow the backend to do proper stack slot coloring of all allocas that
/// *actually make it to the backend*, which is really what we want.
///
/// Because we don't have this information, we do this simple and useful hack.
static void mergeInlinedArrayAllocas(
    Function *Caller, InlineFunctionInfo &IFI,
    InlinedArrayAllocasTy &InlinedArrayAllocas, int InlineHistory) {
  SmallPtrSet<AllocaInst *, 16> UsedAllocas;

  // When processing our SCC, check to see if CS was inlined from some other
  // call site.  For example, if we're processing "A" in this code:
  //   A() { B() }
  //   B() { x = alloca ... C() }
  //   C() { y = alloca ... }
  // Assume that C was not inlined into B initially, and so we're processing A
  // and decide to inline B into A.  Doing this makes an alloca available for
  // reuse and makes a callsite (C) available for inlining.  When we process
  // the C call site we don't want to do any alloca merging between X and Y
  // because their scopes are not disjoint.  We could make this smarter by
  // keeping track of the inline history for each alloca in the
  // InlinedArrayAllocas but this isn't likely to be a significant win.
  if (InlineHistory != -1) // Only do merging for top-level call sites in SCC.
    return;

  // Loop over all the allocas we have so far and see if they can be merged with
  // a previously inlined alloca.  If not, remember that we had it.
  for (unsigned AllocaNo = 0, e = IFI.StaticAllocas.size(); AllocaNo != e;
       ++AllocaNo) {
    AllocaInst *AI = IFI.StaticAllocas[AllocaNo];

    // Don't bother trying to merge array allocations (they will usually be
    // canonicalized to be an allocation *of* an array), or allocations whose
    // type is not itself an array (because we're afraid of pessimizing SRoA).
    ArrayType *ATy = dyn_cast<ArrayType>(AI->getAllocatedType());
    if (!ATy || AI->isArrayAllocation())
      continue;

    // Get the list of all available allocas for this array type.
    std::vector<AllocaInst *> &AllocasForType = InlinedArrayAllocas[ATy];

    // Loop over the allocas in AllocasForType to see if we can reuse one.  Note
    // that we have to be careful not to reuse the same "available" alloca for
    // multiple different allocas that we just inlined, we use the 'UsedAllocas'
    // set to keep track of which "available" allocas are being used by this
    // function.  Also, AllocasForType can be empty of course!
    bool MergedAwayAlloca = false;
    for (AllocaInst *AvailableAlloca : AllocasForType) {

      unsigned Align1 = AI->getAlignment(),
               Align2 = AvailableAlloca->getAlignment();

      // The available alloca has to be in the right function, not in some other
      // function in this SCC.
      if (AvailableAlloca->getParent() != AI->getParent())
        continue;

      // If the inlined function already uses this alloca then we can't reuse
      // it.
      if (!UsedAllocas.insert(AvailableAlloca).second)
        continue;

      // Otherwise, we *can* reuse it, RAUW AI into AvailableAlloca and declare
      // success!
      DEBUG(dbgs() << "    ***MERGED ALLOCA: " << *AI
                   << "\n\t\tINTO: " << *AvailableAlloca << '\n');

      // Move affected dbg.declare calls immediately after the new alloca to
      // avoid the situation when a dbg.declare precedes its alloca.
      if (auto *L = LocalAsMetadata::getIfExists(AI))
        if (auto *MDV = MetadataAsValue::getIfExists(AI->getContext(), L))
          for (User *U : MDV->users())
            if (DbgDeclareInst *DDI = dyn_cast<DbgDeclareInst>(U))
              DDI->moveBefore(AvailableAlloca->getNextNode());

      AI->replaceAllUsesWith(AvailableAlloca);

      if (Align1 != Align2) {
        if (!Align1 || !Align2) {
          const DataLayout &DL = Caller->getParent()->getDataLayout();
          unsigned TypeAlign = DL.getABITypeAlignment(AI->getAllocatedType());

          Align1 = Align1 ? Align1 : TypeAlign;
          Align2 = Align2 ? Align2 : TypeAlign;
        }

        if (Align1 > Align2)
          AvailableAlloca->setAlignment(AI->getAlignment());
      }

      AI->eraseFromParent();
      MergedAwayAlloca = true;
      ++NumMergedAllocas;
      IFI.StaticAllocas[AllocaNo] = nullptr;
      break;
    }

    // If we already nuked the alloca, we're done with it.
    if (MergedAwayAlloca)
      continue;

    // If we were unable to merge away the alloca either because there are no
    // allocas of the right type available or because we reused them all
    // already, remember that this alloca came from an inlined function and mark
    // it used so we don't reuse it for other allocas from this inline
    // operation.
    AllocasForType.push_back(AI);
    UsedAllocas.insert(AI);
  }
}

/// If it is possible to inline the specified call site,
/// do so and update the CallGraph for this operation.
///
/// This function also does some basic book-keeping to update the IR.  The
/// InlinedArrayAllocas map keeps track of any allocas that are already
/// available from other functions inlined into the caller.  If we are able to
/// inline this call site we attempt to reuse already available allocas or add
/// any new allocas to the set if not possible.
static bool InlineCallIfPossible(
    CallSite CS, InlineFunctionInfo &IFI,
    InlinedArrayAllocasTy &InlinedArrayAllocas, int InlineHistory,
    bool InsertLifetime, function_ref<AAResults &(Function &)> &AARGetter,
    ImportedFunctionsInliningStatistics &ImportedFunctionsStats) {
  Function *Callee = CS.getCalledFunction();
  Function *Caller = CS.getCaller();

  AAResults &AAR = AARGetter(*Callee);

  // Try to inline the function.  Get the list of static allocas that were
  // inlined.
  if (!InlineFunction(CS, IFI, &AAR, InsertLifetime))
    return false;

  if (InlinerFunctionImportStats != InlinerFunctionImportStatsOpts::No)
    ImportedFunctionsStats.recordInline(*Caller, *Callee);

  AttributeFuncs::mergeAttributesForInlining(*Caller, *Callee);

  if (!DisableInlinedAllocaMerging)
    mergeInlinedArrayAllocas(Caller, IFI, InlinedArrayAllocas, InlineHistory);

  return true;
}

/// Return true if inlining of CS can block the caller from being
/// inlined which is proved to be more beneficial. \p IC is the
/// estimated inline cost associated with callsite \p CS.
/// \p TotalAltCost will be set to the estimated cost of inlining the caller
/// if \p CS is suppressed for inlining.
static bool
shouldBeDeferred(Function *Caller, CallSite CS, InlineCost IC,
                 int &TotalSecondaryCost,
                 function_ref<InlineCost(CallSite CS)> GetInlineCost) {

  // For now we only handle local or inline functions.
  if (!Caller->hasLocalLinkage() && !Caller->hasLinkOnceODRLinkage())
    return false;
  // Try to detect the case where the current inlining candidate caller (call
  // it B) is a static or linkonce-ODR function and is an inlining candidate
  // elsewhere, and the current candidate callee (call it C) is large enough
  // that inlining it into B would make B too big to inline later. In these
  // circumstances it may be best not to inline C into B, but to inline B into
  // its callers.
  //
  // This only applies to static and linkonce-ODR functions because those are
  // expected to be available for inlining in the translation units where they
  // are used. Thus we will always have the opportunity to make local inlining
  // decisions. Importantly the linkonce-ODR linkage covers inline functions
  // and templates in C++.
  //
  // FIXME: All of this logic should be sunk into getInlineCost. It relies on
  // the internal implementation of the inline cost metrics rather than
  // treating them as truly abstract units etc.
  TotalSecondaryCost = 0;
  // The candidate cost to be imposed upon the current function.
  int CandidateCost = IC.getCost() - (InlineConstants::CallPenalty + 1);
  // This bool tracks what happens if we do NOT inline C into B.
  bool callerWillBeRemoved = Caller->hasLocalLinkage();
  // This bool tracks what happens if we DO inline C into B.
  bool inliningPreventsSomeOuterInline = false;
  for (User *U : Caller->users()) {
    CallSite CS2(U);

    // If this isn't a call to Caller (it could be some other sort
    // of reference) skip it.  Such references will prevent the caller
    // from being removed.
    if (!CS2 || CS2.getCalledFunction() != Caller) {
      callerWillBeRemoved = false;
      continue;
    }

    InlineCost IC2 = GetInlineCost(CS2);
    ++NumCallerCallersAnalyzed;
    if (!IC2) {
      callerWillBeRemoved = false;
      continue;
    }
    if (IC2.isAlways())
      continue;

    // See if inlining of the original callsite would erase the cost delta of
    // this callsite. We subtract off the penalty for the call instruction,
    // which we would be deleting.
    if (IC2.getCostDelta() <= CandidateCost) {
      inliningPreventsSomeOuterInline = true;
      TotalSecondaryCost += IC2.getCost();
    }
  }
  // If all outer calls to Caller would get inlined, the cost for the last
  // one is set very low by getInlineCost, in anticipation that Caller will
  // be removed entirely.  We did not account for this above unless there
  // is only one caller of Caller.
  if (callerWillBeRemoved && !Caller->use_empty())
    TotalSecondaryCost -= InlineConstants::LastCallToStaticBonus;

  if (inliningPreventsSomeOuterInline && TotalSecondaryCost < IC.getCost())
    return true;

  return false;
}

/// Return true if the inliner should attempt to inline at the given CallSite.
static bool shouldInline(CallSite CS,
                         function_ref<InlineCost(CallSite CS)> GetInlineCost,
                         OptimizationRemarkEmitter &ORE) {
  using namespace ore;
  InlineCost IC = GetInlineCost(CS);
  Instruction *Call = CS.getInstruction();
  Function *Callee = CS.getCalledFunction();

  if (IC.isAlways()) {
    DEBUG(dbgs() << "    Inlining: cost=always"
                 << ", Call: " << *CS.getInstruction() << "\n");
    ORE.emit(OptimizationRemarkAnalysis(DEBUG_TYPE, "AlwaysInline", Call)
             << NV("Callee", Callee)
             << " should always be inlined (cost=always)");
    return true;
  }

  if (IC.isNever()) {
    DEBUG(dbgs() << "    NOT Inlining: cost=never"
                 << ", Call: " << *CS.getInstruction() << "\n");
    ORE.emit(OptimizationRemarkAnalysis(DEBUG_TYPE, "NeverInline", Call)
             << NV("Callee", Callee)
             << " should never be inlined (cost=never)");
    return false;
  }

  Function *Caller = CS.getCaller();
  if (!IC) {
    DEBUG(dbgs() << "    NOT Inlining: cost=" << IC.getCost()
                 << ", thres=" << (IC.getCostDelta() + IC.getCost())
                 << ", Call: " << *CS.getInstruction() << "\n");
    ORE.emit(OptimizationRemarkAnalysis(DEBUG_TYPE, "TooCostly", Call)
             << NV("Callee", Callee) << " too costly to inline (cost="
             << NV("Cost", IC.getCost()) << ", threshold="
             << NV("Threshold", IC.getCostDelta() + IC.getCost()) << ")");
    return false;
  }

  int TotalSecondaryCost = 0;
  if (shouldBeDeferred(Caller, CS, IC, TotalSecondaryCost, GetInlineCost)) {
    DEBUG(dbgs() << "    NOT Inlining: " << *CS.getInstruction()
                 << " Cost = " << IC.getCost()
                 << ", outer Cost = " << TotalSecondaryCost << '\n');
    ORE.emit(OptimizationRemarkAnalysis(DEBUG_TYPE,
                                        "IncreaseCostInOtherContexts", Call)
             << "Not inlining. Cost of inlining " << NV("Callee", Callee)
             << " increases the cost of inlining " << NV("Caller", Caller)
             << " in other contexts");
    return false;
  }

  DEBUG(dbgs() << "    Inlining: cost=" << IC.getCost()
               << ", thres=" << (IC.getCostDelta() + IC.getCost())
               << ", Call: " << *CS.getInstruction() << '\n');
  ORE.emit(OptimizationRemarkAnalysis(DEBUG_TYPE, "CanBeInlined", Call)
           << NV("Callee", Callee) << " can be inlined into "
           << NV("Caller", Caller) << " with cost=" << NV("Cost", IC.getCost())
           << " (threshold="
           << NV("Threshold", IC.getCostDelta() + IC.getCost()) << ")");
  return true;
}

/// Return true if the specified inline history ID
/// indicates an inline history that includes the specified function.
static bool InlineHistoryIncludes(
    Function *F, int InlineHistoryID,
    const SmallVectorImpl<std::pair<Function *, int>> &InlineHistory) {
  while (InlineHistoryID != -1) {
    assert(unsigned(InlineHistoryID) < InlineHistory.size() &&
           "Invalid inline history ID");
    if (InlineHistory[InlineHistoryID].first == F)
      return true;
    InlineHistoryID = InlineHistory[InlineHistoryID].second;
  }
  return false;
}

bool LegacyInlinerBase::doInitialization(CallGraph &CG) {
  if (InlinerFunctionImportStats != InlinerFunctionImportStatsOpts::No)
    ImportedFunctionsStats.setModuleInfo(CG.getModule());
  return false; // No changes to CallGraph.
}

bool LegacyInlinerBase::runOnSCC(CallGraphSCC &SCC) {
  if (skipSCC(SCC))
    return false;
  return inlineCalls(SCC);
}

static bool
inlineCallsImpl(CallGraphSCC &SCC, CallGraph &CG,
                std::function<AssumptionCache &(Function &)> GetAssumptionCache,
                ProfileSummaryInfo *PSI, TargetLibraryInfo &TLI,
                bool InsertLifetime,
                function_ref<InlineCost(CallSite CS)> GetInlineCost,
                function_ref<AAResults &(Function &)> AARGetter,
                ImportedFunctionsInliningStatistics &ImportedFunctionsStats) {
  SmallPtrSet<Function *, 8> SCCFunctions;
  DEBUG(dbgs() << "Inliner visiting SCC:");
  for (CallGraphNode *Node : SCC) {
    Function *F = Node->getFunction();
    if (F)
      SCCFunctions.insert(F);
    DEBUG(dbgs() << " " << (F ? F->getName() : "INDIRECTNODE"));
  }

  // Scan through and identify all call sites ahead of time so that we only
  // inline call sites in the original functions, not call sites that result
  // from inlining other functions.
  SmallVector<std::pair<CallSite, int>, 16> CallSites;

  // When inlining a callee produces new call sites, we want to keep track of
  // the fact that they were inlined from the callee.  This allows us to avoid
  // infinite inlining in some obscure cases.  To represent this, we use an
  // index into the InlineHistory vector.
  SmallVector<std::pair<Function *, int>, 8> InlineHistory;

  for (CallGraphNode *Node : SCC) {
    Function *F = Node->getFunction();
    if (!F || F->isDeclaration())
      continue;

    OptimizationRemarkEmitter ORE(F);
    for (BasicBlock &BB : *F)
      for (Instruction &I : BB) {
        CallSite CS(cast<Value>(&I));
        // If this isn't a call, or it is a call to an intrinsic, it can
        // never be inlined.
        if (!CS || isa<IntrinsicInst>(I))
          continue;

        // If this is a direct call to an external function, we can never inline
        // it.  If it is an indirect call, inlining may resolve it to be a
        // direct call, so we keep it.
        if (Function *Callee = CS.getCalledFunction())
          if (Callee->isDeclaration()) {
            using namespace ore;
            ORE.emit(OptimizationRemarkMissed(DEBUG_TYPE, "NoDefinition", &I)
                     << NV("Callee", Callee) << " will not be inlined into "
                     << NV("Caller", CS.getCaller())
                     << " because its definition is unavailable"
                     << setIsVerbose());
            continue;
          }

        CallSites.push_back(std::make_pair(CS, -1));
      }
  }

  DEBUG(dbgs() << ": " << CallSites.size() << " call sites.\n");

  // If there are no calls in this function, exit early.
  if (CallSites.empty())
    return false;

  // Now that we have all of the call sites, move the ones to functions in the
  // current SCC to the end of the list.
  unsigned FirstCallInSCC = CallSites.size();
  for (unsigned i = 0; i < FirstCallInSCC; ++i)
    if (Function *F = CallSites[i].first.getCalledFunction())
      if (SCCFunctions.count(F))
        std::swap(CallSites[i--], CallSites[--FirstCallInSCC]);

  InlinedArrayAllocasTy InlinedArrayAllocas;
  InlineFunctionInfo InlineInfo(&CG, &GetAssumptionCache);

  // Now that we have all of the call sites, loop over them and inline them if
  // it looks profitable to do so.
  bool Changed = false;
  bool LocalChange;
  do {
    LocalChange = false;
    // Iterate over the outer loop because inlining functions can cause indirect
    // calls to become direct calls.
    // CallSites may be modified inside so ranged for loop can not be used.
    for (unsigned CSi = 0; CSi != CallSites.size(); ++CSi) {
      CallSite CS = CallSites[CSi].first;

      Function *Caller = CS.getCaller();
      Function *Callee = CS.getCalledFunction();

      // If this call site is dead and it is to a readonly function, we should
      // just delete the call instead of trying to inline it, regardless of
      // size.  This happens because IPSCCP propagates the result out of the
      // call and then we're left with the dead call.
      if (isInstructionTriviallyDead(CS.getInstruction(), &TLI)) {
        DEBUG(dbgs() << "    -> Deleting dead call: " << *CS.getInstruction()
                     << "\n");
        // Update the call graph by deleting the edge from Callee to Caller.
        CG[Caller]->removeCallEdgeFor(CS);
        CS.getInstruction()->eraseFromParent();
        ++NumCallsDeleted;
      } else {
        // We can only inline direct calls to non-declarations.
        if (!Callee || Callee->isDeclaration())
          continue;

        // If this call site was obtained by inlining another function, verify
        // that the include path for the function did not include the callee
        // itself.  If so, we'd be recursively inlining the same function,
        // which would provide the same callsites, which would cause us to
        // infinitely inline.
        int InlineHistoryID = CallSites[CSi].second;
        if (InlineHistoryID != -1 &&
            InlineHistoryIncludes(Callee, InlineHistoryID, InlineHistory))
          continue;

        // Get DebugLoc to report. CS will be invalid after Inliner.
        DebugLoc DLoc = CS.getInstruction()->getDebugLoc();
        BasicBlock *Block = CS.getParent();
        // FIXME for new PM: because of the old PM we currently generate ORE and
        // in turn BFI on demand.  With the new PM, the ORE dependency should
        // just become a regular analysis dependency.
        OptimizationRemarkEmitter ORE(Caller);

        // If the policy determines that we should inline this function,
        // try to do so.
        using namespace ore;
        if (!shouldInline(CS, GetInlineCost, ORE)) {
          ORE.emit(
              OptimizationRemarkMissed(DEBUG_TYPE, "NotInlined", DLoc, Block)
              << NV("Callee", Callee) << " will not be inlined into "
              << NV("Caller", Caller));
          continue;
        }

        // Attempt to inline the function.
        if (!InlineCallIfPossible(CS, InlineInfo, InlinedArrayAllocas,
                                  InlineHistoryID, InsertLifetime, AARGetter,
                                  ImportedFunctionsStats)) {
          ORE.emit(
              OptimizationRemarkMissed(DEBUG_TYPE, "NotInlined", DLoc, Block)
              << NV("Callee", Callee) << " will not be inlined into "
              << NV("Caller", Caller));
          continue;
        }
        ++NumInlined;

        // Report the inline decision.
        ORE.emit(OptimizationRemark(DEBUG_TYPE, "Inlined", DLoc, Block)
                 << NV("Callee", Callee) << " inlined into "
                 << NV("Caller", Caller));

        // If inlining this function gave us any new call sites, throw them
        // onto our worklist to process.  They are useful inline candidates.
        if (!InlineInfo.InlinedCalls.empty()) {
          // Create a new inline history entry for this, so that we remember
          // that these new callsites came about due to inlining Callee.
          int NewHistoryID = InlineHistory.size();
          InlineHistory.push_back(std::make_pair(Callee, InlineHistoryID));

          for (Value *Ptr : InlineInfo.InlinedCalls)
            CallSites.push_back(std::make_pair(CallSite(Ptr), NewHistoryID));
        }
      }

      // If we inlined or deleted the last possible call site to the function,
      // delete the function body now.
      if (Callee && Callee->use_empty() && Callee->hasLocalLinkage() &&
          // TODO: Can remove if in SCC now.
          !SCCFunctions.count(Callee) &&

          // The function may be apparently dead, but if there are indirect
          // callgraph references to the node, we cannot delete it yet, this
          // could invalidate the CGSCC iterator.
          CG[Callee]->getNumReferences() == 0) {
        DEBUG(dbgs() << "    -> Deleting dead function: " << Callee->getName()
                     << "\n");
        CallGraphNode *CalleeNode = CG[Callee];

        // Remove any call graph edges from the callee to its callees.
        CalleeNode->removeAllCalledFunctions();

        // Removing the node for callee from the call graph and delete it.
        delete CG.removeFunctionFromModule(CalleeNode);
        ++NumDeleted;
      }

      // Remove this call site from the list.  If possible, use
      // swap/pop_back for efficiency, but do not use it if doing so would
      // move a call site to a function in this SCC before the
      // 'FirstCallInSCC' barrier.
      if (SCC.isSingular()) {
        CallSites[CSi] = CallSites.back();
        CallSites.pop_back();
      } else {
        CallSites.erase(CallSites.begin() + CSi);
      }
      --CSi;

      Changed = true;
      LocalChange = true;
    }
  } while (LocalChange);

  return Changed;
}

bool LegacyInlinerBase::inlineCalls(CallGraphSCC &SCC) {
  CallGraph &CG = getAnalysis<CallGraphWrapperPass>().getCallGraph();
  ACT = &getAnalysis<AssumptionCacheTracker>();
  PSI = getAnalysis<ProfileSummaryInfoWrapperPass>().getPSI();
  auto &TLI = getAnalysis<TargetLibraryInfoWrapperPass>().getTLI();
  // We compute dedicated AA results for each function in the SCC as needed. We
  // use a lambda referencing external objects so that they live long enough to
  // be queried, but we re-use them each time.
  Optional<BasicAAResult> BAR;
  Optional<AAResults> AAR;
  auto AARGetter = [&](Function &F) -> AAResults & {
    BAR.emplace(createLegacyPMBasicAAResult(*this, F));
    AAR.emplace(createLegacyPMAAResults(*this, F, *BAR));
    return *AAR;
  };
  auto GetAssumptionCache = [&](Function &F) -> AssumptionCache & {
    return ACT->getAssumptionCache(F);
  };
  return inlineCallsImpl(SCC, CG, GetAssumptionCache, PSI, TLI, InsertLifetime,
                         [this](CallSite CS) { return getInlineCost(CS); },
                         AARGetter, ImportedFunctionsStats);
}

/// Remove now-dead linkonce functions at the end of
/// processing to avoid breaking the SCC traversal.
bool LegacyInlinerBase::doFinalization(CallGraph &CG) {
  if (InlinerFunctionImportStats != InlinerFunctionImportStatsOpts::No)
    ImportedFunctionsStats.dump(InlinerFunctionImportStats ==
                                InlinerFunctionImportStatsOpts::Verbose);
  return removeDeadFunctions(CG);
}

/// Remove dead functions that are not included in DNR (Do Not Remove) list.
bool LegacyInlinerBase::removeDeadFunctions(CallGraph &CG,
                                            bool AlwaysInlineOnly) {
  SmallVector<CallGraphNode *, 16> FunctionsToRemove;
  SmallVector<Function *, 16> DeadFunctionsInComdats;

  auto RemoveCGN = [&](CallGraphNode *CGN) {
    // Remove any call graph edges from the function to its callees.
    CGN->removeAllCalledFunctions();

    // Remove any edges from the external node to the function's call graph
    // node.  These edges might have been made irrelegant due to
    // optimization of the program.
    CG.getExternalCallingNode()->removeAnyCallEdgeTo(CGN);

    // Removing the node for callee from the call graph and delete it.
    FunctionsToRemove.push_back(CGN);
  };

  // Scan for all of the functions, looking for ones that should now be removed
  // from the program.  Insert the dead ones in the FunctionsToRemove set.
  for (const auto &I : CG) {
    CallGraphNode *CGN = I.second.get();
    Function *F = CGN->getFunction();
    if (!F || F->isDeclaration())
      continue;

    // Handle the case when this function is called and we only want to care
    // about always-inline functions. This is a bit of a hack to share code
    // between here and the InlineAlways pass.
    if (AlwaysInlineOnly && !F->hasFnAttribute(Attribute::AlwaysInline))
      continue;

    // If the only remaining users of the function are dead constants, remove
    // them.
    F->removeDeadConstantUsers();

    if (!F->isDefTriviallyDead())
      continue;

    // It is unsafe to drop a function with discardable linkage from a COMDAT
    // without also dropping the other members of the COMDAT.
    // The inliner doesn't visit non-function entities which are in COMDAT
    // groups so it is unsafe to do so *unless* the linkage is local.
    if (!F->hasLocalLinkage()) {
      if (F->hasComdat()) {
        DeadFunctionsInComdats.push_back(F);
        continue;
      }
    }

    RemoveCGN(CGN);
  }
  if (!DeadFunctionsInComdats.empty()) {
    // Filter out the functions whose comdats remain alive.
    filterDeadComdatFunctions(CG.getModule(), DeadFunctionsInComdats);
    // Remove the rest.
    for (Function *F : DeadFunctionsInComdats)
      RemoveCGN(CG[F]);
  }

  if (FunctionsToRemove.empty())
    return false;

  // Now that we know which functions to delete, do so.  We didn't want to do
  // this inline, because that would invalidate our CallGraph::iterator
  // objects. :(
  //
  // Note that it doesn't matter that we are iterating over a non-stable order
  // here to do this, it doesn't matter which order the functions are deleted
  // in.
  array_pod_sort(FunctionsToRemove.begin(), FunctionsToRemove.end());
  FunctionsToRemove.erase(
      std::unique(FunctionsToRemove.begin(), FunctionsToRemove.end()),
      FunctionsToRemove.end());
  for (CallGraphNode *CGN : FunctionsToRemove) {
    delete CG.removeFunctionFromModule(CGN);
    ++NumDeleted;
  }
  return true;
}

PreservedAnalyses InlinerPass::run(LazyCallGraph::SCC &InitialC,
                                   CGSCCAnalysisManager &AM, LazyCallGraph &CG,
                                   CGSCCUpdateResult &UR) {
  FunctionAnalysisManager &FAM =
      AM.getResult<FunctionAnalysisManagerCGSCCProxy>(InitialC, CG)
          .getManager();
  const ModuleAnalysisManager &MAM =
      AM.getResult<ModuleAnalysisManagerCGSCCProxy>(InitialC, CG).getManager();
  bool Changed = false;

  assert(InitialC.size() > 0 && "Cannot handle an empty SCC!");
  Module &M = *InitialC.begin()->getFunction().getParent();
  ProfileSummaryInfo *PSI = MAM.getCachedResult<ProfileSummaryAnalysis>(M);

  std::function<AssumptionCache &(Function &)> GetAssumptionCache =
      [&](Function &F) -> AssumptionCache & {
    return FAM.getResult<AssumptionAnalysis>(F);
  };

  // Setup the data structure used to plumb customization into the
  // `InlineFunction` routine.
  InlineFunctionInfo IFI(/*cg=*/nullptr, &GetAssumptionCache);

  auto GetInlineCost = [&](CallSite CS) {
    Function &Callee = *CS.getCalledFunction();
    auto &CalleeTTI = FAM.getResult<TargetIRAnalysis>(Callee);
    return getInlineCost(CS, Params, CalleeTTI, GetAssumptionCache, PSI);
  };

  // We use a worklist of nodes to process so that we can handle if the SCC
  // structure changes and some nodes are no longer part of the current SCC. We
  // also need to use an updatable pointer for the SCC as a consequence.
  SmallVector<LazyCallGraph::Node *, 16> Nodes;
  for (auto &N : InitialC)
    Nodes.push_back(&N);
  auto *C = &InitialC;
  auto *RC = &C->getOuterRefSCC();

  // We also use a secondary worklist of call sites within a particular node to
  // allow quickly continuing to inline through newly inlined call sites where
  // possible.
  SmallVector<std::pair<CallSite, int>, 16> Calls;

  // When inlining a callee produces new call sites, we want to keep track of
  // the fact that they were inlined from the callee.  This allows us to avoid
  // infinite inlining in some obscure cases.  To represent this, we use an
  // index into the InlineHistory vector.
  SmallVector<std::pair<Function *, int>, 16> InlineHistory;

  // Track a set vector of inlined callees so that we can augment the caller
  // with all of their edges in the call graph before pruning out the ones that
  // got simplified away.
  SmallSetVector<Function *, 4> InlinedCallees;

  // Track the dead functions to delete once finished with inlining calls. We
  // defer deleting these to make it easier to handle the call graph updates.
  SmallVector<Function *, 4> DeadFunctions;

  do {
    auto &N = *Nodes.pop_back_val();
    if (CG.lookupSCC(N) != C)
      continue;
    Function &F = N.getFunction();
    if (F.hasFnAttribute(Attribute::OptimizeNone))
      continue;

    // Get the remarks emission analysis for the caller.
    auto &ORE = FAM.getResult<OptimizationRemarkEmitterAnalysis>(F);

    // We want to generally process call sites top-down in order for
    // simplifications stemming from replacing the call with the returned value
    // after inlining to be visible to subsequent inlining decisions. So we
    // walk the function backwards and then process the back of the vector.
    // FIXME: Using reverse is a really bad way to do this. Instead we should
    // do an actual PO walk of the function body.
    for (Instruction &I : reverse(instructions(F)))
      if (auto CS = CallSite(&I))
        if (Function *Callee = CS.getCalledFunction())
          if (!Callee->isDeclaration())
            Calls.push_back({CS, -1});

    bool DidInline = false;
    while (!Calls.empty()) {
      int InlineHistoryID;
      CallSite CS;
      std::tie(CS, InlineHistoryID) = Calls.pop_back_val();
      Function &Callee = *CS.getCalledFunction();

      if (InlineHistoryID != -1 &&
          InlineHistoryIncludes(&Callee, InlineHistoryID, InlineHistory))
        continue;

      // Check whether we want to inline this callsite.
      if (!shouldInline(CS, GetInlineCost, ORE))
        continue;

      if (!InlineFunction(CS, IFI))
        continue;
      DidInline = true;
      InlinedCallees.insert(&Callee);

      // Add any new callsites to defined functions to the worklist.
      if (!IFI.InlinedCallSites.empty()) {
        int NewHistoryID = InlineHistory.size();
        InlineHistory.push_back({&Callee, InlineHistoryID});
        for (CallSite &CS : reverse(IFI.InlinedCallSites))
          if (Function *NewCallee = CS.getCalledFunction())
            if (!NewCallee->isDeclaration())
              Calls.push_back({CS, NewHistoryID});
      }

      // Merge the attributes based on the inlining.
      AttributeFuncs::mergeAttributesForInlining(F, Callee);

      // For local functions, check whether this makes the callee trivially
      // dead. In that case, we can drop the body of the function eagerly
      // which may reduce the number of callers of other functions to one,
      // changing inline cost thresholds.
      if (Callee.hasLocalLinkage()) {
        // To check this we also need to nuke any dead constant uses (perhaps
        // made dead by this operation on other functions).
        Callee.removeDeadConstantUsers();
        if (Callee.use_empty()) {
          // Clear the body and queue the function itself for deletion when we
          // finish inlining and call graph updates.
          // Note that after this point, it is an error to do anything other
          // than use the callee's address or delete it.
          Callee.dropAllReferences();
          assert(find(DeadFunctions, &Callee) == DeadFunctions.end() &&
                 "Cannot put cause a function to become dead twice!");
          DeadFunctions.push_back(&Callee);
        }
      }
    }

    if (!DidInline)
      continue;
    Changed = true;

    // Add all the inlined callees' edges to the caller. These are by
    // definition trivial edges as we already had a transitive call edge to the
    // callee.
    for (Function *InlinedCallee : InlinedCallees) {
      LazyCallGraph::Node &CalleeN = *CG.lookup(*InlinedCallee);
      for (LazyCallGraph::Edge &E : CalleeN)
        if (E.isCall())
          RC->insertTrivialCallEdge(N, *E.getNode());
        else
          RC->insertTrivialRefEdge(N, *E.getNode());
    }
    InlinedCallees.clear();

    // At this point, since we have made changes we have at least removed
    // a call instruction. However, in the process we do some incremental
    // simplification of the surrounding code. This simplification can
    // essentially do all of the same things as a function pass and we can
    // re-use the exact same logic for updating the call graph to reflect the
    // change..
    C = &updateCGAndAnalysisManagerForFunctionPass(CG, *C, N, AM, UR);
    RC = &C->getOuterRefSCC();
  } while (!Nodes.empty());

  // Now that we've finished inlining all of the calls across this SCC, delete
  // all of the trivially dead functions, updating the call graph and the CGSCC
  // pass manager in the process.
  //
  // Note that this walks a pointer set which has non-deterministic order but
  // that is OK as all we do is delete things and add pointers to unordered
  // sets.
  for (Function *DeadF : DeadFunctions) {
    // Get the necessary information out of the call graph and nuke the
    // function there.
    auto &DeadC = *CG.lookupSCC(*CG.lookup(*DeadF));
    auto &DeadRC = DeadC.getOuterRefSCC();
    CG.removeDeadFunction(*DeadF);

    // Mark the relevant parts of the call graph as invalid so we don't visit
    // them.
    UR.InvalidatedSCCs.insert(&DeadC);
    UR.InvalidatedRefSCCs.insert(&DeadRC);

    // And delete the actual function from the module.
    M.getFunctionList().erase(DeadF);
  }
  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
