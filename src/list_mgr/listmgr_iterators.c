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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "list_mgr.h"
#include "listmgr_common.h"
#include "listmgr_stripe.h"
#include "listmgr_internal.h"
#include "database.h"
#include "Memory.h"
#include "RobinhoodLogs.h"
#include <stdio.h>
#include <stdlib.h>


/* append a directory condition (sort or filter on dirattr) to an iterator request */
void append_dir_req(char * outstr, const char * req_start, const char * table_filter,
                    const char * sort_dirattr_str, filter_dir_e filter_dir,
                    const char * filter_dir_str, unsigned int filter_dir_index)
{
    char * currstr = outstr;

    if (sort_dirattr_str == NULL)
    {
        switch (filter_dir)
        {
            case FILTERDIR_NONE:
                /* no dirattr involved */
                strcpy(outstr, req_start); /* no "where" needed */
                currstr += strlen(outstr);
                if (table_filter)
                    sprintf( currstr, " WHERE %s", table_filter );
                break;
            case FILTERDIR_EMPTY:
                /* only empty dir filter is to be appended */
                currstr += sprintf( outstr, "%s WHERE %s", req_start, filter_dir_str);
                if (table_filter)
                    sprintf( currstr, " AND %s", table_filter );
                break;
            case FILTERDIR_OTHER:
                /* must join on dirattr_filter */
                currstr += sprintf( outstr, "%s LEFT JOIN (SELECT parent_id, %s as dirattr from ENTRIES "
                             "GROUP BY parent_id) as da ON id=da.parent_id "
                             "WHERE %s", req_start, dirattr2str(filter_dir_index),
                             filter_dir_str );
                if (table_filter)
                    sprintf( currstr, " AND %s", table_filter );
                break;
            default:
                DisplayLog( LVL_CRIT, LISTMGR_TAG, "Unexpected filter on directory attribute in %s(): %#x",
                            __func__, filter_dir );
                /* ignore dir filter */
                append_dir_req( outstr, req_start, table_filter, sort_dirattr_str,
                                FILTERDIR_NONE, NULL, 0);
                break;
        }
    }
    else /* sorting on dirattr */
    {
        /* sort on 1 dirattr, possibly needs a second for filter */
        switch (filter_dir)
        {
            case FILTERDIR_NONE:
                /* left join with dirattr_sort */
                /* @TODO directly perform request on parent_id if no table_filter? */
                currstr += sprintf( outstr, "%s LEFT JOIN (SELECT parent_id, %s as dirattr_sort "
                                    "FROM ENTRIES GROUP BY parent_id) as da ON id=da.parent_id",
                                    req_start, sort_dirattr_str );
                if (table_filter)
                    sprintf( currstr, " WHERE %s", table_filter );
                break;
            case FILTERDIR_EMPTY:
                /* left join with dirattr_sort + add empty dir filter */
                currstr += sprintf( outstr, "%s LEFT JOIN (SELECT parent_id, %s as dirattr_sort "
                                   "FROM ENTRIES GROUP BY parent_id) as da ON id=da.parent_id "
                                   "WHERE %s", req_start, sort_dirattr_str, filter_dir_str );
                if (table_filter)
                    sprintf( currstr, " AND %s", table_filter );
                break;
            case FILTERDIR_OTHER:
                /* must join on dirattr_filter */
                /* @TODO: optimize if dirattr_sort is the same as dirattr */
                currstr += sprintf( outstr, "%s LEFT JOIN (SELECT parent_id, %s as dirattr, "
                               "%s as dirattr_sort FROM ENTRIES GROUP BY parent_id) "
                               "as da ON id=da.parent_id WHERE %s", req_start,
                               dirattr2str(filter_dir_index), sort_dirattr_str,
                               filter_dir_str);
                if (table_filter)
                    sprintf( currstr, " AND %s", table_filter );
                break;
            default:
                DisplayLog( LVL_CRIT, LISTMGR_TAG, "Unexpected filter on directory attribute in %s(): %#x",
                            __func__, filter_dir );
                /* ignore dir filter */
                append_dir_req( outstr, req_start, table_filter, sort_dirattr_str,
                                FILTERDIR_NONE, NULL, 0);
                break;
        }
    }
}

/** get an iterator on a list of entries */
struct lmgr_iterator_t *ListMgr_Iterator( lmgr_t * p_mgr,
                                          const lmgr_filter_t * p_filter,
                                          const lmgr_sort_type_t *
                                          p_sort_type, const lmgr_iter_opt_t * p_opt )
{
    char           query[4096];
    char           filter_str_main[2048];
    char           filter_str_annex[2048];
    char           filter_str_stripe_info[2048];
    char           filter_str_stripe_items[2048];
    char           filter_dir_str[512];
    filter_dir_e   filter_dir_type = FILTERDIR_NONE;
    unsigned int   filter_dir_index = 0;
    int            filter_main = 0;
    int            filter_annex = 0;
    int            filter_stripe_info = 0;
    int            filter_stripe_items = 0;
    int            rc;
    char           fields[2048];
    char           tables[2048];
    lmgr_iterator_t *it;
    table_enum     sort_table = T_NONE;
    int            sort_dirattr = 0;
    int            do_sort = 0;

    filter_str_main[0] = '\0';
    filter_str_annex[0] = '\0';
    filter_str_stripe_info[0] = '\0';
    filter_str_stripe_items[0] = '\0';

    char          *query_end;

    /* Iterator only select a sorted list of ids.
     * Entry attributes are retrieved afterward in ListMgr_GetNext() call.
     */

    /* is there a sort order ? */
    do_sort = ( p_sort_type && ( p_sort_type->order != SORT_NONE ) );

    /* check sort order */
    if ( do_sort )
    {
        if ( is_main_field( p_sort_type->attr_index ) )
            sort_table = T_MAIN;
        else if ( is_annex_field( p_sort_type->attr_index ) )
            sort_table = T_ANNEX;
        else if ( field_infos[p_sort_type->attr_index].db_type == DB_STRIPE_INFO )
            sort_table = T_STRIPE_INFO;
        else if ( field_infos[p_sort_type->attr_index].db_type == DB_STRIPE_ITEMS )
            sort_table = T_STRIPE_ITEMS;
        else if ( is_dirattr(p_sort_type->attr_index) )
            sort_dirattr = 1;
        else
        {
            DisplayLog( LVL_CRIT, LISTMGR_TAG, "Invalid field for sort order (index=%d)", p_sort_type->attr_index );
            return NULL;
        }
    }

    /* check filter location */

    /* /!\ possible cases:
     * - simplest: the fields of the filter and the attributes to be changed are in the same table
     * - harder: the fields of the filter are in the same table and attributes are in another different table 
     */

    /* 1) check the location of filters */

    if ( p_filter )
    {
        filter_dir_type = dir_filter(p_mgr, p_filter, filter_dir_str, &filter_dir_index);
        /* is sort dirattr the same as filter dirattr? */

        filter_main = filter2str( p_mgr, filter_str_main, p_filter, T_MAIN,
                                  FALSE, TRUE );

        if ( annex_table )
            filter_annex = filter2str( p_mgr, filter_str_annex, p_filter,
                                       T_ANNEX, ( filter_main > 0 ), TRUE );
        else
            filter_annex = 0;

        filter_stripe_info =
            filter2str( p_mgr, filter_str_stripe_info, p_filter, T_STRIPE_INFO,
                        ( filter_main > 0 ) || ( filter_annex > 0 ), TRUE );

        filter_stripe_items =
            filter2str( p_mgr, filter_str_stripe_items, p_filter, T_STRIPE_ITEMS,
                        ( filter_main > 0 ) || ( filter_annex > 0 )
                        || ( filter_stripe_info > 0 ), TRUE );


        if (( filter_main == 0 ) && ( filter_annex == 0 ) && ( filter_stripe_items == 0 )
             && ( filter_stripe_info == 0 ))
        {
            /* for dirattr filter, we can deal with table mixing */

            /* all records */
            if (filter_dir_type == FILTERDIR_NONE)
                DisplayLog( LVL_FULL, LISTMGR_TAG, "Empty filter: all records will be affected" );

            if ( do_sort )
            {
                /* entries must be selected depending on sort order */
                if ( sort_table == T_MAIN )
                    append_dir_req(query, "SELECT id FROM " MAIN_TABLE, NULL,
                                   sort_dirattr ? dirattr2str(p_sort_type->attr_index) : NULL,
                                   filter_dir_type, filter_dir_str, filter_dir_index);
                else if ( sort_table == T_ANNEX )
                    append_dir_req(query, "SELECT id FROM " ANNEX_TABLE, NULL,
                                   sort_dirattr ? dirattr2str(p_sort_type->attr_index) : NULL,
                                   filter_dir_type, filter_dir_str, filter_dir_index);
                else if ( sort_table == T_STRIPE_INFO )
                    append_dir_req(query, "SELECT id FROM " STRIPE_INFO_TABLE, NULL,
                                   sort_dirattr ? dirattr2str(p_sort_type->attr_index) : NULL,
                                   filter_dir_type, filter_dir_str, filter_dir_index);
                else if ( sort_table == T_STRIPE_ITEMS )
                    append_dir_req(query, "SELECT DISTINCT(id) FROM " STRIPE_ITEMS_TABLE, NULL,
                                   sort_dirattr ? dirattr2str(p_sort_type->attr_index) : NULL,
                                   filter_dir_type, filter_dir_str, filter_dir_index);
                else if ( sort_dirattr )
                {
                    if (filter_dir_type == FILTERDIR_NONE)
                        sprintf( query, "SELECT parent_id, %s as dirattr_sort FROM "MAIN_TABLE" GROUP BY parent_id",
                                 dirattr2str(p_sort_type->attr_index) );
                    else if (filter_dir_type == FILTERDIR_EMPTY)
                    {
                        DisplayLog( LVL_MAJOR, LISTMGR_TAG, "Unexpected case: sort %s for empty directories",
                                    dirattr2str(p_sort_type->attr_index) );
                        return NULL;
                    }
                    else
                    {
                        sprintf( query, "SELECT parent_id, %s as dirattr, %s as dirattr_sort FROM "MAIN_TABLE
                                 " GROUP BY parent_id HAVING %s",
                                 dirattr2str(filter_dir_index), dirattr2str(p_sort_type->attr_index),
                                 filter_dir_str);
                    }
                }
            }
            else
            {
                append_dir_req(query, "SELECT id FROM " MAIN_TABLE, NULL, NULL,
                               filter_dir_type, filter_dir_str, filter_dir_index);
            }
        }
        else if ( filter_main && !( filter_annex || filter_stripe_items || filter_stripe_info )
                  && ( !do_sort || ( sort_table == T_MAIN ) || sort_dirattr ) )
        {
            DisplayLog( LVL_FULL, LISTMGR_TAG, "Filter is only on " MAIN_TABLE " table" );

            /* filter/sort on main table
             * + optionally filter/sort on dirattrs
             */
            append_dir_req(query, "SELECT id FROM " MAIN_TABLE, filter_str_main,
                           sort_dirattr ? dirattr2str(p_sort_type->attr_index) : NULL,
                           filter_dir_type, filter_dir_str, filter_dir_index);
        }
        else if ( filter_annex && !( filter_main || filter_stripe_items || filter_stripe_info )
                  && ( !do_sort || ( sort_table == T_ANNEX ) || sort_dirattr ) )
        {
            DisplayLog( LVL_FULL, LISTMGR_TAG, "Filter is only on " ANNEX_TABLE " table" );

            append_dir_req(query, "SELECT id FROM " ANNEX_TABLE, filter_str_annex,
                           sort_dirattr ? dirattr2str(p_sort_type->attr_index) : NULL,
                           filter_dir_type, filter_dir_str, filter_dir_index);
        }
        else if ( filter_stripe_info && !( filter_main || filter_annex || filter_stripe_items )
                  && ( !do_sort || ( sort_table == T_STRIPE_INFO ) || sort_dirattr ) )
        {
            DisplayLog( LVL_FULL, LISTMGR_TAG, "Filter is only on " STRIPE_INFO_TABLE " table" );

            append_dir_req(query, "SELECT id FROM " STRIPE_INFO_TABLE, filter_str_stripe_info,
                           sort_dirattr ? dirattr2str(p_sort_type->attr_index) : NULL,
                           filter_dir_type, filter_dir_str, filter_dir_index);
        }
        else if ( filter_stripe_items && !( filter_main || filter_annex || filter_stripe_info )
                  && ( !do_sort || ( sort_table == T_STRIPE_ITEMS ) || sort_dirattr ) )
        {
            DisplayLog( LVL_FULL, LISTMGR_TAG, "Filter is only on " STRIPE_ITEMS_TABLE " table" );

            append_dir_req(query, "SELECT DISTINCT(id) FROM " STRIPE_ITEMS_TABLE, filter_str_stripe_items,
                           sort_dirattr ? dirattr2str(p_sort_type->attr_index) : NULL,
                           filter_dir_type, filter_dir_str, filter_dir_index);
        }
        else /* filter or sort on several tables */
        {
            char          *curr_fields = fields;
            char          *curr_tables = tables;
            char          *first_table = NULL;

            DisplayLog( LVL_FULL, LISTMGR_TAG,
                        "Filter or sort order on several tables: "
                        MAIN_TABLE ":%d, " ANNEX_TABLE ":%d, "
                        STRIPE_INFO_TABLE ":%d, "
                        STRIPE_ITEMS_TABLE ":%d",
                        filter_main, filter_annex, filter_stripe_info, filter_stripe_items );

            if ( ( filter_main > 0 ) || ( do_sort && ( sort_table == T_MAIN ) ) )
            {
                first_table = MAIN_TABLE;

                if ( filter_main > 0 )
                    curr_fields += sprintf( curr_fields, "%s", filter_str_main );

                curr_tables += sprintf( curr_tables, "%s", MAIN_TABLE );
            }

            if ( ( filter_annex > 0 ) || ( do_sort && ( sort_table == T_ANNEX ) ) )
            {
                if ( filter_annex > 0 )
                    curr_fields += sprintf( curr_fields, "%s", filter_str_annex );

                if ( ( filter_main > 0 ) || ( do_sort && ( sort_table == T_MAIN ) ) )
                {
                    *curr_tables = ',';
                    curr_tables++;

                    /* also add junction condition */
                    curr_fields +=
                        sprintf( curr_fields, " AND %s.id=%s.id", MAIN_TABLE, ANNEX_TABLE );
                }
                else
                    first_table = ANNEX_TABLE;

                curr_tables += sprintf( curr_tables, "%s", ANNEX_TABLE );
            }

            if ( ( filter_stripe_items > 0 ) || ( do_sort && ( sort_table == T_STRIPE_ITEMS ) ) )
            {
                if ( filter_stripe_items > 0 )
                    curr_fields += sprintf( curr_fields, "%s", filter_str_stripe_items );

                if ( ( filter_main > 0 )
                     || ( do_sort && ( sort_table == T_MAIN ) )
                     || ( filter_annex > 0 ) || ( do_sort && ( sort_table == T_ANNEX ) ) )
                {
                    *curr_tables = ',';
                    curr_tables++;

                    /* add junction condition */
                    if ( ( filter_main > 0 ) || ( do_sort && ( sort_table == T_MAIN ) ) )
                        curr_fields +=
                            sprintf( curr_fields, " AND %s.id=%s.id",
                                     MAIN_TABLE, STRIPE_ITEMS_TABLE );
                    if ( ( filter_annex > 0 ) || ( do_sort && ( sort_table == T_ANNEX ) ) )
                        curr_fields +=
                            sprintf( curr_fields, " AND %s.id=%s.id",
                                     ANNEX_TABLE, STRIPE_ITEMS_TABLE );
                }
                else
                    first_table = STRIPE_ITEMS_TABLE;

                curr_tables += sprintf( curr_tables, "%s", STRIPE_ITEMS_TABLE );
            }

            if ( ( filter_stripe_info > 0 ) || ( do_sort && ( sort_table == T_STRIPE_INFO ) ) )
            {
                if ( filter_stripe_info > 0 )
                    curr_fields += sprintf( curr_fields, "%s", filter_str_stripe_info );

                if ( ( filter_main > 0 )
                     || ( do_sort && ( sort_table == T_MAIN ) )
                     || ( filter_annex > 0 ) || ( do_sort && ( sort_table == T_ANNEX ) )
                     || ( filter_stripe_items > 0 ) || ( do_sort
                                                         && ( sort_table == T_STRIPE_ITEMS ) ) )
                {
                    *curr_tables = ',';
                    curr_tables++;

                    /* add junction condition */
                    if ( ( filter_main > 0 ) || ( do_sort && ( sort_table == T_MAIN ) ) )
                        curr_fields +=
                            sprintf( curr_fields, " AND %s.id=%s.id",
                                     MAIN_TABLE, STRIPE_INFO_TABLE );
                    if ( ( filter_annex > 0 ) || ( do_sort && ( sort_table == T_ANNEX ) ) )
                        curr_fields +=
                            sprintf( curr_fields, " AND %s.id=%s.id",
                                     ANNEX_TABLE, STRIPE_INFO_TABLE );
                    if ( ( filter_stripe_items > 0 )
                         || ( do_sort && ( sort_table == T_STRIPE_ITEMS ) ) )
                        curr_fields +=
                            sprintf( curr_fields, " AND %s.id=%s.id", STRIPE_ITEMS_TABLE,
                                     STRIPE_INFO_TABLE );

                }
                else
                    first_table = STRIPE_INFO_TABLE;

                curr_tables += sprintf( curr_tables, "%s", STRIPE_INFO_TABLE );
            }

            DisplayLog(LVL_FULL, LISTMGR_TAG, "first_table=%s, tables=%s",
                       first_table, tables);

            char tmp[1024];
            sprintf( tmp, "SELECT %s.id AS id FROM %s", first_table, tables );
            append_dir_req( query, tmp, fields,
                            sort_dirattr ? dirattr2str(p_sort_type->attr_index) : NULL,
                            filter_dir_type, filter_dir_str, filter_dir_index );
        }
    }
    else if ( do_sort )
    {

        /* no filter: entries must be selected depending on sort order */
        if ( sort_table == T_MAIN )
            strcpy( query, "SELECT id FROM " MAIN_TABLE );
        else if ( sort_table == T_ANNEX )
            strcpy( query, "SELECT id FROM " ANNEX_TABLE );
        else if ( sort_table == T_STRIPE_ITEMS )
            strcpy( query, "SELECT DISTINCT(id) FROM " STRIPE_ITEMS_TABLE );
        else if ( sort_table == T_STRIPE_INFO )
            strcpy( query, "SELECT id FROM " STRIPE_INFO_TABLE );
        else if (sort_dirattr)
            sprintf( query, "SELECT parent_id, %s as dirattr_sort FROM "MAIN_TABLE" GROUP BY parent_id",
                    dirattr2str(p_sort_type->attr_index) );
    }
    else
    {
        DisplayLog( LVL_FULL, LISTMGR_TAG, "Empty filter: all records will be selected" );
        strcpy( query, "SELECT id FROM " MAIN_TABLE );
    }

    query_end = query + strlen( query );

    /* sort order */
    if ( do_sort )
    {
        if ( sort_table == T_MAIN )
        {
            query_end +=
                sprintf( query_end, " ORDER BY " MAIN_TABLE ".%s ",
                         field_infos[p_sort_type->attr_index].field_name );
        }
        else if ( sort_table == T_ANNEX )
        {
            query_end +=
                sprintf( query_end, " ORDER BY " ANNEX_TABLE ".%s ",
                         field_infos[p_sort_type->attr_index].field_name );
        }
        else if ( sort_table == T_STRIPE_ITEMS )
        {
            query_end += sprintf( query_end, " ORDER BY " STRIPE_ITEMS_TABLE ".storage_unit " );
        }
        else if ( sort_table == T_STRIPE_INFO )
        {
            query_end += sprintf( query_end, " ORDER BY " STRIPE_INFO_TABLE ".pool_name " );
        }
        else if ( sort_dirattr )
        {
            query_end += sprintf( query_end, " ORDER BY dirattr_sort " );
        }

        if ( p_sort_type->order == SORT_ASC )
            query_end += sprintf( query_end, "ASC" );
        else
            query_end += sprintf( query_end, "DESC" );
    }

    /* iterator opt */
    if ( p_opt && ( p_opt->list_count_max > 0 ) )
    {
        query_end += sprintf( query_end, " LIMIT %u", p_opt->list_count_max );
    }

    /* allocate a new iterator */
    it = ( lmgr_iterator_t * ) MemAlloc( sizeof( lmgr_iterator_t ) );
    it->p_mgr = p_mgr;
    if (p_opt)
    {
        it->opt = *p_opt;
        it->opt_is_set = 1;
    }
    else
    {
        it->opt_is_set = 0;
    }

#ifdef _DEBUG_DB
    printf( "Iterator is specified by: %s\n", query );
#endif

    /* execute request */
    rc = db_exec_sql( &p_mgr->conn, query, &it->select_result );

    if ( rc )
    {
        MemFree( it );
        return NULL;
    }
    else
        return it;
}



int ListMgr_GetNext( struct lmgr_iterator_t *p_iter, entry_id_t * p_id, attr_set_t * p_info )
{
    int            rc = 0;
    /* can contain id+dirattr+dirattr_sort in case of directory listing */
    char          *idstr[3];
    DEF_PK(pk);

    int            entry_disappeared = FALSE;

    do
    {
        entry_disappeared = FALSE;

        idstr[0] = idstr[1] = idstr[2] = NULL;
        rc = db_next_record( &p_iter->p_mgr->conn, &p_iter->select_result, idstr, 3 );

        if ( rc )
            return rc;
        if ( idstr == NULL )
            return DB_REQUEST_FAILED;

        if ( sscanf( idstr[0], SPK, PTR_PK(pk) ) != 1 )
            return DB_REQUEST_FAILED;

        /* retrieve entry id (except validator) */
        rc = pk2entry_id( p_iter->p_mgr, pk, p_id );

        /* /!\ If the entry disappeared from DB, we must go to next record */
        if ( rc == DB_NOT_EXISTS )
            entry_disappeared = TRUE;
        else if ( rc )
            return rc;

        /* Idem */
        rc = listmgr_get_by_pk( p_iter->p_mgr, pk, p_info );

        if ( rc == DB_NOT_EXISTS )
        {
            if (p_iter->opt_is_set && p_iter->opt.allow_no_attr)
            {
                /* clear missing fields */
                p_info->attr_mask &= dir_attr_set;
                /* special field dircount */
                if (dirattr_fields( p_info->attr_mask ))
                {
                    if (listmgr_get_dirattrs(p_iter->p_mgr, pk, p_info))
                    {
                        DisplayLog( LVL_MAJOR, LISTMGR_TAG, "listmgr_get_dirattr failed for "DPK, pk );
                        p_info->attr_mask &= ~dir_attr_set;
                    }
                }

                /* compute generated fields if asked */
                generate_fields( p_info );

                rc = 0;
            }
            else
                entry_disappeared = TRUE;
        }
    }
    while ( entry_disappeared );        /* goto next record if entry desappered */

    return rc;

}


void ListMgr_CloseIterator( struct lmgr_iterator_t *p_iter )
{
    db_result_free( &p_iter->p_mgr->conn, &p_iter->select_result );
    MemFree( p_iter );
}
