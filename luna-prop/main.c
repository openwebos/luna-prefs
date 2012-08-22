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

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <glib.h>
#include <unistd.h>
#include <stdarg.h>
#include <cjson/json.h>
#include <getopt.h>

#include "lunaprefs.h"

static void
usage( char** namep, const char* fmt, ... )
{
    va_list ap;
    va_start(ap, fmt);
    gchar* message = fmt? g_strdup_vprintf( fmt, ap ) : NULL;
    va_end(ap);

    const char* name = *namep;
    if ( message ) {
        fprintf( stderr, "Error: %s.\n", message );
    }
    fprintf( stderr, 
             "usage: %s \\\n"
             "    [-n appID]              # operate on appID props (otherwise on sys props) \\\n"
             "    [-m]                    # shell mode \\\n"
             "    [[-k] key_name          # print (or delete, with -k) entry_for_key \\\n"
             "        |-s key_name value  # set value for key_name \\\n"
             "        |-a ]               # dump all key/value pairs \\\n"
             , name );
    fprintf( stderr, "\teg: %s -n com.palm.browser\n", name );
    fprintf( stderr, "\teg: %s -n com.palm.browser currentURL\n", name );
    fprintf( stderr, "\teg: %s com.palm.properties.installer\n", name );
    fprintf( stderr, "\teg: %s com.palm.properties.installer -a\n", name );

    g_free( message );
    exit( 0 );
}

static void
dumpArray( bool shellMode, bool all, const char* value )
{
    if ( shellMode ) {
        struct json_object* array = json_tokener_parse( value );
        g_assert( !is_error(array) );
        g_assert( json_object_is_type( array, json_type_array ) );

        int len = json_object_array_length( array );
        int ii;
        for ( ii = 0; ii < len; ++ii ) {
            struct json_object* child = json_object_array_get_idx( array, ii );
            char* str;

            if ( all && json_object_is_type( array, json_type_object ) ) { /* why test for all? */
                str = json_object_to_json_string( child );
            } else {
                str = json_object_get_string( child );
            }
            fprintf( stdout, "%s ", str );
        }

        json_object_put( array );
    } else {
        fprintf( stdout, "%s\n", value );
    }
}

int
main( int argc, char** argv )
{
    const char* appId = NULL;
    const char* key = NULL;
    bool delete = false;
    bool set = false;
    bool all = false;
    bool shellMode = false;
    char* setValue = NULL;
    int exclusives = 0;
    gchar* freeMe = NULL;

    for ( ; ; ) {
        int opt = getopt( argc, argv, "a?hk:mn:s:" );
        if ( opt == -1 ) {
            break;
        }
        switch( opt ) {
        case 'a':
            all = true;
            ++exclusives;
            break;
        case 'h':
        case '?':
            usage( argv, NULL );
            break;
        case 'm':
            shellMode = true;
            break;
        case 'n':
            appId = optarg;
            break;
        case 'k':
            delete = true;
            key = optarg;
            ++exclusives;
            break;
        case 's':
            set = true;
            key = optarg;
            ++exclusives;
            break;
        default:
            usage( argv, "unknown argument" );
            break;
        }
    }

    if ( optind < argc ) {      /* any params left? */
        if ( !!key ) {
            setValue = argv[optind++];
        } else {
            key = argv[optind++];
        }
    }

    if ( set && !appId ) {
        usage( argv, "system properties are read-only; use -n" );
    } else if ( exclusives > 1 ) {
        usage( argv, "pass at most 1 of -a, -k and -s" );
    } else if ( set && !setValue ) {
        usage( argv, "need value to set" );
    } else if ( delete && setValue ) {
        usage( argv, "too many arguments" );
    } else if ( all && !!key ) {
        usage( argv, "nothing to do with \"%s\"", key );
    } else if ( optind < argc ) {
        usage( argv, "too many arguments" );
    }

    if ( !!setValue && shellMode ) {
        /* coerce it into a json if it isn't already */
        struct json_object* tree = json_tokener_parse( setValue );
        enum json_type typ;
        if ( is_error(tree) ) {
            typ = json_type_null;
        } else {
            typ = json_object_get_type( tree );
            json_object_put( tree );
        }

        if ( typ != json_type_object && typ != json_type_array ) {
            tree = json_object_new_array();
            g_assert( !!tree );
            struct json_object* str = json_object_new_string(setValue);
            int err = json_object_array_add( tree, str );
            g_assert( 0 == err );
            setValue = g_strdup( json_object_to_json_string( tree ) );
            freeMe = setValue;
            json_object_put( tree );
        }
    }

    gchar* value = NULL;
    LPErr err;

    if ( NULL != appId ) {
        LPAppHandle handle;
        err = LPAppGetHandle( appId, &handle );
        if ( err == 0 ) {
            if ( NULL == key ) {
                if ( all ) {
                    err = LPAppCopyAll( handle, &value );
                } else {
                    err = LPAppCopyKeys( handle, &value );
                }
            } else if ( delete ) {
                err = LPAppRemoveValue( handle, key );
            } else if ( set ) {
                err = LPAppSetValue( handle, key, setValue );
            } else if ( shellMode) {
                err = LPAppCopyValueString( handle, key, &value );
            } else {
                err = LPAppCopyValue( handle, key, &value );
            }
            (void)LPAppFreeHandle( handle, delete || set );
        }
    } else {
        if ( NULL == key ) {
            if ( all ) {
                err = LPSystemCopyAll( &value );
            } else {
                err = LPSystemCopyKeys( &value );
            }
        } else if ( shellMode ) {
            err = LPSystemCopyStringValue( key, &value );
        } else {
            err = LPSystemCopyValue( key, &value );
        }
    }

    if ( 0 == err) 
    {
        if ( value != NULL ) {
            if ( NULL == key ) {
                dumpArray( shellMode, all, value );
            } else if ( delete ) {
                usage( argv, NULL );
            } else {
                fprintf( stdout, "%s", value ); /* no carriage return */
            }
            fprintf( stdout, "\n" );
            fsync( 1 /*stdout*/ );
            g_free( (gchar*)value );                /* we're exiting; why bother? */
        }
    } else {
        char* msg = NULL;
        LPErrorString( err, &msg );
        fprintf( stderr, "error: %s\n", msg );
        g_free( (char*)msg );
    }

    g_free( freeMe );

    return (0 == err) ? 0 : 1;
} /* main */
