
/*
    adios_flexpath.c
    uses evpath for io in conjunction with read/read_flexpath.c
*/

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <errno.h>

#include <pthread.h>

// xml parser
#include <mxml.h>

// add by Kimmy 10/15/2012
#include <sys/types.h>
#include <sys/stat.h>
// end of change

#ifdef __MACH__
#include <mach/clock.h>
#include <mach/mach.h>
#endif

#include "public/adios.h"
#include "public/adios_mpi.h"
#include "public/adios_error.h"
#include "core/adios_transport_hooks.h"
#include "core/adios_bp_v1.h"
#include "core/adios_internals.h"
#include "core/buffer.h"
#include "core/util.h"
#include "core/adios_logger.h"

#ifdef HAVE_ICEE

// // evpath libraries
#include <evpath.h>
#include <cod.h>
#include <sys/queue.h>

///////////////////////////
// Global Variables
///////////////////////////
#include "icee.h"

#define DUMP(fmt, ...) fprintf(stderr, ">>> "fmt"\n", ## __VA_ARGS__); 

static int adios_icee_initialized = 0;

CManager cm;
EVstone stone;
EVstone remote_stone;
EVsource source;

icee_fileinfo_rec_ptr_t fp = NULL;

int get_ndims(struct adios_var_struct *f)
{
    struct adios_var_struct * item;
    int ndims = 0;

    item = f;
    while (item != NULL)
    {
        item = item->next;
        ndims++;
    }

    return ndims;
}

// Initializes flexpath write local data structures
extern void 
adios_icee_init(const PairStruct *params, struct adios_method_struct *method) 
{
    log_debug ("%s\n", __FUNCTION__);
    
    int cm_port = 59999;
    char *cm_host = "localhost";
    char *cm_attr = NULL;
    attr_list contact_list;

    int rank;
    MPI_Comm_rank(method->init_comm, &rank);
    log_debug ("rank : %d\n", rank);
    

    const PairStruct * p = params;

    while (p)
    {
        if (!strcasecmp (p->name, "cm_attr"))
        {
            cm_attr = p->value;
        }
        else if (!strcasecmp (p->name, "cm_host"))
        {
            cm_host = p->value;
        }
        else if (!strcasecmp (p->name, "cm_port"))
        {
            cm_port = atoi(p->value);
        }
        else if (!strcasecmp (p->name, "cm_list"))
        {
            char **plist;
            int plen = 8;

            plist = malloc(plen * sizeof(char *));

            char* token = strtok(p->value, ",");
            int len = 0;
            while (token) 
            {
                plist[len] = token;

                token = strtok(NULL, ",");
                len++;

                if (len > plen)
                {
                    plen = plen*2;
                    realloc (plist, plen * sizeof(char *));
                }
            }

            char *myparam = plist[rank % len];
            token = strtok(myparam, ":");

            if (myparam[0] == ':')
            {
                cm_port = atoi(token);
            }
            else
            {
                cm_host = token;
                token = strtok(NULL, ":");
                cm_port = atoi(token);
            }

            free(plist);
        }

        p = p->next;
    }

    //log_info ("cm_attr : %s\n", cm_attr);
    log_info ("cm_host : %s\n", cm_host);
    log_info ("cm_port : %d\n", cm_port);

    if (!adios_icee_initialized)
    {
        cm = CManager_create();
        CMlisten(cm);
        
        stone = EValloc_stone(cm);

        contact_list = create_attr_list();
        add_int_attr(contact_list, attr_atom_from_string("IP_PORT"), cm_port);
        add_string_attr(contact_list, attr_atom_from_string("IP_HOST"), cm_host);

        EVaction evaction = EVassoc_bridge_action(cm, stone, contact_list, remote_stone);
        if (evaction == -1)
        {
            fprintf(stderr, "No connection. Exit.\n");
            exit(1);
        }

        source = EVcreate_submit_handle(cm, stone, icee_fileinfo_format_list);

        adios_icee_initialized = 1;
    }
}

extern int 
adios_icee_open(struct adios_file_struct *fd, 
		    struct adios_method_struct *method, 
		    MPI_Comm comm) 
{    
    log_debug ("%s\n", __FUNCTION__);

    if( fd == NULL || method == NULL) {
        perror("open: Bad input parameters\n");
        return -1;
    }

    if (fp == NULL) fp = calloc(1, sizeof(icee_fileinfo_rec_t));
    
    fp->fname = fd->name;
    MPI_Comm_rank(comm, &(fp->rank));

    return 0;	
}

//  writes data to multiqueue
extern void
adios_icee_write(
    struct adios_file_struct *fd, 
    struct adios_var_struct *f, 
    void *data, 
    struct adios_method_struct *method) 
{
    log_debug ("%s\n", __FUNCTION__);

    if( fd == NULL || method == NULL) {
        perror("open: Bad input parameters\n");
    }

    icee_varinfo_rec_ptr_t vp = fp->varinfo;
    icee_varinfo_rec_ptr_t prev = NULL;

    while (vp != NULL)
    {
        prev = vp;
        vp = vp->next;
    }

    vp = calloc(1, sizeof(icee_varinfo_rec_t));

    if (prev == NULL)
        fp->varinfo = vp;
    else
        prev->next = vp;

    if (f->path[0] == '\0')
        vp->varname = strdup(f->name);
    else
    {
        char buff[80];
        sprintf(buff, "%s/%s", f->path, f->name);
        vp->varname = strdup(buff);
    }

    vp->varid = f->id;
    if (adios_verbose_level > 3) DUMP("id,name = %d,%s", vp->varid, vp->varname);
    vp->type = f->type;
    vp->typesize = adios_get_type_size(f->type, ""); 

    vp->ndims = count_dimensions(f->dimensions);

    vp->varlen = vp->typesize;
    if (vp->ndims > 0)
    {
        vp->gdims = calloc(vp->ndims, sizeof(uint64_t));
        vp->ldims = calloc(vp->ndims, sizeof(uint64_t));
        vp->offsets = calloc(vp->ndims, sizeof(uint64_t));
        
        struct adios_dimension_struct *d = f->dimensions;
        // Default: Fortran. 
        if (fd->group->adios_host_language_fortran == adios_flag_yes)
        {
            int i;
            for (i = 0; i < vp->ndims; ++i)
            {
                vp->gdims[i] = adios_get_dim_value(&d->global_dimension);
                vp->ldims[i] = adios_get_dim_value(&d->dimension);
                vp->offsets[i] = adios_get_dim_value(&d->local_offset);
                
                vp->varlen *= vp->ldims[i];
                
                d = d->next;
            }
        }
        else
        {
            int i;
            for (i = vp->ndims-1; i >= 0; --i)
            {
                vp->gdims[i] = adios_get_dim_value(&d->global_dimension);
                vp->ldims[i] = adios_get_dim_value(&d->dimension);
                vp->offsets[i] = adios_get_dim_value(&d->local_offset);
                
                vp->varlen *= vp->ldims[i];
                
                d = d->next;
            }
        }
    }
    
    vp->data = f->data;

    fp->nvars++;
}

extern void 
adios_icee_close(struct adios_file_struct *fd, struct adios_method_struct *method) 
{
    log_debug ("%s\n", __FUNCTION__);

    if( fd == NULL || method == NULL) {
        perror("open: Bad input parameters\n");
    }

    // Write data to the network
    EVsubmit(source, fp, NULL);

    // Free
    icee_varinfo_rec_ptr_t vp = fp->varinfo;
    while (vp != NULL)
    {
        free(vp->varname);
        free(vp->gdims);
        free(vp->ldims);
        free(vp->offsets);

        icee_varinfo_rec_ptr_t prev = vp;
        vp = vp->next;

        free(prev);
    }

    free(fp);
    fp = NULL;
}

// wait until all open files have finished sending data to shutdown
extern void 
adios_icee_finalize(int mype, struct adios_method_struct *method) 
{
    log_debug ("%s\n", __FUNCTION__);

    if (adios_icee_initialized)
    {
        CManager_close(cm);
        adios_icee_initialized = 0;
    }
}

// provides unknown functionality
extern enum ADIOS_FLAG 
adios_icee_should_buffer (struct adios_file_struct * fd,struct adios_method_struct * method) 
{
    return adios_flag_no;
}

// provides unknown functionality
extern void 
adios_icee_end_iteration(struct adios_method_struct *method) 
{
}

// provides unknown functionality
extern void 
adios_icee_start_calculation(struct adios_method_struct *method) 
{
}

// provides unknown functionality
extern void 
adios_icee_stop_calculation(struct adios_method_struct *method) 
{
}

// provides unknown functionality
extern void 
adios_icee_get_write_buffer(struct adios_file_struct *fd, 
				struct adios_var_struct *v, 
				uint64_t *size, 
				void **buffer, 
				struct adios_method_struct *method) 
{
}

// should not be called from write, reason for inclusion here unknown
void 
adios_icee_read(struct adios_file_struct *fd, 
		    struct adios_var_struct *f, 
		    void *buffer, 
		    uint64_t buffer_size, 
		    struct adios_method_struct *method) 
{
}

#else // print empty version of all functions (if HAVE_FLEXPATH == 0)

void 
adios_icee_read(struct adios_file_struct *fd, 
		    struct adios_var_struct *f, 
		    void *buffer, 
		    struct adios_method_struct *method) 
{
}

extern void 
adios_icee_get_write_buffer(struct adios_file_struct *fd, 
				struct adios_var_struct *f, 
				unsigned long long *size, 
				void **buffer, 
				struct adios_method_struct *method) 
{
}

extern void 
adios_icee_stop_calculation(struct adios_method_struct *method) 
{
}

extern void 
adios_icee_start_calculation(struct adios_method_struct *method) 
{
}

extern void 
adios_icee_end_iteration(struct adios_method_struct *method) 
{
}

#endif