/*
** Zabbix
** Copyright (C) 2000-2011 Zabbix SIA
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
**/

#include "common.h"
#include "log.h"
#include "threads.h"

#include "db.h"
#include "dbcache.h"
#include "ipc.h"
#include "mutexs.h"
#include "zbxserver.h"
#include "proxy.h"
#include "events.h"
#include "memalloc.h"
#include "zbxalgo.h"

static zbx_mem_info_t	*history_mem = NULL;
static zbx_mem_info_t	*history_text_mem = NULL;
static zbx_mem_info_t	*trend_mem = NULL;
static zbx_mem_info_t	*analyzer_avail_uptimes_mem = NULL;
static zbx_mem_info_t	*analyzer_avail_pings_mem = NULL;
static zbx_mem_info_t	*analyzer_avail_uptimes2_mem = NULL;
static zbx_mem_info_t	*analyzer_avail_pings2_mem = NULL;

#define	LOCK_CACHE	zbx_mutex_lock(&cache_lock)
#define	UNLOCK_CACHE	zbx_mutex_unlock(&cache_lock)
#define	LOCK_TRENDS	zbx_mutex_lock(&trends_lock)
#define	UNLOCK_TRENDS	zbx_mutex_unlock(&trends_lock)
#define	LOCK_CACHE_IDS		zbx_mutex_lock(&cache_ids_lock)
#define	UNLOCK_CACHE_IDS	zbx_mutex_unlock(&cache_ids_lock)
#define	LOCK_TRENDS_DB	zbx_mutex_lock(&trends_db_lock)
#define	UNLOCK_TRENDS_DB	zbx_mutex_unlock(&trends_db_lock)
#define	LOCK_ANALYZER_AVAIL_UPTIMES zbx_mutex_lock(&analyzer_avail_uptimes_lock)
#define	UNLOCK_ANALYZER_AVAIL_UPTIMES zbx_mutex_unlock(&analyzer_avail_uptimes_lock)
#define	LOCK_ANALYZER_AVAIL_PINGS zbx_mutex_lock(&analyzer_avail_pings_lock)
#define	UNLOCK_ANALYZER_AVAIL_PINGS zbx_mutex_unlock(&analyzer_avail_pings_lock)
#define	LOCK_ANALYZER_AVAIL_UPTIMES2 zbx_mutex_lock(&analyzer_avail_uptimes2_lock)
#define	UNLOCK_ANALYZER_AVAIL_UPTIMES2 zbx_mutex_unlock(&analyzer_avail_uptimes2_lock)
#define	LOCK_ANALYZER_AVAIL_PINGS2 zbx_mutex_lock(&analyzer_avail_pings2_lock)
#define	UNLOCK_ANALYZER_AVAIL_PINGS2 zbx_mutex_unlock(&analyzer_avail_pings2_lock)
#define	LOCK_ANALYZER_AVAIL_Q zbx_mutex_lock(&analyzer_avail_q_lock)
#define	UNLOCK_ANALYZER_AVAIL_Q zbx_mutex_unlock(&analyzer_avail_q_lock)

static ZBX_MUTEX	cache_lock;
static ZBX_MUTEX	trends_lock;
static ZBX_MUTEX	cache_ids_lock;
static ZBX_MUTEX	trends_db_lock;
static ZBX_MUTEX analyzer_avail_uptimes_lock;
static ZBX_MUTEX analyzer_avail_pings_lock;
static ZBX_MUTEX analyzer_avail_uptimes2_lock;
static ZBX_MUTEX analyzer_avail_pings2_lock;
static ZBX_MUTEX analyzer_avail_q_lock;

static char		*sql = NULL;
static size_t		sql_alloc = 64 * ZBX_KIBIBYTE;

extern unsigned char	daemon_type;

extern int		CONFIG_HISTSYNCER_FREQUENCY;
extern int		CONFIG_NODE_NOHISTORY;
extern int CONFIG_TRIGGERS_HISTORY;
extern int CONFIG_TRENDS_SQL_WRITE;
extern int CONFIG_ANALYZER_AVAIL_DIR;

extern int		process_num;
extern unsigned char	process_type;

static int		ZBX_HISTORY_SIZE = 0;
int			ZBX_SYNC_MAX = 1000;	/* must be less than ZBX_HISTORY_SIZE */
static int		ZBX_ITEMIDS_SIZE = 0;

static int		ZBX_TRENDS_DB_SIZE = 0;
static int ZBX_ANALYZER_AVAIL_Q_SIZE = 0;

#define ZBX_IDS_SIZE	10

extern int CONFIG_ANALYZER_AVAIL_INTERVAL1;
extern int CONFIG_ANALYZER_AVAIL_INTERVAL2;
extern int CONFIG_ANALYZER_AVAIL_STORE_INTERVAL;
extern int CONFIG_ANALYZER_AVAIL_PING_FREQ;

extern char* CONFIG_TARGET_TRENDS;
extern char* CONFIG_TARGET_AVAIL;

typedef struct
{
	char		table_name[ZBX_TABLENAME_LEN_MAX];
	zbx_uint64_t	lastid;
}
ZBX_DC_ID;

typedef struct
{
	ZBX_DC_ID	id[ZBX_IDS_SIZE];
}
ZBX_DC_IDS;

static ZBX_DC_IDS	*ids = NULL;

typedef struct
{
	zbx_uint64_t	itemid;
	history_value_t	value_orig;
	history_value_t	value;			/* used as source for log items */
	zbx_uint64_t	lastlogsize;
	int		clock;
	int		ns;
	int		timestamp;
	int		severity;
	int		logeventid;
	int		mtime;
	int		num;			/* number of continuous values with the same itemid */
	unsigned char	value_type;
	unsigned char	value_null;
	unsigned char	keep_history;
	unsigned char	keep_trends;
	unsigned char	status;
}
ZBX_DC_HISTORY;

typedef struct
{
	zbx_uint64_t	itemid;
	history_value_t	value_min;
	history_value_t	value_avg;
	history_value_t	value_max;
	int		clock;
	int		num;
	int		disable_from;
	unsigned char	value_type;
}
ZBX_DC_TREND;

typedef struct
{
    double avail;
    int progress;
	int clock;
}
ZBX_DC_ANALYZER_AVAIL_VALUE;

typedef struct
{
	zbx_uint64_t itemid;
    zbx_uint64_t value_last;
    int clock_last;
    int curr;
    int prev;
    ZBX_DC_ANALYZER_AVAIL_VALUE slot[2];
}
ZBX_DC_ANALYZER_AVAIL;

typedef struct
{
    int (*is_avail)(zbx_uint64_t itemid);
    int (*validate)(ZBX_DC_HISTORY *history, zbx_uint64_t value);
    void (*finish_prev)(ZBX_DC_HISTORY *history, ZBX_DC_ANALYZER_AVAIL *avail,
            int prev_interval_end, zbx_uint64_t value, int isvalid);
    void (*increase)(ZBX_DC_ANALYZER_AVAIL *avail, ZBX_DC_HISTORY *history,
            int interval_start, zbx_uint64_t value, int isvalid, int interval);
    char *name;
}
ZBX_DC_ANALYZER_AVAIL_ACTIONS;

typedef struct
{
    zbx_uint64_t itemid;
    double value;
    int clock;
    int interval;
}
ZBX_DC_ANALYZER_AVAIL_METRIC;

typedef struct
{
	zbx_uint64_t	history_counter;	/* the total number of processed values */
	zbx_uint64_t	history_float_counter;	/* the number of processed float values */
	zbx_uint64_t	history_uint_counter;	/* the number of processed uint values */
	zbx_uint64_t	history_str_counter;	/* the number of processed str values */
	zbx_uint64_t	history_log_counter;	/* the number of processed log values */
	zbx_uint64_t	history_text_counter;	/* the number of processed text values */
	zbx_uint64_t	notsupported_counter;	/* the number of processed not supported items */
}
ZBX_DC_STATS;

typedef struct
{
	zbx_hashset_t	trends;
	ZBX_DC_STATS	stats;
	ZBX_DC_HISTORY	*history;
	char		*text;
	zbx_uint64_t	*itemids;	/* items, processed by other syncers */
	char		*last_text;
	int		history_first;
	int		history_num;
	int		history_gap_num;
	int		text_free;
	int		trends_num;
	int		itemids_alloc, itemids_num;
	zbx_timespec_t	last_ts;
    ZBX_DC_TREND *trends_db;
    int trends_num_db;
    zbx_hashset_t analyzer_avail_uptimes;
    zbx_hashset_t analyzer_avail_pings;
    zbx_hashset_t analyzer_avail_uptimes2;
    zbx_hashset_t analyzer_avail_pings2;
    ZBX_DC_ANALYZER_AVAIL_METRIC *analyzer_avail_q;
    int analyzer_avail_q_num;
}
ZBX_DC_CACHE;

static ZBX_DC_CACHE	*cache = NULL;

/******************************************************************************
 *                                                                            *
 * Function: DCget_stats                                                      *
 *                                                                            *
 * Purpose: get statistics of the database cache                              *
 *                                                                            *
 * Author: Alexander Vladishev                                                *
 *                                                                            *
 ******************************************************************************/
void	*DCget_stats(int request)
{
	static zbx_uint64_t	value_uint;
	static double		value_double;

	switch (request)
	{
		case ZBX_STATS_HISTORY_COUNTER:
			value_uint = cache->stats.history_counter;
			return &value_uint;
		case ZBX_STATS_HISTORY_FLOAT_COUNTER:
			value_uint = cache->stats.history_float_counter;
			return &value_uint;
		case ZBX_STATS_HISTORY_UINT_COUNTER:
			value_uint = cache->stats.history_uint_counter;
			return &value_uint;
		case ZBX_STATS_HISTORY_STR_COUNTER:
			value_uint = cache->stats.history_str_counter;
			return &value_uint;
		case ZBX_STATS_HISTORY_LOG_COUNTER:
			value_uint = cache->stats.history_log_counter;
			return &value_uint;
		case ZBX_STATS_HISTORY_TEXT_COUNTER:
			value_uint = cache->stats.history_text_counter;
			return &value_uint;
		case ZBX_STATS_NOTSUPPORTED_COUNTER:
			value_uint = cache->stats.notsupported_counter;
			return &value_uint;
		case ZBX_STATS_HISTORY_TOTAL:
			value_uint = CONFIG_HISTORY_CACHE_SIZE;
			return &value_uint;
		case ZBX_STATS_HISTORY_USED:
			value_uint = cache->history_num * sizeof(ZBX_DC_HISTORY);
			return &value_uint;
		case ZBX_STATS_HISTORY_FREE:
			value_uint = CONFIG_HISTORY_CACHE_SIZE - cache->history_num * sizeof(ZBX_DC_HISTORY);
			return &value_uint;
		case ZBX_STATS_HISTORY_PFREE:
			value_double = 100 * ((double)(ZBX_HISTORY_SIZE - cache->history_num) / ZBX_HISTORY_SIZE);
			return &value_double;
		case ZBX_STATS_TREND_TOTAL:
			value_uint = trend_mem->orig_size;
			return &value_uint;
		case ZBX_STATS_TREND_USED:
			value_uint = trend_mem->orig_size - trend_mem->free_size;
			return &value_uint;
		case ZBX_STATS_TREND_FREE:
			value_uint = trend_mem->free_size;
			return &value_uint;
		case ZBX_STATS_TREND_PFREE:
			value_double = 100 * ((double)trend_mem->free_size / trend_mem->orig_size);
			return &value_double;
		case ZBX_STATS_TEXT_TOTAL:
			value_uint = CONFIG_TEXT_CACHE_SIZE;
			return &value_uint;
		case ZBX_STATS_TEXT_USED:
			value_uint = CONFIG_TEXT_CACHE_SIZE - cache->text_free;
			return &value_uint;
		case ZBX_STATS_TEXT_FREE:
			value_uint = cache->text_free;
			return &value_uint;
		case ZBX_STATS_TEXT_PFREE:
			value_double = 100.0 * ((double)cache->text_free / CONFIG_TEXT_CACHE_SIZE);
			return &value_double;
		default:
			return NULL;
	}
}

/******************************************************************************
 *                                                                            *
 * Function: DCget_trend                                                      *
 *                                                                            *
 * Purpose: find existing or add new structure and return pointer             *
 *                                                                            *
 * Return value: pointer to a trend structure                                 *
 *                                                                            *
 * Author: Alexander Vladishev                                                *
 *                                                                            *
 ******************************************************************************/
static ZBX_DC_TREND	*DCget_trend(zbx_uint64_t itemid)
{
	ZBX_DC_TREND	*ptr, trend;

	if (NULL != (ptr = (ZBX_DC_TREND *)zbx_hashset_search(&cache->trends, &itemid)))
		return ptr;

	memset(&trend, 0, sizeof(ZBX_DC_TREND));
	trend.itemid = itemid;

	return (ZBX_DC_TREND *)zbx_hashset_insert(&cache->trends, &trend, sizeof(ZBX_DC_TREND));
}

/******************************************************************************
 *                                                                            *
 * Function: DCflush_trends                                                   *
 *                                                                            *
 * Purpose: flush trend to the database                                       *
 *                                                                            *
 * Author: Alexander Vladishev                                                *
 *                                                                            *
 ******************************************************************************/
static void	DCflush_trends(ZBX_DC_TREND *trends, int *trends_num, int update_cache)
{
	const char	*__function_name = "DCflush_trends";
	DB_RESULT	result;
	DB_ROW		row;
	size_t		sql_offset;
	int		num, i, clock;
	history_value_t	value_min, value_avg, value_max;
	unsigned char	value_type;
	zbx_uint64_t	*ids = NULL, itemid;
	int		ids_alloc, ids_num = 0, trends_to = *trends_num;
	ZBX_DC_TREND	*trend = NULL;
	const char	*table_name;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() trends_num:%d", __function_name, *trends_num);

	clock = trends[0].clock;
	value_type = trends[0].value_type;

	switch (value_type)
	{
		case ITEM_VALUE_TYPE_FLOAT:
			table_name = "trends";
			break;
		case ITEM_VALUE_TYPE_UINT64:
			table_name = "trends_uint";
			break;
		default:
			assert(0);
	}

	ids_alloc = MIN(ZBX_SYNC_MAX, *trends_num);
	ids = zbx_malloc(ids, ids_alloc * sizeof(zbx_uint64_t));

	for (i = 0; i < *trends_num; i++)
	{
		trend = &trends[i];

		if (clock != trend->clock || value_type != trend->value_type)
			continue;

		if (0 != trend->disable_from)
			continue;

		uint64_array_add(&ids, &ids_alloc, &ids_num, trend->itemid, 64);

		if (ZBX_SYNC_MAX == ids_num)
		{
			trends_to = i + 1;
			break;
		}
	}

	if (0 != ids_num)
	{
		sql_offset = 0;
		zbx_snprintf_alloc(&sql, &sql_alloc, &sql_offset,
				"select distinct itemid"
				" from %s"
				" where clock>=%d and",
				table_name, clock);

		DBadd_condition_alloc(&sql, &sql_alloc, &sql_offset, "itemid", ids, ids_num);

		result = DBselect("%s", sql);

		while (NULL != (row = DBfetch(result)))
		{
			ZBX_STR2UINT64(itemid, row[0]);
			uint64_array_remove(ids, &ids_num, &itemid, 1);
		}
		DBfree_result(result);

		while (0 != ids_num)
		{
			itemid = ids[--ids_num];

			for (i = 0; i < trends_to; i++)
			{
				trend = &trends[i];

				if (itemid != trend->itemid)
					continue;

				if (clock != trend->clock || value_type != trend->value_type)
					continue;

				trend->disable_from = clock;
				break;
			}
		}
	}

	for (i = 0; i < trends_to; i++)
	{
		trend = &trends[i];

		if (clock != trend->clock || value_type != trend->value_type)
			continue;

		if (0 != trend->disable_from && trend->disable_from <= clock)
			continue;

		uint64_array_add(&ids, &ids_alloc, &ids_num, trend->itemid, 64);
	}

	if (0 != ids_num)
	{
		sql_offset = 0;
		zbx_snprintf_alloc(&sql, &sql_alloc, &sql_offset,
				"select itemid,num,value_min,value_avg,value_max"
				" from %s"
				" where clock=%d and",
				table_name, clock);

		DBadd_condition_alloc(&sql, &sql_alloc, &sql_offset, "itemid", ids, ids_num);

		result = DBselect("%s", sql);

		sql_offset = 0;
		DBbegin_multiple_update(&sql, &sql_alloc, &sql_offset);

		while (NULL != (row = DBfetch(result)))
		{
			ZBX_STR2UINT64(itemid, row[0]);

			for (i = 0; i < trends_to; i++)
			{
				trend = &trends[i];

				if (itemid != trend->itemid)
					continue;

				if (clock != trend->clock || value_type != trend->value_type)
					continue;

				break;
			}

			if (i == trends_to)
			{
				THIS_SHOULD_NEVER_HAPPEN;
				continue;
			}

			num = atoi(row[1]);

			if (value_type == ITEM_VALUE_TYPE_FLOAT)
			{
				value_min.dbl = atof(row[2]);
				value_avg.dbl = atof(row[3]);
				value_max.dbl = atof(row[4]);

				if (value_min.dbl < trend->value_min.dbl)
					trend->value_min.dbl = value_min.dbl;
				if (value_max.dbl > trend->value_max.dbl)
					trend->value_max.dbl = value_max.dbl;
				trend->value_avg.dbl = (trend->num * trend->value_avg.dbl
						+ num * value_avg.dbl) / (trend->num + num);
				trend->num += num;

				zbx_snprintf_alloc(&sql, &sql_alloc, &sql_offset,
						"update trends set num=%d,value_min=" ZBX_FS_DBL ",value_avg=" ZBX_FS_DBL
						",value_max=" ZBX_FS_DBL " where itemid=" ZBX_FS_UI64 " and clock=%d;\n",
						trend->num,
						trend->value_min.dbl,
						trend->value_avg.dbl,
						trend->value_max.dbl,
						trend->itemid,
						trend->clock);
			}
			else
			{
				ZBX_STR2UINT64(value_min.ui64, row[2]);
				ZBX_STR2UINT64(value_avg.ui64, row[3]);
				ZBX_STR2UINT64(value_max.ui64, row[4]);

				if (value_min.ui64 < trend->value_min.ui64)
					trend->value_min.ui64 = value_min.ui64;
				if (value_max.ui64 > trend->value_max.ui64)
					trend->value_max.ui64 = value_max.ui64;
				trend->value_avg.ui64 = (trend->num * trend->value_avg.ui64
						+ num * value_avg.ui64) / (trend->num + num);
				trend->num += num;

				zbx_snprintf_alloc(&sql, &sql_alloc, &sql_offset,
						"update trends_uint set num=%d,value_min=" ZBX_FS_UI64 ",value_avg=" ZBX_FS_UI64
						",value_max=" ZBX_FS_UI64 " where itemid=" ZBX_FS_UI64 " and clock=%d;\n",
						trend->num,
						trend->value_min.ui64,
						trend->value_avg.ui64,
						trend->value_max.ui64,
						trend->itemid,
						trend->clock);
			}

			trend->itemid = 0;

			DBexecute_overflowed_sql(&sql, &sql_alloc, &sql_offset);
		}
		DBfree_result(result);

		DBend_multiple_update(&sql, &sql_alloc, &sql_offset);

		if (sql_offset > 16)	/* In ORACLE always present begin..end; */
			DBexecute("%s", sql);
	}

	zbx_free(ids);

	/* if 'trends' is not a primary trends buffer */
	if (0 != update_cache)
	{
		/* we update it too */
		LOCK_TRENDS;

		for (i = 0; i < trends_to; i++)
		{
			if (0 == trends[i].itemid)
				continue;

			if (clock != trends[i].clock || value_type != trends[i].value_type)
				continue;

			if (0 == trends[i].disable_from || trends[i].disable_from > clock)
				continue;

			if (NULL != (trend = zbx_hashset_search(&cache->trends, &trends[i].itemid)))
				trend->disable_from = clock + SEC_PER_HOUR;
		}

		UNLOCK_TRENDS;
	}

	sql_offset = 0;

	if (value_type == ITEM_VALUE_TYPE_FLOAT)
	{
		for (i = 0; i < trends_to; i++)
		{
			trend = &trends[i];

			if (0 == trend->itemid)
				continue;

			if (clock != trend->clock || value_type != trend->value_type)
				continue;

			if (0 == sql_offset)
			{
				DBbegin_multiple_update(&sql, &sql_alloc, &sql_offset);
#ifdef HAVE_MULTIROW_INSERT
				zbx_strcpy_alloc(&sql, &sql_alloc, &sql_offset,
						"insert into trends (itemid,clock,num,value_min,value_avg,value_max) values ");
#endif
			}

#ifdef HAVE_MULTIROW_INSERT
			zbx_snprintf_alloc(&sql, &sql_alloc, &sql_offset,
					"(" ZBX_FS_UI64 ",%d,%d," ZBX_FS_DBL "," ZBX_FS_DBL "," ZBX_FS_DBL "),",
					trend->itemid,
					trend->clock,
					trend->num,
					trend->value_min.dbl,
					trend->value_avg.dbl,
					trend->value_max.dbl);
#else
			zbx_snprintf_alloc(&sql, &sql_alloc, &sql_offset,
					"insert into trends (itemid,clock,num,value_min,value_avg,value_max)"
					" values (" ZBX_FS_UI64 ",%d,%d," ZBX_FS_DBL "," ZBX_FS_DBL "," ZBX_FS_DBL ");\n",
					trend->itemid,
					trend->clock,
					trend->num,
					trend->value_min.dbl,
					trend->value_avg.dbl,
					trend->value_max.dbl);
#endif
			trend->itemid = 0;

			DBexecute_overflowed_sql(&sql, &sql_alloc, &sql_offset);
		}
	}
	else
	{
		for (i = 0; i < trends_to; i++)
		{
			trend = &trends[i];

			if (0 == trend->itemid)
				continue;

			if (clock != trend->clock || value_type != trend->value_type)
				continue;

			if (0 == sql_offset)
			{
				DBbegin_multiple_update(&sql, &sql_alloc, &sql_offset);
#ifdef HAVE_MULTIROW_INSERT
				zbx_strcpy_alloc(&sql, &sql_alloc, &sql_offset,
						"insert into trends_uint (itemid,clock,num,value_min,value_avg,value_max) values ");
#endif
			}

#ifdef HAVE_MULTIROW_INSERT
			zbx_snprintf_alloc(&sql, &sql_alloc, &sql_offset,
					"(" ZBX_FS_UI64 ",%d,%d," ZBX_FS_UI64 "," ZBX_FS_UI64 "," ZBX_FS_UI64 "),",
					trend->itemid,
					trend->clock,
					trend->num,
					trend->value_min.ui64,
					trend->value_avg.ui64,
					trend->value_max.ui64);
#else
			zbx_snprintf_alloc(&sql, &sql_alloc, &sql_offset,
					"insert into trends_uint (itemid,clock,num,value_min,value_avg,value_max)"
					" values (" ZBX_FS_UI64 ",%d,%d," ZBX_FS_UI64 "," ZBX_FS_UI64 "," ZBX_FS_UI64 ");\n",
					trend->itemid,
					trend->clock,
					trend->num,
					trend->value_min.ui64,
					trend->value_avg.ui64,
					trend->value_max.ui64);
#endif
			trend->itemid = 0;

			DBexecute_overflowed_sql(&sql, &sql_alloc, &sql_offset);
		}
	}

	if (0 != sql_offset)
	{
#ifdef HAVE_MULTIROW_INSERT
		sql_offset--;
		zbx_strcpy_alloc(&sql, &sql_alloc, &sql_offset, ";\n");
#endif
		DBend_multiple_update(&sql, &sql_alloc, &sql_offset);
		DBexecute("%s", sql);
	}

	/* clean trends */
	for (i = 0, num = 0; i < *trends_num; i++)
	{
		if (0 == trends[i].itemid)
			continue;

		memcpy(&trends[num++], &trends[i], sizeof(ZBX_DC_TREND));
	}
	*trends_num = num;

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}

/******************************************************************************
 *                                                                            *
 * Function: DCflush_trend                                                    *
 *                                                                            *
 * Purpose: move trend to the array of trends for flushing to DB              *
 *                                                                            *
 * Author: Alexander Vladishev                                                *
 *                                                                            *
 ******************************************************************************/
static void	DCflush_trend(ZBX_DC_TREND *trend, ZBX_DC_TREND **trends, int *trends_alloc, int *trends_num)
{
	if (*trends_num == *trends_alloc)
	{
		*trends_alloc += 256;
		*trends = zbx_realloc(*trends, *trends_alloc * sizeof(ZBX_DC_TREND));
	}

	memcpy(&(*trends)[*trends_num], trend, sizeof(ZBX_DC_TREND));
	(*trends_num)++;

	trend->clock = 0;
	trend->num = 0;
	memset(&trend->value_min, 0, sizeof(history_value_t));
	memset(&trend->value_avg, 0, sizeof(history_value_t));
	memset(&trend->value_max, 0, sizeof(history_value_t));
}

/******************************************************************************
 *                                                                            *
 * Function: DCadd_trend                                                      *
 *                                                                            *
 * Purpose: add new value to the trends                                       *
 *                                                                            *
 * Author: Alexander Vladishev                                                *
 *                                                                            *
 ******************************************************************************/
static void	DCadd_trend(ZBX_DC_HISTORY *history, ZBX_DC_TREND **trends, int *trends_num)
{
	ZBX_DC_TREND	*trend = NULL;
	int		hour, unlock = 0;

	hour = history->clock - history->clock % SEC_PER_HOUR;
    
    //zabbix_log(LOG_LEVEL_INFORMATION, "DCadd_trend: hour: %d", hour);

	trend = DCget_trend(history->itemid);

	if (trend->num > 0 && (trend->clock != hour || trend->value_type != history->value_type)) {
        char *process_type_str = get_process_type_string(process_type);
        LOCK_TRENDS_DB;
        unlock = 1;
        zabbix_log(LOG_LEVEL_DEBUG, "[%s]#%d: DCadd_trend: trends_num: %d, size: %d/%d",
            process_type_str, process_num, *trends_num,
            (*trends_num) * sizeof(ZBX_DC_TREND), ZBX_TRENDS_DB_SIZE * sizeof(ZBX_DC_TREND));
        // if trends db cache is full, wait for free space
        while(*trends_num == ZBX_TRENDS_DB_SIZE) {
            UNLOCK_TRENDS_DB;
            UNLOCK_TRENDS;
            zabbix_log(LOG_LEVEL_WARNING, "[%s]#%d: DCadd_trend: trends db cache full",
                process_type_str, process_num);
            zbx_sleep(1);
            LOCK_TRENDS;
            LOCK_TRENDS_DB;
        }
        memcpy(&(*trends)[*trends_num], trend, sizeof(ZBX_DC_TREND));
        (*trends_num)++;
        trend->clock = 0;
        trend->num = 0;
        memset(&trend->value_min, 0, sizeof(history_value_t));
        memset(&trend->value_avg, 0, sizeof(history_value_t));
        memset(&trend->value_max, 0, sizeof(history_value_t));
    }

	trend->value_type = history->value_type;
	trend->clock = hour;

	switch (trend->value_type)
	{
		case ITEM_VALUE_TYPE_FLOAT:
			if (trend->num == 0 || history->value.dbl < trend->value_min.dbl)
				trend->value_min.dbl = history->value.dbl;
			if (trend->num == 0 || history->value.dbl > trend->value_max.dbl)
				trend->value_max.dbl = history->value.dbl;
			trend->value_avg.dbl = (trend->num * trend->value_avg.dbl
				+ history->value.dbl) / (trend->num + 1);
			break;
		case ITEM_VALUE_TYPE_UINT64:
			if (trend->num == 0 || history->value.ui64 < trend->value_min.ui64)
				trend->value_min.ui64 = history->value.ui64;
			if (trend->num == 0 || history->value.ui64 > trend->value_max.ui64)
				trend->value_max.ui64 = history->value.ui64;
			trend->value_avg.ui64 = (trend->num * trend->value_avg.ui64
				+ history->value.ui64) / (trend->num + 1);
			break;
	}
	trend->num++;
    
    if (1 == unlock) {
        UNLOCK_TRENDS_DB;
    }
}

/******************************************************************************
 *                                                                            *
 * Function: DCmass_update_trends                                             *
 *                                                                            *
 * Parameters: history - array of history data                                *
 *             history_num - number of history structures                     *
 *                                                                            *
 * Author: Alexander Vladishev                                                *
 *                                                                            *
 ******************************************************************************/
static void	DCmass_update_trends(ZBX_DC_HISTORY *history, int history_num)
{
	const char	*__function_name = "DCmass_update_trends";
	int		i;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	LOCK_TRENDS;

	for (i = 0; i < history_num; i++)
	{
		if (0 == history[i].keep_trends)
			continue;

		if (history[i].value_type != ITEM_VALUE_TYPE_FLOAT &&
				history[i].value_type != ITEM_VALUE_TYPE_UINT64)
			continue;

		if (0 != history[i].value_null)
			continue;

		DCadd_trend(&history[i], &cache->trends_db, &cache->trends_num_db);
	}

	UNLOCK_TRENDS;

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}

static void analyzer_avail_queue(ZBX_DC_ANALYZER_AVAIL *avail, int interval) {
    // if analyzer queue is full wait for free space
    while(cache->analyzer_avail_q_num == ZBX_ANALYZER_AVAIL_Q_SIZE) {
        UNLOCK_ANALYZER_AVAIL_Q;
        zabbix_log(LOG_LEVEL_WARNING, "[%s]#%d: analyzer_avail_queue: analyzer avail queue full",
            get_process_type_string(process_type), process_num);
        zbx_sleep(1);
        LOCK_ANALYZER_AVAIL_Q;
    }
    
    cache->analyzer_avail_q[cache->analyzer_avail_q_num].itemid = avail->itemid;
    cache->analyzer_avail_q[cache->analyzer_avail_q_num].value = avail->slot[avail->prev].avail;
    cache->analyzer_avail_q[cache->analyzer_avail_q_num].clock = avail->slot[avail->prev].clock;
    cache->analyzer_avail_q[cache->analyzer_avail_q_num].interval = interval;
    cache->analyzer_avail_q_num++;
    
    avail->slot[avail->prev].avail = 0;
    avail->slot[avail->prev].progress = 0;
    avail->slot[avail->prev].clock = 0;
}

static ZBX_DC_ANALYZER_AVAIL *analyzer_avail_create_avail(
        ZBX_DC_HISTORY *history, zbx_hashset_t *analyzer_avails) {

    ZBX_DC_ANALYZER_AVAIL *avail, *avail_tmp;

    avail_tmp = (ZBX_DC_ANALYZER_AVAIL *) malloc(sizeof(ZBX_DC_ANALYZER_AVAIL));
    avail_tmp->itemid = history->itemid;
    avail_tmp->value_last = 0;
    avail_tmp->clock_last = 0;
    avail_tmp->slot[0].avail = 0;
    avail_tmp->slot[0].progress = 0;
    avail_tmp->slot[0].clock = 0;
    avail_tmp->slot[1].avail = 0;
    avail_tmp->slot[1].progress = 0;
    avail_tmp->slot[1].clock = 0;
    avail_tmp->curr = 0;
    avail_tmp->prev = 1;

    avail = zbx_hashset_insert(analyzer_avails, avail_tmp, sizeof(ZBX_DC_ANALYZER_AVAIL));
    free(avail_tmp);

    return avail;
}

static int analyzer_avail_get_item_value(ZBX_DC_HISTORY *history, zbx_uint64_t *value) {
    switch (history->value_type)
	{
		case ITEM_VALUE_TYPE_FLOAT:
            *value = (zbx_uint64_t) history->value.dbl;
			break;
		case ITEM_VALUE_TYPE_UINT64:
			*value = history->value.ui64;
			break;
        default:
            return -1;
	}

    return 0;
}

static int analyzer_avail_send_unfinished(ZBX_DC_ANALYZER_AVAIL *avail, int prev_interval_end, int interval) {
    if (avail->slot[avail->prev].clock != 0
            && avail->slot[avail->prev].progress < prev_interval_end) {
        LOCK_ANALYZER_AVAIL_Q;
        analyzer_avail_queue(avail, interval);
        UNLOCK_ANALYZER_AVAIL_Q;
        return 0;
    }

    return -1;
}

static void analyzer_avail_slot_swap(ZBX_DC_ANALYZER_AVAIL *avail, int interval_start) {
    if (0 == avail->curr) {
        avail->curr = 1;
        avail->prev = 0;
    } else {
        avail->curr = 0;
        avail->prev = 1;
    }

    avail->slot[avail->curr].avail = 0;
    avail->slot[avail->curr].progress = interval_start;
    avail->slot[avail->curr].clock = interval_start;
    avail->value_last = 0;
    avail->clock_last = 0;
}

static int analyzer_avail_validate_ping(ZBX_DC_HISTORY *history, zbx_uint64_t value) {
    // check if item value should be considered as available
    return DCverify_avail_ping_value(history->itemid, (int) value);
}

static int analyzer_avail_validate_uptime(ZBX_DC_HISTORY *history, zbx_uint64_t value) {
    return 1;
}

static void analyzer_avail_finish_prev_uptime(
        ZBX_DC_HISTORY *history, ZBX_DC_ANALYZER_AVAIL *avail,
        int prev_interval_end, zbx_uint64_t value, int isvalid) {

    // if avail value reaches previous hour
    if (value >= (history->clock - prev_interval_end)) {
        int downtime = 
            (history->clock - value) - (avail->slot[avail->prev].progress - 1);
        if (downtime < 0)
            downtime = 0;            
        int avail_time = prev_interval_end - (avail->slot[avail->prev].progress - 1);
        avail->slot[avail->prev].avail += avail_time - downtime;
    }
    avail->slot[avail->prev].progress = prev_interval_end;
}

static void analyzer_avail_finish_prev_ping(
        ZBX_DC_HISTORY *history, ZBX_DC_ANALYZER_AVAIL *avail,
        int prev_interval_end, zbx_uint64_t value, int isvalid) {

    // if last hour ping was not too far in the past
    if (isvalid && avail->slot[avail->prev].progress > history->clock - CONFIG_ANALYZER_AVAIL_PING_FREQ)
        avail->slot[avail->prev].avail += prev_interval_end - avail->slot[avail->prev].progress;

    avail->slot[avail->prev].progress = prev_interval_end;
}

static void analyzer_avail_increase_uptime(ZBX_DC_ANALYZER_AVAIL *avail, ZBX_DC_HISTORY *history,
        int interval_start, zbx_uint64_t value, int isvalid, int interval) {
    // if first uptime during current hour starts after current hour beginning - move progress
    if ((0 == avail->clock_last) && (avail->slot[avail->curr].progress == interval_start)) {
        int avail_start = history->clock - value;
        if (avail_start > interval_start) {
            avail->slot[avail->curr].progress = avail_start;
            zabbix_log(LOG_LEVEL_DEBUG,
                "[ANALYZER/uptime%d] first avail after current interval, "
                "itemid: " ZBX_FS_UI64 ", clock: %d, progress: %d, avail: %f, avail: "
                ZBX_FS_UI64 ", clock: %d", interval,
                avail->itemid, avail->slot[avail->curr].clock, avail->slot[avail->curr].progress,
                avail->slot[avail->curr].avail, value, history->clock);
        }
    }
    
    if (value < avail->value_last) {
        // machine was down
        zabbix_log(LOG_LEVEL_DEBUG,
            "[ANALYZER/uptime%d] machine down discovered, "
            "itemid: " ZBX_FS_UI64 ", clock: %d, progress: %d, avail: %f, avail: "
            ZBX_FS_UI64 ", clock: %d", interval,
            avail->itemid, avail->slot[avail->curr].clock, avail->slot[avail->curr].progress,
            avail->slot[avail->curr].avail, value, history->clock);
    } else {
        // avail increases
        int avail_time = history->clock - (avail->slot[avail->curr].progress - 1);
        avail->slot[avail->curr].avail += avail_time;
    }
}

static void analyzer_avail_increase_ping(ZBX_DC_ANALYZER_AVAIL *avail, ZBX_DC_HISTORY *history,
        int interval_start, zbx_uint64_t value, int isvalid, int interval) {
    // if previous ping is within ping max freq increase availability
    if (isvalid && avail->slot[avail->curr].progress > history->clock - CONFIG_ANALYZER_AVAIL_PING_FREQ) {
        // count first second too
        if (0 == avail->slot[avail->curr].avail)
            avail->slot[avail->curr].avail++;
        avail->slot[avail->curr].avail += history->clock - avail->slot[avail->curr].progress;
    }
}

static void analyzer_avail_process(ZBX_DC_HISTORY *history,
        zbx_hashset_t *analyzer_avails, int interval, ZBX_DC_ANALYZER_AVAIL_ACTIONS *actions) {

    ZBX_DC_ANALYZER_AVAIL *avail;
    int interval_start, prev_interval_end;
    zbx_uint64_t value;
    int isvalid = 0;

    avail = (ZBX_DC_ANALYZER_AVAIL *) zbx_hashset_search(analyzer_avails, &history->itemid);
    
    if (NULL == avail) {
        if (!actions->is_avail(history->itemid)) {
            return;
        }

        avail = analyzer_avail_create_avail(history, analyzer_avails);
        if (NULL == avail) {
            zabbix_log(LOG_LEVEL_DEBUG, 
                "[ANALYZER/%s%d] failed crateing new avail avail, itemid: " ZBX_FS_UI64,
                actions->name, interval, history->itemid);
            return;
        }

        zabbix_log(LOG_LEVEL_DEBUG, 
            "[ANALYZER/%s%d] item added, itemid: " ZBX_FS_UI64,
            actions->name, interval, history->itemid);
    }

    if (history->clock < avail->clock_last) {
        // out of order data point
        zabbix_log(LOG_LEVEL_DEBUG,
            "[ANALYZER/%s%d] out of order data point, "
            "itemid: " ZBX_FS_UI64 ", avail clock: %d, progress: %d, avail: %f, data point clock: %d",
            actions->name, interval, avail->itemid, avail->slot[avail->curr].clock,
            avail->slot[avail->curr].progress, avail->slot[avail->curr].avail, history->clock);
        return;
    }
    
	interval_start = history->clock - (history->clock % interval);
    prev_interval_end = interval_start - 1;
    
    if (analyzer_avail_get_item_value(history, &value) < 0) {
        zabbix_log(LOG_LEVEL_DEBUG, "[ANALYZER/%s%d] Invalid avail item value type: %d",
               actions->name, interval, history->value_type);
        return;
    }
    
    // new hour
    if (interval_start > avail->slot[avail->curr].clock) {
        if (analyzer_avail_send_unfinished(avail, prev_interval_end, interval) == 0) {
            zabbix_log(LOG_LEVEL_DEBUG, 
                "[ANALYZER/%s%d] unfinished analyze, "
                "itemid: " ZBX_FS_UI64 ", clock: %d, progress: %d, avail: %f",
                actions->name, interval, avail->itemid, avail->slot[avail->prev].clock,
                avail->slot[avail->prev].progress, avail->slot[avail->prev].avail);
        }

        analyzer_avail_slot_swap(avail, interval_start);
        
        zabbix_log(LOG_LEVEL_DEBUG,
            "[ANALYZER/%s%d] new interval begins, " "itemid: " ZBX_FS_UI64
            ", clock: %d, progress: %d, avail: %f, avail: " ZBX_FS_UI64 ", clock: %d",
            actions->name, interval, avail->itemid, avail->slot[avail->curr].clock,
            avail->slot[avail->curr].progress, avail->slot[avail->curr].avail, value, history->clock);
    }

    isvalid = actions->validate(history, value);
    
    // how previous interval should have ended
    prev_interval_end = avail->slot[avail->prev].clock + interval - 1;
    
    // previous hour not finished
    if (avail->slot[avail->prev].progress != 0
            && avail->slot[avail->prev].progress < prev_interval_end) {
        
        actions->finish_prev(history, avail, prev_interval_end, value, isvalid);

        zabbix_log(LOG_LEVEL_DEBUG,
            "[ANALYZER/%s%d] finished prev interval, "
            "itemid: " ZBX_FS_UI64 ", clock: %d, progress: %d, avail: %f, avail: "
            ZBX_FS_UI64 ", clock: %d", actions->name, interval, avail->itemid,
            avail->slot[avail->prev].clock, avail->slot[avail->prev].progress,
            avail->slot[avail->prev].avail, value, history->clock);
    }
    
    actions->increase(avail, history, interval_start, value, isvalid, interval);

    avail->slot[avail->curr].progress = history->clock;
    avail->value_last = value;
    avail->clock_last = history->clock;
}

static void analyzer_avail_load(zbx_hashset_t *analyzer_avail, int interval, char* name) {
    ZBX_DC_ANALYZER_AVAIL avail, *insert_result;
    FILE *fp;
    char buf[1024];
    size_t count = 0;
    static const int SCANF_EXPECTED = 11;
    int scanf_count = SCANF_EXPECTED;

    zbx_snprintf(buf, 1024, "%s/zabbix-analyzer-avail-%s-%d",
            CONFIG_ANALYZER_AVAIL_DIR, name, interval);

    if ((fp = fopen(buf, "r")) == NULL) {
        zabbix_log(LOG_LEVEL_ERR,
                "[ANALYZER/AVAIL] Error opening avail store, file: %s, error: %s",
                buf, strerror(errno));
        return;
    }

    while (1) { 
        scanf_count = fscanf(fp, ZBX_FS_UI64 ":" ZBX_FS_UI64 ":%d:%d:%d:%lf:%d:%d:%lf:%d:%d\n",
                &avail.itemid, &avail.value_last, &avail.clock_last, &avail.curr, &avail.prev,
                &(avail.slot[0].avail), &(avail.slot[0].progress), &(avail.slot[0].clock),
                &(avail.slot[1].avail), &(avail.slot[1].progress), &(avail.slot[1].clock));

        if (EOF == scanf_count && ferror(fp)) {
            goto error_read;
        }

        if (scanf_count == SCANF_EXPECTED) {
            zabbix_log(LOG_LEVEL_DEBUG,
                    "[ANALYZER/AVAIL%d] loaded: " ZBX_FS_UI64 ":" ZBX_FS_UI64
                    ":%d:%d:%d:%lf:%d:%d:%lf:%d:%d",
                    interval, avail.itemid, avail.value_last,
                    avail.clock_last, avail.curr, avail.prev,
                    avail.slot[0].avail, avail.slot[0].progress, avail.slot[0].clock,
                    avail.slot[1].avail, avail.slot[1].progress, avail.slot[1].clock);

            insert_result = zbx_hashset_insert(analyzer_avail, &avail, sizeof(ZBX_DC_ANALYZER_AVAIL));
            if (NULL == insert_result) {
                zabbix_log(LOG_LEVEL_DEBUG,
                        "[ANALYZER/AVAIL%d] failed crateing new avail", interval);
                return;
            }

            count++;
            continue;
        }

        break;
    }

    zabbix_log(LOG_LEVEL_INFORMATION,
            "[ANALYZER/AVAIL] Loaded availability, count: %d, file: %s", count, buf);
    goto out;

error_read:
    zabbix_log(LOG_LEVEL_ERR,
            "[ANALYZER/AVAIL] Error reading avail from store, file: %s, error: %s",
            buf, strerror(errno));

out:
    if (fclose(fp) != 0) {
        zabbix_log(LOG_LEVEL_ERR,
                "[ANALYZER/AVAIL] Error closing avail store stream, file: %s, error: %s",
                buf, strerror(errno));
    }

}

static void analyzer_avail_store(zbx_hashset_t *analyzer_avail, int fd, char *filename) {
    zbx_hashset_iter_t	iter;
    ZBX_DC_ANALYZER_AVAIL *avail;
    size_t count = 0;
    FILE *fp;

    if (ftruncate(fd, 0) != 0)
        goto error_write;

    if (lseek(fd, 0, SEEK_SET) == -1)
        goto error_write;

    if ((fp = fdopen(fd, "w+")) == NULL) {
        zabbix_log(LOG_LEVEL_ERR,
                "[ANALYZER/AVAIL] Error opening file stream, file: %s, error: %s",
                filename, strerror(errno));
        goto out;
    }

	zbx_hashset_iter_reset(analyzer_avail, &iter);
	while (NULL != (avail = (ZBX_DC_ANALYZER_AVAIL *) zbx_hashset_iter_next(&iter))) {

        if (fprintf(fp, ZBX_FS_UI64 ":" ZBX_FS_UI64 ":%d:%d:%d:%lf:%d:%d:%lf:%d:%d\n",
                    avail->itemid, avail->value_last, avail->clock_last, avail->curr, avail->prev,
                    avail->slot[0].avail, avail->slot[0].progress, avail->slot[0].clock,
                    avail->slot[1].avail, avail->slot[1].progress, avail->slot[1].clock) < 0) {
            goto error_write;
        }

        count++;
    }
    zabbix_log(LOG_LEVEL_DEBUG,
            "[ANALYZER/AVAIL] Saved availability, count: %d, file: %s", count, filename);
    goto out;

error_write:
    zabbix_log(LOG_LEVEL_ERR,
            "[ANALYZER/AVAIL] Error writing to avail store, file: %s, error: %s",
            filename, strerror(errno));

out:
    if (fp != NULL && EOF == fclose(fp)) {
        zabbix_log(LOG_LEVEL_ERR,
                "[ANALYZER/AVAIL] Error closing file stream, file: %s, error: %s",
                filename, strerror(errno));
    }

    return;
}

static void analyzer_avail_store_check(zbx_hashset_t *analyzer_avail, int interval,
        time_t write_interval, char* name, int force) {

    int fd;
    time_t now;
    struct stat stat_result;
    char buf[1024];

    now = time(NULL);

    zbx_snprintf(buf, 1024, "%s/zabbix-analyzer-avail-%s-%d",
            CONFIG_ANALYZER_AVAIL_DIR, name, interval);

    if ((fd = open(buf, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)) == -1) {
        zabbix_log(LOG_LEVEL_ERR,
                "[ANALYZER/AVAIL] Error opening avail store, file: %s, error: %s",
                buf, strerror(errno));
        return;
    }

    if (!force)
        if (fstat(fd, &stat_result) == -1) {
            zabbix_log(LOG_LEVEL_ERR,
                    "[ANALYZER/AVAIL] Error fstat avail store, file: %s, error: %s",
                    buf, strerror(errno));
            close(fd);
            return;
        }

    if (force || stat_result.st_mtime + write_interval < now) {
        analyzer_avail_store(analyzer_avail, fd, buf);
        // update file mtime, if there are no avails this prevents trying to store too often
        if (utime(buf, NULL) == -1)
            zabbix_log(LOG_LEVEL_ERR,
                    "[ANALYZER/AVAIL] Error changing file mtime, file: %s, error: %s",
                    buf, strerror(errno));
        // analyzer_avail_store closes file stream which cuses to close file descriptor
    } else {
        if (close(fd) != 0) {
            zabbix_log(LOG_LEVEL_ERR,
                    "[ANALYZER/AVAIL] Error closing file descriptor, file: %s, error: %s",
                    buf, strerror(errno));
        }
    }
}

static void analyzer_avail_check(zbx_hashset_t *analyzer_avail, int interval) {
    zbx_hashset_iter_t	iter;
    ZBX_DC_ANALYZER_AVAIL *avail;
    time_t now;
    int obsolete_interval = 4 * interval;

    LOCK_ANALYZER_AVAIL_Q;
    
    now = time(NULL);
	zbx_hashset_iter_reset(analyzer_avail, &iter);
    
	while (NULL != (avail = (ZBX_DC_ANALYZER_AVAIL *)zbx_hashset_iter_next(&iter))) {
        if (avail->slot[avail->prev].progress == avail->slot[avail->curr].clock - 1) {
            zabbix_log(LOG_LEVEL_DEBUG,
                "[ANALYZER/AVAIL%d] ready, "
                "itemid: " ZBX_FS_UI64 ", clock: %d, progress: %d, avail: %f",
                interval, avail->itemid, avail->slot[avail->prev].clock,
                avail->slot[avail->prev].progress, avail->slot[avail->prev].avail);

            if (avail->slot[avail->prev].avail > interval) {
                zabbix_log(LOG_LEVEL_DEBUG,
                        "[ANALYZER/AVAIL%d] avail exceeds interval - fixing, "
                        "itemid: " ZBX_FS_UI64 ", clock: %d, progress: %d, avail: %f",
                        interval, avail->itemid, avail->slot[avail->prev].clock,
                        avail->slot[avail->prev].progress, avail->slot[avail->prev].avail);
                avail->slot[avail->prev].avail = interval;
            }
            
            analyzer_avail_queue(avail, interval);
        } else if (
            (avail->slot[avail->curr].clock != 0 && avail->slot[avail->curr].clock < now - obsolete_interval) ||
            (avail->slot[avail->prev].clock != 0 && avail->slot[avail->prev].clock < now - obsolete_interval))
        {
            zabbix_log(LOG_LEVEL_DEBUG,
                "[ANALYZER/AVAIL%d] dropping obsolete entry, "
                "itemid: " ZBX_FS_UI64 ", clock: %d, progress: %d, avail: %f",
                interval, avail->itemid, avail->slot[avail->prev].clock,
                avail->slot[avail->prev].progress, avail->slot[avail->prev].avail);
            zbx_hashset_iter_remove(&iter);
        }
    }

	UNLOCK_ANALYZER_AVAIL_Q;

}

int DCis_avail_uptime(zbx_uint64_t itemid);
int DCis_avail_ping(zbx_uint64_t itemid);

static ZBX_DC_ANALYZER_AVAIL_ACTIONS actions_uptime = {
    DCis_avail_uptime,
    analyzer_avail_validate_uptime,
    analyzer_avail_finish_prev_uptime,
    analyzer_avail_increase_uptime,
    "uptime"
};

static ZBX_DC_ANALYZER_AVAIL_ACTIONS actions_ping = {
    DCis_avail_ping,
    analyzer_avail_validate_ping,
    analyzer_avail_finish_prev_ping,
    analyzer_avail_increase_ping,
    "ping"
};

static void DCmass_analyze(ZBX_DC_HISTORY *history, int history_num) {
    const char *__function_name = "DCmass_analyze";
	int i;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	for (i = 0; i < history_num; i++) {
        LOCK_ANALYZER_AVAIL_UPTIMES;
		analyzer_avail_process(&history[i],
                &cache->analyzer_avail_uptimes, CONFIG_ANALYZER_AVAIL_INTERVAL1, &actions_uptime);
        UNLOCK_ANALYZER_AVAIL_UPTIMES;

        LOCK_ANALYZER_AVAIL_PINGS;
		analyzer_avail_process(&history[i],
                &cache->analyzer_avail_pings, CONFIG_ANALYZER_AVAIL_INTERVAL1, &actions_ping);
        UNLOCK_ANALYZER_AVAIL_PINGS;

        // 24h
        LOCK_ANALYZER_AVAIL_UPTIMES2;
		analyzer_avail_process(&history[i],
                &cache->analyzer_avail_uptimes2, CONFIG_ANALYZER_AVAIL_INTERVAL2, &actions_uptime);
        UNLOCK_ANALYZER_AVAIL_UPTIMES2;

        LOCK_ANALYZER_AVAIL_PINGS2;
		analyzer_avail_process(&history[i],
                &cache->analyzer_avail_pings2, CONFIG_ANALYZER_AVAIL_INTERVAL2, &actions_ping);
        UNLOCK_ANALYZER_AVAIL_PINGS2;
	}
    
	LOCK_ANALYZER_AVAIL_UPTIMES;
    analyzer_avail_check(&cache->analyzer_avail_uptimes, CONFIG_ANALYZER_AVAIL_INTERVAL1);
    UNLOCK_ANALYZER_AVAIL_UPTIMES;

	LOCK_ANALYZER_AVAIL_PINGS;
    analyzer_avail_check(&cache->analyzer_avail_pings, CONFIG_ANALYZER_AVAIL_INTERVAL1);
    UNLOCK_ANALYZER_AVAIL_PINGS;

    // 24h

	LOCK_ANALYZER_AVAIL_UPTIMES2;
    analyzer_avail_check(&cache->analyzer_avail_uptimes2, CONFIG_ANALYZER_AVAIL_INTERVAL2);
    UNLOCK_ANALYZER_AVAIL_UPTIMES2;

	LOCK_ANALYZER_AVAIL_PINGS2;
    analyzer_avail_check(&cache->analyzer_avail_pings2, CONFIG_ANALYZER_AVAIL_INTERVAL2);
    UNLOCK_ANALYZER_AVAIL_PINGS2;

    // try storing

    LOCK_ANALYZER_AVAIL_UPTIMES;
    analyzer_avail_store_check(&cache->analyzer_avail_uptimes,
            CONFIG_ANALYZER_AVAIL_INTERVAL1, CONFIG_ANALYZER_AVAIL_STORE_INTERVAL, "uptime", 0);
    UNLOCK_ANALYZER_AVAIL_UPTIMES;

    LOCK_ANALYZER_AVAIL_PINGS;
    analyzer_avail_store_check(&cache->analyzer_avail_pings,
            CONFIG_ANALYZER_AVAIL_INTERVAL1, CONFIG_ANALYZER_AVAIL_STORE_INTERVAL, "ping", 0);
    UNLOCK_ANALYZER_AVAIL_PINGS;

    LOCK_ANALYZER_AVAIL_UPTIMES2;
    analyzer_avail_store_check(&cache->analyzer_avail_uptimes2,
            CONFIG_ANALYZER_AVAIL_INTERVAL2, CONFIG_ANALYZER_AVAIL_STORE_INTERVAL, "uptime", 0);
    UNLOCK_ANALYZER_AVAIL_UPTIMES2;

    LOCK_ANALYZER_AVAIL_PINGS2;
    analyzer_avail_store_check(&cache->analyzer_avail_pings2,
            CONFIG_ANALYZER_AVAIL_INTERVAL2, CONFIG_ANALYZER_AVAIL_STORE_INTERVAL, "ping", 0);
    UNLOCK_ANALYZER_AVAIL_PINGS2;

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}

static void analyzer_avail_load_all() {
    LOCK_ANALYZER_AVAIL_UPTIMES;
    if (cache->analyzer_avail_uptimes.num_data == 0)
        analyzer_avail_load(&cache->analyzer_avail_uptimes, CONFIG_ANALYZER_AVAIL_INTERVAL1, "uptime");
    UNLOCK_ANALYZER_AVAIL_UPTIMES;

    LOCK_ANALYZER_AVAIL_UPTIMES;
    if (cache->analyzer_avail_pings.num_data == 0)
        analyzer_avail_load(&cache->analyzer_avail_pings, CONFIG_ANALYZER_AVAIL_INTERVAL1, "ping");
    UNLOCK_ANALYZER_AVAIL_UPTIMES;

    // 24 h

    LOCK_ANALYZER_AVAIL_UPTIMES;
    if (cache->analyzer_avail_uptimes2.num_data == 0)
        analyzer_avail_load(&cache->analyzer_avail_uptimes2, CONFIG_ANALYZER_AVAIL_INTERVAL2, "uptime");
    UNLOCK_ANALYZER_AVAIL_UPTIMES;

    LOCK_ANALYZER_AVAIL_UPTIMES;
    if (cache->analyzer_avail_pings2.num_data == 0)
        analyzer_avail_load(&cache->analyzer_avail_pings2, CONFIG_ANALYZER_AVAIL_INTERVAL2, "ping");
    UNLOCK_ANALYZER_AVAIL_UPTIMES;
}

#ifdef HAVE_QUEUE
static void add_metric_suffix(char* metric, char* suffix, char* buf) {
    int n;
    char* p = strchr(metric, '[');
    if (p != NULL) {
        n = (int) (p - metric);
        memcpy(buf, metric, n);
        memcpy(buf + n, suffix, strlen(suffix));
        memcpy(buf + n + strlen(suffix), p, strlen(metric) - n);
    } else {
        memcpy(buf, metric, strlen(metric));
        memcpy(buf + strlen(metric), suffix, strlen(suffix));
    }
    buf[strlen(metric) + strlen(suffix)] = '\0';
}

static void add_avail_suffix(char* metric, char* name, int interval, char* buf) {
    const size_t SUFFIX_SIZE = 128;
    char suffix[SUFFIX_SIZE];

    if (strcmp("system.uptime", metric) == 0) {
        if (interval == SEC_PER_HOUR)
            zbx_snprintf(suffix, SUFFIX_SIZE, ".availability[%s]", name);
        else
            zbx_snprintf(suffix, SUFFIX_SIZE, ".availability%d[%s]", interval, name);
    } else {
        if (interval == SEC_PER_HOUR)
            zbx_snprintf(suffix, SUFFIX_SIZE, ".avail.%s", name);
        else
            zbx_snprintf(suffix, SUFFIX_SIZE, ".avail%d.%s", interval, name);
    }

    add_metric_suffix(metric, suffix, buf);
}

static void analyzer_avail_to_json(struct zbx_json *j,
        ZBX_DC_ANALYZER_AVAIL_METRIC *avails, int avails_num) {

    const size_t BUF_SIZE = 300; // db items.key_ column is varchar(255)
    char buf[BUF_SIZE];
    ZBX_DC_ANALYZER_AVAIL_METRIC *avail = NULL;
    size_t sql_offset = 0;
    DB_RESULT result;
	DB_ROW item_row;
    double percent;
    int i;
    char* p;
    int n;
     
    zbx_json_init(j, ZBX_JSON_STAT_BUF_LEN);
    
    // this is information for worker where to store this data
    zbx_json_addstring(j, "target", CONFIG_TARGET_AVAIL, ZBX_JSON_TYPE_STRING);
        
    zbx_json_addarray(j, ZBX_PROTO_TAG_DATA);
    
	for (i = 0; i < avails_num; i++) {
		avail = &avails[i];
        
        // TODO consider querying cache
        sql_offset = 0;
        zbx_snprintf_alloc(&sql, &sql_alloc, &sql_offset,
            "SELECT h.host, i.key_ "
            "FROM items as i LEFT JOIN (hosts as h) ON (i.hostid=h.hostid) "
            "WHERE i.itemid=%d LIMIT 1", avail->itemid);
        result = DBselect("%s", sql);
        item_row = DBfetch(result);
        if (item_row == NULL) {
            zabbix_log(LOG_LEVEL_ERR, "Unable to get item by id: %d", avail->itemid);
            continue;
        }
        
        zbx_json_addobject(j, NULL);
        
        zbx_snprintf(buf, BUF_SIZE, "%f", avail->value);
        zbx_json_addstring(j, ZBX_PROTO_TAG_VALUE, buf, ZBX_JSON_TYPE_STRING);
        
        add_avail_suffix(item_row[1], "second", avail->interval, buf);
        // zabbix_log(LOG_LEVEL_ERR, "avail item: %s", buf);
        zbx_json_addstring(j, ZBX_PROTO_TAG_KEY, buf, ZBX_JSON_TYPE_STRING);
        zbx_json_addstring(j, ZBX_PROTO_TAG_HOST, item_row[0], ZBX_JSON_TYPE_STRING);
        
        zbx_snprintf(buf, BUF_SIZE, "%d", avail->clock);
        zbx_json_addstring(j, ZBX_PROTO_TAG_CLOCK, buf, ZBX_JSON_TYPE_INT);
        
        zbx_json_close(j);

        zbx_json_addobject(j, NULL);
        
        percent = (avail->value * 100.0) / avail->interval;
        zbx_snprintf(buf, BUF_SIZE, "%f", percent);
        zbx_json_addstring(j, ZBX_PROTO_TAG_VALUE, buf, ZBX_JSON_TYPE_STRING);
        
        add_avail_suffix(item_row[1], "percent", avail->interval, buf);
        // zabbix_log(LOG_LEVEL_ERR, "avail item: %s", buf);
        zbx_json_addstring(j, ZBX_PROTO_TAG_KEY, buf, ZBX_JSON_TYPE_STRING);
        zbx_json_addstring(j, ZBX_PROTO_TAG_HOST, item_row[0], ZBX_JSON_TYPE_STRING);
        
        DBfree_result(result);
        
        zbx_snprintf(buf, BUF_SIZE, "%d", avail->clock);
        zbx_json_addstring(j, ZBX_PROTO_TAG_CLOCK, buf, ZBX_JSON_TYPE_INT);
        
        zbx_json_close(j);
    }
    
}
#endif

#ifdef HAVE_QUEUE
void DCmass_flush_analyzer(struct queue_ctx* qctx) {
#else
void DCmass_flush_analyzer() {
#endif

    ZBX_DC_ANALYZER_AVAIL_METRIC *analyzer_metrics = NULL;
    int analyzer_metrics_num = 0;
    struct zbx_json j;
    struct zbx_json_parse jp;
    double sec, sec2;
    int i;
    const int BATCH_MAX_SIZE = 100;

    sec = zbx_time();
    char *process_type_str = get_process_type_string(process_type);

    LOCK_ANALYZER_AVAIL_Q;
    
    if (0 < cache->analyzer_avail_q_num) {
        zabbix_log(LOG_LEVEL_DEBUG, 
                "[%s]#%d: DCmass_flush_analyzer: analyzer_avail_q_num: %d",
        process_type_str, process_num, cache->analyzer_avail_q_num);
        analyzer_metrics_num = cache->analyzer_avail_q_num < BATCH_MAX_SIZE
            ? cache->analyzer_avail_q_num : BATCH_MAX_SIZE;
        analyzer_metrics = zbx_malloc(analyzer_metrics, analyzer_metrics_num * sizeof(ZBX_DC_ANALYZER_AVAIL_METRIC));
        i = 0;
        while (0 < cache->analyzer_avail_q_num && i < BATCH_MAX_SIZE) {
            analyzer_metrics[analyzer_metrics_num - 1 - i] =
                cache->analyzer_avail_q[cache->analyzer_avail_q_num - 1];
            cache->analyzer_avail_q_num--;
            i++;
        }
    }
        
    UNLOCK_ANALYZER_AVAIL_Q;
    
#ifdef HAVE_QUEUE
    // send ready analyzer_metrics to queue
    if (analyzer_metrics_num > 0) {
        sec2 = zbx_time();
        analyzer_avail_to_json(&j, analyzer_metrics, analyzer_metrics_num);
        zbx_json_open(j.buffer, &jp);
        queue_msg(qctx, &jp, NULL, CONFIG_TARGET_AVAIL);
        zbx_json_free(&j);
        sec2 = zbx_time() - sec2;
        zabbix_log(LOG_LEVEL_DEBUG, "[%s]#%d: queue send: " 
            ZBX_FS_DBL " seconds", process_type_str, process_num, sec2);
    }
#else
    // TODO
    zabbix_log(LOG_LEVEL_DEBUG, "[%s]#%d: discarding analyzer metrics data - not sending to queue",
            process_type_str, process_num);
#endif

    zbx_free(analyzer_metrics);
    
    sec = zbx_time() - sec;
    zabbix_log(LOG_LEVEL_DEBUG, "[%s]#%d: DCmass_flush_analyzer: " 
        ZBX_FS_DBL " seconds", process_type_str, process_num, sec);
}

#ifdef HAVE_QUEUE

static void trend_to_json(struct zbx_json *j, DB_ROW item, char *buf, size_t buf_size,
        char *suffix, ZBX_DC_TREND *trend, history_value_t value) {

    switch (trend->value_type) {
        case ITEM_VALUE_TYPE_FLOAT:
            zbx_snprintf(buf, buf_size, "%f", value.dbl);
            break;
        case ITEM_VALUE_TYPE_UINT64:
            zbx_snprintf(buf, buf_size, ZBX_FS_UI64, value.ui64);
            break;
        default:
            zabbix_log(LOG_LEVEL_ERR, "Unknown trend value type: %d", trend->value_type);
            return;
    }

    zbx_json_addobject(j, NULL);
    
    zbx_json_addstring(j, ZBX_PROTO_TAG_VALUE, buf, ZBX_JSON_TYPE_STRING);
    zbx_json_addstring(j, ZBX_PROTO_TAG_HOST, item[1], ZBX_JSON_TYPE_STRING);
    
    add_metric_suffix(item[0], suffix, buf);
    zbx_json_addstring(j, ZBX_PROTO_TAG_KEY, buf, ZBX_JSON_TYPE_STRING);
    
    zbx_snprintf(buf, buf_size, "%d", trend->clock);
    zbx_json_addstring(j, ZBX_PROTO_TAG_CLOCK, buf, ZBX_JSON_TYPE_INT);
    
    zbx_json_close(j);
}

static void trends_to_json(struct zbx_json *j, ZBX_DC_TREND *trends, int trends_num) {
    size_t buf_size = 0;
    char *buf = NULL;
    ZBX_DC_TREND *trend = NULL;
    size_t sql_offset = 0;
    DB_RESULT result;
	DB_ROW item_row;
    int i;

    zbx_json_init(j, ZBX_JSON_STAT_BUF_LEN);
    
    // this is information for worker where to store this data
    zbx_json_addstring(j, "target", CONFIG_TARGET_TRENDS, ZBX_JSON_TYPE_STRING);
        
    zbx_json_addarray(j, ZBX_PROTO_TAG_DATA);
    
	for (i = 0; i < trends_num; i++) {
		trend = &trends[i];

        sql_offset = 0;
        zbx_snprintf_alloc(&sql, &sql_alloc, &sql_offset,
            "SELECT i.key_,h.host "
            "FROM items as i LEFT JOIN (hosts as h) ON (i.hostid=h.hostid) "
            "WHERE i.itemid=%d LIMIT 1", trend->itemid);
        result = DBselect("%s", sql);

        item_row = DBfetch(result);
        if (item_row == NULL) {
            zabbix_log(LOG_LEVEL_ERR, "Unable to get item by id: %d", trend->itemid);
            DBfree_result(result);
            continue;
        }

        if (buf_size < strlen(item_row[0]) + 20) {
            buf_size = strlen(item_row[0]) + 128;
            buf = zbx_realloc(buf, buf_size);
        }
        
        trend_to_json(j, item_row, buf, buf_size, ".trendavg1h", trend, trend->value_avg);
        trend_to_json(j, item_row, buf, buf_size, ".trendmin1h", trend, trend->value_min);
        trend_to_json(j, item_row, buf, buf_size, ".trendmax1h", trend, trend->value_max);

        DBfree_result(result);
    }

    zbx_free(buf);
}
#endif

#ifdef HAVE_QUEUE
void DCmass_flush_trends(struct queue_ctx* qctx) {
    struct zbx_json j;
    struct zbx_json_parse jp;
#else
void DCmass_flush_trends() {
#endif
    ZBX_DC_TREND *trends = NULL;
    double sec, sec2;
    int trends_num = 0;

    sec = zbx_time();
    
    char *process_type_str = get_process_type_string(process_type);
    
    LOCK_TRENDS_DB;
    
    zabbix_log(LOG_LEVEL_DEBUG, "[%s]#%d: DCmass_flush_trends: trends_num_db: %d",
        process_type_str, process_num, cache->trends_num_db);
    if (0 < cache->trends_num_db) {
        trends_num = cache->trends_num_db;
        trends = zbx_malloc(trends, trends_num * sizeof(ZBX_DC_TREND));
        while (0 < cache->trends_num_db) {
            trends[cache->trends_num_db - 1] = cache->trends_db[cache->trends_num_db - 1];
            cache->trends_num_db--;
        }
    }
        
    UNLOCK_TRENDS_DB;
    
#ifdef HAVE_QUEUE
    if (trends_num > 0) {
        sec2 = zbx_time();
        trends_to_json(&j, trends, trends_num);
        zbx_json_open(j.buffer, &jp);
        queue_msg(qctx, &jp, NULL, CONFIG_TARGET_TRENDS);
        zbx_json_free(&j);
        sec2 = zbx_time() - sec2;
        zabbix_log(LOG_LEVEL_DEBUG, "[%s]#%d: queue send: " 
            ZBX_FS_DBL " seconds", process_type_str, process_num, sec2);
    }
#endif

    // trends write to zabbix sql can be disabled in config
    if (CONFIG_TRENDS_SQL_WRITE) {
        DBbegin();
        
        while (0 < trends_num)
            DCflush_trends(trends, &trends_num, 1);
        
        DBcommit();
    }
        
    zbx_free(trends);
    
    sec = zbx_time() - sec;
    zabbix_log(LOG_LEVEL_DEBUG, "[%s]#%d: DCmass_flush_trends: " 
        ZBX_FS_DBL " seconds", process_type_str, process_num, sec);
}


/******************************************************************************
 *                                                                            *
 * Function: DCsync_trends                                                    *
 *                                                                            *
 * Purpose: flush all trends to the database                                  *
 *                                                                            *
 * Author: Alexander Vladishev                                                *
 *                                                                            *
 ******************************************************************************/
static void	DCsync_trends()
{
	const char		*__function_name = "DCsync_trends";
	zbx_hashset_iter_t	iter;
	ZBX_DC_TREND		*trends = NULL, *trend;
	int			trends_alloc = 0, trends_num = 0;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() trends_num:%d", __function_name, cache->trends_num);

	zabbix_log(LOG_LEVEL_WARNING, "syncing trends data...");

	LOCK_TRENDS;

	zbx_hashset_iter_reset(&cache->trends, &iter);

	while (NULL != (trend = (ZBX_DC_TREND *)zbx_hashset_iter_next(&iter)))
		DCflush_trend(trend, &trends, &trends_alloc, &trends_num);

	UNLOCK_TRENDS;

	DBbegin();

	while (trends_num > 0)
		DCflush_trends(trends, &trends_num, 0);

	DBcommit();

	zabbix_log(LOG_LEVEL_WARNING, "syncing trends data done");

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}

/******************************************************************************
 *                                                                            *
 * Function: DCmass_update_triggers                                           *
 *                                                                            *
 * Purpose: re-calculate and update values of triggers related to the items   *
 *                                                                            *
 * Parameters: history - array of history data                                *
 *             history_num - number of history structures                     *
 *                                                                            *
 * Author: Alexei Vladishev, Alexander Vladishev                              *
 *                                                                            *
 ******************************************************************************/
static void	DCmass_update_triggers(ZBX_DC_HISTORY *history, int history_num)
{
	const char		*__function_name = "DCmass_update_triggers";
	size_t			sql_offset = 0;
	int			i, item_num = 0, events_num = 0;
	zbx_uint64_t		*itemids = NULL;
	zbx_timespec_t		*timespecs = NULL;
	zbx_hashset_t		trigger_info;
	zbx_vector_ptr_t	trigger_order;
	DC_TRIGGER		*trigger;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	itemids = zbx_malloc(itemids, history_num * sizeof(zbx_uint64_t));
	timespecs = zbx_malloc(timespecs, history_num * sizeof(zbx_timespec_t));

	for (i = 0; i < history_num; i++)
	{
		if (0 != history[i].value_null)
			continue;

		itemids[item_num] = history[i].itemid;

		timespecs[item_num].sec = history[i].clock;
		timespecs[item_num].ns = history[i].ns;

		item_num++;
	}

	if (0 == item_num)
		goto clean_items;

	zbx_hashset_create(&trigger_info, MAX(100, 2 * item_num),
			ZBX_DEFAULT_UINT64_HASH_FUNC, ZBX_DEFAULT_UINT64_COMPARE_FUNC);

	zbx_vector_ptr_create(&trigger_order);
	zbx_vector_ptr_reserve(&trigger_order, item_num);

	DCconfig_get_triggers_by_itemids(&trigger_info, &trigger_order, itemids, timespecs, NULL, item_num);

	if (0 == trigger_order.values_num)
		goto clean_triggers;

	evaluate_expressions(&trigger_order);

	DBbegin_multiple_update(&sql, &sql_alloc, &sql_offset);

	for (i = 0; i < trigger_order.values_num; i++)
	{
		trigger = (DC_TRIGGER *)trigger_order.values[i];

		if (SUCCEED == DBget_trigger_update_sql(&sql, &sql_alloc, &sql_offset, trigger->triggerid,
				trigger->type, trigger->value, trigger->value_flags, trigger->error, trigger->new_value,
				trigger->new_error, &trigger->timespec, &trigger->add_event, &trigger->value_changed))
		{
			zbx_strcpy_alloc(&sql, &sql_alloc, &sql_offset, ";\n");

			DBexecute_overflowed_sql(&sql, &sql_alloc, &sql_offset);
		}

		zbx_free(trigger->expression);
		zbx_free(trigger->new_error);

		if (1 == trigger->add_event)
			events_num++;
	}

	DBend_multiple_update(&sql, &sql_alloc, &sql_offset);

	if (sql_offset > 16)	/* In ORACLE always present begin..end; */
		DBexecute("%s", sql);

	if (0 != events_num)
	{
		zbx_uint64_t	eventid;

		eventid = DBget_maxid_num("events", events_num);

		for (i = 0; i < trigger_order.values_num; i++)
		{
			trigger = (DC_TRIGGER *)trigger_order.values[i];

			if (1 != trigger->add_event)
				continue;

			process_event(eventid++, EVENT_SOURCE_TRIGGERS, EVENT_OBJECT_TRIGGER, trigger->triggerid,
					&trigger->timespec, trigger->new_value, trigger->value_changed, 0, 0);
		}
	}
clean_triggers:
	zbx_hashset_destroy(&trigger_info);
	zbx_vector_ptr_destroy(&trigger_order);
clean_items:
	zbx_free(itemids);
	zbx_free(timespecs);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}

static int	DBchk_double(double value)
{
	/* field with precision 20, scale 4 [NUMERIC(20,4)] */
	const double	pg_min_numeric = -1e16;
	const double	pg_max_numeric = 1e16;

	if (value <= pg_min_numeric || value >= pg_max_numeric)
		return FAIL;

	return SUCCEED;
}

static void	DCadd_update_item_sql(size_t *sql_offset, DB_ITEM *item, ZBX_DC_HISTORY *h)
{
	char	*value_esc;

	zbx_snprintf_alloc(&sql, &sql_alloc, sql_offset, "update items set lastclock=%d,lastns=%d",
			h->clock, h->ns);

	if (ITEM_STATUS_NOTSUPPORTED == h->status)
		goto notsupported;

	switch (h->value_type)
	{
		case ITEM_VALUE_TYPE_FLOAT:
			switch (item->delta)
			{
				case ITEM_STORE_AS_IS:
					if (0 == item->prevorgvalue_null)
					{
						zbx_strcpy_alloc(&sql, &sql_alloc, sql_offset, ",prevorgvalue=null");
						item->prevorgvalue_null = 1;
					}

					h->value.dbl = DBmultiply_value_float(item, h->value_orig.dbl);

					if (SUCCEED != DBchk_double(h->value.dbl))
					{
						h->status = ITEM_STATUS_NOTSUPPORTED;
						h->value_null = 1;
					}
					break;
				case ITEM_STORE_SPEED_PER_SECOND:
					if (0 == item->prevorgvalue_null && item->prevorgvalue.dbl <= h->value_orig.dbl &&
							(item->lastclock < h->clock ||
								(item->lastclock == h->clock && item->lastns < h->ns)))
					{
						h->value.dbl = (h->value_orig.dbl - item->prevorgvalue.dbl) /
								((h->clock - item->lastclock) +
									(double)(h->ns - item->lastns) / 1000000000);
						h->value.dbl = DBmultiply_value_float(item, h->value.dbl);

						if (SUCCEED != DBchk_double(h->value.dbl))
						{
							h->status = ITEM_STATUS_NOTSUPPORTED;
							h->value_null = 1;
						}
					}
					else
						h->value_null = 1;

					if (ITEM_STATUS_NOTSUPPORTED != h->status)
					{
						zbx_snprintf_alloc(&sql, &sql_alloc, sql_offset,
								",prevorgvalue='" ZBX_FS_DBL "'", h->value_orig.dbl);
						item->prevorgvalue_null = 0;
					}
					break;
				case ITEM_STORE_SIMPLE_CHANGE:
					if (0 == item->prevorgvalue_null && item->prevorgvalue.dbl <= h->value_orig.dbl)
					{
						h->value.dbl = h->value_orig.dbl - item->prevorgvalue.dbl;
						h->value.dbl = DBmultiply_value_float(item, h->value.dbl);

						if (SUCCEED != DBchk_double(h->value.dbl))
						{
							h->status = ITEM_STATUS_NOTSUPPORTED;
							h->value_null = 1;
						}
					}
					else
						h->value_null = 1;

					if (ITEM_STATUS_NOTSUPPORTED != h->status)
					{
						zbx_snprintf_alloc(&sql, &sql_alloc, sql_offset,
								",prevorgvalue='" ZBX_FS_DBL "'", h->value_orig.dbl);
						item->prevorgvalue_null = 0;
					}
					break;
			}

			if (0 == h->value_null)
			{
				zbx_snprintf_alloc(&sql, &sql_alloc, sql_offset,
						",prevvalue=lastvalue,lastvalue='" ZBX_FS_DBL "'",
						h->value.dbl);
			}

			if (ITEM_STATUS_NOTSUPPORTED == h->status)
			{
				h->value_orig.err = zbx_dsprintf(NULL, "Type of received value"
						" [" ZBX_FS_DBL "] is not suitable for value type [%s]",
						h->value.dbl, zbx_item_value_type_string(h->value_type));

				DCrequeue_reachable_item(h->itemid, h->status, h->clock);
			}
			break;
		case ITEM_VALUE_TYPE_UINT64:
			switch (item->delta)
			{
				case ITEM_STORE_AS_IS:
					if (0 == item->prevorgvalue_null)
					{
						zbx_strcpy_alloc(&sql, &sql_alloc, sql_offset, ",prevorgvalue=null");
						item->prevorgvalue_null = 1;
					}

					h->value.ui64 = DBmultiply_value_uint64(item, h->value_orig.ui64);
					break;
				case ITEM_STORE_SPEED_PER_SECOND:
					if (0 == item->prevorgvalue_null &&
							item->prevorgvalue.ui64 <= h->value_orig.ui64 &&
							(item->lastclock < h->clock ||
								(item->lastclock == h->clock && item->lastns < h->ns)))
					{
						h->value.ui64 = (h->value_orig.ui64 - item->prevorgvalue.ui64) /
								((h->clock - item->lastclock) +
									(double)(h->ns - item->lastns) / 1000000000);
						h->value.ui64 = DBmultiply_value_uint64(item, h->value.ui64);
					}
					else
						h->value_null = 1;

					zbx_snprintf_alloc(&sql, &sql_alloc, sql_offset,
							",prevorgvalue='" ZBX_FS_UI64 "'", h->value_orig.ui64);
					item->prevorgvalue_null = 0;
					break;
				case ITEM_STORE_SIMPLE_CHANGE:
					if (0 == item->prevorgvalue_null && item->prevorgvalue.ui64 <= h->value_orig.ui64)
					{
						h->value.ui64 = h->value_orig.ui64 - item->prevorgvalue.ui64;
						h->value.ui64 = DBmultiply_value_uint64(item, h->value.ui64);
					}
					else
						h->value_null = 1;

					zbx_snprintf_alloc(&sql, &sql_alloc, sql_offset,
							",prevorgvalue='" ZBX_FS_UI64 "'", h->value_orig.ui64);
					item->prevorgvalue_null = 0;
					break;
			}

			if (0 == h->value_null)
			{
				zbx_snprintf_alloc(&sql, &sql_alloc, sql_offset,
						",prevvalue=lastvalue,lastvalue='" ZBX_FS_UI64 "'",
						h->value.ui64);
			}
			break;
		case ITEM_VALUE_TYPE_LOG:
			zbx_snprintf_alloc(&sql, &sql_alloc, sql_offset, ",lastlogsize=" ZBX_FS_UI64 ",mtime=%d",
					h->lastlogsize, h->mtime);
		case ITEM_VALUE_TYPE_STR:
		case ITEM_VALUE_TYPE_TEXT:
			value_esc = DBdyn_escape_string_len(h->value_orig.str, ITEM_LASTVALUE_LEN);
			zbx_snprintf_alloc(&sql, &sql_alloc, sql_offset, ",prevvalue=lastvalue,lastvalue='%s'",
					value_esc);
			zbx_free(value_esc);
			break;
	}
notsupported:
	if (ITEM_STATUS_NOTSUPPORTED == h->status)
	{
		if (ITEM_STATUS_NOTSUPPORTED != item->status)
		{
			zabbix_log(LOG_LEVEL_WARNING, "item [%s] became not supported: %s",
					zbx_host_key_string(h->itemid), h->value_orig.err);

			zbx_snprintf_alloc(&sql, &sql_alloc, sql_offset, ",status=%d", (int)h->status);
		}

		if (0 == item->prevorgvalue_null)
		{
			zbx_snprintf_alloc(&sql, &sql_alloc, sql_offset,
					",prevorgvalue=null");
			item->prevorgvalue_null = 1;
		}

		if (0 != strcmp(item->error, h->value_orig.err))
		{
			value_esc = DBdyn_escape_string_len(h->value_orig.err, ITEM_ERROR_LEN);
			zbx_snprintf_alloc(&sql, &sql_alloc, sql_offset, ",error='%s'", value_esc);
			zbx_free(value_esc);
		}

		DCadd_nextcheck(item->itemid, h->clock, h->value_orig.err);
	}
	else
	{
		if (ITEM_STATUS_NOTSUPPORTED == item->status)
		{
			zabbix_log(LOG_LEVEL_WARNING, "item [%s] became supported", zbx_host_key_string(item->itemid));

			zbx_snprintf_alloc(&sql, &sql_alloc, sql_offset, ",status=%d,error=''", (int)h->status);
		}
	}

	zbx_snprintf_alloc(&sql, &sql_alloc, sql_offset, " where itemid=" ZBX_FS_UI64 ";\n", item->itemid);
}

static void	DCadd_update_inventory_sql(size_t *sql_offset, DB_ITEM *item, ZBX_DC_HISTORY *h, unsigned char inventory_link)
{
	const char	*inventory_field;
	char		value[MAX_BUFFER_LEN], *value_esc;
	int		update_inventory = 0;
	unsigned short	inventory_field_len;

	if (1 == h->value_null || NULL == (inventory_field = DBget_inventory_field(inventory_link)))
		return;

	switch (h->value_type)
	{
		case ITEM_VALUE_TYPE_FLOAT:
			zbx_snprintf(value, sizeof(value), ZBX_FS_DBL, h->value.dbl);
			update_inventory = 1;
			break;
		case ITEM_VALUE_TYPE_UINT64:
			zbx_snprintf(value, sizeof(value), ZBX_FS_UI64, h->value.ui64);
			update_inventory = 1;
			break;
		case ITEM_VALUE_TYPE_STR:
		case ITEM_VALUE_TYPE_TEXT:
			strscpy(value, h->value_orig.str);
			update_inventory = 1;
			break;
	}

	if (1 != update_inventory)
		return;

	zbx_format_value(value, sizeof(value), item->valuemapid, item->units, h->value_type);

	inventory_field_len = DBget_inventory_field_len(inventory_link);
	value_esc = DBdyn_escape_string_len(value, inventory_field_len);
	zbx_snprintf_alloc(&sql, &sql_alloc, sql_offset,
			"update host_inventory set %s='%s' where hostid=" ZBX_FS_UI64 ";\n",
			inventory_field, value_esc, item->hostid);
	zbx_free(value_esc);
}

/******************************************************************************
 *                                                                            *
 * Function: DCmass_update_items                                              *
 *                                                                            *
 * Purpose: update items info after new value is received                     *
 *                                                                            *
 * Parameters: history - array of history data                                *
 *             history_num - number of history structures                     *
 *                                                                            *
 * Author: Alexei Vladishev, Eugene Grigorjev, Alexander Vladishev            *
 *                                                                            *
 ******************************************************************************/
static void	DCmass_update_items(ZBX_DC_HISTORY *history, int history_num)
{
	const char		*__function_name = "DCmass_update_items";
	DB_RESULT		result;
	DB_ROW			row;
	DB_ITEM			item;
	size_t			sql_offset = 0;
	ZBX_DC_HISTORY		*h;
	zbx_vector_uint64_t	ids;
	int			i;
	unsigned char		inventory_link;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	zbx_vector_uint64_create(&ids);
	zbx_vector_uint64_reserve(&ids, history_num);

	for (i = 0; i < history_num; i++)
		zbx_vector_uint64_append(&ids, history[i].itemid);

	zbx_vector_uint64_sort(&ids, ZBX_DEFAULT_UINT64_COMPARE_FUNC);

	zbx_snprintf_alloc(&sql, &sql_alloc, &sql_offset,
			"select i.itemid,i.status,i.lastclock,i.prevorgvalue,i.delta,i.multiplier,i.formula,"
				"i.history,i.trends,i.lastns,i.hostid,i.inventory_link,hi.inventory_mode,i.valuemapid,"
				"i.units,i.error"
			" from items i"
				" left join host_inventory hi"
					" on hi.hostid=i.hostid"
			" where status in (%d,%d)"
				" and",
			ITEM_STATUS_ACTIVE, ITEM_STATUS_NOTSUPPORTED);

	DBadd_condition_alloc(&sql, &sql_alloc, &sql_offset, "i.itemid", ids.values, ids.values_num);

	zbx_strcpy_alloc(&sql, &sql_alloc, &sql_offset, " order by i.itemid");

	result = DBselect("%s", sql);

	ids.values_num = 0;	/* item ids that are not disabled and not deleted in DB */
	sql_offset = 0;

	DBbegin_multiple_update(&sql, &sql_alloc, &sql_offset);

	while (NULL != (row = DBfetch(result)))
	{
		ZBX_STR2UINT64(item.itemid, row[0]);

		zbx_vector_uint64_append(&ids, item.itemid);

		h = NULL;

		for (i = 0; i < history_num; i++)
		{
			if (item.itemid == history[i].itemid)
			{
				h = &history[i];
				break;
			}
		}

		if (NULL == h)
			continue;

		item.status = atoi(row[1]);
		if (SUCCEED != DBis_null(row[2]))
			item.lastclock = atoi(row[2]);
		else
			item.lastclock = 0;
		if (SUCCEED != DBis_null(row[9]))
			item.lastns = atoi(row[9]);
		else
			item.lastns = 0;
		if (SUCCEED != DBis_null(row[3]))
		{
			item.prevorgvalue_null = 0;
			switch (h->value_type)
			{
				case ITEM_VALUE_TYPE_FLOAT:
					item.prevorgvalue.dbl = atof(row[3]);
					break;
				case ITEM_VALUE_TYPE_UINT64:
					ZBX_STR2UINT64(item.prevorgvalue.ui64, row[3]);
					break;
			}
		}
		else
			item.prevorgvalue_null = 1;
		item.delta = atoi(row[4]);
		item.multiplier = atoi(row[5]);
		item.formula = row[6];
		item.history = atoi(row[7]);
		item.trends = atoi(row[8]);
		ZBX_STR2UINT64(item.hostid, row[10]);

		if (SUCCEED != DBis_null(row[12]) && HOST_INVENTORY_AUTOMATIC == (unsigned char)atoi(row[12]))
			inventory_link = (unsigned char)atoi(row[11]);
		else
			inventory_link = 0;

		ZBX_DBROW2UINT64(item.valuemapid, row[13]);
		item.units = row[14];
		item.error = row[15];

		h->keep_history = (0 != item.history ? 1 : 0);
		h->keep_trends = (0 != item.trends ? 1 : 0);

		DCadd_update_item_sql(&sql_offset, &item, h);
		DCadd_update_inventory_sql(&sql_offset, &item, h, inventory_link);

		DBexecute_overflowed_sql(&sql, &sql_alloc, &sql_offset);
	}
	DBfree_result(result);

	/* disable processing of deleted and disabled history items by setting value_null */
	for (i = 0; i < history_num; i++)
	{
		if (FAIL == zbx_vector_uint64_bsearch(&ids, history[i].itemid, ZBX_DEFAULT_UINT64_COMPARE_FUNC))
			history[i].value_null = 1;
	}

	zbx_vector_uint64_destroy(&ids);

	DBend_multiple_update(&sql, &sql_alloc, &sql_offset);

	if (sql_offset > 16)	/* In ORACLE always present begin..end; */
		DBexecute("%s", sql);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}

/******************************************************************************
 *                                                                            *
 * Function: DCmass_proxy_update_items                                        *
 *                                                                            *
 * Purpose: update items info after new value is received                     *
 *                                                                            *
 * Parameters: history - array of history data                                *
 *             history_num - number of history structures                     *
 *                                                                            *
 * Author: Alexei Vladishev, Eugene Grigorjev, Alexander Vladishev            *
 *                                                                            *
 ******************************************************************************/
static void	DCmass_proxy_update_items(ZBX_DC_HISTORY *history, int history_num)
{
	const char	*__function_name = "DCmass_proxy_update_items";
	size_t		sql_offset = 0;
	zbx_uint64_t	*ids = NULL, lastlogsize;
	int		ids_alloc, ids_num = 0;
	int		mtime, i, j;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	ids_alloc = history_num;
	ids = zbx_malloc(ids, ids_alloc * sizeof(zbx_uint64_t));

	for (i = 0; i < history_num; i++)
	{
		if (ITEM_VALUE_TYPE_LOG == history[i].value_type)
			uint64_array_add(&ids, &ids_alloc, &ids_num, history[i].itemid, 64);
	}

	DBbegin_multiple_update(&sql, &sql_alloc, &sql_offset);

	for (i = 0; i < ids_num; i++)
	{
		lastlogsize = mtime = -1;

		for (j = 0; j < history_num; j++)
		{
			if (history[j].itemid != ids[i])
				continue;

			if (ITEM_VALUE_TYPE_LOG != history[j].value_type)
				continue;

			if (lastlogsize < history[j].lastlogsize)
				lastlogsize = history[j].lastlogsize;

			if (mtime < history[j].mtime)
				mtime = history[j].mtime;
		}

		if (-1 == lastlogsize || -1 == mtime)
			continue;

		zbx_snprintf_alloc(&sql, &sql_alloc, &sql_offset,
				"update items set lastlogsize=" ZBX_FS_UI64 ",mtime=%d where itemid=" ZBX_FS_UI64 ";\n",
				lastlogsize,
				mtime,
				ids[i]);

		DBexecute_overflowed_sql(&sql, &sql_alloc, &sql_offset);
	}

	DBend_multiple_update(&sql, &sql_alloc, &sql_offset);

	zbx_free(ids);

	if (sql_offset > 16)	/* In ORACLE always present begin..end; */
		DBexecute("%s", sql);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}

/******************************************************************************
 *                                                                            *
 * Function: DCmass_add_history                                               *
 *                                                                            *
 * Purpose: inserting new history data after new value is received            *
 *                                                                            *
 * Parameters: history - array of history data                                *
 *             history_num - number of history structures                     *
 *                                                                            *
 * Author: Alexander Vladishev                                                *
 *                                                                            *
 ******************************************************************************/
static void	DCmass_add_history(ZBX_DC_HISTORY *history, int history_num)
{
	const char	*__function_name = "DCmass_add_history";
	size_t		sql_offset = 0;
	char		*value_esc, *source_esc;
	int		history_text_num, history_log_num, i;
	zbx_uint64_t	id;
#ifdef HAVE_MULTIROW_INSERT
	size_t		tmp_offset;
	const char	*row_dl = ",";
#else
	const char	*row_dl = ";\n";
#endif

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	DBbegin_multiple_update(&sql, &sql_alloc, &sql_offset);

/*
 * history
 */
#ifdef HAVE_MULTIROW_INSERT
	tmp_offset = sql_offset;
	zbx_strcpy_alloc(&sql, &sql_alloc, &sql_offset, "insert into history (itemid,clock,value,ns,hour) values ");
#endif

	for (i = 0; i < history_num; i++)
	{
		if (0 == history[i].keep_history 
        || (CONFIG_TRIGGERS_HISTORY && !DChas_triggers(history[i].itemid)))
			continue;

		if (ITEM_VALUE_TYPE_FLOAT != history[i].value_type)
			continue;

		if (0 != history[i].value_null)
			continue;

#ifndef HAVE_MULTIROW_INSERT
		zbx_strcpy_alloc(&sql, &sql_alloc, &sql_offset, "insert into history (itemid,clock,value,ns,hour) values ");
#endif
		zbx_snprintf_alloc(&sql, &sql_alloc, &sql_offset,
				"(" ZBX_FS_UI64 ",%d," ZBX_FS_DBL ",%d,%d",
				history[i].itemid,
				history[i].clock,
				history[i].value.dbl,
				history[i].ns,
                history[i].clock / SEC_PER_HOUR);
		zbx_snprintf_alloc(&sql, &sql_alloc, &sql_offset, ")%s", row_dl);
	}

#ifdef HAVE_MULTIROW_INSERT
	if (',' == sql[sql_offset - 1])
	{
		sql_offset--;
		zbx_strcpy_alloc(&sql, &sql_alloc, &sql_offset, ";\n");
	}
	else
		sql_offset = tmp_offset;
#endif

	if (0 == CONFIG_NODE_NOHISTORY && 0 < CONFIG_MASTER_NODEID)
	{
#ifdef HAVE_MULTIROW_INSERT
		tmp_offset = sql_offset;
		zbx_strcpy_alloc(&sql, &sql_alloc, &sql_offset,
				"insert into history_sync (nodeid,itemid,clock,value,ns) values ");
#endif

		for (i = 0; i < history_num; i++)
		{
			if (0 == history[i].keep_history)
				continue;

			if (ITEM_VALUE_TYPE_FLOAT != history[i].value_type)
				continue;

			if (0 != history[i].value_null)
				continue;

#ifndef HAVE_MULTIROW_INSERT
			zbx_strcpy_alloc(&sql, &sql_alloc, &sql_offset,
					"insert into history_sync (nodeid,itemid,clock,value,ns) values ");
#endif
			zbx_snprintf_alloc(&sql, &sql_alloc, &sql_offset,
					"(%d," ZBX_FS_UI64 ",%d," ZBX_FS_DBL ",%d",
					get_nodeid_by_id(history[i].itemid),
					history[i].itemid,
					history[i].clock,
					history[i].value.dbl,
					history[i].ns);
			zbx_snprintf_alloc(&sql, &sql_alloc, &sql_offset, ")%s", row_dl);
		}

#ifdef HAVE_MULTIROW_INSERT
		if (',' == sql[sql_offset - 1])
		{
			sql_offset--;
			zbx_strcpy_alloc(&sql, &sql_alloc, &sql_offset, ";\n");
		}
		else
			sql_offset = tmp_offset;
#endif
	}

/*
 * history_uint
 */
#ifdef HAVE_MULTIROW_INSERT
	tmp_offset = sql_offset;
	zbx_strcpy_alloc(&sql, &sql_alloc, &sql_offset, "insert into history_uint (itemid,clock,value,ns) values ");
#endif

	for (i = 0; i < history_num; i++)
	{
		if (0 == history[i].keep_history)
			continue;

		if (ITEM_VALUE_TYPE_UINT64 != history[i].value_type)
			continue;

		if (0 != history[i].value_null)
			continue;

#ifndef HAVE_MULTIROW_INSERT
		zbx_strcpy_alloc(&sql, &sql_alloc, &sql_offset,
				"insert into history_uint (itemid,clock,value,ns) values ");
#endif
		zbx_snprintf_alloc(&sql, &sql_alloc, &sql_offset,
				"(" ZBX_FS_UI64 ",%d," ZBX_FS_UI64 ",%d",
				history[i].itemid,
				history[i].clock,
				history[i].value.ui64,
				history[i].ns);
		zbx_snprintf_alloc(&sql, &sql_alloc, &sql_offset, ")%s", row_dl);
	}

#ifdef HAVE_MULTIROW_INSERT
	if (',' == sql[sql_offset - 1])
	{
		sql_offset--;
		zbx_strcpy_alloc(&sql, &sql_alloc, &sql_offset, ";\n");
	}
	else
		sql_offset = tmp_offset;
#endif

	if (0 == CONFIG_NODE_NOHISTORY && 0 < CONFIG_MASTER_NODEID)
	{
#ifdef HAVE_MULTIROW_INSERT
		tmp_offset = sql_offset;
		zbx_strcpy_alloc(&sql, &sql_alloc, &sql_offset,
				"insert into history_uint_sync (nodeid,itemid,clock,value,ns) values ");
#endif

		for (i = 0; i < history_num; i++)
		{
			if (0 == history[i].keep_history)
				continue;

			if (ITEM_VALUE_TYPE_UINT64 != history[i].value_type)
				continue;

			if (0 != history[i].value_null)
				continue;

#ifndef HAVE_MULTIROW_INSERT
			zbx_strcpy_alloc(&sql, &sql_alloc, &sql_offset,
					"insert into history_uint_sync (nodeid,itemid,clock,value,ns) values ");
#endif
			zbx_snprintf_alloc(&sql, &sql_alloc, &sql_offset,
					"(%d," ZBX_FS_UI64 ",%d," ZBX_FS_UI64 ",%d",
					get_nodeid_by_id(history[i].itemid),
					history[i].itemid,
					history[i].clock,
					history[i].value.ui64,
					history[i].ns);
			zbx_snprintf_alloc(&sql, &sql_alloc, &sql_offset, ")%s", row_dl);
		}

#ifdef HAVE_MULTIROW_INSERT
		if (',' == sql[sql_offset - 1])
		{
			sql_offset--;
			zbx_strcpy_alloc(&sql, &sql_alloc, &sql_offset, ";\n");
		}
		else
			sql_offset = tmp_offset;
#endif
	}

/*
 * history_str
 */
#ifdef HAVE_MULTIROW_INSERT
	tmp_offset = sql_offset;
	zbx_strcpy_alloc(&sql, &sql_alloc, &sql_offset, "insert into history_str (itemid,clock,value,ns) values ");
#endif

	for (i = 0; i < history_num; i++)
	{
		if (0 == history[i].keep_history)
			continue;

		if (ITEM_VALUE_TYPE_STR != history[i].value_type)
			continue;

		if (0 != history[i].value_null)
			continue;

		value_esc = DBdyn_escape_string(history[i].value_orig.str);
#ifndef HAVE_MULTIROW_INSERT
		zbx_strcpy_alloc(&sql, &sql_alloc, &sql_offset,
				"insert into history_str (itemid,clock,value,ns) values ");
#endif
		zbx_snprintf_alloc(&sql, &sql_alloc, &sql_offset,
				"(" ZBX_FS_UI64 ",%d,'%s',%d",
				history[i].itemid,
				history[i].clock,
				value_esc,
				history[i].ns);
		zbx_snprintf_alloc(&sql, &sql_alloc, &sql_offset, ")%s", row_dl);
		zbx_free(value_esc);
	}

#ifdef HAVE_MULTIROW_INSERT
	if (',' == sql[sql_offset - 1])
	{
		sql_offset--;
		zbx_strcpy_alloc(&sql, &sql_alloc, &sql_offset, ";\n");
	}
	else
		sql_offset = tmp_offset;
#endif

	if (0 == CONFIG_NODE_NOHISTORY && 0 < CONFIG_MASTER_NODEID)
	{
#ifdef HAVE_MULTIROW_INSERT
		tmp_offset = sql_offset;
		zbx_strcpy_alloc(&sql, &sql_alloc, &sql_offset,
				"insert into history_str_sync (nodeid,itemid,clock,value,ns) values ");
#endif

		for (i = 0; i < history_num; i++)
		{
			if (0 == history[i].keep_history)
				continue;

			if (ITEM_VALUE_TYPE_STR != history[i].value_type)
				continue;

			if (0 != history[i].value_null)
				continue;

			value_esc = DBdyn_escape_string(history[i].value_orig.str);
#ifndef HAVE_MULTIROW_INSERT
			zbx_strcpy_alloc(&sql, &sql_alloc, &sql_offset,
					"insert into history_str_sync (nodeid,itemid,clock,value,ns) values ");
#endif
			zbx_snprintf_alloc(&sql, &sql_alloc, &sql_offset,
					"(%d," ZBX_FS_UI64 ",%d,'%s',%d",
					get_nodeid_by_id(history[i].itemid),
					history[i].itemid,
					history[i].clock,
					value_esc,
					history[i].ns);
			zbx_snprintf_alloc(&sql, &sql_alloc, &sql_offset, ")%s", row_dl);
			zbx_free(value_esc);
		}

#ifdef HAVE_MULTIROW_INSERT
		if (',' == sql[sql_offset - 1])
		{
			sql_offset--;
			zbx_strcpy_alloc(&sql, &sql_alloc, &sql_offset, ";\n");
		}
		else
			sql_offset = tmp_offset;
#endif
	}

	history_text_num = 0;
	history_log_num = 0;

	for (i = 0; i < history_num; i++)
	{
		if (ITEM_VALUE_TYPE_TEXT == history[i].value_type)
			history_text_num++;
		else if (ITEM_VALUE_TYPE_LOG == history[i].value_type)
			history_log_num++;
	}

/*
 * history_text
 */
	if (0 < history_text_num)
	{
		id = DBget_maxid_num("history_text", history_text_num);

#ifdef HAVE_MULTIROW_INSERT
		tmp_offset = sql_offset;
		zbx_strcpy_alloc(&sql, &sql_alloc, &sql_offset,
				"insert into history_text (id,itemid,clock,value,ns) values ");
#endif

		for (i = 0; i < history_num; i++)
		{
			if (0 == history[i].keep_history)
				continue;

			if (ITEM_VALUE_TYPE_TEXT != history[i].value_type)
				continue;

			if (0 != history[i].value_null)
				continue;

			value_esc = DBdyn_escape_string(history[i].value_orig.str);
#ifndef HAVE_MULTIROW_INSERT
			zbx_strcpy_alloc(&sql, &sql_alloc, &sql_offset,
					"insert into history_text (id,itemid,clock,value,ns) values ");
#endif
			zbx_snprintf_alloc(&sql, &sql_alloc, &sql_offset,
					"(" ZBX_FS_UI64 "," ZBX_FS_UI64 ",%d,'%s',%d",
					id,
					history[i].itemid,
					history[i].clock,
					value_esc,
					history[i].ns);
			zbx_snprintf_alloc(&sql, &sql_alloc, &sql_offset, ")%s", row_dl);
			zbx_free(value_esc);
			id++;
		}

#ifdef HAVE_MULTIROW_INSERT
		if (',' == sql[sql_offset - 1])
		{
			sql_offset--;
			zbx_strcpy_alloc(&sql, &sql_alloc, &sql_offset, ";\n");
		}
		else
			sql_offset = tmp_offset;
#endif
	}

/*
 * history_log
 */
	if (0 < history_log_num)
	{
		id = DBget_maxid_num("history_log", history_log_num);

#ifdef HAVE_MULTIROW_INSERT
		tmp_offset = sql_offset;
		zbx_strcpy_alloc(&sql, &sql_alloc, &sql_offset,
				"insert into history_log (id,itemid,clock,timestamp,"
				"source,severity,value,logeventid,ns) values ");
#endif

		for (i = 0; i < history_num; i++)
		{
			if (0 == history[i].keep_history)
				continue;

			if (ITEM_VALUE_TYPE_LOG != history[i].value_type)
				continue;

			if (0 != history[i].value_null)
				continue;

			source_esc = DBdyn_escape_string_len(history[i].value.str, HISTORY_LOG_SOURCE_LEN);
			value_esc = DBdyn_escape_string(history[i].value_orig.str);
#ifndef HAVE_MULTIROW_INSERT
			zbx_strcpy_alloc(&sql, &sql_alloc, &sql_offset,
					"insert into history_log (id,itemid,clock,timestamp,"
					"source,severity,value,logeventid,ns) values ");
#endif
			zbx_snprintf_alloc(&sql, &sql_alloc, &sql_offset,
					"(" ZBX_FS_UI64 "," ZBX_FS_UI64 ",%d,%d,'%s',%d,'%s',%d,%d",
					id,
					history[i].itemid,
					history[i].clock,
					history[i].timestamp,
					source_esc,
					history[i].severity,
					value_esc,
					history[i].logeventid,
					history[i].ns);
			zbx_snprintf_alloc(&sql, &sql_alloc, &sql_offset, ")%s", row_dl);
			zbx_free(value_esc);
			zbx_free(source_esc);
			id++;
		}

#ifdef HAVE_MULTIROW_INSERT
		if (',' == sql[sql_offset - 1])
		{
			sql_offset--;
			zbx_strcpy_alloc(&sql, &sql_alloc, &sql_offset, ";\n");
		}
		else
			sql_offset = tmp_offset;
#endif
	}

#ifdef HAVE_MULTIROW_INSERT
	sql[sql_offset] = '\0';
#endif

	DBend_multiple_update(&sql, &sql_alloc, &sql_offset);

	if (sql_offset > 16)	/* In ORACLE always present begin..end; */
		DBexecute("%s", sql);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}

/******************************************************************************
 *                                                                            *
 * Function: DCmass_proxy_add_history                                         *
 *                                                                            *
 * Purpose: inserting new history data after new value is received            *
 *                                                                            *
 * Parameters: history - array of history data                                *
 *             history_num - number of history structures                     *
 *                                                                            *
 * Author: Alexander Vladishev                                                *
 *                                                                            *
 ******************************************************************************/
static void	DCmass_proxy_add_history(ZBX_DC_HISTORY *history, int history_num)
{
	const char	*__function_name = "DCmass_proxy_add_history";
	size_t		sql_offset = 0;
	int		i;
	char		*value_esc, *source_esc;
#ifdef HAVE_MULTIROW_INSERT
	size_t		tmp_offset;
	const char	*row_dl = ",";
#else
	const char	*row_dl = ";\n";
#endif
	const char	*ins_proxy_history_sql =
			"insert into proxy_history"
			" (itemid,clock,ns,value,status)"
			" values ";
	const char      *ins_proxy_history_log_sql =
			"insert into proxy_history"
			" (itemid,clock,ns,timestamp,source,severity,value,logeventid,status)"
			" values ";

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	DBbegin_multiple_update(&sql, &sql_alloc, &sql_offset);

#ifdef HAVE_MULTIROW_INSERT
	tmp_offset = sql_offset;
	zbx_strcpy_alloc(&sql, &sql_alloc, &sql_offset, ins_proxy_history_sql);
#endif

	for (i = 0; i < history_num; i++)
	{
		if (ITEM_VALUE_TYPE_LOG == history[i].value_type)
			continue;

#ifndef HAVE_MULTIROW_INSERT
		zbx_strcpy_alloc(&sql, &sql_alloc, &sql_offset, ins_proxy_history_sql);
#endif
		if (ITEM_STATUS_NOTSUPPORTED == history[i].status)
		{
			value_esc = DBdyn_escape_string(history[i].value_orig.err);

			zbx_snprintf_alloc(&sql, &sql_alloc, &sql_offset,
					"(" ZBX_FS_UI64 ",%d,%d,'%s',%d)%s",
					history[i].itemid, history[i].clock, history[i].ns,
					value_esc, (int)history[i].status, row_dl);

			zbx_free(value_esc);
		}
		else
		{
			switch (history[i].value_type)
			{
				case ITEM_VALUE_TYPE_FLOAT:
					zbx_snprintf_alloc(&sql, &sql_alloc, &sql_offset,
							"(" ZBX_FS_UI64 ",%d,%d,'" ZBX_FS_DBL "',%d)%s",
							history[i].itemid, history[i].clock, history[i].ns,
							history[i].value_orig.dbl, (int)history[i].status, row_dl);
					break;
				case ITEM_VALUE_TYPE_UINT64:
					zbx_snprintf_alloc(&sql, &sql_alloc, &sql_offset,
							"(" ZBX_FS_UI64 ",%d,%d,'" ZBX_FS_UI64 "',%d)%s",
							history[i].itemid, history[i].clock, history[i].ns,
							history[i].value_orig.ui64, (int)history[i].status, row_dl);
					break;
				case ITEM_VALUE_TYPE_STR:
				case ITEM_VALUE_TYPE_TEXT:
					value_esc = DBdyn_escape_string(history[i].value_orig.str);

					zbx_snprintf_alloc(&sql, &sql_alloc, &sql_offset,
							"(" ZBX_FS_UI64 ",%d,%d,'%s',%d)%s",
							history[i].itemid, history[i].clock, history[i].ns,
							value_esc, (int)history[i].status, row_dl);

					zbx_free(value_esc);
					break;
			}
		}
	}

#ifdef HAVE_MULTIROW_INSERT
	if (sql[sql_offset - 1] == ',')
	{
		sql_offset--;
		zbx_strcpy_alloc(&sql, &sql_alloc, &sql_offset, ";\n");
	}
	else
		sql_offset = tmp_offset;
#endif

#ifdef HAVE_MULTIROW_INSERT
	tmp_offset = sql_offset;
	zbx_strcpy_alloc(&sql, &sql_alloc, &sql_offset, ins_proxy_history_log_sql);
#endif

	for (i = 0; i < history_num; i++)
	{
		if (ITEM_VALUE_TYPE_LOG != history[i].value_type)
			continue;

#ifndef HAVE_MULTIROW_INSERT
		zbx_strcpy_alloc(&sql, &sql_alloc, &sql_offset, ins_proxy_history_log_sql);
#endif
		if (ITEM_STATUS_NOTSUPPORTED == history[i].status)
		{
			value_esc = DBdyn_escape_string(history[i].value_orig.err);

			zbx_snprintf_alloc(&sql, &sql_alloc, &sql_offset,
					"(" ZBX_FS_UI64 ",%d,%d,0,'',0,'%s',0,%d)%s",
					history[i].itemid, history[i].clock, history[i].ns,
					value_esc, (int)history[i].status, row_dl);

			zbx_free(value_esc);
		}
		else
		{
			source_esc = DBdyn_escape_string_len(history[i].value.str, HISTORY_LOG_SOURCE_LEN);
			value_esc = DBdyn_escape_string(history[i].value_orig.str);

			zbx_snprintf_alloc(&sql, &sql_alloc, &sql_offset,
					"(" ZBX_FS_UI64 ",%d,%d,%d,'%s',%d,'%s',%d,%d)%s",
					history[i].itemid, history[i].clock, history[i].ns,
					history[i].timestamp, source_esc, history[i].severity,
					value_esc, history[i].logeventid, (int)history[i].status, row_dl);

			zbx_free(value_esc);
			zbx_free(source_esc);
		}
	}

#ifdef HAVE_MULTIROW_INSERT
	if (sql[sql_offset - 1] == ',')
	{
		sql_offset--;
		zbx_strcpy_alloc(&sql, &sql_alloc, &sql_offset, ";\n");
	}
	else
		sql_offset = tmp_offset;
#endif

#ifdef HAVE_MULTIROW_INSERT
	sql[sql_offset] = '\0';
#endif

	DBend_multiple_update(&sql, &sql_alloc, &sql_offset);

	if (sql_offset > 16)	/* In ORACLE always present begin..end; */
		DBexecute("%s", sql);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}

static int	DCskip_items(int index, int n)
{
	zbx_uint64_t	itemid;
	int		f, num;

	itemid = cache->history[index].itemid;
	num = cache->history[index].num;

	while (0 < n - num)
	{
		if (ZBX_HISTORY_SIZE <= (f = index + num))
			f -= ZBX_HISTORY_SIZE;

		if (itemid != cache->history[f].itemid)
			break;

		num += cache->history[f].num;
	}

	cache->history[index].num = num;

	if (1 < num)
	{
		if (ZBX_HISTORY_SIZE == (f = index + 1))
			f = 0;

		cache->history[f].num = num - 1;
	}

	return num;
}

/******************************************************************************
 *                                                                            *
 * Function: DCsync_history                                                   *
 *                                                                            *
 * Purpose: writes updates and new data from pool to database                 *
 *                                                                            *
 * Return value: number of synced values                                      *
 *                                                                            *
 * Author: Alexei Vladishev                                                   *
 *                                                                            *
 ******************************************************************************/
int	DCsync_history(int sync_type)
{
	const char		*__function_name = "DCsync_history";
	static ZBX_DC_HISTORY	*history = NULL;
	int			i, history_num, n, f;
	int			syncs;
	int			total_num = 0;
	int			skipped_clock, max_delay;
	time_t			now = 0;
    double	sec;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() history_first:%d history_num:%d",
			__function_name, cache->history_first, cache->history_num);

	if (ZBX_SYNC_FULL == sync_type)
	{
		zabbix_log(LOG_LEVEL_WARNING, "syncing history data...");
		now = time(NULL);
		cache->itemids_num = 0;
	}

	if (0 == cache->history_num)
		goto finish;

	if (NULL == history)
		history = zbx_malloc(history, ZBX_SYNC_MAX * sizeof(ZBX_DC_HISTORY));

	syncs = cache->history_num / ZBX_SYNC_MAX;
	max_delay = (int)time(NULL) - CONFIG_HISTSYNCER_FREQUENCY;

	do
	{
		LOCK_CACHE;

		history_num = 0;
		skipped_clock = 0;

		for (n = cache->history_num, f = cache->history_first; 0 < n && ZBX_SYNC_MAX > history_num;)
		{
			int	num;

			if (ZBX_HISTORY_SIZE <= f)
				f -= ZBX_HISTORY_SIZE;

			if (0 != (daemon_type & ZBX_DAEMON_TYPE_SERVER))
			{
				num = DCskip_items(f, n);

				if (0 == cache->history[f].itemid)
				{
					if (f == cache->history_first)
					{
						cache->history_num -= num;
						cache->history_gap_num -= num;
						if (ZBX_HISTORY_SIZE <= (cache->history_first += num))
							cache->history_first -= ZBX_HISTORY_SIZE;
					}
					n -= num;
					f += num;
					continue;
				}

				if (SUCCEED == uint64_array_exists(cache->itemids, cache->itemids_num,
						cache->history[f].itemid))
				{
					if (0 == skipped_clock)
						skipped_clock = cache->history[f].clock;
					n -= num;
					f += num;
					continue;
				}
				else if (1 < num && 0 == skipped_clock)
					skipped_clock = cache->history[ZBX_HISTORY_SIZE == f + 1 ? 0 : f + 1].clock;

				uint64_array_add(&cache->itemids, &cache->itemids_alloc,
						&cache->itemids_num, cache->history[f].itemid, 0);
			}
			else
				num = 1;

			memcpy(&history[history_num], &cache->history[f], sizeof(ZBX_DC_HISTORY));

			if (ITEM_STATUS_NOTSUPPORTED == history[history_num].status)
			{
				history[history_num].value_orig.err =
						zbx_strdup(NULL, cache->history[f].value_orig.err);
				cache->text_free += strlen(cache->history[f].value_orig.err) + 1;
			}
			else
			{
				switch (history[history_num].value_type)
				{
					case ITEM_VALUE_TYPE_LOG:
						if (NULL != cache->history[f].value.str)
						{
							history[history_num].value.str =
									zbx_strdup(NULL, cache->history[f].value.str);
							cache->text_free += strlen(cache->history[f].value.str) + 1;
						}
					case ITEM_VALUE_TYPE_STR:
					case ITEM_VALUE_TYPE_TEXT:
						history[history_num].value_orig.str =
								zbx_strdup(NULL, cache->history[f].value_orig.str);
						cache->text_free += strlen(cache->history[f].value_orig.str) + 1;
						break;
				}
			}

			if (f == cache->history_first)
			{
				cache->history_num--;
				if (ZBX_HISTORY_SIZE == ++cache->history_first)
					cache->history_first = 0;
			}
			else
			{
				cache->history[f].itemid = 0;
				cache->history[f].num = 1;
				cache->history_gap_num++;
			}

			history_num++;
			n -= num;
			f += num;
		}

		if (ZBX_HISTORY_SIZE <= (f = cache->history_first + cache->history_num))
			f -= ZBX_HISTORY_SIZE;

		for (n = cache->history_num; 0 < n; n--)
		{
			if (0 == f)
				f = ZBX_HISTORY_SIZE;
			f--;

			if (0 != cache->history[f].itemid)
				break;

			cache->history_num--;
			cache->history_gap_num--;
		}

		UNLOCK_CACHE;

		if (0 == history_num)
			break;

		DCinit_nextchecks();

		DBbegin();

		if (0 != (daemon_type & ZBX_DAEMON_TYPE_SERVER))
		{
            char *process_type_str = get_process_type_string(process_type);
            
            sec = zbx_time();
            DCmass_update_items(history, history_num);
            sec = zbx_time() - sec;
            zabbix_log(LOG_LEVEL_DEBUG, "[%s]#%d: sync %d, DCmass_update_items: " 
                ZBX_FS_DBL " seconds", process_type_str, process_num, syncs, sec);
            
            sec = zbx_time();    
            DCmass_add_history(history, history_num);
            sec = zbx_time() - sec;
            zabbix_log(LOG_LEVEL_DEBUG, "[%s]#%d: sync %d, DCmass_add_history: " 
                ZBX_FS_DBL " seconds", process_type_str, process_num, syncs, sec);
            
            sec = zbx_time(); 
            DCmass_update_triggers(history, history_num);
            sec = zbx_time() - sec;
            zabbix_log(LOG_LEVEL_DEBUG, "[%s]#%d: sync %d, DCmass_update_triggers: " 
                ZBX_FS_DBL " seconds", process_type_str, process_num, syncs, sec);

            sec = zbx_time();
            DCmass_update_trends(history, history_num);
            sec = zbx_time() - sec;
            zabbix_log(LOG_LEVEL_DEBUG, "[%s]#%d: sync %d, DCmass_update_trends: " 
                ZBX_FS_DBL " seconds", process_type_str, process_num, syncs, sec);
            
            sec = zbx_time();
            DCmass_analyze(history, history_num);
            sec = zbx_time() - sec;
            zabbix_log(LOG_LEVEL_DEBUG, "[%s]#%d: sync %d, DCmass_analyze: " 
                ZBX_FS_DBL " seconds", process_type_str, process_num, syncs, sec);
		}
		else
		{
			DCmass_proxy_add_history(history, history_num);
			DCmass_proxy_update_items(history, history_num);
		}

		DBcommit();

		DCflush_nextchecks();

		if (0 != (daemon_type & ZBX_DAEMON_TYPE_SERVER))
		{
			LOCK_CACHE;

			for (i = 0; i < history_num; i ++)
				uint64_array_remove(cache->itemids, &cache->itemids_num, &history[i].itemid, 1);

			UNLOCK_CACHE;
		}

		for (i = 0; i < history_num; i++)
		{
			if (ITEM_STATUS_NOTSUPPORTED == history[i].status)
			{
				zbx_free(history[i].value_orig.err);
			}
			else
			{
				switch (history[i].value_type)
				{
					case ITEM_VALUE_TYPE_LOG:
						zbx_free(history[i].value.str);
					case ITEM_VALUE_TYPE_STR:
					case ITEM_VALUE_TYPE_TEXT:
						zbx_free(history[i].value_orig.str);
						break;
				}
			}
		}

		total_num += history_num;

		if (ZBX_SYNC_FULL == sync_type && time(NULL) - now >= 10)
		{
			zabbix_log(LOG_LEVEL_WARNING, "syncing history data... " ZBX_FS_DBL "%%",
					(double)total_num / (cache->history_num + total_num) * 100);
			now = time(NULL);
		}
	}
	while (--syncs > 0 || sync_type == ZBX_SYNC_FULL || (skipped_clock != 0 && skipped_clock < max_delay));
finish:
	if (ZBX_SYNC_FULL == sync_type)
		zabbix_log(LOG_LEVEL_WARNING, "syncing history data done");

	return total_num;
}

static void	DCmove_history(int src, int n_data, int n_gap)
{
	int	dst, n_data1, n_data2;

	dst = src + n_gap;

	if (ZBX_HISTORY_SIZE <= dst || ZBX_HISTORY_SIZE >= dst + n_data)
	{
		if (ZBX_HISTORY_SIZE <= dst)
			dst -= ZBX_HISTORY_SIZE;
		memmove(&cache->history[dst], &cache->history[src], n_data * sizeof(ZBX_DC_HISTORY));
	}
	else
	{
		n_data2 = dst + n_data - ZBX_HISTORY_SIZE;
		n_data1 = n_data - n_data2;
		memmove(&cache->history[0], &cache->history[src + n_data1], n_data2 * sizeof(ZBX_DC_HISTORY));
		memmove(&cache->history[dst], &cache->history[src], n_data1 * sizeof(ZBX_DC_HISTORY));
	}
}

static void	DCvacuum_history()
{
	const char	*__function_name = "DCvacuum_history";
	int		n, f, n_gap = 0, n_data = 0;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() history_gap_num:%d/%d",
			__function_name, cache->history_gap_num, ZBX_HISTORY_SIZE);

	if (ZBX_HISTORY_SIZE / 100 >= cache->history_gap_num)
		goto exit;

	if (ZBX_HISTORY_SIZE <= (f = cache->history_first + cache->history_num))
		f -= ZBX_HISTORY_SIZE;

	for (n = cache->history_num; 0 < n; n--)
	{
		if (0 == f)
			f = ZBX_HISTORY_SIZE;
		f--;

		if (0 == cache->history[f].itemid)
		{
			if (0 != n_data)
			{
				DCmove_history(f + 1, n_data, n_gap);
				n_data = 0;
			}

			n_gap++;
		}
		else if (0 != n_gap)
		{
			n_data++;

			if (0 == f)
			{
				DCmove_history(f, n_data, n_gap);
				n_data = 0;
			}
		}
	}

	if (0 != n_data)
		DCmove_history(f, n_data, n_gap);

	cache->history_num -= n_gap;
	cache->history_gap_num -= n_gap;
	if (ZBX_HISTORY_SIZE <= (cache->history_first += n_gap))
		cache->history_first -= ZBX_HISTORY_SIZE;
exit:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}

static void	DCmove_text(char **str)
{
	size_t	sz;

	sz = strlen(*str) + 1;

	if (cache->last_text != *str)
	{
		memmove(cache->last_text, *str, sz);
		*str = cache->last_text;
	}

	cache->last_text += sz;
}

/******************************************************************************
 *                                                                            *
 * Function: DCvacuum_text                                                    *
 *                                                                            *
 * Author: Alexander Vladishev                                                *
 *                                                                            *
 ******************************************************************************/
static void	DCvacuum_text()
{
	const char	*__function_name = "DCvacuum_text";
	int		n, f;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() text_free:%d/%d",
			__function_name, cache->text_free, CONFIG_TEXT_CACHE_SIZE);

	if (CONFIG_TEXT_CACHE_SIZE / 1024 >= cache->text_free)
		goto exit;

	cache->last_text = cache->text;

	for (n = cache->history_num, f = cache->history_first; 0 < n; n--, f++)
	{
		if (ZBX_HISTORY_SIZE == f)
			f = 0;

		if (ITEM_STATUS_NOTSUPPORTED == cache->history[f].status)
		{
			DCmove_text(&cache->history[f].value_orig.err);
			continue;
		}

		switch (cache->history[f].value_type)
		{
			case ITEM_VALUE_TYPE_STR:
			case ITEM_VALUE_TYPE_TEXT:
				DCmove_text(&cache->history[f].value_orig.str);
				break;
			case ITEM_VALUE_TYPE_LOG:
				DCmove_text(&cache->history[f].value_orig.str);
				if (NULL != cache->history[f].value.str)
					DCmove_text(&cache->history[f].value.str);
				break;
		}
	}
exit:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}

/******************************************************************************
 *                                                                            *
 * Function: DCget_history_ptr                                                *
 *                                                                            *
 * Author: Alexander Vladishev                                                *
 *                                                                            *
 ******************************************************************************/
static ZBX_DC_HISTORY	*DCget_history_ptr(size_t text_len)
{
	ZBX_DC_HISTORY	*history;
	int		f;
	size_t		free_len;
retry:
	if (cache->history_num == ZBX_HISTORY_SIZE)
	{
		DCvacuum_history();

		if (cache->history_num == ZBX_HISTORY_SIZE)
		{
			UNLOCK_CACHE;

			zabbix_log(LOG_LEVEL_DEBUG, "History buffer is full. Sleeping for 1 second.");
			sleep(1);

			LOCK_CACHE;

			goto retry;
		}
	}

	if (0 != text_len)
	{
		if (text_len > CONFIG_TEXT_CACHE_SIZE)
		{
			zabbix_log(LOG_LEVEL_ERR, "insufficient shared memory for text cache");
			exit(-1);
		}

		free_len = CONFIG_TEXT_CACHE_SIZE - (cache->last_text - cache->text);

		if (text_len > free_len)
		{
			DCvacuum_text();

			free_len = CONFIG_TEXT_CACHE_SIZE - (cache->last_text - cache->text);

			if (text_len > free_len)
			{
				UNLOCK_CACHE;

				zabbix_log(LOG_LEVEL_DEBUG, "History text buffer is full. Sleeping for 1 second.");
				sleep(1);

				LOCK_CACHE;

				goto retry;
			}
		}
	}

	if (ZBX_HISTORY_SIZE <= (f = (cache->history_first + cache->history_num)))
		f -= ZBX_HISTORY_SIZE;
	history = &cache->history[f];
	history->num = 1;
	history->keep_history = 0;
	history->keep_trends = 0;

	cache->history_num++;

	return history;
}

static void	DCcheck_ns(zbx_timespec_t *ts)
{
	if (ts->ns >= 0)
		return;

	ts->ns = cache->last_ts.ns++;
	if ((cache->last_ts.ns > 999900000 && cache->last_ts.sec != ts->sec) || cache->last_ts.ns == 1000000000)
		cache->last_ts.ns = 0;
	cache->last_ts.sec = ts->sec;
}

static void	DCadd_text(char **dst, const char *src, size_t len)
{
	*dst = cache->last_text;
	cache->last_text += len;
	cache->text_free -= len;

	len--;	/* '\0' */
	memcpy(*dst, src, len);
	(*dst)[len] = '\0';
}

static void	DCadd_history(zbx_uint64_t itemid, double value_orig, zbx_timespec_t *ts)
{
	ZBX_DC_HISTORY	*history;

	LOCK_CACHE;

	DCcheck_ns(ts);

	history = DCget_history_ptr(0);

	history->itemid = itemid;
	history->clock = ts->sec;
	history->ns = ts->ns;
	history->status = ITEM_STATUS_ACTIVE;
	history->value_type = ITEM_VALUE_TYPE_FLOAT;
	history->value_orig.dbl = value_orig;
	history->value.dbl = 0;
	history->value_null = 0;

	cache->stats.history_counter++;
	cache->stats.history_float_counter++;

	UNLOCK_CACHE;
}

static void	DCadd_history_uint(zbx_uint64_t itemid, zbx_uint64_t value_orig, zbx_timespec_t *ts)
{
	ZBX_DC_HISTORY	*history;

	LOCK_CACHE;

	DCcheck_ns(ts);

	history = DCget_history_ptr(0);

	history->itemid = itemid;
	history->clock = ts->sec;
	history->ns = ts->ns;
	history->status = ITEM_STATUS_ACTIVE;
	history->value_type = ITEM_VALUE_TYPE_UINT64;
	history->value_orig.ui64 = value_orig;
	history->value.ui64 = 0;
	history->value_null = 0;

	cache->stats.history_counter++;
	cache->stats.history_uint_counter++;

	UNLOCK_CACHE;
}

static void	DCadd_history_str(zbx_uint64_t itemid, const char *value_orig, zbx_timespec_t *ts)
{
	ZBX_DC_HISTORY	*history;
	size_t		len;

	len = zbx_strlen_utf8_n(value_orig, HISTORY_STR_VALUE_LEN) + 1;

	LOCK_CACHE;

	DCcheck_ns(ts);

	history = DCget_history_ptr(len);

	history->itemid = itemid;
	history->clock = ts->sec;
	history->ns = ts->ns;
	history->status = ITEM_STATUS_ACTIVE;
	history->value_type = ITEM_VALUE_TYPE_STR;
	DCadd_text(&history->value_orig.str, value_orig, len);
	history->value_null = 0;

	cache->stats.history_counter++;
	cache->stats.history_str_counter++;

	UNLOCK_CACHE;
}

static void	DCadd_history_text(zbx_uint64_t itemid, const char *value_orig, zbx_timespec_t *ts)
{
	ZBX_DC_HISTORY	*history;
	size_t		len;

	len = zbx_strlen_utf8_n(value_orig, HISTORY_TEXT_VALUE_LEN) + 1;

	LOCK_CACHE;

	DCcheck_ns(ts);

	history = DCget_history_ptr(len);

	history->itemid = itemid;
	history->clock = ts->sec;
	history->ns = ts->ns;
	history->status = ITEM_STATUS_ACTIVE;
	history->value_type = ITEM_VALUE_TYPE_TEXT;
	DCadd_text(&history->value_orig.str, value_orig, len);
	history->value_null = 0;

	cache->stats.history_counter++;
	cache->stats.history_text_counter++;

	UNLOCK_CACHE;
}

/* lld item values should be stored without a limit */
static void	DCadd_history_lld(zbx_uint64_t itemid, const char *value_orig, zbx_timespec_t *ts)
{
	ZBX_DC_HISTORY	*history;
	size_t		len;

	len = strlen(value_orig) + 1;

	LOCK_CACHE;

	DCcheck_ns(ts);

	history = DCget_history_ptr(len);

	history->itemid = itemid;
	history->clock = ts->sec;
	history->ns = ts->ns;
	history->status = ITEM_STATUS_ACTIVE;
	history->value_type = ITEM_VALUE_TYPE_TEXT;
	DCadd_text(&history->value_orig.str, value_orig, len);
	history->value_null = 0;

	cache->stats.history_counter++;
	cache->stats.history_text_counter++;

	UNLOCK_CACHE;
}

static void	DCadd_history_log(zbx_uint64_t itemid, const char *value_orig, zbx_timespec_t *ts,
		int timestamp, const char *source, int severity, int logeventid, zbx_uint64_t lastlogsize, int mtime)
{
	ZBX_DC_HISTORY	*history;
	size_t		len1, len2;

	len1 = zbx_strlen_utf8_n(value_orig, HISTORY_LOG_VALUE_LEN) + 1;

	if (NULL != source && '\0' != *source)
		len2 = zbx_strlen_utf8_n(source, HISTORY_LOG_SOURCE_LEN) + 1;
	else
		len2 = 0;

	LOCK_CACHE;

	DCcheck_ns(ts);

	history = DCget_history_ptr(len1 + len2);

	history->itemid = itemid;
	history->clock = ts->sec;
	history->ns = ts->ns;
	history->status = ITEM_STATUS_ACTIVE;
	history->value_type = ITEM_VALUE_TYPE_LOG;
	DCadd_text(&history->value_orig.str, value_orig, len1);
	history->value_null = 0;
	history->timestamp = timestamp;

	if (0 != len2)
		DCadd_text(&history->value.str, source, len2);
	else
		history->value.str = NULL;

	history->severity = severity;
	history->logeventid = logeventid;
	history->lastlogsize = lastlogsize;
	history->mtime = mtime;

	cache->stats.history_counter++;
	cache->stats.history_log_counter++;

	UNLOCK_CACHE;
}

static void	DCadd_history_notsupported(zbx_uint64_t itemid, const char *error, zbx_timespec_t *ts)
{
	ZBX_DC_HISTORY	*history;
	size_t		len;

	len = zbx_strlen_utf8_n(error, ITEM_ERROR_LEN) + 1;

	LOCK_CACHE;

	DCcheck_ns(ts);

	history = DCget_history_ptr(len);

	history->itemid = itemid;
	history->clock = ts->sec;
	history->ns = ts->ns;
	history->status = ITEM_STATUS_NOTSUPPORTED;
	DCadd_text(&history->value_orig.err, error, len);
	history->value_null = 1;

	cache->stats.notsupported_counter++;

	UNLOCK_CACHE;
}

/******************************************************************************
 *                                                                            *
 * Function: dc_add_history                                                   *
 *                                                                            *
 * Purpose: add new value to the cache                                        *
 *                                                                            *
 * Author: Alexander Vladishev                                                *
 *                                                                            *
 ******************************************************************************/
void	dc_add_history(zbx_uint64_t itemid, unsigned char value_type, unsigned char flags,
		AGENT_RESULT *value, zbx_timespec_t *ts, unsigned char status, const char *error,
		int timestamp, const char *source, int severity, int logeventid, zbx_uint64_t lastlogsize, int mtime)
{
	if (ITEM_STATUS_NOTSUPPORTED == status)
	{
		DCadd_history_notsupported(itemid, error, ts);
		return;
	}

	if (0 != (ZBX_FLAG_DISCOVERY & flags))
	{
		/* server processes low-level discovery (lld) items while proxy stores their values in db */
		if (0 != (ZBX_DAEMON_TYPE_SERVER & daemon_type))
		{
			DBlld_process_discovery_rule(itemid, value->text, ts);
		}
		else
		{
			if (GET_TEXT_RESULT(value))
				DCadd_history_lld(itemid, value->text, ts);
		}

		return;
	}

	switch (value_type)
	{
		case ITEM_VALUE_TYPE_FLOAT:
			if (GET_DBL_RESULT(value))
				DCadd_history(itemid, value->dbl, ts);
			break;
		case ITEM_VALUE_TYPE_UINT64:
			if (GET_UI64_RESULT(value))
				DCadd_history_uint(itemid, value->ui64, ts);
			break;
		case ITEM_VALUE_TYPE_STR:
			if (GET_STR_RESULT(value))
				DCadd_history_str(itemid, value->str, ts);
			break;
		case ITEM_VALUE_TYPE_TEXT:
			if (GET_TEXT_RESULT(value))
				DCadd_history_text(itemid, value->text, ts);
			break;
		case ITEM_VALUE_TYPE_LOG:
			if (GET_STR_RESULT(value))
			{
				DCadd_history_log(itemid, value->str, ts, timestamp, source,
						severity, logeventid, lastlogsize, mtime);
			}
			break;
		default:
			zabbix_log(LOG_LEVEL_ERR, "unknown value type [%d] for itemid [" ZBX_FS_UI64 "]",
					value_type, itemid);
	}
}

/******************************************************************************
 *                                                                            *
 * Function: init_trend_cache                                                 *
 *                                                                            *
 * Purpose: Allocate shared memory for trend cache (part of database cache)   *
 *                                                                            *
 * Author: Vladimir Levijev                                                   *
 *                                                                            *
 * Comments: Is optionally called from init_database_cache()                  *
 *                                                                            *
 ******************************************************************************/

ZBX_MEM_FUNC_IMPL(__trend, trend_mem);

static void	init_trend_cache()
{
	const char	*__function_name = "init_trend_cache";
	key_t		trend_shm_key;
	size_t		sz;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	if (-1 == (trend_shm_key = zbx_ftok(CONFIG_FILE, ZBX_IPC_TREND_ID)))
	{
		zabbix_log(LOG_LEVEL_CRIT, "cannot create IPC key for trend cache");
		exit(FAIL);
	}

	if (ZBX_MUTEX_ERROR == zbx_mutex_create_force(&trends_lock, ZBX_MUTEX_TRENDS))
	{
		zbx_error("cannot create mutex for trend cache");
		exit(FAIL);
	}
    
    if (ZBX_MUTEX_ERROR == zbx_mutex_create_force(&trends_db_lock, ZBX_MUTEX_TRENDS_DB))
	{
		zbx_error("cannot create mutex for trends db write cache");
		exit(FAIL);
	}

	sz = zbx_mem_required_size(CONFIG_TRENDS_CACHE_SIZE, 1, "trend cache", "TrendCacheSize");
	zbx_mem_create(&trend_mem, trend_shm_key, ZBX_NO_MUTEX, sz, "trend cache", "TrendCacheSize");

	cache->trends_num = 0;

#define INIT_HASHSET_SIZE	1000	/* should be calculated dynamically based on trends size? */

	zbx_hashset_create_ext(&cache->trends, INIT_HASHSET_SIZE,
			ZBX_DEFAULT_UINT64_HASH_FUNC, ZBX_DEFAULT_UINT64_COMPARE_FUNC,
			__trend_mem_malloc_func, __trend_mem_realloc_func, __trend_mem_free_func);

#undef INIT_HASHSET_SIZE

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}

ZBX_MEM_FUNC_IMPL(__analyzer_avail_uptimes, analyzer_avail_uptimes_mem);
ZBX_MEM_FUNC_IMPL(__analyzer_avail_pings, analyzer_avail_pings_mem);
ZBX_MEM_FUNC_IMPL(__ANALYZER_AVAIL_UPTIMES2, analyzer_avail_uptimes2_mem);
ZBX_MEM_FUNC_IMPL(__ANALYZER_AVAIL_PINGS2, analyzer_avail_pings2_mem);

static void	init_analyzer_cache(char ipc_id, int mutex_id, ZBX_MUTEX *mutex, zbx_mem_info_t *mem_info,
        zbx_hashset_t *hashset,
        zbx_mem_malloc_func_t mem_malloc_func,
        zbx_mem_realloc_func_t mem_realloc_func,
		zbx_mem_free_func_t mem_free_func)
{
	const char	*__function_name = "init_analyzer_cache";
	key_t		shm_key;
	size_t		sz;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	if (-1 == (shm_key = zbx_ftok(CONFIG_FILE, ipc_id))) {
		zabbix_log(LOG_LEVEL_CRIT, "cannot create IPC key for analyzer avail cache");
		exit(FAIL);
	}

	if (ZBX_MUTEX_ERROR == zbx_mutex_create_force(mutex, mutex_id)) {
		zbx_error("cannot create mutex for analyzer avail cache");
		exit(FAIL);
	}

	sz = zbx_mem_required_size(CONFIG_ANALYZER_AVAIL_UPTIMES_CACHE_SIZE, 1,
            "analyzer avail cache", "AnalyzerCacheSize");
	zbx_mem_create(mem_info, shm_key, ZBX_NO_MUTEX, sz,
            "analyzer avail cache", "AnalyzerCacheSize");

#define INIT_HASHSET_SIZE	300

	zbx_hashset_create_ext(hashset, INIT_HASHSET_SIZE,
			ZBX_DEFAULT_UINT64_HASH_FUNC, ZBX_DEFAULT_UINT64_COMPARE_FUNC,
			mem_malloc_func, mem_realloc_func, mem_free_func);

#undef INIT_HASHSET_SIZE

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}

/******************************************************************************
 *                                                                            *
 * Function: init_database_cache                                              *
 *                                                                            *
 * Purpose: Allocate shared memory for database cache                         *
 *                                                                            *
 * Author: Alexei Vladishev, Alexander Vladishev                              *
 *                                                                            *
 ******************************************************************************/

ZBX_MEM_FUNC1_IMPL_MALLOC(__history, history_mem);
ZBX_MEM_FUNC1_IMPL_MALLOC(__history_text, history_text_mem);

void	init_database_cache()
{
	const char	*__function_name = "init_database_cache";
	key_t		history_shm_key, history_text_shm_key;
	size_t		sz;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	if (-1 == (history_shm_key = zbx_ftok(CONFIG_FILE, ZBX_IPC_HISTORY_ID)) ||
			-1 == (history_text_shm_key = zbx_ftok(CONFIG_FILE, ZBX_IPC_HISTORY_TEXT_ID)))
	{
		zabbix_log(LOG_LEVEL_CRIT, "cannot create IPC keys for history cache");
		exit(FAIL);
	}

	if (ZBX_MUTEX_ERROR == zbx_mutex_create_force(&cache_lock, ZBX_MUTEX_CACHE))
	{
		zbx_error("cannot create mutex for history cache");
		exit(FAIL);
	}

	if (ZBX_MUTEX_ERROR == zbx_mutex_create_force(&cache_ids_lock, ZBX_MUTEX_CACHE_IDS))
	{
		zbx_error("cannot create mutex for IDs cache");
		exit(FAIL);
	}
    
    if (ZBX_MUTEX_ERROR == zbx_mutex_create_force(&analyzer_avail_q_lock, ZBX_MUTEX_ANALYZER_AVAIL_Q))
	{
		zbx_error("cannot create mutex for analyzer uptime cache");
		exit(FAIL);
	}

	ZBX_HISTORY_SIZE = CONFIG_HISTORY_CACHE_SIZE / sizeof(ZBX_DC_HISTORY);
    ZBX_TRENDS_DB_SIZE = CONFIG_TRENDS_DB_CACHE_SIZE / sizeof(ZBX_DC_TREND);
    ZBX_ANALYZER_AVAIL_Q_SIZE = CONFIG_ANALYZER_AVAIL_Q_CACHE_SIZE / sizeof(ZBX_DC_ANALYZER_AVAIL_METRIC);
	if (ZBX_SYNC_MAX > ZBX_HISTORY_SIZE)
		ZBX_SYNC_MAX = ZBX_HISTORY_SIZE;
	ZBX_ITEMIDS_SIZE = CONFIG_HISTSYNCER_FORKS * ZBX_SYNC_MAX;

	/* history cache */

	sz = sizeof(ZBX_DC_CACHE);
	sz += ZBX_HISTORY_SIZE * sizeof(ZBX_DC_HISTORY);
    sz += ZBX_TRENDS_DB_SIZE * sizeof(ZBX_DC_TREND);
    sz += ZBX_ANALYZER_AVAIL_Q_SIZE * sizeof(ZBX_DC_ANALYZER_AVAIL_METRIC);
	sz += ZBX_ITEMIDS_SIZE * sizeof(zbx_uint64_t);
	sz += sizeof(ZBX_DC_IDS);
	sz = zbx_mem_required_size(sz, 4, "history cache", "HistoryCacheSize");

	zbx_mem_create(&history_mem, history_shm_key, ZBX_NO_MUTEX, sz, "history cache", "HistoryCacheSize");

	cache = (ZBX_DC_CACHE *)__history_mem_malloc_func(NULL, sizeof(ZBX_DC_CACHE));

	cache->history = (ZBX_DC_HISTORY *)__history_mem_malloc_func(NULL, ZBX_HISTORY_SIZE * sizeof(ZBX_DC_HISTORY));
	cache->history_first = 0;
	cache->history_num = 0;
	cache->itemids = (zbx_uint64_t *)__history_mem_malloc_func(NULL, ZBX_ITEMIDS_SIZE * sizeof(zbx_uint64_t));
	cache->itemids_alloc = ZBX_ITEMIDS_SIZE;
	cache->itemids_num = 0;
    cache->trends_db = (ZBX_DC_TREND *)__history_mem_malloc_func(NULL, ZBX_TRENDS_DB_SIZE * sizeof(ZBX_DC_TREND));
    cache->trends_num_db = 0;
    cache->analyzer_avail_q = (ZBX_DC_ANALYZER_AVAIL_METRIC *)__history_mem_malloc_func(NULL, ZBX_ANALYZER_AVAIL_Q_SIZE * sizeof(ZBX_DC_ANALYZER_AVAIL_METRIC));
    cache->analyzer_avail_q_num = 0;
	memset(&cache->stats, 0, sizeof(ZBX_DC_STATS));

	ids = (ZBX_DC_IDS *)__history_mem_malloc_func(NULL, sizeof(ZBX_DC_IDS));
	memset(ids, 0, sizeof(ZBX_DC_IDS));

	/* history text cache */

	sz = zbx_mem_required_size(CONFIG_TEXT_CACHE_SIZE, 1, "history text cache", "HistoryTextCacheSize");

	zbx_mem_create(&history_text_mem, history_text_shm_key, ZBX_NO_MUTEX, sz, "history text cache", "HistoryTextCacheSize");

	cache->text = (char *)__history_text_mem_malloc_func(NULL, CONFIG_TEXT_CACHE_SIZE);
	cache->last_text = cache->text;
	cache->text_free = CONFIG_TEXT_CACHE_SIZE;

	/* trend cache */
	if (0 != (daemon_type & ZBX_DAEMON_TYPE_SERVER))
		init_trend_cache();
    
    init_analyzer_cache(ZBX_IPC_ANALYZER_AVAIL_UPTIMES_ID, ZBX_MUTEX_ANALYZER_AVAIL_UPTIMES,
            &analyzer_avail_uptimes_lock, &analyzer_avail_uptimes_mem,
            &cache->analyzer_avail_uptimes,
            __analyzer_avail_uptimes_mem_malloc_func,
            __analyzer_avail_uptimes_mem_realloc_func,
            __analyzer_avail_uptimes_mem_free_func);
    init_analyzer_cache(ZBX_IPC_ANALYZER_AVAIL_PINGS_ID, ZBX_MUTEX_ANALYZER_AVAIL_PINGS,
            &analyzer_avail_pings_lock, &analyzer_avail_pings_mem,
            &cache->analyzer_avail_pings,
            __analyzer_avail_pings_mem_malloc_func,
            __analyzer_avail_pings_mem_realloc_func,
            __analyzer_avail_pings_mem_free_func);
    init_analyzer_cache(ZBX_IPC_ANALYZER_AVAIL_UPTIMES2_ID, ZBX_MUTEX_ANALYZER_AVAIL_UPTIMES2,
            &analyzer_avail_uptimes2_lock, &analyzer_avail_uptimes2_mem,
            &cache->analyzer_avail_uptimes2,
            __ANALYZER_AVAIL_UPTIMES2_mem_malloc_func,
            __ANALYZER_AVAIL_UPTIMES2_mem_realloc_func,
            __ANALYZER_AVAIL_UPTIMES2_mem_free_func);
    init_analyzer_cache(ZBX_IPC_ANALYZER_AVAIL_PINGS2_ID, ZBX_MUTEX_ANALYZER_AVAIL_PINGS2,
            &analyzer_avail_pings2_lock, &analyzer_avail_pings2_mem,
            &cache->analyzer_avail_pings2,
            __ANALYZER_AVAIL_PINGS2_mem_malloc_func,
            __ANALYZER_AVAIL_PINGS2_mem_realloc_func,
            __ANALYZER_AVAIL_PINGS2_mem_free_func);

    analyzer_avail_load_all();

	cache->last_ts.sec = 0;
	cache->last_ts.ns = 0;

	if (NULL == sql)
		sql = zbx_malloc(sql, sql_alloc);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}

/******************************************************************************
 *                                                                            *
 * Function: DCsync_all                                                       *
 *                                                                            *
 * Purpose: writes updates and new data from pool and cache data to database  *
 *                                                                            *
 * Author: Alexei Vladishev                                                   *
 *                                                                            *
 ******************************************************************************/
static void	DCsync_all()
{
	zabbix_log(LOG_LEVEL_DEBUG, "In DCsync_all()");

	DCsync_history(ZBX_SYNC_FULL);
	if (CONFIG_TRENDS_SQL_WRITE && 0 != (daemon_type & ZBX_DAEMON_TYPE_SERVER))
		DCsync_trends();
    
    zabbix_log(LOG_LEVEL_INFORMATION, "[ANALYZER/AVAIL] Flushing avail data...");

    analyzer_avail_check(&cache->analyzer_avail_uptimes, CONFIG_ANALYZER_AVAIL_INTERVAL1);
    analyzer_avail_check(&cache->analyzer_avail_pings, CONFIG_ANALYZER_AVAIL_INTERVAL1);
    analyzer_avail_check(&cache->analyzer_avail_uptimes2, CONFIG_ANALYZER_AVAIL_INTERVAL2);
    analyzer_avail_check(&cache->analyzer_avail_pings2, CONFIG_ANALYZER_AVAIL_INTERVAL2);

    analyzer_avail_store_check(&cache->analyzer_avail_uptimes,
            CONFIG_ANALYZER_AVAIL_INTERVAL1, CONFIG_ANALYZER_AVAIL_STORE_INTERVAL, "uptime", 1);
    analyzer_avail_store_check(&cache->analyzer_avail_pings,
            CONFIG_ANALYZER_AVAIL_INTERVAL1, CONFIG_ANALYZER_AVAIL_STORE_INTERVAL, "ping", 1);
    analyzer_avail_store_check(&cache->analyzer_avail_uptimes2,
            CONFIG_ANALYZER_AVAIL_INTERVAL2, CONFIG_ANALYZER_AVAIL_STORE_INTERVAL, "uptime", 1);
    analyzer_avail_store_check(&cache->analyzer_avail_pings2,
            CONFIG_ANALYZER_AVAIL_INTERVAL2, CONFIG_ANALYZER_AVAIL_STORE_INTERVAL, "ping", 1);

    zabbix_log(LOG_LEVEL_INFORMATION, "[ANALYZER/AVAIL] Flushed avail data");

	zabbix_log(LOG_LEVEL_DEBUG, "End of DCsync_all()");
}

/******************************************************************************
 *                                                                            *
 * Function: free_database_cache                                              *
 *                                                                            *
 * Purpose: Free memory allocated for database cache                          *
 *                                                                            *
 * Author: Alexei Vladishev, Alexander Vladishev                              *
 *                                                                            *
 ******************************************************************************/
void	free_database_cache()
{
	const char	*__function_name = "free_database_cache";

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	DCsync_all();

	cache = NULL;
	zbx_mem_destroy(history_mem);
	zbx_mem_destroy(history_text_mem);
	if (0 != (daemon_type & ZBX_DAEMON_TYPE_SERVER))
		zbx_mem_destroy(trend_mem);

	zbx_mutex_destroy(&cache_lock);
	zbx_mutex_destroy(&cache_ids_lock);
	if (0 != (daemon_type & ZBX_DAEMON_TYPE_SERVER)) {
		zbx_mutex_destroy(&trends_lock);
        zbx_mutex_destroy(&trends_db_lock);
    }

    zbx_free(sql);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}

/******************************************************************************
 *                                                                            *
 * Function: DCget_nextid                                                     *
 *                                                                            *
 * Purpose: Return next id for requested table                                *
 *                                                                            *
 * Author: Alexander Vladishev                                                *
 *                                                                            *
 ******************************************************************************/
zbx_uint64_t	DCget_nextid(const char *table_name, int num)
{
	const char	*__function_name = "DCget_nextid";
	int		i, nodeid;
	DB_RESULT	result;
	DB_ROW		row;
	const ZBX_TABLE	*table;
	ZBX_DC_ID	*id;
	zbx_uint64_t	min, max, nextid;

	LOCK_CACHE_IDS;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() table:'%s' num:%d",
			__function_name, table_name, num);

	for (i = 0; i < ZBX_IDS_SIZE; i++)
	{
		id = &ids->id[i];
		if ('\0' == *id->table_name)
			break;

		if (0 == strcmp(id->table_name, table_name))
		{
			nextid = id->lastid + 1;
			id->lastid += num;

			zabbix_log(LOG_LEVEL_DEBUG, "End of %s() table:'%s' [" ZBX_FS_UI64 ":" ZBX_FS_UI64 "]",
					__function_name, table_name, nextid, id->lastid);

			UNLOCK_CACHE_IDS;

			return nextid;
		}
	}

	if (i == ZBX_IDS_SIZE)
	{
		zabbix_log(LOG_LEVEL_ERR, "insufficient shared memory for ids");
		exit(-1);
	}

	zbx_strlcpy(id->table_name, table_name, sizeof(id->table_name));

	table = DBget_table(table_name);
	nodeid = CONFIG_NODEID >= 0 ? CONFIG_NODEID : 0;

	min = (zbx_uint64_t)__UINT64_C(100000000000000) * (zbx_uint64_t)nodeid;
	max = (zbx_uint64_t)__UINT64_C(100000000000000) * (zbx_uint64_t)nodeid;

	if (table->flags & ZBX_SYNC)
	{
		min += (zbx_uint64_t)__UINT64_C(100000000000) * (zbx_uint64_t)nodeid;
		max += (zbx_uint64_t)__UINT64_C(100000000000) * (zbx_uint64_t)nodeid + (zbx_uint64_t)__UINT64_C(99999999999);
	}
	else
		max += (zbx_uint64_t)__UINT64_C(99999999999999);

	result = DBselect("select max(%s) from %s where %s between " ZBX_FS_UI64 " and " ZBX_FS_UI64,
			table->recid,
			table_name,
			table->recid,
			min, max);

	if (NULL == (row = DBfetch(result)) || SUCCEED == DBis_null(row[0]))
		id->lastid = min;
	else
		ZBX_STR2UINT64(id->lastid, row[0]);

	nextid = id->lastid + 1;
	id->lastid += num;

	DBfree_result(result);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s() table:'%s' [" ZBX_FS_UI64 ":" ZBX_FS_UI64 "]",
			__function_name, table_name, nextid, id->lastid);

	UNLOCK_CACHE_IDS;

	return nextid;
}

/******************************************************************************
 *                                                                            *
 * Function: DCget_item_lastclock                                             *
 *                                                                            *
 * Return value: last clock or FAIL if item not found in dbcache              *
 *                                                                            *
 * Author: Alexander Vladishev                                                *
 *                                                                            *
 ******************************************************************************/
int	DCget_item_lastclock(zbx_uint64_t itemid)
{
	const char	*__function_name = "DCget_item_lastclock";
	int		i, index, clock = FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() itemid:" ZBX_FS_UI64, __function_name, itemid);

	LOCK_CACHE;

	index = (cache->history_first + cache->history_num - 1) % ZBX_HISTORY_SIZE;

	for (i = cache->history_num - 1; i >= 0; i--)
	{
		if (cache->history[index].itemid == itemid)
		{
			clock = cache->history[index].clock;
			break;
		}

		if (--index < 0)
			index = ZBX_HISTORY_SIZE - 1;
	}

	UNLOCK_CACHE;

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%d", __function_name, clock);

	return clock;
}
