/* mtest6.c - memory-mapped database tester/toy */
/*
 * Copyright 2011-2021 Howard Chu, Symas Corp.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted only as authorized by the OpenLDAP
 * Public License.
 *
 * A copy of this license is available in the file LICENSE in the
 * top-level directory of the distribution or, alternatively, at
 * <http://www.OpenLDAP.org/license.html>.
 */

/* Tests for mdb_cursor_get_batch  */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "lmdb.h"

#define E(expr) CHECK((rc = (expr)) == MDB_SUCCESS, #expr)
#define RES(err, expr) ((rc = expr) == (err) || (CHECK(!rc, #expr), 0))
#define CHECK(test, msg) ((test) ? (void)0 : ((void)fprintf(stderr, \
	"%s:%d: %s: %s\n", __FILE__, __LINE__, msg, mdb_strerror(rc)), abort()))
#define ITEMS_SIZE 100
#define KEYS_COUNT 1000

char dkbuf[1024];

int main(int argc,char * argv[])
{
	int rc;
	size_t i,j;
	MDB_env *env;
	MDB_dbi dbi;
	MDB_val key, data, sdata;
    MDB_item items[ITEMS_SIZE];
	MDB_txn *txn;
	MDB_stat mst;
	MDB_cursor *cursor, *cursor2;
	long kval;
	size_t items_count = 0;
	char sval[32] = "";

	srand(time(NULL));

	E(mdb_env_create(&env));
	E(mdb_env_set_mapsize(env, 10485760));
	E(mdb_env_set_maxdbs(env, 4));
	E(mdb_env_open(env, "./testdb", MDB_NOSYNC, 0664));

	E(mdb_txn_begin(env, NULL, 0, &txn));
	E(mdb_dbi_open(txn, "id7", MDB_CREATE|MDB_INTEGERKEY, &dbi));
	E(mdb_cursor_open(txn, dbi, &cursor));
	E(mdb_stat(txn, dbi, &mst));

	key.mv_size = sizeof(long);
	key.mv_data = &kval;
	sdata.mv_size = sizeof(sval);
	sdata.mv_data = sval;

	printf("Adding %d values\n", KEYS_COUNT);
	for (i=0;i<KEYS_COUNT;i++) {
		kval = i;
		sprintf(sval, "%08lx", kval);
		data = sdata;
		(void)RES(MDB_KEYEXIST, mdb_cursor_put(cursor, &key, &data, MDB_NOOVERWRITE));
	}

	E(mdb_txn_commit(txn));
	E(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn));
	E(mdb_cursor_open(txn, dbi, &cursor));
	E(mdb_cursor_open(txn, dbi, &cursor2));
	E(mdb_cursor_get_batch(cursor, &items_count, items, ITEMS_SIZE, MDB_FIRST_BATCH));

	for (j=0,i=0;i<items_count;i++,j++) {

		printf("key: %p %ld, data %p %s\n",
			items[i].key.mv_data, *((long*)items[i].key.mv_data),
			items[i].value.mv_data, ((char*)items[i].value.mv_data)
		);

		mdb_cursor_get(cursor2, &key, &data, MDB_NEXT);
		CHECK(key.mv_size == items[i].key.mv_size && data.mv_size == items[i].value.mv_size, "Key and Data sizes are correct");
		CHECK(memcmp(key.mv_data, items[i].key.mv_data, data.mv_size) == 0, "Key is the same");
		CHECK(memcmp(data.mv_data, items[i].value.mv_data, data.mv_size) == 0, "Data is the same");
	}

	while ((rc = mdb_cursor_get_batch(cursor, &items_count, items, ITEMS_SIZE, MDB_NEXT_BATCH)) == 0) {
		for (i=0;i<items_count;i++,j++) {
			printf("key: %p %ld, data %p %s\n",
				items[i].key.mv_data, *((long*)items[i].key.mv_data),
				items[i].value.mv_data, ((char*)items[i].value.mv_data)
			);
			mdb_cursor_get(cursor2, &key, &data, MDB_NEXT);
			CHECK(key.mv_size == items[i].key.mv_size && data.mv_size == items[i].value.mv_size, "Key and Data sizes are correct");
			CHECK(memcmp(key.mv_data, items[i].key.mv_data, data.mv_size) == 0, "Key is the same");
			CHECK(memcmp(data.mv_data, items[i].value.mv_data, data.mv_size) == 0, "Data is the same");
		}
	}
	CHECK(rc == MDB_NOTFOUND, "mdb_cursor_get_batch");
	CHECK(KEYS_COUNT == j, "The number of key are the same");

	key.mv_size = sizeof(long);
	key.mv_data = &kval;
	kval = 50;
	mdb_cursor_get(cursor, &key, NULL, MDB_SET);
	E(mdb_cursor_get_batch(cursor, &items_count, items, ITEMS_SIZE, MDB_CURRENT_BATCH));
	CHECK(*(long*)(items[0].key.mv_data) == 50, "Got 50");
	CHECK(*(long*)(items[1].key.mv_data) == 51, "Got 51");

	mdb_cursor_renew(txn, cursor2);
	E(mdb_cursor_get_batch(cursor, &items_count, items, ITEMS_SIZE, MDB_LAST_BATCH));
	for (j=0,i=0;i<items_count;i++,j++) {
		printf("key: %p %ld, data %p %s\n",
			items[i].key.mv_data, *((long*)items[i].key.mv_data),
			items[i].value.mv_data, ((char*)items[i].value.mv_data)
		);

		mdb_cursor_get(cursor2, &key, &data, MDB_PREV);
		CHECK(key.mv_size == items[i].key.mv_size && data.mv_size == items[i].value.mv_size, "Key and Data sizes are correct");
		CHECK(memcmp(key.mv_data, items[i].key.mv_data, data.mv_size) == 0, "Key is the same");
		CHECK(memcmp(data.mv_data, items[i].value.mv_data, data.mv_size) == 0, "Data is the same");
	}

	while ((rc = mdb_cursor_get_batch(cursor, &items_count, items, ITEMS_SIZE, MDB_PREV_BATCH)) == 0) {
		for (i=0;i<items_count;i++,j++) {
			printf("key: %p %ld, data %p %s\n",
				items[i].key.mv_data, *((long*)items[i].key.mv_data),
				items[i].value.mv_data, ((char*)items[i].value.mv_data)
			);
			mdb_cursor_get(cursor2, &key, &data, MDB_PREV);
			CHECK(key.mv_size == items[i].key.mv_size && data.mv_size == items[i].value.mv_size, "Key and Data sizes are correct");
			CHECK(memcmp(key.mv_data, items[i].key.mv_data, data.mv_size) == 0, "Key is the same");
			CHECK(memcmp(data.mv_data, items[i].value.mv_data, data.mv_size) == 0, "Data is the same");
		}
	}
	CHECK(KEYS_COUNT == j, "The number of key are the same");

	key.mv_size = sizeof(long);
	key.mv_data = &kval;
	kval = 50;
	mdb_cursor_get(cursor, &key, NULL, MDB_SET);
	E(mdb_cursor_get_batch(cursor, &items_count, items, ITEMS_SIZE, MDB_CURRENT_R_BATCH));
	CHECK(*(long*)(items[0].key.mv_data) == 50, "Got 50");
	CHECK(*(long*)(items[1].key.mv_data) == 49, "Got 49");

	mdb_cursor_close(cursor);
	mdb_cursor_close(cursor2);
	mdb_txn_commit(txn);
	mdb_env_close(env);

	return 0;
}
