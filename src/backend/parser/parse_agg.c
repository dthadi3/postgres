/*-------------------------------------------------------------------------
 *
 * parse_agg.c
 *	  handle aggregates in parser
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/parser/parse_agg.c,v 1.57 2003/08/04 02:40:01 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/makefuncs.h"
#include "nodes/params.h"
#include "optimizer/clauses.h"
#include "optimizer/tlist.h"
#include "optimizer/var.h"
#include "parser/parse_agg.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "utils/lsyscache.h"


typedef struct
{
	ParseState *pstate;
	List	   *groupClauses;
	bool		have_non_var_grouping;
	int			sublevels_up;
} check_ungrouped_columns_context;

static void check_ungrouped_columns(Node *node, ParseState *pstate,
						List *groupClauses, bool have_non_var_grouping);
static bool check_ungrouped_columns_walker(Node *node,
							   check_ungrouped_columns_context *context);


/*
 * transformAggregateCall -
 *		Finish initial transformation of an aggregate call
 *
 * parse_func.c has recognized the function as an aggregate, and has set
 * up all the fields of the Aggref except agglevelsup.	Here we must
 * determine which query level the aggregate actually belongs to, set
 * agglevelsup accordingly, and mark p_hasAggs true in the corresponding
 * pstate level.
 */
void
transformAggregateCall(ParseState *pstate, Aggref *agg)
{
	int			min_varlevel;

	/*
	 * The aggregate's level is the same as the level of the lowest-level
	 * variable or aggregate in its argument; or if it contains no
	 * variables at all, we presume it to be local.
	 */
	min_varlevel = find_minimum_var_level((Node *) agg->target);

	/*
	 * An aggregate can't directly contain another aggregate call of the
	 * same level (though outer aggs are okay).  We can skip this check if
	 * we didn't find any local vars or aggs.
	 */
	if (min_varlevel == 0)
	{
		if (checkExprHasAggs((Node *) agg->target))
			ereport(ERROR,
					(errcode(ERRCODE_GROUPING_ERROR),
				  errmsg("aggregate function calls may not be nested")));
	}

	if (min_varlevel < 0)
		min_varlevel = 0;
	agg->agglevelsup = min_varlevel;

	/* Mark the correct pstate as having aggregates */
	while (min_varlevel-- > 0)
		pstate = pstate->parentParseState;
	pstate->p_hasAggs = true;
}


/*
 * parseCheckAggregates
 *	Check for aggregates where they shouldn't be and improper grouping.
 *
 *	Ideally this should be done earlier, but it's difficult to distinguish
 *	aggregates from plain functions at the grammar level.  So instead we
 *	check here.  This function should be called after the target list and
 *	qualifications are finalized.
 */
void
parseCheckAggregates(ParseState *pstate, Query *qry)
{
	List	   *groupClauses = NIL;
	bool		have_non_var_grouping = false;
	List	   *lst;
	bool		hasJoinRTEs;
	Node	   *clause;

	/* This should only be called if we found aggregates or grouping */
	Assert(pstate->p_hasAggs || qry->groupClause);

	/*
	 * Aggregates must never appear in WHERE or JOIN/ON clauses.
	 *
	 * (Note this check should appear first to deliver an appropriate error
	 * message; otherwise we are likely to complain about some innocent
	 * variable in the target list, which is outright misleading if the
	 * problem is in WHERE.)
	 */
	if (checkExprHasAggs(qry->jointree->quals))
		ereport(ERROR,
				(errcode(ERRCODE_GROUPING_ERROR),
				 errmsg("aggregates not allowed in WHERE clause")));
	if (checkExprHasAggs((Node *) qry->jointree->fromlist))
		ereport(ERROR,
				(errcode(ERRCODE_GROUPING_ERROR),
				 errmsg("aggregates not allowed in JOIN conditions")));

	/*
	 * No aggregates allowed in GROUP BY clauses, either.
	 *
	 * While we are at it, build a list of the acceptable GROUP BY
	 * expressions for use by check_ungrouped_columns() (this avoids
	 * repeated scans of the targetlist within the recursive routine...).
	 * And detect whether any of the expressions aren't simple Vars.
	 */
	foreach(lst, qry->groupClause)
	{
		GroupClause *grpcl = (GroupClause *) lfirst(lst);
		Node	   *expr;

		expr = get_sortgroupclause_expr(grpcl, qry->targetList);
		if (expr == NULL)
			continue;			/* probably cannot happen */
		if (checkExprHasAggs(expr))
			ereport(ERROR,
					(errcode(ERRCODE_GROUPING_ERROR),
				   errmsg("aggregates not allowed in GROUP BY clause")));
		groupClauses = lcons(expr, groupClauses);
		if (!IsA(expr, Var))
			have_non_var_grouping = true;
	}

	/*
	 * If there are join alias vars involved, we have to flatten them to
	 * the underlying vars, so that aliased and unaliased vars will be
	 * correctly taken as equal.  We can skip the expense of doing this if
	 * no rangetable entries are RTE_JOIN kind.
	 */
	hasJoinRTEs = false;
	foreach(lst, pstate->p_rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lst);

		if (rte->rtekind == RTE_JOIN)
		{
			hasJoinRTEs = true;
			break;
		}
	}

	if (hasJoinRTEs)
		groupClauses = (List *) flatten_join_alias_vars(qry,
												  (Node *) groupClauses);

	/*
	 * Check the targetlist and HAVING clause for ungrouped variables.
	 */
	clause = (Node *) qry->targetList;
	if (hasJoinRTEs)
		clause = flatten_join_alias_vars(qry, clause);
	check_ungrouped_columns(clause, pstate,
							groupClauses, have_non_var_grouping);

	clause = (Node *) qry->havingQual;
	if (hasJoinRTEs)
		clause = flatten_join_alias_vars(qry, clause);
	check_ungrouped_columns(clause, pstate,
							groupClauses, have_non_var_grouping);
}


/*
 * check_ungrouped_columns -
 *	  Scan the given expression tree for ungrouped variables (variables
 *	  that are not listed in the groupClauses list and are not within
 *	  the arguments of aggregate functions).  Emit a suitable error message
 *	  if any are found.
 *
 * NOTE: we assume that the given clause has been transformed suitably for
 * parser output.  This means we can use expression_tree_walker.
 *
 * NOTE: we recognize grouping expressions in the main query, but only
 * grouping Vars in subqueries.  For example, this will be rejected,
 * although it could be allowed:
 *		SELECT
 *			(SELECT x FROM bar where y = (foo.a + foo.b))
 *		FROM foo
 *		GROUP BY a + b;
 * The difficulty is the need to account for different sublevels_up.
 * This appears to require a whole custom version of equal(), which is
 * way more pain than the feature seems worth.
 */
static void
check_ungrouped_columns(Node *node, ParseState *pstate,
						List *groupClauses, bool have_non_var_grouping)
{
	check_ungrouped_columns_context context;

	context.pstate = pstate;
	context.groupClauses = groupClauses;
	context.have_non_var_grouping = have_non_var_grouping;
	context.sublevels_up = 0;
	check_ungrouped_columns_walker(node, &context);
}

static bool
check_ungrouped_columns_walker(Node *node,
							   check_ungrouped_columns_context *context)
{
	List	   *gl;

	if (node == NULL)
		return false;
	if (IsA(node, Const) ||
		IsA(node, Param))
		return false;			/* constants are always acceptable */

	/*
	 * If we find an aggregate call of the original level, do not recurse
	 * into its arguments; ungrouped vars in the arguments are not an
	 * error. We can also skip looking at the arguments of aggregates of
	 * higher levels, since they could not possibly contain Vars that are
	 * of concern to us (see transformAggregateCall).  We do need to look
	 * into the arguments of aggregates of lower levels, however.
	 */
	if (IsA(node, Aggref) &&
		(int) ((Aggref *) node)->agglevelsup >= context->sublevels_up)
		return false;

	/*
	 * If we have any GROUP BY items that are not simple Vars, check to
	 * see if subexpression as a whole matches any GROUP BY item. We need
	 * to do this at every recursion level so that we recognize GROUPed-BY
	 * expressions before reaching variables within them. But this only
	 * works at the outer query level, as noted above.
	 */
	if (context->have_non_var_grouping && context->sublevels_up == 0)
	{
		foreach(gl, context->groupClauses)
		{
			if (equal(node, lfirst(gl)))
				return false;	/* acceptable, do not descend more */
		}
	}

	/*
	 * If we have an ungrouped Var of the original query level, we have a
	 * failure.  Vars below the original query level are not a problem,
	 * and neither are Vars from above it.	(If such Vars are ungrouped as
	 * far as their own query level is concerned, that's someone else's
	 * problem...)
	 */
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;
		RangeTblEntry *rte;
		char	   *attname;

		if (var->varlevelsup != context->sublevels_up)
			return false;		/* it's not local to my query, ignore */

		/*
		 * Check for a match, if we didn't do it above.
		 */
		if (!context->have_non_var_grouping || context->sublevels_up != 0)
		{
			foreach(gl, context->groupClauses)
			{
				Var		   *gvar = (Var *) lfirst(gl);

				if (IsA(gvar, Var) &&
					gvar->varno == var->varno &&
					gvar->varattno == var->varattno &&
					gvar->varlevelsup == 0)
					return false;		/* acceptable, we're okay */
			}
		}

		/* Found an ungrouped local variable; generate error message */
		Assert(var->varno > 0 &&
			   (int) var->varno <= length(context->pstate->p_rtable));
		rte = rt_fetch(var->varno, context->pstate->p_rtable);
		attname = get_rte_attribute_name(rte, var->varattno);
		if (context->sublevels_up == 0)
			ereport(ERROR,
					(errcode(ERRCODE_GROUPING_ERROR),
					 errmsg("attribute \"%s.%s\" must be GROUPed or used in an aggregate function",
							rte->eref->aliasname, attname)));
		else
			ereport(ERROR,
					(errcode(ERRCODE_GROUPING_ERROR),
					 errmsg("sub-select uses un-GROUPed attribute \"%s.%s\" from outer query",
							rte->eref->aliasname, attname)));

	}

	if (IsA(node, Query))
	{
		/* Recurse into subselects */
		bool		result;

		context->sublevels_up++;
		result = query_tree_walker((Query *) node,
								   check_ungrouped_columns_walker,
								   (void *) context,
								   0);
		context->sublevels_up--;
		return result;
	}
	return expression_tree_walker(node, check_ungrouped_columns_walker,
								  (void *) context);
}

/*
 * Create expression trees for the transition and final functions
 * of an aggregate.  These are needed so that polymorphic functions
 * can be used within an aggregate --- without the expression trees,
 * such functions would not know the datatypes they are supposed to use.
 * (The trees will never actually be executed, however, so we can skimp
 * a bit on correctness.)
 *
 * agg_input_type, agg_state_type, agg_result_type identify the input,
 * transition, and result types of the aggregate.  These should all be
 * resolved to actual types (ie, none should ever be ANYARRAY or ANYELEMENT).
 *
 * transfn_oid and finalfn_oid identify the funcs to be called; the latter
 * may be InvalidOid.
 *
 * Pointers to the constructed trees are returned into *transfnexpr and
 * *finalfnexpr.  The latter is set to NULL if there's no finalfn.
 */
void
build_aggregate_fnexprs(Oid agg_input_type,
						Oid agg_state_type,
						Oid agg_result_type,
						Oid transfn_oid,
						Oid finalfn_oid,
						Expr **transfnexpr,
						Expr **finalfnexpr)
{
	Oid			transfn_arg_types[FUNC_MAX_ARGS];
	int			transfn_nargs;
	Param	   *arg0;
	Param	   *arg1;
	List	   *args;

	/* get the transition function signature (only need nargs) */
	(void) get_func_signature(transfn_oid, transfn_arg_types, &transfn_nargs);

	/*
	 * Build arg list to use in the transfn FuncExpr node. We really only
	 * care that transfn can discover the actual argument types at runtime
	 * using get_fn_expr_argtype(), so it's okay to use Param nodes that
	 * don't correspond to any real Param.
	 */
	arg0 = makeNode(Param);
	arg0->paramkind = PARAM_EXEC;
	arg0->paramid = -1;
	arg0->paramtype = agg_state_type;

	if (transfn_nargs == 2)
	{
		arg1 = makeNode(Param);
		arg1->paramkind = PARAM_EXEC;
		arg1->paramid = -1;
		arg1->paramtype = agg_input_type;

		args = makeList2(arg0, arg1);
	}
	else
		args = makeList1(arg0);

	*transfnexpr = (Expr *) makeFuncExpr(transfn_oid,
										 agg_state_type,
										 args,
										 COERCE_DONTCARE);

	/* see if we have a final function */
	if (!OidIsValid(finalfn_oid))
	{
		*finalfnexpr = NULL;
		return;
	}

	/*
	 * Build expr tree for final function
	 */
	arg0 = makeNode(Param);
	arg0->paramkind = PARAM_EXEC;
	arg0->paramid = -1;
	arg0->paramtype = agg_state_type;
	args = makeList1(arg0);

	*finalfnexpr = (Expr *) makeFuncExpr(finalfn_oid,
										 agg_result_type,
										 args,
										 COERCE_DONTCARE);
}
