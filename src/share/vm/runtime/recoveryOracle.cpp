#include "runtime/recoveryOracle.hpp"

#include "precompiled.hpp"

#include "interpreter/oopMapCache.hpp"

bool has_string_void_init(KlassHandle klass);

bool RecoveryAction::can_early_return() {
  if (_recovery_type == RecoveryOracle::_early_return) {
    return _early_return_offset != -1;
  }
  return false;
}

bool RecoveryAction::can_error_transformation() {
  if (_recovery_type == RecoveryOracle::_error_transformation) {
    return _target_exception_klass != NULL;
  }

  return false;
}

void RecoveryAction::print() {
  ResourceMark rm;
  tty->print_cr("(%s)(%s)(%s)",
      origin_exception()->klass()->name()->as_C_string(),
      RecoveryOracle::failure_type_name(_failure_type),
      RecoveryOracle::recovery_type_name(_recovery_type)
      );
}

Handle RecoveryAction::allocate_target_exception(JavaThread* thread, Handle origin_exception) {
  KlassHandle exception_klass(thread, target_exception_klass());
  return RecoveryOracle::allocate_target_exception(thread, origin_exception, exception_klass);
}

void recoveryOracle_init(){
  RecoveryOracle::initialize();
}

redisContext* RecoveryOracle::_context = NULL;

redisContext* RecoveryOracle::context(){
  return _context;
}

const char* get_recovery_mode(){
  if(UseRedis){
    if(UseInduced){
      return "UseInduced";
    }
    return "UseRedis";
  }

  if(UseStack){
    return "UseStack";
  }

  return "ForceThrowable";
}

void RecoveryOracle::initialize() {

  if (UseRedis) {
    redisContext* c = redisConnect("127.0.0.1", 6379);
    if (c != NULL && c->err) {
      tty->print_cr("ERROR: create redis context failed %s\n", c->errstr);
      return;
    }
    _context = c;
  }

}

// Caller should call this first before allocate a RecoveryAction
// and call recover()
bool RecoveryOracle::quick_cannot_recover_check(JavaThread* thread, Handle exception) {
  if (!EnableRecovery) {
    return true;
  }

  if (exception.is_null()){
    return true;
  }

  if (thread->runtime_recovery_state()->last_checked_exception() == exception()) { // we checked, we failed
    return true;
  }

  // TODO Deprecated RuntimeExceptionOnly option
  if (/*RuntimeExeptionOnly && */
      !exception->is_a(SystemDictionary::RuntimeException_klass())){
    return true;
  }

  return false;
}

const char* RecoveryOracle::failure_type_name(FailureType type) {
  switch(type) {
  case _not_a_failure:
    return "not a failure";
  case _recovery_disabled:
    return "recovery disabled";
  case _internal_error:
    return "internal error";
  case _uncaught_exception:
    return "uncaught exception";
  case _trivially_handled:
    return "trivially handled";
  default:
    break;
  }
  return "invalid type";
}


const char* RecoveryOracle::recovery_type_name(RecoveryType type) {
  switch(type) {
  case _no_recovery:
    return "no recovery";
  case _error_transformation:
    return "error transformation";
  case _early_return:
    return "early return";
  default:
    break;
  }

  return "invalid recovery type";
}


bool RecoveryOracle::is_trivial_handler(KlassHandle handler_klass) {
  assert(handler_klass.not_null(), "sanity check");

  return handler_klass() == SystemDictionary::Throwable_klass()
    || handler_klass() == SystemDictionary::Exception_klass();
}

void RecoveryOracle::recover(JavaThread* thread, RecoveryAction* action) {
  elapsedTimer timer;
  if ((TraceRuntimeRecovery & TRACE_PRINT_ACTION) != 0) {
    timer.start();
  }

  do_recover(thread, action);

  if ((TraceRuntimeRecovery & TRACE_PRINT_ACTION) != 0) {
    timer.stop();
    tty->print_cr("recovery time: %lxms, result: %d", timer.milliseconds(), can_recover(action));
  }
}

bool RecoveryOracle::is_sun_reflect_NativeMethodAccessorImpl(methodHandle mh) {

  if(mh->is_native()) {
    if (mh->name()->equals("invoke0")) {
      if (mh->method_holder()->name()->equals("sun/reflect/NativeMethodAccessorImpl")) {
        return true;
      }
    }
  }

  return false;
}

void RecoveryOracle::do_recover(JavaThread* thread, RecoveryAction* action) {
  // Caller should do this check first
  //assert(!quick_cannot_recover_check(thread, action->origin_exception()), "sanity check");
  assert(action->origin_exception().not_null(), "sanity check");
  // Any way we checked this exception
  //assert(!thread->runtime_recovery_state()->has_last_checked_exception(), "sanity check");
  thread->runtime_recovery_state()->set_last_checked_exception(action->origin_exception()());
  assert(thread->runtime_recovery_state()->has_last_checked_exception(), "sanity check");

  ResourceMark rm(thread);

  if ((TraceRuntimeRecovery & TRACE_PRINT_STACK) != 0) {
    tty->print_cr("recover: Begin printing stack trace of the original exception.");
    java_lang_Throwable::print(action->origin_exception()(), tty);
    tty->cr();
    java_lang_Throwable::print_stack_trace(action->origin_exception()(), tty);
    tty->print_cr("recover: End printing stack trace of the original exception.");
  }

  // 0). Collect stack
  GrowableArray<methodHandle>* methods = new GrowableArray<methodHandle>(50);
  GrowableArray<int>* bcis = new GrowableArray<int>(50);
  fill_stack(thread, methods, bcis);

  if (methods->length() == 0) {
    action->set_recovery_type(_no_recovery);
    if (TraceRuntimeRecovery > 0) {
      tty->print_cr("recovery stack is empty.");
    }
    return;
  }


  // 1). Determine failure type and recovery context
  determine_failure_type_and_recovery_context(thread, methods, bcis, action);

  if (!require_recovery(action->failure_type())) {
    return;
  }

  // We need to check whether we will recover in unsafe <init>
  // which will produce invalid object.
  if (has_unsafe_init(thread, methods, bcis, action)) {
    action->set_recovery_type(_no_recovery);
    thread->runtime_recovery_state()->set_last_checked_exception(NULL);
    return;
  }


#ifdef ASSERT
 {
    methodHandle top_method = methods->at(0);
    if(top_method->is_native()) {
      if (top_method->name()->equals("invoke0")) {
        if (top_method->method_holder()->name()->equals("sun/reflect/NativeMethodAccessorImpl")) {
          // invoke0 will wrap all caught throwable into an InvocationTargetException
          //action->set_recovery_type(_no_recovery);
          //thread->runtime_recovery_state()->set_last_checked_exception(NULL);
          //return;
          assert(action->has_top_method(), "sanity check, top method must be setted in reflection.cpp");
        }
      }
    }
  }
#endif

  if ((TraceRuntimeRecovery & TRACE_CHECKING) != 0) {
    tty->print_cr("recover: FailureType: %s, recovery context offset=%d",
        failure_type_name(action->failure_type()),
        action->recovery_context_offset());
  }

  // double check
  assert(action->recovery_context_offset() >= 0, "sanity check");

  // 2). Determine recovery action
  determine_recovery_action(thread, methods, bcis, action);

  if ((TraceRuntimeRecovery & TRACE_CHECKING) != 0) {
    tty->print_cr("recover: RecoveryType: %s", recovery_type_name(action->recovery_type()));
  }
}


// Make fill_stack simple
bool RecoveryOracle::has_unsafe_init(JavaThread* thread, GrowableArray<methodHandle>* methods, GrowableArray<int>* bcis, RecoveryAction* action) {
  HandleMark hm(thread);

  int end_index = action->recovery_context_offset();

  if (end_index == -1) {
    end_index = methods->length() - 1;
  }

  for (int index; index <= end_index; index++) {
    methodHandle mh = methods->at(index);
    int bci = bcis->at(index);
    if (!mh->is_native()) {
      Bytecodes::Code java_code = mh->java_code_at(bci);

      if (java_code == Bytecodes::_invokespecial) { // invoke to init
        Bytecode_invoke bi(mh, bci);

        methodHandle callee = bi.static_target(thread);
        if (callee->is_initializer()) {
          if (callee->method_holder() == SystemDictionary::String_klass()) {
            if ((TraceRuntimeRecovery & TRACE_SKIP_UNSAFE) != 0) {
              ResourceMark rm(thread);
              tty->print_cr("Skip unsafe caller to <init> or <clinit> %s at %d",
                  callee->name_and_sig_as_C_string(), index);
            }
            return true;
          }
        }
      }
    }

    if (mh->is_initializer()) {
      if (mh->method_holder() == SystemDictionary::String_klass()) {
        if ((TraceRuntimeRecovery & TRACE_SKIP_UNSAFE) != 0) {
          ResourceMark rm(thread);
          tty->print_cr("Skip unsafe <init> or <clinit> %s at %d",
              mh->name_and_sig_as_C_string(), index);
        }
        return true;
      }
    }
  }

  return false;
}

void RecoveryOracle::determine_failure_type_and_recovery_context(JavaThread* thread, GrowableArray<methodHandle>* methods, GrowableArray<int>* bcis, RecoveryAction* action) {
  HandleMark hm(thread);
  const int methods_length = methods->length();
  KlassHandle ex_klass (thread, action->origin_exception()->klass());
  KlassHandle caught_klass;
  KlassHandle null_klass;
  int handler_bci;
  int handler_index;
  int index = 0;
  for (; index < methods_length; index++) {
    methodHandle current_method = methods->at(index);
    int current_bci = bcis->at(index);

    handler_bci = -1;
    caught_klass = null_klass;
    fast_exception_handler_bci_and_caught_klass_for(current_method, ex_klass, current_bci, caught_klass, handler_bci, false, thread);

    if (thread->has_pending_exception()) {
      ResourceMark rm(thread);
      Handle internal_ex (thread, thread->pending_exception());
      tty->print_cr("We encounter an internal exception when determining failure type");
      java_lang_Throwable::print_stack_trace(internal_ex(), tty);
      thread->clear_pending_exception();
      return;
    }

    if (handler_bci != -1) {
      if (caught_klass.is_null()) {
        assert(!IgnoreFinallyBlock, "We found a finally block");
        if ((TraceRuntimeRecovery & TRACE_CHECKING) != 0) {
          ResourceMark rm(thread);
          tty->print_cr("determine_failure_type: (%s) not a failure, we found a finally block, index=%d",
              ex_klass->name()->as_C_string(),
              index);
        }
        action->set_failure_type(_not_a_failure);
        return;
      }

      if (RecoverTrivial && is_trivial_handler(caught_klass)) {
        if ((TraceRuntimeRecovery & TRACE_CHECKING) != 0) {
          ResourceMark rm(thread);
          tty->print_cr("determine_failure_type: (%s) trivially handled, index=%d, caught class=%s",
              ex_klass->name()->as_C_string(),
              index,
              caught_klass->name()->as_C_string());
        }
        action->set_recovery_context_offset(index);
        action->set_failure_type(_trivially_handled);
        return;
      }

      if ((TraceRuntimeRecovery & TRACE_CHECKING) != 0) {
        ResourceMark rm(thread);
        tty->print_cr("determine_failure_type: (%s) we found a non-trivial handler, index=%d, caught class=%s",
            ex_klass->name()->as_C_string(),
            index,
            caught_klass->name()->as_C_string());
      }

      action->set_failure_type(_not_a_failure);
      return;
    }
  }

  assert(index == methods_length, "sanity check");

  if ((TraceRuntimeRecovery & TRACE_CHECKING) != 0) {
    tty->print_cr("determine_failure_type: uncaught exception, index=%d", index-1);
  }

  action->set_recovery_context_offset(index-1);
  action->set_failure_type(_uncaught_exception);
  return;
}

bool RecoveryOracle::can_recover(RecoveryAction* action) {
  return action->recovery_type() != _no_recovery;
}


Handle RecoveryOracle::allocate_target_exception(JavaThread* thread, Handle origin_exception, KlassHandle target_exception_klass) {
  ResourceMark rm(thread);
  stringStream ss;
  ss.print("Exception transformation: %s -> %s.",
      origin_exception->klass()->name()->as_C_string(),
      target_exception_klass->name()->as_C_string()
      );

  assert(has_string_void_init(target_exception_klass), "sanity check!");

  if ((TraceRuntimeRecovery & TRACE_TRANSFORMING) != 0) {
    ResourceMark rm(thread);
    tty->print_cr("Transforming an exception %s to %s..",
        origin_exception->klass()->name()->as_C_string(),
        target_exception_klass->name()->as_C_string());
  }

  Handle target_exception = Exceptions::new_exception(thread,
      target_exception_klass->name(),
      ss.as_string(),
      origin_exception,
      Handle(thread, target_exception_klass->class_loader()),
      Handle(thread, target_exception_klass->protection_domain()));

  if (thread->has_pending_exception()) {
    java_lang_Throwable::print_stack_trace(thread->pending_exception(), tty);
    thread->clear_pending_exception();
    return Handle(thread, NULL);
  }
  return target_exception;
}

void RecoveryOracle::fast_error_transformation(JavaThread* thread, GrowableArray<methodHandle>* methods, GrowableArray<int>* bcis, RecoveryAction* action) {
  const int end_index = action->recovery_context_offset();

  // TODO clear all handles
  // save result as a global JNI handle
  HandleMark hm(thread);

  KlassHandle known_exception_type;
  methodHandle handler_method;
  int handler_bci = -1;
  int handler_index = -1;
 
  methodHandle current_mh;
  int current_bci;

  if (action->has_top_method()) {
    current_mh = action->top_method();
    const int ce_length = current_mh->checked_exceptions_length();

    if (ce_length > 0) {
      if (has_known_exception_handler(0, end_index, methods, bcis,
            current_mh, known_exception_type, handler_method, handler_bci, handler_index, thread)) {
        // TODO
        assert(!thread->has_pending_exception(), "sanity check");
        assert(known_exception_type.not_null(), "sanity check");
        assert(handler_bci != -1, "sanity check");
        assert(handler_index != -1, "sanity check");

        if ((TraceRuntimeRecovery & TRACE_TRANSFORMING) != 0) {
          ResourceMark rm(thread);
          tty->print_cr("fast_error_transformation: (target_exception_klass=%s)",
              known_exception_type->name()->as_C_string());
        }

        if ((TraceRuntimeRecovery & TRACE_PRINT_ACTION) != 0) {
          ResourceMark rm(thread);
          tty->print_cr("(%s) (%s) (error transformation) (end_index=%d, target_exception_klass=%s, top method)",
              action->origin_exception()->klass()->name()->as_C_string(),
              failure_type_name(action->failure_type()),
              end_index,
              known_exception_type->name()->as_C_string());
        }

        action->set_recovery_type(_error_transformation);
        action->set_target_exception_klass(known_exception_type); // XXX this will create a JNI global handle.
        return;
      }

      if (thread->has_pending_exception()) {
        tty->print_cr("fast_error_transformation: querying handler results in an exception.");
        java_lang_Throwable::print_stack_trace(thread->pending_exception(), tty);
        thread->clear_pending_exception();
        return;
      }
    }
  }

  assert(end_index>=0, "sanity check");
  assert(methods->length()>0, "sanity check");

  current_mh = methods->at(0);

  if (!current_mh->is_native()) {
    current_bci = bcis->at(0);

    Bytecodes::Code java_code = current_mh->java_code_at(current_bci);
    if (Bytecodes::is_invoke(java_code)) {
      Bytecode_invoke bi(current_mh, current_bci);
      current_mh = bi.static_target(thread);

      {
        const int ce_length = current_mh->checked_exceptions_length();

        if (ce_length > 0) {
          if (has_known_exception_handler(0, end_index, methods, bcis,
                current_mh, known_exception_type, handler_method, handler_bci, handler_index, thread)) {
            // TODO
            assert(!thread->has_pending_exception(), "sanity check");
            assert(known_exception_type.not_null(), "sanity check");
            assert(handler_bci != -1, "sanity check");
            assert(handler_index != -1, "sanity check");

            if ((TraceRuntimeRecovery & TRACE_TRANSFORMING) != 0) {
              ResourceMark rm(thread);
              tty->print_cr("fast_error_transformation: (target_exception_klass=%s)",
                  known_exception_type->name()->as_C_string());
            }

            if ((TraceRuntimeRecovery & TRACE_PRINT_ACTION) != 0) {
              ResourceMark rm(thread);
              tty->print_cr("(%s) (%s) (error transformation) (end_index=%d, target_exception_klass=%s, top is invoke)",
                  action->origin_exception()->klass()->name()->as_C_string(),
                  failure_type_name(action->failure_type()),
                  end_index,
                  known_exception_type->name()->as_C_string());
            }

            action->set_recovery_type(_error_transformation);
            action->set_target_exception_klass(known_exception_type); // XXX this will create a JNI global handle.
            return;
          }

          if (thread->has_pending_exception()) {
            tty->print_cr("fast_error_transformation: querying handler results in an exception.");
            java_lang_Throwable::print_stack_trace(thread->pending_exception(), tty);
            thread->clear_pending_exception();
            return;
          }
        }
      } // end of has exceptions
    } // end of is_invoke
  }

  for (int index = 0; index <= end_index; index++) {
    current_mh = methods->at(index);
    assert(current_mh.not_null(), "sanity check");
    current_bci = bcis->at(index);

    const int ce_length = current_mh->checked_exceptions_length();

    if (ce_length > 0) {
      if (has_known_exception_handler(index+1, end_index, methods, bcis,
            current_mh, known_exception_type, handler_method, handler_bci, handler_index, thread)) {
        // TODO
        assert(!thread->has_pending_exception(), "sanity check");
        assert(known_exception_type.not_null(), "sanity check");
        assert(handler_bci != -1, "sanity check");
        assert(handler_index != -1, "sanity check");

        if ((TraceRuntimeRecovery & TRACE_TRANSFORMING) != 0) {
          ResourceMark rm(thread);
          tty->print_cr("fast_error_transformation: (index=%d, target_exception_klass=%s)",
              index,
              known_exception_type->name()->as_C_string());
        }

        if ((TraceRuntimeRecovery & TRACE_PRINT_ACTION) != 0) {
          ResourceMark rm(thread);
          tty->print_cr("(%s) (%s) (error transformation) (end_index=%d, index=%d, target_exception_klass=%s)",
              action->origin_exception()->klass()->name()->as_C_string(),
              failure_type_name(action->failure_type()),
              end_index,
              index,
              known_exception_type->name()->as_C_string());
        }

        action->set_recovery_type(_error_transformation);
        action->set_target_exception_klass(known_exception_type); // XXX this will create a JNI global handle.
        return;
      }

      if (thread->has_pending_exception()) {
        tty->print_cr("fast_error_transformation: querying handler results in an exception.");
        java_lang_Throwable::print_stack_trace(thread->pending_exception(), tty);
        thread->clear_pending_exception();
        return;
      }
    }

    assert(known_exception_type.is_null(), "sanity check");
    assert(handler_bci == -1, "sanity check");
    assert(handler_index == -1, "sanity check");
  }
}

void RecoveryOracle::fast_early_return(JavaThread* thread, GrowableArray<methodHandle>* methods, GrowableArray<int>* bcis, RecoveryAction* action) {
  // TODO clear all handles
  // save result as a global JNI handle
  HandleMark hm(thread);

  const int end_index = action->recovery_context_offset();

  if (ForceEarlyReturnAt) {
    int index = ForceEarlyReturnAtIndex;
    if (index >= 0 && index <= end_index) {
      methodHandle current_mh = methods->at(index);
      int current_bci = bcis->at(index);

//      if (current_mh->is_native()) {
//
//        action->set_recovery_type(_early_return);
//        action->set_early_return_offset(index);
//
//        if ((TraceRuntimeRecovery & TRACE_EARLYRET) != 0) {
//          ResourceMark rm(thread);
//
//          tty->print_cr("fast_early_return: force early return into native (index=%d, %s)",
//              index,
//              current_mh->name_and_sig_as_C_string()
//              );
//        }
//
//        return;
//      }

      if (!current_mh->is_native()) {
        Bytecodes::Code java_code = current_mh->java_code_at(current_bci);

        if (Bytecodes::is_invoke(java_code)) {
          Bytecode_invoke bi(current_mh, current_bci);

          action->set_recovery_type(_early_return);
          action->set_early_return_offset(index);
          action->set_early_return_type(bi.result_type());
          action->set_early_return_size_of_parameters(bi.static_target(thread)->size_of_parameters());

          if ((TraceRuntimeRecovery & TRACE_EARLYRET) != 0) {
            ResourceMark rm(thread);

            tty->print_cr("fast_early_return: force early return (index=%d, rettype=%s, size_of_p=%d, %s@%d->%s",
                index,
                type2name(bi.result_type()),
                action->early_return_size_of_parameters(),
                current_mh->name_and_sig_as_C_string(),
                current_bci,
                bi.static_target(thread)->name_and_sig_as_C_string()
                );
          }

          return;
        }
      } // end of valid method
    } // end of valid ForceEarlyReturnAtIndex
    if ((TraceRuntimeRecovery & TRACE_EARLYRET) != 0) {
      tty->print_cr("We fail to force early return at %d", index);
    }
  } // end of ForceEarlyReturnAt

  if ((TraceRuntimeRecovery & TRACE_EARLYRET) != 0) {
    tty->print_cr("fast_early_return: end_index=%d, OnlyEarlyReturnVoid=%d", end_index, OnlyEarlyReturnVoid);
  }

  if (action->has_top_method()) {
    methodHandle current_mh = action->top_method();

    if (OnlyEarlyReturnVoid) {
      if (current_mh->result_type() == T_VOID) {
        action->set_recovery_type(_early_return);
        action->set_early_return_offset(0);
        action->set_early_return_type(current_mh->result_type());
        action->set_early_return_size_of_parameters(current_mh->size_of_parameters());
        if ((TraceRuntimeRecovery & TRACE_PRINT_ACTION) != 0) {
          ResourceMark rm(thread);
          tty->print_cr("(%s) (%s) (early return) (end_index=%d, index=0, rettype=%s, top method)",
              action->origin_exception()->klass()->name()->as_C_string(),
              failure_type_name(action->failure_type()),
              end_index,
              type2name(current_mh->result_type()));
        }
        return;
      }
    } else {
      action->set_recovery_type(_early_return);
      action->set_early_return_offset(0);
      action->set_early_return_type(current_mh->result_type());
      action->set_early_return_size_of_parameters(current_mh->size_of_parameters());

      if ((TraceRuntimeRecovery & TRACE_PRINT_ACTION) != 0) {
        ResourceMark rm(thread);
        tty->print_cr("(%s) (%s) (early return) (end_index=%d, index=0, rettype=%s, top method)",
            action->origin_exception()->klass()->name()->as_C_string(),
            failure_type_name(action->failure_type()),
            end_index,
            type2name(current_mh->result_type()));
      }

      return;
    }
  }

  for (int index = 0; index <= end_index; index++) {
    methodHandle current_mh = methods->at(index);
    assert(current_mh.not_null(), "sanity check");
    int current_bci = bcis->at(index);

    // Check at explicit call

    if ((TraceRuntimeRecovery & TRACE_EARLYRET) != 0) {
      ResourceMark rm(thread);

      tty->print_cr("fast_early_return: checking index=%d, %s@%d, bytecode is %s",
          index,
          current_mh->name_and_sig_as_C_string(),
          current_bci,
          current_mh->is_native() ? "native" : Bytecodes::name(current_mh->java_code_at(current_bci))
          );
    }

//    if (current_mh->is_native()) {
//      action->set_recovery_type(_early_return);
//      action->set_early_return_offset(index);
//
//      if ((TraceRuntimeRecovery & TRACE_EARLYRET) != 0) {
//        ResourceMark rm(thread);
//
//        tty->print_cr("fast_early_return: force early return into native (index=%d, %s)",
//            index,
//            current_mh->name_and_sig_as_C_string()
//            );
//      }
//
//      if ((TraceRuntimeRecovery & TRACE_PRINT_ACTION) != 0) {
//        ResourceMark rm(thread);
//        tty->print_cr("(%s) early_return: (end_index=%d, index=%d, native)",
//            action->origin_exception()->klass()->name()->as_C_string(),
//            end_index,
//            index);
//      }
//
//      return;
//    }




    if (!current_mh->is_native()) {
      Bytecodes::Code java_code = current_mh->java_code_at(current_bci);

      if (Bytecodes::is_invoke(java_code)) {
        Bytecode_invoke bi(current_mh, current_bci);

        if (OnlyEarlyReturnVoid && bi.result_type() != T_VOID) {
          continue;
        }
        action->set_recovery_type(_early_return);
        action->set_early_return_offset(index);
        action->set_early_return_type(bi.result_type());
        action->set_early_return_size_of_parameters(bi.static_target(thread)->size_of_parameters());

        if ((TraceRuntimeRecovery & TRACE_EARLYRET) != 0) {
          ResourceMark rm(thread);

          tty->print_cr("fast_early_return: index=%d, rettype=%s, size_of_p=%d, %s@%d->%s",
              index,
              type2name(bi.result_type()),
              action->early_return_size_of_parameters(),
              current_mh->name_and_sig_as_C_string(),
              current_bci,
              bi.static_target(thread)->name_and_sig_as_C_string()
              );
        }

        if ((TraceRuntimeRecovery & TRACE_PRINT_ACTION) != 0) {
          ResourceMark rm(thread);
          tty->print_cr("(%s) (%s) (early return) (end_index=%d, index=%d, rettype=%s)",
              action->origin_exception()->klass()->name()->as_C_string(),
              failure_type_name(action->failure_type()),
              end_index,
              index,
              type2name(bi.result_type()));
        }

        return;
      }
    }
  }
}

void RecoveryOracle::determine_recovery_action(JavaThread* thread, GrowableArray<methodHandle>* methods, GrowableArray<int>* bcis, RecoveryAction* action) {
  const int recovery_context_offset = action->recovery_context_offset();
  assert(recovery_context_offset >= 0, "sanity check");

  if (UseJPF) {
    run_jpf_with_recovery_action(thread, methods, bcis, action);
    return; // JPF will try all other strategy
  }

  if (UseErrorTransformation) {
    fast_error_transformation(thread, methods, bcis, action);

    if (can_recover(action)) {
      return;
    }
  }

  if (UseEarlyReturn) {
    fast_early_return(thread, methods, bcis, action);

    if (can_recover(action)) {
      return;
    }
  }

  return;
}



void RecoveryOracle::fast_exception_handler_bci_and_caught_klass_use_induced(
    methodHandle mh, int throw_bci,
    KlassHandle &caught_klass, 
    int &handler_bci,
    TRAPS) {
  // exception table holds quadruple entries of the form (beg_bci, end_bci, handler_bci, klass_index)
  // access exception table
  ExceptionTable table(mh());
  int length = table.length();
  // iterate through all entries sequentially
  constantPoolHandle pool(THREAD, mh->constants());
  for (int i = 0; i < length; i ++) {
    //reacquire the table in case a GC happened
    ExceptionTable table(mh());
    int beg_bci = table.start_pc(i);
    int end_bci = table.end_pc(i);
    assert(beg_bci <= end_bci, "inconsistent exception table");
    if (beg_bci <= throw_bci && throw_bci < end_bci) {
      // exception handler bci range covers throw_bci => investigate further
      handler_bci = table.handler_pc(i);
      int klass_index = table.catch_type_index(i);
      if (klass_index == 0) {
        // Do nothing if it is a finally
      } else {
        ResourceMark rm(THREAD);
        // Query induced
        stringStream key;

        key.print("%s-induced:%s:%d:%d:%d",
            RedisKeyPrefix,
            mh->name_and_sig_as_C_string(),
            beg_bci,
            end_bci,
            handler_bci
            );

        if(!redis_contains_key_precise(key.as_string(), THREAD)){
          continue;
        }

        // we know the exception class => get the constraint class
        // this may require loading of the constraint class; if verification
        // fails or some other exception occurs, return handler_bci
        Klass* k = pool->klass_at(klass_index, CHECK);
        KlassHandle klass = KlassHandle(THREAD, k);
        assert(klass.not_null(), "klass not loaded");
        caught_klass = klass;
        if ((TraceRuntimeRecovery & TRACE_USE_INDUCED) != 0) {
          ResourceMark rm(THREAD);
          tty->print_cr("Induced: %s:%s",
              key.as_string(),
              caught_klass->name()->as_C_string()
              );
        }

        return;
      }
    }
  }

  // Nullify these value
  handler_bci = -1;
  caught_klass = KlassHandle(THREAD, (Klass*)NULL);
  return;
}


bool has_string_void_init(KlassHandle klass) {
  if(!klass->oop_is_instance()){
    ShouldNotReachHere();
  }

  return InstanceKlass::cast(klass())->find_method(
      vmSymbols::object_initializer_name(),
      vmSymbols::string_void_signature()) != NULL;
}

// caught_klass may be null,
// use handler_bci == -1 to determine whether we found a handler
void RecoveryOracle::fast_exception_handler_bci_and_caught_klass_for(
    methodHandle mh,
    KlassHandle ex_klass,
    int throw_bci,
    KlassHandle &caught_klass,
    int &handler_bci,
    bool ignore_no_string_void,
    TRAPS) {
  assert(handler_bci == -1, "sanity check");
  assert(caught_klass.is_null(), "sanity check");
  // exception table holds quadruple entries of the form (beg_bci, end_bci, handler_bci, klass_index)
  // access exception table
  ExceptionTable table(mh());
  int length = table.length();
  // iterate through all entries sequentially
  constantPoolHandle pool(THREAD, mh->constants());
  for (int i = 0; i < length; i ++) {
    //reacquire the table in case a GC happened
    ExceptionTable table(mh());
    int beg_bci = table.start_pc(i);
    int end_bci = table.end_pc(i);
    assert(beg_bci <= end_bci, "inconsistent exception table");
    if (beg_bci <= throw_bci && throw_bci < end_bci) {
      // exception handler bci range covers throw_bci => investigate further
      handler_bci = table.handler_pc(i);
      int klass_index = table.catch_type_index(i);
      if (klass_index == 0) {
        if (IgnoreFinallyBlock) { /* This is a finally block */
          continue;
        }
        assert(caught_klass.is_null(), "sanity check");
        return;
      } else if (ex_klass.is_null()) {
        // Caller passes in a NullKlassHandler to simulate finding of a Throwable
        Klass* k = pool->klass_at(klass_index, CHECK);
        KlassHandle klass = KlassHandle(THREAD, k);
        assert(klass.not_null(), "klass not loaded");

        // TODO We should use a parameter to control this
        if (ignore_no_string_void && !has_string_void_init(klass)){
            if ((TraceRuntimeRecovery & TRACE_IGNORE) != 0) {
                ResourceMark rm(THREAD);
                tty->print_cr("Ignore Klass: %s", klass->name()->as_C_string());
            }
            continue;
        }

        caught_klass = klass;
        assert(caught_klass.not_null(), "sanity check");
        return;
      } else {
        // we know the exception class => get the constraint class
        // this may require loading of the constraint class; if verification
        // fails or some other exception occurs, return handler_bci
        Klass* k = pool->klass_at(klass_index, CHECK);
        KlassHandle klass = KlassHandle(THREAD, k);

        // TODO We should a parameter to control this
        if(ignore_no_string_void && !has_string_void_init(klass)){
            if((TraceRuntimeRecovery & TRACE_IGNORE) != 0){
                ResourceMark rm(THREAD);
                tty->print_cr("Ignore Klass: %s", klass->name()->as_C_string());
            }
            continue;
        }

        assert(klass.not_null(), "klass not loaded");
        if (ex_klass->is_subtype_of(klass())) {
          caught_klass = klass;
          assert(caught_klass.not_null(), "sanity check");
          return;
        }
      }
    }
  }

  // Nullify these value
  handler_bci = -1;
  caught_klass = KlassHandle(THREAD, (Klass*)NULL);
  assert(caught_klass.is_null(), "sanity check");
  return;
}


bool RecoveryOracle::error_transformation(JavaThread* thread,
    Handle original_exception,
    Handle &transformed_exception){
  if(!EnableRecovery){
    return false;
  }

  TRAPS = thread;

  if (original_exception.is_null()){
    return false;
  }

  // TODO Deprecated RuntimeExceptionOnly option
  if (/*RuntimeExeptionOnly && */
      !original_exception->is_a(SystemDictionary::RuntimeException_klass())){
    return false;
  }

  if ((TraceRuntimeRecovery & TRACE_CHECKING) != 0) {
    ResourceMark rm(THREAD);
    tty->print_cr("Try to recovery an exception %s", original_exception->klass()->name()->as_C_string());
  }


  // TODO, now we recover exceptions that 
  // 1) no handler
  // 2) escape a handler
  bool do_transformation = false;
  bool no_handler  = false;


  ResourceMark rm(thread);

  GrowableArray<methodHandle>* methods = new GrowableArray<methodHandle>(50);
  GrowableArray<int>* bcis = new GrowableArray<int>(50);

  if ((TraceRuntimeRecovery & TRACE_PRINT_STACK) != 0) {
    tty->print_cr("Begin: Print stack trace of the original exception.");
    java_lang_Throwable::print_stack_trace(original_exception(), tty);
    tty->print_cr("End: Print stack trace of the original exception.");
  }

  fill_stack(thread, methods, bcis);

  {
    HandleMark hm(thread);

    bool should_repeat;
    Method* current_method = NULL;
    methodHandle current_mh = methodHandle(thread, current_method);
    Handle exception_handle = original_exception;
    int current_throw_bci = -1;
    int current_handler_bci = -1;
    int current_other_throw_bci = -1;
    int current_other_handler_bci = -1;
    bool escaped = false;
    KlassHandle caught_klass;
    KlassHandle escaped_caught_klass;
    KlassHandle null_handle;


    int length = methods->length();
    for (int i=0; i<length; i++) {
      current_mh = methods->at(i);
      current_throw_bci = bcis->at(i);

      assert(current_mh.not_null(), "sanity check!");

      if((TraceRuntimeRecovery & TRACE_CHECK_ESCAPE) != 0){
        ResourceMark rm(thread);
        tty->print_cr("Check Escape: %d %s@%d", i, current_mh->name_and_sig_as_C_string(), current_throw_bci);
      }

      do {
        should_repeat = false;
        assert(exception_handle.not_null(), "sanity check!");
        KlassHandle eh_klass(thread, exception_handle->klass());
        // TODO save current_bci_other for double check
        current_other_throw_bci = current_throw_bci;

        assert(eh_klass.not_null(), "sanity check!");
        fast_exception_handler_bci_and_caught_klass_for(
            current_mh, eh_klass, current_throw_bci,
            caught_klass, current_handler_bci, false,
            THREAD);
        if (HAS_PENDING_EXCEPTION) {
          exception_handle = Handle(thread, PENDING_EXCEPTION);
          CLEAR_PENDING_EXCEPTION;
          should_repeat = true;
          current_throw_bci = current_handler_bci;
        }

        // Comment this check block if you want to ignore finally block
        if (current_handler_bci != -1){
          if (caught_klass.is_null()){ // a finally block
            current_handler_bci = -1;
          }
        }

        if (current_handler_bci == -1 // not a real catch block (may be a finally block)
            && !escaped){
          // TODO double check
          // if the eh_klass is null,
          // it will return the handler_bci immediately
          assert(null_handle.is_null(), "sanity check!");
          fast_exception_handler_bci_and_caught_klass_for(
              current_mh, null_handle, current_other_throw_bci,
              escaped_caught_klass, current_other_handler_bci,
              false, THREAD);
          // Should not have pending exception
          if (HAS_PENDING_EXCEPTION) {
            tty->print_cr("WARNING: An exception occurred during error transformation preparation.");
            CLEAR_PENDING_EXCEPTION;
          }

          // there is another handler for another kind of exception
          if(current_other_handler_bci != -1){
            //if(escaped_caught_klass.not_null()) { // not a finally block
            escaped = true;
            //}
          }
        }
      } while (should_repeat && (current_handler_bci != -1));

      if (current_handler_bci >= 0 ){
        break;
      }
    }

    if ((TraceRuntimeRecovery & TRACE_CHECK_ESCAPE) != 0) {
      tty->print_cr("Finish Check Escape %d", escaped);
    }

    //  if(!escaped && 
    //          original_exception->klass()->name() == (SystemDictionary::NullPointerException_klass())){
    //    escaped = true;
    //  }

    if (current_handler_bci < 0) {
      if ((TraceRuntimeRecovery & TRACE_CHECKING) != 0) {
        ResourceMark rm(THREAD);
        tty->print_cr("Try to transform an exception %s that has no handler in the stack.", original_exception->klass()->name()->as_C_string());
      }
      do_transformation = true;
      no_handler = true;
    }else if(TransformEscaped
        && escaped){
      // a runtime exception that is caught by a catch block wrt Exception or Throwable
      if(/*caught_klass.not_null() && */
          !caught_klass->is_subtype_of(SystemDictionary::RuntimeException_klass())){
        if((TraceRuntimeRecovery & TRACE_CHECKING) != 0) {
          ResourceMark rm(THREAD);
          tty->print_cr("Try to transform an exception %s that escapes an error handler in the stack.", 
              original_exception->klass()->name()->as_C_string());
        }
        do_transformation = true;
      }else {
        if((TraceRuntimeRecovery & TRACE_CHECKING) != 0) {
          ResourceMark rm(THREAD);
          tty->print_cr("Give up to transform an exception %s that has an handler [%s] in the stack.", 
              original_exception->klass()->name()->as_C_string(),
              caught_klass.is_null()?"any":caught_klass->name()->as_C_string());
        }
        // avoid duplicated checking
        //thread->set_checked_recovering_exception(original_exception());
      }
    } else {
      if ((TraceRuntimeRecovery & TRACE_CHECKING) != 0) {
        ResourceMark rm(THREAD);
        tty->print_cr("Cannot transform an exception %s.",
            original_exception->klass()->name()->as_C_string());
        tty->print_cr("Caught klass: %s, Handler Method: %s, Handler bci: %d, Escaped: %d",
            caught_klass.is_null()?"any":caught_klass->name()->as_C_string(),
            current_mh->name_and_sig_as_C_string(),
            current_handler_bci,
            escaped
            );
      }

      // TODO

      if (caught_klass() == SystemDictionary::Throwable_klass()
          || caught_klass() == SystemDictionary::Exception_klass()) {
        no_handler = true;
      }
      // avoid duplicated checking
      //thread->set_checked_recovering_exception(original_exception());
    }

  } // end of the big lookup

  if (do_transformation) {
    KlassHandle known_exception_type;
    methodHandle handler_method;
    int bci = -1;
    int index = -1;
    RecoveryOracle::query_known_exception_handler(
          methods,
          bcis,
          original_exception,
          known_exception_type,
          handler_method,
          bci,
          index,
          THREAD
        );

    if((TraceRuntimeRecovery & TRACE_CHECKING) != 0) {
      ResourceMark rm(THREAD);
      tty->print_cr("Now we begin to transform an exception %s.",
          original_exception->klass()->name()->as_C_string()
          );
    }

    if (handler_method.not_null()) {
      ResourceMark rm(THREAD);
      stringStream ss;
      ss.print("Exception transformation: %s -> %s.",
          original_exception->klass()->name()->as_C_string(),
          known_exception_type->name()->as_C_string()
          );


      assert(has_string_void_init(known_exception_type), "sanity check!");

      if((TraceRuntimeRecovery & TRACE_TRANSFORMING) != 0) {
        ResourceMark rm(THREAD);
        tty->print_cr("Transforming an exception %s to %s w.r.t. %s@%d %s.",
          original_exception->klass()->name()->as_C_string(),
          known_exception_type->name()->as_C_string(),
          handler_method->name_and_sig_as_C_string(),
          bci,
          get_recovery_mode()
          );
      }

      Handle new_exception = Exceptions::new_exception(thread,
          known_exception_type->name(),
          ss.as_string(),
          original_exception,
          Handle(THREAD, known_exception_type->class_loader()),
          Handle(THREAD, known_exception_type->protection_domain()));


      transformed_exception = new_exception;

      run_jpf_with_exception(thread, transformed_exception, index);

    } else {
      if ((TraceRuntimeRecovery & TRACE_CHECKING) != 0) {
        ResourceMark rm(THREAD);
        tty->print_cr("We failed to transform an exception %s as there is no handler.",
            original_exception->klass()->name()->as_C_string()
            );
      }
      //thread->set_checked_recovering_exception(original_exception());
      // TODO there is a handler, but we cannot recover it
      no_handler = true;
    }
  }

  if (no_handler) {
    // TODO
  }

  //
  return no_handler;
}

// return false when reply is nil, empty list, empty string
bool RecoveryOracle::redis_contains_key_common(const char* keys_command, TRAPS){

  redisReply* reply = (redisReply*)redisCommand(
      _context,
      keys_command);

  if(reply == NULL){
    freeReplyObject(reply);
    //tty->print_cr("ERROR: redis reply is null.");
    return false;
  }

  if(reply->type == REDIS_REPLY_ERROR){
    freeReplyObject(reply);
    //tty->print_cr("ERROR: redis error message %s.", reply->str);
    return false;
  }

  if(reply->type ==  REDIS_REPLY_NIL){
    freeReplyObject(reply);
    //tty->print_cr("INFO: redis reply is null %s.");
    return false;
  }

  if(reply->type ==  REDIS_REPLY_INTEGER){
    freeReplyObject(reply);
    //tty->print_cr("INFO: redis reply is integer %d.", reply->integer);
    return reply->integer != 0;
  }

  if(reply->type == REDIS_REPLY_ARRAY){
    freeReplyObject(reply);
    //tty->print_cr("INFO: Redis reply is array with %d elements.", reply->elements);
    return reply->elements > 0;
  }

  freeReplyObject(reply);
  //tty->print_cr("ERROR: Redis unknown reply. %d", reply->type);
  return false;
}

bool RecoveryOracle::redis_contains_key_prefix(const char* prefix, TRAPS){
  ResourceMark rm(THREAD);

  stringStream ss;
  ss.print("KEYS %s*", prefix);

  return redis_contains_key_common(ss.as_string(), THREAD);
}

bool RecoveryOracle::redis_contains_key_precise(const char* precise, TRAPS){
  ResourceMark rm(THREAD);

  stringStream ss;
  ss.print("EXISTS %s", precise);

  return redis_contains_key_common(ss.as_string(), THREAD);
}

bool RecoveryOracle::has_known_exception_handler_use_redis(
       int begin_index,
       int end_index,
       GrowableArray<methodHandle>* methods,
       GrowableArray<int>* bcis,
       methodHandle top_method,
       KlassHandle &known_exception_type,
       methodHandle &handler_method,
       int &handler_bci,
       int &handler_index,
       TRAPS
    ){

  JavaThread* thread = (JavaThread*)THREAD;

  int length = top_method->checked_exceptions_length();

  if (length <= 0) {
    return false;
  }

  CheckedExceptionElement* table = top_method->checked_exceptions_start();
  assert(table != NULL, "sanity check");

  methodHandle current_mh (thread, NULL);
  int current_bci = -1;

  {
    // begin build key
    ResourceMark rm(THREAD);
    stringStream cs;
    assert(methods != NULL, "sanity check");
    for (int index = begin_index; index <= end_index; index++) {
      current_mh = methods->at(index);
      assert(current_mh.not_null(), "sanity check!");
      current_bci = bcis->at(index);

      cs.print("%d:%s:",
          current_bci,
          current_mh->name_and_sig_as_C_string());

      for (int i=0; i<length; i++) {
        KlassHandle klass (THREAD,
            top_method->constants()->klass_at(table[i].class_cp_index, THREAD));
        assert(klass.not_null(), "sanity check");

        ResourceMark rm(THREAD);
        stringStream key;

        //      key.print("%s:%s:%s:%s",
        key.print("%s-fuzzing:%s:%s",
            RedisKeyPrefix,
            klass->name()->as_C_string(),
            //          top_method->name_and_sig_as_C_string(),
            cs.as_string());

        if (redis_contains_key_precise(key.as_string(), THREAD)) {
          if ((TraceRuntimeRecovery & TRACE_USE_REDIS) != 0) {
            ResourceMark rm(THREAD);
            tty->print_cr("Find the call string in redis. %s", key.as_string());
          }

          known_exception_type = klass;
          handler_method = current_mh;
          handler_bci = current_bci;
          handler_index = index;
          return true;
        } else {
          if ((TraceRuntimeRecovery & TRACE_USE_REDIS) != 0) {
            ResourceMark rm(THREAD);
            tty->print_cr("Cannot find the call string in redis. %s", key.as_string());
          }
        }
      }

      if (UseInduced) {
        fast_exception_handler_bci_and_caught_klass_use_induced(current_mh, current_bci,
            known_exception_type, handler_bci, THREAD);
        if (HAS_PENDING_EXCEPTION) {
          ShouldNotReachHere();
          CLEAR_PENDING_EXCEPTION;
        }
        if (known_exception_type.not_null()) {
          handler_method = current_mh;
          handler_index = index;
          return true;
        }
      }
    }
  }

  return false;
}


bool RecoveryOracle::has_known_exception_handler_use_stack(
       int begin_index,
       int end_index,
       GrowableArray<methodHandle>* methods,
       GrowableArray<int>* bcis,
       methodHandle top_method,
       KlassHandle &known_exception_type,
       methodHandle &handler_method,
       int &handler_bci,
       int &handler_index,
       TRAPS
    ){
  ResourceMark rm(THREAD);

  JavaThread* thread = (JavaThread*)THREAD;

  int ce_length = top_method->checked_exceptions_length();

  if (ce_length <= 0) {
    return false;
  }

  CheckedExceptionElement* table = top_method->checked_exceptions_start();

  methodHandle current_mh = methodHandle(thread, NULL);
  int current_bci = -1;

  if ((TraceRuntimeRecovery & TRACE_USE_STACK) != 0) {
    tty->print_cr("Top Method: %s", top_method->name_and_sig_as_C_string());
  }

  // TODO
  assert(thread->runtime_recovery_state()->has_last_checked_exception(), "sanity check");
  KlassHandle cause_ex_klass (thread, thread->runtime_recovery_state()->last_checked_exception()->klass());

  for (int index=begin_index; index <= end_index; index++) {
    current_mh = methods->at(index);
    assert(current_mh.not_null(), "sanity check");

    current_bci = bcis->at(index);

    if ((TraceRuntimeRecovery & TRACE_USE_STACK) != 0) {
      tty->print_cr("Use Stack: %d %s@%d", index, current_mh->name_and_sig_as_C_string(), current_bci);
    }

    for (int i=0; i<ce_length; i++) {
      current_bci = bcis->at(index); // update current_bci, as we may change it in the following

      KlassHandle klass (THREAD, top_method->constants()->klass_at(table[i].class_cp_index, THREAD));
      assert(klass.not_null(), "sanity check");

      // TODO we need to control this
      if (!has_string_void_init(klass)) {
        continue;
      }

      if (RecoverTrivial && is_trivial_handler(klass)) {
        continue;
      }

      // TODO IllegalArgumentException is a runtime exception but must be handled sometimes.
      if (cause_ex_klass == klass) {
        continue;
      }

      current_bci = Method::fast_exception_handler_bci_for(current_mh, klass, current_bci, THREAD);

      if (HAS_PENDING_EXCEPTION) {
        // TODO
        ShouldNotReachHere();
        CLEAR_PENDING_EXCEPTION;
      }

      if (current_bci != -1 ){
        known_exception_type = klass;
        handler_bci = current_bci;
        handler_method = current_mh;
        handler_index = index;

        if ((TraceRuntimeRecovery & TRACE_USE_STACK) != 0) {
          ResourceMark rm(thread);
          tty->print_cr("Top Method: %s", top_method->name_and_sig_as_C_string());
          tty->print_cr("Handler: %s@%d#%s", handler_method->name_and_sig_as_C_string(), handler_bci, known_exception_type->name()->as_C_string());
        }

        return true;
      }
    }
  }

  return false;
}

bool RecoveryOracle::has_known_exception_handler_force_throwable(
       int begin_index,
       int end_index,
       GrowableArray<methodHandle>* methods,
       GrowableArray<int>* bcis,
       methodHandle top_method,
       KlassHandle &known_exception_type,
       methodHandle &handler_method,
       int &handler_bci,
       int &handler_index,
       TRAPS
    ){
  ResourceMark rm(THREAD);

  JavaThread* thread = (JavaThread*)THREAD;

  methodHandle current_mh;
  int current_bci = -1;

  assert(top_method.not_null(), "sanity check");
  if((TraceRuntimeRecovery & TRACE_USE_STACK) != 0) {
    tty->print_cr("Top Method: %s", top_method->name_and_sig_as_C_string());
  }

  int methods_length = methods->length();
  KlassHandle null_handle(THREAD, (Klass*)NULL);
  for (int index=begin_index; index <= end_index; index++) {
    current_mh = methods->at(index);
    assert(current_mh.not_null(), "sanity check");
    current_bci = bcis->at(index);

    if ((TraceRuntimeRecovery & TRACE_USE_STACK) != 0) {
      ResourceMark rm(THREAD);
      tty->print_cr("Force Throwable: %d %s@%d", index, current_mh->name_and_sig_as_C_string(), current_bci);
    }

    KlassHandle caught_klass(THREAD, (Klass*)NULL);
    int current_handler_bci = -1;
    fast_exception_handler_bci_and_caught_klass_for(
        current_mh, null_handle, current_bci,
        caught_klass, current_handler_bci,
        true, THREAD);

    if (HAS_PENDING_EXCEPTION) {
      // TODO
      ShouldNotReachHere();
      CLEAR_PENDING_EXCEPTION;
    }

    if (caught_klass.is_null()){
      continue; // a finally block
    }

    assert(caught_klass.not_null(), "sanity check");

    if (current_handler_bci != -1 ){
      assert(has_string_void_init(caught_klass), "sanity check!");
      known_exception_type = caught_klass;
      handler_bci = current_handler_bci;
      handler_method = current_mh;
      handler_index = index;

      assert(caught_klass.not_null(), "sanity check");
      assert(handler_method.not_null(), "sanity check");
      assert(known_exception_type.not_null(), "sanity check");

      if((TraceRuntimeRecovery & TRACE_USE_STACK) != 0){
        ResourceMark rm(THREAD);
        tty->print_cr("Handler: %s <> %s@%d#%s",
            top_method->name_and_sig_as_C_string(),
            handler_method->name_and_sig_as_C_string(), 
            handler_bci,
            known_exception_type->name()->as_C_string());
      }

      return true;
    }
  }

  return false;
}



void RecoveryOracle::fill_stack(JavaThread* thread, GrowableArray<methodHandle>* methods, GrowableArray<int>* bcis, int max_depth, int max_frame_depth){
  int frame_count = 0;
  int total_count = 0;

  Method* current_method = NULL;
  int current_bci = -1;

  RegisterMap map(thread, false);
  int decode_offset = 0;
  nmethod* nm = NULL;
  bool skip_hidden = !ShowHiddenFrames;

  for (frame fr = thread->last_frame(); max_depth != total_count;) {

    // Compiled java method case.
    if (decode_offset != 0) {
      DebugInfoReadStream stream(nm, decode_offset);
      decode_offset = stream.read_int();
      current_method = (Method*)nm->metadata_at(stream.read_int());
      current_bci = stream.read_bci();
    } else {
      if (fr.is_first_frame()) break;
      address pc = fr.pc();
      //fr.print_on(tty);
      if (fr.is_interpreted_frame()) {
        intptr_t bcx = fr.interpreter_frame_bcx();
        current_method = fr.interpreter_frame_method();
        current_bci =  fr.is_bci(bcx) ? bcx : current_method->bci_from((address)bcx);
        fr = fr.sender(&map);
        frame_count++;
        if (frame_count > max_frame_depth) {
          break;
        }
      } else {
        CodeBlob* cb = fr.cb();
        // HMMM QQQ might be nice to have frame return nm as NULL if cb is non-NULL
        // but non nmethod
        fr = fr.sender(&map);
        frame_count++;
        if (frame_count > max_frame_depth) {
          break;
        }
        if (cb == NULL || !cb->is_nmethod()) {
          continue;
        }
        nm = (nmethod*)cb;
        if (nm->method()->is_native()) {
          current_method = nm->method();
          current_bci = 0;
        } else {
          PcDesc* pd = nm->pc_desc_at(pc);
          decode_offset = pd->scope_decode_offset();
          // if decode_offset is not equal to 0, it will execute the
          // "compiled java method case" at the beginning of the loop.
          continue;
        }
      }
    }

    if (current_method->is_hidden()) {
      if (skip_hidden){
        continue;
      }
    }

//    if (StopStackWalkingAtReflection
//        && current_method->is_native()) {
//      if (current_method->name()->equals("invoke0")) {
//        if (current_method->method_holder()->name()->equals("sun/reflect/NativeMethodAccessorImpl")) {
//          // invoke0 will wrap all caught throwable into an InvocationTargetException
//          break;
//        }
//      }
//    }

    methodHandle current_mh(thread, current_method);

    assert(current_mh.not_null(), "sanity check");

    if((TraceRuntimeRecovery & TRACE_FILL_STACK) != 0){
      ResourceMark rm(thread);
      tty->print_cr("Fill Stack: %d. %s@%d", total_count, current_method->name_and_sig_as_C_string(), current_bci);
    }

    methods->append(current_mh);
    bcis->append(current_bci);

    total_count++;
  }

  if (total_count > 0) {
    HandleMark hm(thread);
    assert(methods->length() > 0, "sanity check");
    methodHandle current_mh = methods->at(0);
    if (!current_mh->is_native()) {
      int current_bci = bcis->at(0);

      Bytecodes::Code java_code = current_mh->java_code_at(current_bci);
      if (Bytecodes::is_invoke(java_code)) {
        Bytecode_invoke bi(current_mh, current_bci);
        current_mh = bi.static_target(thread);

        if((TraceRuntimeRecovery & TRACE_FILL_STACK) != 0){
          ResourceMark rm(thread);
          tty->print_cr("Fill Stack: -1. %s@UNKNOWN", current_mh->name_and_sig_as_C_string());
        }
      } // end of is_invoke
    }
  }
}

// Stack trace:
// [0] method (<-- top method)
// [1] bci, method
// [2] bci, method
void RecoveryOracle::query_known_exception_handler(
        Handle cause_exception,
        KlassHandle &known_exception_type,
        methodHandle &handler_method,
        int &handler_bci,
        int &handler_index,
        TRAPS
      ){
  JavaThread* thread = (JavaThread*)THREAD;

  ResourceMark rm(thread);

  GrowableArray<methodHandle>* methods = new GrowableArray<methodHandle>(20);
  GrowableArray<int>* bcis = new GrowableArray<int>(20);

  fill_stack(thread, methods, bcis);

  query_known_exception_handler(methods, bcis, cause_exception, known_exception_type, handler_method, handler_bci, handler_bci, CHECK);
}
void RecoveryOracle::query_known_exception_handler(
        GrowableArray<methodHandle>* methods,
        GrowableArray<int>* bcis,
        Handle cause_exception,
        KlassHandle &known_exception_type,
        methodHandle &handler_method,
        int &handler_bci,
        int &handler_index,
        TRAPS
    ){
  JavaThread* thread = (JavaThread*) THREAD;

  int length = methods->length();
  assert(bcis->length() == length, "sanity check");
  for (int index = 0; index < length; index++) {
    methodHandle current_mh = methods->at(index);
    assert(current_mh.not_null(), "sanity check");
    int current_bci = bcis->at(index);

    int c_length = current_mh->checked_exceptions_length();

    if ((TraceRuntimeRecovery & TRACE_CHECKING) != 0) {
      ResourceMark rm(THREAD);
      //tty->print_cr("Query Begin: %d/%d", index, length);
      tty->print_cr("Query Begin: %s@%d#%d",
          current_mh->name_and_sig_as_C_string(),
          current_bci,
          c_length);
    }

    //if(length > 0){
      if(has_known_exception_handler(index+1, methods->length(), methods, bcis,
            current_mh, known_exception_type, handler_method, handler_bci, handler_index, THREAD)){
        return;
      }

    if ((TraceRuntimeRecovery & TRACE_CHECKING) != 0) {
      ResourceMark rm(THREAD);
      tty->print_cr("Query End: %d/%d", index, length);
      //tty->print_cr("Query: %s@%d#%d",
      //    current_mh->name_and_sig_as_C_string(),
      //    current_bci,
      //    c_length);
    }
    //}
  }
}

bool RecoveryOracle::has_known_exception_handler(
    int begin_index,
    int end_index,
    GrowableArray<methodHandle>* methods,
    GrowableArray<int>* bcis,
    methodHandle top_method,
    KlassHandle &known_exception_type,
    methodHandle &handler_method,
    int &handler_bci,
    int &handler_index,
    TRAPS) {

  if (UseRedis) {
    return has_known_exception_handler_use_redis(
        begin_index,
        end_index,
        methods, bcis,
        top_method,
        known_exception_type,
        handler_method, handler_bci, handler_index, THREAD);
  }

  if (UseStack) {
    return has_known_exception_handler_use_stack(
        begin_index,
        end_index,
        methods, bcis,
        top_method,
        known_exception_type,
        handler_method, handler_bci, handler_index, THREAD);
  }

  if (UseForceThrowable) {
    return has_known_exception_handler_force_throwable(
        begin_index,
        end_index,
        methods, bcis,
        top_method,
        known_exception_type,
        handler_method, handler_bci, handler_index, THREAD);
  }

  return false;
}


void RecoveryOracle::run_jpf_with_recovery_action(JavaThread* thread, GrowableArray<methodHandle>* methods,
      GrowableArray<int>* bcis, RecoveryAction* action) {
  HandleMark hm(thread);

  // TODO FIXME this should be renamed to max_offset
  int max_depth = action->recovery_context_offset();

  assert(max_depth >= 0, "sanity check");
  assert(methods->length()>0, "sanity check");

  methodHandle top_mh = methods->at(0);

  if (top_mh->is_native()) {
    // currently we do not want run_jpf_in_native
    // Clear last checked exception and make a re-try 
    //

    bool is_reflection_at_top = false;

    if (top_mh->name()->equals("invoke0")) {
      if (top_mh->method_holder()->name()->equals("sun/reflect/NativeMethodAccessorImpl")) {
        is_reflection_at_top = true;

#ifdef ASSERT
        assert(max_depth >= 3, "sanity check");
        assert(methods->length() >= 3, "sanity check");
        methodHandle second_mh = methods->at(1);
        assert(second_mh->name()->equals("invoke"), "sanity check");
        assert(second_mh->method_holder() == top_mh->method_holder(), "sanity check");
        methodHandle third_mh = methods->at(2);
        assert(third_mh->name()->equals("invoke"), "sanity check");
        assert(third_mh->method_holder()->name()->equals("sun/reflect/DelegatingMethodAccessorImpl"), "sanity check");
        methodHandle forth_mh = methods->at(3);
        assert(forth_mh->name()->equals("invoke"), "sanity check");
        assert(forth_mh->method_holder()->name()->equals("java/lang/reflect/Method"), "sanity check");
#endif
      }
    }


    if (!is_reflection_at_top) {
      action->set_recovery_type(_no_recovery);
      thread->runtime_recovery_state()->set_last_checked_exception(NULL);
      return;
    }
  }

  Handle exception = action->origin_exception();

  int final_max_depth = max_depth;

  objArrayOop result_oop = run_jpf_with_exception(thread, exception, final_max_depth);

  if (result_oop == NULL) {
    return;
  }

  objArrayHandle result(thread, result_oop);

  const int length = result->length();

  assert(length == 2, "now we only support two types of actions");

  ResourceMark rm(thread);

  assert(result->obj_at(0) != NULL, "sanity check");
  assert(result->obj_at(1) != NULL, "sanity check");
  char* type_char_array = java_lang_String::as_utf8_string(result->obj_at(0));

  if (strcmp("ErrorTransformation", type_char_array) == 0) {
    KlassHandle target_klass(thread, java_lang_Class::as_Klass(result->obj_at(1)));
    action->set_recovery_type(_error_transformation);
    action->set_target_exception_klass(target_klass); // XXX this will create a JNI global handle.

    if ((TraceRuntimeRecovery & TRACE_TRANSFORMING) != 0) {
      ResourceMark rm(thread);
      tty->print_cr("run_jpf_with_recovery_action: error_transformation, target_exception=%s",
          target_klass->name()->as_C_string());
    }

    if ((TraceRuntimeRecovery & TRACE_PRINT_ACTION) != 0) {
      ResourceMark rm(thread);
      tty->print_cr("(%s) (%s) (error transformation) (target_exception_klass=%s, max_depth=%d, final_max_depth=%d)",
          action->origin_exception()->klass()->name()->as_C_string(),
          failure_type_name(action->failure_type()),
          target_klass->name()->as_C_string(),
          max_depth, final_max_depth);
    }

  } else if (strcmp("EarlyReturn", type_char_array) == 0) {
    jvalue value;
    java_lang_boxing_object::get_value(result->obj_at(1), &value);


    int index = final_max_depth - value.i;

    if ((TraceRuntimeRecovery & TRACE_EARLYRET) != 0) {
      tty->print_cr("run_jpf_with_recovery_action: early return at %d = %d - %d", index, final_max_depth, value.i);
    }

    assert(index >=0 && index <= final_max_depth, "sanity check");

    methodHandle current_mh = methods->at(index);
    int current_bci = bcis->at(index);

    assert(!current_mh->is_native(), "sanity check");

    Bytecodes::Code java_code = current_mh->java_code_at(current_bci);

    assert(Bytecodes::is_invoke(java_code), "sanity check");

    Bytecode_invoke bi(current_mh, current_bci);

    action->set_recovery_type(_early_return);
    action->set_early_return_offset(index);
    action->set_early_return_type(bi.result_type());
    action->set_early_return_size_of_parameters(bi.static_target(thread)->size_of_parameters());

    if ((TraceRuntimeRecovery & TRACE_EARLYRET) != 0) {
      ResourceMark rm(thread);

      tty->print_cr("run_jpf_with_recovery_action: force early return (index=%d, rettype=%s, size_of_p=%d, %s@%d->%s",
          index,
          type2name(bi.result_type()),
          action->early_return_size_of_parameters(),
          current_mh->name_and_sig_as_C_string(),
          current_bci,
          bi.static_target(thread)->name_and_sig_as_C_string()
          );
    }

    if ((TraceRuntimeRecovery & TRACE_PRINT_ACTION) != 0) {
      ResourceMark rm(thread);
      tty->print_cr("(%s) (%s) (early return) (end_index=%d, final_max_depth=%d, index=%d, rettype=%s)",
          action->origin_exception()->klass()->name()->as_C_string(),
          failure_type_name(action->failure_type()),
          max_depth,
          final_max_depth,
          index,
          type2name(bi.result_type()));
    }

  } else {
    ShouldNotReachHere();
  }
}

objArrayOop RecoveryOracle::run_jpf_with_exception(JavaThread* thread, Handle exception, int &max_depth) {
  HandleMark hm(thread);

  TempNewSymbol javaPlusName = SymbolTable::new_symbol("gov/nasa/jpf/JavaPlus", thread);
  if (javaPlusName == NULL) {
    tty->print_cr("Cannot create javaPlusName");
    return NULL;
  }
  instanceKlassHandle javaPlusKlass (thread, SystemDictionary::resolve_or_null(javaPlusName, thread));
  if (thread->has_pending_exception()) {
    Handle ex_h(thread, thread->pending_exception());
    ex_h.print();
    thread->clear_pending_exception();
    return NULL;
  }

  if (javaPlusKlass.is_null()) {
    tty->print_cr("Cannot resolve gov/nasa/jpf/JavaPlus");
    return NULL;
  }

  TempNewSymbol run_name = SymbolTable::new_symbol("runDefault", thread);
  TempNewSymbol run_desc = SymbolTable::new_symbol("([Ljava/lang/Object;)[Ljava/lang/Object;", thread);

  Method* run_method = javaPlusKlass->find_method(run_name, run_desc);

  if (run_method == NULL) {
    tty->print_cr("Cannot find method run ([Ljava/lang/Object;)[Ljava/lang/Object;");
    return NULL;
  }

  methodHandle run_method_h(thread, run_method);

  objArrayOop data = load_stack_data(thread, exception, max_depth);

  if (data == NULL) {
    tty->print_cr("Cannnot laod stack data");
    return NULL;
  }

  objArrayHandle data_h(thread, data);

  {
    assert(!thread->runtime_recovery_state()->is_in_run_jpf(), "sanity check");

    thread->runtime_recovery_state()->set_in_run_jpf();
    JavaValue result(T_ARRAY);
    JavaCallArguments args;
    args.push_oop(data_h);
    JavaCalls::call_static(&result, javaPlusKlass, run_name, run_desc, data_h, thread);


    if (thread->has_pending_exception()) {
      Handle px (thread, thread->pending_exception());
      thread->clear_pending_exception();
      tty->print("Run jpf result in an exception: ");
      java_lang_Throwable::print(px, tty);
      tty->cr();
      java_lang_Throwable::print_stack_trace(px(), tty);
      thread->runtime_recovery_state()->clr_in_run_jpf();
      return NULL;
    }


    thread->runtime_recovery_state()->clr_in_run_jpf();

    // XXX no need to use handle as if there is a exception, we cannot reach here.
    return (objArrayOop)result.get_jobject();
  }
}

objArrayOop RecoveryOracle::load_stack_data(JavaThread* thread, Handle exception, int &max_offset) {
  // vframes are resource allocated
  assert(thread == Thread::current(), "sanity check");
  ResourceMark rm(thread);
  HandleMark hm(thread);

  int stack_depth = 0;

  GrowableArray<Handle>* data = new GrowableArray<Handle>((max_offset + 1) * 3 + 2);
  GrowableArray<int>* bcis = new GrowableArray<int>(max_offset + 1);

#ifndef _LP64
  ShouldNotReachHere();
#endif

  if (thread->has_last_Java_frame()) {
    RegisterMap reg_map(thread);
    frame f = thread->last_frame();
    vframe* vf = vframe::new_vframe(&f, &reg_map, thread);
    frame* last_entry_frame = NULL;
    int extra_frames = 0;

    while (vf != NULL) {

      // TODO we care java frame number
      // depth here is not the same as JPF
      // stack_depth is indeed stack offset from top
      if (stack_depth > max_offset) {
        break;
      }

      if (vf->is_java_frame()) {
        // java frame (interpreted, compiled, ...)
        javaVFrame *jvf = javaVFrame::cast(vf);

        bool skip_this_frame = false;

        if (jvf->method()->name()->equals("invoke")) {
          // Skip two invoke of Reflection
          if (jvf->method()->method_holder()->name()->equals("sun/reflect/NativeMethodAccessorImpl")) {
            skip_this_frame = true;
          } else if (jvf->method()->method_holder()->name()->equals("sun/reflect/DelegatingMethodAccessorImpl")) {
            skip_this_frame = true;
          } else if (jvf->method()->method_holder() == SystemDictionary::reflect_Method_klass()) {
            skip_this_frame = true;
          }
        }

        if (!skip_this_frame) {
          if (!(jvf->method()->is_native())) {

            Handle reflection_method;
            if (jvf->method()->is_initializer()) {
              reflection_method = Handle(thread, Reflection::new_constructor(jvf->method(), thread));
            } else {
              reflection_method = Handle(thread, Reflection::new_method(jvf->method(), UseNewReflection, false, thread));
            }

            bcis->append(jvf->bci());
            data->append(reflection_method);

            {
              StackValueCollection* locals = jvf->locals();
              StackValueCollection* expressions = jvf->expressions();

              int nof_locals = locals->size();
              int nof_expressions = expressions->size();

              if ((TraceRuntimeRecovery & TRACE_LOAD_STACK) != 0) {
                tty->print_cr("load_stack_data: %s %d %d %d %d",
                    jvf->method()->name_and_sig_as_C_string(),
                    nof_locals, nof_expressions,
                    jvf->method()->max_locals(), jvf->method()->max_stack());
                jvf->fr().print_on(tty);
                if (jvf->method()->verifier_max_stack() < nof_expressions) {
                  tty->print_cr("invalid frame ");
                  jvf->fr().print_on(tty);

                }
              }



              int slot_size = nof_locals + nof_expressions;

              typeArrayHandle longSlots(thread, oopFactory::new_longArray(slot_size, thread));
              objArrayHandle objectSlots(thread, oopFactory::new_objectArray(slot_size, thread));

              data->append(longSlots);
              data->append(objectSlots);

              int slot = 0;
              for (; slot<locals->size(); slot++) {
                //tty->print_cr("locals %d %s", slot, type2name(locals->at(slot)->type()));
                if (locals->at(slot)->type() == T_OBJECT) {
                  oop o = locals->obj_at(slot)();

                  if (o != NULL) {
                    objectSlots->obj_at_put(slot, o);
                  }
                } else {
                  longSlots->long_at_put(slot, locals->at(slot)->get_int());
                }
                //tty->print_cr("%d", slot);
              }

              for (int offset= 0; offset<expressions->size(); offset++) {
                //tty->print_cr("expressions %d %s", slot, type2name(expressions->at(slot)->type()));
                if (expressions->at(offset)->type() == T_OBJECT) {
                  oop o = expressions->obj_at(offset)();

                  if (o != NULL) {
                    objectSlots->obj_at_put(slot, o);
                  }
                } else {
                  longSlots->long_at_put(slot, expressions->at(offset)->get_int());
                }
                slot++;
                //tty->print_cr("%d", slot);
              }


              if ((TraceRuntimeRecovery & TRACE_LOAD_STACK) != 0) {
                tty->print_cr("Begin print long and object slots %d", slot_size);
                for (int i=0; i<slot_size; i++) {
                  jlong v = longSlots->long_at(i);
                  oop r = objectSlots->obj_at(i);

                  tty->print("%3d " INTPTR_FORMAT " ", i, v);
                  if (r == NULL) {
                    tty->print_cr("NULL");
                  } else {
                    r->print_address_on(tty);
                    tty->cr();
                  }
                }
                tty->print_cr("finish print long and object slots");
              }
            }

          } else {
            // native frame
            if (stack_depth == 0) {
              // JNI locals for the top frame.
              // java_thread->active_handles()->oops_do(&blk);
            } else {
              if (last_entry_frame != NULL) {
                // JNI locals for the entry frame
                // assert(last_entry_frame->is_entry_frame(), "checking");
                // last_entry_frame->entry_frame_call_wrapper()->handles()->oops_do(&blk);
              }
            }


            bool stop_loading = true;
            if (stack_depth == 0) {
              methodHandle mh (thread, jvf->method());
              if (is_sun_reflect_NativeMethodAccessorImpl(mh)) {
                stop_loading = false;
              }
            }

            if (stop_loading) {
              // TODO caller should check whether the stack is beginning with native method
              // TODO we update the caller depth here
              max_offset = stack_depth - 1; // convert depth to offset
              assert(max_offset >= 0, "sanity check");
              break;
            }
          } // end of native
        } // end of not skip

        // increment only for Java frames
        stack_depth++;
        last_entry_frame = NULL;

      } else {
        // externalVFrame - if it's an entry frame then report any JNI locals
        // as roots when we find the corresponding native javaVFrame
        frame* fr = vf->frame_pointer();
        assert(fr != NULL, "sanity check");
        if (fr->is_entry_frame()) {
          last_entry_frame = fr;
        }
      }
      vf = vf->sender();
    }
  } else {
    ShouldNotReachHere();
  }

  typeArrayHandle pcs (thread, oopFactory::new_intArray(bcis->length(), thread));

  for (int index = 0; index< bcis->length(); index++) {
    int pc = bcis->at(index);
    pcs->int_at_put(index, pc);
  }

  data->append(pcs);
  data->append(exception);

  objArrayHandle dataOop(thread, oopFactory::new_objectArray(data->length(), thread));

  for (int index = 0; index< data->length(); index++) {
    Handle d = data->at(index);
    dataOop->obj_at_put(index, d());
  }

  return dataOop();
}
