/* -*- mode: c; c-basic-offset: 4; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=4:tabstop=4:
 */
/*
 * Copyright (C) 2009, 2010 CEA/DAM
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the CeCILL License.
 *
 * The fact that you are presently reading this means that you have had
 * knowledge of the CeCILL license (http://www.cecill.info) and that you
 * accept its terms.
 */

/**
 * Command for recovering filesystem content after a disaster (backup flavor)
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "list_mgr.h"
#include "RobinhoodConfig.h"
#include "RobinhoodLogs.h"
#include "RobinhoodMisc.h"
#include "Memory.h"
#include "xplatform_print.h"

#define SRUB_TAG "Scrubber"



/* initially empty array */
static entry_id_t  * dir_array = NULL;
static unsigned int array_len = 0;

/* first/last+1 set entry ids in array */
static unsigned int array_first = 0;
static unsigned int array_next = 0;
#define array_used ((int)array_next-(int)array_first)

#define LS_CHUNK    50

static size_t what_2_power(size_t s)
{
    size_t c = 1;
    while (c < s)
        c <<= 1;
    return c;
}

/** add a list of ids to the scrubbing array */
static int add_id_list(entry_id_t  * list, unsigned int count)
{
    /* always add at the beginning to have LIFO behavior */

    /* is there enough room before the first item ? */
    if (count <= array_first)
    {
        /* copy it just before 'first' (entries must be consecutive) */
        memcpy(&dir_array[array_first-count], list, count * sizeof(entry_id_t));
        array_first -= count;

#ifdef _DEBUG_ID_LIST
        printf("1)...<new_ids:%u-%u><ids:%u-%u>...(len=%Lu)\n", array_first,
                array_first+count-1, array_first+count, array_next-1, array_len);
#endif
    }
    /* is the array empty ?*/
    else if ((array_used == 0) && (count <= array_len))
    {
        /* copy from the begginning */
        memcpy(dir_array, list, count * sizeof(entry_id_t));
        array_first = 0;
        array_next = count;

#ifdef _DEBUG_ID_LIST
        printf("2) <new_ids:%u-%u>...(len=%Lu)\n", array_first, array_next - 1,
               array_len);
#endif
    }
    else /* increase array size */
    {
        entry_id_t  * dir_array_new;
        size_t new_len = what_2_power(array_len + count);
        dir_array_new = MemAlloc(new_len * sizeof(entry_id_t));
        if (!dir_array_new)
            return -ENOMEM;
        /* first copy new ids */
        memcpy(dir_array_new, list, count * sizeof(entry_id_t));
        if (dir_array && (array_used > 0))
        {
            /* then copy current ids */
            memcpy(&dir_array_new[count+1], &dir_array[array_first],
                   array_used * sizeof(entry_id_t));

#ifdef _DEBUG_ID_LIST
            printf("3) <new_ids:%u-%u><ids:%u-%u>...(len=%Lu)\n", 0, count - 1,
                   count+1, array_next-1, new_len);
#endif
        }
#ifdef _DEBUG_ID_LIST
        else
            printf("4) <new_ids:%u-%u>...(len=%Lu)\n", 0, count - 1,
                   new_len);
#endif

        /* free old array */
        if (dir_array)
            MemFree(dir_array);

        /* update array info */
        dir_array = dir_array_new;
        array_next = array_used + count;
        array_first = 0;
        array_len = new_len;
    }
    return 0;
}

/** release a list of ids from the array */
static inline void rbh_scrub_release_list(unsigned int first, unsigned int count)
{
    if (first != array_first)
        DisplayLog(LVL_CRIT, SCRUB_TAG, "IMPLEMENTATION ISSUE: array_first was %u, is now %u\n",
                   first, array_first);
    array_first += count;

#ifdef _DEBUG_ID_LIST
    printf("released %u-%u\n", array_first - count, array_first - 1);
#endif
}


/** The caller's function to be called for scanned entries */
typedef int    ( *scrub_callback_t ) ( entry_id_t * id_list,
                                       attr_set_t * attr_list,
                                       unsigned int entry_count );


/** scan sets of directories
 * \param cb_func, callback function for each set of directory
 */
int rbh_scrub(list_mgr_t   * p_mgr, entry_id_t * id_list,
              unsigned int id_count,
              lmgr_filter_t * entry_filter, int attr_mask,
              scrub_callback_t cb_func)
{
    entry_id_t  * curr_array;
    unsigned int count;
    lmgr_filter_t  filter;
    filter_value_t fv;
    int i, rc;
    int last_err = 0;

    rc = add_id_list(id_list, id_count);
    if (rc)
        return rc;

    /* only get subdirs (for scanning) */
    fv.val_str = STR_TYPE_DIR;
    lmgr_simple_filter_init( &filter );
    lmgr_simple_filter_add( &filter, ATTR_INDEX_type, EQUAL, fv, 0 );

    /* while the array is not empty */
    while (array_used > 0)
    {
        unsigned int res_count = 0;
        entry_id_t * child_ids;
        attr_set_t * child_attrs;

        /* get a set of entry_ids */
        if (array_used < LS_CHUNK)
        {
            /* get all available dirs */
            curr_array = &dir_array[array_first];
            count = array_used;
        }
        else
        {
            /* get a constant chunk */
            curr_array = &dir_array[array_first];
            count = LS_CHUNK;
        }

#ifdef _DEBUG_ID_LIST
        printf("processing %u-%u\n", array_first, array_first+count-1);
#endif

        /* read childs */
        res_count = 0;
        child_ids = NULL;
        child_attrs = NULL;
        rc = ListMgr_GetChild(p_mgr, &filter, curr_array, count, 0,
                              &child_ids, &child_attrs, &res_count);

        if (rc)
        {
            DisplayLog(LVL_CRIT, SCRUB_TAG, "ListMgr_GetChild() terminated with error %d", rc);
            /* @TODO free allocated resources */
            break;
        }

        /* Call the callback func for each listed dir */
        rc = cb_func(child_ids, child_attrs, res_count);
        if (rc)
            /* XXX break the scan? */
            last_err = rc;

        /* attributes no more needed */
        /* release attrs */
        if (child_attrs)
        {
            for (i = 0; i < res_count; i++)
                ListMgr_FreeAttrs(&child_attrs[i]);
            MemFree(child_attrs);
            child_attrs = NULL;
        }
        /* can release the list of input ids */
        rbh_scrub_release_list(array_first, count);

        /* copy entry ids before freeing them */
        add_id_list(child_ids, res_count);

        /* free the returned id array */
        if (child_ids)
        {
            MemFree(child_ids);
            child_ids = NULL;
        }
    }

    return last_err;
}
