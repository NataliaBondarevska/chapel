/*
 * Copyright 2004-2016 Cray Inc.
 * Other additional copyright holders may be indicated within.
 * 
 * The entirety of this work is licensed under the Apache License,
 * Version 2.0 (the "License"); you may not use this file except
 * in compliance with the License.
 * 
 * You may obtain a copy of the License at
 * 
 *     http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef _misc_H_
#define _misc_H_

#include "driver.h"

#include <cstdio>

#define exit(x) dont_use_exit_use_clean_exit_instead

// INT_FATAL(ast, format, ...)
//   where ast         == BaseAST* or NULL
//         format, ... == normal printf stuff
// results in something like:
// INTERNAL ERROR in compilerSrc.c (lineno): your text here (usrSrc:usrLineno)

#define INT_FATAL      setupError(__FILE__, __LINE__, 1), handleError
#define USR_FATAL      setupError(__FILE__, __LINE__, 2), handleError
#define USR_FATAL_CONT setupError(__FILE__, __LINE__, 3), handleError
#define USR_WARN       setupError(__FILE__, __LINE__, 4), handleError
#define USR_PRINT      setupError(__FILE__, __LINE__, 5), handleError

#define USR_STOP       exitIfFatalErrorsEncountered

// INT_ASSERT is intended to become no-op in production builds of compiler
#define INT_ASSERT(x) do { if (!(x)) INT_FATAL("assertion error"); } while (0)

#define iterKindTypename          "iterKind"
#define iterKindLeaderTagname     "leader"
#define iterKindFollowerTagname   "follower"
#define iterKindStandaloneTagname "standalone"
#define iterFollowthisArgname     "followThis"

enum WARN_TAGS {
  PLACEHOLDER = 0,
  PATH_MISMTH, // "$CHPL_HOME=%s mismatched with executable home=%s"
  NOT_CHPL_DISTRBTN, //"CHPL_HOME=%s is not a Chapel distribution"
  ERR_OPNG_PRNT_PASS_FILE, //Error opening printPassesFile: %s."
  CANNT_DO_STACK_CHECK, //CHPL_TASKS=%s cannot do stack checks.
  CHPL_TASK_TRACK, //Enabling task tracking with CHPL_TASKS=%s has no effect other than to slow down compilation
  STTC_COMPILATION_NO_SUPPORT, //Static compilation is not supported on OS X, ignoring flag.
  CHPL_TARGET_ARCH_UNKNOWN, //--specialize was set, but CHPL_TARGET_ARCH is 'unknown'. If
  //you want any specialization to occur please set CHPL_TARGET_ARCH
  INFNT_LOOP, //"Infinite loop? The loop condition variable is never updated within the loop."
  WHILE_LOOP_CONST_CONDTN, //A while loop with a constant condition
  EXTERN_TYPE_DEF_MISSING, //Could not find extern def of type %s
  EXTERN_DEF_MISSING, //Could not find extern def of %s
  RETURN_TYPE_NOT_VALUE, //function's return type is not a value type.  Ignoring.
  COBEGIN_INSUFFCNT_STTNS, //cobegin has no effect if it contains fewer than 2 statements
  ATOM_WORD_IGNORED, //"atomic keyword is ignored (not implemented)
  NOT_PREINCRMNT, //++ is not a pre-increment
  NOT_PREDECRMNT, //-- is not a pre-decrement
  PROMOTION_WARN, //promotion on %s
  DEPTH_VALUE_EXCEED_STACK, //compiler diagnostic depth value exceeds call stack depth
  NEGATIVE_DEPTH_VALUE, //compiler diagnostic depth value can not be negative
  TYPE_NO_SUPPORT_NOINIT,//type %s does not currently support noinit, using default initialization
  LOCALE_ACCESSING, //accessing the locale of a local expression
  REF_VAR_INTENT_PASS,//sync, single, or atomic var '%s' currently can be passed into the forall loop by 'ref'
  //intent only - %s is ignored
  REF_CONST_MULTI_VAR_INTENT_PASS,//"Arrays, domains, and distributions currently can be passed into the forall loop by
  //'const', 'ref' or 'const ref' intent only. '%s' will be passed by %s.
  WRONG_VAR_DECL,//"Achtung! var %s  type %s  decl %s\n"
  FORALL_INTENT_COPY_IMPLEMENTATION,//The blank forall intent for record and union variables is temporarily implemented as a copy, not 'const ref'.
  //As a result, the %s variable '%s' is affected. Its field %s: %s inside the loop will be a copy,
  //not alias, of its field outside the loop. Use a task-intent-clause to pass it by reference, e.g. 'with (ref %s)'."
  USE_STMNT,//as written, '%s' is a sub-module of the module created for file '%s' due to the file-level 'use' statements.  If you "
  //"meant for '%s' to be a top-level module, move the 'use' statements into its scope.
  PRIVATE_DECL_WITHIN_FUNCS, //Private declarations within function bodies are meaningless
  PRIVATE_DECL_WITHIN_NESTED_BLOCK, //Private declarations within nested blocks are meaningless
  PRIVATE_DECL_WITHIN_MODULE, //Private declarations are meaningless outside of module level declarations
  OPERAND_REF_INTENT, //The left operand of '=' and '<op>=' should have 'ref' intent.
  REDANDANT_DEFNTN, //"'%s' defined more than once in Chapel internal modules."
  C_PACKED_POINTER_CODEGEN, //C code generation for packed pointers not supported
  REPEATED_IDENTIFIER, //"identifier '%s' is repeated
  MODULE_SYMBOL_HIDES_FUNC_ARG, //Module level symbol is hiding function argument '%s'
  REDUCE_INTENT_VALUE, //the variable '%s' is given a reduce intent and not mentioned in the
  //loop body - it will have the unit value after the loop
  PRAGMA_NONCLASS_FIELD_APPLIED,//\"local field\" pragma applied to non-class field %s (%s) in type %s\n
  PRAGMA_NONFIELD_APPLIED, //"\"local field\" pragma applied to non-field %s\n
  AMBIGUOUS_SOURCE_FILE, //Ambiguous module source file -- using %s over %s

  MISCONFIGURED_ENVIROUNMENT = 0x20000000,
  VERIFICATION,
  COMPILATION_PROBLEMS,
  UNSUPPORTED,
  LOGIC_ISSUES,
  STYLE_ISSUES
};

class BaseAST;

bool forceWidePtrsForLocal();
bool requireWideReferences();
bool requireOutlinedOn();

const char* cleanFilename(BaseAST*    ast);
const char* cleanFilename(const char* name);

void setupError(const char* filename, int lineno, int tag);
void handleError(const char* fmt, ...);
void handleError(WARN_TAGS wtag, const char* fmt, ...);
void handleError(BaseAST* ast, const char* fmt, ...);
void handleError(FILE* file, BaseAST* ast, const char* fmt, ...);
void handleError(WARN_TAGS wtag, BaseAST* ast, const char* fmt, ...);
void handleError(WARN_TAGS wtag, FILE* file, BaseAST* ast, const char* fmt, ...);
void exitIfFatalErrorsEncountered(void);
void considerExitingEndOfPass(void);
void printCallStack(bool force, bool shortModule, FILE* out);

void startCatchingSignals(void);
void stopCatchingSignals(void);

void clean_exit(int status);

void gdbShouldBreakHere(void); // must be exposed to avoid dead-code elim.

#endif
