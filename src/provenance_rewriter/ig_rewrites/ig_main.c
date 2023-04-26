/*-----------------------------------------------------------------------------
 *
 * ig_main.c
 *
 *
 *		AUTHOR: shemon & seokki
 *
 *
 *
 *-----------------------------------------------------------------------------
 */

#include "configuration/option.h"
#include "instrumentation/timing_instrumentation.h"
#include "provenance_rewriter/pi_cs_rewrites/pi_cs_main.h"
#include "provenance_rewriter/ig_rewrites/ig_main.h"
#include "provenance_rewriter/prov_utility.h"
#include "utility/string_utils.h"
#include "model/query_operator/query_operator.h"
#include "model/query_operator/query_operator_model_checker.h"
#include "model/query_operator/operator_property.h"
#include "mem_manager/mem_mgr.h"
#include "log/logger.h"
#include "model/node/nodetype.h"
#include "provenance_rewriter/prov_schema.h"
#include "model/list/list.h"
#include "model/set/set.h"
#include "model/expression/expression.h"
#include "model/set/hashmap.h"
#include "parser/parser_jp.h"
#include "provenance_rewriter/transformation_rewrites/transformation_prov_main.h"
#include "provenance_rewriter/semiring_combiner/sc_main.h"
#include "provenance_rewriter/coarse_grained/coarse_grained_rewrite.h"

#define LOG_RESULT(mes,op) \
    do { \
        INFO_OP_LOG(mes,op); \
        DEBUG_NODE_BEATIFY_LOG(mes,op); \
    } while(0)

static QueryOperator *rewriteIG_Operator (QueryOperator *op);
static QueryOperator *rewriteIG_Conversion (ProjectionOperator *op);
static QueryOperator *rewriteIG_Projection(ProjectionOperator *op);
static QueryOperator *rewriteIG_Join(JoinOperator *op);
static QueryOperator *rewriteIG_TableAccess(TableAccessOperator *op);

static Node *asOf;
static RelCount *nameState;
//static QueryOperator *provComputation;
//static ProvenanceType provType;
List *attrL = NIL;
List *attrR = NIL;


QueryOperator *
rewriteIG (ProvenanceComputation  *op)
{
//	// IG
//	provType = op->provType;

    START_TIMER("rewrite - IG rewrite");

    // unset relation name counters
    nameState = (RelCount *) NULL;

    DEBUG_NODE_BEATIFY_LOG("*************************************\nREWRITE INPUT\n"
            "******************************\n", op);

    //mark the number of table - used in provenance scratch
    markNumOfTableAccess((QueryOperator *) op);

    QueryOperator *rewRoot = OP_LCHILD(op);
    DEBUG_NODE_BEATIFY_LOG("rewRoot is:", rewRoot);

    // cache asOf
    asOf = op->asOf;

    // rewrite subquery under provenance computation
    rewriteIG_Operator(rewRoot);
    DEBUG_NODE_BEATIFY_LOG("before rewritten query root is switched:", rewRoot);

    // update root of rewritten subquery
    rewRoot = OP_LCHILD(op);

    // adapt inputs of parents to remove provenance computation
    switchSubtrees((QueryOperator *) op, rewRoot);
    DEBUG_NODE_BEATIFY_LOG("rewritten query root is:", rewRoot);

    STOP_TIMER("rewrite - IG rewrite");

    return rewRoot;
}


static QueryOperator *
rewriteIG_Operator (QueryOperator *op)
{
    QueryOperator *rewrittenOp;

    switch(op->type)
    {
    	case T_CastOperator:
        	FATAL_LOG("no rewrite implemented for operator ", nodeToString(op));
        	return NULL;
        case T_SelectionOperator:
        	FATAL_LOG("no rewrite implemented for operator ", nodeToString(op));
        	return NULL;
        case T_ProjectionOperator:
            rewrittenOp = rewriteIG_Projection((ProjectionOperator *) op);
            break;
        case T_AggregationOperator:
        	FATAL_LOG("no rewrite implemented for operator ", nodeToString(op));
        	return NULL;
        case T_JoinOperator:
            rewrittenOp = rewriteIG_Join((JoinOperator *) op);
            break;
        case T_SetOperator:
        	FATAL_LOG("no rewrite implemented for operator ", nodeToString(op));
        	return NULL;
        case T_TableAccessOperator:
            rewrittenOp = rewriteIG_TableAccess((TableAccessOperator *) op);
            break;
        case T_ConstRelOperator:
        	FATAL_LOG("no rewrite implemented for operator ", nodeToString(op));
        	return NULL;
        case T_DuplicateRemoval:
        	FATAL_LOG("no rewrite implemented for operator ", nodeToString(op));
        	return NULL;
        case T_OrderOperator:
        	FATAL_LOG("no rewrite implemented for operator ", nodeToString(op));
        	return NULL;
        case T_JsonTableOperator:
        	FATAL_LOG("no rewrite implemented for operator ", nodeToString(op));
        	return NULL;
        case T_NestingOperator:
        	FATAL_LOG("no rewrite implemented for operator ", nodeToString(op));
        	return NULL;
        default:
            FATAL_LOG("no rewrite implemented for operator ", nodeToString(op));
            return NULL;
    }

    if (isRewriteOptionActivated(OPTION_AGGRESSIVE_MODEL_CHECKING)){
        ASSERT(checkModel(rewrittenOp));
    }
    DEBUG_NODE_BEATIFY_LOG("rewritten query operators:", rewrittenOp);
    return rewrittenOp;
}


static QueryOperator *
rewriteIG_Conversion (ProjectionOperator *op)
{
	List *newProjExprs = NIL;
	List *attrNames = NIL;
	List *newNames = NIL;

	FOREACH(AttributeReference, a, op->projExprs)
	{
		if(isPrefix(a->name,"ig"))
		{
			if (a->attrType == DT_STRING)
			{
				StringToArray *toArray;
				Unnest *tounnest;
				Ascii *toAscii;

				toArray = createStringToArrayExpr((Node *) a, "NULL");
				tounnest = createUnnestExpr((Node *) toArray);
				toAscii = createAsciiExpr((Node *) tounnest);

				newProjExprs = appendToTailOfList(newProjExprs, toAscii);
			}
			else
			{
				newProjExprs = appendToTailOfList(newProjExprs, a);
			}
		}
		else
		{
			newProjExprs = appendToTailOfList(newProjExprs, a);
		}

	}

	op->projExprs = newProjExprs;

	// CREATING a projection to not feed ascii expression into aggregation
	int cnt = 0;
	List *projExprs = NIL;

	FOREACH(AttributeDef,a,op->op.schema->attrDefs)
	{
		projExprs = appendToTailOfList(projExprs,
				createFullAttrReference(a->attrName, 0, cnt, 0, a->dataType));

		attrNames = appendToTailOfList(attrNames, a->attrName);

		cnt++;
	}

	//create projection operator upon selection operator from select clause
	ProjectionOperator *po = createProjectionOp(projExprs, NULL, NIL, attrNames);
	addChildOperator((QueryOperator *) po, (QueryOperator *) op);
	// Switch the subtree with this newly created projection operator.
	switchSubtrees((QueryOperator *) op, (QueryOperator *) po);

	List *aggrs = NIL;
	List *groupBy = NIL;
	attrNames = NIL;
	int i = 0;

	FOREACH(Node,n,newProjExprs)
	{
		if(isA(n,Ascii))
		{
			char *attrName = getAttrNameByPos((QueryOperator *) po, i);
			AttributeReference *ar = createAttrsRefByName((QueryOperator *) po, attrName);
			FunctionCall *sum = createFunctionCall("SUM", singleton(ar));
			aggrs = appendToTailOfList(aggrs,sum);

		}
		else
		{
			if(isA(n,AttributeReference))
			{
				groupBy = appendToTailOfList(groupBy,n);

			}

			if(isA(n,CastExpr))
			{
				CastExpr *ce = (CastExpr *) n;
				AttributeReference *ar = (AttributeReference *) ce->expr;
				groupBy = appendToTailOfList(groupBy, (Node *) ar);
			}
		}

		i++;
	}


	// CREATING NEW NAMES HERE FOR AGGREGATE OPERATOR
	FOREACH(AttributeReference, a, po->projExprs)
	{
		if(isPrefix(a->name, "ig") && a->attrType == DT_STRING)
		{
			newNames = appendToTailOfList(newNames, a->name);
		}
	}

	FOREACH(AttributeReference, b, po->projExprs)
	{
		if(!isPrefix(b->name, "ig"))
		{
			newNames = appendToTailOfList(newNames, b->name);
		}
		else if(isPrefix(b->name, "ig") && b->attrType != DT_STRING)
		{
			newNames = appendToTailOfList(newNames, b->name);
		}
	}

	AggregationOperator *ao = createAggregationOp(aggrs, groupBy, NULL, NIL, newNames);
	addChildOperator((QueryOperator *) ao, (QueryOperator *) po);

	// Switch the subtree with this newly created projection operator.
	switchSubtrees((QueryOperator *) po, (QueryOperator *) ao);

	// CREATING THE NEW PROJECTION OPERATOR
	projExprs = NIL;
	cnt = 0;

	FOREACH(AttributeDef,a,ao->op.schema->attrDefs)
	{
		projExprs = appendToTailOfList(projExprs,
				createFullAttrReference(a->attrName, 0, cnt, 0, a->dataType));

		cnt++;
	}

	//create projection operator upon selection operator from select clause
	ProjectionOperator *newPo = createProjectionOp(projExprs, NULL, NIL, newNames);
	addChildOperator((QueryOperator *) newPo, (QueryOperator *) ao);

	// Switch the subtree with this newly created projection operator.
	switchSubtrees((QueryOperator *) ao, (QueryOperator *) newPo);

	// CAST_EXPR
	newProjExprs = NIL;

	FOREACH(AttributeReference, a, newPo->projExprs)
	{
		if(a->attrType == DT_INT || a->attrType == DT_FLOAT)
		{
			CastExpr *cast;
			cast = createCastExpr((Node *) a, DT_BIT10);  // TODO: change to bit10
			newProjExprs = appendToTailOfList(newProjExprs, cast);
		}
		else
			newProjExprs = appendToTailOfList(newProjExprs, a);
	}

	newPo->projExprs = newProjExprs;
	// retrieve the original order of the projection attributes
	projExprs = NIL;
	newNames = NIL;


	FOREACH(AttributeDef,a,po->op.schema->attrDefs)
	{
		projExprs = appendToTailOfList(projExprs,
				createFullAttrReference(a->attrName, 0,
						getAttrPos((QueryOperator *) newPo, a->attrName), 0, a->dataType));

		newNames = appendToTailOfList(newNames, a->attrName);
	}

	ProjectionOperator *addPo = createProjectionOp(projExprs, NULL, NIL, newNames);


	char *tblName = "";
	FOREACH(AttributeReference, n, addPo->projExprs)
	{
		if(isPrefix(n->name, "ig"))
		{
			int len1 = strlen(n->name);
			int len2 = strlen(strrchr(n->name, '_'));
			int len = len1 - len2 - 1;
			tblName = substr(n->name, 8, len);
			break;
		}

	}

	int temp = 0;
	int tblLen = strlen(tblName);

	List *newProjExpr = NIL;
	List *newProjExpr1 = NIL;
	List *newProjExpr2 = NIL;



	FOREACH(AttributeReference, n, addPo->projExprs)
	{
		newProjExpr1 = appendToTailOfList(newProjExpr1, n);
		attrNames = appendToTailOfList(attrNames, n->name);
	}

	FOREACH(AttributeReference, n, addPo->projExprs)
	{

		if(temp == 0)
		{
			newProjExpr = appendToTailOfList(newProjExpr, createConstString(tblName));
			temp++;
		}
		else if (isPrefix(n->name, "ig"))
		{
			newProjExpr = appendToTailOfList(newProjExpr, n);
			//this adds first 3 letter for the variable in concat
			newProjExpr = appendToTailOfList(newProjExpr,
					createConstString((substr(n->name, 9 + tblLen, 9 + tblLen + 2))));
		}
	}
	attrNames = appendToTailOfList(attrNames, CONCAT_STRINGS(tblName, "_anno"));
	newProjExpr = LIST_MAKE(createOpExpr("||", newProjExpr));
	newProjExpr2 = concatTwoLists(newProjExpr1, newProjExpr);

	ProjectionOperator *concat = createProjectionOp(newProjExpr2, NULL, NIL, attrNames);

	addChildOperator((QueryOperator *) concat, (QueryOperator *) newPo);

	// Switch the subtree with this newly created projection operator.
	switchSubtrees((QueryOperator *) newPo, (QueryOperator *) concat);

    LOG_RESULT("Converted Operator tree", concat);
	return (QueryOperator *) concat;

}


static QueryOperator *
rewriteIG_Projection (ProjectionOperator *op)
{
    ASSERT(OP_LCHILD(op));
    DEBUG_LOG("REWRITE-IG - Projection");
    DEBUG_LOG("Operator tree \n%s", nodeToString(op));

    // rewrite child
    rewriteIG_Operator(OP_LCHILD(op));
    QueryOperator *child = OP_LCHILD(op);
 	switchSubtrees((QueryOperator *) op, child);

	List *newProjExpr = NIL;
	List *newAttrNames = NIL;
	int pos = 0;

	FOREACH(AttributeDef, a, child->schema->attrDefs)
	{
		newProjExpr = appendToTailOfList(newProjExpr,
				 createFullAttrReference(a->attrName, 0, pos, 0, a->dataType));

		newAttrNames = appendToTailOfList(newAttrNames, a->attrName);
		pos++;
	}

	ProjectionOperator *newProj = createProjectionOp(newProjExpr, NULL, NIL, newAttrNames);
    addChildOperator((QueryOperator *) newProj, (QueryOperator *) child);
    switchSubtrees((QueryOperator *) child, (QueryOperator *) newProj);

    newProjExpr = NIL;
    pos = 0;
    int lenL = LIST_LENGTH(attrL) - 1;
    int l = 0;
    int posOfIgL = LIST_LENGTH(attrL) / 2;
    int posOfIgR = LIST_LENGTH(attrR) / 2;

    List *LprojExprs = NIL;
    List *RprojExprs = NIL;
    List *LattrNames = NIL;
    List *RattrNames = NIL;

    HashMap *nameToIgAttrNameL = NEW_MAP(Constant, Constant);
    HashMap *nameToIgAttrNameR = NEW_MAP(Constant, Constant);


    FOREACH(AttributeDef,a,attrL)
    {
    	if(!isPrefix(a->attrName,"ig"))
    	{
        	char *key = a->attrName;
        	AttributeDef *igA = (AttributeDef *) getNthOfListP(attrL, posOfIgL);
        	char *value = igA->attrName;

        	ADD_TO_MAP(nameToIgAttrNameL,createStringKeyValue(key,value));
        	posOfIgL++;
    	}
    	else if(isSuffix(a->attrName,"anno"))
    	{
        	char *kv = a->attrName;
        	ADD_TO_MAP(nameToIgAttrNameL,createStringKeyValue(kv,kv));
        	posOfIgL++;
    	}

    }

    FOREACH(AttributeDef,a,attrR)
	{
		if(!isPrefix(a->attrName,"ig"))
		{
			char *key = a->attrName;
			AttributeDef *igA = (AttributeDef *) getNthOfListP(attrR,posOfIgR);
			char *value = igA->attrName;

			ADD_TO_MAP(nameToIgAttrNameR,createStringKeyValue(key,value));
			posOfIgR++;
		}
    	else if(isSuffix(a->attrName,"anno"))
    	{
        	char *kv = a->attrName;
        	ADD_TO_MAP(nameToIgAttrNameL,createStringKeyValue(kv,kv));
        	posOfIgR++;
    	}

	}

    FOREACH(AttributeReference, n, newProj->projExprs)
    {
    	if(l <= lenL)
    	{
    		LprojExprs = appendToTailOfList(LprojExprs, n);
			LattrNames = appendToTailOfList(LattrNames, n->name);
			l++;
    	}
    }

    l = 0;
    FOREACH(AttributeReference, n, newProj->projExprs)
    {
		if(l > lenL)
		{
			RprojExprs = appendToTailOfList(RprojExprs, n);
			RattrNames = appendToTailOfList(RattrNames, n->name);
			l++;
		}
		else
		{
			l++;
		}
    }

	FOREACH(AttributeReference, n, LprojExprs)
	{
		if(!isPrefix(n->name, "ig") && !isSuffix(n->name, "anno"))
		{
			newProjExpr = appendToTailOfList(newProjExpr, n);
		}

	}


	FOREACH(AttributeReference, n, LprojExprs)
	{
		if(!isPrefix(n->name, "ig"))
		{
			if(MAP_HAS_STRING_KEY(nameToIgAttrNameR, n->name))
			{
				char *attrIgNameL = STRING_VALUE(MAP_GET_STRING(nameToIgAttrNameL, n->name));
				char *attrIgNameR = STRING_VALUE(MAP_GET_STRING(nameToIgAttrNameR, n->name));

				AttributeReference *ar = createFullAttrReference(attrIgNameL, 0,
						listPosString(LattrNames,attrIgNameL), 0, n->attrType);
				Node *cond = (Node *) createIsNullExpr((Node *) ar);
				Node *then = (Node *) createFullAttrReference(attrIgNameR, 0,
						LIST_LENGTH(LattrNames) + listPosString(RattrNames,attrIgNameR),
						0, n->attrType);
				Node *els = (Node *) ar;

				CaseWhen *caseWhen = createCaseWhen(cond, then);
				CaseExpr *caseExpr = createCaseExpr(NULL, singleton(caseWhen), els);

				newProjExpr = appendToTailOfList(newProjExpr, caseExpr);
			}
			else
				newProjExpr = appendToTailOfList(newProjExpr, n);

		}
	}


	FOREACH(AttributeReference, n, RprojExprs)
	{
		if(!isPrefix(n->name, "ig") && !isSuffix(n->name, "anno"))
		{
			newProjExpr = appendToTailOfList(newProjExpr, n);
		}

	}

	FOREACH(AttributeReference, n, RprojExprs)
	{
		if(!isPrefix(n->name, "ig"))
		{
			char *ch = replaceSubstr(n->name, "1", "");
			if(MAP_HAS_STRING_KEY(nameToIgAttrNameL, ch))
			{
				char *attrIgNameL = STRING_VALUE(MAP_GET_STRING(nameToIgAttrNameL, ch));
				char *attrIgNameR = STRING_VALUE(MAP_GET_STRING(nameToIgAttrNameR, ch));

				AttributeReference *ar = createFullAttrReference(attrIgNameR, 0,
						LIST_LENGTH(LattrNames) + listPosString(RattrNames,attrIgNameR),
						0, n->attrType);
				Node *cond = (Node *) createIsNullExpr((Node *) ar);
				Node *then = (Node *) createFullAttrReference(attrIgNameL, 0,
						listPosString(LattrNames,attrIgNameL), 0, n->attrType);
				Node *els  = (Node *) ar;

				CaseWhen *caseWhen = createCaseWhen(cond, then);
				CaseExpr *caseExpr = createCaseExpr(NULL, singleton(caseWhen), els);

				newProjExpr = appendToTailOfList(newProjExpr, caseExpr);
			}
			else
				newProjExpr = appendToTailOfList(newProjExpr, n);

		}

	}

	List *attrNames = CONCAT_LISTS(LattrNames,RattrNames);
    ProjectionOperator *op1 = createProjectionOp(newProjExpr, NULL, NIL, attrNames);


    List *exprs = NIL;
    List *atNames = NIL;
	FOREACH(AttributeReference, n, LprojExprs)
	{
		List *functioninput = NIL;
		List *cast = NIL;

		if(!isPrefix(n->name, "ig"))
		{
			if(MAP_HAS_STRING_KEY(nameToIgAttrNameR, n->name) && !isSuffix(n->name, "anno"))
			{
				char *attrIgNameL = STRING_VALUE(MAP_GET_STRING(nameToIgAttrNameL, n->name));
				char *attrIgNameR = STRING_VALUE(MAP_GET_STRING(nameToIgAttrNameR, n->name));

				AttributeReference *arL = createFullAttrReference(attrIgNameL, 0,
						listPosString(LattrNames,attrIgNameL), 0, n->attrType);

				AttributeReference *arR = createFullAttrReference(attrIgNameR, 0,
						LIST_LENGTH(LattrNames) + listPosString(RattrNames,attrIgNameR),
										0, n->attrType);

				CastExpr *castL;
				CastExpr *castR;

				castL = createCastExpr((Node *) arL, DT_STRING);
				castR = createCastExpr((Node *) arR, DT_STRING);

				cast = appendToTailOfList(cast, castL);
				cast = appendToTailOfList(cast, castR);

				functioninput = appendToTailOfList(functioninput, arL);
				functioninput = appendToTailOfList(functioninput, arR);

				FunctionCall *hammingdist = createFunctionCall("hammingdist", cast);
				Node *cond = (Node *)(createOpExpr("=",functioninput));
				Node *then = (Node *)(createConstString("0000000000"));
				Node *els  = (Node *) hammingdist;
				CaseWhen *caseWhen = createCaseWhen(cond, then);
				CaseExpr *caseExpr = createCaseExpr(NULL, singleton(caseWhen), els);

				exprs = appendToTailOfList(exprs,caseExpr);
				atNames = appendToTailOfList(atNames, CONCAT_STRINGS("hamming_", n->name));

			}
			else if(!MAP_HAS_STRING_KEY(nameToIgAttrNameR, n->name) && !isSuffix(n->name, "anno"))
			{
				char *attrIgNameL = STRING_VALUE(MAP_GET_STRING(nameToIgAttrNameL, n->name));
				AttributeReference *arL = createFullAttrReference(attrIgNameL, 0,
						listPosString(LattrNames,attrIgNameL), 0, n->attrType);

				CastExpr *castL;

				castL = createCastExpr((Node *) arL, DT_STRING);

				functioninput = appendToTailOfList(functioninput, castL);
				functioninput = appendToTailOfList(functioninput, createConstString("0000000000"));
				FunctionCall *hammingdist = createFunctionCall("hammingdist", functioninput);

				exprs = appendToTailOfList(exprs,hammingdist);
				atNames = appendToTailOfList(atNames, CONCAT_STRINGS("hamming_", n->name));

			}
		}

	}

	FOREACH(AttributeReference, n, RprojExprs)
	{
		List *functioninput = NIL;
		if(!isPrefix(n->name, "ig"))
		{
			char *ch = replaceSubstr(n->name, "1", "");
			if(MAP_HAS_STRING_KEY(nameToIgAttrNameL, ch) && !isSuffix(n->name, "anno") )
			{
				continue;
			}
			else if(!MAP_HAS_STRING_KEY(nameToIgAttrNameL, ch) && !isSuffix(n->name, "anno"))
			{
				char *attrIgNameR = STRING_VALUE(MAP_GET_STRING(nameToIgAttrNameR, ch));
				AttributeReference *arR = createFullAttrReference(attrIgNameR, 0,
						LIST_LENGTH(LattrNames) + listPosString(RattrNames,attrIgNameR),
										0, n->attrType);

				CastExpr *castR;
				castR = createCastExpr((Node *) arR, DT_STRING);

				functioninput = appendToTailOfList(functioninput, castR);
				functioninput = appendToTailOfList(functioninput, createConstString("0000000000"));
				FunctionCall *hammingdist = createFunctionCall("hammingdist", functioninput);

				exprs = appendToTailOfList(exprs,hammingdist);
				atNames = appendToTailOfList(atNames, CONCAT_STRINGS("hamming_", n->name));
			}
		}

	}

	ProjectionOperator *hamming_op = createProjectionOp(exprs, NULL, NIL, atNames);
	LOG_RESULT("TESTING HAMMINGDISTANCE FUNCTION", hamming_op);

	List *h_valueExprs = NIL;
	List *h_valueName = NIL;
	int posV = 0;

//	FOREACH(AttributeReference, n, exprs)
	FOREACH(AttributeDef, a, hamming_op->op.schema->attrDefs)
	{
		if(isPrefix(a->attrName, "hamming"))
		{
			AttributeReference *ar = createFullAttrReference(a->attrName, 0, posV,0, a->dataType);

			FunctionCall *hammingdistvalue = createFunctionCall("hammingdistvalue", singleton(ar));
			h_valueExprs = appendToTailOfList(h_valueExprs, hammingdistvalue);
			h_valueName = appendToTailOfList(h_valueName, CONCAT_STRINGS("value_", a->attrName));
		}
//		else
//		{
//			FunctionCall *hammingdistvalue = createFunctionCall("hammingdistvalue", singleton(n));
//			h_valueExprs = appendToTailOfList(h_valueExprs, hammingdistvalue);
//			h_valueName = appendToTailOfList(h_valueName, CONCAT_STRINGS("value_", n->name));
//		}

		posV++;
	}

	ProjectionOperator *hammingvalue_op = createProjectionOp(h_valueExprs, NULL, NIL, h_valueName);
	LOG_RESULT("TESTING HAMMINGDISTANCE VALUE FUNCTION", hammingvalue_op);


    addChildOperator((QueryOperator *) op1, (QueryOperator *) newProj);
    switchSubtrees((QueryOperator *) newProj, (QueryOperator *) op1);

 	LOG_RESULT("Rewritten Projection Operator tree", op1);
    return (QueryOperator *) op1;
}


static QueryOperator *
rewriteIG_Join (JoinOperator *op)
{
    DEBUG_LOG("REWRITE-IG - Join");

    QueryOperator *lChild = OP_LCHILD(op);
    QueryOperator *rChild = OP_RCHILD(op);

    lChild = rewriteIG_Operator(lChild);
    rChild = rewriteIG_Operator(rChild);

	// update the attribute def for join operator
    List *lAttrDefs = copyObject(getNormalAttrs(lChild));
    List *rAttrDefs = copyObject(getNormalAttrs(rChild));

    attrL = copyObject(lAttrDefs);
    attrR = copyObject(rAttrDefs);

    List *newAttrDefs = CONCAT_LISTS(lAttrDefs,rAttrDefs);
    op->op.schema->attrDefs = copyObject(newAttrDefs);
    makeAttrNamesUnique((QueryOperator *) op);

	LOG_RESULT("Rewritten Join Operator tree",op);
    return (QueryOperator *) op;
}


static QueryOperator *
rewriteIG_TableAccess(TableAccessOperator *op)
{
	List *attrNames = NIL;
	List *projExpr = NIL;
	List *newProjExprs = NIL;
	int relAccessCount = getRelNameCount(&nameState, op->tableName);
	int cnt = 0;

	DEBUG_LOG("REWRITE-IG - Table Access <%s> <%u>", op->tableName, relAccessCount);

	// copy any as of clause if there
	if (asOf)
		op->asOf = copyObject(asOf);

	// normal attributes
	FOREACH(AttributeDef, attr, op->op.schema->attrDefs)
	{
		attrNames = appendToTailOfList(attrNames, strdup(attr->attrName));
		projExpr = appendToTailOfList(projExpr, createFullAttrReference(attr->attrName, 0, cnt, 0, attr->dataType));
		cnt++;
	}

	// ig attributes
    cnt = 0;
    char *newAttrName;
    newProjExprs = copyObject(projExpr);

    FOREACH(AttributeDef, attr, op->op.schema->attrDefs)
    {
    	newAttrName = getIgAttrName(op->tableName, attr->attrName, relAccessCount);
    	attrNames = appendToTailOfList(attrNames, newAttrName);

   		projExpr = appendToTailOfList(projExpr, createFullAttrReference(attr->attrName, 0, cnt, 0, attr->dataType));
    	cnt++;
    }

    List *newIgPosList = NIL;
    CREATE_INT_SEQ(newIgPosList, cnt, (cnt * 2) - 1, 1);

	ProjectionOperator *po = createProjectionOp(projExpr, NULL, NIL, attrNames);

	// set ig attributes and property
	po->op.igAttrs = newIgPosList;
	SET_BOOL_STRING_PROP((QueryOperator *) po, PROP_PROJ_IG_ATTR_DUP);

	addChildOperator((QueryOperator *) po, (QueryOperator *) op);
//	op->op.parents = singleton(proj);

	// Switch the subtree with this newly created projection operator.
    switchSubtrees((QueryOperator *) op, (QueryOperator *) po);

    DEBUG_LOG("table access after adding additional attributes for ig: %s", operatorToOverviewString((Node *) po));

	// add projection expressions for ig attrs
	FOREACH_INT(a, po->op.igAttrs)
	{
		AttributeDef *att = getAttrDef((QueryOperator *) po,a);
		newProjExprs = appendToTailOfList(newProjExprs,
				 createFullAttrReference(att->attrName, 0, a, 0, att->dataType));
	}

	ProjectionOperator *newPo = createProjectionOp(newProjExprs, NULL, NIL, attrNames);
	addChildOperator((QueryOperator *) newPo, (QueryOperator *) po);

	// Switch the subtree with this newly created projection operator.
    switchSubtrees((QueryOperator *) po, (QueryOperator *) newPo);

    DEBUG_LOG("table access after adding ig attributes to the schema: %s", operatorToOverviewString((Node *) newPo));
    LOG_RESULT("Rewritten TableAccess Operator tree", newPo);
    return rewriteIG_Conversion(newPo);
}
