//===-- GCMetadata.h - Garbage collector metadata ---------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file declares the GCFunctionInfo and GCModuleInfo classes, which are
// used as a communication channel from the target code generator to the target
// garbage collectors. This interface allows code generators and garbage
// collectors to be developed independently.
//
// The GCFunctionInfo class logs the data necessary to build a type accurate
// stack map. The code generator outputs:
//
//   - Safe points as specified by the GCStrategy's NeededSafePoints.
//   - Stack offsets for GC roots, as specified by calls to llvm.gcroot or
//     noteroot attributes
//
// As a refinement, liveness analysis calculates the set of live roots at each
// safe point. Liveness analysis is not presently performed by the code
// generator, so all roots are assumed live.
//
// GCModuleInfo simply collects GCFunctionInfo instances for each Function as
// they are compiled. This accretion is necessary for collectors which must emit
// a stack map for the compilation unit as a whole. Therefore, GCFunctionInfo
// outlives the MachineFunction from which it is derived and must not refer to
// any code generator data structures.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CODEGEN_GCMETADATA_H
#define LLVM_CODEGEN_GCMETADATA_H

#include "llvm/Pass.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/Support/DebugLoc.h"

namespace llvm {
  class AsmPrinter;
  class GCStrategy;
  class Constant;
  class MCSymbol;
  class VirtRegMap;

  namespace GC {
    /// PointKind - The type of a collector-safe point.
    ///
    enum PointKind {
      Loop,    //< Instr is a loop (backwards branch).
      Return,  //< Instr is a return instruction.
      PreCall, //< Instr is a call instruction.
      PostCall //< Instr is the return address of a call.
    };
  }

  /// GCRoot - Metadata for a pointer to an object managed by the garbage
  /// collector.
  ///
  struct GCRoot {
    bool Stack;                 //< True if on the stack, false otherwise.
    bool Phys;                  //< True if the register is physical, false if
                                //  the register is virtual.
    unsigned Data;              //< Stack offset if on stack, register number
                                //  otherwise.
    const Constant *Metadata;   //< Metadata associated with this root.

    GCRoot(bool S, const Constant *MD, unsigned D)
        : Stack(S), Phys(false), Data(D), Metadata(MD) {}
  };

  /// GCPoint - Metadata for a collector-safe point in machine code.
  ///
  class GCPoint {
  public:
    typedef std::vector<GCRoot>::iterator iterator;

    GC::PointKind Kind;         //< The kind of the safe point.
    MCSymbol *Label;            //< A label.
    DebugLoc Loc;

    GCPoint(GC::PointKind K, MCSymbol *L, DebugLoc DL)
        : Kind(K), Label(L), Loc(DL) {}

    /// begin/end - Iterators for roots at a safe point.
    ///
    iterator begin()      { return Roots.begin(); }
    iterator end()        { return Roots.end();   }

    unsigned size() const { return Roots.size();  }

    GCRoot *getRootNum(unsigned i) { return &Roots[i]; }
    void addRoot(GCRoot &NewRoot) { Roots.push_back(NewRoot); }

  private:
    std::vector<GCRoot> Roots;  //< The GC roots at this safe point.
  };

  /// GCGlobalRoot - Metadata for a garbage collected global root.
  ///
  struct GCGlobalRoot {
    int Num;                  //< Frame index of this root.
    const Constant *Metadata; //< Metadata associated with this root.

    GCGlobalRoot(int N, const Constant *MD) : Num(N), Metadata(MD) {}
  };

  /// GCCalleeSave - Metadata for a callee-saved register.
  ///
  struct GCCalleeSave {
    uint8_t Reg;  //< Register number.
    uint8_t Loc;  //< Stack offset.

    GCCalleeSave(int R, int L) : Reg(R), Loc(L) {}
  };


  /// GCFunctionInfo - Garbage collection metadata for a single function.
  ///
  class GCFunctionInfo {
  public:
    typedef std::vector<GCPoint>::iterator iterator;
    typedef std::vector<GCGlobalRoot>::iterator global_root_iterator;

  private:
    const Function &F;
    GCStrategy &S;
    uint64_t FrameSize;
    std::vector<GCPoint> SafePoints;
    std::vector<GCGlobalRoot> GlobalRoots;
    std::vector<GCCalleeSave> CalleeSaves;

    // FIXME: Liveness. A 2D BitVector, perhaps?
    //
    //   BitVector Liveness;
    //
    //   bool islive(int point, int root) =
    //     Liveness[point * SafePoints.size() + root]
    //
    // The bit vector is the more compact representation where >3.2% of roots
    // are live per safe point (1.5% on 64-bit hosts).

  public:
    GCFunctionInfo(const Function &F, GCStrategy &S);
    ~GCFunctionInfo();

    /// getFunction - Return the function to which this metadata applies.
    ///
    const Function &getFunction() const { return F; }

    /// getStrategy - Return the GC strategy for the function.
    ///
    GCStrategy &getStrategy() { return S; }

    /// addSafePoint - Notes the existence of a safe point. Num is the ID of the
    /// label just prior to the safe point (if the code generator is using
    /// MachineModuleInfo).
    GCPoint &addSafePoint(GC::PointKind Kind, MCSymbol *Label, DebugLoc DL) {
      SafePoints.push_back(GCPoint(Kind, Label, DL));
      return SafePoints.back();
    }

    GCPoint &getSafePoint(MCSymbol *Label, DebugLoc DL) {
      for (iterator SPI = begin(), SPE = end(); SPI != SPE; ++SPI) {
        if (SPI->Label == Label)
          return *SPI;
      }
      return addSafePoint(GC::PostCall, Label, DL);
    }

    /// addGlobalRoot - Notes the existence of a function-global root. Num
    /// is the ID of the frame index; MD is the front-end-supplied metadata.
    ///
    void addGlobalRoot(int Num, const Constant *Metadata) {
      GlobalRoots.push_back(GCGlobalRoot(Num, Metadata));
    }

    const std::vector<GCCalleeSave> getCalleeSaves() const {
        return CalleeSaves;
    }

    void addCalleeSave(const GCCalleeSave &CS) {
        CalleeSaves.push_back(CS);
    }

    void processRegisterAssignment(VirtRegMap &VM);
    void renameRegister(unsigned SrcReg, unsigned DstReg);

    /// getFrameSize/setFrameSize - Records the function's frame size.
    ///
    uint64_t getFrameSize() const { return FrameSize; }
    void setFrameSize(uint64_t S) { FrameSize = S; }

    /// begin/end - Iterators for safe points.
    ///
    iterator begin() { return SafePoints.begin(); }
    iterator end()   { return SafePoints.end();   }
    size_t size() const { return SafePoints.size(); }

    /// begin/end - Iterators for global roots.
    ///
    global_root_iterator global_root_begin() { return GlobalRoots.begin(); }
    global_root_iterator global_root_end()   { return GlobalRoots.end();   }
  };


  /// GCModuleInfo - Garbage collection metadata for a whole module.
  ///
  class GCModuleInfo : public ImmutablePass {
    typedef StringMap<GCStrategy*> strategy_map_type;
    typedef std::vector<GCStrategy*> list_type;
    typedef DenseMap<const Function*,GCFunctionInfo*> finfo_map_type;

    strategy_map_type StrategyMap;
    list_type StrategyList;
    finfo_map_type FInfoMap;

    GCStrategy *getOrCreateStrategy(const Module *M, const std::string &Name);

  public:
    typedef list_type::const_iterator iterator;

    static char ID;

    GCModuleInfo();
    ~GCModuleInfo();

    /// clear - Resets the pass. The metadata deleter pass calls this.
    ///
    void clear();

    /// begin/end - Iterators for used strategies.
    ///
    iterator begin() const { return StrategyList.begin(); }
    iterator end()   const { return StrategyList.end();   }

    /// get - Look up function metadata.
    ///
    GCFunctionInfo &getFunctionInfo(const Function &F);
  };

}

#endif
