/*
 * ADIOS is freely available under the terms of the BSD license described
 * in the COPYING file in the top level directory of this source distribution.
 *
 * Copyright (c) 2008 - 2009.  UT-BATTELLE, LLC. All rights reserved.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include "public/adios_error.h"
#include "core/adios_logger.h"
#include "core/common_read.h"
#include "core/futils.h"
#include "core/bp_utils.h" // struct namelists_struct

// NCSU ALACRITY-ADIOS
#include "adios_read_hooks.h"
//#include "transforms/adios_transforms_transinfo.h"
#include "transforms/adios_transforms_hooks_read.h"
#include "transforms/adios_transforms_reqgroup.h"
#include "transforms/adios_transforms_datablock.h"
#define BYTE_ALIGN 8

#ifdef DMALLOC
#include "dmalloc.h"
#endif


/* Note: MATLAB reloads the mex64 files each time, so all static variables get the original value.
   Therefore static variables cannot be used to pass info between two Matlab/ADIOS calls */
static struct adios_read_hooks_struct * adios_read_hooks = 0;

struct common_read_internals_struct {
    enum ADIOS_READ_METHOD method;
    struct adios_read_hooks_struct * read_hooks; /* Save adios_read_hooks for each fopen for Matlab */

    /* Group view information *//* Actual method provides the group names */
    int     ngroups;
    char ** group_namelist;
    int   * nvars_per_group;     /* # of variables per each group */
    int   * nattrs_per_group;    /* # of attributes per each group */
    int     group_in_view;       /* 0..ngroups-1: selected group in view,
                                  -1: all groups */
    int     group_varid_offset;  /* offset of var IDs from specific group to full list
                                    if a selected group is in view */
    int     group_attrid_offset;
    int     full_nvars;          /* fp->nvars to save here for a group view */
    char ** full_varnamelist;    /* fp->var_namelist to save here if one group is viewed */
    int     full_nattrs;         /* fp->nvars to save here for a group view */
    char ** full_attrnamelist;   /* fp->attr_namelist to save here if one group is viewed */

    // NCSU ALACRITY-ADIOS - Table of sub-requests issued by transform method
    adios_transform_read_request *transform_reqgroups;

    // NCSU ALACRITY-ADIOS - Cache of VARINFOs and TRANSINFOs
    adios_transform_infocache *infocache;
};

// NCSU ALACRITY-ADIOS - Forward declaration/function prototypes
static void common_read_free_blockinfo(ADIOS_VARBLOCK **varblock, int sum_nblocks);



int common_read_init_method (enum ADIOS_READ_METHOD method,
                             MPI_Comm comm,
                             const char * parameters)
{
    PairStruct *params, *p, *prev_p;
    int verbose_level, removeit, save;
    int retval;
    char *end;

    adios_errno = err_no_error;
    if ((int)method < 0 || (int)method >= ADIOS_READ_METHOD_COUNT) {
        adios_error (err_invalid_read_method,
            "Invalid read method (=%d) passed to adios_read_init_method().\n", (int)method);
        return err_invalid_read_method;
    }
    // init the adios_read_hooks_struct if not yet initialized
    adios_read_hooks_init (&adios_read_hooks);
    // NCSU ALACRITY-ADIOS - Initialize transform methods
    adios_transform_read_init();

    // process common parameters here
    params = text_to_name_value_pairs (parameters);
    p = params;
    prev_p = NULL;
    while (p) {
        removeit = 0;
        if (!strcasecmp (p->name, "verbose"))
        {
            if (p->value) {
                errno = 0;
                verbose_level = strtol(p->value, &end, 10);
                if (errno || (end != 0 && *end != '\0')) {
                    log_error ("Invalid 'verbose' parameter passed to read init function: '%s'\n", p->value);
                    verbose_level = 1; // print errors only
                }
            } else {
                verbose_level = 3;  // info level
            }
            adios_verbose_level = verbose_level;
            removeit = 1;
        }
        else if (!strcasecmp (p->name, "quiet"))
        {
            adios_verbose_level = 0; //don't print errors
            removeit = 1;
        }
        else if (!strcasecmp (p->name, "logfile"))
        {
            if (p->value) {
                adios_logger_open (p->value, -1);
            }
            removeit = 1;
        }
        else if (!strcasecmp (p->name, "abort_on_error"))
        {
            adios_abort_on_error = 1;
            save = adios_verbose_level;
            adios_verbose_level = 2;
            log_warn ("ADIOS is set to abort on error\n");
            adios_verbose_level = save;
            removeit = 1;
        }
        if (removeit) {
            if (p == params) {
                // remove head
                p = p->next;
                params->next = NULL;
                free_name_value_pairs (params);
                params = p;
            } else {
                // remove from middle of the list
                prev_p->next = p->next;
                p->next = NULL;
                free_name_value_pairs (p);
                p = prev_p->next;
            }
        } else {
            prev_p = p;
            p = p->next;
        }
    }

    // call method specific init
    retval = adios_read_hooks[method].adios_init_method_fn (comm, params);
    free_name_value_pairs (params);
    return retval;
}


int common_read_finalize_method(enum ADIOS_READ_METHOD method)
{
    adios_errno = err_no_error;
    if ((int)method < 0 || (int)method >= ADIOS_READ_METHOD_COUNT) {
        adios_error (err_invalid_read_method,
            "Invalid read method (=%d) passed to adios_read_finalize_method().\n", (int)method);
        return err_invalid_read_method;
    }

    return adios_read_hooks[method].adios_finalize_method_fn ();
}


ADIOS_FILE * common_read_open (const char * fname,
                               enum ADIOS_READ_METHOD method,
                               MPI_Comm comm,
                               enum ADIOS_LOCKMODE lock_mode,
                               float timeout_sec)
{
    ADIOS_FILE * fp;
    struct common_read_internals_struct * internals;

    if ((int)method < 0 || (int)method >= ADIOS_READ_METHOD_COUNT) {
        adios_error (err_invalid_read_method,
            "Invalid read method (=%d) passed to adios_read_open().\n", (int)method);
        return NULL;
    }

    adios_errno = err_no_error;
    internals = (struct common_read_internals_struct *)
                    calloc(1,sizeof(struct common_read_internals_struct));
    // init the adios_read_hooks_struct if not yet initialized
    adios_read_hooks_init (&adios_read_hooks);
    // NCSU ALACRITY-ADIOS - Initialize transform methods
    adios_transform_read_init();

    internals->method = method;
    internals->read_hooks = adios_read_hooks;

    // NCSU ALACRITY-ADIOS - Added allocation of infocache for more efficient read processing with transforms
    internals->infocache = adios_transform_infocache_new();

    fp = adios_read_hooks[internals->method].adios_open_fn (fname, comm, lock_mode, timeout_sec);

    // save the method and group information in fp->internal_data
    if (fp){
        adios_read_hooks[internals->method].adios_get_groupinfo_fn (fp, &internals->ngroups,
                &internals->group_namelist, &internals->nvars_per_group, &internals->nattrs_per_group);
        internals->group_in_view = -1;
        internals->group_varid_offset = 0;
        internals->group_attrid_offset = 0;
        fp->internal_data = (void *)internals;
    } else {
        free (internals);
    }
    return fp;
}


ADIOS_FILE * common_read_open_file (const char * fname,
                                    enum ADIOS_READ_METHOD method,
                                    MPI_Comm comm)
{
    ADIOS_FILE * fp;
    struct common_read_internals_struct * internals;

    if ((int)method < 0 || (int)method >= ADIOS_READ_METHOD_COUNT) {
        adios_error (err_invalid_read_method,
            "Invalid read method (=%d) passed to adios_read_open_file().\n", (int)method);
        return NULL;
    }

    adios_errno = err_no_error;
    internals = (struct common_read_internals_struct *)
                    calloc(1,sizeof(struct common_read_internals_struct));
    // init the adios_read_hooks_struct if not yet initialized
    adios_read_hooks_init (&adios_read_hooks);
    // NCSU ALACRITY-ADIOS - Initialize transform methods
    adios_transform_read_init();

    internals->method = method;
    internals->read_hooks = adios_read_hooks;

    // NCSU ALACRITY-ADIOS - Added allocation of infocache for more efficient read processing with transforms
    internals->infocache = adios_transform_infocache_new();

    fp = adios_read_hooks[internals->method].adios_open_file_fn (fname, comm);

    // save the method and group information in fp->internal_data
    if (fp){
        adios_read_hooks[internals->method].adios_get_groupinfo_fn (fp, &internals->ngroups,
                &internals->group_namelist, &internals->nvars_per_group, &internals->nattrs_per_group);
        internals->group_in_view = -1;
        internals->group_varid_offset = 0;
        internals->group_attrid_offset = 0;
        fp->internal_data = (void *)internals;
    } else {
        free (internals);
    }
    return fp;
}

// NCSU ALACRITY-ADIOS - Cleanup for read request groups
#define MYFREE(p) {if (p) free((void*)p); (p)=NULL;}
static void clean_up_read_reqgroups(adios_transform_read_request **reqgroups_head) {
    adios_transform_read_request *removed;
    while ((removed = adios_transform_read_request_pop(reqgroups_head)) != NULL) {
        adios_transform_read_request_free(&removed);
    }
}
#undef MYFREE

int common_read_close (ADIOS_FILE *fp)
{
    struct common_read_internals_struct * internals;
    int retval;

    adios_errno = err_no_error;
    if (fp) {
        internals = (struct common_read_internals_struct *) fp->internal_data;
        if (internals->group_in_view != -1) {
            // reset from group view before calling the real close
            common_read_group_view (fp, -1);
        }
        retval = internals->read_hooks[internals->method].adios_close_fn (fp);
        free_namelist (internals->group_namelist, internals->ngroups);
        free (internals->nvars_per_group);
        free (internals->nattrs_per_group);

        // NCSU ALACRITY-ADIOS - Cleanup read request groups and infocache
        clean_up_read_reqgroups(&internals->transform_reqgroups);
        adios_transform_infocache_free(&internals->infocache);

        free (internals);
    } else {
        adios_error ( err_invalid_file_pointer, "Invalid file pointer at adios_read_close()\n");
        retval = err_invalid_file_pointer;
    }
    return retval;
}

void common_read_reset_dimension_order (const ADIOS_FILE *fp, int is_fortran)
{
    struct common_read_internals_struct * internals;

    adios_errno = err_no_error;
    if (fp) {
        internals = (struct common_read_internals_struct *) fp->internal_data;
        internals->read_hooks[internals->method].adios_reset_dimension_order_fn (fp, is_fortran);
    } else {
        adios_error ( err_invalid_file_pointer, "Invalid file pointer at adios_reset_dimension_order()\n");
    }
}


int common_read_advance_step (ADIOS_FILE *fp, int last, float timeout_sec)
{
    struct common_read_internals_struct * internals;
    int retval;

    adios_errno = err_no_error;
    if (fp) {
        internals = (struct common_read_internals_struct *) fp->internal_data;
        retval = internals->read_hooks[internals->method].adios_advance_step_fn (fp, last, timeout_sec);
        if (!retval) {
            /* Update group information too */
            adios_read_hooks[internals->method].adios_get_groupinfo_fn (fp, &internals->ngroups,
                    &internals->group_namelist, &internals->nvars_per_group, &internals->nattrs_per_group);
            if (internals->group_in_view > -1) {
                /* if we have a group view, we need to update the presented list again */
                /* advance_step updated fp->nvars, nattrs, var_namelist, attr_namelist */
                int groupid = internals->group_in_view;
                internals->group_in_view = -1; // we have the full view at this moment
                common_read_group_view (fp, groupid);
            }
        }
    } else {
        adios_error ( err_invalid_file_pointer, "Invalid file pointer at adios_advance_step()\n");
        retval = err_invalid_file_pointer;
    }
    return retval;
}


void common_read_release_step (ADIOS_FILE *fp)
{
    struct common_read_internals_struct * internals;

    adios_errno = err_no_error;
    if (fp) {
        internals = (struct common_read_internals_struct *) fp->internal_data;
        internals->read_hooks[internals->method].adios_release_step_fn (fp);
    } else {
        adios_error ( err_invalid_file_pointer, "Invalid file pointer at adios_reset_dimension_order()\n");
    }
}


static int common_read_find_name (int n, char ** namelist, const char *name, int role)
{
    /** Find a string name in a list of names and return the index.
        Search should work with starting / characters and without.
        Create adios error and return -1 if name is null or
          if name is not found in the list.
        role = 0 for variable search, 1 for attribute search
     */
    int id, nstartpos=0, sstartpos;
    char ** s = namelist;
    char *rolename[2] = { "variable", "attribute" };
    enum ADIOS_ERRCODES roleerror[2] = { err_invalid_varname, err_invalid_attrname };

    if (!name) {
        adios_error (roleerror[role!=0], "Null pointer passed as %s name!\n", rolename[role!=0]);
        return -1;
    }

    // find names with or without beginning /
    if (*name == '/') nstartpos = 1;

    for (id=0; id < n; id++) {
        if (*s[0] == '/') sstartpos = 1;
        else sstartpos = 0;
        //DBG_PRINTF("     check %s, startpos=%d\n", *s, sstartpos);
        if (!strcmp (*s+sstartpos, name+nstartpos))
            break; // found this name
        s++;
    }

    if (id == n) {
        adios_error (roleerror[role!=0], "%s '%s' is not found! One "
                "possible error is to set the view to a specific group and "
                "then try to read a %s of another group. In this case, "
                "reset the group view with adios_group_view(fp,-1).\n",
                rolename[role!=0], name, rolename[role!=0]);
        return -1;
    }
    return id;
}


ADIOS_VARINFO * common_read_inq_var (const ADIOS_FILE *fp, const char * varname)
{
    struct common_read_internals_struct * internals;
    ADIOS_VARINFO * retval;

    adios_errno = err_no_error;
    if (fp) {
        internals = (struct common_read_internals_struct *) fp->internal_data;
        int varid = common_read_find_name (fp->nvars, fp->var_namelist, varname, 0);
        if (varid >= 0) {
            retval = common_read_inq_var_byid (fp, varid);
        } else {
            retval = NULL;
        }
    } else {
        adios_error (err_invalid_file_pointer, "Null pointer passed as file to adios_inq_var()\n");
        retval = NULL;
    }
    return retval;
}

// NCSU ALACRITY-ADIOS - For copying original metadata from transform
//   info to inq var info
static void patch_varinfo_with_transform_blockinfo(ADIOS_VARINFO *vi, ADIOS_TRANSINFO *ti) {
    common_read_free_blockinfo(&vi->blockinfo, vi->sum_nblocks);    // Free blockinfo in varinfo
    vi->blockinfo = ti->orig_blockinfo;                                // Move blockinfo from transinfo to varinfo
    ti->orig_blockinfo = 0;                                            // Delink blockinfo from transinfo
}
static void patch_varinfo_with_transinfo(ADIOS_VARINFO *vi, ADIOS_TRANSINFO *ti) {
    // First make room for the transform info fields
    free(vi->dims);

    // Now move them
    vi->type = ti->orig_type;
    vi->ndim = ti->orig_ndim;
    vi->global = ti->orig_global;
    vi->dims = ti->orig_dims;

    // Finally, delink them from the transform info so they aren't inadvertently free'd
    ti->orig_dims = 0;

    patch_varinfo_with_transform_blockinfo(vi, ti); // Also move blockinfo if extant
}

// NCSU ALACRITY-ADIOS - Delegate to the 'inq_var_raw_byid' function, then
//   patch the original metadata in from the transform info
ADIOS_VARINFO * common_read_inq_var_byid (const ADIOS_FILE *fp, int varid)
{
    ADIOS_VARINFO *vi;
    ADIOS_TRANSINFO *ti;

    vi = common_read_inq_var_raw_byid(fp, varid);
    if (vi == NULL)
        return NULL;

    // NCSU ALACRITY-ADIOS - translate between original and transformed metadata if necessary
    ti = common_read_inq_transinfo(fp, vi); // No orig_blockinfo
    if (ti && ti->transform_type != adios_transform_none) {
        patch_varinfo_with_transinfo(vi, ti);
    }
    common_read_free_transinfo(vi, ti);

    return vi;
}

// NCSU ALACRITY-ADIOS - Renaming of common_read_inq_var_byid, named 'raw'
//   because it is oblivious to the original metadata as stored in TRANSINFO
ADIOS_VARINFO * common_read_inq_var_raw_byid (const ADIOS_FILE *fp, int varid)
{
    struct common_read_internals_struct * internals;
    ADIOS_VARINFO * retval;

    adios_errno = err_no_error;
    if (fp) {
        if (varid >= 0 && varid < fp->nvars) {
            internals = (struct common_read_internals_struct *) fp->internal_data;
            /* Translate varid to varid in global varlist if a selected group is in view */
            retval = internals->read_hooks[internals->method].adios_inq_var_byid_fn
                                            (fp, varid+internals->group_varid_offset);
            if (retval) {
                /* Translate real varid to the group varid presented to the user */
                retval->varid = varid;
            }
        } else {
            adios_error (err_invalid_varid,
                         "Variable ID %d is not valid adios_inq_var_byid(). "
                         "Available 0..%d\n", varid, fp->nvars-1);
            retval = NULL;
        }
    } else {
        adios_error (err_invalid_file_pointer, "Null pointer passed as file to adios_inq_var_byid()\n");
        retval = NULL;
    }
    return retval;
}

// NCSU ALACRITY-ADIOS - common-layer inquiry function to read transform info.
// NOTE: does not follow the normal pattern of adding the info into
//   ADIOS_VARINFO because this information should not be sent to the user;
//   only in rare cases will a user application need this information (like a
//   query engine using an transform-embedded index), in which case that code
//   can dive deeper and access this function. Alternatively, if this use case
//   becomes more common, a simple 'transform raw' API could be added.
ADIOS_TRANSINFO * common_read_inq_transinfo(const ADIOS_FILE *fp, const ADIOS_VARINFO *vi) {
    if (!fp) {
        adios_error (err_invalid_file_pointer,
                     "Null ADIOS_FILE pointer passed to common_read_inq_transinfo()\n");
        return NULL;
    }
    if (!vi) {
        adios_error (err_invalid_argument,
                     "Null ADIOS_VARINFO pointer passed to common_read_inq_transinfo()\n");
        return NULL;
    }

    struct common_read_internals_struct * internals;
    internals = (struct common_read_internals_struct *) fp->internal_data;

    ADIOS_TRANSINFO *ti = internals->read_hooks[internals->method].adios_inq_var_transinfo_fn(fp, vi);
    return ti;
}

int common_read_inq_trans_blockinfo(const ADIOS_FILE *fp, const ADIOS_VARINFO *vi, ADIOS_TRANSINFO * ti) {
    if (!fp) {
        adios_error (err_invalid_argument,
                     "Null ADIOS_FILE pointer passed to common_read_inq_trans_blockinfo()\n");
        return 1;
    }
    if (!vi) {
        adios_error (err_invalid_argument,
                     "Null ADIOS_VARINFO pointer passed to common_read_inq_trans_blockinfo()\n");
        return 1;
    }
    if (!ti) {
        adios_error (err_invalid_argument,
                     "Null ADIOS_TRANSINFO pointer passed to common_read_inq_trans_blockinfo()\n");
        return 1;
    }

    struct common_read_internals_struct * internals;
    internals = (struct common_read_internals_struct *) fp->internal_data;
    return internals->read_hooks[internals->method].adios_inq_var_trans_blockinfo_fn(fp, vi, ti);
}



int common_read_inq_var_stat (const ADIOS_FILE *fp, ADIOS_VARINFO * varinfo,
                             int per_step_stat, int per_block_stat)
{
    struct common_read_internals_struct * internals;
    int retval;
    int group_varid;

    adios_errno = err_no_error;
    if (fp) {
        internals = (struct common_read_internals_struct *) fp->internal_data;
        if (varinfo) {
            /* Translate group varid presented to the user to the real varid */
            group_varid = varinfo->varid;
            varinfo->varid = varinfo->varid + internals->group_varid_offset;
        }
        retval = internals->read_hooks[internals->method].adios_inq_var_stat_fn (fp, varinfo, per_step_stat, per_block_stat);
        /* Translate back real varid to the group varid presented to the user */
        varinfo->varid = group_varid;
    } else {
        adios_error (err_invalid_file_pointer, "Null pointer passed as file to adios_inq_var_stat()\n");
        retval = err_invalid_file_pointer;
    }
    return retval;
}

// NCSU ALACRITY-ADIOS - Delegate to the 'inq_var_blockinfo_raw' function, then
//   patch the original metadata in from the transform info
int common_read_inq_var_blockinfo (const ADIOS_FILE *fp, ADIOS_VARINFO * varinfo)
{
    ADIOS_TRANSINFO *ti;

    int retval = common_read_inq_var_blockinfo_raw(fp, varinfo);
    if (retval != err_no_error)
        return retval;

    // NCSU ALACRITY-ADIOS - translate between original and transformed metadata if necessary
    ti = common_read_inq_transinfo(fp, varinfo);
    if (ti && ti->transform_type != adios_transform_none) {
        retval = common_read_inq_trans_blockinfo(fp, varinfo, ti);
        if (retval != err_no_error)
            return retval;

        patch_varinfo_with_transform_blockinfo(varinfo, ti);
    }
    common_read_free_transinfo(varinfo, ti);

    return err_no_error;
}

// NCSU ALACRITY-ADIOS - Renaming of common_read_inq_var_blockinfo, named 'raw'
//   because it is oblivious to the original metadata as stored in TRANSINFO
int common_read_inq_var_blockinfo_raw (const ADIOS_FILE *fp, ADIOS_VARINFO * varinfo)
{
    struct common_read_internals_struct * internals;
    int retval;
    int group_varid;

    adios_errno = err_no_error;
    if (fp) {
        internals = (struct common_read_internals_struct *) fp->internal_data;
        if (varinfo) {
            /* Translate group varid presented to the user to the real varid */
            group_varid = varinfo->varid;
            varinfo->varid = varinfo->varid + internals->group_varid_offset;
        }
        retval = internals->read_hooks[internals->method].adios_inq_var_blockinfo_fn (fp, varinfo);
        /* Translate back real varid to the group varid presented to the user */
        varinfo->varid = group_varid;
    } else {
        adios_error (err_invalid_file_pointer, "Null pointer passed as file to adios_inq_var_blockinfo()\n");
        retval = err_invalid_file_pointer;
    }
    return retval;
}

#define MYFREE(p) {free(p); (p)=NULL;}
// NCSU ALACRITY-ADIOS - Factored this out to use elsewhere
static void common_read_free_blockinfo(ADIOS_VARBLOCK **varblock, int sum_nblocks) {
    if (*varblock) {
    int i;
        ADIOS_VARBLOCK *bp = *varblock;
        for (i = 0; i < sum_nblocks; i++) {
                if (bp->start) MYFREE (bp->start);
                if (bp->count) MYFREE (bp->count);
                bp++;
            }
        MYFREE(*varblock);
        }
}

void common_read_free_varinfo (ADIOS_VARINFO *vp)
{
    int i;
    if (vp) {
        common_read_free_blockinfo(&vp->blockinfo, vp->sum_nblocks);

        if (vp->statistics) {
            ADIOS_VARSTAT *sp = vp->statistics;
            if (sp->min && sp->min != vp->value)   MYFREE(sp->min);
            if (sp->max && sp->max != vp->value)   MYFREE(sp->max);
            if (sp->avg && sp->avg != vp->value)   MYFREE(sp->avg);
            if (sp->std_dev)                       MYFREE(sp->std_dev);

            if (sp->steps) {
                if (sp->steps->mins)        MYFREE(sp->steps->mins);
                if (sp->steps->maxs)        MYFREE(sp->steps->maxs);
                if (sp->steps->avgs)        MYFREE(sp->steps->avgs);
                if (sp->steps->std_devs)    MYFREE(sp->steps->std_devs);
                MYFREE(sp->steps);
            }

            if (sp->blocks) {
                if (sp->blocks->mins)        MYFREE(sp->blocks->mins);
                if (sp->blocks->maxs)        MYFREE(sp->blocks->maxs);
                if (sp->blocks->avgs)        MYFREE(sp->blocks->avgs);
                if (sp->blocks->std_devs)    MYFREE(sp->blocks->std_devs);
                MYFREE(sp->blocks);
            }

            if (sp->histogram) {
                if (sp->histogram->breaks)        MYFREE(sp->histogram->breaks);
                if (sp->histogram->frequencies)   MYFREE(sp->histogram->frequencies);
                if (sp->histogram->gfrequencies)  MYFREE(sp->histogram->gfrequencies);
                MYFREE(sp->histogram);
            }

            MYFREE(vp->statistics);
        }

        if (vp->dims)    MYFREE(vp->dims);
        if (vp->value)   MYFREE(vp->value);
        if (vp->nblocks) MYFREE(vp->nblocks);

        free(vp);
    }
}

// NCSU ALACRITY-ADIOS - Free transform info
void common_read_free_transinfo(const ADIOS_VARINFO *vi, ADIOS_TRANSINFO *ti) {
    if (ti) {
        if (ti->orig_dims) MYFREE(ti->orig_dims);
        if (ti->transform_metadata && ti->should_free_transform_metadata)
            MYFREE(ti->transform_metadata);

        if (ti->transform_metadatas) {
        	if (ti->should_free_transform_metadata) {
                int i;
        		for (i = 0; i < vi->sum_nblocks; i++)
                	MYFREE(ti->transform_metadatas[i].content);
        	}
            MYFREE(ti->transform_metadatas);
        }

        common_read_free_blockinfo(&ti->orig_blockinfo, vi->sum_nblocks);

        free(ti);
    }
}
#undef MYFREE

int common_read_schedule_read (const ADIOS_FILE      * fp,
                               const ADIOS_SELECTION * sel,
                               const char            * varname,
                               int                     from_steps,
                               int                     nsteps,
                               const char            * param, // NCSU ALACRITY-ADIOS
                               void                  * data)

{
    struct common_read_internals_struct * internals;
    int retval;

    adios_errno = err_no_error;
    if (fp) {
        internals = (struct common_read_internals_struct *) fp->internal_data;
        int varid = common_read_find_name (fp->nvars, fp->var_namelist, varname, 0);
        if (varid >= 0) {
            retval = common_read_schedule_read_byid (fp, sel, varid, from_steps, nsteps, param /* NCSU ALACRITY-ADIOS */, data);
        } else {
            retval = adios_errno; // adios_errno was set in common_read_find_name
        }
    } else {
        adios_error (err_invalid_file_pointer, "Null pointer passed as file to adios_schedule_read()\n");
        retval = err_invalid_file_pointer;
    }
    return retval;
}

// NCSU ALACRITY-ADIOS - Modified to delegate to transform method when called
//   on a transformed variable
int common_read_schedule_read_byid (const ADIOS_FILE      * fp,
        const ADIOS_SELECTION * sel,
        int                     varid,
        int                     from_steps,
        int                     nsteps,
        const char            * param, // NCSU ALACRITY-ADIOS
        void                  * data)

{
#if defined(WITH_NCSU_TIMER) && defined(TIMER_LEVEL) && (TIMER_LEVEL <= 2)
    timer_start ("adios_schedule_read");
#endif
    struct common_read_internals_struct * internals;
    int retval;

    internals = (struct common_read_internals_struct *) fp->internal_data;

    adios_errno = err_no_error;
    if (fp) {
        if (varid >=0 && varid < fp->nvars) {
            // NCSU ALACRITY-ADIOS - If the variable is transformed, intercept
            //   the read scheduling and schedule our own reads
            ADIOS_VARINFO *raw_varinfo = adios_transforms_infocache_inq_varinfo(fp, internals->infocache, varid); //common_read_inq_var_raw_byid(fp, varid);        // Get the *raw* varinfo
            ADIOS_TRANSINFO *transinfo = adios_transforms_infocache_inq_transinfo(fp, internals->infocache, varid); //common_read_inq_transinfo(fp, raw_varinfo);    // Get the transform info (i.e. original var info)
            assert(raw_varinfo && transinfo);

            // If this variable is transformed, delegate to the transform
            // method to generate subrequests
            // Else, do the normal thing
            if (transinfo && transinfo->transform_type != adios_transform_none) {
                adios_transform_raw_read_request *subreq;
                adios_transform_pg_read_request *pg_reqgroup;
                adios_transform_read_request *new_reqgroup;


#if defined(WITH_NCSU_TIMER) && defined(TIMER_LEVEL) && (TIMER_LEVEL <= 2)
    timer_start ("adios_transform_generate_read_requests");
#endif
                // Generate the read request group and append it to the list
                new_reqgroup = adios_transform_generate_read_reqgroup(raw_varinfo, transinfo, fp, sel, from_steps, nsteps, param, data);
#if defined(WITH_NCSU_TIMER) && defined(TIMER_LEVEL) && (TIMER_LEVEL <= 2)
    timer_stop ("adios_transform_generate_read_requests");
#endif

                // Proceed to register the read request and schedule all of its grandchild raw
                // read requests ONLY IF a non-NULL reqgroup was returned (i.e., the user's
                // selection intersected at least one PG).
#if defined(WITH_NCSU_TIMER) && defined(TIMER_LEVEL) && (TIMER_LEVEL <= 2)
    timer_start ("adios_transform_submit_read_requests");
#endif
                if (new_reqgroup) {
                    adios_transform_read_request_append(&internals->transform_reqgroups, new_reqgroup);

                    // Now schedule all of the new subrequests
                    retval = 0;
                    for (pg_reqgroup = new_reqgroup->pg_reqgroups; pg_reqgroup; pg_reqgroup = pg_reqgroup->next) {
                        for (subreq = pg_reqgroup->subreqs; subreq; subreq = subreq->next) {
                            retval |= internals->read_hooks[internals->method].adios_schedule_read_byid_fn(
                                            fp, subreq->raw_sel, varid+internals->group_varid_offset, pg_reqgroup->timestep, 1, subreq->data);
                        }
                    }
                }
#if defined(WITH_NCSU_TIMER) && defined(TIMER_LEVEL) && (TIMER_LEVEL <= 2)
    timer_stop ("adios_transform_submit_read_requests");
#endif
            } else {
                // Old functionality
                retval = internals->read_hooks[internals->method].adios_schedule_read_byid_fn (fp, sel, varid+internals->group_varid_offset, from_steps, nsteps, data);
            }
        } else {
            adios_error (err_invalid_varid,
                         "Variable ID %d is not valid in adios_schedule_read_byid(). "
                         "Available 0..%d\n", varid, fp->nvars-1);
            retval = err_invalid_varid;
        }
    } else {
        adios_error (err_invalid_file_pointer, "Null pointer passed as file to adios_schedule_read_byid()\n");
        retval = err_invalid_file_pointer;
    }

#if defined(WITH_NCSU_TIMER) && defined(TIMER_LEVEL) && (TIMER_LEVEL <= 2)
    timer_stop ("adios_schedule_read");
#endif
    return retval;
}

// NCSU ALACRITY-ADIOS - Modified to delegate to transform method to combine
//  read subrequests to answer original requests
int common_read_perform_reads (const ADIOS_FILE *fp, int blocking)
{
#if defined(WITH_NCSU_TIMER) && defined(TIMER_LEVEL) && (TIMER_LEVEL <= 2)
    timer_start ("adios_perform_reads");
#endif
    struct common_read_internals_struct * internals;
    int retval;

    adios_errno = err_no_error;
    if (fp) {
        internals = (struct common_read_internals_struct *) fp->internal_data;
        retval = internals->read_hooks[internals->method].adios_perform_reads_fn (fp, blocking);

        // NCSU ALACRITY-ADIOS - If this was a blocking call, consider all read
        //   request groups completed, and reassemble via the transform method.
        //   Otherwise, do nothing.
        if (blocking) {
#if defined(WITH_NCSU_TIMER) && defined(TIMER_LEVEL) && (TIMER_LEVEL <= 2)
    timer_start ("adios_perform_reads_transform");
#endif
            adios_transform_process_all_reads(&internals->transform_reqgroups);
#if defined(WITH_NCSU_TIMER) && defined(TIMER_LEVEL) && (TIMER_LEVEL <= 2)
    timer_stop ("adios_perform_reads_transform");
#endif
        } else {
            // Do nothing; reads will be performed by check_reads
        }
    } else {
        adios_error (err_invalid_file_pointer, "Null pointer passed as file to adios_perform_reads()\n");
        retval = err_invalid_file_pointer;
    }
#if defined(WITH_NCSU_TIMER) && defined(TIMER_LEVEL) && (TIMER_LEVEL <= 2)
    timer_stop ("adios_perform_reads");
#endif
    return retval;
}

int common_read_check_reads (const ADIOS_FILE * fp, ADIOS_VARCHUNK ** chunk)
{
    struct common_read_internals_struct * internals;
    int retval;

    adios_errno = err_no_error;
    if (fp) {
        internals = (struct common_read_internals_struct *) fp->internal_data;

        // NCSU ALACRITY-ADIOS - Handle those VARCHUNKs that correspond to
        //   subrequests; don't return until we get a completed one
        do {
            // Read some chunk
        retval = internals->read_hooks[internals->method].adios_check_reads_fn (fp, chunk);
            if (!*chunk) break; // If no more chunks are available, stop now

            // Process the chunk through a transform method, if necessary
            adios_transform_process_read_chunk(internals->transform_reqgroups, chunk);
        } while (!*chunk); // Keep reading until we have a chunk to return
    } else {
        adios_error (err_invalid_file_pointer, "Null pointer passed as file to adios_check_reads()\n");
        retval = err_invalid_file_pointer;
    }
    return retval;
}


void common_read_free_chunk (ADIOS_VARCHUNK *chunk)
{
    /** Free the memory of a chunk allocated inside adios_check_reads().
     * It only frees the ADIOS_VARCHUNK struct and the ADIOS_SELECTION struct
     * pointed by the chunk. The data pointer should never be freed since
     * that memory belongs to the reading method.
     */
     if (chunk) {
        if (chunk->sel) {
            free(chunk->sel);
            chunk->sel = NULL;
        }
        free(chunk);
     }
}


int common_read_get_attr (const ADIOS_FILE * fp,
                          const char * attrname,
                          enum ADIOS_DATATYPES * type,
                          int * size,
                          void ** data)
{
    struct common_read_internals_struct * internals;
    int retval;

    adios_errno = err_no_error;
    if (fp) {
        internals = (struct common_read_internals_struct *) fp->internal_data;
        int attrid = common_read_find_name (fp->nattrs, fp->attr_namelist, attrname, 1);
        if (attrid > -1) {
            retval = common_read_get_attr_byid (fp, attrid, type, size, data);
        } else {
            retval = adios_errno; // adios_errno was set in common_read_find_name
        }
    } else {
        adios_error (err_invalid_file_pointer, "Null pointer passed as file to adios_read_get_attr()\n");
        retval = err_invalid_file_pointer;
    }
    return retval;
}


int common_read_get_attr_byid (const ADIOS_FILE * fp,
                               int attrid,
                               enum ADIOS_DATATYPES * type,
                               int * size,
                               void ** data)
{
    struct common_read_internals_struct * internals;
    int retval;

    adios_errno = err_no_error;
    if (fp) {
        if (attrid >= 0 && attrid < fp->nattrs) {
            internals = (struct common_read_internals_struct *) fp->internal_data;
            retval = internals->read_hooks[internals->method].adios_get_attr_byid_fn (fp, attrid+internals->group_attrid_offset, type, size, data);
        } else {
            adios_error (err_invalid_attrid,
                         "Attribute ID %d is not valid in adios_get_attr_byid(). "
                         "Available 0..%d\n", attrid, fp->nattrs-1);
            retval = err_invalid_attrid;
        }
    } else {
        adios_error (err_invalid_file_pointer, "Null pointer passed as file to adios_read_get_attr_byid()\n");
        retval = err_invalid_file_pointer;
    }
    return retval;
}


const char * common_read_type_to_string (enum ADIOS_DATATYPES type)
{
    switch (type)
    {
        case adios_unsigned_byte:    return "unsigned byte";
        case adios_unsigned_short:   return "unsigned short";
        case adios_unsigned_integer: return "unsigned integer";
        case adios_unsigned_long:    return "unsigned long long";

        case adios_byte:             return "byte";
        case adios_short:            return "short";
        case adios_integer:          return "integer";
        case adios_long:             return "long long";

        case adios_real:             return "real";
        case adios_double:           return "double";
        case adios_long_double:      return "long double";

        case adios_string:           return "string";
        case adios_complex:          return "complex";
        case adios_double_complex:   return "double complex";

        default:
        {
            static char buf [50];
            sprintf (buf, "(unknown: %d)", type);
            return buf;
        }
    }
}


int common_read_type_size(enum ADIOS_DATATYPES type, void *data)
{
    return bp_get_type_size(type, data);
}


int common_read_get_grouplist (const ADIOS_FILE  *fp, char ***group_namelist)
{
    struct common_read_internals_struct * internals;
    int retval;

    adios_errno = err_no_error;
    if (fp) {
        internals = (struct common_read_internals_struct *) fp->internal_data;
        retval = internals->ngroups;
        *group_namelist = internals->group_namelist;
    } else {
        adios_error (err_invalid_file_pointer, "Null pointer passed as file to adios_get_grouplist()\n");
        retval = err_invalid_file_pointer;
    }
    return retval;
}

/** Select a subset of variables and attributes to present in ADIOS_FILE struct.
    ADIOS_FILE-> nvars, nattrs, var_namelist, attr_namelist will contain
    only a subset of all variables and attributes.
    internals-> full_* stores the complete lists for reset or change of group
 */
int common_read_group_view (ADIOS_FILE  *fp, int groupid)
{
    struct common_read_internals_struct * internals;
    int retval, i;

    adios_errno = err_no_error;
    if (fp) {
        internals = (struct common_read_internals_struct *) fp->internal_data;
        if (groupid >= 0 && groupid < internals->ngroups) {
            /* 1. save complete list if first done */
            if (internals->group_in_view == -1) {
                internals->full_nvars = fp->nvars;
                internals->full_varnamelist = fp->var_namelist;
                internals->full_nattrs = fp->nattrs;
                internals->full_attrnamelist = fp->attr_namelist;
            }
            /* Set ID offsets for easier indexing of vars/attrs in other functions */
            internals->group_varid_offset = 0;
            internals->group_attrid_offset = 0;
            for (i=0; i<groupid; i++) {
                internals->group_varid_offset += internals->nvars_per_group[i];
                internals->group_attrid_offset += internals->nattrs_per_group[i];
            }
            /* Set view to this group */
            fp->nvars = internals->nvars_per_group[groupid];
            fp->var_namelist = &(internals->full_varnamelist [internals->group_varid_offset]);
            fp->nattrs = internals->nattrs_per_group[groupid];
            fp->attr_namelist = &(internals->full_attrnamelist [internals->group_attrid_offset]);
            internals->group_in_view = groupid;
            retval = 0;

        } else if (groupid == -1) {
            /* Reset to full view */
            fp->nvars  = internals->full_nvars;
            fp->var_namelist  = internals->full_varnamelist;
            fp->nattrs = internals->full_nattrs;
            fp->attr_namelist  = internals->full_attrnamelist;
            internals->group_varid_offset = 0;
            internals->group_attrid_offset = 0;
            internals->group_in_view = -1;
            retval = 0;
        } else {
            adios_error (err_invalid_group, "Invalid group ID in adios_group_view()\n");
            retval = err_invalid_group;
        }
    } else {
        adios_error (err_invalid_file_pointer, "Null pointer passed as file to adios_group_view()\n");
        retval = err_invalid_file_pointer;
    }
    return retval;
}

/* internal function to support version 1 time-dimension reads
   called from adios_read_v1.c and adiosf_read_v1.c
*/
int common_read_is_var_timed (const ADIOS_FILE *fp, int varid)
{
    struct common_read_internals_struct * internals;
    int retval;

    adios_errno = err_no_error;
    if (fp) {
        internals = (struct common_read_internals_struct *) fp->internal_data;
        retval = internals->read_hooks[internals->method].adios_is_var_timed_fn (fp, varid+internals->group_varid_offset);
    } else {
        adios_error (err_invalid_file_pointer, "Null pointer passed as file to common_read_is_var_timed()\n");
        retval = err_invalid_file_pointer;
    }
    return retval;
}

void common_read_print_fileinfo (const ADIOS_FILE *fp)
{
    int i;
    int ngroups;
    char **group_namelist;
    ngroups = common_read_get_grouplist (fp, &group_namelist);

    printf ("---------------------------\n");
    printf ("     file information\n");
    printf ("---------------------------\n");
    printf ("  # of groups:     %d\n"
            "  # of variables:  %d\n"
            "  # of attributes: %d\n"
            "  current step:    %d\n"
            "  last step:       %d\n",
            ngroups,
            fp->nvars,
            fp->nattrs,
            fp->current_step,
            fp->last_step);
    printf ("---------------------------\n");
    printf ("     var information\n");
    printf ("---------------------------\n");
    printf ("    var id\tname\n");
    if (fp->var_namelist) {
        for (i=0; i<fp->nvars; i++)
            printf("\t%d)\t%s\n", i, fp->var_namelist[i]);
    }
    printf ("---------------------------\n");
    printf ("     attribute information\n");
    printf ("---------------------------\n");
    printf ("    attr id\tname\n");
    if (fp->attr_namelist) {
        for (i=0; i<fp->nattrs; i++)
            printf("\t%d)\t%s\n", i, fp->attr_namelist[i]);
    }
    printf ("---------------------------\n");
    printf ("     group information\n");
    printf ("---------------------------\n");
    if (group_namelist) {
        for (i=0; i<ngroups; i++)
            printf("\t%d)\t%s\n", i, group_namelist[i]);
    }


    return;
}


/**    SELECTIONS   **/
ADIOS_SELECTION * common_read_selection_boundingbox (int ndim, const uint64_t *start, const uint64_t *count)
{
    adios_errno = err_no_error;
    ADIOS_SELECTION * sel = (ADIOS_SELECTION *) malloc (sizeof(ADIOS_SELECTION));
    if (sel) {
        sel->type = ADIOS_SELECTION_BOUNDINGBOX;
        sel->u.bb.ndim = ndim;
        sel->u.bb.start = (uint64_t *)start;
        sel->u.bb.count = (uint64_t *)count;
    } else {
        adios_error(err_no_memory, "Cannot allocate memory for bounding box selection\n");
    }
    return sel;
}


ADIOS_SELECTION * common_read_selection_points (int ndim, uint64_t npoints, const uint64_t *points)
{
    adios_errno = err_no_error;
    ADIOS_SELECTION * sel = (ADIOS_SELECTION *) malloc (sizeof(ADIOS_SELECTION));
    if (sel) {
        sel->type = ADIOS_SELECTION_POINTS;
        sel->u.points.ndim = ndim;
        sel->u.points.npoints = npoints;
        sel->u.points.points = (uint64_t *) points;
    } else {
        adios_error(err_no_memory, "Cannot allocate memory for points selection\n");
    }
    return sel;
}

ADIOS_SELECTION * common_read_selection_writeblock (int index)
{
    adios_errno = err_no_error;
    ADIOS_SELECTION * sel = (ADIOS_SELECTION *) malloc (sizeof(ADIOS_SELECTION));
    if (sel) {
        sel->type = ADIOS_SELECTION_WRITEBLOCK;
        sel->u.block.index = index;
        // NCSU ALACRITY-ADIOS: Set the writeblock selection to be a full-PG selection by default
        sel->u.block.is_absolute_index = 0;
        sel->u.block.is_sub_pg_selection = 0;
    } else {
        adios_error(err_no_memory, "Cannot allocate memory for writeblock selection\n");
    }
    return sel;
}

ADIOS_SELECTION * common_read_selection_auto (char *hints)
{
    adios_errno = err_no_error;
    ADIOS_SELECTION * sel = (ADIOS_SELECTION *) malloc (sizeof(ADIOS_SELECTION));
    if (sel) {
        sel->type = ADIOS_SELECTION_AUTO;
        sel->u.autosel.hints = hints;
    } else {
        adios_error(err_no_memory, "Cannot allocate memory for auto selection\n");
    }
    return sel;
}

void common_read_selection_delete (ADIOS_SELECTION *sel)
{
    free(sel);
}
