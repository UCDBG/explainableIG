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
    LOG_RESULT("rewritten query operators:", rewrittenOp);
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
	//LOG_RESULT("Rewritten Projection Operator tree", po);

	// Creating aggregate operation
	// QueryOperator *o = copyObject((QueryOperator *) op);

	List *aggrs = NIL;
	List *groupBy = NIL;
	//List *attrNamesOriginal = NIL;
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
//			char *nn = CONCAT_STRINGS(a->name,"_agg");
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

	//LOG_RESULT("Rewritten Aggregation Operator tree", ao);

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
	//LOG_RESULT("Rewritten Projection Operator tree", newPo);

	// CAST_EXPR
	newProjExprs = NIL;

	FOREACH(AttributeReference, a, newPo->projExprs)
	{
		if(a->attrType == DT_INT || a->attrType == DT_FLOAT)
		{
			CastExpr *cast;
			cast = createCastExpr((Node *) a, DT_FLOAT);  // TODO: change to bit10
			newProjExprs = appendToTailOfList(newProjExprs, cast);
		}
		else
			newProjExprs = appendToTailOfList(newProjExprs, a);
	}

	newPo->projExprs = newProjExprs;
	//LOG_RESULT("Rewritten Projection Operator tree with CAST", newPo);

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
	addChildOperator((QueryOperator *) addPo, (QueryOperator *) newPo);

	// Switch the subtree with this newly created projection operator.
	switchSubtrees((QueryOperator *) newPo, (QueryOperator *) addPo);


//	char *tblName = "";
//	FOREACH(AttributeReference, n, addPo->projExprs)
//	{
//		if(isPrefix(n->name, "ig"))
//		{
//			//char *found = strstr(strstr(strstr(n->name, "_"),"_"),"_");
//			//char *found = strrchr(n->name, '_');
//			int len1 = strlen(n->name);
//			int len2 = strlen(strrchr(n->name, '_'));
//			int len = len1 - len2 - 1;
//			tblName = substr(n->name, 8, len);
//			break;
//		}
//
//	}
//	// ADD CASE HERE
//	int temp = 0;
//	//int pos = 0;
//	int tblLen = strlen(tblName);
//
//	List *newProjExpr = NIL;
//	List *newProjExpr1 = NIL;
//	List *newProjExpr2 = NIL;
//
//
//
//	FOREACH(AttributeReference, n, addPo->projExprs)
//	{
//		newProjExpr1 = appendToTailOfList(newProjExpr1, n);
//		attrNames = appendToTailOfList(attrNames, n->name);
//		//pos++ ;
//	}
//
//
//
//	FOREACH(AttributeReference, n, addPo->projExprs)
//	{
//
//		if(temp == 0)
//		{
//			newProjExpr = appendToTailOfList(newProjExpr, createConstString(tblName));
//					//createFullAttrReference(tblName, n->fromClauseItem, pos, 0, n->attrType));
//			temp++;
//		}
//		else if (isPrefix(n->name, "ig"))
//		{
//			newProjExpr = appendToTailOfList(newProjExpr, n);
//			//this adds first 3 letter for the variable in concat
//			newProjExpr = appendToTailOfList(newProjExpr,createConstString((substr(n->name, 9 + tblLen, 9 + tblLen + 2))));
//			//createFullAttrReference((substr(n->name, 9 + tblLen, 9 + tblLen + 2)),n->fromClauseItem, pos, 0, n->attrType));
//			//(createConstString(substr(n->name, 14, 16))
//
//			//pos++;
//		}
//	}
//	attrNames = appendToTailOfList(attrNames, "anno");
//	newProjExpr = LIST_MAKE(createOpExpr("||", newProjExpr));
//
//	newProjExpr2 = concatTwoLists(newProjExpr1, newProjExpr);
//
//
//
//
//	ProjectionOperator *concat = createProjectionOp(newProjExpr2, NULL, NIL, attrNames);
//	//LOG_RESULT("TESTING EXPRESSION LIST -------------", Lconcat);

//
//	addChildOperator((QueryOperator *) concat, (QueryOperator *) addPo);
//
//	// Switch the subtree with this newly created projection operator.
//	switchSubtrees((QueryOperator *) addPo, (QueryOperator *) concat);

//    LOG_RESULT("Converted Operator tree", concat);
//	return (QueryOperator *) concat;


    LOG_RESULT("Converted Operator tree", addPo);
	return (QueryOperator *) addPo;
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
//    child->parents = NIL;

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

    newProjExpr = NIL;
    pos = 0;
    int lenL = LIST_LENGTH(attrL) - 1;
    //int lenR = LIST_LENGTH(attrR) - 1;
    int l = 0;
    int posOfIgL = LIST_LENGTH(attrL) / 2;
    int posOfIgR = LIST_LENGTH(attrR) / 2;

    List *LprojExprs = NIL;
    List *RprojExprs = NIL;
    List *LattrNames = NIL;
    List *RattrNames = NIL;
    List *attrNames = NIL;

    HashMap *nameToIgAttrNameL = NEW_MAP(Constant, Constant);
    HashMap *nameToIgAttrNameR = NEW_MAP(Constant, Constant);


    FOREACH(AttributeDef,a,attrL)
    {
    	if(!isPrefix(a->attrName,"ig"))
    	{
        	char *key = a->attrName;
        	AttributeDef *igA = (AttributeDef *) getNthOfListP(attrL,posOfIgL);
        	char *value = igA->attrName;

        	ADD_TO_MAP(nameToIgAttrNameL,createStringKeyValue(key,value));
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

    ProjectionOperator *Lop = createProjectionOp(LprojExprs, NULL, NIL, LattrNames);
    //LOG_RESULT("TESTING LEFT LIST", Lop);

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

    ProjectionOperator *Rop = createProjectionOp(RprojExprs, NULL, NIL, RattrNames);
    //LOG_RESULT("TESTING RIGHT LIST", Rop);


	FOREACH(AttributeReference, n, Lop->projExprs)
	{
		if(!isPrefix(n->name, "ig"))
		{
			newProjExpr = appendToTailOfList(newProjExpr, n);
			attrNames = appendToTailOfList(attrNames, n->name);
		}

	}

	FOREACH(AttributeReference, n, Lop->projExprs)
	{
		if(!isPrefix(n->name, "ig"))
		{
			if(hasMapStringKey(nameToIgAttrNameR, n->name))
			{
				//AttributeDef *a = (AttributeDef *) getNthOfListP(attrL, lenL1);
				//Node *cond = (Node *) createIsNullExpr((Node *) createAttributeReference(a->attrName));
				Node *cond = (Node *) createIsNullExpr(getMapString(nameToIgAttrNameL, n->name));
				//AttributeDef *a1 = (AttributeDef *) getNthOfListP(attrR, lenR1);
				//Node *then = (Node *) createAttributeReference(a1->attrName);
				Node *then = (Node *) getMapString(nameToIgAttrNameR, n->name);
				//Node *els  = (Node *) createAttributeReference(a->attrName);
				Node *els  = (Node *) getMapString(nameToIgAttrNameL, n->name);
				CaseWhen *caseWhen = createCaseWhen(cond, then);
				CaseExpr *caseExpr = createCaseExpr(NULL, singleton(caseWhen), els);
				newProjExpr = appendToTailOfList(newProjExpr, caseExpr);
				attrNames = appendToTailOfList(attrNames, n->name);
			}
			else if(!hasMapStringKey(nameToIgAttrNameR, n->name))
			{
				newProjExpr = appendToTailOfList(newProjExpr, n);
				attrNames = appendToTailOfList(attrNames, n->name);
			}

		}
	}


	FOREACH(AttributeReference, n, Rop->projExprs)
	{
		if(!isPrefix(n->name, "ig"))
		{
			newProjExpr = appendToTailOfList(newProjExpr, n);
			attrNames = appendToTailOfList(attrNames, n->name);
		}

	}


	FOREACH(AttributeReference, n, Rop->projExprs)
	{
		if(!isPrefix(n->name, "ig"))
		{
			//char *ch = strrchr(n->name, '_');
			char *ch = replaceSubstr(n->name, "1", "");
			if(hasMapStringKey(nameToIgAttrNameL, ch))
			{
				//AttributeDef *a = (AttributeDef *) getNthOfListP(attrL, lenL1);
				//Node *cond = (Node *) createIsNullExpr((Node *) createAttributeReference(a->attrName));
				Node *cond = (Node *) createIsNullExpr(getMapString(nameToIgAttrNameR, ch));
				//AttributeDef *a1 = (AttributeDef *) getNthOfListP(attrR, lenR1);
				//Node *then = (Node *) createAttributeReference(a1->attrName);
				Node *then = (Node *) getMapString(nameToIgAttrNameL, ch);
				//Node *els  = (Node *) createAttributeReference(a->attrName);
				Node *els  = (Node *) getMapString(nameToIgAttrNameR, ch);
				CaseWhen *caseWhen = createCaseWhen(cond, then);
				CaseExpr *caseExpr = createCaseExpr(NULL, singleton(caseWhen), els);
				newProjExpr = appendToTailOfList(newProjExpr, caseExpr);
				attrNames = appendToTailOfList(attrNames, n->name);
			}
			else if(!hasMapStringKey(nameToIgAttrNameL, n->name))
			{
				newProjExpr = appendToTailOfList(newProjExpr, n);
				attrNames = appendToTailOfList(attrNames, n->name);
			}
		}

	}

//	 List *Lkeys = getKeys(nameToIgAttrNameL);
//	 List *Rkeys = getKeys(nameToIgAttrNameR);
//
//	 FOREACH(List, l, Rkeys)
//	 {
//		 char *ch = l;
//		 if(hasMapStringKey(nameToIgAttrNameL, ch))
//		 {
//
//
//
//		 }
//
//	 }
//
//	 FOREACH_HASH(Constant, kv, nameToIgAttrNameL)
//	 {
//		 char *ch = kv->value;
//		 if(hasMapStringKey(nameToIgAttrNameR, ch))
//		 {
//
//		 }
//	 }


    ProjectionOperator *op1 = createProjectionOp(newProjExpr, NULL, NIL, attrNames);
    LOG_RESULT("TESTING CASE EXPRESSIONS", op1);

//    addChildOperator((QueryOperator *) op1, (QueryOperator *) child);
//    switchSubtrees((QueryOperator *) child, (QueryOperator *) op1);

// 	LOG_RESULT("Rewritten Projection Operator tree", op1);
//    return (QueryOperator *) op1;

 	LOG_RESULT("Rewritten Projection Operator tree", newProj);
    return (QueryOperator *) newProj;
}

static QueryOperator *
rewriteIG_Join (JoinOperator *op)
{
    DEBUG_LOG("REWRITE-IG - Join");

//    QueryOperator *o = (QueryOperator *) op;
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