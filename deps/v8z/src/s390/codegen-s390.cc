// Copyright 2012 the V8 project authors. All rights reserved.
//
// Copyright IBM Corp. 2012-2014. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/v8.h"

#if V8_TARGET_ARCH_S390

#include "src/codegen.h"
#include "src/macro-assembler.h"
#include "src/s390/simulator-s390.h"

namespace v8 {
namespace internal {


#define __ masm.


#if defined(USE_SIMULATOR)
byte* fast_exp_s390_machine_code = NULL;
double fast_exp_simulator(double x) {
  return Simulator::current(Isolate::Current())->CallFPReturnsDouble(
      fast_exp_s390_machine_code, x, 0);
}
#endif


UnaryMathFunction CreateExpFunction() {
  if (!FLAG_fast_math) return &std::exp;
  size_t actual_size;
  byte* buffer = static_cast<byte*>(base::OS::Allocate(1 * KB,
                                                       &actual_size, true));
  if (buffer == NULL) return &std::exp;
  ExternalReference::InitializeMathExpData();

  MacroAssembler masm(NULL, buffer, static_cast<int>(actual_size));

  {
    DoubleRegister input = d0;
    DoubleRegister result = d2;
    DoubleRegister double_scratch1 = d3;
    DoubleRegister double_scratch2 = d4;
    Register temp1 = r6;
    Register temp2 = r7;
    Register temp3 = r8;

  // Called from C
#if ABI_USES_FUNCTION_DESCRIPTORS
    __ function_descriptor();
#endif

    __ Push(temp3, temp2, temp1);
    MathExpGenerator::EmitMathExp(
        &masm, input, result, double_scratch1, double_scratch2,
        temp1, temp2, temp3);
    __ Pop(temp3, temp2, temp1);
    __ ldr(d0, result);
    __ Ret();
  }

  CodeDesc desc;
  masm.GetCode(&desc);
#if !ABI_USES_FUNCTION_DESCRIPTORS
  DCHECK(!RelocInfo::RequiresRelocation(desc));
#endif

  CpuFeatures::FlushICache(buffer, actual_size);
  base::OS::ProtectCode(buffer, actual_size);

#if !defined(USE_SIMULATOR)
  return FUNCTION_CAST<UnaryMathFunction>(buffer);
#else
  fast_exp_s390_machine_code = buffer;
  return &fast_exp_simulator;
#endif
}


UnaryMathFunction CreateSqrtFunction() {
#if defined(USE_SIMULATOR)
  return &std::sqrt;
#else
  size_t actual_size;
  byte* buffer = static_cast<byte*>(base::OS::Allocate(1 * KB,
                                                       &actual_size, true));
  if (buffer == NULL) return &std::sqrt;

  MacroAssembler masm(NULL, buffer, static_cast<int>(actual_size));

  // Called from C
#if ABI_USES_FUNCTION_DESCRIPTORS
  __ function_descriptor();
#endif

  __ MovFromFloatParameter(d0);
  __ sqdbr(d0, d0);
  __ MovToFloatResult(d0);
  __ Ret();

  CodeDesc desc;
  masm.GetCode(&desc);
#if !ABI_USES_FUNCTION_DESCRIPTORS
  DCHECK(!RelocInfo::RequiresRelocation(desc));
#endif

  CpuFeatures::FlushICache(buffer, actual_size);
  base::OS::ProtectCode(buffer, actual_size);
  return FUNCTION_CAST<UnaryMathFunction>(buffer);
#endif
}

#undef __


// -------------------------------------------------------------------------
// Platform-specific RuntimeCallHelper functions.

void StubRuntimeCallHelper::BeforeCall(MacroAssembler* masm) const {
  masm->EnterFrame(StackFrame::INTERNAL);
  DCHECK(!masm->has_frame());
  masm->set_has_frame(true);
}


void StubRuntimeCallHelper::AfterCall(MacroAssembler* masm) const {
  masm->LeaveFrame(StackFrame::INTERNAL);
  DCHECK(masm->has_frame());
  masm->set_has_frame(false);
}


// -------------------------------------------------------------------------
// Code generators

#define __ ACCESS_MASM(masm)

void ElementsTransitionGenerator::GenerateMapChangeElementsTransition(
    MacroAssembler* masm,
    Register receiver,
    Register key,
    Register value,
    Register target_map,
    AllocationSiteMode mode,
    Label* allocation_memento_found) {
  Register scratch_elements = r6;
  DCHECK(!AreAliased(receiver, key, value, target_map,
                    scratch_elements));

  if (mode == TRACK_ALLOCATION_SITE) {
    DCHECK(allocation_memento_found != NULL);
    __ JumpIfJSArrayHasAllocationMemento(
       receiver, scratch_elements, allocation_memento_found);
  }

  // Set transitioned map.
  __ StoreP(target_map, FieldMemOperand(receiver, HeapObject::kMapOffset));
  __ RecordWriteField(receiver,
                      HeapObject::kMapOffset,
                      target_map,
                      r1,
                      kLRHasNotBeenSaved,
                      kDontSaveFPRegs,
                      EMIT_REMEMBERED_SET,
                      OMIT_SMI_CHECK);
}


void ElementsTransitionGenerator::GenerateSmiToDouble(
    MacroAssembler* masm,
    Register receiver,
    Register key,
    Register value,
    Register target_map,
    AllocationSiteMode mode,
    Label* fail) {
  // lr contains the return address
  Label loop, entry, convert_hole, gc_required, only_change_map, done;
  Register elements = r6;
  Register length = r7;
  Register array = r8;
  Register array_end = array;

  // target_map parameter can be clobbered.
  Register scratch1 = target_map;
  Register scratch2 = r1;

  // Verify input registers don't conflict with locals.
  DCHECK(!AreAliased(receiver, key, value, target_map,
                     elements, length, array, scratch2));

  if (mode == TRACK_ALLOCATION_SITE) {
    __ JumpIfJSArrayHasAllocationMemento(receiver, elements, fail);
  }

  // Check for empty arrays, which only require a map transition and no changes
  // to the backing store.
  __ LoadP(elements, FieldMemOperand(receiver, JSObject::kElementsOffset));
  __ CompareRoot(elements, Heap::kEmptyFixedArrayRootIndex);
  __ beq(&only_change_map, Label::kNear);

  // Preserve lr and use r14 as a temporary register.
  __ push(r14);

  __ LoadP(length, FieldMemOperand(elements, FixedArray::kLengthOffset));
  // length: number of elements (smi-tagged)

  // Allocate new FixedDoubleArray.
  __ SmiToDoubleArrayOffset(r14, length);
  __ AddP(r14, Operand(FixedDoubleArray::kHeaderSize));
  __ Allocate(r14, array, r9, scratch2, &gc_required, DOUBLE_ALIGNMENT);

   // Set destination FixedDoubleArray's length and map.
  __ LoadRoot(scratch2, Heap::kFixedDoubleArrayMapRootIndex);
  __ StoreP(length, MemOperand(array, FixedDoubleArray::kLengthOffset));
  // Update receiver's map.
  __ StoreP(scratch2, MemOperand(array, HeapObject::kMapOffset));

  __ StoreP(target_map, FieldMemOperand(receiver, HeapObject::kMapOffset));
  __ RecordWriteField(receiver,
                      HeapObject::kMapOffset,
                      target_map,
                      scratch2,
                      kLRHasBeenSaved,
                      kDontSaveFPRegs,
                      OMIT_REMEMBERED_SET,
                      OMIT_SMI_CHECK);
  // Replace receiver's backing store with newly created FixedDoubleArray.
  __ AddP(scratch1, array, Operand(kHeapObjectTag));
  __ StoreP(scratch1, FieldMemOperand(receiver, JSObject::kElementsOffset));
  __ RecordWriteField(receiver,
                      JSObject::kElementsOffset,
                      scratch1,
                      scratch2,
                      kLRHasBeenSaved,
                      kDontSaveFPRegs,
                      EMIT_REMEMBERED_SET,
                      OMIT_SMI_CHECK);

  // Prepare for conversion loop.
  __ AddP(target_map, elements,
          Operand(FixedArray::kHeaderSize - kHeapObjectTag));
  __ AddP(r9, array, Operand(FixedDoubleArray::kHeaderSize));
  __ SmiToDoubleArrayOffset(array, length);
  __ AddP(array_end, r9, array);
  // Repurpose registers no longer in use.
#if V8_TARGET_ARCH_S390X
  Register hole_int64 = elements;
#else
  Register hole_lower = elements;
  Register hole_upper = length;
#endif
  // scratch1: begin of source FixedArray element fields, not tagged
  // hole_lower: kHoleNanLower32 OR hol_int64
  // hole_upper: kHoleNanUpper32
  // array_end: end of destination FixedDoubleArray, not tagged
  // scratch2: begin of FixedDoubleArray element fields, not tagged

  __ b(&entry, Label::kNear);

  __ bind(&only_change_map);
  __ StoreP(target_map, FieldMemOperand(receiver, HeapObject::kMapOffset));
  __ RecordWriteField(receiver,
                      HeapObject::kMapOffset,
                      target_map,
                      scratch2,
                      kLRHasNotBeenSaved,
                      kDontSaveFPRegs,
                      OMIT_REMEMBERED_SET,
                      OMIT_SMI_CHECK);
  __ b(&done, Label::kNear);

  // Call into runtime if GC is required.
  __ bind(&gc_required);
  __ pop(r14);
  __ b(fail);

  // Convert and copy elements.
  __ bind(&loop);
  __ LoadP(r14, MemOperand(scratch1));
  __ la(scratch1, MemOperand(scratch1, kPointerSize));
  // r1: current element
  __ UntagAndJumpIfNotSmi(r14, r14, &convert_hole);

  // Normal smi, convert to double and store.
  __ ConvertIntToDouble(r14, d0);
  __ StoreF(d0, MemOperand(scratch2, 0));
  __ la(scratch2, MemOperand(scratch2, 8));

  __ b(&entry, Label::kNear);

  // Hole found, store the-hole NaN.
  __ bind(&convert_hole);
  if (FLAG_debug_code) {
    // Restore a "smi-untagged" heap object.
    __ LoadP(r1, MemOperand(r5, -kPointerSize));
    __ CompareRoot(r1, Heap::kTheHoleValueRootIndex);
    __ Assert(eq, kObjectFoundInSmiOnlyArray);
  }
#if V8_TARGET_ARCH_S390X
  __ stg(hole_int64, MemOperand(r9, 0));
#else
  // TODO(joransiu): Check if this works
  __ StoreW(hole_upper, MemOperand(r9, Register::kExponentOffset));
  __ StoreW(hole_lower, MemOperand(r9, Register::kMantissaOffset));
#endif
  __ AddP(r9, Operand(8));

  __ bind(&entry);
  __ CmpP(r9, array_end);
  __ blt(&loop);

  __ pop(r14);
  __ bind(&done);
}


void ElementsTransitionGenerator::GenerateDoubleToObject(
    MacroAssembler* masm,
    Register receiver,
    Register key,
    Register value,
    Register target_map,
    AllocationSiteMode mode,
    Label* fail) {
  // Register lr contains the return address.
  Label entry, loop, convert_hole, gc_required, only_change_map;
  Register elements = r6;
  Register array = r8;
  Register length = r7;
  Register scratch = r1;

  // Verify input registers don't conflict with locals.
  DCHECK(!AreAliased(receiver, key, value, target_map,
                     elements, array, length, scratch));

  if (mode == TRACK_ALLOCATION_SITE) {
    __ JumpIfJSArrayHasAllocationMemento(receiver, elements, fail);
  }

  // Check for empty arrays, which only require a map transition and no changes
  // to the backing store.
  __ LoadP(elements, FieldMemOperand(receiver, JSObject::kElementsOffset));
  __ CompareRoot(elements, Heap::kEmptyFixedArrayRootIndex);
  __ beq(&only_change_map);

  __ Push(target_map, receiver, key, value);
  __ LoadP(length, FieldMemOperand(elements, FixedArray::kLengthOffset));
  // elements: source FixedDoubleArray
  // length: number of elements (smi-tagged)

  // Allocate new FixedArray.
  // Re-use value and target_map registers, as they have been saved on the
  // stack.
  Register array_size = value;
  Register allocate_scratch = target_map;
  __ LoadImmP(array_size, Operand(FixedDoubleArray::kHeaderSize));
  __ SmiToPtrArrayOffset(r0, length);
  __ AddP(array_size, r0);
  __ Allocate(array_size, array, allocate_scratch, scratch, &gc_required,
              NO_ALLOCATION_FLAGS);
  // array: destination FixedArray, not tagged as heap object
  // Set destination FixedDoubleArray's length and map.
  __ LoadRoot(scratch, Heap::kFixedArrayMapRootIndex);
  __ StoreP(length, MemOperand(array, FixedDoubleArray::kLengthOffset));
  __ StoreP(scratch, MemOperand(array, HeapObject::kMapOffset));
  __ AddP(array, Operand(kHeapObjectTag));

  // Prepare for conversion loop.
  Register src_elements = elements;
  Register dst_elements = target_map;
  Register dst_end = length;
  Register heap_number_map = scratch;
  __ AddP(src_elements,
          Operand(FixedDoubleArray::kHeaderSize - kHeapObjectTag));
  __ SmiToPtrArrayOffset(length, length);
  __ LoadRoot(r9, Heap::kTheHoleValueRootIndex);

  Label initialization_loop, loop_done;
  __ ShiftRightP(r0, length, Operand(kPointerSizeLog2));
  __ beq(&loop_done, Label::kNear/*, cr0*/);

  // Allocating heap numbers in the loop below can fail and cause a jump to
  // gc_required. We can't leave a partly initialized FixedArray behind,
  // so pessimistically fill it with holes now.
  __ AddP(dst_elements, array, Operand(FixedArray::kHeaderSize -
      kHeapObjectTag - kPointerSize));
  __ bind(&initialization_loop);
  __ StoreP(r9, MemOperand(dst_elements, kPointerSize));
  __ lay(dst_elements, MemOperand(dst_elements, kPointerSize));
  __ BranchOnCount(r0, &initialization_loop);

  __ AddP(dst_elements, array, Operand(FixedArray::kHeaderSize -
      kHeapObjectTag));
  __ AddP(dst_end, dst_elements, length);
  __ LoadRoot(heap_number_map, Heap::kHeapNumberMapRootIndex);
  // Using offsetted addresses in src_elements to fully take advantage of
  // post-indexing.
  // dst_elements: begin of destination FixedArray element fields, not tagged
  // src_elements: begin of source FixedDoubleArray element fields,
  //               not tagged, +4
  // dst_end: end of destination FixedArray, not tagged
  // array: destination FixedArray
  // r9: the-hole pointer
  // heap_number_map: heap number map
  __ b(&loop, Label::kNear);

  // Call into runtime if GC is required.
  __ bind(&gc_required);
  __ Pop(target_map, receiver, key, value);
  __ b(fail);

  // Replace the-hole NaN with the-hole pointer.
  __ bind(&convert_hole);
  __ StoreP(r9, MemOperand(dst_elements));
  __ AddP(dst_elements, Operand(kPointerSize));
  __ CmpLogicalP(dst_elements, dst_end);
  __ bge(&loop_done);

  __ bind(&loop);
  Register upper_bits = key;
  __ LoadlW(upper_bits, MemOperand(src_elements, Register::kExponentOffset));
  __ AddP(src_elements, Operand(kDoubleSize));
  // upper_bits: current element's upper 32 bit
  // src_elements: address of next element's upper 32 bit
  __ CmpP(upper_bits, Operand(kHoleNanUpper32));
  __ beq(&convert_hole, Label::kNear);

  // Non-hole double, copy value into a heap number.
  Register heap_number = receiver;
  Register scratch2 = value;
  __ AllocateHeapNumber(heap_number, scratch2, r1, heap_number_map,
                         &gc_required);
  // heap_number: new heap number
#if V8_TARGET_ARCH_S390X
  __ lg(scratch2, MemOperand(src_elements, -kDoubleSize));
  // subtract tag for std
  __ AddP(upper_bits, heap_number, Operand(-kHeapObjectTag));
  __ stg(scratch2, MemOperand(upper_bits, HeapNumber::kValueOffset));
#else
  __ LoadlW(scratch2, MemOperand(src_elements,
            Register::kMantissaOffset - kDoubleSize));
  __ LoadlW(upper_bits, MemOperand(src_elements,
            Register::kExponentOffset - kDoubleSize));
  __ StoreW(scratch2, FieldMemOperand(heap_number,
            HeapNumber::kMantissaOffset));
  __ StoreW(upper_bits, FieldMemOperand(heap_number,
            HeapNumber::kExponentOffset));
#endif
  __ LoadRR(scratch2, dst_elements);
  __ StoreP(heap_number, MemOperand(dst_elements));
  __ AddP(dst_elements, Operand(kPointerSize));
  __ RecordWrite(array,
                 scratch2,
                 heap_number,
                 kLRHasNotBeenSaved,
                 kDontSaveFPRegs,
                 EMIT_REMEMBERED_SET,
                 OMIT_SMI_CHECK);
  __ CmpLogicalP(dst_elements, dst_end);
  __ blt(&loop);
  __ bind(&loop_done);

  __ Pop(target_map, receiver, key, value);
  // Replace receiver's backing store with newly created and filled FixedArray.
  __ StoreP(array, FieldMemOperand(receiver, JSObject::kElementsOffset));
  __ RecordWriteField(receiver,
                      JSObject::kElementsOffset,
                      array,
                      scratch,
                      kLRHasNotBeenSaved,
                      kDontSaveFPRegs,
                      EMIT_REMEMBERED_SET,
                      OMIT_SMI_CHECK);

  __ bind(&only_change_map);
  // Update receiver's map.
  __ StoreP(target_map, FieldMemOperand(receiver, HeapObject::kMapOffset));
  __ RecordWriteField(receiver,
                      HeapObject::kMapOffset,
                      target_map,
                      scratch,
                      kLRHasNotBeenSaved,
                      kDontSaveFPRegs,
                      OMIT_REMEMBERED_SET,
                      OMIT_SMI_CHECK);
}


// assume ip can be used as a scratch register below
void StringCharLoadGenerator::Generate(MacroAssembler* masm,
                                       Register string,
                                       Register index,
                                       Register result,
                                       Label* call_runtime) {
  // Fetch the instance type of the receiver into result register.
  __ LoadP(result, FieldMemOperand(string, HeapObject::kMapOffset));
  __ LoadlB(result, FieldMemOperand(result, Map::kInstanceTypeOffset));

  // We need special handling for indirect strings.
  Label check_sequential;
  __ mov(r0, Operand(kIsIndirectStringMask));
  __ AndP(r0, result);
  __ beq(&check_sequential, Label::kNear/*, cr0*/);

  // Dispatch on the indirect string shape: slice or cons.
  Label cons_string;
  __ mov(ip, Operand(kSlicedNotConsMask));
  __ LoadRR(r0, result);
  __ AndP(r0, ip/*, SetRC*/);  // Should be okay to remove RC
  __ beq(&cons_string , Label::kNear/*, cr0*/);

  // Handle slices.
  Label indirect_string_loaded;
  __ LoadP(result, FieldMemOperand(string, SlicedString::kOffsetOffset));
  __ LoadP(string, FieldMemOperand(string, SlicedString::kParentOffset));
  __ SmiUntag(ip, result);
  __ AddP(index, ip);
  __ b(&indirect_string_loaded, Label::kNear);

  // Handle cons strings.
  // Check whether the right hand side is the empty string (i.e. if
  // this is really a flat string in a cons string). If that is not
  // the case we would rather go to the runtime system now to flatten
  // the string.
  __ bind(&cons_string);
  __ LoadP(result, FieldMemOperand(string, ConsString::kSecondOffset));
  __ CompareRoot(result, Heap::kempty_stringRootIndex);
  __ bne(call_runtime);
  // Get the first of the two strings and load its instance type.
  __ LoadP(string, FieldMemOperand(string, ConsString::kFirstOffset));

  __ bind(&indirect_string_loaded);
  __ LoadP(result, FieldMemOperand(string, HeapObject::kMapOffset));
  __ LoadlB(result, FieldMemOperand(result, Map::kInstanceTypeOffset));

  // Distinguish sequential and external strings. Only these two string
  // representations can reach here (slices and flat cons strings have been
  // reduced to the underlying sequential or external string).
  Label external_string, check_encoding;
  __ bind(&check_sequential);
  STATIC_ASSERT(kSeqStringTag == 0);
  __ mov(r0, Operand(kStringRepresentationMask));
  __ AndP(r0, result);
  __ bne(&external_string, Label::kNear);

  // Prepare sequential strings
  STATIC_ASSERT(SeqTwoByteString::kHeaderSize == SeqOneByteString::kHeaderSize);
  __ AddP(string, Operand(SeqTwoByteString::kHeaderSize - kHeapObjectTag));
  __ b(&check_encoding, Label::kNear);

  // Handle external strings.
  __ bind(&external_string);
  if (FLAG_debug_code) {
    // Assert that we do not have a cons or slice (indirect strings) here.
    // Sequential strings have already been ruled out.
    __ mov(r0, Operand(kIsIndirectStringMask));
    __ AndP(r0, result);
    __ Assert(eq, kExternalStringExpectedButNotFound, cr0);
  }
  // Rule out short external strings.
  STATIC_ASSERT(kShortExternalStringTag != 0);
  __ mov(r0, Operand(kShortExternalStringMask));
  __ AndP(r0, result);
  __ bne(call_runtime /*, cr0*/);
  __ LoadP(string,
           FieldMemOperand(string, ExternalString::kResourceDataOffset));

  Label ascii, done;
  __ bind(&check_encoding);
  STATIC_ASSERT(kTwoByteStringTag == 0);
  __ mov(r0, Operand(kStringEncodingMask));
  __ AndP(r0, result);
  __ bne(&ascii, Label::kNear);
  // Two-byte string.
  __ ShiftLeftP(result, index, Operand(1));
  __ LoadLogicalHalfWordP(result, MemOperand(string, result));
  __ b(&done, Label::kNear);
  __ bind(&ascii);
  // Ascii string.
  __ LoadlB(result, MemOperand(string, index));
  __ bind(&done);
}


static MemOperand ExpConstant(int index, Register base) {
  return MemOperand(base, index * kDoubleSize);
}


void MathExpGenerator::EmitMathExp(MacroAssembler* masm,
                                   DoubleRegister input,
                                   DoubleRegister result,
                                   DoubleRegister double_scratch1,
                                   DoubleRegister double_scratch2,
                                   Register temp1,
                                   Register temp2,
                                   Register temp3) {
  DCHECK(!input.is(result));
  DCHECK(!input.is(double_scratch1));
  DCHECK(!input.is(double_scratch2));
  DCHECK(!result.is(double_scratch1));
  DCHECK(!result.is(double_scratch2));
  DCHECK(!double_scratch1.is(double_scratch2));
  DCHECK(!temp1.is(temp2));
  DCHECK(!temp1.is(temp3));
  DCHECK(!temp2.is(temp3));
  DCHECK(ExternalReference::math_exp_constants(0).address() != NULL);
  DCHECK(!masm->serializer_enabled());

  Label zero, infinity, done;

  __ mov(temp3, Operand(ExternalReference::math_exp_constants(0)));

  __ LoadF(double_scratch1, ExpConstant(0, temp3));
  __ cdbr(double_scratch1, input);
  __ ldr(result, input);
  __ bunordered(&done, Label::kNear);
  __ bge(&zero, Label::kNear);

  __ LoadF(double_scratch2, ExpConstant(1, temp3));
  __ cdbr(input, double_scratch2);
  __ bge(&infinity, Label::kNear);

  __ LoadF(double_scratch1, ExpConstant(3, temp3));
  __ LoadF(result, ExpConstant(4, temp3));
  // @TODO(Tara): verify madbr for correctness and use here instead of mdbr,adbr
  __ mdbr(double_scratch1, input);
  __ adbr(double_scratch1, result);

  // Move low word of double_scratch1 to temp2
  __ lgdr(temp2, double_scratch1);
  __ nihf(temp2, Operand::Zero());

  __ sdbr(double_scratch1, result);
  __ LoadF(result, ExpConstant(6, temp3));
  __ LoadF(double_scratch2, ExpConstant(5, temp3));
  __ mdbr(double_scratch1, double_scratch2);
  __ sdbr(double_scratch1, input);
  __ sdbr(result, double_scratch1);
  __ ldr(double_scratch2, double_scratch1);
  __ mdbr(double_scratch2, double_scratch2);
  __ mdbr(result, double_scratch2);
  __ LoadF(double_scratch2, ExpConstant(7, temp3));
  __ mdbr(result, double_scratch2);
  __ sdbr(result, double_scratch1);
  __ LoadF(double_scratch2, ExpConstant(8, temp3));
  __ adbr(result, double_scratch2);
  __ ShiftRight(temp1, temp2, Operand(11));
  __ AndP(temp2, Operand(0x7ff));
  __ AddP(temp1, Operand(0x3ff));

  // Must not call ExpConstant() after overwriting temp3!
  __ mov(temp3, Operand(ExternalReference::math_exp_log_table()));
  __ ShiftLeft(temp2, temp2, Operand(3));

  __ lg(temp2, MemOperand(temp2, temp3));
  __ sllg(temp1, temp1, Operand(52));
  __ ogr(temp2, temp1);
  __ ldgr(double_scratch1, temp2);

  __ mdbr(result, double_scratch1);
  __ b(&done, Label::kNear);

  __ bind(&zero);
  __ ldr(result, kDoubleRegZero);
  __ b(&done, Label::kNear);

  __ bind(&infinity);
  __ LoadF(result, ExpConstant(2, temp3));

  __ bind(&done);
}

#undef __


CodeAgingHelper::CodeAgingHelper() {
  DCHECK(young_sequence_.length() == kNoCodeAgeSequenceLength);
  // Since patcher is a large object, allocate it dynamically when needed,
  // to avoid overloading the stack in stress conditions.
  // DONT_FLUSH is used because the CodeAgingHelper is initialized early in
  // the process, before ARM simulator ICache is setup.
  SmartPointer<CodePatcher> patcher(
      new CodePatcher(young_sequence_.start(),
                      young_sequence_.length(),
                      CodePatcher::DONT_FLUSH));
  PredictableCodeSizeScope scope(patcher->masm(), young_sequence_.length());
  patcher->masm()->PushFixedFrame(r3);
  patcher->masm()->la(
        fp, MemOperand(sp, StandardFrameConstants::kFixedFrameSizeFromFp));
}


#ifdef DEBUG
bool CodeAgingHelper::IsOld(byte* candidate) const {
  return Assembler::IsNop(Assembler::instr_at(candidate));
}
#endif


bool Code::IsYoungSequence(Isolate* isolate, byte* sequence) {
  bool result = isolate->code_aging_helper()->IsYoung(sequence);
  DCHECK(result || isolate->code_aging_helper()->IsOld(sequence));
  return result;
}


void Code::GetCodeAgeAndParity(Isolate* isolate, byte* sequence, Age* age,
                               MarkingParity* parity) {
  if (IsYoungSequence(isolate, sequence)) {
    *age = kNoAgeCodeAge;
    *parity = NO_MARKING_PARITY;
  } else {
    Address constant_pool = NULL;
    Address target_address = Assembler::target_address_at(
      sequence + kCodeAgingTargetDelta, constant_pool);
    Code* stub = GetCodeFromTargetAddress(target_address);
    GetCodeAgeAndParity(stub, age, parity);
  }
}


void Code::PatchPlatformCodeAge(Isolate* isolate,
                                byte* sequence,
                                Code::Age age,
                                MarkingParity parity) {
  uint32_t young_length = isolate->code_aging_helper()->young_sequence_length();
  if (age == kNoAgeCodeAge) {
    isolate->code_aging_helper()->CopyYoungSequenceTo(sequence);
    CpuFeatures::FlushICache(sequence, young_length);
  } else {
    // FIXED_SEQUENCE
    Code* stub = GetCodeAgeStub(isolate, age, parity);
    CodePatcher patcher(sequence, young_length);
    Assembler::BlockTrampolinePoolScope block_trampoline_pool(patcher.masm());
    intptr_t target = reinterpret_cast<intptr_t>(stub->instruction_start());
    // Don't use Call -- we need to preserve ip and lr.
    // GenerateMakeCodeYoungAgainCommon for the stub code.
    patcher.masm()->nop();  // marker to detect sequence (see IsOld)
    patcher.masm()->mov(r2, Operand(target));
    patcher.masm()->Jump(r2);
    for (int i = 0;
         i < kNoCodeAgeSequenceLength - kCodeAgingSequenceLength; i += 2) {
      // TODO(joransiu): Create nop function to pad
      //       (kNoCodeAgeSequenceLength - kCodeAgingSequenceLength) bytes.
      patcher.masm()->nop();   // 2-byte nops().
    }
  }
}


} }  // namespace v8::internal

#endif  // V8_TARGET_ARCH_S390
