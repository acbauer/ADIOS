#include <stdint.h>
#include <assert.h>
#include <limits.h>
#include <sys/time.h>

#include "adios_logger.h"
#include "adios_transforms_common.h"
#include "adios_transforms_write.h"
#include "adios_transforms_hooks_write.h"

#ifdef ISOBAR

#include "isobar.h"

#define TEST_SIZE(s) (s)

#define ELEMENT_BYTES	8

static double dclock()
{
	struct timeval tv;
	gettimeofday(&tv,0);
	return (double) tv.tv_sec + (double) tv.tv_usec * 1e-6;
}

static int is_digit_str(char* input_str)
{
	return 1;
}

int compress_isobar_pre_allocated(const void* input_data, const uint64_t input_len, 
								void* output_data, uint64_t* output_len, int compress_level)
{
	assert(input_data != NULL && input_len > 0 && output_data != NULL && output_len != NULL && *output_len > 0);
	
	enum ISOBAR_status status;
	struct isobar_stream i_strm;

	status = isobarDeflateInit(&i_strm, ELEMENT_BYTES, compress_level);
	if(status != ISOBAR_SUCCESS)
	{
		// cout << "isobarDeflateInit error" << endl;
		return -1;
	}

	i_strm.next_in = (void*) input_data;
	i_strm.avail_in = input_len;

	status = isobarDeflateAnalysis(&i_strm);
	if(status != ISOBAR_SUCCESS)
	{
		// cout << "isobarDeflateAnalysis error" << endl;
		return -1;
	}

	i_strm.next_out = (void*) output_data;
	i_strm.avail_out = input_len;

//	printf("i_strm.avail_in: %d i_strm.avail_out: %d buffer_size: %d\n", i_strm.avail_in, i_strm.avail_out, buffer_size);

	status = isobarDeflate(&i_strm, ISOBAR_FINISH);
	if(status != ISOBAR_SUCCESS)
	{
		// cout << "isobarDeflate error" << endl;
		return -1;
	}

//	printf("i_strm.avail_in: %d i_strm.avail_out: %d buffer_size: %d\n", i_strm.avail_in, i_strm.avail_out, buffer_size);

	status = isobarDeflateEnd(&i_strm);
	if(status != ISOBAR_SUCCESS)
	{
		// cout << "isobarDeflateEnd error" << endl;
		return -1;
	}
	
	*output_len = input_len - i_strm.avail_out;
	return 0;
}

uint16_t adios_transform_isobar_get_metadata_size() 
{ 
	return (sizeof(uint64_t));
}

uint64_t adios_transform_isobar_calc_vars_transformed_size(uint64_t orig_size, int num_vars) 
{
    return TEST_SIZE(orig_size);
}

int adios_transform_isobar_apply(struct adios_file_struct *fd, 
								struct adios_var_struct *var,
								uint64_t *transformed_len, 
								int use_shared_buffer, 
								int *wrote_to_shared_buffer)
{
    // Assume this function is only called for COMPRESS transform type
    assert(var->transform_type == adios_transform_isobar);

    // Get the input data and data length
    const uint64_t input_size = adios_transform_get_pre_transform_var_size(fd->group, var);
    const void *input_buff= var->data;

	// parse the compressiong parameter
	int compress_level = ISOBAR_SPEED;
	if(var->transform_type_param 
		&& strlen(var->transform_type_param) > 0 
		&& is_digit_str(var->transform_type_param))
	{
		compress_level = atoi(var->transform_type_param);
		if(compress_level > 9 || compress_level < 1)
		{
			compress_level = ISOBAR_SPEED;
		}
	}
	
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
            log_error("Out of memory allocating %llu bytes for %s for isobar transform\n", output_size, var->name);
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
            log_error("Out of memory allocating %llu bytes for %s for isobar transform\n", output_size, var->name);
            return 0;
        }
    }

    // compress it
	uint64_t actual_output_size = output_size;
    
	double d1 = dclock();
	int rtn = compress_isobar_pre_allocated(input_buff, input_size, output_buff, &actual_output_size, compress_level);
	double d2 = dclock();
	
    if(0 != rtn 					// compression failed for some reason, then just copy the buffer
        || actual_output_size > input_size)  // or size after compression is even larger (not likely to happen since compression lib will return non-zero in this case)
    {
        memcpy(output_buff, input_buff, input_size);
        actual_output_size = input_size;
    }
	
	// printf("compress_isobar_succ|%d|%d|%d|%f\n", rtn, input_size, actual_output_size, d2 - d1);

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
	
	// printf("adios_transform_compress_apply compress %d input_size %d actual_output_size %d\n", 
			// rtn, input_size, actual_output_size);
	
    *transformed_len = actual_output_size; // Return the size of the data buffer
	
    return 1;
}

#else

DECLARE_TRANSFORM_WRITE_METHOD_UNIMPL(isobar)

#endif
