#include "runtime/runtimeRecoveryState.hpp"

RecoveryMark::RecoveryMark(JavaThread* thread) : _thread(thread) {
  assert(!_thread->runtime_recovery_state()->is_in_recovery(), "sanity check");
  _thread->runtime_recovery_state()->set_in_recovery();
}

RecoveryMark::~RecoveryMark() {
  assert(_thread->runtime_recovery_state()->is_in_recovery(), "sanity check");
  _thread->runtime_recovery_state()->clr_in_recovery();
}

RuntimeRecoveryState::RuntimeRecoveryState(JavaThread* thread):
  _thread(thread),
  _in_recovery(false),
  _in_run_jpf(false),
  _recover_in_deopt(false),
  _earlyret_offset(-1),
  _earlyret_type(T_ILLEGAL),
  _earlyret_size_of_parameters(0),
  _earlyret_state(earlyret_inactive),
  _earlyret_tos(ilgl),
  _earlyret_oop(NULL),
  _last_checked_exception(NULL),
  _earlyret_dispatch_next(NULL)
{
  _earlyret_value.j = 0L;
}

void RuntimeRecoveryState::reset_runtime_recovery_state() {
  assert(_thread->runtime_recovery_state()->is_in_recovery(), "sanity check");

  _in_run_jpf = false;
  _recover_in_deopt = false;
  _earlyret_offset = -1;
  _earlyret_type = T_ILLEGAL;
  _earlyret_size_of_parameters = 0;
  _earlyret_state = earlyret_inactive;
  _earlyret_tos = ilgl;
  _earlyret_value.j = 0L;
  _earlyret_oop = NULL;
  _last_checked_exception = NULL;
}

RuntimeRecoveryState::~RuntimeRecoveryState(){
  // TODO
}

void RuntimeRecoveryState::oops_do(OopClosure* f) {
  f->do_oop((oop*) &_earlyret_oop);
  f->do_oop((oop*) &_last_checked_exception);
}
