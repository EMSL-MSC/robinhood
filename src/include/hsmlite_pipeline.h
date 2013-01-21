/* -*- mode: c; c-basic-offset: 4; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=4:tabstop=4:
 */
/*
 * Copyright (C) 2008, 2009 CEA/DAM
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the CeCILL License.
 *
 * The fact that you are presently reading this means that you have had
 * knowledge of the CeCILL license (http://www.cecill.info) and that you
 * accept its terms.
 */

/**
 * \file  lustre_hsm_pipeline.h
 * \brief This file describes EntryProcessor pipeline for Lustre-HSM PolicyEngine.
 */

#ifndef _PIPELINE_DEF_H
#define _PIPELINE_DEF_H

typedef struct changelog_record
{
#ifdef HAVE_CHANGELOG_EXTEND_REC
    struct changelog_ext_rec * p_log_rec;
#else
    struct changelog_rec * p_log_rec;
#endif
    char          *mdt;
} changelog_record_t;

/** purpose specific information attached to a pipeline operation */
typedef struct op_extra_info_t
{
    /** changelog record info */
    changelog_record_t log_record;

    /** is this entry from changelog ?*/
    int            is_changelog_record:1;

    /** indicates what extra info is to be retrieved for this entry */
    int            getstripe_needed:1;
    int            getattr_needed:1;
    int            getpath_needed:1;
    int            getstatus_needed:1;
    int            not_supp:1; /* unsupported type for migration */
} op_extra_info_t;

static void inline extra_info_init( op_extra_info_t * p_extra_info )
{
   memset( &p_extra_info->log_record, 0, sizeof(changelog_record_t) );
   p_extra_info->is_changelog_record = FALSE;
   p_extra_info->getstripe_needed = FALSE; 
   p_extra_info->getattr_needed = FALSE;
   p_extra_info->getpath_needed = FALSE;
   p_extra_info->getstatus_needed = FALSE;
   p_extra_info->not_supp = FALSE;
}

#define POSIX_ATTR_MASK (ATTR_MASK_size | ATTR_MASK_blocks | ATTR_MASK_owner \
                         | ATTR_MASK_gr_name | ATTR_MASK_last_access \
                         | ATTR_MASK_last_mod | ATTR_MASK_type)


static void inline mask2needed_op( unsigned int attr_mask, op_extra_info_t * p_extra_info )
{
   if ( attr_mask & (ATTR_MASK_stripe_info | ATTR_MASK_stripe_items ) )
         p_extra_info->getstripe_needed = TRUE;

   if ( attr_mask & (ATTR_MASK_fullpath | ATTR_MASK_name | ATTR_MASK_depth ) )
         p_extra_info->getpath_needed = TRUE;

   if ( attr_mask & POSIX_ATTR_MASK )
        p_extra_info->getattr_needed = TRUE;
}

/* pipeline stages */
#define STAGE_GET_FID       0
#define STAGE_GET_INFO_DB   1
#define STAGE_GET_INFO_FS   2
#define STAGE_REPORTING     3
#define STAGE_DB_APPLY      4
#define STAGE_CHGLOG_CLR    5
#define STAGE_RM_OLD_ENTRIES  6 /* special stage at the end of FS scan */

#define PIPELINE_STAGE_COUNT (STAGE_RM_OLD_ENTRIES+1)

/** Lustre-HSM pipeline definition */
extern pipeline_stage_t entry_proc_pipeline[PIPELINE_STAGE_COUNT];


#endif
