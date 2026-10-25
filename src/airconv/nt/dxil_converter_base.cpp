#include "dxil_converter_base.hpp"

#include "../airconv_error.hpp"
#include "../dxbc_converter.hpp"
#include "dxil_constants.hpp"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

#include <string>
#include <utility>

using namespace llvm;
using namespace llvm::air;

namespace dxmt::dxil {

llvm::Error
unsupported(llvm::StringRef what) {
  return llvm::make_error<dxmt::UnsupportedFeature>(what);
}

std::optional<uint32_t>
asU32(Value *value) {
  if (auto *ci = dyn_cast<ConstantInt>(value))
    return (uint32_t)ci->getZExtValue();
  return std::nullopt;
}

const Resource *
find_binding(const EntryPoint &entry, Value *bind) {
  auto *c = dyn_cast<Constant>(bind);
  if (!c)
    return nullptr;
  auto field = [&](unsigned i) -> std::optional<uint32_t> {
    auto *element = c->getAggregateElement(i);
    return element ? asU32(element) : std::nullopt;
  };
  auto lower = field(0), upper = field(1), space = field(2), resource_class = field(3);
  if (!lower || !upper || !space || !resource_class)
    return nullptr;
  for (auto &resource : entry.resources) {
    if ((uint32_t)resource.resource_class != *resource_class || resource.space != *space)
      continue;
    // An unbounded range's size is ~0u, as its binding's upper bound is.
    uint64_t last = resource.range_size == ~0u ? ~0u : (uint64_t)resource.lower_bound + resource.range_size - 1;
    if (resource.lower_bound <= *lower && *upper <= last)
      return &resource;
  }
  return nullptr;
}

int32_t
tryGetI32(Value *value) {
  if (auto *ci = dyn_cast<ConstantInt>(value))
    return (int32_t)ci->getSExtValue();
  return (int32_t)0;
}

Converter::Converter(EntryPoint const &entry, dxmt::dxbc::context &ctx) :
    ctx(ctx),
    ir(ctx.builder),
    air(ctx.air),
    entry(entry) {
  for (const auto &property : entry.properties) {
    if (property.tag == EntryPropertyTag::HSState)
      pcf_ = property.hs_state.patch_constant_function;
  }
}

llvm::Expected<llvm::BasicBlock *>
Converter::Run(llvm::BasicBlock *ReturnBB) {

  auto PCFStart = pcf_ ? llvm::BasicBlock::Create(air.getContext(), "pcf_start", ctx.function) : nullptr;
  auto PCFEnd = pcf_ ? llvm::BasicBlock::Create(air.getContext(), "pcf_end", ctx.function) : nullptr;

  if (entry.function) {
    for (auto &BB : *entry.function) {
      auto *NewBB = llvm::BasicBlock::Create(air.getContext(), "", ctx.function);
      block_map_[&BB] = NewBB;
      ir.SetInsertPoint(NewBB);
      for (auto &Inst : BB) {
        auto *PHI = dyn_cast<PHINode>(&Inst);
        if (!PHI)
          break;
        value_map_[PHI] = ir.CreatePHI(MapType(PHI->getType()), 0);
      }
    }

    /* Static variables DXC keeps as globals in address space 0 (an array indexed dynamically) are each thread's own:
       locals here, from their initializers. */
    ir.SetInsertPoint(block_map_[&entry.function->getEntryBlock()]);
    for (auto &Global : const_cast<llvm::Module *>(entry.function->getParent())->globals()) {
      if (Global.getAddressSpace() != 0 || Global.isConstant() || Global.use_empty())
        continue;
      auto *Alloca = ir.CreateAlloca(MapType(Global.getValueType()), nullptr, Global.getName());
      if (auto Align = Global.getAlign())
        Alloca->setAlignment(*Align);
      if (Global.hasInitializer() && !isa<UndefValue>(Global.getInitializer())) {
        auto Initializer = MapConstant(Global.getInitializer());
        if (!Initializer)
          return Initializer.takeError();
        ir.CreateStore(*Initializer, Alloca);
      }
      value_map_[&Global] = Alloca;
    }

    // FIXME: remove const_cast
    /* Blocks in reverse post-order: a value is translated before its uses, as its block dominates theirs, however
       the blocks are laid out (DXC can put a loop's body before a block that dominates it). A phi gets each incoming
       value at its predecessor's terminator. Blocks the entry doesn't reach end in unreachable. */
    auto *Function = const_cast<llvm::Function *>(entry.function);
    llvm::ReversePostOrderTraversal<llvm::Function *> Order(Function);
    SmallPtrSet<llvm::BasicBlock *, 32> Reached;
    for (auto *Block : Order)
      Reached.insert(Block);
    for (auto &BB : *Function) {
      if (!Reached.contains(&BB)) {
        ir.SetInsertPoint(block_map_[&BB]);
        ir.CreateUnreachable();
      }
    }
    for (auto *Block : Order) {
      auto &BB = *Block;
      ir.SetInsertPoint(block_map_[&BB]);
      for (auto &Inst : BB) {
        if (isa<PHINode>(&Inst))
          continue;
        if (Inst.isTerminator()) {
          if (auto err = TranslateTerminator(&Inst, pcf_ ? PCFStart : ReturnBB))
            return err;
          break;
        }
        auto Result = TranslateInstruction(&Inst);
        if (!Result)
          return Result.takeError();
        if (*Result && !(*Result)->getType()->isVoidTy())
          value_map_[&Inst] = *Result;
      }
    }
  }

  if (pcf_) {
    for (auto &BB : *pcf_) {
      auto *NewBB = llvm::BasicBlock::Create(air.getContext(), "", ctx.function);
      block_map_[&BB] = NewBB;
      ir.SetInsertPoint(NewBB);
      for (auto &Inst : BB) {
        auto *PHI = dyn_cast<PHINode>(&Inst);
        if (!PHI)
          break;
        value_map_[PHI] = ir.CreatePHI(MapType(PHI->getType()), 0);
      }
    }

    // FIXME: remove const_cast
    for (auto &BB : *const_cast<llvm::Function *>(pcf_)) {
      ir.SetInsertPoint(block_map_[&BB]);
      for (auto &Inst : BB) {
        if (isa<PHINode>(&Inst))
          continue;
        if (Inst.isTerminator()) {
          if (auto err = TranslateTerminator(&Inst, PCFEnd))
            return err;
          break;
        }
        auto Result = TranslateInstruction(&Inst);
        if (!Result)
          return Result.takeError();
        if (*Result && !(*Result)->getType()->isVoidTy())
          value_map_[&Inst] = *Result;
      }
    }
    ir.SetInsertPoint(PCFStart);
    ir.CreateCondBr(
        ir.CreateICmp(llvm::CmpInst::ICMP_EQ, ctx.resource.thread_id_in_patch, ir.getInt32(0)),
        block_map_[&pcf_->getEntryBlock()], PCFEnd
    );
    ir.SetInsertPoint(PCFEnd);
    air.CreateBarrier(llvm::air::MemFlags::Threadgroup);
    ir.CreateBr(ReturnBB);
    if (!entry.function)
      return PCFStart;
  }

  return block_map_[&entry.function->getEntryBlock()];
}

llvm::Type *
Converter::MapType(llvm::Type *Type) {
  auto Found = type_map_.find(Type);
  if (Found != type_map_.end())
    return Found->second;

  llvm::Type *TyMapped = nullptr;
  if (Type->isVoidTy()) {
    TyMapped = llvm::Type::getVoidTy(air.getContext());
  } else if (Type->isIntegerTy()) {
    TyMapped = llvm::IntegerType::get(air.getContext(), Type->getIntegerBitWidth());
  } else if (Type->isHalfTy()) {
    TyMapped = llvm::Type::getHalfTy(air.getContext());
  } else if (Type->isFloatTy()) {
    TyMapped = llvm::Type::getFloatTy(air.getContext());
  } else if (Type->isDoubleTy()) {
    TyMapped = llvm::Type::getDoubleTy(air.getContext());
  } else if (auto *pointer = llvm::dyn_cast<llvm::PointerType>(Type)) {
    TyMapped = MapType(pointer->getNonOpaquePointerElementType())->getPointerTo(pointer->getAddressSpace());
  } else if (auto *array = llvm::dyn_cast<llvm::ArrayType>(Type)) {
    TyMapped = llvm::ArrayType::get(MapType(array->getElementType()), array->getNumElements());
  } else if (auto *vector = llvm::dyn_cast<llvm::FixedVectorType>(Type)) {
    TyMapped = llvm::FixedVectorType::get(MapType(vector->getElementType()), vector->getNumElements());
  } else if (auto *function = llvm::dyn_cast<llvm::FunctionType>(Type)) {
    llvm::SmallVector<llvm::Type *, 8> Params;
    for (auto *param : function->params())
      Params.push_back(MapType(param));
    TyMapped = llvm::FunctionType::get(MapType(function->getReturnType()), Params, function->isVarArg());
  } else if (auto *TyStruct = llvm::dyn_cast<llvm::StructType>(Type)) {
    if (TyStruct->isOpaque()) {
      TyMapped = llvm::StructType::create(air.getContext(), TyStruct->getName());
    } else if (TyStruct->hasName()) {
      auto *TyNamed = llvm::StructType::create(air.getContext(), TyStruct->getName());
      type_map_[Type] = TyNamed;
      llvm::SmallVector<llvm::Type *, 8> Elements;
      for (auto *TyElement : TyStruct->elements())
        Elements.push_back(MapType(TyElement));
      TyNamed->setBody(Elements);
      TyMapped = TyNamed;
    } else {
      llvm::SmallVector<llvm::Type *, 8> Elements;
      for (auto *TyElement : TyStruct->elements())
        Elements.push_back(MapType(TyElement));
      TyMapped = llvm::StructType::get(air.getContext(), Elements);
    }
  } else {
    TyMapped = Type;
  }

  type_map_[Type] = TyMapped;
  return TyMapped;
}

llvm::Expected<llvm::Constant *>
Converter::MapConstantExpr(llvm::ConstantExpr *Expr) {
  switch (Expr->getOpcode()) {
  case llvm::Instruction::GetElementPtr: {
    llvm::SmallVector<llvm::Constant *, 8> Indices;
    for (unsigned i = 1; i < Expr->getNumOperands(); i++) {
      auto Index = MapConstant(llvm::cast<llvm::Constant>(Expr->getOperand(i)));
      if (!Index)
        return Index.takeError();
      Indices.push_back(*Index);
    }
    auto Ptr = MapConstant(llvm::cast<llvm::Constant>(Expr->getOperand(0)));
    if (!Ptr)
      return Ptr.takeError();
    auto *TySrc = MapType(Expr->getOperand(0)->getType()->getNonOpaquePointerElementType());
    return llvm::ConstantExpr::getGetElementPtr(TySrc, *Ptr, Indices);
  }
  case llvm::Instruction::BitCast:
  case llvm::Instruction::Trunc:
  case llvm::Instruction::ZExt:
  case llvm::Instruction::SExt:
  case llvm::Instruction::FPTrunc:
  case llvm::Instruction::FPExt:
  case llvm::Instruction::UIToFP:
  case llvm::Instruction::SIToFP:
  case llvm::Instruction::FPToUI:
  case llvm::Instruction::FPToSI:
  case llvm::Instruction::PtrToInt:
  case llvm::Instruction::IntToPtr:
  case llvm::Instruction::AddrSpaceCast: {
    auto Operand = MapConstant(llvm::cast<llvm::Constant>(Expr->getOperand(0)));
    if (!Operand)
      return Operand.takeError();
    return llvm::ConstantExpr::getCast(Expr->getOpcode(), *Operand, MapType(Expr->getType()));
  }
  default:
    return unsupported(
        std::string("dxil: unsupported constant expression: ") + Instruction::getOpcodeName(Expr->getOpcode())
    );
  }
}

llvm::Expected<llvm::Constant *>
Converter::MapConstant(llvm::Constant *Constant) {
  auto Found = constant_map_.find(Constant);
  if (Found != constant_map_.end())
    return Found->second;

  llvm::Constant *Mapped = nullptr;
  if (auto *Integer = llvm::dyn_cast<llvm::ConstantInt>(Constant)) {
    Mapped = llvm::ConstantInt::get(MapType(Integer->getType()), Integer->getValue());
  } else if (auto *FP = llvm::dyn_cast<llvm::ConstantFP>(Constant)) {
    Mapped = llvm::ConstantFP::get(air.getContext(), FP->getValueAPF());
  } else if (llvm::isa<llvm::ConstantPointerNull>(Constant)) {
    Mapped = llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(MapType(Constant->getType())));
  } else if (llvm::isa<llvm::UndefValue>(Constant)) {
    Mapped = llvm::UndefValue::get(MapType(Constant->getType()));
  } else if (llvm::isa<llvm::PoisonValue>(Constant)) {
    Mapped = llvm::PoisonValue::get(MapType(Constant->getType()));
  } else if (llvm::isa<llvm::ConstantAggregateZero>(Constant)) {
    Mapped = llvm::ConstantAggregateZero::get(MapType(Constant->getType()));
  } else if (auto *Global = llvm::dyn_cast<llvm::GlobalVariable>(Constant)) {
    auto MappedGlobal = MapGlobal(Global);
    if (!MappedGlobal)
      return MappedGlobal.takeError();
    Mapped = *MappedGlobal;
  } else if (auto *Sequence = llvm::dyn_cast<llvm::ConstantDataSequential>(Constant)) {
    llvm::SmallVector<llvm::Constant *, 16> Elements;
    for (unsigned i = 0; i < Sequence->getNumElements(); i++) {
      auto Element = MapConstant(Sequence->getElementAsConstant(i));
      if (!Element)
        return Element.takeError();
      Elements.push_back(*Element);
    }
    if (Sequence->getType()->isVectorTy())
      Mapped = llvm::ConstantVector::get(Elements);
    else
      Mapped = llvm::ConstantArray::get(llvm::cast<llvm::ArrayType>(MapType(Sequence->getType())), Elements);
  } else if (auto *Vector = llvm::dyn_cast<llvm::ConstantVector>(Constant)) {
    llvm::SmallVector<llvm::Constant *, 16> Elements;
    for (auto &Operand : Vector->operands()) {
      auto Element = MapConstant(llvm::cast<llvm::Constant>(Operand));
      if (!Element)
        return Element.takeError();
      Elements.push_back(*Element);
    }
    Mapped = llvm::ConstantVector::get(Elements);
  } else if (auto *Array = llvm::dyn_cast<llvm::ConstantArray>(Constant)) {
    llvm::SmallVector<llvm::Constant *, 16> Elements;
    for (auto &Operand : Array->operands()) {
      auto Element = MapConstant(llvm::cast<llvm::Constant>(Operand));
      if (!Element)
        return Element.takeError();
      Elements.push_back(*Element);
    }
    Mapped = llvm::ConstantArray::get(llvm::cast<llvm::ArrayType>(MapType(Array->getType())), Elements);
  } else if (auto *Structure = llvm::dyn_cast<llvm::ConstantStruct>(Constant)) {
    llvm::SmallVector<llvm::Constant *, 16> Elements;
    for (auto &Operand : Structure->operands()) {
      auto Element = MapConstant(llvm::cast<llvm::Constant>(Operand));
      if (!Element)
        return Element.takeError();
      Elements.push_back(*Element);
    }
    Mapped = llvm::ConstantStruct::get(llvm::cast<llvm::StructType>(MapType(Structure->getType())), Elements);
  } else if (auto *Expr = llvm::dyn_cast<llvm::ConstantExpr>(Constant)) {
    auto MappedExpr = MapConstantExpr(Expr);
    if (!MappedExpr)
      return MappedExpr.takeError();
    Mapped = *MappedExpr;
  } else {
    return unsupported("dxil: unsupported Constant");
  }

  constant_map_[Constant] = Mapped;
  return Mapped;
}

llvm::Expected<llvm::GlobalVariable *>
Converter::MapGlobal(llvm::GlobalVariable *Global) {
  auto Found = global_map_.find(Global);
  if (Found != global_map_.end())
    return Found->second;

  auto *Type = MapType(Global->getValueType());
  llvm::GlobalVariable *Mapped = nullptr;
  if (Global->getAddressSpace() == 3) {
    if (Global->hasInitializer() && !isa<llvm::UndefValue>(Global->getInitializer()))
      return unsupported("dxil: unsupported tgsm initializer");
    Mapped = new llvm::GlobalVariable(
        *air.getModule(), Type, false, llvm::GlobalValue::InternalLinkage, llvm::UndefValue::get(Type),
        Global->getName(), nullptr, Global->getThreadLocalMode(), 3
    );
  } else if (Global->hasLocalLinkage() && Global->isConstant() && Global->hasInitializer()) {
    auto initializer = MapConstant(Global->getInitializer());
    if (!initializer)
      return initializer.takeError();
    Mapped = new llvm::GlobalVariable(
        *air.getModule(), Type, true, Global->getLinkage(), *initializer, Global->getName(), nullptr,
        Global->getThreadLocalMode(), 2
    );
  } else {
    return unsupported("dxil: unsupported Global variable " + Global->getName().str());
  }
  Mapped->setAlignment(Global->getAlign());
  Mapped->setUnnamedAddr(Global->getUnnamedAddr());
  global_map_[Global] = Mapped;
  return Mapped;
}

bool
Converter::RefersToLocal(const llvm::ConstantExpr *Expr) {
  for (auto &Operand : Expr->operands()) {
    if (isa<llvm::GlobalVariable>(Operand) && value_map_.count(Operand))
      return true;
    if (auto *Nested = llvm::dyn_cast<llvm::ConstantExpr>(Operand); Nested && RefersToLocal(Nested))
      return true;
  }
  return false;
}

llvm::Expected<llvm::Value *>
Converter::Get(llvm::Value *Value) {
  auto Found = value_map_.find(Value);
  if (Found != value_map_.end())
    return Found->second;
  // A constant expression on a static variable (Run's locals): instructions where it's used.
  if (auto *Expr = llvm::dyn_cast<llvm::ConstantExpr>(Value); Expr && RefersToLocal(Expr)) {
    auto *Inst = Expr->getAsInstruction();
    auto Result = TranslateInstruction(Inst);
    Inst->deleteValue();
    return Result;
  }
  if (auto *Constant = llvm::dyn_cast<llvm::Constant>(Value)) {
    auto Mapped = MapConstant(Constant);
    if (!Mapped)
      return Mapped.takeError();
    return *Mapped;
  }
  std::string what;
  llvm::raw_string_ostream os(what);
  Value->print(os);
  return unsupported("dxil: reference to untranslated Value:" + what.substr(0, 200));
}

llvm::Error
Converter::TranslateTerminator(llvm::Instruction *Inst, llvm::BasicBlock *ReturnBB) {
  if (auto *Return = dyn_cast<ReturnInst>(Inst)) {
    if (Return->getNumOperands())
      return unsupported("dxil: non-void return");
    ir.CreateBr(ReturnBB);
  } else if (auto *Branch = dyn_cast<BranchInst>(Inst)) {
    if (Branch->isConditional()) {
      auto Cond = Get(Branch->getCondition());
      if (!Cond)
        return Cond.takeError();
      ir.CreateCondBr(*Cond, block_map_[Branch->getSuccessor(0)], block_map_[Branch->getSuccessor(1)]);
    } else {
      ir.CreateBr(block_map_[Branch->getSuccessor(0)]);
    }
  } else if (llvm::isa<llvm::UnreachableInst>(Inst)) {
    ir.CreateUnreachable();
  } else if (auto *Switch = dyn_cast<SwitchInst>(Inst)) {
    auto Cond = Get(Switch->getCondition());
    if (!Cond)
      return Cond.takeError();
    auto *NewSwitch = ir.CreateSwitch(*Cond, block_map_[Switch->getDefaultDest()], Switch->getNumCases());
    for (auto &Case : Switch->cases()) {
      auto CaseValue = MapConstant(Case.getCaseValue());
      if (!CaseValue)
        return CaseValue.takeError();
      NewSwitch->addCase(llvm::cast<llvm::ConstantInt>(*CaseValue), block_map_[Case.getCaseSuccessor()]);
    }
  } else {
    return unsupported("dxil: unsupported terminator");
  }

  auto *NewBB = ir.GetInsertBlock();
  SmallPtrSet<BasicBlock *, 4> Seen;
  for (auto *Succ : successors(Inst->getParent())) {
    auto Found = block_map_.find(Succ);
    if (Found == block_map_.end() || !Seen.insert(Succ).second)
      continue;
    for (auto &InstInSucc : *Succ) {
      auto *PHI = dyn_cast<PHINode>(&InstInSucc);
      if (!PHI)
        break;
      auto MapFound = value_map_.find(PHI);
      if (MapFound == value_map_.end())
        return unsupported("dxil: phi without translation");
      auto Incoming = Get(PHI->getIncomingValueForBlock(Inst->getParent()));
      if (!Incoming)
        return Incoming.takeError();
      cast<PHINode>(MapFound->second)->addIncoming(*Incoming, NewBB);
    }
  }
  return llvm::Error::success();
}

llvm::Expected<llvm::Value *>
Converter::TranslateInstruction(llvm::Instruction *Inst) {
  auto operand = [&](unsigned index) { return Get(Inst->getOperand(index)); };

  switch (Inst->getOpcode()) {
  case llvm::Instruction::Add:
  case llvm::Instruction::Sub:
  case llvm::Instruction::Mul:
  case llvm::Instruction::UDiv:
  case llvm::Instruction::SDiv:
  case llvm::Instruction::URem:
  case llvm::Instruction::SRem:
  case llvm::Instruction::Shl:
  case llvm::Instruction::LShr:
  case llvm::Instruction::AShr:
  case llvm::Instruction::And:
  case llvm::Instruction::Or:
  case llvm::Instruction::Xor:
  case llvm::Instruction::FAdd:
  case llvm::Instruction::FSub:
  case llvm::Instruction::FMul:
  case llvm::Instruction::FDiv:
  case llvm::Instruction::FRem: {
    auto LHS = operand(0);
    if (!LHS)
      return LHS.takeError();
    auto RHS = operand(1);
    if (!RHS)
      return RHS.takeError();
    auto *BinOp = llvm::BinaryOperator::Create((llvm::Instruction::BinaryOps)Inst->getOpcode(), *LHS, *RHS);
    if (llvm::isa<llvm::FPMathOperator>(BinOp))
      BinOp->copyFastMathFlags(Inst);
    ir.Insert(BinOp);
    return BinOp;
  }
  case llvm::Instruction::FNeg: {
    auto Value = operand(0);
    if (!Value)
      return Value.takeError();
    auto *UnOp = llvm::UnaryOperator::Create((llvm::Instruction::UnaryOps)Inst->getOpcode(), *Value);
    if (llvm::isa<llvm::FPMathOperator>(UnOp))
      UnOp->copyFastMathFlags(Inst);
    ir.Insert(UnOp);
    return UnOp;
  }
  case llvm::Instruction::ICmp:
  case llvm::Instruction::FCmp: {
    auto LHS = operand(0);
    if (!LHS)
      return LHS.takeError();
    auto RHS = operand(1);
    if (!RHS)
      return RHS.takeError();
    auto *Cmp = llvm::CmpInst::Create(
        (llvm::Instruction::OtherOps)Inst->getOpcode(), llvm::cast<llvm::CmpInst>(Inst)->getPredicate(), *LHS, *RHS
    );
    if (llvm::isa<llvm::FPMathOperator>(Cmp))
      Cmp->copyFastMathFlags(Inst);
    ir.Insert(Cmp);
    return Cmp;
  }
  case llvm::Instruction::Select: {
    auto Cond = operand(0);
    if (!Cond)
      return Cond.takeError();
    auto True = operand(1);
    if (!True)
      return True.takeError();
    auto False = operand(2);
    if (!False)
      return False.takeError();
    return ir.CreateSelect(*Cond, *True, *False);
  }
  case llvm::Instruction::Trunc:
  case llvm::Instruction::ZExt:
  case llvm::Instruction::SExt:
  case llvm::Instruction::FPTrunc:
  case llvm::Instruction::FPExt:
  case llvm::Instruction::FPToUI:
  case llvm::Instruction::FPToSI:
  case llvm::Instruction::UIToFP:
  case llvm::Instruction::SIToFP:
  case llvm::Instruction::PtrToInt:
  case llvm::Instruction::IntToPtr:
  case llvm::Instruction::BitCast:
  case llvm::Instruction::AddrSpaceCast: {
    auto Value = operand(0);
    if (!Value)
      return Value.takeError();
    auto *Cast =
        llvm::CastInst::Create((llvm::Instruction::CastOps)Inst->getOpcode(), *Value, MapType(Inst->getType()));
    ir.Insert(Cast);
    return Cast;
  }
  case llvm::Instruction::GetElementPtr: {
    auto *OldGEP = llvm::cast<llvm::GetElementPtrInst>(Inst);
    auto Ptr = operand(0);
    if (!Ptr)
      return Ptr.takeError();
    llvm::SmallVector<llvm::Value *, 8> Indices;
    for (unsigned i = 1; i < Inst->getNumOperands(); i++) {
      auto Index = operand(i);
      if (!Index)
        return Index.takeError();
      Indices.push_back(*Index);
    }
    auto *GEP = llvm::GetElementPtrInst::Create(MapType(OldGEP->getSourceElementType()), *Ptr, Indices);
    GEP->setIsInBounds(OldGEP->isInBounds());
    ir.Insert(GEP);
    return GEP;
  }
  case llvm::Instruction::Load: {
    auto *OldLoad = llvm::cast<llvm::LoadInst>(Inst);
    auto Ptr = operand(0);
    if (!Ptr)
      return Ptr.takeError();
    auto *Load = new llvm::LoadInst(MapType(Inst->getType()), *Ptr, "", OldLoad->isVolatile(), OldLoad->getAlign());
    if (OldLoad->isAtomic())
      Load->setOrdering(OldLoad->getOrdering());
    ir.Insert(Load);
    return Load;
  }
  case llvm::Instruction::Store: {
    auto *OldStore = llvm::cast<llvm::StoreInst>(Inst);
    auto Value = operand(0);
    if (!Value)
      return Value.takeError();
    auto Ptr = operand(1);
    if (!Ptr)
      return Ptr.takeError();
    auto *Store = new llvm::StoreInst(*Value, *Ptr, OldStore->isVolatile(), OldStore->getAlign());
    if (OldStore->isAtomic())
      Store->setOrdering(OldStore->getOrdering());
    ir.Insert(Store);
    return (llvm::Value *)nullptr;
  }
  case llvm::Instruction::Alloca: {
    auto *OldAlloca = llvm::cast<llvm::AllocaInst>(Inst);
    llvm::Value *Size = nullptr;
    if (OldAlloca->isArrayAllocation()) {
      auto MappedSize = operand(0);
      if (!MappedSize)
        return MappedSize.takeError();
      Size = *MappedSize;
    }
    auto *Alloca = new llvm::AllocaInst(
        MapType(OldAlloca->getAllocatedType()), OldAlloca->getAddressSpace(), Size, OldAlloca->getAlign()
    );
    ir.Insert(Alloca);
    return Alloca;
  }
  case llvm::Instruction::ExtractElement: {
    auto Vectpr = operand(0);
    if (!Vectpr)
      return Vectpr.takeError();
    auto Index = operand(1);
    if (!Index)
      return Index.takeError();
    auto *ExtractElement = llvm::ExtractElementInst::Create(*Vectpr, *Index);
    ir.Insert(ExtractElement);
    return ExtractElement;
  }
  case llvm::Instruction::InsertElement: {
    auto Vector = operand(0);
    if (!Vector)
      return Vector.takeError();
    auto Element = operand(1);
    if (!Element)
      return Element.takeError();
    auto Index = operand(2);
    if (!Index)
      return Index.takeError();
    auto *InsertElement = llvm::InsertElementInst::Create(*Vector, *Element, *Index);
    ir.Insert(InsertElement);
    return InsertElement;
  }
  case llvm::Instruction::ShuffleVector: {
    auto First = operand(0);
    if (!First)
      return First.takeError();
    auto Second = operand(1);
    if (!Second)
      return Second.takeError();
    auto Mask = operand(2);
    if (!Mask)
      return Mask.takeError();
    return ir.CreateShuffleVector(*First, *Second, *Mask);
  }
  case llvm::Instruction::ExtractValue: {
    auto *OldExtract = llvm::cast<llvm::ExtractValueInst>(Inst);
    auto Aggregate = operand(0);
    if (!Aggregate)
      return Aggregate.takeError();
    auto *ExtractValue = llvm::ExtractValueInst::Create(*Aggregate, OldExtract->getIndices());
    ir.Insert(ExtractValue);
    return ExtractValue;
  }
  case llvm::Instruction::InsertValue: {
    auto *OldInsert = llvm::cast<llvm::InsertValueInst>(Inst);
    auto Aggregate = operand(0);
    if (!Aggregate)
      return Aggregate.takeError();
    auto Value = operand(1);
    if (!Value)
      return Value.takeError();
    auto *InsertValue = llvm::InsertValueInst::Create(*Aggregate, *Value, OldInsert->getIndices());
    ir.Insert(InsertValue);
    return InsertValue;
  }
  case llvm::Instruction::Freeze: {
    auto Value = operand(0);
    if (!Value)
      return Value.takeError();
    return ir.CreateFreeze(*Value);
  }
  case llvm::Instruction::Call:
    return TranslateCall(*llvm::cast<llvm::CallInst>(Inst));
  case llvm::Instruction::AtomicRMW: {
    auto *OldAtomic = llvm::cast<llvm::AtomicRMWInst>(Inst);
    auto Ptr = operand(0);
    if (!Ptr)
      return Ptr.takeError();
    auto Value = operand(1);
    if (!Value)
      return Value.takeError();
    return ir.CreateAtomicRMW(
        OldAtomic->getOperation(), *Ptr, *Value, OldAtomic->getAlign(), OldAtomic->getOrdering(),
        OldAtomic->getSyncScopeID()
    );
  }
  default:
    return unsupported(std::string("dxil: unsupported instruction: ") + Inst->getOpcodeName());
  }
}

llvm::Expected<HandleInfo>
Converter::ResolveHandleOperand(llvm::Value *Operand) {
  auto Found = handles_.find(Operand);
  if (Found == handles_.end())
    return unsupported("dxil: unresolvable resource handle");
  return Found->second;
}

llvm::Optional<dxmt::dxbc::TextureDescirptor>
Converter::GetTextureDescriptor(const HandleInfo &Handle) {
  return Handle.resource_class == ResourceClass::SRV ? ctx.binding.GetSRVTexture(air, Handle.range_id, Handle.index)
                                                     : ctx.binding.GetUAVTexture(air, Handle.range_id, Handle.index);
}

llvm::Optional<dxmt::dxbc::BufferDescriptor>
Converter::GetBufferDescriptor(const HandleInfo &Handle) {
  return Handle.resource_class == ResourceClass::SRV ? ctx.binding.GetSRVBuffer(air, Handle.range_id, Handle.index)
                                                     : ctx.binding.GetUAVBuffer(air, Handle.range_id, Handle.index);
}

/* A pointer to the buffer's Type at ByteOffset, or a null one when it isn't all within the view's byte length (its
   metadata): Direct3D's robust buffer access, as the DXBC path's CreateGEPInt32WithBoundCheck gives it. A load from
   the null page reads 0 and a store to it is dropped. */
llvm::Value *
Converter::BufferElementPointer(const dxmt::dxbc::BufferDescriptor &Buffer, llvm::Value *ByteOffset, llvm::Type *Type) {
  auto AddrSpace = Buffer.Pointer->getType()->getPointerAddressSpace();
  auto *Ptr = ir.CreatePointerCast(
      ir.CreateGEP(ir.getInt8Ty(), ir.CreatePointerCast(Buffer.Pointer, ir.getInt8PtrTy(AddrSpace)), {ByteOffset}),
      Type->getPointerTo(AddrSpace)
  );
  if (!Buffer.Metadata)
    return Ptr;
  auto *ByteLength = ir.CreateTrunc(Buffer.Metadata, ir.getInt32Ty());
  auto *End = ir.CreateAdd(ByteOffset, ir.getInt32(Type->getPrimitiveSizeInBits() / 8));
  auto *Within = ir.CreateAnd(ir.CreateICmpULT(ByteOffset, ByteLength), ir.CreateICmpULE(End, ByteLength));
  return ir.CreateSelect(Within, Ptr, Constant::getNullValue(Ptr->getType()));
}

llvm::Expected<dxmt::dxbc::SamplerDescriptor>
Converter::GetSamplerDescriptor(const HandleInfo &Handle) {
  auto descriptor = ctx.binding.GetSampler(air, Handle.range_id, Handle.index);
  if (!descriptor)
    return unsupported("dxil: no sampler descriptor for resource");
  return *descriptor;
}

bool
Converter::SupportsMemoryCoherency() const {
  return ctx.metal_version >= SM50_SHADER_METAL_320;
}

bool
Converter::SupportsNonExecutionBarrier() const {
  return ctx.metal_version >= SM50_SHADER_METAL_320;
}

llvm::air::Texture
Converter::MakeTexture(const dxmt::dxbc::TextureDescirptor &Descriptor) {
  return llvm::air::Texture{
      .kind = Descriptor.ResourceKind,
      .sample_type = Descriptor.SampleType,
      .memory_access = Descriptor.MemoryAccess,
  };
}

Value *
Converter::BitcastIfNeeded(Value *Value, Type *Type) {
  if (Value->getType() == Type)
    return Value;
  return ir.CreateBitCast(Value, Type);
}

Value *
Converter::BuildResRet(const CallInst &Call, Value *VecOrScalar, Value *Residency) {
  auto *TyStruct = cast<StructType>(MapType(Call.getType()));
  Type *Element = TyStruct->getElementType(0);
  Value *Result = PoisonValue::get(TyStruct);
  if (auto *Vector = dyn_cast<FixedVectorType>(VecOrScalar->getType())) {
    for (unsigned i = 0; i < 4 && i < Vector->getNumElements(); i++)
      Result = ir.CreateInsertValue(Result, ir.CreateExtractElement(VecOrScalar, (uint64_t)i), i);
  } else {
    Result = ir.CreateInsertValue(Result, VecOrScalar, 0);
    for (unsigned i = 1; i < 4; i++)
      Result = ir.CreateInsertValue(Result, Constant::getNullValue(Element), i);
  }
  Value *Status = Residency ? ir.CreateZExt(Residency, ir.getInt32Ty()) : ir.getInt32(0);
  Result = ir.CreateInsertValue(Result, Status, 4);
  return Result;
}

Value *
Converter::MakeVec(ArrayRef<Value *> Components) {
  Type *element = Components.front()->getType();
  Value *vec = PoisonValue::get(FixedVectorType::get(element, Components.size()));
  for (unsigned i = 0; i < Components.size(); i++)
    vec = ir.CreateInsertElement(vec, Components[i], (uint64_t)i);
  return vec;
}

Value *
Converter::AddOffsetIfNeeded(Value *Coord, int32_t Offset) {
  if (Offset == 0)
    return Coord;
  return ir.CreateAdd(Coord, ir.getInt32(Offset));
}

Type *
Converter::GetTextureSampleType(Texture::SampleType SampleType) {
  switch (SampleType) {
  case Texture::sample_float:
    return ir.getFloatTy();
  case Texture::sample_int:
  case Texture::sample_uint:
    return ir.getInt32Ty();
  case Texture::sample_half:
    return ir.getHalfTy();
  case Texture::sample_short:
  case Texture::sample_ushort:
    return ir.getInt16Ty();
  }
}

llvm::Value *
Converter::GetSamplePos(llvm::Value *SampleCount, llvm::Value *Index) {
  using namespace llvm;

  auto &Context = air.getContext();
  auto Attrs = AttributeList::get(
      Context, {{~0U, Attribute::get(Context, Attribute::AttrKind::NoUnwind)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::WillReturn)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::ReadNone)}}
  );

  std::string FnName = "dxmt.sample_pos.i32";
  auto Fn = air.getModule()->getOrInsertFunction(
      FnName, llvm::FunctionType::get(air.getFloatTy(2), {air.getIntTy(), air.getIntTy()}, false), Attrs
  );
  return ir.CreateCall(Fn, {SampleCount, Index});
}

llvm::Expected<std::tuple<Value *, Value *, Value *>>
Converter::BuildRwAddress(
    Texture::ResourceKind LogicalKind, Value *C0, Value *C1, Value *C2, Value *MipOrSampleIndex, int32_t Off0,
    int32_t Off1, int32_t Off2
) {
  Value *Position = nullptr;
  Value *ArraySlice = nullptr;
  Value *SampleIndex = nullptr;
  switch (LogicalKind) {
  case Texture::texture1d:
    Position = MakeVec({AddOffsetIfNeeded(C0, Off0), ir.getInt32(0)});
    break;
  case Texture::texture1d_array:
    Position = MakeVec({AddOffsetIfNeeded(C0, Off0), ir.getInt32(0)});
    ArraySlice = C1;
    break;
  case Texture::texture2d:
  case Texture::depth2d:
    Position = MakeVec({AddOffsetIfNeeded(C0, Off0), AddOffsetIfNeeded(C1, Off1)});
    break;
  case Texture::texture2d_array:
  case Texture::depth2d_array:
    Position = MakeVec({AddOffsetIfNeeded(C0, Off0), AddOffsetIfNeeded(C1, Off1)});
    ArraySlice = C2;
    break;
  case Texture::texture3d:
    Position = MakeVec({AddOffsetIfNeeded(C0, Off0), AddOffsetIfNeeded(C1, Off1), AddOffsetIfNeeded(C2, Off2)});
    break;
  case Texture::texture2d_ms:
  case Texture::depth_2d_ms:
    Position = MakeVec({AddOffsetIfNeeded(C0, Off0), AddOffsetIfNeeded(C1, Off1)});
    SampleIndex = MipOrSampleIndex;
    break;
  case Texture::texture2d_ms_array:
  case Texture::depth_2d_ms_array:
    Position = MakeVec({AddOffsetIfNeeded(C0, Off0), AddOffsetIfNeeded(C1, Off1)});
    ArraySlice = C2;
    SampleIndex = MipOrSampleIndex;
    break;
  case Texture::texture_buffer:
    Position = C0;
    break;
  default:
    return unsupported(
        "dxil: unsupported texture kind for indexing: " + std::to_string(static_cast<uint32_t>(LogicalKind))
    );
  }
  return std::make_tuple(Position, ArraySlice, SampleIndex);
}

llvm::Expected<llvm::Value *>
Converter::TranslateCall(llvm::CallInst &Call) {
  auto *Callee = Call.getCalledFunction();
  if (!Callee)
    return unsupported("dxil: indirect call");
  if (Callee->getName().startswith("dx.op."))
    return EmitDxilOp(Call);
  // Lifetime markers (DXIL 1.6 has them, for local arrays) only help optimization.
  if (Callee->getName().startswith("llvm.lifetime."))
    return (llvm::Value *)nullptr;
  return unsupported("dxil: unsupported call to " + Callee->getName().str());
}

llvm::Expected<llvm::Value *>
Converter::EmitDxilOp(CallInst &Call) {
  auto Op = asU32(Call.getArgOperand(0));
  if (!Op)
    return unsupported("dxil: non-constant dxil op");

  switch (auto OpId = *Op) {
  case kOpCreateHandle:
    return EmitCreateHandle(Call);
  case kOpCreateHandleFromBinding:
    return EmitCreateHandleFromBinding(Call);
  case kOpAnnotateHandle:
    return EmitAnnotateHandle(Call);
  case kOpCBufferLoadLegacy:
    return EmitCbufferLoadLegacy(Call);
  case kOpThreadId:
    return EmitThreadId(Call, ThreadBuiltin::kGlobalId);
  case kOpGroupId:
    return EmitThreadId(Call, ThreadBuiltin::kGroupId);
  case kOpThreadIdInGroup:
    return EmitThreadId(Call, ThreadBuiltin::kLocalId);
  case kOpFlattenedThreadIdInGroup:
    return EmitThreadId(Call, ThreadBuiltin::kLocalLinearId);
  case kOpBufferStore:
    return EmitBufferStore(Call);
  case kOpBufferLoad:
    return EmitBufferLoad(Call);
  case kOpTextureStore:
    return EmitTextureStore(Call);
  case kOpTextureLoad:
    return EmitTextureLoad(Call);
  case kOpBufferUpdateCounter:
    return EmitBufferUpdateCounter(Call);
  case kOpGetDimensions:
    return EmitGetDimensions(Call);
  case kOpRawBufferStore:
    return EmitRawBufferStore(Call);
  case kOpRawBufferLoad:
    return EmitRawBufferLoad(Call);
  case kOpLegacyF16ToF32:
    return EmitLegacyF16ToF32(Call);
  case kOpLegacyF32ToF16:
    return EmitLegacyF32ToF16(Call);
  case kOpCountbits:
  case kOpFirstbitLo:
  case kOpFirstbitHi:
  case kOpFirstbitSHi:
    return EmitUnaryBits(Call, OpId);
  case kOpBarrier:
    return EmitBarrier(Call);
  case kOpLoadInput:
    return EmitLoadInput(Call);
  case kOpStoreOutput:
    return EmitStoreOutput(Call);
  case kOpSample:
  case kOpSampleLevel:
  case kOpSampleBias:
  case kOpSampleCmp:
  case kOpSampleCmpLevelZero:
  case kOpSampleGrad:
    return EmitSample(Call, OpId);
  case kOpTextureGather:
  case kOpTextureGatherCmp:
    return EmitGather(Call, OpId);
  case kOpBfrev:
  case kOpDerivCoarseX:
  case kOpDerivCoarseY:
  case kOpDerivFineX:
  case kOpDerivFineY:
  case kOpFAbs:
  case kOpSaturate:
  case kOpCos:
  case kOpSin:
  case kOpTan:
  case kOpAcos:
  case kOpAsin:
  case kOpAtan:
  case kOpHcos:
  case kOpHsin:
  case kOpHtan:
  case kOpExp:
  case kOpFrc:
  case kOpLog:
  case kOpSqrt:
  case kOpRsqrt:
  case kOpRound_ne:
  case kOpRound_ni:
  case kOpRound_pi:
  case kOpRound_z:
    return EmitUnary(Call, OpId);
  case kOpFMax:
  case kOpFMin:
  case kOpIMax:
  case kOpIMin:
  case kOpUMax:
  case kOpUMin:
    return EmitBinary(Call, OpId);
  case kOpFMad:
  case kOpFma:
  case kOpIMad:
  case kOpUMad:
  case kOpIbfe:
  case kOpUbfe:
    return EmitTertiary(Call, OpId);
  case kOpDot2:
    return EmitDotProduct(Call, 2);
  case kOpDot3:
    return EmitDotProduct(Call, 3);
  case kOpDot4:
    return EmitDotProduct(Call, 4);
  case kOpDot2AddHalf:
    return EmitDot2AddHalf(Call);
  case kOpDot4AddI8Packed:
  case kOpDot4AddU8Packed:
    return EmitDot4AddPacked(Call, OpId);
  case kOpPack4x8:
    return EmitPack4x8(Call);
  case kOpUnpack4x8:
    return EmitUnpack4x8(Call);
  case kOpIsNaN:
  case kOpIsInf:
  case kOpIsFinite:
  case kOpIsNormal:
    return EmitIsSpecialFloat(Call, OpId);
  case kOpAtomicBinOp:
    return EmitAtomicRMW(Call);
  case kOpAtomicCmpXchg:
    return EmitAtomicCmpXchg(Call);
  case kOpDiscard:
    return EmitDiscard(Call);
  case kOpCalculateLOD:
    return EmitCalculateLOD(Call);
  case kOpTexture2DMSGetSamplePosition:
    return EmitGetTextureSamplePos(Call);
  case kOpWaveGetLaneIndex:
    return air.CreateGetBuiltinID("get_thread_index_in_simdgroup");
  case kOpWaveGetLaneCount:
    return air.CreateGetBuiltinID("get_simdgroup_size");
  case kOpQuadReadLaneAt:
    return EmitReadLaneAt(Call, true);
  case kOpWaveReadLaneAt:
    return EmitReadLaneAt(Call, false);
  case kOpQuadOp:
    return EmitQuadOp(Call);
  case kOpSampleIndex:
    return ctx.resource.sample_index_arg;
  case kOpCoverage:
    return ctx.pso_sample_mask != 0xffffffff ? ir.CreateAnd(ctx.resource.coverage_mask_arg, ctx.pso_sample_mask)
                                             : ctx.resource.coverage_mask_arg;
  case kOpRenderTargetGetSampleCount:
    return air.CreateGetNumSamples();
  case kOpRenderTargetGetSamplePosition: {
    auto Index = Get(Call.getOperand(1));
    if (!Index)
      return Index.takeError();
    auto Position = GetSamplePos(air.CreateGetNumSamples(), *Index);
    return ir.CreateInsertValue(
        ir.CreateInsertValue(
            llvm::PoisonValue::get(MapType(Call.getType())), ir.CreateExtractElement(Position, 0ull), {0ull}
        ),
        ir.CreateExtractElement(Position, 1ull), {1ull}
    );
  }
  case kOpEmitStream: {
    ctx.resource.call_emit();
    return nullptr;
  }
  case kOpEmitThenCutStream: {
    ctx.resource.call_emit();
    ctx.resource.call_cut();
    return nullptr;
  }
  case kOpCutStream: {
    ctx.resource.call_cut();
    return nullptr;
  }
  case kOpGSInstanceID:
    return ctx.resource.gs_instance_id;
  case kOpPrimitiveID:
    return ctx.resource.patch_id;
  case kOpUDiv:
  case kOpUMul:
  case kOpIMul:
  case kOpUAddc:
  case kOpUSubb:
    return EmitBinaryWithTwoOutputs(Call, OpId);
  case kOpBitcastF16toI16:
  case kOpBitcastF32toI32:
  case kOpBitcastI16toF16:
  case kOpBitcastI32toF32:
  case kOpBitcastF64toI64:
  case kOpBitcastI64toF64: {
    auto Operand = Get(Call.getOperand(1));
    if (!Operand)
      return Operand.takeError();
    return ir.CreateBitCast(*Operand, MapType(Call.getType()));
  }
  case kOpOutputControlPointID:
    return ctx.resource.thread_id_in_patch;
  case kOpDomainLocation: {
    auto Component = asU32(Call.getOperand(1));
    if (!Component)
      return unsupported("dxil: non-constant domainLocation component");
    return ir.CreateExtractElement(ctx.resource.domain, *Component);
  }
  case kOpStorePatchConstant:
    return EmitStorePatchConstant(Call);
  case kOpLoadOutputControlPoint:
    return EmitLoadOutputControlPoint(Call);
  case kOpLoadPatchConstant:
    return EmitLoadPatchConstant(Call);
  case kOpViewID:
    return Constant::getNullValue(MapType(Call.getType()));
  case kOpEvalCentroid:
  case kOpEvalSampleIndex:
  case kOpEvalSnapped:
    return EmitEval(Call, OpId);
  case kOpWaveAllTrue:
  case kOpWaveAnyTrue: {
    auto Expr = Get(Call.getOperand(1));
    if (!Expr)
      return Expr.takeError();
    return air.CreateSIMDAllAny(*Expr, OpId == kOpWaveAnyTrue);
  }
  case kOpWaveIsFirstLane:
    return air.CreateSIMDIsFirst();
  case kOpWaveActiveOp:
  case kOpWavePrefixOp: {
    auto Operand = Get(Call.getOperand(1));
    if (!Operand)
      return Operand.takeError();
    if (Operand.get()->getType()->getScalarType()->isIntOrIntVectorTy(64))
      return unsupported("dxil: 64-bit wave ops not supported");
    auto Op = asU32(Call.getOperand(2));
    if (!Op || *Op > 4)
      return unsupported("dxil: invalid waveActiveOp op");
    auto Sign = asU32(Call.getOperand(3));
    if (!Sign || *Sign > 1)
      return unsupported("dxil: invalid waveActiveOp sign");
    return air.CreateSIMDOp(
        (AIRBuilder::SIMDOp)(AIRBuilder::simd_sum + *Op), *Operand, !*Sign, OpId == kOpWavePrefixOp
    );
  }
  case kOpWaveAllBitCount:
  case kOpWavePrefixBitCount: {
    auto Expr = Get(Call.getOperand(1));
    if (!Expr)
      return Expr.takeError();
    return air.CreateSIMDOp(
        AIRBuilder::simd_sum, ir.CreateZExt(*Expr, ir.getInt32Ty()), false, OpId == kOpWavePrefixBitCount
    );
  }
  case kOpWaveActiveBit: {
    auto Operand = Get(Call.getOperand(1));
    if (!Operand)
      return Operand.takeError();
    if (Operand.get()->getType()->getScalarType()->isIntOrIntVectorTy(64))
      return unsupported("dxil: 64-bit wave ops not supported");
    auto Op = asU32(Call.getOperand(2));
    if (!Op || *Op > 2)
      return unsupported("dxil: invalid waveActiveBit op");
    return air.CreateSIMDOp((AIRBuilder::SIMDOp)(AIRBuilder::simd_and + *Op), *Operand, false);
  }
  case kOpWaveActiveBallot: {
    auto Cond = Get(Call.getOperand(1));
    if (!Cond)
      return Cond.takeError();
    auto Result = ir.CreateBitCast(air.CreateSIMDBallot(*Cond), air.getIntTy(2));
    return ir.CreateInsertValue(
        ir.CreateInsertValue(
            llvm::ConstantAggregateZero::get(MapType(Call.getType())), ir.CreateExtractElement(Result, 0ull), {0ull}
        ),
        ir.CreateExtractElement(Result, 1ull), {1ull}
    );
  }
  case kOpWaveReadLaneFirst: {
    auto Operand = Get(Call.getOperand(1));
    if (!Operand)
      return Operand.takeError();
    if (Operand.get()->getType()->getScalarType()->isIntOrIntVectorTy(64))
      return unsupported("dxil: 64-bit wave ops not supported");
    return air.CreateSIMDBroadcastFirst(*Operand);
  }
  case kOpWaveActiveAllEqual:
    return EmitWaveActiveAllEqual(Call);
  case kOpMsad:
    return EmitMSAD(Call);
  case kOpBfi:
    return EmitBitfieldInsert(Call);
  case kOpCBufferLoad:
    return EmitCbufferLoad(Call);
  default:
    break;
  }
  auto *callee = Call.getCalledFunction();
  return unsupported("dxil: unsupported operation " + callee->getName().str() + "(" + std::to_string(*Op) + ")");
}

llvm::Expected<llvm::Value *>
Converter::EmitCreateHandle(CallInst &Call) {
  auto Class = asU32(Call.getArgOperand(1));
  auto Range = asU32(Call.getArgOperand(2));
  if (!Class || !Range)
    return unsupported("dxil: non-constant createHandle arguments");
  auto Index = Get(Call.getArgOperand(3));
  if (!Index)
    return Index.takeError();
  handles_[&Call] = HandleInfo{(ResourceClass)*Class, *Range, *Index};
  return (llvm::Value *)nullptr;
}

/* Shader model 6.6's handle: its index is the register, as createHandle's is. */
llvm::Expected<llvm::Value *>
Converter::EmitCreateHandleFromBinding(CallInst &Call) {
  auto *Resource = find_binding(entry, Call.getArgOperand(1));
  if (!Resource)
    return unsupported("dxil: createHandleFromBinding outside the declared resources");
  auto Index = Get(Call.getArgOperand(2));
  if (!Index)
    return Index.takeError();
  handles_[&Call] = HandleInfo{Resource->resource_class, Resource->id, *Index};
  return (llvm::Value *)nullptr;
}

/* The properties annotateHandle gives are the declaration's, which the handle has already: it stays the same. */
llvm::Expected<llvm::Value *>
Converter::EmitAnnotateHandle(CallInst &Call) {
  auto Handle = ResolveHandleOperand(Call.getArgOperand(1));
  if (!Handle)
    return Handle.takeError();
  handles_[&Call] = *Handle;
  return (llvm::Value *)nullptr;
}

llvm::Expected<llvm::Value *>
Converter::EmitCbufferLoadLegacy(CallInst &Call) {
  auto Handle = ResolveHandleOperand(Call.getArgOperand(1));
  if (!Handle)
    return Handle.takeError();
  auto Descriptor = ctx.binding.GetConstantBuffer(air, Handle->range_id, Handle->index);
  if (!Descriptor)
    return unsupported("dxil: no constant buffer descriptor for resource");
  auto Index = Get(Call.getArgOperand(2));
  if (!Index)
    return Index.takeError();

  auto *TyResult = MapType(Call.getType());
  auto *TyElement = TyResult->getStructElementType(0);
  auto *PackedType = FixedVectorType::get(TyElement, TyResult->getStructNumElements());
  auto *PackedPtr = ir.CreateBitCast(
      ir.CreateGEP(air.getIntTy(4), Descriptor->Pointer, {*Index}),
      PackedType->getPointerTo(Descriptor->Pointer->getType()->getPointerAddressSpace())
  );
  auto *PackedLoad = ir.CreateLoad(PackedType, PackedPtr);
  llvm::Value *Result = PoisonValue::get(TyResult);
  for (unsigned i = 0; i < PackedType->getNumElements(); i++)
    Result = ir.CreateInsertValue(Result, ir.CreateExtractElement(PackedLoad, (uint64_t)i), i);
  return Result;
}

llvm::Expected<llvm::Value *>
Converter::EmitThreadId(CallInst &Call, ThreadBuiltin Kind) {
  if (Kind == ThreadBuiltin::kLocalLinearId)
    return air.CreateGetLocalLinearID();
  auto component = asU32(Call.getArgOperand(1));
  if (!component || *component > 2)
    return unsupported("dxil: invalid thread id component");
  switch (Kind) {
  case ThreadBuiltin::kGlobalId:
    return air.CreateGetGlobalID(*component);
  case ThreadBuiltin::kGroupId:
    return air.CreateGetGroupID(*component);
  default:
    return air.CreateGetLocalID(*component);
  }
}

llvm::Value *
Converter::CreateGEPInt32WithBoundCheck(dxbc::BufferDescriptor &Buffer, llvm::Value *Index) {
  auto Addr = ir.CreateGEP(ir.getInt32Ty(), Buffer.Pointer, {Index});
  if (!Buffer.Metadata) {
    return Addr;
  }
  auto ByteLength = ir.CreateTrunc(Buffer.Metadata, air.getIntTy());
  return ir.CreateSelect(
      ir.CreateICmpULT(Index, ir.CreateLShr(ByteLength, 2)), Addr, llvm::Constant::getNullValue(Addr->getType())
  );
}

llvm::Expected<llvm::Value *>
Converter::EmitBufferStore(CallInst &Call) {
  auto Handle = ResolveHandleOperand(Call.getArgOperand(1));
  if (!Handle)
    return Handle.takeError();

  auto Index = Get(Call.getArgOperand(2));
  if (!Index)
    return Index.takeError();
  auto Mask = asU32(Call.getArgOperand(8));
  if (!Mask)
    return unsupported("dxil: non-constant bufferStore mask");

  auto Texture = GetTextureDescriptor(*Handle);
  if (Texture) {
    auto *TyElement = GetTextureSampleType(Texture->SampleType);
    auto *VecType = FixedVectorType::get(TyElement, 4);
    Value *Value = PoisonValue::get(VecType);
    for (unsigned i = 0; i < 4; i++) {
      if (!(*Mask & (1u << i)))
        continue;
      auto component = Get(Call.getArgOperand(4 + i));
      if (!component)
        return component.takeError();
      Value = ir.CreateInsertElement(Value, BitcastIfNeeded(*component, TyElement), (uint64_t)i);
    }
    auto *Address = *Index;
    auto *OOB = ir.CreateICmpUGE(Address, ir.CreateTrunc(ir.CreateLShr(Texture->Metadata, 32uLL), air.getIntTy()));
    Address = ir.CreateAdd(Address, ir.CreateTrunc(Texture->Metadata, air.getIntTy()));
    bool Coherent = Texture->GlobalCoherent && SupportsMemoryCoherency();
    air.CreateWrite(
        MakeTexture(*Texture), Texture->ResourceHandle,
        ir.CreateSelect(OOB, llvm::ConstantInt::getAllOnesValue(Address->getType()), Address), nullptr, nullptr,
        air.getInt(0), Value, Coherent
    );
    return (llvm::Value *)nullptr;
  }

  auto Buffer = GetBufferDescriptor(*Handle);
  if (!Buffer)
    return unsupported("dxil: no buffer descriptor for resource");

  Value *ByteOffset = *Index;
  if (Buffer->StructureStride != 0) {
    auto ElementOffset = Get(Call.getArgOperand(3));
    if (!ElementOffset)
      return ElementOffset.takeError();
    ByteOffset = ir.CreateMul(ByteOffset, ir.getInt32(Buffer->StructureStride));
    ByteOffset = ir.CreateAdd(ByteOffset, *ElementOffset);
  }
  auto Base = ir.CreateLShr(ByteOffset, ir.getInt32(2));
  bool Coherent = Buffer->GlobalCoherent;

  for (unsigned i = 0; i < 4; i++) {
    if (!(*Mask & (1u << i)))
      continue;
    auto Component = Get(Call.getArgOperand(4 + i));
    if (!Component)
      return Component.takeError();
    Value *Word = BitcastIfNeeded(*Component, ir.getInt32Ty());
    auto *Ptr = CreateGEPInt32WithBoundCheck(*Buffer, ir.CreateAdd(Base, ir.getInt32(i)));
    if (Coherent)
      air.CreateDeviceCoherentStore(Word, Ptr);
    else
      ir.CreateStore(Word, Ptr);
  }
  return (llvm::Value *)nullptr;
}

llvm::Expected<llvm::Value *>
Converter::EmitBufferLoad(CallInst &Call) {
  auto Handle = ResolveHandleOperand(Call.getArgOperand(1));
  if (!Handle)
    return Handle.takeError();

  auto Index = Get(Call.getArgOperand(2));
  if (!Index)
    return Index.takeError();

  auto Texture = GetTextureDescriptor(*Handle);
  if (Texture) {
    auto *Address = *Index;
    auto *OOB = ir.CreateICmpUGE(Address, ir.CreateTrunc(ir.CreateLShr(Texture->Metadata, 32uLL), air.getIntTy()));
    Address = ir.CreateAdd(Address, ir.CreateTrunc(Texture->Metadata, air.getIntTy()));
    bool Coherent = Texture->GlobalCoherent && SupportsMemoryCoherency();
    auto [Value, Residency] = air.CreateRead(
        MakeTexture(*Texture), Texture->ResourceHandle,
        ir.CreateSelect(OOB, llvm::ConstantInt::getAllOnesValue(Address->getType()), Address), nullptr, nullptr,
        air.getInt(0), Coherent
    );
    return BuildResRet(Call, Value, Residency);
  }

  auto Buffer = GetBufferDescriptor(*Handle);
  if (!Buffer)
    return unsupported("dxil: no buffer descriptor for resource");

  Value *ByteOffset = *Index;
  if (Buffer->StructureStride != 0) {
    auto ElementOffset = Get(Call.getArgOperand(3));
    if (!ElementOffset)
      return ElementOffset.takeError();
    ByteOffset = ir.CreateMul(ByteOffset, ir.getInt32(Buffer->StructureStride));
    ByteOffset = ir.CreateAdd(ByteOffset, *ElementOffset);
  }
  auto Base = ir.CreateLShr(ByteOffset, ir.getInt32(2));
  bool Coherent = Buffer->GlobalCoherent;

  Value *Words = Constant::getNullValue(FixedVectorType::get(ir.getInt32Ty(), 4));
  for (unsigned i = 0; i < 4; i++) {
    auto *Ptr = CreateGEPInt32WithBoundCheck(*Buffer, ir.CreateAdd(Base, ir.getInt32(i)));
    Value *Word = Coherent ? (Value *)air.CreateDeviceCoherentLoad(ir.getInt32Ty(), Ptr)
                           : (Value *)ir.CreateLoad(ir.getInt32Ty(), Ptr);
    Words = ir.CreateInsertElement(Words, Word, (uint64_t)i);
  }

  auto *TyStruct = cast<StructType>(MapType(Call.getType()));
  Type *TyElement = TyStruct->getElementType(0);
  if (TyElement->isFloatingPointTy())
    Words = ir.CreateBitCast(Words, FixedVectorType::get(TyElement, 4));
  return BuildResRet(Call, Words, nullptr);
}

llvm::Expected<llvm::Value *>
Converter::EmitTextureStore(CallInst &Call) {
  auto Handle = ResolveHandleOperand(Call.getArgOperand(1));
  if (!Handle)
    return Handle.takeError();
  auto Descriptor = GetTextureDescriptor(*Handle);
  if (!Descriptor)
    return unsupported("dxil: no texture descriptor for resource");

  auto C0 = Get(Call.getArgOperand(2));
  if (!C0)
    return C0.takeError();
  auto C1 = Get(Call.getArgOperand(3));
  if (!C1)
    return C1.takeError();
  auto C2 = Get(Call.getArgOperand(4));
  if (!C2)
    return C2.takeError();

  auto Mask = asU32(Call.getArgOperand(9));
  if (!Mask)
    return unsupported("dxil: non-constant textureStore mask");

  auto Address = BuildRwAddress(Descriptor->ResourceKindLogical, *C0, *C1, *C2, nullptr, 0, 0, 0);
  if (!Address)
    return Address.takeError();

  auto [Position, Slice, Unused] = *Address;
  auto Texture = MakeTexture(*Descriptor);
  auto *TyElement = GetTextureSampleType(Texture.sample_type);
  auto *TyVec = FixedVectorType::get(TyElement, 4);
  Value *Value = PoisonValue::get(TyVec);
  for (unsigned i = 0; i < 4; i++) {
    if (!(*Mask & (1u << i)))
      continue;
    auto Component = Get(Call.getArgOperand(5 + i));
    if (!Component)
      return Component.takeError();
    Value = ir.CreateInsertElement(Value, BitcastIfNeeded(*Component, TyElement), (uint64_t)i);
  }

  bool Coherent = Descriptor->GlobalCoherent && SupportsMemoryCoherency();
  air.CreateWrite(Texture, Descriptor->ResourceHandle, Position, Slice, nullptr, air.getInt(0), Value, Coherent);
  return (llvm::Value *)nullptr;
}

llvm::Expected<llvm::Value *>
Converter::EmitTextureLoad(CallInst &Call) {
  auto Handle = ResolveHandleOperand(Call.getArgOperand(1));
  if (!Handle)
    return Handle.takeError();
  auto Descriptor = GetTextureDescriptor(*Handle);
  if (!Descriptor)
    return unsupported("dxil: no texture descriptor for resource");

  auto MipOrSampleIndex = Get(Call.getArgOperand(2));
  if (!MipOrSampleIndex)
    return MipOrSampleIndex.takeError();
  auto C0 = Get(Call.getArgOperand(3));
  if (!C0)
    return C0.takeError();
  auto C1 = Get(Call.getArgOperand(4));
  if (!C1)
    return C1.takeError();
  auto C2 = Get(Call.getArgOperand(5));
  if (!C2)
    return C2.takeError();

  int32_t O0 = tryGetI32(Call.getArgOperand(6));
  int32_t O1 = tryGetI32(Call.getArgOperand(7));
  int32_t O2 = tryGetI32(Call.getArgOperand(8));

  auto Address = BuildRwAddress(Descriptor->ResourceKindLogical, *C0, *C1, *C2, *MipOrSampleIndex, O0, O1, O2);
  if (!Address)
    return Address.takeError();

  auto [Position, Slice, SampleIndex] = *Address;
  auto Texture = MakeTexture(*Descriptor);
  Value *Level = SampleIndex ? air.getInt(0) : *MipOrSampleIndex;
  bool Coherent = Descriptor->GlobalCoherent && SupportsMemoryCoherency();
  auto [Value, Residency] =
      air.CreateRead(Texture, Descriptor->ResourceHandle, Position, Slice, SampleIndex, Level, Coherent);
  return BuildResRet(Call, Value, Residency);
}

llvm::Expected<llvm::Value *>
Converter::EmitBufferUpdateCounter(CallInst &Call) {
  auto Handle = ResolveHandleOperand(Call.getArgOperand(1));
  if (!Handle)
    return Handle.takeError();
  auto Descriptor = ctx.binding.GetUAVCounter(air, Handle->range_id, Handle->index);
  if (!Descriptor)
    return unsupported("dxil: no counter descriptor for resource");
  if (auto Increment = llvm::dyn_cast<llvm::ConstantInt>(Call.getArgOperand(2))) {
    auto Amount = Increment->getSExtValue();
    auto *OldValue = air.CreateAtomicRMW(AtomicRMWInst::Add, Descriptor->Pointer, ir.getInt32(Amount));
    if (!OldValue)
      return unsupported("dxil: buffer counter update failed");
    if (Amount < 0)
      return ir.CreateAdd(OldValue, ir.getInt32(Amount));
    return OldValue;
  }
  return unsupported("dxil: non-constant buffer counter increment");
}

llvm::Expected<llvm::Value *>
Converter::EmitGetDimensions(CallInst &Call) {
  auto Handle = ResolveHandleOperand(Call.getArgOperand(1));
  if (!Handle)
    return Handle.takeError();

  auto Level = Get(Call.getArgOperand(2));
  if (!Level)
    return Level.takeError();

  Value *C1 = llvm::UndefValue::get(ir.getInt32Ty());
  Value *C2 = llvm::UndefValue::get(ir.getInt32Ty());
  Value *C3 = llvm::UndefValue::get(ir.getInt32Ty());
  Value *C4 = llvm::UndefValue::get(ir.getInt32Ty());

  if (auto Descriptor = GetTextureDescriptor(*Handle)) {
    auto Texture = MakeTexture(*Descriptor);
    switch (Descriptor->ResourceKindLogical) {
    case Texture::texture_buffer:
      C1 = ir.CreateTrunc(ir.CreateLShr(Descriptor->Metadata, 32uLL), air.getIntTy());
      break;
    case Texture::texture1d:
      C1 = air.CreateTextureQuery(Texture, Descriptor->ResourceHandle, Texture::width, *Level);
      C4 = air.CreateTextureQuery(Texture, Descriptor->ResourceHandle, Texture::num_mip_levels, nullptr);
      break;
    case Texture::texture1d_array:
      C1 = air.CreateTextureQuery(Texture, Descriptor->ResourceHandle, Texture::width, *Level);
      C2 = air.CreateTextureQuery(Texture, Descriptor->ResourceHandle, Texture::array_length, nullptr);
      C4 = air.CreateTextureQuery(Texture, Descriptor->ResourceHandle, Texture::num_mip_levels, nullptr);
      break;
    case Texture::texture2d:
    case Texture::depth2d:
      C1 = air.CreateTextureQuery(Texture, Descriptor->ResourceHandle, Texture::width, *Level);
      C2 = air.CreateTextureQuery(Texture, Descriptor->ResourceHandle, Texture::height, *Level);
      C4 = air.CreateTextureQuery(Texture, Descriptor->ResourceHandle, Texture::num_mip_levels, nullptr);
      break;
    case Texture::texture2d_array:
    case Texture::depth2d_array:
      C1 = air.CreateTextureQuery(Texture, Descriptor->ResourceHandle, Texture::width, *Level);
      C2 = air.CreateTextureQuery(Texture, Descriptor->ResourceHandle, Texture::height, *Level);
      C3 = air.CreateTextureQuery(Texture, Descriptor->ResourceHandle, Texture::array_length, nullptr);
      C4 = air.CreateTextureQuery(Texture, Descriptor->ResourceHandle, Texture::num_mip_levels, nullptr);
      break;
    case Texture::texture3d:
      C1 = air.CreateTextureQuery(Texture, Descriptor->ResourceHandle, Texture::width, *Level);
      C2 = air.CreateTextureQuery(Texture, Descriptor->ResourceHandle, Texture::height, *Level);
      C3 = air.CreateTextureQuery(Texture, Descriptor->ResourceHandle, Texture::depth, *Level);
      C4 = air.CreateTextureQuery(Texture, Descriptor->ResourceHandle, Texture::num_mip_levels, nullptr);
      break;
    case Texture::texturecube:
    case Texture::depthcube:
      C1 = air.CreateTextureQuery(Texture, Descriptor->ResourceHandle, Texture::width, *Level);
      C2 = air.CreateTextureQuery(Texture, Descriptor->ResourceHandle, Texture::height, *Level);
      C4 = air.CreateTextureQuery(Texture, Descriptor->ResourceHandle, Texture::num_mip_levels, nullptr);
      break;
    case Texture::texturecube_array:
    case Texture::depthcube_array:
      C1 = air.CreateTextureQuery(Texture, Descriptor->ResourceHandle, Texture::width, *Level);
      C2 = air.CreateTextureQuery(Texture, Descriptor->ResourceHandle, Texture::height, *Level);
      C3 = air.CreateTextureQuery(Texture, Descriptor->ResourceHandle, Texture::array_length, nullptr);
      C4 = air.CreateTextureQuery(Texture, Descriptor->ResourceHandle, Texture::num_mip_levels, nullptr);
      break;
    case Texture::texture2d_ms:
    case Texture::depth_2d_ms:
      C1 = air.CreateTextureQuery(Texture, Descriptor->ResourceHandle, Texture::width, nullptr);
      C2 = air.CreateTextureQuery(Texture, Descriptor->ResourceHandle, Texture::height, nullptr);
      C4 = air.CreateTextureQuery(Texture, Descriptor->ResourceHandle, Texture::num_samples, nullptr);
      break;
    case Texture::texture2d_ms_array:
    case Texture::depth_2d_ms_array:
      C1 = air.CreateTextureQuery(Texture, Descriptor->ResourceHandle, Texture::width, nullptr);
      C2 = air.CreateTextureQuery(Texture, Descriptor->ResourceHandle, Texture::height, nullptr);
      C3 = air.CreateTextureQuery(Texture, Descriptor->ResourceHandle, Texture::array_length, nullptr);
      C4 = air.CreateTextureQuery(Texture, Descriptor->ResourceHandle, Texture::num_samples, nullptr);
      break;
    }
  } else if (auto Buffer = GetBufferDescriptor(*Handle)) {
    C1 = ir.CreateTrunc(Buffer->Metadata, air.getIntTy());
    if (Buffer->StructureStride) {
      C1 = ir.CreateUDiv(C1, ir.getInt32(Buffer->StructureStride));
    }
  } else
    return unsupported("dxil: no buffer descriptor for resource");

  auto *TyStruct = cast<StructType>(MapType(Call.getType()));
  Value *Ret = llvm::PoisonValue::get(TyStruct);
  Ret = ir.CreateInsertValue(Ret, C1, {0u});
  Ret = ir.CreateInsertValue(Ret, C2, {1u});
  Ret = ir.CreateInsertValue(Ret, C3, {2u});
  Ret = ir.CreateInsertValue(Ret, C4, {3u});
  return Ret;
}

llvm::Expected<llvm::Value *>
Converter::EmitRawBufferStore(CallInst &Call) {
  auto Handle = ResolveHandleOperand(Call.getArgOperand(1));
  if (!Handle)
    return Handle.takeError();

  auto Index = Get(Call.getArgOperand(2));
  if (!Index)
    return Index.takeError();
  auto Mask = asU32(Call.getArgOperand(8));
  if (!Mask)
    return unsupported("dxil: non-constant rawBufferStore mask");

  auto Component0 = Get(Call.getArgOperand(4));
  if (!Component0)
    return Component0.takeError();

  auto Buffer = GetBufferDescriptor(*Handle);
  if (!Buffer)
    return unsupported("dxil: no buffer descriptor for resource");

  Value *ByteOffset = *Index;
  if (Buffer->StructureStride != 0) {
    auto ElementOffset = Get(Call.getArgOperand(3));
    if (!ElementOffset)
      return ElementOffset.takeError();
    ByteOffset = ir.CreateMul(ByteOffset, ir.getInt32(Buffer->StructureStride));
    ByteOffset = ir.CreateAdd(ByteOffset, *ElementOffset);
  }
  bool Coherent = Buffer->GlobalCoherent;
  auto *TyElement = (*Component0)->getType();
  auto ElementSize = TyElement->getPrimitiveSizeInBits() / 8;

  for (unsigned i = 0; i < 4; i++) {
    if (!(*Mask & (1u << i)))
      continue;
    auto Component = Get(Call.getArgOperand(4 + i));
    if (!Component)
      return Component.takeError();
    auto *Ptr = BufferElementPointer(*Buffer, ir.CreateAdd(ByteOffset, ir.getInt32(i * ElementSize)), TyElement);
    if (Coherent)
      air.CreateDeviceCoherentStore(*Component, Ptr);
    else
      ir.CreateStore(*Component, Ptr);
  }
  return (llvm::Value *)nullptr;
}

llvm::Expected<llvm::Value *>
Converter::EmitRawBufferLoad(CallInst &Call) {
  auto Handle = ResolveHandleOperand(Call.getArgOperand(1));
  if (!Handle)
    return Handle.takeError();

  auto Index = Get(Call.getArgOperand(2));
  if (!Index)
    return Index.takeError();

  auto Buffer = GetBufferDescriptor(*Handle);
  if (!Buffer)
    return unsupported("dxil: no buffer descriptor for resource");
  auto Mask = asU32(Call.getArgOperand(4));
  if (!Mask)
    return unsupported("dxil: non-constant rawBufferLoad mask");

  Value *ByteOffset = *Index;
  if (Buffer->StructureStride != 0) {
    auto ElementOffset = Get(Call.getArgOperand(3));
    if (!ElementOffset)
      return ElementOffset.takeError();
    ByteOffset = ir.CreateMul(ByteOffset, ir.getInt32(Buffer->StructureStride));
    ByteOffset = ir.CreateAdd(ByteOffset, *ElementOffset);
  }
  bool Coherent = Buffer->GlobalCoherent;
  auto *TyStruct = cast<StructType>(MapType(Call.getType()));
  Type *TyElement = TyStruct->getElementType(0);
  auto ElementSize = TyElement->getPrimitiveSizeInBits() / 8;

  Value *Vector = Constant::getNullValue(FixedVectorType::get(TyElement, 4));
  for (unsigned i = 0; i < 4; i++) {
    if (!(*Mask & (1u << i)))
      continue;
    auto *Ptr = BufferElementPointer(*Buffer, ir.CreateAdd(ByteOffset, ir.getInt32(i * ElementSize)), TyElement);
    Value *Component =
        Coherent ? (Value *)air.CreateDeviceCoherentLoad(TyElement, Ptr) : (Value *)ir.CreateLoad(TyElement, Ptr);
    Vector = ir.CreateInsertElement(Vector, Component, (uint64_t)i);
  }
  return BuildResRet(Call, Vector, nullptr);
}

llvm::Expected<llvm::Value *>
Converter::EmitLegacyF16ToF32(CallInst &Call) {
  auto HalfInI32 = Get(Call.getArgOperand(1));
  if (!HalfInI32)
    return HalfInI32.takeError();

  return air.CreateConvertToFloat(ir.CreateBitCast(ir.CreateTrunc(*HalfInI32, ir.getInt16Ty()), ir.getHalfTy()));
}

llvm::Expected<llvm::Value *>
Converter::EmitLegacyF32ToF16(CallInst &Call) {
  auto Float = Get(Call.getArgOperand(1));
  if (!Float)
    return Float.takeError();

  return ir.CreateZExt(ir.CreateBitCast(air.CreateConvertToHalf(*Float), ir.getInt16Ty()), ir.getInt32Ty());
}

llvm::Expected<llvm::Value *>
Converter::EmitUnaryBits(const CallInst &Call, uint32_t OpId) {
  auto Bits = Get(Call.getArgOperand(1));
  if (!Bits)
    return Bits.takeError();

  auto NormalizeOverflow = [this](llvm::Value *Value) {
    llvm::Value *Max = llvm::ConstantInt::getAllOnesValue(Value->getType());
    auto BitWidth = Max->getType()->getIntegerBitWidth();
    return ir.CreateSExtOrTrunc(
        ir.CreateSelect(ir.CreateIsNull(ir.CreateAnd(Value, ~(BitWidth - 1))), Value, Max), ir.getInt32Ty()
    );
  };

  switch (OpId) {
  case kOpCountbits:
    return ir.CreateSExtOrTrunc(air.CreateIntUnOp(AIRBuilder::popcount, *Bits), ir.getInt32Ty());
  case kOpFirstbitLo:
    return NormalizeOverflow(air.CreateCountZero(*Bits, true));
  case kOpFirstbitHi:
    return NormalizeOverflow(air.CreateCountZero(*Bits, false));
  case kOpFirstbitSHi:
    return NormalizeOverflow(ir.CreateSelect(
        ir.CreateIsNotNeg(*Bits), air.CreateCountZero(*Bits, false),
        ir.CreateAdd(
            air.CreateCountZero(ir.CreateShl(ir.CreateNot(*Bits), 1ull), false),
            llvm::ConstantInt::get((*Bits)->getType(), 1)
        )
    ));
  default:
    return unsupported("dxil: unsupported unaryBits op " + std::to_string(OpId));
  }
}

llvm::Expected<llvm::Value *>
Converter::EmitBarrier(CallInst &Call) {
  auto Mode = asU32(Call.getArgOperand(1));
  if (!Mode)
    return unsupported("dxil: non-constant barrier mode");
  uint32_t Flags = 0;
  if (*Mode & (2 | 4))
    Flags |= (uint32_t)MemFlags::Device | (uint32_t)MemFlags::Texture;
  if (*Mode & 8)
    Flags |= (uint32_t)MemFlags::Threadgroup;
  // UAVFenceGlobal = 2
  // UAVFenceThreadGroup = 4
  // TGSMFence = 8
  if ((*Mode & (2 | 4 | 8)) && SupportsNonExecutionBarrier())
    air.CreateAtomicFence((MemFlags)Flags, (*Mode & 2) ? ThreadScope::Device : ThreadScope::Threadgroup);
  // SyncThreadGroup = 1
  if ((*Mode & 1) || !(SupportsNonExecutionBarrier()))
    air.CreateBarrier((MemFlags)Flags);
  return (llvm::Value *)nullptr;
}

llvm::Expected<llvm::Value *>
Converter::EmitLoadInput(const CallInst &Call) {
  auto Element = asU32(Call.getArgOperand(1));
  auto Component = asU32(Call.getArgOperand(3));
  if (!Element || !Component)
    return unsupported("dxil: non-constant loadInput index");
  auto Row = Get(Call.getArgOperand(2));
  if (!Row)
    return Row.takeError();
  if (*Element >= entry.inputs.size())
    return unsupported("dxil: loadInput element index out of range");
  const auto &signature = entry.inputs[*Element];
  if (signature.start_row < 0 || signature.start_col < 0)
    return unsupported("dxil: input signature element is not mapped");
  if (!ctx.resource.input.ptr_int4 || !ctx.resource.input.ptr_float4)
    return unsupported("dxil: input register file is not available");

  auto GSVertex = Get(Call.getArgOperand(4));
  if (!GSVertex)
    return GSVertex.takeError();

  switch (signature.semantic_kind) {
  case SemanticKind::Position:
  case SemanticKind::VertexID:
  case SemanticKind::InstanceID:
  case SemanticKind::Arbitrary:
  case SemanticKind::IsFrontFace:
  case SemanticKind::RenderTargetArrayIndex:
  case SemanticKind::ViewPortArrayIndex:
  case SemanticKind::ClipDistance:
  case SemanticKind::CullDistance:
  case SemanticKind::PrimitiveID:
    break;
  default:
    return unsupported("dxil: unsupported loadInput semantic " + std::to_string((uint32_t)signature.semantic_kind));
  }

  auto *Type = Call.getType();
  uint32_t Lane = (uint32_t)signature.start_col + *Component;
  if (Lane > 3)
    return unsupported("dxil: loadInput component out of range");

  SmallVector<Value *> RegIdxs;
  RegIdxs.push_back(ir.getInt32(0));
  if (!isa<UndefValue>(*GSVertex))
    RegIdxs.push_back(*GSVertex);
  RegIdxs.push_back(ir.CreateAdd(*Row, ir.getInt32(signature.start_row)));
  RegIdxs.push_back(ir.getInt32(Lane));

  if (Type->isIntegerTy()) {
    auto *Ptr = ir.CreateGEP(
        ctx.resource.input.ptr_int4->getType()->getNonOpaquePointerElementType(), ctx.resource.input.ptr_int4, RegIdxs
    );
    auto *Value = ir.CreateLoad(ir.getInt32Ty(), Ptr);
    if (Type->getIntegerBitWidth() < 32)
      return ir.CreateTrunc(Value, Type);
    if (Type->getIntegerBitWidth() == 32)
      return Value;
    return unsupported("dxil: unsupported loadInput integer width");
  }
  if (Type->isFloatTy()) {
    auto *Ptr = ir.CreateGEP(
        ctx.resource.input.ptr_float4->getType()->getNonOpaquePointerElementType(), ctx.resource.input.ptr_float4,
        RegIdxs
    );
    return ir.CreateLoad(ir.getFloatTy(), Ptr);
  }
  if (Type->isHalfTy()) {
    if (signature.semantic_kind == SemanticKind::Position) {
      auto *Ptr = ir.CreateGEP(
          ctx.resource.input.ptr_float4->getType()->getNonOpaquePointerElementType(), ctx.resource.input.ptr_float4,
          RegIdxs
      );
      return air.CreateConvertToHalf(ir.CreateLoad(ir.getFloatTy(), Ptr));
    }
    auto *Ptr = ir.CreateGEP(
        ctx.resource.input.ptr_int4->getType()->getNonOpaquePointerElementType(), ctx.resource.input.ptr_int4, RegIdxs
    );
    auto *Value = ir.CreateLoad(ir.getInt32Ty(), Ptr);
    return ir.CreateBitCast(ir.CreateTrunc(Value, ir.getInt16Ty()), ir.getHalfTy());
  }
  return unsupported("dxil: unsupported loadInput type");
}

llvm::Expected<llvm::Value *>
Converter::EmitStoreOutput(const CallInst &Call) {
  auto Element = asU32(Call.getArgOperand(1));
  auto Row = asU32(Call.getArgOperand(2));
  auto Component = asU32(Call.getArgOperand(3));
  if (!Element || !Row || !Component)
    return unsupported("dxil: non-constant storeOutput index");
  if (*Element >= entry.outputs.size())
    return unsupported("dxil: storeOutput element index out of range");
  auto Value = Get(Call.getArgOperand(4));
  if (!Value)
    return Value.takeError();
  if (!*Value)
    return unsupported("dxil: missing storeOutput value");
  const auto &signature = entry.outputs[*Element];
  auto *Stored = *Value;

  switch (signature.semantic_kind) {
  case SemanticKind::Position:
  case SemanticKind::Target:
  case SemanticKind::Arbitrary:
  case SemanticKind::RenderTargetArrayIndex:
  case SemanticKind::ViewPortArrayIndex:
  case SemanticKind::ClipDistance:
  case SemanticKind::CullDistance:
  case SemanticKind::PrimitiveID:
    break;
  case SemanticKind::Depth:
  case SemanticKind::DepthLessEqual:
  case SemanticKind::DepthGreaterEqual: {
    if (!ctx.resource.depth_output_reg)
      return unsupported("dxil: depth output is not available");
    if (!Stored->getType()->isFloatTy())
      return unsupported("dxil: unsupported depth output type");
    ir.CreateStore(Stored, ir.CreateConstInBoundsGEP1_32(ir.getFloatTy(), ctx.resource.depth_output_reg, 0));
    return (llvm::Value *)nullptr;
  }
  case SemanticKind::Coverage: {
    if (!ctx.resource.coverage_mask_reg)
      return unsupported("dxil: coverage output is not available");
    if (!Stored->getType()->isIntegerTy(32))
      return unsupported("dxil: unsupported coverage output type");
    ir.CreateStore(Stored, ir.CreateConstInBoundsGEP1_32(ir.getInt32Ty(), ctx.resource.coverage_mask_reg, 0));
    return (llvm::Value *)nullptr;
  }
  case SemanticKind::StencilRef: {
    if (!ctx.resource.stencil_ref_reg)
      return unsupported("dxil: stencil_ref output is not available");
    if (!Stored->getType()->isIntegerTy(32))
      return unsupported("dxil: unsupported stencil output type");
    ir.CreateStore(Stored, ir.CreateConstInBoundsGEP1_32(ir.getInt32Ty(), ctx.resource.stencil_ref_reg, 0));
    return (llvm::Value *)nullptr;
  }
  default:
    return unsupported("dxil: unsupported storeOutput semantic " + std::to_string((uint32_t)signature.semantic_kind));
  }

  if (signature.start_row < 0 || signature.start_col < 0)
    return unsupported("dxil: output signature element is not mapped");
  if (!ctx.resource.output.ptr_int4 || !ctx.resource.output.ptr_float4)
    return unsupported("dxil: output register file is not available");
  uint32_t Reg = (uint32_t)signature.start_row + *Row;
  uint32_t Lane = (uint32_t)signature.start_col + *Component;
  if (Lane > 3)
    return unsupported("dxil: storeOutput component out of range");

  SmallVector<llvm::Value *> RegIdxs;
  RegIdxs.push_back(ir.getInt32(0));
  if (ctx.shader_type == microsoft::D3D11_SB_HULL_SHADER)
    RegIdxs.push_back(ctx.resource.thread_id_in_patch);
  RegIdxs.push_back(ir.getInt32(Reg));
  RegIdxs.push_back(ir.getInt32(Lane));

  if (Stored->getType()->isFloatTy()) {
    auto *Ptr = ir.CreateGEP(
        ctx.resource.output.ptr_float4->getType()->getNonOpaquePointerElementType(), ctx.resource.output.ptr_float4,
        RegIdxs
    );
    ir.CreateStore(Stored, Ptr);
    return (llvm::Value *)nullptr;
  }
  if (Stored->getType()->isHalfTy()) {
    if (signature.semantic_kind == SemanticKind::Position) {
      auto *Ptr = ir.CreateGEP(
          ctx.resource.output.ptr_float4->getType()->getNonOpaquePointerElementType(), ctx.resource.output.ptr_float4,
          RegIdxs
      );
      ir.CreateStore(air.CreateConvertToFloat(Stored), Ptr);
      return (llvm::Value *)nullptr;
    }
    Stored = ir.CreateZExt(ir.CreateBitCast(Stored, ir.getInt16Ty()), ir.getInt32Ty());
    auto *Ptr = ir.CreateGEP(
        ctx.resource.output.ptr_int4->getType()->getNonOpaquePointerElementType(), ctx.resource.output.ptr_int4, RegIdxs
    );
    ir.CreateStore(Stored, Ptr);
    return (llvm::Value *)nullptr;
  }
  if (Stored->getType()->isIntegerTy()) {
    if (Stored->getType()->getIntegerBitWidth() < 32)
      Stored = ir.CreateZExt(Stored, ir.getInt32Ty());
    else if (Stored->getType()->getIntegerBitWidth() > 32)
      return unsupported("dxil: unsupported storeOutput integer width");
    auto *Ptr = ir.CreateGEP(
        ctx.resource.output.ptr_int4->getType()->getNonOpaquePointerElementType(), ctx.resource.output.ptr_int4, RegIdxs
    );
    ir.CreateStore(Stored, Ptr);
    return (llvm::Value *)nullptr;
  }
  return unsupported("dxil: unsupported storeOutput type");
}

llvm::Expected<llvm::Value *>
Converter::EmitSample(const CallInst &Call, uint32_t OpId) {
  auto TextureHandle = ResolveHandleOperand(Call.getArgOperand(1));
  if (!TextureHandle)
    return TextureHandle.takeError();
  auto TextureDescriptor = GetTextureDescriptor(*TextureHandle);
  if (!TextureDescriptor)
    return unsupported("dxil: no texture descriptor for resource");
  auto SamplerHandle = ResolveHandleOperand(Call.getArgOperand(2));
  if (!SamplerHandle)
    return SamplerHandle.takeError();
  auto SamplerDescriptor = GetSamplerDescriptor(*SamplerHandle);
  if (!SamplerDescriptor)
    return SamplerDescriptor.takeError();
  auto Texture = MakeTexture(*TextureDescriptor);
  auto Sampler = SamplerDescriptor->SamplerHandle;

  auto C0 = Get(Call.getArgOperand(3));
  if (!C0)
    return C0.takeError();
  auto C1 = Get(Call.getArgOperand(4));
  if (!C1)
    return C1.takeError();
  auto C2 = Get(Call.getArgOperand(5));
  if (!C2)
    return C2.takeError();
  auto C3 = Get(Call.getArgOperand(6));
  if (!C3)
    return C3.takeError();

  const int32_t Offset[3] = {
      tryGetI32(Call.getArgOperand(7)), tryGetI32(Call.getArgOperand(8)), tryGetI32(Call.getArgOperand(9))
  };

  Value *Coord = nullptr;
  Value *Array = nullptr;

  auto ConvertArrayIndexFromFloat = [this](Value *Value) {
    return air.CreateConvertToSigned(air.CreateFPUnOp(AIRBuilder::rint, Value));
  };

  switch (TextureDescriptor->ResourceKindLogical) {
  case Texture::texture1d:
    Coord = MakeVec({*C0, air.getFloat(0)});
    break;
  case Texture::texture1d_array:
    Coord = MakeVec({*C0, air.getFloat(0)});
    Array = ConvertArrayIndexFromFloat(*C1);
    break;
  case Texture::texture2d:
  case Texture::depth2d:
    Coord = MakeVec({*C0, *C1});
    break;
  case Texture::texture2d_array:
  case Texture::depth2d_array:
    Coord = MakeVec({*C0, *C1});
    Array = ConvertArrayIndexFromFloat(*C2);
    break;
  case Texture::texture3d:
    Coord = MakeVec({*C0, *C1, *C2});
    break;
  case Texture::texturecube:
  case Texture::depthcube:
    Coord = MakeVec({*C0, *C1, *C2});
    break;
  case Texture::texturecube_array:
  case Texture::depthcube_array:
    Coord = MakeVec({*C0, *C1, *C2});
    Array = ConvertArrayIndexFromFloat(*C3);
    break;
  default:
    return unsupported(
        "dxil: unsupported texture kind for sampling: " +
        std::to_string(static_cast<uint32_t>(TextureDescriptor->ResourceKindLogical))
    );
  }

  llvm::Value *Value = nullptr, *Residency = nullptr;
  if (OpId == kOpSampleLevel) {
    auto Level = Get(Call.getArgOperand(10));
    if (!Level)
      return Level.takeError();
    std::tie(Value, Residency) = air.CreateSample(
        Texture, TextureDescriptor->ResourceHandle, Sampler, Coord, Array, Offset, sample_level{*Level}
    );
  } else if (OpId == kOpSampleBias) {
    auto Bias = Get(Call.getArgOperand(10));
    if (!Bias)
      return Bias.takeError();
    std::tie(Value, Residency) =
        air.CreateSample(Texture, TextureDescriptor->ResourceHandle, Sampler, Coord, Array, Offset, sample_bias{*Bias});
  } else if (OpId == kOpSampleCmpLevelZero) {
    auto Reference = Get(Call.getArgOperand(10));
    if (!Reference)
      return Reference.takeError();
    std::tie(Value, Residency) = air.CreateSampleCmp(
        Texture, TextureDescriptor->ResourceHandle, Sampler, Coord, Array, *Reference, Offset,
        sample_level{air.getFloat(0)}
    );
  } else if (OpId == kOpSampleCmp) {
    auto Reference = Get(Call.getArgOperand(10));
    if (!Reference)
      return Reference.takeError();
    auto Clamp = Get(Call.getArgOperand(11));
    if (!Clamp)
      return Clamp.takeError();
    std::tie(Value, Residency) = air.CreateSampleCmp(
        Texture, TextureDescriptor->ResourceHandle, Sampler, Coord, Array, *Reference, Offset,
        sample_bias{air.getFloat(0)}, sample_min_lod_clamp{*Clamp}
    );
  } else if (OpId == kOpSampleGrad) {
    auto DDX0 = Get(Call.getArgOperand(10));
    if (!DDX0)
      return DDX0.takeError();
    auto DDX1 = Get(Call.getArgOperand(11));
    if (!DDX1)
      return DDX1.takeError();
    auto DDX2 = Get(Call.getArgOperand(12));
    if (!DDX2)
      return DDX2.takeError();
    auto DDY0 = Get(Call.getArgOperand(13));
    if (!DDY0)
      return DDY0.takeError();
    auto DDY1 = Get(Call.getArgOperand(14));
    if (!DDY1)
      return DDY1.takeError();
    auto DDY2 = Get(Call.getArgOperand(15));
    if (!DDY2)
      return DDY2.takeError();
    switch (TextureDescriptor->ResourceKindLogical) {
    case Texture::texture1d:
    case Texture::texture1d_array:
      std::tie(Value, Residency) = air.CreateSampleGrad(
          Texture, TextureDescriptor->ResourceHandle, Sampler, Coord, Array, MakeVec({*DDX0, *DDX0}),
          MakeVec({*DDY0, *DDY0}), Offset
      );
      break;
    case Texture::texture2d:
    case Texture::texture2d_array:
    case Texture::depth2d:
    case Texture::depth2d_array:
      std::tie(Value, Residency) = air.CreateSampleGrad(
          Texture, TextureDescriptor->ResourceHandle, Sampler, Coord, Array, MakeVec({*DDX0, *DDX1}),
          MakeVec({*DDY0, *DDY1}), Offset
      );
      break;
    case Texture::texture3d:
    case Texture::texturecube:
    case Texture::texturecube_array:
    case Texture::depthcube:
    case Texture::depthcube_array:
      std::tie(Value, Residency) = air.CreateSampleGrad(
          Texture, TextureDescriptor->ResourceHandle, Sampler, Coord, Array, MakeVec({*DDX0, *DDX1, *DDX2}),
          MakeVec({*DDY0, *DDY1, *DDY2}), Offset
      );
      break;
    default:
      break;
    }
  } else {
    std::tie(Value, Residency) =
        air.CreateSample(Texture, TextureDescriptor->ResourceHandle, Sampler, Coord, Array, Offset);
  }
  if (!Value)
    return unsupported("dxil: sampling operation failed");
  // Metal depth texture must use float type, resolve potential type mismatch here
  if (Call.getType()->getStructElementType(0)->isHalfTy())
    Value = air.CreateConvertToHalf(Value);
  return BuildResRet(Call, Value, Residency);
}

llvm::Expected<llvm::Value *>
Converter::EmitGather(const CallInst &Call, uint32_t OpId) {
  auto TextureHandle = ResolveHandleOperand(Call.getArgOperand(1));
  if (!TextureHandle)
    return TextureHandle.takeError();
  auto TextureDescriptor = GetTextureDescriptor(*TextureHandle);
  if (!TextureDescriptor)
    return unsupported("dxil: no texture descriptor for resource");
  auto SamplerHandle = ResolveHandleOperand(Call.getArgOperand(2));
  if (!SamplerHandle)
    return SamplerHandle.takeError();
  auto SamplerDescriptor = GetSamplerDescriptor(*SamplerHandle);
  if (!SamplerDescriptor)
    return SamplerDescriptor.takeError();
  auto Texture = MakeTexture(*TextureDescriptor);
  auto Sampler = SamplerDescriptor->SamplerHandle;

  auto C0 = Get(Call.getArgOperand(3));
  if (!C0)
    return C0.takeError();
  auto C1 = Get(Call.getArgOperand(4));
  if (!C1)
    return C1.takeError();
  auto C2 = Get(Call.getArgOperand(5));
  if (!C2)
    return C2.takeError();
  auto C3 = Get(Call.getArgOperand(6));
  if (!C3)
    return C3.takeError();
  auto O0 = Get(Call.getArgOperand(7));
  if (!O0)
    return O0.takeError();
  auto O1 = Get(Call.getArgOperand(8));
  if (!O1)
    return O1.takeError();
  auto Component = Get(Call.getArgOperand(9));
  if (!Component)
    return Component.takeError();
  // A gather without an offset has undef ones in DXIL. Metal's compiler takes zeros there, as its own front end
  // writes them, and crashed on undef (AGCLLVMUserObject::replaceSamplerFunctionUses: Godot's compute shaders).
  if (isa<UndefValue>(*O0))
    *O0 = ir.getInt32(0);
  if (isa<UndefValue>(*O1))
    *O1 = ir.getInt32(0);

  Value *Offset = nullptr;
  Value *Coord = nullptr;
  Value *Array = nullptr;

  auto ConvertArrayIndexFromFloat = [this](Value *Value) {
    return air.CreateConvertToSigned(air.CreateFPUnOp(AIRBuilder::rint, Value));
  };

  switch (TextureDescriptor->ResourceKindLogical) {
  case Texture::texture2d:
  case Texture::depth2d:
    Coord = MakeVec({*C0, *C1});
    Offset = MakeVec({*O0, *O1});
    break;
  case Texture::texture2d_array:
  case Texture::depth2d_array:
    Coord = MakeVec({*C0, *C1});
    Array = ConvertArrayIndexFromFloat(*C2);
    Offset = MakeVec({*O0, *O1});
    break;
  case Texture::texturecube:
  case Texture::depthcube:
    Coord = MakeVec({*C0, *C1, *C2});
    Offset = air.getInt2(0, 0);
    break;
  case Texture::texturecube_array:
  case Texture::depthcube_array:
    Coord = MakeVec({*C0, *C1, *C2});
    Array = ConvertArrayIndexFromFloat(*C3);
    Offset = air.getInt2(0, 0);
    break;
  default:
    return unsupported(
        "dxil: unsupported texture kind for gathering: " +
        std::to_string(static_cast<uint32_t>(TextureDescriptor->ResourceKindLogical))
    );
  }

  llvm::Value *Value = nullptr, *Residency = nullptr;
  if (OpId == kOpTextureGatherCmp) {
    auto Reference = Get(Call.getArgOperand(10));
    if (!Reference)
      return Reference.takeError();
    std::tie(Value, Residency) =
        air.CreateGatherCompare(Texture, TextureDescriptor->ResourceHandle, Sampler, Coord, Array, *Reference, Offset);
  } else {
    std::tie(Value, Residency) =
        air.CreateGather(Texture, TextureDescriptor->ResourceHandle, Sampler, Coord, Array, Offset, *Component);
  }
  if (!Value)
    return unsupported("dxil: gather operation failed");
  // Metal depth texture must use float type, resolve potential type mismatch here
  if (Call.getType()->getStructElementType(0)->isHalfTy())
    Value = air.CreateConvertToHalf(Value);
  return BuildResRet(Call, Value, Residency);
}

llvm::Expected<llvm::Value *>
Converter::EmitUnary(const CallInst &Call, uint32_t OpId) {
  if (Call.arg_size() != 2)
    return unsupported("dxil: unexpected unary signature");
  auto Operand = Get(Call.getArgOperand(1));
  if (!Operand)
    return Operand.takeError();

  AIRBuilder::FPUnOp FPUnOp;
  switch (OpId) {
  case kOpBfrev:
    if (!(*Operand)->getType()->isIntegerTy())
      return unsupported("dxil: bfrev on a non-integer operand");
    return air.CreateIntUnOp(AIRBuilder::reverse_bits, *Operand);
  case kOpDerivCoarseX:
  case kOpDerivCoarseY:
  case kOpDerivFineX:
  case kOpDerivFineY:
    if (!(*Operand)->getType()->isFloatingPointTy())
      return unsupported("dxil: derivative on a non-float operand");
    return air.CreateDerivative(*Operand, OpId == kOpDerivCoarseY || OpId == kOpDerivFineY);
  case kOpFAbs:
    FPUnOp = AIRBuilder::fabs;
    break;
  case kOpSaturate:
    FPUnOp = AIRBuilder::saturate;
    break;
  case kOpCos:
    FPUnOp = AIRBuilder::cos;
    break;
  case kOpSin:
    FPUnOp = AIRBuilder::sin;
    break;
  case kOpTan:
    FPUnOp = AIRBuilder::sin;
    break;
  case kOpAcos:
    FPUnOp = AIRBuilder::acos;
    break;
  case kOpAsin:
    FPUnOp = AIRBuilder::asin;
    break;
  case kOpAtan:
    FPUnOp = AIRBuilder::atan;
    break;
  case kOpHcos:
    FPUnOp = AIRBuilder::hcos;
    break;
  case kOpHsin:
    FPUnOp = AIRBuilder::hsin;
    break;
  case kOpHtan:
    FPUnOp = AIRBuilder::htan;
    break;
  case kOpExp:
    FPUnOp = AIRBuilder::exp2;
    break;
  case kOpFrc:
    FPUnOp = AIRBuilder::fract;
    break;
  case kOpLog:
    FPUnOp = AIRBuilder::log2;
    break;
  case kOpSqrt:
    FPUnOp = AIRBuilder::sqrt;
    break;
  case kOpRsqrt:
    FPUnOp = AIRBuilder::rsqrt;
    break;
  case kOpRound_ne:
    FPUnOp = AIRBuilder::rint;
    break;
  case kOpRound_ni:
    FPUnOp = AIRBuilder::floor;
    break;
  case kOpRound_pi:
    FPUnOp = AIRBuilder::ceil;
    break;
  case kOpRound_z:
    FPUnOp = AIRBuilder::trunc;
    break;
  default:
    return unsupported("dxil: unsupported unary operation: " + std::to_string(OpId));
  }
  if (!(*Operand)->getType()->isFloatingPointTy())
    return unsupported("dxil: float unary operation on a non-float operand");
  return air.CreateFPUnOp(FPUnOp, *Operand);
}

llvm::Expected<llvm::Value *>
Converter::EmitBinary(const CallInst &Call, uint32_t OpId) {
  if (Call.arg_size() != 3)
    return unsupported("dxil: unexpected binary signature");
  auto LHS = Get(Call.getArgOperand(1));
  if (!LHS)
    return LHS.takeError();
  auto RHS = Get(Call.getArgOperand(2));
  if (!RHS)
    return RHS.takeError();

  switch (OpId) {
  case kOpFMax:
  case kOpFMin:
    if (!(*LHS)->getType()->isFloatingPointTy())
      return unsupported("dxil: fp binary operation on a non-float operand");
    return air.CreateFPBinOp(OpId == kOpFMax ? AIRBuilder::fmax : AIRBuilder::fmin, *LHS, *RHS);
  case kOpIMax:
  case kOpIMin:
  case kOpUMax:
  case kOpUMin: {
    if (!(*LHS)->getType()->isIntegerTy())
      return unsupported("dxil: integer binary operation on a non-integer operand");
    bool IsMax = OpId == kOpIMax || OpId == kOpUMax;
    bool IsSigned = OpId == kOpIMax || OpId == kOpIMin;
    return air.CreateIntBinOp(IsMax ? AIRBuilder::max : AIRBuilder::min, *LHS, *RHS, IsSigned);
  }
  default:
    return unsupported("dxil: unsupported binary operation: " + std::to_string(OpId));
  }
}

llvm::Expected<llvm::Value *>
Converter::EmitTertiary(const CallInst &Call, uint32_t OpId) {
  if (Call.arg_size() != 4)
    return unsupported("dxil: unexpected tertiary signature");
  auto A = Get(Call.getArgOperand(1));
  if (!A)
    return A.takeError();
  auto B = Get(Call.getArgOperand(2));
  if (!B)
    return B.takeError();
  auto C = Get(Call.getArgOperand(3));
  if (!C)
    return C.takeError();

  switch (OpId) {
  case kOpFMad:
    return ir.CreateFAdd(ir.CreateFMul(*A, *B), *C);
  case kOpFma:
    return air.CreateFMA(*A, *B, *C);
  case kOpIMad:
  case kOpUMad:
    return ir.CreateAdd(ir.CreateMul(*A, *B), *C);
  case kOpIbfe:
  case kOpUbfe: {
    auto Width = ir.CreateAnd(*A, 0x1F);
    auto Offset = ir.CreateAnd(*B, 0X1F);
    auto WidthAddOffset = ir.CreateAdd(Width, Offset);
    auto Constant_32 = llvm::ConstantInt::get(Width->getType(), 32);
    auto ClampedC = ir.CreateShl(*C, ir.CreateSub(Constant_32, WidthAddOffset));

    llvm::Value *NeedClamp, *NoClamp, *Clamped;

    if (OpId == kOpIbfe) {
      NeedClamp = ir.CreateICmpSLT(WidthAddOffset, Constant_32);
      NoClamp = ir.CreateAShr(*C, Offset);
      Clamped = ir.CreateAShr(ClampedC, ir.CreateSub(Constant_32, Width));
    } else {
      NeedClamp = ir.CreateICmpULT(WidthAddOffset, Constant_32);
      NoClamp = ir.CreateLShr(*C, Offset);
      Clamped = ir.CreateLShr(ClampedC, ir.CreateSub(Constant_32, Width));
    }
    return ir.CreateSelect(
        ir.CreateIsNull(Width), llvm::ConstantInt::get(Width->getType(), 0),
        ir.CreateSelect(NeedClamp, Clamped, NoClamp)
    );
  }
  default:
    return unsupported("dxil: unsupported tertiary operation: " + std::to_string(OpId));
  }
}

llvm::Expected<llvm::Value *>
Converter::EmitDotProduct(const CallInst &Call, unsigned Dimension) {
  if (Call.arg_size() != 1 + 2 * Dimension)
    return unsupported("dxil: unexpected dot signature");
  SmallVector<Value *, 4> LHSs;
  SmallVector<Value *, 4> RHSs;
  for (unsigned i = 0; i < Dimension; ++i) {
    auto LHS = Get(Call.getArgOperand(1 + i));
    if (!LHS)
      return LHS.takeError();
    auto RHS = Get(Call.getArgOperand(1 + Dimension + i));
    if (!RHS)
      return RHS.takeError();
    LHSs.push_back(*LHS);
    RHSs.push_back(*RHS);
  }
  return air.CreateDotProduct(MakeVec(LHSs), MakeVec(RHSs));
}

/* dot2add: the accumulator plus the dot product of two half2, in float. */
llvm::Expected<llvm::Value *>
Converter::EmitDot2AddHalf(const CallInst &Call) {
  if (Call.arg_size() != 6)
    return unsupported("dxil: unexpected dot2AddHalf signature");
  SmallVector<Value *, 5> Operands;
  for (unsigned i = 1; i < 6; ++i) {
    auto Operand = Get(Call.getArgOperand(i));
    if (!Operand)
      return Operand.takeError();
    Operands.push_back(i == 1 ? *Operand : ir.CreateFPExt(*Operand, ir.getFloatTy()));
  }
  auto *Result = ir.CreateFAdd(Operands[0], ir.CreateFMul(Operands[1], Operands[3]));
  return ir.CreateFAdd(Result, ir.CreateFMul(Operands[2], Operands[4]));
}

/* dot4add_i8packed and dot4add_u8packed: the accumulator plus the dot product of the four bytes of each operand, signed
   or unsigned. */
llvm::Expected<llvm::Value *>
Converter::EmitDot4AddPacked(const CallInst &Call, uint32_t OpId) {
  if (Call.arg_size() != 4)
    return unsupported("dxil: unexpected dot4AddPacked signature");
  auto Accumulator = Get(Call.getArgOperand(1));
  if (!Accumulator)
    return Accumulator.takeError();
  auto A = Get(Call.getArgOperand(2));
  if (!A)
    return A.takeError();
  auto B = Get(Call.getArgOperand(3));
  if (!B)
    return B.takeError();
  bool IsSigned = OpId == kOpDot4AddI8Packed;
  auto Byte = [&](Value *Packed, unsigned i) {
    return IsSigned ? ir.CreateAShr(ir.CreateShl(Packed, 24 - 8 * i), 24)
                    : ir.CreateAnd(ir.CreateLShr(Packed, 8 * i), 0xff);
  };
  Value *Result = *Accumulator;
  for (unsigned i = 0; i < 4; ++i)
    Result = ir.CreateAdd(Result, ir.CreateMul(Byte(*A, i), Byte(*B, i)));
  return Result;
}

/* pack_u8, pack_s8 and their clamping forms: the low byte of each of four values, after clamping to an unsigned (mode
   1) or signed (mode 2) byte's range. */
llvm::Expected<llvm::Value *>
Converter::EmitPack4x8(const CallInst &Call) {
  auto Mode = asU32(Call.getArgOperand(1));
  if (!Mode || *Mode > 2 || Call.arg_size() != 6)
    return unsupported("dxil: unexpected pack4x8 signature");
  Value *Result = ir.getInt32(0);
  for (unsigned i = 0; i < 4; ++i) {
    auto Component = Get(Call.getArgOperand(2 + i));
    if (!Component)
      return Component.takeError();
    Value *Value = ir.CreateSExtOrTrunc(*Component, ir.getInt32Ty());
    if (*Mode) {
      int32_t Min = *Mode == 1 ? 0 : -128, Max = *Mode == 1 ? 255 : 127;
      Value = ir.CreateSelect(ir.CreateICmpSLT(Value, ir.getInt32(Min)), ir.getInt32(Min), Value);
      Value = ir.CreateSelect(ir.CreateICmpSGT(Value, ir.getInt32(Max)), ir.getInt32(Max), Value);
    }
    Result = ir.CreateOr(Result, ir.CreateShl(ir.CreateAnd(Value, 0xff), 8 * i));
  }
  return Result;
}

/* unpack_u8u32, unpack_s8s32 and their 16-bit forms: the four bytes, zero-extended (mode 0) or sign-extended (mode 1). */
llvm::Expected<llvm::Value *>
Converter::EmitUnpack4x8(const CallInst &Call) {
  auto Mode = asU32(Call.getArgOperand(1));
  if (!Mode || *Mode > 1 || Call.arg_size() != 3)
    return unsupported("dxil: unexpected unpack4x8 signature");
  auto Packed = Get(Call.getArgOperand(2));
  if (!Packed)
    return Packed.takeError();
  auto *TyResult = llvm::cast<StructType>(MapType(Call.getType()));
  Value *Result = PoisonValue::get(TyResult);
  for (unsigned i = 0; i < 4; ++i) {
    auto *Byte = *Mode ? ir.CreateAShr(ir.CreateShl(*Packed, 24 - 8 * i), 24)
                       : ir.CreateAnd(ir.CreateLShr(*Packed, 8 * i), 0xff);
    Result = ir.CreateInsertValue(Result, ir.CreateTrunc(Byte, TyResult->getElementType(i)), i);
  }
  return Result;
}

llvm::Expected<llvm::Value *>
Converter::EmitIsSpecialFloat(const CallInst &Call, uint32_t OpId) {
  auto Operand = Get(Call.getArgOperand(1));
  if (!Operand)
    return Operand.takeError();

  switch (OpId) {
  case kOpIsNaN:
    return air.CreateIsNaN(*Operand);
  case kOpIsInf:
    return air.CreateIsInfinite(*Operand);
  case kOpIsFinite:
    return air.CreateIsFinite(*Operand);
  case kOpIsNormal:
    return air.CreateIsNormal(*Operand);
  default:
    return unsupported("dxil: unsupported isSpecialFloat operation: " + std::to_string(OpId));
  }
}

llvm::Expected<llvm::Value *>
Converter::EmitAtomicRMW(const CallInst &Call) {
  auto Handle = ResolveHandleOperand(Call.getArgOperand(1));
  if (!Handle)
    return Handle.takeError();
  auto Op = asU32(Call.getArgOperand(2));
  if (!Op)
    return unsupported("dxil: non-constant atomic operation");

  AtomicRMWInst::BinOp BinOp;
  switch (auto OpId = *Op) {
  case kAtomicOpAdd:
    BinOp = AtomicRMWInst::Add;
    break;
  case kAtomicOpAnd:
    BinOp = AtomicRMWInst::And;
    break;
  case kAtomicOpOr:
    BinOp = AtomicRMWInst::Or;
    break;
  case kAtomicOpXor:
    BinOp = AtomicRMWInst::Xor;
    break;
  case kAtomicOpIMin:
    BinOp = AtomicRMWInst::Min;
    break;
  case kAtomicOpIMax:
    BinOp = AtomicRMWInst::Max;
    break;
  case kAtomicOpUMin:
    BinOp = AtomicRMWInst::UMin;
    break;
  case kAtomicOpUMax:
    BinOp = AtomicRMWInst::UMax;
    break;
  case kAtomicOpExchange:
    BinOp = AtomicRMWInst::Xchg;
    break;
  default:
    return unsupported("dxil: unsupported atomic operation: " + std::to_string(OpId));
  }

  auto NewValue = Get(Call.getArgOperand(6));
  if (!NewValue)
    return NewValue.takeError();

  /* Metal's 64-bit atomics are unsigned min and max that return nothing, on buffers and on ulong textures (RG32Uint,
     as a RWTexture2D<uint64_t> over R32G32_UINT is): what Nanite's InterlockedMax into its visibility buffer needs. */
  bool Is64 = Call.getType()->isIntegerTy(64);
  bool Max = BinOp == AtomicRMWInst::UMax;
  if (Is64 && ((!Max && BinOp != AtomicRMWInst::UMin) || !Call.use_empty()))
    return unsupported("dxil: 64-bit atomic other than an unsigned min or max whose result is unused");

  auto Buffer = GetBufferDescriptor(*Handle);
  if (Buffer) {
    auto Offset0 = Get(Call.getArgOperand(3));
    if (!Offset0)
      return Offset0.takeError();
    Value *ByteOffset = *Offset0;
    if (Buffer->StructureStride != 0) {
      auto ElementOffset = Get(Call.getArgOperand(4));
      if (!ElementOffset)
        return ElementOffset.takeError();
      ByteOffset = ir.CreateMul(ByteOffset, ir.getInt32(Buffer->StructureStride));
      ByteOffset = ir.CreateAdd(ByteOffset, *ElementOffset);
    }
    if (Is64) {
      air.CreateAtomicMinMax64(
          Max, BufferElementPointer(*Buffer, ir.CreateAnd(ByteOffset, ir.getInt32(~7u)), ir.getInt64Ty()), *NewValue
      );
      return (llvm::Value *)nullptr;
    }
    auto *Ptr = BufferElementPointer(*Buffer, ir.CreateAnd(ByteOffset, ir.getInt32(~3u)), ir.getInt32Ty());
    auto Value = air.CreateAtomicRMW(BinOp, Ptr, *NewValue);
    if (!Value)
      return unsupported("dxil: atomic operation failed");
    return Value;
  }

  auto Texture = GetTextureDescriptor(*Handle);
  if (!Texture)
    return unsupported("dxil: no descriptor for atomic operation");

  auto X = Get(Call.getArgOperand(3));
  if (!X)
    return X.takeError();
  auto Y = Get(Call.getArgOperand(4));
  if (!Y)
    return Y.takeError();
  auto Z = Get(Call.getArgOperand(5));
  if (!Z)
    return Z.takeError();

  auto Address = BuildRwAddress(Texture->ResourceKindLogical, *X, *Y, *Z, ir.getInt32(0), 0, 0, 0);
  if (!Address)
    return Address.takeError();
  auto [Position, Slice, Unused] = *Address;
  if (Texture->ResourceKindLogical == Texture::texture_buffer) {
    auto *OOB = ir.CreateICmpUGE(Position, ir.CreateTrunc(ir.CreateLShr(Texture->Metadata, 32uLL), air.getIntTy()));
    Position = ir.CreateAdd(Position, ir.CreateTrunc(Texture->Metadata, air.getIntTy()));
    Position = ir.CreateSelect(OOB, llvm::ConstantInt::getAllOnesValue(Position->getType()), Position);
  }

  if (Is64) {
    auto *Value = ir.CreateInsertElement(PoisonValue::get(FixedVectorType::get(ir.getInt64Ty(), 4)), *NewValue, 0ull);
    air.CreateAtomicMinMax64(MakeTexture(*Texture), Texture->ResourceHandle, Max, Position, Value, Slice);
    return (llvm::Value *)nullptr;
  }
  auto *TyVector = FixedVectorType::get(GetTextureSampleType(Texture->SampleType), 4);
  auto *Value = ir.CreateInsertElement(PoisonValue::get(TyVector), *NewValue, 0ull);
  auto Result = air.CreateAtomicRMW(MakeTexture(*Texture), Texture->ResourceHandle, BinOp, Position, Value, Slice);
  if (!Result)
    return unsupported("dxil: atomic operation failed");
  return ir.CreateExtractElement(Result, 0ull);
}

llvm::Expected<llvm::Value *>
Converter::EmitAtomicCmpXchg(const CallInst &Call) {
  auto Handle = ResolveHandleOperand(Call.getArgOperand(1));
  if (!Handle)
    return Handle.takeError();

  auto Compare = Get(Call.getArgOperand(5));
  if (!Compare)
    return Compare.takeError();
  auto NewValue = Get(Call.getArgOperand(6));
  if (!NewValue)
    return NewValue.takeError();

  auto Buffer = GetBufferDescriptor(*Handle);
  if (Buffer) {
    auto Offset0 = Get(Call.getArgOperand(2));
    if (!Offset0)
      return Offset0.takeError();
    Value *ByteOffset = *Offset0;
    if (Buffer->StructureStride != 0) {
      auto ElementOffset = Get(Call.getArgOperand(3));
      if (!ElementOffset)
        return ElementOffset.takeError();
      ByteOffset = ir.CreateMul(ByteOffset, ir.getInt32(Buffer->StructureStride));
      ByteOffset = ir.CreateAdd(ByteOffset, *ElementOffset);
    }
    auto *Ptr = BufferElementPointer(*Buffer, ir.CreateAnd(ByteOffset, ir.getInt32(~3u)), ir.getInt32Ty());
    auto Result = ir.CreateAtomicCmpXchg(
        Ptr, *Compare, *NewValue, MaybeAlign(), AtomicOrdering::Monotonic, AtomicOrdering::Monotonic
    );
    return ir.CreateExtractValue(Result, 0);
  }

  auto Texture = GetTextureDescriptor(*Handle);
  if (!Texture)
    return unsupported("dxil: no descriptor for atomic operation");

  auto X = Get(Call.getArgOperand(2));
  if (!X)
    return X.takeError();
  auto Y = Get(Call.getArgOperand(3));
  if (!Y)
    return Y.takeError();
  auto Z = Get(Call.getArgOperand(4));
  if (!Z)
    return Z.takeError();

  auto Address = BuildRwAddress(Texture->ResourceKindLogical, *X, *Y, *Z, ir.getInt32(0), 0, 0, 0);
  if (!Address)
    return Address.takeError();
  auto [Position, Slice, Unused] = *Address;
  if (Texture->ResourceKindLogical == Texture::texture_buffer) {
    auto *OOB = ir.CreateICmpUGE(Position, ir.CreateTrunc(ir.CreateLShr(Texture->Metadata, 32uLL), air.getIntTy()));
    Position = ir.CreateAdd(Position, ir.CreateTrunc(Texture->Metadata, air.getIntTy()));
    Position = ir.CreateSelect(OOB, llvm::ConstantInt::getAllOnesValue(Position->getType()), Position);
  }
  auto *TyElement = GetTextureSampleType(Texture->SampleType);
  auto *TyVector = FixedVectorType::get(TyElement, 4);
  auto *Cmp = ir.CreateInsertElement(PoisonValue::get(TyVector), *Compare, 0ull);
  auto *Value = ir.CreateInsertElement(PoisonValue::get(TyVector), *NewValue, 0ull);
  auto [Result, Flag_DISCARDED] =
      air.CreateAtomicCmpXchg(MakeTexture(*Texture), Texture->ResourceHandle, Position, Cmp, Value, Slice);
  if (!Result)
    return unsupported("dxil: atomic operation failed");
  return ir.CreateExtractElement(Result, 0ull);
}

llvm::Expected<llvm::Value *>
Converter::EmitDiscard(const CallInst &Call) {
  auto Cond = Get(Call.getArgOperand(1));
  if (!Cond)
    return Cond.takeError();
  /* Changed for VMPal, 2026-10-24: anywhere in a block, as D3D9On12's alpha test and texkill put it. The rest of the
   * block goes on in ContinueBB, as DXBC's discard goes on, and its terminator's successors take their PHIs' values
   * from the block it ends in (TranslateTerminator). */
  auto *DiscardBB = llvm::BasicBlock::Create(air.getContext(), "discard", ctx.function);
  auto *ContinueBB = llvm::BasicBlock::Create(air.getContext(), "continue", ctx.function);
  ir.CreateCondBr(*Cond, DiscardBB, ContinueBB);
  ir.SetInsertPoint(DiscardBB);
  air.CreateDiscard();
  ir.CreateBr(ContinueBB);
  ir.SetInsertPoint(ContinueBB);
  return (llvm::Value *)nullptr;
}

llvm::Expected<llvm::Value *>
Converter::EmitCalculateLOD(const CallInst &Call) {
  auto TextureHandle = ResolveHandleOperand(Call.getArgOperand(1));
  if (!TextureHandle)
    return TextureHandle.takeError();
  auto TextureDescriptor = GetTextureDescriptor(*TextureHandle);
  if (!TextureDescriptor)
    return unsupported("dxil: no texture descriptor for resource");
  auto SamplerHandle = ResolveHandleOperand(Call.getArgOperand(2));
  if (!SamplerHandle)
    return SamplerHandle.takeError();
  auto SamplerDescriptor = GetSamplerDescriptor(*SamplerHandle);
  if (!SamplerDescriptor)
    return SamplerDescriptor.takeError();
  auto Texture = MakeTexture(*TextureDescriptor);
  auto Sampler = SamplerDescriptor->SamplerHandle;

  auto C0 = Get(Call.getArgOperand(3));
  if (!C0)
    return C0.takeError();
  auto C1 = Get(Call.getArgOperand(4));
  if (!C1)
    return C1.takeError();
  auto C2 = Get(Call.getArgOperand(5));
  if (!C2)
    return C2.takeError();

  auto Clamped = asU32(Call.getArgOperand(6));
  if (!Clamped)
    return unsupported("dxil: non-constant calculateLOD arguments");

  Value *Coord = nullptr;
  switch (TextureDescriptor->ResourceKindLogical) {
  case Texture::texture1d:
  case Texture::texture1d_array:
    Coord = MakeVec({*C0, air.getFloat(0)});
    break;
  case Texture::texture2d:
  case Texture::depth2d:
  case Texture::texture2d_array:
  case Texture::depth2d_array:
    Coord = MakeVec({*C0, *C1});
    break;
  case Texture::texture3d:
  case Texture::texturecube:
  case Texture::depthcube:
  case Texture::texturecube_array:
  case Texture::depthcube_array:
    Coord = MakeVec({*C0, *C1, *C2});
    break;
  default:
    return unsupported(
        "dxil: unsupported texture kind for calculateLOD: " +
        std::to_string(static_cast<uint32_t>(TextureDescriptor->ResourceKindLogical))
    );
  }

  return air.CreateCalculateLOD(Texture, TextureDescriptor->ResourceHandle, Sampler, Coord, *Clamped);
}

llvm::Expected<llvm::Value *>
Converter::EmitGetTextureSamplePos(const CallInst &Call) {
  auto TextureHandle = ResolveHandleOperand(Call.getArgOperand(1));
  if (!TextureHandle)
    return TextureHandle.takeError();
  auto TextureDescriptor = GetTextureDescriptor(*TextureHandle);
  if (!TextureDescriptor)
    return unsupported("dxil: no texture descriptor for resource");
  auto Texture = MakeTexture(*TextureDescriptor);

  auto Index = Get(Call.getArgOperand(2));
  if (!Index)
    return Index.takeError();
  auto *SampleCount =
      air.CreateTextureQuery(Texture, TextureDescriptor->ResourceHandle, Texture::num_samples, ir.getInt32(0));
  auto Position = GetSamplePos(SampleCount, *Index);
  return ir.CreateInsertValue(
      ir.CreateInsertValue(
          llvm::PoisonValue::get(MapType(Call.getType())), ir.CreateExtractElement(Position, 0ull), {0ull}
      ),
      ir.CreateExtractElement(Position, 1ull), {1ull}
  );
}

llvm::Expected<llvm::Value *>
Converter::EmitReadLaneAt(const CallInst &Call, bool Quad) {
  auto Value = Get(Call.getArgOperand(1));
  if (!Value)
    return Value.takeError();

  auto LaneId = Get(Call.getArgOperand(2));
  if (!LaneId)
    return LaneId.takeError();

  return air.CreateShuffle(*Value, ir.CreateTrunc(*LaneId, ir.getInt16Ty()), Quad);
}

llvm::Expected<llvm::Value *>
Converter::EmitQuadOp(const CallInst &Call) {
  auto Value = Get(Call.getArgOperand(1));
  if (!Value)
    return Value.takeError();

  auto Kind = asU32(Call.getArgOperand(2));
  if (!Kind)
    return unsupported("dxil: non-constant quadOp kind");

  return air.CreateQuadShuffleXor(*Value, *Kind + 1);
}

llvm::Expected<llvm::Value *>
Converter::EmitBinaryWithTwoOutputs(const CallInst &Call, uint32_t OpId) {
  if (Call.arg_size() != 3)
    return unsupported("dxil: unexpected binary signature");
  auto LHS = Get(Call.getArgOperand(1));
  if (!LHS)
    return LHS.takeError();
  auto RHS = Get(Call.getArgOperand(2));
  if (!RHS)
    return RHS.takeError();

  switch (OpId) {
  case kOpUDiv: {
    // {quotient, remainder}, both 0xffffffff for a divisor of 0, as Direct3D's udiv (LLVM's udiv by 0 is undefined).
    auto *ByZero = ir.CreateICmpEQ(*RHS, ir.getInt32(0));
    auto *Divisor = ir.CreateSelect(ByZero, ir.getInt32(1), *RHS);
    return ir.CreateInsertValue(
        ir.CreateInsertValue(
            llvm::PoisonValue::get(MapType(Call.getType())),
            ir.CreateSelect(ByZero, ir.getInt32(~0u), ir.CreateUDiv(*LHS, Divisor)), {0ull}
        ),
        ir.CreateSelect(ByZero, ir.getInt32(~0u), ir.CreateURem(*LHS, Divisor)), {1ull}
    );
  }
  case kOpUMul: {
    auto Result = ir.CreateMul(
        ir.CreateZExt(*LHS, (*LHS)->getType()->getExtendedType()),
        ir.CreateZExt(*RHS, (*RHS)->getType()->getExtendedType())
    );
    return ir.CreateInsertValue(
        ir.CreateInsertValue(
            llvm::PoisonValue::get(MapType(Call.getType())),
            ir.CreateTrunc(ir.CreateLShr(Result, 32u), (*LHS)->getType()), {0ull}
        ),
        ir.CreateTrunc(Result, (*RHS)->getType()), {1ull}
    );
  }
  case kOpIMul: {
    auto Result = ir.CreateMul(
        ir.CreateSExt(*LHS, (*LHS)->getType()->getExtendedType()),
        ir.CreateSExt(*RHS, (*RHS)->getType()->getExtendedType())
    );
    return ir.CreateInsertValue(
        ir.CreateInsertValue(
            llvm::PoisonValue::get(MapType(Call.getType())),
            ir.CreateTrunc(ir.CreateLShr(Result, 32u), (*LHS)->getType()), {0ull}
        ),
        ir.CreateTrunc(Result, (*RHS)->getType()), {1ull}
    );
  }
  case kOpUAddc: {
    auto Result = ir.CreateAdd(
        ir.CreateZExt(*LHS, (*LHS)->getType()->getExtendedType()),
        ir.CreateZExt(*RHS, (*RHS)->getType()->getExtendedType())
    );
    return ir.CreateInsertValue(
        ir.CreateInsertValue(
            llvm::PoisonValue::get(MapType(Call.getType())), ir.CreateTrunc(Result, (*LHS)->getType()), {0ull}
        ),
        ir.CreateTrunc(ir.CreateLShr(Result, 32u), Result->getType()->getWithNewBitWidth(1)), {1ull}
    );
  }
  case kOpUSubb: {
    auto Result = ir.CreateSub(
        ir.CreateZExt(*LHS, (*LHS)->getType()->getExtendedType()),
        ir.CreateZExt(*RHS, (*RHS)->getType()->getExtendedType())
    );
    return ir.CreateInsertValue(
        ir.CreateInsertValue(
            llvm::PoisonValue::get(MapType(Call.getType())), ir.CreateTrunc(Result, (*LHS)->getType()), {0ull}
        ),
        ir.CreateTrunc(ir.CreateLShr(Result, 32u), Result->getType()->getWithNewBitWidth(1)), {1ull}
    );
  }
  default:
    return unsupported("dxil: unsupported binaryWithTwoOutputs operation: " + std::to_string(OpId));
  }
}

llvm::Expected<llvm::Value *>
Converter::EmitStorePatchConstant(const CallInst &Call) {
  auto Element = asU32(Call.getArgOperand(1));
  auto Component = asU32(Call.getArgOperand(3));
  if (!Element || !Component)
    return unsupported("dxil: non-constant storePatchConstant index");
  if (*Element >= entry.patch_constants.size())
    return unsupported("dxil: storePatchConstant element index out of range");
  auto Row = Get(Call.getArgOperand(2));
  if (!Row)
    return Row.takeError();
  auto Value = Get(Call.getArgOperand(4));
  if (!Value)
    return Value.takeError();
  if (!*Value)
    return unsupported("dxil: missing storePatchConstant value");
  const auto &signature = entry.patch_constants[*Element];
  auto *Stored = *Value;

  uint32_t Lane = (uint32_t)signature.start_col + *Component;
  if (Lane > 3)
    return unsupported("dxil: storePatchConstant component out of range");

  if (signature.start_row < 0 || signature.start_col < 0)
    return unsupported("dxil: storePatchConstant signature element is not mapped");
  auto &registers = ctx.resource.patch_constant_output;
  if (!registers.ptr_int4 || !ctx.resource.output.ptr_float4)
    return unsupported("dxil: storePatchConstant register file is not available");

  SmallVector<llvm::Value *> RegIdxs;
  RegIdxs.push_back(ir.getInt32(0));
  RegIdxs.push_back(ir.CreateAdd(*Row, ir.getInt32(signature.start_row)));
  RegIdxs.push_back(ir.getInt32(Lane));

  if (Stored->getType()->isFloatTy()) {
    auto *Ptr =
        ir.CreateGEP(registers.ptr_float4->getType()->getNonOpaquePointerElementType(), registers.ptr_float4, RegIdxs);
    ir.CreateStore(Stored, Ptr);
    return (llvm::Value *)nullptr;
  }
  if (Stored->getType()->isHalfTy()) {
    Stored = ir.CreateZExt(ir.CreateBitCast(Stored, ir.getInt16Ty()), ir.getInt32Ty());
    auto *Ptr =
        ir.CreateGEP(registers.ptr_int4->getType()->getNonOpaquePointerElementType(), registers.ptr_int4, RegIdxs);
    ir.CreateStore(Stored, Ptr);
    return (llvm::Value *)nullptr;
  }
  if (Stored->getType()->isIntegerTy()) {
    if (Stored->getType()->getIntegerBitWidth() < 32)
      Stored = ir.CreateZExt(Stored, ir.getInt32Ty());
    else if (Stored->getType()->getIntegerBitWidth() > 32)
      return unsupported("dxil: unsupported storeOutput integer width");
    auto *Ptr =
        ir.CreateGEP(registers.ptr_int4->getType()->getNonOpaquePointerElementType(), registers.ptr_int4, RegIdxs);
    ir.CreateStore(Stored, Ptr);
    return (llvm::Value *)nullptr;
  }
  return unsupported("dxil: unsupported storePatchConstant type");
}

llvm::Expected<llvm::Value *>
Converter::EmitLoadOutputControlPoint(const CallInst &Call) {
  auto Element = asU32(Call.getArgOperand(1));
  auto Component = asU32(Call.getArgOperand(3));
  if (!Element || !Component)
    return unsupported("dxil: non-constant loadOutputControlPoint index");
  auto Row = Get(Call.getArgOperand(2));
  if (!Row)
    return Row.takeError();
  if (*Element >= entry.outputs.size())
    return unsupported("dxil: loadOutputControlPoint element index out of range");
  const auto &signature = entry.outputs[*Element];
  if (signature.start_row < 0 || signature.start_col < 0)
    return unsupported("dxil: loadOutputControlPoint signature element is not mapped");
  auto &registers = ctx.resource.output;
  if (!registers.ptr_int4 || !registers.ptr_float4)
    return unsupported("dxil: loadOutputControlPoint register file is not available");

  auto CPId = Get(Call.getArgOperand(4));
  if (!CPId)
    return CPId.takeError();

  auto *Type = Call.getType();
  uint32_t Lane = (uint32_t)signature.start_col + *Component;
  if (Lane > 3)
    return unsupported("dxil: loadOutputControlPoint component out of range");

  SmallVector<Value *> RegIdxs;
  RegIdxs.push_back(ir.getInt32(0));
  RegIdxs.push_back(*CPId);
  RegIdxs.push_back(ir.CreateAdd(*Row, ir.getInt32(signature.start_row)));
  RegIdxs.push_back(ir.getInt32(Lane));

  if (Type->isIntegerTy()) {
    auto *Ptr =
        ir.CreateGEP(registers.ptr_int4->getType()->getNonOpaquePointerElementType(), registers.ptr_int4, RegIdxs);
    auto *Value = ir.CreateLoad(ir.getInt32Ty(), Ptr);
    if (Type->getIntegerBitWidth() < 32)
      return ir.CreateTrunc(Value, Type);
    if (Type->getIntegerBitWidth() == 32)
      return Value;
    return unsupported("dxil: unsupported loadInput integer width");
  }
  if (Type->isFloatTy()) {
    auto *Ptr =
        ir.CreateGEP(registers.ptr_float4->getType()->getNonOpaquePointerElementType(), registers.ptr_float4, RegIdxs);
    return ir.CreateLoad(ir.getFloatTy(), Ptr);
  }
  if (Type->isHalfTy()) {
    if (signature.semantic_kind == SemanticKind::Position) {
      auto *Ptr = ir.CreateGEP(
          registers.ptr_float4->getType()->getNonOpaquePointerElementType(), registers.ptr_float4, RegIdxs
      );
      return air.CreateConvertToHalf(ir.CreateLoad(ir.getFloatTy(), Ptr));
    }
    auto *Ptr =
        ir.CreateGEP(registers.ptr_int4->getType()->getNonOpaquePointerElementType(), registers.ptr_int4, RegIdxs);
    auto *Value = ir.CreateLoad(ir.getInt32Ty(), Ptr);
    return ir.CreateBitCast(ir.CreateTrunc(Value, ir.getInt16Ty()), ir.getHalfTy());
  }
  return unsupported("dxil: unsupported loadOutputControlPoint type");
}

llvm::Expected<llvm::Value *>
Converter::EmitLoadPatchConstant(const CallInst &Call) {
  auto Element = asU32(Call.getArgOperand(1));
  auto Row = asU32(Call.getArgOperand(2));
  auto Component = asU32(Call.getArgOperand(3));
  if (!Element || !Row || !Component)
    return unsupported("dxil: non-constant loadPatchConstant index");
  if (*Element >= entry.patch_constants.size())
    return unsupported("dxil: loadPatchConstant element index out of range");
  const auto &signature = entry.patch_constants[*Element];
  if (signature.start_row < 0 || signature.start_col < 0)
    return unsupported("dxil: loadPatchConstant signature element is not mapped");
  auto &registers = ctx.resource.patch_constant_output;
  if (!registers.ptr_int4 || !registers.ptr_float4)
    return unsupported("dxil: loadPatchConstant register file is not available");

  auto *Type = Call.getType();
  uint32_t Reg = (uint32_t)signature.start_row + *Row;
  uint32_t Lane = (uint32_t)signature.start_col + *Component;
  if (Lane > 3)
    return unsupported("dxil: loadPatchConstant component out of range");

  SmallVector<Value *> RegIdxs;
  RegIdxs.push_back(ir.getInt32(0));
  RegIdxs.push_back(ir.getInt32(Reg));
  RegIdxs.push_back(ir.getInt32(Lane));

  if (Type->isIntegerTy()) {
    auto *Ptr =
        ir.CreateGEP(registers.ptr_int4->getType()->getNonOpaquePointerElementType(), registers.ptr_int4, RegIdxs);
    auto *Value = ir.CreateLoad(ir.getInt32Ty(), Ptr);
    if (Type->getIntegerBitWidth() < 32)
      return ir.CreateTrunc(Value, Type);
    if (Type->getIntegerBitWidth() == 32)
      return Value;
    return unsupported("dxil: unsupported loadPatchConstant integer width");
  }
  if (Type->isFloatTy()) {
    auto *Ptr =
        ir.CreateGEP(registers.ptr_float4->getType()->getNonOpaquePointerElementType(), registers.ptr_float4, RegIdxs);
    return ir.CreateLoad(ir.getFloatTy(), Ptr);
  }
  if (Type->isHalfTy()) {
    auto *Ptr =
        ir.CreateGEP(registers.ptr_int4->getType()->getNonOpaquePointerElementType(), registers.ptr_int4, RegIdxs);
    auto *Value = ir.CreateLoad(ir.getInt32Ty(), Ptr);
    return ir.CreateBitCast(ir.CreateTrunc(Value, ir.getInt16Ty()), ir.getHalfTy());
  }
  return unsupported("dxil: unsupported loadPatchConstant type");
}

llvm::Expected<llvm::Value *>
Converter::EmitEval(const CallInst &Call, uint32_t OpId) {
  auto Element = asU32(Call.getArgOperand(1));
  auto Row = asU32(Call.getArgOperand(2));
  auto Component = asU32(Call.getArgOperand(3));
  if (!Element || !Row || !Component)
    return unsupported("dxil: non-constant eval index");
  if (*Element >= entry.inputs.size())
    return unsupported("dxil: eval element index out of range");
  const auto &signature = entry.inputs[*Element];
  if (signature.start_row < 0 || signature.start_col < 0)
    return unsupported("dxil: eval signature element is not mapped");

  auto *Type = Call.getType();
  uint32_t Reg = (uint32_t)signature.start_row + *Row;
  uint32_t Lane = (uint32_t)signature.start_col + *Component;
  if (Lane > 3)
    return unsupported("dxil: eval component out of range");

  auto iter = ctx.resource.interpolant_map.find(Reg);
  if (iter == ctx.resource.interpolant_map.end())
    return unsupported("dxil: no eval interpolant");

  if (Type->isFloatTy() || Type->isHalfTy()) {
    auto &[ArgIndex, Perspective] = iter->second;

    if (OpId == kOpEvalSampleIndex) {
      auto SampleIndex = Get(Call.getArgOperand(4));
      if (!SampleIndex)
        return SampleIndex.takeError();
      return ir.CreateExtractElement(
          air.CreateInterpolateAtSample(ctx.function->getArg(ArgIndex), *SampleIndex, Perspective), Lane
      );
    } else if (OpId == kOpEvalSnapped) {
      auto X = Get(Call.getArgOperand(4));
      if (!X)
        return X.takeError();
      auto Y = Get(Call.getArgOperand(5));
      if (!Y)
        return Y.takeError();
      auto Truncated = ir.CreateAnd(
          ir.CreateAdd(
              ir.CreateInsertElement(ir.CreateInsertElement(PoisonValue::get(air.getIntTy(2)), *X, 0ull), *Y, 1ull),
              air.getInt2(8, 8)
          ),
          air.getInt2(0b1111, 0b1111)
      );
      auto OffsetFloat = ir.CreateFMul(air.CreateConvertToFloat(Truncated), air.getFloat2(1.0f / 16.0f, 1.0f / 16.0f));
      return ir.CreateExtractElement(
          air.CreateInterpolateAtOffset(ctx.function->getArg(ArgIndex), OffsetFloat, Perspective), Lane
      );
    } else {
      return ir.CreateExtractElement(
          air.CreateInterpolateAtCentroid(ctx.function->getArg(ArgIndex), Perspective), Lane
      );
    }
  }
  return unsupported("dxil: unsupported eval type");
}

llvm::Expected<llvm::Value *>
Converter::EmitWaveActiveAllEqual(const CallInst &Call) {
  auto Operand = Get(Call.getArgOperand(1));
  if (!Operand)
    return Operand.takeError();

  if (Operand.get()->getType()->getScalarType()->isIntOrIntVectorTy(64))
    return unsupported("dxil: 64-bit wave ops not supported");

  auto *Value = *Operand;

  if (Value->getType()->isFPOrFPVectorTy()) {
    if (auto TyVec = dyn_cast<FixedVectorType>(Value->getType())) {
      if (TyVec->getElementType()->isFloatTy()) {
        Value = ir.CreateBitCast(Value, FixedVectorType::get(ir.getInt32Ty(), TyVec->getNumElements()));
      } else if (TyVec->getElementType()->isHalfTy()) {
        Value = ir.CreateBitCast(Value, FixedVectorType::get(ir.getInt16Ty(), TyVec->getNumElements()));
      } else {
        return unsupported("dxil: unhandled floating point type");
      }
    } else if (Value->getType()->isFloatTy()) {
      Value = ir.CreateBitCast(Value, ir.getInt32Ty());
    } else if (Value->getType()->isHalfTy()) {
      Value = ir.CreateBitCast(Value, ir.getInt16Ty());
    } else {
      return unsupported("dxil: unhandled floating point type");
    }
  }

  llvm::Value *ValueFirst = air.CreateSIMDBroadcastFirst(Value);

  auto *Result = ir.CreateICmpEQ(Value, ValueFirst);
  if (auto TyVec = dyn_cast<FixedVectorType>(Result->getType())) {
    llvm::Value *Ret = Constant::getNullValue(Result->getType());
    for (unsigned i = 0; i < TyVec->getNumElements(); i++) {
      Ret = ir.CreateInsertElement(Ret, air.CreateSIMDAllAny(ir.CreateExtractElement(Result, i), false), i);
    }
    return Ret;
  } else {
    return air.CreateSIMDAllAny(Result, false);
  }
}

llvm::Expected<llvm::Value *>
Converter::EmitMSAD(const CallInst &Call) {
  auto Ref = Get(Call.getArgOperand(1));
  if (!Ref)
    return Ref.takeError();
  auto Src = Get(Call.getArgOperand(2));
  if (!Src)
    return Src.takeError();
  auto Accum = Get(Call.getArgOperand(3));
  if (!Accum)
    return Accum.takeError();

  auto &Context = air.getContext();
  auto Attrs = AttributeList::get(
      Context, {{~0U, Attribute::get(Context, Attribute::AttrKind::ReadNone)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::NoUnwind)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::WillReturn)}}
  );

  SmallVector<Value *> Ops;
  SmallVector<Type *> Tys;

  Tys.push_back((*Ref)->getType());
  Ops.push_back(*Ref);
  Tys.push_back((*Src)->getType());
  Ops.push_back(*Src);
  Tys.push_back((*Accum)->getType());
  Ops.push_back(*Accum);

  std::string FnName = "dxmt.msad";
  FnName += air.getTypeOverloadSuffix((*Ref)->getType());

  auto Fn = air.getModule()->getOrInsertFunction(FnName, FunctionType::get((*Accum)->getType(), Tys, false), Attrs);
  return ir.CreateCall(Fn, Ops);
}

llvm::Expected<llvm::Value *>
Converter::EmitBitfieldInsert(const CallInst &Call) {
  auto Src0 = Get(Call.getArgOperand(1));
  if (!Src0)
    return Src0.takeError();
  auto Src1 = Get(Call.getArgOperand(2));
  if (!Src1)
    return Src1.takeError();
  auto Src2 = Get(Call.getArgOperand(3));
  if (!Src2)
    return Src2.takeError();
  auto Src3 = Get(Call.getArgOperand(4));
  if (!Src3)
    return Src3.takeError();

  auto Width = ir.CreateAnd(*Src0, 0x1F);
  auto Offset = ir.CreateAnd(*Src1, 0X1F);
  auto Constant_1 = llvm::ConstantInt::get(Width->getType(), 1);
  auto Bitmask = ir.CreateShl(ir.CreateSub(ir.CreateShl(Constant_1, Width), Constant_1), Offset);

  return ir.CreateOr(ir.CreateAnd(ir.CreateShl(*Src2, Offset), Bitmask), ir.CreateAnd(*Src3, ir.CreateNot(Bitmask)));
}

llvm::Expected<llvm::Value *>
Converter::EmitCbufferLoad(const CallInst &Call) {
  auto Handle = ResolveHandleOperand(Call.getArgOperand(1));
  if (!Handle)
    return Handle.takeError();
  auto Descriptor = ctx.binding.GetConstantBuffer(air, Handle->range_id, Handle->index);
  if (!Descriptor)
    return unsupported("dxil: no constant buffer descriptor for resource");
  auto ByteOffset = Get(Call.getArgOperand(2));
  if (!ByteOffset)
    return ByteOffset.takeError();
  auto Alignment = asU32(Call.getArgOperand(3));
  if (!Alignment)
    return unsupported("dxil: non-constant cbufferLoad alignment");

  auto *TyResult = MapType(Call.getType());
  auto AddrSpace = Descriptor->Pointer->getType()->getPointerAddressSpace();
  auto *PtrCast = ir.CreatePointerCast(
      ir.CreateGEP(
          ir.getInt8Ty(), ir.CreatePointerCast(Descriptor->Pointer, ir.getInt8PtrTy(AddrSpace)), {*ByteOffset}
      ),
      TyResult->getPointerTo(AddrSpace)
  );
  return ir.CreateAlignedLoad(TyResult, PtrCast, *Alignment ? Align(*Alignment) : Align());
}

} // namespace dxmt::dxil
