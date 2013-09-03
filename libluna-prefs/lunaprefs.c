/* @@@LICENSE
*
*      Copyright (c) 2008-2013 LG Electronics, Inc.
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*
* LICENSE@@@ */


/* -*-mode: C; fill-column: 78; c-basic-offset: 4; -*- */

#include <glib.h>
#include <lunaprefs.h>
#include <sqlite3.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/vfs.h>

#ifdef USE_MJSON
#include <json.h>
#endif
#include <cjson/json.h>
#include <nyx/nyx_client.h>
/* todo:
 *
 * set auto-vaccuum property
 *
 * compile two parameterized statements, the getter and setter.  But where do
 * they live?  Do I keep them in globals?  Would make sense if I'm being
 * called by a service that has state, but not if I'm just an API.  Maybe
 * they're in the handle.  That'd be an optimization for the case where a
 * handle is kept across a number of get/set calls.
 *
 * Versioning.  In table name alone for now?
 *
 * Apple's API assumes app's provide their own IDs and pass them in.  Do they
 * do security checks?
 *
 * There's a sync() call.
 *
 * There's a heirarchy of prefs databases.  Any_app provides some default
 * values that apps may override.
 *
 * What goes across the bus is always json.  lunaprop has a shell mode that
 * makes things non-json.
 *
 * A note on overlapping system properties: files named KEY in /etc/properties and
 * containing VALUE will be treated as com.palm.properties.KEY,VALUE pairs --
 * and they'll trump any pair with the same key coming from anywhere else.
 * The implementation keeping keys unique when building lists sucks -- it's
 * N^^2, and in particular doesn't use json objects which locate keys using a
 * hashtable.  But I did a test with 1000 files in /etc/properties and the command
 * 'lunaprop -a' returns in 1.23 seconds.  With only 100 such files, a more
 * realistic scenario, it's .15 seconds.  Without the -a there's less work and
 * so it's faster.  I figure it's good enough for its intended use and for
 * now.
 */

#define LUNAPREFS_DEBUG
#ifdef LUNAPREFS_DEBUG
# define LOG_IN()   fprintf( stderr, "entering %s()\n", __func__ )
# define LOG_OUT(res)   fprintf( stderr, "%s()=>%d\n", __func__, (res) )
#else
# define LOG_IN()
# define LOG_OUT(res)
#endif

#define PROPS_DIR "/etc/prefs/properties"
#define WHITELIST_PATH "/etc/prefs/public_properties"
#define TOKENS_DIR "/dev/tokens"

/* properties from the build info file */
#define BUILD_INFO_PATH "/etc/palm-build-info"
#define INFO_NAME_VERSION "version" /* as specified by user */
#define INFO_NAME_BUILDNAME "buildName" /* as specified by user */
#define INFO_NAME_BUILDNUMBER "buildNumber" /* as specified by user */

#define INFO_KEY_VERSION "PRODUCT_VERSION_STRING"  /* as found in BUILD_INFO_PATH */
#define INFO_KEY_BUILDNAME "BUILDNAME"
#define INFO_KEY_BUILDNUMBER "BUILDNUMBER"

/* properties derived from runtime info */
#define PROP_NAME_NDUID           "nduid"
#define PROP_NAME_BOARDTYPE       "boardType"
#define PROP_NAME_DISKSIZE        "storageCapacity"
#define PROP_NAME_FREESPACE       "storageFreeSpace"
#define PROP_NAME_PREVPANIC       "prevBootPanicked"
#define PROP_NAME_PREVSHUTCLEAN   "prevShutdownClean"


static const char* PALM_TOKEN_PREFIX = "com.palm.properties.";

typedef struct LPAppHandle_t {
    gchar*   pPath;
    sqlite3* pDb;
} LPAppHandle_t;

static LPErr openDB( LPAppHandle_t* handle );
static LPErr addTable( LPAppHandle_t* handle );
static LPErr LPSystemCopyAllCJ_impl( struct json_object** json, 
                                     bool onPublicBus );
static LPErr LPSystemCopyKeysCJ_impl( struct json_object** json, 
                                      bool onPublicBus );
static bool systemKeyIsPublic( const char* key );


static bool
is_toplevel_json( const struct json_object* jobj )
{
    enum json_type typ = json_object_get_type( jobj );
    return (typ == json_type_object) || (typ == json_type_array);
}

static bool
check_is_json( const char* text )
{
    struct json_object* jobj = json_tokener_parse( text );
    bool isJson = !is_error( jobj );
    if ( isJson ) {
        isJson = is_toplevel_json( jobj );
        json_object_put( jobj );
    }

    return isJson;
}

static struct json_object*
keyValueAsObject( const char* key, const char* value )
{
    struct json_object* jobject = json_object_new_object();
    struct json_object* jvalue = json_tokener_parse( value );
    if ( is_error( jvalue ) ) {
        jvalue = json_object_new_string( value );
    } else if ( !is_toplevel_json(jvalue) ) {   /* not a legal document string */
        json_object_put( jvalue );
        jvalue = json_object_new_string( value );
    }
    g_assert( !is_error(jvalue) );
    json_object_object_add(jobject, key, jvalue);

    return jobject;
}

static bool
keyFoundInArray( struct json_object* array, const char* key )
{
    bool found = false;
    int len = NULL == array ? 0 : json_object_array_length( array );
    int ii;

    for ( ii = 0; ii < len && !found; ++ii ) {
        struct json_object* pair = json_object_array_get_idx( array, ii );
        found = NULL != json_object_object_get( pair, key );
    }
    return found;
}

static void
addPairToArray( struct json_object** array, struct json_object* pair )
{
    if ( !*array ) {
        *array = json_object_new_array();
    }

    int res = json_object_array_add( *array, pair );
    g_assert( res == 0 );
}

static LPErr
strToJsonWithCheck( const char* jstr, struct json_object** json )
{
    LPErr err = LP_ERR_NONE;
    struct json_object* tmp = json_tokener_parse( jstr );
    if ( !is_error( tmp ) && is_toplevel_json( tmp ) )
    {
        *json = tmp;
    }
    else
    {
        g_critical( "string \"%s\" not acceptable to cjson or not a json doc", jstr );
        err = LP_ERR_VALUENOTJSON;
    }
    return err;
}

static LPErr
sqlerr_to_lperr( int err )
{
    LPErr lperr;
    if ( err == SQLITE_OK ) {
        lperr = LP_ERR_NONE;
    } else if ( SQLITE_BUSY == err ) {
        lperr = LP_ERR_BUSY;
    } else {
        lperr = LP_ERR_DBERROR;
    }
    return lperr;
}

static LPErr
runSQL( LPAppHandle_t* handle, bool canAddTable,
        int (*callback)(void*,int,char**,char**), void* context,
        const char* fmt, ... )
{
    LPErr lperr = openDB( handle );
    if ( LP_ERR_NONE == lperr ) {
        va_list ap;
        va_start( ap, fmt );
        char* stmt = sqlite3_vmprintf( fmt, ap );
        va_end( ap );
        g_assert( !!stmt );
        char* errmsg;
        int err;
    again:
        errmsg = NULL;
        err = sqlite3_exec( handle->pDb, stmt,
                            callback, context,
                            &errmsg );

        if ( SQLITE_OK != err ) 
        {
            if ( SQLITE_ERROR == err && canAddTable )
            {
                if ( LP_ERR_NONE == addTable( handle ) )
                {
                    sqlite3_free( errmsg ); /* no-op if NULL */
                    canAddTable = false;
                    goto again;
                }
            }
            if ( NULL != errmsg )
            {
                fprintf( stderr, "sqlite3_exec(\"%s\")=>%d/\"%s\"\n", stmt, err, errmsg );
                sqlite3_free (errmsg );
            }
        }

        lperr = sqlerr_to_lperr( err );
    
        sqlite3_free( stmt );
    }
    return lperr;
} /* runSQL */

static LPErr
addTable( LPAppHandle_t* handle )
{
    LPErr err = runSQL( handle, false, NULL, NULL, 
                        "CREATE TABLE IF NOT EXISTS data( key TEXT PRIMARY KEY, value TEXT );" );
    return err;
}

/* 
 * Open the sqlite DB if it isn't already open.  Since there are ways to wind
 * up with a DB file that exists but doesn't have a table, we're prepared to
 * add a table in reponse to errors on read or write.  Thus we don't add one
 * here.
 */
static LPErr
openDB( LPAppHandle_t* handle )
{
    LPErr err = LP_ERR_NONE;
    if ( handle->pDb == NULL ) {
        if ( handle->pPath == NULL ) {
            err = LP_ERR_INVALID_HANDLE;
        } else {
            g_mkdir_with_parents( handle->pPath, O_RDWR );
            gchar* fullPath = g_strdup_printf( "%s/prefsDB.sl", handle->pPath );

            sqlite3* pDb;
            int result = sqlite3_open( fullPath, &pDb );
            if ( result == 0 ) {
                handle->pDb = pDb; /* assign this before calling runSQL()!!! */

                err = runSQL( handle, false, NULL, NULL, "BEGIN;" ); /* begin a transaction */
            } else {
                err = sqlerr_to_lperr( result );
            }
            g_free( fullPath );
        }
    }
    return err;
} /* openDB */

LPErr
LPAppClearData( const char* appId )
{
    gchar* path = g_strdup_printf( "/var/preferences/%s/prefsDB.sl", appId );
    int err = unlink( path );
    g_free( path );
    return (err == 0)? LP_ERR_NONE : LP_ERR_PARAM_ERR;
}

LPErr
LPAppGetHandle( const char* appId, LPAppHandle* handle )
{
    LPErr err = -EINVAL;
    gchar* path = NULL;
    g_return_val_if_fail( handle != NULL, -EINVAL );
    g_return_val_if_fail( appId != NULL, -EINVAL );

    path = g_strdup_printf( "/var/preferences/%s", appId );
    if ( NULL == path ) goto error; 

    LPAppHandle_t* hndl = g_new0( LPAppHandle_t, 1 );
    if ( NULL == hndl ) goto error; 
    hndl->pPath = path;

    *handle = (LPAppHandle)hndl;

    err = 0;
 error:
    return err;
} /* LPAppGetHandle */

LPErr
LPAppFreeHandle( LPAppHandle handle, bool commit )
{
    LPErr lperr = LP_ERR_NONE;
    g_return_val_if_fail( handle != NULL, -EINVAL );
    LPAppHandle_t* hndl = (LPAppHandle_t*)handle;

    if ( hndl->pDb ) {
        lperr = runSQL( handle, false, NULL, NULL, "%s;", (commit?"COMMIT":"ROLLBACK") );
        if ( LP_ERR_NONE == lperr ) {
            lperr = sqlerr_to_lperr(sqlite3_close( hndl->pDb ) );
            hndl->pDb = NULL;
        }
    }

    g_free( hndl->pPath );
    g_free( hndl );
    return lperr;
}

static int 
getValue( void* context, int nColumns, char** colValues, char** colNames )
{
    g_assert( nColumns == 1 );    /* I asked for one column, not '*' */
    gchar** result = (gchar**)context;
    *result = g_strdup( colValues[0] );
    return 0;      /* non-0 return aborts, but causes sqlite3_exec to return
                      SQLITE_ABORT  */
}

#ifdef USE_MJSON
static void
cjson_to_mjson_put( json_t** jsont, struct json_object* json )
{
    const char* asStr = json_object_get_string( json );
    *jsont = json_parse_document( asStr );
    json_object_put( json );
}
#endif

#ifdef USE_MJSON
static void
mjson_to_cjson( struct json_object** json, const json_t* jsont )
{
    char *text = NULL;
    enum json_error err = json_tree_to_string( (json_t*)jsont, &text );
    if ( JSON_OK == err )
    {
        *json = json_tokener_parse( text );
        g_free( text );
    } else {
        g_assert(0);            /* what to do? */
    }
}
#endif

LPErr
LPAppCopyValue( LPAppHandle handle, const char* key, char** jstr )
{
    g_return_val_if_fail( handle != NULL, -EINVAL );
    g_return_val_if_fail( key != NULL, -EINVAL );
    g_return_val_if_fail( jstr != NULL, -EINVAL );

    gchar* value = NULL;

    LPErr err = runSQL( handle, true, getValue, &value,
                        "SELECT VALUE FROM data WHERE key = \'%q\';", key );
    
    if ( err == 0 ) {
        if ( !value ) {         /* will be null if getValue() never fired */
            err = LP_ERR_NO_SUCH_KEY;
        } else if ( !check_is_json(value) ) {
            g_critical( "non-json value stored: %s", value );
            err = LP_ERR_VALUENOTJSON;
        } else {
            *jstr = value;
            value = NULL;
        }
    }

    g_free( (gchar*)value );
    return err;
}

LPErr
LPAppCopyValueString( LPAppHandle handle, const char* key, char** str )
{
    struct json_object* json;
    LPErr err = LPAppCopyValueCJ( handle, key, &json );
    if ( LP_ERR_NONE == err && json_object_is_type( json, json_type_array ) )
    {
        /* assume it's an array of length one.  Return the string at elem 0. */
        struct json_object* child = json_object_array_get_idx( json, 0 );
        
        if ( (NULL != child) && ( json_object_is_type( child, json_type_string)) )
        {
            *str = g_strdup( json_object_get_string( child ) );
        } 
        else 
        {
            err = LP_ERR_VALUENOTJSON;
        }
        json_object_put( json );
    }
    return err;
}

LPErr
LPAppCopyValueInt( LPAppHandle handle, const char* key, int* intValue )
{
    struct json_object* json;
    LPErr err = LPAppCopyValueCJ( handle, key, &json );
    if ( LP_ERR_NONE == err )
    {
        /* assume it's an array of length one.  Return the string at elem 0. */
        struct json_object* child = json_object_array_get_idx( json, 0 );
        if ( (NULL != child) && json_object_is_type( child, json_type_string) ) 
        {
            *intValue = atoi( json_object_get_string( child ) );
        } 
        else 
        {
            err = LP_ERR_VALUENOTJSON;
        }
        json_object_put( json );
    }

    return err;
}

#ifdef USE_MJSON
LPErr
LPAppCopyValueJ( LPAppHandle handle, const char* key, json_t** jsont )
{
    LPErr err;
    struct json_object* json;
    err = LPAppCopyValueCJ( handle, key, &json );
    if ( LP_ERR_NONE == err ) {
        cjson_to_mjson_put( jsont, json );
    }
    return err;
}
#endif

LPErr
LPAppCopyValueCJ( LPAppHandle handle, const char* key, struct json_object** json )
{
    g_return_val_if_fail( handle != NULL, -EINVAL );
    g_return_val_if_fail( key != NULL, -EINVAL );
    g_return_val_if_fail( json != NULL, -EINVAL );

    char* jstr = NULL;
    LPErr err = LPAppCopyValue( handle, key, &jstr );

    if ( LP_ERR_NONE == err ) 
    {
        err = strToJsonWithCheck( jstr, json );
    }

    g_free( jstr );
    return err;
}

static LPErr
copy_as_string( struct json_object* json, char** out )
{
    LPErr err = LP_ERR_NONE;
    const char* txt = json_object_get_string( json );
    g_assert( !!txt );
    *out = g_strdup( txt );
    return err;
}

static int
addValueToArray( void* context, int nColumns, char** colValues, char** colNames )
{
    g_assert( nColumns == 1 );
    struct json_object* jarray = (struct json_object*)context;
	struct json_object* jstr = json_object_new_string( colValues[0] );

	json_object_array_add( jarray, jstr );
    
    return 0;
} /* addValueToArray */

LPErr
LPAppCopyKeys( LPAppHandle handle, char** jstr )
{
    LPErr err = -EINVAL;
    g_return_val_if_fail( handle != NULL, -EINVAL );
    g_return_val_if_fail( jstr != NULL, -EINVAL );

	struct json_object* jarray = json_object_new_array();

    err = runSQL( handle, true, addValueToArray, jarray, "SELECT key FROM data;" );

    if ( 0 == err ) {
        err = copy_as_string( jarray, jstr );
    }

    json_object_put( jarray );
    return err;
} /* LPAppCopyKeys */

#ifdef USE_MJSON
LPErr
LPAppCopyKeysJ( LPAppHandle handle, json_t** jsont )
{
    LPErr err;
    struct json_object* json;
    err = LPAppCopyKeysCJ( handle, &json );
    if ( LP_ERR_NONE == err ) {
        cjson_to_mjson_put( jsont, json );
    }
    return err;
}
#endif

LPErr
LPAppCopyKeysCJ( LPAppHandle handle, struct json_object** json )
{
    LPErr err = -EINVAL;
    g_return_val_if_fail( handle != NULL, -EINVAL );
    g_return_val_if_fail( json != NULL, -EINVAL );

	struct json_object* jarray = json_object_new_array();

    err = runSQL( handle, true, addValueToArray, jarray, "SELECT key FROM data;" );

    if ( LP_ERR_NONE == err ) 
    {
        *json = jarray;
    } 
    else
    {
        json_object_put( jarray );
    }
    return err;
}

static int
addKeyValueToArray( void* context, int nColumns, char** colValues, char** colNames )
{
    int err = -1;
    g_assert( nColumns == 2 );
    struct json_object* jarray = (struct json_object*)context;
    struct json_object* obj = json_object_new_object();
    if ( NULL != obj ) {
        struct json_object* value = json_tokener_parse( colValues[1] );
        if ( !is_error(value) && is_toplevel_json(value) ) {
            json_object_object_add( obj, colValues[0], value );
            json_object_array_add( jarray, obj );
            err = 0;
        }
    }
    
    return err;
} /* addValueToArray */

LPErr
LPAppCopyAll( LPAppHandle handle, char** jstr )
{
    LPErr err;
    g_return_val_if_fail( handle != NULL, -EINVAL );
    g_return_val_if_fail( jstr != NULL, -EINVAL );

	struct json_object* jarray = json_object_new_array();

    err = runSQL( handle, true, addKeyValueToArray, jarray, "SELECT key,value FROM data;" );

    if ( 0 == err ) {
        err = copy_as_string( jarray, jstr );
    }

    json_object_put( jarray );
    return err;
}

#ifdef USE_MJSON
LPErr
LPAppCopyAllJ( LPAppHandle handle, json_t** jsont )
{
    LPErr err;
    struct json_object* json;
    err = LPAppCopyAllCJ( handle, &json );
    if ( LP_ERR_NONE == err ) {
        cjson_to_mjson_put( jsont, json );
    }
    return err;
}
#endif

LPErr
LPAppCopyAllCJ( LPAppHandle handle, struct json_object** json )
{
    char* jstr = NULL;
    LPErr err = LPAppCopyAll( handle, &jstr );

    if ( LP_ERR_NONE == err )
    {
        err = strToJsonWithCheck( jstr, json );
    }
    g_free( jstr );
    return err;
}

static LPErr 
setValueString( LPAppHandle handle, const char* key, const char* jstr ) 
{
        /* Use REPLACE, not INSERT, to avoid duplicates.  */
    return runSQL( handle, true, NULL, NULL,
                   "REPLACE INTO data VALUES( \'%q\', \'%q\' );", key, jstr );
}

LPErr
LPAppSetValue( LPAppHandle handle, const char* key, const char* const jstr )
{
    g_return_val_if_fail( handle != NULL, -EINVAL );
    g_return_val_if_fail( key != NULL, -EINVAL );
    g_return_val_if_fail( jstr != NULL, -EINVAL );

    LPErr err;
    if ( *key == '\0' ) {       /* empty string? */
        err = LP_ERR_ILLEGALKEY;
    } else if ( !check_is_json( jstr ) ) {
        err = LP_ERR_VALUENOTJSON;
    } else {
        /* Use REPLACE, not INSERT, to avoid duplicates.  */
        err = setValueString( handle, key, jstr );
    }
    return err;
} /* LPAppSetValue */

LPErr
LPAppSetValueString( LPAppHandle handle, const char* key, const char* const str )
{
    LPErr err = LP_ERR_MEM;

    struct json_object* array = json_object_new_array();
    if ( NULL != array )
    {
        struct json_object* jstr = json_object_new_string( str );
        if ( NULL != jstr )
        {
            json_object_array_add( array, jstr );
            err = LPAppSetValueCJ( handle, key, array );
        }
        json_object_put( array );
    }
    return err;
}

LPErr
LPAppSetValueInt( LPAppHandle handle, const char* key, int intValue )
{
    LPErr err = LP_ERR_MEM;

    struct json_object* array = json_object_new_array();
    if ( NULL != array )
    {
        char buf[32];
        snprintf( buf, sizeof(buf), "%d", intValue );
        struct json_object* jstr = json_object_new_string( buf );
        if ( NULL != jstr )
        {
            json_object_array_add( array, jstr );
            err = LPAppSetValueCJ( handle, key, array );
        }
        json_object_put( array );
    }
    return err;
} /* LPAppSetValueInt */

#ifdef USE_MJSON
LPErr
LPAppSetValueJ( LPAppHandle handle, const char* key, const json_t* jsont )
{
    g_return_val_if_fail( handle != NULL, -EINVAL );
    g_return_val_if_fail( key != NULL, -EINVAL );
    g_return_val_if_fail( jsont != NULL, -EINVAL );

    struct json_object* json;
    mjson_to_cjson( &json, jsont );
    LPErr err = LPAppSetValueCJ( handle, key, json );
    json_object_put( json );
    return err;
}
#endif

LPErr
LPAppSetValueCJ( LPAppHandle handle, const char* key, struct json_object* json )
{
    g_return_val_if_fail( handle != NULL, -EINVAL );
    g_return_val_if_fail( key != NULL, -EINVAL );
    g_return_val_if_fail( json != NULL, -EINVAL );

    LPErr err;
    if ( *key == '\0' ) {       /* empty string? */
        err = LP_ERR_ILLEGALKEY;
    } else if ( !json_object_is_type(json, json_type_object)
                && !json_object_is_type(json, json_type_array) ) {
        err = LP_ERR_VALUENOTJSON;
    } else {
        const char* jstr = json_object_get_string( json );
        if ( !!jstr ) {
            err = setValueString( handle, key, jstr );
        } else {
            g_critical( "json supplied to %s not acceptable to mjson", __func__ );
            err = LP_ERR_VALUENOTJSON;
        }
    }
    return err;
} /* LPAppSetValueCJ */

LPErr
LPAppRemoveValue( LPAppHandle handle, const char* key )
{
    g_return_val_if_fail( handle != NULL, -EINVAL );
    g_return_val_if_fail( key != NULL, -EINVAL );
    
    LPErr err = -EINVAL;

    err = runSQL( handle, true, NULL, NULL, "DELETE FROM data WHERE key = \'%q\';", key );

    return err;
}

/*****************************************************************************
* System prefs
*****************************************************************************/

/* Some tokens may be known to the system, but there's a set that is not: if
   the factory adds it, we need to pick it up without a recompile and to
   support it.  So the prefix "com.palm.properties" maps to tokens that
   trenchcoat (or any other flashing app) puts in /dev/tokens.  If a key
   begins with "com.palm.properties." then we strip that prefix and assume a
   file in /dev/tokens.  Other prefixes are treated as special cases.
 */ 

static LPErr
get_from_cmdline( char** jstr, const char* key )
{
    LPErr err = LP_ERR_SYSCONFIG;

    FILE* cmdline = fopen( "/proc/cmdline", "r" );
    if ( NULL != cmdline ) {
        char buf[4096];

        if ( buf == fgets( buf, sizeof(buf), cmdline ) ) 
        {
            char* token;
            char* str = buf;
            char* saveptr;
            int keylen = strlen( key );
            while ( NULL != (token = strtok_r( str, " ", &saveptr )) ) 
            {
                if ( 0 == strncmp( key, token, keylen )
                     && token[keylen] == '=' ) 
                {
                    *jstr = g_strdup( token + keylen + 1 ); /* skip '=' */
                    err = LP_ERR_NONE;
                    break;
                }

                str = NULL;         /* for subsequent strtok_r calls */
            }
        }
        
        fclose( cmdline );
    }

    return err;
}
static LPErr read_machine_type(char** jstr,const char* key)
{
    nyx_error_t error = NYX_ERROR_GENERIC;
    nyx_device_handle_t device = NULL;
    const char *dev_name;

    LPErr err = LP_ERR_SYSCONFIG;
    error = nyx_init();
    if (NYX_ERROR_NONE == error)
    {
        error = nyx_device_open(NYX_DEVICE_DEVICE_INFO, "Main", &device);

        if ((NYX_ERROR_NONE == error) & (NULL != device))
        {
            if ( 0 == strncmp( key, "nduid", strlen(key) ))
            {
                error = nyx_device_info_query(device, NYX_DEVICE_INFO_NDUID, &dev_name);
            }
            else if(0 == strncmp(key,"boardType",strlen(key)))
            {
                error = nyx_device_info_query(device, NYX_DEVICE_INFO_BOARD_TYPE, &dev_name);
            }
            if (NYX_ERROR_NONE == error)
            {
                *jstr = g_strdup(dev_name);
                err = LP_ERR_NONE;
            }
            nyx_device_close(device);
        }
        nyx_deinit();
    }
    return err;
}

static LPErr read_OS_Info(char **jstr,const char* key)
{
    nyx_error_t error = NYX_ERROR_GENERIC;
    nyx_device_handle_t device = NULL;
    const char *dev_name;
    LPErr err = LP_ERR_SYSCONFIG;
    error = nyx_init();
    if (NYX_ERROR_NONE == error)
    {
        error = nyx_device_open(NYX_DEVICE_OS_INFO, "Main", &device);
        if ((NYX_ERROR_NONE == error) & (NULL != device))
        {
            if ( 0 == strncmp( key, "version",strlen(key) ))
            {
                error = nyx_os_info_query(device, NYX_OS_INFO_CORE_OS_KERNEL_VERSION, &dev_name);
            }
            else if(0 == strncmp(key,"buildNumber",strlen(key)))
            {
                error = nyx_os_info_query(device, NYX_OS_INFO_WEBOS_BUILD_ID, &dev_name);
            }
            else if(0 == strncmp(key,"buildName",strlen(key)))
            {
                error = nyx_os_info_query(device, NYX_OS_INFO_WEBOS_IMAGENAME, &dev_name);
            }
            else
            {
                error = nyx_device_info_query(device, NYX_OS_INFO_WEBOS_BUILD_ID, &dev_name);
            }
            if (NYX_ERROR_NONE == error)
            {
                *jstr = g_strdup(dev_name);
                err = LP_ERR_NONE;
            }
            nyx_device_close(device);
         }
         nyx_deinit();
   }
   return err;
}
static LPErr
get_from_buildInfo( const char* fileKey, char** jstr )
{
    LPErr err = LP_ERR_NO_SUCH_KEY;

    FILE* file = fopen( BUILD_INFO_PATH, "r" );

    if ( NULL != file ) {
        for ( ; ; ) {
            char buf[128];
            char* value;

            if ( !fgets( buf, sizeof(buf)/sizeof(buf[0]), file ) ) {
                break;
            }

            value = strstr( buf, "=" );
            if ( !!value )
            {
                *value = '\0';
                ++value;        /* skip null */

                if ( 0 == strcmp( fileKey, buf ) ) {
                    int len = strlen( value );
                    while ( '\n' == value[len-1] && len > 0 ) {
                        --len;
                    }
                    value[len] = '\0';
                    *jstr = g_strdup( value );

                    err = LP_ERR_NONE;
                    break;
                }
            }
        }
        fclose( file );
    }

    return err;
} /* readBuildInfo */

static LPErr
figureDiskCapacity( char** jstr )
{
    /*
      major minor  #blocks  name

      7     0      51200 loop0
      179     0    7864320 mmcblk0            <- the one we want
      179     1       4096 mmcblk0p1
      179     2     409600 mmcblk0p2
      179     3     307200 mmcblk0p3
      179     4    7142912 mmcblk0p4
    */
    LPErr err = LP_ERR_SYSCONFIG;
    /* current format has line ending in mmcblk0, but let's allow for some
       whitespace should the formatting change. */
    FILE* file = popen( "grep 'mmcblk0\\s*$' /proc/partitions", "r" );
    if ( !!file )
    {
        int major, minor;
        long long unsigned nBlocks;
        char name[64];             /* change format specifiers if sizes changed!! */

        int nRead = fscanf( file, "%d%d%llu%63s", &major, &minor, &nBlocks, name );
        if ( 4 == nRead ) {
            nBlocks *= 1024;
            *jstr = g_strdup_printf( "%llu", nBlocks );
            err = LP_ERR_NONE;
        }
        
        pclose( file );
    }
    return err;
}

static LPErr
figureDiskFree( char** jstr )
{
    LPErr err = LP_ERR_SYSCONFIG;

    struct statfs buf;
    if ( 0 == statfs( "/media/internal", &buf ) ) {
        long long unsigned int freeBytes;
        freeBytes = buf.f_bavail * (unsigned long long) buf.f_bsize;
        *jstr = g_strdup_printf( "%llu", freeBytes );
        err = LP_ERR_NONE;
    } else {
        fprintf( stderr, "statfs=>%d (%s)\n", errno, strerror(errno) );
    }
    return err;
}

static LPErr
figurePrevPanic( char** jstr )
{
    LPErr err = LP_ERR_SYSCONFIG;
    bool panic = false;

    FILE* fil = fopen( "/proc/cmdline", "r" );
    if ( NULL != fil )
    {
        char buf[512];
        if ( NULL != fgets( buf, sizeof(buf), fil ) )
        {
            panic = (NULL != strstr( buf, "lastboot=panic" ));
            err = LP_ERR_NONE;
        }

        fclose( fil );
    }

    *jstr = g_strdup( panic? "true" : "false" );
    return err;
}

/* This, and the lunaprop command in /etc/init.d/mountall.sh, is a hack meant
 * to last until the writable system-properties work is done.  This property
 * will stay, but will be implemented in some general way -- which might well
 * mean an apps-db called com.palm.system :-)
 */
static LPErr
figureShutdownClean( char** jstr )
{
    LPAppHandle handle;
    LPErr err = LPAppGetHandle( "com.palm.system", &handle );



    if ( LP_ERR_NONE == err ) 
    {
        char* str = NULL;
        err = LPAppCopyValueString( handle, "last_umount_clean", &str );

        if ( LP_ERR_NONE == err ) 
        {
            *jstr = g_strdup( str );
        }
        else if ( err == LP_ERR_NO_SUCH_KEY )
        {
            char* tempstr = " ";
            *jstr = g_strdup(tempstr);
            err=LP_ERR_NONE ;

        }
        g_free( str );
        (void)LPAppFreeHandle( handle, false );
    }

    return err;
}

static const char* g_non_tokens[] = {
    INFO_NAME_VERSION
    ,INFO_NAME_BUILDNAME
    ,INFO_NAME_BUILDNUMBER
    ,PROP_NAME_NDUID
    ,PROP_NAME_BOARDTYPE
    ,PROP_NAME_DISKSIZE
    ,PROP_NAME_FREESPACE
    ,PROP_NAME_PREVPANIC
    ,PROP_NAME_PREVSHUTCLEAN
};

static char*
getTokenPath( const char* token, const char* dir )
{
    char* path = g_strdup_printf( "%s/%s", dir, token );
    if ( NULL != path ) 
    {
        if ( !g_file_test( path, G_FILE_TEST_EXISTS ) ) {
            g_free( path );
            path = NULL;
        }
    }
    return path;
} /* getTokenPath */

static LPErr
readFromFile( const char* path, char** jstrp )
{
    LPErr err = LP_ERR_NO_SUCH_KEY;
    char* jstr;
    GMappedFile* mf = g_mapped_file_new( path, FALSE, NULL );
    if ( NULL != mf ) 
    {
        gsize siz = g_mapped_file_get_length( mf );
        gchar buf[siz + 1];
        memcpy( buf, g_mapped_file_get_contents(mf), siz );
        //g_mapped_file_free( mf );
	g_mapped_file_unref( mf );

        buf[siz] = '\0';
        jstr = g_strdup( buf );
        err = NULL == jstr? LP_ERR_MEM : LP_ERR_NONE;
        *jstrp = jstr;
    } else {
        g_critical( "failed to open file %s", path );
    }
    return err;
} /* readFromFile */

LPErr
LPSystemCopyStringValue( const char* key, char** jstr )
{
    g_return_val_if_fail( key != NULL, -EINVAL );
    g_return_val_if_fail( jstr != NULL, -EINVAL );

    LPErr err = LP_ERR_NO_SUCH_KEY;
    const char* token = NULL;

    if ( !strncmp( PALM_TOKEN_PREFIX, key, strlen(PALM_TOKEN_PREFIX) ) ) {
        token = key + strlen(PALM_TOKEN_PREFIX);
    }

    if ( NULL != token ) {
        char* path = NULL;
        if ( NULL != (path = getTokenPath( token, PROPS_DIR )) )
        {
            /* if the file exists, we'll stop the search here, even if an
             * error is returned.  Might want to think about scenarios and
             * whether that makes sense.
            */
            err = readFromFile( path, jstr );
        } else if ( 0 == strcmp( token, PROP_NAME_NDUID ) ) {
#if MACHINE==qemux86
            gchar* standard_output;
            gchar* standard_error;
            gint exit_status;
            GError* error = NULL;
            gboolean success
                = g_spawn_command_line_sync( "cat /var/lib/nyx/nduid", &standard_output,
                                             &standard_error, &exit_status, 
                                             &error );
            if ( success && 0 == exit_status ) {
                int len = strlen(standard_output);
                if ( standard_output[len-1] == '\n' ) {
                    standard_output[len-1] = '\0';
                }
                *jstr = g_strdup( standard_output );
                g_free( standard_output );
                g_free( standard_error );
                err = LP_ERR_NONE;
            }
#else
            err = read_machine_type(jstr,"nduid");
#endif
        } else if ( 0 == strcmp( token, PROP_NAME_BOARDTYPE ) ) {
            err=read_machine_type(jstr,"boardType");
        } else if ( ! strcmp( token, INFO_NAME_VERSION ) ) {
            err=read_OS_Info(jstr,"version");
        } else if ( ! strcmp( token, INFO_NAME_BUILDNAME ) ) {
            err=read_OS_Info(jstr,"buildName");
        } else if ( ! strcmp( token, INFO_NAME_BUILDNUMBER ) ) {
            err=read_OS_Info(jstr,"buildNumber");
        } else if ( ! strcmp( token, PROP_NAME_DISKSIZE ) ) {
            err = figureDiskCapacity( jstr );
        } else if ( ! strcmp( token, PROP_NAME_FREESPACE ) ) {
            err = figureDiskFree( jstr );
        } else if ( ! strcmp( token, PROP_NAME_PREVPANIC ) ) {
            err = figurePrevPanic( jstr );
        } else if ( ! strcmp( token, PROP_NAME_PREVSHUTCLEAN ) ) {
            err = figureShutdownClean( jstr );
        } else if ( NULL != (path = getTokenPath( token, TOKENS_DIR )) ) {
            err = readFromFile( path, jstr );
        } else if ( NULL != (path = getTokenPath( token, LP_RUNTIME_DIR )) ) {
            err = readFromFile( path, jstr );
        }
        g_free( path );
    }

    return err;
} /* LPSystemCopyStringValue */

static LPErr
LPSystemCopyKeys_impl( char** jstr, bool onPublicBus )
{
    g_return_val_if_fail( jstr != NULL, -EINVAL );

    struct json_object* jarray = NULL;
    LPErr err = LPSystemCopyKeysCJ_impl( &jarray, onPublicBus );

    if ( LP_ERR_NONE == err )
    {
        const char* str = json_object_get_string( jarray );
        if ( !!str ) 
        {
            *jstr = g_strdup( str );
        } 
        else
        {
            g_critical( "json_object_get_string failed" );
            err = -EINVAL;
        }
    }

    json_object_put( jarray );
    return err;
} /* LPSystemCopyKeys */

LPErr
LPSystemCopyKeys( char** jstr )
{
    return LPSystemCopyKeys_impl( jstr, false );
    
}

LPErr
LPSystemCopyKeysPublic( char** jstr )
{
    return LPSystemCopyKeys_impl( jstr, true );
}

LPErr
LPSystemCopyKeysPublicCJ( struct json_object** json )
{
    return LPSystemCopyKeysCJ_impl( json, true );
}

static LPErr
for_each_dir_token( const char* dirpath,
                    LPErr (*proc)( const gchar* name, bool onPublicBus, void* closure ),
                    bool onPublicBus, void* closure )
{
    LPErr err = LP_ERR_NONE;
    GDir *dir = g_dir_open( dirpath, 0, NULL );
    while ( !!dir )
    {
        const gchar* name = g_dir_read_name( dir );
        if ( !name ) {
            break;
        }
        err = (*proc)( name, onPublicBus, closure );
        if ( LP_ERR_NONE != err ) {
            break;
        }
    }
    if ( NULL != dir ) { /* glib docs say NULL is ok, but code asserts !NULL */
        g_dir_close( dir );
    }
    return err;
}

static LPErr
addToArrayIfUnique( const gchar* name, bool onPublicBus, void* closure )
{
    LPErr err = LP_ERR_NONE;
    struct json_object* jarray = (struct json_object*)closure;

    gchar* val = g_strdup_printf( "%s%s", PALM_TOKEN_PREFIX, name );
    if ( !onPublicBus || systemKeyIsPublic(val) ) {

        int len = json_object_array_length( jarray );
        bool found = false;
        int ii;
        for ( ii = 0; ii < len && !found; ++ii ) {
            struct json_object* str = json_object_array_get_idx( jarray, ii );
            found = !strcmp( val, json_object_get_string(str) );
        }

        if ( found ) {
            /* do nothing; dups are not an error */
        } else {
            struct json_object* jstr = json_object_new_string( val );
            if ( 0 != json_object_array_add( jarray, jstr ) ) 
            {
                /* Am I leaking jstr in this case?  We're probably hosed anyway. */
                err = -EINVAL;
            }
        }
    }
    g_free( val );
    return err;
} /* addToArrayIfUnique */

#ifdef USE_MJSON
LPErr
LPSystemCopyKeysJ( json_t** jsont )
{
    LPErr err;
    struct json_object* json;
    err = LPSystemCopyKeysCJ( &json );
    if ( LP_ERR_NONE == err ) {
        cjson_to_mjson_put( jsont, json );
    }
    return err;
}
#endif

static LPErr
LPSystemCopyKeysCJ_impl( struct json_object** json, bool onPublicBus )
{
    g_return_val_if_fail( json != NULL, -EINVAL );

    LPErr err = LP_ERR_NONE;

	struct json_object* jarray = json_object_new_array();
    if ( NULL == jarray ) {
        err = LP_ERR_MEM;
        goto err;
    }

    err = for_each_dir_token( PROPS_DIR, addToArrayIfUnique, onPublicBus, jarray );
    if ( LP_ERR_NONE == err ) {
        err = for_each_dir_token( TOKENS_DIR, addToArrayIfUnique, onPublicBus, jarray );
        if ( LP_ERR_NONE == err ) {
            err = for_each_dir_token( LP_RUNTIME_DIR, addToArrayIfUnique, onPublicBus, jarray );
        }
    }
    int ii;
    for ( ii = 0; 
          LP_ERR_NONE == err && ii < sizeof(g_non_tokens)/sizeof(g_non_tokens[0]);
          ++ii ) 
    {
        err = addToArrayIfUnique( g_non_tokens[ii], onPublicBus, jarray );
    }

    if ( LP_ERR_NONE == err )
    {
        *json = jarray;
    } 
    else
    {
        json_object_put( jarray );
    }
 err:
    return err;
}

LPErr
LPSystemCopyKeysCJ( struct json_object** json )
{
    return LPSystemCopyKeysCJ_impl( json, false );
} /* LPSystemCopyKeysCJ */

static LPErr
LPSystemCopyAll_impl( char** jstr, bool onPublicBus )
{
    g_return_val_if_fail( jstr != NULL, -EINVAL );

    struct json_object* array = NULL;
    LPErr err = LPSystemCopyAllCJ_impl( &array, onPublicBus );
    
    if ( LP_ERR_NONE == err ) {
        const char* str = json_object_get_string( array );
        g_assert( NULL != str );
        *jstr = g_strdup(str);

        json_object_put( array );
    } else {
        g_critical( "LPSystemCopyAllJ=>%d", err );
    }

    return err;
}

LPErr
LPSystemCopyAll( char** jstr )
{
    return LPSystemCopyAll_impl( jstr, false );
}


LPErr
LPSystemCopyAllPublic( char** jstr )
{
    return LPSystemCopyAll_impl( jstr, true );
}

LPErr
LPSystemCopyAllPublicCJ( struct json_object** json )
{
    return LPSystemCopyAllCJ_impl( json, true );
}

static LPErr
addValToArray( const gchar* name, bool onPublicBus, void* closure )
{
    struct json_object* array = (struct json_object*)closure;
    char* value = NULL;
    LPErr err = LP_ERR_NONE;

    gchar* key = g_strdup_printf( "%s%s", PALM_TOKEN_PREFIX, name );
    if ( !onPublicBus || systemKeyIsPublic( key ) ) {

        if ( !keyFoundInArray( array, key ) ) {
            err = LPSystemCopyStringValue( key, &value );
            if ( LP_ERR_NONE == err ) 
            {
                struct json_object* pair = keyValueAsObject( key, value );
                addPairToArray( &array, pair );
                g_free( (gchar*)value );
            }
        }
    }
    g_free( key );
    return err;
}

#ifdef USE_MJSON
LPErr
LPSystemCopyAllJ( json_t** jsont )
{
    LPErr err;
    struct json_object* json;
    err = LPSystemCopyAllCJ( &json );
    if ( LP_ERR_NONE == err ) {
        cjson_to_mjson_put( jsont, json );
    }
    return err;
}
#endif

static LPErr
LPSystemCopyAllCJ_impl( struct json_object** json, bool onPublicBus )
{
    g_return_val_if_fail( json != NULL, -EINVAL );

    LPErr err = -EINVAL;
    int ii;
    struct json_object* array = json_object_new_array();

    err = for_each_dir_token( PROPS_DIR, addValToArray, onPublicBus, array );
    if ( LP_ERR_NONE == err ) {
        err = for_each_dir_token( TOKENS_DIR, addValToArray, onPublicBus, array );
        if ( LP_ERR_NONE == err ) {
            err = for_each_dir_token( LP_RUNTIME_DIR, addValToArray, onPublicBus, array );
        }
    }
    for ( ii = 0; 
          LP_ERR_NONE == err && ii < sizeof(g_non_tokens)/sizeof(g_non_tokens[0]); 
          ++ii ) 
    {
        err = addValToArray( g_non_tokens[ii], onPublicBus, array );
    }

    if ( LP_ERR_NONE == err ) {
        *json = array;
    } else {
        json_object_put( array );
    }

    return err;
} /* LPSystemCopyAllJ */

LPErr
LPSystemCopyAllCJ( struct json_object** json )
{
    return LPSystemCopyAllCJ_impl( json, false );
}    

LPErr
LPSystemCopyValue( const char* key, char** jstr )
{
    g_return_val_if_fail( key != NULL, -EINVAL );
    g_return_val_if_fail( jstr != NULL, -EINVAL );

    struct json_object* json;
    LPErr err = LPSystemCopyValueCJ( key, &json );

    if ( (0 == err) && (NULL != json) ) 
    {
        err = copy_as_string( json, jstr );
        json_object_put( json );
    }

    return err;
} /* LPSystemCopyValue */

#ifdef USE_MJSON
LPErr
LPSystemCopyValueJ( const char* key, json_t** jsont )
{
    LPErr err;
    struct json_object* json;
    err = LPSystemCopyValueCJ( key, &json );
    if ( LP_ERR_NONE == err ) {
        cjson_to_mjson_put( jsont, json );
    }
    return err;
}
#endif

LPErr
LPSystemCopyValueCJ( const char* key, struct json_object** json )
{
    char* jstr = NULL;
    LPErr err = LPSystemCopyStringValue( key, &jstr );
    if ( LP_ERR_NONE == err )
    {
        struct json_object* array = json_object_new_array();
        struct json_object* str = json_object_new_string( jstr );
        json_object_array_add( array, str );
        *json = array;
    }
    g_free( jstr );
    return err;
}

LPErr
LPSystemKeyIsPublic( const char* key, bool* allowedOnPublicBus )
{
    LPErr err = LP_ERR_NONE;
    gboolean found = false;
    static GHashTable* sHash = NULL;

    if ( !sHash ) {
        /* Keep a hashtable for faster lookup.  Don't worry about deleting:
           just let the OS reclaim process memory on exit.  As to the data
           changing, no worries there either: this file is owned by our
           package and so we'll always be restarted after an update.

           Note that this function will only get called in response to
           activity on the public bus.  When the C API is used by clients
           other than the service, e.g. by lunaprop, this hash table will
           never get created.  It's only the long-running service that will
           need it.  So it's not a waste.
        */
        sHash = g_hash_table_new( g_str_hash, g_str_equal );
        g_assert( NULL != sHash );
        
        if ( NULL != sHash ) {
            FILE* fp = fopen( WHITELIST_PATH, "r" );
            if ( NULL != fp ) {
                char buf[128];
                while ( NULL != fgets( buf, sizeof(buf), fp ) ) {
                    size_t len = strlen(buf) - 1;
                    g_assert( buf[len] == '\n' ); /* let's catch too-long key names early */
                    buf[len] = '\0';

                    /* no dupes, please */
                    g_assert( !g_hash_table_lookup_extended( sHash, buf, NULL, NULL ) );
                    g_hash_table_insert( sHash, g_strdup(buf), NULL );
                }
                fclose( fp );
            }
        }
    }

    if ( NULL != sHash ) {
        found = g_hash_table_lookup_extended( sHash, key, NULL, NULL );
    }

    *allowedOnPublicBus = found;
    return err;
}

static bool
systemKeyIsPublic( const char* key )
{
    bool onWhitelist;
    LPErr err = LPSystemKeyIsPublic( key, &onWhitelist );
    return LP_ERR_NONE == err && onWhitelist;
}


LPErr
LPErrorString( LPErr err, char** str )
{
    LPErr result = LP_ERR_NONE;
    g_return_val_if_fail( str != NULL, -EINVAL );

    gchar* msg = NULL;
    switch( err ) {
    case LP_ERR_NONE:
        msg = "no error";
        break;
    case LP_ERR_INVALID_HANDLE:
        msg = "invalid handle";
        break;
    case LP_ERR_NO_SUCH_KEY:
        msg = "no such key";
        break;
    case LP_ERR_MEM:
        msg = "unable to allocate memory";
        break;
    case LP_ERR_NOSUCHERR:
        msg = "unknown error code";
        break;
    case LP_ERR_BUSY:
        msg = "underlying database is busy";
        break;
    case LP_ERR_NOTIMPL:
        msg = "unimplemented";
        break;
    case LP_ERR_VALUENOTJSON:
        msg = "illegal value (not a json document)";
        break;
    case LP_ERR_ILLEGALKEY:
        msg = "illegal key";
        break;
    case LP_ERR_SYSCONFIG:
        msg = "required system resource is missing";
        break;
    case LP_ERR_PARAM_ERR:
        msg = "general parameter error";
        break;
    case LP_ERR_INTERNAL:
        msg = "unspecified failure occurred";
        break;
    case LP_ERR_DBERROR:
        msg = "unspecified sqlite3 error";
        break;
    }

    if ( !msg ) {
        result = LP_ERR_NOSUCHERR;
    } else {
        *str = g_strdup( msg );
    }
    return result;
}
