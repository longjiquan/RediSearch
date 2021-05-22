
#include "expression.h"
#include "result_processor.h"

///////////////////////////////////////////////////////////////////////////////////////////////

static int evalInternal(ExprEval *eval, const RSExpr *e, RSValue *res);

//---------------------------------------------------------------------------------------------

static void setReferenceValue(RSValue *dst, RSValue *src) {
  RSValue_MakeReference(dst, src);
}

//---------------------------------------------------------------------------------------------

int func_exists(ExprEval *ctx, RSValue *result, RSValue **argv, size_t argc, QueryError *err);

int ExprEval::evalFunc(const RSFunctionExpr *f, RSValue *result) {
  int rc = EXPR_EVAL_ERR;

  // First, evaluate every argument
  size_t nusedargs = 0;
  size_t nargs = f->args->len;
  RSValue *argspp[nargs];
  RSValue args[nargs];

  for (size_t ii = 0; ii < nargs; ii++) {
    args[ii] = (RSValue)RSVALUE_STATIC;
    argspp[ii] = &args[ii];
    int internalRes = evalInternal(eval, f->args->args[ii], &args[ii]);
    if (internalRes == EXPR_EVAL_ERR ||
        (internalRes == EXPR_EVAL_NULL && f->Call != func_exists)) {
      // TODO: Free other results
      goto cleanup;
    }
    nusedargs++;
  }

  // We pass an RSValue**, not an RSValue*, as the arguments
  rc = f->Call(eval, result, argspp, nargs, eval->err);

cleanup:
  for (size_t ii = 0; ii < nusedargs; ii++) {
    RSValue_Clear(&args[ii]);
  }
  return rc;
}

//---------------------------------------------------------------------------------------------

int ExprEval::evalOp(const RSExprOp *op, RSValue *result) {
  RSValue l = RSVALUE_STATIC, r = RSVALUE_STATIC;
  int rc = EXPR_EVAL_ERR;

  if (evalInternal(op->left, &l) != EXPR_EVAL_OK) {
    goto cleanup;
  }
  if (evalInternal(op->right, &r) != EXPR_EVAL_OK) {
    goto cleanup;
  }

  double n1, n2;
  if (!RSValue_ToNumber(&l, &n1) || !RSValue_ToNumber(&r, &n2)) {

    err->SetError(QUERY_ENOTNUMERIC, NULL);
    rc = EXPR_EVAL_ERR;
    goto cleanup;
  }

  double res;
  switch (op->op) {
    case '+':
      res = n1 + n2;
      break;
    case '/':
      res = n1 / n2;
      break;
    case '-':
      res = n1 - n2;
      break;
    case '*':
      res = n1 * n2;
      break;
    case '%':
      res = (long long)n1 % (long long)n2;
      break;
    case '^':
      res = pow(n1, n2);
      break;
    default:
      res = NAN;  // todo : we can not really reach here
  }

  result->numval = res;
  result->t = RSValue_Number;
  rc = EXPR_EVAL_OK;

cleanup:
  RSValue_Clear(&l);
  RSValue_Clear(&r);
  return rc;
}

//---------------------------------------------------------------------------------------------

int ExprEval::getPredicateBoolean(const RSValue *l, const RSValue *r, RSCondition op) {
  QueryError *qerr = eval->err;
  switch (op) {
    case RSCondition_Eq:
      return RSValue_Equal(l, r, qerr);

    case RSCondition_Lt:
      return RSValue_Cmp(l, r, qerr) < 0;

    // Less than or equal, <=
    case RSCondition_Le:
      return RSValue_Cmp(l, r, qerr) <= 0;

    // Greater than, >
    case RSCondition_Gt:
      return RSValue_Cmp(l, r, qerr) > 0;

    // Greater than or equal, >=
    case RSCondition_Ge:
      return RSValue_Cmp(l, r, qerr) >= 0;

    // Not equal, !=
    case RSCondition_Ne:
      return !RSValue_Equal(l, r, qerr);

    // Logical AND of 2 expressions, &&
    case RSCondition_And:
      return RSValue_BoolTest(l) && RSValue_BoolTest(r);

    // Logical OR of 2 expressions, ||
    case RSCondition_Or:
      return RSValue_BoolTest(l) || RSValue_BoolTest(r);

    default:
      RS_LOG_ASSERT(0, "invalid RSCondition");
      return 0;
  }
}

//---------------------------------------------------------------------------------------------

int ExprEval::evalInverted(const RSInverted *vv, RSValue *result) {
  RSValue tmpval = RSVALUE_STATIC;
  if (evalInternal(vv->child, &tmpval) != EXPR_EVAL_OK) {
    return EXPR_EVAL_ERR;
  }

  result->numval = !RSValue_BoolTest(&tmpval);
  result->t = RSValue_Number;

  RSValue_Clear(&tmpval);
  return EXPR_EVAL_OK;
}

//---------------------------------------------------------------------------------------------

int ExprEval::evalPredicate(const RSPredicate *pred, RSValue *result) {
  int res;
  RSValue l = RSVALUE_STATIC, r = RSVALUE_STATIC;
  int rc = EXPR_EVAL_ERR;
  if (evalInternal(pred->left, &l) != EXPR_EVAL_OK) {
    goto cleanup;
  } else if (pred->cond == RSCondition_Or && RSValue_BoolTest(&l)) {
    res = 1;
    goto success;
  } else if (pred->cond == RSCondition_And && !RSValue_BoolTest(&l)) {
    res = 0;
    goto success;
  } else if (evalInternal(pred->right, &r) != EXPR_EVAL_OK) {
    goto cleanup;
  }

  res = getPredicateBoolean(&l, &r, pred->cond);

success:
  if (!err || err->code == QUERY_OK) {
    result->numval = res;
    result->t = RSValue_Number;
    rc = EXPR_EVAL_OK;
  } else {
    result->t = RSValue_Undef;
  }

cleanup:
  RSValue_Clear(&l);
  RSValue_Clear(&r);
  return rc;
}

//---------------------------------------------------------------------------------------------

int ExprEval::evalProperty(const RSLookupExpr *e, RSValue *res) {
  if (!e->lookupObj) {
    // todo : this can not happened
    // No lookup object. This means that the key does not exist
    // Note: Because this is evaluated for each row potentially, do not assume
    // that query error is present:
    if (err) {
      err->SetError(QUERY_ENOPROPKEY, NULL);
    }
    return EXPR_EVAL_ERR;
  }

  // Find the actual value
  RSValue *value = RLookup_GetItem(e->lookupObj, srcrow);
  if (!value) {
    if (err) {
      err->SetError(QUERY_ENOPROPVAL, NULL);
    }
    res->t = RSValue_Null;
    return EXPR_EVAL_NULL;
  }

  setReferenceValue(res, value);
  return EXPR_EVAL_OK;
}

//---------------------------------------------------------------------------------------------

int ExprEval::evalInternal(const RSExpr *e, RSValue *res) {
  RSValue_Clear(res);
  switch (e->t) {
    case RSExpr_Property:
      return evalProperty(eval, &e->property, res);
    case RSExpr_Literal:
      RSValue_MakeReference(res, (RSValue *)&e->literal);
      return EXPR_EVAL_OK;
    case RSExpr_Function:
      return evalFunc(eval, &e->func, res);
    case RSExpr_Op:
      return evalOp(eval, &e->op, res);
    case RSExpr_Predicate:
      return evalPredicate(eval, &e->pred, res);
    case RSExpr_Inverted:
      return evalInverted(eval, &e->inverted, res);
  }
  return EXPR_EVAL_ERR;  // todo: this can not happened
}

//---------------------------------------------------------------------------------------------

int ExprEval::Eval(RSValue *result) {
  return evalInternal(root, result);
}

//---------------------------------------------------------------------------------------------

int ExprAST_GetLookupKeys(RSExpr *expr, RLookup *lookup, QueryError *err) {

#define RECURSE(v)                                                                    \
  do {                                                                                \
    if (!v) {                                                                         \
      err->SetErrorFmt(QUERY_EEXPR, "Missing (or badly formatted) value for %s", #v); \
      return EXPR_EVAL_ERR;                                                           \
    }                                                                                 \
    if (ExprAST_GetLookupKeys(v, lookup, err) != EXPR_EVAL_OK) {                      \
      return EXPR_EVAL_ERR;                                                           \
    }                                                                                 \
  while (0)

  switch (expr->t) {
    case RSExpr_Property:
      expr->property.lookupObj = RLookup_GetKey(lookup, expr->property.key, RLOOKUP_F_NOINCREF);
      if (!expr->property.lookupObj) {
        err->SetErrorFmt(QUERY_ENOPROPKEY, "Property `%s` not loaded in pipeline",
                               expr->property.key);
        return EXPR_EVAL_ERR;
      }
      break;
    case RSExpr_Function:
      for (size_t ii = 0; ii < expr->func.args->len; ii++) {
        RECURSE(expr->func.args->args[ii])
      }
      break;
    case RSExpr_Op:
      RECURSE(expr->op.left);
      RECURSE(expr->op.right);
      break;
    case RSExpr_Predicate:
      RECURSE(expr->pred.left);
      RECURSE(expr->pred.right);
      break;
    case RSExpr_Inverted:
      RECURSE(expr->inverted.child);
      break;
    default:
      break;
  }
  return EXPR_EVAL_OK;
}

//---------------------------------------------------------------------------------------------

// Allocate some memory for a function that can be freed automatically when the execution is done

void *ExprEval::UnalignedAlloc(size_t sz) {
  return stralloc.Alloc(sz, MAX(sz, 1024));
}

//---------------------------------------------------------------------------------------------

char *ExprEval::Strndup(const char *str, size_t len) {
  char *ret = UnalignedAlloc(len + 1);
  memcpy(ret, str, len);
  ret[len] = '\0';
  return ret;
}

///////////////////////////////////////////////////////////////////////////////////////////////

#define RESULT_EVAL_ERR (RS_RESULT_MAX + 1)

//---------------------------------------------------------------------------------------------

int RPEvaluator::Next(SearchResult *r) {
  // Get the upstream result
  int rc = upstream->Next(r);
  if (rc != RS_RESULT_OK) {
    return rc;
  }

  eval.res = r;
  eval.srcrow = &r->rowdata;

  // TODO: Set this once only
  eval.err = parent->err;

  if (!val) {
    val = RS_NewValue(RSValue_Undef);
  }

  rc = eval->Eval(val);
  return rc == EXPR_EVAL_OK ? RS_RESULT_OK : RS_RESULT_ERROR;
}

//---------------------------------------------------------------------------------------------

int RPProjector::Next(SearchResult *r) {
  int rc = RPEvaluator::Next(r);
  if (rc != RS_RESULT_OK) {
    return rc;
  }
  RLookup_WriteOwnKey(pc->outkey, &r->rowdata, pc->val);
  pc->val = NULL;
  return RS_RESULT_OK;
}

//---------------------------------------------------------------------------------------------

int RPFilter::Next(SearchResult *r) {
  int rc;
  while ((rc = RPEvaluator::Next(r)) == RS_RESULT_OK) {
    // Check if it's a boolean result!
    int boolrv = RSValue_BoolTest(pc->val);
    RSValue_Clear(pc->val);

    if (boolrv) {
      return RS_RESULT_OK;
    }

    // Otherwise, the result must be filtered out.
    r->Clear();
  }
  return rc;
}

//---------------------------------------------------------------------------------------------

RPEvaluator::~RPEvaluator() {
  if (val) {
    val.Decref();
  }
}

//---------------------------------------------------------------------------------------------

RPEvaluator::RPEvaluator(const RSExpr *ast, const RLookup *lookup, const RLookupKey *dstkey) {
  eval.lookup = lookup;
  eval.root = ast;
  outkey = dstkey;
}

//---------------------------------------------------------------------------------------------

RPProjector::RPProjector(const RSExpr *ast, const RLookup *lookup, const RLookupKey *dstkey) :
    RPEvaluator(ast, lookup, dstkey) {
  name = "Projector";
}

//---------------------------------------------------------------------------------------------

RPFilter::RPFilter(const RSExpr *ast, const RLookup *lookup) :
    RPEvaluator(ast, lookup, NULL) {
  name = "Filter";
}

///////////////////////////////////////////////////////////////////////////////////////////////
