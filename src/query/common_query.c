#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "common_query.h"
#include "adios_query_hooks.h"

static struct adios_query_hooks_struct * gAdios_query_hooks = 0;

enum ADIOS_QUERY_TOOL gAssigned_query_tool = 0;

void common_query_init(enum ADIOS_QUERY_TOOL tool) {
	adios_query_hooks_init(&gAdios_query_hooks, tool);
	gAdios_query_hooks[tool].adios_query_init_method_fn();
	gAssigned_query_tool = tool;
}

void common_query_free(ADIOS_QUERY* q) {
	gAdios_query_hooks[gAssigned_query_tool].adios_query_free_method_fn(q);
}

void common_query_clean() {
	gAdios_query_hooks[gAssigned_query_tool].adios_query_clean_method_fn();
	free(gAdios_query_hooks);
}

int getTotalByteSize(ADIOS_VARINFO* v, ADIOS_SELECTION* sel,
		uint64_t* total_byte_size, uint64_t* dataSize) {
	*total_byte_size = adios_type_size(v->type, v->value);
	*dataSize = 1;

	if (sel == 0) {
		uint64_t s = 0;
		for (s = 0; s < v->ndim; s++) {
			*total_byte_size *= v->dims[s];
			*dataSize *= v->dims[s];
			printf(" dim %" PRIu64 "default count %" PRIu64 "\n", s, v->dims[s]);
		}
		return 0;
	}

	switch (sel->type) {
	case ADIOS_SELECTION_BOUNDINGBOX: {
		const ADIOS_SELECTION_BOUNDINGBOX_STRUCT *bb = &(sel->u.bb);
		uint64_t* count = bb->count;
		uint64_t* start = bb->start;
		uint64_t s = 0;

		for (s = 0; s < v->ndim; s++) {
			if (start[s] + count[s] > v->dims[s]) {
				printf(
						" invalid bounding box start %ld plus count %ld exceeds dim size: %ld\n",
						start[s], count[s], v->dims[s]);
				return -1;
			}
			*total_byte_size *= count[s];
			*dataSize *= count[s];
			printf(" dim %" PRIu64 "count %" PRIu64 " \n", s, count[s]);
		}

		printf("\tThe data size is = %" PRIu64 " \n", *dataSize);
		break;
	}
	case ADIOS_SELECTION_POINTS: {
		const ADIOS_SELECTION_POINTS_STRUCT *pts = &(sel->u.points);
		*total_byte_size *= pts->npoints;
		*dataSize = pts->npoints;
		break;
	}
	case ADIOS_SELECTION_WRITEBLOCK: {
		const ADIOS_SELECTION_WRITEBLOCK_STRUCT *wb = &(sel->u.block);
		*total_byte_size *= wb->nelements;
		*dataSize = wb->nelements;
		break;
	}
	default:
		break;
	}
	return 0;
}

void initialize(ADIOS_QUERY* result) {
	result->_onTimeStep = -1; // no data recorded
	result->_maxResultDesired = NO_EVAL_BEFORE;// init
	result->_lastRead = 0; // init
}

ADIOS_QUERY* common_query_create(ADIOS_FILE* f, const char* varName,
		ADIOS_SELECTION* queryBoundry, enum ADIOS_PREDICATE_MODE op,
		const char* value) {
	if (gAdios_query_hooks == NULL) {
		printf(
				"Error: Query environment is not initialized. Call adios_query_init() first.\n");
		exit(EXIT_FAILURE);
	}

	if ((value == NULL) || (f == NULL) || (varName == NULL) || (queryBoundry
			== NULL)) {
		printf("Error:No valid input is provided when creating query.\n");
		exit(EXIT_FAILURE);
	}

	if (gAssigned_query_tool == ADIOS_QUERY_TOOL_FASTBIT || gAssigned_query_tool
			== ADIOS_QUERY_TOOL_ALACRITY) {
		//TODO:
		//    if ((queryBoundry->type == ADIOS_SELECTION_BOUNDINGBOX) &&
		//	(queryBoundry->type == ADIOS_SELECTION_POINTS))
		if ((queryBoundry->type != ADIOS_SELECTION_BOUNDINGBOX)
				&& (queryBoundry->type != ADIOS_SELECTION_POINTS)) {
			printf(
					"Error: selection type is not supported by fastbit or alacrity. Choose either boundingbox or points\n");
			exit(EXIT_FAILURE);
		}
	}

	// get varinfo from bp structure
	ADIOS_VARINFO* v = adios_inq_var(f, varName);
	if (v == NULL) {
		printf(" Error! no such var:%s \n", varName);
		exit(EXIT_FAILURE);
	}

	uint64_t total_byte_size, dataSize;

	if (getTotalByteSize(v, queryBoundry, &total_byte_size, &dataSize) < 0) {
		exit(EXIT_FAILURE);
	}

	//
	// create selection string in adios_query structure
	//
	ADIOS_QUERY* query = (ADIOS_QUERY*) calloc(1, sizeof(ADIOS_QUERY));
	query->_condition = malloc(strlen(varName) + strlen(value) + 10); // 10 is enough for op and spaces
	if (op == ADIOS_LT) {
		sprintf(query->_condition, "(%s < %s)", varName, value);
	} else if (op == ADIOS_LTEQ) {
		sprintf(query->_condition, "(%s <= %s)", varName, value);
	} else if (op == ADIOS_GT) {
		sprintf(query->_condition, "(%s > %s)", varName, value);
	} else if (op == ADIOS_GTEQ) {
		sprintf(query->_condition, "(%s >= %s)", varName, value);
	} else if (op == ADIOS_EQ) {
		sprintf(query->_condition, "(%s = %s)", varName, value);
	} else {
		sprintf(query->_condition, "(%s != %s)", varName, value);
	}

	(query->_condition)[strlen(query->_condition)] = 0;

	query->_var = v;
	query->_f = f;

	query->_dataSlice = malloc(total_byte_size);
	query->_rawDataSize = dataSize;

	query->_sel = queryBoundry;

	query->_op = op;
	query->_value = strdup(value);

	//TODO: initialize two pointers
	query->_left = NULL;
	query->_right = NULL;
	initialize(query);
	return query;
}

ADIOS_QUERY* common_query_combine(ADIOS_QUERY* q1,
		enum ADIOS_CLAUSE_OP_MODE operator, ADIOS_QUERY* q2) {
	// combine selection sel3 = q1.fastbitSelection & q2.fastbitSelection
	//create a new query (q1.cond :op: q2.cond, sel3);
	//ADIOS_QUERY* result = (ADIOS_QUERY*)malloc(sizeof(ADIOS_QUERY));
	ADIOS_QUERY* result = (ADIOS_QUERY*) calloc(1, sizeof(ADIOS_QUERY));
	result->_condition = malloc(
			strlen(q1->_condition) + strlen(q2->_condition) + 10);

	if (operator == ADIOS_QUERY_OP_AND) {
		sprintf(result->_condition, "(%s and %s)", q1->_condition,
				q2->_condition);
	} else {
		sprintf(result->_condition, "(%s or %s)", q1->_condition,
				q2->_condition);
	}
	result->_condition[strlen(result->_condition)] = 0;

	result->_left = q1;
	result->_right = q2;
	result->_leftToRightOp = operator;

	initialize(result);
	return result;
}

int64_t common_query_estimate(ADIOS_QUERY* q) {
	gAdios_query_hooks[gAssigned_query_tool].adios_query_estimate_method_fn(q);
}

void common_query_set_timestep(int timeStep) {
	gCurrentTimeStep = timeStep;
}

int common_query_get_selection(ADIOS_QUERY* q,
//const char* varName,
		//int timeStep,
		uint64_t batchSize, // limited by maxResult
		ADIOS_SELECTION* outputBoundry, ADIOS_SELECTION** result) {
	gAdios_query_hooks[gAssigned_query_tool].adios_query_get_selection_method_fn(
			q, batchSize, outputBoundry, result);
}

