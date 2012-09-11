/* @@@LICENSE
*
*      Copyright (c) 2008-2012 Hewlett-Packard Development Company, L.P.
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

#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <glib.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <syslog.h>

#include <lunaservice.h>
#include <lunaprefs.h>
#include <cjson/json.h>

// #define DROP_DEPRECATED         /* Define to disable the older, deprecated method names */

static GMainLoop *g_mainloop = NULL;
static int sLogLevel = G_LOG_LEVEL_MESSAGE;
static bool sUseSyslog = false;
#define EXIT_TIMER_SECONDS 30

#define FREE_IF_SET(lserrp)                     \
    if ( LSErrorIsSet( lserrp ) ) {             \
        LSErrorFree( lserrp );                  \
    }

static void
term_handler( int signal )
{
    g_main_loop_quit( g_mainloop );
}

static gboolean            
sourceFunc( gpointer data )
{
    g_debug( "%s()", __func__ );
    g_main_loop_quit( g_mainloop );
    return false;
}

static void
reset_timer( void )
{
    g_debug( "%s()", __func__ );
    static GSource* s_source = NULL;

    if ( NULL != s_source ) {
        g_source_destroy( s_source );
    }

    s_source = g_timeout_source_new_seconds( EXIT_TIMER_SECONDS );
    g_source_set_callback( s_source, sourceFunc, NULL, NULL );
    (void)g_source_attach( s_source, NULL );
}


static void
errorReplyStr( LSHandle* lsh, LSMessage* message, const char* errString )
{
    LSError lserror;

    if ( !errString ) {
        errString = "error text goes here";
    }

    char* errJson = g_strdup_printf( "{\"returnValue\": false, \"errorText\": \"%s\"}", errString );
    g_debug( "sending error reply: %s", errJson );

    LSErrorInit( &lserror );
    if ( !LSMessageReply( lsh, message, errJson, &lserror ) )
    {
        g_critical( "error from LSMessageReply: %s", lserror.message );
        LSErrorPrint( &lserror, stderr );
    }
    FREE_IF_SET(&lserror);
    g_free( errJson );
} /* errorReply */

static void
errorReplyStrMissingParam( LSHandle* lsh, LSMessage* message, const char* param )
{
    char* msg = g_strdup_printf( "Missing required parameter \"%s\".", param );
    errorReplyStr( lsh, message, msg );
    g_free( msg );
}

static void
errorReplyErr( LSHandle* lsh, LSMessage* message, LPErr err )
{
    if ( LP_ERR_NONE != err ) {
        char* errMsg = NULL;
        (void)LPErrorString( err, &errMsg );
        errorReplyStr( lsh, message, errMsg );
        g_free( errMsg );
    }
}

static void
successReply( LSHandle* lsh, LSMessage* message )
{
    LSError lserror;
    const char* answer = "{\"returnValue\": true}";

    LSErrorInit( &lserror );
    if ( !LSMessageReply( lsh, message, answer, &lserror ) )
    {
        g_critical( "error from LSMessageReply: %s", lserror.message );
        LSErrorPrint( &lserror, stderr );
    }
    FREE_IF_SET(&lserror);
} /* successReply */

static bool
parseMessage( LSMessage* message, const char* firstKey, ... )
{
    bool success = false;
    const char* str = LSMessageGetPayload( message );
    if ( NULL != str ) {
        struct json_object* doc = json_tokener_parse( str );
        if ( !is_error(doc) ) {
            va_list ap;
            va_start( ap, firstKey );

            const char* key;
            for ( key = firstKey; !!key; key = va_arg(ap, char*) ) {
                enum json_type typ = va_arg(ap, enum json_type);
                g_assert( typ == json_type_string );

                char** out = va_arg(ap, char**);
                g_assert( out != NULL );
                *out = NULL;
                
                struct json_object* match = json_object_object_get( doc, key );
                if (NULL == match) {
                    goto error;
                }
                g_assert( json_object_is_type( match, typ ) );
                *out = g_strdup( json_object_get_string( match ) );
            }
            success = key == NULL; /* reached the end of arglist correctly */
        error:
            va_end( ap );
            json_object_put( doc );
        }
    }

    return success;
} /* parseMessage */

static void
add_true_result( struct json_object* obj )
{
    json_object_object_add( obj, "returnValue", 
                            json_object_new_boolean( true ) );
}

static bool
replyWithValue( LSHandle* sh, LSMessage* message, LSError* lserror,
                const gchar* value )
{
    g_assert( !!value );
    g_debug( "%s(%s)", __func__, value );
    return LSMessageReply( sh, message, value, lserror );
} /* replyWithValue */

static bool
replyWithKeyValue( LSHandle* sh, LSMessage* message, LSError* lserror,
                   const gchar* key, const gchar* value )
{
    g_assert( !!value );
    struct json_object* jsonVal = json_tokener_parse( value );

    /* If it doesn't parse, it's probably just a string.  Turn it into a json string */
    if ( is_error(jsonVal) ) 
    {
        jsonVal = json_object_new_string( value );
    }
    else 
    {
        enum json_type typ = json_object_get_type( jsonVal );
        if ( (typ != json_type_object) && (typ != json_type_array) )
        {
            json_object_put( jsonVal );
            jsonVal = json_object_new_string( value );
        }
    }

    struct json_object* result = json_object_new_object();
    g_assert( !!result );
    g_assert( !!key );
    json_object_object_add( result, key, jsonVal );

    add_true_result( result );

    const char* text = json_object_to_json_string( result );
    g_assert( !!text );
    bool success = replyWithValue( sh, message, lserror, text );

    json_object_put( result );

    return success;
} /* replyWithKeyValue */

static struct json_object*
wrapArray( struct json_object* jarray )
{
    g_debug( "%s", __func__ );
    g_assert( json_type_array == json_object_get_type( jarray ) );
    struct json_object* result = json_object_new_object();
    json_object_object_add( result, "values", jarray );
    add_true_result( result );
    return result;
}

typedef LPErr (*SysGetter)( struct json_object** json );

static bool
sysGet_internal( LSHandle* sh, LSMessage* message, SysGetter getter, 
                 bool asObj )
{
    LPErr err;
    bool retVal = false;

    struct json_object* json = NULL;
    err = (*getter)( &json );
    if ( 0 != err ) goto error;

    if ( asObj ) {
        json = wrapArray( json );
    }

    const char* jstr = json_object_to_json_string( json );

    LSError lserror;
    LSErrorInit( &lserror );
    retVal = LSMessageReply( sh, message, jstr, &lserror );
    if ( !retVal ) {
        LSErrorPrint( &lserror, stderr );
    }
    FREE_IF_SET (&lserror);

 error:
    errorReplyErr( sh, message, err );
    g_free( (gchar*)json );
    return true;
} /* sysGet_internal */

static bool
sysGetKeys_impl( LSHandle* sh, LSMessage* message, void* user_data, bool asObj )
{
    LSPalmService* psh = (LSPalmService*)user_data;
    bool isPublic = LSMessageIsPublic(psh, message );
    g_debug( "%s(%s)", __func__, LSMessageGetPayload(message) );
    return sysGet_internal( sh, message, 
                            isPublic ? LPSystemCopyKeysPublicCJ
                            : LPSystemCopyKeysCJ, asObj );
}

static bool
sysGetKeys( LSHandle* sh, LSMessage* message, void* user_data )
{
    reset_timer();
    return sysGetKeys_impl( sh, message, user_data, false );
}

static bool
sysGetKeysObj( LSHandle* sh, LSMessage* message, void* user_data )
{
    reset_timer();
    return sysGetKeys_impl( sh, message, user_data, true );
}


static bool
sysGetAll_impl( LSHandle* sh, LSMessage* message, void* user_data, 
                bool asObj )
{
    LSPalmService* psh = (LSPalmService*)user_data;
    bool isPublic = LSMessageIsPublic( psh, message );
    return sysGet_internal( sh, message, 
                            isPublic? LPSystemCopyAllPublicCJ:LPSystemCopyAllCJ,
                            asObj );
}

static bool
sysGetAll( LSHandle* sh, LSMessage* message, void* user_data )
{
    reset_timer();
    return sysGetAll_impl( sh, message, user_data, false );
}

static bool
sysGetAllObj( LSHandle* sh, LSMessage* message, void* user_data )
{
    reset_timer();
    return sysGetAll_impl( sh, message, user_data, true );
}

static void
addKeyValueToArray( struct json_object* array, const char* key, const char* value )
{
    struct json_object* elemOut = json_object_new_object();
    struct json_object* jsonVal = json_object_new_string( value );
    json_object_object_add( elemOut, key, jsonVal );
    (void)json_object_array_add( array, elemOut );
}

static bool
onWhitelist( const char* key )
{
    bool isPublic;
    LPErr err = LPSystemKeyIsPublic( key, &isPublic );
    g_assert( LP_ERR_NONE == err );
    return isPublic;
}

static bool
sysGetSome_impl( LSHandle* sh, LSMessage* message, void* user_data,
                 bool asObj )
{
    struct json_object* arrayOut = NULL;

    g_debug( "%s(%s)", __func__, LSMessageGetPayload(message) );
    /* Takes an array of what are meant to be property keys and returns an
     * array of key-value pairs equivalent to what Get would have returned for
     * each key.  If one of them fails an error is returned in that element of
     * the array but the rest go through.
     *
     * See ./tests/scripts/tests.sh for examples
    */

    LSPalmService* psh = (LSPalmService*)user_data;
    bool isPublic = LSMessageIsPublic( psh, message );

    const char* str = LSMessageGetPayload( message );
    if ( NULL != str ) {
        struct json_object* doc = json_tokener_parse( str );
        if ( !is_error(doc) && json_object_is_type( doc, json_type_array ) ) {
            int len = json_object_array_length( doc );
            int ii;

            arrayOut = json_object_new_array();

            for ( ii = 0; ii < len; ++ii ) 
            {
                struct json_object* key;
                struct json_object* elem = json_object_array_get_idx( doc, ii );
                if ( ( json_object_is_type( elem, json_type_object ) )
                    && (NULL != (key = json_object_object_get( elem, "key" ))) )
                {
                    char* errMsg = NULL;
                    const char* keyText = json_object_get_string( key );
                    if ( isPublic && !onWhitelist(keyText) ) {
                        (void)LPErrorString( LP_ERR_NO_SUCH_KEY, &errMsg );
                        addKeyValueToArray( arrayOut, "errorText", errMsg );
                        g_free( errMsg );
                    } else {
                        gchar* value = NULL;
                        LPErr err = LPSystemCopyStringValue( keyText, &value );
                        if ( LP_ERR_NONE == err ) {
                            addKeyValueToArray( arrayOut, keyText, value );
                        } else {
                            (void)LPErrorString( err, &errMsg );
                            addKeyValueToArray( arrayOut, "errorText", errMsg );
                            g_free( errMsg );
                        }
                    }
                } else {
                    addKeyValueToArray( arrayOut, "errorText", "missing \"key\" parameter" );
                }
            } /* for */
        }
    }

    if ( !!arrayOut )
    {
        if ( asObj ) {
            arrayOut = wrapArray( arrayOut );
        }

        const char* text = json_object_to_json_string( arrayOut );
        g_assert( !!text );

        LSError lserror;
        LSErrorInit( &lserror );
        (void)replyWithValue( sh, message, &lserror, text );

        json_object_put( arrayOut );
        FREE_IF_SET( &lserror );
    } else {
        errorReplyErr( sh, message, LP_ERR_PARAM_ERR ); /* Takes an array */
    }

    return true;
} /* sysGetSome_impl */

static bool
sysGetSome( LSHandle* sh, LSMessage* message, void* user_data )
{
    reset_timer();
    return sysGetSome_impl( sh, message, user_data, false );
}

static bool
sysGetSomeObj( LSHandle* sh, LSMessage* message, void* user_data )
{
    reset_timer();
    return sysGetSome_impl( sh, message, user_data, true );
}

static bool
sysGetValue( LSHandle* sh, LSMessage* message, void* user_data )
{
    reset_timer();
    g_debug( "%s(%s)", __func__, LSMessageGetPayload(message) );
    LPErr err = LP_ERR_NONE;

    LSPalmService* psh = (LSPalmService*)user_data;
    bool isPublic = LSMessageIsPublic( psh, message );

    gchar* key = NULL;
    if ( parseMessage( message, "key", json_type_string, &key, NULL ) 
         && ( NULL != key ) ) {
        if ( isPublic && !onWhitelist( key ) ) {
            err = LP_ERR_NO_SUCH_KEY;
        } else {
            gchar* value = NULL;
            err = LPSystemCopyStringValue( key, &value );
            if ( LP_ERR_NONE == err && NULL != value ) {
                LSError lserror;
                LSErrorInit( &lserror );
                if ( !replyWithKeyValue( sh, message, &lserror, key, value ) ) {
                    LSErrorPrint( &lserror, stderr );
                    /* TODO: how do we report this error?  We just failed to
                       reply, so attempting to reply with the error from that
                       failure will just fail again. */
/*                     err = LP_ERR_INTERNAL; */
                }
                FREE_IF_SET( &lserror );
            }
            g_free( value );
        }
    } else {
        errorReplyStr( sh, message, "missing parameter key" );
    }
    g_free( key );

    errorReplyErr( sh, message, err );
   
    return true;
} /* sysGetValue */

static LSMethod sysPropGetMethods[] = {
#ifndef DROP_DEPRECATED
   { "GetKeys", sysGetKeys },
   { "GetAll", sysGetAll },
   { "GetSome", sysGetSome },
   { "Get", sysGetValue },
#endif
   { "getSysKeys", sysGetKeys },
   { "getSysKeysObj", sysGetKeysObj },
   { "getAllSysProperties", sysGetAll },
   { "getAllSysPropertiesObj", sysGetAllObj },
   { "getSomeSysProperties", sysGetSome },
   { "getSomeSysPropertiesObj", sysGetSomeObj },
   { "getSysProperty", sysGetValue },
   { },
};

typedef LPErr (*AppGetter)( LPAppHandle handle, struct json_object** json );

static bool
appGet_internal( LSHandle* sh, LSMessage* message, AppGetter getter, bool asObj )
{
    LPErr err = LP_ERR_NONE;
    gchar* appId = NULL;
    struct json_object* json = NULL;
    LPAppHandle handle = NULL;

    if ( parseMessage( message,
                       "appId", json_type_string, &appId,
                       NULL ) ) {
        err = LPAppGetHandle( appId, &handle );
        if ( 0 != err ) goto error;

        err = (*getter)( handle, &json );
        if ( 0 != err ) goto error;

        if ( asObj ) {
            json = wrapArray( json );
        }

        LSError lserror;
        LSErrorInit( &lserror );

        if ( !LSMessageReply( sh, message, 
                              json_object_to_json_string(json),
                              &lserror ) ) {
            LSErrorPrint( &lserror, stderr );
        }
        FREE_IF_SET (&lserror);
    } else {
        errorReplyStr( sh, message, "no appId parameter found" );
    }

 error:
    errorReplyErr( sh, message, err );
    if ( !!handle ) {
        (void)LPAppFreeHandle( handle, FALSE );
    }
    g_free( appId );
    json_object_put( json );

    return true;
} /* appGetKeys */

static bool
appGetKeys( LSHandle* sh, LSMessage* message, void* user_data )
{
    reset_timer();
    g_debug( "%s(%s)", __func__, LSMessageGetPayload(message) );
    return appGet_internal( sh, message, LPAppCopyKeysCJ, false );
}

static bool
appGetKeysObj( LSHandle* sh, LSMessage* message, void* user_data )
{
    reset_timer();
    g_debug( "%s(%s)", __func__, LSMessageGetPayload(message) );
    return appGet_internal( sh, message, LPAppCopyKeysCJ, true );
}

static bool
appGetAll( LSHandle* sh, LSMessage* message, void* user_data )
{
    g_debug( "%s(%s)", __func__, LSMessageGetPayload(message) );
    reset_timer();
    return appGet_internal( sh, message, LPAppCopyAllCJ, false );
} /* appGetAll */

static bool
appGetAllObj( LSHandle* sh, LSMessage* message, void* user_data )
{
    g_debug( "%s(%s)", __func__, LSMessageGetPayload(message) );
    reset_timer();
    return appGet_internal( sh, message, LPAppCopyAllCJ, true );
} /* appGetAllObj */

static bool
appGetValue( LSHandle* sh, LSMessage* message, void* user_data )
{
    g_debug( "%s(%s)", __func__, LSMessageGetPayload(message) );
    reset_timer();

    LPErr err = -1;           /* not 0 */

    gchar* appId = NULL;
    gchar* key = NULL;
    if ( parseMessage( message,
                       "appId", json_type_string, &appId,
                       "key", json_type_string, &key,
                       NULL ) ) {
        LPAppHandle handle;
        gchar* value = NULL;

        LSError lserror;
        LSErrorInit(&lserror);

        err = LPAppGetHandle( appId, &handle );
        if ( 0 != err ) goto error;
        err = LPAppCopyValue( handle, key, &value );
        if ( 0 != err ) goto err_with_handle;
        if ( !replyWithKeyValue( sh, message, &lserror, key, value ) ) goto err_with_handle;
        err = 0;
        LSErrorPrint(&lserror, stderr);
        FREE_IF_SET (&lserror);
    err_with_handle:
        errorReplyErr( sh, message, err );
        err = LPAppFreeHandle( handle, true );
        if ( 0 != err ) goto error;
    error:
        g_free( appId );
        g_free( key );
        g_free( value );
    } else {
        errorReplyStr( sh, message, "no appId or key parameter found" );
    }
    
    return true;
} /* appGetValue */

static bool
getStringParam( struct json_object* param, char** str )
{
    bool ok = !!param 
        && json_object_is_type( param, json_type_string );
    if ( ok ) {
        *str = g_strdup( json_object_get_string( param ) );
    } 
    return ok;
}

static bool
appSetValue( LSHandle* sh, LSMessage* message, void* user_data )
{
    reset_timer();

    g_debug( "%s(%s)", __func__, LSMessageGetPayload(message) );
    bool success = false;
    LPErr err;

    struct json_object* payload = json_tokener_parse( LSMessageGetPayload( message ) );
    if ( !is_error(payload) ) {
        struct json_object* appId = json_object_object_get( payload, "appId" );
        struct json_object* key = json_object_object_get( payload, "key");
        struct json_object* value = json_object_object_get( payload, "value");
        char* appIdString;
        char* keyString;

        if ( !getStringParam( appId, &appIdString ) ) {
            errorReplyStrMissingParam( sh, message, "appId" );
        } else if ( !getStringParam( key, &keyString ) ) {
            errorReplyStrMissingParam( sh, message, "key" );
        } else if ( !value ) {
            errorReplyStrMissingParam( sh, message, "value" );
        } else {
            LPAppHandle handle;
            err = LPAppGetHandle( appIdString, &handle );
            if ( 0 != err ) goto err;

            gchar* valString = json_object_get_string( value );
            if ( !!valString ) {
                err = LPAppSetValue( handle, keyString, valString );
            } else {
                err = LP_ERR_VALUENOTJSON;
            }

            (void)LPAppFreeHandle( handle, true );
            success = LP_ERR_NONE == err;
        err:
            errorReplyErr( sh, message, err );
        }

        json_object_put( payload );
    }
    if ( success ) {
        successReply( sh, message );
    }

    return true;
} /* appSetValue */

static bool
appRemoveValue( LSHandle* sh, LSMessage* message, void* user_data )
{
    g_debug( "%s(%s)", __func__, LSMessageGetPayload(message) );
    reset_timer();

    bool retVal = false;
    gchar* appId = NULL;
    gchar* key = NULL;

    if ( parseMessage( message,
                       "appId", json_type_string, &appId,
                       "key", json_type_string, &key,
                       NULL ) ) 
    {
        LPAppHandle handle;
        LPErr err = LPAppGetHandle( appId, &handle );
        if ( LP_ERR_NONE == err ) 
        {
            err = LPAppRemoveValue( handle, key );
            (void)LPAppFreeHandle( handle, true );
            errorReplyErr( sh, handle, err );
            retVal = LP_ERR_NONE == err;
        }
    }

    if ( retVal ) {
        successReply( sh, message );
    }

    g_free( appId );
    g_free( key );
    return true;
} /* appRemoveValue */

static LSMethod appPropMethods[] = {
#ifndef DROP_DEPRECATED
   { "GetKeys", appGetKeys },
   { "GetAll", appGetAll },
   { "Get", appGetValue },
   { "Set", appSetValue },
   { "Remove", appRemoveValue },
#endif
   { "getAppKeys", appGetKeys },
   { "getAppKeysObj", appGetKeysObj },
   { "getAllAppProperties", appGetAll },
   { "getAllAppPropertiesObj", appGetAllObj },
   { "getAppProperty", appGetValue },
   { "setAppProperty", appSetValue },
   { "removeAppProperty", appRemoveValue },
   { },
};

static void
logFilter(const gchar *log_domain, GLogLevelFlags log_level, 
          const gchar *message, gpointer unused_data )
{
    if (log_level > sLogLevel) return;

    if (sUseSyslog)
    {
        int priority;
        switch (log_level & G_LOG_LEVEL_MASK) {
            case G_LOG_LEVEL_ERROR:
                priority = LOG_CRIT;
                break;
            case G_LOG_LEVEL_CRITICAL:
                priority = LOG_ERR;
                break;
            case G_LOG_LEVEL_WARNING:
                priority = LOG_WARNING;
                break;
            case G_LOG_LEVEL_MESSAGE:
                priority = LOG_NOTICE;
                break;
            case G_LOG_LEVEL_DEBUG:
                priority = LOG_DEBUG;
                break;
            case G_LOG_LEVEL_INFO:
            default:
                priority = LOG_INFO;
                break;
        }
        syslog(priority, "%s", message);
    }
    else
    {
        g_log_default_handler(log_domain, log_level, message, unused_data);
    }
} /* logFilter */

static void
usage( char** argv )
{
    fprintf( stderr,
             "usage: %s \\\n"
             "    [-d]        # enable debug logging \\\n"
             "    [-l]        # log to syslog instead of stderr \\\n"
             , argv[0] );
}

int
main( int argc, char** argv )
{
    bool retVal;
    LSError lserror;
    bool optdone = false;

    while ( !optdone )
    {
        switch( getopt( argc, argv, "dl" ) ) {
        case 'd':
            sLogLevel = G_LOG_LEVEL_DEBUG;
            break;
        case 'l':
            sUseSyslog = true;
            break;
        case -1:
            optdone = true;
            break;
        default:
            usage( argv );
            exit( 0 );
        }
    }

    g_log_set_default_handler(logFilter, NULL);

    LSErrorInit( &lserror );

    g_debug( "%s() in %s starting", __func__, __FILE__ );

    g_mainloop = g_main_loop_new( NULL, FALSE );

    /* Man pages say prefer sigaction() to signal() */
    struct sigaction sact;
    memset( &sact, 0, sizeof(sact) );
    sact.sa_handler = term_handler;
    (void)sigaction( SIGTERM, &sact, NULL );

    LSPalmService* psh;
    retVal = LSRegisterPalmService( "com.palm.preferences", &psh, &lserror);
    if (!retVal) goto error;

    LSHandle* serviceHandle_private = LSPalmServiceGetPrivateConnection( psh );

    retVal = LSPalmServiceRegisterCategory( psh, "/systemProperties",
                                            sysPropGetMethods, NULL,
                                            NULL, /* signals */
                                            psh, /* user data */
                                            &lserror );
    if (!retVal) goto error;

    /* These are private only, so use the old API */
    retVal = LSRegisterCategory( serviceHandle_private, "/appProperties", appPropMethods,
                                 NULL, /* signals */
                                 NULL, /* properties */
                                 &lserror );
    if (!retVal) goto error;

    retVal = LSGmainAttachPalmService( psh, g_mainloop, &lserror );

    g_main_loop_run( g_mainloop );
    g_main_loop_unref( g_mainloop );
    goto no_error;

 error:
    fprintf( stderr, "error from LS call: %s\n", lserror.message );
 no_error:

    (void)LSUnregisterPalmService( psh, &lserror );

    FREE_IF_SET(&lserror);

    g_debug( "%s() exiting", __func__ );
    return 0;
}
