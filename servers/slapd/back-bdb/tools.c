/* tools.c - tools for slap tools */
/* $OpenLDAP$ */
/*
 * Copyright 1998-2003 The OpenLDAP Foundation, All Rights Reserved.
 * COPYING RESTRICTIONS APPLY, see COPYRIGHT file
 */

#include "portable.h"

#include <stdio.h>
#include <ac/string.h>

#include "back-bdb.h"
#include "external.h"

static DBC *cursor = NULL;
static DBT key, data;

typedef struct dn_id {
	ID id;
	struct berval dn;
} dn_id;

#define	HOLE_SIZE	4096
static dn_id hbuf[HOLE_SIZE], *holes = hbuf;
static unsigned nhmax = HOLE_SIZE;
static unsigned nholes;

int bdb_tool_entry_open(
	BackendDB *be, int mode )
{
	/* initialize key and data thangs */
	DBTzero( &key );
	DBTzero( &data );
	key.flags = DB_DBT_REALLOC;
	data.flags = DB_DBT_REALLOC;

	return 0;
}

int bdb_tool_entry_close(
	BackendDB *be )
{
	assert( be != NULL );

	if( key.data ) {
		ch_free( key.data );
		key.data = NULL;
	}
	if( data.data ) {
		ch_free( data.data );
		data.data = NULL;
	}

	if( cursor ) {
		cursor->c_close( cursor );
		cursor = NULL;
	}

	if( nholes ) {
		unsigned i;
		fprintf( stderr, "Error, entries missing!\n");
		for (i=0; i<nholes; i++) {
			fprintf(stderr, "  entry %ld: %s\n",
				holes[i].id, holes[i].dn.bv_val, 0);
		}
		return -1;
	}
			
	return 0;
}

ID bdb_tool_entry_next(
	BackendDB *be )
{
	int rc;
	ID id;
	struct bdb_info *bdb = (struct bdb_info *) be->be_private;

	assert( be != NULL );
	assert( slapMode & SLAP_TOOL_MODE );
	assert( bdb != NULL );
	
	if (cursor == NULL) {
		rc = bdb->bi_id2entry->bdi_db->cursor(
			bdb->bi_id2entry->bdi_db, NULL, &cursor,
			bdb->bi_db_opflags );
		if( rc != 0 ) {
			return NOID;
		}
	}

	rc = cursor->c_get( cursor, &key, &data, DB_NEXT );

	if( rc != 0 ) {
		return NOID;
	}

	if( data.data == NULL ) {
		return NOID;
	}

	AC_MEMCPY( &id, key.data, key.size );
	return id;
}

Entry* bdb_tool_entry_get( BackendDB *be, ID id )
{
	int rc;
	Entry *e = NULL;
	struct berval bv;

	assert( be != NULL );
	assert( slapMode & SLAP_TOOL_MODE );
	assert( data.data != NULL );

#ifndef BDB_HIER
	DBT2bv( &data, &bv );

	rc = entry_decode( &bv, &e );

	if( rc == LDAP_SUCCESS ) {
		e->e_id = id;
	}
#else
	{
		EntryInfo *ei = NULL;
		rc = bdb_cache_find_id( be, NULL, id, &ei, 0, 0,
			NULL, NULL );
		if ( rc == LDAP_SUCCESS )
			e = ei->bei_e;
	}
#endif
	return e;
}

static int bdb_tool_next_id(
	BackendDB *be,
	DB_TXN *tid,
	Entry *e,
	struct berval *text,
	int hole,
	u_int32_t locker )
{
	struct bdb_info *bdb = (struct bdb_info *) be->be_private;
	struct berval dn = e->e_nname;
	struct berval pdn;
	EntryInfo *ei = NULL;
	int rc;

	rc = bdb_cache_find_ndn( be, tid, &dn, &ei, locker, NULL );
	if ( ei ) bdb_cache_entryinfo_unlock( ei );
	if ( rc == DB_NOTFOUND ) {
		if ( be_issuffix( be, &dn ) ) {
			pdn = slap_empty_bv;
		} else {
			dnParent( &dn, &pdn );
			e->e_nname = pdn;
			rc = bdb_tool_next_id( be, tid, e, text, 1, locker );
			if ( rc ) {
				return rc;
			}
		}
		rc = bdb_next_id( be, tid, &e->e_id );
		if ( rc ) {
			snprintf( text->bv_val, text->bv_len,
				"next_id failed: %s (%d)",
				db_strerror(rc), rc );
#ifdef NEW_LOGGING
		LDAP_LOG ( TOOLS, ERR, 
			"=> bdb_tool_entry_put: %s\n", text->bv_val, 0, 0 );
#else
		Debug( LDAP_DEBUG_ANY,
			"=> bdb_tool_entry_put: %s\n", text->bv_val, 0, 0 );
#endif
			return rc;
		}
		e->e_nname = dn;
		rc = bdb_dn2id_add( be, tid, ei, e, NULL );
		if ( rc ) {
			snprintf( text->bv_val, text->bv_len, 
				"dn2id_add failed: %s (%d)",
				db_strerror(rc), rc );
#ifdef NEW_LOGGING
		LDAP_LOG ( TOOLS, ERR, 
			"=> bdb_tool_entry_put: %s\n", text->bv_val, 0, 0 );
#else
		Debug( LDAP_DEBUG_ANY,
			"=> bdb_tool_entry_put: %s\n", text->bv_val, 0, 0 );
#endif
		} else if ( hole ) {
			if ( nholes == nhmax - 1 ) {
				if ( holes == hbuf ) {
					holes = ch_malloc( nhmax * sizeof(ID) * 2 );
					AC_MEMCPY( holes, hbuf, sizeof(hbuf) );
				} else {
					holes = ch_realloc( holes, nhmax * sizeof(ID) * 2 );
				}
				nhmax *= 2;
			}
			ber_dupbv( &holes[nholes].dn, &dn );
			holes[nholes++].id = e->e_id;
		}
	} else if ( !hole ) {
		unsigned i;

		for ( i=0; i<nholes; i++) {
			if ( holes[i].id == e->e_id ) {
				int j;
				free(holes[i].dn.bv_val);
				for (j=i;j<nholes;j++) holes[j] = holes[j+1];
				holes[j].id = 0;
				nholes--;
				break;
			} else if ( holes[i].id > e->e_id ) {
				break;
			}
		}
	}
	return rc;
}

ID bdb_tool_entry_put(
	BackendDB *be,
	Entry *e,
	struct berval *text )
{
	int rc;
	struct bdb_info *bdb = (struct bdb_info *) be->be_private;
	DB_TXN *tid = NULL;
	Operation op = {0};
	u_int32_t locker;

	assert( be != NULL );
	assert( slapMode & SLAP_TOOL_MODE );

	assert( text );
	assert( text->bv_val );
	assert( text->bv_val[0] == '\0' );	/* overconservative? */

#ifdef NEW_LOGGING
	LDAP_LOG ( TOOLS, ARGS, "=> bdb_tool_entry_put( %ld, \"%s\" )\n",
		(long) e->e_id, e->e_dn, 0 );
#else
	Debug( LDAP_DEBUG_TRACE, "=> bdb_tool_entry_put( %ld, \"%s\" )\n",
		(long) e->e_id, e->e_dn, 0 );
#endif

	rc = TXN_BEGIN( bdb->bi_dbenv, NULL, &tid, 
		bdb->bi_db_opflags );
	if( rc != 0 ) {
		snprintf( text->bv_val, text->bv_len,
			"txn_begin failed: %s (%d)",
			db_strerror(rc), rc );
#ifdef NEW_LOGGING
	LDAP_LOG ( TOOLS, ERR, "=> bdb_tool_entry_put: %s\n", text->bv_val, 0, 0 );
#else
		Debug( LDAP_DEBUG_ANY,
			"=> bdb_tool_entry_put: %s\n",
			 text->bv_val, 0, 0 );
#endif
		return NOID;
	}

	locker = TXN_ID( tid );
	/* add dn2id indices */
	rc = bdb_tool_next_id( be, tid, e, text, 0, locker );
	if( rc != 0 ) {
		goto done;
	}

	/* id2entry index */
	rc = bdb_id2entry_add( be, tid, e );
	if( rc != 0 ) {
		snprintf( text->bv_val, text->bv_len,
				"id2entry_add failed: %s (%d)",
				db_strerror(rc), rc );
#ifdef NEW_LOGGING
		LDAP_LOG ( TOOLS, ERR, 
			"=> bdb_tool_entry_put: %s\n", text->bv_val, 0, 0 );
#else
		Debug( LDAP_DEBUG_ANY,
			"=> bdb_tool_entry_put: %s\n", text->bv_val, 0, 0 );
#endif
		goto done;
	}

	op.o_bd = be;
	op.o_tmpmemctx = NULL;
	op.o_tmpmfuncs = &ch_mfuncs;
	rc = bdb_index_entry_add( &op, tid, e );
	if( rc != 0 ) {
		snprintf( text->bv_val, text->bv_len,
				"index_entry_add failed: %s (%d)",
				db_strerror(rc), rc );
#ifdef NEW_LOGGING
		LDAP_LOG ( TOOLS, ERR, 
			"=> bdb_tool_entry_put: %s\n", text->bv_val, 0, 0 );
#else
		Debug( LDAP_DEBUG_ANY,
			"=> bdb_tool_entry_put: %s\n", text->bv_val, 0, 0 );
#endif
		goto done;
	}

done:
	if( rc == 0 ) {
		rc = TXN_COMMIT( tid, 0 );
		if( rc != 0 ) {
			snprintf( text->bv_val, text->bv_len,
					"txn_commit failed: %s (%d)",
					db_strerror(rc), rc );
#ifdef NEW_LOGGING
			LDAP_LOG ( TOOLS, ERR, 
				"=> bdb_tool_entry_put: %s\n", text->bv_val, 0, 0 );
#else
			Debug( LDAP_DEBUG_ANY,
				"=> bdb_tool_entry_put: %s\n",
				text->bv_val, 0, 0 );
#endif
			e->e_id = NOID;
		}

	} else {
		TXN_ABORT( tid );
		snprintf( text->bv_val, text->bv_len,
			"txn_aborted! %s (%d)",
			db_strerror(rc), rc );
#ifdef NEW_LOGGING
		LDAP_LOG ( TOOLS, ERR, 
			"=> bdb_tool_entry_put: %s\n", text->bv_val, 0, 0 );
#else
		Debug( LDAP_DEBUG_ANY,
			"=> bdb_tool_entry_put: %s\n",
			text->bv_val, 0, 0 );
#endif
		e->e_id = NOID;
	}

	return e->e_id;
}

int bdb_tool_entry_reindex(
	BackendDB *be,
	ID id )
{
	struct bdb_info *bi = (struct bdb_info *) be->be_private;
	int rc;
	Entry *e;
	DB_TXN *tid = NULL;
	Operation op = {0};

#ifdef NEW_LOGGING
	LDAP_LOG ( TOOLS, ARGS, 
		"=> bdb_tool_entry_reindex( %ld )\n", (long) id, 0, 0 );
#else
	Debug( LDAP_DEBUG_ARGS, "=> bdb_tool_entry_reindex( %ld )\n",
		(long) id, 0, 0 );
#endif

	e = bdb_tool_entry_get( be, id );

	if( e == NULL ) {
#ifdef NEW_LOGGING
		LDAP_LOG ( TOOLS, DETAIL1, 
			"bdb_tool_entry_reindex:: could not locate id=%ld\n", 
			(long) id, 0, 0 );
#else
		Debug( LDAP_DEBUG_ANY,
			"bdb_tool_entry_reindex:: could not locate id=%ld\n",
			(long) id, 0, 0 );
#endif
		return -1;
	}

	rc = TXN_BEGIN( bi->bi_dbenv, NULL, &tid, bi->bi_db_opflags );
	if( rc != 0 ) {
#ifdef NEW_LOGGING
		LDAP_LOG ( TOOLS, ERR, 
			"=> bdb_tool_entry_reindex: txn_begin failed: %s (%d)\n", 
			db_strerror(rc), rc, 0 );
#else
		Debug( LDAP_DEBUG_ANY,
			"=> bdb_tool_entry_reindex: txn_begin failed: %s (%d)\n",
			db_strerror(rc), rc, 0 );
#endif
		goto done;
	}
 	
	/*
	 * just (re)add them for now
	 * assume that some other routine (not yet implemented)
	 * will zap index databases
	 *
	 */

#ifdef NEW_LOGGING
	LDAP_LOG ( TOOLS, ERR, 
		"=> bdb_tool_entry_reindex( %ld, \"%s\" )\n", (long) id, e->e_dn, 0 );
#else
	Debug( LDAP_DEBUG_TRACE, "=> bdb_tool_entry_reindex( %ld, \"%s\" )\n",
		(long) id, e->e_dn, 0 );
#endif

#ifndef BDB_HIER
	/* add dn2id indices */
	rc = bdb_dn2id_add( be, tid, NULL, e, NULL );
	if( rc != 0 && rc != DB_KEYEXIST ) {
#ifdef NEW_LOGGING
		LDAP_LOG ( TOOLS, ERR, 
			"=> bdb_tool_entry_reindex: dn2id_add failed: %s (%d)\n", 
			db_strerror(rc), rc, 0 );
#else
		Debug( LDAP_DEBUG_ANY,
			"=> bdb_tool_entry_reindex: dn2id_add failed: %s (%d)\n",
			db_strerror(rc), rc, 0 );
#endif
		goto done;
	}
#endif

	op.o_bd = be;
	op.o_tmpmemctx = NULL;
	op.o_tmpmfuncs = &ch_mfuncs;
	rc = bdb_index_entry_add( &op, tid, e );

done:
	if( rc == 0 ) {
		rc = TXN_COMMIT( tid, 0 );
		if( rc != 0 ) {
#ifdef NEW_LOGGING
			LDAP_LOG ( TOOLS, ERR, 
				"=> bdb_tool_entry_reindex: txn_commit failed: %s (%d)\n", 
				db_strerror(rc), rc, 0 );
#else
			Debug( LDAP_DEBUG_ANY,
				"=> bdb_tool_entry_reindex: txn_commit failed: %s (%d)\n",
				db_strerror(rc), rc, 0 );
#endif
			e->e_id = NOID;
		}

	} else {
		TXN_ABORT( tid );
#ifdef NEW_LOGGING
		LDAP_LOG ( TOOLS, DETAIL1, 
			"=> bdb_tool_entry_reindex: txn_aborted! %s (%d)\n", 
			db_strerror(rc), rc, 0 );
#else
		Debug( LDAP_DEBUG_ANY,
			"=> bdb_tool_entry_reindex: txn_aborted! %s (%d)\n",
			db_strerror(rc), rc, 0 );
#endif
		e->e_id = NOID;
	}

	return rc;
}
