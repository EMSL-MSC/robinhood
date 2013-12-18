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

#include "listmgr_stripe.h"
#include "database.h"
#include "listmgr_common.h"
#include "Memory.h"
#include "RobinhoodLogs.h"
#include "var_str.h"
#include <stdio.h>
#include <stdlib.h>

#define STRIPE_INFO_FIELDS "id,validator,stripe_count,stripe_size,pool_name"
#define STRIPE_INFO_SET_VALUES "validator=VALUES(validator),stripe_count=VALUES(stripe_count)," \
                               "stripe_size=VALUES(stripe_size),pool_name=VALUES(pool_name)"

#define STRIPE_ITEMS_FIELDS "id,stripe_index,ostidx,details"

static int delete_stripe_info( lmgr_t * p_mgr, PK_ARG_T pk )
{
    int            rc;
    char           query[4096];

    sprintf( query, "DELETE FROM " STRIPE_ITEMS_TABLE " WHERE id="DPK, pk );

    rc = db_exec_sql( &p_mgr->conn, query, NULL );
    if ( rc )
        return rc;

    sprintf( query, "DELETE FROM " STRIPE_INFO_TABLE " WHERE id="DPK, pk );

    rc = db_exec_sql( &p_mgr->conn, query, NULL );
    if ( rc )
        return rc;

    return 0;
}

int update_stripe_info(lmgr_t *p_mgr, PK_ARG_T pk, int validator,
                       const stripe_info_t *p_stripe,
                       const stripe_items_t *p_items, int insert_if_absent)
{
    int rc;

    if (p_stripe == NULL)
        return DB_INVALID_ARG;

    rc = delete_stripe_info(p_mgr, pk);
    if (rc && !insert_if_absent)
        return rc;

    return insert_stripe_info(p_mgr, pk, validator, p_stripe, p_items, FALSE);
}


int insert_stripe_info( lmgr_t * p_mgr, PK_ARG_T pk,
                        int validator, const stripe_info_t * p_stripe,
                        const stripe_items_t * p_items,
                        int update_if_exists )
{
    int            i, rc;
    int            created = FALSE;

    if ( p_stripe == NULL )
        return DB_INVALID_ARG;

    do
    {
        char short_query[4096];
        /* First insert info into STRIPE_INFO_TABLE,
         * so if a file is already present with the same id,
         * we will remove its previous stripe info */

        sprintf( short_query, "INSERT INTO " STRIPE_INFO_TABLE
                 "("STRIPE_INFO_FIELDS") "
                 "VALUES ("DPK",%u,%u,%u,'%s')", pk, validator,
                 p_stripe->stripe_count, ( unsigned int ) p_stripe->stripe_size,
                 p_stripe->pool_name );

        if ( update_if_exists )
            rc = db_exec_sql_quiet( &p_mgr->conn, short_query, NULL );
        else
            rc = db_exec_sql( &p_mgr->conn, short_query, NULL );

        if ( rc == 0 )
        {
            created = TRUE;
        }
        else if ( (rc == DB_ALREADY_EXISTS) && update_if_exists )
        {
            /* remove previous stripe info */
            DisplayLog( LVL_EVENT, LISTMGR_TAG,
                        "A stripe info already exists with this identifier, removing it" );
            rc = delete_stripe_info( p_mgr, pk );
        }

        if ( rc != 0 )
        {
            DisplayLog( LVL_CRIT, LISTMGR_TAG,
                        "DB query failed in %s line %d: code=%d: %s",
                        __FUNCTION__, __LINE__, rc, db_errmsg( &p_mgr->conn, short_query, 4096 ) );
            return rc;
        }

    }
    while ( !created );         /* retry loop in case a similar entry already exists */

    /* then insert stripe items */
    if ( (p_items != NULL) && (p_items->count > 0) )
    {
        ssize_t         len;
        unsigned int    est_len;
        char            *query = NULL;
        char buff[2*STRIPE_DETAIL_SZ+1];

        /* estimate query size = fix part + stripe_count * ( 4 + pklen + ost_idx_len + index_len + 2*detail_len )
         *                     = ~64(oversize to 128) + stripe_count * 128
         */
        est_len = 128 + p_items->count * (128+2*STRIPE_DETAIL_SZ);
        DisplayLog( LVL_FULL, LISTMGR_TAG, "Estimated query size for %u stripe = %u",  p_items->count, est_len );
        query = MemAlloc(est_len);
        if (query == NULL) {
            DisplayLog( LVL_CRIT, LISTMGR_TAG, "Not enough memory to build SQL query (length = %u)", est_len );
            return DB_NO_MEMORY;
        }

        strcpy( query, "INSERT INTO " STRIPE_ITEMS_TABLE "(id, stripe_index, ostidx, details) VALUES " );
        len = strlen( query );

        /* first stripe item */
        if (buf2hex(buff, sizeof(buff), (unsigned char *)(&p_items->stripe[0].ost_gen), STRIPE_DETAIL_SZ ) < 0){
            DisplayLog( LVL_CRIT, LISTMGR_TAG, "Buffer too small to store details stripe info");
            buff[2*STRIPE_DETAIL_SZ] = '\0';
        }
        else
            DisplayLog( LVL_FULL, LISTMGR_TAG, "Stripe details encoding = x'%s'", buff);

        len += snprintf( query + len, est_len - len, "("DPK",0,%u,x'%s')", pk,
                         p_items->stripe[0].ost_idx, buff );

        /* next items */
        for ( i = 1; i < p_items->count; i++ )
        {
            /* first stripe item */
            if (buf2hex(buff, sizeof(buff), (unsigned char *)(&p_items->stripe[i].ost_gen), STRIPE_DETAIL_SZ ) < 0){
                DisplayLog( LVL_CRIT, LISTMGR_TAG, "Buffer too small to store details stripe info");
                buff[2*STRIPE_DETAIL_SZ] = '\0';
            }
            else
                DisplayLog( LVL_FULL, LISTMGR_TAG, "Stripe details encoding = x'%s'", buff);

            len += snprintf( query + len, est_len - len, ",("DPK",%u,%u,x'%s')", pk,
                             i, p_items->stripe[i].ost_idx, buff );

            if ( len >= est_len )
            {
                DisplayLog( LVL_CRIT, LISTMGR_TAG,
                            "Error in %s(): query too long (>%u bytes long), stripe_count=%d",
                            __FUNCTION__, est_len, p_items->count );
                MemFree( query );
                return DB_BUFFER_TOO_SMALL;
            }
        }

        rc = db_exec_sql( &p_mgr->conn, query, NULL );
        if ( rc )
        {
            DisplayLog( LVL_CRIT, LISTMGR_TAG,
                        "DB query failed in %s line %d: code=%d: %s",
                        __FUNCTION__, __LINE__, rc, db_errmsg( &p_mgr->conn, query, 4096 ) );
            MemFree( query );
            return rc;
        }
        MemFree( query );
    }

    return 0;
}

int batch_insert_stripe_info(lmgr_t *p_mgr, pktype *pklist, int *validators,
                             attr_set_t **p_attrs,
                             unsigned int count, int update_if_exists)
{
    int     i, rc;
    char    tmp[1024];
    var_str query = VAR_STR_NULL;

    if (!ATTR_MASK_TEST(p_attrs[0], stripe_info))
        return DB_INVALID_ARG;

    /* build batch request for STRIPE_INFO table */
    var_str_append(&query, "INSERT INTO "STRIPE_INFO_TABLE
                   " ("STRIPE_INFO_FIELDS") VALUES ");
    for (i = 0; i < count; i++)
    {
        sprintf(tmp, "%s("DPK",%u,%u,%u,'%s')", i == 0 ? "" : ",",
                pklist[i], validators[i], ATTR(p_attrs[i], stripe_info).stripe_count,
                (unsigned int)ATTR(p_attrs[i], stripe_info).stripe_size,
                ATTR(p_attrs[i], stripe_info).pool_name);
        var_str_append(&query, tmp);
    }

    if (update_if_exists)
        /* append "on duplicate key ..." */
        var_str_append(&query, " ON DUPLICATE KEY UPDATE "STRIPE_INFO_SET_VALUES);

    rc = db_exec_sql(&p_mgr->conn, VAR_STR_START(query), NULL);
    if (rc)
        goto out;

    var_str_reset(&query);

    /* Stripe items more tricky because we want to delete previous items on update */
    /* If update_if_exists is false, insert them all as a batch.
     * For the update case, remove previous items before bluk insert.
     */
    if (update_if_exists)
    {
        for (i = 0; i < count; i++)
        {
            sprintf(tmp, "DELETE FROM " STRIPE_ITEMS_TABLE " WHERE id="DPK, pklist[i]);

            rc = db_exec_sql(&p_mgr->conn, tmp, NULL);
            if ( rc )
                goto out;
        }
    }

    /* bulk insert stripe items */
    if (!ATTR_MASK_TEST(p_attrs[0], stripe_items))
        goto out;

    var_str_append(&query, "INSERT INTO " STRIPE_ITEMS_TABLE
                           " ("STRIPE_ITEMS_FIELDS") VALUES ");
    /* loop on all entries and all stripe items */
    for (i = 0; i < count; i++)
    {
        int s;
        const stripe_items_t * p_items;

        p_items = &ATTR(p_attrs[i], stripe_items);
        for (s = 0; s < p_items->count; s++)
        {
            char buff[2*STRIPE_DETAIL_SZ+1];
            if (buf2hex(buff, sizeof(buff), (unsigned char *)(&p_items->stripe[s].ost_gen), STRIPE_DETAIL_SZ ) < 0)
            {
                DisplayLog(LVL_CRIT, LISTMGR_TAG, "Buffer too small to store details stripe info");
                memset(buff, 0, sizeof(buff));
            }
            sprintf(tmp, "%s("DPK",%u,%u,x'%s')", (i == 0) && (s == 0) ? "" : ",",
                    pklist[i], s, p_items->stripe[s].ost_idx, buff);
            var_str_append(&query, tmp);
        }
    }

    rc = db_exec_sql(&p_mgr->conn, VAR_STR_START(query), NULL);

out:
    var_str_free(&query);
    return rc;
}


int get_stripe_info( lmgr_t * p_mgr, PK_ARG_T pk, stripe_info_t * p_stripe_info,
                     stripe_items_t * p_items )
{
    char           query[1024];
    char          *res[3];
    result_handle_t result;
    int            i;
    int            rc = DB_SUCCESS;

    /* retrieve basic stripe info */
    sprintf( query,
             "SELECT stripe_count, stripe_size, pool_name FROM " STRIPE_INFO_TABLE " WHERE id="DPK,
             pk );

    rc = db_exec_sql( &p_mgr->conn, query, &result );
    if ( rc )
        goto out;

    rc = db_next_record( &p_mgr->conn, &result, res, 3 );
    if ( rc == DB_END_OF_LIST )
        rc = DB_NOT_EXISTS;
    if ( rc )
        goto res_free;

    if ( res[0] == NULL || res[1] == NULL || res[2] == NULL )
    {
        rc = DB_ATTR_MISSING;
        goto res_free;
    }

    p_stripe_info->stripe_count = atoi( res[0] );
    p_stripe_info->stripe_size = atoi( res[1] );
    strncpy( p_stripe_info->pool_name, res[2], MAX_POOL_LEN );
    p_stripe_info->pool_name[MAX_POOL_LEN-1] = 0;

    db_result_free( &p_mgr->conn, &result );

    if ( p_items )
    {
        /* retrieve stripe list */
        sprintf( query, "SELECT stripe_index,ostidx,details FROM " STRIPE_ITEMS_TABLE " WHERE id="DPK
                        " ORDER BY stripe_index ASC", pk );

        rc = db_exec_sql( &p_mgr->conn, query, &result );
        if ( rc )
            goto out;

        if ( p_stripe_info->stripe_count != db_result_nb_records( &p_mgr->conn, &result ) )
        {
            DisplayLog( LVL_MAJOR, LISTMGR_TAG,
                        "Warning: the number of stripe items (%d) doesn't match stripe count (%u)! (Pk="DPK")",
                        db_result_nb_records( &p_mgr->conn, &result ), p_stripe_info->stripe_count, pk );
        }
        p_items->count = db_result_nb_records( &p_mgr->conn, &result );

        if ( p_items->count > 0 )
        {

            /* allocate stripe array */
            p_items->stripe = MemCalloc( p_items->count, sizeof( stripe_item_t ) );

            if ( !p_items->stripe )
            {
                rc = DB_NO_MEMORY;
                goto res_free;
            }

            /* fill stripe units */
            for ( i = 0; i < p_items->count; i++ )
            {
                rc = db_next_record( &p_mgr->conn, &result, res, 3 );
                if ( rc )
                    goto stripe_free;

                if ( res[0] == NULL )
                {
                    rc = DB_ATTR_MISSING;
                    goto stripe_free;
                }

                if (i != atoi( res[0] ))
                {
                    DisplayLog(LVL_MAJOR, LISTMGR_TAG, "Warning: inconsistent stripe order: stripe %s returned in position %u",
                               res[0], i);
                }
                p_items->stripe[i].ost_idx = atoi( res[1] );
                /* raw copy of binary buffer (last 3 fields of stripe_item_t = address of ost_gen field) */
                memcpy(&p_items->stripe[i].ost_gen, res[2], STRIPE_DETAIL_SZ);
            }
        }
        else
            p_items->stripe = NULL;

        /* last query result must be freed */
        rc = DB_SUCCESS;
        goto res_free;
    }

    /* nothing to free */
    return DB_SUCCESS;

    stripe_free:
    MemFree( p_items->stripe );
    p_items->stripe = NULL;
    p_items->count = 0;
    p_stripe_info->stripe_count = 0;
    res_free:
    db_result_free( &p_mgr->conn, &result );
    out:
    return rc;
}

/** release stripe information */
void free_stripe_items( stripe_items_t * p_stripe_items )
{
    if ( p_stripe_items->stripe )
        MemFree( p_stripe_items->stripe );
    p_stripe_items->stripe = NULL;
    p_stripe_items->count = 0;
}


/* check that validator is matching for a given entry */
int ListMgr_CheckStripe( lmgr_t * p_mgr, const entry_id_t * p_id )
{
    char           query[1024];
    char          *res;
    result_handle_t result;
    int            rc = DB_SUCCESS;
    DEF_PK(pk);

    entry_id2pk(p_id, PTR_PK(pk));

    sprintf( query, "SELECT validator FROM " STRIPE_INFO_TABLE " WHERE id="DPK, pk );

    rc = db_exec_sql( &p_mgr->conn, query, &result );
    if ( rc )
        goto out;

    rc = db_next_record( &p_mgr->conn, &result, &res, 1 );

    if ( rc == DB_END_OF_LIST )
        rc = DB_NOT_EXISTS;

    if ( rc )
        goto res_free;

    if ( res == NULL )
    {
        rc = DB_ATTR_MISSING;
        goto res_free;
    }

    if ( atoi( res ) != VALID(p_id) )
    {
        delete_stripe_info( p_mgr, pk );
        rc = DB_OUT_OF_DATE;
    }
    else
        rc = DB_SUCCESS;

  res_free:
    db_result_free( &p_mgr->conn, &result );
  out:
    return rc;
}


int ListMgr_SetStripe( lmgr_t * p_mgr, const entry_id_t * p_id,
                       stripe_info_t * p_stripe_info, stripe_items_t * p_stripe_items )
{
    DEF_PK(pk);

    entry_id2pk(p_id, PTR_PK(pk));

    return insert_stripe_info( p_mgr, pk, VALID(p_id), p_stripe_info, p_stripe_items,
                               TRUE );
}
