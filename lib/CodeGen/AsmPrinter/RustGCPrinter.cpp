//===-- RustGCPrinter.cpp - Rust frametable emitter -----------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements printing the assembly code for a Rust frametable.
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/GCs.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/GCMetadataPrinter.h"
#include "llvm/Module.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/Target/Mangler.h"
#include "llvm/Target/TargetData.h"
#include "llvm/Target/TargetLoweringObjectFile.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/ADT/SmallString.h"

using namespace llvm;

namespace {

class RustGCMetadataPrinter : public GCMetadataPrinter {
public:
  void beginAssembly(AsmPrinter &AP);
  void finishAssembly(AsmPrinter &AP);

private:
  void emitRustGlobal(const Module &M, AsmPrinter &AP, const char *Name);
  void alignToPointer(AsmPrinter &AP);
  unsigned getFrameDescriptorCount();
  unsigned getCalleeSaveCount();
  void emitFrameDescriptors(AsmPrinter &AP);
  void emitCalleeSaves(AsmPrinter &AP);
  void emitRoots(AsmPrinter &AP);
};

GCMetadataPrinterRegistry::Add<RustGCMetadataPrinter>
Y("rust", "Rust-compatible collector");

void RustGCMetadataPrinter::emitRustGlobal(const Module &M, AsmPrinter &AP,
                                           const char *Name) {
  const std::string &MId = M.getModuleIdentifier();

  std::string SymName("_rust_");
  SymName += Name;
  SymName += "_";
  SymName += MId;

  SmallString<128> Mangled;
  AP.Mang->getNameWithPrefix(Mangled, SymName);

  MCSymbol *MCSym = AP.OutContext.GetOrCreateSymbol(Mangled);

  AP.OutStreamer.EmitSymbolAttribute(MCSym, MCSA_Global);
  AP.OutStreamer.EmitLabel(MCSym);
}

void RustGCMetadataPrinter::beginAssembly(AsmPrinter &AP) {
  // No-op.
}

/// Print the frametable. This is the format of a Rust frametable:
///
///   struct rust_callee_save {
///       uint32_t loc;   // stack offset
///       uint32_t reg;   // register number
///   };
///
///   struct rust_root {
///       int32_t loc;   // positive = stack offset, negative = register
///       uint32_t type; // type ID
///   };
///
///   struct rust_frame_desc {
///       void (*ret_addr)();
///       uint32_t first_callee_save;   // index of first callee saved reg
///       uint32_t n_callee_saves;      // number of callee-saved registers;
///                                     // these come first
///       uint32_t first_root;          // index of first GC root
///       uint32_t n_roots;             // number of GC roots
///   };
///
///   struct rust_frametable {
///       uint32_t n_frame_descs;
///       uint32_t n_callee_saves;
///       rust_frame_desc frame_descs[n_frame_descs];
///       rust_callee_save callee_saves[n_callee_saves];
///       rust_root roots[];
///   };
///
void RustGCMetadataPrinter::finishAssembly(AsmPrinter &AP) {
  AP.OutStreamer.SwitchSection(AP.getObjFileLowering().getDataSection());

  alignToPointer(AP);
  emitRustGlobal(getModule(), AP, "frametable");

  // n_frame_descs
  AP.EmitInt32(getFrameDescriptorCount());

  // n_callee_saves
  AP.EmitInt32(getCalleeSaveCount());

  // frame_descs
  emitFrameDescriptors(AP);

  // callee_saves
  emitCalleeSaves(AP);

  // roots
  emitRoots(AP);
}

void RustGCMetadataPrinter::alignToPointer(AsmPrinter &AP) {
  AP.EmitAlignment(AP.TM.getTargetData()->getPointerSize() == 4 ? 2 : 3);
}

unsigned RustGCMetadataPrinter::getFrameDescriptorCount() {
  unsigned Count = 0;
  for (iterator FB = begin(), FE = end(); FB != FE; ++FB) {
    GCFunctionInfo *FI = *FB;
    for (GCFunctionInfo::iterator PB = FI->begin(), PE = FI->end();
         PB != PE; ++PB) {
      Count++;
    }
  }
  return Count;
}

unsigned RustGCMetadataPrinter::getCalleeSaveCount() {
  unsigned Count = 0;
  for (iterator FB = begin(), FE = end(); FB != FE; ++FB) {
    GCFunctionInfo *FI = *FB;
    Count += FI->getCalleeSaves().size();
  }
  return Count;
}

void RustGCMetadataPrinter::emitFrameDescriptors(AsmPrinter &AP) {
  unsigned CalleeSaveCount = 0, RootCount = 0;
  for (iterator FB = begin(), FE = end(); FB != FE; ++FB) {
    GCFunctionInfo *FI = *FB;
    unsigned CSC = FI->getCalleeSaves().size();

    for (GCFunctionInfo::iterator PB = FI->begin(), PE = FI->end();
         PB != PE; ++PB) {
      AP.OutStreamer.AddComment("Frame descriptor for " +
                                Twine(FI->getFunction().getName()));
      AP.OutStreamer.AddBlankLine();

      alignToPointer(AP);

      // ret_addr
      unsigned PointerSize = AP.TM.getTargetData()->getPointerSize();
      AP.OutStreamer.EmitSymbolValue(PB->Label, PointerSize);

      // first_callee_save
      AP.EmitInt32(CalleeSaveCount);

      // n_callee_saves
      AP.EmitInt32(CSC);

      // first_root
      AP.EmitInt32(RootCount);

      // n_roots
      unsigned RC = PB->size();
      AP.EmitInt32(RC);
      RootCount += RC;
    }

    CalleeSaveCount += CSC;
  }
}

void RustGCMetadataPrinter::emitCalleeSaves(AsmPrinter &AP) {
  for (iterator FB = begin(), FE = end(); FB != FE; ++FB) {
    GCFunctionInfo *FI = *FB;
    AP.OutStreamer.AddComment("Callee saves for " +
                              Twine(FI->getFunction().getName()));
    AP.OutStreamer.AddBlankLine();

    const std::vector<GCCalleeSave> &CSs = FI->getCalleeSaves();
    for (std::vector<GCCalleeSave>::const_iterator CSB = CSs.begin(),
                                                   CSE = CSs.end();
            CSB != CSE; ++CSB) {
      AP.EmitInt8(CSB->Reg);
      AP.EmitInt8(CSB->Loc);
    }
  }
}

void RustGCMetadataPrinter::emitRoots(AsmPrinter &AP) {
  alignToPointer(AP);

  for (iterator FB = begin(), FE = end(); FB != FE; ++FB) {
    GCFunctionInfo *FI = *FB;
    for (GCFunctionInfo::iterator PB = FI->begin(), PE = FI->end();
         PB != PE; ++PB) {
      AP.OutStreamer.AddComment("GC roots for " +
                                Twine(FI->getFunction().getName()));
      AP.OutStreamer.AddBlankLine();

      for (GCPoint::iterator RB = PB->begin(), RE = PB->end();
           RB != RE; ++RB) {
        AP.EmitInt32(-RB->Data);          // loc
        // FIXME: emit constant
        //AP.EmitInt32(RB->Metadata.Num);   // type
      }
    }
  }
}

} // end unnamed namespace

void llvm::linkRustGCPrinter() { }

