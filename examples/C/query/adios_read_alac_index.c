/*
 * ADIOS is freely available under the terms of the BSD license described
 * in the COPYING file in the top level directory of this source distribution.
 *
 * Copyright (c) 2008 - 2009.  UT-BATTELLE, LLC. All rights reserved.
 */

/* ADIOS C Example: read global arrays from a BP file
 *
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <string.h>
#include <assert.h>
//#include "adios_read.h"
#include "adios_query.h"
//#include "adios_types.h"

void printPoints(const ADIOS_SELECTION_POINTS_STRUCT * pts){
	uint64_t i = 0;
	if (pts->ndim == 3){

		for(i = 0; i < pts->npoints; i++){
			printf("[ %"PRIu64", %"PRIu64", %"PRIu64" ] ,"
					, pts->points[i*3], pts->points[i*3+1], pts->points[i*3+ 2]);
		}

	}
	if (pts->ndim ==2){
		for(i = 0; i < pts->npoints; i++){
				printf("[ %"PRIu64", %"PRIu64" ] ,"
					, pts->points[i*2], pts->points[i*2 +1]);
		}
	}
	printf("\n");

}

ADIOS_QUERY * createQueryConstraints(ADIOS_FILE* bf, const char* varName , ADIOS_SELECTION* box
		, const char * lb, const char * hb){
	/* lb <= x <= hb */
    ADIOS_QUERY* q1 = adios_query_create(bf, varName, box, ADIOS_GTEQ, lb);
    ADIOS_QUERY* q2 = adios_query_create(bf, varName, box, ADIOS_LTEQ, hb);
    ADIOS_QUERY* q = adios_query_combine(q1, ADIOS_QUERY_OP_AND, q2);
    return q;

    /* x != lb
     * return adios_query_create(bf, varName, box, ADIOS_NE, lb);*/
}

#define CHECK_ERROR_DATA(data, num, check) {                     \
		 uint64_t di = 0;                                        \
		 for(di = 0; di < (num); di++){                          \
				if (check)                                       \
					printf("error data: %f, ", (data)[di]);      \
		 }                                                       \
}


void multiBoundBox(ADIOS_FILE* bf , const char * b1, const char * b2, ADIOS_FILE *dataF) {
  printf("\n=============== testing multiple bound box  ===========\n");
  const char* varName1 = "temp";
  int ndim = 3;
  uint64_t start1[] = {0, 0, 0}; // block 0 -> 1st block
  uint64_t count1[] = {64, 10, 32};
  ADIOS_SELECTION *box1 = adios_selection_boundingbox(ndim, start1, count1);

  uint64_t start2[] = {0, 32, 0}; //block 2 -> 3rd block
  uint64_t count2[] = {64, 10,32};
  ADIOS_SELECTION* box2 = adios_selection_boundingbox(ndim, start2, count2);


/*
  uint64_t start3[] = {64, 32, 0}; //block 4 -> 5th block
  uint64_t count3[] = {64, 10,32};
  ADIOS_SELECTION* outBox = adios_selection_boundingbox(ndim, start3, count3);
*/

  ADIOS_VARINFO * dataV = adios_inq_var(dataF, varName1);
  double lb1v = atof(b1);
  double lb2v = atof(b2);
  int timestep = 0;
  adios_query_set_timestep(timestep);
  int64_t batchSize = 1000;

  ADIOS_QUERY* q1 = adios_query_create(bf, varName1, box1, ADIOS_GT, b1); // > b1
  ADIOS_QUERY* q2 = adios_query_create(bf, varName1, box2, ADIOS_LT, b2); // < b2
  // if box has a different shape, e.g. different count values, then combine() returns error
  ADIOS_QUERY* q = adios_query_combine(q1, ADIOS_QUERY_OP_AND, q2);


  // box3 is the same shape as other boxes , if it is in different shape from box1/box2, then error
  ADIOS_SELECTION* outBox = box1;
  while (1) {
    ADIOS_SELECTION* currBatch = NULL;
    int hasMore =  adios_query_get_selection(q, batchSize, outBox, &currBatch);
    if (currBatch == NULL)  // there is no results at all
    	break;


    const ADIOS_SELECTION_POINTS_STRUCT * retrievedPts = &(currBatch->u.points);
    printf("retrieved points %" PRIu64 " \n",  retrievedPts->npoints);

    double * data = (double *) malloc(retrievedPts->npoints * sizeof(double));
    adios_schedule_read_byid (dataF, currBatch, dataV->varid, 0, 1, data);
    adios_perform_reads(dataF, 1);
    CHECK_ERROR_DATA(data, retrievedPts->npoints, (data[di] <= lb1v ));
    free(data);
    adios_selection_delete(currBatch);
    if (hasMore == 0) { // there is no left results to retrieve
              break;
    }
  }

     //reset query engine
  q->_onTimeStep = -1; // NO_EVAL_BEFORE
  outBox = box2; // switch to different output box
  while (1) {
    ADIOS_SELECTION* currBatch = NULL;
    int hasMore =  adios_query_get_selection(q, batchSize, outBox, &currBatch);
    if (currBatch == NULL)  // there is no results at all
         	break;

    const ADIOS_SELECTION_POINTS_STRUCT * retrievedPts = &(currBatch->u.points);
    printf("retrieved points %" PRIu64 " \n",  retrievedPts->npoints);

    double * data = (double *) malloc(retrievedPts->npoints * sizeof(double));
    adios_schedule_read_byid (dataF, currBatch, dataV->varid, 0, 1, data);
    adios_perform_reads(dataF, 1);
    CHECK_ERROR_DATA(data, retrievedPts->npoints, (data[di] >= lb2v ));
    free(data);
    adios_selection_delete(currBatch);
    if (hasMore == 0) { // there is no left results to retrieve
                  break;
     }
  }

  adios_query_free(q);

  adios_selection_delete(box1);
  adios_selection_delete(box2);
}

void oneDefinedBox(ADIOS_FILE* bf , const char * lb, const char * hb, ADIOS_FILE *dataF){

	  printf("\n=============== testing one single bounding box ===========\n");
	  // temp data is in the range btw 100 and 200
	  const char* varName1 = "temp";
	  int ndim = 3;
	  uint64_t start1[] = {0, 0, 0};
	  uint64_t count1[] = {64, 32, 32};
	  ADIOS_SELECTION *box1 = adios_selection_boundingbox(ndim, start1, count1);
	  ADIOS_VARINFO * dataV = adios_inq_var(dataF, varName1);
      double lbv = atof(lb);
      double hbv = atof(hb);
      int timestep = 0;
      adios_query_set_timestep(timestep);
      int64_t batchSize = 10000;

      ADIOS_QUERY * q = createQueryConstraints(bf, varName1, box1, lb, hb);
//      int64_t estimateResults = adios_query_estimate(q1);
//      printf("estimated result number %"PRIu64 " \n", estimateResults);

      while (1) {
        ADIOS_SELECTION* currBatch = NULL;
        int hasMore =  adios_query_get_selection(q, batchSize, box1, &currBatch);
        if (currBatch == NULL)  // there is no results at all
        	break;

        assert(currBatch->type ==ADIOS_SELECTION_POINTS);
        const ADIOS_SELECTION_POINTS_STRUCT * retrievedPts = &(currBatch->u.points);
        printf("retrieved points %" PRIu64 " \n",  retrievedPts->npoints);

        double * data = (double *) malloc(retrievedPts->npoints * sizeof(double));
        adios_schedule_read_byid (dataF, currBatch, dataV->varid, 0, 1, data);
        adios_perform_reads(dataF, 1);
        // lb <= x <= lb
        CHECK_ERROR_DATA(data, retrievedPts->npoints, (data[di] > hbv || data[di] < lbv));
//        CHECK_ERROR_DATA(data, retrievedPts->npoints, (data[di] == lbv));
        free(data);
        	//        printPoints(retrievedPts);
        adios_selection_delete(currBatch);

        if (hasMore == 0) { // there is no left results to retrieve
          break;
        }
      }

      adios_query_free(q);
      adios_selection_delete(box1);

}


int main (int argc, char ** argv)
{

    char        filename [256];
    int         i, j, datasize, if_any;
    MPI_Comm    comm = MPI_COMM_WORLD;
    enum ADIOS_READ_METHOD method = ADIOS_READ_METHOD_BP;
    ADIOS_SELECTION * sel1, * sel2;
    ADIOS_VARCHUNK * chunk = 0;
    double * data = NULL;
    uint64_t start[2], count[2], npoints, * points;
    MPI_Init (&argc, &argv);
    if (argc < 2 ){
    	printf(" usage: %s {input bp file}, {lb} {hb} {bp file without transform} \n", argv[0]);
    	return 1;
    }

    char lbstr[255], hbstr[255];
    argc >= 3 ? strcpy(lbstr, argv[2]) : strcpy(lbstr, "0.0");
    argc >= 4 ? strcpy(hbstr, argv[3]) : strcpy(hbstr, "0.0");

    char dataFileName[256];
    if (argc == 5) {
    	strcpy(dataFileName, argv[4]);
    }else{
    	strcpy(dataFileName, "./xml/alacrity-1var-no-transform_524288.bp");
    }

    adios_read_init_method (method, comm, NULL);

    ADIOS_FILE * f = adios_read_open_file (argv[1], method, comm);

    if ( f == NULL){
    	MPI_Finalize ();
    	printf(" can not open file %s \n", argv[1]);
    	return 1;
    }

    ADIOS_FILE * dataF = adios_read_open_file (dataFileName, method, comm);

    if ( dataF == NULL){
    	if (f) {
		  adios_read_close (f);
		  MPI_Finalize ();
    	}
		printf(" can not open file %s \n", dataFileName);
		return 1;
    }

    adios_query_init(ADIOS_QUERY_TOOL_ALACRITY);

    //====================== start to test ===================//
    oneDefinedBox(f , lbstr, hbstr, dataF);
    multiBoundBox(f , lbstr, hbstr, dataF);


    adios_read_close (f);
    adios_read_close (dataF);

    adios_read_finalize_method (ADIOS_READ_METHOD_BP);


    MPI_Finalize ();
    return 0;
}
