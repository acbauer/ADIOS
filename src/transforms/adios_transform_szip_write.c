#include <stdint.h>
#include <assert.h>
#include <limits.h>

#include "adios_logger.h"
#include "adios_transforms_common.h"
#include "adios_transforms_write.h"
#include "adios_transforms_hooks_write.h"

#ifdef SZIP

#include "adios_transform_szip.h"

#define TEST_SIZE(s) (s)

// assume double precision only
int compress_szip_pre_allocated(const void* input_data, const uint64_t input_len, 
								void* output_data, uint64_t* output_len,
								const int ndims, const uint64_t* dim)
{
	assert(input_data != NULL && input_len > 0 && output_data != NULL && output_len != NULL && *output_len > 0);
	
	int rtn = 0;
	
	SZ_com_t sz_param;
	
	rtn = init_szip_parameters(&sz_param, ndims, dim);
	if(rtn != 0)
	{
		return -1;
	}

	size_t temp = *output_len;
	
	rtn = SZ_BufftoBuffCompress(output_data, &temp, input_data, input_len, &sz_param);
	if(SZ_OK!= rtn)
	{
		printf("SZ_BufftoBuffCompress error %d\n", rtn);
		return -1;
	}

	*output_len = temp;
	
	return 0;
}

uint16_t adios_transform_szip_get_metadata_size() 
{ 
	return (sizeof(uint64_t));
}

uint64_t adios_transform_szip_calc_vars_transformed_size(uint64_t orig_size, int num_vars) {
    return TEST_SIZE(orig_size);
}

int adios_transform_szip_apply(struct adios_file_struct *fd, 
									struct adios_var_struct *var,
									uint64_t *transformed_len, 
									int use_shared_buffer, 
									int *wrote_to_shared_buffer)
{
    // Assume this function is only called for SZIP transform method
    assert(var->transform_type == adios_transform_szip);

    // Get the input data and data length
    const uint64_t input_size = adios_transform_get_pre_transform_var_size(fd->group, var);
    const void *input_buff= var->data;

    // decide the output buffer
    uint64_t output_size = TEST_SIZE(input_size);
    void* output_buff = NULL;

    uint64_t mem_allowed = 0;
    if (use_shared_buffer)
    {	
		*wrote_to_shared_buffer = 1;
		// If shared buffer is permitted, serialize to there
        if (!shared_buffer_reserve(fd, output_size))
        {
            log_error("Out of memory allocating %llu bytes for %s for szip transform\n", output_size, var->name);
            return 0;
        }

        // Write directly to the shared buffer
        output_buff = fd->buffer + fd->offset;
    }
    else	// Else, fall back to var->data memory allocation
    {
		*wrote_to_shared_buffer = 0;
		output_buff = malloc(output_size);
        if (!output_buff)
        {
            log_error("Out of memory allocating %llu bytes for %s for szip transform\n", output_size, var->name);
            return 0;
        }
    }

    // compress it
	uint64_t actual_output_size = output_size;
	int ndims = 1;
	uint64_t dim[1] = {input_size/sizeof(double)};
	int rtn = compress_szip_pre_allocated(input_buff, input_size, output_buff, &actual_output_size, ndims, dim);

    if(0 != rtn 					// compression failed for some reason, then just copy the buffer
        || actual_output_size > input_size)  // or size after compression is even larger (not likely to happen since compression lib will return non-zero in this case)
    {
        memcpy(output_buff, input_buff, input_size);
        actual_output_size = input_size;
    }

    // Wrap up, depending on buffer mode
    if (use_shared_buffer)
    {
        shared_buffer_mark_written(fd, actual_output_size);
    }
    else
    {
        var->data = output_buff;
        var->data_size = actual_output_size;
        var->free_data = adios_flag_yes;
    }

    // copy the metadata, simply the compress type
	if(var->transform_metadata && var->transform_metadata_len > 0)
    {
        memcpy(var->transform_metadata, &input_size, sizeof(uint64_t));
    }
	
	printf("adios_transform_compress_apply compress %d input_size %d actual_output_size %d\n", 
			rtn, input_size, actual_output_size);
	
	
    *transformed_len = actual_output_size; // Return the size of the data buffer
    return 1;
}

#else

DECLARE_TRANSFORM_WRITE_METHOD_UNIMPL(szip)

#endif
