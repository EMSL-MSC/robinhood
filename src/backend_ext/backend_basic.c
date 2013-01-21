/* -*- mode: c; c-basic-offset: 4; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=4:tabstop=4:
 */
/*
 * Copyright (C) 2010 CEA/DAM
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the CeCILL License.
 *
 * The fact that you are presently reading this means that you have had
 * knowledge of the CeCILL license (http://www.cecill.info) and that you
 * accept its terms.
 */

/**
 * \file   backend_basic.c
 * \author Th. Leibovici
 * \brief  basic backend implementation
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "backend_ext.h"
#include "backend_mgr.h"
#include "RobinhoodLogs.h"
#include "RobinhoodMisc.h"
#include "global_config.h"
#include "xplatform_print.h"
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <utime.h>
#include <libgen.h>
#include <pwd.h>
#include <grp.h>
#include <ctype.h>


#ifdef HAVE_PURGE_POLICY
#ifdef HAVE_SHOOK
#include <shook_svr.h>
#endif
#endif


#define RBHEXT_TAG "Backend"

/**
 * Get compatibility information,
 * to check compatibility with the current FS.
 */
int rbhext_compat_flags()
{
    int compat_flags = 0;

    /* if entry id is fid, this module is only compatible with Lustre filesystems */
#ifdef _HAVE_FID
    compat_flags |= RBHEXT_COMPAT_LUSTRE;
#endif
    return compat_flags;
}

static backend_config_t config;
static dev_t backend_dev = 0;
static char  backend_name[RBH_PATH_MAX] = "";

/* is it a special shell character */
static inline int is_shell_special(char c)
{
    static const char * specials = "`#$*?!|;&<>[]{}'\"\\";
    const char * curr;
    for (curr = specials; (*curr) != '\0'; curr++)
        if (c == (*curr))
            return TRUE;
    /* not found */
    return FALSE;
}

#define is_allowed_char(_c) (isascii(_c) && !isspace(_c) && !is_shell_special(_c))

/* clean non ascii characters, spaces, special chars, ... */
static void clean_bad_chars(char * path)
{
    char * curr;
    for ( curr = path; *curr != '\0'; curr++ )
    {
        if ( !is_allowed_char(*curr) )
            *curr = '_';
    }
}

/**
 * Initialize the extension module.
 * \param[in] config_string specific config string (e.g path to config file...)
 * \param[out] p_behaviors_flags pointer to output mask that describes extension behavior
 */
int rbhext_init( const backend_config_t * conf,
                 unsigned int * p_behaviors_flags )
{
    int rc;

    config = *conf;

    /* synchronous archiving and rm support */
    *p_behaviors_flags = RBHEXT_SYNC_ARCHIVE | RBHEXT_RM_SUPPORT;

#ifdef HAVE_PURGE_POLICY
    *p_behaviors_flags |= RBHEXT_RELEASE_SUPPORT;
#endif

#ifdef HAVE_SHOOK
    rc = shook_svr_init(config.shook_cfg);
    if (rc)
    {
        DisplayLog( LVL_CRIT, RBHEXT_TAG, "ERROR %d initializing shook server library",
                    rc );
        return rc;
    }
#endif

    /* check that backend filesystem is mounted */
    rc = CheckFSInfo( config.root, config.mnt_type, &backend_dev, backend_name,
                      config.check_mounted, FALSE );
    if ( rc )
        return -rc;

    return 0;
}

/**
 * Determine attributes to be provided for rbhext_get_status().
 * \param[in] entry_type type of entry to check status.
 * \param[out] p_attr_allow_cached list of attributes needed for determining status
 *                                 that can be retrieved from DB (cached)
 * \param[out] p_attr_need_fresh list of attributes needed for determining status
 *                                 that need to be up-to-date.
 * \retval 0 on success
 * \retval <0 on error
 * \retval -ENOTSUP backup is not implemented for this type of entry.
 */
int rbhext_status_needs( obj_type_t   entry_type,
                         unsigned int * p_attr_allow_cached,
                         unsigned int * p_attr_need_fresh )
{
    *p_attr_allow_cached = 0;
    *p_attr_need_fresh = 0;

    /* support files and symlinks */
    if ( (entry_type != TYPE_FILE)
         && (entry_type != TYPE_LINK)
         && (entry_type != TYPE_NONE) )
        return -ENOTSUP;

    /* type is useful in any case (does not change during entry lifetime,
     * so we can use a cached value). */
    (*p_attr_allow_cached) |= ATTR_MASK_type;

    /* Previous backup path is also needed.
     * it is only from DB (so it is a cached information). */
    (*p_attr_allow_cached) |= ATTR_MASK_backendpath;
    (*p_attr_allow_cached) |= ATTR_MASK_last_archive;

    /* needs fresh mtime/size information from lustre
     * to determine if the entry changed */
    (*p_attr_need_fresh) |= ATTR_MASK_last_mod;
    (*p_attr_need_fresh) |= ATTR_MASK_size;

#ifndef _HAVE_FID
    /* for lustre<2.0, need fresh entry path */
    (*p_attr_need_fresh) |= ATTR_MASK_fullpath;
#else
    /* just needed to have human readable backend path */
    (*p_attr_allow_cached) |= ATTR_MASK_fullpath;
#endif
    return 0;
}

typedef enum {
       FOR_LOOKUP,
       FOR_NEW_COPY
} what_for_e;

/* path for entry we don't known the path in Lustre */
#define UNK_PATH    "__unknown_path"
/* name for entry we don't known the name in Lustre */
#define UNK_NAME    "__unknown_name"
/* extension for temporary copy file */
#define COPY_EXT    "xfer"
/* trash directory for orphan files */
#define TRASH_DIR   ".orphans"


/**
 * Build the path of a given entry in the backend.
 */
static int entry2backend_path( const entry_id_t * p_id,
                               const attr_set_t * p_attrs_in,
                               what_for_e what_for,
                               char * backend_path )
{
    int pathlen;
    char rel_path[RBH_PATH_MAX];

    if ( ATTR_MASK_TEST(p_attrs_in, backendpath) )
    {
       DisplayLog( LVL_DEBUG, RBHEXT_TAG, "%s: previous backend_path: %s",
                   (what_for == FOR_LOOKUP)?"LOOKUP":"NEW_COPY",
                   ATTR(p_attrs_in, backendpath) );
    }
    else if (ATTR_MASK_TEST(p_attrs_in, type) &&
             !strcasecmp(ATTR(p_attrs_in, type), STR_TYPE_DIR))
    {
        if (ATTR_MASK_TEST(p_attrs_in, fullpath) &&
            relative_path(ATTR(p_attrs_in, fullpath), global_config.fs_path,
                          rel_path ) == 0)
        {
            DisplayLog(LVL_DEBUG, RBHEXT_TAG, "%s is a directory: backend path is the same",
                       ATTR(p_attrs_in, fullpath));

            if (!strcmp(config.root, "/")) /* root is '/' */
                sprintf(backend_path, "/%s", rel_path);
            else
                sprintf(backend_path, "%s/%s", config.root, rel_path);
        }
        else /* we don't have fullpath available */
        {
            const char * fname;

            if ( ATTR_MASK_TEST(p_attrs_in, name) )
                fname = ATTR(p_attrs_in, name);
            else
                fname = UNK_NAME;

            /* backup entry to a special dir */
            if ( !strcmp(config.root, "/") ) /* root is '/' */
                sprintf(backend_path, "/%s/%s", UNK_PATH, fname);
            else
                sprintf(backend_path, "%s/%s/%s", config.root, UNK_PATH, fname );
        }

        /* clean bad characters */
        clean_bad_chars(backend_path);
        return 0;
    }
#ifdef HAVE_SHOOK
    else
    {
        int rc;
        char fidpath[RBH_PATH_MAX];

        BuildFidPath( p_id, fidpath );

        /* retrieve backend path from shook xattrs */
        rc = shook_get_hsm_info(fidpath, backend_path, NULL);
        if ((rc == 0) && !EMPTY_STRING(backend_path))
            return 0;
    }
#endif

    if ( (what_for == FOR_LOOKUP) && ATTR_MASK_TEST(p_attrs_in, backendpath) )
    {
        /* For lookup, if there is a previous path in the backend, use it. */
        strcpy(backend_path, ATTR(p_attrs_in, backendpath));
    }
    else /* in any other case, build a path from scratch */
    {
        /* if the fullpath is available, build human readable path */
        if ( ATTR_MASK_TEST(p_attrs_in, fullpath) &&
             relative_path( ATTR(p_attrs_in, fullpath), global_config.fs_path,
                            rel_path ) == 0 )
        {
            /* backend path is '<bakend_root>/<rel_path>' */

            if ( !strcmp(config.root, "/") ) /* root is '/' */
                sprintf(backend_path, "/%s", rel_path);
            else
                sprintf(backend_path, "%s/%s", config.root, rel_path);
        }
        else /* we don't have fullpath available */
        {
            const char * fname;

            if ( ATTR_MASK_TEST(p_attrs_in, name) )
                fname = ATTR(p_attrs_in, name);
            else
                fname = UNK_NAME;

            /* backup entry to a special dir */
            if ( !strcmp(config.root, "/") ) /* root is '/' */
                sprintf(backend_path, "/%s/%s", UNK_PATH, fname);
            else
                sprintf(backend_path, "%s/%s/%s", config.root, UNK_PATH, fname );
        }

        /* clean bad characters */
        clean_bad_chars(backend_path);

        /* add __<id> after the name */
        pathlen = strlen(backend_path);
#ifdef  _HAVE_FID
        sprintf( backend_path + pathlen, "__"DFID_NOBRACE, PFID(p_id) );
#else
        sprintf( backend_path + pathlen, "__%#LX:%#LX",
                 (unsigned long long)p_id->device,
                 (unsigned long long)p_id->inode );
#endif
    }
    return 0;
}

/**
 * Determine if an entry is being archived
 * \retval 0: not archiving
 * \retval <0: error
 * \retval >0: last modification time
 */
static int entry_is_archiving(const char * backend_path )
{
    char xfer_path[RBH_PATH_MAX];
    struct stat cp_md;
    int rc;
    sprintf(xfer_path, "%s.%s", backend_path, COPY_EXT );

    if ( lstat(xfer_path, &cp_md ) != 0 )
    {
        rc = -errno;
        if ( (rc == -ENOENT) || (rc == -ESTALE) )
            return 0;
        else
            return rc;
    }
    /* xfer is running. return last action time */
    return MAX3( cp_md.st_mtime, cp_md.st_ctime, cp_md.st_atime );
}

/**
 * Cleans a timed-out transfer
 */
static int transfer_cleanup(const char * backend_path)
{
    char xfer_path[RBH_PATH_MAX];
    int rc;
    sprintf(xfer_path, "%s.%s", backend_path, COPY_EXT );

    if ( unlink(xfer_path) != 0 )
    {
        rc = -errno;
        return rc;
    }
    return 0;
}

/**
 * Move an orphan file to orphan directory
 */
static int move_orphan(const char * path)
{
    char dest[RBH_PATH_MAX];
    char tmp[RBH_PATH_MAX];
    char * fname;
    int rc;

    /* does the trash directory exist? */
    sprintf( dest, "%s/%s", config.root, TRASH_DIR );
    if ( (mkdir(dest, 0750) != 0) && (errno != EEXIST) )
    {
        rc = -errno;
        DisplayLog( LVL_MAJOR, RBHEXT_TAG, "Error creating directory %s: %s",
                    dest, strerror(-rc) );
        return rc;
    }

    strcpy(tmp, path);
    fname = basename(tmp);
    if ( fname == NULL || (strcmp(fname, "/") == 0) || EMPTY_STRING(fname) )
    {
        DisplayLog( LVL_MAJOR, RBHEXT_TAG, "Invalid path '%s'",
                    path );
        return -EINVAL;
    }
    /* move the orphan to the directory */
    sprintf( dest, "%s/%s/%s", config.root, TRASH_DIR, fname );

    if ( rename(path, dest) != 0 )
    {
        rc = -errno;
        DisplayLog( LVL_MAJOR, RBHEXT_TAG, "Error moving '%s' to '%s'",
                    path, dest );
        return rc;
    }

    DisplayLog( LVL_EVENT, RBHEXT_TAG, "'%s' moved to '%s'",
                path, dest );
    return 0;
}


/* check if there is a running copy and if it timed-out
 * return <0 on error
 * 0 if no copy is running
 * 1 if a copy is already running
 * */
int check_running_copy(const char * bkpath)
{
    int rc;
    /* is a copy running for this entry? */
    rc = entry_is_archiving( bkpath );
    if ( rc < 0 )
    {
        DisplayLog( LVL_MAJOR, RBHEXT_TAG, "Error %d checking if copy is running for %s: %s",
                    rc, bkpath, strerror(-rc) );
        return rc;
    }
    else if ( rc > 0 )
    {
        if ( config.copy_timeout && ( time(NULL) - rc > config.copy_timeout ))
        {
            DisplayLog( LVL_EVENT, RBHEXT_TAG, "Copy timed out for %s (inactive for %us)",
                        bkpath, (unsigned int)(time(NULL) - rc) );
            /* previous copy timed out: clean it */
            transfer_cleanup( bkpath );
        }
        else
        {
            DisplayLog( LVL_DEBUG, RBHEXT_TAG,
                        "'%s' is being archived (last mod: %us ago)",
                        bkpath, (unsigned int)(time(NULL) - rc) );
            return 1;
        }
    }
    return 0;
}


/**
 * Get the status for an entry.
 * \param[in] p_id pointer to entry id
 * \param[in] p_attrs_in pointer to entry attributes
 * \param[out] p_attrs_changed changed/retrieved attributes
 */
int rbhext_get_status( const entry_id_t * p_id,
                       const attr_set_t * p_attrs_in,
                       attr_set_t * p_attrs_changed )
{
    int rc;
    struct stat bkmd;
    obj_type_t entry_type;
    char bkpath[RBH_PATH_MAX];

    /* check if mtime is provided (mandatory) */
    if ( !ATTR_MASK_TEST(p_attrs_in, last_mod) || !ATTR_MASK_TEST(p_attrs_in, type) )
    {
        DisplayLog( LVL_MAJOR, RBHEXT_TAG, "Missing mandatory attribute for checking entry status" );
        return -EINVAL;
    }

    /* path to lookup the entry in the backend */
    rc = entry2backend_path( p_id, p_attrs_in, FOR_LOOKUP, bkpath );
    if (rc)
    {
        DisplayLog( LVL_MAJOR, RBHEXT_TAG, "Error %d building backend path: %s",
                    rc, strerror(-rc) );
        return rc;
    }

    /* is the entry has a supported type? */
    entry_type = ListMgr2PolicyType(ATTR(p_attrs_in, type));
    if ( (entry_type != TYPE_FILE) && (entry_type != TYPE_LINK) )
    {
        DisplayLog( LVL_VERB, RBHEXT_TAG, "Unsupported type %s for this backend",
                    ATTR(p_attrs_in, type) );
        return -ENOTSUP;
    }

#ifdef HAVE_PURGE_POLICY
#ifdef HAVE_SHOOK
    /* @TODO: ignore shook special entries */

    /* check status from libshook.
     * return if status != ONLINE
     * else, continue checking.
     */
    char fidpath[RBH_PATH_MAX];
    file_status_t status;

    BuildFidPath( p_id, fidpath );

    rc = ShookGetStatus( fidpath, &status );
    if (rc)
        return rc;

    /* if status is 'release_pending' or 'restore_running',
     * check timeout. */
    if (status == STATUS_RELEASE_PENDING || status == STATUS_RESTORE_RUNNING)
    {
        rc = ShookRecoverById(p_id, &status);
        if (rc < 0)
            return rc;
    }

    if ( status != STATUS_SYNCHRO )
    {
        DisplayLog( LVL_FULL, RBHEXT_TAG, "shook reported status<>online: %d",
                    status );
        ATTR_MASK_SET( p_attrs_changed, status );
        ATTR( p_attrs_changed, status ) = status;

        /* set backend path if it is not known */
        if (!ATTR_MASK_TEST(p_attrs_in, backendpath)
            && !ATTR_MASK_TEST(p_attrs_changed, backendpath))
        {
            ATTR_MASK_SET(p_attrs_changed, backendpath);
            strcpy(ATTR(p_attrs_changed, backendpath), bkpath);
        }

        return 0;
    }
    /* else: must compare status with backend */
#else
    #error "Unexpected compilation case"
#endif
#endif

    if ( entry_type == TYPE_FILE )
    {
        /* is a copy running for this entry? */
        rc = check_running_copy(bkpath);
        if (rc < 0)
            return rc;
        else if (rc > 0)/* current archive */
        {
            ATTR_MASK_SET( p_attrs_changed, status );
            ATTR( p_attrs_changed, status ) = STATUS_ARCHIVE_RUNNING;
            return 0;
        }
    }

    /* get entry info */
    if ( lstat( bkpath, &bkmd ) != 0 )
    {
        rc = -errno;
        if ( (rc != -ENOENT) && (rc != -ESTALE) )
        {
            DisplayLog( LVL_MAJOR, RBHEXT_TAG, "Lookup error for path '%s': %s",
                        bkpath, strerror(-rc) );
            return rc;
        }
        else
        {
            DisplayLog( LVL_DEBUG, RBHEXT_TAG,
                        "'%s' does not exist in the backend (new entry): %s",
                        bkpath, strerror(-rc) );
            /* no entry in the backend: new entry */
            ATTR_MASK_SET( p_attrs_changed, status );
            ATTR( p_attrs_changed, status ) = STATUS_NEW;
            return 0;
        }
    }

    if ( entry_type == TYPE_FILE )
    {
        if ( !S_ISREG(bkmd.st_mode))
        {
            /* entry of invalid type */
            DisplayLog( LVL_MAJOR, RBHEXT_TAG,
                        "Different type in backend for entry %s. Moving it to orphan dir.",
                        bkpath );
            rc = move_orphan( bkpath );
            if (rc)
                return rc;
            ATTR_MASK_SET( p_attrs_changed, status );
            ATTR( p_attrs_changed, status ) = STATUS_NEW;
            return 0;
        }
        /* compare mtime and size to check if the entry changed */
        /* XXX consider it modified this even if mtime is smaller */
        if ( (ATTR( p_attrs_in, last_mod ) != bkmd.st_mtime )
             || (ATTR( p_attrs_in, size ) != bkmd.st_size ) )
        {
                /* display a warning if last_mod in FS < mtime in backend */
                if (ATTR( p_attrs_in, last_mod ) < bkmd.st_mtime)
                    DisplayLog(LVL_MAJOR, RBHEXT_TAG,
                               "Warning: mtime in filesystem < mtime in backend (%s)",
                               bkpath);

                ATTR_MASK_SET( p_attrs_changed, status );
                ATTR( p_attrs_changed, status ) = STATUS_MODIFIED;

                /* update path in the backend */
                ATTR_MASK_SET( p_attrs_changed, backendpath );
                strcpy( ATTR( p_attrs_changed, backendpath ), bkpath) ;
                return 0;
        }
        else
        {
                ATTR_MASK_SET( p_attrs_changed, status );
                ATTR( p_attrs_changed, status ) = STATUS_SYNCHRO;

                /* update path in the backend */
                ATTR_MASK_SET( p_attrs_changed, backendpath );
                strcpy( ATTR( p_attrs_changed, backendpath ), bkpath) ;
                return 0;
        }
    }
    else if ( entry_type == TYPE_LINK )
    {
        char lnk1[RBH_PATH_MAX];
        char lnk2[RBH_PATH_MAX];
        char fspath[RBH_PATH_MAX];

        if ( !S_ISLNK(bkmd.st_mode))
        {
            DisplayLog( LVL_MAJOR, RBHEXT_TAG,
                        "Different type in backend for entry %s. Moving it to orphan dir.",
                        bkpath );
            rc = move_orphan( bkpath );
            if (rc)
                return rc;
            ATTR_MASK_SET( p_attrs_changed, status );
            ATTR( p_attrs_changed, status ) = STATUS_NEW;
            return 0;
        }

#ifdef _HAVE_FID
        /* for Lustre 2, use fid path so the operation is not disturbed by renames... */
        BuildFidPath( p_id, fspath );
#else
        /* we need the posix path */
        if ( !ATTR_MASK_TEST(p_attrs, fullpath) )
        {
            DisplayLog( LVL_CRIT, RBHEXT_TAG, "Error in %s(): path argument is mandatory for archive command",
                        __FUNCTION__ );
            return -EINVAL;
        }
        strcpy(fspath, ATTR(p_attrs, fullpath));
#endif

        /* compare symlink content */
        if ( (rc = readlink(bkpath, lnk1, RBH_PATH_MAX )) < 0 )
        {
            rc = -errno;
            if ( rc == ENOENT )
            {
                /* entry disapeared */
                ATTR_MASK_SET( p_attrs_changed, status );
                ATTR( p_attrs_changed, status ) = STATUS_NEW;
                return 0;
            }
            else
                return rc;
        }
        lnk1[rc] = '\0';
        DisplayLog( LVL_FULL, RBHEXT_TAG, "backend symlink => %s", lnk1 );
        if ( (rc = readlink(fspath, lnk2, RBH_PATH_MAX )) < 0 )
        {
            rc = -errno;
            DisplayLog( LVL_EVENT, RBHEXT_TAG, "Error performing readlink(%s): %s",
                        fspath, strerror(-rc) );
            return rc;
        }
        lnk2[rc] = '\0';
        DisplayLog( LVL_FULL, RBHEXT_TAG, "FS symlink => %s", lnk2 );
        if ( strcmp(lnk1, lnk2) )
        {
            /* symlink content is different */
            ATTR_MASK_SET( p_attrs_changed, status );
            ATTR( p_attrs_changed, status ) = STATUS_MODIFIED;

            /* update path in the backend */
            ATTR_MASK_SET( p_attrs_changed, backendpath );
            strcpy( ATTR( p_attrs_changed, backendpath ), bkpath ) ;
            return 0;
        }
        else /* same content */
        {
            ATTR_MASK_SET( p_attrs_changed, status );
            ATTR( p_attrs_changed, status ) = STATUS_SYNCHRO;

            /* update path in the backend */
            ATTR_MASK_SET( p_attrs_changed, backendpath );
            strcpy( ATTR( p_attrs_changed, backendpath ), bkpath ) ;
            return 0;
        }
    }
    else
    {
        return -ENOTSUP;
    }

    /* TODO What about STATUS_REMOVED? */
}

typedef enum { TO_FS, TO_BACKEND } target_e;

/**
 * get metadata of a directory in filesystem or in backend
 * by target path
 */
static int get_orig_dir_md( const char * target_dir, struct stat * st,
                            target_e target )
{
    char rel_path[RBH_PATH_MAX];
    char orig_path[RBH_PATH_MAX];
    int rc;
    const char * dest_root;
    const char * src_root;

    if ( target == TO_BACKEND )
    {
        dest_root = config.root;
        src_root = global_config.fs_path;
    }
    else
    {
        dest_root = global_config.fs_path;
        src_root = config.root;
    }

    rc =  relative_path( target_dir, dest_root, rel_path );
    if (rc)
        return rc;

    /* orig path is '<fs_root>/<rel_path>' */
    sprintf(orig_path, "%s/%s", src_root, rel_path);

    DisplayLog( LVL_FULL, RBHEXT_TAG, "Target directory: %s, source directory: %s",
                target_dir, orig_path );

    if ( lstat(orig_path, st) )
    {
        rc = -errno;
        DisplayLog( LVL_DEBUG, RBHEXT_TAG, "Cannot stat %s: %s",
                    orig_path, strerror(-rc) );
        return rc;
    }
    else
        return 0;
}

/**
 *  Ensure POSIX directory exists
 */
static int mkdir_recurse( const char * full_path, mode_t default_mode,
                          target_e target )
{
    char path_copy[MAXPATHLEN];
    const char * curr;
    struct stat st;
    mode_t mode;
    int rc;
    int setattrs = FALSE;

    /* to backend or the other way? */
    if ( target == TO_BACKEND )
    {
        if ( strncmp(config.root,full_path, strlen(config.root)) != 0 )
        {
            DisplayLog( LVL_MAJOR, RBHEXT_TAG, "Error: '%s' in not under backend root '%s'",
                        full_path, config.root );
            return -EINVAL;
        }
        /* skip backend root */
        curr = full_path + strlen(config.root);
    }
    else
    {
        if ( strncmp(global_config.fs_path,full_path, strlen(global_config.fs_path)) != 0 )
        {
            DisplayLog( LVL_MAJOR, RBHEXT_TAG, "Error: '%s' in not under filesystem root '%s'",
                        full_path, global_config.fs_path );
            return -EINVAL;
        }
        /* skip fs root */
        curr = full_path + strlen(global_config.fs_path);
    }

    if ( *curr == '\0' ) /* full_path is root dir */
        return 0;
    else if ( *curr != '/' ) /* slash expected */
    {
        DisplayLog( LVL_MAJOR, RBHEXT_TAG, "Error: '%s' in not under backend root '%s'",
                    full_path, (target == TO_BACKEND)?config.root:global_config.fs_path );
        return -EINVAL;
    }

    /* skip first slash */
    curr ++;

    while( (curr = strchr( curr, '/' )) != NULL )
    {
         /* if fullpath = '/a/b',
         * curr = &(fullpath[2]);
         * so, copy 2 chars to get '/a'.
         * and set fullpath[2] = '\0'
         */
        int path_len = curr - full_path;

        /* extract directory name */
        strncpy( path_copy, full_path, path_len );
        path_copy[path_len]='\0';

        /* stat dir */
        if ( lstat( path_copy, &st ) != 0 )
        {
            rc = -errno;
            if (rc != -ENOENT)
            {
                DisplayLog( LVL_CRIT, RBHEXT_TAG, "Cannot lstat() '%s': %s", path_copy, strerror(-rc) );
                return rc;
            }

            if (get_orig_dir_md(path_copy, &st, target) == 0)
            {
                mode = st.st_mode & 07777;
                setattrs = TRUE;
            }
            else
            {
                mode = default_mode;
                setattrs = FALSE;
            }

            DisplayLog(LVL_FULL, RBHEXT_TAG, "mkdir(%s)", path_copy );
            if ( (mkdir( path_copy, mode ) != 0) && (errno != EEXIST) )
            {
                rc = -errno;
                DisplayLog( LVL_CRIT, RBHEXT_TAG, "mkdir(%s) failed: %s",
                            path_copy, strerror(-rc) );
                return rc;
            }

            if ( setattrs )
            {
                /* set owner and group */
                if ( lchown( path_copy, st.st_uid, st.st_gid ) )
                    DisplayLog( LVL_MAJOR, RBHEXT_TAG,
                                "Error setting owner/group for '%s': %s",
                                path_copy, strerror(errno) );
            }
        }
        else if ( !S_ISDIR( st.st_mode ) )
        {
            DisplayLog( LVL_CRIT, RBHEXT_TAG,
                        "Cannot create directory '%s': existing non-directory",
                        path_copy );
            return -ENOTDIR;
        }

        curr++;
    }

    if (get_orig_dir_md(full_path, &st, target) == 0)
    {
        mode = st.st_mode & 07777;
        setattrs = TRUE;
    }
    else
    {
        mode = default_mode;
        setattrs = FALSE;
    }

    /* finaly create this dir */
    DisplayLog(LVL_FULL, RBHEXT_TAG, "mkdir(%s)", full_path );
    if ( (mkdir( full_path, mode ) != 0) && (errno != EEXIST) )
    {
        rc = -errno;
        DisplayLog( LVL_CRIT, RBHEXT_TAG, "mkdir(%s) failed: %s", full_path, strerror(-rc) );
        return rc;
    } else if (setattrs) {
        /* set owner and group */
        if ( lchown( full_path, st.st_uid, st.st_gid ) )
            DisplayLog( LVL_MAJOR, RBHEXT_TAG, "Error setting owner/group for '%s': %s",
                        full_path, strerror(errno) );
    }

    return 0;
}


/**
 * Performs an archiving operation.
 * \param[in] arch_meth archiving method (sync or async)
 * \param[in] p_id pointer to id of entry to be archived
 * \param[in,out] p_attrs pointer to entry attributes
 *        function must update at least the entry status
 *        and the path in the backend.
 */
int rbhext_archive( rbhext_arch_meth arch_meth,
                    const entry_id_t * p_id,
                    attr_set_t * p_attrs,
                    const char * hints )
{
    int rc;
    char bkpath[RBH_PATH_MAX];
    char fspath[RBH_PATH_MAX];
    char tmp[RBH_PATH_MAX];
    char * destdir;
    struct stat info;
    struct stat void_stat;
    int check_moved = FALSE;
    obj_type_t entry_type;

    if ( arch_meth != RBHEXT_SYNC )
        return -ENOTSUP;

    /* if status is not determined, retrieve it */
    if ( !ATTR_MASK_TEST(p_attrs, status) )
    {
        DisplayLog( LVL_DEBUG, RBHEXT_TAG, "Status not provided to rbhext_archive()" );
        rc = rbhext_get_status( p_id, p_attrs, p_attrs );
        if (rc)
            return rc;
    }

    /* is it the good type? */
    if ( !ATTR_MASK_TEST(p_attrs, type) )
    {
        DisplayLog( LVL_MAJOR, RBHEXT_TAG, "Missing mandatory attribute 'type' in %s()",
                    __FUNCTION__ );
        return -EINVAL;
    }

    entry_type = ListMgr2PolicyType(ATTR(p_attrs, type));
    if ( (entry_type != TYPE_FILE) && (entry_type != TYPE_LINK) )
    {
        DisplayLog( LVL_MAJOR, RBHEXT_TAG, "Unsupported type for archive operation: %s",
                    ATTR(p_attrs, type) );
        return -ENOTSUP;
    }

    /* compute path for target file */
    rc = entry2backend_path( p_id, p_attrs, FOR_NEW_COPY, bkpath );
    if (rc)
    {
        DisplayLog( LVL_MAJOR, RBHEXT_TAG, "Error %d building backend path: %s",
                    rc, strerror(-rc) );
        return rc;
    }

    /* check the status */
    if ( ATTR(p_attrs, status) == STATUS_NEW )
    {
        /* check the entry does not already exist */
        if ( (lstat(bkpath, &void_stat) == 0) || (errno != ENOENT) )
        {
            rc = -errno;
            DisplayLog( LVL_MAJOR, RBHEXT_TAG, "Error: new entry %s already exist. errno=%d, %s",
                        bkpath, -rc, strerror(-rc) );
            return rc;
        }
    }
    else if ( ATTR(p_attrs, status) == STATUS_MODIFIED
             || ATTR(p_attrs, status) == STATUS_ARCHIVE_RUNNING ) /* for timed out copies.. or ourselves! */
     {
        /* chexck if somebody else is about to copy */
        rc = check_running_copy(bkpath);
        if (rc < 0)
            return rc;
        else if (rc > 0)/* current archive */
            return -EALREADY;

       /* check that previous path exists */
        if ( ATTR_MASK_TEST(p_attrs, backendpath) )
        {
            /* need to check if the entry was renamed */
            check_moved = TRUE;
            if ( lstat(ATTR(p_attrs,backendpath), &void_stat) != 0 )
            {
                rc = -errno;
                DisplayLog( LVL_MAJOR, RBHEXT_TAG, "Warning: previous copy %s not found in backend (errno=%d, %s): archiving anyway.",
                            ATTR(p_attrs,backendpath) , -rc, strerror(-rc) );
            }
        }
    }
    else /* invalid status */
    {
        /* invalid status for performing archive() */
        DisplayLog( LVL_MAJOR, RBHEXT_TAG, "Unexpected status %d for calling %s()",
                     ATTR(p_attrs, status), __FUNCTION__ );
        return -EINVAL;
    }

#ifdef _HAVE_FID
    /* for Lustre 2, use fid path so the operation is not disturbed by renames... */
    BuildFidPath( p_id, fspath );
#else
    /* we need the posix path */
    if ( !ATTR_MASK_TEST(p_attrs, fullpath) )
    {
        DisplayLog( LVL_CRIT, RBHEXT_TAG, "Error in %s(): path argument is mandatory for archive command",
                    __FUNCTION__ );
        return -EINVAL;
    }
    strcpy(fspath, ATTR(p_attrs, fullpath));
#endif

    /* 1) extract dir path */
    strcpy( tmp, bkpath );
    destdir = dirname( tmp );
    if ( destdir == NULL )
    {
        DisplayLog( LVL_CRIT, RBHEXT_TAG, "Error extracting directory path of '%s'",
                    bkpath );
        return -EINVAL; 
    }
    /* 2) create it recursively */
    rc = mkdir_recurse( destdir, 0750, TO_BACKEND );
    if ( rc )
        return rc;

    if ( entry_type == TYPE_FILE )
    {
        /* temporary copy path */
        sprintf( tmp, "%s.%s", bkpath, COPY_EXT );

#ifdef HAVE_SHOOK
        rc = shook_archive_start(get_fsname(), p_id, bkpath);
        if (rc)
        {
            DisplayLog( LVL_CRIT, RBHEXT_TAG, "Failed to initialize transfer: shook_archive_start() returned error %d",
                        rc );
            return rc;
        }
#endif

        /* execute the archive command */
        if ( hints )
            rc = execute_shell_command( config.action_cmd, 4, "ARCHIVE", fspath, tmp, hints);
        else
            rc = execute_shell_command( config.action_cmd, 3, "ARCHIVE", fspath, tmp );

        if (rc)
        {
#ifdef HAVE_SHOOK
            shook_archive_abort(get_fsname(), p_id);
#endif
            /* cleanup tmp copy */
            unlink(tmp);
            /* the transfer failed. still needs to be archived */
            ATTR_MASK_SET( p_attrs, status );
            ATTR( p_attrs, status ) = STATUS_MODIFIED;
            return rc;
        }
        else
        {
            /* finalize tranfer */

            /* owner/group is saved by the copy command */

            /* reset initial mtime */
            if ( ATTR_MASK_TEST( p_attrs, last_mod ) )
            {
                  struct utimbuf tbuf;
                  tbuf.actime = time(NULL);
                  tbuf.modtime = ATTR( p_attrs, last_mod );

                if (utime(tmp, &tbuf) != 0)
                {
                    rc = -errno;
                    DisplayLog( LVL_CRIT, RBHEXT_TAG, "Error setting mtime for file %s: %s",
                                tmp, strerror(-rc) );
                    /* ignore the error */
                    rc = 0;
                }
            }

            /* move entry to final path */
            if (rename(tmp, bkpath) != 0 )
            {
                rc = -errno;
                DisplayLog( LVL_CRIT, RBHEXT_TAG, "Error renaming tmp copy file '%s' to final name '%s': %s",
                            tmp, bkpath, strerror(-rc) );

                /* the transfer failed. still needs to be archived */
                ATTR_MASK_SET( p_attrs, status );
                ATTR( p_attrs, status ) = STATUS_MODIFIED;
                return rc;
            }

            /* did the file been renamed since last copy? */
            if ( check_moved && strcmp( bkpath, ATTR( p_attrs, backendpath ) ))
            {
                DisplayLog( LVL_DEBUG, RBHEXT_TAG, "Removing previous copy %s",
                             ATTR( p_attrs, backendpath ) );
                if ( unlink( ATTR( p_attrs, backendpath ) ))
                {
                    rc = -errno;
                    DisplayLog( LVL_DEBUG, RBHEXT_TAG, "Error removing previous copy %s: %s",
                                ATTR( p_attrs, backendpath ), strerror(-rc) );
                    /* ignore */
                    rc = 0;
                }
            }

            ATTR_MASK_SET( p_attrs, status );
            ATTR( p_attrs, status ) = STATUS_SYNCHRO;

            ATTR_MASK_SET( p_attrs, backendpath );
            strcpy( ATTR( p_attrs, backendpath ), bkpath );

            ATTR_MASK_SET( p_attrs, last_archive );
            ATTR( p_attrs, last_archive) = time(NULL);

#ifdef HAVE_SHOOK
            rc = shook_archive_finalize(get_fsname(), p_id, bkpath);
            if (rc)
            {
                DisplayLog( LVL_CRIT, RBHEXT_TAG, "Failed to finalize transfer: shook_archive_finalize() returned error %d",
                            rc );
                return rc;
           }
#endif
        }

        if ( lstat(fspath, &info) != 0 )
        {
            rc = -errno;
            DisplayLog( LVL_EVENT, RBHEXT_TAG, "Error performing final lstat(%s): %s",
                        fspath, strerror(-rc) );
            ATTR_MASK_SET( p_attrs, status );
            ATTR( p_attrs, status ) = STATUS_UNKNOWN;
        }
        else
        {
            if ( (info.st_mtime != ATTR( p_attrs, last_mod ))
                 || (info.st_size != ATTR( p_attrs, size )) )
            {
                DisplayLog( LVL_EVENT, RBHEXT_TAG, "Entry %s has been modified during transfer: "
                            "size before/after: %"PRI_SZ"/%"PRI_SZ", "
                            "mtime before/after: %u/%"PRI_TT,
                            fspath, ATTR( p_attrs, size ), info.st_size,
                            ATTR( p_attrs, last_mod ), info.st_mtime );
                ATTR_MASK_SET( p_attrs, status );
                ATTR( p_attrs, status ) = STATUS_MODIFIED;
            }

            /* update entry attributes */
            PosixStat2EntryAttr( &info, p_attrs, TRUE );
        }
    }
    else if ( entry_type == TYPE_LINK )
    {
        char link[RBH_PATH_MAX] = "";

        /* read link content from filesystem */
        if ( readlink(fspath, link, RBH_PATH_MAX) < 0 )
        {
            rc = -errno;
            DisplayLog( LVL_MAJOR,  RBHEXT_TAG, "Error reading symlink content (%s): %s",
                        fspath, strerror(-rc) );
            return rc;
        }
        /* link content is not supposed to change during its lifetime */
        if ( symlink(link, bkpath) != 0 )
        {
            rc = -errno;
            DisplayLog( LVL_MAJOR,  RBHEXT_TAG, "Error creating symlink %s->\"%s\" in backend: %s",
                        bkpath, link, strerror(-rc) );
            return rc;
        }

        ATTR_MASK_SET( p_attrs, status );
        ATTR( p_attrs, status ) = STATUS_SYNCHRO;

        /* set symlink owner/group */
        if ( lstat(fspath, &info) != 0 )
        {
            rc = -errno;
            DisplayLog( LVL_EVENT, RBHEXT_TAG, "Error performing final lstat(%s): %s",
                        fspath, strerror(-rc) );
            ATTR_MASK_SET( p_attrs, status );
            ATTR( p_attrs, status ) = STATUS_UNKNOWN;
        }
        else
        {
            if (lchown(bkpath, info.st_uid, info.st_gid))
            {
                DisplayLog( LVL_EVENT, RBHEXT_TAG, "error setting owner/group in backend on %s: %s",
                            bkpath, strerror(-rc) );
            }
        }

        ATTR_MASK_SET( p_attrs, backendpath );
        strcpy( ATTR( p_attrs, backendpath ), bkpath );

        ATTR_MASK_SET( p_attrs, last_archive );
        ATTR( p_attrs, last_archive) = time(NULL);
    }

    return 0;
}

/**
 * Performs entry removal in the backend
 * \param[in] p_id pointer to id of entry to be archived
 * \param[in,out] p_attrs pointer to entry attributes
 *                        must be updated even on failure
 * \retval  -ENOENT entry not in backend
 * \retval  -EINVAL empty path provided
 */
int rbhext_remove( const entry_id_t * p_id, const char * backend_path )
{
    int rc;
    if ( backend_path && !EMPTY_STRING(backend_path) )
    {
        if ( unlink(backend_path) != 0 )
        {
            rc = -errno;
            if ( rc == -ENOENT )
            {
                DisplayLog( LVL_DEBUG, RBHEXT_TAG, "'%s' not found in backend",
                            backend_path );
                return rc;
            }
            else
            {
                DisplayLog( LVL_EVENT, RBHEXT_TAG, "Error removing '%s' from backend: %s",
                            backend_path, strerror(-rc) );
                return rc;
            }
        }
    }
    else
        return -EINVAL;
    return 0;
}

/** recover a file from the backend after formatting FS
 * \retval recovery status
 */
recov_status_t rbhext_recover( const entry_id_t * p_old_id,
                               attr_set_t * p_attrs_old,
                               entry_id_t * p_new_id,
                               attr_set_t * p_attrs_new,
                               struct stat * bkinfo )
{
    char bkpath[RBH_PATH_MAX];
    const char * backend_path;
    const char * fspath;
    char tmp[RBH_PATH_MAX];
    char * destdir;
    int rc;
    struct stat st_bk;
    struct stat st_dest;
    int delta = FALSE;
    attr_set_t attr_bk;
    int fd;
    entry_id_t  parent_id;

    if ( !ATTR_MASK_TEST( p_attrs_old, fullpath ) )
    {
        DisplayLog( LVL_MAJOR, RBHEXT_TAG, "Missing mandatory attribute 'fullpath' for restoring entry "DFID, PFID(p_old_id) );
        return RS_ERROR;
    }
    fspath = ATTR( p_attrs_old, fullpath );

    /* if there is no backend path, try to guess */
    if ( !ATTR_MASK_TEST( p_attrs_old, backendpath) )
    {
        rc = entry2backend_path( p_old_id, p_attrs_old, FOR_LOOKUP, bkpath );
        if ( rc == 0 )
        {
            DisplayLog( LVL_EVENT, RBHEXT_TAG,
                        "No backend path is set for '%s', guess it could be '%s'",
                        fspath, bkpath );
            backend_path = bkpath;
        }
        else
        {
            DisplayLog( LVL_MAJOR, RBHEXT_TAG,
                        "Cannot determine backend path for '%s'",
                        fspath );
            return RS_ERROR;
        }
    }
    else
        backend_path = ATTR( p_attrs_old, backendpath );

    /* if the entry is a directory, create it in filesystem and set its atributes from DB */
    if (ATTR_MASK_TEST(p_attrs_old, type) && 
        !strcasecmp(ATTR(p_attrs_old, type), STR_TYPE_DIR))
    {
        rc = mkdir_recurse(fspath, 0750, TO_FS);
        if (rc)
            return RS_ERROR;

        if (bkinfo)
        {
            /* set the same mode as in the backend */
            DisplayLog( LVL_FULL, RBHEXT_TAG, "Restoring mode for '%s': mode=%#o",
                        fspath, bkinfo->st_mode & 07777 );
            if ( chmod( fspath, bkinfo->st_mode & 07777 ) )
                DisplayLog( LVL_MAJOR, RBHEXT_TAG, "Warning: couldn't restore mode for '%s': %s",
                            fspath, strerror(errno) );
        }

        /* extract dir path */
        strcpy( tmp, fspath );
        destdir = dirname( tmp );
        if ( destdir == NULL )
        {
            DisplayLog( LVL_CRIT, RBHEXT_TAG, "Error extracting directory path of '%s'",
                        fspath );
            return RS_ERROR;
        }

        /* retrieve parent fid */
#ifdef _HAVE_FID
        rc = Lustre_GetFidFromPath( destdir, &parent_id );
        if (rc)
            return RS_ERROR;
#else
        if (lstat(destdir, &parent_stat))
        {
            rc = errno;
            DisplayLog( LVL_CRIT, RBHEXT_TAG, "ERROR: cannot stat target directory '%s': %s",
                        destdir, strerror(rc) );
            return RS_ERROR;
        }
        /* build id from dev/inode*/
        parent_id.inode = parent_stat.st_ino;
        parent_id.device = parent_stat.st_dev;
        parent_id.validator = parent_stat.st_ctime;
#endif
    }
    else /* non directory */
    {
        /* test if this copy exists */
        if (bkinfo)
            st_bk=*bkinfo;
        else if ( lstat( backend_path, &st_bk ) != 0 )
        {
            rc = -errno;
            DisplayLog( LVL_MAJOR, RBHEXT_TAG, "Cannot stat '%s' in backend: %s",
                        backend_path, strerror(-rc) );
            if (rc == -ENOENT )
                return RS_NOBACKUP;
            else
                return RS_ERROR;
        }

        ATTR_MASK_INIT( &attr_bk );
        /* merge missing posix attrs to p_attrs_old */
        PosixStat2EntryAttr( &st_bk, &attr_bk, TRUE );
        /* leave attrs unchanged if they are already set in p_attrs_old */
        ListMgr_MergeAttrSets( p_attrs_old, &attr_bk, FALSE );

        /* test if the target does not already exist */
        rc = lstat( ATTR(p_attrs_old, fullpath), &st_dest );
        if ( rc == 0 )
        {
            DisplayLog( LVL_MAJOR, RBHEXT_TAG, "Error: cannot recover '%s': already exists",
                        fspath );
            return RS_ERROR;
        }
        else if ( (rc = -errno) != -ENOENT )
        {
            DisplayLog( LVL_MAJOR, RBHEXT_TAG, "Unexpected error performing lstat(%s): %s",
                        fspath, strerror(-rc) );
            return RS_ERROR;
        }

        /* check that this is not a cross-device import or recovery (entry could not be moved
         * in that case) */
        if (config.check_mounted && (backend_dev != st_bk.st_dev))
        {
            DisplayLog( LVL_MAJOR, RBHEXT_TAG, "Source file %s is not in the same device as target %s",
                        backend_path, config.root );
            return RS_ERROR;
        }

        /* recursively create the parent directory */
        /* extract dir path */
        strcpy( tmp, fspath );
        destdir = dirname( tmp );
        if ( destdir == NULL )
        {
            DisplayLog( LVL_CRIT, RBHEXT_TAG, "Error extracting directory path of '%s'",
                        fspath );
            return RS_ERROR;
        }

        rc = mkdir_recurse( destdir, 0750, TO_FS );
        if (rc)
            return RS_ERROR;

        /* retrieve parent fid */
#ifdef _HAVE_FID
        rc = Lustre_GetFidFromPath( destdir, &parent_id );
        if (rc)
            return RS_ERROR;
#else
        if (lstat(destdir, &parent_stat))
        {
            rc = errno;
            DisplayLog( LVL_CRIT, RBHEXT_TAG, "ERROR: cannot stat target directory '%s': %s",
                        destdir, strerror(rc) );
            return RS_ERROR;
        }
        /* build id from dev/inode*/
        parent_id.inode = parent_stat.st_ino;
        parent_id.device = parent_stat.st_dev;
        parent_id.validator = parent_stat.st_ctime;
#endif

        /* restore FILE entry */
        if ( S_ISREG( st_bk.st_mode ) )
        {
            struct utimbuf utb;

    #ifdef _LUSTRE
            /* restripe the file in Lustre */
            if ( ATTR_MASK_TEST( p_attrs_old, stripe_info ) )
                File_CreateSetStripe( fspath, &ATTR( p_attrs_old, stripe_info ) );
            else {
    #endif
            fd = creat( fspath, st_bk.st_mode & 07777 );
            if (fd < 0)
            {
                rc = -errno;
                DisplayLog( LVL_CRIT, RBHEXT_TAG, "ERROR: couldn't create '%s': %s",
                            fspath, strerror(-rc) );
                return RS_ERROR;
            }
            else
                close(fd);
    #ifdef _LUSTRE
            }
    #endif


    #ifdef HAVE_PURGE_POLICY
        /* this backend is restore/release capable.
         * Recover the entry in released state (md only),
         * so it will be recovered at first open.
         */
    #ifdef HAVE_SHOOK
            /* set the file in "released" state */
            rc = shook_set_status(fspath, SS_RELEASED);
            if (rc)
            {
                DisplayLog( LVL_CRIT, RBHEXT_TAG, "ERROR setting released state for '%s': %s",
                            fspath, strerror(-rc));
                return RS_ERROR;
            }

            rc = truncate( fspath, st_bk.st_size );
            if (rc)
            {
                DisplayLog( LVL_CRIT, RBHEXT_TAG, "ERROR could not set original size %"PRI_SZ" for '%s': %s",
                            st_bk.st_size, fspath, strerror(-rc));
                return RS_ERROR;
            }
    #else
        #error "Unexpected case"
    #endif
    #else
            /* full restore (even data) */
            rc = execute_shell_command( config.action_cmd, 3, "RESTORE",
                                        backend_path, fspath );
            if (rc)
                return RS_ERROR;
            /* TODO: remove partial copy */
    #endif

            /* set the same mode as in the backend */
            DisplayLog( LVL_FULL, RBHEXT_TAG, "Restoring mode for '%s': mode=%#o",
                        fspath, st_bk.st_mode & 07777 );
            if ( chmod( fspath, st_bk.st_mode & 07777 ) )
                DisplayLog( LVL_MAJOR, RBHEXT_TAG, "Warning: couldn't restore mode for '%s': %s",
                            fspath, strerror(errno) );

            /* set the same mtime as in the backend */
            DisplayLog( LVL_FULL, RBHEXT_TAG, "Restoring times for '%s': atime=%lu, mtime=%lu",
                        fspath, st_bk.st_atime, st_bk.st_mtime );
            utb.actime = st_bk.st_atime;
            utb.modtime = st_bk.st_mtime;
            if ( utime( fspath, &utb ) )
                DisplayLog( LVL_MAJOR, RBHEXT_TAG, "Warning: couldn't restore times for '%s': %s",
                            fspath, strerror(errno) );
        }
        else if (S_ISLNK(st_bk.st_mode)) /* restore symlink */
        {
            char link[RBH_PATH_MAX] = "";

            /* read link content from backend */
            if ( readlink(backend_path, link, RBH_PATH_MAX) < 0 )
            {
                rc = -errno;
                DisplayLog( LVL_MAJOR,  RBHEXT_TAG, "Error reading symlink content (%s): %s",
                            backend_path, strerror(-rc) );
                return RS_ERROR;
            }
            /* set it in FS */
            if ( symlink(link, fspath) != 0 )
            {
                rc = -errno;
                DisplayLog( LVL_MAJOR,  RBHEXT_TAG, "Error creating symlink %s->\"%s\" in filesystem: %s",
                            fspath, link, strerror(-rc) );
                return RS_ERROR;
            }
        }
    } /* end if dir/non-dir */

    /* set owner, group */
    if ( ATTR_MASK_TEST( p_attrs_old, owner ) || ATTR_MASK_TEST( p_attrs_old, gr_name ) )
    {
        uid_t uid = -1;
        gid_t gid = -1;
        char buff[4096];

        if ( ATTR_MASK_TEST( p_attrs_old, owner ) )
        {
            struct passwd pw;
            struct passwd * p_pw;

            if ( getpwnam_r( ATTR(p_attrs_old, owner ), &pw, buff, 4096, &p_pw ) != 0 )
            {
                DisplayLog( LVL_MAJOR, RBHEXT_TAG, "Warning: couldn't resolve uid for user '%s'",
                            ATTR(p_attrs_old, owner ));
                uid = -1;
            }
            else
                uid = p_pw->pw_uid;
        }

        if ( ATTR_MASK_TEST( p_attrs_old, gr_name ) )
        {
            struct group gr;
            struct group * p_gr;
            if ( getgrnam_r( ATTR(p_attrs_old, gr_name ), &gr, buff, 4096, &p_gr ) != 0 )
            {
                DisplayLog( LVL_MAJOR, RBHEXT_TAG, "Warning: couldn't resolve gid for group '%s'",
                            ATTR(p_attrs_old, gr_name ) );
                gid = -1;
            }
            else
                gid = p_gr->gr_gid;
        }

        DisplayLog( LVL_FULL, RBHEXT_TAG, "Restoring owner/group for '%s': uid=%u, gid=%u",
                    fspath, uid, gid );

        if ( lchown( fspath, uid, gid ) )
        {
            rc = errno;
            DisplayLog( LVL_MAJOR, RBHEXT_TAG, "Warning: cannot set owner/group for '%s': %s",
                        fspath, strerror(-rc) );
        }
    }

    if ( lstat( fspath, &st_dest ) )
    {
        rc = -errno;
        DisplayLog( LVL_CRIT, RBHEXT_TAG, "ERROR: lstat() failed on restored entry '%s': %s",
                    fspath, strerror(-rc) );
        return RS_ERROR;
    }

    /* compare restored size and mtime with the one saved in the DB (for warning purpose)
     * (not for directories) */
    if (!S_ISDIR(st_dest.st_mode))
    {
        if ( ATTR_MASK_TEST(p_attrs_old, size) && (st_dest.st_size != ATTR(p_attrs_old, size)) )
        {
            DisplayLog( LVL_MAJOR, RBHEXT_TAG, "%s: the restored size (%zu) is "
                        "different from the last known size in filesystem (%"PRIu64"): "
                        "it should have been modified in filesystem after the last backup.",
                        fspath, st_dest.st_size, ATTR(p_attrs_old, size) );
            delta = TRUE;
        }
    }
    /* only for files */
    if (S_ISREG(st_dest.st_mode))
    {
        if ( ATTR_MASK_TEST( p_attrs_old, last_mod) && (st_dest.st_mtime != ATTR(p_attrs_old, last_mod)) )
        {
            DisplayLog( LVL_MAJOR, RBHEXT_TAG, "%s: the restored mtime (%lu) is "
                        "different from the last time in filesystem (%u): "
                        "it may have been modified in filesystem after the last backup.",
                        fspath, st_dest.st_mtime, ATTR(p_attrs_old, last_mod) );
            delta = TRUE;
        }
    }

    /* set the new attributes */
    ATTR_MASK_INIT( p_attrs_new );
    PosixStat2EntryAttr( &st_dest, p_attrs_new, TRUE );
    strcpy( ATTR( p_attrs_new, fullpath ), fspath );
    ATTR_MASK_SET( p_attrs_new, fullpath );
    /* status is always synchro or released after a recovery */
#ifdef HAVE_SHOOK
    /* only files remain released, others are synchro */
    if (S_ISREG(st_dest.st_mode))
        ATTR( p_attrs_new, status ) = STATUS_RELEASED;
    else
        ATTR( p_attrs_new, status ) = STATUS_SYNCHRO;
#else
    ATTR( p_attrs_new, status ) = STATUS_SYNCHRO;
#endif
    ATTR_MASK_SET( p_attrs_new, status );

#ifdef _HAVE_FID
    /* get the new fid */
    rc = Lustre_GetFidFromPath( fspath, p_new_id );
    if (rc)
        return RS_ERROR;
#else
    /* build id from dev/inode*/
    p_new_id->inode =  st_dest.st_ino;
    p_new_id->device =  st_dest.st_dev;
    p_new_id->validator =  st_dest.st_ctime;
#endif

    /* set parent id */
    ATTR_MASK_SET( p_attrs_new, parent_id );
    ATTR( p_attrs_new, parent_id ) = parent_id;

#ifdef _LUSTRE
    if (!ATTR_MASK_TEST( p_attrs_new, type) || !strcmp(ATTR(p_attrs_new, type), STR_TYPE_FILE))
    {
        /* get the new stripe info */
        if ( File_GetStripeByPath( fspath,
                                   &ATTR( p_attrs_new, stripe_info ),
                                   &ATTR( p_attrs_new, stripe_items ) ) == 0 )
        {
            ATTR_MASK_SET( p_attrs_new, stripe_info );
            ATTR_MASK_SET( p_attrs_new, stripe_items );
        }
    }
#endif

    if (!S_ISDIR(st_dest.st_mode))
    {
        /* set the new entry path in backend, according to the new fid */
        rc = entry2backend_path( p_new_id, p_attrs_new,
                                 FOR_NEW_COPY,
                                 ATTR(p_attrs_new, backendpath ) );
        if (rc)
            return RS_ERROR;
        ATTR_MASK_SET( p_attrs_new, backendpath );

        /* recursively create the parent directory */
        /* extract dir path */
        strcpy( tmp, ATTR(p_attrs_new, backendpath) );
        destdir = dirname( tmp );
        if ( destdir == NULL )
        {
            DisplayLog( LVL_CRIT, RBHEXT_TAG, "Error extracting directory path of '%s'",
                        ATTR(p_attrs_new, backendpath) );
            return -EINVAL;
        }

        rc = mkdir_recurse( destdir, 0750, TO_BACKEND );
        if (rc)
            return RS_ERROR;

        /* rename the entry in backend */
        if ( strcmp( ATTR(p_attrs_new, backendpath), backend_path ) != 0 )
        {
            DisplayLog( LVL_DEBUG, RBHEXT_TAG, "Moving the entry in backend: '%s'->'%s'",
                        backend_path, ATTR(p_attrs_new, backendpath) );
            if ( rename( backend_path, ATTR(p_attrs_new, backendpath) ) )
            {
                rc = -errno;
                DisplayLog( LVL_MAJOR, RBHEXT_TAG, "Could not move entry in backend ('%s'->'%s'): %s",
                            backend_path, ATTR(p_attrs_new, backendpath), strerror(-rc) );
                /* keep the old path */
                strcpy( ATTR(p_attrs_new, backendpath), backend_path );
            }
        }

#ifdef HAVE_SHOOK
        /* save new backendpath to filesystem */
        /* XXX for now, don't manage several hsm_index */
        rc = shook_set_hsm_info( fspath, ATTR(p_attrs_new, backendpath), 0 );
        if (rc)
            DisplayLog( LVL_MAJOR, RBHEXT_TAG, "Could not set backend path for %s: error %d",
                        fspath, rc );
#endif
    }

    if (delta)
        return RS_DELTA;
    else
        return RS_OK;
}


/* rebind a backend entry to a new file in Lustre (with new fid)
 * Notice: fs_path is not necessarily the current path of new_id
 * but it should be moved to this path in the end.
 */
int rbhext_rebind(const char *fs_path, const char *old_bk_path,
                  char *new_bk_path, entry_id_t *new_id)
{
    int rc;
    attr_set_t attrs_new;
    struct stat st;
    char tmp[RBH_PATH_MAX];
    char fidpath[RBH_PATH_MAX];
    char * destdir;

    BuildFidPath( new_id, fidpath );

    if (lstat(fidpath, &st))
    {
        rc = -errno;
        DisplayLog(LVL_CRIT, RBHEXT_TAG, "ERROR: lstat() failed on target "DFID": %s",
                   PFID(new_id), strerror(-rc));
        return rc;
    }

    if (!S_ISREG(st.st_mode))
    {
        DisplayLog( LVL_MAJOR, RBHEXT_TAG, "%s() is only supported for files", __func__);
        return -ENOTSUP;
    }

    /* build attr struct */
    ATTR_MASK_INIT( &attrs_new );
    PosixStat2EntryAttr( &st, &attrs_new, TRUE );
    strcpy( ATTR( &attrs_new, fullpath ), fs_path );
    ATTR_MASK_SET( &attrs_new, fullpath );

    /* build new path in backend */
    rc = entry2backend_path(new_id, &attrs_new, FOR_NEW_COPY, new_bk_path);
    if (rc)
    {
        DisplayLog( LVL_MAJOR, RBHEXT_TAG, "Error building backend path for %s: %s",
                    fs_path, strerror(-rc) );
        return rc;
    }

    /* -- move entry from old bk path to the new location -- */

    /* recursively create the parent directory */
    /* extract dir path */
    strcpy(tmp, new_bk_path);
    destdir = dirname(tmp);
    if (destdir == NULL)
    {
        DisplayLog( LVL_CRIT, RBHEXT_TAG, "Error extracting directory path of '%s'",
                    new_bk_path );
        return -EINVAL;
    }

    rc = mkdir_recurse(destdir, 0750, TO_BACKEND);
    if (rc)
        return rc;

    /* rename the entry in backend */
    DisplayLog(LVL_DEBUG, RBHEXT_TAG, "Moving entry in the backend: '%s'->'%s'",
               old_bk_path, new_bk_path);
    if (rename(old_bk_path, new_bk_path))
    {
        rc = -errno;
        DisplayLog( LVL_MAJOR, RBHEXT_TAG, "Could not move entry in the backend ('%s'->'%s'): %s",
                    old_bk_path, new_bk_path, strerror(-rc) );
        /* keep the old path */
        strcpy( new_bk_path, old_bk_path);
        return rc;
    }

#ifdef HAVE_SHOOK
    /* save new backendpath to filesystem */
    /* XXX for now, don't manage several hsm_index */
    rc = shook_set_hsm_info(fidpath, new_bk_path, 0);
    if (rc)
        DisplayLog( LVL_MAJOR, RBHEXT_TAG, "Could not set backend path for "DFID": error %d",
                    PFID(new_id), rc );
#endif

    return rc;
}


int rbhext_release( const entry_id_t * p_id,
                    attr_set_t * p_attrs )
{
#ifndef HAVE_PURGE_POLICY
    return -ENOTSUP;
#else
    int rc;
    obj_type_t entry_type;

    /* if status is not determined, retrieve it */
    if ( !ATTR_MASK_TEST(p_attrs, status) )
    {
        DisplayLog( LVL_DEBUG, RBHEXT_TAG, "Status not provided to rbhext_release()" );
        rc = rbhext_get_status( p_id, p_attrs, p_attrs );
        if (rc)
            return rc;
    }

    /* is it the good type? */
    if ( !ATTR_MASK_TEST(p_attrs, type) )
    {
        DisplayLog( LVL_MAJOR, RBHEXT_TAG, "Missing mandatory attribute 'type' in %s()",
                    __FUNCTION__ );
        return -EINVAL;
    }

    entry_type = ListMgr2PolicyType(ATTR(p_attrs, type));
    if ( entry_type != TYPE_FILE )
    {
        DisplayLog( LVL_MAJOR, RBHEXT_TAG, "Unsupported type for release operation: %s",
                    ATTR(p_attrs, type) );
        return -ENOTSUP;
    }

    return shook_release(get_fsname(), p_id);
#endif
}
