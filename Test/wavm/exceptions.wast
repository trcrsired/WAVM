(module $A
  (exception_type $a (export "a") i32)
  (exception_type $b (export "b") i32)
  (exception_type $c (export "c") i32 f64)
  (exception_type $d (export "d"))

  ;; WAVM's throw passes the exception payload through linear memory.
  (memory 1)

  (type $i32_to_void_sig (func (param i32)))
  (func $throw_a (export "throw_a") (type $i32_to_void_sig) (param i32) (throw $a (local.get 0)))
  (func $throw_b (export "throw_b") (type $i32_to_void_sig) (param i32) (throw $b (local.get 0)))
  (func $throw_c (export "throw_c") (param i32 f64) (throw $c (local.get 1) (local.get 0)))
  (func $throw_d (export "throw_d") i32.const 0 (throw $d))
  (func $no_throw (export "no_throw") (type $i32_to_void_sig) (param i32))
  (func $divide_by_zero (export "divide_by_zero") (type $i32_to_void_sig) (param i32)
    local.get 0
    i32.const 0
    i32.div_s
    drop
    )


  (table funcref (elem $no_throw $divide_by_zero $throw_a $throw_b))

  ;; try_table/catch_all: the handler is an arity-0 label; the code after the block
  ;; is the handler body.
  (func (export "try_table_without_throw") (result i32)
    block $h
      try_table (catch_all $h)
        i32.const 5
        return
      end
    end
    i32.const 6
    )

  ;; Traps are host exceptions, not wasm exceptions: catch_all must not catch them.
  (func (export "try_table_with_divide_by_zero") (result i32)
    block $h
      try_table (catch_all $h)
        i32.const 0
        i32.const 0
        i32.div_s
        drop
        i32.const 7
        return
      end
    end
    i32.const 8
    )

  ;; catch $a pushes the exception's argument(s) at the label.
  (func (export "catch_throw") (result i32)
    block $h (result i32)
      try_table (catch $a $h)
        i32.const 9
        throw $a
      end
      unreachable
    end
    )

  (func (export "catch_call_throw") (result i32)
    block $h (result i32)
      try_table (result i32) (catch $a $h)
        i32.const 9
        call $throw_a
        i32.const 10
      end
    end
    )

  ;; A thrown $a doesn't match a $b clause: rethrown and escapes uncaught.
  (func (export "catch_with_different_throw") (result i32)
    block $h (result i32)
      try_table (result i32) (catch $b $h)
        i32.const 11
        throw $a
      end
    end
    )

  (func (export "catch_all") (param $thunk i32) (result i32)
    block $h
      try_table (catch_all $h)
        i32.const 13
        local.get $thunk
        call_indirect (type $i32_to_void_sig)
        i32.const 14
        return
      end
    end
    i32.const 15
    )

  ;; catch_all_ref produces an exnref at the label; the handler can hold it in a local.
  (func (export "try_table_inside_handler") (result i32) (local $e exnref)
    block $ha (result exnref)
      try_table (catch_all_ref $ha)
        i32.const 16
        throw $a
      end
      unreachable
    end
    local.set $e
    block $hb (result i32)
      try_table (catch $b $hb)
        i32.const 18
        throw $b
      end
      unreachable
    end
    )

  ;; catch_ref produces the tag's arguments followed by an exnref at the label.
  ;; throw_ref re-raises the exception named by the exnref (the legacy 'rethrow' role).
  (func (export "catch_ref_rethrow") (result i32) (local $ea exnref) (local $eb exnref)
    block $ha (result i32 exnref)
      try_table (catch_ref $a $ha)
        i32.const 20
        throw $a
      end
      unreachable
    end
    local.set $ea
    drop
    block $hb (result i32 exnref)
      try_table (catch_ref $b $hb)
        i32.const 22
        throw $b
      end
      unreachable
    end
    local.set $eb
    drop
    local.get $ea
    throw_ref
    )

  (func (export "catch_all_ref_rethrow") (result i32) (local $ea exnref) (local $eb exnref)
    block $ha (result exnref)
      try_table (catch_all_ref $ha)
        i32.const 23
        throw $a
      end
      unreachable
    end
    local.set $ea
    block $hb (result exnref)
      try_table (catch_all_ref $hb)
        i32.const 25
        throw $b
      end
      unreachable
    end
    local.set $eb
    local.get $ea
    throw_ref
    )

  ;; Throwing a new exception from inside a handler.
  (func (export "throw_from_catch") (result i32) (local $e exnref)
    block $ha (result i32 exnref)
      try_table (catch_ref $a $ha)
        i32.const 27
        throw $a
      end
      unreachable
    end
    local.set $e
    drop
    i32.const 27
    throw $b
    )

  ;; An exnref that survives its catch scope (stored in a global) can still be rethrown.
  (global $saved (mut exnref) (ref.null exnref))
  (func (export "exnref_outlives_scope") (param i32) (result i32) (local $e exnref)
    block $ha (result i32 exnref)
      try_table (catch_ref $a $ha)
        local.get 0
        throw $a
      end
      unreachable
    end
    local.set $e
    drop
    local.get $e
    global.set $saved
    ;; The catch scope that produced the exnref is over, but the record is still owned
    ;; by the runtime's dead list, so throw_ref rethrows it and the outer catch_all gets it.
    block $out
      try_table (catch_all $out)
        global.get $saved
        throw_ref
      end
      unreachable
    end
    i32.const 31
    )

  ;; throw_ref on a null exnref fails closed.
  (func (export "throw_ref_null") (result i32)
    ref.null exnref
    throw_ref
    )
)

(assert_trap (invoke "throw_a" (i32.const 1)) "uncaught exception")
(assert_trap (invoke "throw_b" (i32.const 2)) "uncaught exception")
(assert_trap (invoke "throw_c" (i32.const 3) (f64.const 4.0)) "uncaught exception")
(assert_trap (invoke "throw_d") "uncaught exception")

(assert_return (invoke "try_table_without_throw") (i32.const 5))
(assert_trap (invoke "try_table_with_divide_by_zero") "integer divide by zero")
(assert_return (invoke "catch_throw") (i32.const 9))
(assert_return (invoke "catch_call_throw") (i32.const 9))
(assert_trap (invoke "catch_with_different_throw") "uncaught exception")

(assert_return (invoke "catch_all" (i32.const 0)) (i32.const 14))
(assert_trap (invoke "catch_all" (i32.const 1)) "integer divide by zero")
(assert_return (invoke "catch_all" (i32.const 2)) (i32.const 15))
(assert_return (invoke "catch_all" (i32.const 3)) (i32.const 15))

(assert_return (invoke "try_table_inside_handler") (i32.const 18))
(assert_trap (invoke "catch_ref_rethrow") "uncaught exception")
(assert_trap (invoke "catch_all_ref_rethrow") "uncaught exception")

(assert_trap (invoke "throw_from_catch") "uncaught exception")
(assert_return (invoke "exnref_outlives_scope" (i32.const 30)) (i32.const 31))
(assert_trap (invoke "throw_ref_null") "invalid exnref")

;; todo:
;; throw inside of function vs directly in try_table
;; throw in handler
;; try_table with multiple catch clauses with the same exception types
;; try_table with catch_ref and catch_all
;; catch clause label arity checking

(register "A" $A)

(module
  (exception_type $a (import "A" "a") i32)
  (exception_type $b (import "A" "b") i32)
  (exception_type $c (import "A" "c") i32 f64)
  (exception_type $d (import "A" "d"))
)

(assert_unlinkable (module (exception_type (import "A" "a") i64)) "import type doesn't match")
(assert_unlinkable (module (exception_type (import "A" "c") i32 i64)) "import type doesn't match")

;; An imported tag is the same tag: a throw of A's $a is caught by a catch on the
;; imported $a, even in a different module.
(module $B
  (func $throw_a (import "A" "throw_a") (param i32))
  (exception_type $a (import "A" "a") i32)
  (memory 1)
  (func (export "cross_module_catch") (param i32) (result i32)
    block $h (result i32)
      try_table (result i32) (catch $a $h)
        local.get 0
        call $throw_a
        i32.const -1
      end
    end
  )
)
(assert_return (invoke "cross_module_catch" (i32.const 77)) (i32.const 77))

;; A different tag with the same signature is a different tag: a locally defined tag
;; does not match A's $a.
(module $C
  (func $throw_a (import "A" "throw_a") (param i32))
  (exception_type $mine i32)
  (memory 1)
  (func (export "cross_module_no_match") (param i32) (result i32)
    block $h (result i32)
      try_table (result i32) (catch $mine $h)
        local.get 0
        call $throw_a
        i32.const -1
      end
    end
  )
)
(assert_trap (invoke "cross_module_no_match" (i32.const 5)) "uncaught exception")
