//
//  Copyright (C) 2022  Nick Gasson
//
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include "util.h"
#include "array.h"
#include "hash.h"
#include "ident.h"
#include "jit/jit-llvm.h"
#include "jit/jit-priv.h"
#include "lib.h"
#include "opt.h"
#include "rt/rt.h"
#include "thread.h"

#include <assert.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>

#include <llvm-c/Analysis.h>
#include <llvm-c/BitReader.h>
#include <llvm-c/Core.h>
#include <llvm-c/DebugInfo.h>
#include <llvm-c/Error.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Transforms/Scalar.h>

#ifdef LLVM_HAS_LLJIT
#include <llvm-c/LLJIT.h>
#endif

typedef enum {
   LLVM_VOID,
   LLVM_PTR,
   LLVM_INT1,
   LLVM_INT8,
   LLVM_INT16,
   LLVM_INT32,
   LLVM_INT64,
   LLVM_INTPTR,
   LLVM_DOUBLE,

   LLVM_PAIR_I8_I1,
   LLVM_PAIR_I16_I1,
   LLVM_PAIR_I32_I1,
   LLVM_PAIR_I64_I1,

   LLVM_ENTRY_FN,
   LLVM_ANCHOR,
   LLVM_CTOR_FN,
   LLVM_CTOR,

   LLVM_LAST_TYPE
} llvm_type_t;

typedef enum {
   LLVM_ADD_OVERFLOW_S8,
   LLVM_ADD_OVERFLOW_S16,
   LLVM_ADD_OVERFLOW_S32,
   LLVM_ADD_OVERFLOW_S64,

   LLVM_ADD_OVERFLOW_U8,
   LLVM_ADD_OVERFLOW_U16,
   LLVM_ADD_OVERFLOW_U32,
   LLVM_ADD_OVERFLOW_U64,

   LLVM_SUB_OVERFLOW_S8,
   LLVM_SUB_OVERFLOW_S16,
   LLVM_SUB_OVERFLOW_S32,
   LLVM_SUB_OVERFLOW_S64,

   LLVM_SUB_OVERFLOW_U8,
   LLVM_SUB_OVERFLOW_U16,
   LLVM_SUB_OVERFLOW_U32,
   LLVM_SUB_OVERFLOW_U64,

   LLVM_MUL_OVERFLOW_S8,
   LLVM_MUL_OVERFLOW_S16,
   LLVM_MUL_OVERFLOW_S32,
   LLVM_MUL_OVERFLOW_S64,

   LLVM_MUL_OVERFLOW_U8,
   LLVM_MUL_OVERFLOW_U16,
   LLVM_MUL_OVERFLOW_U32,
   LLVM_MUL_OVERFLOW_U64,

   LLVM_POW_F64,
   LLVM_ROUND_F64,

   LLVM_DO_EXIT,
   LLVM_GETPRIV,
   LLVM_PUTPRIV,
   LLVM_MSPACE_ALLOC,
   LLVM_DO_FFICALL,
   LLVM_TRAMPOLINE,
   LLVM_REGISTER,
   LLVM_GET_FUNC,
   LLVM_GET_FOREIGN,

   LLVM_LAST_FN,
} llvm_fn_t;

typedef struct _cgen_func  cgen_func_t;
typedef struct _cgen_block cgen_block_t;

typedef struct _llvm_obj {
   LLVMModuleRef         module;
   LLVMContextRef        context;
   LLVMTargetMachineRef  target;
   LLVMBuilderRef        builder;
   LLVMTargetDataRef     data_ref;
   LLVMTypeRef           types[LLVM_LAST_TYPE];
   LLVMValueRef          fns[LLVM_LAST_FN];
   LLVMTypeRef           fntypes[LLVM_LAST_FN];
   LLVMValueRef          ctor;
   shash_t              *string_pool;
} llvm_obj_t;

typedef struct _cgen_block {
   LLVMBasicBlockRef  bbref;
   LLVMValueRef       inflags;
   LLVMValueRef       outflags;
   LLVMValueRef      *inregs;
   LLVMValueRef      *outregs;
   jit_block_t       *source;
   cgen_func_t       *func;
} cgen_block_t;

typedef struct _cgen_func {
   LLVMValueRef  llvmfn;
   LLVMValueRef  args;
   LLVMValueRef  frame;
   LLVMValueRef  anchor;
   LLVMValueRef  cpool;
   cgen_block_t *blocks;
   jit_func_t   *source;
   jit_cfg_t    *cfg;
   char         *name;
} cgen_func_t;

#define LLVM_CHECK(op, ...) do {                        \
      LLVMErrorRef error = op(__VA_ARGS__);             \
      if (unlikely(error != LLVMErrorSuccess)) {        \
         char *msg = LLVMGetErrorMessage(error);        \
         fatal(#op " failed: %s", msg);                 \
      }                                                 \
   } while (0)

#ifdef LLVM_HAS_OPAQUE_POINTERS
#define PTR(x) x
#else
#define PTR(x) \
   LLVMBuildPointerCast(obj->builder, (x), obj->types[LLVM_PTR], "")
#endif

////////////////////////////////////////////////////////////////////////////////
// LLVM wrappers

static LLVMValueRef llvm_int1(llvm_obj_t *obj, bool b)
{
   return LLVMConstInt(obj->types[LLVM_INT1], b, false);
}

static LLVMValueRef llvm_int8(llvm_obj_t *obj, int8_t i)
{
   return LLVMConstInt(obj->types[LLVM_INT8], i, false);
}

static LLVMValueRef llvm_int32(llvm_obj_t *obj, int32_t i)
{
   return LLVMConstInt(obj->types[LLVM_INT32], i, false);
}

static LLVMValueRef llvm_int64(llvm_obj_t *obj, int64_t i)
{
   return LLVMConstInt(obj->types[LLVM_INT64], i, false);
}

static LLVMValueRef llvm_intptr(llvm_obj_t *obj, intptr_t i)
{
   return LLVMConstInt(obj->types[LLVM_INTPTR], i, false);
}

static LLVMValueRef llvm_ptr(llvm_obj_t *obj, void *ptr)
{
   return LLVMConstIntToPtr(llvm_intptr(obj, (intptr_t)ptr),
                            obj->types[LLVM_PTR]);
}

static LLVMValueRef llvm_real(llvm_obj_t *obj, double r)
{
   return LLVMConstReal(obj->types[LLVM_DOUBLE], r);
}

static void llvm_register_types(llvm_obj_t *obj)
{
   obj->types[LLVM_VOID]   = LLVMVoidTypeInContext(obj->context);
   obj->types[LLVM_INT1]   = LLVMInt1TypeInContext(obj->context);
   obj->types[LLVM_INT8]   = LLVMInt8TypeInContext(obj->context);
   obj->types[LLVM_INT16]  = LLVMInt16TypeInContext(obj->context);
   obj->types[LLVM_INT32]  = LLVMInt32TypeInContext(obj->context);
   obj->types[LLVM_INT64]  = LLVMInt64TypeInContext(obj->context);
   obj->types[LLVM_DOUBLE] = LLVMDoubleTypeInContext(obj->context);

   obj->types[LLVM_INTPTR] = LLVMIntPtrTypeInContext(obj->context,
                                                     obj->data_ref);

#ifdef LLVM_HAS_OPAQUE_POINTERS
   obj->types[LLVM_PTR] = LLVMPointerTypeInContext(obj->context, 0);
#else
   obj->types[LLVM_PTR] = LLVMPointerType(obj->types[LLVM_INT8], 0);
#endif

   {
      LLVMTypeRef atypes[] = {
         obj->types[LLVM_PTR],    // Function
         obj->types[LLVM_PTR],    // Anchor
         obj->types[LLVM_PTR]     // Arguments
      };
      obj->types[LLVM_ENTRY_FN] = LLVMFunctionType(obj->types[LLVM_VOID],
                                                   atypes, ARRAY_LEN(atypes),
                                                   false);
   }

   obj->types[LLVM_CTOR_FN] = LLVMFunctionType(obj->types[LLVM_VOID],
                                               NULL, 0, false);

   {
      LLVMTypeRef fields[] = {
         obj->types[LLVM_PTR],    // Caller
         obj->types[LLVM_PTR],    // Function
         obj->types[LLVM_INT32]   // IR position
      };
      obj->types[LLVM_ANCHOR] = LLVMStructTypeInContext(obj->context, fields,
                                                        ARRAY_LEN(fields),
                                                        false);
   }

   {
      LLVMTypeRef fields[] = {
         obj->types[LLVM_INT32],
         obj->types[LLVM_INT1]
      };
      obj->types[LLVM_PAIR_I32_I1] = LLVMStructTypeInContext(obj->context,
                                                             fields,
                                                             ARRAY_LEN(fields),
                                                             false);
   }

   {
      LLVMTypeRef fields[] = {
         obj->types[LLVM_INT32],
#ifdef LLVM_HAS_OPAQUE_POINTERS
         obj->types[LLVM_PTR],
#else
         LLVMPointerType(obj->types[LLVM_CTOR_FN], 0),
#endif
         obj->types[LLVM_PTR],
      };
      obj->types[LLVM_CTOR] = LLVMStructTypeInContext(obj->context, fields,
                                                      ARRAY_LEN(fields), false);
   }
}

static void llvm_dump_module(LLVMModuleRef module, const char *tag)
{
   size_t length;
   const char *module_name = LLVMGetModuleIdentifier(module, &length);

   if (!opt_get_verbose(OPT_LLVM_VERBOSE, module_name))
      return;

   LOCAL_TEXT_BUF tb = tb_new();
   tb_printf(tb, "%s.%s.ll", module_name, tag);

   char *error;
   if (LLVMPrintModuleToFile(module, tb_get(tb), &error))
      fatal("Failed to write LLVM IR file: %s", error);

   debugf("wrote LLVM IR for %s to %s", module_name, tb_get(tb));
}

static void llvm_verify_module(LLVMModuleRef module)
{
#ifdef DEBUG
   if (LLVMVerifyModule(module, LLVMPrintMessageAction, NULL)) {
      size_t len;
      const char *name = LLVMGetModuleIdentifier(module, &len);
      fatal("LLVM verification failed for %s", name);
   }
#endif
}

static void llvm_optimise(LLVMModuleRef module)
{
   LLVMPassManagerRef fpm = LLVMCreateFunctionPassManagerForModule(module);

   LLVMAddScalarReplAggregatesPass(fpm);
   LLVMAddInstructionCombiningPass(fpm);
   LLVMAddReassociatePass(fpm);
   LLVMAddGVNPass(fpm);
   LLVMAddCFGSimplificationPass(fpm);

   LLVMInitializeFunctionPassManager(fpm);

   for (LLVMValueRef fn = LLVMGetFirstFunction(module);
        fn != NULL; fn = LLVMGetNextFunction(fn))
      LLVMRunFunctionPassManager(fpm, fn);

   LLVMFinalizeFunctionPassManager(fpm);
   LLVMDisposePassManager(fpm);
}

static void llvm_finalise(llvm_obj_t *obj)
{
   llvm_dump_module(obj->module, "initial");
   llvm_verify_module(obj->module);
   llvm_optimise(obj->module);
   llvm_dump_module(obj->module, "final");
}

static LLVMTargetMachineRef llvm_target_machine(LLVMRelocMode reloc,
                                                LLVMCodeModel model)
{
   char *def_triple = LLVMGetDefaultTargetTriple();
   char *error;
   LLVMTargetRef target_ref;
   if (LLVMGetTargetFromTriple(def_triple, &target_ref, &error))
      fatal("failed to get LLVM target for %s: %s", def_triple, error);

   LLVMTargetMachineRef tm = LLVMCreateTargetMachine(target_ref, def_triple,
                                                     "", "",
                                                     LLVMCodeGenLevelDefault,
                                                     reloc, model);
   LLVMDisposeMessage(def_triple);

   return tm;
}

static LLVMBasicBlockRef llvm_append_block(llvm_obj_t *obj, LLVMValueRef fn,
                                           const char *name)
{
   return LLVMAppendBasicBlockInContext(obj->context, fn, name);
}

static LLVMValueRef llvm_add_fn(llvm_obj_t *obj, const char *name,
                                LLVMTypeRef type)
{
   LLVMValueRef fn = LLVMGetNamedFunction(obj->module, name);
   if (fn == NULL)
      fn = LLVMAddFunction(obj->module, name, type);

   return fn;
}

static LLVMValueRef llvm_get_fn(llvm_obj_t *obj, llvm_fn_t which)
{
   if (obj->fns[which] != NULL)
      return obj->fns[which];

   LLVMValueRef fn = NULL;
   switch (which) {
   case LLVM_ADD_OVERFLOW_S8:
   case LLVM_ADD_OVERFLOW_S16:
   case LLVM_ADD_OVERFLOW_S32:
   case LLVM_ADD_OVERFLOW_S64:
      {
         jit_size_t sz = which - LLVM_ADD_OVERFLOW_S8;
         LLVMTypeRef int_type = obj->types[LLVM_INT8 + sz];
         LLVMTypeRef pair_type = obj->types[LLVM_PAIR_I8_I1 + sz];
         LLVMTypeRef args[] = { int_type, int_type };
         obj->fntypes[which] = LLVMFunctionType(pair_type, args,
                                                ARRAY_LEN(args), false);

         static const char *names[] = {
            "llvm.sadd.with.overflow.i8",
            "llvm.sadd.with.overflow.i16",
            "llvm.sadd.with.overflow.i32",
            "llvm.sadd.with.overflow.i64"
         };
         fn = llvm_add_fn(obj, names[sz], obj->fntypes[which]);
      }
      break;

   case LLVM_ADD_OVERFLOW_U8:
   case LLVM_ADD_OVERFLOW_U16:
   case LLVM_ADD_OVERFLOW_U32:
   case LLVM_ADD_OVERFLOW_U64:
      {
         jit_size_t sz = which - LLVM_ADD_OVERFLOW_U8;
         LLVMTypeRef int_type = obj->types[LLVM_INT8 + sz];
         LLVMTypeRef pair_type = obj->types[LLVM_PAIR_I8_I1 + sz];
         LLVMTypeRef args[] = { int_type, int_type };
         obj->fntypes[which] = LLVMFunctionType(pair_type, args,
                                                ARRAY_LEN(args), false);

         static const char *names[] = {
            "llvm.uadd.with.overflow.i8",
            "llvm.uadd.with.overflow.i16",
            "llvm.uadd.with.overflow.i32",
            "llvm.uadd.with.overflow.i64"
         };
         fn = llvm_add_fn(obj, names[sz], obj->fntypes[which]);
      }
      break;

   case LLVM_SUB_OVERFLOW_S8:
   case LLVM_SUB_OVERFLOW_S16:
   case LLVM_SUB_OVERFLOW_S32:
   case LLVM_SUB_OVERFLOW_S64:
      {
         jit_size_t sz = which - LLVM_SUB_OVERFLOW_S8;
         LLVMTypeRef int_type = obj->types[LLVM_INT8 + sz];
         LLVMTypeRef pair_type = obj->types[LLVM_PAIR_I8_I1 + sz];
         LLVMTypeRef args[] = { int_type, int_type };
         obj->fntypes[which] = LLVMFunctionType(pair_type, args,
                                                ARRAY_LEN(args), false);

         static const char *names[] = {
            "llvm.ssub.with.overflow.i8",
            "llvm.ssub.with.overflow.i16",
            "llvm.ssub.with.overflow.i32",
            "llvm.ssub.with.overflow.i64"
         };
         fn = llvm_add_fn(obj, names[sz], obj->fntypes[which]);
      }
      break;

   case LLVM_SUB_OVERFLOW_U8:
   case LLVM_SUB_OVERFLOW_U16:
   case LLVM_SUB_OVERFLOW_U32:
   case LLVM_SUB_OVERFLOW_U64:
      {
         jit_size_t sz = which - LLVM_SUB_OVERFLOW_U8;
         LLVMTypeRef int_type = obj->types[LLVM_INT8 + sz];
         LLVMTypeRef pair_type = obj->types[LLVM_PAIR_I8_I1 + sz];
         LLVMTypeRef args[] = { int_type, int_type };
         obj->fntypes[which] = LLVMFunctionType(pair_type, args,
                                                ARRAY_LEN(args), false);

         static const char *names[] = {
            "llvm.usub.with.overflow.i8",
            "llvm.usub.with.overflow.i16",
            "llvm.usub.with.overflow.i32",
            "llvm.usub.with.overflow.i64"
         };
         fn = llvm_add_fn(obj, names[sz], obj->fntypes[which]);
      }
      break;

   case LLVM_MUL_OVERFLOW_S8:
   case LLVM_MUL_OVERFLOW_S16:
   case LLVM_MUL_OVERFLOW_S32:
   case LLVM_MUL_OVERFLOW_S64:
      {
         jit_size_t sz = which - LLVM_MUL_OVERFLOW_S8;
         LLVMTypeRef int_type = obj->types[LLVM_INT8 + sz];
         LLVMTypeRef pair_type = obj->types[LLVM_PAIR_I8_I1 + sz];
         LLVMTypeRef args[] = { int_type, int_type };
         obj->fntypes[which] = LLVMFunctionType(pair_type, args,
                                                ARRAY_LEN(args), false);

         static const char *names[] = {
            "llvm.smul.with.overflow.i8",
            "llvm.smul.with.overflow.i16",
            "llvm.smul.with.overflow.i32",
            "llvm.smul.with.overflow.i64"
         };
         fn = llvm_add_fn(obj, names[sz], obj->fntypes[which]);
      }
      break;

   case LLVM_MUL_OVERFLOW_U8:
   case LLVM_MUL_OVERFLOW_U16:
   case LLVM_MUL_OVERFLOW_U32:
   case LLVM_MUL_OVERFLOW_U64:
      {
         jit_size_t sz = which - LLVM_MUL_OVERFLOW_U8;
         LLVMTypeRef int_type = obj->types[LLVM_INT8 + sz];
         LLVMTypeRef pair_type = obj->types[LLVM_PAIR_I8_I1 + sz];
         LLVMTypeRef args[] = { int_type, int_type };
         obj->fntypes[which] = LLVMFunctionType(pair_type, args,
                                                ARRAY_LEN(args), false);

         static const char *names[] = {
            "llvm.umul.with.overflow.i8",
            "llvm.umul.with.overflow.i16",
            "llvm.umul.with.overflow.i32",
            "llvm.umul.with.overflow.i64"
         };
         fn = llvm_add_fn(obj, names[sz], obj->fntypes[which]);
      }
      break;

   case LLVM_POW_F64:
      {
         LLVMTypeRef args[] = {
            obj->types[LLVM_DOUBLE],
            obj->types[LLVM_DOUBLE]
         };
         obj->fntypes[which] = LLVMFunctionType(obj->types[LLVM_DOUBLE],
                                                args, ARRAY_LEN(args), false);

         fn = llvm_add_fn(obj, "llvm.pow.f64", obj->fntypes[which]);
      }
      break;

   case LLVM_ROUND_F64:
      {
         LLVMTypeRef args[] = { obj->types[LLVM_DOUBLE] };
         obj->fntypes[which] = LLVMFunctionType(obj->types[LLVM_DOUBLE],
                                                args, ARRAY_LEN(args), false);

         fn = llvm_add_fn(obj, "llvm.round.f64", obj->fntypes[which]);
      }
      break;

   case LLVM_DO_EXIT:
      {
         LLVMTypeRef args[] = {
            obj->types[LLVM_INT32],
            obj->types[LLVM_PTR],
            obj->types[LLVM_PTR]
         };
         obj->fntypes[which] = LLVMFunctionType(obj->types[LLVM_VOID], args,
                                                ARRAY_LEN(args), false);

         fn = llvm_add_fn(obj, "__nvc_do_exit", obj->fntypes[which]);
      }
      break;

   case LLVM_DO_FFICALL:
      {
         LLVMTypeRef args[] = {
            obj->types[LLVM_PTR],
            obj->types[LLVM_PTR],
            obj->types[LLVM_PTR]
         };
         obj->fntypes[which] = LLVMFunctionType(obj->types[LLVM_VOID], args,
                                                ARRAY_LEN(args), false);

         fn = llvm_add_fn(obj, "__nvc_do_fficall", obj->fntypes[which]);
      }
      break;

   case LLVM_GETPRIV:
      {
         LLVMTypeRef args[] = { obj->types[LLVM_INT32] };
         obj->fntypes[which] = LLVMFunctionType(obj->types[LLVM_PTR], args,
                                                ARRAY_LEN(args), false);

         fn = llvm_add_fn(obj, "__nvc_getpriv", obj->fntypes[which]);
      }
      break;

   case LLVM_PUTPRIV:
      {
         LLVMTypeRef args[] = {
            obj->types[LLVM_INT32],
            obj->types[LLVM_PTR]
         };
         obj->fntypes[which] = LLVMFunctionType(obj->types[LLVM_VOID], args,
                                                ARRAY_LEN(args), false);

         fn = llvm_add_fn(obj, "__nvc_putpriv", obj->fntypes[which]);
      }
      break;

   case LLVM_MSPACE_ALLOC:
      {
         LLVMTypeRef args[] = {
            obj->types[LLVM_INT32],
            obj->types[LLVM_INT32]
         };
         obj->fntypes[which] = LLVMFunctionType(obj->types[LLVM_PTR], args,
                                                ARRAY_LEN(args), false);

         fn = llvm_add_fn(obj, "__nvc_mspace_alloc", obj->fntypes[which]);
      }
      break;

   case LLVM_TRAMPOLINE:
      {
         obj->fntypes[which] = obj->types[LLVM_ENTRY_FN];
         fn = llvm_add_fn(obj, "__nvc_trampoline", obj->fntypes[which]);
      }
      break;

   case LLVM_REGISTER:
      {
         LLVMTypeRef args[] = {
            obj->types[LLVM_PTR],
            obj->types[LLVM_PTR],
            obj->types[LLVM_PTR],
            obj->types[LLVM_INT32],
         };
         obj->fntypes[which] = LLVMFunctionType(obj->types[LLVM_VOID], args,
                                                ARRAY_LEN(args), false);
         fn = llvm_add_fn(obj, "__nvc_register", obj->fntypes[which]);
      }
      break;

   case LLVM_GET_FUNC:
      {
         LLVMTypeRef args[] = { obj->types[LLVM_PTR] };
         obj->fntypes[which] = LLVMFunctionType(obj->types[LLVM_PTR], args,
                                                ARRAY_LEN(args), false);
         fn = llvm_add_fn(obj, "__nvc_get_func", obj->fntypes[which]);
      }
      break;

   case LLVM_GET_FOREIGN:
      {
         LLVMTypeRef args[] = {
            obj->types[LLVM_PTR],
            obj->types[LLVM_INT64]
         };
         obj->fntypes[which] = LLVMFunctionType(obj->types[LLVM_PTR], args,
                                                ARRAY_LEN(args), false);
         fn = llvm_add_fn(obj, "__nvc_get_foreign", obj->fntypes[which]);
      }
      break;

   default:
      fatal_trace("cannot generate prototype for function %d", which);
   }

   return (obj->fns[which] = fn);
}

static LLVMValueRef llvm_call_fn(llvm_obj_t *obj, llvm_fn_t which,
                                 LLVMValueRef *args, unsigned count)
{
   LLVMValueRef fn = llvm_get_fn(obj, which);
   return LLVMBuildCall2(obj->builder, obj->fntypes[which], fn,
                         args, count, "");
}

static LLVMValueRef llvm_const_string(llvm_obj_t *obj, const char *str)
{
   if (obj->string_pool == NULL)
      obj->string_pool = shash_new(256);

   LLVMValueRef ref = shash_get(obj->string_pool, str);
   if (ref == NULL) {
      const size_t len = strlen(str);
      LLVMValueRef init =
         LLVMConstStringInContext(obj->context, str, len, false);
      ref = LLVMAddGlobal(obj->module,
                          LLVMArrayType(obj->types[LLVM_INT8], len + 1),
                          "const_string");
      LLVMSetGlobalConstant(ref, true);
      LLVMSetInitializer(ref, init);
      LLVMSetLinkage(ref, LLVMPrivateLinkage);
      LLVMSetUnnamedAddr(ref, true);

      shash_put(obj->string_pool, str, ref);
   }

#ifdef LLVM_HAS_OPAQUE_POINTERS
   return ref;
#else
   LLVMValueRef indexes[] = {
      llvm_int32(obj, 0),
      llvm_int32(obj, 0)
   };
   return LLVMBuildGEP(obj->builder, ref, indexes, ARRAY_LEN(indexes), "");
#endif
}

////////////////////////////////////////////////////////////////////////////////
// JIT IR to LLVM lowering

static const char *cgen_reg_name(jit_reg_t reg)
{
#ifdef DEBUG
   static volatile int uniq = 0;
   static __thread char buf[32];
   checked_sprintf(buf, sizeof(buf), "R%d.%d", reg, relaxed_add(&uniq, 1));
   return buf;
#else
   return "";
#endif
}

static const char *cgen_arg_name(int nth)
{
#ifdef DEBUG
   static volatile int uniq = 0;
   static __thread char buf[32];
   checked_sprintf(buf, sizeof(buf), "A%d.%d", nth, relaxed_add(&uniq, 1));
   return buf;
#else
   return "";
#endif
}

__attribute__((noreturn))
static void cgen_abort(cgen_block_t *cgb, jit_ir_t *ir, const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);

   char *text LOCAL = xvasprintf(fmt, ap);
   jit_dump_with_mark(cgb->func->source, ir - cgb->func->source->irbuf, false);
   fatal_trace("%s", text);

   va_end(ap);
}

static LLVMValueRef cgen_get_value(llvm_obj_t *obj, cgen_block_t *cgb,
                                   jit_value_t value)
{
   switch (value.kind) {
   case JIT_VALUE_REG:
      assert(value.reg < cgb->func->source->nregs);
      assert(cgb->outregs[value.reg] != NULL);
      return cgb->outregs[value.reg];
   case JIT_VALUE_INT64:
      return llvm_int64(obj, value.int64);
   case JIT_VALUE_DOUBLE:
      return llvm_real(obj, value.dval);
   case JIT_ADDR_FRAME:
      {
         assert(value.int64 >= 0 && value.int64 < cgb->func->source->framesz);
         LLVMValueRef indexes[] = { llvm_intptr(obj, value.int64) };
         LLVMTypeRef byte_type = obj->types[LLVM_INT8];
         return LLVMBuildInBoundsGEP2(obj->builder, byte_type,
                                      cgb->func->frame, indexes,
                                      ARRAY_LEN(indexes), "");
      }
   case JIT_ADDR_CPOOL:
      assert(value.int64 >= 0 && value.int64 <= cgb->func->source->cpoolsz);
      if (cgb->func->cpool != NULL) {
         LLVMValueRef indexes[] = { llvm_intptr(obj, value.int64) };
         LLVMTypeRef byte_type = obj->types[LLVM_INT8];
         return LLVMBuildInBoundsGEP2(obj->builder, byte_type,
                                      cgb->func->cpool, indexes,
                                      ARRAY_LEN(indexes), "");
      }
      else
         return llvm_ptr(obj, cgb->func->source->cpool + value.int64);
   case JIT_ADDR_REG:
      {
         assert(value.reg < cgb->func->source->nregs);
         LLVMValueRef ptr = cgb->outregs[value.reg];

         if (value.disp != 0) {
            LLVMValueRef disp = llvm_int64(obj, value.disp);
            ptr = LLVMBuildAdd(obj->builder, ptr, disp, "");
         }

         return ptr;
      }
   case JIT_VALUE_EXIT:
   case JIT_VALUE_HANDLE:
      return llvm_int32(obj, value.exit);
   case JIT_ADDR_ABS:
      assert(obj->ctor == NULL || value.int64 == 0);
      return llvm_ptr(obj, (void *)(intptr_t)value.int64);
   case JIT_VALUE_FOREIGN:
      return llvm_ptr(obj, (void *)(intptr_t)0xdeadbeef);
   default:
      fatal_trace("cannot handle value kind %d", value.kind);
   }
}

static LLVMValueRef cgen_coerce_value(llvm_obj_t *obj, cgen_block_t *cgb,
                                      jit_value_t value, llvm_type_t type)
{
   LLVMValueRef raw = cgen_get_value(obj, cgb, value);
   LLVMTypeRef lltype = LLVMTypeOf(raw);

   switch (type) {
   case LLVM_PTR:
      if (LLVMGetTypeKind(lltype) == LLVMIntegerTypeKind)
         return LLVMBuildIntToPtr(obj->builder, raw, obj->types[LLVM_PTR], "");
      else
         return raw;

   case LLVM_INTPTR:
   case LLVM_INT64:
   case LLVM_INT32:
   case LLVM_INT16:
   case LLVM_INT8:
   case LLVM_INT1:
      switch (LLVMGetTypeKind(lltype)) {
      case LLVMPointerTypeKind:
         return LLVMBuildIntToPtr(obj->builder, raw, obj->types[LLVM_PTR], "");
      case LLVMIntegerTypeKind:
         {
            const int bits1 = LLVMGetIntTypeWidth(lltype);
            const int bits2 = LLVMGetIntTypeWidth(obj->types[type]);

            if (bits2 == 1) {
               LLVMValueRef zero = LLVMConstInt(lltype, 0, false);
               return LLVMBuildICmp(obj->builder, LLVMIntNE, raw, zero, "");
            }
            else if (bits1 < bits2)
               return LLVMBuildSExt(obj->builder, raw, obj->types[type], "");
            else if (bits1 == bits2)
               return raw;
            else
               return LLVMBuildTrunc(obj->builder, raw, obj->types[type], "");
         }
      case LLVMDoubleTypeKind:
         return LLVMBuildBitCast(obj->builder, raw, obj->types[type], "");
      default:
         LLVMDumpType(lltype);
         fatal_trace("cannot coerce type to integer");
      }
      break;

   case LLVM_DOUBLE:
      return LLVMBuildBitCast(obj->builder, raw, obj->types[type], "");

   default:
      return raw;
   }
}

static void cgen_sext_result(llvm_obj_t *obj, cgen_block_t *cgb, jit_ir_t *ir,
                             LLVMValueRef value)
{
   LLVMTypeRef type = LLVMTypeOf(value);
   switch (LLVMGetTypeKind(type)) {
   case LLVMIntegerTypeKind:
      if (LLVMGetIntTypeWidth(type) == 64) {
         DEBUG_ONLY(LLVMSetValueName(value, cgen_reg_name(ir->result)));
         cgb->outregs[ir->result] = value;
      }
      else
         cgb->outregs[ir->result] = LLVMBuildSExt(obj->builder, value,
                                                  obj->types[LLVM_INT64],
                                                  cgen_reg_name(ir->result));
      break;

   case LLVMDoubleTypeKind:
      cgb->outregs[ir->result] = LLVMBuildBitCast(obj->builder, value,
                                                  obj->types[LLVM_INT64],
                                                  cgen_reg_name(ir->result));
      break;

   default:
      LLVMDumpType(type);
      fatal_trace("unhandled LLVM type kind in cgen_sext_result");
   }
}

static void cgen_zext_result(llvm_obj_t *obj, cgen_block_t *cgb, jit_ir_t *ir,
                             LLVMValueRef value)
{
   LLVMTypeRef type = LLVMTypeOf(value);
   switch (LLVMGetTypeKind(type)) {
   case LLVMIntegerTypeKind:
      if (LLVMGetIntTypeWidth(type) == 64) {
         DEBUG_ONLY(LLVMSetValueName(value, cgen_reg_name(ir->result)));
         cgb->outregs[ir->result] = value;
      }
      else
         cgb->outregs[ir->result] = LLVMBuildZExt(obj->builder, value,
                                                  obj->types[LLVM_INT64],
                                                  cgen_reg_name(ir->result));
      break;

   default:
      LLVMDumpType(type);
      fatal_trace("unhandled LLVM type kind in cgen_sext_result");
   }
}

static void cgen_sync_irpos(llvm_obj_t *obj, cgen_block_t *cgb, jit_ir_t *ir)
{
   const unsigned irpos = ir - cgb->func->source->irbuf;
   LLVMValueRef irpos_ptr = LLVMBuildStructGEP2(obj->builder,
                                                obj->types[LLVM_ANCHOR],
                                                cgb->func->anchor, 2, "irpos");
   LLVMBuildStore(obj->builder, llvm_int32(obj, irpos), irpos_ptr);
}

static LLVMBasicBlockRef cgen_add_ctor(llvm_obj_t *obj)
{
   assert(obj->ctor != NULL);
   LLVMBasicBlockRef old_bb = LLVMGetInsertBlock(obj->builder);
   LLVMPositionBuilderAtEnd(obj->builder, LLVMGetLastBasicBlock(obj->ctor));
   return old_bb;
}

static void cgen_op_recv(llvm_obj_t *obj, cgen_block_t *cgb, jit_ir_t *ir)
{
   assert(ir->arg1.kind == JIT_VALUE_INT64);
   const int nth = ir->arg1.int64;

   assert(nth < JIT_MAX_ARGS);
   LLVMValueRef indexes[] = { llvm_int32(obj, nth) };
   LLVMTypeRef int64_type = obj->types[LLVM_INT64];
#ifdef LLVM_HAS_OPAQUE_POINTERS
   LLVMValueRef cast = cgb->func->args;
#else
   LLVMTypeRef ptr_type = LLVMPointerType(int64_type, 0);
   LLVMValueRef cast =
      LLVMBuildPointerCast(obj->builder, cgb->func->args, ptr_type, "");
#endif
   LLVMValueRef ptr = LLVMBuildInBoundsGEP2(obj->builder, int64_type,
                                            cast, indexes,
                                            ARRAY_LEN(indexes),
                                            cgen_arg_name(nth));

   cgb->outregs[ir->result] = LLVMBuildLoad2(obj->builder, int64_type, ptr,
                                             cgen_reg_name(ir->result));
}

static void cgen_op_send(llvm_obj_t *obj, cgen_block_t *cgb, jit_ir_t *ir)
{
   assert(ir->arg1.kind == JIT_VALUE_INT64);
   const int nth = ir->arg1.int64;

   LLVMValueRef value = cgen_get_value(obj, cgb, ir->arg2);

   assert(nth < JIT_MAX_ARGS);
   LLVMValueRef indexes[] = { llvm_int32(obj, nth) };
   LLVMTypeRef int64_type = obj->types[LLVM_INT64];
#ifdef LLVM_HAS_OPAQUE_POINTERS
   LLVMValueRef args_cast = cgb->func->args;
#else
   LLVMTypeRef args_ptr_type = LLVMPointerType(int64_type, 0);
   LLVMValueRef args_cast =
      LLVMBuildPointerCast(obj->builder, cgb->func->args, args_ptr_type, "");
#endif
   LLVMValueRef ptr = LLVMBuildInBoundsGEP2(obj->builder, int64_type,
                                            args_cast, indexes,
                                            ARRAY_LEN(indexes),
                                            cgen_arg_name(nth));

#ifdef LLVM_HAS_OPAQUE_POINTERS
   LLVMBuildStore(obj->builder, value, ptr);
#else
   LLVMTypeRef ptr_type = LLVMPointerType(LLVMTypeOf(value), 0);
   LLVMValueRef cast = LLVMBuildPointerCast(obj->builder, ptr, ptr_type, "");
   LLVMBuildStore(obj->builder, value, cast);
#endif
}

static void cgen_op_store(llvm_obj_t *obj, cgen_block_t *cgb, jit_ir_t *ir)
{
   llvm_type_t type   = LLVM_INT8 + ir->size;
   LLVMValueRef value = cgen_coerce_value(obj, cgb, ir->arg1, type);
   LLVMValueRef ptr   = cgen_coerce_value(obj, cgb, ir->arg2, LLVM_PTR);

#ifdef LLVM_HAS_OPAQUE_POINTERS
   LLVMBuildStore(obj->builder, value, ptr);
#else
   LLVMTypeRef ptr_type = LLVMPointerType(LLVMTypeOf(value), 0);
   LLVMValueRef cast = LLVMBuildPointerCast(obj->builder, ptr, ptr_type, "");
   LLVMBuildStore(obj->builder, value, cast);
#endif
}

static void cgen_op_load(llvm_obj_t *obj, cgen_block_t *cgb, jit_ir_t *ir)
{
   llvm_type_t type = LLVM_INT8 + ir->size;
   LLVMValueRef ptr = cgen_coerce_value(obj, cgb, ir->arg1, LLVM_PTR);

#ifndef LLVM_HAS_OPAQUE_POINTERS
   LLVMTypeRef ptr_type = LLVMPointerType(obj->types[type], 0);
   ptr = LLVMBuildPointerCast(obj->builder, ptr, ptr_type, "");
#endif

   if (type == LLVM_INT64)
      cgb->outregs[ir->result] = LLVMBuildLoad2(obj->builder, obj->types[type],
                                                ptr, cgen_reg_name(ir->result));
   else {
      LLVMValueRef tmp =
         LLVMBuildLoad2(obj->builder, obj->types[type], ptr, "");
      if (ir->op == J_ULOAD)
         cgen_zext_result(obj, cgb, ir, tmp);
      else
         cgen_sext_result(obj, cgb, ir, tmp);
   }
}

static void cgen_op_add(llvm_obj_t *obj, cgen_block_t *cgb, jit_ir_t *ir)
{
   llvm_fn_t fn = LLVM_LAST_FN;
   if (ir->cc == JIT_CC_O)
      fn = LLVM_ADD_OVERFLOW_S8 + ir->size;
   else if (ir->cc == JIT_CC_C)
      fn = LLVM_ADD_OVERFLOW_U8 + ir->size;

   if (fn != LLVM_LAST_FN) {
      llvm_type_t type = LLVM_INT8 + ir->size;
      LLVMValueRef arg1 = cgen_coerce_value(obj, cgb, ir->arg1, type);
      LLVMValueRef arg2 = cgen_coerce_value(obj, cgb, ir->arg2, type);

      LLVMValueRef args[] = { arg1, arg2 };
      LLVMValueRef pair = llvm_call_fn(obj, fn, args, 2);

      LLVMValueRef result = LLVMBuildExtractValue(obj->builder, pair, 0, "");
      cgb->outflags = LLVMBuildExtractValue(obj->builder, pair, 1, "FLAGS");

      if (ir->cc == JIT_CC_C)
         cgen_zext_result(obj, cgb, ir, result);
      else
         cgen_sext_result(obj, cgb, ir, result);
   }
   else {
      LLVMValueRef arg1 = cgen_get_value(obj, cgb, ir->arg1);
      LLVMValueRef arg2 = cgen_get_value(obj, cgb, ir->arg2);
      cgb->outregs[ir->result] = LLVMBuildAdd(obj->builder, arg1, arg2,
                                              cgen_reg_name(ir->result));
   }
}

static void cgen_op_sub(llvm_obj_t *obj, cgen_block_t *cgb, jit_ir_t *ir)
{
   llvm_fn_t fn = LLVM_LAST_FN;
   if (ir->cc == JIT_CC_O)
      fn = LLVM_SUB_OVERFLOW_S8 + ir->size;
   else if (ir->cc == JIT_CC_C)
      fn = LLVM_SUB_OVERFLOW_U8 + ir->size;

   if (fn != LLVM_LAST_FN) {
      llvm_type_t type = LLVM_INT8 + ir->size;
      LLVMValueRef arg1 = cgen_coerce_value(obj, cgb, ir->arg1, type);
      LLVMValueRef arg2 = cgen_coerce_value(obj, cgb, ir->arg2, type);

      LLVMValueRef args[] = { arg1, arg2 };
      LLVMValueRef pair = llvm_call_fn(obj, fn, args, 2);

      LLVMValueRef result = LLVMBuildExtractValue(obj->builder, pair, 0, "");
      cgb->outflags = LLVMBuildExtractValue(obj->builder, pair, 1, "FLAGS");

      if (ir->cc == JIT_CC_C)
         cgen_zext_result(obj, cgb, ir, result);
      else
         cgen_sext_result(obj, cgb, ir, result);
   }
   else {
      LLVMValueRef arg1 = cgen_get_value(obj, cgb, ir->arg1);
      LLVMValueRef arg2 = cgen_get_value(obj, cgb, ir->arg2);
      cgb->outregs[ir->result] = LLVMBuildSub(obj->builder, arg1, arg2,
                                              cgen_reg_name(ir->result));
   }
}

static void cgen_op_mul(llvm_obj_t *obj, cgen_block_t *cgb, jit_ir_t *ir)
{
   llvm_fn_t fn = LLVM_LAST_FN;
   if (ir->cc == JIT_CC_O)
      fn = LLVM_MUL_OVERFLOW_S8 + ir->size;
   else if (ir->cc == JIT_CC_C)
      fn = LLVM_MUL_OVERFLOW_U8 + ir->size;

   if (fn != LLVM_LAST_FN) {
      llvm_type_t type = LLVM_INT8 + ir->size;
      LLVMValueRef arg1 = cgen_coerce_value(obj, cgb, ir->arg1, type);
      LLVMValueRef arg2 = cgen_coerce_value(obj, cgb, ir->arg2, type);

      LLVMValueRef args[] = { arg1, arg2 };
      LLVMValueRef pair = llvm_call_fn(obj, fn, args, 2);

      LLVMValueRef result = LLVMBuildExtractValue(obj->builder, pair, 0, "");
      cgb->outflags = LLVMBuildExtractValue(obj->builder, pair, 1, "FLAGS");

      if (ir->cc == JIT_CC_C)
         cgen_zext_result(obj, cgb, ir, result);
      else
         cgen_sext_result(obj, cgb, ir, result);
   }
   else {
      LLVMValueRef arg1 = cgen_get_value(obj, cgb, ir->arg1);
      LLVMValueRef arg2 = cgen_get_value(obj, cgb, ir->arg2);
      cgb->outregs[ir->result] = LLVMBuildMul(obj->builder, arg1, arg2,
                                              cgen_reg_name(ir->result));
   }
}

static void cgen_op_div(llvm_obj_t *obj, cgen_block_t *cgb, jit_ir_t *ir)
{
   LLVMValueRef arg1 = cgen_get_value(obj, cgb, ir->arg1);
   LLVMValueRef arg2 = cgen_get_value(obj, cgb, ir->arg2);

   cgb->outregs[ir->result] = LLVMBuildSDiv(obj->builder, arg1, arg2,
                                            cgen_reg_name(ir->result));
}

static void cgen_op_rem(llvm_obj_t *obj, cgen_block_t *cgb, jit_ir_t *ir)
{
   LLVMValueRef arg1 = cgen_get_value(obj, cgb, ir->arg1);
   LLVMValueRef arg2 = cgen_get_value(obj, cgb, ir->arg2);

   cgb->outregs[ir->result] = LLVMBuildSRem(obj->builder, arg1, arg2,
                                            cgen_reg_name(ir->result));
}

static void cgen_op_fadd(llvm_obj_t *obj, cgen_block_t *cgb, jit_ir_t *ir)
{
   LLVMValueRef arg1 = cgen_coerce_value(obj, cgb, ir->arg1, LLVM_DOUBLE);
   LLVMValueRef arg2 = cgen_coerce_value(obj, cgb, ir->arg2, LLVM_DOUBLE);

   LLVMValueRef real = LLVMBuildFAdd(obj->builder, arg1, arg2, "");
   cgen_sext_result(obj, cgb, ir, real);
}

static void cgen_op_fsub(llvm_obj_t *obj, cgen_block_t *cgb, jit_ir_t *ir)
{
   LLVMValueRef arg1 = cgen_coerce_value(obj, cgb, ir->arg1, LLVM_DOUBLE);
   LLVMValueRef arg2 = cgen_coerce_value(obj, cgb, ir->arg2, LLVM_DOUBLE);

   LLVMValueRef real = LLVMBuildFSub(obj->builder, arg1, arg2, "");
   cgen_sext_result(obj, cgb, ir, real);
}

static void cgen_op_fmul(llvm_obj_t *obj, cgen_block_t *cgb, jit_ir_t *ir)
{
   LLVMValueRef arg1 = cgen_coerce_value(obj, cgb, ir->arg1, LLVM_DOUBLE);
   LLVMValueRef arg2 = cgen_coerce_value(obj, cgb, ir->arg2, LLVM_DOUBLE);

   LLVMValueRef real = LLVMBuildFMul(obj->builder, arg1, arg2, "");
   cgen_sext_result(obj, cgb, ir, real);
}

static void cgen_op_fdiv(llvm_obj_t *obj, cgen_block_t *cgb, jit_ir_t *ir)
{
   LLVMValueRef arg1 = cgen_coerce_value(obj, cgb, ir->arg1, LLVM_DOUBLE);
   LLVMValueRef arg2 = cgen_coerce_value(obj, cgb, ir->arg2, LLVM_DOUBLE);

   LLVMValueRef real = LLVMBuildFDiv(obj->builder, arg1, arg2, "");
   cgen_sext_result(obj, cgb, ir, real);
}

static void cgen_op_fneg(llvm_obj_t *obj, cgen_block_t *cgb, jit_ir_t *ir)
{
   LLVMValueRef arg1 = cgen_coerce_value(obj, cgb, ir->arg1, LLVM_DOUBLE);

   LLVMValueRef real = LLVMBuildFNeg(obj->builder, arg1, "");
   cgen_sext_result(obj, cgb, ir, real);
}

static void cgen_op_fcvtns(llvm_obj_t *obj, cgen_block_t *cgb, jit_ir_t *ir)
{
   LLVMValueRef arg1 = cgen_coerce_value(obj, cgb, ir->arg1, LLVM_DOUBLE);

   LLVMValueRef args[] = { arg1 };
   LLVMValueRef rounded = llvm_call_fn(obj, LLVM_ROUND_F64, args, 1);

   cgb->outregs[ir->result] = LLVMBuildFPToSI(obj->builder, rounded,
                                              obj->types[LLVM_INT64],
                                              cgen_reg_name(ir->result));
}

static void cgen_op_scvtf(llvm_obj_t *obj, cgen_block_t *cgb, jit_ir_t *ir)
{
   LLVMValueRef arg1 = cgen_get_value(obj, cgb, ir->arg1);

   LLVMValueRef real = LLVMBuildSIToFP(obj->builder, arg1,
                                       obj->types[LLVM_DOUBLE], "");
   cgen_sext_result(obj, cgb, ir, real);
}

static void cgen_op_not(llvm_obj_t *obj, cgen_block_t *cgb, jit_ir_t *ir)
{
   LLVMValueRef arg1 = cgen_coerce_value(obj, cgb, ir->arg1, LLVM_INT1);
   LLVMValueRef logical = LLVMBuildNot(obj->builder, arg1, "");
   cgen_zext_result(obj, cgb, ir, logical);
}

static void cgen_op_and(llvm_obj_t *obj, cgen_block_t *cgb, jit_ir_t *ir)
{
   LLVMValueRef arg1 = cgen_coerce_value(obj, cgb, ir->arg1, LLVM_INT1);
   LLVMValueRef arg2 = cgen_coerce_value(obj, cgb, ir->arg2, LLVM_INT1);

   LLVMValueRef logical = LLVMBuildAnd(obj->builder, arg1, arg2, "");
   cgen_zext_result(obj, cgb, ir, logical);
}

static void cgen_op_or(llvm_obj_t *obj, cgen_block_t *cgb, jit_ir_t *ir)
{
   LLVMValueRef arg1 = cgen_coerce_value(obj, cgb, ir->arg1, LLVM_INT1);
   LLVMValueRef arg2 = cgen_coerce_value(obj, cgb, ir->arg2, LLVM_INT1);

   LLVMValueRef logical = LLVMBuildOr(obj->builder, arg1, arg2, "");
   cgen_zext_result(obj, cgb, ir, logical);
}

static void cgen_op_xor(llvm_obj_t *obj, cgen_block_t *cgb, jit_ir_t *ir)
{
   LLVMValueRef arg1 = cgen_coerce_value(obj, cgb, ir->arg1, LLVM_INT1);
   LLVMValueRef arg2 = cgen_coerce_value(obj, cgb, ir->arg2, LLVM_INT1);

   LLVMValueRef logical = LLVMBuildXor(obj->builder, arg1, arg2, "");
   cgen_zext_result(obj, cgb, ir, logical);
}

static void cgen_op_ret(llvm_obj_t *obj, jit_ir_t *ir)
{
   LLVMBuildRetVoid(obj->builder);
}

static void cgen_op_jump(llvm_obj_t *obj, cgen_block_t *cgb, jit_ir_t *ir)
{
   if (ir->cc == JIT_CC_NONE) {
      assert(cgb->source->out.count == 1);
      LLVMBasicBlockRef dest =
         cgb->func->blocks[jit_get_edge(&(cgb->source->out), 0)].bbref;
      LLVMBuildBr(obj->builder, dest);
   }
   else if (ir->cc == JIT_CC_T) {
      assert(cgb->source->out.count == 2);
      LLVMBasicBlockRef dest_t =
         cgb->func->blocks[jit_get_edge(&(cgb->source->out), 1)].bbref;
      LLVMBasicBlockRef dest_f = (cgb + 1)->bbref;
      LLVMBuildCondBr(obj->builder, cgb->outflags, dest_t, dest_f);
   }
   else if (ir->cc == JIT_CC_F) {
      assert(cgb->source->out.count == 2);
      LLVMBasicBlockRef dest_t =
         cgb->func->blocks[jit_get_edge(&(cgb->source->out), 1)].bbref;
      LLVMBasicBlockRef dest_f = (cgb + 1)->bbref;
      LLVMBuildCondBr(obj->builder, cgb->outflags, dest_f, dest_t);
   }
   else
      cgen_abort(cgb, ir, "unhandled jump condition code");
}

static void cgen_op_cmp(llvm_obj_t *obj, cgen_block_t *cgb, jit_ir_t *ir)
{
   LLVMValueRef arg1 = cgen_get_value(obj, cgb, ir->arg1);
   LLVMValueRef arg2 = cgen_get_value(obj, cgb, ir->arg2);

   LLVMIntPredicate pred;
   switch (ir->cc) {
   case JIT_CC_EQ: pred = LLVMIntEQ; break;
   case JIT_CC_NE: pred = LLVMIntNE; break;
   case JIT_CC_GT: pred = LLVMIntSGT; break;
   case JIT_CC_LT: pred = LLVMIntSLT; break;
   case JIT_CC_LE: pred = LLVMIntSLE; break;
   case JIT_CC_GE: pred = LLVMIntSGE; break;
   default:
      cgen_abort(cgb, ir, "unhandled cmp condition code");
   }

   cgb->outflags = LLVMBuildICmp(obj->builder, pred, arg1, arg2, "FLAGS");
}

static void cgen_op_fcmp(llvm_obj_t *obj, cgen_block_t *cgb, jit_ir_t *ir)
{
   LLVMValueRef arg1 = cgen_coerce_value(obj, cgb, ir->arg1, LLVM_DOUBLE);
   LLVMValueRef arg2 = cgen_coerce_value(obj, cgb, ir->arg2, LLVM_DOUBLE);

   LLVMRealPredicate pred;
   switch (ir->cc) {
   case JIT_CC_EQ: pred = LLVMRealUEQ; break;
   case JIT_CC_NE: pred = LLVMRealUNE; break;
   case JIT_CC_GT: pred = LLVMRealUGT; break;
   case JIT_CC_LT: pred = LLVMRealULT; break;
   case JIT_CC_LE: pred = LLVMRealULE; break;
   case JIT_CC_GE: pred = LLVMRealUGE; break;
   default:
      cgen_abort(cgb, ir, "unhandled fcmp condition code");
   }

   cgb->outflags = LLVMBuildFCmp(obj->builder, pred, arg1, arg2, "FLAGS");
}

static void cgen_op_cset(llvm_obj_t *obj, cgen_block_t *cgb, jit_ir_t *ir)
{
   cgen_zext_result(obj, cgb, ir, cgb->outflags);
}

static void cgen_op_csel(llvm_obj_t *obj, cgen_block_t *cgb, jit_ir_t *ir)
{
   LLVMValueRef arg1 = cgen_get_value(obj, cgb, ir->arg1);
   LLVMValueRef arg2 = cgen_get_value(obj, cgb, ir->arg2);

   LLVMValueRef result =
      LLVMBuildSelect(obj->builder, cgb->outflags, arg1, arg2, "");

   cgen_sext_result(obj, cgb, ir, result);
}

static void cgen_op_call(llvm_obj_t *obj, cgen_block_t *cgb, jit_ir_t *ir)
{
   cgen_sync_irpos(obj, cgb, ir);

   jit_func_t *callee = jit_get_func(cgb->func->source->jit, ir->arg1.handle);

   LLVMValueRef entry = NULL, fptr = NULL;
   if (obj->ctor != NULL) {
      entry = llvm_get_fn(obj, LLVM_TRAMPOLINE);

      LOCAL_TEXT_BUF tb = tb_new();
      tb_istr(tb, callee->name);
      tb_cat(tb, ".func");

      LLVMValueRef global = LLVMGetNamedGlobal(obj->module, tb_get(tb));
      if (global == NULL) {
         global = LLVMAddGlobal(obj->module, obj->types[LLVM_PTR], tb_get(tb));
         LLVMSetUnnamedAddr(global, true);
         LLVMSetLinkage(global, LLVMPrivateLinkage);
         LLVMSetInitializer(global, llvm_ptr(obj, NULL));

         LLVMBasicBlockRef old_bb = cgen_add_ctor(obj);

         tb_trim(tb, ident_len(callee->name));  // Strip .func

         LLVMValueRef args[] = {
            llvm_const_string(obj, tb_get(tb)),
         };
         LLVMValueRef init =
            llvm_call_fn(obj, LLVM_GET_FUNC, args, ARRAY_LEN(args));
         LLVMBuildStore(obj->builder, init, global);

         LLVMPositionBuilderAtEnd(obj->builder, old_bb);
      }

      fptr = LLVMBuildLoad2(obj->builder, obj->types[LLVM_PTR], global, "");
   }
   else {
      entry = llvm_ptr(obj, callee->entry);
      fptr = llvm_ptr(obj, callee);

#ifndef LLVM_HAS_OPAQUE_POINTERS
      LLVMTypeRef ptr_type = LLVMPointerType(obj->types[LLVM_ENTRY_FN], 0);
      entry = LLVMBuildPointerCast(obj->builder, entry, ptr_type, "");
#endif
   }

   LLVMValueRef args[] = {
      fptr,
      PTR(cgb->func->anchor),
      cgb->func->args
   };
   LLVMBuildCall2(obj->builder, obj->types[LLVM_ENTRY_FN], entry,
                  args, ARRAY_LEN(args), "");
}

static void cgen_op_lea(llvm_obj_t *obj, cgen_block_t *cgb, jit_ir_t *ir)
{
   LLVMValueRef ptr = cgen_get_value(obj, cgb, ir->arg1);

   if (LLVMGetTypeKind(LLVMTypeOf(ptr)) == LLVMPointerTypeKind)
      cgb->outregs[ir->result] = LLVMBuildPtrToInt(obj->builder, ptr,
                                                   obj->types[LLVM_INT64],
                                                   cgen_reg_name(ir->result));
   else
      cgen_zext_result(obj, cgb, ir, ptr);
}

static void cgen_op_mov(llvm_obj_t *obj, cgen_block_t *cgb, jit_ir_t *ir)
{
   LLVMValueRef value = cgen_get_value(obj, cgb, ir->arg1);
   cgen_sext_result(obj, cgb, ir, value);
}

static void cgen_op_neg(llvm_obj_t *obj, cgen_block_t *cgb, jit_ir_t *ir)
{
   LLVMValueRef arg1 = cgen_get_value(obj, cgb, ir->arg1);
   LLVMValueRef neg =
      LLVMBuildNeg(obj->builder, arg1, cgen_reg_name(ir->result));

   cgb->outregs[ir->result] = neg;
}

static void cgen_macro_exp(llvm_obj_t *obj, cgen_block_t *cgb, jit_ir_t *ir)
{
   LLVMValueRef arg1 = cgen_get_value(obj, cgb, ir->arg1);
   LLVMValueRef arg2 = cgen_get_value(obj, cgb, ir->arg2);

   // TODO: implement this without the cast
   LLVMValueRef cast[] = {
      LLVMBuildUIToFP(obj->builder, arg1, obj->types[LLVM_DOUBLE], ""),
      LLVMBuildUIToFP(obj->builder, arg2, obj->types[LLVM_DOUBLE], "")
   };
   LLVMValueRef real = llvm_call_fn(obj, LLVM_POW_F64, cast, 2);

   cgb->outregs[ir->result] = LLVMBuildFPToUI(
      obj->builder, real, obj->types[LLVM_INT64], cgen_reg_name(ir->result));
}

static void cgen_macro_fexp(llvm_obj_t *obj, cgen_block_t *cgb, jit_ir_t *ir)
{
   LLVMValueRef arg1 = cgen_coerce_value(obj, cgb, ir->arg1, LLVM_DOUBLE);
   LLVMValueRef arg2 = cgen_coerce_value(obj, cgb, ir->arg2, LLVM_DOUBLE);

   LLVMValueRef args[] = { arg1, arg2 };
   LLVMValueRef real = llvm_call_fn(obj, LLVM_POW_F64, args, 2);

   cgen_sext_result(obj, cgb, ir, real);
}

static void cgen_macro_copy(llvm_obj_t *obj, cgen_block_t *cgb, jit_ir_t *ir)
{
   LLVMValueRef count = cgb->outregs[ir->result];
   LLVMValueRef dest  = cgen_coerce_value(obj, cgb, ir->arg1, LLVM_PTR);
   LLVMValueRef src   = cgen_coerce_value(obj, cgb, ir->arg2, LLVM_PTR);

   LLVMBuildMemMove(obj->builder, dest, 0, src, 0, count);
}

static void cgen_macro_bzero(llvm_obj_t *obj, cgen_block_t *cgb, jit_ir_t *ir)
{
   LLVMValueRef count = cgb->outregs[ir->result];
   LLVMValueRef dest  = cgen_coerce_value(obj, cgb, ir->arg1, LLVM_PTR);

   LLVMBuildMemSet(obj->builder, PTR(dest), llvm_int8(obj, 0), count, 0);
}

static void cgen_macro_exit(llvm_obj_t *obj, cgen_block_t *cgb, jit_ir_t *ir)
{
   cgen_sync_irpos(obj, cgb, ir);

   LLVMValueRef which = cgen_get_value(obj, cgb, ir->arg1);

   LLVMValueRef args[] = {
      which,
      PTR(cgb->func->anchor),
      cgb->func->args
   };
   llvm_call_fn(obj, LLVM_DO_EXIT, args, ARRAY_LEN(args));
}

static void cgen_macro_fficall(llvm_obj_t *obj, cgen_block_t *cgb, jit_ir_t *ir)
{
   cgen_sync_irpos(obj, cgb, ir);

   LLVMValueRef ffptr;
   if (obj->ctor != NULL) {
      assert(ir->arg1.kind == JIT_VALUE_FOREIGN);
      ident_t sym = ffi_get_sym(ir->arg1.foreign);

      LOCAL_TEXT_BUF tb = tb_new();
      tb_istr(tb, sym);
      tb_cat(tb, ".ffi");

      LLVMValueRef global = LLVMGetNamedGlobal(obj->module, tb_get(tb));
      if (global == NULL) {
         global = LLVMAddGlobal(obj->module, obj->types[LLVM_PTR], tb_get(tb));
         LLVMSetUnnamedAddr(global, true);
         LLVMSetLinkage(global, LLVMPrivateLinkage);
         LLVMSetInitializer(global, llvm_ptr(obj, NULL));

         LLVMBasicBlockRef old_bb = cgen_add_ctor(obj);

         tb_trim(tb, ident_len(sym));   // Strip .ffi

         LLVMValueRef args[] = {
            llvm_const_string(obj, tb_get(tb)),
            llvm_int64(obj, ffi_get_spec(ir->arg1.foreign)),
         };
         LLVMValueRef init =
            llvm_call_fn(obj, LLVM_GET_FOREIGN, args, ARRAY_LEN(args));
         LLVMBuildStore(obj->builder, init, global);

         LLVMPositionBuilderAtEnd(obj->builder, old_bb);
      }

      ffptr = LLVMBuildLoad2(obj->builder, obj->types[LLVM_PTR], global, "");
   }
   else
      ffptr = cgen_get_value(obj, cgb, ir->arg1);

   LLVMValueRef args[] = {
      ffptr,
      PTR(cgb->func->anchor),
      cgb->func->args
   };
   llvm_call_fn(obj, LLVM_DO_FFICALL, args, ARRAY_LEN(args));
}

static void cgen_macro_galloc(llvm_obj_t *obj, cgen_block_t *cgb, jit_ir_t *ir)
{
   // TODO: use TLAB

   cgen_sync_irpos(obj, cgb, ir);

   LLVMValueRef size = cgen_get_value(obj, cgb, ir->arg1);

   LLVMValueRef args[] = {
      LLVMBuildTrunc(obj->builder, size, obj->types[LLVM_INT32], ""),
      llvm_int32(obj, 1),
   };
   LLVMValueRef ptr = llvm_call_fn(obj, LLVM_MSPACE_ALLOC, args,
                                   ARRAY_LEN(args));

   cgb->outregs[ir->result] = LLVMBuildPtrToInt(obj->builder, ptr,
                                                obj->types[LLVM_INT64],
                                                cgen_reg_name(ir->result));
}

static void cgen_macro_getpriv(llvm_obj_t *obj, cgen_block_t *cgb, jit_ir_t *ir)
{
   // TODO: this needs some kind of fast-path

   LLVMValueRef args[] = {
      cgen_get_value(obj, cgb, ir->arg1)
   };
   LLVMValueRef ptr = llvm_call_fn(obj, LLVM_GETPRIV, args, ARRAY_LEN(args));

   cgb->outregs[ir->result] = LLVMBuildPtrToInt(obj->builder, ptr,
                                                obj->types[LLVM_INT64],
                                                cgen_reg_name(ir->result));
}

static void cgen_macro_putpriv(llvm_obj_t *obj, cgen_block_t *cgb, jit_ir_t *ir)
{
   LLVMValueRef args[] = {
      cgen_get_value(obj, cgb, ir->arg1),
      cgen_coerce_value(obj, cgb, ir->arg2, LLVM_PTR),
   };
   llvm_call_fn(obj, LLVM_PUTPRIV, args, ARRAY_LEN(args));
}

static void cgen_ir(llvm_obj_t *obj, cgen_block_t *cgb, jit_ir_t *ir)
{
   switch (ir->op) {
   case J_RECV:
      cgen_op_recv(obj, cgb, ir);
      break;
   case J_SEND:
      cgen_op_send(obj, cgb, ir);
      break;
   case J_STORE:
      cgen_op_store(obj, cgb, ir);
      break;
   case J_LOAD:
   case J_ULOAD:
      cgen_op_load(obj, cgb, ir);
      break;
   case J_ADD:
      cgen_op_add(obj, cgb, ir);
      break;
   case J_SUB:
      cgen_op_sub(obj, cgb, ir);
      break;
   case J_MUL:
      cgen_op_mul(obj, cgb, ir);
      break;
   case J_DIV:
      cgen_op_div(obj, cgb, ir);
      break;
   case J_REM:
      cgen_op_rem(obj, cgb, ir);
      break;
   case J_FADD:
      cgen_op_fadd(obj, cgb, ir);
      break;
   case J_FSUB:
      cgen_op_fsub(obj, cgb, ir);
      break;
   case J_FMUL:
      cgen_op_fmul(obj, cgb, ir);
      break;
   case J_FDIV:
      cgen_op_fdiv(obj, cgb, ir);
      break;
   case J_FNEG:
      cgen_op_fneg(obj, cgb, ir);
      break;
   case J_FCVTNS:
      cgen_op_fcvtns(obj, cgb, ir);
      break;
   case J_SCVTF:
      cgen_op_scvtf(obj, cgb, ir);
      break;
   case J_NOT:
      cgen_op_not(obj, cgb, ir);
      break;
   case J_AND:
      cgen_op_and(obj, cgb, ir);
      break;
   case J_OR:
      cgen_op_or(obj, cgb, ir);
      break;
   case J_XOR:
      cgen_op_xor(obj, cgb, ir);
      break;
   case J_RET:
      cgen_op_ret(obj, ir);
      break;
   case J_JUMP:
      cgen_op_jump(obj, cgb, ir);
      break;
   case J_CMP:
      cgen_op_cmp(obj, cgb, ir);
      break;
   case J_FCMP:
      cgen_op_fcmp(obj, cgb, ir);
      break;
   case J_CSET:
      cgen_op_cset(obj, cgb, ir);
      break;
   case J_CSEL:
      cgen_op_csel(obj, cgb, ir);
      break;
   case J_DEBUG:
      break;
   case J_CALL:
      cgen_op_call(obj, cgb, ir);
      break;
   case J_LEA:
      cgen_op_lea(obj, cgb, ir);
      break;
   case J_MOV:
      cgen_op_mov(obj, cgb, ir);
      break;
   case J_NEG:
      cgen_op_neg(obj, cgb, ir);
      break;
   case MACRO_EXP:
      cgen_macro_exp(obj, cgb, ir);
      break;
   case MACRO_FEXP:
      cgen_macro_fexp(obj, cgb, ir);
      break;
   case MACRO_COPY:
      cgen_macro_copy(obj, cgb, ir);
      break;
   case MACRO_BZERO:
      cgen_macro_bzero(obj, cgb, ir);
      break;
   case MACRO_EXIT:
      cgen_macro_exit(obj, cgb, ir);
      break;
   case MACRO_FFICALL:
      cgen_macro_fficall(obj, cgb, ir);
      break;
   case MACRO_GALLOC:
      cgen_macro_galloc(obj, cgb, ir);
      break;
   case MACRO_GETPRIV:
      cgen_macro_getpriv(obj, cgb, ir);
      break;
   case MACRO_PUTPRIV:
      cgen_macro_putpriv(obj, cgb, ir);
      break;
   default:
      cgen_abort(cgb, ir, "cannot generate LLVM for %s", jit_op_name(ir->op));
   }
}

static void cgen_basic_blocks(llvm_obj_t *obj, cgen_func_t *func,
                              jit_cfg_t *cfg)
{
   func->blocks = xcalloc_array(cfg->nblocks, sizeof(cgen_block_t));

   for (int i = 0; i < cfg->nblocks; i++) {
#ifdef DEBUG
      char name[32];
      checked_sprintf(name, sizeof(name), "BB%d", i);
#else
      const char *name = "";
#endif

      cgen_block_t *cgb = &(func->blocks[i]);
      cgb->bbref  = llvm_append_block(obj, func->llvmfn, name);
      cgb->source = &(cfg->blocks[i]);
      cgb->func   = func;

      cgb->inregs  = xcalloc_array(func->source->nregs, sizeof(LLVMValueRef));
      cgb->outregs = xcalloc_array(func->source->nregs, sizeof(LLVMValueRef));
   }
}

static void cgen_frame_anchor(llvm_obj_t *obj, cgen_func_t *func)
{
   LLVMTypeRef type = obj->types[LLVM_ANCHOR];
   func->anchor = LLVMBuildAlloca(obj->builder, type, "anchor");

   LLVMValueRef func_arg = LLVMGetParam(func->llvmfn, 0);
   LLVMSetValueName(func_arg, "func");

   LLVMValueRef caller_arg = LLVMGetParam(func->llvmfn, 1);
   LLVMSetValueName(caller_arg, "caller");

   LLVMValueRef caller_ptr = LLVMBuildStructGEP2(obj->builder, type,
                                                 func->anchor, 0, "");
   LLVMBuildStore(obj->builder, caller_arg, caller_ptr);

   LLVMValueRef func_ptr = LLVMBuildStructGEP2(obj->builder, type,
                                               func->anchor, 1, "");
   LLVMBuildStore(obj->builder, func_arg, func_ptr);

   LLVMValueRef irpos_ptr = LLVMBuildStructGEP2(obj->builder, type,
                                                func->anchor, 2, "");
   LLVMBuildStore(obj->builder, llvm_int32(obj, 0), irpos_ptr);
}

static void cgen_aot_cpool(llvm_obj_t *obj, cgen_func_t *func)
{
   jit_func_t *f = func->source;

   LOCAL_TEXT_BUF tb = tb_new();
   tb_istr(tb, f->name);
   tb_cat(tb, ".cpool");

   LLVMTypeRef array_type = LLVMArrayType(obj->types[LLVM_INT8], f->cpoolsz);

   LLVMValueRef global = LLVMAddGlobal(obj->module, array_type, tb_get(tb));
   LLVMSetLinkage(global, LLVMPrivateLinkage);
   LLVMSetGlobalConstant(global, true);
   LLVMSetUnnamedAddr(global, true);

   LLVMValueRef *data LOCAL = xmalloc_array(f->cpoolsz, sizeof(LLVMValueRef));
   for (int i = 0; i < f->cpoolsz; i++)
      data[i] = llvm_int8(obj, f->cpool[i]);

   LLVMValueRef init =
      LLVMConstArray(obj->types[LLVM_INT8], data, f->cpoolsz);
   LLVMSetInitializer(global, init);

   func->cpool = global;
}

static LLVMValueRef cgen_debug_irbuf(llvm_obj_t *obj, jit_func_t *f)
{
   LOCAL_TEXT_BUF tb = tb_new();
   tb_istr(tb, f->name);
   tb_cat(tb, ".debug");

   int run = 0, lineno = 0;
   const char *file = NULL;

   SCOPED_A(LLVMValueRef) enc = AINIT;
   ARESERVE(enc, MIN(f->nirs + 100, 1024));

   for (int i = 0; i < f->nirs; i++) {
      jit_ir_t *ir = &(f->irbuf[i]);
      if ((ir->target || ir->op == J_DEBUG) && run > 0) {
         if (run < 16)
            APUSH(enc, llvm_int8(obj, (DC_TRAP << 4) | run));
         else {
            APUSH(enc, llvm_int8(obj, DC_LONG_TRAP << 4));
            APUSH(enc, llvm_int8(obj, run & 0xff));
            APUSH(enc, llvm_int8(obj, (run >> 8) & 0xff));
         }
         run = 0;
      }

      if (ir->target)
         APUSH(enc, llvm_int8(obj, DC_TARGET << 4));

      if (ir->op == J_DEBUG) {
         if (file == NULL) {
            file = loc_file_str(&ir->arg1.loc);
            lineno = 0;
            const int len2 = ilog2(strlen(file) + 1);
            assert(len2 < 16);
            APUSH(enc, llvm_int8(obj, (DC_FILE << 4) | len2));

            const char *p = file;
            do {
               APUSH(enc, llvm_int8(obj, *p));
            } while (*p++);
         }

         const int delta = ir->arg1.loc.first_line - lineno;
         if (delta >= 0 && delta < 16)
            APUSH(enc, llvm_int8(obj, (DC_LOCINFO << 4) | delta));
         else {
            APUSH(enc, llvm_int8(obj, DC_LONG_LOCINFO << 4));
            APUSH(enc, llvm_int8(obj, ir->arg1.loc.first_line & 0xff));
            APUSH(enc, llvm_int8(obj, (ir->arg1.loc.first_line >> 8) & 0xff));
         }
      }
      else
         run++;
   }

   if (run > 0 && run < 16)
      APUSH(enc, llvm_int8(obj, (DC_TRAP << 4) | run));
   else if (run > 0) {
      APUSH(enc, llvm_int8(obj, DC_LONG_TRAP << 4));
      APUSH(enc, llvm_int8(obj, run & 0xff));
      APUSH(enc, llvm_int8(obj, (run >> 8) & 0xff));
   }

   APUSH(enc, llvm_int8(obj, DC_STOP << 4));

   LLVMTypeRef array_type = LLVMArrayType(obj->types[LLVM_INT8], enc.count);

   LLVMValueRef global = LLVMAddGlobal(obj->module, array_type, tb_get(tb));
   LLVMSetLinkage(global, LLVMPrivateLinkage);
   LLVMSetGlobalConstant(global, true);
   LLVMSetUnnamedAddr(global, true);

   LLVMValueRef init =
      LLVMConstArray(obj->types[LLVM_INT8], enc.items, enc.count);
   LLVMSetInitializer(global, init);

   return global;
}

static void cgen_function(llvm_obj_t *obj, cgen_func_t *func)
{
   func->llvmfn = LLVMAddFunction(obj->module, func->name,
                                  obj->types[LLVM_ENTRY_FN]);

   if (obj->ctor != NULL) {
      cgen_add_ctor(obj);
      cgen_aot_cpool(obj, func);

      LLVMValueRef args[] = {
         llvm_const_string(obj, func->name),
         PTR(func->llvmfn),
         PTR(cgen_debug_irbuf(obj, func->source)),
         llvm_int32(obj, func->source->nirs),
      };
      llvm_call_fn(obj, LLVM_REGISTER, args, ARRAY_LEN(args));

      LLVMSetLinkage(func->llvmfn, LLVMPrivateLinkage);
   }

   LLVMBasicBlockRef entry_bb = llvm_append_block(obj, func->llvmfn, "entry");
   LLVMPositionBuilderAtEnd(obj->builder, entry_bb);

   cgen_frame_anchor(obj, func);

   func->args = LLVMGetParam(func->llvmfn, 2);
   LLVMSetValueName(func->args, "args");

   if (func->source->framesz > 0) {
      LLVMTypeRef frame_type =
         LLVMArrayType(obj->types[LLVM_INT8], func->source->framesz);
      func->frame = LLVMBuildAlloca(obj->builder, frame_type, "frame");
      LLVMSetAlignment(func->frame, sizeof(double));
   }

   jit_cfg_t *cfg = func->cfg = jit_get_cfg(func->source);
   cgen_basic_blocks(obj, func, cfg);

   cgen_block_t *cgb = func->blocks;

   int maxin = 0;
   for (int i = 0; i < func->source->nirs; i++) {
      if (i == cgb->source->first) {
         LLVMPositionBuilderAtEnd(obj->builder, cgb->bbref);

         LLVMTypeRef int1_type = obj->types[LLVM_INT1];
         cgb->inflags = LLVMBuildPhi(obj->builder, int1_type, "FLAGS");
         cgb->outflags = cgb->inflags;

         for (int j = 0; j < func->source->nregs; j++) {
            if (mask_test(&cgb->source->livein, j)) {
               const char *name = cgen_reg_name(j);
               LLVMValueRef init = i == 0   // Entry block
                  ? LLVMConstNull(obj->types[LLVM_INT64])
                  : LLVMBuildPhi(obj->builder, obj->types[LLVM_INT64], name);
               cgb->inregs[j] = cgb->outregs[j] = init;
            }
         }

         maxin = MAX(maxin, cgb->source->in.count);
      }

      assert(i >= cgb->source->first && i <= cgb->source->last);

      cgen_ir(obj, cgb, &(func->source->irbuf[i]));

      if (i == cgb->source->last) {
         if (cgb->source->aborts)
            LLVMBuildUnreachable(obj->builder);

         if (LLVMGetBasicBlockTerminator(cgb->bbref) == NULL) {
            // Fall through to next block
            assert(!cgb->source->returns);
            assert(cgb + 1 < func->blocks + cfg->nblocks);
            LLVMBuildBr(obj->builder, (++cgb)->bbref);
         }
         else
            ++cgb;
      }
   }

   LLVMValueRef flags0_in[] = { llvm_int1(obj, false) };
   LLVMBasicBlockRef flags0_bb[] = { entry_bb };
   LLVMAddIncoming(func->blocks[0].inflags, flags0_in, flags0_bb, 1);

   LLVMValueRef *phi_in LOCAL = xmalloc_array(maxin, sizeof(LLVMValueRef));
   LLVMBasicBlockRef *phi_bb LOCAL =
      xmalloc_array(maxin, sizeof(LLVMBasicBlockRef));

   for (int i = 0; i < cfg->nblocks; i++) {
      jit_block_t *bb = &(cfg->blocks[i]);
      cgen_block_t *cgb = &(func->blocks[i]);

      // Flags
      for (int j = 0; j < bb->in.count; j++) {
         const int edge = jit_get_edge(&bb->in, j);
         phi_in[j] = func->blocks[edge].outflags;
         phi_bb[j] = func->blocks[edge].bbref;
      }
      LLVMAddIncoming(cgb->inflags, phi_in, phi_bb, bb->in.count);

      // Live-in registers
      for (int j = 0; j < func->source->nregs; j++) {
         if (cgb->inregs[j] != NULL) {
            for (int k = 0; k < bb->in.count; k++) {
               const int edge = jit_get_edge(&bb->in, k);
               assert(func->blocks[edge].outregs[j] != NULL);
               phi_in[k] = func->blocks[edge].outregs[j];
               phi_bb[k] = func->blocks[edge].bbref;
            }
            LLVMAddIncoming(cgb->inregs[j], phi_in, phi_bb, bb->in.count);
         }
      }
   }

   for (int i = 0; i < cfg->nblocks; i++) {
      cgen_block_t *cgb = &(func->blocks[i]);
      free(cgb->inregs);
      free(cgb->outregs);
      cgb->inregs = cgb->outregs = NULL;
   }

   LLVMPositionBuilderAtEnd(obj->builder, entry_bb);
   LLVMBuildBr(obj->builder, func->blocks[0].bbref);

   jit_free_cfg(func->source);
   func->cfg = cfg = NULL;

   free(func->blocks);
   func->blocks = NULL;
}

////////////////////////////////////////////////////////////////////////////////
// JIT plugin interface

#ifdef LLVM_HAS_LLJIT

typedef struct {
   LLVMOrcThreadSafeContextRef context;
   LLVMOrcLLJITRef             jit;
   LLVMOrcExecutionSessionRef  session;
   LLVMOrcJITDylibRef          dylib;
   LLVMTargetMachineRef        target;
} lljit_state_t;

static void *jit_llvm_init(void)
{
   LLVMInitializeNativeTarget();
   LLVMInitializeNativeAsmPrinter();

   lljit_state_t *state = xcalloc(sizeof(lljit_state_t));

   LLVMOrcLLJITBuilderRef builder = LLVMOrcCreateLLJITBuilder();

   LLVM_CHECK(LLVMOrcCreateLLJIT, &state->jit, builder);

   state->session = LLVMOrcLLJITGetExecutionSession(state->jit);
   state->dylib   = LLVMOrcLLJITGetMainJITDylib(state->jit);
   state->context = LLVMOrcCreateNewThreadSafeContext();
   state->target  = llvm_target_machine(LLVMRelocDefault,
                                        LLVMCodeModelJITDefault);

   const char prefix = LLVMOrcLLJITGetGlobalPrefix(state->jit);

   LLVMOrcDefinitionGeneratorRef gen_ref;
   LLVM_CHECK(LLVMOrcCreateDynamicLibrarySearchGeneratorForProcess,
              &gen_ref, prefix, NULL, NULL);

   LLVMOrcJITDylibAddGenerator(state->dylib, gen_ref);

   return state;
}

static void jit_llvm_cgen(jit_t *j, jit_handle_t handle, void *context)
{
   lljit_state_t *state = context;

   jit_func_t *f = jit_get_func(j, handle);

   const char *only = getenv("NVC_JIT_ONLY");
   if (only != NULL && !icmp(f->name, only))
      return;

   const uint64_t start_us = get_timestamp_us();

   llvm_obj_t obj = {
      .context = LLVMOrcThreadSafeContextGetContext(state->context),
      .target  = state->target,
   };

   LOCAL_TEXT_BUF tb = tb_new();
   tb_istr(tb, f->name);

   obj.module   = LLVMModuleCreateWithNameInContext(tb_get(tb), obj.context);
   obj.builder  = LLVMCreateBuilderInContext(obj.context);
   obj.data_ref = LLVMCreateTargetDataLayout(obj.target);

   llvm_register_types(&obj);

   cgen_func_t func = {
      .name   = tb_claim(tb),
      .source = f,
   };

   cgen_function(&obj, &func);

   llvm_finalise(&obj);

   LLVMOrcThreadSafeModuleRef tsm =
      LLVMOrcCreateNewThreadSafeModule(obj.module, state->context);
   LLVMOrcLLJITAddLLVMIRModule(state->jit, state->dylib, tsm);

   LLVMOrcJITTargetAddress addr;
   LLVM_CHECK(LLVMOrcLLJITLookup, state->jit, &addr, func.name);

   const uint64_t end_us = get_timestamp_us();
   static __thread uint64_t slowest = 0;
   if (end_us - start_us > slowest)
      debugf("%s at %p [%"PRIi64" us]", func.name, (void *)addr,
             (slowest = end_us - start_us));

   store_release(&f->entry, (jit_entry_fn_t)addr);

   LLVMDisposeTargetData(obj.data_ref);
   LLVMDisposeBuilder(obj.builder);
   free(func.name);
}

static void jit_llvm_cleanup(void *context)
{
   lljit_state_t *state = context;

   LLVMOrcDisposeThreadSafeContext(state->context);
   LLVMOrcDisposeLLJIT(state->jit);

   free(state);
}

static const jit_plugin_t jit_llvm = {
   .init    = jit_llvm_init,
   .cgen    = jit_llvm_cgen,
   .cleanup = jit_llvm_cleanup
};

void jit_register_llvm_plugin(jit_t *j)
{
   const int threshold = opt_get_int(OPT_JIT_THRESHOLD);
   if (threshold > 0) {
      extern const jit_plugin_t jit_llvm;
      jit_add_tier(j, threshold, &jit_llvm);
   }
   else if (threshold < 0)
      warnf("invalid NVC_JIT_THRESOLD setting %d", threshold);
}

#endif  // LLVM_HAS_LLJIT

////////////////////////////////////////////////////////////////////////////////
// Ahead-of-time code generation

llvm_obj_t *llvm_obj_new(const char *name)
{
   llvm_obj_t *obj = xcalloc(sizeof(llvm_obj_t));
   obj->context  = LLVMContextCreate();
   obj->module   = LLVMModuleCreateWithNameInContext(name, obj->context);
   obj->builder  = LLVMCreateBuilderInContext(obj->context);
   obj->target   = llvm_target_machine(LLVMRelocPIC, LLVMCodeModelDefault);
   obj->data_ref = LLVMCreateTargetDataLayout(obj->target);

   char *triple = LLVMGetTargetMachineTriple(obj->target);
   LLVMSetTarget(obj->module, triple);
   LLVMDisposeMessage(triple);

   LLVMSetModuleDataLayout(obj->module, obj->data_ref);

   llvm_register_types(obj);

   obj->ctor = LLVMAddFunction(obj->module, "ctor", obj->types[LLVM_CTOR_FN]);
   LLVMSetLinkage(obj->ctor, LLVMPrivateLinkage);

   llvm_append_block(obj, obj->ctor, "entry");

   LLVMValueRef entry = LLVMGetUndef(obj->types[LLVM_CTOR]);
   entry = LLVMBuildInsertValue(obj->builder, entry,
                                llvm_int32(obj, 65535), 0, "");
   entry = LLVMBuildInsertValue(obj->builder, entry, obj->ctor, 1, "");
   entry = LLVMBuildInsertValue(obj->builder, entry,
                                LLVMConstNull(obj->types[LLVM_PTR]), 2, "");

   LLVMTypeRef array_type = LLVMArrayType(obj->types[LLVM_CTOR], 1);
   LLVMValueRef global =
      LLVMAddGlobal(obj->module, array_type, "llvm.global_ctors");
   LLVMSetLinkage(global, LLVMAppendingLinkage);

   LLVMValueRef ctors[] = { entry };
   LLVMValueRef array = LLVMConstArray(obj->types[LLVM_CTOR], ctors, 1);
   LLVMSetInitializer(global, array);

   LLVMValueRef abi_version =
      LLVMAddGlobal(obj->module, obj->types[LLVM_INT32], "__nvc_abi_version");
   LLVMSetInitializer(abi_version, llvm_int32(obj, RT_ABI_VERSION));
   LLVMSetGlobalConstant(abi_version, true);
#ifdef IMPLIB_REQUIRED
   LLVMSetDLLStorageClass(abi_version, LLVMDLLExportStorageClass);
#endif

   return obj;
}

void llvm_aot_compile(llvm_obj_t *obj, jit_t *j, jit_handle_t handle)
{
   jit_func_t *f = jit_get_func(j, handle);
   if (f->irbuf == NULL)
      jit_irgen(f);

   const uint64_t start_us = get_timestamp_us();

   LOCAL_TEXT_BUF tb = tb_new();
   tb_istr(tb, f->name);

   cgen_func_t func = {
      .name   = tb_claim(tb),
      .source = f,
   };

   cgen_function(obj, &func);

   const uint64_t end_us = get_timestamp_us();
   static __thread uint64_t slowest = 0;
   if (end_us - start_us > slowest)
      debugf("compiled %s [%"PRIi64" us]", func.name,
             (slowest = end_us - start_us));

   free(func.name);
}

void llvm_obj_emit(llvm_obj_t *obj, const char *path)
{
   LLVMPositionBuilderAtEnd(obj->builder, LLVMGetLastBasicBlock(obj->ctor));
   LLVMBuildRetVoid(obj->builder);

   llvm_finalise(obj);

   char *error;
   if (LLVMTargetMachineEmitToFile(obj->target, obj->module, path,
                                   LLVMObjectFile, &error))
      fatal("Failed to write object file: %s", error);

   LLVMDisposeTargetData(obj->data_ref);
   LLVMDisposeTargetMachine(obj->target);
   LLVMDisposeBuilder(obj->builder);
   LLVMDisposeModule(obj->module);
   LLVMContextDispose(obj->context);

   shash_free(obj->string_pool);

   free(obj);
}
