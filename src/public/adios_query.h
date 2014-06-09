#ifndef __ADIOS_QUERY_H__
#define __ADIOS_QUERY_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <adios_read.h>

#define ADIOS_QUERY_TOOL_COUNT  2

int gCurrentTimeStep;

enum ADIOS_QUERY_TOOL 
{
	ADIOS_QUERY_TOOL_FASTBIT = 0,
	ADIOS_QUERY_TOOL_ALACRITY = 0,  // TODO: why do they have same value?
	ADIOS_QUERY_TOOL_OTHER = 1
};
    

enum ADIOS_PREDICATE_MODE 
{
	ADIOS_LT = 0,
	ADIOS_LTEQ = 1,
	ADIOS_GT = 2,
	ADIOS_GTEQ = 3,
	ADIOS_EQ = 4,
	ADIOS_NE = 5
};

enum ADIOS_CLAUSE_OP_MODE 
{
	ADIOS_QUERY_OP_AND = 0,
	ADIOS_QUERY_OP_OR  = 1
};

typedef struct {
    char* _condition;
    void* _queryInternal;

  // keeping start/count to map 1d results from fastbit to N-d

    ADIOS_SELECTION* _sel;
    void* _dataSlice;

    ADIOS_VARINFO* _var;
    ADIOS_FILE* _f;

    enum ADIOS_PREDICATE_MODE _op;
    char* _value;
    uint64_t _rawDataSize; // this is the result of dim/start+count

    void* _left;
    void* _right;
    enum ADIOS_CLAUSE_OP_MODE _leftToRightOp;

    int _onTimeStep; // dataSlice is obtained with this timeStep 

    uint64_t _maxResultDesired;
    uint64_t _lastRead;
} ADIOS_QUERY;
   


/* functions */
void adios_query_init(enum ADIOS_QUERY_TOOL tool);

ADIOS_QUERY* adios_query_create(ADIOS_FILE* f, 
				const char* varName,
				ADIOS_SELECTION* queryBoundry,
				enum ADIOS_PREDICATE_MODE op,
				const char* value); //this value needs to be &int &double etc, not a string!
					

ADIOS_QUERY* adios_query_combine(ADIOS_QUERY* q1, 
				 enum ADIOS_CLAUSE_OP_MODE operator,		    
				 ADIOS_QUERY* q2);

int64_t adios_query_estimate(ADIOS_QUERY* q );

void adios_query_set_timestep(int timeStep);


int64_t adios_query_evaluate(ADIOS_QUERY* q, 
			     int timeStep,
			     uint64_t maxResult);

int  adios_query_get_selection(ADIOS_QUERY* q, 
			       //const char* varName,
		 	 	   //int timeStep, // same as in evaluate
			       int batchSize, // limited by maxResult
			       ADIOS_SELECTION* outputBoundry,// must supply to get results
			       ADIOS_SELECTION** queryResult);


void adios_query_free(ADIOS_QUERY* q);

void adios_query_clean();
  
#ifdef __cplusplus
}
#endif

#endif /* __ADIOS_QUERY_H__ */
