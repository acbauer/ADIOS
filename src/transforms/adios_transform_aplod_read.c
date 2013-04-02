#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <assert.h>

#include "util.h"
#include "adios_transforms_hooks_read.h"
#include "adios_transforms_reqgroup.h"

#ifdef APLOD

#include "aplod.h"

int decompress_aplod_pre_allocated(const void* input_data, const uint64_t input_len,
                                    void* output_data, uint64_t* output_len)
{
    assert(input_data != NULL && input_len > 0 && output_data != NULL && output_len != NULL && *output_len > 0);

    // uLongf dest_temp = *output_len;

    // printf("decompress_aplod_pre_allocated %d %d\n", dest_temp, input_len);

    // int z_rtn = uncompress((Bytef*)output_data, &dest_temp, (Bytef*)input_data, input_len);
    // if(z_rtn != Z_OK)
    // {
        // printf("aplod uncompress error %d\n", z_rtn);
        // return -1;
    // }

    // *output_len = (uint64_t)dest_temp;

    return 0;
}

int adios_transform_aplod_generate_read_subrequests(adios_transform_read_request *reqgroup,
                                                    adios_transform_pg_read_request *pg_reqgroup)
{

    assert(reqgroup && pg_reqgroup);

    void *buf = malloc(pg_reqgroup->raw_var_length);

    // printf("[adios_transform_aplod_generate_read_subrequests] raw_var_length %d %d %d %d %d\n",
            // pg_reqgroup->raw_var_length, pg_reqgroup->raw_varblock->start[0], pg_reqgroup->raw_varblock->count[0],
            // pg_reqgroup->raw_varblock->start[1], pg_reqgroup->raw_varblock->count[1]);

    // adios_transform_raw_read_request *subreq = adios_transform_raw_read_request_new(pg_reqgroup->raw_varblock, buf);
    // adios_transform_raw_read_request_append(pg_reqgroup, subreq);
	
	adios_transform_raw_read_request *subreq = adios_transform_raw_read_request_new_whole_pg(pg_reqgroup->raw_varblock, buf);
    adios_transform_raw_read_request_append(pg_reqgroup, subreq);

    return 0;
}

// Do nothing for individual subrequest
adios_datablock * adios_transform_aplod_subrequest_completed(adios_transform_read_request *reqgroup,
                                                            adios_transform_pg_read_request *pg_reqgroup,
                                                            adios_transform_raw_read_request *completed_subreq)
{
    return NULL;
}



adios_datablock * adios_transform_aplod_pg_reqgroup_completed(adios_transform_read_request *reqgroup,
                                                             adios_transform_pg_read_request *completed_pg_reqgroup)
{
    uint64_t compressed_len = (uint64_t)completed_pg_reqgroup->raw_var_length;
    void* compressed_buff = completed_pg_reqgroup->subreqs->data;

    uint64_t decompressed_len_test = adios_get_type_size(reqgroup->transinfo->orig_type, "");
    int d = 0;
    for(d = 0; d < reqgroup->raw_varinfo->ndim; d++)
    {
        decompressed_len_test *= (uint64_t)(completed_pg_reqgroup->orig_varblock->count[d]);
    }

    // retrieve the original buffer length from metadata
    uint64_t decompressed_len = *((uint64_t*)(reqgroup->transinfo->transform_metadata));
    void* decompressed_buff = malloc(decompressed_len);

    int rtn = decompress_aplod_pre_allocated(compressed_buff, compressed_len, decompressed_buff, &decompressed_len);
    if(0 != rtn)
    {
        if(decompressed_buff)
        {
            free(decompressed_buff);
            decompressed_buff = NULL;
        }
        return NULL;
    }

    return adios_datablock_new(reqgroup->transinfo->orig_type,
                               completed_pg_reqgroup->timestep,
                               completed_pg_reqgroup->pg_bounds_sel,
                               decompressed_buff);
}

adios_datablock * adios_transform_aplod_reqgroup_completed(adios_transform_read_request *completed_reqgroup)
{
    return NULL;
}


#else

DECLARE_TRANSFORM_READ_METHOD_UNIMPL(aplod);

#endif
