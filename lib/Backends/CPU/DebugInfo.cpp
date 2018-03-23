// Copyright 2017 Facebook Inc.  All Rights Reserved.

#include "LLVMIRGen.h"

#include "CommandLine.h"

#include "glow/Graph/Graph.h"
#include "glow/IR/Instrs.h"

#include "llvm/IR/Verifier.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include <sstream>

using namespace glow;
using llvm::StringRef;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

extern llvm::cl::OptionCategory CPUBackendCat;

static llvm::cl::opt<bool>
    emitDebugInfo("g", llvm::cl::desc("Emit debug information for debuggers"),
                  llvm::cl::init(false), llvm::cl::cat(CPUBackendCat));

void LLVMIRGen::setCurrentDebugLocation(llvm::IRBuilder<> &builder,
                                        glow::Instruction *I) {
  if (!emitDebugInfo)
    return;
  auto instrNum = instrNumbering_->getInstrNumber(I);
  auto DILoc = llvm::DILocation::get(
      ctx_, dbgInfo_.mainFileFirstInstrLineNo_ + instrNum, 0, dbgInfo_.mainF_);
  llvm::DebugLoc loc(DILoc);
  builder.SetCurrentDebugLocation(loc);
}

llvm::DIType *LLVMIRGen::getDebugType(llvm::IRBuilder<> &builder,
                                      llvm::Type *ty) {
  // Check if the debug info for the type is in the cache and use it, if it is
  // available.
  if (dbgInfo_.DITypes_.count(ty))
    return dbgInfo_.DITypes_[ty];
  llvm::DIType *DITy{nullptr};
  if (ty == builder.getVoidTy()) {
    DITy = nullptr;
  } else if (ty == builder.getFloatTy()) {
    DITy = DIBuilder_->createBasicType("float", sizeof(float) * 8,
                                       llvm::dwarf::DW_ATE_float);
  } else if (ty == builder.getIntNTy(sizeof(size_t) * 8)) {
    DITy = DIBuilder_->createBasicType("size_t", sizeof(size_t) * 8,
                                       llvm::dwarf::DW_ATE_unsigned);
  } else if (auto *intTy = dyn_cast<llvm::IntegerType>(ty)) {
    std::string tyName = "int" + std::to_string(intTy->getBitWidth());
    DITy = DIBuilder_->createBasicType(tyName, intTy->getBitWidth(),
                                       llvm::dwarf::DW_ATE_unsigned);
  } else if (ty->isPointerTy()) {
    std::string tyName = "ptr" + std::to_string(dbgInfo_.DITypes_.size());
    DITy = DIBuilder_->createPointerType(
        getDebugType(builder, ty->getPointerElementType()), sizeof(void *) * 8);
  } else {
    llvm_unreachable("Cannot create DWARF debug type for an LLVM type");
  }
  dbgInfo_.DITypes_[ty] = DITy;
  return DITy;
}

void LLVMIRGen::generateFunctionDebugInfo(llvm::Function *F) {
  if (!emitDebugInfo)
    return;
  // First, generate a DISubprogram for the function.
  auto *DIFunction = getOrCreateFunctionDebugInfo(F, dbgInfo_.mainFile_,
                                                  dbgInfo_.mainFile_, 0);
  size_t lineNo = 0;
  auto file = dbgInfo_.mainFile_;
  auto *currentScope = DIFunction;
  // Find the insertion poisition for debug instructions.
  llvm::IRBuilder<> builder(&F->getEntryBlock());
  if (!F->getEntryBlock().empty()) {
    // Insert before the first instruction in the entry block.
    builder.SetInsertPoint(&F->getEntryBlock().front());
  }
  llvm::DebugLoc DL;
  builder.SetCurrentDebugLocation(DL);
  // Create debug information for the arguments, so that a debugger can expect
  // their values.
  for (unsigned i = 0, e = F->arg_size(); i != e; ++i) {
    // Create an alloca for storing a shadow of the function argument. The
    // parameter value will be copied there to make it easier for debugger to
    // inspect it.
    auto *paramAlloca =
        builder.CreateAlloca(F->getFunctionType()->getParamType(i));
    // Create a debug descriptor for the function argument.
    // TODO: Try to produce semantically meaningful parameter names, e.g. by
    // analyzing the debug information of the libjit.
    std::string paramName = "arg" + std::to_string(i + 1);
    auto param = DIBuilder_->createParameterVariable(
        currentScope, paramName, i + 1, file, lineNo,
        getDebugType(builder, F->getFunctionType()->getParamType(i)),
        /* alwaysPreserve */ true);
    // Store the initial value into the alloca, so that the debugger can show
    // it.
    auto *store = builder.CreateStore(F->arg_begin() + i, paramAlloca);
    DIBuilder_->insertDeclare(
        paramAlloca, param, DIBuilder_->createExpression(),
        llvm::DebugLoc::get(lineNo, 0, currentScope), store);
  }
  DIBuilder_->finalizeSubprogram(F->getSubprogram());
  llvm::DIScope *scope = F->getSubprogram();
  // Add debug locations to all instructions inside the functions which have
  // debug information. This is required for the proper emission of the debug
  // information into object files. If debug locations are missing, LLVM would
  // not emit such information like e.g. types of function parameters, etc.
  for (auto &BB : *F) {
    if (!scope)
      continue;
    for (auto &I : BB) {
      if (I.getDebugLoc())
        continue;
      I.setDebugLoc(llvm::DebugLoc(llvm::DILocation::get(ctx_, 0, 0, scope)));
    }
  }
}

llvm::DISubprogram *
LLVMIRGen::getOrCreateFunctionDebugInfo(llvm::Function *F, llvm::DIScope *scope,
                                        llvm::DIFile *file, unsigned lineNo) {
  // Do not emit any function debug information for LLVM internal functions.
  if (F->getName().empty() || F->getName().startswith("llvm."))
    return nullptr;
  auto *DIFunction = F->getSubprogram();
  if (!DIFunction) {
    // Create a function type. The result type should be stored in the first
    // element.
    llvm::SmallVector<llvm::Metadata *, 8> paramTys;

    // Add the result type.
    llvm::DIType *returnTy = getDebugType(*builder_, F->getReturnType());
    paramTys.push_back(returnTy);

    // Add the argument types.
    for (unsigned i = 0, e = F->arg_size(); i != e; ++i) {
      paramTys.push_back(
          getDebugType(*builder_, F->getFunctionType()->getParamType(i)));
    }
    // Create a function type.
    auto *DIFunctionTy = DIBuilder_->createSubroutineType(
        DIBuilder_->getOrCreateTypeArray(paramTys));
    // Create a debug information for the current function.
    DIFunction = DIBuilder_->createFunction(
        scope, F->getName(), "", file, lineNo, DIFunctionTy,
        false /* internal linkage */, true /* definition */, lineNo,
        llvm::DINode::FlagPrototyped, true /* isOptimized */);
    assert(DIFunction);
    F->setSubprogram(DIFunction);
  }

  assert(F->getSubprogram() == DIFunction &&
         "Function has been assigned wrong debug information");
  return DIFunction;
}

void LLVMIRGen::initDebugInfo() {
  if (!emitDebugInfo)
    return;
  // Add the current debug info version into the module.
  llmodule_->addModuleFlag(llvm::Module::Override, "Debug Info Version",
                           llvm::DEBUG_METADATA_VERSION);
  llmodule_->addModuleFlag(llvm::Module::Override, "Dwarf Version", 4);

  // Store the base addresses into global variables to enable access to weights
  // and activations inside the debugger.
  auto *main = getModule().getFunction("main");
  dbgInfo_.constWeightsBaseAddressGV_ = new llvm::GlobalVariable(
      getModule(), builder_->getInt8PtrTy(), /* isConst */ false,
      llvm::GlobalValue::InternalLinkage, nullptr, "constWeightsBaseAddress");
  dbgInfo_.mutableWeightsBaseAddressGV_ = new llvm::GlobalVariable(
      getModule(), builder_->getInt8PtrTy(), /* isConst */ false,
      llvm::GlobalValue::InternalLinkage, nullptr, "mutableWeightsBaseAddress");
  dbgInfo_.activationsBaseAddressGV_ = new llvm::GlobalVariable(
      getModule(), builder_->getInt8PtrTy(), /* isConst */ false,
      llvm::GlobalValue::InternalLinkage, nullptr, "activationsBaseAddress");
  dbgInfo_.constWeightsBaseAddressGV_->setInitializer(
      llvm::ConstantPointerNull::get(builder_->getInt8PtrTy()));
  dbgInfo_.mutableWeightsBaseAddressGV_->setInitializer(
      llvm::ConstantPointerNull::get(builder_->getInt8PtrTy()));
  dbgInfo_.activationsBaseAddressGV_->setInitializer(
      llvm::ConstantPointerNull::get(builder_->getInt8PtrTy()));
  builder_->CreateStore(main->args().begin() + 0,
                        dbgInfo_.constWeightsBaseAddressGV_);
  builder_->CreateStore(main->args().begin() + 1,
                        dbgInfo_.mutableWeightsBaseAddressGV_);
  builder_->CreateStore(main->args().begin() + 2,
                        dbgInfo_.activationsBaseAddressGV_);

  // Construct the DIBuilder.
  DIBuilder_ = llvm::make_unique<llvm::DIBuilder>(getModule());

  // Normalize names of weights and activations to become valid identifiers.
  // Replace all characters of the name that cannot be part of a valid C/C++
  // identifier by underscores. This allows for using them with a debugger.
  auto normalizeName = [](Value *v) {
    std::string name = v->getName();
    bool changed = false;
    for (auto &c : name) {
      if (!isalpha(c) && !isdigit(c) && c != '_') {
        c = '_';
        changed = true;
      }
    }
    if (changed) {
      v->setName(name);
    }
  };

  for (auto &v : F_->getGraph()->getParent()->getVars()) {
    auto *w = cast<WeightVar>(F_->getWeightForNode(v));
    normalizeName(w);
  }

  for (auto I : F_->getInstrs()) {
    if (!isa<AllocActivationInst>(I) && !isa<TensorViewInst>(I))
      continue;
    normalizeName(I);
  }

  // Create a textual representation of the IR for the main function.
  // First store the textual IR into a string.
  std::string irContent;
  llvm::raw_string_ostream irfileContent(irContent);
  F_->dump(irfileContent);
  irfileContent.str();

  // Write the IR into a file.
  std::error_code EC;
  // The name of the file for the IR, without a path.
  auto irfileName = getMainEntryName() + ".glow";
  // Use the absolute path, so that a debugger can always find a file.
  llvm::SmallVector<char, 128> path(getOutputDir().begin(),
                                    getOutputDir().end());
  EC = llvm::sys::fs::make_absolute(path);
  assert(!EC && "Could not create absolute path for a file");
  auto irfileFullPath = (path + "/" + irfileName).str();
  llvm::raw_fd_ostream irfile(irfileFullPath, EC,
                              llvm::sys::fs::OpenFlags::F_Text);
  assert(!EC && "Error opening output file");
  irfile << irContent;
  irfile.close();

  // Find out the line number of the first IR instruction. It is required to
  // enable stepping in the debugger.
  std::istringstream in(irContent);
  std::string s;
  size_t lineNo = 0;
  while (getline(in, s)) {
    lineNo++;
    // The first IR instruction comes right after the line "code {".
    if (s.substr(0, 6) == "code {") {
      dbgInfo_.mainFileFirstInstrLineNo_ = lineNo + 1;
      break;
    }
  }
  assert(dbgInfo_.mainFileFirstInstrLineNo_ &&
         "No IR code was found in the textual IR representation");

  // Create the debug information for the current file. It does not create a
  // real file. It is just a file name and path used for the debug locations.
  dbgInfo_.mainFile_ = DIBuilder_->createFile(
      irfileName, llvm::StringRef(path.data(), path.size()));

  // Create the compile unit for the module.
  dbgInfo_.compilationUnit_ = DIBuilder_->createCompileUnit(
      llvm::dwarf::DW_LANG_C, dbgInfo_.mainFile_, "Glow Compiler", 0, "", 0, "",
      llvm::DICompileUnit::DebugEmissionKind::FullDebug,
      /* SplitDebugInlining */ true,
      /* DebugInfoForProfiling */ true);

  // Create the debug info for the main function.
  dbgInfo_.mainF_ = main ? getOrCreateFunctionDebugInfo(
                               main, dbgInfo_.mainFile_, dbgInfo_.mainFile_, 0)
                         : nullptr;
}

void LLVMIRGen::emitDebugGlobalVariableForValue(Value *val) {
  auto name = val->getName();
  val = getOrigin(val);
  // Create a proper type for the variable.
  // Represent Glow's N-dimensional tensors as N-dimensional C arrays in the
  // debug information. This allows for inspecting them in the debugger using a
  // natural array notation, i.e. tensor[idx1][idx2]...[idxN].
  auto *ty = val->getType();
  auto dims = ty->dims();
  auto dbgElemTy = getDebugType(*builder_, getElementType(*builder_, val));
  llvm::SmallVector<llvm::Metadata *, 8> subranges;
  for (auto dim : dims) {
    subranges.push_back(llvm::DISubrange::get(ctx_, dim));
  }
  auto subscripts = llvm::MDTuple::get(ctx_, subranges);
  auto dbgArrayTy = DIBuilder_->createArrayType(
      ty->getSizeInBytes() * 8, sizeof(float), dbgElemTy, subscripts);

  // Create a debug info for the logical global variable representing a weight
  // or an activation. This allows for inspecting the values of weights and
  // activations when using a debugger. The address of this logical global
  // variable is computed as (base address of the memory area + offset) using
  // the information from the AllocationsInfo.
  llvm::GlobalVariable *baseAddress{nullptr};

  switch (allocationsInfo_.valueNumbers_[val].first) {
  case AllocationsInfo::ValueKind::Activation: {
    baseAddress = dbgInfo_.activationsBaseAddressGV_;
    break;
  }
  case AllocationsInfo::ValueKind::ConstantWeight: {
    baseAddress = dbgInfo_.constWeightsBaseAddressGV_;
    break;
  }
  case AllocationsInfo::ValueKind::MutableWeight: {
    baseAddress = dbgInfo_.mutableWeightsBaseAddressGV_;
    break;
  }
  }
  // DWARF operations to be performed with the base address to compute the
  // address of the logical global variable.
  llvm::SmallVector<uint64_t, 4> ops;
  assert(allocationsInfo_.allocatedAddressed_.count(val) &&
         "The weight should be in the map");
  auto offset = allocationsInfo_.allocatedAddressed_[val];
  // Get the value of the global var.
  ops.push_back(llvm::dwarf::DW_OP_deref);
  // Add the offset to the value of the global var to get the address of the
  // logical debug variable being created.
  ops.push_back(llvm::dwarf::DW_OP_constu);
  ops.push_back(offset);
  ops.push_back(llvm::dwarf::DW_OP_plus);
  llvm::DIExpression *DIexpr{nullptr};
  DIexpr = DIBuilder_->createExpression(ops);
  auto *DIgv = DIBuilder_->createGlobalVariableExpression(
      dbgInfo_.compilationUnit_, name, "", dbgInfo_.mainFile_, 0, dbgArrayTy,
      /* isLocalToUnit */ false, DIexpr);
  baseAddress->addDebugInfo(DIgv);
}

void LLVMIRGen::generateDebugInfo() {
  if (!emitDebugInfo)
    return;

  // Iterate over all functions in the module and generate a debug information
  // for them.
  for (auto &F : getModule()) {
    if (F.isDeclaration())
      continue;
    // If a function has a debug information already, no need to re-emit it.
    if (F.getSubprogram())
      continue;
    llvm_unreachable(
        "Expected all functions to have debug information at this point");
  }

  // Now iterate over the module and add debug locations to all instructions
  // inside the functions which have debug information. This is required for the
  // proper emission of the debug information into object files. If debug
  // locations are missing, LLVM would not emit such information like e.g. types
  // of function parameters, etc.
  for (auto &F : getModule()) {
    if (F.isDeclaration())
      continue;
    // Bail if the function has no debug information.
    llvm::DIScope *scope = F.getSubprogram();
    if (!scope)
      continue;
    for (auto &BB : F) {
      for (auto &I : BB) {
        // Do not update debug locations that are not belonging to the current
        // scope.
        if (I.getDebugLoc() &&
            I.getDebugLoc()->getScope()->getName() != F.getName())
          continue;
        I.setDebugLoc(llvm::DebugLoc(llvm::DILocation::get(ctx_, 0, 0, scope)));
      }
    }
  }

  // Emit the debug info for weight variables and activations variables used by
  // the Glow IR. Represent those variables as global variables.
  for (auto &v : F_->getGraph()->getParent()->getVars()) {
    auto *w = cast<WeightVar>(F_->getWeightForNode(v));
    emitDebugGlobalVariableForValue(w);
  }

  for (auto I : F_->getInstrs()) {
    if (!isa<AllocActivationInst>(I) && !isa<TensorViewInst>(I))
      continue;
    emitDebugGlobalVariableForValue(I);
  }

  // Finalize the debug info.
  DIBuilder_->finalize();

  // Verify the module to see if there are any errors due to the debug
  // information.
  bool brokenDebugInfo = false;
  // Pass brokenDebugInfo as a reference to the verifyModule.
  assert(!llvm::verifyModule(getModule(), &llvm::errs(), &brokenDebugInfo) &&
         "LLVM module verification error");
  assert(!brokenDebugInfo && "Debug information is broken");
}