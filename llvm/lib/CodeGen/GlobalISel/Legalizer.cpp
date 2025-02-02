//===-- llvm/CodeGen/GlobalISel/Legalizer.cpp -----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file This file implements the LegalizerHelper class to legalize individual
/// instructions and the LegalizePass wrapper pass for the primary
/// legalization.
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/GlobalISel/Legalizer.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/CodeGen/GlobalISel/CSEInfo.h"
#include "llvm/CodeGen/GlobalISel/CSEMIRBuilder.h"
#include "llvm/CodeGen/GlobalISel/GISelChangeObserver.h"
#include "llvm/CodeGen/GlobalISel/GISelWorkList.h"
#include "llvm/CodeGen/GlobalISel/LegalizationArtifactCombiner.h"
#include "llvm/CodeGen/GlobalISel/LegalizerHelper.h"
#include "llvm/CodeGen/GlobalISel/Utils.h"
#include "llvm/CodeGen/MachineOptimizationRemarkEmitter.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/TargetPassConfig.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/InitializePasses.h"
#include "llvm/Support/Debug.h"
#include "llvm/Target/TargetMachine.h"

#include <iterator>

#define DEBUG_TYPE "legalizer"

using namespace llvm;

static cl::opt<bool>
    EnableCSEInLegalizer("enable-cse-in-legalizer",
                         cl::desc("Should enable CSE in Legalizer"),
                         cl::Optional, cl::init(false));

char Legalizer::ID = 0;
INITIALIZE_PASS_BEGIN(Legalizer, DEBUG_TYPE,
                      "Legalize the Machine IR a function's Machine IR", false,
                      false)
INITIALIZE_PASS_DEPENDENCY(TargetPassConfig)
INITIALIZE_PASS_DEPENDENCY(GISelCSEAnalysisWrapperPass)
INITIALIZE_PASS_END(Legalizer, DEBUG_TYPE,
                    "Legalize the Machine IR a function's Machine IR", false,
                    false)

Legalizer::Legalizer() : MachineFunctionPass(ID) { }

void Legalizer::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TargetPassConfig>();
  AU.addRequired<GISelCSEAnalysisWrapperPass>();
  AU.addPreserved<GISelCSEAnalysisWrapperPass>();
  getSelectionDAGFallbackAnalysisUsage(AU);
  MachineFunctionPass::getAnalysisUsage(AU);
}

void Legalizer::init(MachineFunction &MF) {
}

static bool isArtifact(const MachineInstr &MI) {
  switch (MI.getOpcode()) {
  default:
    return false;
  case TargetOpcode::G_IMPLICIT_DEF:
  case TargetOpcode::G_TRUNC:
  case TargetOpcode::G_ZEXT:
  case TargetOpcode::G_ANYEXT:
  case TargetOpcode::G_SEXT:
  case TargetOpcode::G_MERGE_VALUES:
  case TargetOpcode::G_UNMERGE_VALUES:
  case TargetOpcode::G_CONCAT_VECTORS:
  case TargetOpcode::G_BUILD_VECTOR:
  case TargetOpcode::G_EXTRACT:
  case TargetOpcode::G_INSERT:
    return true;
  }
}
static bool isPreISelGenericOpcodeOrCopy(unsigned Opcode) {
  return isPreISelGenericOpcode(Opcode) || Opcode == TargetOpcode::COPY;
}
using InstListTy = GISelWorkList<256>;
using ArtifactListTy = GISelWorkList<128>;

namespace {
class LegalizerWorkListManager : public GISelChangeObserver {
  InstListTy &InstList;
  ArtifactListTy &ArtifactList;
  MachineRegisterInfo &MRI;
#ifndef NDEBUG
  SmallVector<MachineInstr *, 4> NewMIs;
#endif

public:
  LegalizerWorkListManager(InstListTy &Insts, ArtifactListTy &Arts,
                           MachineRegisterInfo &MRI)
      : InstList(Insts), ArtifactList(Arts), MRI(MRI) {}

  void createdOrChangedInstr(MachineInstr &MI) {
    // Only legalize pre-isel generic instructions.
    // Legalization process could generate Target specific pseudo
    // instructions with generic types. Don't record them
    // Do record copies in case they become dead and use an artifact.
    if (isPreISelGenericOpcodeOrCopy(MI.getOpcode())) {
      if (isArtifact(MI))
        ArtifactList.insert(&MI);
      else
        InstList.insert(&MI);
    }
  }

  void maybeRemovingUses(MachineInstr &MI) {
    for (const MachineOperand &MO : MI.explicit_uses())
      if (MO.isReg() && MO.getReg().isVirtual())
        if (MachineInstr *DefMI = MRI.getVRegDef(MO.getReg()))
          createdOrChangedInstr(*DefMI);
  }

  void createdInstr(MachineInstr &MI) override {
    LLVM_DEBUG(dbgs() << ".. .. New MI: " << MI);
    LLVM_DEBUG(NewMIs.push_back(&MI));
    createdOrChangedInstr(MI);
  }

  void printNewInstrs() {
    LLVM_DEBUG({
      for (const auto *MI : NewMIs)
        dbgs() << ".. .. New MI: " << *MI;
      NewMIs.clear();
    });
  }

  void erasingInstr(MachineInstr &MI) override {
    LLVM_DEBUG(dbgs() << ".. .. Erasing: " << MI);
    maybeRemovingUses(MI);
    InstList.remove(&MI);
    ArtifactList.remove(&MI);
  }

  void changingInstr(MachineInstr &MI) override {
    LLVM_DEBUG(dbgs() << ".. .. Changing MI: " << MI);
    maybeRemovingUses(MI);
  }

  void changedInstr(MachineInstr &MI) override {
    // When insts change, we want to revisit them to legalize them again.
    // We'll consider them the same as created.
    LLVM_DEBUG(dbgs() << ".. .. Changed MI: " << MI);
    createdOrChangedInstr(MI);
  }
};
} // namespace

bool Legalizer::runOnMachineFunction(MachineFunction &MF) {
  // If the ISel pipeline failed, do not bother running that pass.
  if (MF.getProperties().hasProperty(
          MachineFunctionProperties::Property::FailedISel))
    return false;
  LLVM_DEBUG(dbgs() << "Legalize Machine IR for: " << MF.getName() << '\n');
  init(MF);
  const TargetPassConfig &TPC = getAnalysis<TargetPassConfig>();
  GISelCSEAnalysisWrapper &Wrapper =
      getAnalysis<GISelCSEAnalysisWrapperPass>().getCSEWrapper();
  MachineOptimizationRemarkEmitter MORE(MF, /*MBFI=*/nullptr);

  const size_t NumBlocks = MF.size();
  MachineRegisterInfo &MRI = MF.getRegInfo();

  // Populate Insts
  InstListTy InstList;
  ArtifactListTy ArtifactList;
  ReversePostOrderTraversal<MachineFunction *> RPOT(&MF);
  // Perform legalization bottom up so we can DCE as we legalize.
  // Traverse BB in RPOT and within each basic block, add insts top down,
  // so when we pop_back_val in the legalization process, we traverse bottom-up.
  for (auto *MBB : RPOT) {
    if (MBB->empty())
      continue;
    for (MachineInstr &MI : *MBB) {
      // Only legalize pre-isel generic instructions: others don't have types
      // and are assumed to be legal.
      if (!isPreISelGenericOpcodeOrCopy(MI.getOpcode()))
        continue;
      if (isArtifact(MI))
        ArtifactList.deferred_insert(&MI);
      else
        InstList.deferred_insert(&MI);
    }
  }
  ArtifactList.finalize();
  InstList.finalize();
  std::unique_ptr<MachineIRBuilder> MIRBuilder;
  GISelCSEInfo *CSEInfo = nullptr;
  bool EnableCSE = EnableCSEInLegalizer.getNumOccurrences()
                       ? EnableCSEInLegalizer
                       : TPC.isGISelCSEEnabled();

  if (EnableCSE) {
    MIRBuilder = std::make_unique<CSEMIRBuilder>();
    CSEInfo = &Wrapper.get(TPC.getCSEConfig());
    MIRBuilder->setCSEInfo(CSEInfo);
  } else
    MIRBuilder = std::make_unique<MachineIRBuilder>();
  // This observer keeps the worklist updated.
  LegalizerWorkListManager WorkListObserver(InstList, ArtifactList, MRI);
  // We want both WorkListObserver as well as CSEInfo to observe all changes.
  // Use the wrapper observer.
  GISelObserverWrapper WrapperObserver(&WorkListObserver);
  if (EnableCSE && CSEInfo)
    WrapperObserver.addObserver(CSEInfo);
  // Now install the observer as the delegate to MF.
  // This will keep all the observers notified about new insertions/deletions.
  RAIIDelegateInstaller DelInstall(MF, &WrapperObserver);
  LegalizerHelper Helper(MF, WrapperObserver, *MIRBuilder.get());
  const LegalizerInfo &LInfo(Helper.getLegalizerInfo());
  LegalizationArtifactCombiner ArtCombiner(*MIRBuilder.get(), MF.getRegInfo(),
                                           LInfo);
  auto RemoveDeadInstFromLists = [&](MachineInstr &DeadMI) {
    LLVM_DEBUG(dbgs() << DeadMI << "Is dead\n");
    WrapperObserver.erasingInstr(DeadMI);
    DeadMI.eraseFromParentAndMarkDBGValuesForRemoval();
  };
  auto stopLegalizing = [&](MachineInstr &MI) {
    Helper.MIRBuilder.stopObservingChanges();
    reportGISelFailure(MF, TPC, MORE, "gisel-legalize",
                       "unable to legalize instruction", MI);
  };
  bool Changed = false;
  SmallVector<MachineInstr *, 128> RetryList;
  do {
    assert(RetryList.empty() && "Expected no instructions in RetryList");
    unsigned NumArtifacts = ArtifactList.size();
    while (!InstList.empty()) {
      MachineInstr &MI = *InstList.pop_back_val();
      assert(MI.getParent() && "Instruction deleted?");
      assert(isPreISelGenericOpcodeOrCopy(MI.getOpcode()) &&
             "Expecting generic opcode or copy");
      if (isTriviallyDead(MI, MRI)) {
        RemoveDeadInstFromLists(MI);
        continue;
      }

      // Do the legalization for this instruction.
      auto Res = Helper.legalizeInstrStep(MI);
      // Error out if we couldn't legalize this instruction. We may want to
      // fall back to DAG ISel instead in the future.
      if (Res == LegalizerHelper::UnableToLegalize) {
        // Move illegal artifacts to RetryList instead of aborting because
        // legalizing InstList may generate artifacts that allow
        // ArtifactCombiner to combine away them.
        if (isArtifact(MI)) {
          RetryList.push_back(&MI);
          continue;
        }
        stopLegalizing(MI);
        return false;
      }
      WorkListObserver.printNewInstrs();
      Changed |= Res == LegalizerHelper::Legalized;
    }
    // Try to combine the instructions in RetryList again if there
    // are new artifacts. If not, stop legalizing.
    if (!RetryList.empty()) {
      if (ArtifactList.size() > NumArtifacts) {
        while (!RetryList.empty())
          ArtifactList.insert(RetryList.pop_back_val());
      } else {
        MachineInstr *MI = *RetryList.begin();
        stopLegalizing(*MI);
        return false;
      }
    }
    while (!ArtifactList.empty()) {
      MachineInstr &MI = *ArtifactList.pop_back_val();
      assert(MI.getParent() && "Instruction deleted?");
      assert(isArtifact(MI) && "Expecting artifact");
      if (isTriviallyDead(MI, MRI)) {
        RemoveDeadInstFromLists(MI);
        continue;
      }
      SmallVector<MachineInstr *, 4> DeadInstructions;
      if (ArtCombiner.tryCombineInstruction(MI, DeadInstructions,
                                            WrapperObserver)) {
        WorkListObserver.printNewInstrs();
        for (auto *DeadMI : DeadInstructions)
          RemoveDeadInstFromLists(*DeadMI);
        Changed = true;
        continue;
      }
      // If this was not an artifact (that could be combined away), this might
      // need special handling. Add it to InstList, so when it's processed
      // there, it has to be legal or specially handled.
      else
        InstList.insert(&MI);
    }
  } while (!InstList.empty());

  // For now don't support if new blocks are inserted - we would need to fix the
  // outerloop for that.
  if (MF.size() != NumBlocks) {
    MachineOptimizationRemarkMissed R("gisel-legalize", "GISelFailure",
                                      MF.getFunction().getSubprogram(),
                                      /*MBB=*/nullptr);
    R << "inserting blocks is not supported yet";
    reportGISelFailure(MF, TPC, MORE, R);
    return false;
  }

  return Changed;
}
