#include "llvm/ADT/StringRef.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace {

static std::pair<Value *, Value *> getFileLine(IRBuilder<> &B, Instruction &I) {
  LLVMContext &Ctx = I.getContext();

  std::string File = "<unknown>";
  unsigned Line = 0;

  if (DILocation *Loc = I.getDebugLoc()) {
    Line = Loc->getLine();

    StringRef Dir = Loc->getDirectory();
    StringRef Name = Loc->getFilename();
    if (!Name.empty()) {
      if (!Dir.empty()) {
        File = (Dir + "/" + Name).str();
      } else {
        File = Name.str();
      }
    }
  }

  Value *FilePtr = B.CreateGlobalStringPtr(File);
  Value *LineVal = ConstantInt::get(Type::getInt32Ty(Ctx), Line);
  return {FilePtr, LineVal};
}

struct MPISanitizePass : public PassInfoMixin<MPISanitizePass> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &) {
    LLVMContext &Ctx = M.getContext();

    Type *VoidTy = Type::getVoidTy(Ctx);
    Type *I8PtrTy = Type::getInt8PtrTy(Ctx);
    Type *I32Ty = Type::getInt32Ty(Ctx);
    Type *I64Ty = Type::getInt64Ty(Ctx);

    // void __msan_before_send(void *buf, int count, uint64_t dt_handle, int dest, int tag, uint64_t comm_handle, const char *file, int line);
    FunctionCallee BeforeSend =
      M.getOrInsertFunction("__msan_before_send", VoidTy, I8PtrTy, I32Ty,
                  I64Ty, I32Ty, I32Ty, I64Ty, I8PtrTy, I32Ty);

    // void __msan_after_recv(void *buf, int count, uint64_t dt_handle, int source, int tag, uint64_t comm_handle, void *status, const char *file, int line);
    FunctionCallee AfterRecv =
      M.getOrInsertFunction("__msan_after_recv", VoidTy, I8PtrTy, I32Ty,
                  I64Ty, I32Ty, I32Ty, I64Ty, I8PtrTy, I8PtrTy,
                  I32Ty);

    // void __msan_finalize(const char *file, int line);
    FunctionCallee Finalize =
        M.getOrInsertFunction("__msan_finalize", VoidTy, I8PtrTy, I32Ty);

    // void __msan_init(const char *file, int line);
    FunctionCallee Init =
      M.getOrInsertFunction("__msan_init", VoidTy, I8PtrTy, I32Ty);

    bool Changed = false;

    auto toI64Handle = [&](IRBuilder<> &B, Value *V) -> Value * {
      Type *Ty = V->getType();
      if (Ty->isPointerTy())
        return B.CreatePtrToInt(V, I64Ty);
      if (Ty->isIntegerTy())
        return B.CreateZExtOrTrunc(V, I64Ty);
      return nullptr;
    };

    auto toI8Ptr = [&](IRBuilder<> &B, Value *V) -> Value * {
      Type *Ty = V->getType();
      if (Ty->isPointerTy())
        return B.CreateBitCast(V, I8PtrTy);
      if (Ty->isIntegerTy()) {
        Value *AsI64 = B.CreateZExtOrTrunc(V, I64Ty);
        return B.CreateIntToPtr(AsI64, I8PtrTy);
      }
      return nullptr;
    };

    for (Function &F : M) {
      if (F.isDeclaration())
        continue;

      for (Instruction &I : instructions(F)) {
        auto *CB = dyn_cast<CallBase>(&I);
        if (!CB)
          continue;

        Function *Callee = CB->getCalledFunction();
        if (!Callee)
          continue;

        StringRef Name = Callee->getName();

        if (Name == "MPI_Init" || Name == "MPI_Init_thread") {
          Instruction *InsertPt = nullptr;
          if (auto *CallI = dyn_cast<CallInst>(CB))
            InsertPt = CallI->getNextNode();
          if (!InsertPt)
            InsertPt = CB->getParent()->getTerminator();

          IRBuilder<> B(InsertPt);
          auto [FilePtr, LineVal] = getFileLine(B, I);
          B.CreateCall(Init, {FilePtr, LineVal});
          Changed = true;
          continue;
        }

        // Handle point-to-point.
        if (Name == "MPI_Send") {
          // MPI_Send(buf, count, datatype, dest, tag, comm)
          if (CB->arg_size() < 6)
            continue;

          IRBuilder<> B(CB);
          auto [FilePtr, LineVal] = getFileLine(B, I);

          Value *Buf = toI8Ptr(B, CB->getArgOperand(0));
          Value *Count = B.CreateIntCast(CB->getArgOperand(1), I32Ty, true);
          Value *Datatype = toI64Handle(B, CB->getArgOperand(2));
          Value *Dest = B.CreateIntCast(CB->getArgOperand(3), I32Ty, true);
          Value *Tag = B.CreateIntCast(CB->getArgOperand(4), I32Ty, true);
          Value *Comm = toI64Handle(B, CB->getArgOperand(5));

          if (!Buf || !Datatype || !Comm)
            continue;

          B.CreateCall(BeforeSend,
                       {Buf, Count, Datatype, Dest, Tag, Comm, FilePtr, LineVal});
          Changed = true;
          continue;
        }

        if (Name == "MPI_Recv") {
          // MPI_Recv(buf, count, datatype, source, tag, comm, status)
          if (CB->arg_size() < 7)
            continue;

          Instruction *InsertPt = nullptr;
          if (auto *CallI = dyn_cast<CallInst>(CB)) {
            InsertPt = CallI->getNextNode();
          }

          if (!InsertPt) {
            // Fallback: insert right after the call.
            InsertPt = CB->getParent()->getTerminator();
          }

          IRBuilder<> B(InsertPt);
          auto [FilePtr, LineVal] = getFileLine(B, I);

          Value *Buf = toI8Ptr(B, CB->getArgOperand(0));
          Value *Count = B.CreateIntCast(CB->getArgOperand(1), I32Ty, true);
          Value *Datatype = toI64Handle(B, CB->getArgOperand(2));
          Value *Source = B.CreateIntCast(CB->getArgOperand(3), I32Ty, true);
          Value *Tag = B.CreateIntCast(CB->getArgOperand(4), I32Ty, true);
          Value *Comm = toI64Handle(B, CB->getArgOperand(5));
          Value *Status = toI8Ptr(B, CB->getArgOperand(6));

          if (!Buf || !Datatype || !Comm || !Status)
            continue;

          B.CreateCall(AfterRecv, {Buf, Count, Datatype, Source, Tag, Comm, Status,
                                   FilePtr, LineVal});
          Changed = true;
          continue;
        }

        if (Name == "MPI_Finalize") {
          IRBuilder<> B(CB);
          auto [FilePtr, LineVal] = getFileLine(B, I);
          B.CreateCall(Finalize, {FilePtr, LineVal});
          Changed = true;
          continue;
        }
      }
    }

    return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  }
};

} // namespace

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "MPISanitizePass", "0.1",
          [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name, ModulePassManager &MPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "mpi-sanitize") {
                    MPM.addPass(MPISanitizePass());
                    return true;
                  }
                  return false;
                });
          }};
}
