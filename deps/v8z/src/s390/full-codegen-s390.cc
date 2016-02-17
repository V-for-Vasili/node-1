// Copyright 2015 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/v8.h"

#if V8_TARGET_ARCH_S390

#include "src/code-factory.h"
#include "src/code-stubs.h"
#include "src/codegen.h"
#include "src/compiler.h"
#include "src/debug.h"
#include "src/full-codegen.h"
#include "src/ic/ic.h"
#include "src/parser.h"
#include "src/scopes.h"

#include "src/s390/code-stubs-s390.h"
#include "src/s390/macro-assembler-s390.h"

namespace v8 {
namespace internal {

#define __ ACCESS_MASM(masm_)

// A patch site is a location in the code which it is possible to patch. This
// class has a number of methods to emit the code which is patchable and the
// method EmitPatchInfo to record a marker back to the patchable code. This
// marker is a cmpi rx, #yyy instruction, and x * 0x0000ffff + yyy (raw 16 bit
// immediate value is used) is the delta from the pc to the first instruction of
// the patchable code.
// See PatchInlinedSmiCode in ic-s390.cc for the code that patches it
class JumpPatchSite BASE_EMBEDDED {
 public:
  explicit JumpPatchSite(MacroAssembler* masm) : masm_(masm) {
#ifdef DEBUG
    info_emitted_ = false;
#endif
  }

  ~JumpPatchSite() { DCHECK(patch_site_.is_bound() == info_emitted_); }

  // When initially emitting this ensure that a jump is always generated to skip
  // the inlined smi code.
  void EmitJumpIfNotSmi(Register reg, Label* target) {
    DCHECK(!patch_site_.is_bound() && !info_emitted_);
    Assembler::BlockTrampolinePoolScope block_trampoline_pool(masm_);
    __ bind(&patch_site_);
    __ CmpP(reg, reg);
    // Emit the Nop to make bigger place for patching on 31-bit
    // as the TestIfSmi sequence uses 4-byte TMLL
#ifndef V8_TARGET_ARCH_S390X
    __ nop();
#endif
    __ beq(target);  // Always taken before patched.
  }

  // When initially emitting this ensure that a jump is never generated to skip
  // the inlined smi code.
  void EmitJumpIfSmi(Register reg, Label* target) {
    Assembler::BlockTrampolinePoolScope block_trampoline_pool(masm_);
    DCHECK(!patch_site_.is_bound() && !info_emitted_);
    __ bind(&patch_site_);
    __ CmpP(reg, reg);
    // Emit the Nop to make bigger place for patching on 31-bit
    // as the TestIfSmi sequence uses 4-byte TMLL
#ifndef V8_TARGET_ARCH_S390X
    __ nop();
#endif
    __ bne(target);  // Never taken before patched.
  }

  void EmitPatchInfo() {
    if (patch_site_.is_bound()) {
      int delta_to_patch_site = masm_->SizeOfCodeGeneratedSince(&patch_site_);
      DCHECK(is_int16(delta_to_patch_site));
      __ chi(r0, Operand(delta_to_patch_site));
#ifdef DEBUG
      info_emitted_ = true;
#endif
    } else {
      __ nop();
      __ nop();
    }
  }

 private:
  MacroAssembler* masm_;
  Label patch_site_;
#ifdef DEBUG
  bool info_emitted_;
#endif
};


// Generate code for a JS function.  On entry to the function the receiver
// and arguments have been pushed on the stack left to right.  The actual
// argument count matches the formal parameter count expected by the
// function.
//
// The live registers are:
//   o r3: the JS function object being called (i.e., ourselves)
//   o cp: our context
//   o fp: our caller's frame pointer
//   o sp: stack pointer
//   o lr: return address
//   o ip: our own function entry (required by the prologue)
//
// The function builds a JS frame.  Please see JavaScriptFrameConstants in
// frames-s390.h for its layout.
void FullCodeGenerator::Generate() {
  CompilationInfo* info = info_;
  profiling_counter_ = isolate()->factory()->NewCell(
      Handle<Smi>(Smi::FromInt(FLAG_interrupt_budget), isolate()));
  SetFunctionPosition(function());
  Comment cmnt(masm_, "[ function compiled by full code generator");

  ProfileEntryHookStub::MaybeCallEntryHook(masm_);

#ifdef DEBUG
  if (strlen(FLAG_stop_at) > 0 &&
      info->function()->name()->IsUtf8EqualTo(CStrVector(FLAG_stop_at))) {
    __ stop("stop-at");
  }
#endif

  // Sloppy mode functions and builtins need to replace the receiver with the
  // global proxy when called as functions (without an explicit receiver
  // object).
  if (is_sloppy(info->language_mode()) && !info->is_native() &&
      info->MayUseThis() && info->scope()->has_this_declaration()) {
    Label ok;
    int receiver_offset = info->scope()->num_parameters() * kPointerSize;
    __ LoadP(r4, MemOperand(sp, receiver_offset), r0);
    __ CompareRoot(r4, Heap::kUndefinedValueRootIndex);
    __ bne(&ok, Label::kNear);

    __ LoadP(r4, GlobalObjectOperand());
    __ LoadP(r4, FieldMemOperand(r4, GlobalObject::kGlobalProxyOffset));

    __ StoreP(r4, MemOperand(sp, receiver_offset), r0);

    __ bind(&ok);
  }

  // Open a frame scope to indicate that there is a frame on the stack.  The
  // MANUAL indicates that the scope shouldn't actually generate code to set up
  // the frame (that is done below).
  FrameScope frame_scope(masm_, StackFrame::MANUAL);
  int prologue_offset = masm_->pc_offset();

  info->set_prologue_offset(prologue_offset);
  __ Prologue(info->IsCodePreAgingActive(), prologue_offset);
  info->AddNoFrameRange(0, masm_->pc_offset());

  {
    Comment cmnt(masm_, "[ Allocate locals");
    int locals_count = info->scope()->num_stack_slots();
    // Generators allocate locals, if any, in context slots.
    DCHECK(!IsGeneratorFunction(info->function()->kind()) || locals_count == 0);
    if (locals_count > 0) {
      if (locals_count >= 128) {
        Label ok;
        __ AddP(ip, sp, Operand(-(locals_count * kPointerSize)));
        __ LoadRoot(r5, Heap::kRealStackLimitRootIndex);
        __ CmpLogicalP(ip, r5);
        __ bge(&ok, Label::kNear);
        __ InvokeBuiltin(Builtins::STACK_OVERFLOW, CALL_FUNCTION);
        __ bind(&ok);
      }
      __ LoadRoot(ip, Heap::kUndefinedValueRootIndex);
      int kMaxPushes = FLAG_optimize_for_size ? 4 : 32;
      if (locals_count >= kMaxPushes) {
        int loop_iterations = locals_count / kMaxPushes;
        __ mov(r4, Operand(loop_iterations));
        Label loop_header;
        __ bind(&loop_header);
        // Do pushes.
        // TODO(joransiu): Consider using MVC
        __ lay(sp, MemOperand(sp, -kMaxPushes * kPointerSize));
        for (int i = 0; i < kMaxPushes; i++) {
          __ StoreP(ip, MemOperand(sp, i * kPointerSize));
        }
        // Continue loop if not done.
        __ BranchOnCount(r4, &loop_header);
      }
      int remaining = locals_count % kMaxPushes;
      // Emit the remaining pushes.
      // TODO(joransiu): Consider using MVC
      if (remaining > 0) {
        __ lay(sp, MemOperand(sp, -remaining * kPointerSize));
      for (int i = 0; i < remaining; i++) {
          __ StoreP(ip, MemOperand(sp, i * kPointerSize));
        }
      }
    }
  }

  bool function_in_register = true;

  // Possibly allocate a local context.
  if (info->scope()->num_heap_slots() > 0) {
    // Argument to NewContext is the function, which is still in r3.
    Comment cmnt(masm_, "[ Allocate context");
    bool need_write_barrier = true;
    int slots = info->scope()->num_heap_slots() - Context::MIN_CONTEXT_SLOTS;
    if (info->scope()->is_script_scope()) {
      __ push(r3);
      __ Push(info->scope()->GetScopeInfo(info->isolate()));
      __ CallRuntime(Runtime::kNewScriptContext, 2);
    } else if (slots <= FastNewContextStub::kMaximumSlots) {
      FastNewContextStub stub(isolate(), slots);
      __ CallStub(&stub);
      // Result of FastNewContextStub is always in new space.
      need_write_barrier = false;
    } else {
      __ push(r3);
      __ CallRuntime(Runtime::kNewFunctionContext, 1);
    }
    function_in_register = false;
    // Context is returned in r2.  It replaces the context passed to us.
    // It's saved in the stack and kept live in cp.
    __ LoadRR(cp, r2);
    __ StoreP(r2, MemOperand(fp, StandardFrameConstants::kContextOffset));
    // Copy any necessary parameters into the context.
    int num_parameters = info->scope()->num_parameters();
    int first_parameter = info->scope()->has_this_declaration() ? -1 : 0;
    for (int i = first_parameter; i < num_parameters; i++) {
      Variable* var = (i == -1) ? scope()->receiver() : scope()->parameter(i);
      if (var->IsContextSlot()) {
        int parameter_offset = StandardFrameConstants::kCallerSPOffset +
                               (num_parameters - 1 - i) * kPointerSize;
        // Load parameter from stack.
        __ LoadP(r2, MemOperand(fp, parameter_offset), r0);
        // Store it in the context.
        MemOperand target = ContextOperand(cp, var->index());
        __ StoreP(r2, target);

        // Update the write barrier.
        if (need_write_barrier) {
          __ RecordWriteContextSlot(cp, target.offset(), r2, r5,
                                    kLRHasBeenSaved, kDontSaveFPRegs);
        } else if (FLAG_debug_code) {
          Label done;
          __ JumpIfInNewSpace(cp, r2, &done);
          __ Abort(kExpectedNewSpaceObject);
          __ bind(&done);
        }
      }
    }
  }

  // Possibly set up a local binding to the this function which is used in
  // derived constructors with super calls.
  Variable* this_function_var = scope()->this_function_var();
  if (this_function_var != nullptr) {
    Comment cmnt(masm_, "[ This function");
    if (!function_in_register) {
      __ LoadP(r3, MemOperand(fp, JavaScriptFrameConstants::kFunctionOffset));
      // The write barrier clobbers register again, keep is marked as such.
    }
    SetVar(this_function_var, r3, r2, r4);
  }

  Variable* new_target_var = scope()->new_target_var();
  if (new_target_var != nullptr) {
    Comment cmnt(masm_, "[ new.target");

    // Get the frame pointer for the calling frame.
    __ LoadP(r4, MemOperand(fp, StandardFrameConstants::kCallerFPOffset));

    // Skip the arguments adaptor frame if it exists.
    __ LoadP(r3, MemOperand(r4, StandardFrameConstants::kContextOffset));
    __ CmpSmiLiteral(r3, Smi::FromInt(StackFrame::ARGUMENTS_ADAPTOR), r0);
    Label skip;
    __ bne(&skip);
    __ LoadP(r4, MemOperand(r4, StandardFrameConstants::kCallerFPOffset));
    __ bind(&skip);

    // Check the marker in the calling frame.
    __ LoadP(r3, MemOperand(r4, StandardFrameConstants::kMarkerOffset));
    __ CmpSmiLiteral(r3, Smi::FromInt(StackFrame::CONSTRUCT), r0);
    Label non_construct_frame, done;

    __ bne(&non_construct_frame);
    __ LoadP(r2, MemOperand(
                     r4, ConstructFrameConstants::kOriginalConstructorOffset));
    __ b(&done);

    __ bind(&non_construct_frame);
    __ LoadRoot(r2, Heap::kUndefinedValueRootIndex);
    __ bind(&done);

    SetVar(new_target_var, r2, r4, r5);
  }

  // Possibly allocate RestParameters
  int rest_index;
  Variable* rest_param = scope()->rest_parameter(&rest_index);
  if (rest_param) {
    Comment cmnt(masm_, "[ Allocate rest parameter array");

    int num_parameters = info->scope()->num_parameters();
    int offset = num_parameters * kPointerSize;

    __ AddP(r5, fp, Operand(StandardFrameConstants::kCallerSPOffset + offset));
    __ LoadSmiLiteral(r4, Smi::FromInt(num_parameters));
    __ LoadSmiLiteral(r3, Smi::FromInt(rest_index));
    __ LoadSmiLiteral(r2, Smi::FromInt(language_mode()));
    __ Push(r5, r4, r3, r2);

    RestParamAccessStub stub(isolate());
    __ CallStub(&stub);

    SetVar(rest_param, r2, r3, r4);
  }

// @TODO ---- VERIFY THE REGS BELOW to see if they work on S390!!!

  Variable* arguments = scope()->arguments();
  if (arguments != NULL) {
    // Function uses arguments object.
    Comment cmnt(masm_, "[ Allocate arguments object");
    if (!function_in_register) {
      // Load this again, if it's used by the local context below.
      __ LoadP(r5, MemOperand(fp, JavaScriptFrameConstants::kFunctionOffset));
    } else {
      __ LoadRR(r5, r3);
    }
    // Receiver is just before the parameters on the caller's stack.
    int num_parameters = info->scope()->num_parameters();
    int offset = num_parameters * kPointerSize;
    __ la(r4, MemOperand(fp, StandardFrameConstants::kCallerSPOffset + offset));
    __ LoadSmiLiteral(r3, Smi::FromInt(num_parameters));
    __ Push(r5, r4, r3);

    // Arguments to ArgumentsAccessStub:
    //   function, receiver address, parameter count.
    // The stub will rewrite receiever and parameter count if the previous
    // stack frame was an arguments adapter frame.
    ArgumentsAccessStub::Type type;
    if (is_strict(language_mode()) || !is_simple_parameter_list()) {
      type = ArgumentsAccessStub::NEW_STRICT;
    } else if (function()->has_duplicate_parameters()) {
      type = ArgumentsAccessStub::NEW_SLOPPY_SLOW;
    } else {
      type = ArgumentsAccessStub::NEW_SLOPPY_FAST;
    }
    ArgumentsAccessStub stub(isolate(), type);
    __ CallStub(&stub);

    SetVar(arguments, r2, r3, r4);
  }

  if (FLAG_trace) {
    __ CallRuntime(Runtime::kTraceEnter, 0);
  }

  // Visit the declarations and body unless there is an illegal
  // redeclaration.
  if (scope()->HasIllegalRedeclaration()) {
    Comment cmnt(masm_, "[ Declarations");
    VisitForEffect(scope()->GetIllegalRedeclaration());

  } else {
    PrepareForBailoutForId(BailoutId::FunctionEntry(), NO_REGISTERS);
    {
      Comment cmnt(masm_, "[ Declarations");
      // For named function expressions, declare the function name as a
      // constant.
      if (scope()->is_function_scope() && scope()->function() != NULL) {
        VariableDeclaration* function = scope()->function();
        DCHECK(function->proxy()->var()->mode() == CONST ||
               function->proxy()->var()->mode() == CONST_LEGACY);
        DCHECK(!function->proxy()->var()->IsUnallocatedOrGlobalSlot());
        VisitVariableDeclaration(function);
      }
      VisitDeclarations(scope()->declarations());
    }

    {
      Comment cmnt(masm_, "[ Stack check");
      PrepareForBailoutForId(BailoutId::Declarations(), NO_REGISTERS);
      Label ok;
      __ LoadRoot(ip, Heap::kStackLimitRootIndex);
      __ CmpLogicalP(sp, ip);
      __ bge(&ok, Label::kNear);
      __ Call(isolate()->builtins()->StackCheck(), RelocInfo::CODE_TARGET);
      __ bind(&ok);
    }

    {
      Comment cmnt(masm_, "[ Body");
      DCHECK(loop_depth() == 0);
      VisitStatements(function()->body());
      DCHECK(loop_depth() == 0);
    }
  }

  // Always emit a 'return undefined' in case control fell off the end of
  // the body.
  {
    Comment cmnt(masm_, "[ return <undefined>;");
    __ LoadRoot(r2, Heap::kUndefinedValueRootIndex);
  }
  EmitReturnSequence();
}


void FullCodeGenerator::ClearAccumulator() {
  __ LoadSmiLiteral(r2, Smi::FromInt(0));
}


void FullCodeGenerator::EmitProfilingCounterDecrement(int delta) {
  __ mov(r4, Operand(profiling_counter_));
  intptr_t smi_delta = reinterpret_cast<intptr_t>(Smi::FromInt(delta));
  if (CpuFeatures::IsSupported(GENERAL_INSTR_EXT) && is_int8(-smi_delta)) {
    __ AddP(FieldMemOperand(r4, Cell::kValueOffset),
            Operand(-smi_delta));
    __ LoadP(r5, FieldMemOperand(r4, Cell::kValueOffset));
  } else {
    __ LoadP(r5, FieldMemOperand(r4, Cell::kValueOffset));
    __ SubSmiLiteral(r5, r5, Smi::FromInt(delta), r0);
    __ StoreP(r5, FieldMemOperand(r4, Cell::kValueOffset));
  }
}


void FullCodeGenerator::EmitProfilingCounterReset() {
  int reset_value = FLAG_interrupt_budget;
  if (info_->is_debug()) {
    // Detect debug break requests as soon as possible.
    reset_value = FLAG_interrupt_budget >> 4;
  }
  __ mov(r4, Operand(profiling_counter_));
  __ LoadSmiLiteral(r5, Smi::FromInt(reset_value));
  __ StoreP(r5, FieldMemOperand(r4, Cell::kValueOffset));
}


void FullCodeGenerator::EmitBackEdgeBookkeeping(IterationStatement* stmt,
                                                Label* back_edge_target) {
  Comment cmnt(masm_, "[ Back edge bookkeeping");
  Label ok;

  DCHECK(back_edge_target->is_bound());
  int distance = masm_->SizeOfCodeGeneratedSince(back_edge_target) +
                 kCodeSizeMultiplier / 2;
  int weight = Min(kMaxBackEdgeWeight, Max(1, distance / kCodeSizeMultiplier));
  EmitProfilingCounterDecrement(weight);
  {
    Assembler::BlockTrampolinePoolScope block_trampoline_pool(masm_);
    // BackEdgeTable::PatchAt manipulates this sequence.
    __ bge(&ok, Label::kNear);
    __ Call(isolate()->builtins()->InterruptCheck(), RelocInfo::CODE_TARGET);

    // Record a mapping of this PC offset to the OSR id.  This is used to find
    // the AST id from the unoptimized code in order to use it as a key into
    // the deoptimization input data found in the optimized code.
    RecordBackEdge(stmt->OsrEntryId());
  }
  EmitProfilingCounterReset();

  __ bind(&ok);
  PrepareForBailoutForId(stmt->EntryId(), NO_REGISTERS);
  // Record a mapping of the OSR id to this PC.  This is used if the OSR
  // entry becomes the target of a bailout.  We don't expect it to be, but
  // we want it to work if it is.
  PrepareForBailoutForId(stmt->OsrEntryId(), NO_REGISTERS);
}


void FullCodeGenerator::EmitReturnSequence() {
  Comment cmnt(masm_, "[ Return sequence");
  if (return_label_.is_bound()) {
    __ b(&return_label_);
  } else {
    __ bind(&return_label_);
    if (FLAG_trace) {
      // Push the return value on the stack as the parameter.
      // Runtime::TraceExit returns its parameter in r2
      __ push(r2);
      __ CallRuntime(Runtime::kTraceExit, 1);
    }
    // Pretend that the exit is a backwards jump to the entry.
    int weight = 1;
    if (info_->ShouldSelfOptimize()) {
      weight = FLAG_interrupt_budget / FLAG_self_opt_count;
    } else {
      int distance = masm_->pc_offset() + kCodeSizeMultiplier / 2;
      weight = Min(kMaxBackEdgeWeight, Max(1, distance / kCodeSizeMultiplier));
    }
    EmitProfilingCounterDecrement(weight);
    Label ok;
    __ CmpP(r5, Operand::Zero());
    __ bge(&ok, Label::kNear);
    __ push(r2);
    __ Call(isolate()->builtins()->InterruptCheck(), RelocInfo::CODE_TARGET);
    __ pop(r2);
    EmitProfilingCounterReset();
    __ bind(&ok);

#ifdef DEBUG
    // Add a label for checking the size of the code used for returning.
    Label check_exit_codesize;
    __ bind(&check_exit_codesize);
#endif
    // Make sure that the constant pool is not emitted inside of the return
    // sequence.
    {
      Assembler::BlockTrampolinePoolScope block_trampoline_pool(masm_);
      // Here we use masm_-> instead of the __ macro to avoid the code coverage
      // tool from instrumenting as we rely on the code size here.
      int32_t arg_count = info_->scope()->num_parameters() + 1;
      int32_t sp_delta = arg_count * kPointerSize;
      SetReturnPosition(function());
      __ RecordJSReturn();
      masm_->LoadRR(sp, fp);

      // Mark range where no frame exists, in case profiler receives sample here
      int32_t no_frame_start = masm_->pc_offset();
      masm_->LoadP(fp, MemOperand(sp));
      masm_->LoadP(r14, MemOperand(sp, kPointerSize));
      masm_->lay(sp,
                 MemOperand(sp, (uint32_t)(sp_delta + (2 * kPointerSize))));
      masm_->Ret();
      info_->AddNoFrameRange(no_frame_start, masm_->pc_offset());
    }

#ifdef DEBUG
    // Check that the size of the code used for returning is large enough
    // for the debugger's requirements.
    DCHECK(Assembler::kJSReturnSequenceLength <=
           masm_->SizeOfCodeGeneratedSince(&check_exit_codesize));
#endif
  }
}


void FullCodeGenerator::EffectContext::Plug(Variable* var) const {
  DCHECK(var->IsStackAllocated() || var->IsContextSlot());
}


void FullCodeGenerator::AccumulatorValueContext::Plug(Variable* var) const {
  DCHECK(var->IsStackAllocated() || var->IsContextSlot());
  codegen()->GetVar(result_register(), var);
}


void FullCodeGenerator::StackValueContext::Plug(Variable* var) const {
  DCHECK(var->IsStackAllocated() || var->IsContextSlot());
  codegen()->GetVar(result_register(), var);
  __ push(result_register());
}


void FullCodeGenerator::TestContext::Plug(Variable* var) const {
  DCHECK(var->IsStackAllocated() || var->IsContextSlot());
  // For simplicity we always test the accumulator register.
  codegen()->GetVar(result_register(), var);
  codegen()->PrepareForBailoutBeforeSplit(condition(), false, NULL, NULL);
  codegen()->DoTest(this);
}


void FullCodeGenerator::EffectContext::Plug(Heap::RootListIndex index) const {}


void FullCodeGenerator::AccumulatorValueContext::Plug(
    Heap::RootListIndex index) const {
  __ LoadRoot(result_register(), index);
}


void FullCodeGenerator::StackValueContext::Plug(
    Heap::RootListIndex index) const {
  __ LoadRoot(result_register(), index);
  __ push(result_register());
}


void FullCodeGenerator::TestContext::Plug(Heap::RootListIndex index) const {
  codegen()->PrepareForBailoutBeforeSplit(condition(), true, true_label_,
                                          false_label_);
  if (index == Heap::kUndefinedValueRootIndex ||
      index == Heap::kNullValueRootIndex ||
      index == Heap::kFalseValueRootIndex) {
    if (false_label_ != fall_through_) __ b(false_label_);
  } else if (index == Heap::kTrueValueRootIndex) {
    if (true_label_ != fall_through_) __ b(true_label_);
  } else {
    __ LoadRoot(result_register(), index);
    codegen()->DoTest(this);
  }
}


void FullCodeGenerator::EffectContext::Plug(Handle<Object> lit) const {}


void FullCodeGenerator::AccumulatorValueContext::Plug(
    Handle<Object> lit) const {
  __ mov(result_register(), Operand(lit));
}


void FullCodeGenerator::StackValueContext::Plug(Handle<Object> lit) const {
  // Immediates cannot be pushed directly.
  __ mov(result_register(), Operand(lit));
  __ push(result_register());
}


void FullCodeGenerator::TestContext::Plug(Handle<Object> lit) const {
  codegen()->PrepareForBailoutBeforeSplit(condition(), true, true_label_,
                                          false_label_);
  DCHECK(!lit->IsUndetectableObject());  // There are no undetectable literals.
  if (lit->IsUndefined() || lit->IsNull() || lit->IsFalse()) {
    if (false_label_ != fall_through_) __ b(false_label_);
  } else if (lit->IsTrue() || lit->IsJSObject()) {
    if (true_label_ != fall_through_) __ b(true_label_);
  } else if (lit->IsString()) {
    if (String::cast(*lit)->length() == 0) {
      if (false_label_ != fall_through_) __ b(false_label_);
    } else {
      if (true_label_ != fall_through_) __ b(true_label_);
    }
  } else if (lit->IsSmi()) {
    if (Smi::cast(*lit)->value() == 0) {
      if (false_label_ != fall_through_) __ b(false_label_);
    } else {
      if (true_label_ != fall_through_) __ b(true_label_);
    }
  } else {
    // For simplicity we always test the accumulator register.
    __ mov(result_register(), Operand(lit));
    codegen()->DoTest(this);
  }
}


void FullCodeGenerator::EffectContext::DropAndPlug(int count,
                                                   Register reg) const {
  DCHECK(count > 0);
  __ Drop(count);
}


void FullCodeGenerator::AccumulatorValueContext::DropAndPlug(
    int count, Register reg) const {
  DCHECK(count > 0);
  __ Drop(count);
  __ Move(result_register(), reg);
}


void FullCodeGenerator::StackValueContext::DropAndPlug(int count,
                                                       Register reg) const {
  DCHECK(count > 0);
  if (count > 1) __ Drop(count - 1);
  __ StoreP(reg, MemOperand(sp, 0));
}


void FullCodeGenerator::TestContext::DropAndPlug(int count,
                                                 Register reg) const {
  DCHECK(count > 0);
  // For simplicity we always test the accumulator register.
  __ Drop(count);
  __ Move(result_register(), reg);
  codegen()->PrepareForBailoutBeforeSplit(condition(), false, NULL, NULL);
  codegen()->DoTest(this);
}


void FullCodeGenerator::EffectContext::Plug(Label* materialize_true,
                                            Label* materialize_false) const {
  DCHECK(materialize_true == materialize_false);
  __ bind(materialize_true);
}


void FullCodeGenerator::AccumulatorValueContext::Plug(
    Label* materialize_true, Label* materialize_false) const {
  Label done;
  __ bind(materialize_true);
  __ LoadRoot(result_register(), Heap::kTrueValueRootIndex);
  __ b(&done, Label::kNear);
  __ bind(materialize_false);
  __ LoadRoot(result_register(), Heap::kFalseValueRootIndex);
  __ bind(&done);
}


void FullCodeGenerator::StackValueContext::Plug(
    Label* materialize_true, Label* materialize_false) const {
  Label done;
  __ bind(materialize_true);
  __ LoadRoot(ip, Heap::kTrueValueRootIndex);
  __ b(&done, Label::kNear);
  __ bind(materialize_false);
  __ LoadRoot(ip, Heap::kFalseValueRootIndex);
  __ bind(&done);
  __ push(ip);
}


void FullCodeGenerator::TestContext::Plug(Label* materialize_true,
                                          Label* materialize_false) const {
  DCHECK(materialize_true == true_label_);
  DCHECK(materialize_false == false_label_);
}


void FullCodeGenerator::EffectContext::Plug(bool flag) const {}


void FullCodeGenerator::AccumulatorValueContext::Plug(bool flag) const {
  Heap::RootListIndex value_root_index =
      flag ? Heap::kTrueValueRootIndex : Heap::kFalseValueRootIndex;
  __ LoadRoot(result_register(), value_root_index);
}


void FullCodeGenerator::StackValueContext::Plug(bool flag) const {
  Heap::RootListIndex value_root_index =
      flag ? Heap::kTrueValueRootIndex : Heap::kFalseValueRootIndex;
  __ LoadRoot(ip, value_root_index);
  __ push(ip);
}


void FullCodeGenerator::TestContext::Plug(bool flag) const {
  codegen()->PrepareForBailoutBeforeSplit(condition(), true, true_label_,
                                          false_label_);
  if (flag) {
    if (true_label_ != fall_through_) __ b(true_label_);
  } else {
    if (false_label_ != fall_through_) __ b(false_label_);
  }
}


void FullCodeGenerator::DoTest(Expression* condition, Label* if_true,
                               Label* if_false, Label* fall_through) {
  Handle<Code> ic = ToBooleanStub::GetUninitialized(isolate());
  CallIC(ic, condition->test_id());
  __ CmpP(result_register(), Operand::Zero());
  Split(ne, if_true, if_false, fall_through);
}


void FullCodeGenerator::Split(Condition cond, Label* if_true, Label* if_false,
                              Label* fall_through, CRegister cr) {
  if (if_false == fall_through) {
    __ b(cond, if_true /*, cr*/);
  } else if (if_true == fall_through) {
    __ b(NegateCondition(cond), if_false /*, cr*/);
  } else {
    __ b(cond, if_true /*, cr*/);
    __ b(if_false);
  }
}


MemOperand FullCodeGenerator::StackOperand(Variable* var) {
  DCHECK(var->IsStackAllocated());
  // Offset is negative because higher indexes are at lower addresses.
  int offset = -var->index() * kPointerSize;
  // Adjust by a (parameter or local) base offset.
  if (var->IsParameter()) {
    offset += (info_->scope()->num_parameters() + 1) * kPointerSize;
  } else {
    offset += JavaScriptFrameConstants::kLocal0Offset;
  }
  return MemOperand(fp, offset);
}


MemOperand FullCodeGenerator::VarOperand(Variable* var, Register scratch) {
  DCHECK(var->IsContextSlot() || var->IsStackAllocated());
  if (var->IsContextSlot()) {
    int context_chain_length = scope()->ContextChainLength(var->scope());
    __ LoadContext(scratch, context_chain_length);
    return ContextOperand(scratch, var->index());
  } else {
    return StackOperand(var);
  }
}


void FullCodeGenerator::GetVar(Register dest, Variable* var) {
  // Use destination as scratch.
  MemOperand location = VarOperand(var, dest);
  __ LoadP(dest, location, r0);
}


void FullCodeGenerator::SetVar(Variable* var, Register src, Register scratch0,
                               Register scratch1) {
  DCHECK(var->IsContextSlot() || var->IsStackAllocated());
  DCHECK(!scratch0.is(src));
  DCHECK(!scratch0.is(scratch1));
  DCHECK(!scratch1.is(src));
  MemOperand location = VarOperand(var, scratch0);
  __ StoreP(src, location);

  // Emit the write barrier code if the location is in the heap.
  if (var->IsContextSlot()) {
    __ RecordWriteContextSlot(scratch0, location.offset(), src, scratch1,
                              kLRHasBeenSaved, kDontSaveFPRegs);
  }
}


void FullCodeGenerator::PrepareForBailoutBeforeSplit(Expression* expr,
                                                     bool should_normalize,
                                                     Label* if_true,
                                                     Label* if_false) {
  // Only prepare for bailouts before splits if we're in a test
  // context. Otherwise, we let the Visit function deal with the
  // preparation to avoid preparing with the same AST id twice.
  if (!context()->IsTest() || !info_->IsOptimizable()) return;

  Label skip;
  if (should_normalize) __ b(&skip);
  PrepareForBailout(expr, TOS_REG);
  if (should_normalize) {
    __ CompareRoot(r2, Heap::kTrueValueRootIndex);
    Split(eq, if_true, if_false, NULL);
    __ bind(&skip);
  }
}


void FullCodeGenerator::EmitDebugCheckDeclarationContext(Variable* variable) {
  // The variable in the declaration always resides in the current function
  // context.
  DCHECK_EQ(0, scope()->ContextChainLength(variable->scope()));
  if (generate_debug_code_) {
    // Check that we're not inside a with or catch context.
    __ LoadP(r3, FieldMemOperand(cp, HeapObject::kMapOffset));
    __ CompareRoot(r3, Heap::kWithContextMapRootIndex);
    __ Check(ne, kDeclarationInWithContext);
    __ CompareRoot(r3, Heap::kCatchContextMapRootIndex);
    __ Check(ne, kDeclarationInCatchContext);
  }
}


void FullCodeGenerator::VisitVariableDeclaration(
    VariableDeclaration* declaration) {
  // If it was not possible to allocate the variable at compile time, we
  // need to "declare" it at runtime to make sure it actually exists in the
  // local context.
  VariableProxy* proxy = declaration->proxy();
  VariableMode mode = declaration->mode();
  Variable* variable = proxy->var();
  bool hole_init = mode == LET || mode == CONST || mode == CONST_LEGACY;
  switch (variable->location()) {
    case VariableLocation::GLOBAL:
    case VariableLocation::UNALLOCATED:
      globals_->Add(variable->name(), zone());
      globals_->Add(variable->binding_needs_init()
                        ? isolate()->factory()->the_hole_value()
                        : isolate()->factory()->undefined_value(),
                    zone());
      break;

    case VariableLocation::PARAMETER:
    case VariableLocation::LOCAL:
      if (hole_init) {
        Comment cmnt(masm_, "[ VariableDeclaration");
        __ LoadRoot(ip, Heap::kTheHoleValueRootIndex);
        __ StoreP(ip, StackOperand(variable));
      }
      break;

    case VariableLocation::CONTEXT:
      if (hole_init) {
        Comment cmnt(masm_, "[ VariableDeclaration");
        EmitDebugCheckDeclarationContext(variable);
        __ LoadRoot(ip, Heap::kTheHoleValueRootIndex);
        __ StoreP(ip, ContextOperand(cp, variable->index()));
        // No write barrier since the_hole_value is in old space.
        PrepareForBailoutForId(proxy->id(), NO_REGISTERS);
      }
      break;

    case VariableLocation::LOOKUP: {
      Comment cmnt(masm_, "[ VariableDeclaration");
      __ mov(r4, Operand(variable->name()));
      // Declaration nodes are always introduced in one of four modes.
      DCHECK(IsDeclaredVariableMode(mode));
      PropertyAttributes attr =
          IsImmutableVariableMode(mode) ? READ_ONLY : NONE;
      __ LoadSmiLiteral(r3, Smi::FromInt(attr));
      // Push initial value, if any.
      // Note: For variables we must not push an initial value (such as
      // 'undefined') because we may have a (legal) redeclaration and we
      // must not destroy the current value.
      if (hole_init) {
        __ LoadRoot(r2, Heap::kTheHoleValueRootIndex);
        __ Push(cp, r4, r3, r2);
      } else {
        __ LoadSmiLiteral(r2, Smi::FromInt(0));  // Indicate no initial value.
        __ Push(cp, r4, r3, r2);
      }
      __ CallRuntime(Runtime::kDeclareLookupSlot, 4);
      break;
    }
  }
}


void FullCodeGenerator::VisitFunctionDeclaration(
    FunctionDeclaration* declaration) {
  VariableProxy* proxy = declaration->proxy();
  Variable* variable = proxy->var();
  switch (variable->location()) {
    case VariableLocation::GLOBAL:
    case VariableLocation::UNALLOCATED: {
      globals_->Add(variable->name(), zone());
      Handle<SharedFunctionInfo> function =
          Compiler::GetSharedFunctionInfo(declaration->fun(), script(), info_);
      // Check for stack-overflow exception.
      if (function.is_null()) return SetStackOverflow();
      globals_->Add(function, zone());
      break;
    }

    case VariableLocation::PARAMETER:
    case VariableLocation::LOCAL: {
      Comment cmnt(masm_, "[ FunctionDeclaration");
      VisitForAccumulatorValue(declaration->fun());
      __ StoreP(result_register(), StackOperand(variable));
      break;
    }

    case VariableLocation::CONTEXT: {
      Comment cmnt(masm_, "[ FunctionDeclaration");
      EmitDebugCheckDeclarationContext(variable);
      VisitForAccumulatorValue(declaration->fun());
      __ StoreP(result_register(), ContextOperand(cp, variable->index()));
      int offset = Context::SlotOffset(variable->index());
      // We know that we have written a function, which is not a smi.
      __ RecordWriteContextSlot(cp, offset, result_register(), r4,
                                kLRHasBeenSaved, kDontSaveFPRegs,
                                EMIT_REMEMBERED_SET, OMIT_SMI_CHECK);
      PrepareForBailoutForId(proxy->id(), NO_REGISTERS);
      break;
    }

    case VariableLocation::LOOKUP: {
      Comment cmnt(masm_, "[ FunctionDeclaration");
      __ mov(r4, Operand(variable->name()));
      __ LoadSmiLiteral(r3, Smi::FromInt(NONE));
      __ Push(cp, r4, r3);
      // Push initial value for function declaration.
      VisitForStackValue(declaration->fun());
      __ CallRuntime(Runtime::kDeclareLookupSlot, 4);
      break;
    }
  }
}


void FullCodeGenerator::VisitImportDeclaration(ImportDeclaration* declaration) {
  VariableProxy* proxy = declaration->proxy();
  Variable* variable = proxy->var();
  switch (variable->location()) {
    case VariableLocation::GLOBAL:
    case VariableLocation::UNALLOCATED:
      // TODO(rossberg)
      break;

    case VariableLocation::CONTEXT: {
      Comment cmnt(masm_, "[ ImportDeclaration");
      EmitDebugCheckDeclarationContext(variable);
      // TODO(rossberg)
      break;
    }

    case VariableLocation::PARAMETER:
    case VariableLocation::LOCAL:
    case VariableLocation::LOOKUP:
      UNREACHABLE();
  }
}


void FullCodeGenerator::VisitExportDeclaration(ExportDeclaration* declaration) {
  // TODO(rossberg)
}


void FullCodeGenerator::DeclareGlobals(Handle<FixedArray> pairs) {
  // Call the runtime to declare the globals.
  // The context is the first argument.
  __ mov(r3, Operand(pairs));
  __ LoadSmiLiteral(r2, Smi::FromInt(DeclareGlobalsFlags()));
  __ Push(cp, r3, r2);
  __ CallRuntime(Runtime::kDeclareGlobals, 3);
  // Return value is ignored.
}


void FullCodeGenerator::DeclareModules(Handle<FixedArray> descriptions) {
  // Call the runtime to declare the modules.
  __ Push(descriptions);
  __ CallRuntime(Runtime::kDeclareModules, 1);
  // Return value is ignored.
}


void FullCodeGenerator::VisitSwitchStatement(SwitchStatement* stmt) {
  Comment cmnt(masm_, "[ SwitchStatement");
  Breakable nested_statement(this, stmt);
  SetStatementPosition(stmt);

  // Keep the switch value on the stack until a case matches.
  VisitForStackValue(stmt->tag());
  PrepareForBailoutForId(stmt->EntryId(), NO_REGISTERS);

  ZoneList<CaseClause*>* clauses = stmt->cases();
  CaseClause* default_clause = NULL;  // Can occur anywhere in the list.

  Label next_test;  // Recycled for each test.
  // Compile all the tests with branches to their bodies.
  for (int i = 0; i < clauses->length(); i++) {
    CaseClause* clause = clauses->at(i);
    clause->body_target()->Unuse();

    // The default is not a test, but remember it as final fall through.
    if (clause->is_default()) {
      default_clause = clause;
      continue;
    }

    Comment cmnt(masm_, "[ Case comparison");
    __ bind(&next_test);
    next_test.Unuse();

    // Compile the label expression.
    VisitForAccumulatorValue(clause->label());

    // Perform the comparison as if via '==='.
    __ LoadP(r3, MemOperand(sp, 0));  // Switch value.
    bool inline_smi_code = ShouldInlineSmiCase(Token::EQ_STRICT);
    JumpPatchSite patch_site(masm_);
    if (inline_smi_code) {
      Label slow_case;
      __ LoadRR(r4, r2);
      __ OrP(r4, r3);
      patch_site.EmitJumpIfNotSmi(r4, &slow_case);

      __ CmpP(r3, r2);
      __ bne(&next_test);
      __ Drop(1);  // Switch value is no longer needed.
      __ b(clause->body_target());
      __ bind(&slow_case);
    }

    // Record position before stub call for type feedback.
    SetExpressionPosition(clause);
    Handle<Code> ic = CodeFactory::CompareIC(isolate(), Token::EQ_STRICT,
                                             strength(language_mode())).code();
    CallIC(ic, clause->CompareId());
    patch_site.EmitPatchInfo();

    Label skip;
    __ b(&skip);
    PrepareForBailout(clause, TOS_REG);
    __ CompareRoot(r2, Heap::kTrueValueRootIndex);
    __ bne(&next_test);
    __ Drop(1);
    __ b(clause->body_target());
    __ bind(&skip);

    __ CmpP(r2, Operand::Zero());
    __ bne(&next_test);
    __ Drop(1);  // Switch value is no longer needed.
    __ b(clause->body_target());
  }

  // Discard the test value and jump to the default if present, otherwise to
  // the end of the statement.
  __ bind(&next_test);
  __ Drop(1);  // Switch value is no longer needed.
  if (default_clause == NULL) {
    __ b(nested_statement.break_label());
  } else {
    __ b(default_clause->body_target());
  }

  // Compile all the case bodies.
  for (int i = 0; i < clauses->length(); i++) {
    Comment cmnt(masm_, "[ Case body");
    CaseClause* clause = clauses->at(i);
    __ bind(clause->body_target());
    PrepareForBailoutForId(clause->EntryId(), NO_REGISTERS);
    VisitStatements(clause->statements());
  }

  __ bind(nested_statement.break_label());
  PrepareForBailoutForId(stmt->ExitId(), NO_REGISTERS);
}


void FullCodeGenerator::VisitForInStatement(ForInStatement* stmt) {
  Comment cmnt(masm_, "[ ForInStatement");
  SetStatementPosition(stmt, SKIP_BREAK);

  FeedbackVectorSlot slot = stmt->ForInFeedbackSlot();

  Label loop, exit;
  ForIn loop_statement(this, stmt);
  increment_loop_depth();

  // Get the object to enumerate over. Both SpiderMonkey and JSC
  // ignore null and undefined in contrast to the specification; see
  // ECMA-262 section 12.6.4.
  // Get the object to enumerate over. If the object is null or undefined, skip
  // over the loop.  See ECMA-262 version 5, section 12.6.4.
  SetExpressionAsStatementPosition(stmt->enumerable());
  VisitForAccumulatorValue(stmt->enumerable());
  __ CompareRoot(r2, Heap::kUndefinedValueRootIndex);
  __ beq(&exit);
  Register null_value = r6;
  __ LoadRoot(null_value, Heap::kNullValueRootIndex);
  __ CmpP(r2, null_value);
  __ beq(&exit);

  PrepareForBailoutForId(stmt->PrepareId(), TOS_REG);

  // Convert the object to a JS object.
  Label convert, done_convert;
  __ JumpIfSmi(r2, &convert);
  __ CompareObjectType(r2, r3, r3, FIRST_SPEC_OBJECT_TYPE);
  __ bge(&done_convert);
  __ bind(&convert);
  __ push(r2);
  __ InvokeBuiltin(Builtins::TO_OBJECT, CALL_FUNCTION);
  __ bind(&done_convert);
  PrepareForBailoutForId(stmt->ToObjectId(), TOS_REG);
  __ push(r2);

  // Check for proxies.
  Label call_runtime;
  STATIC_ASSERT(FIRST_JS_PROXY_TYPE == FIRST_SPEC_OBJECT_TYPE);
  __ CompareObjectType(r2, r3, r3, LAST_JS_PROXY_TYPE);
  __ ble(&call_runtime);

  // Check cache validity in generated code. This is a fast case for
  // the JSObject::IsSimpleEnum cache validity checks. If we cannot
  // guarantee cache validity, call the runtime system to check cache
  // validity or get the property names in a fixed array.
  __ CheckEnumCache(null_value, &call_runtime);

  // The enum cache is valid.  Load the map of the object being
  // iterated over and use the cache for the iteration.
  Label use_cache;
  __ LoadP(r2, FieldMemOperand(r2, HeapObject::kMapOffset));
  __ b(&use_cache);

  // Get the set of properties to enumerate.
  __ bind(&call_runtime);
  __ push(r2);  // Duplicate the enumerable object on the stack.
  __ CallRuntime(Runtime::kGetPropertyNamesFast, 1);
  PrepareForBailoutForId(stmt->EnumId(), TOS_REG);

  // If we got a map from the runtime call, we can do a fast
  // modification check. Otherwise, we got a fixed array, and we have
  // to do a slow check.
  Label fixed_array;
  __ LoadP(r4, FieldMemOperand(r2, HeapObject::kMapOffset));
  __ CompareRoot(r4, Heap::kMetaMapRootIndex);
  __ bne(&fixed_array);

  // We got a map in register r2. Get the enumeration cache from it.
  Label no_descriptors;
  __ bind(&use_cache);

  __ EnumLength(r3, r2);
  __ CmpSmiLiteral(r3, Smi::FromInt(0), r0);
  __ beq(&no_descriptors, Label::kNear);

  __ LoadInstanceDescriptors(r2, r4);
  __ LoadP(r4, FieldMemOperand(r4, DescriptorArray::kEnumCacheOffset));
  __ LoadP(r4,
           FieldMemOperand(r4, DescriptorArray::kEnumCacheBridgeCacheOffset));

  // Set up the four remaining stack slots.
  __ push(r2);  // Map.
  __ LoadSmiLiteral(r2, Smi::FromInt(0));
  // Push enumeration cache, enumeration cache length (as smi) and zero.
  __ Push(r4, r3, r2);
  __ b(&loop);

  __ bind(&no_descriptors);
  __ Drop(1);
  __ b(&exit);

  // We got a fixed array in register r2. Iterate through that.
  Label non_proxy;
  __ bind(&fixed_array);

  __ Move(r3, FeedbackVector());
  __ mov(r4, Operand(TypeFeedbackVector::MegamorphicSentinel(isolate())));
  int vector_index = FeedbackVector()->GetIndex(slot);
  __ StoreP(r4, FieldMemOperand(r3,
        FixedArray::OffsetOfElementAt(vector_index)));

  __ LoadSmiLiteral(r3, Smi::FromInt(1));  // Smi indicates slow check
  __ LoadP(r4, MemOperand(sp, 0 * kPointerSize));  // Get enumerated object
  STATIC_ASSERT(FIRST_JS_PROXY_TYPE == FIRST_SPEC_OBJECT_TYPE);
  __ CompareObjectType(r4, r5, r5, LAST_JS_PROXY_TYPE);
  __ bgt(&non_proxy, Label::kNear);
  __ LoadSmiLiteral(r3, Smi::FromInt(0));  // Zero indicates proxy
  __ bind(&non_proxy);
  __ Push(r3, r2);  // Smi and array
  __ LoadP(r3, FieldMemOperand(r2, FixedArray::kLengthOffset));
  __ LoadSmiLiteral(r2, Smi::FromInt(0));
  __ Push(r3, r2);  // Fixed array length (as smi) and initial index.

  // Generate code for doing the condition check.
  PrepareForBailoutForId(stmt->BodyId(), NO_REGISTERS);
  __ bind(&loop);
  SetExpressionAsStatementPosition(stmt->each());

  // Load the current count to r2, load the length to r3.
  __ LoadP(r2, MemOperand(sp, 0 * kPointerSize));
  __ LoadP(r3, MemOperand(sp, 1 * kPointerSize));
  __ CmpLogicalP(r2, r3);  // Compare to the array length.
  __ bge(loop_statement.break_label());

  // Get the current entry of the array into register r5.
  __ LoadP(r4, MemOperand(sp, 2 * kPointerSize));
  __ AddP(r4, Operand(FixedArray::kHeaderSize - kHeapObjectTag));
  __ SmiToPtrArrayOffset(r5, r2);
  __ LoadP(r5, MemOperand(r5, r4));

  // Get the expected map from the stack or a smi in the
  // permanent slow case into register r4.
  __ LoadP(r4, MemOperand(sp, 3 * kPointerSize));

  // Check if the expected map still matches that of the enumerable.
  // If not, we may have to filter the key.
  Label update_each;
  __ LoadP(r3, MemOperand(sp, 4 * kPointerSize));
  __ LoadP(r6, FieldMemOperand(r3, HeapObject::kMapOffset));
  __ CmpP(r6, r4);
  __ beq(&update_each);

  // For proxies, no filtering is done.
  // TODO(rossberg): What if only a prototype is a proxy? Not specified yet.
  __ CmpSmiLiteral(r4, Smi::FromInt(0), r0);
  __ beq(&update_each);

  // Convert the entry to a string or (smi) 0 if it isn't a property
  // any more. If the property has been removed while iterating, we
  // just skip it.
  __ Push(r3, r5);  // Enumerable and current entry.
  __ CallRuntime(Runtime::kForInFilter, 2);
  PrepareForBailoutForId(stmt->FilterId(), TOS_REG);
  __ LoadRR(r5, r2);
  __ LoadRoot(r0, Heap::kUndefinedValueRootIndex);
  __ CmpP(r2, r0);
  __ beq(loop_statement.continue_label());

  // Update the 'each' property or variable from the possibly filtered
  // entry in register r5.
  __ bind(&update_each);
  __ LoadRR(result_register(), r5);
  // Perform the assignment as if via '='.
  {
    EffectContext context(this);
    EmitAssignment(stmt->each(), stmt->EachFeedbackSlot());
    PrepareForBailoutForId(stmt->AssignmentId(), NO_REGISTERS);
  }

  // Generate code for the body of the loop.
  Visit(stmt->body());

  // Generate code for the going to the next element by incrementing
  // the index (smi) stored on top of the stack.
  __ bind(loop_statement.continue_label());
  __ pop(r2);
  __ AddSmiLiteral(r2, r2, Smi::FromInt(1), r0);
  __ push(r2);

  EmitBackEdgeBookkeeping(stmt, &loop);
  __ b(&loop);

  // Remove the pointers stored on the stack.
  __ bind(loop_statement.break_label());
  __ Drop(5);

  // Exit and decrement the loop depth.
  PrepareForBailoutForId(stmt->ExitId(), NO_REGISTERS);
  __ bind(&exit);
  decrement_loop_depth();
}


void FullCodeGenerator::EmitNewClosure(Handle<SharedFunctionInfo> info,
                                       bool pretenure) {
  // Use the fast case closure allocation code that allocates in new
  // space for nested functions that don't need literals cloning. If
  // we're running with the --always-opt or the --prepare-always-opt
  // flag, we need to use the runtime function so that the new function
  // we are creating here gets a chance to have its code optimized and
  // doesn't just get a copy of the existing unoptimized code.
  if (!FLAG_always_opt && !FLAG_prepare_always_opt && !pretenure &&
      scope()->is_function_scope() && info->num_literals() == 0) {
    FastNewClosureStub stub(isolate(), info->language_mode(), info->kind());
    __ mov(r4, Operand(info));
    __ CallStub(&stub);
  } else {
    __ mov(r2, Operand(info));
    __ LoadRoot(
        r3, pretenure ? Heap::kTrueValueRootIndex : Heap::kFalseValueRootIndex);
    __ Push(cp, r2, r3);
    __ CallRuntime(Runtime::kNewClosure, 3);
  }
  context()->Plug(r2);
}


void FullCodeGenerator::VisitVariableProxy(VariableProxy* expr) {
  Comment cmnt(masm_, "[ VariableProxy");
  EmitVariableLoad(expr);
}


void FullCodeGenerator::EmitSetHomeObjectIfNeeded(Expression* initializer,
                                                  int offset,
                                                  FeedbackVectorICSlot slot) {
  if (NeedsHomeObject(initializer)) {
    __ LoadP(StoreDescriptor::ReceiverRegister(), MemOperand(sp));
    __ mov(StoreDescriptor::NameRegister(),
           Operand(isolate()->factory()->home_object_symbol()));
    __ LoadP(StoreDescriptor::ValueRegister(),
             MemOperand(sp, offset * kPointerSize));
    if (FLAG_vector_stores) EmitLoadStoreICSlot(slot);
    CallStoreIC();
  }
}


void FullCodeGenerator::EmitLoadGlobalCheckExtensions(VariableProxy* proxy,
                                                      TypeofState typeof_state,
                                                      Label* slow) {
  Register current = cp;
  Register next = r3;
  Register temp = r4;

  Scope* s = scope();
  while (s != NULL) {
    if (s->num_heap_slots() > 0) {
      if (s->calls_sloppy_eval()) {
        // Check that extension is NULL.
        __ LoadP(temp, ContextOperand(current, Context::EXTENSION_INDEX));
        __ CmpP(temp, Operand::Zero());
        __ bne(slow);
      }
      // Load next context in chain.
      __ LoadP(next, ContextOperand(current, Context::PREVIOUS_INDEX));
      // Walk the rest of the chain without clobbering cp.
      current = next;
    }
    // If no outer scope calls eval, we do not need to check more
    // context extensions.
    if (!s->outer_scope_calls_sloppy_eval() || s->is_eval_scope()) break;
    s = s->outer_scope();
  }

  if (s->is_eval_scope()) {
    Label loop, fast;
    if (!current.is(next)) {
      __ Move(next, current);
    }
    __ bind(&loop);
    // Terminate at native context.
    __ LoadP(temp, FieldMemOperand(next, HeapObject::kMapOffset));
    __ CompareRoot(temp, Heap::kNativeContextMapRootIndex);
    __ beq(&fast, Label::kNear);
    // Check that extension is NULL.
    __ LoadP(temp, ContextOperand(next, Context::EXTENSION_INDEX));
    __ CmpP(temp, Operand::Zero());
    __ bne(slow);
    // Load next context in chain.
    __ LoadP(next, ContextOperand(next, Context::PREVIOUS_INDEX));
    __ b(&loop);
    __ bind(&fast);
  }

  // All extension objects were empty and it is safe to use a normal global
  // load machinery.
  EmitGlobalVariableLoad(proxy, typeof_state);
}


MemOperand FullCodeGenerator::ContextSlotOperandCheckExtensions(Variable* var,
                                                                Label* slow) {
  DCHECK(var->IsContextSlot());
  Register context = cp;
  Register next = r5;
  Register temp = r6;

  for (Scope* s = scope(); s != var->scope(); s = s->outer_scope()) {
    if (s->num_heap_slots() > 0) {
      if (s->calls_sloppy_eval()) {
        // Check that extension is NULL.
        __ LoadP(temp, ContextOperand(context, Context::EXTENSION_INDEX));
        __ CmpP(temp, Operand::Zero());
        __ bne(slow);
      }
      __ LoadP(next, ContextOperand(context, Context::PREVIOUS_INDEX));
      // Walk the rest of the chain without clobbering cp.
      context = next;
    }
  }
  // Check that last extension is NULL.
  __ LoadP(temp, ContextOperand(context, Context::EXTENSION_INDEX));
  __ CmpP(temp, Operand::Zero());
  __ bne(slow);

  // This function is used only for loads, not stores, so it's safe to
  // return an cp-based operand (the write barrier cannot be allowed to
  // destroy the cp register).
  return ContextOperand(context, var->index());
}


void FullCodeGenerator::EmitDynamicLookupFastCase(VariableProxy* proxy,
                                                  TypeofState typeof_state,
                                                  Label* slow, Label* done) {
  // Generate fast-case code for variables that might be shadowed by
  // eval-introduced variables.  Eval is used a lot without
  // introducing variables.  In those cases, we do not want to
  // perform a runtime call for all variables in the scope
  // containing the eval.
  Variable* var = proxy->var();
  if (var->mode() == DYNAMIC_GLOBAL) {
    EmitLoadGlobalCheckExtensions(proxy, typeof_state, slow);
    __ b(done);
  } else if (var->mode() == DYNAMIC_LOCAL) {
    Variable* local = var->local_if_not_shadowed();
    __ LoadP(r2, ContextSlotOperandCheckExtensions(local, slow));
    if (local->mode() == LET || local->mode() == CONST ||
        local->mode() == CONST_LEGACY) {
      __ CompareRoot(r2, Heap::kTheHoleValueRootIndex);
      __ bne(done);
      if (local->mode() == CONST_LEGACY) {
        __ LoadRoot(r2, Heap::kUndefinedValueRootIndex);
      } else {  // LET || CONST
        __ mov(r2, Operand(var->name()));
        __ push(r2);
        __ CallRuntime(Runtime::kThrowReferenceError, 1);
      }
    }
    __ b(done);
  }
}


void FullCodeGenerator::EmitGlobalVariableLoad(VariableProxy* proxy,
                                               TypeofState typeof_state) {
  Variable* var = proxy->var();
  DCHECK(var->IsUnallocatedOrGlobalSlot() ||
         (var->IsLookupSlot() && var->mode() == DYNAMIC_GLOBAL));
  __ LoadP(LoadDescriptor::ReceiverRegister(), GlobalObjectOperand());
  __ mov(LoadDescriptor::NameRegister(), Operand(var->name()));
  __ mov(LoadDescriptor::SlotRegister(),
         Operand(SmiFromSlot(proxy->VariableFeedbackSlot())));
  // Inside typeof use a regular load, not a contextual load, to avoid
  // a reference error.
  CallLoadIC(typeof_state == NOT_INSIDE_TYPEOF ? CONTEXTUAL : NOT_CONTEXTUAL);
}


void FullCodeGenerator::EmitVariableLoad(VariableProxy* proxy,
                                         TypeofState typeof_state) {
  // Record position before possible IC call.
  SetExpressionPosition(proxy);
  PrepareForBailoutForId(proxy->BeforeId(), NO_REGISTERS);
  Variable* var = proxy->var();

  // Three cases: global variables, lookup variables, and all other types of
  // variables.
  switch (var->location()) {
    case VariableLocation::GLOBAL:
    case VariableLocation::UNALLOCATED: {
      Comment cmnt(masm_, "[ Global variable");
      EmitGlobalVariableLoad(proxy, typeof_state);
      context()->Plug(r2);
      break;
    }

    case VariableLocation::PARAMETER:
    case VariableLocation::LOCAL:
    case VariableLocation::CONTEXT: {
      DCHECK_EQ(NOT_INSIDE_TYPEOF, typeof_state);
      Comment cmnt(masm_, var->IsContextSlot() ? "[ Context variable"
                                               : "[ Stack variable");
      if (var->binding_needs_init()) {
        // var->scope() may be NULL when the proxy is located in eval code and
        // refers to a potential outside binding. Currently those bindings are
        // always looked up dynamically, i.e. in that case
        //     var->location() == LOOKUP.
        // always holds.
        DCHECK(var->scope() != NULL);

        // Check if the binding really needs an initialization check. The check
        // can be skipped in the following situation: we have a LET or CONST
        // binding in harmony mode, both the Variable and the VariableProxy have
        // the same declaration scope (i.e. they are both in global code, in the
        // same function or in the same eval code) and the VariableProxy is in
        // the source physically located after the initializer of the variable.
        //
        // We cannot skip any initialization checks for CONST in non-harmony
        // mode because const variables may be declared but never initialized:
        //   if (false) { const x; }; var y = x;
        //
        // The condition on the declaration scopes is a conservative check for
        // nested functions that access a binding and are called before the
        // binding is initialized:
        //   function() { f(); let x = 1; function f() { x = 2; } }
        //
        bool skip_init_check;
        if (var->scope()->DeclarationScope() != scope()->DeclarationScope()) {
          skip_init_check = false;
        } else if (var->is_this()) {
          CHECK(info_->function() != nullptr &&
                (info_->function()->kind() & kSubclassConstructor) != 0);
          // TODO(dslomov): implement 'this' hole check elimination.
          skip_init_check = false;
        } else {
          // Check that we always have valid source position.
          DCHECK(var->initializer_position() != RelocInfo::kNoPosition);
          DCHECK(proxy->position() != RelocInfo::kNoPosition);
          skip_init_check = var->mode() != CONST_LEGACY &&
                            var->initializer_position() < proxy->position();
        }

        if (!skip_init_check) {
          Label done;
          // Let and const need a read barrier.
          GetVar(r2, var);
          __ CompareRoot(r2, Heap::kTheHoleValueRootIndex);
          __ bne(&done);
          if (var->mode() == LET || var->mode() == CONST) {
            // Throw a reference error when using an uninitialized let/const
            // binding in harmony mode.
            __ mov(r2, Operand(var->name()));
            __ push(r2);
            __ CallRuntime(Runtime::kThrowReferenceError, 1);
          } else {
            // Uninitalized const bindings outside of harmony mode are unholed.
            DCHECK(var->mode() == CONST_LEGACY);
            __ LoadRoot(r2, Heap::kUndefinedValueRootIndex);
          }
          __ bind(&done);
          context()->Plug(r2);
          break;
        }
      }
      context()->Plug(var);
      break;
    }

    case VariableLocation::LOOKUP: {
      Comment cmnt(masm_, "[ Lookup variable");
      Label done, slow;
      // Generate code for loading from variables potentially shadowed
      // by eval-introduced variables.
      EmitDynamicLookupFastCase(proxy, typeof_state, &slow, &done);
      __ bind(&slow);
      __ mov(r3, Operand(var->name()));
      __ Push(cp, r3);  // Context and name.
      Runtime::FunctionId function_id =
          typeof_state == NOT_INSIDE_TYPEOF
              ? Runtime::kLoadLookupSlot
              : Runtime::kLoadLookupSlotNoReferenceError;
      __ CallRuntime(function_id, 2);
      __ bind(&done);
      context()->Plug(r2);
    }
  }
}


void FullCodeGenerator::VisitRegExpLiteral(RegExpLiteral* expr) {
  Comment cmnt(masm_, "[ RegExpLiteral");
  Label materialized;
  // Registers will be used as follows:
  // r7 = materialized value (RegExp literal)
  // r6 = JS function, literals array
  // r5 = literal index
  // r4 = RegExp pattern
  // r3 = RegExp flags
  // r2 = RegExp literal clone
  __ LoadP(r2, MemOperand(fp, JavaScriptFrameConstants::kFunctionOffset));
  __ LoadP(r6, FieldMemOperand(r2, JSFunction::kLiteralsOffset));
  int literal_offset =
      FixedArray::kHeaderSize + expr->literal_index() * kPointerSize;
  __ LoadP(r7, FieldMemOperand(r6, literal_offset), r0);
  __ CompareRoot(r7, Heap::kUndefinedValueRootIndex);
  __ bne(&materialized);

  // Create regexp literal using runtime function.
  // Result will be in r2.
  __ LoadSmiLiteral(r5, Smi::FromInt(expr->literal_index()));
  __ mov(r4, Operand(expr->pattern()));
  __ mov(r3, Operand(expr->flags()));
  __ Push(r6, r5, r4, r3);
  __ CallRuntime(Runtime::kMaterializeRegExpLiteral, 4);
  __ LoadRR(r7, r2);

  __ bind(&materialized);
  int size = JSRegExp::kSize + JSRegExp::kInObjectFieldCount * kPointerSize;
  Label allocated, runtime_allocate;
  __ Allocate(size, r2, r4, r5, &runtime_allocate, TAG_OBJECT);
  __ b(&allocated);

  __ bind(&runtime_allocate);
  __ LoadSmiLiteral(r2, Smi::FromInt(size));
  __ Push(r7, r2);
  __ CallRuntime(Runtime::kAllocateInNewSpace, 1);
  __ pop(r7);

  __ bind(&allocated);
  // After this, registers are used as follows:
  // r2: Newly allocated regexp.
  // r7: Materialized regexp.
  // r4: temp.
  __ CopyFields(r2, r7, r4.bit(), size / kPointerSize);
  context()->Plug(r2);
}


void FullCodeGenerator::EmitAccessor(Expression* expression) {
  if (expression == NULL) {
    __ LoadRoot(r3, Heap::kNullValueRootIndex);
    __ push(r3);
  } else {
    VisitForStackValue(expression);
  }
}


void FullCodeGenerator::VisitObjectLiteral(ObjectLiteral* expr) {
  Comment cmnt(masm_, "[ ObjectLiteral");

  Handle<FixedArray> constant_properties = expr->constant_properties();
  __ LoadP(r5, MemOperand(fp,  JavaScriptFrameConstants::kFunctionOffset));
  __ LoadP(r5, FieldMemOperand(r5, JSFunction::kLiteralsOffset));
  __ LoadSmiLiteral(r4, Smi::FromInt(expr->literal_index()));
  __ mov(r3, Operand(constant_properties));
  int flags = expr->ComputeFlags();
  __ LoadSmiLiteral(r2, Smi::FromInt(flags));
  if (MustCreateObjectLiteralWithRuntime(expr)) {
    __ Push(r5, r4, r3, r2);
    __ CallRuntime(Runtime::kCreateObjectLiteral, 4);
  } else {
    FastCloneShallowObjectStub stub(isolate(), expr->properties_count());
    __ CallStub(&stub);
  }
  PrepareForBailoutForId(expr->CreateLiteralId(), TOS_REG);

  // If result_saved is true the result is on top of the stack.  If
  // result_saved is false the result is in r2.
  bool result_saved = false;

  AccessorTable accessor_table(zone());
  int property_index = 0;
  // store_slot_index points to the vector IC slot for the next store IC used.
  // ObjectLiteral::ComputeFeedbackRequirements controls the allocation of slots
  // and must be updated if the number of store ICs emitted here changes.
  int store_slot_index = 0;
  for (; property_index < expr->properties()->length(); property_index++) {
    ObjectLiteral::Property* property = expr->properties()->at(property_index);
    if (property->is_computed_name()) break;
    if (property->IsCompileTimeValue()) continue;

    Literal* key = property->key()->AsLiteral();
    Expression* value = property->value();
    if (!result_saved) {
      __ push(r2);  // Save result on stack
      result_saved = true;
    }
    switch (property->kind()) {
      case ObjectLiteral::Property::CONSTANT:
        UNREACHABLE();
      case ObjectLiteral::Property::MATERIALIZED_LITERAL:
        DCHECK(!CompileTimeValue::IsCompileTimeValue(property->value()));
      // Fall through.
      case ObjectLiteral::Property::COMPUTED:
        // It is safe to use [[Put]] here because the boilerplate already
        // contains computed properties with an uninitialized value.
        if (key->value()->IsInternalizedString()) {
          if (property->emit_store()) {
            VisitForAccumulatorValue(value);
            DCHECK(StoreDescriptor::ValueRegister().is(r2));
            __ mov(StoreDescriptor::NameRegister(), Operand(key->value()));
            __ LoadP(StoreDescriptor::ReceiverRegister(), MemOperand(sp));
            if (FLAG_vector_stores) {
              EmitLoadStoreICSlot(expr->GetNthSlot(store_slot_index++));
              CallStoreIC();
            } else {
              CallStoreIC(key->LiteralFeedbackId());
            }
            PrepareForBailoutForId(key->id(), NO_REGISTERS);

            if (NeedsHomeObject(value)) {
              __ Move(StoreDescriptor::ReceiverRegister(), r2);
              __ mov(StoreDescriptor::NameRegister(),
                     Operand(isolate()->factory()->home_object_symbol()));
              __ LoadP(StoreDescriptor::ValueRegister(), MemOperand(sp));
              if (FLAG_vector_stores) {
                EmitLoadStoreICSlot(expr->GetNthSlot(store_slot_index++));
              }
              CallStoreIC();
            }
          } else {
            VisitForEffect(value);
          }
          break;
        }
        // Duplicate receiver on stack.
        __ LoadP(r2, MemOperand(sp));
        __ push(r2);
        VisitForStackValue(key);
        VisitForStackValue(value);
        if (property->emit_store()) {
          EmitSetHomeObjectIfNeeded(
              value, 2, expr->SlotForHomeObject(value, &store_slot_index));
          __ LoadSmiLiteral(r2, Smi::FromInt(SLOPPY));  // PropertyAttributes
          __ push(r2);
          __ CallRuntime(Runtime::kSetProperty, 4);
        } else {
          __ Drop(3);
        }
        break;
      case ObjectLiteral::Property::PROTOTYPE:
        // Duplicate receiver on stack.
        __ LoadP(r2, MemOperand(sp));
        __ push(r2);
        VisitForStackValue(value);
        DCHECK(property->emit_store());
        __ CallRuntime(Runtime::kInternalSetPrototype, 2);
        break;
      case ObjectLiteral::Property::GETTER:
        if (property->emit_store()) {
          accessor_table.lookup(key)->second->getter = value;
        }
        break;
      case ObjectLiteral::Property::SETTER:
        if (property->emit_store()) {
          accessor_table.lookup(key)->second->setter = value;
        }
        break;
    }
  }

  // Emit code to define accessors, using only a single call to the runtime for
  // each pair of corresponding getters and setters.
  for (AccessorTable::Iterator it = accessor_table.begin();
       it != accessor_table.end(); ++it) {
    __ LoadP(r2, MemOperand(sp));  // Duplicate receiver.
    __ push(r2);
    VisitForStackValue(it->first);
    EmitAccessor(it->second->getter);
    EmitSetHomeObjectIfNeeded(
        it->second->getter, 2,
        expr->SlotForHomeObject(it->second->getter, &store_slot_index));
    EmitAccessor(it->second->setter);
    EmitSetHomeObjectIfNeeded(
        it->second->setter, 3,
        expr->SlotForHomeObject(it->second->setter, &store_slot_index));
    __ LoadSmiLiteral(r2, Smi::FromInt(NONE));
    __ push(r2);
    __ CallRuntime(Runtime::kDefineAccessorPropertyUnchecked, 5);
  }

  // Object literals have two parts. The "static" part on the left contains no
  // computed property names, and so we can compute its map ahead of time; see
  // runtime.cc::CreateObjectLiteralBoilerplate. The second "dynamic" part
  // starts with the first computed property name, and continues with all
  // properties to its right.  All the code from above initializes the static
  // component of the object literal, and arranges for the map of the result to
  // reflect the static order in which the keys appear. For the dynamic
  // properties, we compile them into a series of "SetOwnProperty" runtime
  // calls. This will preserve insertion order.
  for (; property_index < expr->properties()->length(); property_index++) {
    ObjectLiteral::Property* property = expr->properties()->at(property_index);

    Expression* value = property->value();
    if (!result_saved) {
      __ push(r2);  // Save result on the stack
      result_saved = true;
    }

    __ LoadP(r2, MemOperand(sp));  // Duplicate receiver.
    __ push(r2);

    if (property->kind() == ObjectLiteral::Property::PROTOTYPE) {
      DCHECK(!property->is_computed_name());
      VisitForStackValue(value);
      DCHECK(property->emit_store());
      __ CallRuntime(Runtime::kInternalSetPrototype, 2);
    } else {
      EmitPropertyKey(property, expr->GetIdForProperty(property_index));
      VisitForStackValue(value);
      EmitSetHomeObjectIfNeeded(
          value, 2, expr->SlotForHomeObject(value, &store_slot_index));

      switch (property->kind()) {
        case ObjectLiteral::Property::CONSTANT:
        case ObjectLiteral::Property::MATERIALIZED_LITERAL:
        case ObjectLiteral::Property::COMPUTED:
          if (property->emit_store()) {
            __ LoadSmiLiteral(r2, Smi::FromInt(NONE));
            __ push(r2);
            __ CallRuntime(Runtime::kDefineDataPropertyUnchecked, 4);
          } else {
            __ Drop(3);
          }
          break;

        case ObjectLiteral::Property::PROTOTYPE:
          UNREACHABLE();
          break;

        case ObjectLiteral::Property::GETTER:
          __ mov(r2, Operand(Smi::FromInt(NONE)));
          __ push(r2);
          __ CallRuntime(Runtime::kDefineGetterPropertyUnchecked, 4);
          break;

        case ObjectLiteral::Property::SETTER:
          __ mov(r2, Operand(Smi::FromInt(NONE)));
          __ push(r2);
          __ CallRuntime(Runtime::kDefineSetterPropertyUnchecked, 4);
          break;
      }
    }
  }

  if (expr->has_function()) {
    DCHECK(result_saved);
    __ LoadP(r2, MemOperand(sp));
    __ push(r2);
    __ CallRuntime(Runtime::kToFastProperties, 1);
  }

  if (result_saved) {
    context()->PlugTOS();
  } else {
    context()->Plug(r2);
  }

  // Verify that compilation exactly consumed the number of store ic slots that
  // the ObjectLiteral node had to offer.
  DCHECK(!FLAG_vector_stores || store_slot_index == expr->slot_count());
}


void FullCodeGenerator::VisitArrayLiteral(ArrayLiteral* expr) {
  Comment cmnt(masm_, "[ ArrayLiteral");

  expr->BuildConstantElements(isolate());
  Handle<FixedArray> constant_elements = expr->constant_elements();
  bool has_fast_elements =
      IsFastObjectElementsKind(expr->constant_elements_kind());
  Handle<FixedArrayBase> constant_elements_values(
      FixedArrayBase::cast(constant_elements->get(1)));

  AllocationSiteMode allocation_site_mode = TRACK_ALLOCATION_SITE;
  if (has_fast_elements && !FLAG_allocation_site_pretenuring) {
    // If the only customer of allocation sites is transitioning, then
    // we can turn it off if we don't have anywhere else to transition to.
    allocation_site_mode = DONT_TRACK_ALLOCATION_SITE;
  }

  __ LoadP(r5, MemOperand(fp, JavaScriptFrameConstants::kFunctionOffset));
  __ LoadP(r5, FieldMemOperand(r5, JSFunction::kLiteralsOffset));
  __ LoadSmiLiteral(r4, Smi::FromInt(expr->literal_index()));
  __ mov(r3, Operand(constant_elements));
  if (MustCreateArrayLiteralWithRuntime(expr)) {
    __ LoadSmiLiteral(r2, Smi::FromInt(expr->ComputeFlags()));
    __ Push(r5, r4, r3, r2);
    __ CallRuntime(Runtime::kCreateArrayLiteral, 4);
  } else {
    FastCloneShallowArrayStub stub(isolate(), allocation_site_mode);
    __ CallStub(&stub);
  }
  PrepareForBailoutForId(expr->CreateLiteralId(), TOS_REG);

  bool result_saved = false;  // Is the result saved to the stack?
  ZoneList<Expression*>* subexprs = expr->values();
  int length = subexprs->length();

  // Emit code to evaluate all the non-constant subexpressions and to store
  // them into the newly cloned array.
  int array_index = 0;
  for (; array_index < length; array_index++) {
    Expression* subexpr = subexprs->at(array_index);
    if (subexpr->IsSpread()) break;
    // If the subexpression is a literal or a simple materialized literal it
    // is already set in the cloned array.
    if (CompileTimeValue::IsCompileTimeValue(subexpr)) continue;

    if (!result_saved) {
      __ push(r2);
      __ Push(Smi::FromInt(expr->literal_index()));
      result_saved = true;
    }
    VisitForAccumulatorValue(subexpr);

    if (has_fast_elements) {
      int offset = FixedArray::kHeaderSize + (array_index * kPointerSize);
      __ LoadP(r7, MemOperand(sp, kPointerSize));  // Copy of array literal.
      __ LoadP(r3, FieldMemOperand(r7, JSObject::kElementsOffset));
      __ StoreP(result_register(), FieldMemOperand(r3, offset));
      // Update the write barrier for the array store.
      __ RecordWriteField(r3, offset, result_register(), r4, kLRHasBeenSaved,
                          kDontSaveFPRegs, EMIT_REMEMBERED_SET,
                          INLINE_SMI_CHECK);
    } else {
      __ LoadSmiLiteral(r5, Smi::FromInt(array_index));
      StoreArrayLiteralElementStub stub(isolate());
      __ CallStub(&stub);
    }

    PrepareForBailoutForId(expr->GetIdForElement(array_index), NO_REGISTERS);
  }

  // In case the array literal contains spread expressions it has two parts. The
  // first part is  the "static" array which has a literal index is  handled
  // above. The second part is the part after the first spread expression
  // (inclusive) and these elements gets appended to the array. Note that the
  // number elements an iterable produces is unknown ahead of time.
  if (array_index < length && result_saved) {
    __ Drop(1);  // literal index
    __ Pop(r2);
    result_saved = false;
  }
  for (; array_index < length; array_index++) {
    Expression* subexpr = subexprs->at(array_index);

    __ Push(r2);
    if (subexpr->IsSpread()) {
      VisitForStackValue(subexpr->AsSpread()->expression());
      __ InvokeBuiltin(Builtins::CONCAT_ITERABLE_TO_ARRAY, CALL_FUNCTION);
    } else {
      VisitForStackValue(subexpr);
      __ CallRuntime(Runtime::kAppendElement, 2);
    }

    PrepareForBailoutForId(expr->GetIdForElement(array_index), NO_REGISTERS);
  }

  if (result_saved) {
    __ Drop(1);  // literal index
    context()->PlugTOS();
  } else {
    context()->Plug(r2);
  }
}


void FullCodeGenerator::VisitAssignment(Assignment* expr) {
  DCHECK(expr->target()->IsValidReferenceExpression());

  Comment cmnt(masm_, "[ Assignment");
  SetExpressionPosition(expr, INSERT_BREAK);

  Property* property = expr->target()->AsProperty();
  LhsKind assign_type = Property::GetAssignType(property);

  // Evaluate LHS expression.
  switch (assign_type) {
    case VARIABLE:
      // Nothing to do here.
      break;
    case NAMED_PROPERTY:
      if (expr->is_compound()) {
        // We need the receiver both on the stack and in the register.
        VisitForStackValue(property->obj());
        __ LoadP(LoadDescriptor::ReceiverRegister(), MemOperand(sp, 0));
      } else {
        VisitForStackValue(property->obj());
      }
      break;
    case NAMED_SUPER_PROPERTY:
      VisitForStackValue(
          property->obj()->AsSuperPropertyReference()->this_var());
      VisitForAccumulatorValue(
          property->obj()->AsSuperPropertyReference()->home_object());
      __ Push(result_register());
      if (expr->is_compound()) {
        const Register scratch = r3;
        __ LoadP(scratch, MemOperand(sp, kPointerSize));
        __ Push(scratch, result_register());
      }
      break;
    case KEYED_SUPER_PROPERTY: {
      const Register scratch = r3;
      VisitForStackValue(
          property->obj()->AsSuperPropertyReference()->this_var());
      VisitForAccumulatorValue(
          property->obj()->AsSuperPropertyReference()->home_object());
      __ LoadRR(scratch, result_register());
      VisitForAccumulatorValue(property->key());
      __ Push(scratch, result_register());
      if (expr->is_compound()) {
        const Register scratch1 = r4;
        __ LoadP(scratch1, MemOperand(sp, 2 * kPointerSize));
        __ Push(scratch1, scratch, result_register());
      }
      break;
    }
    case KEYED_PROPERTY:
      if (expr->is_compound()) {
        VisitForStackValue(property->obj());
        VisitForStackValue(property->key());
        __ LoadP(LoadDescriptor::ReceiverRegister(),
                 MemOperand(sp, 1 * kPointerSize));
        __ LoadP(LoadDescriptor::NameRegister(), MemOperand(sp, 0));
      } else {
        VisitForStackValue(property->obj());
        VisitForStackValue(property->key());
      }
      break;
  }

  // For compound assignments we need another deoptimization point after the
  // variable/property load.
  if (expr->is_compound()) {
    {
      AccumulatorValueContext context(this);
      switch (assign_type) {
        case VARIABLE:
          EmitVariableLoad(expr->target()->AsVariableProxy());
          PrepareForBailout(expr->target(), TOS_REG);
          break;
        case NAMED_PROPERTY:
          EmitNamedPropertyLoad(property);
          PrepareForBailoutForId(property->LoadId(), TOS_REG);
          break;
        case NAMED_SUPER_PROPERTY:
          EmitNamedSuperPropertyLoad(property);
          PrepareForBailoutForId(property->LoadId(), TOS_REG);
          break;
        case KEYED_SUPER_PROPERTY:
          EmitKeyedSuperPropertyLoad(property);
          PrepareForBailoutForId(property->LoadId(), TOS_REG);
          break;
        case KEYED_PROPERTY:
          EmitKeyedPropertyLoad(property);
          PrepareForBailoutForId(property->LoadId(), TOS_REG);
          break;
      }
    }

    Token::Value op = expr->binary_op();
    __ push(r2);  // Left operand goes on the stack.
    VisitForAccumulatorValue(expr->value());

    AccumulatorValueContext context(this);
    if (ShouldInlineSmiCase(op)) {
      EmitInlineSmiBinaryOp(expr->binary_operation(), op, expr->target(),
                            expr->value());
    } else {
      EmitBinaryOp(expr->binary_operation(), op);
    }

    // Deoptimization point in case the binary operation may have side effects.
    PrepareForBailout(expr->binary_operation(), TOS_REG);
  } else {
    VisitForAccumulatorValue(expr->value());
  }

  SetExpressionPosition(expr);

  // Store the value.
  switch (assign_type) {
    case VARIABLE:
      EmitVariableAssignment(expr->target()->AsVariableProxy()->var(),
                             expr->op(), expr->AssignmentSlot());
      PrepareForBailoutForId(expr->AssignmentId(), TOS_REG);
      context()->Plug(r2);
      break;
    case NAMED_PROPERTY:
      EmitNamedPropertyAssignment(expr);
      break;
    case NAMED_SUPER_PROPERTY:
      EmitNamedSuperPropertyStore(property);
      context()->Plug(r2);
      break;
    case KEYED_SUPER_PROPERTY:
      EmitKeyedSuperPropertyStore(property);
      context()->Plug(r2);
      break;
    case KEYED_PROPERTY:
      EmitKeyedPropertyAssignment(expr);
      break;
  }
}


void FullCodeGenerator::VisitYield(Yield* expr) {
  Comment cmnt(masm_, "[ Yield");
  SetExpressionPosition(expr);

  // Evaluate yielded value first; the initial iterator definition depends on
  // this.  It stays on the stack while we update the iterator.
  VisitForStackValue(expr->expression());

  switch (expr->yield_kind()) {
    case Yield::kSuspend:
      // Pop value from top-of-stack slot; box result into result register.
      EmitCreateIteratorResult(false);
      __ push(result_register());
    // Fall through.
    case Yield::kInitial: {
      Label suspend, continuation, post_runtime, resume;

      __ b(&suspend, Label::kNear);

      __ bind(&continuation);
      __ b(&resume);

      __ bind(&suspend);
      VisitForAccumulatorValue(expr->generator_object());
      DCHECK(continuation.pos() > 0 && Smi::IsValid(continuation.pos()));
      __ LoadSmiLiteral(r3, Smi::FromInt(continuation.pos()));
      __ StoreP(r3, FieldMemOperand(r2,
            JSGeneratorObject::kContinuationOffset));
      __ StoreP(cp, FieldMemOperand(r2,
            JSGeneratorObject::kContextOffset));
      __ LoadRR(r3, cp);
      __ RecordWriteField(r2, JSGeneratorObject::kContextOffset, r3, r4,
                          kLRHasBeenSaved, kDontSaveFPRegs);
      __ AddP(r3, fp, Operand(StandardFrameConstants::kExpressionsOffset));
      __ CmpP(sp, r3);
      __ beq(&post_runtime);
      __ push(r2);  // generator object
      __ CallRuntime(Runtime::kSuspendJSGeneratorObject, 1);
      __ LoadP(cp, MemOperand(fp, StandardFrameConstants::kContextOffset));
      __ bind(&post_runtime);
      __ pop(result_register());
      EmitReturnSequence();

      __ bind(&resume);
      context()->Plug(result_register());
      break;
    }

    case Yield::kFinal: {
      VisitForAccumulatorValue(expr->generator_object());
      __ LoadSmiLiteral(r3, Smi::FromInt(JSGeneratorObject::kGeneratorClosed));
      __ StoreP(r3, FieldMemOperand(result_register(),
                                    JSGeneratorObject::kContinuationOffset));
      // Pop value from top-of-stack slot, box result into result register.
      EmitCreateIteratorResult(true);
      EmitUnwindBeforeReturn();
      EmitReturnSequence();
      break;
    }

    case Yield::kDelegating: {
      VisitForStackValue(expr->generator_object());

      // Initial stack layout is as follows:
      // [sp + 1 * kPointerSize] iter
      // [sp + 0 * kPointerSize] g

      Label l_catch, l_try, l_suspend, l_continuation, l_resume;
      Label l_next, l_call;
      Register load_receiver = LoadDescriptor::ReceiverRegister();
      Register load_name = LoadDescriptor::NameRegister();

      // Initial send value is undefined.
      __ LoadRoot(r2, Heap::kUndefinedValueRootIndex);
      __ b(&l_next);

      // catch (e) { receiver = iter; f = 'throw'; arg = e; goto l_call; }
      __ bind(&l_catch);
      __ LoadRoot(load_name, Heap::kthrow_stringRootIndex);  // "throw"
      __ LoadP(r5, MemOperand(sp, 1 * kPointerSize));   // iter
      __ Push(load_name, r5, r2);   // "throw", iter, except
      __ b(&l_call);

      // try { received = %yield result }
      // Shuffle the received result above a try handler and yield it without
      // re-boxing.
      __ bind(&l_try);
      __ pop(r2);  // result
      int handler_index = NewHandlerTableEntry();
      EnterTryBlock(handler_index, &l_catch);
      const int try_block_size = TryCatch::kElementCount * kPointerSize;
      __ push(r2);                                       // result
      __ b(&l_suspend, Label::kNear);
      __ bind(&l_continuation);
      __ b(&l_resume);
      __ bind(&l_suspend);
      const int generator_object_depth = kPointerSize + try_block_size;
      __ LoadP(r2, MemOperand(sp, generator_object_depth));
      __ push(r2);  // g
      __ Push(Smi::FromInt(handler_index));  // handler-index
      DCHECK(l_continuation.pos() > 0 && Smi::IsValid(l_continuation.pos()));
      __ LoadSmiLiteral(r3, Smi::FromInt(l_continuation.pos()));
      __ StoreP(r3, FieldMemOperand(r2,
            JSGeneratorObject::kContinuationOffset));
      __ StoreP(cp, FieldMemOperand(r2, JSGeneratorObject::kContextOffset));
      __ LoadRR(r3, cp);
      __ RecordWriteField(r2, JSGeneratorObject::kContextOffset, r3, r4,
                          kLRHasBeenSaved, kDontSaveFPRegs);
      __ CallRuntime(Runtime::kSuspendJSGeneratorObject, 2);
      __ LoadP(cp, MemOperand(fp, StandardFrameConstants::kContextOffset));
      __ pop(r2);   // result
      EmitReturnSequence();
      __ bind(&l_resume);  // received in r2
      ExitTryBlock(handler_index);

      // receiver = iter; f = 'next'; arg = received;
      __ bind(&l_next);

      __ LoadRoot(load_name, Heap::knext_stringRootIndex);  // "next"
      __ LoadP(r5, MemOperand(sp, 1 * kPointerSize));  // iter
      __ Push(load_name, r5, r2);  // "next", iter, received

      // result = receiver[f](arg);
      __ bind(&l_call);
      __ LoadP(load_receiver, MemOperand(sp, kPointerSize));
      __ LoadP(load_name, MemOperand(sp, 2 * kPointerSize));
      __ mov(LoadDescriptor::SlotRegister(),
             Operand(SmiFromSlot(expr->KeyedLoadFeedbackSlot())));
      Handle<Code> ic = CodeFactory::KeyedLoadIC(isolate(), SLOPPY).code();
      CallIC(ic, TypeFeedbackId::None());
      __ LoadRR(r3, r2);
      __ StoreP(r3, MemOperand(sp, 2 * kPointerSize));
      CallFunctionStub stub(isolate(), 1, CALL_AS_METHOD);
      __ CallStub(&stub);

      __ LoadP(cp, MemOperand(fp, StandardFrameConstants::kContextOffset));
      __ Drop(1);  // The function is still on the stack; drop it.

      // if (!result.done) goto l_try;
      __ Move(load_receiver, r2);

      __ push(load_receiver);                               // save result
      __ LoadRoot(load_name, Heap::kdone_stringRootIndex);  // "done"
      __ mov(LoadDescriptor::SlotRegister(),
             Operand(SmiFromSlot(expr->DoneFeedbackSlot())));
      CallLoadIC(NOT_CONTEXTUAL);  // r0=result.done
      Handle<Code> bool_ic = ToBooleanStub::GetUninitialized(isolate());
      CallIC(bool_ic);
      __ CmpP(r2, Operand::Zero());
      __ beq(&l_try);

      // result.value
      __ pop(load_receiver);                                 // result
      __ LoadRoot(load_name, Heap::kvalue_stringRootIndex);  // "value"
      __ mov(LoadDescriptor::SlotRegister(),
             Operand(SmiFromSlot(expr->ValueFeedbackSlot())));
      CallLoadIC(NOT_CONTEXTUAL);     // r2=result.value
      context()->DropAndPlug(2, r2);  // drop iter and g
      break;
    }
  }
}


void FullCodeGenerator::EmitGeneratorResume(
    Expression* generator, Expression* value,
    JSGeneratorObject::ResumeMode resume_mode) {
  // The value stays in r2, and is ultimately read by the resumed generator, as
  // if CallRuntime(Runtime::kSuspendJSGeneratorObject) returned it. Or it
  // is read to throw the value when the resumed generator is already closed.
  // r3 will hold the generator object until the activation has been resumed.
  VisitForStackValue(generator);
  VisitForAccumulatorValue(value);
  __ pop(r3);

  // Load suspended function and context.
  __ LoadP(cp, FieldMemOperand(r3, JSGeneratorObject::kContextOffset));
  __ LoadP(r6, FieldMemOperand(r3, JSGeneratorObject::kFunctionOffset));

  // Load receiver and store as the first argument.
  __ LoadP(r4, FieldMemOperand(r3, JSGeneratorObject::kReceiverOffset));
  __ push(r4);

  // Push holes for the rest of the arguments to the generator function.
  __ LoadP(r5, FieldMemOperand(r6, JSFunction::kSharedFunctionInfoOffset));
  __ LoadW(
      r5, FieldMemOperand(r5, SharedFunctionInfo::kFormalParameterCountOffset));
  __ LoadRoot(r4, Heap::kTheHoleValueRootIndex);
  Label argument_loop, push_frame;
#if V8_TARGET_ARCH_S390X
  __ CmpP(r5, Operand::Zero());
  __ beq(&push_frame, Label::kNear);
#else
  __ SmiUntag(r5);
  __ beq(&push_frame, Label::kNear);
#endif
  __ LoadRR(r0, r5);
  __ bind(&argument_loop);
  __ push(r4);
  __ SubP(r0, Operand(1));
  __ bne(&argument_loop);

  // Enter a new JavaScript frame, and initialize its slots as they were when
  // the generator was suspended.
  Label resume_frame, done;
  __ bind(&push_frame);
  __ b(r14, &resume_frame);  // brasl
  __ b(&done);
  __ bind(&resume_frame);
  // lr = return address.
  // fp = caller's frame pointer.
  // cp = callee's context,
  // r6 = callee's JS function.
  __ PushFixedFrame(r6);
  // Adjust FP to point to saved FP.
  __ lay(fp, MemOperand(sp, StandardFrameConstants::kFixedFrameSizeFromFp));

  // Load the operand stack size.
  __ LoadP(r5, FieldMemOperand(r3, JSGeneratorObject::kOperandStackOffset));
  __ LoadP(r5, FieldMemOperand(r5, FixedArray::kLengthOffset));
  __ SmiUntag(r5);

  // If we are sending a value and there is no operand stack, we can jump back
  // in directly.
  Label call_resume;
  if (resume_mode == JSGeneratorObject::NEXT) {
    Label slow_resume;
    __ bne(&slow_resume, Label::kNear);
    __ LoadP(ip, FieldMemOperand(r6, JSFunction::kCodeEntryOffset));
    __ LoadP(r4, FieldMemOperand(r3, JSGeneratorObject::kContinuationOffset));
    __ SmiUntag(r4);
    __ AddP(ip, ip, r4);
    __ LoadSmiLiteral(r4,
                      Smi::FromInt(JSGeneratorObject::kGeneratorExecuting));
    __ StoreP(r4, FieldMemOperand(r3,
          JSGeneratorObject::kContinuationOffset));
    __ Jump(ip);
    __ bind(&slow_resume);
  } else {
    __ beq(&call_resume);
  }

  // Otherwise, we push holes for the operand stack and call the runtime to fix
  // up the stack and the handlers.
  Label operand_loop;
  __ LoadRR(r0, r5);
  __ bind(&operand_loop);
  __ push(r4);
  __ SubP(r0, Operand(1));
  __ bne(&operand_loop);

  __ bind(&call_resume);
  DCHECK(!result_register().is(r3));
  __ Push(r3, result_register());
  __ Push(Smi::FromInt(resume_mode));
  __ CallRuntime(Runtime::kResumeJSGeneratorObject, 3);
  // Not reached: the runtime call returns elsewhere.
  __ stop("not-reached");

  __ bind(&done);
  context()->Plug(result_register());
}


void FullCodeGenerator::EmitCreateIteratorResult(bool done) {
  Label gc_required;
  Label allocated;

  const int instance_size = 5 * kPointerSize;
  DCHECK_EQ(isolate()->native_context()->iterator_result_map()->instance_size(),
            instance_size);

  __ Allocate(instance_size, r2, r4, r5, &gc_required, TAG_OBJECT);
  __ b(&allocated);

  __ bind(&gc_required);
  __ Push(Smi::FromInt(instance_size));
  __ CallRuntime(Runtime::kAllocateInNewSpace, 1);
  __ LoadP(context_register(),
           MemOperand(fp, StandardFrameConstants::kContextOffset));

  __ bind(&allocated);
  __ LoadP(r3, ContextOperand(cp, Context::GLOBAL_OBJECT_INDEX));
  __ LoadP(r3, FieldMemOperand(r3, GlobalObject::kNativeContextOffset));
  __ LoadP(r3, ContextOperand(r3, Context::ITERATOR_RESULT_MAP_INDEX));
  __ pop(r4);
  __ mov(r5, Operand(isolate()->factory()->ToBoolean(done)));
  __ mov(r6, Operand(isolate()->factory()->empty_fixed_array()));
  __ StoreP(r3, FieldMemOperand(r2, HeapObject::kMapOffset));
  __ StoreP(r6, FieldMemOperand(r2, JSObject::kPropertiesOffset));
  __ StoreP(r6, FieldMemOperand(r2, JSObject::kElementsOffset));
  __ StoreP(r4,
            FieldMemOperand(r2, JSGeneratorObject::kResultValuePropertyOffset));
  __ StoreP(r5,
            FieldMemOperand(r2, JSGeneratorObject::kResultDonePropertyOffset));

  // Only the value field needs a write barrier, as the other values are in the
  // root set.
  __ RecordWriteField(r2, JSGeneratorObject::kResultValuePropertyOffset, r4,
      r5, kLRHasBeenSaved, kDontSaveFPRegs);
}


void FullCodeGenerator::EmitNamedPropertyLoad(Property* prop) {
  SetExpressionPosition(prop);
  Literal* key = prop->key()->AsLiteral();
  DCHECK(!prop->IsSuperAccess());

  __ mov(LoadDescriptor::NameRegister(), Operand(key->value()));
  __ mov(LoadDescriptor::SlotRegister(),
         Operand(SmiFromSlot(prop->PropertyFeedbackSlot())));
  CallLoadIC(NOT_CONTEXTUAL, language_mode());
}


void FullCodeGenerator::EmitNamedSuperPropertyLoad(Property* prop) {
  // Stack: receiver, home_object.
  SetExpressionPosition(prop);
  Literal* key = prop->key()->AsLiteral();
  DCHECK(!key->value()->IsSmi());
  DCHECK(prop->IsSuperAccess());

  __ Push(key->value());
  __ Push(Smi::FromInt(language_mode()));
  __ CallRuntime(Runtime::kLoadFromSuper, 4);
}


void FullCodeGenerator::EmitKeyedPropertyLoad(Property* prop) {
  SetExpressionPosition(prop);
  Handle<Code> ic = CodeFactory::KeyedLoadIC(isolate(), language_mode()).code();
  __ mov(LoadDescriptor::SlotRegister(),
         Operand(SmiFromSlot(prop->PropertyFeedbackSlot())));
  CallIC(ic);
}


void FullCodeGenerator::EmitKeyedSuperPropertyLoad(Property* prop) {
  // Stack: receiver, home_object, key.
  SetExpressionPosition(prop);
  __ Push(Smi::FromInt(language_mode()));
  __ CallRuntime(Runtime::kLoadKeyedFromSuper, 4);
}


void FullCodeGenerator::EmitInlineSmiBinaryOp(BinaryOperation* expr,
                                              Token::Value op,
                                              Expression* left_expr,
                                              Expression* right_expr) {
  Label done, smi_case, stub_call;

  Register scratch1 = r4;
  Register scratch2 = r5;

  // Get the arguments.
  Register left = r3;
  Register right = r2;
  __ pop(left);

  // Perform combined smi check on both operands.
  __ LoadRR(scratch1, right);
  __ OrP(scratch1, left);
  STATIC_ASSERT(kSmiTag == 0);
  JumpPatchSite patch_site(masm_);
  patch_site.EmitJumpIfSmi(scratch1, &smi_case);

  __ bind(&stub_call);
  Handle<Code> code =
      CodeFactory::BinaryOpIC(isolate(), op, strength(language_mode())).code();
  CallIC(code, expr->BinaryOperationFeedbackId());
  patch_site.EmitPatchInfo();
  __ b(&done);

  __ bind(&smi_case);
  // Smi case. This code works the same way as the smi-smi case in the type
  // recording binary operation stub.
  switch (op) {
    case Token::SAR:
      __ GetLeastBitsFromSmi(scratch1, right, 5);
      __ ShiftRightArithP(right, left, scratch1);
      __ ClearRightImm(right, right, Operand(kSmiTagSize + kSmiShiftSize));
      break;
    case Token::SHL: {
      __ GetLeastBitsFromSmi(scratch2, right, 5);
#if V8_TARGET_ARCH_S390X
      __ ShiftLeftP(right, left, scratch2);
#else
      __ SmiUntag(scratch1, left);
      __ ShiftLeftP(scratch1, scratch1, scratch2);
      // Check that the *signed* result fits in a smi
      __ JumpIfNotSmiCandidate(scratch1, scratch2, &stub_call);
      __ SmiTag(right, scratch1);
#endif
      break;
    }
    case Token::SHR: {
      __ SmiUntag(scratch1, left);
      __ GetLeastBitsFromSmi(scratch2, right, 5);
      __ srl(scratch1, scratch2);
      // Unsigned shift is not allowed to produce a negative number.
      __ JumpIfNotUnsignedSmiCandidate(scratch1, r0, &stub_call);
      __ SmiTag(right, scratch1);
      break;
    }
    case Token::ADD: {
      __ AddAndCheckForOverflow(scratch1, left, right, scratch2, r0);
      __ BranchOnOverflow(&stub_call);
      __ LoadRR(right, scratch1);
      break;
    }
    case Token::SUB: {
      __ SubAndCheckForOverflow(scratch1, left, right, scratch2, r0);
      __ BranchOnOverflow(&stub_call);
      __ LoadRR(right, scratch1);
      break;
    }
    case Token::MUL: {
      Label mul_zero;
#if V8_TARGET_ARCH_S390X
      // Remove tag from both operands.
      __ SmiUntag(ip, right);
      __ SmiUntag(scratch2, left);
      __ mr_z(scratch1, ip);
      // Check for overflowing the smi range - no overflow if higher 33 bits of
      // the result are identical.
      __ lr(ip, scratch2);  // 32 bit load
      __ sra(ip, Operand(31));
      __ cr_z(ip, scratch1);  // 32 bit compare
      // TODO(JOHN): The above 3 instr expended from 31-bit TestIfInt32
      // __ TestIfInt32(scratch2, scratch1, ip);
      __ bne(&stub_call);
#else
      __ SmiUntag(ip, right);
      __ LoadRR(scratch2, left);    // load into low order of reg pair
      __ mr_z(scratch1, ip);        // R4:R5 = R5 * ip
      // Check for overflowing the smi range - no overflow if higher 33 bits of
      // the result are identical.
      __ TestIfInt32(scratch1, scratch2, ip);
      __ bne(&stub_call);
#endif
      // Go slow on zero result to handle -0.
      __ chi(scratch2, Operand::Zero());
      __ beq(&mul_zero, Label::kNear);
#if V8_TARGET_ARCH_S390X
      __ SmiTag(right, scratch2);
#else
      __ LoadRR(right, scratch2);
#endif
      __ b(&done);
      // We need -0 if we were multiplying a negative number with 0 to get 0.
      // We know one of them was zero.
      __ bind(&mul_zero);
      __ AddP(scratch2, right, left);
      __ CmpP(scratch2, Operand::Zero());
      __ blt(&stub_call);
      __ LoadSmiLiteral(right, Smi::FromInt(0));
      break;
    }
    case Token::BIT_OR:
      __ OrP(right, left);
      break;
    case Token::BIT_AND:
      __ AndP(right, left);
      break;
    case Token::BIT_XOR:
      __ XorP(right, left);
      break;
    default:
      UNREACHABLE();
  }

  __ bind(&done);
  context()->Plug(r2);
}


void FullCodeGenerator::EmitClassDefineProperties(ClassLiteral* lit,
                                                  int* used_store_slots) {
  // Constructor is in r2.
  DCHECK(lit != NULL);
  __ push(r2);

  // No access check is needed here since the constructor is created by the
  // class literal.
  Register scratch = r3;
  __ LoadP(scratch,
           FieldMemOperand(r2, JSFunction::kPrototypeOrInitialMapOffset));
  __ push(scratch);

  for (int i = 0; i < lit->properties()->length(); i++) {
    ObjectLiteral::Property* property = lit->properties()->at(i);
    Expression* value = property->value();

    if (property->is_static()) {
      __ LoadP(scratch, MemOperand(sp, kPointerSize));  // constructor
    } else {
      __ LoadP(scratch, MemOperand(sp, 0));  // prototype
    }
    __ push(scratch);
    EmitPropertyKey(property, lit->GetIdForProperty(i));

    // The static prototype property is read only. We handle the non computed
    // property name case in the parser. Since this is the only case where we
    // need to check for an own read only property we special case this so we do
    // not need to do this for every property.
    if (property->is_static() && property->is_computed_name()) {
      __ CallRuntime(Runtime::kThrowIfStaticPrototype, 1);
      __ push(r2);
    }

    VisitForStackValue(value);
    EmitSetHomeObjectIfNeeded(value, 2,
                              lit->SlotForHomeObject(value, used_store_slots));

    switch (property->kind()) {
      case ObjectLiteral::Property::CONSTANT:
      case ObjectLiteral::Property::MATERIALIZED_LITERAL:
      case ObjectLiteral::Property::PROTOTYPE:
        UNREACHABLE();
      case ObjectLiteral::Property::COMPUTED:
        __ CallRuntime(Runtime::kDefineClassMethod, 3);
        break;

      case ObjectLiteral::Property::GETTER:
        __ mov(r2, Operand(Smi::FromInt(DONT_ENUM)));
        __ push(r2);
        __ CallRuntime(Runtime::kDefineGetterPropertyUnchecked, 4);
        break;

      case ObjectLiteral::Property::SETTER:
        __ mov(r2, Operand(Smi::FromInt(DONT_ENUM)));
        __ push(r2);
        __ CallRuntime(Runtime::kDefineSetterPropertyUnchecked, 4);
        break;

      default:
        UNREACHABLE();
    }
  }

  // prototype
  __ CallRuntime(Runtime::kToFastProperties, 1);

  // constructor
  __ CallRuntime(Runtime::kToFastProperties, 1);
}


void FullCodeGenerator::EmitBinaryOp(BinaryOperation* expr, Token::Value op) {
  __ pop(r3);
  Handle<Code> code =
      CodeFactory::BinaryOpIC(isolate(), op, strength(language_mode())).code();
  JumpPatchSite patch_site(masm_);  // unbound, signals no inlined smi code.
  CallIC(code, expr->BinaryOperationFeedbackId());
  patch_site.EmitPatchInfo();
  context()->Plug(r2);
}


void FullCodeGenerator::EmitAssignment(Expression* expr,
                                       FeedbackVectorICSlot slot) {
  DCHECK(expr->IsValidReferenceExpression());

  Property* prop = expr->AsProperty();
  LhsKind assign_type = Property::GetAssignType(prop);

  switch (assign_type) {
    case VARIABLE: {
      Variable* var = expr->AsVariableProxy()->var();
      EffectContext context(this);
      EmitVariableAssignment(var, Token::ASSIGN, slot);
      break;
    }
    case NAMED_PROPERTY: {
      __ push(r2);  // Preserve value.
      VisitForAccumulatorValue(prop->obj());
      __ Move(StoreDescriptor::ReceiverRegister(), r2);
      __ pop(StoreDescriptor::ValueRegister());  // Restore value.
      __ mov(StoreDescriptor::NameRegister(),
             Operand(prop->key()->AsLiteral()->value()));
      if (FLAG_vector_stores) EmitLoadStoreICSlot(slot);
      CallStoreIC();
      break;
    }
    case NAMED_SUPER_PROPERTY: {
      __ Push(r2);
      VisitForStackValue(prop->obj()->AsSuperPropertyReference()->this_var());
      VisitForAccumulatorValue(
          prop->obj()->AsSuperPropertyReference()->home_object());
      // stack: value, this; r2: home_object
      Register scratch = r4;
      Register scratch2 = r5;
      __ LoadRR(scratch, result_register());                  // home_object
      __ LoadP(r2, MemOperand(sp, kPointerSize));         // value
      __ LoadP(scratch2, MemOperand(sp, 0));              // this
      __ StoreP(scratch2, MemOperand(sp, kPointerSize));  // this
      __ StoreP(scratch, MemOperand(sp, 0));              // home_object
      // stack: this, home_object; r2: value
      EmitNamedSuperPropertyStore(prop);
      break;
    }
    case KEYED_SUPER_PROPERTY: {
      __ Push(r2);
      VisitForStackValue(prop->obj()->AsSuperPropertyReference()->this_var());
      VisitForStackValue(
          prop->obj()->AsSuperPropertyReference()->home_object());
      VisitForAccumulatorValue(prop->key());
      Register scratch = r4;
      Register scratch2 = r5;
      __ LoadP(scratch2, MemOperand(sp, 2 * kPointerSize));  // value
      // stack: value, this, home_object; r3: key, r6: value
      __ LoadP(scratch, MemOperand(sp, kPointerSize));  // this
      __ StoreP(scratch, MemOperand(sp, 2 * kPointerSize));
      __ LoadP(scratch, MemOperand(sp, 0));  // home_object
      __ StoreP(scratch, MemOperand(sp, kPointerSize));
      __ StoreP(r2, MemOperand(sp, 0));
      __ Move(r2, scratch2);
      // stack: this, home_object, key; r2: value.
      EmitKeyedSuperPropertyStore(prop);
      break;
    }
    case KEYED_PROPERTY: {
      __ push(r2);  // Preserve value.
      VisitForStackValue(prop->obj());
      VisitForAccumulatorValue(prop->key());
      __ Move(StoreDescriptor::NameRegister(), r2);
      __ Pop(StoreDescriptor::ValueRegister(),
             StoreDescriptor::ReceiverRegister());
      if (FLAG_vector_stores) EmitLoadStoreICSlot(slot);
      Handle<Code> ic =
          CodeFactory::KeyedStoreIC(isolate(), language_mode()).code();
      CallIC(ic);
      break;
    }
  }
  context()->Plug(r2);
}


void FullCodeGenerator::EmitStoreToStackLocalOrContextSlot(
    Variable* var, MemOperand location) {
  __ StoreP(result_register(), location);
  if (var->IsContextSlot()) {
    // RecordWrite may destroy all its register arguments.
    __ LoadRR(r5, result_register());
    int offset = Context::SlotOffset(var->index());
    __ RecordWriteContextSlot(r3, offset, r5, r4, kLRHasBeenSaved,
                              kDontSaveFPRegs);
  }
}


void FullCodeGenerator::EmitVariableAssignment(Variable* var, Token::Value op,
                                               FeedbackVectorICSlot slot) {
  if (var->IsUnallocatedOrGlobalSlot()) {
    // Global var, const, or let.
    __ mov(StoreDescriptor::NameRegister(), Operand(var->name()));
    __ LoadP(StoreDescriptor::ReceiverRegister(), GlobalObjectOperand());
    if (FLAG_vector_stores) EmitLoadStoreICSlot(slot);
    CallStoreIC();

  } else if (var->mode() == LET && op != Token::INIT_LET) {
    // Non-initializing assignment to let variable needs a write barrier.
    DCHECK(!var->IsLookupSlot());
    DCHECK(var->IsStackAllocated() || var->IsContextSlot());
    Label assign;
    MemOperand location = VarOperand(var, r3);
    __ LoadP(r5, location);
    __ CompareRoot(r5, Heap::kTheHoleValueRootIndex);
    __ bne(&assign);
    __ mov(r5, Operand(var->name()));
    __ push(r5);
    __ CallRuntime(Runtime::kThrowReferenceError, 1);
    // Perform the assignment.
    __ bind(&assign);
    EmitStoreToStackLocalOrContextSlot(var, location);

  } else if (var->mode() == CONST && op != Token::INIT_CONST) {
    // Assignment to const variable needs a write barrier.
    DCHECK(!var->IsLookupSlot());
    DCHECK(var->IsStackAllocated() || var->IsContextSlot());
    Label const_error;
    MemOperand location = VarOperand(var, r3);
    __ LoadP(r5, location);
    __ CompareRoot(r5, Heap::kTheHoleValueRootIndex);
    __ bne(&const_error, Label::kNear);
    __ mov(r5, Operand(var->name()));
    __ push(r5);
    __ CallRuntime(Runtime::kThrowReferenceError, 1);
    __ bind(&const_error);
    __ CallRuntime(Runtime::kThrowConstAssignError, 0);

  } else if (!var->is_const_mode() || op == Token::INIT_CONST) {
    if (var->IsLookupSlot()) {
      // Assignment to var.
      __ push(r2);  // Value.
      __ mov(r3, Operand(var->name()));
      __ mov(r2, Operand(Smi::FromInt(language_mode())));
      __ Push(cp, r3, r2);  // Context, name, language mode.
      __ CallRuntime(Runtime::kStoreLookupSlot, 4);
    } else {
      // Assignment to var or initializing assignment to let/const in harmony
      // mode.
      DCHECK((var->IsStackAllocated() || var->IsContextSlot()));
      MemOperand location = VarOperand(var, r3);
      if (generate_debug_code_ && op == Token::INIT_LET) {
        // Check for an uninitialized let binding.
        __ LoadP(r4, location);
        __ CompareRoot(r4, Heap::kTheHoleValueRootIndex);
        __ Check(eq, kLetBindingReInitialization);
      }
      EmitStoreToStackLocalOrContextSlot(var, location);
    }
  } else if (op == Token::INIT_CONST_LEGACY) {
    // Const initializers need a write barrier.
    DCHECK(var->mode() == CONST_LEGACY);
    DCHECK(!var->IsParameter());  // No const parameters.
    if (var->IsLookupSlot()) {
      __ push(r2);
      __ mov(r2, Operand(var->name()));
      __ Push(cp, r2);  // Context and name.
      __ CallRuntime(Runtime::kInitializeLegacyConstLookupSlot, 3);
    } else {
      DCHECK(var->IsStackAllocated() || var->IsContextSlot());
      Label skip;
      MemOperand location = VarOperand(var, r3);
      __ LoadP(r4, location);
      __ CompareRoot(r4, Heap::kTheHoleValueRootIndex);
      __ bne(&skip);
      EmitStoreToStackLocalOrContextSlot(var, location);
      __ bind(&skip);
    }

  } else {
    DCHECK(var->mode() == CONST_LEGACY && op != Token::INIT_CONST_LEGACY);
    if (is_strict(language_mode())) {
      __ CallRuntime(Runtime::kThrowConstAssignError, 0);
    }
    // Silently ignore store in sloppy mode.
  }
}


void FullCodeGenerator::EmitNamedPropertyAssignment(Assignment* expr) {
  // Assignment to a property, using a named store IC.
  Property* prop = expr->target()->AsProperty();
  DCHECK(prop != NULL);
  DCHECK(prop->key()->IsLiteral());

  __ mov(StoreDescriptor::NameRegister(),
         Operand(prop->key()->AsLiteral()->value()));
  __ pop(StoreDescriptor::ReceiverRegister());
  if (FLAG_vector_stores) {
    EmitLoadStoreICSlot(expr->AssignmentSlot());
    CallStoreIC();
  } else {
    CallStoreIC(expr->AssignmentFeedbackId());
  }

  PrepareForBailoutForId(expr->AssignmentId(), TOS_REG);
  context()->Plug(r2);
}


void FullCodeGenerator::EmitNamedSuperPropertyStore(Property* prop) {
  // Assignment to named property of super.
  // r2 : value
  // stack : receiver ('this'), home_object
  DCHECK(prop != NULL);
  Literal* key = prop->key()->AsLiteral();
  DCHECK(key != NULL);

  __ Push(key->value());
  __ Push(r2);
  __ CallRuntime((is_strict(language_mode()) ? Runtime::kStoreToSuper_Strict
                                             : Runtime::kStoreToSuper_Sloppy),
                 4);
}


void FullCodeGenerator::EmitKeyedSuperPropertyStore(Property* prop) {
  // Assignment to named property of super.
  // r2 : value
  // stack : receiver ('this'), home_object, key
  DCHECK(prop != NULL);

  __ Push(r2);
  __ CallRuntime(
      (is_strict(language_mode()) ? Runtime::kStoreKeyedToSuper_Strict
                                  : Runtime::kStoreKeyedToSuper_Sloppy),
      4);
}


void FullCodeGenerator::EmitKeyedPropertyAssignment(Assignment* expr) {
  // Assignment to a property, using a keyed store IC.
  __ Pop(StoreDescriptor::ReceiverRegister(), StoreDescriptor::NameRegister());
  DCHECK(StoreDescriptor::ValueRegister().is(r2));

  Handle<Code> ic =
      CodeFactory::KeyedStoreIC(isolate(), language_mode()).code();
  if (FLAG_vector_stores) {
    EmitLoadStoreICSlot(expr->AssignmentSlot());
    CallIC(ic);
  } else {
    CallIC(ic, expr->AssignmentFeedbackId());
  }

  PrepareForBailoutForId(expr->AssignmentId(), TOS_REG);
  context()->Plug(r2);
}


void FullCodeGenerator::VisitProperty(Property* expr) {
  Comment cmnt(masm_, "[ Property");
  SetExpressionPosition(expr);

  Expression* key = expr->key();

  if (key->IsPropertyName()) {
    if (!expr->IsSuperAccess()) {
      VisitForAccumulatorValue(expr->obj());
      __ Move(LoadDescriptor::ReceiverRegister(), r2);
      EmitNamedPropertyLoad(expr);
    } else {
      VisitForStackValue(expr->obj()->AsSuperPropertyReference()->this_var());
      VisitForStackValue(
          expr->obj()->AsSuperPropertyReference()->home_object());
      EmitNamedSuperPropertyLoad(expr);
    }
  } else {
    if (!expr->IsSuperAccess()) {
      VisitForStackValue(expr->obj());
      VisitForAccumulatorValue(expr->key());
      __ Move(LoadDescriptor::NameRegister(), r2);
      __ pop(LoadDescriptor::ReceiverRegister());
      EmitKeyedPropertyLoad(expr);
    } else {
      VisitForStackValue(expr->obj()->AsSuperPropertyReference()->this_var());
      VisitForStackValue(
          expr->obj()->AsSuperPropertyReference()->home_object());
      VisitForStackValue(expr->key());
      EmitKeyedSuperPropertyLoad(expr);
    }
  }
  PrepareForBailoutForId(expr->LoadId(), TOS_REG);
  context()->Plug(r2);
}


void FullCodeGenerator::CallIC(Handle<Code> code, TypeFeedbackId ast_id) {
  ic_total_count_++;
  __ Call(code, RelocInfo::CODE_TARGET, ast_id);
}


// Code common for calls using the IC.
void FullCodeGenerator::EmitCallWithLoadIC(Call* expr) {
  Expression* callee = expr->expression();

  CallICState::CallType call_type =
      callee->IsVariableProxy() ? CallICState::FUNCTION : CallICState::METHOD;

  // Get the target function.
  if (call_type == CallICState::FUNCTION) {
    {
      StackValueContext context(this);
      EmitVariableLoad(callee->AsVariableProxy());
      PrepareForBailout(callee, NO_REGISTERS);
    }
    // Push undefined as receiver. This is patched in the method prologue if it
    // is a sloppy mode method.
    __ LoadRoot(r1, Heap::kUndefinedValueRootIndex);
    __ push(r1);
  } else {
    // Load the function from the receiver.
    DCHECK(callee->IsProperty());
    DCHECK(!callee->AsProperty()->IsSuperAccess());
    __ LoadP(LoadDescriptor::ReceiverRegister(), MemOperand(sp, 0));
    EmitNamedPropertyLoad(callee->AsProperty());
    PrepareForBailoutForId(callee->AsProperty()->LoadId(), TOS_REG);
    // Push the target function under the receiver.
    __ LoadP(r1, MemOperand(sp, 0));
    __ push(r1);
    __ StoreP(r2, MemOperand(sp, kPointerSize));
  }

  EmitCall(expr, call_type);
}


void FullCodeGenerator::EmitSuperCallWithLoadIC(Call* expr) {
  Expression* callee = expr->expression();
  DCHECK(callee->IsProperty());
  Property* prop = callee->AsProperty();
  DCHECK(prop->IsSuperAccess());
  SetExpressionPosition(prop);

  Literal* key = prop->key()->AsLiteral();
  DCHECK(!key->value()->IsSmi());
  // Load the function from the receiver.
  const Register scratch = r3;
  SuperPropertyReference* super_ref = prop->obj()->AsSuperPropertyReference();
  VisitForAccumulatorValue(super_ref->home_object());
  __ LoadRR(scratch, r2);
  VisitForAccumulatorValue(super_ref->this_var());
  __ Push(scratch, r2, r2, scratch);
  __ Push(key->value());
  __ Push(Smi::FromInt(language_mode()));

  // Stack here:
  //  - home_object
  //  - this (receiver)
  //  - this (receiver) <-- LoadFromSuper will pop here and below.
  //  - home_object
  //  - key
  //  - language_mode
  __ CallRuntime(Runtime::kLoadFromSuper, 4);

  // Replace home_object with target function.
  __ StoreP(r2, MemOperand(sp, kPointerSize));

  // Stack here:
  // - target function
  // - this (receiver)
  EmitCall(expr, CallICState::METHOD);
}


// Code common for calls using the IC.
void FullCodeGenerator::EmitKeyedCallWithLoadIC(Call* expr, Expression* key) {
  // Load the key.
  VisitForAccumulatorValue(key);

  Expression* callee = expr->expression();

  // Load the function from the receiver.
  DCHECK(callee->IsProperty());
  __ LoadP(LoadDescriptor::ReceiverRegister(), MemOperand(sp, 0));
  __ Move(LoadDescriptor::NameRegister(), r2);
  EmitKeyedPropertyLoad(callee->AsProperty());
  PrepareForBailoutForId(callee->AsProperty()->LoadId(), TOS_REG);

  // Push the target function under the receiver.
  __ LoadP(ip, MemOperand(sp, 0));
  __ push(ip);
  __ StoreP(r2, MemOperand(sp, kPointerSize));

  EmitCall(expr, CallICState::METHOD);
}


void FullCodeGenerator::EmitKeyedSuperCallWithLoadIC(Call* expr) {
  Expression* callee = expr->expression();
  DCHECK(callee->IsProperty());
  Property* prop = callee->AsProperty();
  DCHECK(prop->IsSuperAccess());

  SetExpressionPosition(prop);
  // Load the function from the receiver.
  const Register scratch = r3;
  SuperPropertyReference* super_ref = prop->obj()->AsSuperPropertyReference();
  VisitForAccumulatorValue(super_ref->home_object());
  __ LoadRR(scratch, r2);
  VisitForAccumulatorValue(super_ref->this_var());
  __ Push(scratch, r2, r2, scratch);
  VisitForStackValue(prop->key());
  __ Push(Smi::FromInt(language_mode()));

  // Stack here:
  //  - home_object
  //  - this (receiver)
  //  - this (receiver) <-- LoadKeyedFromSuper will pop here and below.
  //  - home_object
  //  - key
  //  - language_mode
  __ CallRuntime(Runtime::kLoadKeyedFromSuper, 4);

  // Replace home_object with target function.
  __ StoreP(r2, MemOperand(sp, kPointerSize));

  // Stack here:
  // - target function
  // - this (receiver)
  EmitCall(expr, CallICState::METHOD);
}


void FullCodeGenerator::EmitCall(Call* expr, CallICState::CallType call_type) {
  // Load the arguments.
  ZoneList<Expression*>* args = expr->arguments();
  int arg_count = args->length();
  for (int i = 0; i < arg_count; i++) {
    VisitForStackValue(args->at(i));
  }

  SetExpressionPosition(expr);
  Handle<Code> ic = CodeFactory::CallIC(isolate(), arg_count, call_type).code();
  __ LoadSmiLiteral(r5, SmiFromSlot(expr->CallFeedbackICSlot()));
  __ LoadP(r3, MemOperand(sp, (arg_count + 1) * kPointerSize), r0);
  // Don't assign a type feedback id to the IC, since type feedback is provided
  // by the vector above.
  CallIC(ic);

  RecordJSReturnSite(expr);
  // Restore context register.
  __ LoadP(cp, MemOperand(fp, StandardFrameConstants::kContextOffset));
  context()->DropAndPlug(1, r2);
}


void FullCodeGenerator::EmitResolvePossiblyDirectEval(int arg_count) {
  // r6: copy of the first argument or undefined if it doesn't exist.
  if (arg_count > 0) {
    __ LoadP(r6, MemOperand(sp, arg_count * kPointerSize), r0);
  } else {
    __ LoadRoot(r6, Heap::kUndefinedValueRootIndex);
  }

  // r5: the receiver of the enclosing function.
  __ LoadP(r5, MemOperand(fp, JavaScriptFrameConstants::kFunctionOffset));

  // r4: language mode.
  __ LoadSmiLiteral(r4, Smi::FromInt(language_mode()));

  // r3: the start position of the scope the calls resides in.
  __ LoadSmiLiteral(r3, Smi::FromInt(scope()->start_position()));

  // Do the runtime call.
  __ Push(r6, r5, r4, r3);
  __ CallRuntime(Runtime::kResolvePossiblyDirectEval, 5);
}


void FullCodeGenerator::EmitInitializeThisAfterSuper(
    SuperCallReference* super_ref, FeedbackVectorICSlot slot) {
  Variable* this_var = super_ref->this_var()->var();
  GetVar(r3, this_var);
  __ CompareRoot(r3, Heap::kTheHoleValueRootIndex);
  Label uninitialized_this;
  __ beq(&uninitialized_this);
  __ mov(r3, Operand(this_var->name()));
  __ push(r3);
  __ CallRuntime(Runtime::kThrowReferenceError, 1);
  __ bind(&uninitialized_this);

  EmitVariableAssignment(this_var, Token::INIT_CONST, slot);
}


// See http://www.ecma-international.org/ecma-262/6.0/#sec-function-calls.
void FullCodeGenerator::PushCalleeAndWithBaseObject(Call* expr) {
  VariableProxy* callee = expr->expression()->AsVariableProxy();
  if (callee->var()->IsLookupSlot()) {
    Label slow, done;
    SetExpressionPosition(callee);
    // Generate code for loading from variables potentially shadowed by
    // eval-introduced variables.
    EmitDynamicLookupFastCase(callee, NOT_INSIDE_TYPEOF, &slow, &done);

    __ bind(&slow);
    // Call the runtime to find the function to call (returned in r2) and
    // the object holding it (returned in r3).
    DCHECK(!context_register().is(r4));
    __ mov(r4, Operand(callee->name()));
    __ Push(context_register(), r4);
    __ CallRuntime(Runtime::kLoadLookupSlot, 2);
    __ Push(r2, r3);  // Function, receiver.
    PrepareForBailoutForId(expr->LookupId(), NO_REGISTERS);

    // If fast case code has been generated, emit code to push the function
    // and receiver and have the slow path jump around this code.
    if (done.is_linked()) {
      Label call;
      __ b(&call);
      __ bind(&done);
      // Push function.
      __ push(r2);
      // Pass undefined as the receiver, which is the WithBaseObject of a
      // non-object environment record.  If the callee is sloppy, it will patch
      // it up to be the global receiver.
      __ LoadRoot(r3, Heap::kUndefinedValueRootIndex);
      __ push(r3);
      __ bind(&call);
    }
  } else {
    VisitForStackValue(callee);
    // refEnv.WithBaseObject()
    __ LoadRoot(r4, Heap::kUndefinedValueRootIndex);
    __ push(r4);  // Reserved receiver slot.
  }
}


void FullCodeGenerator::VisitCall(Call* expr) {
#ifdef DEBUG
  // We want to verify that RecordJSReturnSite gets called on all paths
  // through this function.  Avoid early returns.
  expr->return_is_recorded_ = false;
#endif

  Comment cmnt(masm_, "[ Call");
  Expression* callee = expr->expression();
  Call::CallType call_type = expr->GetCallType(isolate());

  if (call_type == Call::POSSIBLY_EVAL_CALL) {
    // In a call to eval, we first call RuntimeHidden_ResolvePossiblyDirectEval
    // to resolve the function we need to call.  Then we call the resolved
    // function using the given arguments.
    ZoneList<Expression*>* args = expr->arguments();
    int arg_count = args->length();

    PushCalleeAndWithBaseObject(expr);

    // Push the arguments.
    for (int i = 0; i < arg_count; i++) {
      VisitForStackValue(args->at(i));
    }

    // Push a copy of the function (found below the arguments) and
    // resolve eval.
    __ LoadP(r3, MemOperand(sp, (arg_count + 1) * kPointerSize), r0);
    __ push(r3);
    EmitResolvePossiblyDirectEval(arg_count);

    // Touch up the stack with the resolved function.
    __ StoreP(r2, MemOperand(sp, (arg_count + 1) * kPointerSize), r0);

    PrepareForBailoutForId(expr->EvalId(), NO_REGISTERS);

    // Record source position for debugger.
    SetExpressionPosition(expr);
    CallFunctionStub stub(isolate(), arg_count, NO_CALL_FUNCTION_FLAGS);
    __ LoadP(r3, MemOperand(sp, (arg_count + 1) * kPointerSize), r0);
    __ CallStub(&stub);
    RecordJSReturnSite(expr);
    // Restore context register.
    __ LoadP(cp, MemOperand(fp, StandardFrameConstants::kContextOffset));
    context()->DropAndPlug(1, r2);
  } else if (call_type == Call::GLOBAL_CALL) {
    EmitCallWithLoadIC(expr);

  } else if (call_type == Call::LOOKUP_SLOT_CALL) {
    // Call to a lookup slot (dynamically introduced variable).
    PushCalleeAndWithBaseObject(expr);
    EmitCall(expr);
  } else if (call_type == Call::PROPERTY_CALL) {
    Property* property = callee->AsProperty();
    bool is_named_call = property->key()->IsPropertyName();
    if (property->IsSuperAccess()) {
      if (is_named_call) {
        EmitSuperCallWithLoadIC(expr);
      } else {
        EmitKeyedSuperCallWithLoadIC(expr);
      }
    } else {
      VisitForStackValue(property->obj());
      if (is_named_call) {
        EmitCallWithLoadIC(expr);
      } else {
        EmitKeyedCallWithLoadIC(expr, property->key());
      }
    }
  } else if (call_type == Call::SUPER_CALL) {
    EmitSuperConstructorCall(expr);
  } else {
    DCHECK(call_type == Call::OTHER_CALL);
    // Call to an arbitrary expression not handled specially above.
    VisitForStackValue(callee);
    __ LoadRoot(r3, Heap::kUndefinedValueRootIndex);
    __ push(r3);
    // Emit function call.
    EmitCall(expr);
  }

#ifdef DEBUG
  // RecordJSReturnSite should have been called.
  DCHECK(expr->return_is_recorded_);
#endif
}


void FullCodeGenerator::VisitCallNew(CallNew* expr) {
  Comment cmnt(masm_, "[ CallNew");
  // According to ECMA-262, section 11.2.2, page 44, the function
  // expression in new calls must be evaluated before the
  // arguments.

  // Push constructor on the stack.  If it's not a function it's used as
  // receiver for CALL_NON_FUNCTION, otherwise the value on the stack is
  // ignored.
  DCHECK(!expr->expression()->IsSuperPropertyReference());
  VisitForStackValue(expr->expression());

  // Push the arguments ("left-to-right") on the stack.
  ZoneList<Expression*>* args = expr->arguments();
  int arg_count = args->length();
  for (int i = 0; i < arg_count; i++) {
    VisitForStackValue(args->at(i));
  }

  // Call the construct call builtin that handles allocation and
  // constructor invocation.
  SetExpressionPosition(expr);

  // Load function and argument count into r3 and r2.
  __ mov(r2, Operand(arg_count));
  __ LoadP(r3, MemOperand(sp, arg_count * kPointerSize), r0);

  // Record call targets in unoptimized code.
  if (FLAG_pretenuring_call_new) {
    EnsureSlotContainsAllocationSite(expr->AllocationSiteFeedbackSlot());
    DCHECK(expr->AllocationSiteFeedbackSlot().ToInt() ==
           expr->CallNewFeedbackSlot().ToInt() + 1);
  }

  __ Move(r4, FeedbackVector());
  __ LoadSmiLiteral(r5, SmiFromSlot(expr->CallNewFeedbackSlot()));

  CallConstructStub stub(isolate(), RECORD_CONSTRUCTOR_TARGET);
  __ Call(stub.GetCode(), RelocInfo::CONSTRUCT_CALL);
  PrepareForBailoutForId(expr->ReturnId(), TOS_REG);
  context()->Plug(r2);
}


void FullCodeGenerator::EmitSuperConstructorCall(Call* expr) {
  SuperCallReference* super_call_ref =
      expr->expression()->AsSuperCallReference();
  DCHECK_NOT_NULL(super_call_ref);

  VariableProxy* new_target_proxy = super_call_ref->new_target_var();
  VisitForStackValue(new_target_proxy);

  EmitLoadSuperConstructor(super_call_ref);
  __ push(result_register());

  // Push the arguments ("left-to-right") on the stack.
  ZoneList<Expression*>* args = expr->arguments();
  int arg_count = args->length();
  for (int i = 0; i < arg_count; i++) {
    VisitForStackValue(args->at(i));
  }

  // Call the construct call builtin that handles allocation and
  // constructor invocation.
  SetExpressionPosition(expr);

  // Load function and argument count into r1 and r0.
  __ mov(r2, Operand(arg_count));
  __ LoadP(r3, MemOperand(sp, arg_count * kPointerSize));

  // Record call targets in unoptimized code.
  if (FLAG_pretenuring_call_new) {
    UNREACHABLE();
    /* TODO(dslomov): support pretenuring.
    EnsureSlotContainsAllocationSite(expr->AllocationSiteFeedbackSlot());
    DCHECK(expr->AllocationSiteFeedbackSlot().ToInt() ==
           expr->CallNewFeedbackSlot().ToInt() + 1);
    */
  }

  __ Move(r4, FeedbackVector());
  __ LoadSmiLiteral(r5, SmiFromSlot(expr->CallFeedbackSlot()));

  CallConstructStub stub(isolate(), SUPER_CALL_RECORD_TARGET);
  __ Call(stub.GetCode(), RelocInfo::CONSTRUCT_CALL);

  __ Drop(1);

  RecordJSReturnSite(expr);

  EmitInitializeThisAfterSuper(super_call_ref, expr->CallFeedbackICSlot());
  context()->Plug(r2);
}


void FullCodeGenerator::EmitIsSmi(CallRuntime* expr) {
  ZoneList<Expression*>* args = expr->arguments();
  DCHECK(args->length() == 1);

  VisitForAccumulatorValue(args->at(0));

  Label materialize_true, materialize_false, skip_lookup;
  Label* if_true = NULL;
  Label* if_false = NULL;
  Label* fall_through = NULL;
  context()->PrepareTest(&materialize_true, &materialize_false, &if_true,
                         &if_false, &fall_through);

  PrepareForBailoutBeforeSplit(expr, true, if_true, if_false);
  __ TestIfSmi(r2);
  Split(eq, if_true, if_false, fall_through, cr0);

  context()->Plug(if_true, if_false);
}


void FullCodeGenerator::EmitIsNonNegativeSmi(CallRuntime* expr) {
  ZoneList<Expression*>* args = expr->arguments();
  DCHECK(args->length() == 1);

  VisitForAccumulatorValue(args->at(0));

  Label materialize_true, materialize_false;
  Label* if_true = NULL;
  Label* if_false = NULL;
  Label* fall_through = NULL;
  context()->PrepareTest(&materialize_true, &materialize_false, &if_true,
                         &if_false, &fall_through);

  PrepareForBailoutBeforeSplit(expr, true, if_true, if_false);
  __ TestIfPositiveSmi(r2, r0);
  Split(eq, if_true, if_false, fall_through, cr0);

  context()->Plug(if_true, if_false);
}


void FullCodeGenerator::EmitIsObject(CallRuntime* expr) {
  ZoneList<Expression*>* args = expr->arguments();
  DCHECK(args->length() == 1);

  VisitForAccumulatorValue(args->at(0));

  Label materialize_true, materialize_false;
  Label* if_true = NULL;
  Label* if_false = NULL;
  Label* fall_through = NULL;
  context()->PrepareTest(&materialize_true, &materialize_false, &if_true,
                         &if_false, &fall_through);

  __ JumpIfSmi(r2, if_false);
  __ CompareRoot(r2, Heap::kNullValueRootIndex);
  __ beq(if_true);
  __ LoadP(r4, FieldMemOperand(r2, HeapObject::kMapOffset));
  // Undetectable objects behave like undefined when tested with typeof.
  __ tm(FieldMemOperand(r4, Map::kBitFieldOffset),
        Operand(1 << Map::kIsUndetectable));
  __ bne(if_false /*, cr0*/);
  __ LoadlB(r3, FieldMemOperand(r4, Map::kInstanceTypeOffset));
  __ CmpP(r3, Operand(FIRST_NONCALLABLE_SPEC_OBJECT_TYPE));
  __ blt(if_false);
  __ CmpP(r3, Operand(LAST_NONCALLABLE_SPEC_OBJECT_TYPE));
  PrepareForBailoutBeforeSplit(expr, true, if_true, if_false);
  Split(le, if_true, if_false, fall_through);

  context()->Plug(if_true, if_false);
}


void FullCodeGenerator::EmitIsSpecObject(CallRuntime* expr) {
  ZoneList<Expression*>* args = expr->arguments();
  DCHECK(args->length() == 1);

  VisitForAccumulatorValue(args->at(0));

  Label materialize_true, materialize_false;
  Label* if_true = NULL;
  Label* if_false = NULL;
  Label* fall_through = NULL;
  context()->PrepareTest(&materialize_true, &materialize_false, &if_true,
                         &if_false, &fall_through);

  __ JumpIfSmi(r2, if_false);
  __ CompareObjectType(r2, r3, r3, FIRST_SPEC_OBJECT_TYPE);
  PrepareForBailoutBeforeSplit(expr, true, if_true, if_false);
  Split(ge, if_true, if_false, fall_through);

  context()->Plug(if_true, if_false);
}


void FullCodeGenerator::EmitIsUndetectableObject(CallRuntime* expr) {
  ZoneList<Expression*>* args = expr->arguments();
  DCHECK(args->length() == 1);

  VisitForAccumulatorValue(args->at(0));

  Label materialize_true, materialize_false;
  Label* if_true = NULL;
  Label* if_false = NULL;
  Label* fall_through = NULL;
  context()->PrepareTest(&materialize_true, &materialize_false, &if_true,
                         &if_false, &fall_through);

  __ JumpIfSmi(r2, if_false);
  __ LoadP(r3, FieldMemOperand(r2, HeapObject::kMapOffset));
  __ tm(FieldMemOperand(r3, Map::kBitFieldOffset),
        Operand(1 << Map::kIsUndetectable));
  PrepareForBailoutBeforeSplit(expr, true, if_true, if_false);
  Split(ne, if_true, if_false, fall_through, cr0);

  context()->Plug(if_true, if_false);
}


void FullCodeGenerator::EmitIsStringWrapperSafeForDefaultValueOf(
    CallRuntime* expr) {
  ZoneList<Expression*>* args = expr->arguments();
  DCHECK(args->length() == 1);

  VisitForAccumulatorValue(args->at(0));

  Label materialize_true, materialize_false, skip_lookup;
  Label* if_true = NULL;
  Label* if_false = NULL;
  Label* fall_through = NULL;
  context()->PrepareTest(&materialize_true, &materialize_false, &if_true,
                         &if_false, &fall_through);

  __ AssertNotSmi(r2);

  __ LoadP(r3, FieldMemOperand(r2, HeapObject::kMapOffset));
  __ LoadlB(ip, FieldMemOperand(r3, Map::kBitField2Offset));
  __ mov(r0, Operand(1 << Map::kStringWrapperSafeForDefaultValueOf));
  __ AndP(r0, ip);
  __ bne(&skip_lookup);

  // Check for fast case object. Generate false result for slow case object.
  __ LoadP(r4, FieldMemOperand(r2, JSObject::kPropertiesOffset));
  __ LoadP(r4, FieldMemOperand(r4, HeapObject::kMapOffset));
  __ CompareRoot(r4, Heap::kHashTableMapRootIndex);
  __ beq(if_false);

  // Look for valueOf name in the descriptor array, and indicate false if
  // found. Since we omit an enumeration index check, if it is added via a
  // transition that shares its descriptor array, this is a false positive.
  Label entry, loop, done;

  // Skip loop if no descriptors are valid.
  __ NumberOfOwnDescriptors(r5, r3);
  __ CmpP(r5, Operand::Zero());
  __ beq(&done);

  __ LoadInstanceDescriptors(r3, r6);
  // r6: descriptor array.
  // r5: valid entries in the descriptor array.
  __ mov(ip, Operand(DescriptorArray::kDescriptorSize));
  __ Mul(r5, r5, ip);
  // Calculate location of the first key name.
  __ AddP(r6, Operand(DescriptorArray::kFirstOffset - kHeapObjectTag));
  // Calculate the end of the descriptor array.
  __ LoadRR(r4, r6);
  __ ShiftLeftP(ip, r5, Operand(kPointerSizeLog2));
  __ AddP(r4, ip);

  // Loop through all the keys in the descriptor array. If one of these is the
  // string "valueOf" the result is false.
  // The use of ip to store the valueOf string assumes that it is not otherwise
  // used in the loop below.
  __ mov(ip, Operand(isolate()->factory()->value_of_string()));
  __ b(&entry, Label::kNear);
  __ bind(&loop);
  __ LoadP(r5, MemOperand(r6, 0));
  __ CmpP(r5, ip);
  __ beq(if_false);
  __ AddP(r6, Operand(DescriptorArray::kDescriptorSize * kPointerSize));
  __ bind(&entry);
  __ CmpP(r6, r4);
  __ bne(&loop);

  __ bind(&done);

  // Set the bit in the map to indicate that there is no local valueOf field.
  __ LoadlB(r4, FieldMemOperand(r3, Map::kBitField2Offset));
  __ OrP(r4, Operand(1 << Map::kStringWrapperSafeForDefaultValueOf));
  __ stc(r4, FieldMemOperand(r3, Map::kBitField2Offset));

  __ bind(&skip_lookup);

  // If a valueOf property is not found on the object check that its
  // prototype is the un-modified String prototype. If not result is false.
  __ LoadP(r4, FieldMemOperand(r3, Map::kPrototypeOffset));
  __ JumpIfSmi(r4, if_false);
  __ LoadP(r4, FieldMemOperand(r4, HeapObject::kMapOffset));
  __ LoadP(r5, ContextOperand(cp, Context::GLOBAL_OBJECT_INDEX));
  __ LoadP(r5, FieldMemOperand(r5, GlobalObject::kNativeContextOffset));
  __ LoadP(r5,
           ContextOperand(r5, Context::STRING_FUNCTION_PROTOTYPE_MAP_INDEX));
  __ CmpP(r4, r5);
  PrepareForBailoutBeforeSplit(expr, true, if_true, if_false);
  Split(eq, if_true, if_false, fall_through);

  context()->Plug(if_true, if_false);
}


void FullCodeGenerator::EmitIsFunction(CallRuntime* expr) {
  ZoneList<Expression*>* args = expr->arguments();
  DCHECK(args->length() == 1);

  VisitForAccumulatorValue(args->at(0));

  Label materialize_true, materialize_false;
  Label* if_true = NULL;
  Label* if_false = NULL;
  Label* fall_through = NULL;
  context()->PrepareTest(&materialize_true, &materialize_false, &if_true,
                         &if_false, &fall_through);

  __ JumpIfSmi(r2, if_false);
  __ CompareObjectType(r2, r3, r4, JS_FUNCTION_TYPE);
  PrepareForBailoutBeforeSplit(expr, true, if_true, if_false);
  Split(eq, if_true, if_false, fall_through);

  context()->Plug(if_true, if_false);
}


void FullCodeGenerator::EmitIsMinusZero(CallRuntime* expr) {
  ZoneList<Expression*>* args = expr->arguments();
  DCHECK(args->length() == 1);

  VisitForAccumulatorValue(args->at(0));

  Label materialize_true, materialize_false;
  Label* if_true = NULL;
  Label* if_false = NULL;
  Label* fall_through = NULL;
  context()->PrepareTest(&materialize_true, &materialize_false, &if_true,
                         &if_false, &fall_through);

  __ CheckMap(r2, r3, Heap::kHeapNumberMapRootIndex, if_false, DO_SMI_CHECK);
#if V8_TARGET_ARCH_S390X
  __ LoadP(r3, FieldMemOperand(r2, HeapNumber::kValueOffset));
  __ llihf(r4, Operand(0x80000000));  // r4 = 0x80000000_00000000
  __ CmpP(r3, r4);
#else
  __ LoadlW(r4, FieldMemOperand(r2, HeapNumber::kExponentOffset));
  __ LoadlW(r3, FieldMemOperand(r2, HeapNumber::kMantissaOffset));
  Label skip;
  __ iilf(r0, Operand(0x80000000));
  __ CmpP(r4, r0);
  __ bne(&skip, Label::kNear);
  __ CmpP(r3, Operand::Zero());
  __ bind(&skip);
#endif

  PrepareForBailoutBeforeSplit(expr, true, if_true, if_false);
  Split(eq, if_true, if_false, fall_through);

  context()->Plug(if_true, if_false);
}


void FullCodeGenerator::EmitIsArray(CallRuntime* expr) {
  ZoneList<Expression*>* args = expr->arguments();
  DCHECK(args->length() == 1);

  VisitForAccumulatorValue(args->at(0));

  Label materialize_true, materialize_false;
  Label* if_true = NULL;
  Label* if_false = NULL;
  Label* fall_through = NULL;
  context()->PrepareTest(&materialize_true, &materialize_false, &if_true,
                         &if_false, &fall_through);

  __ JumpIfSmi(r2, if_false);
  __ CompareObjectType(r2, r3, r3, JS_ARRAY_TYPE);
  PrepareForBailoutBeforeSplit(expr, true, if_true, if_false);
  Split(eq, if_true, if_false, fall_through);

  context()->Plug(if_true, if_false);
}


void FullCodeGenerator::EmitIsTypedArray(CallRuntime* expr) {
  ZoneList<Expression*>* args = expr->arguments();
  DCHECK(args->length() == 1);

  VisitForAccumulatorValue(args->at(0));

  Label materialize_true, materialize_false;
  Label* if_true = NULL;
  Label* if_false = NULL;
  Label* fall_through = NULL;
  context()->PrepareTest(&materialize_true, &materialize_false, &if_true,
                         &if_false, &fall_through);

  __ JumpIfSmi(r2, if_false);
  __ CompareObjectType(r2, r3, r3, JS_TYPED_ARRAY_TYPE);
  PrepareForBailoutBeforeSplit(expr, true, if_true, if_false);
  Split(eq, if_true, if_false, fall_through);

  context()->Plug(if_true, if_false);
}


void FullCodeGenerator::EmitIsRegExp(CallRuntime* expr) {
  ZoneList<Expression*>* args = expr->arguments();
  DCHECK(args->length() == 1);

  VisitForAccumulatorValue(args->at(0));

  Label materialize_true, materialize_false;
  Label* if_true = NULL;
  Label* if_false = NULL;
  Label* fall_through = NULL;
  context()->PrepareTest(&materialize_true, &materialize_false, &if_true,
                         &if_false, &fall_through);

  __ JumpIfSmi(r2, if_false);
  __ CompareObjectType(r2, r3, r3, JS_REGEXP_TYPE);
  PrepareForBailoutBeforeSplit(expr, true, if_true, if_false);
  Split(eq, if_true, if_false, fall_through);

  context()->Plug(if_true, if_false);
}


void FullCodeGenerator::EmitIsJSProxy(CallRuntime* expr) {
  ZoneList<Expression*>* args = expr->arguments();
  DCHECK(args->length() == 1);

  VisitForAccumulatorValue(args->at(0));

  Label materialize_true, materialize_false;
  Label* if_true = NULL;
  Label* if_false = NULL;
  Label* fall_through = NULL;
  context()->PrepareTest(&materialize_true, &materialize_false, &if_true,
                         &if_false, &fall_through);

  __ JumpIfSmi(r2, if_false);
  Register map = r3;
  Register type_reg = r4;
  __ LoadP(map, FieldMemOperand(r2, HeapObject::kMapOffset));
  __ LoadlB(type_reg, FieldMemOperand(map, Map::kInstanceTypeOffset));
  __ SubP(type_reg, Operand(FIRST_JS_PROXY_TYPE));
  __ CmpLogicalP(type_reg, Operand(LAST_JS_PROXY_TYPE - FIRST_JS_PROXY_TYPE));
  PrepareForBailoutBeforeSplit(expr, true, if_true, if_false);
  Split(le, if_true, if_false, fall_through);

  context()->Plug(if_true, if_false);
}


void FullCodeGenerator::EmitIsConstructCall(CallRuntime* expr) {
  DCHECK(expr->arguments()->length() == 0);

  Label materialize_true, materialize_false;
  Label* if_true = NULL;
  Label* if_false = NULL;
  Label* fall_through = NULL;
  context()->PrepareTest(&materialize_true, &materialize_false, &if_true,
                         &if_false, &fall_through);

  // Get the frame pointer for the calling frame.
  __ LoadP(r4, MemOperand(fp, StandardFrameConstants::kCallerFPOffset));

  // Skip the arguments adaptor frame if it exists.
  Label check_frame_marker;
  __ LoadP(r3, MemOperand(r4, StandardFrameConstants::kContextOffset));
  __ CmpSmiLiteral(r3, Smi::FromInt(StackFrame::ARGUMENTS_ADAPTOR), r0);
  __ bne(&check_frame_marker, Label::kNear);
  __ LoadP(r4, MemOperand(r4, StandardFrameConstants::kCallerFPOffset));

  // Check the marker in the calling frame.
  __ bind(&check_frame_marker);
  __ LoadP(r3, MemOperand(r4, StandardFrameConstants::kMarkerOffset));
  STATIC_ASSERT(StackFrame::CONSTRUCT < 0x4000);
  __ CmpSmiLiteral(r3, Smi::FromInt(StackFrame::CONSTRUCT), r0);
  PrepareForBailoutBeforeSplit(expr, true, if_true, if_false);
  Split(eq, if_true, if_false, fall_through);

  context()->Plug(if_true, if_false);
}


void FullCodeGenerator::EmitObjectEquals(CallRuntime* expr) {
  ZoneList<Expression*>* args = expr->arguments();
  DCHECK(args->length() == 2);

  // Load the two objects into registers and perform the comparison.
  VisitForStackValue(args->at(0));
  VisitForAccumulatorValue(args->at(1));

  Label materialize_true, materialize_false;
  Label* if_true = NULL;
  Label* if_false = NULL;
  Label* fall_through = NULL;
  context()->PrepareTest(&materialize_true, &materialize_false, &if_true,
                         &if_false, &fall_through);

  __ pop(r3);
  __ CmpP(r2, r3);
  PrepareForBailoutBeforeSplit(expr, true, if_true, if_false);
  Split(eq, if_true, if_false, fall_through);

  context()->Plug(if_true, if_false);
}


void FullCodeGenerator::EmitArguments(CallRuntime* expr) {
  ZoneList<Expression*>* args = expr->arguments();
  DCHECK(args->length() == 1);

  // ArgumentsAccessStub expects the key in r4 and the formal
  // parameter count in r2.
  VisitForAccumulatorValue(args->at(0));
  __ LoadRR(r3, r2);
  __ LoadSmiLiteral(r2, Smi::FromInt(info_->scope()->num_parameters()));
  ArgumentsAccessStub stub(isolate(), ArgumentsAccessStub::READ_ELEMENT);
  __ CallStub(&stub);
  context()->Plug(r2);
}


void FullCodeGenerator::EmitArgumentsLength(CallRuntime* expr) {
  DCHECK(expr->arguments()->length() == 0);
  Label exit;
  // Get the number of formal parameters.
  __ LoadSmiLiteral(r2, Smi::FromInt(info_->scope()->num_parameters()));

  // Check if the calling frame is an arguments adaptor frame.
  __ LoadP(r4, MemOperand(fp, StandardFrameConstants::kCallerFPOffset));
  __ LoadP(r5, MemOperand(r4, StandardFrameConstants::kContextOffset));
  __ CmpSmiLiteral(r5, Smi::FromInt(StackFrame::ARGUMENTS_ADAPTOR), r0);
  __ bne(&exit, Label::kNear);

  // Arguments adaptor case: Read the arguments length from the
  // adaptor frame.
  __ LoadP(r2,
          MemOperand(r4, ArgumentsAdaptorFrameConstants::kLengthOffset));

  __ bind(&exit);
  context()->Plug(r2);
}


void FullCodeGenerator::EmitClassOf(CallRuntime* expr) {
  ZoneList<Expression*>* args = expr->arguments();
  DCHECK(args->length() == 1);
  Label done, null, function, non_function_constructor;

  VisitForAccumulatorValue(args->at(0));

  // If the object is a smi, we return null.
  __ JumpIfSmi(r2, &null);

  // Check that the object is a JS object but take special care of JS
  // functions to make sure they have 'Function' as their class.
  // Assume that there are only two callable types, and one of them is at
  // either end of the type range for JS object types. Saves extra comparisons.
  STATIC_ASSERT(NUM_OF_CALLABLE_SPEC_OBJECT_TYPES == 2);
  __ CompareObjectType(r2, r2, r3, FIRST_SPEC_OBJECT_TYPE);
  // Map is now in r2.
  __ blt(&null);
  STATIC_ASSERT(FIRST_NONCALLABLE_SPEC_OBJECT_TYPE ==
                FIRST_SPEC_OBJECT_TYPE + 1);
  __ beq(&function);

  __ CmpP(r3, Operand(LAST_SPEC_OBJECT_TYPE));
  STATIC_ASSERT(LAST_NONCALLABLE_SPEC_OBJECT_TYPE == LAST_SPEC_OBJECT_TYPE - 1);
  __ beq(&function);
  // Assume that there is no larger type.
  STATIC_ASSERT(LAST_NONCALLABLE_SPEC_OBJECT_TYPE == LAST_TYPE - 1);

  // Check if the constructor in the map is a JS function.
  Register instance_type = r4;
  __ GetMapConstructor(r2, r2, r3, instance_type);
  __ CmpP(instance_type, Operand(JS_FUNCTION_TYPE));
  __ bne(&non_function_constructor, Label::kNear);

  // r2 now contains the constructor function. Grab the
  // instance class name from there.
  __ LoadP(r2, FieldMemOperand(r2, JSFunction::kSharedFunctionInfoOffset));
  __ LoadP(r2,
           FieldMemOperand(r2, SharedFunctionInfo::kInstanceClassNameOffset));
  __ b(&done, Label::kNear);

  // Functions have class 'Function'.
  __ bind(&function);
  __ LoadRoot(r2, Heap::kFunction_stringRootIndex);
  __ b(&done, Label::kNear);

  // Objects with a non-function constructor have class 'Object'.
  __ bind(&non_function_constructor);
  __ LoadRoot(r2, Heap::kObject_stringRootIndex);
  __ b(&done, Label::kNear);

  // Non-JS objects have class null.
  __ bind(&null);
  __ LoadRoot(r2, Heap::kNullValueRootIndex);

  // All done.
  __ bind(&done);

  context()->Plug(r2);
}


void FullCodeGenerator::EmitSubString(CallRuntime* expr) {
  // Load the arguments on the stack and call the stub.
  SubStringStub stub(isolate());
  ZoneList<Expression*>* args = expr->arguments();
  DCHECK(args->length() == 3);
  VisitForStackValue(args->at(0));
  VisitForStackValue(args->at(1));
  VisitForStackValue(args->at(2));
  __ CallStub(&stub);
  context()->Plug(r2);
}


void FullCodeGenerator::EmitRegExpExec(CallRuntime* expr) {
  // Load the arguments on the stack and call the stub.
  RegExpExecStub stub(isolate());
  ZoneList<Expression*>* args = expr->arguments();
  DCHECK(args->length() == 4);
  VisitForStackValue(args->at(0));
  VisitForStackValue(args->at(1));
  VisitForStackValue(args->at(2));
  VisitForStackValue(args->at(3));
  __ CallStub(&stub);
  context()->Plug(r2);
}


void FullCodeGenerator::EmitValueOf(CallRuntime* expr) {
  ZoneList<Expression*>* args = expr->arguments();
  DCHECK(args->length() == 1);
  VisitForAccumulatorValue(args->at(0));  // Load the object.

  Label done;
  // If the object is a smi return the object.
  __ JumpIfSmi(r2, &done);
  // If the object is not a value type, return the object.
  __ CompareObjectType(r2, r3, r3, JS_VALUE_TYPE);
  __ bne(&done, Label::kNear);
  __ LoadP(r2, FieldMemOperand(r2, JSValue::kValueOffset));

  __ bind(&done);
  context()->Plug(r2);
}


void FullCodeGenerator::EmitIsDate(CallRuntime* expr) {
  ZoneList<Expression*>* args = expr->arguments();
  DCHECK_EQ(1, args->length());

  VisitForAccumulatorValue(args->at(0));

  Label materialize_true, materialize_false;
  Label* if_true = nullptr;
  Label* if_false = nullptr;
  Label* fall_through = nullptr;
  context()->PrepareTest(&materialize_true, &materialize_false, &if_true,
                         &if_false, &fall_through);

  __ JumpIfSmi(r2, if_false);
  __ CompareObjectType(r2, r3, r3, JS_DATE_TYPE);
  PrepareForBailoutBeforeSplit(expr, true, if_true, if_false);
  Split(eq, if_true, if_false, fall_through);

  context()->Plug(if_true, if_false);
}


void FullCodeGenerator::EmitDateField(CallRuntime* expr) {
  ZoneList<Expression*>* args = expr->arguments();
  DCHECK(args->length() == 2);
  DCHECK_NOT_NULL(args->at(1)->AsLiteral());
  Smi* index = Smi::cast(*(args->at(1)->AsLiteral()->value()));

  VisitForAccumulatorValue(args->at(0));  // Load the object.

  Register object = r2;
  Register result = r2;
  Register scratch0 = r1;
  Register scratch1 = r3;

  if (index->value() == 0) {
    __ LoadP(result, FieldMemOperand(object, JSDate::kValueOffset));
  } else {
    Label runtime, done;
    if (index->value() < JSDate::kFirstUncachedField) {
      ExternalReference stamp = ExternalReference::date_cache_stamp(isolate());
      __ mov(scratch1, Operand(stamp));
      __ LoadP(scratch1, MemOperand(scratch1));
      __ LoadP(scratch0, FieldMemOperand(object, JSDate::kCacheStampOffset));
      __ CmpP(scratch1, scratch0);
      __ bne(&runtime);
      __ LoadP(result,
               FieldMemOperand(object, JSDate::kValueOffset +
                                           kPointerSize * index->value()),
               scratch0);
      __ b(&done);
    }
    __ bind(&runtime);
    __ PrepareCallCFunction(2, scratch1);
    __ LoadSmiLiteral(r3, index);
    __ CallCFunction(ExternalReference::get_date_field_function(isolate()), 2);
    __ bind(&done);
  }

  context()->Plug(result);
}


void FullCodeGenerator::EmitOneByteSeqStringSetChar(CallRuntime* expr) {
  ZoneList<Expression*>* args = expr->arguments();
  DCHECK_EQ(3, args->length());

  Register string = r2;
  Register index = r3;
  Register value = r4;

  VisitForStackValue(args->at(0));        // index
  VisitForStackValue(args->at(1));        // value
  VisitForAccumulatorValue(args->at(2));  // string
  __ Pop(index, value);

  if (FLAG_debug_code) {
    __ TestIfSmi(value);
    __ Check(eq, kNonSmiValue, cr0);
    __ TestIfSmi(index);
    __ Check(eq, kNonSmiIndex, cr0);
    __ SmiUntag(index);
    static const uint32_t one_byte_seq_type = kSeqStringTag | kOneByteStringTag;
    __ EmitSeqStringSetCharCheck(string, index, value, one_byte_seq_type);
    __ SmiTag(index);
  }

  __ SmiUntag(value);
  __ AddP(ip, string, Operand(SeqOneByteString::kHeaderSize - kHeapObjectTag));
  __ SmiToByteArrayOffset(r1, index);
  __ StoreByte(value, MemOperand(ip, r1));
  context()->Plug(string);
}


void FullCodeGenerator::EmitTwoByteSeqStringSetChar(CallRuntime* expr) {
  ZoneList<Expression*>* args = expr->arguments();
  DCHECK_EQ(3, args->length());

  Register string = r2;
  Register index = r3;
  Register value = r4;

  VisitForStackValue(args->at(0));        // index
  VisitForStackValue(args->at(1));        // value
  VisitForAccumulatorValue(args->at(2));  // string
  __ Pop(index, value);

  if (FLAG_debug_code) {
    __ TestIfSmi(value);
    __ Check(eq, kNonSmiValue, cr0);
    __ TestIfSmi(index);
    __ Check(eq, kNonSmiIndex, cr0);
    __ SmiUntag(index, index);
    static const uint32_t two_byte_seq_type = kSeqStringTag | kTwoByteStringTag;
    __ EmitSeqStringSetCharCheck(string, index, value, two_byte_seq_type);
    __ SmiTag(index, index);
  }

  __ SmiUntag(value);
  __ SmiToShortArrayOffset(r1, index);
  __ StoreHalfWord(value, MemOperand(r1, string,
                          SeqTwoByteString::kHeaderSize - kHeapObjectTag));
  context()->Plug(string);
}


void FullCodeGenerator::EmitMathPow(CallRuntime* expr) {
  // Load the arguments on the stack and call the runtime function.
  ZoneList<Expression*>* args = expr->arguments();
  DCHECK(args->length() == 2);
  VisitForStackValue(args->at(0));
  VisitForStackValue(args->at(1));
  MathPowStub stub(isolate(), MathPowStub::ON_STACK);
  __ CallStub(&stub);
  context()->Plug(r2);
}


void FullCodeGenerator::EmitSetValueOf(CallRuntime* expr) {
  ZoneList<Expression*>* args = expr->arguments();
  DCHECK(args->length() == 2);
  VisitForStackValue(args->at(0));        // Load the object.
  VisitForAccumulatorValue(args->at(1));  // Load the value.
  __ pop(r3);                             // r2 = value. r3 = object.

  Label done;
  // If the object is a smi, return the value.
  __ JumpIfSmi(r3, &done);

  // If the object is not a value type, return the value.
  __ CompareObjectType(r3, r4, r4, JS_VALUE_TYPE);
  __ bne(&done);

  // Store the value.
  __ StoreP(r2, FieldMemOperand(r3, JSValue::kValueOffset));
  // Update the write barrier.  Save the value as it will be
  // overwritten by the write barrier code and is needed afterward.
  __ LoadRR(r4, r2);
  __ RecordWriteField(r3, JSValue::kValueOffset, r4, r5, kLRHasBeenSaved,
                      kDontSaveFPRegs);

  __ bind(&done);
  context()->Plug(r2);
}


void FullCodeGenerator::EmitNumberToString(CallRuntime* expr) {
  ZoneList<Expression*>* args = expr->arguments();
  DCHECK_EQ(args->length(), 1);
  // Load the argument into r2 and call the stub.
  VisitForAccumulatorValue(args->at(0));

  NumberToStringStub stub(isolate());
  __ CallStub(&stub);
  context()->Plug(r2);
}


void FullCodeGenerator::EmitStringCharFromCode(CallRuntime* expr) {
  ZoneList<Expression*>* args = expr->arguments();
  DCHECK(args->length() == 1);
  VisitForAccumulatorValue(args->at(0));

  Label done;
  StringCharFromCodeGenerator generator(r2, r3);
  generator.GenerateFast(masm_);
  __ b(&done);

  NopRuntimeCallHelper call_helper;
  generator.GenerateSlow(masm_, call_helper);

  __ bind(&done);
  context()->Plug(r3);
}


void FullCodeGenerator::EmitStringCharCodeAt(CallRuntime* expr) {
  ZoneList<Expression*>* args = expr->arguments();
  DCHECK(args->length() == 2);
  VisitForStackValue(args->at(0));
  VisitForAccumulatorValue(args->at(1));

  Register object = r3;
  Register index = r2;
  Register result = r5;

  __ pop(object);

  Label need_conversion;
  Label index_out_of_range;
  Label done;
  StringCharCodeAtGenerator generator(object, index, result, &need_conversion,
                                      &need_conversion, &index_out_of_range,
                                      STRING_INDEX_IS_NUMBER);
  generator.GenerateFast(masm_);
  __ b(&done);

  __ bind(&index_out_of_range);
  // When the index is out of range, the spec requires us to return
  // NaN.
  __ LoadRoot(result, Heap::kNanValueRootIndex);
  __ b(&done);

  __ bind(&need_conversion);
  // Load the undefined value into the result register, which will
  // trigger conversion.
  __ LoadRoot(result, Heap::kUndefinedValueRootIndex);
  __ b(&done);

  NopRuntimeCallHelper call_helper;
  generator.GenerateSlow(masm_, NOT_PART_OF_IC_HANDLER, call_helper);

  __ bind(&done);
  context()->Plug(result);
}


void FullCodeGenerator::EmitStringCharAt(CallRuntime* expr) {
  ZoneList<Expression*>* args = expr->arguments();
  DCHECK(args->length() == 2);
  VisitForStackValue(args->at(0));
  VisitForAccumulatorValue(args->at(1));

  Register object = r3;
  Register index = r2;
  Register scratch = r5;
  Register result = r2;

  __ pop(object);

  Label need_conversion;
  Label index_out_of_range;
  Label done;
  StringCharAtGenerator generator(object, index, scratch, result,
                                  &need_conversion, &need_conversion,
                                  &index_out_of_range, STRING_INDEX_IS_NUMBER);
  generator.GenerateFast(masm_);
  __ b(&done);

  __ bind(&index_out_of_range);
  // When the index is out of range, the spec requires us to return
  // the empty string.
  __ LoadRoot(result, Heap::kempty_stringRootIndex);
  __ b(&done);

  __ bind(&need_conversion);
  // Move smi zero into the result register, which will trigger
  // conversion.
  __ LoadSmiLiteral(result, Smi::FromInt(0));
  __ b(&done);

  NopRuntimeCallHelper call_helper;
  generator.GenerateSlow(masm_, NOT_PART_OF_IC_HANDLER, call_helper);

  __ bind(&done);
  context()->Plug(result);
}


void FullCodeGenerator::EmitStringAdd(CallRuntime* expr) {
  ZoneList<Expression*>* args = expr->arguments();
  DCHECK_EQ(2, args->length());
  VisitForStackValue(args->at(0));
  VisitForAccumulatorValue(args->at(1));

  __ pop(r3);
  StringAddStub stub(isolate(), STRING_ADD_CHECK_BOTH, NOT_TENURED);
  __ CallStub(&stub);
  context()->Plug(r2);
}


void FullCodeGenerator::EmitStringCompare(CallRuntime* expr) {
  ZoneList<Expression*>* args = expr->arguments();
  DCHECK_EQ(2, args->length());
  VisitForStackValue(args->at(0));
  VisitForStackValue(args->at(1));

  StringCompareStub stub(isolate());
  __ CallStub(&stub);
  context()->Plug(r2);
}


void FullCodeGenerator::EmitCallFunction(CallRuntime* expr) {
  ZoneList<Expression*>* args = expr->arguments();
  DCHECK(args->length() >= 2);

  int arg_count = args->length() - 2;  // 2 ~ receiver and function.
  for (int i = 0; i < arg_count + 1; i++) {
    VisitForStackValue(args->at(i));
  }
  VisitForAccumulatorValue(args->last());  // Function.

  Label runtime, done;
  // Check for non-function argument (including proxy).
  __ JumpIfSmi(r2, &runtime);
  __ CompareObjectType(r2, r3, r3, JS_FUNCTION_TYPE);
  __ bne(&runtime);

  // InvokeFunction requires the function in r3. Move it in there.
  __ LoadRR(r3, result_register());
  ParameterCount count(arg_count);
  __ InvokeFunction(r3, count, CALL_FUNCTION, NullCallWrapper());
  __ LoadP(cp, MemOperand(fp, StandardFrameConstants::kContextOffset));
  __ b(&done);

  __ bind(&runtime);
  __ push(r2);
  __ CallRuntime(Runtime::kCall, args->length());
  __ bind(&done);

  context()->Plug(r2);
}


void FullCodeGenerator::EmitDefaultConstructorCallSuper(CallRuntime* expr) {
  ZoneList<Expression*>* args = expr->arguments();
  DCHECK(args->length() == 2);

  // new.target
  VisitForStackValue(args->at(0));

  // .this_function
  VisitForStackValue(args->at(1));
  __ CallRuntime(Runtime::kGetPrototype, 1);
  __ LoadRR(r3, result_register());
  __ Push(r3);

  // Check if the calling frame is an arguments adaptor frame.
  Label adaptor_frame, args_set_up, runtime;
  __ LoadP(r4, MemOperand(fp, StandardFrameConstants::kCallerFPOffset));
  __ LoadP(r5, MemOperand(r4, StandardFrameConstants::kContextOffset));
  __ CmpSmiLiteral(r5, Smi::FromInt(StackFrame::ARGUMENTS_ADAPTOR), r1);
  __ beq(&adaptor_frame, Label::kNear);

  // default constructor has no arguments, so no adaptor frame means no args.
  __ LoadImmP(r2, Operand::Zero());
  __ b(&args_set_up);

  // Copy arguments from adaptor frame.
  {
    __ bind(&adaptor_frame);
    __ LoadP(r2, MemOperand(r4, ArgumentsAdaptorFrameConstants::kLengthOffset));
    __ SmiUntag(r2);

    // Get arguments pointer in r4.
    __ ShiftLeftP(r1, r2, Operand(kPointerSizeLog2));
    __ AddP(r4, r1);
    __ AddP(r4, Operand(StandardFrameConstants::kCallerSPOffset));

    Label loop;
    __ LoadRR(r1, r2);
    __ bind(&loop);
    // Pre-decrement in order to skip receiver.
    __ LoadP(r5, MemOperand(r4, -kPointerSize));
    __ lay(r4, MemOperand(r4, -kPointerSize));
    __ Push(r5);
    __ BranchOnCount(r1, &loop);
  }

  __ bind(&args_set_up);
  __ LoadRoot(r4, Heap::kUndefinedValueRootIndex);

  CallConstructStub stub(isolate(), SUPER_CONSTRUCTOR_CALL);
  __ Call(stub.GetCode(), RelocInfo::CONSTRUCT_CALL);

  __ Drop(1);

  context()->Plug(result_register());
}


void FullCodeGenerator::EmitRegExpConstructResult(CallRuntime* expr) {
  RegExpConstructResultStub stub(isolate());
  ZoneList<Expression*>* args = expr->arguments();
  DCHECK(args->length() == 3);
  VisitForStackValue(args->at(0));
  VisitForStackValue(args->at(1));
  VisitForAccumulatorValue(args->at(2));
  __ Pop(r4, r3);
  __ CallStub(&stub);
  context()->Plug(r2);
}


void FullCodeGenerator::EmitGetFromCache(CallRuntime* expr) {
  ZoneList<Expression*>* args = expr->arguments();
  DCHECK_EQ(2, args->length());
  DCHECK_NOT_NULL(args->at(0)->AsLiteral());
  int cache_id = Smi::cast(*(args->at(0)->AsLiteral()->value()))->value();

  Handle<FixedArray> jsfunction_result_caches(
      isolate()->native_context()->jsfunction_result_caches());
  if (jsfunction_result_caches->length() <= cache_id) {
    __ Abort(kAttemptToUseUndefinedCache);
    __ LoadRoot(r2, Heap::kUndefinedValueRootIndex);
    context()->Plug(r2);
    return;
  }

  VisitForAccumulatorValue(args->at(1));

  Register key = r2;
  Register cache = r3;
  __ LoadP(cache, ContextOperand(cp, Context::GLOBAL_OBJECT_INDEX));
  __ LoadP(cache, FieldMemOperand(cache, GlobalObject::kNativeContextOffset));
  __ LoadP(cache,
           ContextOperand(cache, Context::JSFUNCTION_RESULT_CACHES_INDEX));
  __ LoadP(cache,
         FieldMemOperand(cache, FixedArray::OffsetOfElementAt(cache_id)), r0);

  Label done, not_found;
  __ LoadP(r4, FieldMemOperand(cache, JSFunctionResultCache::kFingerOffset));
  // r4 now holds finger offset as a smi.
  __ AddP(r5, cache, Operand(FixedArray::kHeaderSize - kHeapObjectTag));
  // r5 now points to the start of fixed array elements.
  __ SmiToPtrArrayOffset(r4, r4);
  __ LoadP(r4, MemOperand(r5, r4));
  __ lay(r4, MemOperand(r5, r4));

  // r5 now points to the key of the pair.
  __ CmpP(key, r4);
  __ bne(&not_found, Label::kNear);

  __ LoadP(r2, MemOperand(r5, kPointerSize));
  __ b(&done);

  __ bind(&not_found);
  // Call runtime to perform the lookup.
  __ Push(cache, key);
  __ CallRuntime(Runtime::kGetFromCacheRT, 2);

  __ bind(&done);
  context()->Plug(r2);
}


void FullCodeGenerator::EmitHasCachedArrayIndex(CallRuntime* expr) {
  ZoneList<Expression*>* args = expr->arguments();
  VisitForAccumulatorValue(args->at(0));

  Label materialize_true, materialize_false;
  Label* if_true = NULL;
  Label* if_false = NULL;
  Label* fall_through = NULL;
  context()->PrepareTest(&materialize_true, &materialize_false, &if_true,
                         &if_false, &fall_through);

  __ LoadlW(r2, FieldMemOperand(r2, String::kHashFieldOffset));
  __ AndP(r0, r2, Operand(String::kContainsCachedArrayIndexMask));
  __ CmpP(r0, Operand::Zero());
  PrepareForBailoutBeforeSplit(expr, true, if_true, if_false);
  Split(eq, if_true, if_false, fall_through);

  context()->Plug(if_true, if_false);
}


void FullCodeGenerator::EmitGetCachedArrayIndex(CallRuntime* expr) {
  ZoneList<Expression*>* args = expr->arguments();
  DCHECK(args->length() == 1);
  VisitForAccumulatorValue(args->at(0));

  __ AssertString(r2);

  __ LoadlW(r2, FieldMemOperand(r2, String::kHashFieldOffset));
  __ IndexFromHash(r2, r2);

  context()->Plug(r2);
}


void FullCodeGenerator::EmitFastOneByteArrayJoin(CallRuntime* expr) {
  Label bailout, done, one_char_separator, long_separator, non_trivial_array,
      not_size_one_array, loop, empty_separator_loop, one_char_separator_loop,
      one_char_separator_loop_entry, long_separator_loop;
  ZoneList<Expression*>* args = expr->arguments();
  DCHECK(args->length() == 2);
  VisitForStackValue(args->at(1));
  VisitForAccumulatorValue(args->at(0));

  // All aliases of the same register have disjoint lifetimes.
  Register array = r2;
  Register elements = no_reg;  // Will be r2.
  Register result = no_reg;    // Will be r2.
  Register separator = r3;
  Register array_length = r4;
  Register result_pos = no_reg;  // Will be r4
  Register string_length = r5;
  Register string = r6;
  Register element = r7;
  Register elements_end = r8;
  Register scratch1 = r9;
  Register scratch2 = r1;

  // Separator operand is on the stack.
  __ pop(separator);

  // Check that the array is a JSArray.
  __ JumpIfSmi(array, &bailout);
  __ CompareObjectType(array, scratch1, scratch2, JS_ARRAY_TYPE);
  __ bne(&bailout);

  // Check that the array has fast elements.
  __ CheckFastElements(scratch1, scratch2, &bailout);

  // If the array has length zero, return the empty string.
  __ LoadP(array_length, FieldMemOperand(array, JSArray::kLengthOffset));
  __ SmiUntag(array_length);
  __ CmpP(array_length, Operand::Zero());
  __ bne(&non_trivial_array, Label::kNear);
  __ LoadRoot(r2, Heap::kempty_stringRootIndex);
  __ b(&done);

  __ bind(&non_trivial_array);

  // Get the FixedArray containing array's elements.
  elements = array;
  __ LoadP(elements, FieldMemOperand(array, JSArray::kElementsOffset));
  array = no_reg;  // End of array's live range.

  // Check that all array elements are sequential one-byte strings, and
  // accumulate the sum of their lengths, as a smi-encoded value.
  __ LoadImmP(string_length, Operand::Zero());
  __ AddP(element, elements, Operand(FixedArray::kHeaderSize - kHeapObjectTag));
  __ ShiftLeftP(elements_end, array_length, Operand(kPointerSizeLog2));
  __ AddP(elements_end, element);
  // Loop condition: while (element < elements_end).
  // Live values in registers:
  //   elements: Fixed array of strings.
  //   array_length: Length of the fixed array of strings (not smi)
  //   separator: Separator string
  //   string_length: Accumulated sum of string lengths (smi).
  //   element: Current array element.
  //   elements_end: Array end.
  if (generate_debug_code_) {
    __ CmpP(array_length, Operand::Zero());
    __ Assert(gt, kNoEmptyArraysHereInEmitFastOneByteArrayJoin);
  }
  __ bind(&loop);
  __ LoadP(string, MemOperand(element));
  __ AddP(element, Operand(kPointerSize));
  __ JumpIfSmi(string, &bailout);
  __ LoadP(scratch1, FieldMemOperand(string, HeapObject::kMapOffset));
  __ LoadlB(scratch1, FieldMemOperand(scratch1, Map::kInstanceTypeOffset));
  __ JumpIfInstanceTypeIsNotSequentialOneByte(scratch1, scratch2, &bailout);
  __ LoadP(scratch1, FieldMemOperand(string, SeqOneByteString::kLengthOffset));

  __ AddAndCheckForOverflow(string_length, string_length, scratch1, scratch2,
                            r0);
  __ BranchOnOverflow(&bailout);

  __ CmpP(element, elements_end);
  __ blt(&loop);

  // If array_length is 1, return elements[0], a string.
  __ CmpP(array_length, Operand(1));
  __ bne(&not_size_one_array, Label::kNear);
  __ LoadP(r2, FieldMemOperand(elements, FixedArray::kHeaderSize));
  __ b(&done);

  __ bind(&not_size_one_array);

  // Live values in registers:
  //   separator: Separator string
  //   array_length: Length of the array.
  //   string_length: Sum of string lengths (smi).
  //   elements: FixedArray of strings.

  // Check that the separator is a flat one-byte string.
  __ JumpIfSmi(separator, &bailout);
  __ LoadP(scratch1, FieldMemOperand(separator, HeapObject::kMapOffset));
  __ LoadlB(scratch1, FieldMemOperand(scratch1, Map::kInstanceTypeOffset));
  __ JumpIfInstanceTypeIsNotSequentialOneByte(scratch1, scratch2, &bailout);

  // Add (separator length times array_length) - separator length to the
  // string_length to get the length of the result string.
  __ LoadP(scratch1,
           FieldMemOperand(separator, SeqOneByteString::kLengthOffset));
  __ SubP(string_length, string_length, scratch1);
#if V8_TARGET_ARCH_S390X
  __ SmiUntag(scratch1, scratch1);
  __ LoadRR(scratch2, array_length);
  __ mr_z(r0, scratch1);  // r0:r1 = r1 * scratch1
  // Check for smi overflow. No overflow if higher 33 bits of 64-bit result are
  // zero.
  __ lr(ip, r1);
  __ sra(ip, Operand(31));
  __ cr_z(ip, r0);
  // TODO(JOHN): The above 3 instr expended from 31-bit TestIfInt32
  // __ TestIfInt32(scratch2, scratch1, ip);
  __ bne(&bailout /*, cr0*/);
  __ SmiTag(scratch2, scratch2);
#else
  // array_length is not smi but the other values are, so the result is a smi
  __ LoadRR(scratch2, array_length);           // scratch2 = r1
  __ mr_z(r0, scratch1);  // r0:r1 = r1 * scratch1
  // Check for smi overflow. No overflow if higher 33 bits of 64-bit result are
  // zero.
  __ CmpP(r0, Operand::Zero());
  __ bne(&bailout);
  // TODO(john): TestIfInt32 expanded into the following two instructions
  __ ShiftRightArith(ip, r0, Operand(31));
  __ CmpP(ip, scratch2);
  // __ TestSignBit32(scratch2, r0);
  __ bne(&bailout /*, cr0*/);
#endif

  __ AddAndCheckForOverflow(string_length, string_length, scratch2, scratch1,
                            r0);
  __ BranchOnOverflow(&bailout);
  __ SmiUntag(string_length);

  // Get first element in the array to free up the elements register to be used
  // for the result.
  __ AddP(element, elements, Operand(FixedArray::kHeaderSize - kHeapObjectTag));
  result = elements;  // End of live range for elements.
  elements = no_reg;
  // Live values in registers:
  //   element: First array element
  //   separator: Separator string
  //   string_length: Length of result string (not smi)
  //   array_length: Length of the array.
  __ AllocateOneByteString(result, string_length, scratch1, scratch2,
                           elements_end, &bailout);
  // Prepare for looping. Set up elements_end to end of the array. Set
  // result_pos to the position of the result where to write the first
  // character.
  __ ShiftLeftP(elements_end, array_length, Operand(kPointerSizeLog2));
  __ AddP(elements_end, element);
  result_pos = array_length;  // End of live range for array_length.
  array_length = no_reg;
  __ AddP(result_pos, result,
          Operand(SeqOneByteString::kHeaderSize - kHeapObjectTag));

  // Check the length of the separator.
  __ LoadP(scratch1,
           FieldMemOperand(separator, SeqOneByteString::kLengthOffset));
  __ CmpSmiLiteral(scratch1, Smi::FromInt(1), r0);
  __ beq(&one_char_separator);
  __ bgt(&long_separator);

  // Empty separator case
  __ bind(&empty_separator_loop);
  // Live values in registers:
  //   result_pos: the position to which we are currently copying characters.
  //   element: Current array element.
  //   elements_end: Array end.

  // Copy next array element to the result.
  __ LoadP(string, MemOperand(element));
  __ AddP(element, Operand(kPointerSize));
  __ LoadP(string_length, FieldMemOperand(string, String::kLengthOffset));
  __ SmiUntag(string_length);
  __ AddP(string, string,
          Operand(SeqOneByteString::kHeaderSize - kHeapObjectTag));
  __ CopyBytes(string, result_pos, string_length, scratch1);
  __ CmpP(element, elements_end);
  __ blt(&empty_separator_loop);  // End while (element < elements_end).
  DCHECK(result.is(r2));
  __ b(&done);

  // One-character separator case
  __ bind(&one_char_separator);
  // Replace separator with its one-byte character value.
  __ LoadlB(separator,
        FieldMemOperand(separator, SeqOneByteString::kHeaderSize));
  // Jump into the loop after the code that copies the separator, so the first
  // element is not preceded by a separator
  __ b(&one_char_separator_loop_entry);

  __ bind(&one_char_separator_loop);
  // Live values in registers:
  //   result_pos: the position to which we are currently copying characters.
  //   element: Current array element.
  //   elements_end: Array end.
  //   separator: Single separator one-byte char (in lower byte).

  // Copy the separator character to the result.
  __ stc(separator, MemOperand(result_pos));
  __ AddP(result_pos, Operand(1));

  // Copy next array element to the result.
  __ bind(&one_char_separator_loop_entry);
  __ LoadP(string, MemOperand(element));
  __ AddP(element, Operand(kPointerSize));
  __ LoadP(string_length, FieldMemOperand(string, String::kLengthOffset));
  __ SmiUntag(string_length);
  __ AddP(string,
          Operand(SeqOneByteString::kHeaderSize - kHeapObjectTag));
  __ CopyBytes(string, result_pos, string_length, scratch1);
  __ CmpLogicalP(element, elements_end);
  __ blt(&one_char_separator_loop);  // End while (element < elements_end).
  DCHECK(result.is(r2));
  __ b(&done);

  // Long separator case (separator is more than one character). Entry is at the
  // label long_separator below.
  __ bind(&long_separator_loop);
  // Live values in registers:
  //   result_pos: the position to which we are currently copying characters.
  //   element: Current array element.
  //   elements_end: Array end.
  //   separator: Separator string.

  // Copy the separator to the result.
  __ LoadP(string_length, FieldMemOperand(separator, String::kLengthOffset));
  __ SmiUntag(string_length);
  __ AddP(string, separator,
          Operand(SeqOneByteString::kHeaderSize - kHeapObjectTag));
  __ CopyBytes(string, result_pos, string_length, scratch1);

  __ bind(&long_separator);
  __ LoadP(string, MemOperand(element));
  __ AddP(element, Operand(kPointerSize));
  __ LoadP(string_length, FieldMemOperand(string, String::kLengthOffset));
  __ SmiUntag(string_length);
  __ AddP(string,
          Operand(SeqOneByteString::kHeaderSize - kHeapObjectTag));
  __ CopyBytes(string, result_pos, string_length, scratch1);
  __ CmpLogicalP(element, elements_end);
  __ blt(&long_separator_loop);  // End while (element < elements_end).
  DCHECK(result.is(r2));
  __ b(&done);

  __ bind(&bailout);
  __ LoadRoot(r2, Heap::kUndefinedValueRootIndex);
  __ bind(&done);
  context()->Plug(r2);
}


void FullCodeGenerator::EmitDebugIsActive(CallRuntime* expr) {
  DCHECK(expr->arguments()->length() == 0);
  ExternalReference debug_is_active =
      ExternalReference::debug_is_active_address(isolate());
  __ mov(ip, Operand(debug_is_active));
  __ LoadlB(r2, MemOperand(ip));
  __ SmiTag(r2);
  context()->Plug(r2);
}


void FullCodeGenerator::EmitCallSuperWithSpread(CallRuntime* expr) {
  // Assert: expr === CallRuntime("ReflectConstruct")
  DCHECK_EQ(1, expr->arguments()->length());
  CallRuntime* call = expr->arguments()->at(0)->AsCallRuntime();

  ZoneList<Expression*>* args = call->arguments();
  DCHECK_EQ(3, args->length());

  SuperCallReference* super_call_ref = args->at(0)->AsSuperCallReference();
  DCHECK_NOT_NULL(super_call_ref);

  // Load ReflectConstruct function
  EmitLoadJSRuntimeFunction(call);

  // Push the target function under the receiver.
  __ LoadP(r0, MemOperand(sp, 0));
  __ push(r0);
  __ StoreP(r2, MemOperand(sp, kPointerSize));

  // Push super constructor
  EmitLoadSuperConstructor(super_call_ref);
  __ Push(result_register());

  // Push arguments array
  VisitForStackValue(args->at(1));

  // Push NewTarget
  DCHECK(args->at(2)->IsVariableProxy());
  VisitForStackValue(args->at(2));

  EmitCallJSRuntimeFunction(call);

  // Restore context register.
  __ LoadP(cp, MemOperand(fp, StandardFrameConstants::kContextOffset));
  context()->DropAndPlug(1, r2);

  // TODO(mvstanton): with FLAG_vector_stores this needs a slot id.
  EmitInitializeThisAfterSuper(super_call_ref);
}


void FullCodeGenerator::EmitLoadJSRuntimeFunction(CallRuntime* expr) {
  // Push the builtins object as the receiver.
  Register receiver = LoadDescriptor::ReceiverRegister();
  __ LoadP(receiver, GlobalObjectOperand());
  __ LoadP(receiver, FieldMemOperand(receiver, GlobalObject::kBuiltinsOffset));
  __ push(receiver);

  // Load the function from the receiver.
  __ mov(LoadDescriptor::NameRegister(), Operand(expr->name()));
  __ mov(LoadDescriptor::SlotRegister(),
         Operand(SmiFromSlot(expr->CallRuntimeFeedbackSlot())));
  CallLoadIC(NOT_CONTEXTUAL);
}


void FullCodeGenerator::EmitCallJSRuntimeFunction(CallRuntime* expr) {
  ZoneList<Expression*>* args = expr->arguments();
  int arg_count = args->length();

  SetExpressionPosition(expr);
  CallFunctionStub stub(isolate(), arg_count, NO_CALL_FUNCTION_FLAGS);
  __ LoadP(r3, MemOperand(sp, (arg_count + 1) * kPointerSize), r0);
  __ CallStub(&stub);
}


void FullCodeGenerator::VisitCallRuntime(CallRuntime* expr) {
  ZoneList<Expression*>* args = expr->arguments();
  int arg_count = args->length();

  if (expr->is_jsruntime()) {
    Comment cmnt(masm_, "[ CallRuntime");
    EmitLoadJSRuntimeFunction(expr);

    // Push the target function under the receiver.
    __ LoadP(ip, MemOperand(sp, 0));
    __ push(ip);
    __ StoreP(r2, MemOperand(sp, kPointerSize));

    // Push the arguments ("left-to-right").
    for (int i = 0; i < arg_count; i++) {
      VisitForStackValue(args->at(i));
    }

    PrepareForBailoutForId(expr->CallId(), NO_REGISTERS);
    EmitCallJSRuntimeFunction(expr);

    // Restore context register.
    __ LoadP(cp, MemOperand(fp, StandardFrameConstants::kContextOffset));

    context()->DropAndPlug(1, r2);

  } else {
    const Runtime::Function* function = expr->function();
    switch (function->function_id) {
#define CALL_INTRINSIC_GENERATOR(Name)     \
  case Runtime::kInline##Name: {           \
    Comment cmnt(masm_, "[ Inline" #Name); \
    return Emit##Name(expr);               \
  }
      FOR_EACH_FULL_CODE_INTRINSIC(CALL_INTRINSIC_GENERATOR)
#undef CALL_INTRINSIC_GENERATOR
      default: {
        Comment cmnt(masm_, "[ CallRuntime for unhandled intrinsic");
        // Push the arguments ("left-to-right").
        for (int i = 0; i < arg_count; i++) {
          VisitForStackValue(args->at(i));
        }

        // Call the C runtime function.
        PrepareForBailoutForId(expr->CallId(), NO_REGISTERS);
        __ CallRuntime(expr->function(), arg_count);
        context()->Plug(r2);
      }
    }
  }
}


void FullCodeGenerator::VisitUnaryOperation(UnaryOperation* expr) {
  switch (expr->op()) {
    case Token::DELETE: {
      Comment cmnt(masm_, "[ UnaryOperation (DELETE)");
      Property* property = expr->expression()->AsProperty();
      VariableProxy* proxy = expr->expression()->AsVariableProxy();

      if (property != NULL) {
        VisitForStackValue(property->obj());
        VisitForStackValue(property->key());
        __ LoadSmiLiteral(r3, Smi::FromInt(language_mode()));
        __ push(r3);
        __ InvokeBuiltin(Builtins::DELETE, CALL_FUNCTION);
        context()->Plug(r2);
      } else if (proxy != NULL) {
        Variable* var = proxy->var();
        // Delete of an unqualified identifier is disallowed in strict mode but
        // "delete this" is allowed.
        bool is_this = var->HasThisName(isolate());
        DCHECK(is_sloppy(language_mode()) || is_this);
        if (var->IsUnallocatedOrGlobalSlot()) {
          __ LoadP(r4, GlobalObjectOperand());
          __ mov(r3, Operand(var->name()));
          __ LoadSmiLiteral(r2, Smi::FromInt(SLOPPY));
          __ Push(r4, r3, r2);
          __ InvokeBuiltin(Builtins::DELETE, CALL_FUNCTION);
          context()->Plug(r2);
        } else if (var->IsStackAllocated() || var->IsContextSlot()) {
          // Result of deleting non-global, non-dynamic variables is false.
          // The subexpression does not have side effects.
          context()->Plug(is_this);
        } else {
          // Non-global variable.  Call the runtime to try to delete from the
          // context where the variable was introduced.
          DCHECK(!context_register().is(r4));
          __ mov(r4, Operand(var->name()));
          __ Push(context_register(), r4);
          __ CallRuntime(Runtime::kDeleteLookupSlot, 2);
          context()->Plug(r2);
        }
      } else {
        // Result of deleting non-property, non-variable reference is true.
        // The subexpression may have side effects.
        VisitForEffect(expr->expression());
        context()->Plug(true);
      }
      break;
    }

    case Token::VOID: {
      Comment cmnt(masm_, "[ UnaryOperation (VOID)");
      VisitForEffect(expr->expression());
      context()->Plug(Heap::kUndefinedValueRootIndex);
      break;
    }

    case Token::NOT: {
      Comment cmnt(masm_, "[ UnaryOperation (NOT)");
      if (context()->IsEffect()) {
        // Unary NOT has no side effects so it's only necessary to visit the
        // subexpression.  Match the optimizing compiler by not branching.
        VisitForEffect(expr->expression());
      } else if (context()->IsTest()) {
        const TestContext* test = TestContext::cast(context());
        // The labels are swapped for the recursive call.
        VisitForControl(expr->expression(), test->false_label(),
                        test->true_label(), test->fall_through());
        context()->Plug(test->true_label(), test->false_label());
      } else {
        // We handle value contexts explicitly rather than simply visiting
        // for control and plugging the control flow into the context,
        // because we need to prepare a pair of extra administrative AST ids
        // for the optimizing compiler.
        DCHECK(context()->IsAccumulatorValue() || context()->IsStackValue());
        Label materialize_true, materialize_false, done;
        VisitForControl(expr->expression(), &materialize_false,
                        &materialize_true, &materialize_true);
        __ bind(&materialize_true);
        PrepareForBailoutForId(expr->MaterializeTrueId(), NO_REGISTERS);
        __ LoadRoot(r2, Heap::kTrueValueRootIndex);
        if (context()->IsStackValue()) __ push(r2);
        __ b(&done);
        __ bind(&materialize_false);
        PrepareForBailoutForId(expr->MaterializeFalseId(), NO_REGISTERS);
        __ LoadRoot(r2, Heap::kFalseValueRootIndex);
        if (context()->IsStackValue()) __ push(r2);
        __ bind(&done);
      }
      break;
    }

    case Token::TYPEOF: {
      Comment cmnt(masm_, "[ UnaryOperation (TYPEOF)");
      {
        AccumulatorValueContext context(this);
        VisitForTypeofValue(expr->expression());
      }
      __ LoadRR(r5, r2);
      TypeofStub typeof_stub(isolate());
      __ CallStub(&typeof_stub);
      context()->Plug(r2);
      break;
    }

    default:
      UNREACHABLE();
  }
}


void FullCodeGenerator::VisitCountOperation(CountOperation* expr) {
  DCHECK(expr->expression()->IsValidReferenceExpression());

  Comment cmnt(masm_, "[ CountOperation");

  Property* prop = expr->expression()->AsProperty();
  LhsKind assign_type = Property::GetAssignType(prop);

  // Evaluate expression and get value.
  if (assign_type == VARIABLE) {
    DCHECK(expr->expression()->AsVariableProxy()->var() != NULL);
    AccumulatorValueContext context(this);
    EmitVariableLoad(expr->expression()->AsVariableProxy());
  } else {
    // Reserve space for result of postfix operation.
    if (expr->is_postfix() && !context()->IsEffect()) {
      __ LoadSmiLiteral(ip, Smi::FromInt(0));
      __ push(ip);
    }
    switch (assign_type) {
      case NAMED_PROPERTY: {
        // Put the object both on the stack and in the register.
        VisitForStackValue(prop->obj());
        __ LoadP(LoadDescriptor::ReceiverRegister(), MemOperand(sp, 0));
        EmitNamedPropertyLoad(prop);
        break;
      }

      case NAMED_SUPER_PROPERTY: {
        VisitForStackValue(prop->obj()->AsSuperPropertyReference()->this_var());
        VisitForAccumulatorValue(
            prop->obj()->AsSuperPropertyReference()->home_object());
        __ Push(result_register());
        const Register scratch = r3;
        __ LoadP(scratch, MemOperand(sp, kPointerSize));
        __ Push(scratch, result_register());
        EmitNamedSuperPropertyLoad(prop);
        break;
      }

      case KEYED_SUPER_PROPERTY: {
        VisitForStackValue(prop->obj()->AsSuperPropertyReference()->this_var());
        VisitForAccumulatorValue(
            prop->obj()->AsSuperPropertyReference()->home_object());
        const Register scratch = r3;
        const Register scratch1 = r4;
        __ LoadRR(scratch, result_register());
        VisitForAccumulatorValue(prop->key());
        __ Push(scratch, result_register());
        __ LoadP(scratch1, MemOperand(sp, 2 * kPointerSize));
        __ Push(scratch1, scratch, result_register());
        EmitKeyedSuperPropertyLoad(prop);
        break;
      }

      case KEYED_PROPERTY: {
        VisitForStackValue(prop->obj());
        VisitForStackValue(prop->key());
        __ LoadP(LoadDescriptor::ReceiverRegister(),
                 MemOperand(sp, 1 * kPointerSize));
        __ LoadP(LoadDescriptor::NameRegister(), MemOperand(sp, 0));
        EmitKeyedPropertyLoad(prop);
        break;
      }

      case VARIABLE:
        UNREACHABLE();
    }
  }

  // We need a second deoptimization point after loading the value
  // in case evaluating the property load my have a side effect.
  if (assign_type == VARIABLE) {
    PrepareForBailout(expr->expression(), TOS_REG);
  } else {
    PrepareForBailoutForId(prop->LoadId(), TOS_REG);
  }

  // Inline smi case if we are in a loop.
  Label stub_call, done;
  JumpPatchSite patch_site(masm_);

  int count_value = expr->op() == Token::INC ? 1 : -1;
  if (ShouldInlineSmiCase(expr->op())) {
    Label slow;
    patch_site.EmitJumpIfNotSmi(r2, &slow);

    // Save result for postfix expressions.
    if (expr->is_postfix()) {
      if (!context()->IsEffect()) {
        // Save the result on the stack. If we have a named or keyed property
        // we store the result under the receiver that is currently on top
        // of the stack.
        switch (assign_type) {
          case VARIABLE:
            __ push(r2);
            break;
          case NAMED_PROPERTY:
            __ StoreP(r2, MemOperand(sp, kPointerSize));
            break;
          case NAMED_SUPER_PROPERTY:
            __ StoreP(r2, MemOperand(sp, 2 * kPointerSize));
            break;
          case KEYED_PROPERTY:
            __ StoreP(r2, MemOperand(sp, 2 * kPointerSize));
            break;
          case KEYED_SUPER_PROPERTY:
            __ StoreP(r2, MemOperand(sp, 3 * kPointerSize));
            break;
        }
      }
    }

    Register scratch1 = r3;
    Register scratch2 = r4;
    __ LoadSmiLiteral(scratch1, Smi::FromInt(count_value));
    __ AddAndCheckForOverflow(r2, r2, scratch1, scratch2, r0);
    __ BranchOnNoOverflow(&done);
    // Call stub. Undo operation first.
    __ SubP(r2, r2, scratch1);
    __ b(&stub_call);
    __ bind(&slow);
  }
  if (!is_strong(language_mode())) {
    ToNumberStub convert_stub(isolate());
    __ CallStub(&convert_stub);
    PrepareForBailoutForId(expr->ToNumberId(), TOS_REG);
  }

  // Save result for postfix expressions.
  if (expr->is_postfix()) {
    if (!context()->IsEffect()) {
      // Save the result on the stack. If we have a named or keyed property
      // we store the result under the receiver that is currently on top
      // of the stack.
      switch (assign_type) {
        case VARIABLE:
          __ push(r2);
          break;
        case NAMED_PROPERTY:
          __ StoreP(r2, MemOperand(sp, kPointerSize));
          break;
        case NAMED_SUPER_PROPERTY:
          __ StoreP(r2, MemOperand(sp, 2 * kPointerSize));
          break;
        case KEYED_PROPERTY:
          __ StoreP(r2, MemOperand(sp, 2 * kPointerSize));
          break;
        case KEYED_SUPER_PROPERTY:
          __ StoreP(r2, MemOperand(sp, 3 * kPointerSize));
          break;
      }
    }
  }

  __ bind(&stub_call);
  __ LoadRR(r3, r2);
  __ LoadSmiLiteral(r2, Smi::FromInt(count_value));

  SetExpressionPosition(expr);

  Handle<Code> code = CodeFactory::BinaryOpIC(isolate(), Token::ADD,
                                              strength(language_mode())).code();
  CallIC(code, expr->CountBinOpFeedbackId());
  patch_site.EmitPatchInfo();
  __ bind(&done);

  if (is_strong(language_mode())) {
    PrepareForBailoutForId(expr->ToNumberId(), TOS_REG);
  }
  // Store the value returned in r2.
  switch (assign_type) {
    case VARIABLE:
      if (expr->is_postfix()) {
        {
          EffectContext context(this);
          EmitVariableAssignment(expr->expression()->AsVariableProxy()->var(),
                                 Token::ASSIGN, expr->CountSlot());
          PrepareForBailoutForId(expr->AssignmentId(), TOS_REG);
          context.Plug(r2);
        }
        // For all contexts except EffectConstant We have the result on
        // top of the stack.
        if (!context()->IsEffect()) {
          context()->PlugTOS();
        }
      } else {
        EmitVariableAssignment(expr->expression()->AsVariableProxy()->var(),
                               Token::ASSIGN, expr->CountSlot());
        PrepareForBailoutForId(expr->AssignmentId(), TOS_REG);
        context()->Plug(r2);
      }
      break;
    case NAMED_PROPERTY: {
      __ mov(StoreDescriptor::NameRegister(),
             Operand(prop->key()->AsLiteral()->value()));
      __ pop(StoreDescriptor::ReceiverRegister());
      if (FLAG_vector_stores) {
        EmitLoadStoreICSlot(expr->CountSlot());
        CallStoreIC();
      } else {
        CallStoreIC(expr->CountStoreFeedbackId());
      }
      PrepareForBailoutForId(expr->AssignmentId(), TOS_REG);
      if (expr->is_postfix()) {
        if (!context()->IsEffect()) {
          context()->PlugTOS();
        }
      } else {
        context()->Plug(r2);
      }
      break;
    }
    case NAMED_SUPER_PROPERTY: {
      EmitNamedSuperPropertyStore(prop);
      if (expr->is_postfix()) {
        if (!context()->IsEffect()) {
          context()->PlugTOS();
        }
      } else {
        context()->Plug(r2);
      }
      break;
    }
    case KEYED_SUPER_PROPERTY: {
      EmitKeyedSuperPropertyStore(prop);
      if (expr->is_postfix()) {
        if (!context()->IsEffect()) {
          context()->PlugTOS();
        }
      } else {
        context()->Plug(r2);
      }
      break;
    }
    case KEYED_PROPERTY: {
      __ Pop(StoreDescriptor::ReceiverRegister(),
             StoreDescriptor::NameRegister());
      Handle<Code> ic =
          CodeFactory::KeyedStoreIC(isolate(), language_mode()).code();
      if (FLAG_vector_stores) {
        EmitLoadStoreICSlot(expr->CountSlot());
        CallIC(ic);
      } else {
        CallIC(ic, expr->CountStoreFeedbackId());
      }
      PrepareForBailoutForId(expr->AssignmentId(), TOS_REG);
      if (expr->is_postfix()) {
        if (!context()->IsEffect()) {
          context()->PlugTOS();
        }
      } else {
        context()->Plug(r2);
      }
      break;
    }
  }
}


void FullCodeGenerator::EmitLiteralCompareTypeof(Expression* expr,
                                                 Expression* sub_expr,
                                                 Handle<String> check) {
  Label materialize_true, materialize_false;
  Label* if_true = NULL;
  Label* if_false = NULL;
  Label* fall_through = NULL;
  context()->PrepareTest(&materialize_true, &materialize_false, &if_true,
                         &if_false, &fall_through);

  {
    AccumulatorValueContext context(this);
    VisitForTypeofValue(sub_expr);
  }
  PrepareForBailoutBeforeSplit(expr, true, if_true, if_false);

  Factory* factory = isolate()->factory();
  if (String::Equals(check, factory->number_string())) {
    __ JumpIfSmi(r2, if_true);
    __ LoadP(r2, FieldMemOperand(r2, HeapObject::kMapOffset));
    __ CompareRoot(r2, Heap::kHeapNumberMapRootIndex);
    Split(eq, if_true, if_false, fall_through);
  } else if (String::Equals(check, factory->string_string())) {
    __ JumpIfSmi(r2, if_false);
    // Check for undetectable objects => false.
    __ CompareObjectType(r2, r2, r3, FIRST_NONSTRING_TYPE);
    __ bge(if_false);
    __ tm(FieldMemOperand(r2, Map::kBitFieldOffset),
          Operand(1 << Map::kIsUndetectable));
    Split(eq, if_true, if_false, fall_through, cr0);
  } else if (String::Equals(check, factory->symbol_string())) {
    __ JumpIfSmi(r2, if_false);
    __ CompareObjectType(r2, r2, r3, SYMBOL_TYPE);
    Split(eq, if_true, if_false, fall_through);
  } else if (String::Equals(check, factory->boolean_string())) {
    __ CompareRoot(r2, Heap::kTrueValueRootIndex);
    __ beq(if_true);
    __ CompareRoot(r2, Heap::kFalseValueRootIndex);
    Split(eq, if_true, if_false, fall_through);
  } else if (String::Equals(check, factory->undefined_string())) {
    __ CompareRoot(r2, Heap::kUndefinedValueRootIndex);
    __ beq(if_true);
    __ JumpIfSmi(r2, if_false);
    // Check for undetectable objects => true.
    __ LoadP(r2, FieldMemOperand(r2, HeapObject::kMapOffset));
    __ tm(FieldMemOperand(r2, Map::kBitFieldOffset),
          Operand(1 << Map::kIsUndetectable));
    Split(ne, if_true, if_false, fall_through, cr0);

  } else if (String::Equals(check, factory->function_string())) {
    __ JumpIfSmi(r2, if_false);
    STATIC_ASSERT(NUM_OF_CALLABLE_SPEC_OBJECT_TYPES == 2);
    __ CompareObjectType(r2, r2, r3, JS_FUNCTION_TYPE);
    __ beq(if_true);
    __ CmpP(r3, Operand(JS_FUNCTION_PROXY_TYPE));
    Split(eq, if_true, if_false, fall_through);
  } else if (String::Equals(check, factory->object_string())) {
    __ JumpIfSmi(r2, if_false);
    __ CompareRoot(r2, Heap::kNullValueRootIndex);
    __ beq(if_true);
    // Check for JS objects => true.
    __ CompareObjectType(r2, r2, r3, FIRST_NONCALLABLE_SPEC_OBJECT_TYPE);
    __ blt(if_false);
    __ CompareInstanceType(r2, r3, LAST_NONCALLABLE_SPEC_OBJECT_TYPE);
    __ bgt(if_false);
    // Check for undetectable objects => false.
    __ tm(FieldMemOperand(r2, Map::kBitFieldOffset),
          Operand(1 << Map::kIsUndetectable));
    Split(eq, if_true, if_false, fall_through, cr0);
  } else {
    if (if_false != fall_through) __ b(if_false);
  }
  context()->Plug(if_true, if_false);
}


void FullCodeGenerator::VisitCompareOperation(CompareOperation* expr) {
  Comment cmnt(masm_, "[ CompareOperation");
  SetExpressionPosition(expr);

  // First we try a fast inlined version of the compare when one of
  // the operands is a literal.
  if (TryLiteralCompare(expr)) return;

  // Always perform the comparison for its control flow.  Pack the result
  // into the expression's context after the comparison is performed.
  Label materialize_true, materialize_false;
  Label* if_true = NULL;
  Label* if_false = NULL;
  Label* fall_through = NULL;
  context()->PrepareTest(&materialize_true, &materialize_false, &if_true,
                         &if_false, &fall_through);

  Token::Value op = expr->op();
  VisitForStackValue(expr->left());
  switch (op) {
    case Token::IN:
      VisitForStackValue(expr->right());
      __ InvokeBuiltin(Builtins::IN, CALL_FUNCTION);
      PrepareForBailoutBeforeSplit(expr, false, NULL, NULL);
      __ CompareRoot(r2, Heap::kTrueValueRootIndex);
      Split(eq, if_true, if_false, fall_through);
      break;

    case Token::INSTANCEOF: {
      VisitForStackValue(expr->right());
      InstanceofStub stub(isolate(), InstanceofStub::kNoFlags);
      __ CallStub(&stub);
      PrepareForBailoutBeforeSplit(expr, true, if_true, if_false);
      // The stub returns 0 for true.
      __ CmpP(r2, Operand::Zero());
      Split(eq, if_true, if_false, fall_through);
      break;
    }

    default: {
      VisitForAccumulatorValue(expr->right());
      Condition cond = CompareIC::ComputeCondition(op);
      __ pop(r3);

      bool inline_smi_code = ShouldInlineSmiCase(op);
      JumpPatchSite patch_site(masm_);
      if (inline_smi_code) {
        Label slow_case;
        __ LoadRR(r4, r3);
        __ OrP(r4, r2);
        patch_site.EmitJumpIfNotSmi(r4, &slow_case);
        __ CmpP(r3, r2);
        Split(cond, if_true, if_false, NULL);
        __ bind(&slow_case);
      }

      Handle<Code> ic = CodeFactory::CompareIC(
                            isolate(), op, strength(language_mode())).code();
      CallIC(ic, expr->CompareOperationFeedbackId());
      patch_site.EmitPatchInfo();
      PrepareForBailoutBeforeSplit(expr, true, if_true, if_false);
      __ CmpP(r2, Operand::Zero());
      Split(cond, if_true, if_false, fall_through);
    }
  }

  // Convert the result of the comparison into one expected for this
  // expression's context.
  context()->Plug(if_true, if_false);
}


void FullCodeGenerator::EmitLiteralCompareNil(CompareOperation* expr,
                                              Expression* sub_expr,
                                              NilValue nil) {
  Label materialize_true, materialize_false;
  Label* if_true = NULL;
  Label* if_false = NULL;
  Label* fall_through = NULL;
  context()->PrepareTest(&materialize_true, &materialize_false, &if_true,
                         &if_false, &fall_through);

  VisitForAccumulatorValue(sub_expr);
  PrepareForBailoutBeforeSplit(expr, true, if_true, if_false);
  if (expr->op() == Token::EQ_STRICT) {
    Heap::RootListIndex nil_value = nil == kNullValue
                                        ? Heap::kNullValueRootIndex
                                        : Heap::kUndefinedValueRootIndex;
    __ CompareRoot(r2, nil_value);
    Split(eq, if_true, if_false, fall_through);
  } else {
    Handle<Code> ic = CompareNilICStub::GetUninitialized(isolate(), nil);
    CallIC(ic, expr->CompareOperationFeedbackId());
    __ CmpP(r2, Operand::Zero());
    Split(ne, if_true, if_false, fall_through);
  }
  context()->Plug(if_true, if_false);
}


void FullCodeGenerator::VisitThisFunction(ThisFunction* expr) {
  __ LoadP(r2, MemOperand(fp, JavaScriptFrameConstants::kFunctionOffset));
  context()->Plug(r2);
}


Register FullCodeGenerator::result_register() { return r2; }


Register FullCodeGenerator::context_register() { return cp; }


void FullCodeGenerator::StoreToFrameField(int frame_offset, Register value) {
  DCHECK_EQ(static_cast<int>(POINTER_SIZE_ALIGN(frame_offset)), frame_offset);
  __ StoreP(value, MemOperand(fp, frame_offset));
}


void FullCodeGenerator::LoadContextField(Register dst, int context_index) {
  __ LoadP(dst, ContextOperand(cp, context_index), r0);
}


void FullCodeGenerator::PushFunctionArgumentForContextAllocation() {
  Scope* declaration_scope = scope()->DeclarationScope();
  if (declaration_scope->is_script_scope() ||
      declaration_scope->is_module_scope()) {
    // Contexts nested in the native context have a canonical empty function
    // as their closure, not the anonymous closure containing the global
    // code.  Pass a smi sentinel and let the runtime look up the empty
    // function.
    __ LoadSmiLiteral(ip, Smi::FromInt(0));
  } else if (declaration_scope->is_eval_scope()) {
    // Contexts created by a call to eval have the same closure as the
    // context calling eval, not the anonymous closure containing the eval
    // code.  Fetch it from the context.
    __ LoadP(ip, ContextOperand(cp, Context::CLOSURE_INDEX));
  } else {
    DCHECK(declaration_scope->is_function_scope());
    __ LoadP(ip, MemOperand(fp, JavaScriptFrameConstants::kFunctionOffset));
  }
  __ push(ip);
}


// ----------------------------------------------------------------------------
// Non-local control flow support.

void FullCodeGenerator::EnterFinallyBlock() {
  DCHECK(!result_register().is(r3));
  // Store result register while executing finally block.
  __ push(result_register());
  // Cook return address in link register to stack (smi encoded Code* delta)
  __ LoadRR(r3, r14);
  __ CleanseP(r3);
  __ mov(ip, Operand(masm_->CodeObject()));
  __ SubP(r3, r3, ip);
  __ SmiTag(r3);

  // Store result register while executing finally block.
  __ push(r3);

  // Store pending message while executing finally block.
  ExternalReference pending_message_obj =
      ExternalReference::address_of_pending_message_obj(isolate());
  __ mov(ip, Operand(pending_message_obj));
  __ LoadP(r3, MemOperand(ip));
  __ push(r3);

  ClearPendingMessage();
}


void FullCodeGenerator::ExitFinallyBlock() {
  DCHECK(!result_register().is(r3));
  // Restore pending message from stack.
  __ pop(r3);
  ExternalReference pending_message_obj =
      ExternalReference::address_of_pending_message_obj(isolate());
  __ mov(ip, Operand(pending_message_obj));
  __ StoreP(r3, MemOperand(ip));

  // Restore result register from stack.
  __ pop(r3);

  // Uncook return address and return.
  __ pop(result_register());
  __ SmiUntag(r3);
  __ mov(ip, Operand(masm_->CodeObject()));
  __ AddP(ip, r3);
  __ b(ip);
}


void FullCodeGenerator::ClearPendingMessage() {
  DCHECK(!result_register().is(r3));
  ExternalReference pending_message_obj =
      ExternalReference::address_of_pending_message_obj(isolate());
  __ LoadRoot(r3, Heap::kTheHoleValueRootIndex);
  __ mov(ip, Operand(pending_message_obj));
  __ StoreP(r3, MemOperand(ip));
}


void FullCodeGenerator::EmitLoadStoreICSlot(FeedbackVectorICSlot slot) {
  DCHECK(FLAG_vector_stores && !slot.IsInvalid());
  __ mov(VectorStoreICTrampolineDescriptor::SlotRegister(),
         Operand(SmiFromSlot(slot)));
}


#undef __

#if V8_TARGET_ARCH_S390X
static const FourByteInstr kInterruptBranchInstruction = 0xA7A40011;
static const FourByteInstr kOSRBranchInstruction = 0xA7040011;
static const int16_t kBackEdgeBranchOffset = 0x11 * 2;
#else
static const FourByteInstr kInterruptBranchInstruction = 0xA7A4000D;
static const FourByteInstr kOSRBranchInstruction = 0xA704000D;
static const int16_t kBackEdgeBranchOffset = 0xD * 2;
#endif

void BackEdgeTable::PatchAt(Code* unoptimized_code, Address pc,
                            BackEdgeState target_state,
                            Code* replacement_code) {
  Address call_address = Assembler::target_address_from_return_address(pc);
  Address branch_address = call_address - 4;
  CodePatcher patcher(branch_address, 4);

  switch (target_state) {
    case INTERRUPT: {
      //  <decrement profiling counter>
      //         bge     <ok>            ;; patched to GE BRC
      //         brasrl    r14, <interrupt stub address>
      //  <reset profiling counter>
      //  ok-label
      patcher.masm()->brc(ge, Operand(kBackEdgeBranchOffset));
      break;
    }
    case ON_STACK_REPLACEMENT:
    case OSR_AFTER_STACK_CHECK:
      //  <decrement profiling counter>
      //         brc   0x0, <ok>            ;;  patched to NOP BRC
      //         brasrl    r14, <interrupt stub address>
      //  <reset profiling counter>
      //  ok-label ----- pc_after points here
      patcher.masm()->brc(CC_NOP, Operand(kBackEdgeBranchOffset));
      break;
  }

  // Replace the stack check address in the mov sequence with the
  // entry address of the replacement code.
  Assembler::set_target_address_at(call_address, unoptimized_code,
                                   replacement_code->entry());

  unoptimized_code->GetHeap()->incremental_marking()->RecordCodeTargetPatch(
      unoptimized_code, call_address, replacement_code);
}


BackEdgeTable::BackEdgeState BackEdgeTable::GetBackEdgeState(
    Isolate* isolate, Code* unoptimized_code, Address pc) {
  Address call_address = Assembler::target_address_from_return_address(pc);
  Address branch_address = call_address - 4;
  Address interrupt_address =
      Assembler::target_address_at(call_address, unoptimized_code);

  DCHECK(BRC == Instruction::S390OpcodeValue(branch_address));
  // For interrupt, we expect a branch greater than or equal
  // i.e. BRC 0xa, +XXXX  (0xA7A4XXXX)
  FourByteInstr br_instr = Instruction::InstructionBits(
                              reinterpret_cast<const byte*>(branch_address));
  if (kInterruptBranchInstruction == br_instr) {
    DCHECK(interrupt_address == isolate->builtins()->InterruptCheck()->entry());
    return INTERRUPT;
  }

  // Expect BRC to be patched to NOP branch.
  // i.e. BRC 0x0, +XXXX (0xA704XXXX)
  USE(kOSRBranchInstruction);
  DCHECK(kOSRBranchInstruction == br_instr);

  if (interrupt_address == isolate->builtins()->OnStackReplacement()->entry()) {
    return ON_STACK_REPLACEMENT;
  }

  DCHECK(interrupt_address ==
         isolate->builtins()->OsrAfterStackCheck()->entry());
  return OSR_AFTER_STACK_CHECK;
}


}  // namespace internal
}  // namespace v8
#endif  // V8_TARGET_ARCH_S390
