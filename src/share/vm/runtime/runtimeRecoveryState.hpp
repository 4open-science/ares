#ifndef SHARE_VM_RUNTIME_RUNTIMERECOVERYSTATE_HPP
#define SHARE_VM_RUNTIME_RUNTIMERECOVERYSTATE_HPP

#include "memory/allocation.hpp"
#include "memory/allocation.inline.hpp"

#include "runtime/thread.hpp"

class RecoveryMark : public StackObj {

private:
  JavaThread* _thread;

public:
  RecoveryMark(JavaThread* thread);
  ~RecoveryMark();
};

//
// A simplified version of jvmtiThreadState
//
class RuntimeRecoveryState: public CHeapObj<mtInternal> {

private:

  JavaThread        *_thread;

  bool          _in_recovery;
  bool          _in_run_jpf;


  bool          _recover_in_deopt;

  int           _earlyret_offset;
  BasicType     _earlyret_type;
  int           _earlyret_size_of_parameters;

  int           _earlyret_state;
  TosState      _earlyret_tos;
  jvalue        _earlyret_value;
  oop           _earlyret_oop;         // Used to return an oop result into Java code from

  oop           _last_checked_exception;

  // XXX Only used by interpreter
  // We choose the tos in interpreterRuntime
  address       _earlyret_dispatch_next;

 public:

  RuntimeRecoveryState(JavaThread* thread);
  ~RuntimeRecoveryState();

  JavaThread* get_thread() { return _thread; }

  void set_recover_in_deopt(void) { _recover_in_deopt = true;  }
  void clr_recover_in_deopt(void) { _recover_in_deopt = false; }
  bool is_recover_in_deopt(void)  { return _recover_in_deopt;  }

  void set_in_recovery(void) { _in_recovery = true;  }
  void clr_in_recovery(void) { _in_recovery = false; }
  bool is_in_recovery(void)     { return _in_recovery;  }

  void set_in_run_jpf(void)  { _in_run_jpf = true;  }
  void clr_in_run_jpf(void)  { _in_run_jpf = false; }
  bool is_in_run_jpf(void)   { return _in_run_jpf;  }

  enum EarlyretState {
    earlyret_inactive = 0,
    earlyret_pending  = 1
  };

  void set_earlyret_offset(int offset) { _earlyret_offset = offset; }
  int  earlyret_offset(void)           { return _earlyret_offset; }
  void decrease_earlyret_offset()      { _earlyret_offset--; }
  bool make_earlyret_now()             { return _earlyret_offset == 0; }

  void set_earlyret_result_type(BasicType type) { _earlyret_type = type; }
  BasicType earlyret_result_type(void)          { return _earlyret_type; }
  void clr_earlyret_result_type(void)           { _earlyret_type = T_ILLEGAL; }

  void set_earlyret_size_of_parameters(int size) { _earlyret_size_of_parameters = size; }
  int  earlyret_size_of_parameters(void)         { return _earlyret_size_of_parameters; }
  void clr_earlyret_size_of_parameters(void)     { _earlyret_size_of_parameters = 0; }

  void set_earlyret_pending(void) { _earlyret_state = earlyret_pending;  }
  void clr_earlyret_pending(void) { _earlyret_state = earlyret_inactive; }
  bool is_earlyret_pending(void)  { return (_earlyret_state == earlyret_pending);  }

  TosState earlyret_tos()                            { return _earlyret_tos; }
  oop  earlyret_oop() const                          { return _earlyret_oop; }
  void set_earlyret_oop (oop x)                      { _earlyret_oop = x;    }
  jvalue earlyret_value()                            { return _earlyret_value; }
  void set_earlyret_value(jvalue val, TosState tos)  { _earlyret_tos = tos;  _earlyret_value = val;  }
  void clr_earlyret_value()                          { _earlyret_tos = ilgl; _earlyret_value.j = 0L; }

  static ByteSize earlyret_state_offset() { return byte_offset_of(RuntimeRecoveryState, _earlyret_state); }
  static ByteSize earlyret_tos_offset()   { return byte_offset_of(RuntimeRecoveryState, _earlyret_tos); }
  static ByteSize earlyret_oop_offset()   { return byte_offset_of(RuntimeRecoveryState, _earlyret_oop); }
  static ByteSize earlyret_value_offset() { return byte_offset_of(RuntimeRecoveryState, _earlyret_value); }


  void set_earlyret_dispatch_next(address next) { _earlyret_dispatch_next = next; }
  static ByteSize earlyret_dispatch_next_offset() { return byte_offset_of(RuntimeRecoveryState, _earlyret_dispatch_next); }

  oop  last_checked_exception(void)              { return _last_checked_exception; }
  bool has_last_checked_exception(void)          { return _last_checked_exception != NULL; }
  void set_last_checked_exception(oop exception) { _last_checked_exception = exception; }

  void reset_runtime_recovery_state();

  void oops_do(OopClosure* f); // GC support

};

#endif  // SHARE_VM_RUNTIME_RUNTIMERECOVERYSTATE_HPP
