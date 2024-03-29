/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ART_RUNTIME_INSTRUMENTATION_H_
#define ART_RUNTIME_INSTRUMENTATION_H_

#include <stdint.h>
#include <list>
#include <memory>
#include <unordered_set>

#include "arch/instruction_set.h"
#include "base/enums.h"
#include "base/locks.h"
#include "base/macros.h"
#include "base/safe_map.h"
#include "gc_root.h"

namespace art {
namespace mirror {
class Class;
class Object;
class Throwable;
}  // namespace mirror
class ArtField;
class ArtMethod;
template <typename T> class Handle;
template <typename T> class MutableHandle;
union JValue;
class SHARED_LOCKABLE ReaderWriterMutex;
class ShadowFrame;
class Thread;
enum class DeoptimizationMethodType;

namespace instrumentation {

// Interpreter handler tables.
enum InterpreterHandlerTable {
  kMainHandlerTable = 0,          // Main handler table: no suspend check, no instrumentation.
  kAlternativeHandlerTable = 1,   // Alternative handler table: suspend check and/or instrumentation
                                  // enabled.
  kNumHandlerTables
};

// Do we want to deoptimize for method entry and exit listeners or just try to intercept
// invocations? Deoptimization forces all code to run in the interpreter and considerably hurts the
// application's performance.
static constexpr bool kDeoptimizeForAccurateMethodEntryExitListeners = true;

// Instrumentation event listener API. Registered listeners will get the appropriate call back for
// the events they are listening for. The call backs supply the thread, method and dex_pc the event
// occurred upon. The thread may or may not be Thread::Current().
struct InstrumentationListener {
  InstrumentationListener() {}
  virtual ~InstrumentationListener() {}

  // Call-back for when a method is entered.
  virtual void MethodEntered(Thread* thread,
                             Handle<mirror::Object> this_object,
                             ArtMethod* method,
                             uint32_t dex_pc) REQUIRES_SHARED(Locks::mutator_lock_) = 0;

  virtual void MethodExited(Thread* thread,
                            Handle<mirror::Object> this_object,
                            ArtMethod* method,
                            uint32_t dex_pc,
                            Handle<mirror::Object> return_value)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Call-back for when a method is exited. The implementor should either handler-ize the return
  // value (if appropriate) or use the alternate MethodExited callback instead if they need to
  // go through a suspend point.
  virtual void MethodExited(Thread* thread,
                            Handle<mirror::Object> this_object,
                            ArtMethod* method,
                            uint32_t dex_pc,
                            const JValue& return_value)
      REQUIRES_SHARED(Locks::mutator_lock_) = 0;

  // Call-back for when a method is popped due to an exception throw. A method will either cause a
  // MethodExited call-back or a MethodUnwind call-back when its activation is removed.
  virtual void MethodUnwind(Thread* thread,
                            Handle<mirror::Object> this_object,
                            ArtMethod* method,
                            uint32_t dex_pc)
      REQUIRES_SHARED(Locks::mutator_lock_) = 0;

  // Call-back for when the dex pc moves in a method.
  virtual void DexPcMoved(Thread* thread,
                          Handle<mirror::Object> this_object,
                          ArtMethod* method,
                          uint32_t new_dex_pc)
      REQUIRES_SHARED(Locks::mutator_lock_) = 0;

  // Call-back for when we read from a field.
  virtual void FieldRead(Thread* thread,
                         Handle<mirror::Object> this_object,
                         ArtMethod* method,
                         uint32_t dex_pc,
                         ArtField* field) = 0;

  virtual void FieldWritten(Thread* thread,
                            Handle<mirror::Object> this_object,
                            ArtMethod* method,
                            uint32_t dex_pc,
                            ArtField* field,
                            Handle<mirror::Object> field_value)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Call-back for when we write into a field.
  virtual void FieldWritten(Thread* thread,
                            Handle<mirror::Object> this_object,
                            ArtMethod* method,
                            uint32_t dex_pc,
                            ArtField* field,
                            const JValue& field_value)
      REQUIRES_SHARED(Locks::mutator_lock_) = 0;

  // Call-back when an exception is thrown.
  virtual void ExceptionThrown(Thread* thread,
                               Handle<mirror::Throwable> exception_object)
      REQUIRES_SHARED(Locks::mutator_lock_) = 0;

  // Call-back when an exception is caught/handled by java code.
  virtual void ExceptionHandled(Thread* thread, Handle<mirror::Throwable> exception_object)
      REQUIRES_SHARED(Locks::mutator_lock_) = 0;

  // Call-back for when we execute a branch.
  virtual void Branch(Thread* thread,
                      ArtMethod* method,
                      uint32_t dex_pc,
                      int32_t dex_pc_offset)
      REQUIRES_SHARED(Locks::mutator_lock_) = 0;

  // Call-back when a shadow_frame with the needs_notify_pop_ boolean set is popped off the stack by
  // either return or exceptions. Normally instrumentation listeners should ensure that there are
  // shadow-frames by deoptimizing stacks.
  virtual void WatchedFramePop(Thread* thread ATTRIBUTE_UNUSED,
                               const ShadowFrame& frame ATTRIBUTE_UNUSED)
      REQUIRES_SHARED(Locks::mutator_lock_) = 0;
};

class Instrumentation;
// A helper to send instrumentation events while popping the stack in a safe way.
class InstrumentationStackPopper {
 public:
  explicit InstrumentationStackPopper(Thread* self);
  ~InstrumentationStackPopper() REQUIRES_SHARED(Locks::mutator_lock_);

  // Increase the number of frames being popped to 'desired_pops' return true if the frames were
  // popped without any exceptions, false otherwise. The exception that caused the pop is
  // 'exception'.
  bool PopFramesTo(uint32_t desired_pops, /*in-out*/MutableHandle<mirror::Throwable>& exception)
      REQUIRES_SHARED(Locks::mutator_lock_);

 private:
  Thread* self_;
  Instrumentation* instrumentation_;
  uint32_t frames_to_remove_;
};

// Instrumentation is a catch-all for when extra information is required from the runtime. The
// typical use for instrumentation is for profiling and debugging. Instrumentation may add stubs
// to method entry and exit, it may also force execution to be switched to the interpreter and
// trigger deoptimization.
class Instrumentation {
 public:
  enum InstrumentationEvent {
    kMethodEntered = 0x1,
    kMethodExited = 0x2,
    kMethodUnwind = 0x4,
    kDexPcMoved = 0x8,
    kFieldRead = 0x10,
    kFieldWritten = 0x20,
    kExceptionThrown = 0x40,
    kBranch = 0x80,
    kWatchedFramePop = 0x200,
    kExceptionHandled = 0x400,
  };

  enum class InstrumentationLevel {
    kInstrumentNothing,                   // execute without instrumentation
    kInstrumentWithInstrumentationStubs,  // execute with instrumentation entry/exit stubs
    kInstrumentWithInterpreter            // execute with interpreter
  };

  Instrumentation();

  // Add a listener to be notified of the masked together sent of instrumentation events. This
  // suspend the runtime to install stubs. You are expected to hold the mutator lock as a proxy
  // for saying you should have suspended all threads (installing stubs while threads are running
  // will break).
  void AddListener(InstrumentationListener* listener, uint32_t events)
      REQUIRES(Locks::mutator_lock_, !Locks::thread_list_lock_, !Locks::classlinker_classes_lock_);

  // Removes a listener possibly removing instrumentation stubs.
  void RemoveListener(InstrumentationListener* listener, uint32_t events)
      REQUIRES(Locks::mutator_lock_, !Locks::thread_list_lock_, !Locks::classlinker_classes_lock_);

  // Deoptimization.
  void EnableDeoptimization()
      REQUIRES(Locks::mutator_lock_)
      REQUIRES(!GetDeoptimizedMethodsLock());
  // Calls UndeoptimizeEverything which may visit class linker classes through ConfigureStubs.
  void DisableDeoptimization(const char* key)
      REQUIRES(Locks::mutator_lock_, Roles::uninterruptible_)
      REQUIRES(!GetDeoptimizedMethodsLock());

  bool AreAllMethodsDeoptimized() const {
    return interpreter_stubs_installed_;
  }
  bool ShouldNotifyMethodEnterExitEvents() const REQUIRES_SHARED(Locks::mutator_lock_);

  bool CanDeoptimize() {
    return deoptimization_enabled_;
  }

  // Executes everything with interpreter.
  void DeoptimizeEverything(const char* key)
      REQUIRES(Locks::mutator_lock_, Roles::uninterruptible_)
      REQUIRES(!Locks::thread_list_lock_,
               !Locks::classlinker_classes_lock_,
               !GetDeoptimizedMethodsLock());

  // Executes everything with compiled code (or interpreter if there is no code). May visit class
  // linker classes through ConfigureStubs.
  void UndeoptimizeEverything(const char* key)
      REQUIRES(Locks::mutator_lock_, Roles::uninterruptible_)
      REQUIRES(!Locks::thread_list_lock_,
               !Locks::classlinker_classes_lock_,
               !GetDeoptimizedMethodsLock());

  // Deoptimize a method by forcing its execution with the interpreter. Nevertheless, a static
  // method (except a class initializer) set to the resolution trampoline will be deoptimized only
  // once its declaring class is initialized.
  void Deoptimize(ArtMethod* method)
      REQUIRES(Locks::mutator_lock_, !Locks::thread_list_lock_, !GetDeoptimizedMethodsLock());

  // Undeoptimze the method by restoring its entrypoints. Nevertheless, a static method
  // (except a class initializer) set to the resolution trampoline will be updated only once its
  // declaring class is initialized.
  void Undeoptimize(ArtMethod* method)
      REQUIRES(Locks::mutator_lock_, !Locks::thread_list_lock_, !GetDeoptimizedMethodsLock());

  // Indicates whether the method has been deoptimized so it is executed with the interpreter.
  bool IsDeoptimized(ArtMethod* method)
      REQUIRES(!GetDeoptimizedMethodsLock()) REQUIRES_SHARED(Locks::mutator_lock_);

  // Enable method tracing by installing instrumentation entry/exit stubs or interpreter.
  void EnableMethodTracing(const char* key,
                           bool needs_interpreter = kDeoptimizeForAccurateMethodEntryExitListeners)
      REQUIRES(Locks::mutator_lock_, Roles::uninterruptible_)
      REQUIRES(!Locks::thread_list_lock_,
               !Locks::classlinker_classes_lock_,
               !GetDeoptimizedMethodsLock());

  // Disable method tracing by uninstalling instrumentation entry/exit stubs or interpreter.
  void DisableMethodTracing(const char* key)
      REQUIRES(Locks::mutator_lock_, Roles::uninterruptible_)
      REQUIRES(!Locks::thread_list_lock_,
               !Locks::classlinker_classes_lock_,
               !GetDeoptimizedMethodsLock());

  InterpreterHandlerTable GetInterpreterHandlerTable() const
      REQUIRES_SHARED(Locks::mutator_lock_) {
    return interpreter_handler_table_;
  }

  void InstrumentQuickAllocEntryPoints() REQUIRES(!Locks::instrument_entrypoints_lock_);
  void UninstrumentQuickAllocEntryPoints() REQUIRES(!Locks::instrument_entrypoints_lock_);
  void InstrumentQuickAllocEntryPointsLocked()
      REQUIRES(Locks::instrument_entrypoints_lock_, !Locks::thread_list_lock_,
               !Locks::runtime_shutdown_lock_);
  void UninstrumentQuickAllocEntryPointsLocked()
      REQUIRES(Locks::instrument_entrypoints_lock_, !Locks::thread_list_lock_,
               !Locks::runtime_shutdown_lock_);
  void ResetQuickAllocEntryPoints() REQUIRES(Locks::runtime_shutdown_lock_);

  // Update the code of a method respecting any installed stubs.
  void UpdateMethodsCode(ArtMethod* method, const void* quick_code)
      REQUIRES_SHARED(Locks::mutator_lock_) REQUIRES(!GetDeoptimizedMethodsLock());

  // Update the code of a native method to a JITed stub.
  void UpdateNativeMethodsCodeToJitCode(ArtMethod* method, const void* quick_code)
      REQUIRES_SHARED(Locks::mutator_lock_) REQUIRES(!GetDeoptimizedMethodsLock());

  // Update the code of a method to the interpreter respecting any installed stubs from debugger.
  void UpdateMethodsCodeToInterpreterEntryPoint(ArtMethod* method)
      REQUIRES_SHARED(Locks::mutator_lock_) REQUIRES(!GetDeoptimizedMethodsLock());

  // Update the code of a method respecting any installed stubs from debugger.
  void UpdateMethodsCodeForJavaDebuggable(ArtMethod* method, const void* quick_code)
      REQUIRES_SHARED(Locks::mutator_lock_) REQUIRES(!GetDeoptimizedMethodsLock());

  // Return the code that we can execute for an invoke including from the JIT.
  const void* GetCodeForInvoke(ArtMethod* method) const
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Get the quick code for the given method. More efficient than asking the class linker as it
  // will short-cut to GetCode if instrumentation and static method resolution stubs aren't
  // installed.
  const void* GetQuickCodeFor(ArtMethod* method, PointerSize pointer_size) const
      REQUIRES_SHARED(Locks::mutator_lock_);

  void ForceInterpretOnly() {
    interpret_only_ = true;
    forced_interpret_only_ = true;
  }

  // Called by ArtMethod::Invoke to determine dispatch mechanism.
  bool InterpretOnly() const {
    return interpret_only_;
  }

  bool IsForcedInterpretOnly() const {
    return forced_interpret_only_;
  }

  // Code is in boot image oat file which isn't compiled as debuggable.
  // Need debug version (interpreter or jitted) if that's the case.
  bool NeedDebugVersionFor(ArtMethod* method) const
      REQUIRES_SHARED(Locks::mutator_lock_);

  bool AreExitStubsInstalled() const {
    return instrumentation_stubs_installed_;
  }

  bool HasMethodEntryListeners() const REQUIRES_SHARED(Locks::mutator_lock_) {
    return have_method_entry_listeners_;
  }

  bool HasMethodExitListeners() const REQUIRES_SHARED(Locks::mutator_lock_) {
    return have_method_exit_listeners_;
  }

  bool HasMethodUnwindListeners() const REQUIRES_SHARED(Locks::mutator_lock_) {
    return have_method_unwind_listeners_;
  }

  bool HasDexPcListeners() const REQUIRES_SHARED(Locks::mutator_lock_) {
    return have_dex_pc_listeners_;
  }

  bool HasFieldReadListeners() const REQUIRES_SHARED(Locks::mutator_lock_) {
    return have_field_read_listeners_;
  }

  bool HasFieldWriteListeners() const REQUIRES_SHARED(Locks::mutator_lock_) {
    return have_field_write_listeners_;
  }

  bool HasExceptionThrownListeners() const REQUIRES_SHARED(Locks::mutator_lock_) {
    return have_exception_thrown_listeners_;
  }

  bool HasBranchListeners() const REQUIRES_SHARED(Locks::mutator_lock_) {
    return have_branch_listeners_;
  }

  bool HasWatchedFramePopListeners() const REQUIRES_SHARED(Locks::mutator_lock_) {
    return have_watched_frame_pop_listeners_;
  }

  bool HasExceptionHandledListeners() const REQUIRES_SHARED(Locks::mutator_lock_) {
    return have_exception_handled_listeners_;
  }

  bool IsActive() const REQUIRES_SHARED(Locks::mutator_lock_) {
    return have_dex_pc_listeners_ || have_method_entry_listeners_ || have_method_exit_listeners_ ||
        have_field_read_listeners_ || have_field_write_listeners_ ||
        have_exception_thrown_listeners_ || have_method_unwind_listeners_ ||
        have_branch_listeners_ || have_watched_frame_pop_listeners_ ||
        have_exception_handled_listeners_;
  }

  // Inform listeners that a method has been entered. A dex PC is provided as we may install
  // listeners into executing code and get method enter events for methods already on the stack.
  void MethodEnterEvent(Thread* thread, mirror::Object* this_object,
                        ArtMethod* method, uint32_t dex_pc) const
      REQUIRES_SHARED(Locks::mutator_lock_) {
    if (UNLIKELY(HasMethodEntryListeners())) {
      MethodEnterEventImpl(thread, this_object, method, dex_pc);
    }
  }

  // Inform listeners that a method has been exited.
  void MethodExitEvent(Thread* thread,
                       mirror::Object* this_object,
                       ArtMethod* method,
                       uint32_t dex_pc,
                       const JValue& return_value) const
      REQUIRES_SHARED(Locks::mutator_lock_) {
    if (UNLIKELY(HasMethodExitListeners())) {
      MethodExitEventImpl(thread, this_object, method, dex_pc, return_value);
    }
  }

  // Inform listeners that a method has been exited due to an exception.
  void MethodUnwindEvent(Thread* thread, mirror::Object* this_object,
                         ArtMethod* method, uint32_t dex_pc) const
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Inform listeners that the dex pc has moved (only supported by the interpreter).
  void DexPcMovedEvent(Thread* thread, mirror::Object* this_object,
                       ArtMethod* method, uint32_t dex_pc) const
      REQUIRES_SHARED(Locks::mutator_lock_) {
    if (UNLIKELY(HasDexPcListeners())) {
      DexPcMovedEventImpl(thread, this_object, method, dex_pc);
    }
  }

  // Inform listeners that a branch has been taken (only supported by the interpreter).
  void Branch(Thread* thread, ArtMethod* method, uint32_t dex_pc, int32_t offset) const
      REQUIRES_SHARED(Locks::mutator_lock_) {
    if (UNLIKELY(HasBranchListeners())) {
      BranchImpl(thread, method, dex_pc, offset);
    }
  }

  // Inform listeners that we read a field (only supported by the interpreter).
  void FieldReadEvent(Thread* thread, mirror::Object* this_object,
                      ArtMethod* method, uint32_t dex_pc,
                      ArtField* field) const
      REQUIRES_SHARED(Locks::mutator_lock_) {
    if (UNLIKELY(HasFieldReadListeners())) {
      FieldReadEventImpl(thread, this_object, method, dex_pc, field);
    }
  }

  // Inform listeners that we write a field (only supported by the interpreter).
  void FieldWriteEvent(Thread* thread, mirror::Object* this_object,
                       ArtMethod* method, uint32_t dex_pc,
                       ArtField* field, const JValue& field_value) const
      REQUIRES_SHARED(Locks::mutator_lock_) {
    if (UNLIKELY(HasFieldWriteListeners())) {
      FieldWriteEventImpl(thread, this_object, method, dex_pc, field, field_value);
    }
  }

  // Inform listeners that a branch has been taken (only supported by the interpreter).
  void WatchedFramePopped(Thread* thread, const ShadowFrame& frame) const
      REQUIRES_SHARED(Locks::mutator_lock_) {
    if (UNLIKELY(HasWatchedFramePopListeners())) {
      WatchedFramePopImpl(thread, frame);
    }
  }

  // Inform listeners that an exception was thrown.
  void ExceptionThrownEvent(Thread* thread, mirror::Throwable* exception_object) const
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Inform listeners that an exception has been handled. This is not sent for native code or for
  // exceptions which reach the end of the thread's stack.
  void ExceptionHandledEvent(Thread* thread, mirror::Throwable* exception_object) const
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Called when an instrumented method is entered. The intended link register (lr) is saved so
  // that returning causes a branch to the method exit stub. Generates method enter events.
  void PushInstrumentationStackFrame(Thread* self, mirror::Object* this_object,
                                     ArtMethod* method, uintptr_t lr,
                                     bool interpreter_entry)
      REQUIRES_SHARED(Locks::mutator_lock_);

  DeoptimizationMethodType GetDeoptimizationMethodType(ArtMethod* method)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Called when an instrumented method is exited. Removes the pushed instrumentation frame
  // returning the intended link register. Generates method exit events. The gpr_result and
  // fpr_result pointers are pointers to the locations where the integer/pointer and floating point
  // result values of the function are stored. Both pointers must always be valid but the values
  // held there will only be meaningful if interpreted as the appropriate type given the function
  // being returned from.
  TwoWordReturn PopInstrumentationStackFrame(Thread* self, uintptr_t* return_pc,
                                             uint64_t* gpr_result, uint64_t* fpr_result)
      REQUIRES_SHARED(Locks::mutator_lock_) REQUIRES(!GetDeoptimizedMethodsLock());

  // Pops nframes instrumentation frames from the current thread. Returns the return pc for the last
  // instrumentation frame that's popped.
  uintptr_t PopFramesForDeoptimization(Thread* self, size_t nframes) const
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Call back for configure stubs.
  void InstallStubsForClass(mirror::Class* klass) REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!GetDeoptimizedMethodsLock());

  void InstallStubsForMethod(ArtMethod* method)
      REQUIRES_SHARED(Locks::mutator_lock_) REQUIRES(!GetDeoptimizedMethodsLock());

  // Install instrumentation exit stub on every method of the stack of the given thread.
  // This is used by the debugger to cause a deoptimization of the thread's stack after updating
  // local variable(s).
  void InstrumentThreadStack(Thread* thread)
      REQUIRES_SHARED(Locks::mutator_lock_);

  static size_t ComputeFrameId(Thread* self,
                               size_t frame_depth,
                               size_t inlined_frames_before_frame)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Does not hold lock, used to check if someone changed from not instrumented to instrumented
  // during a GC suspend point.
  bool AllocEntrypointsInstrumented() const REQUIRES_SHARED(Locks::mutator_lock_) {
    return alloc_entrypoints_instrumented_;
  }

  InstrumentationLevel GetCurrentInstrumentationLevel() const;

 private:
  // Returns true if moving to the given instrumentation level requires the installation of stubs.
  // False otherwise.
  bool RequiresInstrumentationInstallation(InstrumentationLevel new_level) const;

  // Does the job of installing or removing instrumentation code within methods.
  // In order to support multiple clients using instrumentation at the same time,
  // the caller must pass a unique key (a string) identifying it so we remind which
  // instrumentation level it needs. Therefore the current instrumentation level
  // becomes the highest instrumentation level required by a client.
  void ConfigureStubs(const char* key, InstrumentationLevel desired_instrumentation_level)
      REQUIRES(Locks::mutator_lock_, Roles::uninterruptible_)
      REQUIRES(!GetDeoptimizedMethodsLock(),
               !Locks::thread_list_lock_,
               !Locks::classlinker_classes_lock_);

  void UpdateInterpreterHandlerTable() REQUIRES(Locks::mutator_lock_) {
    /*
     * TUNING: Dalvik's mterp stashes the actual current handler table base in a
     * tls field.  For Arm, this enables all suspend, debug & tracing checks to be
     * collapsed into a single conditionally-executed ldw instruction.
     * Move to Dalvik-style handler-table management for both the goto interpreter and
     * mterp.
     */
    interpreter_handler_table_ = IsActive() ? kAlternativeHandlerTable : kMainHandlerTable;
  }

  // No thread safety analysis to get around SetQuickAllocEntryPointsInstrumented requiring
  // exclusive access to mutator lock which you can't get if the runtime isn't started.
  void SetEntrypointsInstrumented(bool instrumented) NO_THREAD_SAFETY_ANALYSIS;

  void MethodEnterEventImpl(Thread* thread,
                            ObjPtr<mirror::Object> this_object,
                            ArtMethod* method,
                            uint32_t dex_pc) const
      REQUIRES_SHARED(Locks::mutator_lock_);
  void MethodExitEventImpl(Thread* thread,
                           ObjPtr<mirror::Object> this_object,
                           ArtMethod* method,
                           uint32_t dex_pc,
                           const JValue& return_value) const
      REQUIRES_SHARED(Locks::mutator_lock_);
  void DexPcMovedEventImpl(Thread* thread,
                           ObjPtr<mirror::Object> this_object,
                           ArtMethod* method,
                           uint32_t dex_pc) const
      REQUIRES_SHARED(Locks::mutator_lock_);
  void BranchImpl(Thread* thread, ArtMethod* method, uint32_t dex_pc, int32_t offset) const
      REQUIRES_SHARED(Locks::mutator_lock_);
  void WatchedFramePopImpl(Thread* thread, const ShadowFrame& frame) const
      REQUIRES_SHARED(Locks::mutator_lock_);
  void FieldReadEventImpl(Thread* thread,
                          ObjPtr<mirror::Object> this_object,
                          ArtMethod* method,
                          uint32_t dex_pc,
                          ArtField* field) const
      REQUIRES_SHARED(Locks::mutator_lock_);
  void FieldWriteEventImpl(Thread* thread,
                           ObjPtr<mirror::Object> this_object,
                           ArtMethod* method,
                           uint32_t dex_pc,
                           ArtField* field,
                           const JValue& field_value) const
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Read barrier-aware utility functions for accessing deoptimized_methods_
  bool AddDeoptimizedMethod(ArtMethod* method)
      REQUIRES_SHARED(Locks::mutator_lock_) REQUIRES(GetDeoptimizedMethodsLock());
  bool IsDeoptimizedMethod(ArtMethod* method)
      REQUIRES_SHARED(Locks::mutator_lock_, GetDeoptimizedMethodsLock());
  bool RemoveDeoptimizedMethod(ArtMethod* method)
      REQUIRES_SHARED(Locks::mutator_lock_) REQUIRES(GetDeoptimizedMethodsLock());
  ArtMethod* BeginDeoptimizedMethod()
      REQUIRES_SHARED(Locks::mutator_lock_, GetDeoptimizedMethodsLock());
  bool IsDeoptimizedMethodsEmpty() const
      REQUIRES_SHARED(Locks::mutator_lock_, GetDeoptimizedMethodsLock());
  void UpdateMethodsCodeImpl(ArtMethod* method, const void* quick_code)
      REQUIRES_SHARED(Locks::mutator_lock_) REQUIRES(!GetDeoptimizedMethodsLock());

  ReaderWriterMutex* GetDeoptimizedMethodsLock() const {
    return deoptimized_methods_lock_.get();
  }

  // Have we hijacked ArtMethod::code_ so that it calls instrumentation/interpreter code?
  bool instrumentation_stubs_installed_;

  // Have we hijacked ArtMethod::code_ to reference the enter/exit stubs?
  bool entry_exit_stubs_installed_;

  // Have we hijacked ArtMethod::code_ to reference the enter interpreter stub?
  bool interpreter_stubs_installed_;

  // Do we need the fidelity of events that we only get from running within the interpreter?
  bool interpret_only_;

  // Did the runtime request we only run in the interpreter? ie -Xint mode.
  bool forced_interpret_only_;

  // Do we have any listeners for method entry events? Short-cut to avoid taking the
  // instrumentation_lock_.
  bool have_method_entry_listeners_ GUARDED_BY(Locks::mutator_lock_);

  // Do we have any listeners for method exit events? Short-cut to avoid taking the
  // instrumentation_lock_.
  bool have_method_exit_listeners_ GUARDED_BY(Locks::mutator_lock_);

  // Do we have any listeners for method unwind events? Short-cut to avoid taking the
  // instrumentation_lock_.
  bool have_method_unwind_listeners_ GUARDED_BY(Locks::mutator_lock_);

  // Do we have any listeners for dex move events? Short-cut to avoid taking the
  // instrumentation_lock_.
  bool have_dex_pc_listeners_ GUARDED_BY(Locks::mutator_lock_);

  // Do we have any listeners for field read events? Short-cut to avoid taking the
  // instrumentation_lock_.
  bool have_field_read_listeners_ GUARDED_BY(Locks::mutator_lock_);

  // Do we have any listeners for field write events? Short-cut to avoid taking the
  // instrumentation_lock_.
  bool have_field_write_listeners_ GUARDED_BY(Locks::mutator_lock_);

  // Do we have any exception thrown listeners? Short-cut to avoid taking the instrumentation_lock_.
  bool have_exception_thrown_listeners_ GUARDED_BY(Locks::mutator_lock_);

  // Do we have any frame pop listeners? Short-cut to avoid taking the instrumentation_lock_.
  bool have_watched_frame_pop_listeners_ GUARDED_BY(Locks::mutator_lock_);

  // Do we have any branch listeners? Short-cut to avoid taking the instrumentation_lock_.
  bool have_branch_listeners_ GUARDED_BY(Locks::mutator_lock_);

  // Do we have any exception handled listeners? Short-cut to avoid taking the
  // instrumentation_lock_.
  bool have_exception_handled_listeners_ GUARDED_BY(Locks::mutator_lock_);

  // Contains the instrumentation level required by each client of the instrumentation identified
  // by a string key.
  typedef SafeMap<const char*, InstrumentationLevel> InstrumentationLevelTable;
  InstrumentationLevelTable requested_instrumentation_levels_ GUARDED_BY(Locks::mutator_lock_);

  // The event listeners, written to with the mutator_lock_ exclusively held.
  // Mutators must be able to iterate over these lists concurrently, that is, with listeners being
  // added or removed while iterating. The modifying thread holds exclusive lock,
  // so other threads cannot iterate (i.e. read the data of the list) at the same time but they
  // do keep iterators that need to remain valid. This is the reason these listeners are std::list
  // and not for example std::vector: the existing storage for a std::list does not move.
  // Note that mutators cannot make a copy of these lists before iterating, as the instrumentation
  // listeners can also be deleted concurrently.
  // As a result, these lists are never trimmed. That's acceptable given the low number of
  // listeners we have.
  std::list<InstrumentationListener*> method_entry_listeners_ GUARDED_BY(Locks::mutator_lock_);
  std::list<InstrumentationListener*> method_exit_listeners_ GUARDED_BY(Locks::mutator_lock_);
  std::list<InstrumentationListener*> method_unwind_listeners_ GUARDED_BY(Locks::mutator_lock_);
  std::list<InstrumentationListener*> branch_listeners_ GUARDED_BY(Locks::mutator_lock_);
  std::list<InstrumentationListener*> dex_pc_listeners_ GUARDED_BY(Locks::mutator_lock_);
  std::list<InstrumentationListener*> field_read_listeners_ GUARDED_BY(Locks::mutator_lock_);
  std::list<InstrumentationListener*> field_write_listeners_ GUARDED_BY(Locks::mutator_lock_);
  std::list<InstrumentationListener*> exception_thrown_listeners_ GUARDED_BY(Locks::mutator_lock_);
  std::list<InstrumentationListener*> watched_frame_pop_listeners_ GUARDED_BY(Locks::mutator_lock_);
  std::list<InstrumentationListener*> exception_handled_listeners_ GUARDED_BY(Locks::mutator_lock_);

  // The set of methods being deoptimized (by the debugger) which must be executed with interpreter
  // only.
  mutable std::unique_ptr<ReaderWriterMutex> deoptimized_methods_lock_ BOTTOM_MUTEX_ACQUIRED_AFTER;
  std::unordered_set<ArtMethod*> deoptimized_methods_ GUARDED_BY(GetDeoptimizedMethodsLock());
  bool deoptimization_enabled_;

  // Current interpreter handler table. This is updated each time the thread state flags are
  // modified.
  InterpreterHandlerTable interpreter_handler_table_ GUARDED_BY(Locks::mutator_lock_);

  // Greater than 0 if quick alloc entry points instrumented.
  size_t quick_alloc_entry_points_instrumentation_counter_;

  // alloc_entrypoints_instrumented_ is only updated with all the threads suspended, this is done
  // to prevent races with the GC where the GC relies on thread suspension only see
  // alloc_entrypoints_instrumented_ change during suspend points.
  bool alloc_entrypoints_instrumented_;

  friend class InstrumentationTest;  // For GetCurrentInstrumentationLevel and ConfigureStubs.
  friend class InstrumentationStackPopper;  // For popping instrumentation frames.

  DISALLOW_COPY_AND_ASSIGN(Instrumentation);
};
std::ostream& operator<<(std::ostream& os, const Instrumentation::InstrumentationEvent& rhs);
std::ostream& operator<<(std::ostream& os, const Instrumentation::InstrumentationLevel& rhs);

// An element in the instrumentation side stack maintained in art::Thread.
struct InstrumentationStackFrame {
  InstrumentationStackFrame(mirror::Object* this_object,
                            ArtMethod* method,
                            uintptr_t return_pc,
                            size_t frame_id,
                            bool interpreter_entry)
      : this_object_(this_object),
        method_(method),
        return_pc_(return_pc),
        frame_id_(frame_id),
        interpreter_entry_(interpreter_entry) {
  }

  std::string Dump() const REQUIRES_SHARED(Locks::mutator_lock_);

  mirror::Object* this_object_;
  ArtMethod* method_;
  uintptr_t return_pc_;
  size_t frame_id_;
  bool interpreter_entry_;
};

}  // namespace instrumentation
}  // namespace art

#endif  // ART_RUNTIME_INSTRUMENTATION_H_
