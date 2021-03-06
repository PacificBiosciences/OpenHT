/* Copyright (c) 2015 Convey Computer Corporation
 *
 * This file is part of the OpenHT toolset located at:
 *
 * https://github.com/TonyBrewer/OpenHT
 *
 * Use and distribution licensed under the BSD 3-clause license.
 * See the LICENSE file for the complete license text.
 */
#include <boost/foreach.hpp>
#include "AstFromString.h"
#include "HtcAttributes.h"
#include "HtDeclManager.h"
#include "HtSageUtils.h"
#include "HtdInfoAttribute.h"
#include "LateOmpLowering.h"
#include "HtSCDecls.h"
#include "IsolateModules.h"

extern scDeclarations *SCDecls;

using namespace AstFromString;


#define foreach BOOST_FOREACH  // Replace with range-based for when C++'11 



//-------------------------------------------------------------------------
// RewriteOmp
//
// Perform the ht-specific (or "late") portion of OMP lowering
// transformations.  Most of the OMP lowering is standard ROSE
// code that happens early in the front-end.
//
//-------------------------------------------------------------------------
class RewriteOmp : public AstPrePostProcessing {
public:
  void preOrderVisit(SgNode *S) {
    switch (S->variantT()) {
    case V_SgFunctionDeclaration:
      preVisitSgFunctionDeclaration(dynamic_cast<SgFunctionDeclaration *>(S));
      break;
    default:
      break;
    }
  }

  void postOrderVisit(SgNode *S) {
    switch (S->variantT()) {
    case V_SgFunctionDeclaration:
      postVisitSgFunctionDeclaration(dynamic_cast<SgFunctionDeclaration *>(S));
      break;
    case V_SgFunctionCallExp:
      postVisitSgFunctionCallExp(dynamic_cast<SgFunctionCallExp *>(S));
      break;
    default:
      break;
    }
  }

private: 
  void preVisitSgFunctionDeclaration(SgFunctionDeclaration *FD);
  void postVisitSgFunctionDeclaration(SgFunctionDeclaration *FD);
  void postVisitSgFunctionCallExp(SgFunctionCallExp *FCE);
  SgExpression *getOmpTidExpr(SgFunctionDefinition *fdef, std::string);
  SgExpression *getOmpTeamSizeExpr(SgFunctionDefinition *fdef, std::string);
  SgExpression *getOmpLeagueSizeExpr(SgFunctionDefinition *fdef);
  SgExpression *getParentHtidExpr(SgFunctionDefinition *fdef);
  SgStatement *buildComputeMinUpperBound(SgExpression *lower,
    SgExpression *upper, SgExpression *n_lower, SgExpression *n_upper);
  void buildLocalIterSpaceComputation(SgStatement *insertPoint,
      SgFunctionDefinition *fdef, SgExpression *lower, SgExpression *upper,
      SgExpression *stride, SgExpression *n_lower, SgExpression *n_upper,
      bool isDistribute);
  void buildLocalIterSpaceComputation_StaticSpec(SgStatement *insertPoint,
      SgFunctionDefinition *fdef, SgExpression *lower, SgExpression *upper,
      SgExpression *stride, SgExpression *chunk_size, SgExpression *n_lower,
      SgExpression *n_upper, SgVariableDeclaration *chunkDecl,
      bool isDistribute, bool doChunkSpecial);
  void buildThreadSpawnLoop(SgFunctionDefinition *fdef,
      SgFunctionCallExp *fce, std::vector<SgExpression *> &vv,
      SgVariableDeclaration *declHtId);

  std::vector<SgFunctionCallExp *> rewriteList;
};

void RewriteOmp::preVisitSgFunctionDeclaration(SgFunctionDeclaration *FD)
{
  SgFunctionDefinition *fdef = FD->get_definition();
  if (!fdef) {
    return;
  }

  rewriteList.clear();

}


// Iterate over the list of XOMP call expressions, rewriting each one.
void RewriteOmp::postVisitSgFunctionDeclaration(SgFunctionDeclaration *FD)
{
  SgFunctionDefinition *fdef = FD->get_definition();
  if (!fdef) {
    return;
  }

  static bool first = true;

  std::map<SgBasicBlock *, SgExpression *> newStaticCond;
  bool sawBarrier = false;

  // Grab decls for PR_htId, HtBarrier, etc.
  SgGlobal *GS = SageInterface::getGlobalScope(FD);
  SgVariableDeclaration *declHtId =
      HtDeclMgr::buildHtlVarDecl("PR_htId", GS);
  SgVariableDeclaration *declUnitId =
      HtDeclMgr::buildHtlVarDecl("SR_unitId", GS);
  SgFunctionDeclaration *declHtBarrier = 
      HtDeclMgr::buildHtlFuncDecl("HtBarrier", GS);
  SgFunctionDeclaration *declTestLock = 
      HtDeclMgr::buildHtlFuncDecl("rhomp_test_lock", GS);
  SgFunctionDeclaration *declSetLock = 
      HtDeclMgr::buildHtlFuncDecl("rhomp_set_lock", GS);
  SgFunctionDeclaration *declUnsetLock = 
      HtDeclMgr::buildHtlFuncDecl("rhomp_unset_lock", GS);
  SgFunctionDeclaration *declInitLock = 
      HtDeclMgr::buildHtlFuncDecl("rhomp_init_lock", GS);

  SgTypedefSymbol *lock_typedef = GS->lookup_typedef_symbol("omp_lock_t");

  // Redefine typedef of omp_lock_t as an int
  //
  // This type is supposed to be "opaque" to the OpenMP user.
  // Various omp.h files define it according to their implementation.
  // We force it to match the HT implementation, which is an int.
  if (first) {
      first = false;
      if (lock_typedef) {
          lock_typedef->get_declaration()->
              set_base_type(SageBuilder::buildIntType());
      }
  }

  // If this function is not generated for an outlined region, add
  // the extra HT OMP parameters to the beginning of the parameter
  // list for the function declaration.
  //
  // These are PREpended to the argument list, so added in reverse
  // order.

  if (SageInterface::is_OpenMP_language() && (getOmpEnclosingFunctionAttribute(FD) == NULL)) {
      SgFunctionParameterList* params = FD->get_parameterList ();
      SgInitializedName* parameter1;
      SgType* ptype = SCDecls->get_htc_teams_t_type();

      parameter1 = 
          SageBuilder::buildInitializedName("__htc_my_omp_league_size",ptype);
      SageInterface::prependArg(params, parameter1);

      ptype = SCDecls->get_htc_tid_t_type();

      parameter1 = 
          SageBuilder::buildInitializedName("__htc_my_omp_team_size",ptype);
      SageInterface::prependArg(params, parameter1);

      parameter1 = 
          SageBuilder::buildInitializedName("__htc_my_omp_thread_num",ptype);
      SageInterface::prependArg(params, parameter1);

      parameter1 = 
          SageBuilder::buildInitializedName("__htc_parent_htid",ptype);
      SageInterface::prependArg(params, parameter1);
  }


  //
  // Process each XOMP or omp call expression to be rewritten.
  //
  foreach (SgFunctionCallExp *fce, rewriteList) {
    SgFunctionDeclaration *calleeFD = fce->getAssociatedFunctionDeclaration();
    std::string calleeName = calleeFD->get_name().getString();

    if (calleeName == "XOMP_parallel_end"
        || calleeName == "XOMP_terminate"
        || calleeName == "XOMP_sections_end_nowait"
        || calleeName == "XOMP_loop_static_init"
        || calleeName == "XOMP_distribute_static_init"
        || calleeName == "XOMP_loop_end_nowait"
        || calleeName == "XOMP_distribute_end_nowait"
        || calleeName == "XOMP_init") {
      // Just comment out XOMP calls that don't otherwise need translation.
      // This can help document the generated code.
      SgStatement *nstmt = SageBuilder::buildNullStatement();
      SageInterface::attachComment(nstmt,
          fce->get_parent()->unparseToString(),
          PreprocessingInfo::before);
      SageInterface::replaceStatement(isSgStatement(fce->get_parent()), nstmt); 
    } else if (calleeName == "XOMP_task"
        || calleeName == "XOMP_taskwait") {
      std::cerr << "Warning: OMP tasking unsupported on FPGA target, ignored."
          << std::endl;
      SgStatement *nstmt = SageBuilder::buildNullStatement();
      SageInterface::attachComment(nstmt,
          fce->get_parent()->unparseToString(),
          PreprocessingInfo::before);
      SageInterface::replaceStatement(isSgStatement(fce->get_parent()), nstmt); 

    } else if (calleeName == "XOMP_parallel_start") {

      // mark the function with the contains_omp_parallel attribute
      if (!fdef->attributeExists("contains_omp_parallel")) {
          fdef->setAttribute("contains_omp_parallel", new AstAttribute);
      }

      //
      // Rewrite XOMP_parallel_start(OUT_func, data, ifclause, numthreads,
      //   filestring, fileline) to spawn a new team of threads.
      //
      // Note: If the user hardcodes num_threads(1), we will not generate
      // the fork-loop, as we only make one synchronous SendCall.
      //  
      SgExprListExp *pstartParmList = fce->get_args();
      std::vector<SgExpression *> &vv = pstartParmList->get_expressions();
      bool numThreadsIsOne = 
          isSgIntVal(vv[3]) && isSgIntVal(vv[3])->get_value() == 1;
      if (!numThreadsIsOne) { 
        buildThreadSpawnLoop(fdef, fce, vv, declHtId);
      } else {
        // Insert a call to the outlined routine in the loop body.
        // The outlined function prototype is this:
        //   OUT_foo(int __htc_arg_parentHtId, int __htc_arg_myOmpTid, 
        //           int __htc_team_size, int __htc_league_size);
        // NOTE: The (int) cast is to satisfy htv strictness.
        SgStatement *insertPoint = HtSageUtils::findSafeInsertPoint(fce);
        SgName outFunc = isSgFunctionRefExp(vv[0])->get_symbol()->get_name();
        SgExprStatement *newCallStmt =  
            SageBuilder::buildFunctionCallStmt(outFunc, 
            SageBuilder::buildVoidType(), SageBuilder::buildExprListExp(
                SageBuilder::buildCastExp(
                    SageBuilder::buildVarRefExp(declHtId),
                    SCDecls->get_htc_tid_t_type()),
                SageBuilder::buildCastExp(
                    SageBuilder::buildIntVal(0),
                    SCDecls->get_htc_tid_t_type()),
                SageBuilder::buildCastExp(
                    SageBuilder::buildIntVal(1),
                    SCDecls->get_htc_tid_t_type()),
                getOmpLeagueSizeExpr(fdef)),
            fdef->get_body());
        SageInterface::insertStatementAfter(insertPoint, newCallStmt);
      }

      // Comment the original call for documentation purposes.
      SgStatement *nstmt = SageBuilder::buildNullStatement();
      SageInterface::replaceStatement(isSgStatement(fce->get_parent()), nstmt);
      SageInterface::attachComment(nstmt, fce->get_parent()->unparseToString(),
          PreprocessingInfo::before);

    } else if (calleeName == "XOMP_barrier"
        || calleeName == "XOMP_loop_end"
        || calleeName == "XOMP_sections_end") {

      // Rewrite:
      //     ...
      //     XOMP_barrier();
      //     ...
      // as:
      //     ...
      //     HtBarrier(barrier_id, barrier_rtn, thread_count);
      //   barrier_rtn:
      //     ...
      // 

      // Insert point is just after the statement enclosing the original
      // call expression.
      SgStatement *insertPoint = HtSageUtils::findSafeInsertPoint(fce);

      SgExpression *teamSz = getOmpTeamSizeExpr(fdef, calleeName);
      assert(teamSz);
      SgExpression *parHtid = getParentHtidExpr(fdef);
      assert(parHtid);

      int ln = ++SageInterface::gensym_counter;
      SgLabelStatement *labelBarrierRtnStmt = HtSageUtils::makeLabelStatement(
          "barrier_rtn", ln, fdef);

      // Build the new 'HtBarrier(...)' call statement.
      SgExpression *barrierId = parHtid;
      SgExpression *threadCnt = teamSz;
      SgExpression *placeholder = SageBuilder::buildIntVal(-999);
      SgExprStatement *barrCallStmt = 
          SageBuilder::buildFunctionCallStmt("HtBarrier", 
          SageBuilder::buildVoidType(),
          SageBuilder::buildExprListExp(barrierId, placeholder, threadCnt), 
          fdef->get_body());
  
      // Mark the placeholder expression with an FsmPlaceHolderAttribute to 
      // indicate a deferred FSM name is needed.
      FsmPlaceHolderAttribute *attr =
          new FsmPlaceHolderAttribute(labelBarrierRtnStmt);
      placeholder->addNewAttribute("fsm_placeholder", attr);

      // Append AddBarrier to htd (only one unnamed barrier per module).
      if (!sawBarrier) {
        sawBarrier = true;
        HtdInfoAttribute *htd = getHtdInfoAttribute(fdef);
        htd->appendBarrier("5" /* TODO: better width */);
      }

      SageInterface::insertStatementAfter(insertPoint, barrCallStmt);
      SageInterface::insertStatementAfter(barrCallStmt, labelBarrierRtnStmt);

      // Comment the original call for documentation purposes.
      SgStatement *nstmt = SageBuilder::buildNullStatement();
      SageInterface::replaceStatement(isSgStatement(fce->get_parent()), nstmt);
      SageInterface::attachComment(nstmt, fce->get_parent()->unparseToString(),
          PreprocessingInfo::before);

    } else if (calleeName == "XOMP_loop_default"
               || calleeName == "XOMP_distribute_default") {

      //
      // Rewrite XOMP_loop_default(lower, upper, stride, &n_lower, &n_upper)
      // to compute the local iteration space for each thread.
      //
      // The loop comparison is assumed to be inclusive (i.e., <= or >=).
      // This is the static case with unspecified chunk_size.
      //

      bool isDistribute = (calleeName == "XOMP_distribute_default");
      SgExprListExp *xompParmList = fce->get_args();
      std::vector<SgExpression *> &vv = xompParmList->get_expressions();

      SgExpression *lower = isSgExpression(vv[0]);
      SgExpression *upper = isSgExpression(vv[1]);
      SgExpression *stride = isSgExpression(vv[2]);
      SgExpression *n_lower = 
          isSgExpression(isSgAddressOfOp(vv[3])->get_operand());
      SgExpression *n_upper = 
          isSgExpression(isSgAddressOfOp(vv[4])->get_operand());

      // Insert point is just after the statement enclosing the original
      // call expression.
      SgStatement *insertPoint = HtSageUtils::findSafeInsertPoint(fce);
      buildLocalIterSpaceComputation(insertPoint, fdef, lower, upper, stride,
          n_lower, n_upper, isDistribute);

      // Comment original XOMP call for documentation.
      SgStatement *nstmt = SageBuilder::buildNullStatement();
      SageInterface::attachComment(nstmt,
          fce->get_parent()->unparseToString(),
          PreprocessingInfo::before);
      SageInterface::replaceStatement(isSgStatement(fce->get_parent()), nstmt); 

    } else if (calleeName == "XOMP_loop_static_start"
               || calleeName == "XOMP_distribute_static_start") {

      //
      // Rewrite XOMP_loop_static_start() to compute the local iteration
      // space for each thread.  This is the static case with specified 
      // chunk_size.
      //
      // The loop comparison is assumed to be inclusive (i.e., <= or >=).
      //
      // CASE: chunk_size = 1 (special case, faster code):
      // Rewrite:
      //   if (XOMP_loop_static_start(lower, upper, stride, chunk_size,
      //       &n_lower, &n_upper)) {
      //     do { 
      //       for (p_index = n_lower; p_index <= n_upper; p_index += stride)
      //         ...
      //     } while (XOMP_loop_static_next(&n_lower, &n_upper));
      //   }
      // as:
      //   if (1) {
      //     n_lower = lower + stride * omp_get_thread_num();
      //     n_upper = upper;
      //     next_chunk = stride * team_size;
      //     do { 
      //       for (p_index = n_lower; p_index <= n_upper;
      //            p_index += next_chunk)
      //         ...
      //     } while (0);
      //   }
      //
      // CASE: chunk_size > 1, rewrite as:
      //   if (1) {
      //     n_lower = lower + chunk_size * stride * omp_get_thread_num();
      //     n_upper = n_lower + chunk_size * stride;
      //     next_chunk = chunk_size * stride * team_size;
      //     do { 
      //       n_upper = min(n_upper - 1, upper); // (or +1 for downcounting) 
      //       for (p_index = n_lower; p_index <= n_upper; p_index += stride) {
      //         ... 
      //       }
      //       n_lower += next_chunk;
      //       n_upper = n_lower + chunk_size * stride;
      //     } while (n_lower <= upper);
      //   }
      //  
      // Note the XOMP_loop_static_next call will be replaced by the
      // appropriate new condition when that call is rewritten.
      //

      bool isDistribute = (calleeName == "XOMP_distribute_static_start");

      // Collect the statements we'll need to examine or modify.  This
      // assumes the very specific code generated by the OpenMP lowerer.
      SgIfStmt *ifStmt = isSgIfStmt(fce->get_parent()->get_parent());
      assert(ifStmt);
      SgBasicBlock *trueBody = isSgBasicBlock(ifStmt->get_true_body());
      assert(trueBody);
      SgDoWhileStmt *doStmt = isSgDoWhileStmt(
          SageInterface::getFirstStatement(trueBody, true));
      assert(doStmt);
      SgForStatement *forStmt = isSgForStatement(
          SageInterface::getFirstStatement(
          isSgBasicBlock(doStmt->get_body()), true));
      assert(forStmt);

      SgExprListExp *xompParmList = fce->get_args();
      std::vector<SgExpression *> &vv = xompParmList->get_expressions();

      SgExpression *lower = isSgExpression(vv[0]);
      SgExpression *upper = isSgExpression(vv[1]);
      SgExpression *stride = isSgExpression(vv[2]);
      SgExpression *chunk_size = isSgExpression(vv[3]);
      SgExpression *n_lower = 
          isSgExpression(isSgAddressOfOp(vv[4])->get_operand());
      SgExpression *n_upper = 
          isSgExpression(isSgAddressOfOp(vv[5])->get_operand());

      bool chunkIsOne = 
          isSgIntVal(chunk_size) && isSgIntVal(chunk_size)->get_value() == 1;

      // Build decl for the new increment.
      SgName chunkName = "__htc_next_chunk";
      chunkName << ++SageInterface::gensym_counter; 
#if 0
      SgType *ivarType = SageBuilder::buildLongType();
#else
      SgType *ivarType = lower->get_type();
#endif
      SgVariableDeclaration *chunkDecl = 
          SageBuilder::buildVariableDeclaration(chunkName, ivarType, 0, fdef);
      SageInterface::prependStatement(chunkDecl, fdef->get_body());

      // Build the local iteration space computation.
      SgStatement *ns = SageBuilder::buildNullStatement();
      SageInterface::prependStatement(ns, trueBody);
      buildLocalIterSpaceComputation_StaticSpec(ns, fdef, lower,
          upper, stride, chunk_size, n_lower, n_upper, chunkDecl, isDistribute,
          true /* doChunkSpecial */);

      // Build the upper bound check.
      if (!chunkIsOne) {
        SgStatement *minStmt = buildComputeMinUpperBound(lower, upper, n_lower,
            n_upper);
        SageInterface::insertStatementBefore(forStmt, minStmt);
      }

      // For special chunk_size=1 case, rewrite the loop increment to use 
      // the new stride (next_chunk).  Otherwise update the new lower
      // and upper bounds by next_chunk.
      if (chunkIsOne) {
        SageInterface::setLoopStride(forStmt,
            SageBuilder::buildVarRefExp(chunkDecl));
      } else {
        // n_lower += next_chunk;
        // n_upper = n_lower + chunk_size * stride;
        SgStatement *asg1 = SageBuilder::buildAssignStatement(
            SageInterface::deepCopy(n_lower),
            SageBuilder::buildAddOp(
                SageInterface::deepCopy(n_lower),
                SageBuilder::buildVarRefExp(chunkDecl)));
        SageInterface::insertStatementAfter(forStmt, asg1);

        SgStatement *asg2 = SageBuilder::buildAssignStatement(
            SageInterface::deepCopy(n_upper),
            SageBuilder::buildAddOp(
                SageInterface::deepCopy(n_lower),
                SageBuilder::buildMultiplyOp(
                    SageInterface::deepCopy(chunk_size),
                    SageInterface::deepCopy(stride))));
        SageInterface::insertStatementAfter(asg1, asg2);
      }

      // The if-stmt will now be treated as if it always passes, so we
      // replace it with the true branch.
      SgStatement *ifParent = isSgStatement(ifStmt->get_parent());
      ifStmt->set_true_body(0);
      ifParent->replace_statement(ifStmt, trueBody); 
 
      // Tag the basic block that encloses the do-loop with the new
      // conditional expression.  The original do-loop condition will
      // replaced when the XOMP_loop_static_next is rewritten.
      SgExpression *newCond = 0;
      if (chunkIsOne) {
        newCond = SageBuilder::buildIntVal(0);
      } else {
        // n_lower <= upper  (or n_lower >= upper for down-counting).
        SgExpression *forCond = 
            isSgExprStatement(SageInterface::getLoopCondition(forStmt))->
                get_expression();
        if (isSgLessOrEqualOp(forCond)) {
          newCond = SageBuilder::buildLessOrEqualOp(
              SageInterface::deepCopy(n_lower),
              SageInterface::deepCopy(upper));
        } else {
          assert(isSgGreaterOrEqualOp(forCond));
          newCond = SageBuilder::buildGreaterOrEqualOp(
              SageInterface::deepCopy(n_lower),
              SageInterface::deepCopy(upper));
        }
      }
      newStaticCond[trueBody] = newCond;

    } else if (calleeName == "XOMP_distribute_parallel_for_static_start") {
      // 
      // Analogous to XOMP_{loop,distribute}_static_start, except that
      // now there is another construct nested inside of this one.  We
      // also do not treat chunk_size=1 as a special case here.
      // 

      // Collect the statements we'll need to examine or modify.  This
      // assumes the very specific code generated by the OpenMP lowerer.
      SgIfStmt *ifStmt = isSgIfStmt(fce->get_parent()->get_parent());
      assert(ifStmt);
      SgBasicBlock *trueBody = isSgBasicBlock(ifStmt->get_true_body());
      assert(trueBody);
      SgDoWhileStmt *doStmt = isSgDoWhileStmt(
          SageInterface::getFirstStatement(trueBody, true));
      assert(doStmt);

      SgExprListExp *xompParmList = fce->get_args();
      std::vector<SgExpression *> &vv = xompParmList->get_expressions();

      SgExpression *lower = isSgExpression(vv[0]);
      SgExpression *upper = isSgExpression(vv[1]);
      SgExpression *stride = isSgExpression(vv[2]);
      SgExpression *chunk_size = isSgExpression(vv[3]);
      SgExpression *n_lower = 
          isSgExpression(isSgAddressOfOp(vv[4])->get_operand());
      SgExpression *n_upper = 
          isSgExpression(isSgAddressOfOp(vv[5])->get_operand());

      // Build decl for the new increment.
      SgName chunkName = "__htc_next_chunk";
      chunkName << ++SageInterface::gensym_counter; 
#if 0
      SgType *ivarType = SageBuilder::buildLongType();
#else
      SgType *ivarType = lower->get_type();
#endif
      SgVariableDeclaration *chunkDecl = 
          SageBuilder::buildVariableDeclaration(chunkName, ivarType, 0, fdef);
      SageInterface::prependStatement(chunkDecl, fdef->get_body());

      // Build the local iteration space computation.
      SgStatement *ns = SageBuilder::buildNullStatement();
      SageInterface::prependStatement(ns, trueBody);
      buildLocalIterSpaceComputation_StaticSpec(ns, fdef, lower,
          upper, stride, chunk_size, n_lower, n_upper, chunkDecl,
          true /* isDisribute */, false /* doChunkSpecial */);

      // Build the upper bound check.
      SgStatement *minStmt = buildComputeMinUpperBound(lower, upper, n_lower,
          n_upper);
      SageInterface::prependStatement(minStmt, isSgBasicBlock(doStmt->get_body()));

      // For special chunk_size=1 case, rewrite the loop increment to use 
      // the new stride (next_chunk).  Otherwise update the new lower
      // and upper bounds by next_chunk.
      //   n_lower += next_chunk;
      //   n_upper = n_lower + chunk_size * stride;
      SgStatement *asg1 = SageBuilder::buildAssignStatement(
          SageInterface::deepCopy(n_lower),
          SageBuilder::buildAddOp(
              SageInterface::deepCopy(n_lower),
              SageBuilder::buildVarRefExp(chunkDecl)));
      SageInterface::appendStatement(asg1, isSgBasicBlock(doStmt->get_body()));

      SgStatement *asg2 = SageBuilder::buildAssignStatement(
          SageInterface::deepCopy(n_upper),
          SageBuilder::buildAddOp(
              SageInterface::deepCopy(n_lower),
              SageBuilder::buildMultiplyOp(
                  SageInterface::deepCopy(chunk_size),
                  SageInterface::deepCopy(stride))));
      SageInterface::insertStatementAfter(asg1, asg2);

      // The if-stmt will now be treated as if it always passes, so we
      // replace it with the true branch.
      SgStatement *ifParent = isSgStatement(ifStmt->get_parent());
      ifStmt->set_true_body(0);
      ifParent->replace_statement(ifStmt, trueBody); 
 
      // Tag the basic block that encloses the do-loop with the new
      // conditional expression.  The original do-loop condition will
      // replaced when the XOMP_distribute_static_next is rewritten.
      SgExpression *newCond = 0;
      //   n_lower <= upper  (or n_lower >= upper for down-counting).
      // (Need to search for the original inner for-loop in order to obtain
      // original loop direction.)
      std::vector<SgNode *> floops = 
          NodeQuery::querySubTree(doStmt, V_SgForStatement);
      assert(floops.size() > 0);
      SgForStatement *outerForLoop = isSgForStatement(floops[0]);
      assert(outerForLoop && "did not find associated for-loop?");
      SgExpression *forCond = 
          isSgExprStatement(SageInterface::getLoopCondition(outerForLoop))->
              get_expression();
      if (isSgLessOrEqualOp(forCond)) {
        newCond = SageBuilder::buildLessOrEqualOp(
            SageInterface::deepCopy(n_lower),
            SageInterface::deepCopy(upper));
      } else {
        assert(isSgGreaterOrEqualOp(forCond));
        newCond = SageBuilder::buildGreaterOrEqualOp(
            SageInterface::deepCopy(n_lower),
            SageInterface::deepCopy(upper));
      }
      newStaticCond[trueBody] = newCond;

    } else if (calleeName == "XOMP_loop_static_next"
               || calleeName == "XOMP_distribute_static_next") {

      SgDoWhileStmt *doStmt = isSgDoWhileStmt(fce->get_parent()->get_parent());
      assert(doStmt);
      SgBasicBlock *bbstmt = isSgBasicBlock(doStmt->get_parent());
      assert(bbstmt);
      SgExpression *cexpr = newStaticCond[bbstmt];
      assert(cexpr && "did not find newStaticCond?");
      SageInterface::replaceExpression(fce, cexpr, false /* keepOldExp */);

      // If the condition is 0, then the do-stmt will only iterate once,
      // so we replace it with the body.
      if (isSgIntVal(cexpr) && isSgIntVal(cexpr)->get_value() == 0) {
        SgStatement *doParent = isSgStatement(doStmt->get_parent());
        SgStatement *doBody = doStmt->get_body();
        doStmt->set_body(0);
        doParent->replace_statement(doStmt, doBody); 
      }

    } else if (calleeName == "XOMP_sections_init_next") {

      //
      // Treat section construct as a static omp-for instead of XOMP scheme.
      // We'll need to adjust the while-loop condition that ROSE gives us,
      // and then compute a local iteration space as we would for omp-for.
      //  
      // Rewrite:
      //    int xomp_section_1 = XOMP_sections_init_next(3);
      //    while(xomp_section_1 >= 0) { ... }
      // as:
      //    int sec_lb = ... ;               // Compute as in omp-for static
      //    int sec_ub = ... ;               // Compute as in omp-for static
      //    int xomp_section_1 = sec_lb;
      //    while(xomp_section_1 <= sec_ub) { ... }
      // 
      // Note that the loop increment will be adjusted when XOMP_sections_next
      // is rewritten.
      //

      SgExprListExp *xompParmList = fce->get_args();
      std::vector<SgExpression *> &vv = xompParmList->get_expressions();
      SgExpression *origSectionCount = isSgExpression(vv[0]);
      assert(isSgIntVal(origSectionCount));
      int adjSectionCount = isSgIntVal(origSectionCount)->get_value() - 1;
      
      // Build decls for the lower and upper bounds.
      SgName lbName = "__htc_sec_lb";
      lbName << ++SageInterface::gensym_counter; 
      SgType *ivarType = SageBuilder::buildLongType();
      SgVariableDeclaration *lbDecl = 
          SageBuilder::buildVariableDeclaration(lbName, ivarType, 0, fdef);
      SageInterface::prependStatement(lbDecl, fdef->get_body());

      SgName ubName = "__htc_sec_ub";
      ubName << ++SageInterface::gensym_counter; 
      SgVariableDeclaration *ubDecl = 
          SageBuilder::buildVariableDeclaration(ubName, ivarType, 0, fdef);
      SageInterface::prependStatement(ubDecl, fdef->get_body());

      // Find the while-stmt.  This is somewhat fragile in that it depends 
      // on whether any phase will ever insert statements between the XOMP
      // call statement and the while-stmt.
      SgStatement *insertPoint = HtSageUtils::findSafeInsertPoint(fce);
      SgStatement *whileStmt = SageInterface::getNextStatement(insertPoint);
      assert(isSgWhileStmt(whileStmt) && "expected a while statement");

      // Insert assignments to sec_lb and sec_ub (the local iteration 
      // space computation).
      SgStatement *ns = SageBuilder::buildNullStatement();
      SageInterface::insertStatementBefore(insertPoint, ns);
      insertPoint = ns;
 
      buildLocalIterSpaceComputation(insertPoint, fdef,
          SageBuilder::buildIntVal(0), 
          SageBuilder::buildIntVal(adjSectionCount),
          SageBuilder::buildIntVal(1),
          SageBuilder::buildVarRefExp(lbDecl),
          SageBuilder::buildVarRefExp(ubDecl),
          false /* isDistribute */);

      SageInterface::replaceExpression(fce, 
          SageBuilder::buildVarRefExp(lbDecl), false /* keepOldExp */);
     
      // Adjust while-stmt test.
      SgStatement *condStmt = 
          SageInterface::getLoopCondition(isSgWhileStmt(whileStmt));
      assert(condStmt && "loop cond not SgStatement?");
      SgBinaryOp *cexpr = 0;
      assert(isSgExprStatement(condStmt) && "loop cond not SgExprStatement?");
      cexpr =
          isSgGreaterOrEqualOp(isSgExprStatement(condStmt)->get_expression());
      assert(cexpr);
      SgExpression *newCond = 
          SageBuilder::buildLessOrEqualOp(
              SageInterface::deepCopy(cexpr->get_lhs_operand()),
              SageBuilder::buildVarRefExp(ubDecl));
      SageInterface::replaceExpression(cexpr, newCond, false /* keepOldExp */);

    } else if (calleeName == "XOMP_sections_next") {

      //
      // Rewrite:
      //  xomp_section_1 = XOMP_sections_next();
      // as:
      //  xomp_section_1 += 1;
      //
      SgBinaryOp *asgOp = isSgAssignOp(fce->get_parent());
      SgExpression *newAsg = 
          SageBuilder::buildPlusAssignOp(
              SageInterface::deepCopy(asgOp->get_lhs_operand()),
              SageBuilder::buildIntVal(1));
      SageInterface::replaceExpression(asgOp, newAsg, false /* keepOldExp */);

    } else if (calleeName == "omp_get_num_threads") {

      // Replace omp_get_num_threads call with __htc_my_omp_team_size.
      SgExpression *nexpr = getOmpTeamSizeExpr(fdef, calleeName);
      assert(nexpr);
      SageInterface::replaceExpression(fce, nexpr, false /* keepOldExp */);

    } else if (calleeName == "omp_get_thread_num") {

      // Replace omp_get_thread_num call with __htc_my_omp_thread_num.
      SgExpression *nexpr = getOmpTidExpr(fdef, calleeName);
      assert(nexpr);
      SageInterface::replaceExpression(fce, nexpr, false /* keepOldExp */);

    } else if (calleeName == "omp_get_team_num") {

      // Replace omp_get_team_num call with SR_unitId.
      SgExpression *nexpr = SageBuilder::buildVarRefExp(declUnitId);
      SageInterface::replaceExpression(fce, nexpr, false /* keepOldExp */);

    } else if (calleeName == "omp_get_num_teams") {

      // Replace omp_get_num_teams call with __htc_my_omp_league_size.
      SgExpression *nexpr = getOmpLeagueSizeExpr(fdef);
      assert(nexpr);
      SageInterface::replaceExpression(fce, nexpr, false /* keepOldExp */);

    } else if (calleeName == "omp_get_num_devices") {

      SgExpression *nexpr = SageBuilder::buildIntVal(0);
      SageInterface::replaceExpression(fce, nexpr, false /* keepOldExp */);

    } else if (calleeName == "omp_is_initial_device") {

      SgExpression *nexpr = SageBuilder::buildIntVal(0);
      SageInterface::replaceExpression(fce, nexpr, false /* keepOldExp */);

    } else if (calleeName == "XOMP_master" || calleeName == "XOMP_single") {

      // Replace call with comparison of __htc_my_omp_thread_num against 0.
      SgExpression *nexpr = getOmpTidExpr(fdef, calleeName);
      assert(nexpr);
      SgExpression *cmp =
          SageBuilder::buildEqualityOp(nexpr, SageBuilder::buildIntVal(0));
      SageInterface::replaceExpression(fce, cmp, false /* keepOldExp */);

    } else if (calleeName == "XOMP_critical_start"
               || calleeName == "XOMP_critical_end") {
 
      // Build the new 'rhomp_set_lock(0)' (or unset) call statement.
      // 
      // TODO: We are currently ignoring the name and treating every critical
      // as if it was an unnamed one.
      SgStatement *insertPoint = HtSageUtils::findSafeInsertPoint(fce);
      std::string lockfn = (calleeName == "XOMP_critical_start" ?
          "rhomp_set_lock" : "rhomp_unset_lock");
      LockFunctionsInUse.insert(lockfn);
      LockFunctionsInUse.insert("__htc_lock");
      SgExprStatement *callStmt = 
          SageBuilder::buildFunctionCallStmt(lockfn,
          SageBuilder::buildVoidType(),
          SageBuilder::buildExprListExp(SageBuilder::buildIntVal(0)), 
          fdef->get_body());
      SageInterface::insertStatementAfter(insertPoint, callStmt);

      // Comment original XOMP call for documentation.
      SgStatement *nstmt = SageBuilder::buildNullStatement();
      SageInterface::attachComment(nstmt,
          fce->get_parent()->unparseToString(),
          PreprocessingInfo::before);
      SageInterface::replaceStatement(isSgStatement(fce->get_parent()), nstmt); 

    } else if (calleeName == "XOMP_atomic_start"
               || calleeName == "XOMP_atomic_end") {
 
      // Build the new 'rhomp_set_lock(1)' (or unset) call statement.
      SgStatement *insertPoint = HtSageUtils::findSafeInsertPoint(fce);
      std::string lockfn = (calleeName == "XOMP_atomic_start" ?
          "rhomp_set_lock" : "rhomp_unset_lock");
      LockFunctionsInUse.insert(lockfn);
      LockFunctionsInUse.insert("__htc_lock");
      SgExprStatement *callStmt = 
          SageBuilder::buildFunctionCallStmt(lockfn,
          SageBuilder::buildVoidType(),
          SageBuilder::buildExprListExp(SageBuilder::buildIntVal(1)), 
          fdef->get_body());
      SageInterface::insertStatementAfter(insertPoint, callStmt);

      // Comment original XOMP call for documentation.
      SgStatement *nstmt = SageBuilder::buildNullStatement();
      SageInterface::attachComment(nstmt,
          fce->get_parent()->unparseToString(),
          PreprocessingInfo::before);
      SageInterface::replaceStatement(isSgStatement(fce->get_parent()), nstmt); 
    } else if (calleeName == "omp_init_lock") {

        // Rewrite 
        //    omp_init_lock(&lck)
        // as
        //    lck = rhomp_init_lock()
        //
        // We do not have the ability to take addresses in HT.
        // Our omp_lock_t is a simple int.  An available one is
        // returned from rhomp_init_lock().  We assign this result
        // to the lock variable (with the '&' operator removed).

        SgExpression *arg = fce->get_args()->get_expressions().at(0);
        if (isSgAddressOfOp(arg)) {
            SgAddressOfOp *addrOf = isSgAddressOfOp(arg);
            // Skip the '&' operator
            SgExpression *kid = addrOf->get_operand();
            SgStatement *insertPoint = HtSageUtils::findSafeInsertPoint(fce);
            std::string lockfn = "rh" + calleeName;
            LockFunctionsInUse.insert(lockfn);
            SgExpression *callExpr = 
                SageBuilder::buildFunctionCallExp(
                                 lockfn,
                                 lock_typedef->get_type(),
                                 SageBuilder::buildExprListExp(),
                                 fdef->get_body());
            // Assign the result from rhomp_init_lock to the lock variable
            SgExprStatement *assign = 
                SageBuilder::buildAssignStatement(kid, callExpr);
                                                  
            SageInterface::insertStatementAfter(insertPoint, assign);

            // Comment original XOMP call for documentation.
            SgStatement *nstmt = SageBuilder::buildNullStatement();
            SageInterface::attachComment(nstmt,
                                         fce->get_parent()->unparseToString(),
                                         PreprocessingInfo::before);
            SageInterface::replaceStatement(isSgStatement(fce->get_parent()), 
                                            nstmt); 
        } else {
            std::cerr << "DEVWARN: " << calleeName << 
                " with non-& parameter not yet implemented."
                      << std::endl;
        }
    } else if (calleeName == "omp_test_lock") {

        // Rewrite
        //    omp_test_lock(&lck)
        // as
        //    rhomp_test_lock(lck)
        //
        // HT locks are simple ints.   We remove the '&' operator,
        // and prefix the function name with "rh".

        SgExpression *arg = fce->get_args()->get_expressions().at(0);
        if (isSgAddressOfOp(arg)) {
            SgAddressOfOp *addrOf = isSgAddressOfOp(arg);
            // Skip the '&' operator
            SgExpression *kid = addrOf->get_operand();
            SgStatement *insertPoint = HtSageUtils::findSafeInsertPoint(fce);
            std::string lockfn = "rh" + calleeName;

            if (calleeName == "omp_destroy_lock") {
                LockFunctionsInUse.insert("rhomp_init_lock");
            } else {
                LockFunctionsInUse.insert(lockfn);
                LockFunctionsInUse.insert("__htc_lock");
            }
            SgFunctionCallExp *callStmt = 
                SageBuilder::buildFunctionCallExp(
                                 lockfn,
                                 SageBuilder::buildIntType(),
                                 SageBuilder::buildExprListExp(kid),
                                 fdef->get_body());
            SageInterface::replaceExpression(fce, callStmt, true);

            // Comment original XOMP call for documentation.
            SgStatement *nstmt = SageBuilder::buildNullStatement();
            SageInterface::insertStatementAfter(insertPoint, nstmt);
            SageInterface::attachComment(nstmt,
                                         fce->unparseToString(),
                                         PreprocessingInfo::before);
        } else {
            std::cerr << "DEVWARN: " << calleeName << 
                " with non-& parameter not yet implemented."
                      << std::endl;
        }
    } else if (calleeName == "omp_set_lock" ||
               calleeName == "omp_unset_lock" ||
               calleeName == "omp_destroy_lock") {

        // Rewrite
        //    omp_*_lock(&lck)
        // as
        //    rhomp_*lock(lck)
        //
        // HT locks are simple ints.   We remove the '&' operator,
        // and prefix the function name with "rh".

        SgExpression *arg = fce->get_args()->get_expressions().at(0);
        if (isSgAddressOfOp(arg)) {
            SgAddressOfOp *addrOf = isSgAddressOfOp(arg);
            // Skip the '&' operator
            SgExpression *kid = addrOf->get_operand();
            SgStatement *insertPoint = HtSageUtils::findSafeInsertPoint(fce);
            std::string lockfn = "rh" + calleeName;

            if (calleeName == "omp_destroy_lock") {
                LockFunctionsInUse.insert("rhomp_init_lock");
            } else {
                LockFunctionsInUse.insert(lockfn);
                LockFunctionsInUse.insert("__htc_lock");
            }
            SgExprStatement *callStmt = 
                SageBuilder::buildFunctionCallStmt(
                                 lockfn,
                                 SageBuilder::buildVoidType(),
                                 SageBuilder::buildExprListExp(kid),
                                 fdef->get_body());
            SageInterface::insertStatementAfter(insertPoint, callStmt);

            // Comment original XOMP call for documentation.
            SgStatement *nstmt = SageBuilder::buildNullStatement();
            SageInterface::attachComment(nstmt,
                                         fce->get_parent()->unparseToString(),
                                         PreprocessingInfo::before);
            SageInterface::replaceStatement(isSgStatement(fce->get_parent()), 
                                            nstmt); 
        } else {
            std::cerr << "DEVWARN: " << calleeName << 
                " with non-& parameter not yet implemented."
                      << std::endl;
        }
    } else {

      std::cerr << "DEVWARN: " << calleeName << " not yet implemented."
          << std::endl;

    }

  }

  rewriteList.clear();
}


SgStatement *RewriteOmp::buildComputeMinUpperBound(SgExpression *lower,
    SgExpression *upper, SgExpression *n_lower, SgExpression *n_upper) {
  //
  // Compute the minimum of two upper bounds.  The generated code handles
  // both up- and down-counting loops.
  //
  // if (lower > upper) {
  //   n_upper = n_upper + 1;
  //   if (upper > n_upper) {
  //     n_upper = upper;
  //   }
  // } else {
  //   n_upper = n_upper - 1;
  //   if (upper < n_upper) {
  //     n_upper = upper;
  //   }
  // }
  //
  SgStatement *ifstmt = SageBuilder::buildIfStmt(
      SageBuilder::buildExprStatement(SageBuilder::buildGreaterThanOp(
          SageInterface::deepCopy(lower),
          SageInterface::deepCopy(upper))),
      SageBuilder::buildBasicBlock(
          SageBuilder::buildAssignStatement(
              SageInterface::deepCopy(n_upper),
              SageBuilder::buildAddOp(
                  SageInterface::deepCopy(n_upper),
                  SageBuilder::buildIntVal(1))),
          SageBuilder::buildIfStmt(
          SageBuilder::buildExprStatement(SageBuilder::buildGreaterThanOp(
              SageInterface::deepCopy(upper),
              SageInterface::deepCopy(n_upper))),
          SageBuilder::buildBasicBlock(SageBuilder::buildAssignStatement(
              SageInterface::deepCopy(n_upper),
              SageInterface::deepCopy(upper))),
          SageBuilder::buildBasicBlock())),
      SageBuilder::buildBasicBlock(
          SageBuilder::buildAssignStatement(
              SageInterface::deepCopy(n_upper),
              SageBuilder::buildSubtractOp(
                  SageInterface::deepCopy(n_upper), 
                  SageBuilder::buildIntVal(1))),
          SageBuilder::buildIfStmt(
          SageBuilder::buildExprStatement(SageBuilder::buildLessThanOp(
              SageInterface::deepCopy(upper),
              SageInterface::deepCopy(n_upper))),
          SageBuilder::buildBasicBlock(SageBuilder::buildAssignStatement(
              SageInterface::deepCopy(n_upper),
              SageInterface::deepCopy(upper))),
          SageBuilder::buildBasicBlock())));

  return ifstmt;
}


void RewriteOmp::buildLocalIterSpaceComputation(
    SgStatement *insertPoint, SgFunctionDefinition *fdef,
    SgExpression *lower, SgExpression *upper, SgExpression *stride,
    SgExpression *n_lower, SgExpression *n_upper, bool isDistribute) {
  //
  // For the static schedule case with unspecified chunk_size, compute
  // the local iteration space for each thread.  The original loop runs
  // from 'lower' to 'upper' by 'stride'.
  //
  // The loop comparison is assumed to be inclusive (i.e., <= or >=).
  // This is the static case with unspecified chunk_size.
  //
  // Insert the code after 'insertPoint'.
  //
  //    int chunk_size, trip_count;
  //    ...
  //    trip_count = (stride + upper - lower) / stride;
  //    chunk_size = trip_count / omp_get_num_threads();
  //    chunk_size = ((chunk_size * omp_get_num_threads() != trip_count)
  //                  + chunk_size) * stride;
  //    n_lower = lower + chunk_size * omp_get_thread_num();
  //    n_upper = n_lower + chunk_size;
  //  
  //    if (lower > upper) {
  //      n_upper = n_upper + 1;
  //      n_upper = (n_upper > upper ? n_upper : upper);
  //    } else {
  //      n_upper = n_upper - 1;
  //      n_upper = (n_upper < upper ? n_upper : upper);
  //    }
  //

  // Replace omp_get_num_threads calls with __htc_my_omp_team_size.
  SgExpression *teamSz = 0;
  SgExpression *tidExpr = 0;
  if (isDistribute) {
    teamSz = getOmpLeagueSizeExpr(fdef);
    tidExpr = SageBuilder::buildVarRefExp("SR_unitId", fdef);
  } else {
    teamSz = getOmpTeamSizeExpr(fdef, "loop_default/section_init");
    tidExpr = getOmpTidExpr(fdef, "loop_default/section_init");
  }
  assert(teamSz);
  assert(tidExpr);

  // Build decls for the chunk_size and trip_count.
  SgName chunkName = "__htc_chunksz";
  chunkName << ++SageInterface::gensym_counter; 
#if 0
  SgType *ivarType = SageBuilder::buildLongType();  // TODO: long?
#else
  SgType *ivarType = lower->get_type();
#endif
  SgVariableDeclaration *chunkDecl = 
      SageBuilder::buildVariableDeclaration(chunkName, ivarType, 0, fdef);
  SageInterface::prependStatement(chunkDecl, fdef->get_body());

  SgName tripName = "__htc_tripcount";
  tripName << ++SageInterface::gensym_counter; 
  SgVariableDeclaration *tripDecl = 
      SageBuilder::buildVariableDeclaration(tripName, ivarType, 0, fdef);
  SageInterface::prependStatement(tripDecl, fdef->get_body());

  // trip_count = (stride + upper - lower) / stride;
  SgStatement *tripAsg = SageBuilder::buildAssignStatement(
      SageBuilder::buildVarRefExp(tripDecl),
      SageBuilder::buildDivideOp(
          SageBuilder::buildAddOp(
              SageInterface::deepCopy(stride),
              SageBuilder::buildSubtractOp(
                  SageInterface::deepCopy(upper),
                  SageInterface::deepCopy(lower))),
          SageInterface::deepCopy(stride)));
  SageInterface::insertStatementAfter(insertPoint, tripAsg);

  // chunk_size = trip_count / omp_get_num_threads();
  SgStatement *chunkAsg1 = SageBuilder::buildAssignStatement(
      SageBuilder::buildVarRefExp(chunkDecl),
      SageBuilder::buildDivideOp(
          SageBuilder::buildVarRefExp(tripDecl),
          teamSz));
  SageInterface::insertStatementAfter(tripAsg, chunkAsg1);

  // chunk_size = ((chunk_size * omp_get_num_threads() != trip_count)
  //               + chunk_size) * stride;
  SgStatement *chunkAsg2 = SageBuilder::buildAssignStatement(
      SageBuilder::buildVarRefExp(chunkDecl),
      SageBuilder::buildMultiplyOp(
          SageBuilder::buildAddOp(
              SageBuilder::buildNotEqualOp(
                  SageBuilder::buildMultiplyOp(
                      SageBuilder::buildVarRefExp(chunkDecl),
                      SageInterface::deepCopy(teamSz)),
                  SageBuilder::buildVarRefExp(tripDecl)),
              SageBuilder::buildVarRefExp(chunkDecl)),
          SageInterface::deepCopy(stride)));
  SageInterface::insertStatementAfter(chunkAsg1, chunkAsg2);

  // n_lower = lower + chunk_size * omp_get_thread_num();
  SgStatement *nlowerAsg = SageBuilder::buildAssignStatement(
      SageInterface::deepCopy(n_lower),
      SageBuilder::buildAddOp(
          SageInterface::deepCopy(lower),
          SageBuilder::buildMultiplyOp(
              SageBuilder::buildVarRefExp(chunkDecl),
              tidExpr)));
  SageInterface::insertStatementAfter(chunkAsg2, nlowerAsg);

  // n_upper = n_lower + chunk_size;
  SgStatement *nupperAsg1 = SageBuilder::buildAssignStatement(
      SageInterface::deepCopy(n_upper),
      SageBuilder::buildAddOp(
          SageInterface::deepCopy(n_lower),
          SageBuilder::buildVarRefExp(chunkDecl)));
  SageInterface::insertStatementAfter(nlowerAsg, nupperAsg1);

  SgStatement *ifstmt = buildComputeMinUpperBound(lower, upper, n_lower,
      n_upper);

  SageInterface::insertStatementAfter(nupperAsg1, ifstmt);
}


void RewriteOmp::buildLocalIterSpaceComputation_StaticSpec(
    SgStatement *insertPoint, SgFunctionDefinition *fdef,
    SgExpression *lower, SgExpression *upper, SgExpression *stride,
    SgExpression *chunk_size, SgExpression *n_lower, SgExpression *n_upper,
    SgVariableDeclaration *chunkDecl, bool isDistribute, bool doChunkSpecial) {

  //
  // For the static schedule case with specified chunk_size, compute
  // the local iteration space for each thread.  The original loop runs
  // from 'lower' to 'upper' by 'stride'.
  //
  // The loop comparison is assumed to be inclusive (i.e., <= or >=).
  //
  // Insert the code after 'insertPoint'.
  //
  //    int next_chunk;
  //    ...
  //    n_lower = lower + chunk_size * stride * omp_get_thread_num();
  //    n_upper = n_lower + chunk_size * stride;
  //    next_chunk = chunk_size * stride * team_size;
  //  
  // For chunk_size = 1, the code is simplified.
  //

  bool chunkIsOne = doChunkSpecial
      && isSgIntVal(chunk_size) && isSgIntVal(chunk_size)->get_value() == 1;

  // Replace omp_get_num_threads calls with __htc_my_omp_team_size.
  SgExpression *teamSz = 0;
  SgExpression *tidExpr = 0;
  if (isDistribute) {
    teamSz = getOmpLeagueSizeExpr(fdef);
    tidExpr = SageBuilder::buildVarRefExp("SR_unitId", fdef);
  } else {
    teamSz = getOmpTeamSizeExpr(fdef, "loop_static_init");
    tidExpr = getOmpTidExpr(fdef, "loop_static_init");
  }
  assert(teamSz);
  assert(tidExpr);

  // n_lower = lower + chunk_size * stride * omp_get_thread_num();
  SgExpression *factor = 0;
  if (chunkIsOne) {
    factor = SageInterface::deepCopy(stride);
  } else {
    factor = SageBuilder::buildMultiplyOp(
        SageInterface::deepCopy(chunk_size),
        SageInterface::deepCopy(stride));
  }
  SgStatement *nlowerAsg = SageBuilder::buildAssignStatement(
      SageInterface::deepCopy(n_lower),
      SageBuilder::buildAddOp(
          SageInterface::deepCopy(lower),
          SageBuilder::buildMultiplyOp(
              factor, 
              tidExpr)));
  SageInterface::insertStatementAfter(insertPoint, nlowerAsg);

  // n_upper = upper;                                (chunk_size = 1),
  //   or
  // n_upper = n_lower + chunk_size * stride;        (chunk_size > 1)
  SgStatement *nupperAsg = 0;
  if (chunkIsOne) {
    nupperAsg = SageBuilder::buildAssignStatement(
        SageInterface::deepCopy(n_upper),
        SageInterface::deepCopy(upper));
  } else {
    nupperAsg = SageBuilder::buildAssignStatement(
    SageInterface::deepCopy(n_upper),
    SageBuilder::buildAddOp(
        SageInterface::deepCopy(n_lower),
        SageInterface::deepCopy(factor)));
  }
  SageInterface::insertStatementAfter(nlowerAsg, nupperAsg);

  // next_chunk = chunk_size * stride * team_size;
  SgStatement *nextChunkAsg = SageBuilder::buildAssignStatement(
      SageBuilder::buildVarRefExp(chunkDecl),
      SageBuilder::buildMultiplyOp(
          SageInterface::deepCopy(factor),
          teamSz));
  SageInterface::insertStatementAfter(nupperAsg, nextChunkAsg);
}


void RewriteOmp::buildThreadSpawnLoop(SgFunctionDefinition *fdef,
    SgFunctionCallExp *fce, std::vector<SgExpression *> &vv, 
    SgVariableDeclaration *declHtId)
{
  SgGlobal *GS = SageInterface::getGlobalScope(fdef);
  SgName outFuncName = isSgFunctionRefExp(vv[0])->get_symbol()->get_name();

  //
  // Rewrite XOMP_parallel_start(OUT_func, data, ifclause, numthreads,
  //   filestring, fileline) to spawn a new team of threads.
  //
  // Spawn a new team of threads, assign omp thread IDs from 0 to N-1, etc.
  //
  //    ...
  //    piv = 0;              // Loop iv and new OMP thread number.
  //  fork_loop_top:
  //    if (!(piv < numthreads)) {
  //      RecvReturnPause_par(fork_loop_exit);
  //    }
  //    OUT_func(PR_htId, piv, numthreads); // SendCallFork_OUT_func(
  //                                    fork_loop_join_cycle, PR_htId, piv);
  //    piv++;
  //    goto fork_loop_top;
  //  fork_loop_join_cycle:
  //    RecvReturnJoin_par();  // thread terminates.
  //  fork_loop_exit:
  //    ...
  //
  
  // Insert point is just before the statement enclosing the original
  // call expression.
  SgStatement *insertPoint = HtSageUtils::findSafeInsertPoint(fce);

  // Build decl for the new induction variable.
  SgName ivarName = "__htc_t_piv";
  ivarName << ++SageInterface::gensym_counter; 
  SgType *ivarType = 0;
  if (SgTypedefSymbol *tdsym = GS->lookup_typedef_symbol("int16_t")) {
    ivarType = tdsym->get_type(); 
  } else {
    ivarType = SageBuilder::buildShortType();
  }
  SgVariableDeclaration *ivarDecl = 
      SageBuilder::buildVariableDeclaration(ivarName, ivarType, 0, fdef);
  SageInterface::prependStatement(ivarDecl, fdef->get_body());

  // Build decl for the number of threads to fork (loop UB).
  SgName ntName = "__htc_t_nt";
  ntName << ++SageInterface::gensym_counter; 
  SgVariableDeclaration *ntDecl = 
      SageBuilder::buildVariableDeclaration(ntName, ivarType, 0, fdef);
  SageInterface::prependStatement(ntDecl, fdef->get_body());

  // Compute number of threads to fork, incorporating the OpenMP
  // num_threads and if clauses.
  // Build:
  //    if (!(<if_clause_expr>)) {
  //      __htc_t_nt = 1;
  //    } else { 
  //      __htc_t_nt = <num_threads_clause>;   // or default val if unspec
  //    }
  // Don't generate if-stmt when if-clause is not specified.
  // TODO: Incorporate ICVs such as max_levels, is_nested, etc.  We do
  // not yet have the notion of ICVs in Ht OMP.
  SgExpression *ifClauseExpr = 
      SageInterface::deepCopy(isSgExpression(vv[2]));
  SgExpression *numthreadsExpr = 0;
  if (isSgIntVal(vv[3]) && (isSgIntVal(vv[3])->get_value() == 0)) {
    numthreadsExpr = SageBuilder::buildIntVal(8);
  } else {
    numthreadsExpr = SageInterface::deepCopy(isSgExpression(vv[3]));
  }
  SgStatement *asgNtSingle = SageBuilder::buildAssignStatement(
      SageBuilder::buildVarRefExp(ntDecl), SageBuilder::buildIntVal(1));
  SgStatement *asgNtActual = SageBuilder::buildAssignStatement(
      SageBuilder::buildVarRefExp(ntDecl), numthreadsExpr);
  SgStatement *outerIfStmt = 0;
  if (isSgIntVal(ifClauseExpr)
      && (isSgIntVal(ifClauseExpr)->get_value() == 1)) {
    outerIfStmt = asgNtActual;
  } else {
    SgExpression *outerCond = SageBuilder::buildNotOp(ifClauseExpr);
    outerIfStmt = SageBuilder::buildIfStmt(
        SageBuilder::buildExprStatement(outerCond),
        SageBuilder::buildBasicBlock(asgNtSingle),
        SageBuilder::buildBasicBlock(asgNtActual));
  }
  SageInterface::insertStatementAfter(insertPoint, outerIfStmt);

  // Build explict loop to fork new threads.
  SgStatement *forInitStmt = SageBuilder::buildAssignStatement(
      SageBuilder::buildVarRefExp(ivarDecl), SageBuilder::buildIntVal(0));
  SgStatement *forTestStmt = SageBuilder::buildExprStatement(
      SageBuilder::buildLessThanOp(SageBuilder::buildVarRefExp(ivarDecl),
      SageBuilder::buildVarRefExp(ntDecl)));
  SgExpression *forIncrExp = SageBuilder::buildPlusPlusOp(
      SageBuilder::buildVarRefExp(ivarDecl), SgUnaryOp::postfix);
  SgScopeStatement *loopBB = SageBuilder::buildBasicBlock();

  // Construct the label statements.
  int ln = ++SageInterface::gensym_counter;
  SgLabelStatement *labelTopStmt = HtSageUtils::makeLabelStatement(
      "fork_loop_top", ln, fdef);
  SgLabelStatement *labelExitStmt = HtSageUtils::makeLabelStatement(
      "fork_loop_exit", ln, fdef);
  SgLabelStatement *labelJoinCycleStmt = HtSageUtils::makeLabelStatement(
      "fork_loop_join_cycle", ln, fdef);

  // Build the 'void RecvReturnPause_foo(int)' decl for the Ht barrier.
  std::string recvPauseName = "RecvReturnPause_" + outFuncName;
  SgFunctionDeclaration *recvPauseDecl = 
  HtDeclMgr::buildHtlFuncDecl_generic(recvPauseName, 'v', "i", fdef);
#if 0
  recvPauseDecl->setOutputInCodeGeneration();
#endif

  // Build the new 'RecvReturnPause_foo' call statement.
  SgExpression *placeholder = SageBuilder::buildIntVal(-999);
  SgExprStatement *rpCallStmt = 
      SageBuilder::buildFunctionCallStmt(recvPauseName, 
      SageBuilder::buildVoidType(),
      SageBuilder::buildExprListExp(placeholder), fdef->get_body());

  // Mark the placeholder expression with an FsmPlaceHolderAttribute to 
  // indicate a deferred FSM name is needed.
  FsmPlaceHolderAttribute *attr = new FsmPlaceHolderAttribute(labelExitStmt);
  placeholder->addNewAttribute("fsm_placeholder", attr);

  // Insert init statement and "fork_loop_top:" statement.
  SageInterface::insertStatementAfter(outerIfStmt, forInitStmt);
  SageInterface::insertStatementAfter(forInitStmt, labelTopStmt);

  // Build "if (!<<cond>>) { RecvReturPause_foo(loop_exit); }" subtree.
  SgExpression *cexpr = isSgExprStatement(forTestStmt)->get_expression();
  SgExpression *newcond = SageBuilder::buildNotOp(cexpr);
  SgStatement *ifstmt = SageBuilder::buildIfStmt(
      SageBuilder::buildExprStatement(newcond),
      SageBuilder::buildBasicBlock(rpCallStmt), 0 /* else body empty */);
  SageInterface::insertStatementAfter(labelTopStmt, ifstmt);
  SageInterface::insertStatementAfter(ifstmt, loopBB);
   
  // Insert incr statement.
  SgStatement *incrStmt = SageBuilder::buildExprStatement(forIncrExp);
  SageInterface::insertStatementAfter(loopBB, incrStmt);

  // Build and insert "goto loop_top; loop_join_cycle: loop_exit:".
  SgStatement *sgoto = SageBuilder::buildGotoStatement(labelTopStmt);
  SageInterface::insertStatementAfter(incrStmt, sgoto);
  SageInterface::insertStatementAfter(sgoto, labelJoinCycleStmt);
  SageInterface::insertStatementAfter(labelJoinCycleStmt, labelExitStmt);

  // Build the 'RecvReturnJoin_foo' decl for the special join cycle state.
  std::string recvJoinName = "RecvReturnJoin_" + outFuncName;
  SgFunctionDeclaration *recvJoinDecl = 
      HtDeclMgr::buildHtlFuncDecl_generic(recvJoinName, 'v', "v", fdef);
#if 0
  recvJoinDecl->setOutputInCodeGeneration();
#endif

  // Build the new 'RecvReturnJoin_foo' call statement and insert it just
  // after the loop join cycle label.
  SgExprStatement *rjCallStmt = 
      SageBuilder::buildFunctionCallStmt(recvJoinName, 
      SageBuilder::buildVoidType(), SageBuilder::buildExprListExp(),
      fdef->get_body());
  SageInterface::insertStatementAfter(labelJoinCycleStmt, rjCallStmt);

  // Insert a call to the outlined routine in the loop body.
  // The outlined function prototype is this:
  //   OUT_foo(int __htc_arg_parentHtId, int __htc_arg_myOmpTid, 
  //           int __htc_team_size);
  // NOTE: The (int) cast is to satisfy htv strictness.
  SgExprStatement *newCallStmt =  
      SageBuilder::buildFunctionCallStmt(outFuncName, 
      SageBuilder::buildVoidType(), SageBuilder::buildExprListExp(
          SageBuilder::buildCastExp(
              SageBuilder::buildVarRefExp(declHtId),
              SCDecls->get_htc_tid_t_type()),
          SageBuilder::buildCastExp(
              SageBuilder::buildVarRefExp(ivarDecl),
              SCDecls->get_htc_tid_t_type()),
          SageBuilder::buildCastExp(
              SageBuilder::buildVarRefExp(ntDecl),
              SCDecls->get_htc_tid_t_type()),
          getOmpLeagueSizeExpr(fdef)),
      fdef->get_body());
  SageInterface::appendStatement(newCallStmt, loopBB);

  // Mark the parallel call with a ParallelCallAttribute for ProcessCalls.
  ParallelCallAttribute *pattr =
      new ParallelCallAttribute(labelJoinCycleStmt);
  (newCallStmt->get_expression())->addNewAttribute("parallel_call", pattr);
}


// Visit each call expression in post order, creating an ordered list
// of XOMP calls to be lowered.
void RewriteOmp::postVisitSgFunctionCallExp(SgFunctionCallExp *FCE)
{
  SgFunctionDeclaration *calleeFD = FCE->getAssociatedFunctionDeclaration();
  if (!calleeFD) {
    // Call through function pointer.
    return;
  }

  std::string calleeName = calleeFD->get_name().getString();

  size_t pos = 0;
  size_t np = std::string::npos;
  if (((pos = calleeName.find("XOMP_")) != np && pos == 0)
      || calleeName == "omp_set_num_threads"
      || calleeName == "omp_get_num_threads"
      || calleeName == "omp_get_thread_num"
      || calleeName == "omp_get_team_num"
      || calleeName == "omp_get_num_teams"
      || calleeName == "omp_get_num_devices"
      || calleeName == "omp_is_initial_device"
      || calleeName == "omp_get_max_threads"
      || calleeName == "omp_get_num_procs"
      || calleeName == "omp_in_parallel"
      || calleeName == "omp_set_dynamic"
      || calleeName == "omp_get_dynamic"
      || calleeName == "omp_get_cancellation"
      || calleeName == "omp_set_nested"
      || calleeName == "omp_get_nested"
      || calleeName == "omp_set_schedule"
      || calleeName == "omp_get_schedule"
      || calleeName == "omp_get_thread_limit"
      || calleeName == "omp_set_max_active_levels"
      || calleeName == "omp_get_max_active_levels"
      || calleeName == "omp_get_level"
      || calleeName == "omp_get_ancestor_thread_num"
      || calleeName == "omp_get_team_size "
      || calleeName == "omp_get_proc_bind"
      || calleeName == "omp_get_active_level"
      || calleeName == "omp_in_final"
      || calleeName == "omp_init_lock"
      || calleeName == "omp_destroy_lock"
      || calleeName == "omp_set_lock"
      || calleeName == "omp_unset_lock"
      || calleeName == "omp_test_lock"
      || calleeName == "omp_init_nest_lock"
      || calleeName == "omp_destroy_nest_lock"
      || calleeName == "omp_set_nest_lock"
      || calleeName == "omp_unset_nest_lock"
      || calleeName == "omp_test_nest_lock"
      || calleeName == "omp_get_wtime"
      || calleeName == "omp_get_wtick") { 
    rewriteList.push_back(FCE);
  }

}


SgExpression *RewriteOmp::getOmpTidExpr(SgFunctionDefinition *fdef,
                                        std::string calleeName)
{
  // Lookup __htc_my_omp_thread_num and return a SgVarRefExpr.
  SgVariableSymbol *tidSym = 
      fdef->lookup_variable_symbol("__htc_my_omp_thread_num");
  assert(tidSym && "did not find symbol for __htc_my_omp_thread_num");
  return SageBuilder::buildVarRefExp(tidSym);
}

SgExpression *RewriteOmp::getOmpTeamSizeExpr(SgFunctionDefinition *fdef,
                                             std::string calleeName)
{
  // Lookup __htc_my_omp_team_size and return a SgVarRefExpr.
  SgVariableSymbol *teamSym = 
      fdef->lookup_variable_symbol("__htc_my_omp_team_size");
  assert(teamSym && "did not find symbol for __htc_my_omp_team_size");
  return SageBuilder::buildVarRefExp(teamSym);
}

SgExpression *RewriteOmp::getOmpLeagueSizeExpr(SgFunctionDefinition *fdef)
{
  // Lookup __htc_my_omp_league_size and return a SgVarRefExpr.
  SgVariableSymbol *leagueSym = 
      fdef->lookup_variable_symbol("__htc_my_omp_league_size");
  assert(leagueSym && "did not find symbol for __htc_my_omp_league_size");
  return SageBuilder::buildVarRefExp(leagueSym);
}


SgExpression *RewriteOmp::getParentHtidExpr(SgFunctionDefinition *fdef)
{
  // Lookup __htc_parent_htid and return a SgVarRefExpr.
  SgVariableSymbol *parTidSym = 
      fdef->lookup_variable_symbol("__htc_parent_htid");
  assert(parTidSym && "did not find symbol for __htc_parent_htid");
  return SageBuilder::buildVarRefExp(parTidSym);
}


void DoLateOmpLowering(SgProject *project)
{
  //
  // Process module-width pragmas first.
  //
  std::vector<SgNode *> pragmas = 
      NodeQuery::querySubTree(project, V_SgPragmaDeclaration);
  foreach (SgNode *n, pragmas) {
    SgPragmaDeclaration *pragmaDecl = isSgPragmaDeclaration(n);
    assert(pragmaDecl);
    std::string pragmaStr = pragmaDecl->get_pragma()->get_pragma();
    // Parse "rhomp max_phys_threads(N)" where N is an integer constant.
    int max_phys_threads = -1;
    bool matched = false;
    c_char = pragmaStr.c_str();
    if (afs_match_substr("rhomp")) {
      if (afs_match_substr("max_phys_threads")) {
        if (afs_match_char('(')) {
          if (afs_match_integer_const(&max_phys_threads)) {
            if (afs_match_char(')') && max_phys_threads > 0) {
              matched = true;
            }
          }
        }
      }
      if (matched) { 
        // If pragma is in the global scope, then it is setting the default
        // width for every module in the translation unit.  Otherwise, it is
        // setting the width for only the module where it occurs.
        if (isSgGlobal(SageInterface::getScope(pragmaDecl))) {
          SgGlobal *pgs = SageInterface::getGlobalScope(pragmaDecl);
          DefaultModuleWidthAttribute *mwattr = 
              new DefaultModuleWidthAttribute(max_phys_threads);
          pgs->addNewAttribute("default_module_width", mwattr);
        } else {
          SgFunctionDefinition *tfd = 
              SageInterface::getEnclosingProcedure(pragmaDecl);
          HtdInfoAttribute *htd = getHtdInfoAttribute(tfd);
          assert(htd);
          htd->moduleWidth = max_phys_threads;
        }

        // Remove pragma, taking care of any attached PreprocessingInfo.
        if (isSgGlobal(SageInterface::getScope(pragmaDecl))) {
          AttachedPreprocessingInfoType *comments = 
              pragmaDecl->getAttachedPreprocessingInfo();
          if (comments && comments->size() > 0) {
            SgName baseName = "___htc_dummy_decl_";
            baseName << ++SageInterface::gensym_counter;
            SgVariableDeclaration *dstmt = 
                SageBuilder::buildVariableDeclaration(baseName,
                    SageBuilder::buildIntType(), 0,
                    SageInterface::getScope(pragmaDecl));
            SageInterface::setExtern(dstmt);
            SageInterface::insertStatementBefore(pragmaDecl, dstmt);
            moveUpPreprocessingInfo(dstmt, pragmaDecl);
            SageInterface::removeStatement(pragmaDecl);
          } else {
            SageInterface::removeStatement(pragmaDecl);
          }
        } else {
          SgStatement *nstmt = SageBuilder::buildNullStatement();
          SageInterface::replaceStatement(isSgStatement(pragmaDecl), 
              nstmt, true /* move preproc info */); 
        }
      } else {
        std::cerr << "WARNING: unrecognized rhomp pragma '" << pragmaStr
            << "'" << std::endl;
      }
    }
  }

  //
  // Now perform lowering.
  //
  RewriteOmp rv;
  rv.traverseInputFiles(project, STRICT_INPUT_FILE_TRAVERSAL);
}

