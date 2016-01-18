#ifndef SHARE_VM_RUNTIME_RECOVERY_ORACLE_HPP
#define SHARE_VM_RUNTIME_RECOVERY_ORACLE_HPP

#include "memory/universe.hpp"
#include "runtime/jniHandles.hpp"
//#include "utilities/top.hpp"

// hiredis is written in c
// to include it in hotspot we need write a special rule
#include <hiredis/hiredis.h>

#define TRACE_EARLYRET         0x00000001
#define TRACE_TRANSFORMING     0x00000002
#define TRACE_CHECKING         0x00000004
#define TRACE_FILL_STACK       0x00000008
#define TRACE_PRINT_STACK      0x00000010


#define TRACE_USE_STACK        0x00000020
#define TRACE_USE_REDIS        0x00000040
#define TRACE_USE_INDUCED      0x00000080
#define TRACE_CHECK_ESCAPE     0x00000100
// Ignore exception without void_string init
#define TRACE_IGNORE           0x00000200
#define TRACE_LOAD_STACK       0x00000400


#define TRACE_PRINT_ACTION     0x00000800

#define TRACE_SKIP_UNSAFE      0x00001000
#define TRACE_RECURSIVE        0x00002000

//class Recovery : AllStatic {
//
//}

class RecoveryAction;

class RecoveryOracle: AllStatic {

public:
  enum FailureType {
    _not_a_failure = 0,
    _recovery_disabled = 1,
    _internal_error = 2,
    _uncaught_exception = 3,
    _trivially_handled = 4,
  };

  enum RecoveryType {
    _no_recovery = 0,
    _error_transformation = 1,
    _early_return = 2
  };

private:
  static redisContext* _context;

  static volatile jint  _recovered_count;

public:

  static jint next_recovered_count();

  static const char* failure_type_name(FailureType);
  static const char* recovery_type_name(RecoveryType);

  static redisContext* context();
  static void initialize();

  static bool quick_cannot_recover_check(JavaThread* thread, Handle exception);

  static bool require_recovery(FailureType ft) {
    return ft == _uncaught_exception || ft == _trivially_handled;
  }

  static bool can_recover(RecoveryAction* action);

  static bool is_trivial_handler(KlassHandle handler_klass);

  static void recover(JavaThread* thread, RecoveryAction* action);
  static void do_recover(JavaThread* thread, RecoveryAction* action);

  static bool has_unsafe_init(JavaThread* thread, GrowableArray<Method*>* methods, GrowableArray<int>* bcis, RecoveryAction* action);

  static void determine_failure_type_and_recovery_context(JavaThread* thread, GrowableArray<Method*>* methods, GrowableArray<int>* bcis, RecoveryAction* action);
  static void determine_recovery_action(JavaThread* thread, GrowableArray<Method*>* methods, GrowableArray<int>* bcis, RecoveryAction* action);

  static Handle allocate_target_exception(JavaThread* thread, Handle origin_exception, KlassHandle target_exception_klass);

  static void fast_error_transformation(JavaThread* thread, GrowableArray<Method*>* methods, GrowableArray<int>* bcis, RecoveryAction* action);
  static void fast_early_return(JavaThread* thread, GrowableArray<Method*>* methods, GrowableArray<int>* bcis, RecoveryAction* action);

  static void fast_exception_handler_bci_and_caught_klass_for(Method* mh,
          KlassHandle ex_klass, int throw_bci, KlassHandle &caught_klass,
          int &hander_bci, bool ignore_no_string_void, TRAPS);
  static void fast_exception_handler_bci_and_caught_klass_use_induced(Method* mh,
          int throw_bci, KlassHandle &caught_klass, int &hander_bci, TRAPS);


  static bool redis_contains_key_common(const char* keys_command, TRAPS);
  static bool redis_contains_key_prefix(const char* prefix, TRAPS);
  static bool redis_contains_key_precise(const char* key, TRAPS);

  static void fill_stack(JavaThread* thread, GrowableArray<Method*>* methods, GrowableArray<int>* bcis, int max_depth=MaxJavaStackTraceDepth, int max_frame_depth=MaxJavaStackTraceDepth);

  static void query_known_exception_handler(
        GrowableArray<Method*>* methods,
        GrowableArray<int>* bcis,
        Handle cause_exception,
        KlassHandle &known_exception_type,
        Method* &hander_method,
        int &bci,
        int &handler_index,
        TRAPS
      );

  static void query_known_exception_handler(
        Handle cause_exception,
        KlassHandle &known_exception_type,
        Method* &hander_method,
        int &bci,
        int &index,
        TRAPS
      );

  static bool has_known_exception_handler(
       int begin_index,
       int end_index,
       GrowableArray<Method*>* methods,
       GrowableArray<int>* bcis,
       Method* top_method,
       KlassHandle &known_exception_type,
       Method* &handler_method,
       int &handler_bci,
       int &handler_index,
       TRAPS);

  static bool has_known_exception_handler_use_redis(
       int begin_index,
       int end_index,
       GrowableArray<Method*>* methods,
       GrowableArray<int>* bcis,
       Method* top_method,
       KlassHandle &known_exception_type,
       Method* &hander_method,
       int &bci,
       int &handler_index,
       TRAPS);

  static bool has_known_exception_handler_use_stack(
       int begin_index,
       int end_index,
       GrowableArray<Method*>* methods,
       GrowableArray<int>* bcis,
       Method* top_method,
       KlassHandle &known_exception_type,
       Method* &hander_method,
       int &bci,
       int &handler_index,
       TRAPS);

  static bool has_known_exception_handler_force_throwable(
       int begin_index,
       int end_index,
       GrowableArray<Method*>* methods,
       GrowableArray<int>* bcis,
       Method* top_method,
       KlassHandle &known_exception_type,
       Method* &hander_method,
       int &bci,
       int &handler_index,
       TRAPS);

  static bool is_sun_reflect_NativeMethodAccessorImpl(Method* mh);

  static void run_jpf_with_recovery_action(JavaThread* thread, GrowableArray<Method*>* methods,
      GrowableArray<int>* bcis, RecoveryAction* action);
  static objArrayOop run_jpf_with_exception(JavaThread* thread, Handle exception, int &max_depth);
  static objArrayOop load_stack_data(JavaThread* thread, Handle exception, int &max_depth);
};

class RecoveryAction : StackObj {
private:

  JavaThread* _thread;

  RecoveryOracle::FailureType _failure_type;
  RecoveryOracle::RecoveryType _recovery_type;

  int _recovery_context_offset;

  // Currently use offset
  // TODO use frame id
  int _early_return_offset; // top is 0
  BasicType _early_return_type;
  int _early_return_size_of_parameters;

  Handle* _origin_exception;

  // Used by reflection
  Method** _top_method;

  // Use jni handles, thus we can freely use HandleMark
  Klass* _target_exception_klass;

public:
  RecoveryAction(
      JavaThread* thread,
      Handle* origin_exception) :
    _thread(thread),
    _failure_type(RecoveryOracle::_not_a_failure),
    _recovery_type(RecoveryOracle::_no_recovery),
    _recovery_context_offset(-1),
    _early_return_offset(-1),
    _early_return_type(T_ILLEGAL),
    _early_return_size_of_parameters(0),
    _origin_exception(origin_exception),
    _top_method(NULL),
    _target_exception_klass(NULL) {
  }

  ~RecoveryAction() {
  }

  bool is_uncaught_exception() const { return _failure_type == RecoveryOracle::_uncaught_exception; }
  bool is_trivially_handled() const { return _failure_type == RecoveryOracle::_trivially_handled; }

  bool can_early_return();
  bool can_error_transformation();

  JavaThread* java_thread() { return _thread; }

  RecoveryOracle::FailureType failure_type () { return _failure_type; }

  RecoveryOracle::RecoveryType recovery_type() { return _recovery_type; }

  void set_failure_type(RecoveryOracle::FailureType type) {
    _failure_type = type;
  }

  void set_recovery_type(RecoveryOracle::RecoveryType type) {
    _recovery_type = type;
  }

  Handle allocate_target_exception(JavaThread* thread, Handle origin_exception);

  void set_recovery_context_offset(int offset) {
    _recovery_context_offset = offset;
  }

  int recovery_context_offset() {
    return _recovery_context_offset;
  }

  void set_target_exception_klass(KlassHandle klass) {
    _target_exception_klass = klass();
  }

  bool has_top_method() { return _top_method != NULL; }
  Method* top_method() { return *_top_method; }
  void set_top_method(Method** m) { _top_method = m; }

  Handle origin_exception() { return (*_origin_exception); }

  Klass* target_exception_klass() {
    assert(_target_exception_klass != NULL, "sanity check");
    return _target_exception_klass;
  }

  void set_early_return_offset(int offset) {
    _early_return_offset = offset;
  }

  int early_return_offset() {
    return _early_return_offset;
  }

  void set_early_return_type(BasicType type) {
    _early_return_type = type;
  }

  BasicType early_return_type() {
    return _early_return_type;
  }

  void set_early_return_size_of_parameters(int size) {
    _early_return_size_of_parameters = size;
  }

  int early_return_size_of_parameters() {
    return _early_return_size_of_parameters;
  }

  void print();
};


#endif // SHARE_VM_RUNTIME_RECOVERY_ORACLE_HPP

