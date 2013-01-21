/* -*- mode: c; c-basic-offset: 4; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=4:tabstop=4:
 */
/*
 * Copyright (C) 2004-2009 CEA/DAM
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

#include "global_config.h"
#include "RobinhoodMisc.h"
#include "RobinhoodLogs.h"
#include "RobinhoodConfig.h"
#include "xplatform_print.h"
#include "uidgidcache.h"
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <stdio.h>
#include <pthread.h>
#include <libgen.h>             /* for dirname */
#include <stdarg.h>
#include <fnmatch.h>

#ifndef HAVE_GETMNTENT_R
#include "mntent_compat.h"
#else
#include <mntent.h>             /* for handling mntent */
#endif


/* Exa-scale definitions ;-) */
#define KILO_BYTE  (1024LL)
#define MEGA_BYTE  (1024LL * KILO_BYTE )
#define GIGA_BYTE  (1024LL * MEGA_BYTE )
#define TERA_BYTE  (1024LL * GIGA_BYTE )
#define PETA_BYTE  (1024LL * TERA_BYTE )
#define EXA_BYTE   (1024LL * PETA_BYTE )

/* Durations  */
#define MINUTE   60
#define HOUR  (60*MINUTE)
#define DAY   (24*HOUR)
#define WEEK  (7*DAY)
#define YEAR  (365*DAY)


void Exit( int error_code )
{
    DisplayLog( LVL_MAJOR, "EXIT", "Exiting program with code %d", error_code );

#if 0
    /* Remove pid_file, if any */
    if ( pid_file != NULL )
    {
        if ( unlink( pid_file ) != 0 )
        {
            DisplayLog( LVL_CRIT, "EXIT",
                        "Could not remove pid file %s: %s", pid_file, strerror( errno ) );
        }
    }
#endif

    FlushLogs(  );
    exit( error_code );
}

/* global info about the filesystem to be managed */
static char    mount_point[RBH_PATH_MAX] = "";
static char    fsname[RBH_PATH_MAX] = "";
static dev_t   dev_id = 0;
static uint64_t fs_key = 0;

/* to optimize string concatenation */
static unsigned int mount_len = 0;


/* used at initialization time, to avoid several modules
 * that start in parallel to check it several times.
 */
static pthread_mutex_t mount_point_lock = PTHREAD_MUTEX_INITIALIZER;

#define LAST_32PRIME    0xFFFFFFFB
#define LAST_64PRIME    0xFFFFFFFFFFFFFFC5
static uint64_t hash_name(const char * str)
{
    unsigned int i;
    uint64_t val = 1;

    for ( i = 0; i < strlen(str); i++ )
        val = ( val << 5 ) - val + (unsigned int) ( str[i] );

    return val % LAST_32PRIME;
}

static uint64_t fsidto64(fsid_t fsid)
{
    uint64_t out;
    if (sizeof(fsid_t) <= sizeof(uint64_t))
    {
        memset(&out, 0, sizeof(out));
        memcpy((&out)+(sizeof(out)-sizeof(fsid_t)), &fsid, sizeof(fsid));
        DisplayLog(LVL_DEBUG, __func__, "sizeof(fsid)=%lu <= 64bits, fsid as 64=%"PRIX64, sizeof(fsid_t), out);
        return out;
    }
    else
    {
        unsigned int i;
        out = 1;
        char * str = (char *)(&fsid);

        for ( i = 0; i < sizeof(fsid_t); i++ )
            out = ( out << 5 ) - out + (unsigned int) ( str[i] );

        out = out % LAST_64PRIME;
        DisplayLog(LVL_DEBUG, __func__, "sizeof(fsid)=%lu > 64bits, hash64(fsid)=%"PRIX64, sizeof(fsid_t), out);
        return out;
    }
}

/* this set of functions is for retrieving/checking mount point
 * and fs name (once for all threads):
 */
static void _set_mount_point( char *mntpnt )
{
    /* cannot change during a run */
    if (mount_len == 0)
    {
        strcpy( mount_point, mntpnt );
        mount_len = strlen( mntpnt );

        /* remove final slash, if any */
        if ( (mount_len > 1) && (mount_point[mount_len-1] == '/') )
        {
            mount_point[mount_len-1] = '\0';
            mount_len --;
        }
    }
}

void set_fs_info( char *name, char * mountp, dev_t dev, fsid_t fsid)
{
    P( mount_point_lock );
    _set_mount_point(mountp);
    strcpy( fsname, name );
    dev_id = dev;

    switch (global_config.fs_key)
    {
        case FSKEY_FSNAME:
            fs_key = hash_name(name);
            DisplayLog(LVL_DEBUG, "FSInfo", "fs_key: hash(fsname)=%"PRIX64, fs_key);
            break;
        case FSKEY_FSID:
            fs_key = fsidto64(fsid);
            DisplayLog(LVL_DEBUG, "FSInfo", "fs_key: fsid as 64=%"PRIX64, fs_key);
            break;
        case FSKEY_DEVID:
            fs_key = dev_id;
            DisplayLog(LVL_DEBUG, "FSInfo", "fs_key: devid=%"PRIX64, fs_key);
            break;
        default:
            DisplayLog(LVL_MAJOR, "FSInfo", "Invalid fs_key type %#x", global_config.fs_key);
            fs_key = 0;
    }
    V( mount_point_lock );
}

/* retrieve the mount point from any module
 * without final slash.
 */
const char          *get_mount_point( unsigned int * plen )
{
    if (plen) (*plen) = mount_len;
    return mount_point;
}

/* retrieve fsname from any module */
const char          *get_fsname(  )
{
    return fsname;
}

/* return Filesystem device id  */
dev_t          get_fsdev()
{
    return dev_id;
}

uint64_t       get_fskey()
{
    return fs_key;
}



/**
 * send a mail
 */
int SendMail( const char *recipient, const char *subject, const char *message )
{
    char           buffer[MAIL_TITLE_MAX];
    FILE          *fichier;

    snprintf( buffer, MAIL_TITLE_MAX, MAIL " -s \"%s\" %s", subject, recipient );

    if ( ( fichier = popen( buffer, "w" ) ) == NULL )
    {
        DisplayLog( LVL_CRIT, "SENDMAIL", "Error %d sending mail with the following command=%s",
                    errno, buffer );
        return -1;
    }
    fwrite( message, strlen( message ), 1, fichier );
    pclose( fichier );
    return 0;
}

/**
 * Search for Robinhood config file.
 * search a config file with the given name (+extension)
 * If cfg_in is empty: search any config in config paths
 * /!\ not thread safe
 */
int SearchConfig( const char * cfg_in, char * cfg_out, int * changed )
{
    static const char * default_cfg_paths[] =
    {
       "/etc/robinhood.d/"PURPOSE_EXT,
       "/etc/robinhood.d",
       "/etc/robinhood",
       ".",
       NULL
    };
    const char * current_path;
    int i;
    DIR * dir;
    struct dirent * ent;
    struct stat stbuf;
    *changed = 1; /* most of the cases */

    if (cfg_in == NULL || EMPTY_STRING(cfg_in))
    {
       for ( i = 0, current_path = default_cfg_paths[0];
             current_path != NULL;
             i++, current_path = default_cfg_paths[i] )
       {
            /* look for files in current path */
            dir = opendir( current_path );
            if ( !dir )
                continue;

            while ( (ent = readdir(dir)) != NULL )
            {
                /* ignore .xxx files */
                if (ent->d_name[0] == '.')
                    continue;
                if (fnmatch("*.conf", ent->d_name, 0) && fnmatch("*.cfg", ent->d_name, 0))
                    /* not a config file */
                    continue;

                sprintf( cfg_out, "%s/%s", current_path, ent->d_name );
                if ( (stat( cfg_out, &stbuf ) == 0)
                     && S_ISREG(stbuf.st_mode) )
                {
                    /* file found: OK */
                    closedir(dir);
                    return 0;
                }
            }

            closedir(dir);
       }
    }
    else if (access(cfg_in, F_OK) == 0)
    {
        /* the specified config file exists */
        strcpy(cfg_out, cfg_in);
        *changed=0;
        return 0;
    }
    else if (strchr(cfg_in, '/'))
    {
        /* the argument is a path (not a single name
         * and this path was not found) */
        *changed=0;
        return -ENOENT;
    }
    else /* look for a file in the given paths */
    {
        char cfg_cp[RBH_PATH_MAX];
        int has_ext = (strchr(cfg_in, '.') != NULL);

        strcpy(cfg_cp, cfg_in);

        for ( i = 0, current_path = default_cfg_paths[0];
             current_path != NULL;
             i++, current_path = default_cfg_paths[i] )
        {
            /* if the file already has an extension, try path/name */
            if (has_ext)
            {
                sprintf(cfg_out, "%s/%s", current_path, cfg_cp);
                if (access(cfg_out, F_OK) == 0)
                    return 0;
            }

            /* try path/name.cfg, path/name.conf */
            sprintf(cfg_out, "%s/%s.conf", current_path, cfg_cp);
            if (access(cfg_out, F_OK) == 0)
                return 0;

            sprintf(cfg_out, "%s/%s.cfg", current_path, cfg_cp);
            if (access(cfg_out, F_OK) == 0)
                return 0;
        }
    }

    /* no file found, cleaning cfg_out */
    cfg_out[0] = '\0';
    return -ENOENT;
}


#define TEST_LOCK_FILE_PERIOD 10

/*
 * This function is blocking as long as the lock file is present.
 * Optionaly updates an action timestamp, at each test.
 */
void TestLockFile( time_t * p_last_action )
{

    while ( access( global_config.lock_file, F_OK ) == 0 )
    {
        if ( p_last_action )
            *p_last_action = time( NULL );

        DisplayLog( LVL_MAJOR, "LOCK", "Lock file %s detected, waiting %ds",
                    global_config.lock_file, TEST_LOCK_FILE_PERIOD );
        rh_sleep( TEST_LOCK_FILE_PERIOD );
    }

}

char          *uid2str( uid_t uid, char *username )
{
    struct passwd *p = GetPwUid( uid );
    if ( p != NULL )
        strcpy( username, p->pw_name );
    else
        sprintf( username, "%d", ( int ) uid );

    return username;
}

char          *gid2str( gid_t gid, char *groupname )
{
    struct group  *g = GetGrGid( gid );
    if ( g != NULL )
        strcpy( groupname, g->gr_name );
    else
        sprintf( groupname, "%d", ( int ) gid );

    return groupname;
}

void PosixStat2EntryAttr( struct stat *p_inode, attr_set_t * p_attr_set, int size_info )
{
    ATTR_MASK_SET( p_attr_set, owner );
    uid2str( p_inode->st_uid, ATTR( p_attr_set, owner ) );

    ATTR_MASK_SET( p_attr_set, gr_name );
    gid2str( p_inode->st_gid, ATTR( p_attr_set, gr_name ) );

    if ( size_info )
    {
        ATTR_MASK_SET( p_attr_set, size );
        ATTR( p_attr_set, size ) = p_inode->st_size;

        ATTR_MASK_SET( p_attr_set, blocks );
        ATTR( p_attr_set, blocks ) = p_inode->st_blocks;

#ifdef ATTR_INDEX_blksize
        ATTR_MASK_SET( p_attr_set, blksize );
        ATTR( p_attr_set, blksize ) = p_inode->st_blksize;
#endif
    }

    ATTR_MASK_SET( p_attr_set, last_access );
    ATTR( p_attr_set, last_access ) =
        MAX3( p_inode->st_atime, p_inode->st_mtime, p_inode->st_ctime );

    ATTR_MASK_SET( p_attr_set, last_mod );
    /* @TODO is this really what we want? */
#if defined(_SHERPA) || defined(_HSM_LITE)
    ATTR( p_attr_set, last_mod ) = p_inode->st_mtime;
#else
    ATTR( p_attr_set, last_mod ) = MAX2( p_inode->st_mtime, p_inode->st_ctime );
#endif

#ifdef ATTR_INDEX_creation_time
    if (ATTR_MASK_TEST(p_attr_set, creation_time))
    {
        /* creation time is always <= ctime */
        if (p_inode->st_ctime < ATTR(p_attr_set, creation_time))
            ATTR(p_attr_set, creation_time) = p_inode->st_ctime;
    }
    else
    {
        ATTR_MASK_SET(p_attr_set, creation_time);
        ATTR(p_attr_set, creation_time) = p_inode->st_ctime;
    }
#endif

#ifdef ATTR_INDEX_type
    if ( S_ISREG( p_inode->st_mode ) )
    {
        ATTR_MASK_SET( p_attr_set, type );
        strcpy( ATTR( p_attr_set, type ), STR_TYPE_FILE );
    }
    else if ( S_ISDIR( p_inode->st_mode ) )
    {
        ATTR_MASK_SET( p_attr_set, type );
        strcpy( ATTR( p_attr_set, type ), STR_TYPE_DIR );
    }
    else if ( S_ISCHR( p_inode->st_mode ) )
    {
        ATTR_MASK_SET( p_attr_set, type );
        strcpy( ATTR( p_attr_set, type ), STR_TYPE_CHR );
    }
    else if ( S_ISBLK( p_inode->st_mode ) )
    {
        ATTR_MASK_SET( p_attr_set, type );
        strcpy( ATTR( p_attr_set, type ), STR_TYPE_BLK );
    }
    else if ( S_ISFIFO( p_inode->st_mode ) )
    {
        ATTR_MASK_SET( p_attr_set, type );
        strcpy( ATTR( p_attr_set, type ), STR_TYPE_FIFO );
    }
    else if ( S_ISLNK( p_inode->st_mode ) )
    {
        ATTR_MASK_SET( p_attr_set, type );
        strcpy( ATTR( p_attr_set, type ), STR_TYPE_LINK );
    }
    else if ( S_ISSOCK( p_inode->st_mode ) )
    {
        ATTR_MASK_SET( p_attr_set, type );
        strcpy( ATTR( p_attr_set, type ), STR_TYPE_SOCK );
    }
#endif

#ifdef ATTR_INDEX_nlink
    ATTR_MASK_SET( p_attr_set, nlink );
    ATTR( p_attr_set, nlink ) = p_inode->st_nlink;
#endif
}

#ifndef HAVE_GETMNTENT_R

/* if getmntent_r() does not exists, define it as a wrapper of getmntent().
 * use a lock to ensure thread-safety.
 */
static pthread_mutex_t mntent_lock = PTHREAD_MUTEX_INITIALIZER;

/* copy a mntent structure to caller's buffer */
static int copy_mntent( struct mntent *mntout, char *buf, int buflen,
                        const struct mntent * mntin )
{
    char * curr = buf;

    if (!buf || !mntout)
        return EFAULT;

    if (strlen(mntin->mnt_fsname)+1
        +strlen(mntin->mnt_dir)+1
        +strlen(mntin->mnt_type)+1
        +strlen(mntin->mnt_opts)+1 > buflen )
        return ENOMEM;

    strcpy(curr, mntin->mnt_fsname );
    mntout->mnt_fsname = curr;
    curr += strlen(mntin->mnt_fsname)+1;

    strcpy(curr, mntin->mnt_dir );
    mntout->mnt_dir = curr;
    curr += strlen(mntin->mnt_dir)+1;

    strcpy(curr, mntin->mnt_type );
    mntout->mnt_type = curr;
    curr += strlen(mntin->mnt_type)+1;

    strcpy(curr, mntin->mnt_opts );
    mntout->mnt_opts = curr;
    curr += strlen(mntin->mnt_opts)+1;

    mntout->mnt_freq   = mntin->mnt_freq;
    mntout->mnt_passno = mntin->mnt_passno;

    return 0;
}

static struct mntent *getmntent_r(FILE *fp, struct mntent *mntbuf,
                           char *buf, int buflen)
{
    struct mntent * pmntent;
    /* struct mntent *getmntent(FILE *fp); */
    P(mntent_lock);
    pmntent = getmntent(fp);
    /* copy mntent structure to caller buffer */
    if (pmntent)
    {
        if ( copy_mntent(mntbuf, buf, buflen, pmntent) != 0 )
            pmntent = NULL; /* causes an error */
    }
    V(mntent_lock);
    return pmntent;
}
#endif

/* Check mount point and FS type.
 * Also return the associated device number.
 * (for STAY_IN_FS security option).
 */
int CheckFSInfo( char *path, char *expected_type,
                 dev_t * p_fs_dev, char * fsname_out,
                 int check_mounted, int save_fs )
{
    FILE          *fp;
    struct mntent *p_mnt;
    struct mntent  mnt_ent;
    char           mnt_buff[4096];

    char           rpath[RBH_PATH_MAX];
    char           mntdir[RBH_PATH_MAX];
    char           tmp_buff[RBH_PATH_MAX];
    char          *parentmntdir;
    char           fs_spec[RBH_PATH_MAX];

    char           type[256];

    struct stat    pathstat;
    struct stat    parentmntstat;

    size_t         pathlen, outlen;
    char * name = NULL;


    if ( ( expected_type == NULL ) || ( expected_type[0] == '\0' ) )
    {
        DisplayLog( LVL_CRIT, "CheckFS", "/!\\ ERROR /!\\ No filesystem type specified" );
        return EINVAL;
    }

    /* convert to canonic path */
    /* let realpath determine the output length (NULL argument) */
    char * tmp_path = realpath( path, NULL );
    if ( tmp_path == NULL )
    {
        DisplayLog( LVL_CRIT, "CheckFS", "Error %d in realpath(%s): %s",
                    errno, ( path ? path : "<null>" ), strerror( errno ) );
        return errno;
    }
    if (strlen(tmp_path) >= RBH_PATH_MAX)
    {
        free(tmp_path);
        DisplayLog( LVL_CRIT, "CheckFS", "Path length is too long!" );
        return ENAMETOOLONG;
    }
    /* safe because of previous check */
    strcpy(rpath, tmp_path);
    /* now can release tmp path */
    free(tmp_path);

    /* open mount tab and look for the given path */
    outlen = 0;

    fp = setmntent( MOUNTED, "r" );

    if ( fp == NULL )
    {
        DisplayLog( LVL_CRIT, "CheckFS", "Error %d in setmntent(%s): %s",
                    errno, MOUNTED, strerror( errno ) );
        return errno;
    }

    while ( ( p_mnt = getmntent_r( fp, &mnt_ent, mnt_buff, 4096 ) ) != NULL )
    {
        /* get the longest matching path */

        if ( p_mnt->mnt_dir != NULL )
        {

            pathlen = strlen( p_mnt->mnt_dir );

            /* if check_mounted is FALSE, root filesystem is allowed */
            if ( !check_mounted && ( pathlen > outlen )
                 && !strcmp( p_mnt->mnt_dir, "/" ) )
            {
                DisplayLog( LVL_DEBUG, "CheckFS",
                            "Root mountpoint is allowed for matching %s, type=%s, fs=%s",
                            rpath, p_mnt->mnt_type, p_mnt->mnt_fsname );
                outlen = pathlen;
                strncpy( mntdir, p_mnt->mnt_dir, RBH_PATH_MAX );
                strncpy( type, p_mnt->mnt_type, 256 );
                strncpy( fs_spec, p_mnt->mnt_fsname, RBH_PATH_MAX );
            }
            /* in other cases, the filesystem must be <mountpoint>/<smthg> or <mountpoint>\0 */
            else if ( ( pathlen > outlen ) &&
                      !strncmp( rpath, p_mnt->mnt_dir, pathlen ) &&
                      ( ( rpath[pathlen] == '/' ) || ( rpath[pathlen] == '\0' ) ) )
            {
                DisplayLog( LVL_FULL, "CheckFS",
                            "%s is under mountpoint %s, type=%s, fs=%s",
                            rpath, p_mnt->mnt_dir, p_mnt->mnt_type, p_mnt->mnt_fsname );

                outlen = pathlen;
                strncpy( mntdir, p_mnt->mnt_dir, RBH_PATH_MAX );
                strncpy( type, p_mnt->mnt_type, 256 );
                strncpy( fs_spec, p_mnt->mnt_fsname, RBH_PATH_MAX );
            }
        }
    }

    if ( outlen <= 0 )
    {
        DisplayLog( LVL_CRIT, "CheckFS", "No mount entry matches '%s' in %s", rpath, MOUNTED );
        DisplayLog( LVL_CRIT, "CheckFS",
                    "Set 'check_mounted = FALSE' in configuration to force using root filesystem" );
        endmntent( fp );
        return ENOENT;
    }


    /* display the matching entry */
    DisplayLog( LVL_EVENT, "CheckFS",
                "'%s' matches mount point '%s', type=%s, fs=%s", rpath, mntdir, type, fs_spec );

    /* check filesystem type */
    if ( strcasecmp( type, expected_type ) )
    {
        if (check_mounted)
        {
            DisplayLog( LVL_CRIT, "CheckFS",
                        "/!\\ ERROR /!\\ The specified type for '%s' (%s) does not match actual filesystem type (%s)",
                        rpath, expected_type, type );
            endmntent( fp );
            return EINVAL;
        }
        else
        {
            DisplayLog( LVL_MAJOR, "CheckFS",
                        "/!\\ WARNING /!\\ The specified type for '%s' (%s) "
                        "does not match actual filesystem type (%s).",
                        rpath, expected_type, type );
            DisplayLog( LVL_MAJOR, "CheckFS", "check_mounted is disabled: continuing." );
        }
    }

    /* stat the given fs_path */
    if ( stat( rpath, &pathstat ) != 0 )
    {
        DisplayLog( LVL_CRIT, "CheckFS",
                    "/!\\ ERROR /!\\ Couldn't stat '%s': %s", rpath, strerror( errno ) );
        endmntent( fp );
        return errno;
    }

    /* Stat upper level of mount point, to check if
     * the filesystem is mounted (device  must be different).
     * (dirname modifies string content, so we work on a copy
     * in tmp_buff).
     */
    strcpy( tmp_buff, mntdir );
    parentmntdir = dirname( tmp_buff );

    if ( lstat( parentmntdir, &parentmntstat ) != 0 )
    {
        DisplayLog( LVL_CRIT, "CheckFS",
                    "/!\\ ERROR /!\\ Couldn't stat %s: %s", parentmntdir, strerror( errno ) );
        endmntent( fp );
        return errno;
    }

    /* check that filesystem device is different from root (except if check_mounted is disabled) */
    if ( ( pathstat.st_dev == parentmntstat.st_dev ) && check_mounted )
    {
        DisplayLog( LVL_CRIT, "CheckFS",
                    "/!\\ ERROR /!\\ Filesystem '%s' is not mounted ! dev(%s)=dev(%s)=%#"
                    PRIx64, mntdir, parentmntdir, rpath, (uint64_t)parentmntstat.st_dev );
        endmntent( fp );
        return ENOENT;
    }

#ifdef _LUSTRE
    if (!strcmp(type, "lustre"))
    {
        char *ptr;
        ptr = strstr( fs_spec, ":/" );
        if ( ptr != NULL )
        {
            name = ptr + 2;
        }
        else
            name = fs_spec;
    }
    else
#endif
        name = fs_spec;

    /* all checks are OK */

    if ( save_fs )
    {
        /* getting filesystem fsid (needed for fskey) */
        if (global_config.fs_key == FSKEY_FSID)
        {
            struct statfs stf;
            if (statfs(mntdir, &stf))
            {
                int rc = -errno;
                DisplayLog( LVL_CRIT, "CheckFS", "ERROR calling statfs(%s): %s",
                    mntdir, strerror(-rc) );
                    return rc;
            }
            /* if fsid == 0, it may mean that fsid is not significant on the current system
             * => DISPLAY A WARNING */
            if (fsidto64(stf.f_fsid) == 0)
            {
                DisplayLog(LVL_MAJOR, "CheckFS", "WARNING: fsid(0) doesn't look significant on this system. I should not be used as fs_key!");
            }
            set_fs_info(name, mntdir, pathstat.st_dev, stf.f_fsid);
        }
        else
        {
            fsid_t dummy_fsid;
            memset(&dummy_fsid, 0, sizeof(fsid_t));
            set_fs_info(name, mntdir, pathstat.st_dev, dummy_fsid);
        }
    }

    if ( p_fs_dev != NULL )
        *p_fs_dev = pathstat.st_dev;

    if ( fsname_out != NULL )
        strcpy(fsname_out, name);

    endmntent( fp );
    return 0;
}                               /* CheckFSInfo */

/**
 * Initialize filesystem access and retrieve current devid/fs_key
 * - global_config must be set
 * - initialize mount_point, fsname and dev_id
 */
int InitFS()
{
    int rc;

    /* Initialize mount point info */
#ifdef _LUSTRE
    if (!strcmp( global_config.fs_type, "lustre" ))
    {
        if ( ( rc = Lustre_Init() ) )
        {
            DisplayLog( LVL_CRIT, "InitFS", "Error %d initializing liblustreapi", rc );
            return rc;
        }
    }
#endif

    rc = CheckFSInfo( global_config.fs_path, global_config.fs_type, NULL, NULL,
                      global_config.check_mounted, TRUE );
    if (rc)
    {
        DisplayLog( LVL_CRIT, "InitFS", "Error %d checking Filesystem", rc );
        return rc;
    }

    /* OK */
    return 0;
}

/**
 * This is to be called after a dev_id change was detected
 * return 0 if fskey is unchanged and update mount_point, fsname and dev_id
 * else, return -1
 */
int ResetFS()
{
    char   name[RBH_PATH_MAX];
    dev_t  dev;
    struct statfs stf;
    int rc;
    /* check depending on FS key type:
     * - fsname: check mount tab
     * - fsid: check statfs
     * - devid: check dev_id
     */
    switch (global_config.fs_key)
    {
        case FSKEY_FSNAME:
            /* get and compare FS name */
            rc = CheckFSInfo( global_config.fs_path, global_config.fs_type, NULL, name,
                              global_config.check_mounted, FALSE );
            if (rc)
                return rc;
            /* did the name changed ? */
            if (strcmp(name, fsname))
            {
                DisplayLog(LVL_CRIT, "FSInfo", "fsname change detected: %s->%s",
                           fsname, name);
                RaiseAlert( "Filesystem changed",
                             "fsname of '%s' has changed !!! %s->%s => EXITING",
                            global_config.fs_path, fsname, name );
                return -1;
            }
            /* update fsid and devid */
            rc = CheckFSInfo( global_config.fs_path, global_config.fs_type, NULL, NULL,
                              global_config.check_mounted, TRUE );
            return rc;

        case FSKEY_FSID:
            /* get and compare FS ID */
            if (statfs(global_config.fs_path, &stf))
            {
                rc = -errno;
                DisplayLog( LVL_CRIT, "FSInfo", "ERROR calling statfs(%s): %s",
                    global_config.fs_path, strerror(-rc) );
                    return rc;
            }
            if (fsidto64(stf.f_fsid) != fs_key)
            {
                DisplayLog(LVL_CRIT, "FSInfo", "fsid change detected: %"PRIX64"->%"PRIX64,
                           fs_key, fsidto64(stf.f_fsid));
                RaiseAlert( "Filesystem changed",
                             "fsid of '%s' has changed !!! %"PRIX64"->%"PRIX64" => EXITING",
                            global_config.fs_path, fs_key, fsidto64(stf.f_fsid) );
                return -1;
            }
            /* update fsname and devid */
            rc = CheckFSInfo( global_config.fs_path, global_config.fs_type, NULL, NULL,
                              global_config.check_mounted, TRUE );
            return rc;

        case FSKEY_DEVID:
            /* get and compare dev id */
            rc = CheckFSInfo( global_config.fs_path, global_config.fs_type, &dev, NULL,
                              global_config.check_mounted, FALSE );
            if (rc)
                return rc;
            /* did the device change? */
            if (dev != dev_id)
            {
                DisplayLog(LVL_CRIT, "FSInfo", "devid change detected: %"PRI_DT"->%"PRI_DT,
                           dev_id, dev);

                RaiseAlert( "Filesystem changed",
                             "devid of '%s' has changed !!! %"PRI_DT"->%"PRI_DT" => EXITING",
                            global_config.fs_path, dev_id, dev );
                return -1;
            }
            /* update fsname and fsid */
            rc = CheckFSInfo( global_config.fs_path, global_config.fs_type, NULL, NULL,
                              global_config.check_mounted, TRUE );
            return rc;

        default:
            DisplayLog(LVL_MAJOR, "FSInfo", "Invalid fs_key type %#x", global_config.fs_key);
            return -1;
    }

}


/**
 *  Check that FS path is the same as the last time.
 */
int CheckLastFS(  )
{
    int            rc;
    lmgr_t         lmgr;
    char           value[1024];

    rc = ListMgr_InitAccess( &lmgr );
    if ( rc )
    {
        DisplayLog( LVL_CRIT, "CheckFS", "Error %d connecting to database", rc );
        return rc;
    }
    rc = ListMgr_GetVar( &lmgr, FS_PATH_VAR, value );
    if ( rc == DB_SUCCESS )
    {
        if ( strcmp( value, global_config.fs_path ) )
        {
            DisplayLog( LVL_CRIT, "CheckFS",
                        "Filesystem %s does not correspond to database content (%s)",
                        global_config.fs_path, value );
            DisplayLog( LVL_CRIT, "CheckFS", "Drop the database and restart the daemon." );
            rc = -1;
        }
        else
        {
            DisplayLog( LVL_DEBUG, "CheckFS", "%s matches database content.",
                        global_config.fs_path );
            rc = 0;
        }
    }
    else if ( rc == DB_NOT_EXISTS )
    {
        DisplayLog( LVL_FULL, "CheckFS", FS_PATH_VAR "='%s'.", global_config.fs_path );
        rc = ListMgr_SetVar( &lmgr, FS_PATH_VAR, global_config.fs_path );
        if ( rc )
            DisplayLog( LVL_CRIT, "CheckFS", "Error %d setting variable 'FS_path'", rc );
    }
    else
    {
        DisplayLog( LVL_CRIT, "CheckFS", "Error %d retrieving variable 'FS_path'", rc );
    }

    ListMgr_CloseAccess( &lmgr );
    return rc;
}

/* return 0 if thread has been killed,
 * -1 if thread was already terminated.
 */
int TerminateThread( pthread_t thread_id )
{
    if ( pthread_cancel( thread_id ) == ESRCH )
        return -1;

    return 0;
}


/* Format a size in a "human readable" format */

char          *FormatFileSize( char *buff, size_t str_sz, uint64_t file_size )
{
    if ( file_size < KILO_BYTE )
        snprintf( buff, str_sz, "%llu", (unsigned long long)file_size );
    else if ( file_size < MEGA_BYTE )
        snprintf( buff, str_sz, "%.2f KB", ( 0.0 + file_size ) / ( 0.0 + KILO_BYTE ) );
    else if ( file_size < GIGA_BYTE )
        snprintf( buff, str_sz, "%.2f MB", ( 0.0 + file_size ) / ( 0.0 + MEGA_BYTE ) );
    else if ( file_size < TERA_BYTE )
        snprintf( buff, str_sz, "%.2f GB", ( 0.0 + file_size ) / ( 0.0 + GIGA_BYTE ) );
    else if ( file_size < PETA_BYTE )
        snprintf( buff, str_sz, "%.2f TB", ( 0.0 + file_size ) / ( 0.0 + TERA_BYTE ) );
    else if ( file_size < EXA_BYTE )
        snprintf( buff, str_sz, "%.2f PB", ( 0.0 + file_size ) / ( 0.0 + PETA_BYTE ) );
    else
        snprintf( buff, str_sz, "%.2f EB", ( 0.0 + file_size ) / ( 0.0 + EXA_BYTE ) );

    return buff;

}

/* Format a duration (in seconds) to a string with days hours minutes seconds... */

char          *FormatDuration( char *buff, size_t str_sz, time_t duration )
{

    unsigned int   days;
    unsigned int   hours;
    unsigned int   minutes;
    unsigned int   secondes;

    size_t         written = 0;

    if ( duration < 0 )
        duration = -duration;

    days = ( unsigned int ) duration / DAY;
    hours = ( ( unsigned int ) duration % DAY ) / HOUR;
    minutes = ( ( unsigned int ) duration % HOUR ) / MINUTE;
    secondes = ( ( unsigned int ) duration % MINUTE );

    buff[0] = '\0';

    if ( days > 0 )
        written += snprintf( buff, str_sz, "%ud ", days );

    if ( hours > 0 )
        written += snprintf( buff + written, str_sz - written, "%uh ", hours );

    if ( minutes > 0 )
        written += snprintf( buff + written, str_sz - written, "%.2umin ", minutes );

    if ( secondes > 0 || duration == 0 )
        written += snprintf( buff + written, str_sz - written, "%.2us ", secondes );

    if ( ( written > 0 ) && ( buff[written - 1] == ' ' ) )
        buff[written - 1] = '\0';

    return buff;

}

/* Format a duration (in seconds) to a string with the best fitting unit (float value) */

char          *FormatDurationFloat( char *buff, size_t str_sz, time_t duration )
{
    float          days;
    float          hours;
    float          minutes;
    unsigned int   secondes;

    if ( duration < 0 )
        duration = -duration;

    days = ( float ) duration / DAY;
    hours = ( float ) duration / HOUR;
    minutes = ( float ) duration / MINUTE;
    secondes = ( unsigned int ) duration;

    buff[0] = '\0';

    if ( days >= 1.0 )
        snprintf( buff, str_sz, "%.1fd", days );
    else if ( hours >= 1.0 )
        snprintf( buff, str_sz, "%.1fh", hours );
    else if ( minutes >= 1.0 )
        snprintf( buff, str_sz, "%.1fmin", minutes );
    else
        snprintf( buff, str_sz, "%.2us", secondes );

    return buff;

}

char          *FormatStripeList( char *buff, size_t sz, const stripe_items_t * p_stripe_items )
{
    unsigned int   i;
    size_t         written = 0;

    if ( !p_stripe_items || ( p_stripe_items->count == 0 ) )
    {
        strncpy( buff, "(none)", sz );
        return buff;
    }

    buff[0] = '\0';

    for ( i = 0; i < p_stripe_items->count; i++ )
    {
        if ( i != p_stripe_items->count - 1 )
            written +=
                snprintf( ( char * ) ( buff + written ), sz - written, "OST #%u, ",
                          p_stripe_items->stripe_units[i] );
        else
            written +=
                snprintf( ( char * ) ( buff + written ), sz - written, "OST #%u",
                          p_stripe_items->stripe_units[i] );
    }

    return buff;
}

/**
 * Convert a string to an integer
 * @return -1 on error.
 */
int str2int( char *str )
{
    char           suffix[256];
    int            nb_read, value;

    if ( str == NULL )
        return -1;

    nb_read = sscanf( str, "%d%s", &value, suffix );

    if ( nb_read <= 0 )
        return -1;              /* invalid format */

    if ( ( nb_read == 1 ) || ( suffix[0] == '\0' ) )
        return value;           /* no suffix => 0K */
    else
        return -1;
}

/**
 * Convert a string to a long integer
 * @return -1 on error.
 */
long long str2bigint( char *str )
{
    char           suffix[256];
    int            nb_read;
    long long      value;

    if ( str == NULL )
        return -1;

    nb_read = sscanf( str, "%lld%s", &value, suffix );

    if ( nb_read <= 0 )
        return -1;              /* invalid format */

    if ( ( nb_read == 1 ) || ( suffix[0] == '\0' ) )
        return value;           /* no suffix => 0K */
    else
        return -1;
}


/**
 * Convert a string to a boolean 
 * @return -1 on error. 
 */
int str2bool( char *str )
{
    if ( str == NULL )
        return -1;

    if ( !strcmp( str, "1" ) || !strcasecmp( str, "TRUE" ) ||
         !strcasecmp( str, "YES" ) || !strcasecmp( str, "ENABLED" ) || !strcasecmp( str, "ON" ) )
        return 1;

    if ( !strcmp( str, "0" ) || !strcasecmp( str, "FALSE" ) ||
         !strcasecmp( str, "NO" ) || !strcasecmp( str, "DISABLED" ) || !strcasecmp( str, "OFF" ) )
        return 0;

    return -1;
}


/**
 * Convert a string to a duration in seconds 
 * @return -1 on error. 
 */
int str2duration( const char *str )
{
    int            nb_read, duration;
    char           suffix[256];

    if ( str == NULL )
        return -1;

    nb_read = sscanf( str, "%d%s", &duration, suffix );

    if ( nb_read <= 0 )
        return -1;              /* invalid format */

    if ( ( nb_read == 1 ) || ( suffix[0] == '\0' ) )
        return duration;        /* no suffix: duration in seconds */

    if ( !strcasecmp( suffix, "s" ) || !strcasecmp( suffix, "sec" ) )
        return duration;
    if ( !strcasecmp( suffix, "m" ) || !strcasecmp( suffix, "min" ) )
        return MINUTE * duration;
    if ( !strcasecmp( suffix, "h" ) || !strcasecmp( suffix, "hour" ) )
        return HOUR * duration;
    if ( !strcasecmp( suffix, "d" ) || !strcasecmp( suffix, "day" ) )
        return DAY * duration;
    if ( !strcasecmp( suffix, "w" ) || !strcasecmp( suffix, "week" ) )
        return WEEK * duration;
    if ( !strcasecmp( suffix, "y" ) || !strcasecmp( suffix, "year" ) )
        return YEAR * duration;

    return -1;
}


/**
 * Convert a string to a size (in bytes)
 * @return -1 on error. 
 */
uint64_t str2size( char *str )
{
    int                   nb_read;
    unsigned long long    size;
    char                  suffix[256];

    if ( str == NULL )
        return (uint64_t) -1LL;

    nb_read = sscanf( str, "%llu%s", &size, suffix );

#ifdef _DEBUG_PARSING
    printf( "nb_read = %d, str = %s, size = %llu, suffix = %s\n", nb_read, str, size, suffix );
#endif

    if ( nb_read <= 0 )
        return (uint64_t) -1LL;     /* invalid format */

    if ( ( nb_read == 1 ) || ( suffix[0] == '\0' ) )
        return size;            /* no suffix: size in bytes */

    if ( !strcasecmp( suffix, "B" ) )
        return size;
    if ( !strcasecmp( suffix, "kB" ) || !strcasecmp( suffix, "K" ) )
        return ( KILO_BYTE * size );
    if ( !strcasecmp( suffix, "MB" ) ||  !strcasecmp( suffix, "M" ) )
        return ( MEGA_BYTE * size );
    if ( !strcasecmp( suffix, "GB" ) ||  !strcasecmp( suffix, "G" ) )
        return ( GIGA_BYTE * size );
    if ( !strcasecmp( suffix, "TB" ) || !strcasecmp( suffix, "T" ))
        return ( TERA_BYTE * size );
    if ( !strcasecmp( suffix, "PB" ) || !strcasecmp( suffix, "P" ) )
        return ( PETA_BYTE * size );
    if ( !strcasecmp( suffix, "EB" ) || !strcasecmp( suffix, "E" ) )
        return ( EXA_BYTE * size );

    return (uint64_t) -1LL;
}

/**
 * extracts up to count digits from src string,
 * and copy them to dest string. dest is completed
 * with '\0'.
 * @return the number of digits copied to dest.
 */
static inline int extract_digits( const char * src, char * dest, unsigned int count )
{
    unsigned int i;
    unsigned int cpd = 0;
    for ( i = 0; (i < count) && (src[i] != '\0'); i++ )
    {
        dest[i] = src[i];
        cpd ++;
    }
    dest[cpd] = '\0';
    return cpd;
}

/** parse date/time yyyymmdd[HH[MM[SS]]] */
time_t str2date( char *str )
{
    struct tm datetime = {
        .tm_sec = 0,
        .tm_min = 0,
        .tm_hour = 0,
        .tm_mday = 0,
        .tm_mon = 0,
        .tm_year = 0,
        .tm_wday = 0,
        .tm_yday = 0,
        .tm_isdst = -1
    };
    char tmpstr[16];
    int  tmpint;
    char * curr = str;
    
    /* extract year */
    if (extract_digits(curr, tmpstr, 4) < 4)
        return (time_t)-1;
    curr += 4;
    if ((tmpint = str2int(tmpstr)) == -1)
        return (time_t)-1;
    datetime.tm_year = tmpint - 1900; /* 1900 => 0 */

    /* extract month */
    if (extract_digits(curr, tmpstr, 2) < 2)
        return (time_t)-1;
    curr += 2;
    if ((tmpint = str2int(tmpstr)) <= 0)
        return (time_t)-1;
    else if (tmpint > 12)
        return (time_t)-1; 
    datetime.tm_mon = tmpint - 1; /* January => 0 */

    /* extract day */
    if (extract_digits(curr, tmpstr, 2) < 2)
        return (time_t)-1;
    curr += 2;
    if ((tmpint = str2int(tmpstr)) <= 0)
        return (time_t)-1;
    else if (tmpint > 31)
        return (time_t)-1; 
    datetime.tm_mday = tmpint; /* 1st => 1 */

    /* extract hours */
    tmpint = extract_digits(curr, tmpstr, 2);
    if (tmpint == 0) /* not specified */
        goto convert;
    else if (tmpint < 2) /* invalid */
        return (time_t)-1;
    curr += 2;
    if ((tmpint = str2int(tmpstr)) == -1)
        return (time_t)-1;
    else if (tmpint > 23)
        return (time_t)-1; 
    datetime.tm_hour = tmpint;

    /* extract minutes */
    tmpint = extract_digits(curr, tmpstr, 2);
    if (tmpint == 0) /* not specified */
        goto convert;
    else if (tmpint < 2) /* invalid */
        return (time_t)-1;
    curr += 2;
    if ((tmpint = str2int(tmpstr)) == -1)
        return (time_t)-1;
    else if (tmpint > 59)
        return (time_t)-1; 
    datetime.tm_min = tmpint;

    /* extract seconds */
    tmpint = extract_digits(curr, tmpstr, 2);
    if (tmpint == 0) /* not specified */
        goto convert;
    else if (tmpint < 2) /* invalid */
        return (time_t)-1;
    curr += 2;
    if ((tmpint = str2int(tmpstr)) == -1)
        return (time_t)-1;
    else if (tmpint > 59)
        return (time_t)-1; 
    datetime.tm_sec = tmpint;

    if (*curr != '\0')
        return (time_t)-1;

convert:
     return mktime(&datetime);
}


/**
 *  Print attributes to a string
 */
int PrintAttrs( char *out_str, size_t strsize, const attr_set_t * p_attr_set, int overide_mask )
{
    int            mask = p_attr_set->attr_mask;
    size_t         written = 0;
    char           tmpbuf[256];

    if ( overide_mask )
        mask = mask & overide_mask;

    if ( mask & ATTR_MASK_fullpath )
        written +=
            snprintf( out_str + written, strsize - written, "Fullpath: \"%s\"\n",
                      ATTR( p_attr_set, fullpath ) );
    if ( mask & ATTR_MASK_name )
        written +=
            snprintf( out_str + written, strsize - written, "Name:     \"%s\"\n",
                      ATTR( p_attr_set, name ) );
#ifdef ATTR_INDEX_type
    if ( mask & ATTR_MASK_type )
        written +=
            snprintf( out_str + written, strsize - written, "Type:     %s\n",
                      ATTR( p_attr_set, type ) );
#endif
    if ( mask & ATTR_MASK_owner )
        written +=
            snprintf( out_str + written, strsize - written, "Owner:    \"%s\"\n",
                      ATTR( p_attr_set, owner ) );
    if ( mask & ATTR_MASK_gr_name )
        written +=
            snprintf( out_str + written, strsize - written, "Group:    \"%s\"\n",
                      ATTR( p_attr_set, gr_name ) );
    if ( mask & ATTR_MASK_size )
    {
        FormatFileSize( tmpbuf, 256, ATTR( p_attr_set, size ) );
        written += snprintf( out_str + written, strsize - written, "Size:     %s\n", tmpbuf );
    }
    if ( mask & ATTR_MASK_depth )
        written +=
            snprintf( out_str + written, strsize - written, "Depth:    %d\n",
                      ATTR( p_attr_set, depth ) );
#ifdef ATTR_INDEX_dircount
    if ( mask & ATTR_MASK_dircount )
        written +=
            snprintf( out_str + written, strsize - written, "DirCount: %d\n",
                      ATTR( p_attr_set, dircount ) );
#endif
    if ( mask & ATTR_MASK_last_access )
    {
        FormatDurationFloat( tmpbuf, 256, time( NULL ) - ATTR( p_attr_set, last_access ) );
        written +=
            snprintf( out_str + written, strsize - written, "Last Access: %s ago\n", tmpbuf );
    }
#ifdef ATTR_INDEX_last_copy
    if ( mask & ATTR_MASK_last_copy )
    {
        FormatDurationFloat( tmpbuf, 256, time( NULL ) - ATTR( p_attr_set, last_copy ) );
        written += snprintf( out_str + written, strsize - written, "Last Copy: %s ago\n", tmpbuf );
    }
#endif
    if ( mask & ATTR_MASK_last_mod )
    {
        FormatDurationFloat( tmpbuf, 256, time( NULL ) - ATTR( p_attr_set, last_mod ) );
        written += snprintf( out_str + written, strsize - written, "Last Mod: %s ago\n", tmpbuf );
    }

    return written;
}


/** Compute greatest common divisor (GCD) of 2 numbers */
unsigned int gcd( unsigned int x, unsigned int y )
{
    unsigned int   a = x;
    unsigned int   b = y;
    while ( ( a * b ) != 0 )
    {
        if ( a > b )
            a = a - b;
        if ( a < b )
            b = b - a;
        if ( a == b )
            b = 0;
    }
    return a;
}

/** Ensure that the thread is suspended for a given amount
 * of time, event if the process gets interrupts.
 */
void rh_sleep( unsigned int seconds )
{
   time_t start = time(NULL);
   int remain = seconds;
   int spent;

   while ( remain > 0 )
   {
       remain = sleep( remain );
       if ( remain <= 0 )
       {
           spent = time(NULL)-start; 
           if ( spent < seconds )
               remain = seconds - spent;
       }
   }
}

/** replace a pattern in a string with another sub-string
 * \param str_in_out must be large enough to receive
 *  the resulting string, and cannot exceed 1024.
 */
int str_replace( char * str_in_out, const char * to_be_replaced,
                 const char * replacement )
{
    size_t len_from = strlen(to_be_replaced);
    size_t len_to = strlen(replacement);
    char * curr;

    /* - same size: replace inline
     * - smaller string size: replace inline then shift
     */

    for ( curr = strstr( str_in_out, to_be_replaced );
          curr != NULL;
          curr = strstr( curr, to_be_replaced ) )
    {
        unsigned int i;
        char * curr_src;
        char tmp_buff[1024];

        /* if replacement is longer, save end of line */
        if ( len_to > len_from )
            strcpy(tmp_buff, curr+len_from);

        for ( i=0; i<len_to; i++ )
            curr[i]=replacement[i];
        curr = curr + len_to;

        /* if replacement is smaller, need to shift */
        if ( len_to < len_from )
        {
            if ( ((char*)(curr + len_from - len_to))[0] == '\0' )
                curr[0]='\0';

            /* shift of len_from - len_to*/
            for ( curr_src = curr + len_from - len_to, i=0;
                  *curr_src != '\0';
                  curr_src ++, i++ )
            {
                curr[i] = *curr_src;
                if ( curr_src[1] == '\0' )
                    curr[i+1] = '\0';
            }
        }
        else if ( len_to > len_from )
            /* copy saved data */
            strcpy(curr, tmp_buff);
    }
    return 0;
}



/**
 * extract relative path from full path
 */
int relative_path( const char * fullpath, const char * root, char * rel_path )
{
    size_t len;
    char rootcopy[1024];

    /* copy root path */
    strcpy(rootcopy, root);

    len = strlen(rootcopy);
    /* add '/' if needed */
    if ( (len > 1) && (rootcopy[len-1] != '/') )
    {
        rootcopy[len] = '/';
        rootcopy[len+1] = '\0';
        len++;
    }

    /* test if the full path starts with the same dirs */
    if (strncmp(rootcopy, fullpath,len))
    {
        DisplayLog( LVL_MAJOR, "RelPath", "ERROR: file path '%s' is not under filesystem root '%s'",
                    fullpath, rootcopy );
        return -EINVAL;
    }

    strcpy( rel_path, fullpath+len );
    return 0;
}

/**
 * Put a string into double quotes and escape double quotes
 */
static char * escape_shell_arg( const char * in, char * out )
{
    char * curr_out = out;
    const char * curr_in = in;
    curr_out[0] = '"';
    curr_out++;

    while (*curr_in)
    {
        if (*curr_in != '"')
        {
            *curr_out = *curr_in;
            curr_out++;
        }
        else
        {
            curr_out[0] = '\\';
            curr_out[1] = '"';
            curr_out+=2;
        }
        curr_in++;
    }
    curr_out[0]='"';
    curr_out[1]='\0';

    return out;
}

int execute_shell_command( const char * cmd, int argc, ... )
{
#define SHCMD "ShCmd"
    va_list arglist;
    char cmdline[4096];
    char argbuf[1024];
    char * curr = cmdline;
    int rc, i;
    int exrc;

    curr += sprintf( cmdline, "%s", cmd );

    va_start(arglist, argc);
    for (i = 0; i < argc; i++)
        curr += sprintf( curr, " %s",
                         escape_shell_arg( va_arg(arglist, char *), argbuf ));
    va_end(arglist);
    curr += sprintf( curr, " %s", " >/dev/null 2>/dev/null");

    DisplayLog(LVL_DEBUG, SHCMD, "Executing command: %s", cmdline);
    rc = system(cmdline);

    if ( WIFEXITED(rc) )
    {
        const char * str_error;
        exrc = WEXITSTATUS(rc);
        if (exrc == 0)
        {
            DisplayLog(LVL_DEBUG, SHCMD, "Command successful");
            return 0;
        }

        /* shell special return values */
        if (exrc == 126)
            str_error = "permission problem or command is not an executable";
        else if (exrc == 127)
            str_error = "command not found";
        else if (exrc == 128)
            str_error = "invalid argument to exit";
        else
            str_error = "external command exited";

        DisplayLog( LVL_MAJOR, SHCMD,
                    "ERROR: %s, error %d (cmdline=%s)",
                    str_error, exrc, cmdline );
        rc = -exrc;
    }
    else if (WIFSIGNALED(rc))
    {
            DisplayLog( LVL_MAJOR, SHCMD,
                        "ERROR: command terminated by signal %d. cmdline=%s",
                        WTERMSIG(rc), cmdline );
            rc = -EINTR;
    }

    return rc;
}


/**
 * Replace special parameters {cfgfile}, {fspath}, ...
 * in the given cmd line.
 * Result string is allocated using malloc()
 * and must be released using free().
 */
char * replace_cmd_parameters(const char * cmd_in)
{
#define CMDPARAMS "CmdParams"
    int error = FALSE;
    char * pass_begin;
    char * begin_var;
    char * end_var;
    const char * value;

    /* allocate tmp copy of cmd in */
    pass_begin = (char *)malloc(strlen(cmd_in)+1);
    strcpy(pass_begin, cmd_in);

    do
    {
        char * new_str = NULL;

        /* look for a variable */
        begin_var = strchr( pass_begin, '{' );

        /* no more variables */
        if ( !begin_var )
            break;

        *begin_var = '\0';
        begin_var++;

        /* get matching '}' */
        end_var = strchr( begin_var, '}' );
        if (!end_var)
        {
           DisplayLog(LVL_CRIT,CMDPARAMS, "ERROR: unmatched '{' in command parameters '%s'", cmd_in);
           error = TRUE;
           break;
        }

        *end_var = '\0';
        end_var++;

        value = NULL;

        /* compute final length, depending on variable name */
        if (!strcasecmp( begin_var, "cfg" ))
           value = process_config_file;
        else if (!strcasecmp( begin_var, "fspath" ))
           value = global_config.fs_path;
        else
        {
            DisplayLog(LVL_CRIT,CMDPARAMS, "ERROR: unknown parameter '%s' in command parameters '%s'", begin_var, cmd_in);
            error = TRUE;
            break;
        }

        /* allocate a new string if var length < value length */
        new_str = malloc( strlen(pass_begin)+strlen(value)+strlen(end_var)+1 );

        sprintf(new_str, "%s%s%s", pass_begin, value, end_var );

        free(pass_begin);
        pass_begin = new_str;

    } while(1);

    return pass_begin;
}


