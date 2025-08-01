/*****************************************************************************

Copyright (c) 2000, 2020, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2009, Percona Inc.
Copyright (c) 2012, Facebook Inc.
Copyright (c) 2013, 2023, MariaDB Corporation.

Portions of this file contain modifications contributed and copyrighted
by Percona Inc.. Those modifications are
gratefully acknowledged and are described briefly in the InnoDB
documentation. The contributions by Percona Inc. are incorporated with
their permission, and subject to the conditions contained in the file
COPYING.Percona.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

/** @file ha_innodb.cc */

#include "univ.i"

/* Include necessary SQL headers */
#include "ha_prototypes.h"
#include <debug_sync.h>
#include <gstream.h>
#include <log.h>
#include <mysys_err.h>
#include <innodb_priv.h>
#include <strfunc.h>
#include <sql_acl.h>
#include <lex_ident.h>
#include <sql_class.h>
#include <sql_show.h>
#include <sql_table.h>
#include <table_cache.h>
#include <my_check_opt.h>
#include <my_bitmap.h>
#include <my_sys.h>
#include <mysql/service_thd_alloc.h>
#include <mysql/service_thd_wait.h>
#include <mysql/service_print_check_msg.h>
#include <mysql/service_log_warnings.h>
#include "sql_type_geom.h"
#include "scope.h"
#include "srv0srv.h"

// MYSQL_PLUGIN_IMPORT extern my_bool lower_case_file_system;
// MYSQL_PLUGIN_IMPORT extern char mysql_unpacked_real_data_home[];

#include <my_service_manager.h>
#include <key.h>
#include <sql_manager.h>

/* Include necessary InnoDB headers */
#include "btr0btr.h"
#include "btr0cur.h"
#include "btr0bulk.h"
#include "btr0sea.h"
#include "buf0dblwr.h"
#include "buf0dump.h"
#include "buf0buf.h"
#include "buf0flu.h"
#include "buf0lru.h"
#include "dict0boot.h"
#include "dict0load.h"
#include "dict0crea.h"
#include "dict0stats.h"
#include "dict0stats_bg.h"
#include "fil0fil.h"
#include "fsp0fsp.h"
#include "fts0fts.h"
#include "fts0plugin.h"
#include "fts0priv.h"
#include "fts0types.h"
#include "lock0lock.h"
#include "log0crypt.h"
#include "mtr0mtr.h"
#include "os0file.h"
#include "page0zip.h"
#include "row0import.h"
#include "row0ins.h"
#include "row0log.h"
#include "row0merge.h"
#include "row0mysql.h"
#include "row0quiesce.h"
#include "row0sel.h"
#include "row0upd.h"
#include "fil0crypt.h"
#include "srv0mon.h"
#include "srv0start.h"
#include "rem0rec.h"
#include "trx0purge.h"
#include "trx0roll.h"
#include "trx0rseg.h"
#include "trx0trx.h"
#include "fil0pagecompress.h"
#include "ut0mem.h"
#include "row0ext.h"
#include "mariadb_stats.h"
simple_thread_local ha_handler_stats *mariadb_stats;

#include "lz4.h"
#include "lzo/lzo1x.h"
#include "lzma.h"
#include "bzlib.h"
#include "snappy-c.h"

#include <limits>
#include <myisamchk.h>                          // TT_FOR_UPGRADE
#include "sql_type_vector.h"

#define thd_get_trx_isolation(X) ((enum_tx_isolation)thd_tx_isolation(X))

extern "C" void thd_mark_transaction_to_rollback(MYSQL_THD thd, bool all);
unsigned long long thd_get_query_id(const MYSQL_THD thd);
void thd_clear_error(MYSQL_THD thd);

TABLE *find_fk_open_table(THD *thd, const char *db, size_t db_len,
			  const char *table, size_t table_len);
MYSQL_THD create_background_thd();
void reset_thd(MYSQL_THD thd);
TABLE *get_purge_table(THD *thd);
TABLE *open_purge_table(THD *thd, const char *db, size_t dblen,
			const char *tb, size_t tblen);
void close_thread_tables(THD* thd);

#ifdef MYSQL_DYNAMIC_PLUGIN
#define tc_size  400
#endif

#include <mysql/plugin.h>
#include <mysql/service_wsrep.h>

#include "ha_innodb.h"
#include "i_s.h"

#include <string>
#include <sstream>

#ifdef WITH_WSREP
#include <mysql/service_md5.h>
#include "wsrep_sst.h"
#endif /* WITH_WSREP */

#define INSIDE_HA_INNOBASE_CC

#define EQ_CURRENT_THD(thd) ((thd) == current_thd)

struct handlerton* innodb_hton_ptr;

static const long AUTOINC_OLD_STYLE_LOCKING = 0;
static const long AUTOINC_NEW_STYLE_LOCKING = 1;
static const long AUTOINC_NO_LOCKING = 2;

static ulong innobase_open_files;
static long innobase_autoinc_lock_mode;

/** Percentage of the buffer pool to reserve for 'old' blocks.
Connected to buf_LRU_old_ratio. */
static uint innobase_old_blocks_pct;

static char*	innobase_data_file_path;
static char*	innobase_temp_data_file_path;

/* The default values for the following char* start-up parameters
are determined in innodb_init_params(). */

static char*	innobase_data_home_dir;
static char*	innobase_enable_monitor_counter;
static char*	innobase_disable_monitor_counter;
static char*	innobase_reset_monitor_counter;
static char*	innobase_reset_all_monitor_counter;

/* This variable can be set in the server configure file, specifying
stopword table to be used */
static char*	innobase_server_stopword_table;

my_bool innobase_rollback_on_timeout;
static my_bool	innobase_create_status_file;
my_bool	innobase_stats_on_metadata;
static my_bool	innodb_optimize_fulltext_only;

extern uint srv_fil_crypt_rotate_key_age;
extern uint srv_n_fil_crypt_iops;

#ifdef UNIV_DEBUG
my_bool innodb_evict_tables_on_commit_debug;
#endif

#if defined(UNIV_DEBUG) || \
    defined(INNODB_ENABLE_XAP_UNLOCK_UNMODIFIED_FOR_PRIMARY)
my_bool innodb_enable_xap_unlock_unmodified_for_primary_debug;
#endif /* defined(UNIV_DEBUG) ||
          defined(INNODB_ENABLE_XAP_UNLOCK_UNMODIFIED_FOR_PRIMARY) */

/** File format constraint for ALTER TABLE */
ulong innodb_instant_alter_column_allowed;

/** Note we cannot use rec_format_enum because we do not allow
COMPRESSED row format for innodb_default_row_format option. */
enum default_row_format_enum {
	DEFAULT_ROW_FORMAT_REDUNDANT = 0,
	DEFAULT_ROW_FORMAT_COMPACT = 1,
	DEFAULT_ROW_FORMAT_DYNAMIC = 2,
};

static my_bool innodb_truncate_temporary_tablespace_now;

/** Whether ROW_FORMAT=COMPRESSED tables are read-only */
static my_bool innodb_read_only_compressed;

/** A dummy variable */
static uint innodb_max_purge_lag_wait;

/** Wait for trx_sys.history_size() to be below a limit. */
static void innodb_max_purge_lag_wait_update(THD *thd, st_mysql_sys_var *,
                                             void *, const void *limit)
{
  if (high_level_read_only)
    return;
  const uint l= *static_cast<const uint*>(limit);
  if (!trx_sys.history_exceeds(l))
    return;
  mysql_mutex_unlock(&LOCK_global_system_variables);
  while (trx_sys.history_exceeds(l))
  {
    if (thd_kill_level(thd))
      break;
    /* Adjust for purge_coordinator_state::refresh() */
    log_sys.latch.wr_lock(SRW_LOCK_CALL);
    const lsn_t last= log_sys.last_checkpoint_lsn,
      max_age= log_sys.max_checkpoint_age;
    const lsn_t lsn= log_sys.get_lsn();
    log_sys.latch.wr_unlock();
    if ((lsn - last) / 4 >= max_age / 5)
      buf_flush_ahead(last + max_age / 5, false);
    purge_sys.wake_if_not_active();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  mysql_mutex_lock(&LOCK_global_system_variables);
}

static
void set_my_errno(int err)
{
	errno = err;
}

/** Checks whether the file name belongs to a partition of a table.
@param[in]	file_name	file name
@return pointer to the end of the table name part of the file name, or NULL */
static
char*
is_partition(
/*=========*/
	char*		file_name)
{
	/* We look for pattern #P# to see if the table is partitioned
	MariaDB table. */
	return strstr(file_name, table_name_t::part_suffix);
}



/** Checks whether the file name belongs to a to the hlindex
@param[in]	file_name	file name
@return pointer to the end of the table name part of the file name, or NULL */
static
char*
is_hlindex(
/*=========*/
	char*		file_name)
{
	/* We look for pattern #i# to see if the table is hlindex
	MariaDB table. */
	return strstr(file_name, "#i#");
}



/** Return the InnoDB ROW_FORMAT enum value
@param[in]	row_format	row_format from "innodb_default_row_format"
@return InnoDB ROW_FORMAT value from rec_format_t enum. */
static
rec_format_t
get_row_format(
	ulong row_format)
{
	switch(row_format) {
	case DEFAULT_ROW_FORMAT_REDUNDANT:
		return(REC_FORMAT_REDUNDANT);
	case DEFAULT_ROW_FORMAT_COMPACT:
		return(REC_FORMAT_COMPACT);
	case DEFAULT_ROW_FORMAT_DYNAMIC:
		return(REC_FORMAT_DYNAMIC);
	default:
		ut_ad(0);
		return(REC_FORMAT_DYNAMIC);
	}
}

static ulong	innodb_default_row_format = DEFAULT_ROW_FORMAT_DYNAMIC;

/** Possible values for system variable "innodb_stats_method". The values
are defined the same as its corresponding MyISAM system variable
"myisam_stats_method"(see "myisam_stats_method_names"), for better usability */
static const char* innodb_stats_method_names[] = {
	"nulls_equal",
	"nulls_unequal",
	"nulls_ignored",
	NullS
};

/** Used to define an enumerate type of the system variable innodb_stats_method.
This is the same as "myisam_stats_method_typelib" */
static TYPELIB innodb_stats_method_typelib =
			CREATE_TYPELIB_FOR(innodb_stats_method_names);

/** Possible values of the parameter innodb_checksum_algorithm */
const char* innodb_checksum_algorithm_names[] = {
	"crc32",
	"strict_crc32",
	"full_crc32",
	"strict_full_crc32",
	NullS
};

/** Used to define an enumerate type of the system variable
innodb_checksum_algorithm. */
TYPELIB innodb_checksum_algorithm_typelib =
			CREATE_TYPELIB_FOR(innodb_checksum_algorithm_names);

/** Possible values for system variable "innodb_default_row_format". */
static const char* innodb_default_row_format_names[] = {
	"redundant",
	"compact",
	"dynamic",
	NullS
};

/** Used to define an enumerate type of the system variable
innodb_default_row_format. */
static TYPELIB innodb_default_row_format_typelib =
			CREATE_TYPELIB_FOR(innodb_default_row_format_names);

/** Names of allowed values of innodb_flush_method */
static const char* innodb_flush_method_names[] = {
	"fsync",
	"O_DSYNC",
	"littlesync",
	"nosync",
	"O_DIRECT",
	"O_DIRECT_NO_FSYNC",
#ifdef _WIN32
	"unbuffered",
	"async_unbuffered" /* alias for "unbuffered" */,
	"normal" /* alias for "fsync" */,
#endif
	NullS
};

static constexpr ulong innodb_flush_method_default = IF_WIN(6,4);

/** Enumeration of innodb_flush_method */
TYPELIB innodb_flush_method_typelib =
			CREATE_TYPELIB_FOR(innodb_flush_method_names);

/** Deprecated parameter */
static ulong innodb_flush_method;

/** Names of allowed values of innodb_doublewrite */
static const char *innodb_doublewrite_names[]=
  {"OFF", "ON", "fast", nullptr};

/** Enumeration of innodb_doublewrite */
TYPELIB innodb_doublewrite_typelib=
			CREATE_TYPELIB_FOR(innodb_doublewrite_names);

/** Names of allowed values of innodb_deadlock_report */
static const char *innodb_deadlock_report_names[]= {
	"off", /* Do not report any details of deadlocks */
	"basic", /* Report waiting transactions and lock requests */
	"full", /* Also report blocking locks */
	NullS
};

static_assert(Deadlock::REPORT_OFF == 0, "compatibility");
static_assert(Deadlock::REPORT_BASIC == 1, "compatibility");
static_assert(Deadlock::REPORT_FULL == 2, "compatibility");

/** Enumeration of innodb_deadlock_report */
static TYPELIB innodb_deadlock_report_typelib =
			CREATE_TYPELIB_FOR(innodb_deadlock_report_names);

/** Allowed values of innodb_instant_alter_column_allowed */
const char* innodb_instant_alter_column_allowed_names[] = {
	"never", /* compatible with MariaDB 5.5 to 10.2 */
	"add_last",/* allow instant ADD COLUMN ... LAST */
	"add_drop_reorder", /* allow instant ADD anywhere & DROP & reorder */
	NullS
};

/** Enumeration of innodb_instant_alter_column_allowed */
static TYPELIB innodb_instant_alter_column_allowed_typelib =
		CREATE_TYPELIB_FOR(innodb_instant_alter_column_allowed_names);

/** Retrieve the FTS Relevance Ranking result for doc with doc_id
of m_prebuilt->fts_doc_id
@param[in,out]	fts_hdl	FTS handler
@return the relevance ranking value */
static
float
innobase_fts_retrieve_ranking(
	FT_INFO*	fts_hdl);
/** Free the memory for the FTS handler
@param[in,out]	fts_hdl	FTS handler */
static
void
innobase_fts_close_ranking(
	FT_INFO*	fts_hdl);
/** Find and Retrieve the FTS Relevance Ranking result for doc with doc_id
of m_prebuilt->fts_doc_id
@param[in,out]	fts_hdl	FTS handler
@return the relevance ranking value */
static
float
innobase_fts_find_ranking(
	FT_INFO*	fts_hdl,
	uchar*,
	uint);

/* Call back function array defined by MySQL and used to
retrieve FTS results. */
const struct _ft_vft ft_vft_result = {NULL,
				      innobase_fts_find_ranking,
				      innobase_fts_close_ranking,
				      innobase_fts_retrieve_ranking,
				      NULL};

/** @return version of the extended FTS API */
static
uint
innobase_fts_get_version()
{
	/* Currently this doesn't make much sense as returning
	HA_CAN_FULLTEXT_EXT automatically mean this version is supported.
	This supposed to ease future extensions.  */
	return(2);
}

/** @return Which part of the extended FTS API is supported */
static
ulonglong
innobase_fts_flags()
{
	return(FTS_ORDERED_RESULT | FTS_DOCID_IN_RESULT);
}

/** Find and Retrieve the FTS doc_id for the current result row
@param[in,out]	fts_hdl	FTS handler
@return the document ID */
static
ulonglong
innobase_fts_retrieve_docid(
	FT_INFO_EXT*	fts_hdl);

/** Find and retrieve the size of the current result
@param[in,out]	fts_hdl	FTS handler
@return number of matching rows */
static
ulonglong
innobase_fts_count_matches(
	FT_INFO_EXT*	fts_hdl)	/*!< in: FTS handler */
{
	NEW_FT_INFO*	handle = reinterpret_cast<NEW_FT_INFO*>(fts_hdl);

	if (handle->ft_result->rankings_by_id != NULL) {
		return(rbt_size(handle->ft_result->rankings_by_id));
	} else {
		return(0);
	}
}

const struct _ft_vft_ext ft_vft_ext_result = {innobase_fts_get_version,
					      innobase_fts_flags,
					      innobase_fts_retrieve_docid,
					      innobase_fts_count_matches};

#ifdef HAVE_PSI_INTERFACE
# define PSI_KEY(n) {&n##_key, #n, 0}
/* Keys to register pthread mutexes in the current file with
performance schema */
static mysql_pfs_key_t	pending_checkpoint_mutex_key;

# ifdef UNIV_PFS_MUTEX
mysql_pfs_key_t	buf_pool_mutex_key;
mysql_pfs_key_t	dict_foreign_err_mutex_key;
mysql_pfs_key_t	fil_system_mutex_key;
mysql_pfs_key_t	flush_list_mutex_key;
mysql_pfs_key_t	fts_cache_mutex_key;
mysql_pfs_key_t	fts_cache_init_mutex_key;
mysql_pfs_key_t	fts_delete_mutex_key;
mysql_pfs_key_t	fts_doc_id_mutex_key;
mysql_pfs_key_t	recalc_pool_mutex_key;
mysql_pfs_key_t	purge_sys_pq_mutex_key;
mysql_pfs_key_t	recv_sys_mutex_key;
mysql_pfs_key_t page_zip_stat_per_index_mutex_key;
mysql_pfs_key_t rtr_active_mutex_key;
mysql_pfs_key_t	rtr_match_mutex_key;
mysql_pfs_key_t	rtr_path_mutex_key;
mysql_pfs_key_t	srv_innodb_monitor_mutex_key;
mysql_pfs_key_t	srv_misc_tmpfile_mutex_key;
mysql_pfs_key_t	srv_monitor_file_mutex_key;
mysql_pfs_key_t	buf_dblwr_mutex_key;
mysql_pfs_key_t	trx_pool_mutex_key;
mysql_pfs_key_t	trx_pool_manager_mutex_key;
mysql_pfs_key_t	lock_wait_mutex_key;
mysql_pfs_key_t	trx_sys_mutex_key;
mysql_pfs_key_t	srv_threads_mutex_key;

/* all_innodb_mutexes array contains mutexes that are
performance schema instrumented if "UNIV_PFS_MUTEX"
is defined */
static PSI_mutex_info all_innodb_mutexes[] = {
	PSI_KEY(pending_checkpoint_mutex),
	PSI_KEY(buf_pool_mutex),
	PSI_KEY(dict_foreign_err_mutex),
	PSI_KEY(recalc_pool_mutex),
	PSI_KEY(fil_system_mutex),
	PSI_KEY(flush_list_mutex),
	PSI_KEY(fts_cache_mutex),
	PSI_KEY(fts_cache_init_mutex),
	PSI_KEY(fts_delete_mutex),
	PSI_KEY(fts_doc_id_mutex),
	PSI_KEY(index_online_log),
	PSI_KEY(page_zip_stat_per_index_mutex),
	PSI_KEY(purge_sys_pq_mutex),
	PSI_KEY(recv_sys_mutex),
	PSI_KEY(srv_innodb_monitor_mutex),
	PSI_KEY(srv_misc_tmpfile_mutex),
	PSI_KEY(srv_monitor_file_mutex),
	PSI_KEY(buf_dblwr_mutex),
	PSI_KEY(trx_pool_mutex),
	PSI_KEY(trx_pool_manager_mutex),
	PSI_KEY(lock_wait_mutex),
	PSI_KEY(srv_threads_mutex),
	PSI_KEY(rtr_active_mutex),
	PSI_KEY(rtr_match_mutex),
	PSI_KEY(rtr_path_mutex),
	PSI_KEY(trx_sys_mutex),
};
# endif /* UNIV_PFS_MUTEX */

# ifdef UNIV_PFS_RWLOCK
mysql_pfs_key_t	dict_operation_lock_key;
mysql_pfs_key_t	index_tree_rw_lock_key;
mysql_pfs_key_t	index_online_log_key;
mysql_pfs_key_t	fil_space_latch_key;
mysql_pfs_key_t trx_i_s_cache_lock_key;
mysql_pfs_key_t	trx_purge_latch_key;
mysql_pfs_key_t trx_rseg_latch_key;
mysql_pfs_key_t lock_latch_key;
mysql_pfs_key_t	log_latch_key;

/* all_innodb_rwlocks array contains rwlocks that are
performance schema instrumented if "UNIV_PFS_RWLOCK"
is defined */
static PSI_rwlock_info all_innodb_rwlocks[] =
{
#  ifdef BTR_CUR_HASH_ADAPT
  { &btr_search_latch_key, "btr_search_latch", 0 },
#  endif
  { &dict_operation_lock_key, "dict_operation_lock", 0 },
  { &fil_space_latch_key, "fil_space_latch", 0 },
  { &trx_i_s_cache_lock_key, "trx_i_s_cache_lock", 0 },
  { &trx_purge_latch_key, "trx_purge_latch", 0 },
  { &trx_rseg_latch_key, "trx_rseg_latch", 0 },
  { &lock_latch_key, "lock_latch", 0 },
  { &log_latch_key, "log_latch", 0 },
  { &index_tree_rw_lock_key, "index_tree_rw_lock", PSI_RWLOCK_FLAG_SX }
};
# endif /* UNIV_PFS_RWLOCK */

# ifdef UNIV_PFS_THREAD
/* all_innodb_threads array contains threads that are
performance schema instrumented if "UNIV_PFS_THREAD"
is defined */
static PSI_thread_info	all_innodb_threads[] = {
  {&page_cleaner_thread_key, "page_cleaner", 0},
  {&trx_rollback_clean_thread_key, "trx_rollback", 0},
  {&page_encrypt_thread_key, "page_encrypt", 0},
  {&thread_pool_thread_key,"ib_tpool_worker", 0}
};
# endif /* UNIV_PFS_THREAD */

# ifdef UNIV_PFS_IO
/* all_innodb_files array contains the type of files that are
performance schema instrumented if "UNIV_PFS_IO" is defined */
static PSI_file_info	all_innodb_files[] = {
	PSI_KEY(innodb_data_file),
	PSI_KEY(innodb_temp_file)
};
# endif /* UNIV_PFS_IO */
#endif /* HAVE_PSI_INTERFACE */

static void innodb_remember_check_sysvar_funcs();
mysql_var_check_func check_sysvar_enum;
mysql_var_check_func check_sysvar_int;

// should page compression be used by default for new tables
static MYSQL_THDVAR_BOOL(compression_default, PLUGIN_VAR_OPCMDARG,
  "Is compression the default for new tables", 
  NULL, NULL, FALSE);

/** Update callback for SET [SESSION] innodb_default_encryption_key_id */
static void
innodb_default_encryption_key_id_update(THD* thd, st_mysql_sys_var* var,
					void* var_ptr, const void *save)
{
	uint key_id = *static_cast<const uint*>(save);
	if (key_id != FIL_DEFAULT_ENCRYPTION_KEY
	    && !encryption_key_id_exists(key_id)) {
		push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
				    ER_WRONG_ARGUMENTS,
				    "innodb_default_encryption_key=%u"
				    " is not available", key_id);
	}
	*static_cast<uint*>(var_ptr) = key_id;
}

static MYSQL_THDVAR_UINT(default_encryption_key_id, PLUGIN_VAR_RQCMDARG,
			 "Default encryption key id used for table encryption",
			 NULL, innodb_default_encryption_key_id_update,
			 FIL_DEFAULT_ENCRYPTION_KEY, 1, UINT_MAX32, 0);

/**
  Structure for CREATE TABLE options (table options).
  It needs to be called ha_table_option_struct.

  The option values can be specified in the CREATE TABLE at the end:
  CREATE TABLE ( ... ) *here*
*/

ha_create_table_option innodb_table_option_list[]=
{
  /* With this option user can enable page compression feature for the
  table */
  HA_TOPTION_SYSVAR("PAGE_COMPRESSED", page_compressed, compression_default),
  /* With this option user can set zip compression level for page
  compression for this table*/
  HA_TOPTION_NUMBER("PAGE_COMPRESSION_LEVEL", page_compression_level, 0, 1, 9, 1),
  /* With this option the user can enable encryption for the table */
  HA_TOPTION_ENUM("ENCRYPTED", encryption, "DEFAULT,YES,NO", 0),
  /* With this option the user defines the key identifier using for the encryption */
  HA_TOPTION_SYSVAR("ENCRYPTION_KEY_ID", encryption_key_id, default_encryption_key_id),

  HA_TOPTION_END
};

/*************************************************************//**
Check whether valid argument given to innodb_ft_*_stopword_table.
This function is registered as a callback with MySQL.
@return 0 for valid stopword table */
static
int
innodb_stopword_table_validate(
/*===========================*/
	THD*				thd,	/*!< in: thread handle */
	struct st_mysql_sys_var*	var,	/*!< in: pointer to system
						variable */
	void*				save,	/*!< out: immediate result
						for update function */
	struct st_mysql_value*		value);	/*!< in: incoming string */

static
void innodb_ft_cache_size_update(THD*, st_mysql_sys_var*, void*, const void* save)
{
  fts_max_cache_size= *static_cast<const size_t*>(save);
}

static
void innodb_ft_total_cache_size_update(THD*, st_mysql_sys_var*, void*, const void* save)
{
  fts_max_total_cache_size= *static_cast<const size_t*>(save);
}

static bool is_mysql_datadir_path(const char *path);

/** Validate passed-in "value" is a valid directory name.
This function is registered as a callback with MySQL.
@param[in,out]	thd	thread handle
@param[in]	var	pointer to system variable
@param[out]	save	immediate result for update
@param[in]	value	incoming string
@return 0 for valid name */
static
int
innodb_tmpdir_validate(
	THD*				thd,
	struct st_mysql_sys_var*,
	void*				save,
	struct st_mysql_value*		value)
{

	char*	alter_tmp_dir;
	char*	innodb_tmp_dir;
	char	buff[OS_FILE_MAX_PATH];
	int	len = sizeof(buff);
	char	tmp_abs_path[FN_REFLEN + 2];

	ut_ad(save != NULL);
	ut_ad(value != NULL);

	if (check_global_access(thd, FILE_ACL)) {
		push_warning_printf(
			thd, Sql_condition::WARN_LEVEL_WARN,
			ER_WRONG_ARGUMENTS,
			"InnoDB: FILE Permissions required");
		*static_cast<const char**>(save) = NULL;
		return(1);
	}

	alter_tmp_dir = (char*) value->val_str(value, buff, &len);

	if (!alter_tmp_dir) {
		*static_cast<const char**>(save) = alter_tmp_dir;
		return(0);
	}

	if (strlen(alter_tmp_dir) > FN_REFLEN) {
		push_warning_printf(
			thd, Sql_condition::WARN_LEVEL_WARN,
			ER_WRONG_ARGUMENTS,
			"Path length should not exceed %d bytes", FN_REFLEN);
		*static_cast<const char**>(save) = NULL;
		return(1);
	}

	my_realpath(tmp_abs_path, alter_tmp_dir, 0);
	size_t	tmp_abs_len = strlen(tmp_abs_path);

	if (my_access(tmp_abs_path, F_OK)) {

		push_warning_printf(
			thd, Sql_condition::WARN_LEVEL_WARN,
			ER_WRONG_ARGUMENTS,
			"InnoDB: Path doesn't exist.");
		*static_cast<const char**>(save) = NULL;
		return(1);
	} else if (my_access(tmp_abs_path, R_OK | W_OK)) {
		push_warning_printf(
			thd, Sql_condition::WARN_LEVEL_WARN,
			ER_WRONG_ARGUMENTS,
			"InnoDB: Server doesn't have permission in "
			"the given location.");
		*static_cast<const char**>(save) = NULL;
		return(1);
	}

	MY_STAT stat_info_dir;

	if (my_stat(tmp_abs_path, &stat_info_dir, MYF(0))) {
		if ((stat_info_dir.st_mode & S_IFDIR) != S_IFDIR) {

			push_warning_printf(
				thd, Sql_condition::WARN_LEVEL_WARN,
				ER_WRONG_ARGUMENTS,
				"Given path is not a directory. ");
			*static_cast<const char**>(save) = NULL;
			return(1);
		}
	}

	if (!is_mysql_datadir_path(tmp_abs_path)) {

		push_warning_printf(
			thd, Sql_condition::WARN_LEVEL_WARN,
			ER_WRONG_ARGUMENTS,
			"InnoDB: Path Location should not be same as "
			"mysql data directory location.");
		*static_cast<const char**>(save) = NULL;
		return(1);
	}

	innodb_tmp_dir = static_cast<char*>(
		thd_memdup(thd, tmp_abs_path, tmp_abs_len + 1));
	*static_cast<const char**>(save) = innodb_tmp_dir;
	return(0);
}

/******************************************************************//**
Maps a MySQL trx isolation level code to the InnoDB isolation level code
@return	InnoDB isolation level */
static inline
uint
innobase_map_isolation_level(
/*=========================*/
	enum_tx_isolation	iso);	/*!< in: MySQL isolation level code */

/** Gets field offset for a field in a table.
@param[in]	table	MySQL table object
@param[in]	field	MySQL field object (from table->field array)
@return offset */
static inline
uint
get_field_offset(
	const TABLE*	table,
	const Field*	field)
{
	return field->offset(table->record[0]);
}


/*************************************************************//**
Check for a valid value of innobase_compression_algorithm.
@return	0 for valid innodb_compression_algorithm. */
static
int
innodb_compression_algorithm_validate(
/*==================================*/
	THD*				thd,	/*!< in: thread handle */
	struct st_mysql_sys_var*	var,	/*!< in: pointer to system
						variable */
	void*				save,	/*!< out: immediate result
						for update function */
	struct st_mysql_value*		value);	/*!< in: incoming string */

static ibool innodb_have_punch_hole=IF_PUNCH_HOLE(1, 0);

static
int
innodb_encrypt_tables_validate(
/*==================================*/
	THD*				thd,	/*!< in: thread handle */
	struct st_mysql_sys_var*	var,	/*!< in: pointer to system
						variable */
	void*				save,	/*!< out: immediate result
						for update function */
	struct st_mysql_value*		value);	/*!< in: incoming string */

static const char innobase_hton_name[]= "InnoDB";

static MYSQL_THDVAR_BOOL(table_locks, PLUGIN_VAR_OPCMDARG,
  "Enable InnoDB locking in LOCK TABLES",
  /* check_func */ NULL, /* update_func */ NULL,
  /* default */ TRUE);

static MYSQL_THDVAR_BOOL(snapshot_isolation, PLUGIN_VAR_OPCMDARG,
  "Use snapshot isolation (write-write conflict detection)",
  NULL, NULL, TRUE);

static MYSQL_THDVAR_BOOL(strict_mode, PLUGIN_VAR_OPCMDARG,
  "Use strict mode when evaluating create options",
  NULL, NULL, TRUE);

static MYSQL_THDVAR_BOOL(ft_enable_stopword, PLUGIN_VAR_OPCMDARG,
  "Create FTS index with stopword",
  NULL, NULL,
  /* default */ TRUE);

static MYSQL_THDVAR_UINT(lock_wait_timeout, PLUGIN_VAR_RQCMDARG,
  "Timeout in seconds an InnoDB transaction may wait for a lock before being rolled back. The value 100000000 is infinite timeout",
  NULL, NULL, 50, 0, 100000000, 0);

static MYSQL_THDVAR_STR(ft_user_stopword_table,
  PLUGIN_VAR_OPCMDARG|PLUGIN_VAR_MEMALLOC,
  "User supplied stopword table name, effective in the session level",
  innodb_stopword_table_validate, NULL, NULL);

static MYSQL_THDVAR_STR(tmpdir,
  PLUGIN_VAR_OPCMDARG|PLUGIN_VAR_MEMALLOC,
  "Directory for temporary non-tablespace files",
  innodb_tmpdir_validate, NULL, NULL);

static size_t truncated_status_writes;

static SHOW_VAR innodb_status_variables[]= {
#ifdef BTR_CUR_HASH_ADAPT
  {"adaptive_hash_hash_searches", &export_vars.innodb_ahi_hit, SHOW_SIZE_T},
  {"adaptive_hash_non_hash_searches",
  &export_vars.innodb_ahi_miss, SHOW_SIZE_T},
#endif
  {"async_reads_pending",
  &export_vars.async_read_stats.pending_ops, SHOW_SIZE_T},
  {"async_reads_tasks_running",
  &export_vars.async_read_stats.completion_stats.tasks_running, SHOW_SIZE_T},
  {"async_reads_total_count",
  &export_vars.async_read_stats.completion_stats.total_tasks_executed,SHOW_ULONGLONG},
  {"async_reads_total_enqueues",
  &export_vars.async_read_stats.completion_stats.total_tasks_enqueued,SHOW_ULONGLONG},
  {"async_reads_queue_size",
  &export_vars.async_read_stats.completion_stats.queue_size, SHOW_SIZE_T},
  {"async_reads_wait_slot_sec",
  &export_vars.async_read_stats.slot_wait_time_sec, SHOW_DOUBLE},

  {"async_writes_pending",
  &export_vars.async_write_stats.pending_ops,SHOW_SIZE_T},
  {"async_writes_tasks_running",
  &export_vars.async_write_stats.completion_stats.tasks_running, SHOW_SIZE_T},
  {"async_writes_total_count",
  &export_vars.async_write_stats.completion_stats.total_tasks_executed, SHOW_ULONGLONG},
  {"async_writes_total_enqueues",
  &export_vars.async_write_stats.completion_stats.total_tasks_enqueued, SHOW_ULONGLONG},
  {"async_writes_queue_size",
  &export_vars.async_write_stats.completion_stats.queue_size, SHOW_SIZE_T},
  {"async_writes_wait_slot_sec",
   &export_vars.async_write_stats.slot_wait_time_sec, SHOW_DOUBLE},

  {"background_log_sync", &srv_log_writes_and_flush, SHOW_SIZE_T},
  {"buffer_pool_dump_status",
  (char*) &export_vars.innodb_buffer_pool_dump_status,	  SHOW_CHAR},
  {"buffer_pool_load_status",
  (char*) &export_vars.innodb_buffer_pool_load_status,	  SHOW_CHAR},
  {"buffer_pool_resize_status",
  (char*) &export_vars.innodb_buffer_pool_resize_status,  SHOW_CHAR},
  {"buffer_pool_load_incomplete",
  &export_vars.innodb_buffer_pool_load_incomplete,        SHOW_BOOL},
  {"buffer_pool_pages_data", &UT_LIST_GET_LEN(buf_pool.LRU), SHOW_SIZE_T},
  {"buffer_pool_bytes_data",
   &export_vars.innodb_buffer_pool_bytes_data, SHOW_SIZE_T},
  {"buffer_pool_pages_dirty",
   &UT_LIST_GET_LEN(buf_pool.flush_list), SHOW_SIZE_T},
  {"buffer_pool_bytes_dirty", &buf_pool.flush_list_bytes, SHOW_SIZE_T},
  {"buffer_pool_pages_flushed", &buf_pool.stat.n_pages_written, SHOW_SIZE_T},
  {"buffer_pool_pages_free", &UT_LIST_GET_LEN(buf_pool.free), SHOW_SIZE_T},
#ifdef UNIV_DEBUG
  {"buffer_pool_pages_latched",
   &export_vars.innodb_buffer_pool_pages_latched, SHOW_SIZE_T},
#endif /* UNIV_DEBUG */
  {"buffer_pool_pages_made_not_young",
   &buf_pool.stat.n_pages_not_made_young, SHOW_SIZE_T},
  {"buffer_pool_pages_made_young",
   &buf_pool.stat.n_pages_made_young, SHOW_SIZE_T},
  {"buffer_pool_pages_misc",
   &export_vars.innodb_buffer_pool_pages_misc, SHOW_SIZE_T},
  {"buffer_pool_pages_old", &buf_pool.LRU_old_len, SHOW_SIZE_T},
  {"buffer_pool_pages_total",
   &export_vars.innodb_buffer_pool_pages_total, SHOW_SIZE_T},
  {"buffer_pool_pages_LRU_flushed", &buf_lru_flush_page_count, SHOW_SIZE_T},
  {"buffer_pool_pages_LRU_freed", &buf_lru_freed_page_count, SHOW_SIZE_T},
  {"buffer_pool_pages_split", &buf_pool.pages_split, SHOW_SIZE_T},
  {"buffer_pool_read_ahead_rnd",
   &buf_pool.stat.n_ra_pages_read_rnd, SHOW_SIZE_T},
  {"buffer_pool_read_ahead", &buf_pool.stat.n_ra_pages_read, SHOW_SIZE_T},
  {"buffer_pool_read_ahead_evicted",
   &buf_pool.stat.n_ra_pages_evicted, SHOW_SIZE_T},
  {"buffer_pool_read_requests",
   &export_vars.innodb_buffer_pool_read_requests, SHOW_SIZE_T},
  {"buffer_pool_reads", &buf_pool.stat.n_pages_read, SHOW_SIZE_T},
  {"buffer_pool_wait_free", &buf_pool.stat.LRU_waits, SHOW_SIZE_T},
  {"buffer_pool_write_requests", &buf_pool.flush_list_requests, SHOW_SIZE_T},
  {"checkpoint_age", &export_vars.innodb_checkpoint_age, SHOW_SIZE_T},
  {"checkpoint_max_age", &export_vars.innodb_checkpoint_max_age, SHOW_SIZE_T},
  {"data_fsyncs", (size_t*) &os_n_fsyncs, SHOW_SIZE_T},
  {"data_pending_fsyncs",
   (size_t*) &fil_n_pending_tablespace_flushes, SHOW_SIZE_T},
  {"data_pending_reads", &export_vars.innodb_data_pending_reads, SHOW_SIZE_T},
  {"data_pending_writes", &export_vars.innodb_data_pending_writes,SHOW_SIZE_T},
  {"data_read", &export_vars.innodb_data_read, SHOW_SIZE_T},
  {"data_reads", &export_vars.innodb_data_reads, SHOW_SIZE_T},
  {"data_writes", &export_vars.innodb_data_writes, SHOW_SIZE_T},
  {"data_written", &export_vars.innodb_data_written, SHOW_SIZE_T},
  {"dblwr_pages_written", &export_vars.innodb_dblwr_pages_written,SHOW_SIZE_T},
  {"dblwr_writes", &export_vars.innodb_dblwr_writes, SHOW_SIZE_T},
  {"deadlocks", &lock_sys.deadlocks, SHOW_SIZE_T},
  {"history_list_length", &export_vars.innodb_history_list_length,SHOW_SIZE_T},
  {"log_waits", &log_sys.waits, SHOW_SIZE_T},
  {"log_write_requests", &log_sys.write_to_buf, SHOW_SIZE_T},
  {"log_writes", &log_sys.write_to_log, SHOW_SIZE_T},
  {"lsn_current", &export_vars.innodb_lsn_current, SHOW_ULONGLONG},
  {"lsn_flushed", &export_vars.innodb_lsn_flushed, SHOW_ULONGLONG},
  {"lsn_last_checkpoint", &export_vars.innodb_lsn_last_checkpoint,
   SHOW_ULONGLONG},
  {"master_thread_active_loops", &srv_main_active_loops, SHOW_SIZE_T},
  {"master_thread_idle_loops", &srv_main_idle_loops, SHOW_SIZE_T},
  {"max_trx_id", &export_vars.innodb_max_trx_id, SHOW_ULONGLONG},
#ifdef BTR_CUR_HASH_ADAPT
  {"mem_adaptive_hash", &export_vars.innodb_mem_adaptive_hash, SHOW_SIZE_T},
#endif
  {"mem_dictionary", &export_vars.innodb_mem_dictionary, SHOW_SIZE_T},
  {"os_log_written", &export_vars.innodb_os_log_written, SHOW_SIZE_T},
  {"page_size", &srv_page_size, SHOW_ULONG},
  {"pages_created", &buf_pool.stat.n_pages_created, SHOW_SIZE_T},
  {"pages_read", &buf_pool.stat.n_pages_read, SHOW_SIZE_T},
  {"pages_written", &buf_pool.stat.n_pages_written, SHOW_SIZE_T},
  {"row_lock_current_waits", &export_vars.innodb_row_lock_current_waits,
   SHOW_SIZE_T},
  {"row_lock_time", &export_vars.innodb_row_lock_time, SHOW_LONGLONG},
  {"row_lock_time_avg", &export_vars.innodb_row_lock_time_avg, SHOW_ULONGLONG},
  {"row_lock_time_max", &export_vars.innodb_row_lock_time_max, SHOW_ULONGLONG},
  {"row_lock_waits", &export_vars.innodb_row_lock_waits, SHOW_SIZE_T},
  {"num_open_files", &fil_system.n_open, SHOW_SIZE_T},
  {"truncated_status_writes", &truncated_status_writes, SHOW_SIZE_T},
  {"available_undo_logs", &srv_available_undo_logs, SHOW_ULONG},
  {"undo_truncations", &export_vars.innodb_undo_truncations, SHOW_ULONG},

  /* Status variables for page compression */
  {"page_compression_saved",
   &export_vars.innodb_page_compression_saved, SHOW_LONGLONG},
  {"num_pages_page_compressed",
   &export_vars.innodb_pages_page_compressed, SHOW_LONGLONG},
  {"num_page_compressed_trim_op",
   &export_vars.innodb_page_compressed_trim_op, SHOW_LONGLONG},
  {"num_pages_page_decompressed",
   &export_vars.innodb_pages_page_decompressed, SHOW_LONGLONG},
  {"num_pages_page_compression_error",
   &export_vars.innodb_pages_page_compression_error, SHOW_LONGLONG},
  {"num_pages_encrypted",
   &export_vars.innodb_pages_encrypted, SHOW_LONGLONG},
  {"num_pages_decrypted",
   &export_vars.innodb_pages_decrypted, SHOW_LONGLONG},
  {"have_lz4",        &(provider_service_lz4->is_loaded),    SHOW_BOOL},
  {"have_lzo",        &(provider_service_lzo->is_loaded),    SHOW_BOOL},
  {"have_lzma",       &(provider_service_lzma->is_loaded),   SHOW_BOOL},
  {"have_bzip2",      &(provider_service_bzip2->is_loaded),  SHOW_BOOL},
  {"have_snappy",     &(provider_service_snappy->is_loaded), SHOW_BOOL},
  {"have_punch_hole", &innodb_have_punch_hole, SHOW_BOOL},

  {"instant_alter_column",
   &export_vars.innodb_instant_alter_column, SHOW_SIZE_T},

  /* Online alter table status variables */
  {"onlineddl_rowlog_rows",
   &export_vars.innodb_onlineddl_rowlog_rows, SHOW_SIZE_T},
  {"onlineddl_rowlog_pct_used",
   &export_vars.innodb_onlineddl_rowlog_pct_used, SHOW_SIZE_T},
  {"onlineddl_pct_progress",
   &export_vars.innodb_onlineddl_pct_progress, SHOW_SIZE_T},

  /* Encryption */
  {"encryption_rotation_pages_read_from_cache",
   &export_vars.innodb_encryption_rotation_pages_read_from_cache, SHOW_SIZE_T},
  {"encryption_rotation_pages_read_from_disk",
   &export_vars.innodb_encryption_rotation_pages_read_from_disk, SHOW_SIZE_T},
  {"encryption_rotation_pages_modified",
   &export_vars.innodb_encryption_rotation_pages_modified, SHOW_SIZE_T},
  {"encryption_rotation_pages_flushed",
   &export_vars.innodb_encryption_rotation_pages_flushed, SHOW_SIZE_T},
  {"encryption_rotation_estimated_iops",
   &export_vars.innodb_encryption_rotation_estimated_iops, SHOW_SIZE_T},
  {"encryption_n_merge_blocks_encrypted",
   &export_vars.innodb_n_merge_blocks_encrypted, SHOW_LONGLONG},
  {"encryption_n_merge_blocks_decrypted",
   &export_vars.innodb_n_merge_blocks_decrypted, SHOW_LONGLONG},
  {"encryption_n_rowlog_blocks_encrypted",
   &export_vars.innodb_n_rowlog_blocks_encrypted, SHOW_LONGLONG},
  {"encryption_n_rowlog_blocks_decrypted",
   &export_vars.innodb_n_rowlog_blocks_decrypted, SHOW_LONGLONG},
  {"encryption_n_temp_blocks_encrypted",
   &export_vars.innodb_n_temp_blocks_encrypted, SHOW_LONGLONG},
  {"encryption_n_temp_blocks_decrypted",
   &export_vars.innodb_n_temp_blocks_decrypted, SHOW_LONGLONG},
  {"encryption_num_key_requests", &export_vars.innodb_encryption_key_requests,
   SHOW_LONGLONG},

  /* InnoDB bulk operations */
  {"bulk_operations", &export_vars.innodb_bulk_operations, SHOW_SIZE_T},

  {NullS, NullS, SHOW_LONG}
};

/** Cancel any pending lock request associated with the current THD.
@sa THD::awake() @sa ha_kill_query() */
static void innobase_kill_query(handlerton*, THD* thd, enum thd_kill_levels);
static void innobase_commit_ordered(THD* thd, bool all);

/*****************************************************************//**
Commits a transaction in an InnoDB database or marks an SQL statement
ended.
@return 0 */
static
int
innobase_commit(
/*============*/
	THD*		thd,		/*!< in: MySQL thread handle of the
					user for whom the transaction should
					be committed */
	bool		commit_trx);	/*!< in: true - commit transaction
					false - the current SQL statement
					ended */

/*****************************************************************//**
Rolls back a transaction to a savepoint.
@return 0 if success, HA_ERR_NO_SAVEPOINT if no savepoint with the
given name */
static
int
innobase_rollback(
/*==============*/
	THD*		thd,		/*!< in: handle to the MySQL thread
					of the user whose transaction should
					be rolled back */
	bool		rollback_trx);	/*!< in: TRUE - rollback entire
					transaction FALSE - rollback the current
					statement only */

/*****************************************************************//**
Rolls back a transaction to a savepoint.
@return 0 if success, HA_ERR_NO_SAVEPOINT if no savepoint with the
given name */
static
int
innobase_rollback_to_savepoint(
/*===========================*/
	THD*		thd,		/*!< in: handle to the MySQL thread of
					the user whose XA transaction should
					be rolled back to savepoint */
	void*		savepoint);	/*!< in: savepoint data */

/*****************************************************************//**
Check whether innodb state allows to safely release MDL locks after
rollback to savepoint.
@return true if it is safe, false if its not safe. */
static
bool
innobase_rollback_to_savepoint_can_release_mdl(
/*===========================================*/
	THD*		thd);		/*!< in: handle to the MySQL thread of
					the user whose XA transaction should
					be rolled back to savepoint */

/** Request notification of log writes */
static void innodb_log_flush_request(void *cookie) noexcept;

/** Requests for log flushes */
struct log_flush_request
{
  /** earlier request (for a smaller LSN) */
  log_flush_request *next;
  /** parameter provided to innodb_log_flush_request() */
  void *cookie;
  /** log sequence number that is being waited for */
  lsn_t lsn;
};

/** Buffer of pending innodb_log_flush_request() */
alignas(CPU_LEVEL1_DCACHE_LINESIZE) static
struct
{
  /** first request */
  std::atomic<log_flush_request*> start;
  /** last request */
  log_flush_request *end;
  /** mutex protecting this object */
  mysql_mutex_t mutex;
}
log_requests;

/** Adjust some InnoDB startup parameters based on the data directory */
static void innodb_params_adjust();

/*******************************************************************//**
This function is used to prepare an X/Open XA distributed transaction.
@return 0 or error number */
static
int
innobase_xa_prepare(
/*================*/
	THD*		thd,		/*!< in: handle to the MySQL thread of
					the user whose XA transaction should
					be prepared */
	bool		all);		/*!< in: true - prepare transaction
					false - the current SQL statement
					ended */
/*******************************************************************//**
This function is used to recover X/Open XA distributed transactions.
@return number of prepared transactions stored in xid_list */
static
int
innobase_xa_recover(
/*================*/
	XID*		xid_list,	/*!< in/out: prepared transactions */
	uint		len);		/*!< in: number of slots in xid_list */
/*******************************************************************//**
This function is used to commit one X/Open XA distributed transaction
which is in the prepared state
@return 0 or error number */
static
int
innobase_commit_by_xid(
/*===================*/
	XID*		xid);		/*!< in: X/Open XA transaction
					identification */
#ifndef EMBEDDED_LIBRARY
/*******************************************************************//**
This function is used to rollback one X/Open XA distributed transaction
which is in the prepared state asynchronously.

It only set the transaction's status to ACTIVE and persist the status.
The transaction will be rolled back by background rollback thread.

@return 0 or error number
*/
static
int
innobase_recover_rollback_by_xid(
/*===================*/
	const XID*	xid);		/*!< in: X/Open XA transaction
					identification */
/*******************************************************************//**
  This function is called after tc log is opened(typically binlog recovery)
  has done. It starts rollback thread to rollback the transactions
  have been changed from PREPARED to ACTIVE.

  @return 0 or error number
*/
static
void
innobase_tc_log_recovery_done();
#endif


/** Ignore FOREIGN KEY constraints that would be violated by DROP DATABASE */
static ibool innodb_drop_database_ignore_fk(void*,void*) { return false; }

/** FOREIGN KEY error reporting context for DROP DATABASE */
struct innodb_drop_database_fk_report
{
  /** database name, with trailing '/' */
  const span<const char> name;
  /** whether errors were found */
  bool violated;
};

/** Report FOREIGN KEY constraints that would be violated by DROP DATABASE
@return whether processing should continue */
static ibool innodb_drop_database_fk(void *node, void *report)
{
  auto s= static_cast<sel_node_t*>(node);
  auto r= static_cast<innodb_drop_database_fk_report*>(report);
  const dfield_t *name= que_node_get_val(s->select_list);
  ut_ad(name->type.mtype == DATA_VARCHAR);

  if (name->len == UNIV_SQL_NULL || name->len <= r->name.size() ||
      memcmp(static_cast<const char*>(name->data), r->name.data(),
             r->name.size()))
    return false; /* End of matches */

  node= que_node_get_next(s->select_list);
  const dfield_t *id= que_node_get_val(node);
  ut_ad(id->type.mtype == DATA_VARCHAR);
  ut_ad(!que_node_get_next(node));

  if (id->len != UNIV_SQL_NULL)
    sql_print_error("DROP DATABASE: table %.*s is referenced"
                    " by FOREIGN KEY %.*s",
                    static_cast<int>(name->len),
                    static_cast<const char*>(name->data),
                    static_cast<int>(id->len),
                    static_cast<const char*>(id->data));
  else
    ut_ad("corrupted SYS_FOREIGN record" == 0);

  return true;
}

/** After DROP DATABASE executed ha_innobase::delete_table() on all
tables that it was aware of, drop any leftover tables inside InnoDB.
@param path  database path */
static void innodb_drop_database(handlerton*, char *path)
{
  if (high_level_read_only)
    return;

  ulint len= 0;
  char *ptr;

  for (ptr= strend(path) - 2; ptr >= path &&
#ifdef _WIN32
       *ptr != '\\' &&
#endif
       *ptr != '/'; ptr--)
    len++;

  ptr++;
  size_t casedn_nbytes= len * system_charset_info->casedn_multiply();
  char *namebuf= static_cast<char*>
    (my_malloc(PSI_INSTRUMENT_ME, casedn_nbytes + 2, MYF(0)));
  if (!namebuf)
    return;
#ifndef _WIN32
  memcpy(namebuf, ptr, len);
#else /*_WIN32*/
  len= system_charset_info->casedn(ptr, len, namebuf, casedn_nbytes);
#endif /* _WIN32 */
  namebuf[len] = '/';
  namebuf[len + 1] = '\0';

  THD * const thd= current_thd;
  trx_t *trx= innobase_trx_allocate(thd);
  dberr_t err= DB_SUCCESS;

  dict_sys.lock(SRW_LOCK_CALL);

  for (auto i= dict_sys.table_id_hash.n_cells; i--; )
  {
    for (dict_table_t *next, *table= static_cast<dict_table_t*>
         (dict_sys.table_id_hash.array[i].node); table; table= next)
    {
      ut_ad(table->cached);
      next= table->id_hash;
      if (strncmp(table->name.m_name, namebuf, len + 1))
        continue;
      const auto n_handles= table->get_ref_count();
      const bool locks= !n_handles && lock_table_has_locks(table);
      if (n_handles || locks)
      {
        err= DB_ERROR;
        ib::error errmsg;
        errmsg << "DROP DATABASE: cannot DROP TABLE " << table->name;
        if (n_handles)
          errmsg << " due to " << n_handles << " open handles";
        else
          errmsg << " due to locks";
        continue;
      }
      dict_sys.remove(table);
    }
  }

  dict_sys.unlock();

  dict_stats stats;
  const bool stats_failed{stats.open(thd)};
  trx_start_for_ddl(trx);

  uint errors= 0;
  char db[NAME_LEN + 1];
  strconvert(&my_charset_filename, namebuf, len, system_charset_info, db,
             sizeof db, &errors);
  if (!errors && !stats_failed &&
      lock_table_for_trx(stats.table(), trx, LOCK_X) == DB_SUCCESS &&
      lock_table_for_trx(stats.index(), trx, LOCK_X) == DB_SUCCESS)
  {
    row_mysql_lock_data_dictionary(trx);
    if (dict_stats_delete(db, trx))
    {
      /* Ignore this error. Leaving garbage statistics behind is a
      lesser evil. Carry on to try to remove any garbage tables. */
      trx->rollback();
      trx_start_for_ddl(trx);
    }
    row_mysql_unlock_data_dictionary(trx);
  }

  if (err == DB_SUCCESS)
    err= lock_sys_tables(trx);
  row_mysql_lock_data_dictionary(trx);

  static const char drop_database[] =
    "PROCEDURE DROP_DATABASE_PROC () IS\n"
    "fk CHAR;\n"
    "name CHAR;\n"
    "tid CHAR;\n"
    "iid CHAR;\n"

    "DECLARE FUNCTION fk_report;\n"

    "DECLARE CURSOR fkf IS\n"
    "SELECT ID FROM SYS_FOREIGN WHERE ID >= :db FOR UPDATE;\n"

    "DECLARE CURSOR fkr IS\n"
    "SELECT REF_NAME,ID FROM SYS_FOREIGN WHERE REF_NAME >= :db FOR UPDATE\n"
    "ORDER BY REF_NAME;\n"

    "DECLARE CURSOR tab IS\n"
    "SELECT ID,NAME FROM SYS_TABLES WHERE NAME >= :db FOR UPDATE;\n"

    "DECLARE CURSOR idx IS\n"
    "SELECT ID FROM SYS_INDEXES WHERE TABLE_ID = tid FOR UPDATE;\n"

    "BEGIN\n"

    "OPEN fkf;\n"
    "WHILE 1 = 1 LOOP\n"
    "  FETCH fkf INTO fk;\n"
    "  IF (SQL % NOTFOUND) THEN EXIT; END IF;\n"
    "  IF TO_BINARY(SUBSTR(fk, 0, LENGTH(:db)))<>TO_BINARY(:db)"
    " THEN EXIT; END IF;\n"
    "  DELETE FROM SYS_FOREIGN_COLS WHERE TO_BINARY(ID)=TO_BINARY(fk);\n"
    "  DELETE FROM SYS_FOREIGN WHERE CURRENT OF fkf;\n"
    "END LOOP;\n"
    "CLOSE fkf;\n"

    "OPEN fkr;\n"
    "FETCH fkr INTO fk_report();\n"
    "CLOSE fkr;\n"

    "OPEN tab;\n"
    "WHILE 1 = 1 LOOP\n"
    "  FETCH tab INTO tid,name;\n"
    "  IF (SQL % NOTFOUND) THEN EXIT; END IF;\n"
    "  IF TO_BINARY(SUBSTR(name, 0, LENGTH(:db))) <> TO_BINARY(:db)"
    " THEN EXIT; END IF;\n"
    "  DELETE FROM SYS_COLUMNS WHERE TABLE_ID=tid;\n"
    "  DELETE FROM SYS_TABLES WHERE ID=tid;\n"
    "  OPEN idx;\n"
    "  WHILE 1 = 1 LOOP\n"
    "    FETCH idx INTO iid;\n"
    "    IF (SQL % NOTFOUND) THEN EXIT; END IF;\n"
    "    DELETE FROM SYS_FIELDS WHERE INDEX_ID=iid;\n"
    "    DELETE FROM SYS_INDEXES WHERE CURRENT OF idx;\n"
    "  END LOOP;\n"
    "  CLOSE idx;\n"
    "END LOOP;\n"
    "CLOSE tab;\n"

    "END;\n";

  innodb_drop_database_fk_report report{{namebuf, len + 1}, false};

  if (err == DB_SUCCESS)
  {
    pars_info_t* pinfo = pars_info_create();
    pars_info_bind_function(pinfo, "fk_report", trx->check_foreigns
                            ? innodb_drop_database_fk
                            : innodb_drop_database_ignore_fk, &report);
    pars_info_add_str_literal(pinfo, "db", namebuf);
    err= que_eval_sql(pinfo, drop_database, trx);
    if (err == DB_SUCCESS && report.violated)
      err= DB_CANNOT_DROP_CONSTRAINT;
  }

  const trx_id_t trx_id= trx->id;

  if (err != DB_SUCCESS)
  {
    trx->rollback();
    sql_print_error("InnoDB: DROP DATABASE %.*s: %s",
                    int(len), namebuf, ut_strerr(err));
  }
  else
    trx->commit();

  row_mysql_unlock_data_dictionary(trx);
  trx->free();
  if (!stats_failed)
    stats.close();

  if (err == DB_SUCCESS)
  {
    /* Eventually after the DELETE FROM SYS_INDEXES was committed,
    purge would invoke dict_drop_index_tree() to delete the associated
    tablespaces. Because the SQL layer expects the directory to be empty,
    we will "manually" purge the tablespaces that belong to the
    records that we delete-marked. */

    dfield_t dfield;
    dtuple_t tuple{
      0,1,1,0,&dfield,nullptr
#ifdef UNIV_DEBUG
      , DATA_TUPLE_MAGIC_N
#endif
    };
    dict_index_t* sys_index= UT_LIST_GET_FIRST(dict_sys.sys_tables->indexes);
    btr_pcur_t pcur;
    namebuf[len++]= '/';
    dfield_set_data(&dfield, namebuf, len);
    dict_index_copy_types(&tuple, sys_index, 1);
    std::vector<pfs_os_file_t> to_close;
    mtr_t mtr;
    mtr.start();
    pcur.btr_cur.page_cur.index = sys_index;
    err= btr_pcur_open_on_user_rec(&tuple, BTR_SEARCH_LEAF, &pcur, &mtr);
    if (err != DB_SUCCESS)
      goto err_exit;

    for (; btr_pcur_is_on_user_rec(&pcur);
         btr_pcur_move_to_next_user_rec(&pcur, &mtr))
    {
      const rec_t *rec= btr_pcur_get_rec(&pcur);
      if (rec_get_n_fields_old(rec) != DICT_NUM_FIELDS__SYS_TABLES)
      {
        ut_ad("corrupted SYS_TABLES record" == 0);
        break;
      }
      if (!rec_get_deleted_flag(rec, false))
        continue;
      ulint flen;
      static_assert(DICT_FLD__SYS_TABLES__NAME == 0, "compatibility");
      rec_get_nth_field_offs_old(rec, 0, &flen);
      if (flen == UNIV_SQL_NULL || flen <= len || memcmp(rec, namebuf, len))
        /* We ran out of tables that had existed in the database. */
        break;
      const byte *db_trx_id=
        rec_get_nth_field_old(rec, DICT_FLD__SYS_TABLES__DB_TRX_ID, &flen);
      if (flen != 6)
      {
        ut_ad("corrupted SYS_TABLES.SPACE" == 0);
        break;
      }
      if (mach_read_from_6(db_trx_id) != trx_id)
        /* This entry was modified by some other transaction than us.
        Unfortunately, because SYS_TABLES.NAME is the PRIMARY KEY,
        we cannot distinguish RENAME and DROP here. It is possible
        that the table had been renamed to some other database. */
        continue;
      const byte *s=
        rec_get_nth_field_old(rec, DICT_FLD__SYS_TABLES__SPACE, &flen);
      if (flen != 4)
        ut_ad("corrupted SYS_TABLES.SPACE" == 0);
      else if (uint32_t space_id= mach_read_from_4(s))
      {
        pfs_os_file_t detached= fil_delete_tablespace(space_id);
        if (detached != OS_FILE_CLOSED)
          to_close.emplace_back(detached);
      }
    }
  err_exit:
    mtr.commit();
    for (pfs_os_file_t detached : to_close)
      os_file_close(detached);

    /* Any changes must be persisted before we return. */
    if (mtr.commit_lsn())
      log_write_up_to(mtr.commit_lsn(), true);
  }

  my_free(namebuf);
}

/** Shut down the InnoDB storage engine.
@return	0 */
static
int
innobase_end(handlerton*, ha_panic_function);

/*****************************************************************//**
Creates an InnoDB transaction struct for the thd if it does not yet have one.
Starts a new InnoDB transaction if a transaction is not yet started. And
assigns a new snapshot for a consistent read if the transaction does not yet
have one.
@return 0 */
static
int
innobase_start_trx_and_assign_read_view(
/*====================================*/
	THD*		thd);		/* in: MySQL thread handle of the
					user for whom the transaction should
					be committed */

/** Flush InnoDB redo logs to the file system.
@return false */
static bool innobase_flush_logs(handlerton*)
{
  if (!srv_read_only_mode)
    /* Write and flush any outstanding redo log. */
    log_buffer_flush_to_disk(true);
  return false;
}

/************************************************************************//**
Implements the SHOW ENGINE INNODB STATUS command. Sends the output of the
InnoDB Monitor to the client.
@return 0 on success */
static
int
innodb_show_status(
/*===============*/
	handlerton*	hton,		/*!< in: the innodb handlerton */
	THD*		thd,		/*!< in: the MySQL query thread of
					the caller */
	stat_print_fn*	stat_print);
/************************************************************************//**
Return 0 on success and non-zero on failure. Note: the bool return type
seems to be abused here, should be an int. */
static
bool
innobase_show_status(
/*=================*/
	handlerton*		hton,	/*!< in: the innodb handlerton */
	THD*			thd,	/*!< in: the MySQL query thread of
					the caller */
	stat_print_fn*		stat_print,
	enum ha_stat_type	stat_type);

/** After ALTER TABLE, recompute statistics. */
inline void ha_innobase::reload_statistics()
{
  if (dict_table_t *table= m_prebuilt ? m_prebuilt->table : nullptr)
  {
    if (table->is_readable())
      statistics_init(table, true);
    else
      table->stat.fetch_or(dict_table_t::STATS_INITIALIZED);
  }
}

/** After ALTER TABLE, recompute statistics. */
static int innodb_notify_tabledef_changed(handlerton *,
                                          LEX_CSTRING *, LEX_CSTRING *,
                                          LEX_CUSTRING *, LEX_CUSTRING *,
                                          handler *handler)
{
  DBUG_ENTER("innodb_notify_tabledef_changed");
  if (handler)
    static_cast<ha_innobase*>(handler)->reload_statistics();
  DBUG_RETURN(0);
}

/****************************************************************//**
Parse and enable InnoDB monitor counters during server startup.
User can enable monitor counters/groups by specifying
"loose-innodb_monitor_enable = monitor_name1;monitor_name2..."
in server configuration file or at the command line. */
static
void
innodb_enable_monitor_at_startup(
/*=============================*/
	char*	str);	/*!< in: monitor counter enable list */

#ifdef MYSQL_STORE_FTS_DOC_ID
/** Store doc_id value into FTS_DOC_ID field
@param[in,out]	tbl	table containing FULLTEXT index
@param[in]	doc_id	FTS_DOC_ID value */
static
void
innobase_fts_store_docid(
	TABLE*		tbl,
	ulonglong	doc_id)
{
	my_bitmap_map*	old_map
		= dbug_tmp_use_all_columns(tbl, tbl->write_set);

	tbl->fts_doc_id_field->store(static_cast<longlong>(doc_id), true);

	dbug_tmp_restore_column_map(tbl->write_set, old_map);
}
#endif

/*******************************************************************//**
Function for constructing an InnoDB table handler instance. */
static
handler*
innobase_create_handler(
/*====================*/
	handlerton*	hton,	/*!< in: InnoDB handlerton */
	TABLE_SHARE*	table,
	MEM_ROOT*	mem_root)
{
	return(new (mem_root) ha_innobase(hton, table));
}

/* General functions */

/** Check that a page_size is correct for InnoDB.
If correct, set the associated page_size_shift which is the power of 2
for this page size.
@param[in]	page_size	Page Size to evaluate
@return an associated page_size_shift if valid, 0 if invalid. */
inline uint32_t innodb_page_size_validate(ulong page_size)
{
	DBUG_ENTER("innodb_page_size_validate");

	for (uint32_t n = UNIV_PAGE_SIZE_SHIFT_MIN;
	     n <= UNIV_PAGE_SIZE_SHIFT_MAX;
	     n++) {
		if (page_size == static_cast<ulong>(1 << n)) {
			DBUG_RETURN(n);
		}
	}

	DBUG_RETURN(0);
}

/******************************************************************//**
Returns true if transaction should be flagged as read-only.
@return true if the thd is marked as read-only */
bool
thd_trx_is_read_only(
/*=================*/
	THD*	thd)	/*!< in: thread handle */
{
	return(thd != 0 && thd_tx_is_read_only(thd));
}

static MYSQL_THDVAR_BOOL(background_thread,
			 PLUGIN_VAR_NOCMDOPT | PLUGIN_VAR_NOSYSVAR,
			 "Internal (not user visible) flag to mark "
			 "background purge threads", NULL, NULL, 0);

/** Create a MYSQL_THD for a background thread and mark it as such.
@param name thread info for SHOW PROCESSLIST
@return new MYSQL_THD */
MYSQL_THD innobase_create_background_thd(const char* name)
{
	MYSQL_THD thd= create_background_thd();
	thd_proc_info(thd, name);
	THDVAR(thd, background_thread) = true;
	return thd;
}


/** Close opened tables, free memory, delete items for a MYSQL_THD.
@param[in]	thd	MYSQL_THD to reset */
void
innobase_reset_background_thd(MYSQL_THD thd)
{
	if (!thd) {
		thd = current_thd;
	}

	ut_ad(thd);
	ut_ad(THDVAR(thd, background_thread));

	/* background purge thread */
	const char *proc_info= thd_proc_info(thd, "reset");
	reset_thd(thd);
	thd_proc_info(thd, proc_info);
}


/******************************************************************//**
Check if the transaction is an auto-commit transaction. TRUE also
implies that it is a SELECT (read-only) transaction.
@return true if the transaction is an auto commit read-only transaction. */
ibool
thd_trx_is_auto_commit(
/*===================*/
	THD*	thd)	/*!< in: thread handle, can be NULL */
{
	return(thd != NULL
	       && !thd_test_options(
		       thd,
		       OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN)
	       && thd_sql_command(thd) == SQLCOM_SELECT);
}

/******************************************************************//**
Returns the NUL terminated value of glob_hostname.
@return pointer to glob_hostname. */
const char*
server_get_hostname()
/*=================*/
{
	return(glob_hostname);
}

/******************************************************************//**
Returns true if the transaction this thread is processing has edited
non-transactional tables. Used by the deadlock detector when deciding
which transaction to rollback in case of a deadlock - we try to avoid
rolling back transactions that have edited non-transactional tables.
@return true if non-transactional tables have been edited */
ibool
thd_has_edited_nontrans_tables(
/*===========================*/
	THD*	thd)	/*!< in: thread handle */
{
	return((ibool) thd_non_transactional_update(thd));
}

/******************************************************************//**
Returns the lock wait timeout for the current connection.
@return the lock wait timeout, in seconds */
uint&
thd_lock_wait_timeout(
/*==================*/
	THD*	thd)	/*!< in: thread handle, or NULL to query
			the global innodb_lock_wait_timeout */
{
	/* According to <mysql/plugin.h>, passing thd == NULL
	returns the global value of the session variable. */
	return(THDVAR(thd, lock_wait_timeout));
}

/** Get the value of innodb_tmpdir.
@param[in]	thd	thread handle, or NULL to query
			the global innodb_tmpdir.
@retval NULL if innodb_tmpdir="" */
const char *thd_innodb_tmpdir(THD *thd)
{
	const char*	tmp_dir = THDVAR(thd, tmpdir);

	if (tmp_dir != NULL && *tmp_dir == '\0') {
		tmp_dir = NULL;
	}

	return(tmp_dir);
}

/** Obtain the InnoDB transaction of a MySQL thread.
@param[in,out]	thd	thread handle
@return reference to transaction pointer */
static trx_t* thd_to_trx(THD* thd)
{
	return reinterpret_cast<trx_t*>(thd_get_ha_data(thd, innodb_hton_ptr));
}

#ifdef WITH_WSREP
/********************************************************************//**
Obtain the InnoDB transaction id of a MySQL thread.
@return	transaction id */
__attribute__((warn_unused_result, nonnull))
ulonglong
thd_to_trx_id(
	THD*	thd)	/*!< in: MySQL thread */
{
	return(thd_to_trx(thd)->id);
}

Atomic_relaxed<bool> wsrep_sst_disable_writes;

static void sst_disable_innodb_writes()
{
  const uint old_count= srv_n_fil_crypt_threads;
  fil_crypt_set_thread_cnt(0);
  srv_n_fil_crypt_threads= old_count;

  wsrep_sst_disable_writes= true;
  dict_stats_shutdown();
  purge_sys.stop();
  /* We are holding a global MDL thanks to FLUSH TABLES WITH READ LOCK.

  That will prevent any writes from arriving into InnoDB, but it will
  not prevent writes of modified pages from the buffer pool, or log
  checkpoints.

  Let us perform a log checkpoint to ensure that the entire buffer
  pool is clean, so that no writes to persistent files will be
  possible during the snapshot, and to guarantee that no crash
  recovery will be necessary when starting up on the snapshot. */
  log_make_checkpoint();
  /* If any FILE_MODIFY records were written by the checkpoint, an
  extra write of a FILE_CHECKPOINT record could still be invoked by
  buf_flush_page_cleaner(). Let us prevent that by invoking another
  checkpoint (which will write the FILE_CHECKPOINT record). */
  log_make_checkpoint();
  ut_d(recv_no_log_write= true);
  /* If this were not a no-op, an assertion would fail due to
  recv_no_log_write. */
  ut_d(log_make_checkpoint());
}

static void sst_enable_innodb_writes()
{
  ut_ad(recv_no_log_write);
  ut_d(recv_no_log_write= false);
  dict_stats_start();
  purge_sys.resume();
  wsrep_sst_disable_writes= false;
  const uint old_count= srv_n_fil_crypt_threads;
  srv_n_fil_crypt_threads= 0;
  fil_crypt_set_thread_cnt(old_count);
}

static void innodb_disable_internal_writes(bool disable)
{
  /*
    this works only in the SST donor thread and is not yet fixed
    to work in a normal connection thread
  */
  if (thd_get_thread_id(current_thd)) // if normal thread
    return;
  if (disable)
    sst_disable_innodb_writes();
  else
    sst_enable_innodb_writes();
}

static void wsrep_abort_transaction(handlerton *, THD *, THD *, my_bool)
    __attribute__((nonnull));
static int innobase_wsrep_set_checkpoint(handlerton *hton, const XID *xid);
static int innobase_wsrep_get_checkpoint(handlerton* hton, XID* xid);
#endif /* WITH_WSREP */


static inline size_t
normalize_table_name(
	char*		norm_name,
	size_t		norm_name_size,
	const char*	name)
{
	return normalize_table_name_c_low(norm_name, norm_name_size,
					name, IF_WIN(true,false));
}


ulonglong ha_innobase::table_version() const
{
  /* This is either "garbage" or something that was assigned
  on a successful ha_innobase::prepare_inplace_alter_table(). */
  return m_prebuilt->trx_id;
}

#ifdef UNIV_DEBUG
/** whether the DDL log recovery has been completed */
static bool ddl_recovery_done;
#endif

static int innodb_check_version(handlerton *hton, const char *path,
                                const LEX_CUSTRING *version,
                                ulonglong create_id)
{
  DBUG_ENTER("innodb_check_version");
  DBUG_ASSERT(hton == innodb_hton_ptr);
  ut_ad(!ddl_recovery_done);

  if (!create_id)
    DBUG_RETURN(0);

  char norm_path[FN_REFLEN];
  normalize_table_name(norm_path, sizeof(norm_path), path);

  if (dict_table_t *table= dict_table_open_on_name(norm_path, false,
                                                   DICT_ERR_IGNORE_NONE))
  {
    const trx_id_t trx_id= table->def_trx_id;
    DBUG_ASSERT(trx_id <= create_id);
    table->release();
    DBUG_PRINT("info", ("create_id: %llu  trx_id: %" PRIu64, create_id, trx_id));
    DBUG_RETURN(create_id != trx_id);
  }
  else
    DBUG_RETURN(2);
}

/** Drop any garbage intermediate tables that existed in the system
after a backup was restored.

In a final phase of Mariabackup, the commit of DDL operations is blocked,
and those DDL operations will have to be rolled back. Because the
normal DDL recovery will not run due to the lack of the log file,
at least some #sql-alter- garbage tables may remain in the InnoDB
data dictionary (while the data files themselves are missing).
We will attempt to drop the tables here. */
static void drop_garbage_tables_after_restore()
{
  btr_pcur_t pcur;
  mtr_t mtr;
  trx_t *trx= trx_create();

  ut_ad(!purge_sys.enabled());
  ut_d(purge_sys.stop_FTS());

  mtr.start();
  if (pcur.open_leaf(true, dict_sys.sys_tables->indexes.start, BTR_SEARCH_LEAF,
                     &mtr) != DB_SUCCESS)
    goto all_fail;
  for (;;)
  {
    btr_pcur_move_to_next_user_rec(&pcur, &mtr);

    if (!btr_pcur_is_on_user_rec(&pcur))
      break;

    const rec_t *rec= btr_pcur_get_rec(&pcur);
    if (rec_get_deleted_flag(rec, 0))
      continue;

    static_assert(DICT_FLD__SYS_TABLES__NAME == 0, "compatibility");
    size_t len;
    if (rec_get_1byte_offs_flag(rec))
    {
      len= rec_1_get_field_end_info(rec, 0);
      if (len & REC_1BYTE_SQL_NULL_MASK)
        continue; /* corrupted SYS_TABLES.NAME */
    }
    else
    {
      len= rec_2_get_field_end_info(rec, 0);
      static_assert(REC_2BYTE_EXTERN_MASK == 16384, "compatibility");
      if (len >= REC_2BYTE_EXTERN_MASK)
        continue; /* corrupted SYS_TABLES.NAME */
    }

    if (len < tmp_file_prefix_length)
      continue;
    if (const char *f= static_cast<const char*>
        (memchr(rec, '/', len - tmp_file_prefix_length)))
    {
      if (memcmp(f + 1, tmp_file_prefix, tmp_file_prefix_length))
        continue;
    }
    else
      continue;

    btr_pcur_store_position(&pcur, &mtr);
    btr_pcur_commit_specify_mtr(&pcur, &mtr);

    trx_start_for_ddl(trx);
    std::vector<pfs_os_file_t> deleted;
    dberr_t err= DB_TABLE_NOT_FOUND;
    row_mysql_lock_data_dictionary(trx);

    if (dict_table_t *table= dict_sys.load_table
        ({reinterpret_cast<const char*>(pcur.old_rec), len},
         DICT_ERR_IGNORE_DROP))
    {
      table->acquire();
      row_mysql_unlock_data_dictionary(trx);
      err= lock_table_for_trx(table, trx, LOCK_X);
      if (err == DB_SUCCESS &&
          (table->flags2 & (DICT_TF2_FTS_HAS_DOC_ID | DICT_TF2_FTS)))
      {
        fts_optimize_remove_table(table);
        err= fts_lock_tables(trx, *table);
      }
      if (err == DB_SUCCESS)
        err= lock_sys_tables(trx);
      row_mysql_lock_data_dictionary(trx);
      table->release();

      if (err == DB_SUCCESS)
        err= trx->drop_table(*table);
      if (err != DB_SUCCESS)
        goto fail;
      trx->commit(deleted);
    }
    else
    {
fail:
      trx->rollback();
      sql_print_error("InnoDB: cannot drop %.*s: %s",
                      static_cast<int>(len), pcur.old_rec, ut_strerr(err));
    }

    row_mysql_unlock_data_dictionary(trx);
    for (pfs_os_file_t d : deleted)
      os_file_close(d);

    mtr.start();
    if (pcur.restore_position(BTR_SEARCH_LEAF, &mtr) == btr_pcur_t::CORRUPTED)
      break;
  }

all_fail:
  mtr.commit();
  trx->free();
  ut_free(pcur.old_rec_buf);
  ut_d(purge_sys.resume_FTS());
}

static int innodb_ddl_recovery_done(handlerton*)
{
  ut_ad(!ddl_recovery_done);
  ut_d(ddl_recovery_done= true);
  if (!srv_read_only_mode && srv_operation <= SRV_OPERATION_EXPORT_RESTORED &&
      srv_force_recovery < SRV_FORCE_NO_BACKGROUND)
  {
    if (srv_start_after_restore && !high_level_read_only)
      drop_garbage_tables_after_restore();
    srv_init_purge_tasks();
  }
  return 0;
}

/** Report an aborted transaction or statement.
@param thd   execution context
@param all   true=transaction, false=statement
@param err   InnoDB error code */
static void innodb_transaction_abort(THD *thd, bool all, dberr_t err) noexcept
{
  if (!thd)
    return;
  if (!all);
  else if (trx_t *trx = thd_to_trx(thd))
  {
    ut_ad(trx->state == TRX_STATE_NOT_STARTED);
    trx->state= TRX_STATE_ABORTED;
    if (thd_log_warnings(thd) >= 4)
      sql_print_error("InnoDB: Transaction was aborted due to %s",
                      ut_strerr(err));
  }
  thd_mark_transaction_to_rollback(thd, all);
}

/********************************************************************//**
Converts an InnoDB error code to a MySQL error code and also tells to MySQL
about a possible transaction rollback inside InnoDB caused by a lock wait
timeout or a deadlock.
@return MySQL error code */
static int
convert_error_code_to_mysql(
/*========================*/
	dberr_t	error,	/*!< in: InnoDB error code */
	ulint	flags,  /*!< in: InnoDB table flags, or 0 */
	THD*	thd)	/*!< in: user thread handle or NULL */
{
	switch (error) {
	case DB_SUCCESS:
		return(0);

	case DB_INTERRUPTED:
		return(HA_ERR_ABORTED_BY_USER);

	case DB_FOREIGN_EXCEED_MAX_CASCADE:
		ut_ad(thd);
		push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
				    HA_ERR_ROW_IS_REFERENCED,
				    "InnoDB: Cannot delete/update "
				    "rows with cascading foreign key "
				    "constraints that exceed max "
				    "depth of %d. Please "
				    "drop extra constraints and try "
				    "again", FK_MAX_CASCADE_DEL);
		return(HA_ERR_FK_DEPTH_EXCEEDED);

	case DB_CANT_CREATE_GEOMETRY_OBJECT:
		my_error(ER_CANT_CREATE_GEOMETRY_OBJECT, MYF(0));
		return(HA_ERR_NULL_IN_SPATIAL);

	case DB_ERROR:
	default:
		return(HA_ERR_GENERIC); /* unspecified error */

	case DB_DUPLICATE_KEY:
		/* Be cautious with returning this error, since
		mysql could re-enter the storage layer to get
		duplicated key info, the operation requires a
		valid table handle and/or transaction information,
		which might not always be available in the error
		handling stage. */
		return(HA_ERR_FOUND_DUPP_KEY);

	case DB_READ_ONLY:
		return(HA_ERR_TABLE_READONLY);

	case DB_FOREIGN_DUPLICATE_KEY:
		return(HA_ERR_FOREIGN_DUPLICATE_KEY);

	case DB_MISSING_HISTORY:
		return(HA_ERR_TABLE_DEF_CHANGED);

	case DB_RECORD_NOT_FOUND:
		return(HA_ERR_NO_ACTIVE_RECORD);

	case DB_DEADLOCK:
	case DB_RECORD_CHANGED:
		/* Since we rolled back the whole transaction, the
		cached binlog must be emptied. */
		innodb_transaction_abort(thd, true, error);
		return error == DB_DEADLOCK
			? HA_ERR_LOCK_DEADLOCK : HA_ERR_RECORD_CHANGED;

	case DB_LOCK_WAIT_TIMEOUT:
		/* Starting from 5.0.13, we let MySQL just roll back the
		latest SQL statement in a lock wait timeout. Previously, we
		rolled back the whole transaction. */

		innodb_transaction_abort(thd, innobase_rollback_on_timeout,
					 error);
		return(HA_ERR_LOCK_WAIT_TIMEOUT);

	case DB_NO_REFERENCED_ROW:
		return(HA_ERR_NO_REFERENCED_ROW);

	case DB_ROW_IS_REFERENCED:
		return(HA_ERR_ROW_IS_REFERENCED);

	case DB_NO_FK_ON_S_BASE_COL:
	case DB_CANNOT_ADD_CONSTRAINT:
	case DB_CHILD_NO_INDEX:
	case DB_PARENT_NO_INDEX:
		return(HA_ERR_CANNOT_ADD_FOREIGN);

	case DB_CANNOT_DROP_CONSTRAINT:

		return(HA_ERR_ROW_IS_REFERENCED); /* TODO: This is a bit
						misleading, a new MySQL error
						code should be introduced */

	case DB_CORRUPTION:
	case DB_PAGE_CORRUPTED:
		return(HA_ERR_CRASHED);

	case DB_OUT_OF_FILE_SPACE:
		return(HA_ERR_RECORD_FILE_FULL);

	case DB_TEMP_FILE_WRITE_FAIL:
		/* This error can happen during
		copy_data_between_tables() or bulk insert operation */
		innodb_transaction_abort(thd,
					 innobase_rollback_on_timeout,
					 error);
		my_error(ER_GET_ERRMSG, MYF(0),
                         DB_TEMP_FILE_WRITE_FAIL,
                         ut_strerr(DB_TEMP_FILE_WRITE_FAIL),
                         "InnoDB");
		return(HA_ERR_INTERNAL_ERROR);

	case DB_TABLE_NOT_FOUND:
		return(HA_ERR_NO_SUCH_TABLE);

	case DB_DECRYPTION_FAILED:
		return(HA_ERR_DECRYPTION_FAILED);

	case DB_TABLESPACE_NOT_FOUND:
		return(HA_ERR_TABLESPACE_MISSING);

	case DB_TOO_BIG_RECORD: {
		/* If prefix is true then a 768-byte prefix is stored
		locally for BLOB fields. Refer to dict_table_get_format().
		We limit max record size to 16k for 64k page size. */
		bool prefix = !DICT_TF_HAS_ATOMIC_BLOBS(flags);
		bool comp = !!(flags & DICT_TF_COMPACT);
		ulint free_space = page_get_free_space_of_empty(comp) / 2;

		if (free_space >= ulint(comp ? COMPRESSED_REC_MAX_DATA_SIZE :
				          REDUNDANT_REC_MAX_DATA_SIZE)) {
			free_space = (comp ? COMPRESSED_REC_MAX_DATA_SIZE :
				REDUNDANT_REC_MAX_DATA_SIZE) - 1;
		}

		my_printf_error(ER_TOO_BIG_ROWSIZE,
			"Row size too large (> " ULINTPF "). Changing some columns "
			"to TEXT or BLOB %smay help. In current row "
			"format, BLOB prefix of %d bytes is stored inline.",
			MYF(0),
			free_space,
			prefix
			? "or using ROW_FORMAT=DYNAMIC or"
			  " ROW_FORMAT=COMPRESSED "
			: "",
			prefix
			? DICT_MAX_FIXED_COL_LEN
			: 0);
		return(HA_ERR_TO_BIG_ROW);
	}

	case DB_TOO_BIG_INDEX_COL:
		my_error(ER_INDEX_COLUMN_TOO_LONG, MYF(0),
			 (ulong) DICT_MAX_FIELD_LEN_BY_FORMAT_FLAG(flags));
		return(HA_ERR_INDEX_COL_TOO_LONG);

	case DB_LOCK_TABLE_FULL:
		/* Since we rolled back the whole transaction, we must
		tell it also to MySQL so that MySQL knows to empty the
		cached binlog for this transaction */

		if (thd) {
			thd_mark_transaction_to_rollback(thd, 1);
		}

		return(HA_ERR_LOCK_TABLE_FULL);

	case DB_FTS_INVALID_DOCID:
		return(HA_FTS_INVALID_DOCID);
	case DB_FTS_EXCEED_RESULT_CACHE_LIMIT:
		return(HA_ERR_OUT_OF_MEM);
	case DB_TOO_MANY_CONCURRENT_TRXS:
		return(HA_ERR_TOO_MANY_CONCURRENT_TRXS);
	case DB_UNSUPPORTED:
		return(HA_ERR_UNSUPPORTED);
	case DB_INDEX_CORRUPT:
		return(HA_ERR_INDEX_CORRUPT);
	case DB_UNDO_RECORD_TOO_BIG:
		return(HA_ERR_UNDO_REC_TOO_BIG);
	case DB_OUT_OF_MEMORY:
		return(HA_ERR_OUT_OF_MEM);
	case DB_TABLESPACE_EXISTS:
		return(HA_ERR_TABLESPACE_EXISTS);
	case DB_TABLESPACE_DELETED:
		return(HA_ERR_TABLESPACE_MISSING);
	case DB_IDENTIFIER_TOO_LONG:
		return(HA_ERR_INTERNAL_ERROR);
	case DB_TABLE_CORRUPT:
		return(HA_ERR_TABLE_CORRUPT);
	case DB_FTS_TOO_MANY_WORDS_IN_PHRASE:
		return(HA_ERR_FTS_TOO_MANY_WORDS_IN_PHRASE);
	case DB_COMPUTE_VALUE_FAILED:
		return(HA_ERR_GENERIC); // impossible
	}
}

/*************************************************************//**
Prints info of a THD object (== user session thread) to the given file. */
void
innobase_mysql_print_thd(
/*=====================*/
	FILE*	f,		/*!< in: output stream */
	THD*	thd)		/*!< in: MySQL THD object */
{
	char	buffer[3072];

	fputs(thd_get_error_context_description(thd, buffer, sizeof buffer,
						0), f);
	putc('\n', f);
}

/******************************************************************//**
Get the variable length bounds of the given character set. */
static void
innobase_get_cset_width(
/*====================*/
	ulint	cset,		/*!< in: MySQL charset-collation code */
	unsigned*mbminlen,	/*!< out: minimum length of a char (in bytes) */
	unsigned*mbmaxlen)	/*!< out: maximum length of a char (in bytes) */
{
	CHARSET_INFO*	cs;
	ut_ad(cset <= MAX_CHAR_COLL_NUM);
	ut_ad(mbminlen);
	ut_ad(mbmaxlen);

	cs = cset ? get_charset((uint)cset, MYF(MY_WME)) : NULL;
	if (cs) {
		*mbminlen = cs->mbminlen;
		*mbmaxlen = cs->mbmaxlen;
		ut_ad(*mbminlen < DATA_MBMAX);
		ut_ad(*mbmaxlen < DATA_MBMAX);
	} else {
		THD*	thd = current_thd;

		if (thd && thd_sql_command(thd) == SQLCOM_DROP_TABLE) {

			/* Fix bug#46256: allow tables to be dropped if the
			collation is not found, but issue a warning. */
			if (cset != 0) {

				sql_print_warning(
					"Unknown collation #" ULINTPF ".",
					cset);
			}
		} else {

			ut_a(cset == 0);
		}

		*mbminlen = *mbmaxlen = 0;
	}
}

/*********************************************************************//**
Compute the mbminlen and mbmaxlen members of a data type structure. */
void
dtype_get_mblen(
/*============*/
	ulint	mtype,		/*!< in: main type */
	ulint	prtype,		/*!< in: precise type (and collation) */
	unsigned*mbminlen,	/*!< out: minimum length of a
				multi-byte character */
	unsigned*mbmaxlen)	/*!< out: maximum length of a
				multi-byte character */
{
	if (dtype_is_string_type(mtype)) {
		innobase_get_cset_width(dtype_get_charset_coll(prtype),
					mbminlen, mbmaxlen);
		ut_ad(*mbminlen <= *mbmaxlen);
		ut_ad(*mbminlen < DATA_MBMAX);
		ut_ad(*mbmaxlen < DATA_MBMAX);
	} else {
		*mbminlen = *mbmaxlen = 0;
	}
}

/******************************************************************//**
Converts an identifier to UTF-8. */
void
innobase_convert_from_id(
/*=====================*/
	CHARSET_INFO*	cs,	/*!< in: the 'from' character set */
	char*		to,	/*!< out: converted identifier */
	const char*	from,	/*!< in: identifier to convert */
	ulint		len)	/*!< in: length of 'to', in bytes */
{
	uint	errors;

	strconvert(cs, from, FN_REFLEN, system_charset_info, to, (uint) len, &errors);
}


/******************************************************************//**
Compares NUL-terminated UTF-8 strings case insensitively. The
second string contains wildcards.
@return 0 if a match is found, 1 if not */
static
int
innobase_wildcasecmp(
/*=================*/
	const char*	a,	/*!< in: string to compare */
	const char*	b)	/*!< in: wildcard string to compare */
{
	return(wild_case_compare(system_charset_info, a, b));
}

/** Strip dir name from a full path name and return only the file name
@param[in]	path_name	full path name
@return file name or "null" if no file name */
const char*
innobase_basename(
	const char*	path_name)
{
	const char*	name = base_name(path_name);

	return((name) ? name : "null");
}

/** Determines the current SQL statement.
Thread unsafe, can only be called from the thread owning the THD.
@param[in]	thd	MySQL thread handle
@param[out]	length	Length of the SQL statement
@return			SQL statement string */
const char*
innobase_get_stmt_unsafe(
	THD*	thd,
	size_t*	length)
{
	if (const LEX_STRING *stmt = thd_query_string(thd)) {
		*length = stmt->length;
		return stmt->str;
	}

	*length = 0;
	return NULL;
}

/**
  Test a file path whether it is same as mysql data directory path.

  @param path null terminated character string

  @return
    @retval TRUE The path is different from mysql data directory.
    @retval FALSE The path is same as mysql data directory.
*/
static bool is_mysql_datadir_path(const char *path)
{
  if (path == NULL)
    return false;

  char mysql_data_dir[FN_REFLEN], path_dir[FN_REFLEN];
  convert_dirname(path_dir, path, NullS);
  convert_dirname(mysql_data_dir, mysql_unpacked_real_data_home, NullS);
  size_t mysql_data_home_len= dirname_length(mysql_data_dir);
  size_t path_len = dirname_length(path_dir);

  if (path_len < mysql_data_home_len)
    return true;

  if (!lower_case_file_system)
    return(memcmp(mysql_data_dir, path_dir, mysql_data_home_len));

  return(files_charset_info->strnncoll((uchar *) path_dir, path_len,
                                       (uchar *) mysql_data_dir,
                                       mysql_data_home_len,
                                       TRUE));
}

/*********************************************************************//**
Wrapper around MySQL's copy_and_convert function.
@return number of bytes copied to 'to' */
static
ulint
innobase_convert_string(
/*====================*/
	void*		to,		/*!< out: converted string */
	ulint		to_length,	/*!< in: number of bytes reserved
					for the converted string */
	CHARSET_INFO*	to_cs,		/*!< in: character set to convert to */
	const void*	from,		/*!< in: string to convert */
	ulint		from_length,	/*!< in: number of bytes to convert */
	CHARSET_INFO*	from_cs,	/*!< in: character set to convert
					from */
	uint*		errors)		/*!< out: number of errors encountered
					during the conversion */
{
	return(copy_and_convert(
			(char*) to, (uint32) to_length, to_cs,
			(const char*) from, (uint32) from_length, from_cs,
			errors));
}

/*******************************************************************//**
Formats the raw data in "data" (in InnoDB on-disk format) that is of
type DATA_(CHAR|VARCHAR|MYSQL|VARMYSQL) using "charset_coll" and writes
the result to "buf". The result is converted to "system_charset_info".
Not more than "buf_size" bytes are written to "buf".
The result is always NUL-terminated (provided buf_size > 0) and the
number of bytes that were written to "buf" is returned (including the
terminating NUL).
@return number of bytes that were written */
ulint
innobase_raw_format(
/*================*/
	const char*	data,		/*!< in: raw data */
	ulint		data_len,	/*!< in: raw data length
					in bytes */
	ulint		charset_coll,	/*!< in: charset collation */
	char*		buf,		/*!< out: output buffer */
	ulint		buf_size)	/*!< in: output buffer size
					in bytes */
{
	/* XXX we use a hard limit instead of allocating
	but_size bytes from the heap */
	CHARSET_INFO*	data_cs;
	char		buf_tmp[8192];
	ulint		buf_tmp_used;
	uint		num_errors;

	data_cs = all_charsets[charset_coll];

	buf_tmp_used = innobase_convert_string(buf_tmp, sizeof(buf_tmp),
					       system_charset_info,
					       data, data_len, data_cs,
					       &num_errors);

	return(ut_str_sql_format(buf_tmp, buf_tmp_used, buf, buf_size));
}

/*
The helper function nlz(x) calculates the number of leading zeros
in the binary representation of the number "x", either using a
built-in compiler function or a substitute trick based on the use
of the multiplication operation and a table indexed by the prefix
of the multiplication result:
*/
#ifdef __GNUC__
#define nlz(x) __builtin_clzll(x)
#elif defined(_MSC_VER) && !defined(_M_CEE_PURE) && \
  (defined(_M_IX86) || defined(_M_X64) || defined(_M_ARM64))
#ifndef __INTRIN_H_
#pragma warning(push, 4)
#pragma warning(disable: 4255 4668)
#include <intrin.h>
#pragma warning(pop)
#endif
__forceinline unsigned int nlz (ulonglong x)
{
#if defined(_M_IX86) || defined(_M_X64)
  unsigned long n;
#ifdef _M_X64
  _BitScanReverse64(&n, x);
  return (unsigned int) n ^ 63;
#else
  unsigned long y = (unsigned long) (x >> 32);
  unsigned int m = 31;
  if (y == 0)
  {
    y = (unsigned long) x;
    m = 63;
  }
  _BitScanReverse(&n, y);
  return (unsigned int) n ^ m;
#endif
#elif defined(_M_ARM64)
  return _CountLeadingZeros64(x);
#endif
}
#else
inline unsigned int nlz (ulonglong x)
{
  static unsigned char table [48] = {
    32,  6,  5,  0,  4, 12,  0, 20,
    15,  3, 11,  0,  0, 18, 25, 31,
     8, 14,  2,  0, 10,  0,  0,  0,
     0,  0,  0, 21,  0,  0, 19, 26,
     7,  0, 13,  0, 16,  1, 22, 27,
     9,  0, 17, 23, 28, 24, 29, 30
  };
  unsigned int y= (unsigned int) (x >> 32);
  unsigned int n= 0;
  if (y == 0) {
    y= (unsigned int) x;
    n= 32;
  }
  y = y | (y >> 1); // Propagate leftmost 1-bit to the right.
  y = y | (y >> 2);
  y = y | (y >> 4);
  y = y | (y >> 8);
  y = y & ~(y >> 16);
  y = y * 0x3EF5D037;
  return n + table[y >> 26];
}
#endif

/*********************************************************************//**
Compute the next autoinc value.

For MySQL replication the autoincrement values can be partitioned among
the nodes. The offset is the start or origin of the autoincrement value
for a particular node. For n nodes the increment will be n and the offset
will be in the interval [1, n]. The formula tries to allocate the next
value for a particular node.

Note: This function is also called with increment set to the number of
values we want to reserve for multi-value inserts e.g.,

	INSERT INTO T VALUES(), (), ();

innobase_next_autoinc() will be called with increment set to 3 where
autoinc_lock_mode != TRADITIONAL because we want to reserve 3 values for
the multi-value INSERT above.
@return the next value */
ulonglong
innobase_next_autoinc(
/*==================*/
	ulonglong	current,	/*!< in: Current value */
	ulonglong	need,		/*!< in: count of values needed */
	ulonglong	step,		/*!< in: AUTOINC increment step */
	ulonglong	offset,		/*!< in: AUTOINC offset */
	ulonglong	max_value)	/*!< in: max value for type */
{
	ulonglong	next_value;
	ulonglong	block;

	/* Should never be 0. */
	ut_a(need > 0);
	ut_a(step > 0);
	ut_a(max_value > 0);

	/*
	  We need to calculate the "block" value equal to the product
	  "step * need". However, when calculating this product, an integer
	  overflow can occur, so we cannot simply use the usual multiplication
	  operation. The snippet below calculates the product of two numbers
	  and detects an unsigned integer overflow:
	*/
	unsigned int	m= nlz(need);
	unsigned int	n= nlz(step);
	if (m + n <= 8 * sizeof(ulonglong) - 2) {
		// The bit width of the original values is too large,
		// therefore we are guaranteed to get an overflow.
		goto overflow;
	}
	block = need * (step >> 1);
	if ((longlong) block < 0) {
		goto overflow;
	}
	block += block;
	if (step & 1) {
		block += need;
		if (block < need) {
			goto overflow;
		}
	}

	/* Check for overflow. Current can be > max_value if the value
	is in reality a negative value. Also, the visual studio compiler
	converts large double values (which hypothetically can then be
	passed here as the values of the "current" parameter) automatically
	into unsigned long long datatype maximum value: */
	if (current > max_value) {
		goto overflow;
	}

	/* According to MySQL documentation, if the offset is greater than
	the step then the offset is ignored. */
	if (offset > step) {
		offset = 0;
	}

	/*
	  Let's round the current value to within a step-size block:
	*/
	if (current > offset) {
		next_value = current - offset;
	} else {
		next_value = offset - current;
	}
	next_value -= next_value % step;

	/*
	  Add an offset to the next value and check that the addition
	  does not cause an integer overflow:
	*/
	next_value += offset;
	if (next_value < offset) {
		goto overflow;
	}

	/*
	  Add a block to the next value and check that the addition
	  does not cause an integer overflow:
	*/
	next_value += block;
	if (next_value < block) {
		goto overflow;
	}

	return(next_value);

overflow:
	/*
	  Allow auto_increment to go over max_value up to max ulonglong.
	  This allows us to detect that all values are exhausted.
	  If we don't do this, we will return max_value several times
	  and get duplicate key errors instead of auto increment value
	  out of range:
	*/
	return(~(ulonglong) 0);
}

/*********************************************************************//**
Initializes some fields in an InnoDB transaction object. */
static
void
innobase_trx_init(
/*==============*/
	THD*	thd,	/*!< in: user thread handle */
	trx_t*	trx)	/*!< in/out: InnoDB transaction handle */
{
	DBUG_ENTER("innobase_trx_init");
	DBUG_ASSERT(thd == trx->mysql_thd);

	/* Ensure that thd_lock_wait_timeout(), which may be called
	while holding lock_sys.latch, by lock_rec_enqueue_waiting(),
	will not end up acquiring LOCK_global_system_variables in
	intern_sys_var_ptr(). */
	(void) THDVAR(thd, lock_wait_timeout);

	trx->check_foreigns = !thd_test_options(
		thd, OPTION_NO_FOREIGN_KEY_CHECKS);

	trx->check_unique_secondary = !thd_test_options(
		thd, OPTION_RELAXED_UNIQUE_CHECKS);
	trx->snapshot_isolation = THDVAR(thd, snapshot_isolation) & 1;

	DBUG_VOID_RETURN;
}

/*********************************************************************//**
Allocates an InnoDB transaction for a MySQL handler object for DML.
@return InnoDB transaction handle */
trx_t*
innobase_trx_allocate(
/*==================*/
	THD*	thd)	/*!< in: user thread handle */
{
	trx_t*	trx;

	DBUG_ENTER("innobase_trx_allocate");
	DBUG_ASSERT(thd != NULL);
	DBUG_ASSERT(EQ_CURRENT_THD(thd));

	trx = trx_create();

	trx->mysql_thd = thd;

	innobase_trx_init(thd, trx);

	DBUG_RETURN(trx);
}

/*********************************************************************//**
Gets the InnoDB transaction handle for a MySQL handler object, creates
an InnoDB transaction struct if the corresponding MySQL thread struct still
lacks one.
@return InnoDB transaction handle */
static
trx_t*
check_trx_exists(
/*=============*/
	THD*	thd)	/*!< in: user thread handle */
{
	if (trx_t* trx = thd_to_trx(thd)) {
		ut_a(trx->magic_n == TRX_MAGIC_N);
		innobase_trx_init(thd, trx);
		return trx;
	} else {
		trx = innobase_trx_allocate(thd);
		thd_set_ha_data(thd, innodb_hton_ptr, trx);
		return trx;
	}
}

/**
  Gets current trx.

  This function may be called during InnoDB initialisation, when
  innodb_hton_ptr->slot is not yet set to meaningful value.
*/

trx_t *current_trx()
{
	THD *thd=current_thd;
	if (likely(thd != 0) && innodb_hton_ptr->slot != HA_SLOT_UNDEF) {
		return thd_to_trx(thd);
	} else {
		return(NULL);
	}
}

/*********************************************************************//**
Note that a transaction has been registered with MySQL.
@return true if transaction is registered with MySQL 2PC coordinator */
static inline
bool
trx_is_registered_for_2pc(
/*======================*/
	const trx_t*	trx)	/* in: transaction */
{
	return(trx->is_registered == 1);
}

/*********************************************************************//**
Note that a transaction has been deregistered. */
static inline
void
trx_deregister_from_2pc(
/*====================*/
	trx_t*	trx)	/* in: transaction */
{
  trx->is_registered= false;
  trx->active_commit_ordered= false;
}

/**
  Set a transaction savepoint.

  @param thd        server thread descriptor
  @param savepoint  transaction savepoint storage area

  @retval 0                   on success
  @retval HA_ERR_NO_SAVEPOINT if the transaction is in an inconsistent state
*/
static int innobase_savepoint(THD *thd, void *savepoint) noexcept
{
  DBUG_ENTER("innobase_savepoint");

  /* In the autocommit mode there is no sense to set a savepoint
  (unless we are in sub-statement), so SQL layer ensures that
  this method is never called in such situation.  */
  trx_t *trx= check_trx_exists(thd);

  /* Cannot happen outside of transaction */
  DBUG_ASSERT(trx_is_registered_for_2pc(trx));

  switch (UNIV_EXPECT(trx->state, TRX_STATE_ACTIVE)) {
  default:
    ut_ad("invalid state" == 0);
    DBUG_RETURN(HA_ERR_NO_SAVEPOINT);
  case TRX_STATE_NOT_STARTED:
    trx_start_if_not_started_xa(trx, false);
    /* fall through */
  case TRX_STATE_ACTIVE:
    const undo_no_t savept{trx->undo_no};
    *static_cast<undo_no_t*>(savepoint)= savept;
    trx->last_stmt_start= savept;
    trx->end_bulk_insert();

    if (trx->fts_trx)
      fts_savepoint_take(trx->fts_trx, savepoint);

    DBUG_RETURN(0);
  }
}

/**
  Releases a transaction savepoint.

  @param thd        server thread descriptor
  @param savepoint  transaction savepoint to be released

  @return 0 always
*/
static int innobase_release_savepoint(THD *thd, void *savepoint) noexcept
{
  DBUG_ENTER("innobase_release_savepoint");
  trx_t *trx= check_trx_exists(thd);
  ut_ad(trx->mysql_thd == thd);
  if (trx->fts_trx)
    fts_savepoint_release(trx, savepoint);
  DBUG_RETURN(0);
}

/**
  Frees a possible InnoDB trx object associated with the current THD.

  @param thd   server thread descriptor, which resources should be free'd

  @return 0 always
*/
static int innobase_close_connection(THD *thd) noexcept
{
  if (auto trx= thd_to_trx(thd))
  {
    thd_set_ha_data(thd, innodb_hton_ptr, nullptr);
    switch (trx->state) {
    case TRX_STATE_ABORTED:
      trx->state= TRX_STATE_NOT_STARTED;
      /* fall through */
    case TRX_STATE_NOT_STARTED:
      ut_ad(!trx->id);
      trx->will_lock= false;
      break;
    default:
      ut_ad("invalid state" == 0);
      return 0;
    case TRX_STATE_PREPARED:
      if (trx->has_logged_persistent())
      {
        trx_disconnect_prepared(trx);
        return 0;
      }
      /* fall through */
    case TRX_STATE_ACTIVE:
      /* If we had reserved the auto-inc lock for some table (if
      we come here to roll back the latest SQL statement) we
      release it now before a possibly lengthy rollback */
      lock_unlock_table_autoinc(trx);
      trx_rollback_for_mysql(trx);
    }
    trx_deregister_from_2pc(trx);
    trx->free();
    DEBUG_SYNC(thd, "innobase_connection_closed");
  }
  return 0;
}

/**
  XA ROLLBACK after XA PREPARE

  @param xid   X/Open XA transaction identification

  @retval 0            if the transaction was found and rolled back
  @retval XAER_NOTA    if no such transaction exists
  @retval XAER_RMFAIL  if InnoDB is in read-only mode
*/
static int innobase_rollback_by_xid(XID *xid) noexcept
{
  DBUG_EXECUTE_IF("innobase_xa_fail", return XAER_RMFAIL;);
  if (high_level_read_only)
    return XAER_RMFAIL;
  if (trx_t *trx= trx_get_trx_by_xid(xid))
  {
    /* Lookup by xid clears the transaction xid.
       For wsrep we clear it below. */
    ut_ad(trx->xid.is_null() || wsrep_is_wsrep_xid(&trx->xid));
    trx->xid.null();
    trx_deregister_from_2pc(trx);
    THD* thd= trx->mysql_thd;
    dberr_t err= trx_rollback_for_mysql(trx);
    ut_ad(!trx->will_lock);
    trx->free();
    return convert_error_code_to_mysql(err, 0, thd);
  }
  return XAER_NOTA;
}

/** Initialize the InnoDB persistent statistics attributes.
@param table           InnoDB table
@param table_options   MariaDB table options
@param sar             the value of STATS_AUTO_RECALC
@param initialized     whether the InnoDB statistics were already initialized
@return whether table->stats_sample_pages needs to be initialized */
static bool innodb_copy_stat_flags(dict_table_t *table,
                                   ulong table_options,
                                   enum_stats_auto_recalc sar,
                                   bool initialized) noexcept
{
  if (table->is_temporary() || table->no_rollback())
  {
    table->stat= dict_table_t::STATS_INITIALIZED |
      dict_table_t::STATS_PERSISTENT_OFF | dict_table_t::STATS_AUTO_RECALC_OFF;
    table->stats_sample_pages= 1;
    return false;
  }

  static_assert(HA_OPTION_STATS_PERSISTENT ==
                dict_table_t::STATS_PERSISTENT_ON << 11, "");
  static_assert(HA_OPTION_NO_STATS_PERSISTENT ==
                dict_table_t::STATS_PERSISTENT_OFF << 11, "");
  uint32_t stat=
    uint32_t(table_options &
             (HA_OPTION_STATS_PERSISTENT |
              HA_OPTION_NO_STATS_PERSISTENT)) >> 11;
  static_assert(uint32_t{HA_STATS_AUTO_RECALC_ON} << 3 ==
                dict_table_t::STATS_AUTO_RECALC_ON, "");
  static_assert(uint32_t{HA_STATS_AUTO_RECALC_OFF} << 3 ==
                dict_table_t::STATS_AUTO_RECALC_OFF, "");
  static_assert(true == dict_table_t::STATS_INITIALIZED, "");
  stat|= (sar & (HA_STATS_AUTO_RECALC_ON | HA_STATS_AUTO_RECALC_OFF)) << 3 |
    uint32_t(initialized);

  table->stat= stat;
  return true;
}

/*********************************************************************//**
Copy table flags from MySQL's HA_CREATE_INFO into an InnoDB table object.
Those flags are stored in .frm file and end up in the MySQL table object,
but are frequently used inside InnoDB so we keep their copies into the
InnoDB table object. */
static
void
innobase_copy_frm_flags_from_create_info(
/*=====================================*/
	dict_table_t*		innodb_table,	/*!< in/out: InnoDB table */
	const HA_CREATE_INFO*	create_info)	/*!< in: create info */
{
  if (innodb_copy_stat_flags(innodb_table, create_info->table_options,
                             create_info->stats_auto_recalc, false))
    innodb_table->stats_sample_pages= create_info->stats_sample_pages;
}

/*********************************************************************//**
Copy table flags from MySQL's TABLE_SHARE into an InnoDB table object.
Those flags are stored in .frm file and end up in the MySQL table object,
but are frequently used inside InnoDB so we keep their copies into the
InnoDB table object. */
void
innobase_copy_frm_flags_from_table_share(
/*=====================================*/
	dict_table_t*		innodb_table,	/*!< in/out: InnoDB table */
	const TABLE_SHARE*	table_share)	/*!< in: table share */
{
  if (innodb_copy_stat_flags(innodb_table, table_share->db_create_options,
                             table_share->stats_auto_recalc,
                             innodb_table->stat_initialized()))
    innodb_table->stats_sample_pages= table_share->stats_sample_pages;
}

/*********************************************************************//**
Construct ha_innobase handler. */

ha_innobase::ha_innobase(
/*=====================*/
	handlerton*	hton,
	TABLE_SHARE*	table_arg)
	:handler(hton, table_arg),
	m_prebuilt(),
	m_user_thd(),
	m_int_table_flags(HA_REC_NOT_IN_SEQ
			  | HA_NULL_IN_KEY
			  | HA_CAN_VIRTUAL_COLUMNS
			  | HA_CAN_INDEX_BLOBS
			  | HA_CAN_SQL_HANDLER
			  | HA_REQUIRES_KEY_COLUMNS_FOR_DELETE
			  | HA_PRIMARY_KEY_REQUIRED_FOR_POSITION
			  | HA_PRIMARY_KEY_IN_READ_INDEX
			  | HA_BINLOG_ROW_CAPABLE
			  | HA_CAN_GEOMETRY
			  | HA_PARTIAL_COLUMN_READ
			  | HA_TABLE_SCAN_ON_INDEX
			  | HA_CAN_FULLTEXT
			  | HA_CAN_FULLTEXT_EXT
		/* JAN: TODO: MySQL 5.7
			  | HA_CAN_FULLTEXT_HINTS
		*/
			  | HA_CAN_EXPORT
                          | HA_ONLINE_ANALYZE
			  | HA_CAN_RTREEKEYS
                          | HA_CAN_TABLES_WITHOUT_ROLLBACK
                          | HA_CAN_ONLINE_BACKUPS
			  | HA_CONCURRENT_OPTIMIZE
			  | HA_CAN_SKIP_LOCKED
		  ),
	m_start_of_scan(),
        m_mysql_has_locked()
{}

/*********************************************************************//**
Destruct ha_innobase handler. */

ha_innobase::~ha_innobase() = default;
/*======================*/

/*********************************************************************//**
Updates the user_thd field in a handle and also allocates a new InnoDB
transaction handle if needed, and updates the transaction fields in the
m_prebuilt struct. */
void
ha_innobase::update_thd(
/*====================*/
	THD*	thd)	/*!< in: thd to use the handle */
{
	DBUG_ENTER("ha_innobase::update_thd");
	DBUG_PRINT("ha_innobase::update_thd", ("user_thd: %p -> %p",
		   m_user_thd, thd));

	/* The table should have been opened in ha_innobase::open(). */
	DBUG_ASSERT(m_prebuilt->table->get_ref_count() > 0);

	trx_t*	trx = check_trx_exists(thd);

	ut_ad(!trx->dict_operation_lock_mode);
	ut_ad(!trx->dict_operation);

	if (m_prebuilt->trx != trx) {

		row_update_prebuilt_trx(m_prebuilt, trx);
	}

	m_user_thd = thd;

	DBUG_ASSERT(m_prebuilt->trx->magic_n == TRX_MAGIC_N);
	DBUG_ASSERT(m_prebuilt->trx == thd_to_trx(m_user_thd));

	DBUG_VOID_RETURN;
}

/*********************************************************************//**
Updates the user_thd field in a handle and also allocates a new InnoDB
transaction handle if needed, and updates the transaction fields in the
m_prebuilt struct. */

void
ha_innobase::update_thd()
/*=====================*/
{
	THD*	thd = ha_thd();

	ut_ad(EQ_CURRENT_THD(thd));
	update_thd(thd);
}

/*********************************************************************//**
Registers an InnoDB transaction with the MySQL 2PC coordinator, so that
the MySQL XA code knows to call the InnoDB prepare and commit, or rollback
for the transaction. This MUST be called for every transaction for which
the user may call commit or rollback. Calling this several times to register
the same transaction is allowed, too. This function also registers the
current SQL statement. */
static inline
void
innobase_register_trx(
/*==================*/
	handlerton*	hton,	/* in: Innobase handlerton */
	THD*		thd,	/* in: MySQL thd (connection) object */
	trx_t*		trx)	/* in: transaction to register */
{
  ut_ad(!trx->active_commit_ordered);
  const trx_id_t trx_id= trx->id;

  trans_register_ha(thd, false, hton, trx_id);

  if (!trx->is_registered)
  {
    trx->is_registered= true;
    if (thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN))
      trans_register_ha(thd, true, hton, trx_id);
  }
}

/*	BACKGROUND INFO: HOW THE MYSQL QUERY CACHE WORKS WITH INNODB
	------------------------------------------------------------

1) The use of the query cache for TBL is disabled when there is an
uncommitted change to TBL.

2) When a change to TBL commits, InnoDB stores the current value of
its global trx id counter, let us denote it by INV_TRX_ID, to the table object
in the InnoDB data dictionary, and does only allow such transactions whose
id <= INV_TRX_ID to use the query cache.

3) When InnoDB does an INSERT/DELETE/UPDATE to a table TBL, or an implicit
modification because an ON DELETE CASCADE, we invalidate the MySQL query cache
of TBL immediately.

How this is implemented inside InnoDB:

1) Since every modification always sets an IX type table lock on the InnoDB
table, it is easy to check if there can be uncommitted modifications for a
table: just check if there are locks in the lock list of the table.

2) When a transaction inside InnoDB commits, it reads the global trx id
counter and stores the value INV_TRX_ID to the tables on which it had a lock.

3) If there is an implicit table change from ON DELETE CASCADE or SET NULL,
InnoDB calls an invalidate method for the MySQL query cache for that table.

How this is implemented inside sql_cache.cc:

1) The query cache for an InnoDB table TBL is invalidated immediately at an
INSERT/UPDATE/DELETE, just like in the case of MyISAM. No need to delay
invalidation to the transaction commit.

2) To store or retrieve a value from the query cache of an InnoDB table TBL,
any query must first ask InnoDB's permission. We must pass the thd as a
parameter because InnoDB will look at the trx id, if any, associated with
that thd. Also the full_name which is used as key to search for the table
object. The full_name is a string containing the normalized path to the
table in the canonical format.

3) Use of the query cache for InnoDB tables is now allowed also when
AUTOCOMMIT==0 or we are inside BEGIN ... COMMIT. Thus transactions no longer
put restrictions on the use of the query cache.
*/

/** Check if mysql can allow the transaction to read from/store to
the query cache.
@param[in]	table	table object
@param[in]	trx	transaction object
@return whether the storing or retrieving from the query cache is permitted */
TRANSACTIONAL_TARGET
static bool innobase_query_caching_table_check_low(
	dict_table_t* table, trx_t* trx)
{
	/* The following conditions will decide the query cache
	retrieval or storing into:

	(1) There should not be any locks on the table.
	(2) Some other trx shouldn't invalidate the cache before this
	transaction started.
	(3) Read view shouldn't exist. If exists then the view
	low_limit_id should be greater than or equal to the transaction that
	invalidates the cache for the particular table.

	For read-only transaction: should satisfy (1) and (3)
	For read-write transaction: should satisfy (1), (2), (3) */

	const trx_id_t inv = table->query_cache_inv_trx_id;

	if (trx->id && trx->id < inv) {
		return false;
	}

	if (trx->read_view.is_open() && trx->read_view.low_limit_id() < inv) {
		return false;
	}

#if !defined NO_ELISION && !defined SUX_LOCK_GENERIC
	if (xbegin()) {
		if (table->lock_mutex_is_locked())
			xabort();
		auto len = UT_LIST_GET_LEN(table->locks);
		xend();
		return len == 0;
	}
#endif

	table->lock_shared_lock();
	auto len= UT_LIST_GET_LEN(table->locks);
	table->lock_shared_unlock();
	return len == 0;
}

/** Checks if MySQL at the moment is allowed for this table to retrieve a
consistent read result, or store it to the query cache.
@param[in,out]	trx		transaction
@param[in]	norm_name	concatenation of database name,
				'/' char, table name
@return whether storing or retrieving from the query cache is permitted */
static bool innobase_query_caching_table_check(
	trx_t*		trx,
	const char*	norm_name)
{
	dict_table_t*   table = dict_table_open_on_name(
		norm_name, false, DICT_ERR_IGNORE_FK_NOKEY);

	if (table == NULL) {
		return false;
	}

	/* Start the transaction if it is not started yet */
	trx_start_if_not_started(trx, false);

	bool allow = innobase_query_caching_table_check_low(table, trx);

	table->release();

	if (allow) {
		/* If the isolation level is high, assign a read view for the
		transaction if it does not yet have one */

		if (trx->isolation_level >= TRX_ISO_REPEATABLE_READ
		    && !srv_read_only_mode
		    && !trx->read_view.is_open()) {

			/* Start the transaction if it is not started yet */
			trx_start_if_not_started(trx, false);

			trx->read_view.open(trx);
		}
	}

	return allow;
}

/******************************************************************//**
The MySQL query cache uses this to check from InnoDB if the query cache at
the moment is allowed to operate on an InnoDB table. The SQL query must
be a non-locking SELECT.

The query cache is allowed to operate on certain query only if this function
returns TRUE for all tables in the query.

If thd is not in the autocommit state, this function also starts a new
transaction for thd if there is no active trx yet, and assigns a consistent
read view to it if there is no read view yet.

Why a deadlock of threads is not possible: the query cache calls this function
at the start of a SELECT processing. Then the calling thread cannot be
holding any InnoDB semaphores. The calling thread is holding the
query cache mutex, and this function will reserve the trx_sys.mutex.
@return TRUE if permitted, FALSE if not; note that the value FALSE
does not mean we should invalidate the query cache: invalidation is
called explicitly */
static
my_bool
innobase_query_caching_of_table_permitted(
/*======================================*/
	THD*	thd,		/*!< in: thd of the user who is trying to
				store a result to the query cache or
				retrieve it */
	const char* full_name,	/*!< in: normalized path to the table */
	uint	full_name_len,	/*!< in: length of the normalized path
				to the table */
	ulonglong *)
{
	char	norm_name[1000];
	trx_t*	trx = check_trx_exists(thd);

	ut_a(full_name_len < 999);

	if (trx->isolation_level == TRX_ISO_SERIALIZABLE) {
		/* In the SERIALIZABLE mode we add LOCK IN SHARE MODE to every
		plain SELECT if AUTOCOMMIT is not on. */

		return(false);
	}

	if (!thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN)
	    && trx->n_mysql_tables_in_use == 0) {
		/* We are going to retrieve the query result from the query
		cache. This cannot be a store operation to the query cache
		because then MySQL would have locks on tables already.

		TODO: if the user has used LOCK TABLES to lock the table,
		then we open a transaction in the call of row_.. below.
		That trx can stay open until UNLOCK TABLES. The same problem
		exists even if we do not use the query cache. MySQL should be
		modified so that it ALWAYS calls some cleanup function when
		the processing of a query ends!

		We can imagine we instantaneously serialize this consistent
		read trx to the current trx id counter. If trx2 would have
		changed the tables of a query result stored in the cache, and
		trx2 would have already committed, making the result obsolete,
		then trx2 would have already invalidated the cache. Thus we
		can trust the result in the cache is ok for this query. */

		return(true);
	}

	/* Normalize the table name to InnoDB format */
	normalize_table_name(norm_name, sizeof(norm_name), full_name);

	innobase_register_trx(innodb_hton_ptr, thd, trx);

	return innobase_query_caching_table_check(trx, norm_name);
}

/*****************************************************************//**
Invalidates the MySQL query cache for the table. */
void
innobase_invalidate_query_cache(
/*============================*/
	trx_t*		trx,		/*!< in: transaction which
					modifies the table */
	const char*	full_name)	/*!< in: concatenation of
					database name, path separator,
					table name, null char NUL;
					NOTE that in Windows this is
					always in LOWER CASE! */
{
	/* Note that the query cache mutex is just above the trx_sys.mutex.
	The caller of this function must not have latches of a lower rank. */

        char    qcache_key_name[2 * (NAME_LEN + 1)];
        char db_name[NAME_CHAR_LEN * MY_CS_MBMAXLEN + 1];
        const char *key_ptr;
        size_t  tabname_len;

        // Extract the database name.
        key_ptr= strchr(full_name, '/');
        DBUG_ASSERT(key_ptr != NULL); // Database name should be present
        size_t  dbname_len= size_t(key_ptr - full_name);
        memcpy(db_name, full_name, dbname_len);
        db_name[dbname_len]= '\0';

        /* Construct the key("db-name\0table$name\0") for the query cache using
        the path name("db@002dname\0table@0024name\0") of the table in its
        canonical form. */
        dbname_len = filename_to_tablename(db_name, qcache_key_name,
                                           sizeof(qcache_key_name));
        tabname_len = filename_to_tablename(++key_ptr,
                                            (qcache_key_name + dbname_len + 1),
                                            sizeof(qcache_key_name) -
                                            dbname_len - 1);

        /* Argument TRUE below means we are using transactions */
        mysql_query_cache_invalidate4(trx->mysql_thd,
                                      qcache_key_name,
                                      uint(dbname_len + tabname_len + 2),
                                      TRUE);
}

/** Quote a standard SQL identifier like index or column name.
@param[in]	file	output stream
@param[in]	trx	InnoDB transaction, or NULL
@param[in]	id	identifier to quote */
void
innobase_quote_identifier(
	FILE*		file,
	const trx_t*	trx,
	const char*	id)
{
	const int	q = trx != NULL && trx->mysql_thd != NULL
		? get_quote_char_for_identifier(trx->mysql_thd, id, strlen(id))
		: '`';

	if (q == EOF) {
		fputs(id, file);
	} else {
		putc(q, file);

		while (int c = *id++) {
			if (c == q) {
				putc(c, file);
			}
			putc(c, file);
		}

		putc(q, file);
	}
}

/** Quote a standard SQL identifier like tablespace, index or column name.
@param[in]	trx	InnoDB transaction, or NULL
@param[in]	id	identifier to quote
@return quoted identifier */
std::string
innobase_quote_identifier(
/*======================*/
	const trx_t*	trx,
	const char*	id)
{
	std::string quoted_identifier;
	const int	q = trx != NULL && trx->mysql_thd != NULL
		? get_quote_char_for_identifier(trx->mysql_thd, id, strlen(id))
		: '`';

	if (q == EOF) {
		quoted_identifier.append(id);
	} else {
		quoted_identifier += char(q);
		quoted_identifier.append(id);
		quoted_identifier += char(q);
	}

	return (quoted_identifier);
}

/** Convert a table name to the MySQL system_charset_info (UTF-8)
and quote it.
@param[out]	buf	buffer for converted identifier
@param[in]	buflen	length of buf, in bytes
@param[in]	id	identifier to convert
@param[in]	idlen	length of id, in bytes
@param[in]	thd	MySQL connection thread, or NULL
@return pointer to the end of buf */
static
char*
innobase_convert_identifier(
	char*		buf,
	ulint		buflen,
	const char*	id,
	ulint		idlen,
	THD*		thd)
{
	const char*	s	= id;

	char nz[MAX_TABLE_NAME_LEN + 1];
	char nz2[MAX_TABLE_NAME_LEN + 1];

	/* Decode the table name.  The MySQL function expects
	a NUL-terminated string.  The input and output strings
	buffers must not be shared. */
	ut_a(idlen <= MAX_TABLE_NAME_LEN);
	memcpy(nz, id, idlen);
	nz[idlen] = 0;

	s = nz2;
	idlen = explain_filename(thd, nz, nz2, sizeof nz2,
				 EXPLAIN_PARTITIONS_AS_COMMENT);
	if (idlen > buflen) {
		idlen = buflen;
	}
	memcpy(buf, s, idlen);
	return(buf + idlen);
}

/*****************************************************************//**
Convert a table name to the MySQL system_charset_info (UTF-8).
@return pointer to the end of buf */
char*
innobase_convert_name(
/*==================*/
	char*		buf,	/*!< out: buffer for converted identifier */
	ulint		buflen,	/*!< in: length of buf, in bytes */
	const char*	id,	/*!< in: table name to convert */
	ulint		idlen,	/*!< in: length of id, in bytes */
	THD*		thd)	/*!< in: MySQL connection thread, or NULL */
{
	char*		s	= buf;
	const char*	bufend	= buf + buflen;

	const char*	slash = (const char*) memchr(id, '/', idlen);

	if (slash == NULL) {
		return(innobase_convert_identifier(
				buf, buflen, id, idlen, thd));
	}

	/* Print the database name and table name separately. */
	s = innobase_convert_identifier(s, ulint(bufend - s),
					id, ulint(slash - id), thd);
	if (s < bufend) {
		*s++ = '.';
		s = innobase_convert_identifier(s, ulint(bufend - s),
						slash + 1, idlen
						- ulint(slash - id) - 1,
						thd);
	}

	return(s);
}

/*****************************************************************//**
A wrapper function of innobase_convert_name(), convert a table name
to the MySQL system_charset_info (UTF-8) and quote it if needed.
@return pointer to the end of buf */
void
innobase_format_name(
/*==================*/
	char*		buf,	/*!< out: buffer for converted identifier */
	ulint		buflen,	/*!< in: length of buf, in bytes */
	const char*	name)	/*!< in: table name to format */
{
	char*     bufend;

	bufend = innobase_convert_name(buf, buflen, name, strlen(name), NULL);

	ut_ad((ulint) (bufend - buf) < buflen);

	*bufend = '\0';
}

/**********************************************************************//**
Determines if the currently running transaction has been interrupted.
@return true if interrupted */
bool
trx_is_interrupted(
/*===============*/
	const trx_t*	trx)	/*!< in: transaction */
{
	return(trx && trx->mysql_thd && thd_kill_level(trx->mysql_thd));
}

/**************************************************************//**
Resets some fields of a m_prebuilt struct. The template is used in fast
retrieval of just those column values MySQL needs in its processing. */
void
ha_innobase::reset_template(void)
/*=============================*/
{
	ut_ad(m_prebuilt->magic_n == ROW_PREBUILT_ALLOCATED);
	ut_ad(m_prebuilt->magic_n2 == m_prebuilt->magic_n);

	/* Force table to be freed in close_thread_table(). */
	DBUG_EXECUTE_IF("free_table_in_fts_query",
		if (m_prebuilt->in_fts_query) {
                  table->mark_table_for_reopen();
		}
	);

	m_prebuilt->keep_other_fields_on_keyread = false;
	m_prebuilt->read_just_key = 0;
	m_prebuilt->in_fts_query = 0;

	/* Reset index condition pushdown state. */
	if (m_prebuilt->idx_cond) {
		m_prebuilt->idx_cond = NULL;
		m_prebuilt->idx_cond_n_cols = 0;
		/* Invalidate m_prebuilt->mysql_template
		in ha_innobase::write_row(). */
		m_prebuilt->template_type = ROW_MYSQL_NO_TEMPLATE;
	}
	if (m_prebuilt->pk_filter) {
		m_prebuilt->pk_filter = NULL;
		m_prebuilt->template_type = ROW_MYSQL_NO_TEMPLATE;
	}
}

/*****************************************************************//**
Call this when you have opened a new table handle in HANDLER, before you
call index_read_map() etc. Actually, we can let the cursor stay open even
over a transaction commit! Then you should call this before every operation,
fetch next etc. This function inits the necessary things even after a
transaction commit. */

void
ha_innobase::init_table_handle_for_HANDLER(void)
/*============================================*/
{
	/* If current thd does not yet have a trx struct, create one.
	If the current handle does not yet have a m_prebuilt struct, create
	one. Update the trx pointers in the m_prebuilt struct. Normally
	this operation is done in external_lock. */

	update_thd(ha_thd());

	/* Initialize the m_prebuilt struct much like it would be inited in
	external_lock */

	/* If the transaction is not started yet, start it */

	trx_start_if_not_started_xa(m_prebuilt->trx, false);

	/* Assign a read view if the transaction does not have it yet */

	m_prebuilt->trx->read_view.open(m_prebuilt->trx);

	innobase_register_trx(innodb_hton_ptr, m_user_thd, m_prebuilt->trx);

	/* We did the necessary inits in this function, no need to repeat them
	in row_search_mvcc() */

	m_prebuilt->sql_stat_start = FALSE;

	/* We let HANDLER always to do the reads as consistent reads, even
	if the trx isolation level would have been specified as SERIALIZABLE */

	m_prebuilt->select_lock_type = LOCK_NONE;
	m_prebuilt->stored_select_lock_type = LOCK_NONE;

	/* Always fetch all columns in the index record */

	m_prebuilt->hint_need_to_fetch_extra_cols = ROW_RETRIEVE_ALL_COLS;

	/* We want always to fetch all columns in the whole row? Or do
	we???? */

	m_prebuilt->used_in_HANDLER = TRUE;

	reset_template();
	m_prebuilt->trx->bulk_insert &= TRX_DDL_BULK;
}

/*********************************************************************//**
Free any resources that were allocated and return failure.
@return always return 1 */
static int innodb_init_abort()
{
	DBUG_ENTER("innodb_init_abort");

	if (fil_system.temp_space) {
		fil_system.temp_space->close();
	}

	srv_sys_space.shutdown();
	if (srv_tmp_space.get_sanity_check_status()) {
		srv_tmp_space.delete_files();
	}
	srv_tmp_space.shutdown();

	DBUG_RETURN(1);
}

static void innodb_buffer_pool_size_update(THD* thd,st_mysql_sys_var*,void*,
                                           const void *save) noexcept
{
  buf_pool.resize(*static_cast<const size_t*>(save), thd);
}

static MYSQL_SYSVAR_SIZE_T(buffer_pool_size, buf_pool.size_in_bytes_requested,
  PLUGIN_VAR_RQCMDARG,
  "The size of the memory buffer InnoDB uses to cache data"
  " and indexes of its tables",
  nullptr, innodb_buffer_pool_size_update, 128U << 20, 2U << 20,
  size_t(-ssize_t(innodb_buffer_pool_extent_size)), 1U << 20);

#if defined __linux__ || !defined DBUG_OFF
static void innodb_buffer_pool_size_auto_min_update(THD*,st_mysql_sys_var*,
                                                    void*, const void *save)
  noexcept
{
  mysql_mutex_lock(&buf_pool.mutex);
  buf_pool.size_in_bytes_auto_min= *static_cast<const size_t*>(save);
  mysql_mutex_unlock(&buf_pool.mutex);
}

static MYSQL_SYSVAR_SIZE_T(buffer_pool_size_auto_min,
                           buf_pool.size_in_bytes_auto_min,
  PLUGIN_VAR_RQCMDARG,
  "Minimum innodb_buffer_pool_size for dynamic shrinking on memory pressure",
  nullptr, innodb_buffer_pool_size_auto_min_update, 0, 0,
  size_t(-ssize_t(innodb_buffer_pool_extent_size)),
  innodb_buffer_pool_extent_size);
#endif

static MYSQL_SYSVAR_SIZE_T(buffer_pool_size_max, buf_pool.size_in_bytes_max,
                           PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                           "Maximum innodb_buffer_pool_size",
                           nullptr, nullptr, 0, 0,
                           size_t(-ssize_t(innodb_buffer_pool_extent_size)),
                           innodb_buffer_pool_extent_size);

static MYSQL_SYSVAR_UINT(log_write_ahead_size, log_sys.write_size,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Redo log write size to avoid read-on-write; must be a power of two",
  nullptr, nullptr, 512, 512, 4096, 1);

/****************************************************************//**
Gives the file extension of an InnoDB single-table tablespace. */
static const char* ha_innobase_exts[] = {
	dot_ext[IBD],
	dot_ext[ISL],
	NullS
};

/** Determine if system-versioned data was modified by the transaction.
@param[in,out]	thd	current session
@param[out]	trx_id	transaction start ID
@return	transaction commit ID
@retval	0	if no system-versioned data was affected by the transaction */
static ulonglong innodb_prepare_commit_versioned(THD* thd, ulonglong *trx_id)
{
  if (trx_t *trx= thd_to_trx(thd))
  {
    *trx_id= trx->id;
    bool versioned= false;

    for (auto &t : trx->mod_tables)
    {
      if (t.second.is_versioned())
      {
        DBUG_ASSERT(t.first->versioned_by_id());
        DBUG_ASSERT(trx->rsegs.m_redo.rseg);
        versioned= true;
        if (!trx->bulk_insert)
          break;
      }
      if (t.second.is_bulk_insert())
      {
        ut_ad(trx->bulk_insert);
        if (t.second.write_bulk(t.first, trx))
          return ULONGLONG_MAX;
      }
    }

    return versioned ? trx_sys.get_new_trx_id() : 0;
  }

  *trx_id= 0;
  return 0;
}

static bool
compression_algorithm_is_not_loaded(ulong compression_algorithm, myf flags)
{
  bool is_loaded[PAGE_ALGORITHM_LAST+1]= { 1, 1, provider_service_lz4->is_loaded,
    provider_service_lzo->is_loaded, provider_service_lzma->is_loaded,
    provider_service_bzip2->is_loaded, provider_service_snappy->is_loaded };

  DBUG_ASSERT(compression_algorithm <= PAGE_ALGORITHM_LAST);

  if (is_loaded[compression_algorithm])
    return 0;

  my_printf_error(HA_ERR_UNSUPPORTED, "InnoDB: compression algorithm %s (%lu)"
    " is not available. Please, load the corresponding provider plugin.", flags,
    page_compression_algorithms[compression_algorithm], compression_algorithm);
  return 1;
}

/** Initialize, validate and normalize the InnoDB startup parameters.
@return failure code
@retval 0 on success
@retval HA_ERR_OUT_OF_MEM	when out of memory
@retval HA_ERR_INITIALIZATION	when some parameters are out of range */
static int innodb_init_params()
{
  DBUG_ENTER("innodb_init_params");

  srv_page_size_shift= innodb_page_size_validate(srv_page_size);
  if (!srv_page_size_shift)
  {
    sql_print_error("InnoDB: Invalid page size=%lu.\n", srv_page_size);
    DBUG_RETURN(HA_ERR_INITIALIZATION);
  }

  size_t &min= MYSQL_SYSVAR_NAME(buffer_pool_size).min_val;
  min= ut_calc_align<size_t>
    (buf_pool.blocks_in_bytes(BUF_LRU_MIN_LEN + BUF_LRU_MIN_LEN / 4),
     1U << 20);
  size_t innodb_buffer_pool_size= buf_pool.size_in_bytes_requested;

  /* With large pages, buffer pool can't grow or shrink. */
  if (!buf_pool.size_in_bytes_max || my_use_large_pages ||
      innodb_buffer_pool_size > buf_pool.size_in_bytes_max)
    buf_pool.size_in_bytes_max= ut_calc_align(innodb_buffer_pool_size,
                                              innodb_buffer_pool_extent_size);

  MYSQL_SYSVAR_NAME(buffer_pool_size).max_val= buf_pool.size_in_bytes_max;
#if defined __linux__ || !defined DBUG_OFF
  if (!buf_pool.size_in_bytes_auto_min ||
      buf_pool.size_in_bytes_auto_min > buf_pool.size_in_bytes_max)
    buf_pool.size_in_bytes_auto_min= buf_pool.size_in_bytes_max;
  MYSQL_SYSVAR_NAME(buffer_pool_size_auto_min).max_val=
    buf_pool.size_in_bytes_max;
#endif

  if (innodb_buffer_pool_size < min)
  {
     sql_print_error("InnoDB: innodb_page_size=%lu requires "
                     "innodb_buffer_pool_size >= %zu MiB current %zu MiB",
                     srv_page_size, min >> 20, innodb_buffer_pool_size >> 20);
     DBUG_RETURN(HA_ERR_INITIALIZATION);
  }

  if (!ut_is_2pow(log_sys.write_size))
  {
    sql_print_error("InnoDB: innodb_log_write_ahead_size=%u"
                    " is not a power of two",
                    log_sys.write_size);
    DBUG_RETURN(HA_ERR_INITIALIZATION);
  }

  if (compression_algorithm_is_not_loaded(innodb_compression_algorithm,
                                          ME_ERROR_LOG))
    DBUG_RETURN(HA_ERR_INITIALIZATION);

  if ((srv_encrypt_tables || srv_encrypt_log ||
       innodb_encrypt_temporary_tables) &&
      !encryption_key_id_exists(FIL_DEFAULT_ENCRYPTION_KEY))
  {
    sql_print_error("InnoDB: cannot enable encryption, "
                    "encryption plugin is not available");
    DBUG_RETURN(HA_ERR_INITIALIZATION);
  }

#ifdef _WIN32
  if (!is_filename_allowed(srv_buf_dump_filename,
                           strlen(srv_buf_dump_filename), false))
  {
    sql_print_error("InnoDB: innodb_buffer_pool_filename"
                    " cannot have colon (:) in the file name.");
    DBUG_RETURN(HA_ERR_INITIALIZATION);
  }
#endif

  /* First calculate the default path for innodb_data_home_dir etc.,
  in case the user has not given any value.

  Note that when using the embedded server, the datadirectory is not
  necessarily the current directory of this program. */

  fil_path_to_mysql_datadir =
#ifndef HAVE_REPLICATION
    mysqld_embedded ? mysql_real_data_home :
#endif
    "./";

  /* Set InnoDB initialization parameters according to the values
  read from MySQL .cnf file */

  /* The default dir for data files is the datadir of MySQL */

  srv_data_home= innobase_data_home_dir
    ? innobase_data_home_dir
    : const_cast<char*>(fil_path_to_mysql_datadir);
#ifdef WITH_WSREP
  /* If we use the wsrep API, then we need to tell the server
  the path to the data files (for passing it to the SST scripts): */
  wsrep_set_data_home_dir(srv_data_home);
#endif /* WITH_WSREP */


  /*--------------- Shared tablespaces -------------------------*/

  /* Check that the value of system variable innodb_page_size was
  set correctly.  Its value was put into srv_page_size. If valid,
  return the associated srv_page_size_shift. */

  srv_sys_space.set_space_id(TRX_SYS_SPACE);
  /* Temporary tablespace is in full crc32 format. */
  srv_tmp_space.set_flags(FSP_FLAGS_FCRC32_MASK_MARKER |
                          FSP_FLAGS_FCRC32_PAGE_SSIZE());

  switch (srv_checksum_algorithm) {
  case SRV_CHECKSUM_ALGORITHM_FULL_CRC32:
  case SRV_CHECKSUM_ALGORITHM_STRICT_FULL_CRC32:
    srv_sys_space.set_flags(srv_tmp_space.flags());
    break;
  default:
    srv_sys_space.set_flags(FSP_FLAGS_PAGE_SSIZE());
  }

  srv_sys_space.set_path(srv_data_home);

  if (!srv_sys_space.parse_params(innobase_data_file_path, true))
  {
    sql_print_error("InnoDB: Unable to parse innodb_data_file_path=%s",
                    innobase_data_file_path);
    DBUG_RETURN(HA_ERR_INITIALIZATION);
  }

  srv_tmp_space.set_path(srv_data_home);

  if (!srv_tmp_space.parse_params(innobase_temp_data_file_path, false))
  {
    sql_print_error("InnoDB: Unable to parse innodb_temp_data_file_path=%s",
                    innobase_temp_data_file_path);
    DBUG_RETURN(HA_ERR_INITIALIZATION);
  }

  /* Perform all sanity check before we take action of deleting files*/
  if (srv_sys_space.intersection(&srv_tmp_space))
  {
    sql_print_error("innodb_temporary and innodb_system"
                    " file names seem to be the same.");
    DBUG_RETURN(HA_ERR_INITIALIZATION);
  }

  srv_sys_space.normalize_size();
  srv_tmp_space.normalize_size();

  /* ------------ UNDO tablespaces files ---------------------*/
  if (!srv_undo_dir)
    srv_undo_dir= const_cast<char*>(fil_path_to_mysql_datadir);

  if (strchr(srv_undo_dir, ';'))
  {
    sql_print_error("syntax error in innodb_undo_directory");
    DBUG_RETURN(HA_ERR_INITIALIZATION);
  }

  if (!srv_log_group_home_dir)
    srv_log_group_home_dir= const_cast<char*>(fil_path_to_mysql_datadir);

  if (strchr(srv_log_group_home_dir, ';'))
  {
    sql_print_error("syntax error in innodb_log_group_home_dir");
    DBUG_RETURN(HA_ERR_INITIALIZATION);
  }

  /* Check that interdependent parameters have sane values. */
  if (srv_max_buf_pool_modified_pct < srv_max_dirty_pages_pct_lwm)
  {
    sql_print_warning("InnoDB: innodb_max_dirty_pages_pct_lwm"
                      " cannot be set higher than"
                      " innodb_max_dirty_pages_pct.\n"
                      "InnoDB: Setting"
                      " innodb_max_dirty_pages_pct_lwm to %lf\n",
                      srv_max_buf_pool_modified_pct);
    srv_max_dirty_pages_pct_lwm = srv_max_buf_pool_modified_pct;
  }

  if (srv_max_io_capacity == SRV_MAX_IO_CAPACITY_DUMMY_DEFAULT)
  {
    if (srv_io_capacity >= SRV_MAX_IO_CAPACITY_LIMIT / 2)
      /* Avoid overflow. */
      srv_max_io_capacity= SRV_MAX_IO_CAPACITY_LIMIT;
    else
      /* The user has not set the value. We should set it based on
      innodb_io_capacity. */
      srv_max_io_capacity= std::max(2 * srv_io_capacity, 2000UL);
  }
  else if (srv_max_io_capacity < srv_io_capacity)
  {
    sql_print_warning("InnoDB: innodb_io_capacity cannot be set higher than"
                      " innodb_io_capacity_max."
                      "Setting innodb_io_capacity=%lu", srv_max_io_capacity);
    srv_io_capacity= srv_max_io_capacity;
  }

  if (UNIV_PAGE_SIZE_DEF != srv_page_size)
  {
    sql_print_information("InnoDB: innodb_page_size=%lu", srv_page_size);
    srv_max_undo_log_size=
      std::max(srv_max_undo_log_size,
               ulonglong(SRV_UNDO_TABLESPACE_SIZE_IN_PAGES) <<
               srv_page_size_shift);
  }

  if (innobase_open_files < 10)
    innobase_open_files= (srv_file_per_table && tc_size > 300 &&
                          tc_size < open_files_limit)
      ? tc_size
      : 300;

  if (innobase_open_files > open_files_limit)
  {
    sql_print_warning("InnoDB: innodb_open_files %lu"
                      " should not be greater than the open_files_limit %lu",
                      innobase_open_files, open_files_limit);
    if (innobase_open_files > tc_size)
      innobase_open_files= tc_size;
  }

  const size_t min_open_files_limit= srv_undo_tablespaces +
    srv_sys_space.m_files.size() + srv_tmp_space.m_files.size() + 1;
  if (min_open_files_limit > innobase_open_files)
  {
    sql_print_warning("InnoDB: innodb_open_files=%lu is not greater "
                      "than the number of system tablespace files, "
                      "temporary tablespace files, "
                      "innodb_undo_tablespaces=%u; adjusting "
                      "to innodb_open_files=%zu",
                      innobase_open_files, srv_undo_tablespaces,
                      min_open_files_limit);
    innobase_open_files= ulong(min_open_files_limit);
  }

  srv_max_n_open_files= innobase_open_files;
  srv_innodb_status = (ibool) innobase_create_status_file;

  srv_print_verbose_log= !mysqld_embedded;

  if (!ut_is_2pow(fts_sort_pll_degree))
  {
    ulong n;
    for (n= 1; n < fts_sort_pll_degree; n<<= 1) {}
    fts_sort_pll_degree= n;
  }

  if (innodb_flush_method == 1 /* O_DSYNC */)
  {
    log_sys.log_write_through= true;
    fil_system.write_through= true;
    fil_system.buffered= false;
#if defined __linux__ || defined _WIN32
    log_sys.log_buffered= false;
    goto skip_buffering_tweak;
#endif
  }
  else if (innodb_flush_method >= 4 /* O_DIRECT */ &&
           IF_WIN(innodb_flush_method < 8 /* normal */, true))
  {
    /* O_DIRECT and similar settings do nothing */
    if (innodb_flush_method == 5 /* O_DIRECT_NO_FSYNC */ && buf_dblwr.use)
      buf_dblwr.use= buf_dblwr.USE_FAST;
  }
#ifdef O_DIRECT
  else if (srv_use_atomic_writes && my_may_have_atomic_write)
    /* If atomic writes are enabled, do the same as with
    innodb_flush_method=O_DIRECT: retain the default settings */;
#endif
  else
  {
    log_sys.log_write_through= false;
    fil_system.write_through= false;
    fil_system.buffered= true;
  }

#if defined __linux__ || defined _WIN32
  if (srv_flush_log_at_trx_commit == 2)
    /* Do not disable the file system cache if
    innodb_flush_log_at_trx_commit=2. */
    log_sys.log_buffered= true;
skip_buffering_tweak:
#endif

#if !defined LINUX_NATIVE_AIO && !defined HAVE_URING && !defined _WIN32
  /* Currently native AIO is supported only on windows and linux
  and that also when the support is compiled in. In all other
  cases, we ignore the setting of innodb_use_native_aio. */
  srv_use_native_aio= FALSE;
#endif

  DBUG_RETURN(0);
}


/*********************************************************************//**
Setup costs factors for InnoDB to be able to approximate how many
ms different opperations takes. See cost functions in handler.h how
the different variables are used */

static void innobase_update_optimizer_costs(OPTIMIZER_COSTS *costs)
{
  /*
    The following number was found by check_costs.pl when using 1M rows
    and all rows are cached. See optimizer_costs.txt for details
  */
  costs->row_next_find_cost= 0.00007013;
  costs->row_lookup_cost=    0.00076597;
  costs->key_next_find_cost= 0.00009900;
  costs->key_lookup_cost=    0.00079112;
  costs->row_copy_cost=      0.00006087;
}


/** Initialize the InnoDB storage engine plugin.
@param[in,out]	p	InnoDB handlerton
@return error code
@retval 0 on success */
static int innodb_init(void* p)
{
	DBUG_ENTER("innodb_init");
	handlerton* innobase_hton= static_cast<handlerton*>(p);
	innodb_hton_ptr = innobase_hton;

	innobase_hton->db_type = DB_TYPE_INNODB;
	innobase_hton->savepoint_offset = sizeof(undo_no_t);
	innobase_hton->close_connection = innobase_close_connection;
	innobase_hton->kill_query = innobase_kill_query;
	innobase_hton->savepoint_set = innobase_savepoint;
	innobase_hton->savepoint_rollback = innobase_rollback_to_savepoint;

	innobase_hton->savepoint_rollback_can_release_mdl =
				innobase_rollback_to_savepoint_can_release_mdl;

	innobase_hton->savepoint_release = innobase_release_savepoint;
	innobase_hton->prepare_ordered= NULL;
	innobase_hton->commit_ordered= innobase_commit_ordered;
	innobase_hton->commit = innobase_commit;
	innobase_hton->rollback = innobase_rollback;
	innobase_hton->prepare = innobase_xa_prepare;
	innobase_hton->recover = innobase_xa_recover;
	innobase_hton->commit_by_xid = innobase_commit_by_xid;
	innobase_hton->rollback_by_xid = innobase_rollback_by_xid;
#ifndef EMBEDDED_LIBRARY
	innobase_hton->recover_rollback_by_xid =
		innobase_recover_rollback_by_xid;
	innobase_hton->signal_tc_log_recovery_done = innobase_tc_log_recovery_done;
#endif
	innobase_hton->commit_checkpoint_request = innodb_log_flush_request;
	innobase_hton->create = innobase_create_handler;

	innobase_hton->drop_database = innodb_drop_database;
	innobase_hton->panic = innobase_end;
	innobase_hton->pre_shutdown = innodb_preshutdown;

	innobase_hton->start_consistent_snapshot =
		innobase_start_trx_and_assign_read_view;

	innobase_hton->flush_logs = innobase_flush_logs;
	innobase_hton->show_status = innobase_show_status;
	innobase_hton->notify_tabledef_changed= innodb_notify_tabledef_changed;
	innobase_hton->flags =
		HTON_SUPPORTS_EXTENDED_KEYS | HTON_SUPPORTS_FOREIGN_KEYS |
		HTON_NATIVE_SYS_VERSIONING |
		HTON_WSREP_REPLICATION |
		HTON_REQUIRES_CLOSE_AFTER_TRUNCATE |
		HTON_TRUNCATE_REQUIRES_EXCLUSIVE_USE |
		HTON_REQUIRES_NOTIFY_TABLEDEF_CHANGED_AFTER_COMMIT;

#ifdef WITH_WSREP
	innobase_hton->abort_transaction=wsrep_abort_transaction;
	innobase_hton->set_checkpoint=innobase_wsrep_set_checkpoint;
	innobase_hton->get_checkpoint=innobase_wsrep_get_checkpoint;
	innobase_hton->disable_internal_writes=innodb_disable_internal_writes;
#endif /* WITH_WSREP */

	innobase_hton->check_version = innodb_check_version;
	innobase_hton->signal_ddl_recovery_done = innodb_ddl_recovery_done;

	innobase_hton->tablefile_extensions = ha_innobase_exts;
	innobase_hton->table_options = innodb_table_option_list;

	/* System Versioning */
	innobase_hton->prepare_commit_versioned
		= innodb_prepare_commit_versioned;

        innobase_hton->update_optimizer_costs= innobase_update_optimizer_costs;

	innodb_remember_check_sysvar_funcs();

	compile_time_assert(DATA_MYSQL_TRUE_VARCHAR == MYSQL_TYPE_VARCHAR);

#ifndef DBUG_OFF
	static const char	test_filename[] = "-@";
	char			test_tablename[sizeof test_filename
				+ sizeof(srv_mysql50_table_name_prefix) - 1];
	DBUG_ASSERT(sizeof test_tablename - 1
		    == filename_to_tablename(test_filename,
					     test_tablename,
					     sizeof test_tablename, true));
	DBUG_ASSERT(!strncmp(test_tablename,
			     srv_mysql50_table_name_prefix,
			     sizeof srv_mysql50_table_name_prefix - 1));
	DBUG_ASSERT(!strcmp(test_tablename
			    + sizeof srv_mysql50_table_name_prefix - 1,
			    test_filename));
#endif /* DBUG_OFF */

	/* Setup the memory alloc/free tracing mechanisms before calling
	any functions that could possibly allocate memory. */
	ut_new_boot();

	if (int error = innodb_init_params()) {
		DBUG_RETURN(error);
	}

	/* After this point, error handling has to use
	innodb_init_abort(). */

#ifdef HAVE_PSI_INTERFACE
	/* Register keys with MySQL performance schema */
	int	count;

# ifdef UNIV_PFS_MUTEX
	count = array_elements(all_innodb_mutexes);
	mysql_mutex_register("innodb", all_innodb_mutexes, count);
# endif /* UNIV_PFS_MUTEX */

# ifdef UNIV_PFS_RWLOCK
	count = array_elements(all_innodb_rwlocks);
	mysql_rwlock_register("innodb", all_innodb_rwlocks, count);
# endif /* UNIV_PFS_MUTEX */

# ifdef UNIV_PFS_THREAD
	count = array_elements(all_innodb_threads);
	mysql_thread_register("innodb", all_innodb_threads, count);
# endif /* UNIV_PFS_THREAD */

# ifdef UNIV_PFS_IO
	count = array_elements(all_innodb_files);
	mysql_file_register("innodb", all_innodb_files, count);
# endif /* UNIV_PFS_IO */
#endif /* HAVE_PSI_INTERFACE */

	bool	create_new_db = false;

	/* Check whether the data files exist. */
	dberr_t	err = srv_sys_space.check_file_spec(&create_new_db, 5U << 20);

	if (err != DB_SUCCESS) {
		DBUG_RETURN(innodb_init_abort());
	}

	err = srv_start(create_new_db);

	if (err != DB_SUCCESS) {
		innodb_shutdown();
		DBUG_RETURN(innodb_init_abort());
	}

	srv_was_started = true;
	innodb_params_adjust();

	innobase_old_blocks_pct = buf_LRU_old_ratio_update(
		innobase_old_blocks_pct, true);

	mysql_mutex_init(pending_checkpoint_mutex_key,
			 &log_requests.mutex,
			 MY_MUTEX_INIT_FAST);
#ifdef MYSQL_DYNAMIC_PLUGIN
	if (innobase_hton != p) {
		innobase_hton = reinterpret_cast<handlerton*>(p);
		*innobase_hton = *innodb_hton_ptr;
	}
#endif /* MYSQL_DYNAMIC_PLUGIN */

	memset(innodb_counter_value, 0, sizeof innodb_counter_value);

	/* Do this as late as possible so server is fully starts up,
	since  we might get some initial stats if user choose to turn
	on some counters from start up */
	if (innobase_enable_monitor_counter) {
		innodb_enable_monitor_at_startup(
			innobase_enable_monitor_counter);
	}

	/* Turn on monitor counters that are default on */
	srv_mon_default_on();

	/* Unit Tests */
#ifdef UNIV_ENABLE_UNIT_TEST_GET_PARENT_DIR
	unit_test_os_file_get_parent_dir();
#endif /* UNIV_ENABLE_UNIT_TEST_GET_PARENT_DIR */

#ifdef UNIV_ENABLE_UNIT_TEST_MAKE_FILEPATH
	test_make_filepath();
#endif /*UNIV_ENABLE_UNIT_TEST_MAKE_FILEPATH */

#ifdef UNIV_ENABLE_DICT_STATS_TEST
	test_dict_stats_all();
#endif /*UNIV_ENABLE_DICT_STATS_TEST */

#ifdef UNIV_ENABLE_UNIT_TEST_ROW_RAW_FORMAT_INT
# ifdef HAVE_UT_CHRONO_T
	test_row_raw_format_int();
# endif /* HAVE_UT_CHRONO_T */
#endif /* UNIV_ENABLE_UNIT_TEST_ROW_RAW_FORMAT_INT */

	DBUG_RETURN(0);
}

/** Shut down the InnoDB storage engine.
@return	0 */
static
int
innobase_end(handlerton*, ha_panic_function)
{
	DBUG_ENTER("innobase_end");

	if (srv_was_started) {
		THD *thd= current_thd;
		if (thd) { // may be UNINSTALL PLUGIN statement
		 	if (trx_t* trx = thd_to_trx(thd)) {
				trx->free();
		 	}
		}

		innodb_shutdown();
		mysql_mutex_destroy(&log_requests.mutex);
	}

	DBUG_RETURN(0);
}

/*****************************************************************//**
Commits a transaction in an InnoDB database. */
void
innobase_commit_low(
/*================*/
	trx_t*	trx)	/*!< in: transaction handle */
{
#ifdef WITH_WSREP
	const char* tmp = 0;
	const bool is_wsrep = trx->is_wsrep();
	if (is_wsrep) {
		tmp = thd_proc_info(trx->mysql_thd, "innobase_commit_low()");
	}
#endif /* WITH_WSREP */
	trx_commit_for_mysql(trx);
#ifdef WITH_WSREP
	if (is_wsrep) {
		thd_proc_info(trx->mysql_thd, tmp);
	}
#endif /* WITH_WSREP */
}

/*****************************************************************//**
Creates an InnoDB transaction struct for the thd if it does not yet have one.
Starts a new InnoDB transaction if a transaction is not yet started. And
assigns a new snapshot for a consistent read if the transaction does not yet
have one.
@return 0 */
static
int
innobase_start_trx_and_assign_read_view(
/*====================================*/
	THD*		thd)	/*!< in: MySQL thread handle of the user for
				whom the transaction should be committed */
{
	DBUG_ENTER("innobase_start_trx_and_assign_read_view");

	/* Create a new trx struct for thd, if it does not yet have one */

	trx_t*	trx = check_trx_exists(thd);

	/* The transaction should not be active yet, start it */

	ut_ad(!trx->is_started());

	trx_start_if_not_started_xa(trx, false);

	/* Assign a read view if the transaction does not have it yet.
	Do this only if transaction is using REPEATABLE READ isolation
	level. */
	trx->isolation_level = innobase_map_isolation_level(
		thd_get_trx_isolation(thd)) & 3;

	if (trx->isolation_level == TRX_ISO_REPEATABLE_READ) {
		trx->read_view.open(trx);
	} else {
		push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
				    HA_ERR_UNSUPPORTED,
				    "InnoDB: WITH CONSISTENT SNAPSHOT"
				    " was ignored because this phrase"
				    " can only be used with"
				    " REPEATABLE READ isolation level.");
	}

	/* Set the MySQL flag to mark that there is an active transaction */

	innobase_register_trx(innodb_hton_ptr, thd, trx);

	DBUG_RETURN(0);
}

static
void
innobase_commit_ordered_2(
/*======================*/
	trx_t*	trx, 	/*!< in: Innodb transaction */
	THD*	thd)	/*!< in: MySQL thread handle */
{
	DBUG_ENTER("innobase_commit_ordered_2");

	if (trx->id) {
		/* The following call reads the binary log position of
		the transaction being committed.

		Binary logging of other engines is not relevant to
		InnoDB as all InnoDB requires is that committing
		InnoDB transactions appear in the same order in the
		MySQL binary log as they appear in InnoDB logs, which
		is guaranteed by the server.

		If the binary log is not enabled, or the transaction
		is not written to the binary log, the file name will
		be a NULL pointer. */
		thd_binlog_pos(thd, &trx->mysql_log_file_name,
			       &trx->mysql_log_offset);

		/* Don't do write + flush right now. For group commit
		to work we want to do the flush later. */
		trx->flush_log_later = true;
	}

#ifdef WITH_WSREP
	/* If the transaction is not run in 2pc, we must assign wsrep
	XID here in order to get it written in rollback segment. */
	if (trx->is_wsrep()) {
		thd_get_xid(thd, &reinterpret_cast<MYSQL_XID&>(trx->xid));
	}
#endif /* WITH_WSREP */

	innobase_commit_low(trx);
	trx->mysql_log_file_name = NULL;
	trx->flush_log_later = false;

	DBUG_VOID_RETURN;
}

/*****************************************************************//**
Perform the first, fast part of InnoDB commit.

Doing it in this call ensures that we get the same commit order here
as in binlog and any other participating transactional storage engines.

Note that we want to do as little as really needed here, as we run
under a global mutex. The expensive fsync() is done later, in
innobase_commit(), without a lock so group commit can take place.

Note also that this method can be called from a different thread than
the one handling the rest of the transaction. */
static
void
innobase_commit_ordered(
/*====================*/
	THD*	thd,	/*!< in: MySQL thread handle of the user for whom
			the transaction should be committed */
	bool	all)	/*!< in:	TRUE - commit transaction
				FALSE - the current SQL statement ended */
{
	trx_t*		trx;
	DBUG_ENTER("innobase_commit_ordered");

	trx = check_trx_exists(thd);

	if (!trx_is_registered_for_2pc(trx) && trx->is_started()) {
		/* We cannot throw error here; instead we will catch this error
		again in innobase_commit() and report it from there. */
		DBUG_VOID_RETURN;
	}

	/* commit_ordered is only called when committing the whole transaction
	(or an SQL statement when autocommit is on). */
	DBUG_ASSERT(all ||
		(!thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN)));

	innobase_commit_ordered_2(trx, thd);
	trx->active_commit_ordered = true;

	DBUG_VOID_RETURN;
}

/** Mark the end of a statement.
@param trx transaction
@return whether an error occurred */
static bool end_of_statement(trx_t *trx) noexcept
{
  ut_d(const trx_state_t trx_state{trx->state});
  ut_ad(trx_state == TRX_STATE_ACTIVE || trx_state == TRX_STATE_NOT_STARTED);

  if (trx->fts_trx)
    fts_savepoint_laststmt_refresh(trx);
  if (trx->is_bulk_insert())
  {
    /* Allow a subsequent INSERT into an empty table
    if !unique_checks && !foreign_key_checks. */

    /* MDEV-25036 FIXME: we support buffered insert only for the first
    insert statement */
    trx->error_state= trx->bulk_insert_apply();
  }
  else
  {
    trx->last_stmt_start= trx->undo_no;
    trx->end_bulk_insert();
  }

  if (UNIV_LIKELY(trx->error_state == DB_SUCCESS))
    return false;

  undo_no_t savept= 0;
  trx->rollback(&savept);
  /* MariaDB will roll back the entire transaction. */
  trx->bulk_insert&= TRX_DDL_BULK;
  trx->last_stmt_start= 0;
  return true;
}

/*****************************************************************//**
Commits a transaction in an InnoDB database or marks an SQL statement
ended.
@return 0 or deadlock error if the transaction was aborted by another
	higher priority transaction. */
static
int
innobase_commit(
/*============*/
	THD*		thd,		/*!< in: MySQL thread handle of the
					user for whom the transaction should
					be committed */
	bool		commit_trx)	/*!< in: true - commit transaction
					false - the current SQL statement
					ended */
{
	DBUG_ENTER("innobase_commit");
	DBUG_PRINT("enter", ("commit_trx: %d", commit_trx));
	DBUG_PRINT("trans", ("ending transaction"));

	trx_t*	trx = check_trx_exists(thd);

	ut_ad(!trx->dict_operation_lock_mode);
	ut_ad(!trx->dict_operation);

	switch (UNIV_EXPECT(trx->state, TRX_STATE_ACTIVE)) {
	case TRX_STATE_ABORTED:
		trx->state = TRX_STATE_NOT_STARTED;
		/* fall through */
	case TRX_STATE_NOT_STARTED:
		break;
	default:
	case TRX_STATE_COMMITTED_IN_MEMORY:
	case TRX_STATE_PREPARED_RECOVERED:
		ut_ad("invalid state" == 0);
		/* fall through */
	case TRX_STATE_PREPARED:
		ut_ad(commit_trx ||
		      !thd_test_options(thd, OPTION_NOT_AUTOCOMMIT
				       | OPTION_BEGIN));
		/* fall through */
	case TRX_STATE_ACTIVE:
		/* Transaction is deregistered only in a commit or a
		rollback. If it is deregistered we know there cannot
		be resources to be freed and we could return
		immediately.  For the time being, we play safe and do
		the cleanup though there should be nothing to clean
		up. */
		if (!trx_is_registered_for_2pc(trx)) {
			sql_print_error("Transaction not registered"
					" for MariaDB 2PC,"
					" but transaction is active");
		}
	}

	if (commit_trx
	    || (!thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN))) {

		/* Run the fast part of commit if we did not already. */
		if (!trx->active_commit_ordered) {
			innobase_commit_ordered_2(trx, thd);

		}

		/* We were instructed to commit the whole transaction, or
		this is an SQL statement end and autocommit is on */

		/* At this point commit order is fixed and transaction is
		visible to others. So we can wakeup other commits waiting for
		this one, to allow then to group commit with us. */
		thd_wakeup_subsequent_commits(thd, 0);

		/* Now do a write + flush of logs. */
		trx_commit_complete_for_mysql(trx);

		trx_deregister_from_2pc(trx);
	} else {
		/* We just mark the SQL statement ended and do not do a
		transaction commit */
		lock_unlock_table_autoinc(trx);
		if (UNIV_UNLIKELY(end_of_statement(trx))) {
			DBUG_RETURN(1);
		}
	}

	/* Reset the number AUTO-INC rows required */
	trx->n_autoinc_rows = 0;

	/* This is a statement level variable. */
	trx->fts_next_doc_id = 0;

	DBUG_RETURN(0);
}

/*****************************************************************//**
Rolls back a transaction or the latest SQL statement.
@return 0 or error number */
static
int
innobase_rollback(
/*==============*/
	THD*		thd,		/*!< in: handle to the MySQL thread
					of the user whose transaction should
					be rolled back */
	bool		rollback_trx)	/*!< in: TRUE - rollback entire
					transaction FALSE - rollback the current
					statement only */
{
  DBUG_ENTER("innobase_rollback");
  DBUG_PRINT("trans", ("aborting transaction"));

  if (!rollback_trx)
    rollback_trx= !thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN);

  trx_t *trx= check_trx_exists(thd);

  ut_ad(trx->mysql_thd == thd);
  ut_ad(!trx->is_recovered);
  ut_ad(!trx->dict_operation_lock_mode);
  ut_ad(!trx->dict_operation);

  /* Reset the number AUTO-INC rows required */
  trx->n_autoinc_rows= 0;
  /* This is a statement level variable. */
  trx->fts_next_doc_id= 0;

  const trx_state_t trx_state{trx->state};
  switch (UNIV_EXPECT(trx_state, TRX_STATE_ACTIVE)) {
  case TRX_STATE_ABORTED:
    if (rollback_trx)
      trx->state= TRX_STATE_NOT_STARTED;
    /* fall through */
  case TRX_STATE_NOT_STARTED:
    ut_ad(!trx->id);
    trx->will_lock= false;
    if (rollback_trx)
      trx_deregister_from_2pc(trx);
    DBUG_RETURN(0);
  default:
  case TRX_STATE_COMMITTED_IN_MEMORY:
  case TRX_STATE_PREPARED_RECOVERED:
    ut_ad("invalid state" == 0);
    /* fall through */
  case TRX_STATE_PREPARED:
    ut_ad(rollback_trx);
    /* fall through */
  case TRX_STATE_ACTIVE:
    /* If we had reserved the auto-inc lock for some table (if
    we come here to roll back the latest SQL statement) we
    release it now before a possibly lengthy rollback */
    lock_unlock_table_autoinc(trx);

#ifdef WITH_WSREP
    /* If trx was assigned wsrep XID in prepare phase and the
    trx is being rolled back due to BF abort, clear XID in order
    to avoid writing it to rollback segment out of order. The XID
    will be reassigned when the transaction is replayed. */
    if (rollback_trx || wsrep_is_wsrep_xid(&trx->xid))
      trx->xid.null();
#endif /* WITH_WSREP */
    dberr_t error;
    if (rollback_trx)
    {
      error= trx_rollback_for_mysql(trx);
      trx_deregister_from_2pc(trx);
    }
    else
    {
      ut_a(trx_state == TRX_STATE_ACTIVE);
      ut_ad(!trx->is_autocommit_non_locking() || trx->read_only);
      error= trx->rollback(&trx->last_stmt_start);
      if (trx->fts_trx)
      {
        fts_savepoint_rollback_last_stmt(trx);
        fts_savepoint_laststmt_refresh(trx);
      }
      trx->last_stmt_start= trx->undo_no;
      trx->end_bulk_insert();
    }
    DBUG_RETURN(convert_error_code_to_mysql(error, 0, trx->mysql_thd));
  }
}

/** Invoke commit_checkpoint_notify_ha() on completed log flush requests.
@param pending  log_requests.start
@param lsn      log_sys.get_flushed_lsn() */
static void log_flush_notify_and_unlock(log_flush_request *pending, lsn_t lsn)
{
  mysql_mutex_assert_owner(&log_requests.mutex);
  ut_ad(pending == log_requests.start.load(std::memory_order_relaxed));
  log_flush_request *entry= pending, *last= nullptr;
  /* Process the first requests that have been completed. Since
  the list is not necessarily in ascending order of LSN, we may
  miss to notify some requests that have already been completed.
  But there is no harm in delaying notifications for those a bit.
  And in practise, the list is unlikely to have more than one
  element anyway, because the redo log would be flushed every
  srv_flush_log_at_timeout seconds (1 by default). */
  for (; entry && entry->lsn <= lsn; last= entry, entry= entry->next);

  if (!last)
  {
    mysql_mutex_unlock(&log_requests.mutex);
    return;
  }

  /* Detach the head of the list that corresponds to persisted log writes. */
  if (!entry)
    log_requests.end= entry;
  log_requests.start.store(entry, std::memory_order_relaxed);
  mysql_mutex_unlock(&log_requests.mutex);

  /* Now that we have released the mutex, notify the submitters
  and free the head of the list. */
  do
  {
    entry= pending;
    pending= pending->next;
    commit_checkpoint_notify_ha(entry->cookie);
    my_free(entry);
  }
  while (entry != last);
}

/** Invoke commit_checkpoint_notify_ha() to notify that outstanding
log writes have been completed. */
void log_flush_notify(lsn_t flush_lsn)
{
  if (auto pending= log_requests.start.load(std::memory_order_acquire))
  {
    mysql_mutex_lock(&log_requests.mutex);
    pending= log_requests.start.load(std::memory_order_relaxed);
    log_flush_notify_and_unlock(pending, flush_lsn);
  }
}

/** Handle a commit checkpoint request from server layer.
We put the request in a queue, so that we can notify upper layer about
checkpoint complete when we have flushed the redo log.
If we have already flushed all relevant redo log, we notify immediately.*/
static void innodb_log_flush_request(void *cookie) noexcept
{
  log_sys.latch.wr_lock(SRW_LOCK_CALL);
  lsn_t flush_lsn= log_sys.get_flushed_lsn();
  /* Load lsn relaxed after flush_lsn was loaded from the same cache line */
  const lsn_t lsn= log_sys.get_lsn();
  log_sys.latch.wr_unlock();

  if (flush_lsn >= lsn)
    /* All log is already persistent. */;
  else if (UNIV_UNLIKELY(srv_force_recovery >= SRV_FORCE_NO_BACKGROUND))
    /* Normally, srv_master_callback() should periodically invoke
    srv_sync_log_buffer_in_background(), which should initiate a log
    flush about once every srv_flush_log_at_timeout seconds.  But,
    starting with the innodb_force_recovery=2 level, that background
    task will not run. */
    log_write_up_to(flush_lsn= lsn, true);
  else if (log_flush_request *req= static_cast<log_flush_request*>
           (my_malloc(PSI_INSTRUMENT_ME, sizeof *req, MYF(MY_WME))))
  {
    req->next= nullptr;
    req->cookie= cookie;
    req->lsn= lsn;

    log_flush_request *start= nullptr;

    mysql_mutex_lock(&log_requests.mutex);
    /* In order to prevent a race condition where log_flush_notify()
    would skip a notification due to, we must update log_requests.start from
    nullptr (empty) to the first req using std::memory_order_release. */
    if (log_requests.start.compare_exchange_strong(start, req,
                                                   std::memory_order_release,
                                                   std::memory_order_relaxed))
    {
      ut_ad(!log_requests.end);
      start= req;
      /* In case log_flush_notify() executed
      log_requests.start.load(std::memory_order_acquire) right before
      our successful compare_exchange, we must re-read flush_lsn to
      ensure that our request will be notified immediately if applicable. */
      flush_lsn= log_sys.get_flushed_lsn();
    }
    else
    {
      /* Append the entry to the list. Because we determined req->lsn before
      acquiring the mutex, this list may not be ordered by req->lsn,
      even though log_flush_notify_and_unlock() assumes so. */
      log_requests.end->next= req;
    }

    log_requests.end= req;

    /* This hopefully addresses the hang that was reported in MDEV-24302.
    Upon receiving a new request, we will notify old requests of
    completion. */
    log_flush_notify_and_unlock(start, flush_lsn);
    return;
  }
  else
    sql_print_error("Failed to allocate %zu bytes."
                    " Commit checkpoint will be skipped.", sizeof *req);

  /* This hopefully addresses the hang that was reported in MDEV-24302.
  Upon receiving a new request to notify of log writes becoming
  persistent, we will notify old requests of completion. Note:
  log_flush_notify() may skip some notifications because it is
  basically assuming that the list is in ascending order of LSN. */
  log_flush_notify(flush_lsn);
  commit_checkpoint_notify_ha(cookie);
}

/*****************************************************************//**
Rolls back a transaction to a savepoint.
@return 0 if success, HA_ERR_NO_SAVEPOINT if no savepoint with the
given name */
static
int
innobase_rollback_to_savepoint(
/*===========================*/
	THD*		thd,		/*!< in: handle to the MySQL thread
					of the user whose transaction should
					be rolled back to savepoint */
	void*		savepoint)	/*!< in: savepoint data */
{
  DBUG_ENTER("innobase_rollback_to_savepoint");
  trx_t *trx= check_trx_exists(thd);

  /* We are reading trx->state without holding trx->mutex here,
  because the savepoint rollback should be invoked for a running
  active transaction that is associated with the current thread. */
  ut_ad(trx->mysql_thd);

  if (UNIV_UNLIKELY(trx->state != TRX_STATE_ACTIVE))
  {
    ut_ad("invalid state" == 0);
    DBUG_RETURN(HA_ERR_NO_SAVEPOINT);
  }

  const undo_no_t *savept= static_cast<const undo_no_t*>(savepoint);

  if (UNIV_UNLIKELY(*savept > trx->undo_no))
    /* row_mysql_handle_errors() should have invoked rollback during
    a bulk insert into an empty table. */
    DBUG_RETURN(HA_ERR_NO_SAVEPOINT);

  dberr_t error= trx->rollback(savept);
  /* Store the position for rolling back the next SQL statement */
  if (trx->fts_trx)
  {
    fts_savepoint_laststmt_refresh(trx);
    fts_savepoint_rollback(trx, savept);
  }
  trx->last_stmt_start= trx->undo_no;
  trx->end_bulk_insert();
  DBUG_RETURN(convert_error_code_to_mysql(error, 0, nullptr));
}

/*****************************************************************//**
Check whether innodb state allows to safely release MDL locks after
rollback to savepoint.
When binlog is on, MDL locks acquired after savepoint unit are not
released if there are any locks held in InnoDB.
@return true if it is safe, false if its not safe. */
static
bool
innobase_rollback_to_savepoint_can_release_mdl(
/*===========================================*/
	THD*		thd)		/*!< in: handle to the MySQL thread
					of the user whose transaction should
					be rolled back to savepoint */
{
	DBUG_ENTER("innobase_rollback_to_savepoint_can_release_mdl");

	trx_t*	trx = check_trx_exists(thd);

	/* If transaction has not acquired any locks then it is safe
	to release MDL after rollback to savepoint */
	if (UT_LIST_GET_LEN(trx->lock.trx_locks) == 0) {

		DBUG_RETURN(true);
	}

	DBUG_RETURN(false);
}

/** Cancel any pending lock request associated with the current THD.
@sa THD::awake() @sa ha_kill_query() */
static void innobase_kill_query(handlerton*, THD *thd, enum thd_kill_levels)
{
  DBUG_ENTER("innobase_kill_query");

  if (trx_t* trx= thd_to_trx(thd))
  {
    ut_ad(trx->mysql_thd == thd);
    mysql_mutex_lock(&lock_sys.wait_mutex);
    lock_t *lock= trx->lock.wait_lock;

    if (!lock)
      /* The transaction is not waiting for any lock. */;
#ifdef WITH_WSREP
    else if (trx->is_wsrep() && wsrep_thd_is_aborting(thd))
      /* if victim has been signaled by BF thread and/or aborting is already
      progressing, following query aborting is not necessary any more.
      Also, BF thread should own trx mutex for the victim. */;
#endif /* WITH_WSREP */
    else
    {
      if (!trx->dict_operation)
      {
        /* Dictionary transactions must be immune to KILL, because they
        may be executed as part of a multi-transaction DDL operation, such
        as rollback_inplace_alter_table() or ha_innobase::delete_table(). */;
        trx->error_state= DB_INTERRUPTED;
        lock_sys.cancel<false>(trx, lock);
      }
      lock_sys.deadlock_check();
    }
    mysql_mutex_unlock(&lock_sys.wait_mutex);
  }

  DBUG_VOID_RETURN;
}


/*************************************************************************//**
** InnoDB database tables
*****************************************************************************/

/** Get the record format from the data dictionary.
@return one of ROW_TYPE_REDUNDANT, ROW_TYPE_COMPACT,
ROW_TYPE_COMPRESSED, ROW_TYPE_DYNAMIC */

enum row_type
ha_innobase::get_row_type() const
{
	if (m_prebuilt && m_prebuilt->table) {
		const ulint	flags = m_prebuilt->table->flags;

		switch (dict_tf_get_rec_format(flags)) {
		case REC_FORMAT_REDUNDANT:
			return(ROW_TYPE_REDUNDANT);
		case REC_FORMAT_COMPACT:
			return(ROW_TYPE_COMPACT);
		case REC_FORMAT_COMPRESSED:
			return(ROW_TYPE_COMPRESSED);
		case REC_FORMAT_DYNAMIC:
			return(ROW_TYPE_DYNAMIC);
		}
	}
	ut_ad(0);
	return(ROW_TYPE_NOT_USED);
}

/****************************************************************//**
Get the table flags to use for the statement.
@return table flags */

handler::Table_flags
ha_innobase::table_flags() const
/*============================*/
{
	THD*			thd = ha_thd();
	handler::Table_flags	flags = m_int_table_flags;

	/* enforce primary key when a table is created, but not when
	an existing (hlindex?) table is auto-discovered */
	if (srv_force_primary_key &&
	    thd_sql_command(thd) == SQLCOM_CREATE_TABLE) {
		flags|= HA_REQUIRE_PRIMARY_KEY;
	}

	/* Need to use tx_isolation here since table flags is (also)
	called before prebuilt is inited. */

	if (thd_tx_isolation(thd) <= ISO_READ_COMMITTED) {
		return(flags);
	}

	return(flags | HA_BINLOG_STMT_CAPABLE);
}

/****************************************************************//**
Returns the table type (storage engine name).
@return table type */

const char*
ha_innobase::table_type() const
/*===========================*/
{
	return(innobase_hton_name);
}

/****************************************************************//**
Returns the operations supported for indexes.
@return flags of supported operations */

ulong
ha_innobase::index_flags(
/*=====================*/
	uint	key,
	uint,
	bool) const
{
	if (table_share->key_info[key].algorithm == HA_KEY_ALG_FULLTEXT) {
		return(0);
	}

	/* For spatial index, we don't support descending scan
	and ICP so far. */
	if (table_share->key_info[key].algorithm == HA_KEY_ALG_RTREE) {
		return HA_READ_NEXT | HA_READ_ORDER| HA_READ_RANGE
			| HA_KEYREAD_ONLY | HA_KEY_SCAN_NOT_ROR;
	}

	ulong flags= key == table_share->primary_key
		? HA_CLUSTERED_INDEX : HA_KEYREAD_ONLY | HA_DO_RANGE_FILTER_PUSHDOWN;

	flags |= HA_READ_NEXT | HA_READ_PREV | HA_READ_ORDER
              | HA_READ_RANGE
              | HA_DO_INDEX_COND_PUSHDOWN;
	return(flags);
}

/****************************************************************//**
Returns the maximum number of keys.
@return MAX_KEY */

uint
ha_innobase::max_supported_keys() const
/*===================================*/
{
	return(MAX_KEY);
}

/****************************************************************//**
Returns the maximum key length.
@return maximum supported key length, in bytes */

uint
ha_innobase::max_supported_key_length() const
/*=========================================*/
{
	/* An InnoDB page must store >= 2 keys; a secondary key record
	must also contain the primary key value.  Therefore, if both
	the primary key and the secondary key are at this maximum length,
	it must be less than 1/4th of the free space on a page including
	record overhead.

	MySQL imposes its own limit to this number; MAX_KEY_LENGTH = 3072.

	For page sizes = 16k, InnoDB historically reported 3500 bytes here,
	But the MySQL limit of 3072 was always used through the handler
	interface.

	Note: Handle 16k and 32k pages the same here since the limits
	are higher than imposed by MySQL. */

	switch (srv_page_size) {
	case 4096:
		/* Hack: allow mysql.innodb_index_stats to be created. */
		/* FIXME: rewrite this API, and in sql_table.cc consider
		that in index-organized tables (such as InnoDB), secondary
		index records will be padded with the PRIMARY KEY, instead
		of some short ROWID or record heap address. */
		return(1173);
	case 8192:
		return(1536);
	default:
		return(3500);
	}
}

/****************************************************************//**
Returns the key map of keys that are usable for scanning.
@return key_map_full */

const key_map*
ha_innobase::keys_to_use_for_scanning()
/*===================================*/
{
	return(&key_map_full);
}

/****************************************************************//**
Ensure that indexed virtual columns will be computed.
Needs to be done for indexes that are being added with inplace ALTER
in a different thread, because from the server point of view these
columns are not yet indexed.
*/
void ha_innobase::column_bitmaps_signal()
{
  if (!table->vfield || table->current_lock != F_WRLCK)
    return;

  dict_index_t* clust_index= dict_table_get_first_index(m_prebuilt->table);
  if (!clust_index->online_log)
    return;

  uint num_v= 0;
  for (uint j = 0; j < table->s->virtual_fields; j++)
  {
    if (table->vfield[j]->stored_in_db())
      continue;

    dict_col_t *col= &m_prebuilt->table->v_cols[num_v].m_col;
    if (col->ord_part ||
        (dict_index_is_online_ddl(clust_index) &&
         row_log_col_is_indexed(clust_index, num_v)))
      table->mark_virtual_column_with_deps(table->vfield[j]);
    num_v++;
  }
}


/****************************************************************//**
Determines if table caching is supported.
@return HA_CACHE_TBL_ASKTRANSACT */

uint8
ha_innobase::table_cache_type()
/*===========================*/
{
	return(HA_CACHE_TBL_ASKTRANSACT);
}

/** Normalizes a table name string.
A normalized name consists of the database name catenated to '/'
and table name. For example: test/mytable.
On Windows, normalization puts both the database name and the
table name always to lower case if "set_lower_case" is set to TRUE.
@param[out]	norm_name	Normalized name, null-terminated.
@param[in]	name		Name to normalize.
@param[in]	set_lower_case	True if we also should fold to lower case. */
size_t
normalize_table_name_c_low(
/*=======================*/
	char*           norm_name,      /* out: normalized name as a
					null-terminated string */
	size_t          norm_name_size, /*!< in: number of bytes available
					in norm_name*/
	const char*     name,           /* in: table name string */
	bool            set_lower_case) /* in: TRUE if we want to set
					 name to lower case */
{
	const char* name_ptr;
	ulint	name_len;
	const char* db_ptr;
	ulint	db_len;
	const char* ptr;

	/* Scan name from the end */

	ptr = strend(name) - 1;

	/* seek to the last path separator */
	while (ptr >= name && *ptr != '\\' && *ptr != '/') {
		ptr--;
	}

	name_ptr = ptr + 1;
	name_len = strlen(name_ptr);

	/* skip any number of path separators */
	while (ptr >= name && (*ptr == '\\' || *ptr == '/')) {
		ptr--;
	}

	DBUG_ASSERT(ptr >= name);

	/* seek to the last but one path separator or one char before
	the beginning of name */
	db_len = 0;
	while (ptr >= name && *ptr != '\\' && *ptr != '/') {
		ptr--;
		db_len++;
	}

	db_ptr = ptr + 1;
	return Identifier_chain2({db_ptr, db_len}, {name_ptr, name_len}).
		make_sep_name_opt_casedn(norm_name, norm_name_size, '/',
					set_lower_case);
}

create_table_info_t::create_table_info_t(
	THD*		thd,
	const TABLE*	form,
	HA_CREATE_INFO*	create_info,
	bool		file_per_table,
	trx_t*		trx)
	: m_thd(thd),
	  m_trx(trx),
	  m_form(form),
	  m_default_row_format(innodb_default_row_format),
	  m_create_info(create_info),
	  m_table(NULL),
	  m_innodb_file_per_table(file_per_table),
	  m_creating_stub(thd_ddl_options(thd)->import_tablespace())
{
  m_table_name[0]= '\0';
  m_remote_path[0]= '\0';
}

#if !defined(DBUG_OFF)
/*********************************************************************
Test normalize_table_name_low(). */
static
void
test_normalize_table_name_low()
/*===========================*/
{
	char		norm_name[FN_REFLEN];
	const char*	test_data[][2] = {
		/* input, expected result */
		{"./mysqltest/t1", "mysqltest/t1"},
		{"./test/#sql-842b_2", "test/#sql-842b_2"},
		{"./test/#sql-85a3_10", "test/#sql-85a3_10"},
		{"./test/#sql2-842b-2", "test/#sql2-842b-2"},
		{"./test/bug29807", "test/bug29807"},
		{"./test/foo", "test/foo"},
		{"./test/innodb_bug52663", "test/innodb_bug52663"},
		{"./test/t", "test/t"},
		{"./test/t1", "test/t1"},
		{"./test/t10", "test/t10"},
		{"/a/b/db/table", "db/table"},
		{"/a/b/db///////table", "db/table"},
		{"/a/b////db///////table", "db/table"},
		{"/var/tmp/mysqld.1/#sql842b_2_10", "mysqld.1/#sql842b_2_10"},
		{"db/table", "db/table"},
		{"ddd/t", "ddd/t"},
		{"d/ttt", "d/ttt"},
		{"d/t", "d/t"},
		{".\\mysqltest\\t1", "mysqltest/t1"},
		{".\\test\\#sql-842b_2", "test/#sql-842b_2"},
		{".\\test\\#sql-85a3_10", "test/#sql-85a3_10"},
		{".\\test\\#sql2-842b-2", "test/#sql2-842b-2"},
		{".\\test\\bug29807", "test/bug29807"},
		{".\\test\\foo", "test/foo"},
		{".\\test\\innodb_bug52663", "test/innodb_bug52663"},
		{".\\test\\t", "test/t"},
		{".\\test\\t1", "test/t1"},
		{".\\test\\t10", "test/t10"},
		{"C:\\a\\b\\db\\table", "db/table"},
		{"C:\\a\\b\\db\\\\\\\\\\\\\\table", "db/table"},
		{"C:\\a\\b\\\\\\\\db\\\\\\\\\\\\\\table", "db/table"},
		{"C:\\var\\tmp\\mysqld.1\\#sql842b_2_10", "mysqld.1/#sql842b_2_10"},
		{"db\\table", "db/table"},
		{"ddd\\t", "ddd/t"},
		{"d\\ttt", "d/ttt"},
		{"d\\t", "d/t"},
	};

	for (size_t i = 0; i < UT_ARR_SIZE(test_data); i++) {
		printf("test_normalize_table_name_low():"
		       " testing \"%s\", expected \"%s\"... ",
		       test_data[i][0], test_data[i][1]);

		normalize_table_name_c_low(
			norm_name, sizeof(norm_name), test_data[i][0], FALSE);

		if (strcmp(norm_name, test_data[i][1]) == 0) {
			printf("ok\n");
		} else {
			printf("got \"%s\"\n", norm_name);
			ut_error;
		}
	}
}
#endif /* !DBUG_OFF */

/** Match index columns between MySQL and InnoDB.
This function checks whether the index column information
is consistent between KEY info from mysql and that from innodb index.
@param[in]	key_info	Index info from mysql
@param[in]	index_info	Index info from InnoDB
@return true if all column types match. */
static
bool
innobase_match_index_columns(
	const KEY*		key_info,
	const dict_index_t*	index_info)
{
	const KEY_PART_INFO*	key_part;
	const KEY_PART_INFO*	key_end;
	const dict_field_t*	innodb_idx_fld;
	const dict_field_t*	innodb_idx_fld_end;

	DBUG_ENTER("innobase_match_index_columns");

	/* Check whether user defined index column count matches */
	if (key_info->user_defined_key_parts !=
		index_info->n_user_defined_cols) {
		DBUG_RETURN(FALSE);
	}

	key_part = key_info->key_part;
	key_end = key_part + key_info->user_defined_key_parts;
	innodb_idx_fld = index_info->fields;
	innodb_idx_fld_end = index_info->fields + index_info->n_fields;

	/* Check each index column's datatype. We do not check
	column name because there exists case that index
	column name got modified in mysql but such change does not
	propagate to InnoDB.
	One hidden assumption here is that the index column sequences
	are matched up between those in mysql and InnoDB. */
	for (; key_part != key_end; ++key_part) {
		unsigned is_unsigned;
		auto mtype = innodb_idx_fld->col->mtype;

		/* Need to translate to InnoDB column type before
		comparison. */
		auto col_type = get_innobase_type_from_mysql_type(
			&is_unsigned, key_part->field);

		/* Ignore InnoDB specific system columns. */
		while (mtype == DATA_SYS) {
			innodb_idx_fld++;

			if (innodb_idx_fld >= innodb_idx_fld_end) {
				DBUG_RETURN(FALSE);
			}
		}

		/* MariaDB-5.5 compatibility */
		if ((key_part->field->real_type() == MYSQL_TYPE_ENUM ||
		     key_part->field->real_type() == MYSQL_TYPE_SET) &&
		    mtype == DATA_FIXBINARY) {
			col_type= DATA_FIXBINARY;
		}

		if (innodb_idx_fld->descending
		    != !!(key_part->key_part_flag & HA_REVERSE_SORT)) {
			DBUG_RETURN(FALSE);
		}

		if (col_type != mtype) {
			/* If the col_type we get from mysql type is a geometry
			data type, we should check if mtype is a legacy type
			from 5.6, either upgraded to DATA_GEOMETRY or not.
			This is indeed not an accurate check, but should be
			safe, since DATA_BLOB would be upgraded once we create
			spatial index on it and we intend to use DATA_GEOMETRY
			for legacy GIS data types which are of var-length. */
			switch (col_type) {
			case DATA_GEOMETRY:
				if (mtype == DATA_BLOB) {
					break;
				}
				/* Fall through */
			default:
				/* Column type mismatches */
				DBUG_RETURN(false);
			}
		}

		innodb_idx_fld++;
	}

	DBUG_RETURN(TRUE);
}

/** Build a template for a base column for a virtual column
@param[in]	table		MySQL TABLE
@param[in]	clust_index	InnoDB clustered index
@param[in]	field		field in MySQL table
@param[in]	col		InnoDB column
@param[in,out]	templ		template to fill
@param[in]	col_no		field index for virtual col
*/
static
void
innobase_vcol_build_templ(
	const TABLE*		table,
	dict_index_t*		clust_index,
	Field*			field,
	const dict_col_t*	col,
	mysql_row_templ_t*	templ,
	ulint			col_no)
{
	templ->col_no = col_no;
	templ->is_virtual = col->is_virtual();

	if (templ->is_virtual) {
		templ->clust_rec_field_no = ULINT_UNDEFINED;
		templ->rec_field_no = col->ind;
	} else {
		templ->clust_rec_field_no = dict_col_get_clust_pos(
						col, clust_index);
		ut_a(templ->clust_rec_field_no != ULINT_UNDEFINED);

		templ->rec_field_no = templ->clust_rec_field_no;
	}

	if (field->real_maybe_null()) {
                templ->mysql_null_byte_offset =
                        field->null_offset();

                templ->mysql_null_bit_mask = (ulint) field->null_bit;
        } else {
                templ->mysql_null_bit_mask = 0;
        }

        templ->mysql_col_offset = static_cast<ulint>(
					get_field_offset(table, field));
	templ->mysql_col_len = static_cast<ulint>(field->pack_length());
        templ->type = col->mtype;
        templ->mysql_type = static_cast<ulint>(field->type());

	if (templ->mysql_type == DATA_MYSQL_TRUE_VARCHAR) {
		templ->mysql_length_bytes = static_cast<ulint>(
			((Field_varstring*) field)->length_bytes);
	}

        templ->charset = dtype_get_charset_coll(col->prtype);
        templ->mbminlen = dict_col_get_mbminlen(col);
        templ->mbmaxlen = dict_col_get_mbmaxlen(col);
        templ->is_unsigned = col->prtype & DATA_UNSIGNED;
}

/** Build template for the virtual columns and their base columns. This
is done when the table first opened.
@param[in]	table		MySQL TABLE
@param[in]	ib_table	InnoDB dict_table_t
@param[in,out]	s_templ		InnoDB template structure
@param[in]	add_v		new virtual columns added along with
				add index call */
void
innobase_build_v_templ(
	const TABLE*		table,
	const dict_table_t*	ib_table,
	dict_vcol_templ_t*	s_templ,
	const dict_add_v_col_t*	add_v)
{
	ulint	ncol = unsigned(ib_table->n_cols) - DATA_N_SYS_COLS;
	ulint	n_v_col = ib_table->n_v_cols;
	bool	marker[REC_MAX_N_FIELDS];

	DBUG_ENTER("innobase_build_v_templ");
	ut_ad(ncol < REC_MAX_N_FIELDS);
	ut_ad(ib_table->lock_mutex_is_owner());

	if (add_v != NULL) {
		n_v_col += add_v->n_v_col;
	}

	ut_ad(n_v_col > 0);

	if (s_templ->vtempl) {
		DBUG_VOID_RETURN;
	}

	memset(marker, 0, sizeof(bool) * ncol);

	s_templ->vtempl = static_cast<mysql_row_templ_t**>(
		ut_zalloc_nokey((ncol + n_v_col)
				* sizeof *s_templ->vtempl));
	s_templ->n_col = ncol;
	s_templ->n_v_col = n_v_col;
	s_templ->rec_len = table->s->reclength;
	s_templ->default_rec = UT_NEW_ARRAY_NOKEY(uchar, s_templ->rec_len);
	memcpy(s_templ->default_rec, table->s->default_values, s_templ->rec_len);

	/* Mark those columns could be base columns */
	for (ulint i = 0; i < ib_table->n_v_cols; i++) {
		const dict_v_col_t*	vcol = dict_table_get_nth_v_col(
							ib_table, i);

		for (ulint j = vcol->num_base; j--; ) {
			marker[vcol->base_col[j]->ind] = true;
		}
	}

	if (add_v) {
		for (ulint i = 0; i < add_v->n_v_col; i++) {
			const dict_v_col_t*	vcol = &add_v->v_col[i];

			for (ulint j = vcol->num_base; j--; ) {
				marker[vcol->base_col[j]->ind] = true;
			}
		}
	}

	ulint	j = 0;
	ulint	z = 0;

	dict_index_t*	clust_index = dict_table_get_first_index(ib_table);

	for (ulint i = 0; i < table->s->fields; i++) {
		Field*  field = table->field[i];

		/* Build template for virtual columns */
		if (!field->stored_in_db()) {
#ifdef UNIV_DEBUG
			const char*	name;

			if (z >= ib_table->n_v_def) {
				name = add_v->v_col_name[z - ib_table->n_v_def];
			} else {
				name = dict_table_get_v_col_name(ib_table, z).str;
			}

			ut_ad(field->field_name.streq(Lex_cstring_strlen(name)));
#endif
			const dict_v_col_t*	vcol;

			if (z >= ib_table->n_v_def) {
				vcol = &add_v->v_col[z - ib_table->n_v_def];
			} else {
				vcol = dict_table_get_nth_v_col(ib_table, z);
			}

			s_templ->vtempl[z + s_templ->n_col]
				= static_cast<mysql_row_templ_t*>(
					ut_malloc_nokey(
					sizeof *s_templ->vtempl[j]));

			innobase_vcol_build_templ(
				table, clust_index, field,
				&vcol->m_col,
				s_templ->vtempl[z + s_templ->n_col],
				z);
			z++;
			continue;
                }

		ut_ad(j < ncol);

		/* Build template for base columns */
		if (marker[j]) {
			dict_col_t*   col = dict_table_get_nth_col(
						ib_table, j);

			ut_ad(field->field_name.streq(
				  dict_table_get_col_name(ib_table, j)));

			s_templ->vtempl[j] = static_cast<
				mysql_row_templ_t*>(
					ut_malloc_nokey(
					sizeof *s_templ->vtempl[j]));

			innobase_vcol_build_templ(
				table, clust_index, field, col,
				s_templ->vtempl[j], j);
		}

		j++;
	}

	s_templ->db_name = table->s->db.str;
	s_templ->tb_name = table->s->table_name.str;

	DBUG_VOID_RETURN;
}

/** Check consistency between .frm indexes and InnoDB indexes.
@param[in]	ib_table	InnoDB table definition
@retval	true if not errors were found */
bool
ha_innobase::check_index_consistency(const dict_table_t* ib_table) noexcept
{
	ulint mysql_num_index = table->s->keys;
	ulint ib_num_index = UT_LIST_GET_LEN(ib_table->indexes);
	bool ret = true;
	ulint last_unique = 0;

	/* If there exists inconsistency between MySQL and InnoDB dictionary
	(metadata) information, the number of index defined in MySQL
	could exceed that in InnoDB, return error */
	if (ib_num_index < mysql_num_index) {
		ret = false;
		goto func_exit;
	}

	/* For each index in the mysql key_info array, fetch its
	corresponding InnoDB index pointer into index_mapping
	array. */
	for (ulint count = 0; count < mysql_num_index; count++) {
		const dict_index_t* index = dict_table_get_index_on_name(
			ib_table, table->key_info[count].name.str);

		if (index == NULL) {
			sql_print_error("Cannot find index %s in InnoDB"
					" index dictionary.",
					table->key_info[count].name.str);
			ret = false;
			goto func_exit;
		}

		/* Double check fetched index has the same
		column info as those in mysql key_info. */
		if (!innobase_match_index_columns(&table->key_info[count],
						  index)) {
			sql_print_error("Found index %s whose column info"
					" does not match that of MariaDB.",
					table->key_info[count].name.str);
			ret = false;
			goto func_exit;
		}

		if (index->is_unique()) {
			ulint i = 0;
			while ((index = UT_LIST_GET_PREV(indexes, index))) i++;
			/* Check if any unique index in InnoDB
			dictionary are re-ordered compared to
			the index in .frm */
			if (last_unique > i) {
				m_int_table_flags
					|= HA_DUPLICATE_KEY_NOT_IN_ORDER;
			}

			last_unique = i;
		}
	}
func_exit:
	return ret;
}

/** Get the maximum integer value of a numeric column.
@param field   column definition
@return maximum allowed integer value */
ulonglong innobase_get_int_col_max_value(const Field *field)
{
	ulonglong	max_value = 0;

	switch (field->key_type()) {
	/* TINY */
	case HA_KEYTYPE_BINARY:
		max_value = 0xFFULL;
		break;
	case HA_KEYTYPE_INT8:
		max_value = 0x7FULL;
		break;
	/* SHORT */
	case HA_KEYTYPE_USHORT_INT:
		max_value = 0xFFFFULL;
		break;
	case HA_KEYTYPE_SHORT_INT:
		max_value = 0x7FFFULL;
		break;
	/* MEDIUM */
	case HA_KEYTYPE_UINT24:
		max_value = 0xFFFFFFULL;
		break;
	case HA_KEYTYPE_INT24:
		max_value = 0x7FFFFFULL;
		break;
	/* LONG */
	case HA_KEYTYPE_ULONG_INT:
		max_value = 0xFFFFFFFFULL;
		break;
	case HA_KEYTYPE_LONG_INT:
		max_value = 0x7FFFFFFFULL;
		break;
	/* BIG */
	case HA_KEYTYPE_ULONGLONG:
		max_value = 0xFFFFFFFFFFFFFFFFULL;
		break;
	case HA_KEYTYPE_LONGLONG:
		max_value = 0x7FFFFFFFFFFFFFFFULL;
		break;
	case HA_KEYTYPE_FLOAT:
		/* We use the maximum as per IEEE754-2008 standard, 2^24 */
		max_value = 0x1000000ULL;
		break;
	case HA_KEYTYPE_DOUBLE:
		/* We use the maximum as per IEEE754-2008 standard, 2^53 */
		max_value = 0x20000000000000ULL;
		break;
	default:
		ut_error;
	}

	return(max_value);
}

/** Initialize the AUTO_INCREMENT column metadata.

Since a partial table definition for a persistent table can already be
present in the InnoDB dict_sys cache before it is accessed from SQL,
we have to initialize the AUTO_INCREMENT counter on the first
ha_innobase::open().

@param[in,out]	table	persistent table
@param[in]	field	the AUTO_INCREMENT column */
static void initialize_auto_increment(dict_table_t *table, const Field& field,
                                      const TABLE_SHARE &s)
{
  ut_ad(!table->is_temporary());
  const unsigned col_no= innodb_col_no(&field);
  table->autoinc_mutex.wr_lock();
  table->persistent_autoinc=
    uint16_t(dict_table_get_nth_col_pos(table, col_no, nullptr) + 1) &
    dict_index_t::MAX_N_FIELDS;
  if (table->autoinc)
    /* Already initialized. Our caller checked
    table->persistent_autoinc without
    autoinc_mutex protection, and there might be multiple
    ha_innobase::open() executing concurrently. */;
  else if (srv_force_recovery >= SRV_FORCE_NO_UNDO_LOG_SCAN)
    /* If innodb_force_recovery is set so high that writes
       are disabled we force the AUTOINC counter to 0
       value effectively disabling writes to the table.
       Secondly, we avoid reading the table in case the read
       results in failure due to a corrupted table/index.

       We will not return an error to the client, so that the
       tables can be dumped with minimal hassle.  If an error
       were returned in this case, the first attempt to read
       the table would fail and subsequent SELECTs would succeed. */;
  else if (table->persistent_autoinc)
  {
    uint64_t max_value= innobase_get_int_col_max_value(&field);
    table->autoinc=
      innobase_next_autoinc(btr_read_autoinc_with_fallback(table, col_no,
                                                           s.mysql_version,
                                                           max_value),
                            1 /* need */,
                            1 /* auto_increment_increment */,
                            0 /* auto_increment_offset */,
                            max_value);
  }

  table->autoinc_mutex.wr_unlock();
}

dberr_t ha_innobase::statistics_init(dict_table_t *table, bool recalc)
{
  ut_ad(table->is_readable());
  ut_ad(!table->stats_mutex_is_owner());

  uint32_t stat= table->stat;
  dberr_t err= DB_SUCCESS;

  if (!recalc && dict_table_t::stat_initialized(stat));
  else if (srv_force_recovery >= SRV_FORCE_NO_UNDO_LOG_SCAN)
    dict_stats_empty_table(table);
  else
  {
    if (dict_table_t::stats_is_persistent(stat) && !srv_read_only_mode
#ifdef WITH_WSREP
        && !wsrep_thd_skip_locking(m_user_thd)
#endif
    )
    {
      switch (dict_stats_persistent_storage_check(false)) {
      case SCHEMA_OK:
        if (recalc)
        {
        recalc:
          err= dict_stats_update_persistent(table);
          if (err == DB_SUCCESS)
            err= dict_stats_save(table);
        }
        else
        {
          err= dict_stats_fetch_from_ps(table);
          if (err == DB_STATS_DO_NOT_EXIST && table->stats_is_auto_recalc())
            goto recalc;
        }
        if (err == DB_SUCCESS || err == DB_READ_ONLY)
          return err;
        if (!recalc)
          break;
        /* fall through */
      case SCHEMA_INVALID:
        if (table->stats_error_printed)
          break;
        table->stats_error_printed = true;
        if (opt_bootstrap)
          break;
        sql_print_warning("InnoDB: %s of persistent statistics requested"
                          " for table %.*sQ.%sQ"
                          " but the required persistent statistics storage"
                          " is corrupted.",
                          recalc ? "Recalculation" : "Fetch",
                          int(table->name.dblen()), table->name.m_name,
                          table->name.basename());
        /* fall through */
      case SCHEMA_NOT_EXIST:
        err= DB_STATS_DO_NOT_EXIST;
      }
    }

    dict_stats_update_transient(table);
  }

  return err;
}

/** Open an InnoDB table
@param[in]	name	table name
@return	error code
@retval	0	on success */
int
ha_innobase::open(const char* name, int, uint)
{
	char			norm_name[FN_REFLEN];

	DBUG_ENTER("ha_innobase::open");

	normalize_table_name(norm_name, sizeof(norm_name), name);

	m_user_thd = NULL;

	/* Will be allocated if it is needed in ::update_row() */
	m_upd_buf = NULL;
	m_upd_buf_size = 0;
	m_disable_rowid_filter = false;

	char*	is_part = is_partition(norm_name);
	THD*	thd = ha_thd();
	dict_table_t* ib_table = open_dict_table(name, norm_name, is_part,
						 DICT_ERR_IGNORE_FK_NOKEY);

	DEBUG_SYNC(thd, "ib_open_after_dict_open");

	if (UNIV_LIKELY(ib_table != nullptr)) {
	} else if (thd_ddl_options(thd)->import_tablespace()) {
		/* If the table does not exist and we are trying to
		import, create a "stub" table similar to the effects
		of CREATE TABLE followed by ALTER TABLE ... DISCARD
		TABLESPACE. */

		HA_CREATE_INFO create_info;
		if (int err = prepare_create_stub_for_import(thd, norm_name,
							     create_info))
			DBUG_RETURN(err);
		create(norm_name, table, &create_info, true, nullptr);
		DEBUG_SYNC(thd, "ib_after_create_stub_for_import");
		ib_table = open_dict_table(name, norm_name, is_part,
					   DICT_ERR_IGNORE_FK_NOKEY);
	} else {
		if (is_part) {
			sql_print_error("Failed to open table %s.\n",
					norm_name);
		}
		set_my_errno(ENOENT);

		DBUG_RETURN(HA_ERR_NO_SUCH_TABLE);
	}

	size_t n_fields = omits_virtual_cols(*table_share)
		? table_share->stored_fields : table_share->fields;
	size_t n_cols = dict_table_get_n_user_cols(ib_table)
		+ dict_table_get_n_v_cols(ib_table)
		- !!DICT_TF2_FLAG_IS_SET(ib_table, DICT_TF2_FTS_HAS_DOC_ID);

	if (UNIV_UNLIKELY(n_cols != n_fields)) {
		ib::warn() << "Table " << norm_name << " contains "
			<< n_cols << " user"
			" defined columns in InnoDB, but " << n_fields
			<< " columns in MariaDB. Please check"
			" INFORMATION_SCHEMA.INNODB_SYS_COLUMNS and"
			" https://mariadb.com/kb/en/innodb-data-dictionary-troubleshooting/"
			" for how to resolve the issue.";

		/* Mark this table as corrupted, so the drop table
		or force recovery can still use it, but not others. */
		ib_table->file_unreadable = true;
		ib_table->corrupted = true;
		ib_table->release();
		set_my_errno(ENOENT);
		DBUG_RETURN(HA_ERR_CRASHED_ON_USAGE);
	}

	innobase_copy_frm_flags_from_table_share(ib_table, table->s);

	MONITOR_INC(MONITOR_TABLE_OPEN);

	if ((ib_table->flags2 & DICT_TF2_DISCARDED)) {
		/* Allow an open because a proper DISCARD should have set
		all the flags and index root page numbers to FIL_NULL that
		should prevent any DML from running but it should allow DDL
		operations. */
	} else if (!ib_table->is_readable()) {
		const fil_space_t* space = ib_table->space;
		if (!space) {
			ib_senderrf(
				thd, IB_LOG_LEVEL_WARN,
				ER_TABLESPACE_MISSING, norm_name);
		}

		if (!thd_tablespace_op(thd)) {
			set_my_errno(ENOENT);
			int ret_err = HA_ERR_TABLESPACE_MISSING;

			if (space && space->crypt_data
			    && space->crypt_data->is_encrypted()) {
				push_warning_printf(
					thd,
					Sql_condition::WARN_LEVEL_WARN,
					HA_ERR_DECRYPTION_FAILED,
					"Table %s in file %s is encrypted"
					" but encryption service or"
					" used key_id %u is not available. "
					" Can't continue reading table.",
					table_share->table_name.str,
					space->chain.start->name,
					space->crypt_data->key_id);
				ret_err = HA_ERR_DECRYPTION_FAILED;
			}

			ib_table->release();
			DBUG_RETURN(ret_err);
		}
	}

	m_prebuilt = row_create_prebuilt(ib_table, table->s->reclength);

	m_prebuilt->default_rec = table->s->default_values;
	ut_ad(m_prebuilt->default_rec);

	m_prebuilt->m_mysql_table = table;

	/* Looks like MySQL-3.23 sometimes has primary key number != 0 */
	m_primary_key = table->s->primary_key;

	key_used_on_scan = m_primary_key;

	if (ib_table->n_v_cols) {
		ib_table->lock_mutex_lock();

		if (ib_table->vc_templ == NULL) {
			ib_table->vc_templ = UT_NEW_NOKEY(dict_vcol_templ_t());
			innobase_build_v_templ(
				table, ib_table, ib_table->vc_templ);
		}

		ib_table->lock_mutex_unlock();
	}

	if (!check_index_consistency(ib_table)) {
		sql_print_error("InnoDB indexes are inconsistent with what "
				"defined in .frm for table %s",
				name);
	}

	/* Allocate a buffer for a 'row reference'. A row reference is
	a string of bytes of length ref_length which uniquely specifies
	a row in our table. Note that MySQL may also compare two row
	references for equality by doing a simple memcmp on the strings
	of length ref_length! */
	if (!(m_prebuilt->clust_index_was_generated
	      = dict_index_is_auto_gen_clust(ib_table->indexes.start))) {
		if (m_primary_key >= MAX_KEY) {
			ib_table->dict_frm_mismatch = DICT_FRM_NO_PK;

			/* This mismatch could cause further problems
			if not attended, bring this to the user's attention
			by printing a warning in addition to log a message
			in the errorlog */

			ib_push_frm_error(thd, ib_table, table, 0, true);

			/* If m_primary_key >= MAX_KEY, its (m_primary_key)
			value could be out of bound if continue to index
			into key_info[] array. Find InnoDB primary index,
			and assign its key_length to ref_length.
			In addition, since MySQL indexes are sorted starting
			with primary index, unique index etc., initialize
			ref_length to the first index key length in
			case we fail to find InnoDB cluster index.

			Please note, this will not resolve the primary
			index mismatch problem, other side effects are
			possible if users continue to use the table.
			However, we allow this table to be opened so
			that user can adopt necessary measures for the
			mismatch while still being accessible to the table
			date. */
			if (!table->key_info) {
				ut_ad(!table->s->keys);
				ref_length = 0;
			} else {
				ref_length = table->key_info[0].key_length;
			}

			/* Find corresponding cluster index
			key length in MySQL's key_info[] array */
			for (uint i = 0; i < table->s->keys; i++) {
				dict_index_t*	index;
				index = innobase_get_index(i);
				if (dict_index_is_clust(index)) {
					ref_length =
						 table->key_info[i].key_length;
				}
			}
		} else {
			/* MySQL allocates the buffer for ref.
			key_info->key_length includes space for all key
			columns + one byte for each column that may be
			NULL. ref_length must be as exact as possible to
			save space, because all row reference buffers are
			allocated based on ref_length. */

			ref_length = table->key_info[m_primary_key].key_length;
		}
	} else {
		if (m_primary_key != MAX_KEY) {

			ib_table->dict_frm_mismatch = DICT_NO_PK_FRM_HAS;

			/* This mismatch could cause further problems
			if not attended, bring this to the user attention
			by printing a warning in addition to log a message
			in the errorlog */
			ib_push_frm_error(thd, ib_table, table, 0, true);
		}

		ref_length = DATA_ROW_ID_LEN;

		/* If we automatically created the clustered index, then
		MySQL does not know about it, and MySQL must NOT be aware
		of the index used on scan, to make it avoid checking if we
		update the column of the index. That is why we assert below
		that key_used_on_scan is the undefined value MAX_KEY.
		The column is the row id in the automatical generation case,
		and it will never be updated anyway. */

		if (key_used_on_scan != MAX_KEY) {
			sql_print_warning(
				"Table %s key_used_on_scan is %u even "
				"though there is no primary key inside "
				"InnoDB.", name, key_used_on_scan);
		}
	}

	/* Index block size in InnoDB: used by MySQL in query optimization */
	stats.block_size = static_cast<uint>(srv_page_size);

	const my_bool for_vc_purge = THDVAR(thd, background_thread);

	if (for_vc_purge || !m_prebuilt->table
	    || m_prebuilt->table->is_temporary()
	    || m_prebuilt->table->persistent_autoinc
	    || !m_prebuilt->table->is_readable()) {
	} else if (const Field* ai = table->found_next_number_field) {
		initialize_auto_increment(m_prebuilt->table, *ai, *table->s);
	}

	/* Set plugin parser for fulltext index */
	for (uint i = 0; i < table->s->keys; i++) {
		if (table->key_info[i].flags & HA_USES_PARSER) {
			dict_index_t*	index = innobase_get_index(i);
			plugin_ref	parser = table->key_info[i].parser;

			ut_ad(index->type & DICT_FTS);
			index->parser =
				static_cast<st_mysql_ftparser *>(
					plugin_decl(parser)->info);

			DBUG_EXECUTE_IF("fts_instrument_use_default_parser",
				index->parser = &fts_default_parser;);
		}
	}

	ut_ad(!m_prebuilt->table
	      || table->versioned() == m_prebuilt->table->versioned());

	if (!for_vc_purge) {
		info(HA_STATUS_NO_LOCK | HA_STATUS_VARIABLE | HA_STATUS_CONST
		     | HA_STATUS_OPEN);
	}

	DBUG_RETURN(0);
}

/** Convert MySQL column number to dict_table_t::cols[] offset.
@param[in]	field	non-virtual column
@return	column number relative to dict_table_t::cols[] */
unsigned
innodb_col_no(const Field* field)
{
	ut_ad(!innobase_is_s_fld(field));
	const TABLE*	table	= field->table;
	unsigned	col_no	= 0;
	ut_ad(field == table->field[field->field_index]);
	for (unsigned i = 0; i < field->field_index; i++) {
		if (table->field[i]->stored_in_db()) {
			col_no++;
		}
	}
	return(col_no);
}

/** Opens dictionary table object using table name. For partition, we need to
try alternative lower/upper case names to support moving data files across
platforms.
@param[in]	table_name	name of the table/partition
@param[in]	norm_name	normalized name of the table/partition
@param[in]	is_partition	if this is a partition of a table
@param[in]	ignore_err	error to ignore for loading dictionary object
@return dictionary table object or NULL if not found */
dict_table_t*
ha_innobase::open_dict_table(
	const char*
#ifdef _WIN32
	table_name
#endif
	,
	const char*		norm_name,
	bool			is_partition,
	dict_err_ignore_t	ignore_err)
{
	DBUG_ENTER("ha_innobase::open_dict_table");
	/* FIXME: try_drop_aborted */
	dict_table_t*	ib_table = dict_table_open_on_name(norm_name, false,
							   ignore_err);

	if (NULL == ib_table && is_partition) {
		/* MySQL partition engine hard codes the file name
		separator as "#P#". The text case is fixed even if
		lower_case_table_names is set to 1 or 2. This is true
		for sub-partition names as well. InnoDB always
		normalises file names to lower case on Windows, this
		can potentially cause problems when copying/moving
		tables between platforms.

		1) If boot against an installation from Windows
		platform, then its partition table name could
		be in lower case in system tables. So we will
		need to check lower case name when load table.

		2) If we boot an installation from other case
		sensitive platform in Windows, we might need to
		check the existence of table name without lower
		case in the system table. */
		if (lower_case_table_names == 1) {
			char	par_case_name[FN_REFLEN];

#ifndef _WIN32
			/* Check for the table using lower
			case name, including the partition
			separator "P" */
			system_charset_info->casedn_z(
				norm_name, strlen(norm_name),
				par_case_name, sizeof(par_case_name));
#else
			/* On Windows platfrom, check
			whether there exists table name in
			system table whose name is
			not being normalized to lower case */
			normalize_table_name_c_low(
				par_case_name, sizeof(par_case_name),
				table_name, false);
#endif
			/* FIXME: try_drop_aborted */
			ib_table = dict_table_open_on_name(
				par_case_name, false, ignore_err);
		}

		if (ib_table != NULL) {
#ifndef _WIN32
			sql_print_warning("Partition table %s opened"
					  " after converting to lower"
					  " case. The table may have"
					  " been moved from a case"
					  " in-sensitive file system."
					  " Please recreate table in"
					  " the current file system\n",
					  norm_name);
#else
			sql_print_warning("Partition table %s opened"
					  " after skipping the step to"
					  " lower case the table name."
					  " The table may have been"
					  " moved from a case sensitive"
					  " file system. Please"
					  " recreate table in the"
					  " current file system\n",
					  norm_name);
#endif
		}
	}

	DBUG_RETURN(ib_table);
}

handler*
ha_innobase::clone(
/*===============*/
	const char*	name,		/*!< in: table name */
	MEM_ROOT*	mem_root)	/*!< in: memory context */
{
	DBUG_ENTER("ha_innobase::clone");

	ha_innobase*	new_handler = static_cast<ha_innobase*>(
		handler::clone(m_prebuilt->table->name.m_name, mem_root));

	if (new_handler != NULL) {
		DBUG_ASSERT(new_handler->m_prebuilt != NULL);

		new_handler->m_prebuilt->select_lock_type
			= m_prebuilt->select_lock_type;
	}

	DBUG_RETURN(new_handler);
}


uint
ha_innobase::max_supported_key_part_length() const
/*==============================================*/
{
	/* A table format specific index column length check will be performed
	at ha_innobase::add_index() and row_create_index_for_mysql() */
	return(REC_VERSION_56_MAX_INDEX_COL_LEN);
}

/******************************************************************//**
Closes a handle to an InnoDB table.
@return 0 */

int
ha_innobase::close()
/*================*/
{
	DBUG_ENTER("ha_innobase::close");

	row_prebuilt_free(m_prebuilt);

	if (m_upd_buf != NULL) {
		ut_ad(m_upd_buf_size != 0);
		my_free(m_upd_buf);
		m_upd_buf = NULL;
		m_upd_buf_size = 0;
	}

	DBUG_RETURN(0);
}

/* The following accessor functions should really be inside MySQL code! */

#ifdef WITH_WSREP
ulint
wsrep_innobase_mysql_sort(
					/* out: str contains sort string */
	int		mysql_type,	/* in: MySQL type */
	uint		charset_number,	/* in: number of the charset */
	unsigned char*	str,		/* in: data field */
	ulint		str_length,	/* in: data field length,
					not UNIV_SQL_NULL */
	ulint		buf_length)	/* in: total str buffer length */

{
	CHARSET_INFO*		charset;
	enum_field_types	mysql_tp;
	ulint			ret_length =	str_length;

	DBUG_ASSERT(str_length != UNIV_SQL_NULL);

	mysql_tp = (enum_field_types) mysql_type;

	switch (mysql_tp) {

	case MYSQL_TYPE_BIT:
	case MYSQL_TYPE_STRING:
	case MYSQL_TYPE_VAR_STRING:
	case MYSQL_TYPE_TINY_BLOB:
	case MYSQL_TYPE_MEDIUM_BLOB:
	case MYSQL_TYPE_BLOB:
	case MYSQL_TYPE_LONG_BLOB:
	case MYSQL_TYPE_VARCHAR:
	{
		uchar tmp_str[REC_VERSION_56_MAX_INDEX_COL_LEN] = {'\0'};
		ulint tmp_length = REC_VERSION_56_MAX_INDEX_COL_LEN;

		/* Use the charset number to pick the right charset struct for
		the comparison. Since the MySQL function get_charset may be
		slow before Bar removes the mutex operation there, we first
		look at 2 common charsets directly. */

		if (charset_number == default_charset_info->number) {
			charset = default_charset_info;
		} else if (charset_number == my_charset_latin1.number) {
			charset = &my_charset_latin1;
		} else {
			charset = get_charset(charset_number, MYF(MY_WME));

			if (charset == NULL) {
			  sql_print_error("InnoDB needs charset %lu for doing "
					  "a comparison, but MariaDB cannot "
					  "find that charset.",
					  (ulong) charset_number);
				ut_a(0);
			}
		}

		ut_a(str_length <= tmp_length);
		memcpy(tmp_str, str, str_length);

		tmp_length = charset->strnxfrm(str, str_length,
					       uint(str_length), tmp_str,
					       tmp_length, 0).m_result_length;
		DBUG_ASSERT(tmp_length <= str_length);
		if (wsrep_protocol_version < 3) {
			tmp_length = charset->strnxfrm(
				str, str_length,
				uint(str_length), tmp_str,
				tmp_length, 0).m_result_length;
			DBUG_ASSERT(tmp_length <= str_length);
		} else {
			/* strnxfrm will expand the destination string,
			   protocols < 3 truncated the sorted sring
			   protocols >= 3 gets full sorted sring
			*/
			tmp_length = charset->strnxfrm(
				str, buf_length,
				uint(str_length), tmp_str,
				str_length, 0).m_result_length;
			DBUG_ASSERT(tmp_length <= buf_length);
			ret_length = tmp_length;
		}

		break;
	}
	case MYSQL_TYPE_DECIMAL :
	case MYSQL_TYPE_TINY :
	case MYSQL_TYPE_SHORT :
	case MYSQL_TYPE_LONG :
	case MYSQL_TYPE_FLOAT :
	case MYSQL_TYPE_DOUBLE :
	case MYSQL_TYPE_NULL :
	case MYSQL_TYPE_TIMESTAMP :
	case MYSQL_TYPE_LONGLONG :
	case MYSQL_TYPE_INT24 :
	case MYSQL_TYPE_DATE :
	case MYSQL_TYPE_TIME :
	case MYSQL_TYPE_DATETIME :
	case MYSQL_TYPE_YEAR :
	case MYSQL_TYPE_NEWDATE :
	case MYSQL_TYPE_NEWDECIMAL :
	case MYSQL_TYPE_ENUM :
	case MYSQL_TYPE_SET :
	case MYSQL_TYPE_GEOMETRY :
		break;
	default:
		break;
	}

	return ret_length;
}
#endif /* WITH_WSREP */

/******************************************************************//**
compare two character string according to their charset. */
int
innobase_fts_text_cmp(
/*==================*/
	const void*	cs,		/*!< in: Character set */
	const void*     p1,		/*!< in: key */
	const void*     p2)		/*!< in: node */
{
	const CHARSET_INFO*	charset = (const CHARSET_INFO*) cs;
	const fts_string_t*	s1 = (const fts_string_t*) p1;
	const fts_string_t*	s2 = (const fts_string_t*) p2;

	return(ha_compare_word(charset,
		s1->f_str, static_cast<uint>(s1->f_len),
		s2->f_str, static_cast<uint>(s2->f_len)));
}

/******************************************************************//**
Get the first character's code position for FTS index partition. */
ulint
innobase_strnxfrm(
/*==============*/
	const CHARSET_INFO*
			cs,		/*!< in: Character set */
	const uchar*	str,		/*!< in: string */
	const ulint	len)		/*!< in: string length */
{
	uchar		mystr[2];
	ulint		value;

	if (!str || len == 0) {
		return(0);
	}

	cs->strnxfrm((uchar*) mystr, 2, str, len);

	value = mach_read_from_2(mystr);

	if (value > 255) {
		value = value / 256;
	}

	return(value);
}

/******************************************************************//**
compare two character string according to their charset. */
int
innobase_fts_text_cmp_prefix(
/*=========================*/
	const void*	cs,		/*!< in: Character set */
	const void*	p1,		/*!< in: prefix key */
	const void*	p2)		/*!< in: value to compare */
{
	const CHARSET_INFO*	charset = (const CHARSET_INFO*) cs;
	const fts_string_t*	s1 = (const fts_string_t*) p1;
	const fts_string_t*	s2 = (const fts_string_t*) p2;
	int			result;

	result = ha_compare_word_prefix(charset,
		s2->f_str, static_cast<uint>(s2->f_len),
		s1->f_str, static_cast<uint>(s1->f_len));

	/* We switched s1, s2 position in the above call. So we need
	to negate the result */
	return(-result);
}

#define true_word_char(c, ch) ((c) & (_MY_U | _MY_L | _MY_NMR) || (ch) == '_')

#define misc_word_char(X)       0

/*************************************************************//**
Get the next token from the given string and store it in *token.
It is mostly copied from MyISAM's doc parsing function ft_simple_get_word()
@return length of string processed */
ulint
innobase_mysql_fts_get_token(
/*=========================*/
	CHARSET_INFO*	cs,		/*!< in: Character set */
	const byte*	start,		/*!< in: start of text */
	const byte*	end,		/*!< in: one character past end of
					text */
	fts_string_t*	token)		/*!< out: token's text */
{
	int		mbl;
	const uchar*	doc = start;

	ut_a(cs);

	token->f_n_char = token->f_len = 0;
	token->f_str = NULL;

	for (;;) {

		if (doc >= end) {
			return ulint(doc - start);
		}

		int	ctype;

		mbl = cs->ctype(&ctype, doc, (const uchar*) end);

		if (true_word_char(ctype, *doc)) {
			break;
		}

		doc += mbl > 0 ? mbl : (mbl < 0 ? -mbl : 1);
	}

	ulint	mwc = 0;
	ulint	length = 0;
	bool	reset_token_str = false;
reset:
	token->f_str = const_cast<byte*>(doc);

	while (doc < end) {

		int	ctype;

		mbl = cs->ctype(&ctype, (uchar*) doc, (uchar*) end);
		if (true_word_char(ctype, *doc)) {
			mwc = 0;
		} else if (*doc == '\'' && length == 1) {
			/* Could be apostrophe */
			reset_token_str = true;
		} else if (!misc_word_char(*doc) || mwc) {
			break;
		} else {
			++mwc;
		}

		++length;

		doc += mbl > 0 ? mbl : (mbl < 0 ? -mbl : 1);
		if (reset_token_str) {
			/* Reset the token if the single character
			followed by apostrophe */
			mwc = 0;
			length = 0;
			reset_token_str = false;
			goto reset;
		}
	}

	token->f_len = (uint) (doc - token->f_str) - mwc;
	token->f_n_char = length;

	return ulint(doc - start);
}

/** Converts a MySQL type to an InnoDB type. Note that this function returns
the 'mtype' of InnoDB. InnoDB differentiates between MySQL's old <= 4.1
VARCHAR and the new true VARCHAR in >= 5.0.3 by the 'prtype'.
@param[out]	unsigned_flag	DATA_UNSIGNED if an 'unsigned type'; at least
ENUM and SET, and unsigned integer types are 'unsigned types'
@param[in]	f		MySQL Field
@return DATA_BINARY, DATA_VARCHAR, ... */
uint8_t
get_innobase_type_from_mysql_type(unsigned *unsigned_flag, const Field *field)
{
	/* The following asserts try to check that the MySQL type code fits in
	8 bits: this is used when DATA_NOT_NULL is ORed to the type */

	static_assert(MYSQL_TYPE_STRING < 256, "compatibility");
	static_assert(MYSQL_TYPE_VAR_STRING < 256, "compatibility");
	static_assert(MYSQL_TYPE_DOUBLE < 256, "compatibility");
	static_assert(MYSQL_TYPE_FLOAT < 256, "compatibility");
	static_assert(MYSQL_TYPE_DECIMAL < 256, "compatibility");

	if (field->flags & UNSIGNED_FLAG) {

		*unsigned_flag = DATA_UNSIGNED;
	} else {
		*unsigned_flag = 0;
	}

	if (field->real_type() == MYSQL_TYPE_ENUM
		|| field->real_type() == MYSQL_TYPE_SET) {

		/* MySQL has field->type() a string type for these, but the
		data is actually internally stored as an unsigned integer
		code! */

		*unsigned_flag = DATA_UNSIGNED; /* MySQL has its own unsigned
						flag set to zero, even though
						internally this is an unsigned
						integer type */
		return(DATA_INT);
	}

	switch (field->type()) {
		/* NOTE that we only allow string types in DATA_MYSQL and
		DATA_VARMYSQL */
	case MYSQL_TYPE_VAR_STRING:	/* old <= 4.1 VARCHAR */
	case MYSQL_TYPE_VARCHAR:	/* new >= 5.0.3 true VARCHAR */
		if (field->binary()) {
			return(DATA_BINARY);
		} else if (field->charset() == &my_charset_latin1) {
			return(DATA_VARCHAR);
		} else {
			return(DATA_VARMYSQL);
		}
	case MYSQL_TYPE_BIT:
	case MYSQL_TYPE_STRING:
		if (field->binary() || field->key_type() == HA_KEYTYPE_BINARY) {
			return(DATA_FIXBINARY);
		} else if (field->charset() == &my_charset_latin1) {
			return(DATA_CHAR);
		} else {
			return(DATA_MYSQL);
		}
	case MYSQL_TYPE_NEWDECIMAL:
		return(DATA_FIXBINARY);
	case MYSQL_TYPE_LONG:
	case MYSQL_TYPE_LONGLONG:
	case MYSQL_TYPE_TINY:
	case MYSQL_TYPE_SHORT:
	case MYSQL_TYPE_INT24:
	case MYSQL_TYPE_DATE:
	case MYSQL_TYPE_YEAR:
	case MYSQL_TYPE_NEWDATE:
		return(DATA_INT);
	case MYSQL_TYPE_TIME:
	case MYSQL_TYPE_DATETIME:
	case MYSQL_TYPE_TIMESTAMP:
		if (field->key_type() == HA_KEYTYPE_BINARY) {
			return(DATA_FIXBINARY);
		} else {
			return(DATA_INT);
		}
	case MYSQL_TYPE_FLOAT:
		return(DATA_FLOAT);
	case MYSQL_TYPE_DOUBLE:
		return(DATA_DOUBLE);
	case MYSQL_TYPE_DECIMAL:
		return(DATA_DECIMAL);
	case MYSQL_TYPE_GEOMETRY:
		return(DATA_GEOMETRY);
	case MYSQL_TYPE_TINY_BLOB:
	case MYSQL_TYPE_MEDIUM_BLOB:
	case MYSQL_TYPE_BLOB:
	case MYSQL_TYPE_LONG_BLOB:
		return(DATA_BLOB);
	case MYSQL_TYPE_NULL:
		/* MySQL currently accepts "NULL" datatype, but will
		reject such datatype in the next release. We will cope
		with it and not trigger assertion failure in 5.1 */
		break;
	default:
		ut_error;
	}

	return(0);
}

/*******************************************************************//**
Reads an unsigned integer value < 64k from 2 bytes, in the little-endian
storage format.
@return value */
static inline
uint
innobase_read_from_2_little_endian(
/*===============================*/
	const uchar*	buf)	/*!< in: from where to read */
{
	return((uint) ((ulint)(buf[0]) + 256 * ((ulint)(buf[1]))));
}

#ifdef WITH_WSREP
/*******************************************************************//**
Stores a key value for a row to a buffer.
@return	key value length as stored in buff */
static
uint16_t
wsrep_store_key_val_for_row(
/*=========================*/
	THD* 		thd,
	TABLE*		table,
	uint		keynr,	/*!< in: key number */
	char*		buff,	/*!< in/out: buffer for the key value (in MySQL
				format) */
	uint		buff_len,/*!< in: buffer length */
	const uchar*	record,
	bool*		key_is_null)/*!< out: full key was null */
{
	KEY*		key_info	= table->key_info + keynr;
	KEY_PART_INFO*	key_part	= key_info->key_part;
	KEY_PART_INFO*	end		= key_part + key_info->user_defined_key_parts;
	char*		buff_start	= buff;
	enum_field_types mysql_type;
	Field*		field;
	ulint buff_space = buff_len;

	DBUG_ENTER("wsrep_store_key_val_for_row");

	memset(buff, 0, buff_len);
	*key_is_null = true;

	for (; key_part != end; key_part++) {
		uchar sorted[REC_VERSION_56_MAX_INDEX_COL_LEN] = {'\0'};
		bool part_is_null = false;

		if (key_part->null_bit) {
			if (buff_space > 0) {
				if (record[key_part->null_offset]
				    & key_part->null_bit) {
					*buff = 1;
					part_is_null = true;
				} else {
					*buff = 0;
				}
				buff++;
				buff_space--;
			} else {
				fprintf (stderr, "WSREP: key truncated: %s\n",
					 wsrep_thd_query(thd));
			}
		}
		if (!part_is_null)  *key_is_null = false;

		field = key_part->field;
		mysql_type = field->type();

		if (mysql_type == MYSQL_TYPE_VARCHAR) {
						/* >= 5.0.3 true VARCHAR */
			ulint		lenlen;
			ulint		len;
			const byte*	data;
			ulint		key_len;
			ulint		true_len;
			const CHARSET_INFO* cs;
			int		error=0;

			key_len = key_part->length;

			if (part_is_null) {
				true_len = key_len + 2;
				if (true_len > buff_space) {
					fprintf (stderr,
						 "WSREP: key truncated: %s\n",
						 wsrep_thd_query(thd));
					true_len = buff_space;
				}
				buff       += true_len;
				buff_space -= true_len;
				continue;
			}
			cs = field->charset();

			lenlen = (ulint)
				(((Field_varstring*)field)->length_bytes);

			data = row_mysql_read_true_varchar(&len,
				(byte*) (record
				+ (ulint)get_field_offset(table, field)),
				lenlen);

			true_len = len;

			/* For multi byte character sets we need to calculate
			the true length of the key */

			if (len > 0 && cs->mbmaxlen > 1) {
				true_len = (ulint) my_well_formed_length(cs,
						(const char *) data,
						(const char *) data + len,
						(uint) (key_len /
						cs->mbmaxlen),
						&error);
			}

			/* In a column prefix index, we may need to truncate
			the stored value: */
			if (true_len > key_len) {
				true_len = key_len;
			}
			/* cannot exceed max column length either, we may need to truncate
			the stored value: */
			if (true_len > sizeof(sorted)) {
			  true_len = sizeof(sorted);
			}

			memcpy(sorted, data, true_len);
			true_len = wsrep_innobase_mysql_sort(
				mysql_type, cs->number, sorted, true_len,
				REC_VERSION_56_MAX_INDEX_COL_LEN);
			if (wsrep_protocol_version > 1) {
				/* Note that we always reserve the maximum possible
				length of the true VARCHAR in the key value, though
				only len first bytes after the 2 length bytes contain
				actual data. The rest of the space was reset to zero
				in the bzero() call above. */
				if (true_len > buff_space) {
					WSREP_DEBUG (
						 "write set key truncated for: %s\n",
						 wsrep_thd_query(thd));
					true_len = buff_space;
				}
 				memcpy(buff, sorted, true_len);
				buff += true_len;
				buff_space -= true_len;
			} else {
				buff += key_len;
			}
		} else if (mysql_type == MYSQL_TYPE_TINY_BLOB
			|| mysql_type == MYSQL_TYPE_MEDIUM_BLOB
			|| mysql_type == MYSQL_TYPE_BLOB
			|| mysql_type == MYSQL_TYPE_LONG_BLOB
			/* MYSQL_TYPE_GEOMETRY data is treated
			as BLOB data in innodb. */
			|| mysql_type == MYSQL_TYPE_GEOMETRY) {

			const CHARSET_INFO* cs;
			ulint		key_len;
			ulint		true_len;
			int		error=0;
			ulint		blob_len;
			const byte*	blob_data;

			ut_a(key_part->key_part_flag & HA_PART_KEY_SEG);

			key_len = key_part->length;

			if (part_is_null) {
				true_len = key_len + 2;
				if (true_len > buff_space) {
					fprintf (stderr,
						 "WSREP: key truncated: %s\n",
						 wsrep_thd_query(thd));
					true_len = buff_space;
				}
				buff       += true_len;
				buff_space -= true_len;

				continue;
			}

			cs = field->charset();

			blob_data = row_mysql_read_blob_ref(&blob_len,
				(byte*) (record
				+ (ulint)get_field_offset(table, field)),
					(ulint) field->pack_length());

			true_len = blob_len;

			ut_a(get_field_offset(table, field)
				== key_part->offset);

			/* For multi byte character sets we need to calculate
			the true length of the key */

			if (blob_len > 0 && cs->mbmaxlen > 1) {
				true_len = (ulint) my_well_formed_length(cs,
						(const char *) blob_data,
						(const char *) blob_data
							+ blob_len,
						(uint) (key_len /
							cs->mbmaxlen),
						&error);
			}

			/* All indexes on BLOB and TEXT are column prefix
			indexes, and we may need to truncate the data to be
			stored in the key value: */

			if (true_len > key_len) {
				true_len = key_len;
			}

			memcpy(sorted, blob_data, true_len);
			true_len = wsrep_innobase_mysql_sort(
				mysql_type, cs->number, sorted, true_len,
				REC_VERSION_56_MAX_INDEX_COL_LEN);


			/* Note that we always reserve the maximum possible
			length of the BLOB prefix in the key value. */
			if (wsrep_protocol_version > 1) {
				if (true_len > buff_space) {
					fprintf (stderr,
						 "WSREP: key truncated: %s\n",
						 wsrep_thd_query(thd));
					true_len = buff_space;
				}
				buff       += true_len;
				buff_space -= true_len;
			} else {
				buff += key_len;
			}
			memcpy(buff, sorted, true_len);
		} else {
			/* Here we handle all other data types except the
			true VARCHAR, BLOB and TEXT. Note that the column
			value we store may be also in a column prefix
			index. */

			const CHARSET_INFO*	cs = NULL;
			ulint			true_len;
			ulint			key_len;
			const uchar*		src_start;
			int			error=0;
			enum_field_types	real_type;

			key_len = key_part->length;

			if (part_is_null) {
				true_len = key_len;
				if (true_len > buff_space) {
					fprintf (stderr,
						 "WSREP: key truncated: %s\n",
						 wsrep_thd_query(thd));
					true_len = buff_space;
				}
				buff       += true_len;
				buff_space -= true_len;

				continue;
			}

			src_start = record + key_part->offset;
			real_type = field->real_type();
			true_len = key_len;

			/* Character set for the field is defined only
			to fields whose type is string and real field
			type is not enum or set. For these fields check
			if character set is multi byte. */

			if (real_type != MYSQL_TYPE_ENUM
				&& real_type != MYSQL_TYPE_SET
				&& ( mysql_type == MYSQL_TYPE_VAR_STRING
					|| mysql_type == MYSQL_TYPE_STRING)) {

				cs = field->charset();

				/* For multi byte character sets we need to
				calculate the true length of the key */

				if (key_len > 0 && cs->mbmaxlen > 1) {

					true_len = (ulint)
						my_well_formed_length(cs,
							(const char *)src_start,
							(const char *)src_start
								+ key_len,
							(uint) (key_len /
								cs->mbmaxlen),
							&error);
				}
				memcpy(sorted, src_start, true_len);
				true_len = wsrep_innobase_mysql_sort(
					mysql_type, cs->number, sorted, true_len,
					REC_VERSION_56_MAX_INDEX_COL_LEN);

				if (true_len > buff_space) {
					fprintf (stderr,
						 "WSREP: key truncated: %s\n",
						 wsrep_thd_query(thd));
					true_len   = buff_space;
				}
				memcpy(buff, sorted, true_len);
			} else {
				memcpy(buff, src_start, true_len);
			}
			buff       += true_len;
			buff_space -= true_len;
		}
	}

	ut_a(buff <= buff_start + buff_len);

	DBUG_RETURN(static_cast<uint16_t>(buff - buff_start));
}
#endif /* WITH_WSREP */
/**************************************************************//**
Determines if a field is needed in a m_prebuilt struct 'template'.
@return field to use, or NULL if the field is not needed */
static
const Field*
build_template_needs_field(
/*=======================*/
	bool		index_contains,	/*!< in:
					dict_index_t::contains_col_or_prefix(
					i) */
	bool		read_just_key,	/*!< in: TRUE when MySQL calls
					ha_innobase::extra with the
					argument HA_EXTRA_KEYREAD; it is enough
					to read just columns defined in
					the index (i.e., no read of the
					clustered index record necessary) */
	bool		fetch_all_in_key,
					/*!< in: true=fetch all fields in
					the index */
	bool		fetch_primary_key_cols,
					/*!< in: true=fetch the
					primary key columns */
	dict_index_t*	index,		/*!< in: InnoDB index to use */
	const TABLE*	table,		/*!< in: MySQL table object */
	ulint		i,		/*!< in: field index in InnoDB table */
	ulint		num_v)		/*!< in: num virtual column so far */
{
	const Field*	field	= table->field[i];

	if (!field->stored_in_db()
	    && ha_innobase::omits_virtual_cols(*table->s)) {
		return NULL;
	}

	if (!index_contains) {
		if (read_just_key) {
			/* If this is a 'key read', we do not need
			columns that are not in the key */

			return(NULL);
		}
	} else if (fetch_all_in_key) {
		/* This field is needed in the query */

		return(field);
	}

	if (bitmap_is_set(table->read_set, static_cast<uint>(i))
	    || bitmap_is_set(table->write_set, static_cast<uint>(i))) {
		/* This field is needed in the query */

		return(field);
	}

	ut_ad(i >= num_v);
	if (fetch_primary_key_cols
	    && dict_table_col_in_clustered_key(index->table, i - num_v)) {
		/* This field is needed in the query */
		return(field);
	}

	/* This field is not needed in the query, skip it */

	return(NULL);
}

/**************************************************************//**
Determines if a field is needed in a m_prebuilt struct 'template'.
@return whether the field is needed for index condition pushdown */
inline
bool
build_template_needs_field_in_icp(
/*==============================*/
	const dict_index_t*	index,	/*!< in: InnoDB index */
	const row_prebuilt_t*	prebuilt,/*!< in: row fetch template */
	bool			contains,/*!< in: whether the index contains
					column i */
	ulint			i,	/*!< in: column number */
	bool			is_virtual)
					/*!< in: a virtual column or not */
{
	ut_ad(contains == index->contains_col_or_prefix(i, is_virtual));

	return(index == prebuilt->index
	       ? contains
	       : prebuilt->index->contains_col_or_prefix(i, is_virtual));
}

/**************************************************************//**
Adds a field to a m_prebuilt struct 'template'.
@return the field template */
static
mysql_row_templ_t*
build_template_field(
/*=================*/
	row_prebuilt_t*	prebuilt,	/*!< in/out: template */
	dict_index_t*	clust_index,	/*!< in: InnoDB clustered index */
	dict_index_t*	index,		/*!< in: InnoDB index to use */
	TABLE*		table,		/*!< in: MySQL table object */
	const Field*	field,		/*!< in: field in MySQL table */
	ulint		i,		/*!< in: field index in InnoDB table */
	ulint		v_no)		/*!< in: field index for virtual col */
{
	mysql_row_templ_t*	templ;
	const dict_col_t*	col;

	ut_ad(clust_index->table == index->table);

	templ = prebuilt->mysql_template + prebuilt->n_template++;
	MEM_UNDEFINED(templ, sizeof *templ);
	templ->rec_field_is_prefix = FALSE;
	templ->rec_prefix_field_no = ULINT_UNDEFINED;
	templ->is_virtual = !field->stored_in_db();

	if (!templ->is_virtual) {
		templ->col_no = i;
		col = dict_table_get_nth_col(index->table, i);
		templ->clust_rec_field_no = dict_col_get_clust_pos(
						col, clust_index);
		/* If clustered index record field is not found, lets print out
		field names and all the rest to understand why field is not found. */
		if (templ->clust_rec_field_no == ULINT_UNDEFINED) {
			const char* tb_col_name = dict_table_get_col_name(clust_index->table, i).str;
			dict_field_t* field=NULL;
			size_t size = 0;

			for(ulint j=0; j < clust_index->n_user_defined_cols; j++) {
				dict_field_t* ifield = &(clust_index->fields[j]);
				if (ifield && !memcmp(tb_col_name, ifield->name,
						strlen(tb_col_name))) {
					field = ifield;
					break;
				}
			}

			ib::info() << "Looking for field " << i << " name "
				<< (tb_col_name ? tb_col_name : "NULL")
				<< " from table " << clust_index->table->name;


			for(ulint j=0; j < clust_index->n_user_defined_cols; j++) {
				dict_field_t* ifield = &(clust_index->fields[j]);
				ib::info() << "InnoDB Table "
					<< clust_index->table->name
					<< "field " << j << " name "
					<< (ifield ? ifield->name() : "NULL");
			}

			for(ulint j=0; j < table->s->stored_fields; j++) {
				ib::info() << "MySQL table "
					<< table->s->table_name.str
					<< " field " << j << " name "
					<< table->field[j]->field_name.str;
			}

			ib::fatal() << "Clustered record field for column " << i
				<< " not found table n_user_defined "
				<< clust_index->n_user_defined_cols
				<< " index n_user_defined "
				<< clust_index->table->n_cols - DATA_N_SYS_COLS
				<< " InnoDB table "
				<< clust_index->table->name
				<< " field name "
				<< (field ? field->name() : "NULL")
				<< " MySQL table "
				<< table->s->table_name.str
				<< " field name "
				<< (tb_col_name ? tb_col_name : "NULL")
				<< " n_fields "
				<< table->s->stored_fields
				<< " query "
				<< innobase_get_stmt_unsafe(current_thd, &size);
		}

		if (dict_index_is_clust(index)) {
			templ->rec_field_no = templ->clust_rec_field_no;
		} else {
			/* If we're in a secondary index, keep track
			* of the original index position even if this
			* is just a prefix index; we will use this
			* later to avoid a cluster index lookup in
			* some cases.*/

			templ->rec_field_no = dict_index_get_nth_col_pos(index, i,
						&templ->rec_prefix_field_no);
		}
	} else {
		DBUG_ASSERT(!ha_innobase::omits_virtual_cols(*table->s));
		col = &dict_table_get_nth_v_col(index->table, v_no)->m_col;
		templ->clust_rec_field_no = v_no;

		if (dict_index_is_clust(index)) {
			templ->rec_field_no = templ->clust_rec_field_no;
		} else {
			templ->rec_field_no
				= dict_index_get_nth_col_or_prefix_pos(
					index, v_no, FALSE, true,
					&templ->rec_prefix_field_no);
		}
		templ->icp_rec_field_no = ULINT_UNDEFINED;
	}

	if (field->real_maybe_null()) {
		templ->mysql_null_byte_offset =
			field->null_offset();

		templ->mysql_null_bit_mask = (ulint) field->null_bit;
	} else {
		templ->mysql_null_bit_mask = 0;
	}


	templ->mysql_col_offset = (ulint) get_field_offset(table, field);
	templ->mysql_col_len = (ulint) field->pack_length();
	templ->type = col->mtype;
	templ->mysql_type = (ulint) field->type();

	if (templ->mysql_type == DATA_MYSQL_TRUE_VARCHAR) {
		templ->mysql_length_bytes = (ulint)
			(((Field_varstring*) field)->length_bytes);
	} else {
		templ->mysql_length_bytes = 0;
	}

	templ->charset = dtype_get_charset_coll(col->prtype);
	templ->mbminlen = dict_col_get_mbminlen(col);
	templ->mbmaxlen = dict_col_get_mbmaxlen(col);
	templ->is_unsigned = col->prtype & DATA_UNSIGNED;

	if (!dict_index_is_clust(index)
	    && templ->rec_field_no == ULINT_UNDEFINED) {
		prebuilt->need_to_access_clustered = TRUE;

		if (templ->rec_prefix_field_no != ULINT_UNDEFINED) {
			dict_field_t* field = dict_index_get_nth_field(
						index,
						templ->rec_prefix_field_no);
			templ->rec_field_is_prefix = (field->prefix_len != 0);
		}
	}

	/* For spatial index, we need to access cluster index. */
	if (dict_index_is_spatial(index)) {
		prebuilt->need_to_access_clustered = TRUE;
	}

	if (prebuilt->mysql_prefix_len < templ->mysql_col_offset
	    + templ->mysql_col_len) {
		prebuilt->mysql_prefix_len = templ->mysql_col_offset
			+ templ->mysql_col_len;
	}

	if (DATA_LARGE_MTYPE(templ->type)) {
		prebuilt->templ_contains_blob = TRUE;
	}

	return(templ);
}

/**************************************************************//**
Builds a 'template' to the m_prebuilt struct. The template is used in fast
retrieval of just those column values MySQL needs in its processing. */

void
ha_innobase::build_template(
/*========================*/
	bool		whole_row)	/*!< in: true=ROW_MYSQL_WHOLE_ROW,
					false=ROW_MYSQL_REC_FIELDS */
{
	dict_index_t*	index;
	dict_index_t*	clust_index;
	ibool		fetch_all_in_key	= FALSE;
	ibool		fetch_primary_key_cols	= FALSE;

	if (m_prebuilt->select_lock_type == LOCK_X || m_prebuilt->table->no_rollback()) {
		/* We always retrieve the whole clustered index record if we
		use exclusive row level locks, for example, if the read is
		done in an UPDATE statement or if we are using a no rollback
                table */

		whole_row = true;
	} else if (!whole_row) {
		if (m_prebuilt->hint_need_to_fetch_extra_cols
			== ROW_RETRIEVE_ALL_COLS) {

			/* We know we must at least fetch all columns in the
			key, or all columns in the table */

			if (m_prebuilt->read_just_key) {
				/* MySQL has instructed us that it is enough
				to fetch the columns in the key; looks like
				MySQL can set this flag also when there is
				only a prefix of the column in the key: in
				that case we retrieve the whole column from
				the clustered index */

				fetch_all_in_key = TRUE;
			} else {
				whole_row = true;
			}
		} else if (m_prebuilt->hint_need_to_fetch_extra_cols
			== ROW_RETRIEVE_PRIMARY_KEY) {
			/* We must at least fetch all primary key cols. Note
			that if the clustered index was internally generated
			by InnoDB on the row id (no primary key was
			defined), then row_search_mvcc() will always
			retrieve the row id to a special buffer in the
			m_prebuilt struct. */

			fetch_primary_key_cols = TRUE;
		}
	}

	clust_index = dict_table_get_first_index(m_prebuilt->table);

	index = whole_row ? clust_index : m_prebuilt->index;

	m_prebuilt->versioned_write = table->versioned_write(VERS_TRX_ID);
	m_prebuilt->need_to_access_clustered = (index == clust_index);

	if (m_prebuilt->in_fts_query) {
		/* Do clustered index lookup to fetch the FTS_DOC_ID */
		m_prebuilt->need_to_access_clustered = true;
	}

	/* Either m_prebuilt->index should be a secondary index, or it
	should be the clustered index. */
	ut_ad(dict_index_is_clust(index) == (index == clust_index));

	/* Below we check column by column if we need to access
	the clustered index. */

	if (pushed_rowid_filter && rowid_filter_is_active
	    && !m_disable_rowid_filter) {
		fetch_primary_key_cols = TRUE;
		m_prebuilt->pk_filter = this;
	} else {
		m_prebuilt->pk_filter = NULL;
	}

	const bool skip_virtual = omits_virtual_cols(*table_share);
	const ulint n_fields = table_share->fields;

	if (!m_prebuilt->mysql_template) {
		m_prebuilt->mysql_template = (mysql_row_templ_t*)
			ut_malloc_nokey(n_fields * sizeof(mysql_row_templ_t));
	}

	m_prebuilt->template_type = whole_row
		? ROW_MYSQL_WHOLE_ROW : ROW_MYSQL_REC_FIELDS;
	m_prebuilt->null_bitmap_len = table->s->null_bytes
		& dict_index_t::MAX_N_FIELDS;

	/* Prepare to build m_prebuilt->mysql_template[]. */
	m_prebuilt->templ_contains_blob = FALSE;
	m_prebuilt->mysql_prefix_len = 0;
	m_prebuilt->n_template = 0;
	m_prebuilt->idx_cond_n_cols = 0;

	/* Note that in InnoDB, i is the column number in the table.
	MySQL calls columns 'fields'. */

	ulint num_v = 0;

	/* MDEV-31154: For pushed down index condition we don't support virtual
	column and idx_cond_push() does check for it. For row ID filtering we
	don't need such restrictions but we get into trouble trying to use the
	ICP path.

	1. It should be fine to follow no_icp path if primary key is generated.
	However, with user specified primary key(PK), the row is identified by
	the PK and those columns need to be converted to mysql format in
	row_search_idx_cond_check before doing the comparison. Since secondary
	indexes always have PK appended in innodb, it works with current ICP
	handling code when fetch_primary_key_cols is set to TRUE.

	2. Although ICP comparison and Row ID comparison works on different
	columns the current ICP code can be shared by both.

	3. In most cases, it works today by jumping to goto no_icp when we
	encounter a virtual column. This is hackish and already have some
	issues as it cannot handle PK and all states are not reset properly,
	for example, idx_cond_n_cols is not reset.

	4. We already encountered MDEV-28747 m_prebuilt->idx_cond was being set.

	Neither ICP nor row ID comparison needs virtual columns and the code is
	simplified to handle both. It should handle the issues. */

	const bool pushed_down = active_index != MAX_KEY
				 && active_index == pushed_idx_cond_keyno
				 && !m_disable_rowid_filter;

	m_prebuilt->idx_cond = pushed_down ? this : nullptr;

	if (m_prebuilt->idx_cond || m_prebuilt->pk_filter) {
		/* Push down an index condition, end_range check or row ID
		filter */
		for (ulint i = 0; i < n_fields; i++) {
			const Field* field = table->field[i];
			const bool is_v = !field->stored_in_db();

			bool index_contains = index->contains_col_or_prefix(
				is_v ? num_v : i - num_v, is_v);

			if (is_v) {
				if (index_contains) {
					/* We want to ensure that ICP is not
					used with virtual columns. */
					ut_ad(!pushed_down);
					m_prebuilt->idx_cond = nullptr;
				}
				num_v++;
				continue;
			}

			/* Test if an end_range or an index condition
			refers to the field. Note that "index" and
			"index_contains" may refer to the clustered index.
			Index condition pushdown is relative to
			m_prebuilt->index (the index that is being
			looked up first). */

			/* When join_read_always_key() invokes this
			code via handler::ha_index_init() and
			ha_innobase::index_init(), end_range is not
			yet initialized. Because of that, we must
			always check for index_contains, instead of
			the subset
			field->part_of_key.is_set(active_index)
			which would be acceptable if end_range==NULL. */
			if (build_template_needs_field_in_icp(
				    index, m_prebuilt, index_contains,
				    i - num_v, false)) {
				if (!whole_row) {
					field = build_template_needs_field(
						index_contains,
						m_prebuilt->read_just_key,
						fetch_all_in_key,
						fetch_primary_key_cols,
						index, table, i, num_v);
					if (!field) {
						continue;
					}
				}

				mysql_row_templ_t* templ= build_template_field(
					m_prebuilt, clust_index, index,
					table, field, i - num_v, 0);

				ut_ad(!templ->is_virtual);

				m_prebuilt->idx_cond_n_cols++;
				ut_ad(m_prebuilt->idx_cond_n_cols
				      == m_prebuilt->n_template);

				if (index == m_prebuilt->index) {
					templ->icp_rec_field_no
						= templ->rec_field_no;
				} else {
					templ->icp_rec_field_no
						= dict_index_get_nth_col_pos(
							m_prebuilt->index,
							i - num_v,
							&templ->rec_prefix_field_no);
				}

				if (dict_index_is_clust(m_prebuilt->index)) {
					ut_ad(templ->icp_rec_field_no
					      != ULINT_UNDEFINED);
					/* If the primary key includes
					a column prefix, use it in
					index condition pushdown,
					because the condition is
					evaluated before fetching any
					off-page (externally stored)
					columns. */
					if (templ->icp_rec_field_no
					    < m_prebuilt->index->n_uniq) {
						/* This is a key column;
						all set. */
						continue;
					}
				} else if (templ->icp_rec_field_no
					   != ULINT_UNDEFINED) {
					continue;
				}

				/* This is a column prefix index.
				The column prefix can be used in
				an end_range comparison. */

				templ->icp_rec_field_no
					= dict_index_get_nth_col_or_prefix_pos(
						m_prebuilt->index, i - num_v,
						true, false,
						&templ->rec_prefix_field_no);
				ut_ad(templ->icp_rec_field_no
				      != ULINT_UNDEFINED);

				/* Index condition pushdown can be used on
				all columns of a secondary index, and on
				the PRIMARY KEY columns. On the clustered
				index, it must never be used on other than
				PRIMARY KEY columns, because those columns
				may be stored off-page, and we will not
				fetch externally stored columns before
				checking the index condition. */
				/* TODO: test the above with an assertion
				like this. Note that index conditions are
				currently pushed down as part of the
				"optimizer phase" while end_range is done
				as part of the execution phase. Therefore,
				we were unable to use an accurate condition
				for end_range in the "if" condition above,
				and the following assertion would fail.
				ut_ad(!dict_index_is_clust(m_prebuilt->index)
				      || templ->rec_field_no
				      < m_prebuilt->index->n_uniq);
				*/
			}

		}

		num_v = 0;
		ut_ad(m_prebuilt->idx_cond_n_cols == m_prebuilt->n_template);
		if (m_prebuilt->idx_cond_n_cols == 0) {
			/* No columns to push down. It is safe to jump to np ICP
			path. */
			m_prebuilt->idx_cond = nullptr;
			goto no_icp;
		}

		/* Include the fields that are not needed in index condition
		pushdown. */
		for (ulint i = 0; i < n_fields; i++) {
			const Field*		field = table->field[i];
			const bool is_v = !field->stored_in_db();
			if (is_v && skip_virtual) {
				num_v++;
				continue;
			}

			bool index_contains = index->contains_col_or_prefix(
				is_v ? num_v : i - num_v, is_v);

			if (is_v || !build_template_needs_field_in_icp(
				    index, m_prebuilt, index_contains,
				    is_v ? num_v : i - num_v, is_v)) {
				/* Not needed in ICP */
				if (!whole_row) {
					field = build_template_needs_field(
						index_contains,
						m_prebuilt->read_just_key,
						fetch_all_in_key,
						fetch_primary_key_cols,
						index, table, i, num_v);
					if (!field) {
						if (is_v) {
							num_v++;
						}
						continue;
					}
				}

				ut_d(mysql_row_templ_t*	templ =)
				build_template_field(
					m_prebuilt, clust_index, index,
					table, field, i - num_v, num_v);
				ut_ad(templ->is_virtual == (ulint)is_v);

				if (is_v) {
					num_v++;
				}
			}
		}
	} else {
no_icp:
		/* No index condition pushdown */
		ut_ad(!m_prebuilt->idx_cond);
		ut_ad(num_v == 0);

		for (ulint i = 0; i < n_fields; i++) {
			const Field*	field = table->field[i];
			const bool is_v = !field->stored_in_db();

			if (whole_row) {
				if (is_v && skip_virtual) {
					num_v++;
					continue;
				}
				/* Even this is whole_row, if the seach is
				on a virtual column, and read_just_key is
				set, and field is not in this index, we
				will not try to fill the value since they
				are not stored in such index nor in the
				cluster index. */
				if (is_v
				    && m_prebuilt->read_just_key
				    && !m_prebuilt->index->contains_col_or_prefix(
					num_v, true))
				{
					/* Turn off ROW_MYSQL_WHOLE_ROW */
					m_prebuilt->template_type =
						 ROW_MYSQL_REC_FIELDS;
					num_v++;
					continue;
				}
			} else {
				if (is_v
				    && (skip_virtual || index->is_primary())) {
					num_v++;
					continue;
				}

				bool contain = index->contains_col_or_prefix(
					is_v ? num_v: i - num_v, is_v);

				field = build_template_needs_field(
					contain,
					m_prebuilt->read_just_key,
					fetch_all_in_key,
					fetch_primary_key_cols,
					index, table, i, num_v);
				if (!field) {
					if (is_v) {
						num_v++;
					}
					continue;
				}
			}

			ut_d(mysql_row_templ_t* templ =)
			build_template_field(
				m_prebuilt, clust_index, index,
				table, field, i - num_v, num_v);
			ut_ad(templ->is_virtual == (ulint)is_v);
			if (is_v) {
				num_v++;
			}
		}
	}

	if (index != clust_index && m_prebuilt->need_to_access_clustered) {
		/* Change rec_field_no's to correspond to the clustered index
		record */
		for (ulint i = 0; i < m_prebuilt->n_template; i++) {
			mysql_row_templ_t*	templ
				= &m_prebuilt->mysql_template[i];

			templ->rec_field_no = templ->clust_rec_field_no;
		}
	}
}

/********************************************************************//**
This special handling is really to overcome the limitations of MySQL's
binlogging. We need to eliminate the non-determinism that will arise in
INSERT ... SELECT type of statements, since MySQL binlog only stores the
min value of the autoinc interval. Once that is fixed we can get rid of
the special lock handling.
@return DB_SUCCESS if all OK else error code */

dberr_t
ha_innobase::innobase_lock_autoinc(void)
/*====================================*/
{
	DBUG_ENTER("ha_innobase::innobase_lock_autoinc");
	dberr_t		error = DB_SUCCESS;

	ut_ad(!srv_read_only_mode);

	switch (innobase_autoinc_lock_mode) {
	case AUTOINC_NO_LOCKING:
		/* Acquire only the AUTOINC mutex. */
		m_prebuilt->table->autoinc_mutex.wr_lock();
		break;

	case AUTOINC_NEW_STYLE_LOCKING:
		/* For simple (single/multi) row INSERTs/REPLACEs and RBR
		events, we fallback to the old style only if another
		transaction has already acquired the AUTOINC lock on
		behalf of a LOAD FILE or INSERT ... SELECT etc. type of
		statement. */
		switch (thd_sql_command(m_user_thd)) {
		case SQLCOM_INSERT:
		case SQLCOM_REPLACE:
		case SQLCOM_END: // RBR event
			/* Acquire the AUTOINC mutex. */
			m_prebuilt->table->autoinc_mutex.wr_lock();
			/* We need to check that another transaction isn't
			already holding the AUTOINC lock on the table. */
			if (!m_prebuilt->table->n_waiting_or_granted_auto_inc_locks) {
				/* Do not fall back to old style locking. */
				DBUG_RETURN(error);
			}
			m_prebuilt->table->autoinc_mutex.wr_unlock();
		}
		/* Use old style locking. */
		/* fall through */
	case AUTOINC_OLD_STYLE_LOCKING:
		DBUG_EXECUTE_IF("die_if_autoinc_old_lock_style_used",
				ut_ad(0););
		error = row_lock_table_autoinc_for_mysql(m_prebuilt);

		if (error == DB_SUCCESS) {

			/* Acquire the AUTOINC mutex. */
			m_prebuilt->table->autoinc_mutex.wr_lock();
		}
		break;

	default:
		ut_error;
	}

	DBUG_RETURN(error);
}

/********************************************************************//**
Store the autoinc value in the table. The autoinc value is only set if
it's greater than the existing autoinc value in the table.
@return DB_SUCCESS if all went well else error code */

dberr_t
ha_innobase::innobase_set_max_autoinc(
/*==================================*/
	ulonglong	auto_inc)	/*!< in: value to store */
{
	dberr_t		error;

	error = innobase_lock_autoinc();

	if (error == DB_SUCCESS) {

		dict_table_autoinc_update_if_greater(m_prebuilt->table, auto_inc);
		m_prebuilt->table->autoinc_mutex.wr_unlock();
	}

	return(error);
}

int ha_innobase::is_valid_trx(bool altering_to_supported) const noexcept
{
  ut_ad(m_prebuilt->trx == thd_to_trx(m_user_thd));

  if (high_level_read_only)
  {
    ib_senderrf(m_user_thd, IB_LOG_LEVEL_WARN, ER_READ_ONLY_MODE);
    return HA_ERR_TABLE_READONLY;
  }

  switch (UNIV_EXPECT(m_prebuilt->trx->state, TRX_STATE_ACTIVE)) {
  default:
    ut_ad("invalid state" == 0);
    /* fall through */
  case TRX_STATE_ABORTED:
    break;
  case TRX_STATE_NOT_STARTED:
    m_prebuilt->trx->will_lock= true;
    /* fall through */
  case TRX_STATE_ACTIVE:
    if (altering_to_supported ||
        !DICT_TF_GET_ZIP_SSIZE(m_prebuilt->table->flags) ||
        !innodb_read_only_compressed)
      return 0;

    ib_senderrf(m_user_thd, IB_LOG_LEVEL_WARN, ER_UNSUPPORTED_COMPRESSED_TABLE);
    return HA_ERR_TABLE_READONLY;
  }
  return HA_ERR_ROLLBACK;
}

/********************************************************************//**
Stores a row in an InnoDB database, to the table specified in this
handle.
@return error code */

int
ha_innobase::write_row(
/*===================*/
	const uchar*	record)	/*!< in: a row in MySQL format */
{
	dberr_t		error;
#ifdef WITH_WSREP
	bool		wsrep_auto_inc_inserted= false;
#endif
	int		error_result = 0;
	bool		auto_inc_used = false;
	mariadb_set_stats set_stats_temporary(handler_stats);

	DBUG_ENTER("ha_innobase::write_row");

	trx_t*		trx = thd_to_trx(m_user_thd);

	/* Validation checks before we commence write_row operation. */
	if (int err = is_valid_trx()) {
		DBUG_RETURN(err);
	}

	ins_mode_t	vers_set_fields;
	/* Handling of Auto-Increment Columns. */
	if (table->next_number_field && record == table->record[0]) {

		/* Reset the error code before calling
		innobase_get_auto_increment(). */
		m_prebuilt->autoinc_error = DB_SUCCESS;

#ifdef WITH_WSREP
		wsrep_auto_inc_inserted = trx->is_wsrep()
			&& wsrep_drupal_282555_workaround
			&& table->next_number_field->val_int() == 0;
#endif

		if ((error_result = update_auto_increment())) {
			/* MySQL errors are passed straight back. */
			goto func_exit;
		}

		auto_inc_used = true;
	}

	/* Prepare INSERT graph that will be executed for actual INSERT
	(This is a one time operation) */
	if (m_prebuilt->mysql_template == NULL
	    || m_prebuilt->template_type != ROW_MYSQL_WHOLE_ROW) {

		/* Build the template used in converting quickly between
		the two database formats */

		build_template(true);
	}

	vers_set_fields = table->versioned_write(VERS_TRX_ID) ?
		ROW_INS_VERSIONED : ROW_INS_NORMAL;

	/* Execute insert graph that will result in actual insert. */
	error = row_insert_for_mysql((byte*) record, m_prebuilt, vers_set_fields);

	DEBUG_SYNC(m_user_thd, "ib_after_row_insert");

	/* Handling of errors related to auto-increment. */
	if (auto_inc_used) {
		ulonglong	auto_inc;

		/* Note the number of rows processed for this statement, used
		by get_auto_increment() to determine the number of AUTO-INC
		values to reserve. This is only useful for a mult-value INSERT
		and is a statement level counter. */
		if (trx->n_autoinc_rows > 0) {
			--trx->n_autoinc_rows;
		}

		/* Get the value that MySQL attempted to store in the table.*/
		auto_inc = table->next_number_field->val_uint();

		switch (error) {
		case DB_DUPLICATE_KEY:

			/* A REPLACE command and LOAD DATA INFILE REPLACE
			handle a duplicate key error themselves, but we
			must update the autoinc counter if we are performing
			those statements. */

			switch (thd_sql_command(m_user_thd)) {
			case SQLCOM_LOAD:
				if (!trx->duplicates) {
					break;
				}

			case SQLCOM_REPLACE:
			case SQLCOM_INSERT_SELECT:
			case SQLCOM_REPLACE_SELECT:
				goto set_max_autoinc;

#ifdef WITH_WSREP
			/* workaround for LP bug #355000, retrying the insert */
			case SQLCOM_INSERT:

				WSREP_DEBUG("DUPKEY error for autoinc\n"
				      "THD %ld, value %llu, off %llu inc %llu",
				      thd_get_thread_id(m_user_thd),
				      auto_inc,
				      m_prebuilt->autoinc_offset,
				      m_prebuilt->autoinc_increment);

                               if (wsrep_auto_inc_inserted &&
                                   wsrep_thd_retry_counter(m_user_thd) == 0  &&
				    !thd_test_options(m_user_thd,
						      OPTION_NOT_AUTOCOMMIT |
						      OPTION_BEGIN)) {
					WSREP_DEBUG(
					    "retrying insert: %s",
					    wsrep_thd_query(m_user_thd));
					error= DB_SUCCESS;
					wsrep_thd_self_abort(m_user_thd);
                                        /* jump straight to func exit over
                                         * later wsrep hooks */
                                        goto func_exit;
				}
                                break;
#endif /* WITH_WSREP */

			default:
				break;
			}

			break;

		case DB_SUCCESS:
			/* If the actual value inserted is greater than
			the upper limit of the interval, then we try and
			update the table upper limit. Note: last_value
			will be 0 if get_auto_increment() was not called. */

			if (auto_inc >= m_prebuilt->autoinc_last_value) {
set_max_autoinc:
				/* We need the upper limit of the col type to check for
				whether we update the table autoinc counter or not. */
				ulonglong	col_max_value =
					table->next_number_field->get_max_int_value();

				/* This should filter out the negative
				values set explicitly by the user. */
				if (auto_inc <= col_max_value) {
					ut_ad(m_prebuilt->autoinc_increment > 0);

					ulonglong	offset;
					ulonglong	increment;
					dberr_t		err;

					offset = m_prebuilt->autoinc_offset;
					increment = m_prebuilt->autoinc_increment;

					auto_inc = innobase_next_autoinc(
						auto_inc, 1, increment, offset,
						col_max_value);

					err = innobase_set_max_autoinc(
						auto_inc);

					if (err != DB_SUCCESS) {
						error = err;
					}
				}
			}
			break;
		default:
			break;
		}
	}

	/* Cleanup and exit. */
	if (error == DB_TABLESPACE_DELETED) {
		ib_senderrf(
			trx->mysql_thd, IB_LOG_LEVEL_ERROR,
			ER_TABLESPACE_DISCARDED,
			table->s->table_name.str);
	}

	error_result = convert_error_code_to_mysql(
		error, m_prebuilt->table->flags, m_user_thd);

#ifdef WITH_WSREP
#ifdef ENABLED_DEBUG_SYNC
	DBUG_EXECUTE_IF("sync.wsrep_after_write_row",
	{
	  const char act[]=
	    "now "
	    "SIGNAL sync.wsrep_after_write_row_reached "
	    "WAIT_FOR signal.wsrep_after_write_row";
	  DBUG_ASSERT(!debug_sync_set_action(m_user_thd, STRING_WITH_LEN(act)));
	};);
#endif /* ENABLED_DEBUG_SYNC */

	if (!error_result && trx->is_wsrep()
	    && !trx->is_bulk_insert()
	    && wsrep_thd_is_local(m_user_thd)
	    && !wsrep_thd_ignore_table(m_user_thd)
	    && !wsrep_consistency_check(m_user_thd)
	    && (thd_sql_command(m_user_thd) != SQLCOM_CREATE_TABLE)
	    && (thd_sql_command(m_user_thd) != SQLCOM_LOAD ||
	        thd_binlog_format(m_user_thd) == BINLOG_FORMAT_ROW)) {
		if (wsrep_append_keys(m_user_thd, WSREP_SERVICE_KEY_EXCLUSIVE,
				      record,
				      NULL)) {
			WSREP_DEBUG("::write_rows::wsrep_append_keys failed THD %ld for %s.%s",
				    thd_get_thread_id(m_user_thd),
				    table->s->db.str,
				    table->s->table_name.str);
			error_result = HA_ERR_INTERNAL_ERROR;
			goto func_exit;
		}
	}
#endif /* WITH_WSREP */

	if (error_result == HA_FTS_INVALID_DOCID) {
		my_error(HA_FTS_INVALID_DOCID, MYF(0));
	}

func_exit:
	DBUG_RETURN(error_result);
}

/** Fill the update vector's "old_vrow" field for those non-updated,
but indexed columns. Such columns could still be present in the virtual
index rec fields even if they are not updated (some other fields updated),
so they need to be logged.
@param[in]	prebuilt		InnoDB prebuilt struct
@param[in,out]	vfield			field to filled
@param[in]	o_len			actual column length
@param[in,out]	col			column to be filled
@param[in]	old_mysql_row_col	MySQL old field ptr
@param[in]	col_pack_len		MySQL field col length
@param[in,out]	buf			buffer for a converted integer value
@return used buffer ptr from row_mysql_store_col_in_innobase_format() */
static
byte*
innodb_fill_old_vcol_val(
	row_prebuilt_t*	prebuilt,
	dfield_t*	vfield,
	ulint		o_len,
	dict_col_t*	col,
	const byte*	old_mysql_row_col,
	ulint		col_pack_len,
	byte*		buf)
{
	dict_col_copy_type(
		col, dfield_get_type(vfield));
	if (o_len != UNIV_SQL_NULL) {

		buf = row_mysql_store_col_in_innobase_format(
			vfield,
			buf,
			TRUE,
			old_mysql_row_col,
			col_pack_len,
			dict_table_is_comp(prebuilt->table));
	} else {
		dfield_set_null(vfield);
	}

	return(buf);
}

/** Calculate an update vector corresponding to the changes
between old_row and new_row.
@param[out]	uvect		update vector
@param[in]	old_row		current row in MySQL format
@param[in]	new_row		intended updated row in MySQL format
@param[in]	table		MySQL table handle
@param[in,out]	upd_buff	buffer to use for converted values
@param[in]	buff_len	length of upd_buff
@param[in,out]	prebuilt	InnoDB execution context
@param[out]	auto_inc	updated AUTO_INCREMENT value, or 0 if none
@return DB_SUCCESS or error code */
static
dberr_t
calc_row_difference(
	upd_t*		uvect,
	const uchar*	old_row,
	const uchar*	new_row,
	TABLE*		table,
	uchar*		upd_buff,
	ulint		buff_len,
	row_prebuilt_t*	prebuilt,
	ib_uint64_t&	auto_inc)
{
	uchar*		original_upd_buff = upd_buff;
	Field*		field;
	enum_field_types field_mysql_type;
	ulint		o_len;
	ulint		n_len;
	ulint		col_pack_len;
	const byte*	new_mysql_row_col;
	const byte*	old_mysql_row_col;
	const byte*	o_ptr;
	const byte*	n_ptr;
	byte*		buf;
	upd_field_t*	ufield;
	ulint		col_type;
	ulint		n_changed = 0;
	dfield_t	dfield;
	dict_index_t*	clust_index;
	ibool		changes_fts_column = FALSE;
	ibool		changes_fts_doc_col = FALSE;
	trx_t* const	trx = prebuilt->trx;
	doc_id_t	doc_id = FTS_NULL_DOC_ID;
	uint16_t	num_v = 0;
#ifndef DBUG_OFF
	uint		vers_fields = 0;
#endif
	prebuilt->versioned_write = table->versioned_write(VERS_TRX_ID);
	const bool skip_virtual = ha_innobase::omits_virtual_cols(*table->s);

	ut_ad(!srv_read_only_mode);

	clust_index = dict_table_get_first_index(prebuilt->table);
	auto_inc = 0;

	/* We use upd_buff to convert changed fields */
	buf = (byte*) upd_buff;

	for (uint i = 0; i < table->s->fields; i++) {
		field = table->field[i];

#ifndef DBUG_OFF
		if (!field->vers_sys_field()
		    && !field->vers_update_unversioned()) {
			++vers_fields;
		}
#endif

		const bool is_virtual = !field->stored_in_db();
		if (is_virtual && skip_virtual) {
			num_v++;
			continue;
		}
		dict_col_t* col = is_virtual
			? &prebuilt->table->v_cols[num_v].m_col
			: &prebuilt->table->cols[i - num_v];

		o_ptr = (const byte*) old_row + get_field_offset(table, field);
		n_ptr = (const byte*) new_row + get_field_offset(table, field);

		/* Use new_mysql_row_col and col_pack_len save the values */

		new_mysql_row_col = n_ptr;
		old_mysql_row_col = o_ptr;
		col_pack_len = field->pack_length();

		o_len = col_pack_len;
		n_len = col_pack_len;

		/* We use o_ptr and n_ptr to dig up the actual data for
		comparison. */

		field_mysql_type = field->type();

		col_type = col->mtype;

		switch (col_type) {

		case DATA_BLOB:
		case DATA_GEOMETRY:
			o_ptr = row_mysql_read_blob_ref(&o_len, o_ptr, o_len);
			n_ptr = row_mysql_read_blob_ref(&n_len, n_ptr, n_len);

			break;

		case DATA_VARCHAR:
		case DATA_BINARY:
		case DATA_VARMYSQL:
			if (field_mysql_type == MYSQL_TYPE_VARCHAR) {
				/* This is a >= 5.0.3 type true VARCHAR where
				the real payload data length is stored in
				1 or 2 bytes */

				o_ptr = row_mysql_read_true_varchar(
					&o_len, o_ptr,
					(ulint)
					(((Field_varstring*) field)->length_bytes));

				n_ptr = row_mysql_read_true_varchar(
					&n_len, n_ptr,
					(ulint)
					(((Field_varstring*) field)->length_bytes));
			}

			break;
		default:
			;
		}

		if (field_mysql_type == MYSQL_TYPE_LONGLONG
		    && prebuilt->table->fts
		    && field->field_name.streq(FTS_DOC_ID)) {
			doc_id = mach_read_uint64_little_endian(n_ptr);
			if (doc_id == 0) {
				return(DB_FTS_INVALID_DOCID);
			}
		}

		if (field->real_maybe_null()) {
			if (field->is_null_in_record(old_row)) {
				o_len = UNIV_SQL_NULL;
			}

			if (field->is_null_in_record(new_row)) {
				n_len = UNIV_SQL_NULL;
			}
		}

		if (is_virtual) {
			/* If the virtual column is not indexed,
			we shall ignore it for update */
			if (!col->ord_part) {
			next:
				num_v++;
				continue;
			}

			if (!uvect->old_vrow) {
				uvect->old_vrow = dtuple_create_with_vcol(
					uvect->heap, 0, prebuilt->table->n_v_cols);
			}

			ulint   max_field_len = DICT_MAX_FIELD_LEN_BY_FORMAT(
						prebuilt->table);

			/* for virtual columns, we only materialize
			its index, and index field length would not
			exceed max_field_len. So continue if the
			first max_field_len bytes are matched up */
			if (o_len != UNIV_SQL_NULL
			   && n_len != UNIV_SQL_NULL
			   && o_len >= max_field_len
			   && n_len >= max_field_len
			   && memcmp(o_ptr, n_ptr, max_field_len) == 0) {
				dfield_t*	vfield = dtuple_get_nth_v_field(
					uvect->old_vrow, num_v);
				buf = innodb_fill_old_vcol_val(
					prebuilt, vfield, o_len,
					col, old_mysql_row_col,
					col_pack_len, buf);
				goto next;
			}
		}

		if (o_len != n_len || (o_len != 0 && o_len != UNIV_SQL_NULL
				       && 0 != memcmp(o_ptr, n_ptr, o_len))) {
			/* The field has changed */

			ufield = uvect->fields + n_changed;
			MEM_UNDEFINED(ufield, sizeof *ufield);

			/* Let us use a dummy dfield to make the conversion
			from the MySQL column format to the InnoDB format */


			/* If the length of new geometry object is 0, means
			this object is invalid geometry object, we need
			to block it. */
			if (DATA_GEOMETRY_MTYPE(col_type)
			    && o_len != 0 && n_len == 0) {
				trx->error_info = clust_index;
				return(DB_CANT_CREATE_GEOMETRY_OBJECT);
			}

			if (n_len != UNIV_SQL_NULL) {
				dict_col_copy_type(
					col, dfield_get_type(&dfield));

				buf = row_mysql_store_col_in_innobase_format(
					&dfield,
					(byte*) buf,
					TRUE,
					new_mysql_row_col,
					col_pack_len,
					dict_table_is_comp(prebuilt->table));
				dfield_copy(&ufield->new_val, &dfield);
			} else {
				dict_col_copy_type(
					col, dfield_get_type(&ufield->new_val));
				dfield_set_null(&ufield->new_val);
			}

			ufield->exp = NULL;
			ufield->orig_len = 0;
			if (is_virtual) {
				dfield_t*	vfield = dtuple_get_nth_v_field(
					uvect->old_vrow, num_v);
				upd_fld_set_virtual_col(ufield);
				ufield->field_no = num_v;

				ut_ad(col->ord_part);
				ufield->old_v_val = static_cast<dfield_t*>(
					mem_heap_alloc(
						uvect->heap,
						sizeof *ufield->old_v_val));

				if (!field->is_null_in_record(old_row)) {
					if (n_len == UNIV_SQL_NULL) {
						dict_col_copy_type(
							col, dfield_get_type(
								&dfield));
					}

					buf = row_mysql_store_col_in_innobase_format(
						&dfield,
						(byte*) buf,
						TRUE,
						old_mysql_row_col,
						col_pack_len,
						dict_table_is_comp(
						prebuilt->table));
					dfield_copy(ufield->old_v_val,
						    &dfield);
					dfield_copy(vfield, &dfield);
				} else {
					dict_col_copy_type(
						col, dfield_get_type(
						ufield->old_v_val));
					dfield_set_null(ufield->old_v_val);
					dfield_set_null(vfield);
				}
				num_v++;
				ut_ad(field != table->found_next_number_field);
			} else {
				ufield->field_no = static_cast<uint16_t>(
					dict_col_get_clust_pos(
						&prebuilt->table->cols
						[i - num_v],
						clust_index));
				ufield->old_v_val = NULL;
				if (field != table->found_next_number_field
				    || dfield_is_null(&ufield->new_val)) {
				} else {
					auto_inc = field->val_uint();
				}
			}
			n_changed++;

			/* If an FTS indexed column was changed by this
			UPDATE then we need to inform the FTS sub-system.

			NOTE: Currently we re-index all FTS indexed columns
			even if only a subset of the FTS indexed columns
			have been updated. That is the reason we are
			checking only once here. Later we will need to
			note which columns have been updated and do
			selective processing. */
			if (prebuilt->table->fts != NULL && !is_virtual) {
				ulint		offset;
				dict_table_t*   innodb_table;

				innodb_table = prebuilt->table;

				if (!changes_fts_column) {
					offset = row_upd_changes_fts_column(
						innodb_table, ufield);

					if (offset != ULINT_UNDEFINED) {
						changes_fts_column = TRUE;
					}
				}

				if (!changes_fts_doc_col) {
					changes_fts_doc_col =
					row_upd_changes_doc_id(
						innodb_table, ufield);
				}
			}
		} else if (is_virtual) {
			dfield_t*	vfield = dtuple_get_nth_v_field(
				uvect->old_vrow, num_v);
			buf = innodb_fill_old_vcol_val(
				prebuilt, vfield, o_len,
				col, old_mysql_row_col,
				col_pack_len, buf);
			ut_ad(col->ord_part);
			num_v++;
		}
	}

	/* If the update changes a column with an FTS index on it, we
	then add an update column node with a new document id to the
	other changes. We piggy back our changes on the normal UPDATE
	to reduce processing and IO overhead. */
	if (!prebuilt->table->fts) {
		trx->fts_next_doc_id = 0;
	} else if (changes_fts_column || changes_fts_doc_col) {
		dict_table_t*   innodb_table = prebuilt->table;

		ufield = uvect->fields + n_changed;

		if (!DICT_TF2_FLAG_IS_SET(
			innodb_table, DICT_TF2_FTS_HAS_DOC_ID)) {

			/* If Doc ID is managed by user, and if any
			FTS indexed column has been updated, its corresponding
			Doc ID must also be updated. Otherwise, return
			error */
			if (changes_fts_column && !changes_fts_doc_col) {
				ib::warn() << "A new Doc ID must be supplied"
					" while updating FTS indexed columns.";
				return(DB_FTS_INVALID_DOCID);
			}

			/* Doc ID must monotonically increase */
			ut_ad(innodb_table->fts->cache);
			if (doc_id < prebuilt->table->fts->cache->next_doc_id) {

				ib::warn() << "FTS Doc ID must be larger than "
					<< innodb_table->fts->cache->next_doc_id
					- 1  << " for table "
					<< innodb_table->name;

				return(DB_FTS_INVALID_DOCID);
			}


			trx->fts_next_doc_id = doc_id;
		} else {
			/* If the Doc ID is a hidden column, it can't be
			changed by user */
			ut_ad(!changes_fts_doc_col);

			/* Doc ID column is hidden, a new Doc ID will be
			generated by following fts_update_doc_id() call */
			trx->fts_next_doc_id = 0;
		}

		fts_update_doc_id(
			innodb_table, ufield, &trx->fts_next_doc_id);

		++n_changed;
	} else {
		/* We have a Doc ID column, but none of FTS indexed
		columns are touched, nor the Doc ID column, so set
		fts_next_doc_id to UINT64_UNDEFINED, which means do not
		update the Doc ID column */
		trx->fts_next_doc_id = UINT64_UNDEFINED;
	}

	uvect->n_fields = n_changed;
	uvect->info_bits = 0;

	ut_a(buf <= (byte*) original_upd_buff + buff_len);

	if (const TABLE_LIST *tl= table->pos_in_table_list)
	{
		const uint8 op_map= tl->trg_event_map | tl->slave_fk_event_map;
		/* Used to avoid reading history in FK check on DELETE (see MDEV-16210). */
		prebuilt->upd_node->is_delete =
			(op_map & trg2bit(TRG_EVENT_DELETE)
			&& table->versioned(VERS_TIMESTAMP))
			? VERSIONED_DELETE : NO_DELETE;
	}

	if (prebuilt->versioned_write) {
		/* Guaranteed by CREATE TABLE, but anyway we make sure we
		generate history only when there are versioned fields. */
		DBUG_ASSERT(vers_fields);
		prebuilt->upd_node->vers_make_update(trx);
	}

	ut_ad(uvect->validate());
	return(DB_SUCCESS);
}

#ifdef WITH_WSREP
static
int
wsrep_calc_row_hash(
/*================*/
	byte*		digest,		/*!< in/out: md5 sum */
	const uchar*	row,		/*!< in: row in MySQL format */
	TABLE*		table,		/*!< in: table in MySQL data
					dictionary */
	row_prebuilt_t*	prebuilt)	/*!< in: InnoDB prebuilt struct */
{
	void *ctx = alloca(my_md5_context_size());
	my_md5_init(ctx);

	for (uint i = 0; i < table->s->fields; i++) {
		byte null_byte=0;
		byte true_byte=1;
		unsigned is_unsigned;

		const Field* field = table->field[i];
		if (!field->stored_in_db()) {
			continue;
		}

		auto ptr = row + get_field_offset(table, field);
		ulint len = field->pack_length();

		switch (get_innobase_type_from_mysql_type(&is_unsigned,
							  field)) {
		case DATA_BLOB:
			ptr = row_mysql_read_blob_ref(&len, ptr, len);

			break;

		case DATA_VARCHAR:
		case DATA_BINARY:
		case DATA_VARMYSQL:
			if (field->type() == MYSQL_TYPE_VARCHAR) {
				/* This is a >= 5.0.3 type true VARCHAR where
				the real payload data length is stored in
				1 or 2 bytes */

				ptr = row_mysql_read_true_varchar(
					&len, ptr,
					(ulint)
					(((Field_varstring*)field)->length_bytes));

			}

			break;
		default:
			;
		}
		/*
		if (field->null_ptr &&
		    field_in_record_is_null(table, field, (char*) row)) {
		*/

		if (field->is_null_in_record(row)) {
			my_md5_input(ctx, &null_byte, 1);
		} else {
			my_md5_input(ctx, &true_byte, 1);
			my_md5_input(ctx, ptr, len);
		}
	}

	my_md5_result(ctx, digest);

	return(0);
}

/** Append table-level exclusive key.
@param thd   MySQL thread handle
@param table table
@retval false on success
@retval true on failure */
ATTRIBUTE_COLD bool wsrep_append_table_key(MYSQL_THD thd, const dict_table_t &table)
{
  char db_buf[NAME_LEN + 1];
  char tbl_buf[NAME_LEN + 1];
  ulint db_buf_len, tbl_buf_len;

  if (!table.parse_name(db_buf, tbl_buf, &db_buf_len, &tbl_buf_len))
  {
    WSREP_ERROR("Parse_name for table key append failed: %s",
                wsrep_thd_query(thd));
    return true;
  }

  /* Append table-level exclusive key */
  const int rcode = wsrep_thd_append_table_key(thd, db_buf,
                                               tbl_buf, WSREP_SERVICE_KEY_EXCLUSIVE);
  if (rcode)
  {
    WSREP_ERROR("Appending table key failed: %s, %d",
                wsrep_thd_query(thd), rcode);
    return true;
  }

  return false;
}
#endif /* WITH_WSREP */

/**
Updates a row given as a parameter to a new value. Note that we are given
whole rows, not just the fields which are updated: this incurs some
overhead for CPU when we check which fields are actually updated.
TODO: currently InnoDB does not prevent the 'Halloween problem':
in a searched update a single row can get updated several times
if its index columns are updated!
@param[in] old_row	Old row contents in MySQL format
@param[out] new_row	Updated row contents in MySQL format
@return error number or 0 */

int
ha_innobase::update_row(
	const uchar*	old_row,
	const uchar*	new_row)
{
	int		err;

	dberr_t		error;
	trx_t*		trx = thd_to_trx(m_user_thd);
	mariadb_set_stats set_stats_temporary(handler_stats);

	DBUG_ENTER("ha_innobase::update_row");

	if (int err = is_valid_trx()) {
		DBUG_RETURN(err);
	}

	if (m_upd_buf == NULL) {
		ut_ad(m_upd_buf_size == 0);

		/* Create a buffer for packing the fields of a record. Why
		table->reclength did not work here? Obviously, because char
		fields when packed actually became 1 byte longer, when we also
		stored the string length as the first byte. */

		m_upd_buf_size = table->s->reclength + table->s->max_key_length
			+ MAX_REF_PARTS * 3;

		m_upd_buf = reinterpret_cast<uchar*>(
			my_malloc(PSI_INSTRUMENT_ME,
                                  m_upd_buf_size,
				MYF(MY_WME)));

		if (m_upd_buf == NULL) {
			m_upd_buf_size = 0;
			DBUG_RETURN(HA_ERR_OUT_OF_MEM);
		}
	}

	upd_t*		uvect = row_get_prebuilt_update_vector(m_prebuilt);
	ib_uint64_t	autoinc;

	/* Build an update vector from the modified fields in the rows
	(uses m_upd_buf of the handle) */

	error = calc_row_difference(
		uvect, old_row, new_row, table, m_upd_buf, m_upd_buf_size,
		m_prebuilt, autoinc);

	if (error != DB_SUCCESS) {
		goto func_exit;
	}

	if (!uvect->n_fields) {
		/* This is the same as success, but instructs
		MySQL that the row is not really updated and it
		should not increase the count of updated rows.
		This is fix for http://bugs.mysql.com/29157 */
		DBUG_RETURN(HA_ERR_RECORD_IS_THE_SAME);
	} else {
		if (m_prebuilt->upd_node->is_delete) {
			trx->fts_next_doc_id = 0;
		}

		/* row_start was updated by vers_make_update()
		in calc_row_difference() */
		error = row_update_for_mysql(m_prebuilt);

		if (error == DB_SUCCESS && m_prebuilt->versioned_write
		    /* Multiple UPDATE of same rows in single transaction create
		       historical rows only once. */
		    && trx->id != table->vers_start_id()) {
			/* UPDATE is not used by ALTER TABLE. Just precaution
			as we don't need history generation for ALTER TABLE. */
			ut_ad(thd_sql_command(m_user_thd) != SQLCOM_ALTER_TABLE);
			error = row_insert_for_mysql((byte*) old_row,
						     m_prebuilt,
						     ROW_INS_HISTORICAL);
		}
	}

	if (error == DB_SUCCESS && autoinc) {
		/* A value for an AUTO_INCREMENT column
		was specified in the UPDATE statement. */

		/* We need the upper limit of the col type to check for
		whether we update the table autoinc counter or not. */
		ulonglong	col_max_value =
			table->found_next_number_field->get_max_int_value();

		/* This should filter out the negative
		values set explicitly by the user. */
		if (autoinc <= col_max_value) {
			ulonglong	offset;
			ulonglong	increment;

			offset = m_prebuilt->autoinc_offset;
			increment = m_prebuilt->autoinc_increment;

			autoinc = innobase_next_autoinc(
				autoinc, 1, increment, offset,
				col_max_value);

			error = innobase_set_max_autoinc(autoinc);

			if (m_prebuilt->table->persistent_autoinc) {
				/* Update the PAGE_ROOT_AUTO_INC. Yes, we do
				this even if dict_table_t::autoinc already was
				greater than autoinc, because we cannot know
				if any INSERT actually used (and wrote to
				PAGE_ROOT_AUTO_INC) a value bigger than our
				autoinc. */
				btr_write_autoinc(dict_table_get_first_index(
							  m_prebuilt->table),
						  autoinc);
			}
		}
	}

func_exit:
	if (error == DB_FTS_INVALID_DOCID) {
		err = HA_FTS_INVALID_DOCID;
		my_error(HA_FTS_INVALID_DOCID, MYF(0));
	} else {
		err = convert_error_code_to_mysql(
			error, m_prebuilt->table->flags, m_user_thd);
	}

#ifdef WITH_WSREP
	if (error == DB_SUCCESS &&
	    /* For sequences, InnoDB transaction may not have been started yet.
	    Check THD-level wsrep state in that case. */
	    (trx->is_wsrep()
	     || (trx->state == TRX_STATE_NOT_STARTED && wsrep_on(m_user_thd)))
	    && wsrep_thd_is_local(m_user_thd)
	    && !wsrep_thd_ignore_table(m_user_thd)
	    && (thd_sql_command(m_user_thd) != SQLCOM_CREATE_TABLE)
	    && (thd_sql_command(m_user_thd) != SQLCOM_LOAD ||
	        thd_binlog_format(m_user_thd) == BINLOG_FORMAT_ROW)) {

		/* We use table-level exclusive key for SEQUENCES
		   and normal key append for others. */
		if (table->s->table_type == TABLE_TYPE_SEQUENCE) {
			if (wsrep_append_table_key(m_user_thd, *m_prebuilt->table))
				DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
		} else if (wsrep_append_keys(m_user_thd,
					     wsrep_protocol_version >= 4
					     ? WSREP_SERVICE_KEY_UPDATE
					     : WSREP_SERVICE_KEY_EXCLUSIVE,
					     old_row, new_row)) {
			WSREP_DEBUG("::update_rows::wsrep_append_keys failed THD %ld for %s.%s",
				    thd_get_thread_id(m_user_thd),
				    table->s->db.str,
				    table->s->table_name.str);
			DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
		}
	}
#endif /* WITH_WSREP */

	DBUG_RETURN(err);
}

/**********************************************************************//**
Deletes a row given as the parameter.
@return error number or 0 */

int
ha_innobase::delete_row(
/*====================*/
	const uchar*	record)	/*!< in: a row in MySQL format */
{
	dberr_t		error;
	trx_t*		trx = thd_to_trx(m_user_thd);
	mariadb_set_stats set_stats_temporary(handler_stats);

	DBUG_ENTER("ha_innobase::delete_row");

	if (int err = is_valid_trx()) {
		DBUG_RETURN(err);
	}
	if (!m_prebuilt->upd_node) {
		row_get_prebuilt_update_vector(m_prebuilt);
	}

	/* This is a delete */
	m_prebuilt->upd_node->is_delete = table->versioned_write(VERS_TRX_ID)
		&& table->vers_end_field()->is_max()
		&& trx->id != table->vers_start_id()
		? VERSIONED_DELETE
		: PLAIN_DELETE;
	trx->fts_next_doc_id = 0;

	ut_ad(!trx->is_bulk_insert());
	error = row_update_for_mysql(m_prebuilt);

#ifdef WITH_WSREP
	if (error == DB_SUCCESS && trx->is_wsrep()
	    && wsrep_thd_is_local(m_user_thd)
	    && !wsrep_thd_ignore_table(m_user_thd)) {
		if (wsrep_append_keys(m_user_thd, WSREP_SERVICE_KEY_EXCLUSIVE,
				      record,
				      NULL)) {
			WSREP_DEBUG("::delete_rows::wsrep_append_keys failed THD %ld for %s.%s",
				    thd_get_thread_id(m_user_thd),
				    table->s->db.str,
				    table->s->table_name.str);
			DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
		}
	}
#endif /* WITH_WSREP */
	DBUG_RETURN(convert_error_code_to_mysql(
			    error, m_prebuilt->table->flags, m_user_thd));
}

/**********************************************************************//**
Removes a new lock set on a row, if it was not read optimistically. This can
be called after a row has been read in the processing of an UPDATE or a DELETE
query. */

void
ha_innobase::unlock_row(void)
/*=========================*/
{
	DBUG_ENTER("ha_innobase::unlock_row");

	if (m_prebuilt->select_lock_type == LOCK_NONE) {
		DBUG_VOID_RETURN;
	}

	ut_ad(trx_state_eq(m_prebuilt->trx, TRX_STATE_ACTIVE, true));

	switch (m_prebuilt->row_read_type) {
	case ROW_READ_WITH_LOCKS:
		if (m_prebuilt->trx->isolation_level > TRX_ISO_READ_COMMITTED)
			break;
		/* fall through */
	case ROW_READ_TRY_SEMI_CONSISTENT:
		row_unlock_for_mysql(m_prebuilt, FALSE);
		break;
	case ROW_READ_DID_SEMI_CONSISTENT:
		m_prebuilt->row_read_type = ROW_READ_TRY_SEMI_CONSISTENT;
		break;
	}

	DBUG_VOID_RETURN;
}

/* See handler.h and row0mysql.h for docs on this function. */

bool
ha_innobase::was_semi_consistent_read(void)
/*=======================================*/
{
	return(m_prebuilt->row_read_type == ROW_READ_DID_SEMI_CONSISTENT);
}

/* See handler.h and row0mysql.h for docs on this function. */
void ha_innobase::try_semi_consistent_read(bool yes)
{
	ut_ad(m_prebuilt->trx == thd_to_trx(ha_thd()));
	/* Row read type is set to semi consistent read if this was
	requested by the SQL layer and the transaction isolation level is
	READ UNCOMMITTED or READ COMMITTED. */
	m_prebuilt->row_read_type = yes
		&& m_prebuilt->trx->isolation_level <= TRX_ISO_READ_COMMITTED
		? ROW_READ_TRY_SEMI_CONSISTENT
		: ROW_READ_WITH_LOCKS;
}

/******************************************************************//**
Initializes a handle to use an index.
@return 0 or error number */

int
ha_innobase::index_init(
/*====================*/
	uint		keynr,	/*!< in: key (index) number */
	bool)
{
	DBUG_ENTER("index_init");

	DBUG_RETURN(change_active_index(keynr));
}

/******************************************************************//**
Currently does nothing.
@return 0 */

int
ha_innobase::index_end(void)
/*========================*/
{
	DBUG_ENTER("index_end");

	active_index = MAX_KEY;

	in_range_check_pushed_down = FALSE;

	m_ds_mrr.dsmrr_close();

	DBUG_RETURN(0);
}

/** Convert a MariaDB search mode to an InnoDB search mode.
@tparam last_match whether last_match_mode is to be set
@param find_flag       MariaDB search mode
@param mode            InnoDB search mode
@param last_match_mode pointer to ha_innobase::m_last_match_mode
@return whether the search mode is unsupported */
template<bool last_match= false>
static bool convert_search_mode_to_innobase(ha_rkey_function find_flag,
                                            page_cur_mode_t &mode,
                                            uint *last_match_mode= nullptr)
{
  mode= PAGE_CUR_LE;
  if (last_match)
    *last_match_mode= 0;

  switch (find_flag) {
  case HA_READ_KEY_EXACT:
    /* this does not require the index to be UNIQUE */
    if (last_match)
      *last_match_mode= ROW_SEL_EXACT;
    /* fall through */
  case HA_READ_KEY_OR_NEXT:
    mode= PAGE_CUR_GE;
    return false;
  case HA_READ_AFTER_KEY:
    mode= PAGE_CUR_G;
    return false;
  case HA_READ_BEFORE_KEY:
    mode= PAGE_CUR_L;
    return false;
  case HA_READ_PREFIX_LAST:
    if (last_match)
      *last_match_mode= ROW_SEL_EXACT_PREFIX;
    /* fall through */
  case HA_READ_KEY_OR_PREV:
  case HA_READ_PREFIX_LAST_OR_PREV:
    return false;
  case HA_READ_MBR_CONTAIN:
    mode= PAGE_CUR_CONTAIN;
    return false;
  case HA_READ_MBR_INTERSECT:
    mode= PAGE_CUR_INTERSECT;
    return false;
  case HA_READ_MBR_WITHIN:
    mode= PAGE_CUR_WITHIN;
    return false;
  case HA_READ_MBR_DISJOINT:
    mode= PAGE_CUR_DISJOINT;
    return false;
  case HA_READ_MBR_EQUAL:
    mode= PAGE_CUR_MBR_EQUAL;
    return false;
  case HA_READ_PREFIX:
    break;
  }

  return true;
}

/*
   BACKGROUND INFO: HOW A SELECT SQL QUERY IS EXECUTED
   ---------------------------------------------------
The following does not cover all the details, but explains how we determine
the start of a new SQL statement, and what is associated with it.

For each table in the database the MySQL interpreter may have several
table handle instances in use, also in a single SQL query. For each table
handle instance there is an InnoDB  'm_prebuilt' struct which contains most
of the InnoDB data associated with this table handle instance.

  A) if the user has not explicitly set any MySQL table level locks:

  1) MySQL calls ::external_lock to set an 'intention' table level lock on
the table of the handle instance. There we set
m_prebuilt->sql_stat_start = TRUE. The flag sql_stat_start should be set
true if we are taking this table handle instance to use in a new SQL
statement issued by the user. We also increment trx->n_mysql_tables_in_use.

  2) If m_prebuilt->sql_stat_start == TRUE we 'pre-compile' the MySQL search
instructions to m_prebuilt->template of the table handle instance in
::index_read. The template is used to save CPU time in large joins.

  3) In row_search_mvcc(), if m_prebuilt->sql_stat_start is true, we
allocate a new consistent read view for the trx if it does not yet have one,
or in the case of a locking read, set an InnoDB 'intention' table level
lock on the table.

  4) We do the SELECT. MySQL may repeatedly call ::index_read for the
same table handle instance, if it is a join.

  5) When the SELECT ends, MySQL removes its intention table level locks
in ::external_lock. When trx->n_mysql_tables_in_use drops to zero,
 (a) we execute a COMMIT there if the autocommit is on,
 (b) we also release possible 'SQL statement level resources' InnoDB may
have for this SQL statement. The MySQL interpreter does NOT execute
autocommit for pure read transactions, though it should. That is why the
table handler in that case has to execute the COMMIT in ::external_lock.

  B) If the user has explicitly set MySQL table level locks, then MySQL
does NOT call ::external_lock at the start of the statement. To determine
when we are at the start of a new SQL statement we at the start of
::index_read also compare the query id to the latest query id where the
table handle instance was used. If it has changed, we know we are at the
start of a new SQL statement. Since the query id can theoretically
overwrap, we use this test only as a secondary way of determining the
start of a new SQL statement. */


/**********************************************************************//**
Positions an index cursor to the index specified in the handle. Fetches the
row if any.
@return 0, HA_ERR_KEY_NOT_FOUND, or error number */

int
ha_innobase::index_read(
/*====================*/
	uchar*		buf,		/*!< in/out: buffer for the returned
					row */
	const uchar*	key_ptr,	/*!< in: key value; if this is NULL
					we position the cursor at the
					start or end of index; this can
					also contain an InnoDB row id, in
					which case key_len is the InnoDB
					row id length; the key value can
					also be a prefix of a full key value,
					and the last column can be a prefix
					of a full column */
	uint			key_len,/*!< in: key value length */
	enum ha_rkey_function find_flag)/*!< in: search flags from my_base.h */
{
	DBUG_ENTER("index_read");
	mariadb_set_stats set_stats_temporary(handler_stats);
	DEBUG_SYNC_C("ha_innobase_index_read_begin");

	ut_ad(m_prebuilt->trx == thd_to_trx(m_user_thd));

	dict_index_t*	index = m_prebuilt->index;

	if (index == NULL || index->is_corrupted()) {
		m_prebuilt->index_usable = FALSE;
		DBUG_RETURN(HA_ERR_CRASHED);
	}

	if (!m_prebuilt->index_usable) {
		DBUG_RETURN(index->is_corrupted()
			    ? HA_ERR_INDEX_CORRUPT
			    : HA_ERR_TABLE_DEF_CHANGED);
	}

	if (index->type & DICT_FTS) {
		DBUG_RETURN(HA_ERR_KEY_NOT_FOUND);
	}

	const trx_state_t trx_state{m_prebuilt->trx->state};
	if (trx_state == TRX_STATE_ABORTED) {
		DBUG_RETURN(HA_ERR_ROLLBACK);
	}

	/* For R-Tree index, we will always place the page lock to
	pages being searched */
	if (index->is_spatial() && !m_prebuilt->trx->will_lock) {
		if (trx_state != TRX_STATE_NOT_STARTED) {
			DBUG_RETURN(HA_ERR_READ_ONLY_TRANSACTION);
		} else {
			m_prebuilt->trx->will_lock = true;
		}
	}

	/* Note that if the index for which the search template is built is not
	necessarily m_prebuilt->index, but can also be the clustered index */

	if (m_prebuilt->sql_stat_start) {
		build_template(false);
	}

	if (key_len) {
		ut_ad(key_ptr);
		/* Convert the search key value to InnoDB format into
		m_prebuilt->search_tuple */

		row_sel_convert_mysql_key_to_innobase(
			m_prebuilt->search_tuple,
			m_prebuilt->srch_key_val1,
			m_prebuilt->srch_key_val_len,
			index,
			(byte*) key_ptr,
			key_len);

		DBUG_ASSERT(m_prebuilt->search_tuple->n_fields > 0);
	} else {
		ut_ad(find_flag != HA_READ_KEY_EXACT);
		/* We position the cursor to the last or the first entry
		in the index */

		dtuple_set_n_fields(m_prebuilt->search_tuple, 0);
	}

	page_cur_mode_t	mode;

	if (convert_search_mode_to_innobase<true>(find_flag, mode,
						  &m_last_match_mode)) {
		table->status = STATUS_NOT_FOUND;
		DBUG_RETURN(HA_ERR_UNSUPPORTED);
	}

	dberr_t ret =
		row_search_mvcc(buf, mode, m_prebuilt, m_last_match_mode, 0);

	DBUG_EXECUTE_IF("ib_select_query_failure", ret = DB_ERROR;);

	if (UNIV_LIKELY(ret == DB_SUCCESS)) {
		table->status = 0;
		DBUG_RETURN(0);
	}

	table->status = STATUS_NOT_FOUND;

	switch (ret) {
	case DB_TABLESPACE_DELETED:
		ib_senderrf(
			m_prebuilt->trx->mysql_thd, IB_LOG_LEVEL_ERROR,
			ER_TABLESPACE_DISCARDED,
			table->s->table_name.str);
		DBUG_RETURN(HA_ERR_TABLESPACE_MISSING);
	case DB_RECORD_NOT_FOUND:
	case DB_END_OF_INDEX:
		DBUG_RETURN(HA_ERR_KEY_NOT_FOUND);
	case DB_TABLESPACE_NOT_FOUND:
		ib_senderrf(
			m_prebuilt->trx->mysql_thd, IB_LOG_LEVEL_ERROR,
			ER_TABLESPACE_MISSING,
			table->s->table_name.str);
		DBUG_RETURN(HA_ERR_TABLESPACE_MISSING);
	default:
		DBUG_RETURN(convert_error_code_to_mysql(
				    ret, m_prebuilt->table->flags,
				    m_user_thd));
	}
}

/*******************************************************************//**
The following functions works like index_read, but it find the last
row with the current key value or prefix.
@return 0, HA_ERR_KEY_NOT_FOUND, or an error code */

int
ha_innobase::index_read_last(
/*=========================*/
	uchar*		buf,	/*!< out: fetched row */
	const uchar*	key_ptr,/*!< in: key value, or a prefix of a full
				key value */
	uint		key_len)/*!< in: length of the key val or prefix
				in bytes */
{
	return(index_read(buf, key_ptr, key_len, HA_READ_PREFIX_LAST));
}

/********************************************************************//**
Get the index for a handle. Does not change active index.
@return NULL or index instance. */

dict_index_t*
ha_innobase::innobase_get_index(
/*============================*/
	uint		keynr)	/*!< in: use this index; MAX_KEY means always
				clustered index, even if it was internally
				generated by InnoDB */
{
	KEY*		key = NULL;
	dict_table_t*	ib_table = m_prebuilt->table;
	dict_index_t*	index;

	DBUG_ENTER("innobase_get_index");

	if (keynr != MAX_KEY && table->s->keys > 0) {
		key = &table->key_info[keynr];
		index = dict_table_get_index_on_name(ib_table, key->name.str);
	} else {
		index = dict_table_get_first_index(ib_table);
	}

	if (index == NULL) {
		sql_print_error(
			"InnoDB could not find key no %u with name %s"
			" from dict cache for table %s",
			keynr, key ? key->name.str : "NULL",
			ib_table->name.m_name);
	}

	DBUG_RETURN(index);
}

/********************************************************************//**
Changes the active index of a handle.
@return 0 or error code */

int
ha_innobase::change_active_index(
/*=============================*/
	uint	keynr)	/*!< in: use this index; MAX_KEY means always clustered
			index, even if it was internally generated by
			InnoDB */
{
	DBUG_ENTER("change_active_index");

	ut_ad(m_user_thd == ha_thd());
	ut_a(m_prebuilt->trx == thd_to_trx(m_user_thd));

	active_index = keynr;

	m_prebuilt->index = innobase_get_index(keynr);

	if (m_prebuilt->index == NULL) {
		sql_print_warning("InnoDB: change_active_index(%u) failed",
				  keynr);
		m_prebuilt->index_usable = FALSE;
		DBUG_RETURN(1);
	}

	m_prebuilt->index_usable = row_merge_is_index_usable(
		m_prebuilt->trx, m_prebuilt->index);

	if (!m_prebuilt->index_usable) {
		if (m_prebuilt->index->is_corrupted()) {
			char	table_name[MAX_FULL_NAME_LEN + 1];

			innobase_format_name(
				table_name, sizeof table_name,
				m_prebuilt->index->table->name.m_name);

			if (m_prebuilt->index->is_primary()) {
				ut_ad(m_prebuilt->index->table->corrupted);
				push_warning_printf(
					m_user_thd, Sql_condition::WARN_LEVEL_WARN,
					ER_TABLE_CORRUPT,
					"InnoDB: Table %s is corrupted.",
					table_name);
				DBUG_RETURN(ER_TABLE_CORRUPT);
			} else {
				push_warning_printf(
					m_user_thd, Sql_condition::WARN_LEVEL_WARN,
					HA_ERR_INDEX_CORRUPT,
					"InnoDB: Index %s for table %s is"
					" marked as corrupted",
					m_prebuilt->index->name(),
					table_name);
				DBUG_RETURN(HA_ERR_INDEX_CORRUPT);
			}
		} else {
			push_warning_printf(
				m_user_thd, Sql_condition::WARN_LEVEL_WARN,
				HA_ERR_TABLE_DEF_CHANGED,
				"InnoDB: insufficient history for index %u",
				keynr);
		}

		/* The caller seems to ignore this.  Thus, we must check
		this again in row_search_mvcc(). */
		DBUG_RETURN(convert_error_code_to_mysql(DB_MISSING_HISTORY,
				0, NULL));
	}

	ut_a(m_prebuilt->search_tuple != 0);

	/* Initialization of search_tuple is not needed for FT index
	since FT search returns rank only. In addition engine should
	be able to retrieve FTS_DOC_ID column value if necessary. */
	if (m_prebuilt->index->type & DICT_FTS) {
		for (uint i = 0; i < table->s->fields; i++) {
			if (m_prebuilt->read_just_key
			    && bitmap_is_set(table->read_set, i)
			    && !strcmp(table->s->field[i]->field_name.str,
				       FTS_DOC_ID.str)) {
				m_prebuilt->fts_doc_id_in_read_set = true;
				break;
			}
		}
	} else {
		ulint n_fields = dict_index_get_n_unique_in_tree(
			m_prebuilt->index);

		dtuple_set_n_fields(m_prebuilt->search_tuple, n_fields);

		dict_index_copy_types(
			m_prebuilt->search_tuple, m_prebuilt->index,
			n_fields);

		/* If it's FTS query and FTS_DOC_ID exists FTS_DOC_ID field is
		always added to read_set. */
		m_prebuilt->fts_doc_id_in_read_set = m_prebuilt->in_fts_query
			&& m_prebuilt->read_just_key
			&& m_prebuilt->index->contains_col_or_prefix(
				m_prebuilt->table->fts->doc_col, false);
	}

	/* MySQL changes the active index for a handle also during some
	queries, for example SELECT MAX(a), SUM(a) first retrieves the MAX()
	and then calculates the sum. Previously we played safe and used
	the flag ROW_MYSQL_WHOLE_ROW below, but that caused unnecessary
	copying. Starting from MySQL-4.1 we use a more efficient flag here. */

	build_template(false);

	DBUG_RETURN(0);
}

/* @return true if it's necessary to switch current statement log format from
STATEMENT to ROW if binary log format is MIXED and autoincrement values
are changed in the statement */
bool ha_innobase::autoinc_lock_mode_stmt_unsafe() const
{
  return innobase_autoinc_lock_mode == AUTOINC_NO_LOCKING;
}

/***********************************************************************//**
Reads the next or previous row from a cursor, which must have previously been
positioned using index_read.
@return 0, HA_ERR_END_OF_FILE, or error number */

int
ha_innobase::general_fetch(
/*=======================*/
	uchar*	buf,		/*!< in/out: buffer for next row in MySQL
				format */
	uint	direction,	/*!< in: ROW_SEL_NEXT or ROW_SEL_PREV */
	uint	match_mode)	/*!< in: 0, ROW_SEL_EXACT, or
				ROW_SEL_EXACT_PREFIX */
{
	DBUG_ENTER("general_fetch");

	mariadb_set_stats set_stats_temporary(handler_stats);
	const trx_t*	trx = m_prebuilt->trx;

	ut_ad(trx == thd_to_trx(m_user_thd));

	switch (UNIV_EXPECT(trx->state, TRX_STATE_ACTIVE)) {
	default:
		ut_ad("invalid state" == 0);
		/* fall through */
	case TRX_STATE_ABORTED:
		DBUG_RETURN(HA_ERR_ROLLBACK);
	case TRX_STATE_ACTIVE:
	case TRX_STATE_NOT_STARTED:
		break;
	}

	if (m_prebuilt->table->is_readable()) {
	} else if (m_prebuilt->table->corrupted) {
		DBUG_RETURN(HA_ERR_CRASHED);
	} else {
		DBUG_RETURN(m_prebuilt->table->space
			    ? HA_ERR_DECRYPTION_FAILED
			    : HA_ERR_NO_SUCH_TABLE);
	}

	int	error;

	switch (dberr_t	ret = row_search_mvcc(buf, PAGE_CUR_UNSUPP, m_prebuilt,
					      match_mode, direction)) {
	case DB_SUCCESS:
		error = 0;
		table->status = 0;
		break;
	case DB_RECORD_NOT_FOUND:
		error = HA_ERR_END_OF_FILE;
		table->status = STATUS_NOT_FOUND;
		break;
	case DB_END_OF_INDEX:
		error = HA_ERR_END_OF_FILE;
		table->status = STATUS_NOT_FOUND;
		break;
	case DB_TABLESPACE_DELETED:
		ib_senderrf(
			trx->mysql_thd, IB_LOG_LEVEL_ERROR,
			ER_TABLESPACE_DISCARDED,
			table->s->table_name.str);

		table->status = STATUS_NOT_FOUND;
		error = HA_ERR_TABLESPACE_MISSING;
		break;
	case DB_TABLESPACE_NOT_FOUND:

		ib_senderrf(
			trx->mysql_thd, IB_LOG_LEVEL_ERROR,
			ER_TABLESPACE_MISSING,
			table->s->table_name.str);

		table->status = STATUS_NOT_FOUND;
		error = HA_ERR_TABLESPACE_MISSING;
		break;
	default:
		error = convert_error_code_to_mysql(
			ret, m_prebuilt->table->flags, m_user_thd);

		table->status = STATUS_NOT_FOUND;
		break;
	}

	DBUG_RETURN(error);
}

/***********************************************************************//**
Reads the next row from a cursor, which must have previously been
positioned using index_read.
@return 0, HA_ERR_END_OF_FILE, or error number */

int
ha_innobase::index_next(
/*====================*/
	uchar*		buf)	/*!< in/out: buffer for next row in MySQL
				format */
{
	return(general_fetch(buf, ROW_SEL_NEXT, 0));
}

/*******************************************************************//**
Reads the next row matching to the key value given as the parameter.
@return 0, HA_ERR_END_OF_FILE, or error number */

int
ha_innobase::index_next_same(
/*=========================*/
	uchar*		buf,	/*!< in/out: buffer for the row */
	const uchar*, uint)
{
	return(general_fetch(buf, ROW_SEL_NEXT, m_last_match_mode));
}

/***********************************************************************//**
Reads the previous row from a cursor, which must have previously been
positioned using index_read.
@return 0, HA_ERR_END_OF_FILE, or error number */

int
ha_innobase::index_prev(
/*====================*/
	uchar*	buf)	/*!< in/out: buffer for previous row in MySQL format */
{
	return(general_fetch(buf, ROW_SEL_PREV, 0));
}

/********************************************************************//**
Positions a cursor on the first record in an index and reads the
corresponding row to buf.
@return 0, HA_ERR_END_OF_FILE, or error code */

int
ha_innobase::index_first(
/*=====================*/
	uchar*	buf)	/*!< in/out: buffer for the row */
{
	DBUG_ENTER("index_first");

	int	error = index_read(buf, NULL, 0, HA_READ_AFTER_KEY);

	/* MySQL does not seem to allow this to return HA_ERR_KEY_NOT_FOUND */

	if (error == HA_ERR_KEY_NOT_FOUND) {
		error = HA_ERR_END_OF_FILE;
	}

	DBUG_RETURN(error);
}

/********************************************************************//**
Positions a cursor on the last record in an index and reads the
corresponding row to buf.
@return 0, HA_ERR_END_OF_FILE, or error code */

int
ha_innobase::index_last(
/*====================*/
	uchar*	buf)	/*!< in/out: buffer for the row */
{
	DBUG_ENTER("index_last");

	int	error = index_read(buf, NULL, 0, HA_READ_BEFORE_KEY);

	/* MySQL does not seem to allow this to return HA_ERR_KEY_NOT_FOUND */

	if (error == HA_ERR_KEY_NOT_FOUND) {
		error = HA_ERR_END_OF_FILE;
	}

	DBUG_RETURN(error);
}

/****************************************************************//**
Initialize a table scan.
@return 0 or error number */

int
ha_innobase::rnd_init(
/*==================*/
	bool	scan)	/*!< in: true if table/index scan FALSE otherwise */
{
	int		err;

	/* Don't use rowid filter when doing full table scan or rnd_pos calls.*/
	if (!scan) {
		m_disable_rowid_filter = true;
	}

	/* Store the active index value so that we can restore the original
	value after a scan */

	if (m_prebuilt->clust_index_was_generated) {
		err = change_active_index(MAX_KEY);
	} else {
		err = change_active_index(m_primary_key);
	}

	if (err && !scan) {
		/* Restore the original value in case of error */
		m_disable_rowid_filter = false;
	}


	/* Don't use semi-consistent read in random row reads (by position).
	This means we must disable semi_consistent_read if scan is false */

	if (!scan) {
		try_semi_consistent_read(0);
	}

	m_start_of_scan = true;

	return(err);
}

/*****************************************************************//**
Ends a table scan.
@return 0 or error number */

int
ha_innobase::rnd_end(void)
/*======================*/
{
	m_disable_rowid_filter = false;
	return(index_end());
}

/*****************************************************************//**
Reads the next row in a table scan (also used to read the FIRST row
in a table scan).
@return 0, HA_ERR_END_OF_FILE, or error number */

int
ha_innobase::rnd_next(
/*==================*/
	uchar*	buf)	/*!< in/out: returns the row in this buffer,
			in MySQL format */
{
	int	error;
	DBUG_ENTER("rnd_next");

	if (m_start_of_scan) {
		error = index_first(buf);

		if (error == HA_ERR_KEY_NOT_FOUND) {
			error = HA_ERR_END_OF_FILE;
		}

		m_start_of_scan = false;
	} else {
		error = general_fetch(buf, ROW_SEL_NEXT, 0);
	}

	DBUG_RETURN(error);
}

/**********************************************************************//**
Fetches a row from the table based on a row reference.
@return 0, HA_ERR_KEY_NOT_FOUND, or error code */

int
ha_innobase::rnd_pos(
/*=================*/
	uchar*	buf,	/*!< in/out: buffer for the row */
	uchar*	pos)	/*!< in: primary key value of the row in the
			MySQL format, or the row id if the clustered
			index was internally generated by InnoDB; the
			length of data in pos has to be ref_length */
{
	DBUG_ENTER("rnd_pos");
	DBUG_DUMP("key", pos, ref_length);

	/* Note that we assume the length of the row reference is fixed
	for the table, and it is == ref_length */

	DBUG_ASSERT(m_disable_rowid_filter == true);
	int	error = index_read(buf, pos, (uint)ref_length, HA_READ_KEY_EXACT);

	if (error != 0) {
		DBUG_PRINT("error", ("Got error: %d", error));
	}

	DBUG_RETURN(error);
}

/**********************************************************************//**
Initialize FT index scan
@return 0 or error number */

int ha_innobase::ft_init()
{
	DBUG_ENTER("ft_init");

	trx_t*	trx = check_trx_exists(ha_thd());

	/* FTS queries are not treated as autocommit non-locking selects.
	This is because the FTS implementation can acquire locks behind
	the scenes. This has not been verified but it is safer to treat
	them as regular read only transactions for now. */
	switch (trx->state) {
	default:
		DBUG_RETURN(HA_ERR_ROLLBACK);
	case TRX_STATE_ACTIVE:
		break;
	case TRX_STATE_NOT_STARTED:
		trx->will_lock = true;
		break;
	}

        /* If there is an FTS scan in progress, stop it */
        fts_result_t* result =  (reinterpret_cast<NEW_FT_INFO*>(ft_handler))->ft_result;
        if (result)
                result->current= NULL;

	DBUG_RETURN(rnd_init(false));
}

/**********************************************************************//**
Initialize FT index scan
@return FT_INFO structure if successful or NULL */

FT_INFO*
ha_innobase::ft_init_ext(
/*=====================*/
	uint			flags,	/* in: */
	uint			keynr,	/* in: */
	String*			key)	/* in: */
{
	NEW_FT_INFO*		fts_hdl = NULL;
	dict_index_t*		index;
	fts_result_t*		result;
	char			buf_tmp[8192];
	ulint			buf_tmp_used;
	uint			num_errors;
	ulint			query_len = key->length();
	const CHARSET_INFO*	char_set = key->charset();
	const char*		query = key->ptr();

	if (UNIV_UNLIKELY(fts_enable_diag_print)) {
		{
			ib::info	out;
			out << "keynr=" << keynr << ", '";
			out.write(key->ptr(), key->length());
		}

		sql_print_information((flags & FT_BOOL)
				      ? "InnoDB: BOOL search"
				      : "InnoDB: NL search");
	}

        /* Multi byte character sets like utf32 and utf16 are not
           compatible with some string function used. So to convert them
           to uft8 before we proceed. */
	if (char_set->mbminlen != 1) {
		buf_tmp_used = innobase_convert_string(
			buf_tmp, sizeof(buf_tmp) - 1,
			&my_charset_utf8mb3_general_ci,
			query, query_len, (CHARSET_INFO*) char_set,
			&num_errors);

		buf_tmp[buf_tmp_used] = 0;
		query = buf_tmp;
		query_len = buf_tmp_used;
	}

	trx_t*	trx = m_prebuilt->trx;

	/* FTS queries are not treated as autocommit non-locking selects.
	This is because the FTS implementation can acquire locks behind
	the scenes. This has not been verified but it is safer to treat
	them as regular read only transactions for now. */

	switch (trx->state) {
	default:
		ut_ad("invalid state" == 0);
		my_printf_error(HA_ERR_ROLLBACK, "Invalid transaction state",
				ME_ERROR_LOG);
		return nullptr;
	case TRX_STATE_ACTIVE:
		break;
	case TRX_STATE_NOT_STARTED:
		trx->will_lock = true;
		break;
	}

	dict_table_t*	ft_table = m_prebuilt->table;

	/* Table does not have an FTS index */
	if (!ft_table->fts || ib_vector_is_empty(ft_table->fts->indexes)) {
		my_error(ER_TABLE_HAS_NO_FT, MYF(0));
		return(NULL);
	}

	/* If tablespace is discarded, we should return here */
	if (!ft_table->space) {
		my_error(ER_TABLESPACE_MISSING, MYF(0), ft_table->name.m_name);
		return(NULL);
	}

	if (keynr == NO_SUCH_KEY) {
		/* FIXME: Investigate the NO_SUCH_KEY usage */
		index = reinterpret_cast<dict_index_t*>
			(ib_vector_getp(ft_table->fts->indexes, 0));
	} else {
		index = innobase_get_index(keynr);
	}

	if (index == NULL || index->type != DICT_FTS) {
		my_error(ER_TABLE_HAS_NO_FT, MYF(0));
		return(NULL);
	}

	if (!(ft_table->fts->added_synced)) {
		fts_init_index(ft_table, FALSE);

		ft_table->fts->added_synced = true;
	}

	const byte*	q = reinterpret_cast<const byte*>(
		const_cast<char*>(query));

	// FIXME: support ft_init_ext_with_hints(), pass LIMIT
	dberr_t	error = fts_query(trx, index, flags, q, query_len, &result);

	if (error != DB_SUCCESS) {
		my_error(convert_error_code_to_mysql(error, 0, NULL), MYF(0));
		return(NULL);
	}

	/* Allocate FTS handler, and instantiate it before return */
	fts_hdl = reinterpret_cast<NEW_FT_INFO*>(
		my_malloc(PSI_INSTRUMENT_ME, sizeof(NEW_FT_INFO), MYF(0)));

	fts_hdl->please = const_cast<_ft_vft*>(&ft_vft_result);
	fts_hdl->could_you = const_cast<_ft_vft_ext*>(&ft_vft_ext_result);
	fts_hdl->ft_prebuilt = m_prebuilt;
	fts_hdl->ft_result = result;

	/* FIXME: Re-evaluate the condition when Bug 14469540 is resolved */
	m_prebuilt->in_fts_query = true;

	return(reinterpret_cast<FT_INFO*>(fts_hdl));
}

/*****************************************************************//**
Set up search tuple for a query through FTS_DOC_ID_INDEX on
supplied Doc ID. This is used by MySQL to retrieve the documents
once the search result (Doc IDs) is available

@return DB_SUCCESS or DB_INDEX_CORRUPT
*/
static
dberr_t
innobase_fts_create_doc_id_key(
/*===========================*/
	dtuple_t*	tuple,		/* in/out: m_prebuilt->search_tuple */
	const dict_index_t*
			index,		/* in: index (FTS_DOC_ID_INDEX) */
	doc_id_t*	doc_id)		/* in/out: doc id to search, value
					could be changed to storage format
					used for search. */
{
	doc_id_t	temp_doc_id;
	dfield_t*	dfield = dtuple_get_nth_field(tuple, 0);
	const ulint	n_uniq = index->table->fts_n_uniq();

	if (dict_index_get_n_unique(index) != n_uniq)
		return DB_INDEX_CORRUPT;

	dtuple_set_n_fields(tuple, index->n_fields);
	dict_index_copy_types(tuple, index, index->n_fields);

#ifdef UNIV_DEBUG
	/* The unique Doc ID field should be an eight-bytes integer */
	dict_field_t*	field = dict_index_get_nth_field(index, 0);
        ut_a(field->col->mtype == DATA_INT);
	ut_ad(sizeof(*doc_id) == field->fixed_len);
	ut_ad(!strcmp(index->name, FTS_DOC_ID_INDEX.str));
#endif /* UNIV_DEBUG */

	/* Convert to storage byte order */
	mach_write_to_8(reinterpret_cast<byte*>(&temp_doc_id), *doc_id);
	*doc_id = temp_doc_id;
	dfield_set_data(dfield, doc_id, sizeof(*doc_id));

	if (n_uniq == 2) {
		ut_ad(index->table->versioned());
		dfield = dtuple_get_nth_field(tuple, 1);
		if (index->table->versioned_by_id()) {
			dfield_set_data(dfield, trx_id_max_bytes,
					sizeof(trx_id_max_bytes));
		} else {
			dfield_set_data(dfield, timestamp_max_bytes,
					sizeof(timestamp_max_bytes));
		}
	}

	dtuple_set_n_fields_cmp(tuple, n_uniq);

	for (ulint i = n_uniq; i < index->n_fields; i++) {
		dfield = dtuple_get_nth_field(tuple, i);
		dfield_set_null(dfield);
	}
	return DB_SUCCESS;
}

/**********************************************************************//**
Fetch next result from the FT result set
@return error code */

int
ha_innobase::ft_read(
/*=================*/
	uchar*		buf)		/*!< in/out: buf contain result row */
{
	row_prebuilt_t*	ft_prebuilt;
	mariadb_set_stats set_stats_temporary(handler_stats);

	ft_prebuilt = reinterpret_cast<NEW_FT_INFO*>(ft_handler)->ft_prebuilt;

	ut_a(ft_prebuilt == m_prebuilt);

	fts_result_t*	result;

	result = reinterpret_cast<NEW_FT_INFO*>(ft_handler)->ft_result;

	if (result->current == NULL) {
		/* This is the case where the FTS query did not
		contain and matching documents. */
		if (result->rankings_by_id != NULL) {
			/* Now that we have the complete result, we
			need to sort the document ids on their rank
			calculation. */

			fts_query_sort_result_on_rank(result);

			result->current = const_cast<ib_rbt_node_t*>(
				rbt_first(result->rankings_by_rank));
		} else {
			ut_a(result->current == NULL);
		}
	} else {
		result->current = const_cast<ib_rbt_node_t*>(
			rbt_next(result->rankings_by_rank, result->current));
	}

next_record:

	if (result->current != NULL) {
		doc_id_t	search_doc_id;
		dtuple_t*	tuple = m_prebuilt->search_tuple;

		/* If we only need information from result we can return
		   without fetching the table row */
		if (ft_prebuilt->read_just_key) {
#ifdef MYSQL_STORE_FTS_DOC_ID
			if (m_prebuilt->fts_doc_id_in_read_set) {
				fts_ranking_t* ranking;
				ranking = rbt_value(fts_ranking_t,
						    result->current);
				innobase_fts_store_docid(
					table, ranking->doc_id);
			}
#endif
			table->status= 0;
			return(0);
		}

		dict_index_t*	index;

		index = m_prebuilt->table->fts_doc_id_index;

		/* Must find the index */
		ut_a(index != NULL);

		/* Switch to the FTS doc id index */
		m_prebuilt->index = index;

		fts_ranking_t*	ranking = rbt_value(
			fts_ranking_t, result->current);

		search_doc_id = ranking->doc_id;

		/* We pass a pointer of search_doc_id because it will be
		converted to storage byte order used in the search
		tuple. */
		dberr_t ret = innobase_fts_create_doc_id_key(
			tuple, index, &search_doc_id);

		if (ret == DB_SUCCESS) {
			ret = row_search_mvcc(
				buf, PAGE_CUR_GE, m_prebuilt,
				ROW_SEL_EXACT, 0);
		}

		int	error;

		switch (ret) {
		case DB_SUCCESS:
			error = 0;
			table->status = 0;
			break;
		case DB_RECORD_NOT_FOUND:
			result->current = const_cast<ib_rbt_node_t*>(
				rbt_next(result->rankings_by_rank,
					 result->current));

			if (!result->current) {
				/* exhaust the result set, should return
				HA_ERR_END_OF_FILE just like
				ha_innobase::general_fetch() and/or
				ha_innobase::index_first() etc. */
				error = HA_ERR_END_OF_FILE;
				table->status = STATUS_NOT_FOUND;
			} else {
				goto next_record;
			}
			break;
		case DB_END_OF_INDEX:
			error = HA_ERR_END_OF_FILE;
			table->status = STATUS_NOT_FOUND;
			break;
		case DB_TABLESPACE_DELETED:

			ib_senderrf(
				m_prebuilt->trx->mysql_thd, IB_LOG_LEVEL_ERROR,
				ER_TABLESPACE_DISCARDED,
				table->s->table_name.str);

			table->status = STATUS_NOT_FOUND;
			error = HA_ERR_TABLESPACE_MISSING;
			break;
		case DB_TABLESPACE_NOT_FOUND:

			ib_senderrf(
				m_prebuilt->trx->mysql_thd, IB_LOG_LEVEL_ERROR,
				ER_TABLESPACE_MISSING,
				table->s->table_name.str);

			table->status = STATUS_NOT_FOUND;
			error = HA_ERR_TABLESPACE_MISSING;
			break;
		default:
			error = convert_error_code_to_mysql(
				ret, 0, m_user_thd);

			table->status = STATUS_NOT_FOUND;
			break;
		}

		return(error);
	}

	return(HA_ERR_END_OF_FILE);
}

#ifdef WITH_WSREP
inline
const char*
wsrep_key_type_to_str(Wsrep_service_key_type type)
{
	switch (type) {
	case WSREP_SERVICE_KEY_SHARED:
		return "shared";
	case WSREP_SERVICE_KEY_REFERENCE:
		return "reference";
	case WSREP_SERVICE_KEY_UPDATE:
		return "update";
	case WSREP_SERVICE_KEY_EXCLUSIVE:
		return "exclusive";
	};
	return "unknown";
}

extern dberr_t
wsrep_append_foreign_key(
/*===========================*/
	trx_t*		trx,		/*!< in: trx */
	dict_foreign_t*	foreign,	/*!< in: foreign key constraint */
	const rec_t*	rec,		/*!<in: clustered index record */
	dict_index_t*	index,		/*!<in: clustered index */
	bool		referenced,	/*!<in: is check for
					referenced table */
	upd_node_t*	upd_node,	/*<!in: update node */
	bool		pa_disable,	/*<!in: disable parallel apply ?*/
	Wsrep_service_key_type	key_type)	/*!< in: access type of this key
					(shared, exclusive, reference...) */
{
	ut_ad(trx->is_wsrep());

	if (!wsrep_thd_is_local(trx->mysql_thd))
		return DB_SUCCESS;

	if (upd_node && wsrep_protocol_version < 4) {
		key_type = WSREP_SERVICE_KEY_SHARED;
	}

	THD* thd = trx->mysql_thd;

	if (!foreign ||
	    (!foreign->referenced_table && !foreign->foreign_table)) {
		WSREP_INFO("FK: %s missing in: %s",
			   (!foreign ? "constraint" :
			    (!foreign->referenced_table ?
			     "referenced table" : "foreign table")),
			   wsrep_thd_query(thd));
		return DB_ERROR;
	}

	ulint rcode = DB_SUCCESS;
	char  cache_key[MAX_FULL_NAME_LEN] = {'\0'};
	char  db_name[MAX_DATABASE_NAME_LEN+1] = {'\0'};
	size_t cache_key_len = 0;

	if ( !((referenced) ?
		foreign->referenced_table : foreign->foreign_table)) {
		WSREP_DEBUG("pulling %s table into cache",
			    (referenced) ? "referenced" : "foreign");
		dict_sys.lock(SRW_LOCK_CALL);

		if (referenced) {
			foreign->referenced_table =
				dict_sys.load_table(
					{foreign->referenced_table_name_lookup,
					 strlen(foreign->
						referenced_table_name_lookup)
					});
			if (foreign->referenced_table) {
				foreign->referenced_index =
					dict_foreign_find_index(
						foreign->referenced_table, NULL,
						foreign->referenced_col_names,
						foreign->n_fields,
						foreign->foreign_index,
						TRUE, FALSE);
			}
		} else {
	  		foreign->foreign_table =
				dict_sys.load_table(
					{foreign->foreign_table_name_lookup,
					 strlen(foreign->
						foreign_table_name_lookup)});

			if (foreign->foreign_table) {
				foreign->foreign_index =
					dict_foreign_find_index(
						foreign->foreign_table, NULL,
						foreign->foreign_col_names,
						foreign->n_fields,
						foreign->referenced_index,
						TRUE, FALSE);
			}
		}
		dict_sys.unlock();
	}

	if ( !((referenced) ?
		foreign->referenced_table : foreign->foreign_table)) {
		WSREP_WARN("FK: %s missing in query: %s",
			   (!foreign->referenced_table) ?
			   "referenced table" : "foreign table",
			   wsrep_thd_query(thd));
		return DB_ERROR;
	}

	byte  key[WSREP_MAX_SUPPORTED_KEY_LENGTH+1] = {'\0'};
	ulint len = WSREP_MAX_SUPPORTED_KEY_LENGTH;

	dict_index_t *idx_target = (referenced) ?
		foreign->referenced_index : index;
	dict_index_t *idx = (referenced) ?
		UT_LIST_GET_FIRST(foreign->referenced_table->indexes) :
		UT_LIST_GET_FIRST(foreign->foreign_table->indexes);
	int i = 0;

	while (idx != NULL && idx != idx_target) {
		if (!Lex_ident_column(Lex_cstring_strlen(idx->name)).
		      streq(GEN_CLUST_INDEX)) {
			i++;
		}
		idx = UT_LIST_GET_NEXT(indexes, idx);
	}

	ut_a(idx);
	key[0] = byte(i);

	rcode = wsrep_rec_get_foreign_key(
		&key[1], &len, rec, index, idx,
		wsrep_protocol_version > 1);

	if (rcode != DB_SUCCESS) {
		WSREP_ERROR(
			"FK key set failed: " ULINTPF
			" (" ULINTPF "%s), index: %s %s, %s",
			rcode, referenced, wsrep_key_type_to_str(key_type),
			(index)       ? index->name() : "void index",
			(index && index->table) ? index->table->name.m_name :
				"void table",
			wsrep_thd_query(thd));
		return DB_ERROR;
	}

	char * fk_table =
		(wsrep_protocol_version > 1) ?
		((referenced) ?
			foreign->referenced_table->name.m_name :
			foreign->foreign_table->name.m_name) :
		foreign->foreign_table->name.m_name;

        /* convert db and table name parts separately to system charset */
	ulint	db_name_len = dict_get_db_name_len(fk_table);
	strmake(db_name, fk_table, db_name_len);
	uint errors;
	cache_key_len= innobase_convert_to_system_charset(cache_key,
				db_name, sizeof(cache_key), &errors);
	if (errors) {
		WSREP_WARN("unexpected foreign key table %s %s",
			   foreign->referenced_table->name.m_name,
			   foreign->foreign_table->name.m_name);
		return DB_ERROR;
	}

	/* after db name adding 0 and then converted table name */
	cache_key[db_name_len]= '\0';
        cache_key_len++;

	cache_key_len+= innobase_convert_to_system_charset(cache_key+cache_key_len,
				fk_table+db_name_len+1, sizeof(cache_key), &errors);
	if (errors) {
		WSREP_WARN("unexpected foreign key table %s %s",
			   foreign->referenced_table->name.m_name,
			   foreign->foreign_table->name.m_name);
		return DB_ERROR;
        }
#ifdef WSREP_DEBUG_PRINT
	ulint j;
	fprintf(stderr, "FK parent key, table: %s %s len: %lu ",
		cache_key, wsrep_key_type_to_str(key_type), len+1);
	for (j=0; j<len+1; j++) {
		fprintf(stderr, " %hhX, ", key[j]);
	}
	fprintf(stderr, "\n");
#endif
	wsrep_buf_t wkey_part[3];
        wsrep_key_t wkey = {wkey_part, 3};

	if (!wsrep_prepare_key_for_innodb(
		thd,
		(const uchar*)cache_key,
		cache_key_len +  1,
		(const uchar*)key, len+1,
		wkey_part,
		(size_t*)&wkey.key_parts_num)) {
		WSREP_WARN("key prepare failed for cascaded FK: %s",
			   wsrep_thd_query(thd));
		return DB_ERROR;
	}

	rcode = wsrep_thd_append_key(thd, &wkey, 1, key_type);

	if (rcode) {
		WSREP_ERROR("Appending cascaded fk row key failed: %s, "
			    ULINTPF,
			    wsrep_thd_query(thd),
			    rcode);
		return DB_ERROR;
	}

	if (pa_disable) {
		wsrep_thd_set_PA_unsafe(trx->mysql_thd);
	}

	return DB_SUCCESS;
}

static int
wsrep_append_key(
/*=============*/
	THD		*thd,
	trx_t 		*trx,
	TABLE_SHARE 	*table_share,
	const char*	key,
	uint16_t        key_len,
	Wsrep_service_key_type	key_type	/*!< in: access type of this key
					(shared, exclusive, semi...) */
)
{
	ut_ad(!trx->is_bulk_insert());

	DBUG_ENTER("wsrep_append_key");
	DBUG_PRINT("enter",
		    ("thd: %lu trx: %lld", thd_get_thread_id(thd),
		    (long long)trx->id));
#ifdef WSREP_DEBUG_PRINT
	fprintf(stderr, "%s conn %lu, trx " TRX_ID_FMT ", keylen %d, key %s.%s\n",
		wsrep_key_type_to_str(key_type),
		thd_get_thread_id(thd), trx->id, key_len,
		table_share->table_name.str, key);
	for (int i=0; i<key_len; i++) {
		fprintf(stderr, "%hhX, ", key[i]);
	}
	fprintf(stderr, "\n");
#endif
	wsrep_buf_t wkey_part[3];
        wsrep_key_t wkey = {wkey_part, 3};

	if (!wsrep_prepare_key_for_innodb(
			thd,
			(const uchar*)table_share->table_cache_key.str,
			table_share->table_cache_key.length,
			(const uchar*)key, key_len,
			wkey_part,
			(size_t*)&wkey.key_parts_num)) {
		WSREP_WARN("key prepare failed for: %s",
			   (wsrep_thd_query(thd)) ?
			   wsrep_thd_query(thd) : "void");
		DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
	}

	int rcode = wsrep_thd_append_key(thd, &wkey, 1, key_type);
	if (rcode) {
		DBUG_PRINT("wsrep", ("row key failed: %d", rcode));
		WSREP_WARN("Appending row key failed: %s, %d",
			   (wsrep_thd_query(thd)) ?
			   wsrep_thd_query(thd) : "void", rcode);
		DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
	}

	DBUG_RETURN(0);
}

static bool
referenced_by_foreign_key2(
/*=======================*/
	const dict_table_t* table,
	const dict_index_t* index) noexcept
{
	ut_ad(table != NULL);
	ut_ad(index != NULL);

	const dict_foreign_set* fks = &table->referenced_set;

	for (dict_foreign_set::const_iterator it = fks->begin();
             it != fks->end();
             ++it) {
                dict_foreign_t* foreign = *it;

                if (foreign->referenced_index != index) {
                        continue;
                }
                ut_ad(table == foreign->referenced_table);
                return true;
        }
        return false;
}

int
ha_innobase::wsrep_append_keys(
/*===========================*/
	THD 		*thd,
	Wsrep_service_key_type	key_type,	/*!< in: access type of this row
					operation:
					(shared, exclusive, reference...) */
	const uchar*	record0,	/* in: row in MySQL format */
	const uchar*	record1)	/* in: row in MySQL format */
{
	/* Sanity check: newly inserted records should always be passed with
	   EXCLUSIVE key type, all the rest are expected to carry a pre-image
	 */
	ut_a(record1 != NULL || key_type == WSREP_SERVICE_KEY_EXCLUSIVE);

	int rcode;
	DBUG_ENTER("wsrep_append_keys");

	bool key_appended = false;
	trx_t *trx = thd_to_trx(thd);

#ifdef WSREP_DEBUG_PRINT
	fprintf(stderr, "%s conn %lu, trx " TRX_ID_FMT ", table %s\nSQL: %s\n",
		wsrep_key_type_to_str(key_type),
		thd_get_thread_id(thd), trx->id,
		table_share->table_name.str, wsrep_thd_query(thd));
#endif

	if (table_share && table_share->tmp_table  != NO_TMP_TABLE) {
		WSREP_DEBUG("skipping tmp table DML: THD: %lu tmp: %d SQL: %s",
			    thd_get_thread_id(thd),
			    table_share->tmp_table,
			    (wsrep_thd_query(thd)) ?
			    wsrep_thd_query(thd) : "void");
		DBUG_RETURN(0);
	}

	if (wsrep_protocol_version == 0) {
		char 	keyval[WSREP_MAX_SUPPORTED_KEY_LENGTH+1] = {'\0'};
		char 	*key 		= &keyval[0];
		bool    is_null;

		auto len = wsrep_store_key_val_for_row(
			thd, table, 0, key, WSREP_MAX_SUPPORTED_KEY_LENGTH,
			record0, &is_null);

		if (!is_null) {
			rcode = wsrep_append_key(
				thd, trx, table_share, keyval,
				len, key_type);

			if (rcode) {
				DBUG_RETURN(rcode);
			}
		} else {
			WSREP_DEBUG("NULL key skipped (proto 0): %s",
				    wsrep_thd_query(thd));
		}
	} else {
		ut_a(table->s->keys <= 256);
		uint i;
                bool hasPK= false;

		for (i=0; i<table->s->keys; ++i) {
			KEY*  key_info	= table->key_info + i;
			if (key_info->flags & HA_NOSAME) {
				hasPK = true;
				break;
			}
		}

		for (i=0; i<table->s->keys; ++i) {
			KEY*  key_info	= table->key_info + i;

			dict_index_t* idx  = innobase_get_index(i);
			dict_table_t* tab  = (idx) ? idx->table : NULL;

			/* keyval[] shall contain an ordinal number at byte 0
			   and the actual key data shall be written at byte 1.
			   Hence the total data length is the key length + 1 */
			char keyval0[WSREP_MAX_SUPPORTED_KEY_LENGTH+1]= {'\0'};
			char keyval1[WSREP_MAX_SUPPORTED_KEY_LENGTH+1]= {'\0'};
			keyval0[0] = (char)i;
			keyval1[0] = (char)i;
			char* key0 = &keyval0[1];
			char* key1 = &keyval1[1];

			if (!tab) {
				WSREP_WARN("MariaDB-InnoDB key mismatch %s %s",
					   table->s->table_name.str,
					   key_info->name.str);
			}
			/* !hasPK == table with no PK,
			   must append all non-unique keys */
			if (!hasPK || key_info->flags & HA_NOSAME ||
			    ((tab &&
			      referenced_by_foreign_key2(tab, idx)) ||
			     (!tab && referenced_by_foreign_key()))) {

				bool is_null0;
				auto len0 = wsrep_store_key_val_for_row(
					thd, table, i, key0,
					WSREP_MAX_SUPPORTED_KEY_LENGTH,
					record0, &is_null0);

				if (record1) {
					bool is_null1;
					auto len1= wsrep_store_key_val_for_row(
						thd, table, i, key1,
						WSREP_MAX_SUPPORTED_KEY_LENGTH,
						record1, &is_null1);

					if (is_null0 != is_null1 ||
					    len0 != len1 ||
					    memcmp(key0, key1, len0)) {
						/* This key has changed. If it
						  is unique, this is an exclusive
						  operation -> upgrade key type */
						if (key_info->flags & HA_NOSAME) {
						    key_type = WSREP_SERVICE_KEY_EXCLUSIVE;
						}

						if (!is_null1) {
						    rcode = wsrep_append_key(
							thd, trx, table_share,
							keyval1,
						    /* for len1+1 see keyval1
						     initialization comment */
							uint16_t(len1+1),
							key_type);
						    if (rcode)
							DBUG_RETURN(rcode);
						}
					}
				}

				if (!is_null0) {
					rcode = wsrep_append_key(
						thd, trx, table_share,
						/* for len0+1 see keyval0
						   initialization comment */
						keyval0, uint16_t(len0+1),
						key_type);
					if (rcode)
						DBUG_RETURN(rcode);

					if (key_info->flags & HA_NOSAME  ||
					    key_type == WSREP_SERVICE_KEY_SHARED||
					    key_type == WSREP_SERVICE_KEY_REFERENCE)
						key_appended = true;
				} else {
					WSREP_DEBUG("NULL key skipped: %s",
						    wsrep_thd_query(thd));
				}
			}
		}
	}

	/* if no PK, calculate hash of full row, to be the key value */
	if (!key_appended && wsrep_certify_nonPK) {
		uchar digest[16];

		wsrep_calc_row_hash(digest, record0, table, m_prebuilt);

		if (int rcode = wsrep_append_key(thd, trx, table_share,
						 reinterpret_cast<char*>
						 (digest), 16, key_type)) {
			DBUG_RETURN(rcode);
		}

		if (record1) {
			wsrep_calc_row_hash(
				digest, record1, table, m_prebuilt);
			if (int rcode = wsrep_append_key(
				    thd, trx, table_share,
				    reinterpret_cast<char*>(digest), 16,
				    key_type)) {
				DBUG_RETURN(rcode);
			}
		}
		DBUG_RETURN(0);
	}

	DBUG_RETURN(0);
}
#endif /* WITH_WSREP */

/*********************************************************************//**
Stores a reference to the current row to 'ref' field of the handle. Note
that in the case where we have generated the clustered index for the
table, the function parameter is illogical: we MUST ASSUME that 'record'
is the current 'position' of the handle, because if row ref is actually
the row id internally generated in InnoDB, then 'record' does not contain
it. We just guess that the row id must be for the record where the handle
was positioned the last time. */

void
ha_innobase::position(
/*==================*/
	const uchar*	record)	/*!< in: row in MySQL format */
{
	uint		len;

	ut_a(m_prebuilt->trx == thd_to_trx(ha_thd()));

	if (m_prebuilt->clust_index_was_generated) {
		/* No primary key was defined for the table and we
		generated the clustered index from row id: the
		row reference will be the row id, not any key value
		that MySQL knows of */

		len = DATA_ROW_ID_LEN;

		memcpy(ref, m_prebuilt->row_id, len);
	} else {

		/* Copy primary key as the row reference */
		KEY*	key_info = table->key_info + m_primary_key;
		key_copy(ref, (uchar*)record, key_info, key_info->key_length);
		len = key_info->key_length;
	}

	ut_ad(len == ref_length);
}

/*****************************************************************//**
Check whether there exist a column named as "FTS_DOC_ID", which is
reserved for InnoDB FTS Doc ID
@return true if there exist a "FTS_DOC_ID" column */
static
bool
create_table_check_doc_id_col(
/*==========================*/
	trx_t*		trx,		/*!< in: InnoDB transaction handle */
	const TABLE*	form,		/*!< in: information on table
					columns and indexes */
	ulint*		doc_id_col)	/*!< out: Doc ID column number if
					there exist a FTS_DOC_ID column,
					ULINT_UNDEFINED if column is of the
					wrong type/name/size */
{
	for (ulint i = 0; i < form->s->fields; i++) {
		const Field* field = form->field[i];
		if (!field->stored_in_db()) {
			continue;
		}

		unsigned unsigned_type;

		auto col_type = get_innobase_type_from_mysql_type(
			&unsigned_type, field);

		auto col_len = field->pack_length();

		if (field->field_name.streq(FTS_DOC_ID)) {

			/* Note the name is case sensitive due to
			our internal query parser */
			if (col_type == DATA_INT
			    && !field->real_maybe_null()
			    && col_len == sizeof(doc_id_t)
			    && (strcmp(field->field_name.str,
				      FTS_DOC_ID.str) == 0)) {
				*doc_id_col = i;
			} else {
				push_warning_printf(
					trx->mysql_thd,
					Sql_condition::WARN_LEVEL_WARN,
					ER_ILLEGAL_HA_CREATE_OPTION,
					"InnoDB: FTS_DOC_ID column must be"
					" of BIGINT NOT NULL type, and named"
					" in all capitalized characters");
				my_error(ER_WRONG_COLUMN_NAME, MYF(0),
					 field->field_name.str);
				*doc_id_col = ULINT_UNDEFINED;
			}

			return(true);
		}
	}

	return(false);
}


/** Finds all base columns needed to compute a given generated column.
This is returned as a bitmap, in field->table->tmp_set.
Works for both dict_v_col_t and dict_s_col_t columns.
@param[in]	table		InnoDB table
@param[in]	field		MySQL field
@param[in,out]	col		virtual or stored column */
template <typename T>
void
prepare_vcol_for_base_setup(
/*========================*/
	const dict_table_t*	table,
	const Field*	field,
	T*		col)
{
	ut_ad(col->num_base == 0);
	ut_ad(col->base_col == NULL);

	MY_BITMAP *old_read_set = field->table->read_set;

	field->table->read_set = &field->table->tmp_set;

	bitmap_clear_all(&field->table->tmp_set);
	field->vcol_info->expr->walk(
		&Item::register_field_in_read_map, 1, field->table);
	col->num_base= bitmap_bits_set(&field->table->tmp_set)
		& dict_index_t::MAX_N_FIELDS;
	if (col->num_base != 0) {
		col->base_col = static_cast<dict_col_t**>(mem_heap_zalloc(
					table->heap, col->num_base * sizeof(
						* col->base_col)));
	}
	field->table->read_set= old_read_set;
}


/** Set up base columns for virtual column
@param[in]	table		InnoDB table
@param[in]	field		MySQL field
@param[in,out]	v_col		virtual column */
void
innodb_base_col_setup(
	dict_table_t*	table,
	const Field*	field,
	dict_v_col_t*	v_col)
{
	uint16_t n = 0;

	prepare_vcol_for_base_setup(table, field, v_col);

	for (uint i= 0; i < field->table->s->fields; ++i) {
		const Field* base_field = field->table->field[i];
		if (base_field->stored_in_db()
			&& bitmap_is_set(&field->table->tmp_set, i)) {
			ulint   z;

			for (z = 0; z < table->n_cols; z++) {
				const Lex_cstring name =
					dict_table_get_col_name(table, z);
				if (base_field->field_name.streq(name)) {
					break;
				}
			}

			ut_ad(z != table->n_cols);

			v_col->base_col[n] = dict_table_get_nth_col(table, z);
			ut_ad(v_col->base_col[n]->ind == z);
			n++;
		}
	}
	v_col->num_base= n & dict_index_t::MAX_N_FIELDS;
}

/** Set up base columns for stored column
@param[in]	table	InnoDB table
@param[in]	field	MySQL field
@param[in,out]	s_col	stored column */
void
innodb_base_col_setup_for_stored(
	const dict_table_t*	table,
	const Field*		field,
	dict_s_col_t*		s_col)
{
	ulint	n = 0;

	prepare_vcol_for_base_setup(table, field, s_col);

	for (uint i= 0; i < field->table->s->fields; ++i) {
		const Field* base_field = field->table->field[i];

		if (base_field->stored_in_db()
		    && bitmap_is_set(&field->table->tmp_set, i)) {
			ulint	z;
			for (z = 0; z < table->n_cols; z++) {
				const Lex_cstring name =
					dict_table_get_col_name(table, z);
				if (base_field->field_name.streq(name)) {
					break;
				}
			}

			ut_ad(z != table->n_cols);

			s_col->base_col[n] = dict_table_get_nth_col(table, z);
			n++;

			if (n == s_col->num_base) {
				break;
			}
		}
	}
	s_col->num_base= n;
}

/** Create a table definition to an InnoDB database.
@return ER_* level error */
inline MY_ATTRIBUTE((warn_unused_result))
int
create_table_info_t::create_table_def()
{
	dict_table_t*	table;
	ulint		nulls_allowed;
	unsigned	unsigned_type;
	ulint		binary_type;
	ulint		long_true_varchar;
	ulint		charset_no;
	ulint		doc_id_col = 0;
	ibool		has_doc_id_col = FALSE;
	mem_heap_t*	heap;
	ha_table_option_struct *options= m_form->s->option_struct;
	dberr_t		err = DB_SUCCESS;

	DBUG_ENTER("create_table_def");
	DBUG_PRINT("enter", ("table_name: %s", m_table_name));

	DBUG_ASSERT(m_trx->mysql_thd == m_thd);

	/* MySQL does the name length check. But we do additional check
	on the name length here */
	const size_t	table_name_len = strlen(m_table_name);
	if (table_name_len > MAX_FULL_NAME_LEN) {
		push_warning_printf(
			m_thd, Sql_condition::WARN_LEVEL_WARN,
			ER_TABLE_NAME,
			"InnoDB: Table Name or Database Name is too long");

		DBUG_RETURN(ER_TABLE_NAME);
	}

	if (m_table_name[table_name_len - 1] == '/') {
		push_warning_printf(
			m_thd, Sql_condition::WARN_LEVEL_WARN,
			ER_TABLE_NAME,
			"InnoDB: Table name is empty");

		DBUG_RETURN(ER_WRONG_TABLE_NAME);
	}

	/* Find out the number of virtual columns. */
	ulint num_v = 0;
	const bool omit_virtual = ha_innobase::omits_virtual_cols(*m_form->s);
	const ulint n_cols = omit_virtual
		? m_form->s->stored_fields : m_form->s->fields;

	if (!omit_virtual) {
		for (ulint i = 0; i < n_cols; i++) {
			num_v += !m_form->field[i]->stored_in_db();
		}
	}

	/* Check whether there already exists a FTS_DOC_ID column */
	if (create_table_check_doc_id_col(m_trx, m_form, &doc_id_col)){

		/* Raise error if the Doc ID column is of wrong type or name */
		if (doc_id_col == ULINT_UNDEFINED) {
			DBUG_RETURN(HA_ERR_GENERIC);
		} else {
			has_doc_id_col = TRUE;
		}
	}

	/* Adjust the number of columns for the FTS hidden field */
	const ulint actual_n_cols = n_cols
		+ (m_flags2 & DICT_TF2_FTS && !has_doc_id_col);

	table = dict_table_t::create({m_table_name,table_name_len}, nullptr,
				     actual_n_cols, num_v, m_flags, m_flags2);

	/* Set the hidden doc_id column. */
	if (m_flags2 & DICT_TF2_FTS) {
		table->fts->doc_col = has_doc_id_col
				      ? doc_id_col : n_cols - num_v;
	}

	/* Assume the tablespace is not available until we are able to
	import it.*/
	table->file_unreadable = m_creating_stub;

	if (DICT_TF_HAS_DATA_DIR(m_flags)) {
		ut_a(strlen(m_remote_path));

		table->data_dir_path = mem_heap_strdup(
			table->heap, m_remote_path);

	} else {
		table->data_dir_path = NULL;
	}

	heap = mem_heap_create(1000);
	auto _ = make_scope_exit([heap]() { mem_heap_free(heap); });

	ut_d(bool have_vers_start = false);
	ut_d(bool have_vers_end = false);

	for (ulint i = 0, j = 0; j < n_cols; i++) {
		Field*	field = m_form->field[i];
		ulint vers_row = 0;

		if (m_form->versioned()) {
			if (i == m_form->s->vers.start_fieldno) {
				vers_row = DATA_VERS_START;
				ut_d(have_vers_start = true);
			} else if (i == m_form->s->vers.end_fieldno) {
				vers_row = DATA_VERS_END;
				ut_d(have_vers_end = true);
			} else if (!(field->flags
				     & VERS_UPDATE_UNVERSIONED_FLAG)) {
				vers_row = DATA_VERSIONED;
			}
		}

		auto col_type = get_innobase_type_from_mysql_type(
			&unsigned_type, field);

		if (!col_type) {
			push_warning_printf(
				m_thd, Sql_condition::WARN_LEVEL_WARN,
				ER_CANT_CREATE_TABLE,
				"Error creating table '%s' with"
				" column '%s'. Please check its"
				" column type and try to re-create"
				" the table with an appropriate"
				" column type.",
				table->name.m_name, field->field_name.str);
err_col:
			dict_mem_table_free(table);
			DBUG_RETURN(HA_ERR_GENERIC);
		}

		nulls_allowed = field->real_maybe_null() ? 0 : DATA_NOT_NULL;
		binary_type = field->binary() ? DATA_BINARY_TYPE : 0;

		charset_no = 0;

		if (dtype_is_string_type(col_type)) {

			charset_no = (ulint) field->charset()->number;

			DBUG_EXECUTE_IF("simulate_max_char_col",
					charset_no = MAX_CHAR_COLL_NUM + 1;
					);

			if (charset_no > MAX_CHAR_COLL_NUM) {
				/* in data0type.h we assume that the
				number fits in one byte in prtype */
				push_warning_printf(
					m_thd, Sql_condition::WARN_LEVEL_WARN,
					ER_CANT_CREATE_TABLE,
					"In InnoDB, charset-collation codes"
					" must be below 256."
					" Unsupported code " ULINTPF ".",
					charset_no);
				dict_mem_table_free(table);

				DBUG_RETURN(ER_CANT_CREATE_TABLE);
			}
		}

		auto col_len = field->pack_length();

		/* The MySQL pack length contains 1 or 2 bytes length field
		for a true VARCHAR. Let us subtract that, so that the InnoDB
		column length in the InnoDB data dictionary is the real
		maximum byte length of the actual data. */

		long_true_varchar = 0;

		if (field->type() == MYSQL_TYPE_VARCHAR) {
			col_len -= ((Field_varstring*) field)->length_bytes;

			if (((Field_varstring*) field)->length_bytes == 2) {
				long_true_varchar = DATA_LONG_TRUE_VARCHAR;
			}
		}

		/* First check whether the column to be added has a
		system reserved name. */
		if (dict_col_name_is_reserved(field->field_name)){
			my_error(ER_WRONG_COLUMN_NAME, MYF(0),
				 field->field_name.str);
			goto err_col;
		}

		ulint is_virtual = !field->stored_in_db() ? DATA_VIRTUAL : 0;

		if (!is_virtual) {
			dict_mem_table_add_col(table, heap,
				field->field_name.str, col_type,
				dtype_form_prtype(
					(ulint) field->type()
					| nulls_allowed | unsigned_type
					| binary_type | long_true_varchar
					| vers_row,
					charset_no),
				col_len);
		} else if (!omit_virtual) {
			dict_mem_table_add_v_col(table, heap,
				field->field_name.str, col_type,
				dtype_form_prtype(
					(ulint) field->type()
					| nulls_allowed | unsigned_type
					| binary_type | long_true_varchar
					| vers_row
					| is_virtual,
					charset_no),
				col_len, i, 0);
		}

		if (innobase_is_s_fld(field)) {
			ut_ad(!is_virtual);
			/* Added stored column in m_s_cols list. */
			dict_mem_table_add_s_col(
				table, 0);
		}

		if (is_virtual && omit_virtual) {
			continue;
		}

		j++;
	}

	ut_ad(have_vers_start == have_vers_end);
	ut_ad(table->versioned() == have_vers_start);
	ut_ad(!table->versioned() || table->vers_start != table->vers_end);

	if (num_v) {
		for (ulint i = 0, j = 0; i < n_cols; i++) {
			dict_v_col_t*	v_col;

			const Field* field = m_form->field[i];

			if (field->stored_in_db()) {
				continue;
			}

			v_col = dict_table_get_nth_v_col(table, j);

			j++;

			innodb_base_col_setup(table, field, v_col);
		}
	}

	/** Fill base columns for the stored column present in the list. */
	if (table->s_cols && !table->s_cols->empty()) {
		for (ulint i = 0; i < n_cols; i++) {
			Field*  field = m_form->field[i];

			if (!innobase_is_s_fld(field)) {
				continue;
			}

			dict_s_col_list::iterator       it;
			for (it = table->s_cols->begin();
			     it != table->s_cols->end(); ++it) {
				dict_s_col_t	s_col = *it;

				if (s_col.s_pos == i) {
					innodb_base_col_setup_for_stored(
						table, field, &s_col);
					break;
				}
			}
		}
	}

	/* Add the FTS doc_id hidden column. */
	if (m_flags2 & DICT_TF2_FTS && !has_doc_id_col) {
		fts_add_doc_id_column(table, heap);
	}

	dict_table_add_system_columns(table, heap);

	if (table->is_temporary()) {
		if ((options->encryption == 1
		     && !innodb_encrypt_temporary_tables)
		    || (options->encryption == 2
			&& innodb_encrypt_temporary_tables)) {
			push_warning_printf(m_thd,
					    Sql_condition::WARN_LEVEL_WARN,
					    ER_ILLEGAL_HA_CREATE_OPTION,
					    "Ignoring encryption parameter during "
					    "temporary table creation.");
		}

		table->id = dict_sys.acquire_temporary_table_id();
		ut_ad(dict_tf_get_rec_format(table->flags)
		      != REC_FORMAT_COMPRESSED);
		table->space_id = SRV_TMP_SPACE_ID;
		table->space = fil_system.temp_space;
		table->add_to_cache();
	} else {
		ut_ad(dict_sys.sys_tables_exist());

		err = row_create_table_for_mysql(table, m_trx);
	}

	switch (err) {
	case DB_SUCCESS:
		ut_ad(table);
		m_table = table;
		DBUG_RETURN(0);
	default:
		break;
	case DB_DUPLICATE_KEY:
		char display_name[FN_REFLEN];
		char* buf_end = innobase_convert_identifier(
			display_name, sizeof(display_name) - 1,
			m_table_name, strlen(m_table_name),
			m_thd);

		*buf_end = '\0';

		my_error(ER_TABLE_EXISTS_ERROR, MYF(0), display_name);
	}

	DBUG_RETURN(convert_error_code_to_mysql(err, m_flags, m_thd));
}

/*****************************************************************//**
Creates an index in an InnoDB database. */
inline
int
create_index(
/*=========*/
	trx_t*		trx,		/*!< in: InnoDB transaction handle */
	const TABLE*	form,		/*!< in: information on table
					columns and indexes */
	dict_table_t*	table,		/*!< in,out: table */
	uint		key_num)	/*!< in: index number */
{
	dict_index_t*	index;
	int		error;
	const KEY*	key;
	ulint*		field_lengths;

	DBUG_ENTER("create_index");

	key = form->key_info + key_num;

	/* Assert that "GEN_CLUST_INDEX" cannot be used as non-primary index */
	ut_a(!key->name.streq(GEN_CLUST_INDEX));
	const ha_table_option_struct& o = *form->s->option_struct;

	if (key->algorithm == HA_KEY_ALG_FULLTEXT ||
	    key->algorithm == HA_KEY_ALG_RTREE) {
		/* Only one of these can be specified at a time. */
		ut_ad(!(key->flags & HA_NOSAME));
		index = dict_mem_index_create(table, key->name.str,
					      key->algorithm == HA_KEY_ALG_RTREE
					      ? DICT_SPATIAL : DICT_FTS,
					      key->user_defined_key_parts);

		for (ulint i = 0; i < key->user_defined_key_parts; i++) {
			const Field* field = key->key_part[i].field;

			/* We do not support special (Fulltext or Spatial)
			index on virtual columns */
			if (!field->stored_in_db()) {
				ut_ad(0);
				DBUG_RETURN(HA_ERR_UNSUPPORTED);
			}

			dict_mem_index_add_field(index, field->field_name.str,
						 0,
						 key->key_part->key_part_flag
						 & HA_REVERSE_SORT);
		}

		DBUG_RETURN(convert_error_code_to_mysql(
				    row_create_index_for_mysql(
					    index, trx, NULL,
					    fil_encryption_t(o.encryption),
					    uint32_t(o.encryption_key_id)),
				    table->flags, NULL));
	}

	ulint ind_type = 0;

	if (key_num == form->s->primary_key) {
		ind_type |= DICT_CLUSTERED;
	}

	if (key->flags & HA_NOSAME) {
		ind_type |= DICT_UNIQUE;
	}

	field_lengths = (ulint*) my_malloc(PSI_INSTRUMENT_ME,
		key->user_defined_key_parts * sizeof *
				field_lengths, MYF(MY_FAE));

	/* We pass 0 as the space id, and determine at a lower level the space
	id where to store the table */

	index = dict_mem_index_create(table, key->name.str,
				      ind_type, key->user_defined_key_parts);

	for (ulint i = 0; i < key->user_defined_key_parts; i++) {
		KEY_PART_INFO*	key_part = key->key_part + i;
		ulint		prefix_len;
		unsigned	is_unsigned;


		/* (The flag HA_PART_KEY_SEG denotes in MySQL a
		column prefix field in an index: we only store a
		specified number of first bytes of the column to
		the index field.) The flag does not seem to be
		properly set by MySQL. Let us fall back on testing
		the length of the key part versus the column.
		We first reach to the table's column; if the index is on a
		prefix, key_part->field is not the table's column (it's a
		"fake" field forged in open_table_from_share() with length
		equal to the length of the prefix); so we have to go to
		form->field. */
		Field*	field= form->field[key_part->field->field_index];
		if (field == NULL)
		  ut_error;

		const char*	field_name = key_part->field->field_name.str;

		auto col_type = get_innobase_type_from_mysql_type(
			&is_unsigned, key_part->field);

		if (DATA_LARGE_MTYPE(col_type)
		    || (key_part->length < field->pack_length()
			&& field->type() != MYSQL_TYPE_VARCHAR)
		    || (field->type() == MYSQL_TYPE_VARCHAR
			&& key_part->length < field->pack_length()
			- ((Field_varstring*) field)->length_bytes)) {

			switch (col_type) {
			default:
				prefix_len = key_part->length;
				break;
			case DATA_INT:
			case DATA_FLOAT:
			case DATA_DOUBLE:
			case DATA_DECIMAL:
				sql_print_error(
					"MariaDB is trying to create a column"
					" prefix index field, on an"
					" inappropriate data type. Table"
					" name %s, column name %s.",
					form->s->table_name.str,
					key_part->field->field_name.str);

				prefix_len = 0;
			}
		} else {
			prefix_len = 0;
		}

		ut_ad(prefix_len % field->charset()->mbmaxlen == 0);

		field_lengths[i] = key_part->length;

		if (!key_part->field->stored_in_db()) {
			index->type |= DICT_VIRTUAL;
		}

		dict_mem_index_add_field(index, field_name, prefix_len,
					 key_part->key_part_flag
					 & HA_REVERSE_SORT);
	}

	ut_ad(key->algorithm == HA_KEY_ALG_FULLTEXT || !(index->type & DICT_FTS));

	/* Even though we've defined max_supported_key_part_length, we
	still do our own checking using field_lengths to be absolutely
	sure we don't create too long indexes. */
	ulint flags = table->flags;

	error = convert_error_code_to_mysql(
		row_create_index_for_mysql(index, trx, field_lengths,
					   fil_encryption_t(o.encryption),
					   uint32_t(o.encryption_key_id)),
		flags, NULL);

	my_free(field_lengths);

	DBUG_RETURN(error);
}

/** Return a display name for the row format
@param[in]	row_format	Row Format
@return row format name */
static
const char*
get_row_format_name(
	enum row_type	row_format)
{
	switch (row_format) {
	case ROW_TYPE_COMPACT:
		return("COMPACT");
	case ROW_TYPE_COMPRESSED:
		return("COMPRESSED");
	case ROW_TYPE_DYNAMIC:
		return("DYNAMIC");
	case ROW_TYPE_REDUNDANT:
		return("REDUNDANT");
	case ROW_TYPE_DEFAULT:
		return("DEFAULT");
	case ROW_TYPE_FIXED:
		return("FIXED");
	case ROW_TYPE_PAGE:
	case ROW_TYPE_NOT_USED:
		break;
	}
	return("NOT USED");
}

/** Validate DATA DIRECTORY option.
@return true if valid, false if not. */
bool
create_table_info_t::create_option_data_directory_is_valid()
{
	bool		is_valid = true;

	ut_ad(m_create_info->data_file_name
	      && m_create_info->data_file_name[0] != '\0');

	/* Use DATA DIRECTORY only with file-per-table. */
	if (!m_allow_file_per_table) {
		push_warning(
			m_thd, Sql_condition::WARN_LEVEL_WARN,
			ER_ILLEGAL_HA_CREATE_OPTION,
			"InnoDB: DATA DIRECTORY requires"
			" innodb_file_per_table.");
		is_valid = false;
	}

	/* Do not use DATA DIRECTORY with TEMPORARY TABLE. */
	if (m_create_info->tmp_table()) {
		push_warning(
			m_thd, Sql_condition::WARN_LEVEL_WARN,
			ER_ILLEGAL_HA_CREATE_OPTION,
			"InnoDB: DATA DIRECTORY cannot be used"
			" for TEMPORARY tables.");
		is_valid = false;
	}

	/* We check for a DATA DIRECTORY mixed with TABLESPACE in
	create_option_tablespace_is_valid(), no need to here. */

	return(is_valid);
}

/** Validate the create options. Check that the options KEY_BLOCK_SIZE,
ROW_FORMAT, DATA DIRECTORY, TEMPORARY are compatible with
each other and other settings.  These CREATE OPTIONS are not validated
here unless innodb_strict_mode is on. With strict mode, this function
will report each problem it finds using a custom message with error
code ER_ILLEGAL_HA_CREATE_OPTION, not its built-in message.
@return NULL if valid, string name of bad option if not. */
const char*
create_table_info_t::create_options_are_invalid()
{
	bool	has_key_block_size = (m_create_info->key_block_size != 0);

	const char*	ret = NULL;
	enum row_type	row_format	= m_create_info->row_type;
	const bool	is_temp 	= m_create_info->tmp_table();

	ut_ad(m_thd != NULL);

	/* If innodb_strict_mode is not set don't do any more validation. */
	if (!THDVAR(m_thd, strict_mode)) {
		return(NULL);
	}

	/* Check if a non-zero KEY_BLOCK_SIZE was specified. */
	if (has_key_block_size) {
		if (is_temp || innodb_read_only_compressed) {
			my_error(ER_UNSUPPORTED_COMPRESSED_TABLE, MYF(0));
			return("KEY_BLOCK_SIZE");
		}

		switch (m_create_info->key_block_size) {
			ulong	kbs_max;
		case 1:
		case 2:
		case 4:
		case 8:
		case 16:
			/* The maximum KEY_BLOCK_SIZE (KBS) is
			UNIV_PAGE_SIZE_MAX. But if srv_page_size is
			smaller than UNIV_PAGE_SIZE_MAX, the maximum
			KBS is also smaller. */
			kbs_max = ut_min(
				1U << (UNIV_PAGE_SSIZE_MAX - 1),
				1U << (PAGE_ZIP_SSIZE_MAX - 1));
			if (m_create_info->key_block_size > kbs_max) {
				push_warning_printf(
					m_thd, Sql_condition::WARN_LEVEL_WARN,
					ER_ILLEGAL_HA_CREATE_OPTION,
					"InnoDB: KEY_BLOCK_SIZE=%lu"
					" cannot be larger than %lu.",
					m_create_info->key_block_size,
					kbs_max);
				ret = "KEY_BLOCK_SIZE";
			}

			/* Valid KEY_BLOCK_SIZE, check its dependencies. */
			if (!m_allow_file_per_table) {
				push_warning(
					m_thd, Sql_condition::WARN_LEVEL_WARN,
					ER_ILLEGAL_HA_CREATE_OPTION,
					"InnoDB: KEY_BLOCK_SIZE requires"
					" innodb_file_per_table.");
				ret = "KEY_BLOCK_SIZE";
			}
			break;
		default:
			push_warning_printf(
				m_thd, Sql_condition::WARN_LEVEL_WARN,
				ER_ILLEGAL_HA_CREATE_OPTION,
				"InnoDB: invalid KEY_BLOCK_SIZE = %u."
				" Valid values are [1, 2, 4, 8, 16]",
				(uint) m_create_info->key_block_size);
			ret = "KEY_BLOCK_SIZE";
			break;
		}
	}

	/* Check for a valid InnoDB ROW_FORMAT specifier and
	other incompatibilities. */
	switch (row_format) {
	case ROW_TYPE_COMPRESSED:
		if (is_temp || innodb_read_only_compressed) {
			my_error(ER_UNSUPPORTED_COMPRESSED_TABLE, MYF(0));
			return("ROW_FORMAT");
		}
		if (!m_allow_file_per_table) {
			push_warning_printf(
				m_thd, Sql_condition::WARN_LEVEL_WARN,
				ER_ILLEGAL_HA_CREATE_OPTION,
				"InnoDB: ROW_FORMAT=%s requires"
				" innodb_file_per_table.",
				get_row_format_name(row_format));
			ret = "ROW_FORMAT";
		}
		break;
	case ROW_TYPE_DYNAMIC:
	case ROW_TYPE_COMPACT:
	case ROW_TYPE_REDUNDANT:
		if (has_key_block_size) {
			push_warning_printf(
				m_thd, Sql_condition::WARN_LEVEL_WARN,
				ER_ILLEGAL_HA_CREATE_OPTION,
				"InnoDB: cannot specify ROW_FORMAT = %s"
				" with KEY_BLOCK_SIZE.",
				get_row_format_name(row_format));
			ret = "KEY_BLOCK_SIZE";
		}
		break;
	case ROW_TYPE_DEFAULT:
		break;
	case ROW_TYPE_FIXED:
	case ROW_TYPE_PAGE:
	case ROW_TYPE_NOT_USED:
		push_warning(
			m_thd, Sql_condition::WARN_LEVEL_WARN,
			ER_ILLEGAL_HA_CREATE_OPTION,
			"InnoDB: invalid ROW_FORMAT specifier.");
		ret = "ROW_TYPE";
		break;
	}

	if (!m_create_info->data_file_name
	    || !m_create_info->data_file_name[0]) {
	} else if (!my_use_symdir && !m_create_info->recreate_identical_table) {
		my_error(WARN_OPTION_IGNORED, MYF(ME_WARNING),
			 "DATA DIRECTORY");
	} else if (!create_option_data_directory_is_valid()) {
		ret = "DATA DIRECTORY";
	}

	/* Don't support compressed table when page size > 16k. */
	if ((has_key_block_size || row_format == ROW_TYPE_COMPRESSED)
	    && srv_page_size > UNIV_PAGE_SIZE_DEF) {
		push_warning(m_thd, Sql_condition::WARN_LEVEL_WARN,
			     ER_ILLEGAL_HA_CREATE_OPTION,
			     "InnoDB: Cannot create a COMPRESSED table"
			     " when innodb_page_size > 16k.");

		if (has_key_block_size) {
			ret = "KEY_BLOCK_SIZE";
		} else {
			ret = "ROW_TYPE";
		}
	}

	return(ret);
}

/*****************************************************************//**
Check engine specific table options not handled by SQL-parser.
@return	NULL if valid, string if not */
const char*
create_table_info_t::check_table_options()
{
	enum row_type row_format = m_create_info->row_type;
	const ha_table_option_struct *options= m_form->s->option_struct;

	switch (options->encryption) {
	case FIL_ENCRYPTION_OFF:
		if (options->encryption_key_id != FIL_DEFAULT_ENCRYPTION_KEY) {
			push_warning(
				m_thd, Sql_condition::WARN_LEVEL_WARN,
				HA_WRONG_CREATE_OPTION,
				"InnoDB: ENCRYPTED=NO implies"
				" ENCRYPTION_KEY_ID=1");
			compile_time_assert(FIL_DEFAULT_ENCRYPTION_KEY == 1);
		}
		if (srv_encrypt_tables != 2) {
			break;
		}
		push_warning(
			m_thd, Sql_condition::WARN_LEVEL_WARN,
			HA_WRONG_CREATE_OPTION,
			"InnoDB: ENCRYPTED=NO cannot be used with"
			" innodb_encrypt_tables=FORCE");
		return "ENCRYPTED";
	case FIL_ENCRYPTION_DEFAULT:
		if (!srv_encrypt_tables) {
			break;
		}
		/* fall through */
	case FIL_ENCRYPTION_ON:
		const uint32_t key_id = uint32_t(options->encryption_key_id);
		if (!encryption_key_id_exists(key_id)) {
			push_warning_printf(
				m_thd, Sql_condition::WARN_LEVEL_WARN,
				HA_WRONG_CREATE_OPTION,
				"InnoDB: ENCRYPTION_KEY_ID %u not available",
				key_id);
			return "ENCRYPTION_KEY_ID";
		}

		/* We do not support encryption for spatial indexes,
		except if innodb_checksum_algorithm=full_crc32.
		Do not allow ENCRYPTED=YES if any SPATIAL INDEX exists. */
		if (options->encryption != FIL_ENCRYPTION_ON
		    || srv_checksum_algorithm
		    >= SRV_CHECKSUM_ALGORITHM_FULL_CRC32) {
			break;
		}
		for (ulint i = 0; i < m_form->s->keys; i++) {
			if (m_form->key_info[i].algorithm == HA_KEY_ALG_RTREE) {
				push_warning(m_thd,
					     Sql_condition::WARN_LEVEL_WARN,
					     HA_ERR_UNSUPPORTED,
					     "InnoDB: ENCRYPTED=YES is not"
					     " supported for SPATIAL INDEX");
				return "ENCRYPTED";
			}
		}
	}

	if (!m_allow_file_per_table
	    && options->encryption != FIL_ENCRYPTION_DEFAULT) {
		push_warning(
			m_thd, Sql_condition::WARN_LEVEL_WARN,
			HA_WRONG_CREATE_OPTION,
			"InnoDB: ENCRYPTED requires innodb_file_per_table");
		return "ENCRYPTED";
 	}

	/* Check page compression requirements */
	if (options->page_compressed) {

		if (row_format == ROW_TYPE_COMPRESSED) {
			push_warning(
				m_thd, Sql_condition::WARN_LEVEL_WARN,
				HA_WRONG_CREATE_OPTION,
				"InnoDB: PAGE_COMPRESSED table can't have"
				" ROW_TYPE=COMPRESSED");
			return "PAGE_COMPRESSED";
		}

		switch (row_format) {
		default:
			break;
		case ROW_TYPE_DEFAULT:
			if (m_default_row_format
			    != DEFAULT_ROW_FORMAT_REDUNDANT) {
				break;
			}
			/* fall through */
		case ROW_TYPE_REDUNDANT:
			push_warning(
				m_thd, Sql_condition::WARN_LEVEL_WARN,
				HA_WRONG_CREATE_OPTION,
				"InnoDB: PAGE_COMPRESSED table can't have"
				" ROW_TYPE=REDUNDANT");
			return "PAGE_COMPRESSED";
		}

		if (!m_allow_file_per_table) {
			push_warning(
				m_thd, Sql_condition::WARN_LEVEL_WARN,
				HA_WRONG_CREATE_OPTION,
				"InnoDB: PAGE_COMPRESSED requires"
				" innodb_file_per_table.");
			return "PAGE_COMPRESSED";
		}

		if (m_create_info->key_block_size) {
			push_warning(
				m_thd, Sql_condition::WARN_LEVEL_WARN,
				HA_WRONG_CREATE_OPTION,
				"InnoDB: PAGE_COMPRESSED table can't have"
				" key_block_size");
			return "PAGE_COMPRESSED";
		}
	}

	/* Check page compression level requirements, some of them are
	already checked above */
	if (options->page_compression_level != 0) {
		if (options->page_compressed == false) {
			push_warning(
				m_thd, Sql_condition::WARN_LEVEL_WARN,
				HA_WRONG_CREATE_OPTION,
				"InnoDB: PAGE_COMPRESSION_LEVEL requires"
				" PAGE_COMPRESSED");
			return "PAGE_COMPRESSION_LEVEL";
		}

		if (options->page_compression_level < 1 || options->page_compression_level > 9) {
			push_warning_printf(
				m_thd, Sql_condition::WARN_LEVEL_WARN,
				HA_WRONG_CREATE_OPTION,
				"InnoDB: invalid PAGE_COMPRESSION_LEVEL = %llu."
				" Valid values are [1, 2, 3, 4, 5, 6, 7, 8, 9]",
				options->page_compression_level);
			return "PAGE_COMPRESSION_LEVEL";
		}
	}

	return NULL;
}

/*****************************************************************//**
Update create_info.  Used in SHOW CREATE TABLE et al. */

void
ha_innobase::update_create_info(
/*============================*/
	HA_CREATE_INFO*	create_info)	/*!< in/out: create info */
{
	if (!(create_info->used_fields & HA_CREATE_USED_AUTO)) {
		info(HA_STATUS_AUTO);
		create_info->auto_increment_value = stats.auto_increment_value;
	}

	if (m_prebuilt->table->is_temporary()) {
		return;
	}

	dict_get_and_save_data_dir_path(m_prebuilt->table);

	if (m_prebuilt->table->data_dir_path) {
		create_info->data_file_name = m_prebuilt->table->data_dir_path;
	}
}

/*****************************************************************//**
Initialize the table FTS stopword list
@return TRUE if success */
ibool
innobase_fts_load_stopword(
/*=======================*/
	dict_table_t*	table,	/*!< in: Table has the FTS */
	trx_t*		trx,	/*!< in: transaction */
	THD*		thd)	/*!< in: current thread */
{
  ut_ad(dict_sys.locked());

  const char *stopword_table= THDVAR(thd, ft_user_stopword_table);
  if (!stopword_table)
  {
    mysql_mutex_lock(&LOCK_global_system_variables);
    if (innobase_server_stopword_table)
      stopword_table= thd_strdup(thd, innobase_server_stopword_table);
    mysql_mutex_unlock(&LOCK_global_system_variables);
  }

  table->fts->dict_locked= true;
  bool success= fts_load_stopword(table, trx, stopword_table,
                                  THDVAR(thd, ft_enable_stopword), false);
  table->fts->dict_locked= false;
  return success;
}

/** Parse the table name into normal name and remote path if needed.
@param[in]	name	Table name (db/table or full path).
@return 0 if successful, otherwise, error number */
int
create_table_info_t::parse_table_name(
	const char*
#ifdef _WIN32
	name
#endif
				      )
{
	DBUG_ENTER("parse_table_name");

#ifdef _WIN32
	/* Names passed in from server are in two formats:
	1. <database_name>/<table_name>: for normal table creation
	2. full path: for temp table creation, or DATA DIRECTORY.

	When srv_file_per_table is on and mysqld_embedded is off,
	check for full path pattern, i.e.
	X:\dir\...,		X is a driver letter, or
	\\dir1\dir2\...,	UNC path
	returns error if it is in full path format, but not creating a temp.
	table. Currently InnoDB does not support symbolic link on Windows. */

	if (m_innodb_file_per_table
	    && !mysqld_embedded
	    && !m_create_info->tmp_table()) {

		if ((name[1] == ':')
		    || (name[0] == '\\' && name[1] == '\\')) {
			sql_print_error("Cannot create table %s\n", name);
			DBUG_RETURN(HA_ERR_GENERIC);
		}
	}
#endif

	m_remote_path[0] = '\0';

	/* Make sure DATA DIRECTORY is compatible with other options
	and set the remote path.  In the case of either;
	  CREATE TEMPORARY TABLE ... DATA DIRECTORY={path} ... ;
	  CREATE TABLE ... DATA DIRECTORY={path} TABLESPACE={name}... ;
	we ignore the DATA DIRECTORY. */
	if (m_create_info->data_file_name
	    && m_create_info->data_file_name[0]
	    && (my_use_symdir || m_create_info->recreate_identical_table)) {
		if (!create_option_data_directory_is_valid()) {
			push_warning_printf(
				m_thd, Sql_condition::WARN_LEVEL_WARN,
				WARN_OPTION_IGNORED,
				ER_DEFAULT(WARN_OPTION_IGNORED),
				"DATA DIRECTORY");

			m_flags &= ~DICT_TF_MASK_DATA_DIR;
		} else {
			strmake(m_remote_path,
				m_create_info->data_file_name,
				FN_REFLEN - 2);
		}
	}

	if (m_create_info->index_file_name && m_form->s->keys &&
	    (!(m_flags & DICT_TF_MASK_DATA_DIR) ||
	     strcmp(m_remote_path, m_create_info->index_file_name))) {
		my_error(WARN_OPTION_IGNORED, ME_NOTE, "INDEX DIRECTORY");
	}

	DBUG_RETURN(0);
}

/** @return whether innodb_strict_mode is active */
bool ha_innobase::is_innodb_strict_mode(THD *thd)
{
  return THDVAR(thd, strict_mode);
}

/** Determine InnoDB table flags.
If strict_mode=OFF, this will adjust the flags to what should be assumed.
@retval true on success
@retval false on error */
bool create_table_info_t::innobase_table_flags()
{
	DBUG_ENTER("innobase_table_flags");

	const char*	fts_doc_id_index_bad = NULL;
	ulint		zip_ssize = 0;
	enum row_type	row_type;
	rec_format_t	innodb_row_format =
		get_row_format(m_default_row_format);
	const bool	is_temp = m_create_info->tmp_table();
	bool		zip_allowed = !is_temp;

	const ulint	zip_ssize_max =
		ut_min(static_cast<ulint>(UNIV_PAGE_SSIZE_MAX),
		       static_cast<ulint>(PAGE_ZIP_SSIZE_MAX));

	ha_table_option_struct *options= m_form->s->option_struct;

	m_flags = 0;
	m_flags2 = 0;

	/* Check if there are any FTS indexes defined on this table. */
	const uint fts_n_uniq= m_form->versioned() ? 2 : 1;
	for (uint i = 0; i < m_form->s->keys; i++) {
		const KEY*	key = &m_form->key_info[i];

		if (key->algorithm == HA_KEY_ALG_FULLTEXT) {
			m_flags2 |= DICT_TF2_FTS;

			/* We don't support FTS indexes in temporary
			tables. */
			if (is_temp) {
				my_error(ER_NO_INDEX_ON_TEMPORARY, MYF(0),
					 "FULLTEXT",  "InnoDB");
				DBUG_RETURN(false);
			}

			if (fts_doc_id_index_bad) {
				goto index_bad;
			}
		}

		if (!key->name.streq(FTS_DOC_ID_INDEX)) {
			continue;
		}

		/* Do a pre-check on FTS DOC ID index */
		if (!(key->flags & HA_NOSAME)
		    || key->user_defined_key_parts != fts_n_uniq
		    || (key->key_part[0].key_part_flag & HA_REVERSE_SORT)
		    || strcmp(key->name.str, FTS_DOC_ID_INDEX.str)
		    || strcmp(key->key_part[0].field->field_name.str,
			      FTS_DOC_ID.str)) {
			fts_doc_id_index_bad = key->name.str;
		}

		if (fts_doc_id_index_bad && (m_flags2 & DICT_TF2_FTS)) {
index_bad:
			my_error(ER_INNODB_FT_WRONG_DOCID_INDEX, MYF(0),
				 fts_doc_id_index_bad);
			DBUG_RETURN(false);
		}
	}

	if (m_create_info->key_block_size > 0) {
		/* The requested compressed page size (key_block_size)
		is given in kilobytes. If it is a valid number, store
		that value as the number of log2 shifts from 512 in
		zip_ssize. Zero means it is not compressed. */
		ulint	zssize;		/* Zip Shift Size */
		ulint	kbsize;		/* Key Block Size */
		for (zssize = kbsize = 1;
		     zssize <= zip_ssize_max;
		     zssize++, kbsize <<= 1) {
			if (kbsize == m_create_info->key_block_size) {
				zip_ssize = zssize;
				break;
			}
		}

		/* Make sure compressed row format is allowed. */
		if (is_temp) {
			push_warning(
				m_thd, Sql_condition::WARN_LEVEL_WARN,
				ER_ILLEGAL_HA_CREATE_OPTION,
				"InnoDB: KEY_BLOCK_SIZE is ignored"
				" for TEMPORARY TABLE.");
			zip_allowed = false;
		} else if (!m_allow_file_per_table) {
			push_warning(
				m_thd, Sql_condition::WARN_LEVEL_WARN,
				ER_ILLEGAL_HA_CREATE_OPTION,
				"InnoDB: KEY_BLOCK_SIZE requires"
				" innodb_file_per_table.");
			zip_allowed = false;
		}

		if (!zip_allowed
		    || zssize > zip_ssize_max) {
			push_warning_printf(
				m_thd, Sql_condition::WARN_LEVEL_WARN,
				ER_ILLEGAL_HA_CREATE_OPTION,
				"InnoDB: ignoring KEY_BLOCK_SIZE=%u.",
				(uint) m_create_info->key_block_size);
		}
	}

	/* If we are trying to import a tablespace, mark tablespace as
	discarded. */
	m_flags2 |= ulint{m_creating_stub} << DICT_TF2_POS_DISCARDED;

	row_type = m_create_info->row_type;

	if (zip_ssize && zip_allowed) {
		/* if ROW_FORMAT is set to default,
		automatically change it to COMPRESSED. */
		if (row_type == ROW_TYPE_DEFAULT) {
			row_type = ROW_TYPE_COMPRESSED;
		} else if (row_type != ROW_TYPE_COMPRESSED) {
			/* ROW_FORMAT other than COMPRESSED
			ignores KEY_BLOCK_SIZE.  It does not
			make sense to reject conflicting
			KEY_BLOCK_SIZE and ROW_FORMAT, because
			such combinations can be obtained
			with ALTER TABLE anyway. */
			push_warning_printf(
				m_thd, Sql_condition::WARN_LEVEL_WARN,
				ER_ILLEGAL_HA_CREATE_OPTION,
				"InnoDB: ignoring KEY_BLOCK_SIZE=%u"
				" unless ROW_FORMAT=COMPRESSED.",
				(uint) m_create_info->key_block_size);
			zip_allowed = false;
		}
	} else {
		/* zip_ssize == 0 means no KEY_BLOCK_SIZE. */
		if (row_type == ROW_TYPE_COMPRESSED && zip_allowed) {
			/* ROW_FORMAT=COMPRESSED without KEY_BLOCK_SIZE
			implies half the maximum KEY_BLOCK_SIZE(*1k) or
			srv_page_size, whichever is less. */
			zip_ssize = zip_ssize_max - 1;
		}
	}

	/* Validate the row format.  Correct it if necessary */

	switch (row_type) {
	case ROW_TYPE_REDUNDANT:
		innodb_row_format = REC_FORMAT_REDUNDANT;
		break;
	case ROW_TYPE_COMPACT:
		innodb_row_format = REC_FORMAT_COMPACT;
		break;
	case ROW_TYPE_COMPRESSED:
		if (is_temp) {
			push_warning_printf(
				m_thd, Sql_condition::WARN_LEVEL_WARN,
				ER_ILLEGAL_HA_CREATE_OPTION,
				"InnoDB: ROW_FORMAT=%s is ignored for"
				" TEMPORARY TABLE.",
				get_row_format_name(row_type));
		} else if (!m_allow_file_per_table) {
			push_warning_printf(
				m_thd, Sql_condition::WARN_LEVEL_WARN,
				ER_ILLEGAL_HA_CREATE_OPTION,
				"InnoDB: ROW_FORMAT=COMPRESSED requires"
				" innodb_file_per_table.");
		} else {
			innodb_row_format = REC_FORMAT_COMPRESSED;
			break;
		}
		zip_allowed = false;
		/* Set ROW_FORMAT = COMPACT */
		/* fall through */
	case ROW_TYPE_NOT_USED:
	case ROW_TYPE_FIXED:
	case ROW_TYPE_PAGE:
		push_warning(
			m_thd, Sql_condition::WARN_LEVEL_WARN,
			ER_ILLEGAL_HA_CREATE_OPTION,
			"InnoDB: assuming ROW_FORMAT=DYNAMIC.");
		/* fall through */
	case ROW_TYPE_DYNAMIC:
		innodb_row_format = REC_FORMAT_DYNAMIC;
		break;
	case ROW_TYPE_DEFAULT:
		;
	}

	/* Don't support compressed table when page size > 16k. */
	if (zip_allowed && zip_ssize && srv_page_size > UNIV_PAGE_SIZE_DEF) {
		push_warning(m_thd, Sql_condition::WARN_LEVEL_WARN,
			     ER_ILLEGAL_HA_CREATE_OPTION,
			     "InnoDB: Cannot create a COMPRESSED table"
			     " when innodb_page_size > 16k."
			     " Assuming ROW_FORMAT=DYNAMIC.");
		zip_allowed = false;
	}

	ut_ad(!is_temp || !zip_allowed);
	ut_ad(!is_temp || innodb_row_format != REC_FORMAT_COMPRESSED);

	/* Set the table flags */
	if (!zip_allowed) {
		zip_ssize = 0;
	}

	ulint level = 0;

	if (is_temp) {
		m_flags2 |= DICT_TF2_TEMPORARY;
	} else {
		if (m_use_file_per_table) {
			m_flags2 |= DICT_TF2_USE_FILE_PER_TABLE;
		}

		level = ulint(options->page_compression_level);
		if (!level) {
			level = page_zip_level;
			if (!level && options->page_compressed) {
				push_warning_printf(
					m_thd, Sql_condition::WARN_LEVEL_WARN,
					ER_ILLEGAL_HA_CREATE_OPTION,
					"InnoDB: PAGE_COMPRESSED requires"
					" PAGE_COMPRESSION_LEVEL or"
					" innodb_compression_level > 0");
				DBUG_RETURN(false);
			}
		}
	}

	/* Set the table flags */
	dict_tf_set(&m_flags, innodb_row_format, zip_ssize,
		    m_use_data_dir, level && options->page_compressed, level);

	if (m_form->s->table_type == TABLE_TYPE_SEQUENCE) {
		m_flags |= DICT_TF_MASK_NO_ROLLBACK;
	}

	/* Set the flags2 when create table or alter tables */
	m_flags2 |= DICT_TF2_FTS_AUX_HEX_NAME;

	DBUG_RETURN(true);
}

/** Parse MERGE_THRESHOLD value from the string.
@param[in]	thd	connection
@param[in]	str	string which might include 'MERGE_THRESHOLD='
@return	value parsed. 0 means not found or invalid value. */
static
unsigned
innobase_parse_merge_threshold(
	THD*		thd,
	const char*	str)
{
	static const char*	label = "MERGE_THRESHOLD=";
	static const size_t	label_len = strlen(label);
	const char*		pos = str;

	pos = strstr(str, label);

	if (pos == NULL) {
		return(0);
	}

	pos += label_len;

	lint	ret = atoi(pos);

	if (ret > 0 && ret <= 50) {
		return(static_cast<unsigned>(ret));
	}

	push_warning_printf(
		thd, Sql_condition::WARN_LEVEL_WARN,
		ER_ILLEGAL_HA_CREATE_OPTION,
		"InnoDB: Invalid value for MERGE_THRESHOLD in the CREATE TABLE"
		" statement. The value is ignored.");

	return(0);
}

/** Parse hint for table and its indexes, and update the information
in dictionary.
@param[in]	thd		connection
@param[in,out]	table		target table
@param[in]	table_share	table definition */
void
innobase_parse_hint_from_comment(
	THD*			thd,
	dict_table_t*		table,
	const TABLE_SHARE*	table_share)
{
	unsigned merge_threshold_table;
	unsigned merge_threshold_index[MAX_KEY];
	bool	is_found[MAX_KEY];

	if (table_share->comment.str != NULL) {
		merge_threshold_table
			= innobase_parse_merge_threshold(
				thd, table_share->comment.str);
	} else {
		merge_threshold_table = DICT_INDEX_MERGE_THRESHOLD_DEFAULT;
	}

	if (merge_threshold_table == 0) {
		merge_threshold_table = DICT_INDEX_MERGE_THRESHOLD_DEFAULT;
	}

	for (uint i = 0; i < table_share->keys; i++) {
		KEY*	key_info = &table_share->key_info[i];

		ut_ad(i < sizeof(merge_threshold_index)
			  / sizeof(merge_threshold_index[0]));

		if (key_info->flags & HA_USES_COMMENT
		    && key_info->comment.str != NULL) {
			merge_threshold_index[i]
				= innobase_parse_merge_threshold(
					thd, key_info->comment.str);
		} else {
			merge_threshold_index[i] = merge_threshold_table;
		}

		if (merge_threshold_index[i] == 0) {
			merge_threshold_index[i] = merge_threshold_table;
		}
	}

	/* update SYS_INDEX table */
	if (!table->is_temporary()) {
		for (uint i = 0; i < table_share->keys; i++) {
			is_found[i] = false;
		}

		for (dict_index_t* index = UT_LIST_GET_FIRST(table->indexes);
		     index != NULL;
		     index = UT_LIST_GET_NEXT(indexes, index)) {

			if (dict_index_is_auto_gen_clust(index)) {

				/* GEN_CLUST_INDEX should use
				merge_threshold_table */
				dict_index_set_merge_threshold(
					index, merge_threshold_table);
				continue;
			}

			const Lex_cstring_strlen index_name(index->name);
			for (uint i = 0; i < table_share->keys; i++) {
				if (is_found[i]) {
					continue;
				}

				KEY*	key_info = &table_share->key_info[i];

				if (key_info->name.streq(index_name)) {

					dict_index_set_merge_threshold(
						index,
						merge_threshold_index[i]);
					is_found[i] = true;
					break;
				}
			}
		}
	}

	for (uint i = 0; i < table_share->keys; i++) {
		is_found[i] = false;
	}

	/* update in memory */
	for (dict_index_t* index = UT_LIST_GET_FIRST(table->indexes);
	     index != NULL;
	     index = UT_LIST_GET_NEXT(indexes, index)) {

		if (dict_index_is_auto_gen_clust(index)) {

			/* GEN_CLUST_INDEX should use merge_threshold_table */

			/* x-lock index is needed to exclude concurrent
			pessimistic tree operations */
			index->lock.x_lock(SRW_LOCK_CALL);
			index->merge_threshold = merge_threshold_table
				& ((1U << 6) - 1);
			index->lock.x_unlock();

			continue;
		}

		const Lex_cstring_strlen index_name(index->name);
		for (uint i = 0; i < table_share->keys; i++) {
			if (is_found[i]) {
				continue;
			}

			KEY*	key_info = &table_share->key_info[i];

			if (key_info->name.streq(index_name)) {
				/* x-lock index is needed to exclude concurrent
				pessimistic tree operations */
				index->lock.x_lock(SRW_LOCK_CALL);
				index->merge_threshold
					= merge_threshold_index[i]
					& ((1U << 6) - 1);
				index->lock.x_unlock();
				is_found[i] = true;

				break;
			}
		}
	}
}

/** Set m_use_* flags. */
void
create_table_info_t::set_tablespace_type(
	bool	table_being_altered_is_file_per_table)
{
	/** Allow file_per_table for this table either because:
	1) the setting innodb_file_per_table=on,
	2) the table being altered is currently file_per_table */
	m_allow_file_per_table =
		m_innodb_file_per_table
		|| table_being_altered_is_file_per_table;

	/* Ignore the current innodb-file-per-table setting if we are
	creating a temporary table. */
	m_use_file_per_table = m_allow_file_per_table
		&& !m_create_info->tmp_table();

	/* DATA DIRECTORY must have m_use_file_per_table but cannot be
	used with TEMPORARY tables. */
	m_use_data_dir =
		m_use_file_per_table
		&& m_create_info->data_file_name
		&& m_create_info->data_file_name[0]
               && (my_use_symdir || m_create_info->recreate_identical_table);
}

/** Initialize the create_table_info_t object.
@return error number */
int
create_table_info_t::initialize()
{
	DBUG_ENTER("create_table_info_t::initialize");

	ut_ad(m_thd != NULL);
	ut_ad(m_create_info != NULL);

	if (m_form->s->fields > REC_MAX_N_USER_FIELDS) {
		DBUG_RETURN(HA_ERR_TOO_MANY_FIELDS);
	}

	/* Check for name conflicts (with reserved name) for
	any user indices to be created. */
	if (innobase_index_name_is_reserved(m_thd, m_form->key_info,
					    m_form->s->keys)) {
		DBUG_RETURN(HA_ERR_WRONG_INDEX);
	}

	/* Get the transaction associated with the current thd, or create one
	if not yet created */

	check_trx_exists(m_thd);

	DBUG_RETURN(0);
}


/** Check if a virtual column is part of a fulltext or spatial index. */
bool
create_table_info_t::gcols_in_fulltext_or_spatial()
{
	for (ulint i = 0; i < m_form->s->keys; i++) {
		const KEY*	key = m_form->key_info + i;
		if (key->algorithm != HA_KEY_ALG_RTREE &&
		    key->algorithm != HA_KEY_ALG_FULLTEXT) {
			continue;
		}
		for (ulint j = 0; j < key->user_defined_key_parts; j++) {
			/* We do not support special (Fulltext or
			Spatial) index on virtual columns */
			if (!key->key_part[j].field->stored_in_db()) {
				my_error(ER_UNSUPPORTED_ACTION_ON_GENERATED_COLUMN, MYF(0));
				return true;
			}
		}
	}
	return false;
}


/** Prepare to create a new table to an InnoDB database.
@param[in]	name	Table name
@return error number */
int create_table_info_t::prepare_create_table(const char* name, bool strict)
{
	DBUG_ENTER("prepare_create_table");

	ut_ad(m_thd != NULL);
	ut_ad(m_create_info != NULL);

	set_tablespace_type(false);

	normalize_table_name(m_table_name, sizeof(m_table_name), name);

	/* Validate table options not handled by the SQL-parser */
	if (check_table_options()) {
		DBUG_RETURN(HA_WRONG_CREATE_OPTION);
	}

	/* Validate the create options if innodb_strict_mode is set.
	Do not use the regular message for ER_ILLEGAL_HA_CREATE_OPTION
	because InnoDB might actually support the option, but not under
	the current conditions.  The messages revealing the specific
	problems are reported inside this function. */
	if (strict && create_options_are_invalid()) {
		DBUG_RETURN(HA_WRONG_CREATE_OPTION);
	}

	/* Create the table flags and flags2 */
	if (!innobase_table_flags()) {
		DBUG_RETURN(HA_WRONG_CREATE_OPTION);
	}

	if (high_level_read_only) {
		DBUG_RETURN(HA_ERR_TABLE_READONLY);
	}

	if (gcols_in_fulltext_or_spatial()) {
		DBUG_RETURN(HA_ERR_UNSUPPORTED);
	}

	for (uint i = 0; i < m_form->s->keys; i++) {
		const size_t max_field_len
		    = DICT_MAX_FIELD_LEN_BY_FORMAT_FLAG(m_flags);
		const KEY& key = m_form->key_info[i];

		if (key.algorithm == HA_KEY_ALG_FULLTEXT) {
			continue;
		}

		if (too_big_key_part_length(max_field_len, key)) {
			DBUG_RETURN(convert_error_code_to_mysql(
			    DB_TOO_BIG_INDEX_COL, m_flags, NULL));
		}
	}

	DBUG_RETURN(parse_table_name(name));
}

/** Push warning message to SQL-layer based on foreign key constraint index
match error.
@param[in]	trx		Current transaction
@param[in]	operation	Operation ("Create" or "Alter")
@param[in]	create_name	Table name as specified in SQL
@param[in]	columns		Foreign key column names array
@param[in]	index_error 	Index error code
@param[in]	err_col	  	Column where error happened
@param[in]	err_index  	Index where error happened
@param[in]	table	  	Table object */
static void
foreign_push_index_error(trx_t* trx, const char* operation,
			 const char* create_name, const char* fk_text,
			 const char** columns, fkerr_t index_error,
			 ulint err_col, dict_index_t* err_index,
			 dict_table_t* table)
{
	switch (index_error) {
	case FK_SUCCESS:
		break;
	case FK_INDEX_NOT_FOUND:
		ib_foreign_warn(trx, DB_CANNOT_ADD_CONSTRAINT, create_name,
				"%s table %s with foreign key %s constraint"
				" failed. There is no index in the referenced"
				" table where the referenced columns appear"
				" as the first columns.",
				operation, create_name, fk_text);
		return;
	case FK_IS_PREFIX_INDEX:
		ib_foreign_warn(
			trx, DB_CANNOT_ADD_CONSTRAINT, create_name,
			"%s table %s with foreign key %s constraint"
			" failed. There is only prefix index in the referenced"
			" table where the referenced columns appear"
			" as the first columns.",
			operation, create_name, fk_text);
		return;
	case FK_COL_NOT_NULL:
		ib_foreign_warn(
			trx, DB_CANNOT_ADD_CONSTRAINT, create_name,
			"%s table %s with foreign key %s constraint"
			" failed. You have defined a SET NULL condition but "
			"column '%s' on index is defined as NOT NULL.",
			operation, create_name, fk_text, columns[err_col]);
		return;
	case FK_COLS_NOT_EQUAL:
		dict_field_t* field;
		const char*   col_name;
		field = dict_index_get_nth_field(err_index, err_col);

		col_name = field->col->is_virtual()
				   ? "(null)"
				   : dict_table_get_col_name(
					   table, dict_col_get_no(field->col)).str;
		ib_foreign_warn(
			trx, DB_CANNOT_ADD_CONSTRAINT, create_name,
			"%s table %s with foreign key %s constraint"
			" failed. Field type or character set for column '%s' "
			"does not match referenced column '%s'.",
			operation, create_name, fk_text, columns[err_col],
			col_name);
		return;
	}
	DBUG_ASSERT("unknown error" == 0);
}

/** Find column or virtual column in table by its name.
@param[in]	table	Table where column is searched
@param[in]	name	Name to search for
@retval		true	if found
@retval		false	if not found */
static bool
find_col(dict_table_t* table, const char** name)
{
	ulint i;
	const Lex_ident_column outer_name = Lex_cstring_strlen(*name);
	for (i = 0; i < dict_table_get_n_cols(table); i++) {

		const Lex_ident_column inner_name =
		  dict_table_get_col_name(table, i);

		if (outer_name.streq(inner_name)) {
			/* Found */
			strcpy((char*)*name, inner_name.str);
			return true;
		}
	}

	for (i = 0; i < dict_table_get_n_v_cols(table); i++) {

		const Lex_ident_column inner_name =
		  dict_table_get_v_col_name(table, i);

		if (outer_name.streq(inner_name)) {
			/* Found */
			strcpy((char*)*name, inner_name.str);
			return true;
		}
	}
	return false;
}

/** Foreign key printer for error messages. Prints FK name if it exists or
key part list in the form (col1, col2, col3, ...) */
class key_text
{
	static const size_t MAX_TEXT = 48;
	char		    buf[MAX_TEXT + 1];

public:
	key_text(Key* key)
	{
		char* ptr = buf;
		if (key->name.str) {
			size_t len = std::min(key->name.length, MAX_TEXT - 2);
			*(ptr++)   = '`';
			memcpy(ptr, key->name.str, len);
			ptr	  += len;
			*(ptr++)   = '`';
			*ptr	   = '\0';
			return;
		}
		*(ptr++)  = '(';
		List_iterator_fast<Key_part_spec> it(key->columns);
		while (Key_part_spec* k = it++) {
			/* 3 is etc continuation ("...");
			   2 is comma separator (", ") in case of next exists;
			   1 is terminating ')' */
			if (MAX_TEXT - (size_t)(ptr - buf)
				>= (it.peek() ? 3 + 2 + 1 : 3 + 1)
				+ k->field_name.length) {
				memcpy(ptr, k->field_name.str,
				       k->field_name.length);
				ptr += k->field_name.length;
				if (it.peek()) {
					*(ptr++) = ',';
					*(ptr++) = ' ';
				}
			} else {
				ut_ad((size_t)(ptr - buf) <= MAX_TEXT - 4);
				memcpy(ptr, "...", 3);
				ptr += 3;
				break;
			}
		}
		*(ptr++) = ')';
		*ptr 	 = '\0';
	}
	const char* str() { return buf; }
};

char *dict_table_lookup(LEX_CSTRING db, LEX_CSTRING name,
                        dict_table_t **table, mem_heap_t *heap) noexcept
{
  Identifier_chain2 ident(db, name);
  const size_t ref_nbytes= (db.length + name.length) *
	  system_charset_info->casedn_multiply() + 2;
  char *ref= static_cast<char*>(mem_heap_alloc(heap, ref_nbytes));

  size_t len= ident.make_sep_name_opt_casedn(ref, ref_nbytes, '/',
					     lower_case_table_names > 0);
  *table = dict_sys.load_table({ref, len});

  if (lower_case_table_names == 2)
    ident.make_sep_name_opt_casedn(ref, ref_nbytes, '/', false);

  return ref;
}

/** Convert a schema or table name to InnoDB (and file system) format.
@param cs   source character set
@param name name encoded in cs
@param buf  output buffer (MAX_TABLE_NAME_LEN + 1 bytes)
@return the converted string (within buf) */
LEX_CSTRING innodb_convert_name(CHARSET_INFO *cs, LEX_CSTRING name, char *buf)
  noexcept
{
  CHARSET_INFO *to_cs= &my_charset_filename;
  if (!strncmp(name.str, srv_mysql50_table_name_prefix,
               sizeof srv_mysql50_table_name_prefix - 1))
  {
    /* Before MySQL 5.1 introduced my_charset_filename, schema and
    table names were stored in the file system as specified by the
    user, hopefully in ASCII encoding, but it could also be in ISO
    8859-1 or UTF-8. Such schema or table names are distinguished by
    the #mysql50# prefix.

    Let us discard that prefix and convert the name to UTF-8
    (system_charset_info). */
    name.str+= sizeof srv_mysql50_table_name_prefix - 1;
    name.length-= sizeof srv_mysql50_table_name_prefix - 1;
    to_cs= system_charset_info;
  }
  uint errors;
  return LEX_CSTRING{buf, strconvert(cs, name.str, name.length, to_cs,
                                     buf, MAX_TABLE_NAME_LEN, &errors)};
}

/** Find an auto-generated foreign key constraint identifier.
@param table   InnoDB table
@return the next number to assign to a constraint */
ulint dict_table_get_foreign_id(const dict_table_t &table) noexcept
{
  ulint id= 0;

  for (const dict_foreign_t *foreign : table.foreign_set)
  {
    const char *s= foreign->sql_id();
    char *endp;
    ulint f= strtoul(s, &endp, 10);
    if (!*endp && f > id)
      id= f;
  }

  return id + 1;
}

/** Generate a foreign key constraint name for an anonymous constraint.
@param id_nr    sequence to allocate identifiers from
@param name     table name
@param foreign  foreign key */
void dict_create_add_foreign_id(ulint *id_nr, const char *name,
                                dict_foreign_t *foreign) noexcept
{
  if (!foreign->id)
  {
    size_t len= snprintf(nullptr, 0, "%s\377%zu", name, *id_nr);
    foreign->id= static_cast<char*>(mem_heap_alloc(foreign->heap, len + 1));
    snprintf(foreign->id, len + 1, "%s\377%zu", name, (*id_nr)++);
  }
}

/** Create InnoDB foreign keys from MySQL alter_info. Collect all
dict_foreign_t items into local_fk_set and then add into system table.
@return		DB_SUCCESS or specific error code */
dberr_t
create_table_info_t::create_foreign_keys()
{
	dict_foreign_set      local_fk_set;
	dict_foreign_set_free local_fk_set_free(local_fk_set);
	dberr_t		      error;
	ulint		      number	      = 1;
	static const unsigned MAX_COLS_PER_FK = 500;
	const char*	      column_names[MAX_COLS_PER_FK];
	const char*	      ref_column_names[MAX_COLS_PER_FK];
	char		      create_name[MAX_DATABASE_NAME_LEN + 1 +
					  MAX_TABLE_NAME_LEN + 1];
	char db_name[MAX_DATABASE_NAME_LEN + 1];
	char t_name[MAX_TABLE_NAME_LEN + 1];
	static_assert(MAX_TABLE_NAME_LEN == MAX_DATABASE_NAME_LEN, "");
	dict_index_t*	      index	  = NULL;
	fkerr_t		      index_error = FK_SUCCESS;
	dict_index_t*	      err_index	  = NULL;
	ulint		      err_col	= 0;
	const bool	      tmp_table = m_flags2 & DICT_TF2_TEMPORARY;
	const CHARSET_INFO*   cs	= thd_charset(m_thd);
	const char*	      operation = "Create ";

	enum_sql_command sqlcom = enum_sql_command(thd_sql_command(m_thd));
	LEX_CSTRING name= {m_table_name, strlen(m_table_name)};

	if (sqlcom == SQLCOM_ALTER_TABLE) {
		mem_heap_t*   heap = mem_heap_create(10000);
		LEX_CSTRING t = innodb_convert_name(cs, m_form->s->table_name,
						  t_name);
		LEX_CSTRING d = innodb_convert_name(cs, m_form->s->db, db_name);
		dict_table_t* alter_table;
		char* n = dict_table_lookup(d, t, &alter_table, heap);

		/* If we are altering a temporary table, the table name after
		ALTER TABLE does not correspond to the internal table name, and
		alter_table=nullptr. But, we do not support FOREIGN KEY
		constraints for temporary tables. */

		if (alter_table) {
			n = alter_table->name.m_name;
			number = dict_table_get_foreign_id(*alter_table);
		}

		char* bufend = innobase_convert_name(
			create_name, sizeof create_name, n, strlen(n), m_thd);
		*bufend = '\0';
		mem_heap_free(heap);
		operation = "Alter ";
	} else if (strstr(m_table_name, "#P#")
		   || strstr(m_table_name, "#p#")) {
		/* Partitioned table */
		create_name[0] = '\0';
	} else {
		char* bufend = innobase_convert_name(create_name,
						     sizeof create_name,
						     LEX_STRING_WITH_LEN(name),
						     m_thd);
		*bufend = '\0';
	}

	Alter_info* alter_info = m_create_info->alter_info;
	ut_ad(alter_info);
	List_iterator_fast<Key> key_it(alter_info->key_list);

	dict_table_t* table = dict_sys.find_table({name.str, name.length});
	if (!table) {
		ib_foreign_warn(m_trx, DB_CANNOT_ADD_CONSTRAINT, create_name,
				"%s table %s foreign key constraint"
				" failed. Table not found.",
				operation, create_name);

		return (DB_CANNOT_ADD_CONSTRAINT);
	}

	while (Key* key = key_it++) {
		if (key->type != Key::FOREIGN_KEY || key->old)
			continue;

		if (tmp_table) {
			ib_foreign_warn(m_trx, DB_CANNOT_ADD_CONSTRAINT,
					create_name,
					"%s table `%s`.`%s` with foreign key "
					"constraint failed. "
					"Temporary tables can't have "
					"foreign key constraints.",
					operation, m_form->s->db.str,
					m_form->s->table_name.str);

			return (DB_CANNOT_ADD_CONSTRAINT);
		} else if (!*create_name) {
			ut_ad("should be unreachable" == 0);
			return DB_CANNOT_ADD_CONSTRAINT;
		}

		Foreign_key*   fk = static_cast<Foreign_key*>(key);
		Key_part_spec* col;
		bool	       success;

		dict_foreign_t* foreign = dict_mem_foreign_create();
		if (!foreign) {
			return (DB_OUT_OF_MEMORY);
		}

		List_iterator_fast<Key_part_spec> col_it(fk->columns);
		unsigned			  i = 0, j = 0;
		while ((col = col_it++)) {
			column_names[i] = mem_heap_strdupl(
				foreign->heap, col->field_name.str,
				col->field_name.length);
			success = find_col(table, column_names + i);
			if (!success) {
				ib_foreign_warn(
					m_trx, DB_CANNOT_ADD_CONSTRAINT,
					create_name,
					"%s table %s foreign key %s constraint"
					" failed. Column %s was not found.",
					operation, create_name,
					key_text(fk).str(),
					column_names[i]);
				dict_foreign_free(foreign);
				return (DB_CANNOT_ADD_CONSTRAINT);
			}
			++i;
			if (i >= MAX_COLS_PER_FK) {
				ib_foreign_warn(
					m_trx, DB_CANNOT_ADD_CONSTRAINT,
					create_name,
					"%s table %s foreign key %s constraint"
					" failed. Too many columns: %u (%u "
					"allowed).",
					operation, create_name,
					key_text(fk).str(), i,
					MAX_COLS_PER_FK);
				dict_foreign_free(foreign);
				return (DB_CANNOT_ADD_CONSTRAINT);
			}
		}

		index = dict_foreign_find_index(
			table, NULL, column_names, i, NULL, TRUE, FALSE,
			&index_error, &err_col, &err_index);

		if (!index) {
			foreign_push_index_error(m_trx, operation, create_name,
						 key_text(fk).str(),
						 column_names,
						 index_error, err_col,
						 err_index, table);
			dict_foreign_free(foreign);
			return (DB_CANNOT_ADD_CONSTRAINT);
		}

		if (size_t fk_len = fk->constraint_name.length) {
			/* Prepend the table name to the constraint name. */
			size_t s = strlen(table->name.m_name) + 2 + fk_len;
			foreign->id = static_cast<char*>(
				mem_heap_alloc(foreign->heap, s));
			snprintf(foreign->id, s, "%s\377%.*s",
				 table->name.m_name, int(fk_len),
				 fk->constraint_name.str);
		} else {
			dict_create_add_foreign_id(&number, table->name.m_name,
                                                   foreign);
		}

		std::pair<dict_foreign_set::iterator, bool> ret
			= local_fk_set.insert(foreign);

		if (!ret.second) {
			/* A duplicate foreign key name has been found */
			dict_foreign_free(foreign);
			return (DB_CANNOT_ADD_CONSTRAINT);
		}

		foreign->foreign_table = table;
		foreign->foreign_table_name
			= mem_heap_strdup(foreign->heap, table->name.m_name);
		if (!foreign->foreign_table_name) {
			return (DB_OUT_OF_MEMORY);
		}

		foreign->foreign_table_name_lookup_set();

		foreign->foreign_index = index;
		foreign->n_fields      = i & dict_index_t::MAX_N_FIELDS;

		foreign->foreign_col_names = static_cast<const char**>(
			mem_heap_alloc(foreign->heap, i * sizeof(void*)));
		if (!foreign->foreign_col_names) {
			return (DB_OUT_OF_MEMORY);
		}

		memcpy(foreign->foreign_col_names, column_names,
		       i * sizeof(void*));

		LEX_CSTRING t = innodb_convert_name(cs, fk->ref_table, t_name);
		LEX_CSTRING d = fk->ref_db.str
			? innodb_convert_name(cs, fk->ref_db, db_name)
			: LEX_CSTRING{table->name.m_name, table->name.dblen()};
		foreign->referenced_table_name = dict_table_lookup(
			d, t, &foreign->referenced_table, foreign->heap);

		if (!foreign->referenced_table && m_trx->check_foreigns) {
			char  buf[MAX_TABLE_NAME_LEN + 1] = "";
			char* bufend;

			bufend = innobase_convert_name(
				buf, MAX_TABLE_NAME_LEN,
				foreign->referenced_table_name,
				strlen(foreign->referenced_table_name), m_thd);
			*bufend = '\0';
			ib_foreign_warn(m_trx, DB_CANNOT_ADD_CONSTRAINT,
					create_name,
					"%s table %s with foreign key %s "
					"constraint failed. Referenced table "
					"%s not found in the data dictionary.",
					operation, create_name,
					key_text(fk).str(), buf);
			return DB_CANNOT_ADD_CONSTRAINT;
		}

		/* Don't allow foreign keys on partitioned tables yet. */
		if (foreign->referenced_table
		    && dict_table_is_partition(foreign->referenced_table)) {
			/* How could one make a referenced table to be a
			 * partition? */
			ut_ad(0);
			my_error(ER_FEATURE_NOT_SUPPORTED_WITH_PARTITIONING,
				 MYF(0), "FOREIGN KEY");
			return (DB_CANNOT_ADD_CONSTRAINT);
		}

		col_it.init(fk->ref_columns);
		while ((col = col_it++)) {
			ref_column_names[j] = mem_heap_strdupl(
				foreign->heap, col->field_name.str,
				col->field_name.length);
			if (foreign->referenced_table) {
				success = find_col(foreign->referenced_table,
						   ref_column_names + j);
				if (!success) {
					ib_foreign_warn(
						m_trx,
						DB_CANNOT_ADD_CONSTRAINT,
						create_name,
						"%s table %s foreign key %s "
						"constraint failed. "
						"Column %s was not found.",
						operation, create_name,
						key_text(fk).str(),
						ref_column_names[j]);
					return DB_CANNOT_ADD_CONSTRAINT;
				}
			}
			++j;
		}
		/* See ER_WRONG_FK_DEF in mysql_prepare_create_table() */
		ut_ad(i == j);

		/* Try to find an index which contains the columns as the first
		fields and in the right order, and the types are the same as in
		foreign->foreign_index */

		if (foreign->referenced_table) {
			index = dict_foreign_find_index(
				foreign->referenced_table, NULL,
				ref_column_names, i, foreign->foreign_index,
				TRUE, FALSE, &index_error, &err_col,
				&err_index);

			if (!index) {
				foreign_push_index_error(
					m_trx, operation, create_name,
					key_text(fk).str(),
					column_names, index_error, err_col,
					err_index, foreign->referenced_table);
				return DB_CANNOT_ADD_CONSTRAINT;
			}
		} else {
			ut_a(!m_trx->check_foreigns);
			index = NULL;
		}

		foreign->referenced_index = index;
		foreign->referenced_table_name_lookup_set();

		foreign->referenced_col_names = static_cast<const char**>(
			mem_heap_alloc(foreign->heap, i * sizeof(void*)));
		if (!foreign->referenced_col_names) {
			return (DB_OUT_OF_MEMORY);
		}

		memcpy(foreign->referenced_col_names, ref_column_names,
		       i * sizeof(void*));

		if (fk->delete_opt == FK_OPTION_SET_NULL
		    || fk->update_opt == FK_OPTION_SET_NULL) {
			for (j = 0; j < foreign->n_fields; j++) {
				if ((dict_index_get_nth_col(
					     foreign->foreign_index, j)
					     ->prtype)
				    & DATA_NOT_NULL) {
					const dict_col_t* col
						= dict_index_get_nth_col(
							foreign->foreign_index,
							j);
					const char* col_name
						= dict_table_get_col_name(
							foreign->foreign_index
								->table,
							dict_col_get_no(col)).str;

					/* It is not sensible to define SET
					NULL
					if the column is not allowed to be
					NULL! */
					ib_foreign_warn(
						m_trx,
						DB_CANNOT_ADD_CONSTRAINT,
						create_name,
						"%s table %s with foreign key "
						"%s constraint failed. You have"
						" defined a SET NULL condition "
						"but column '%s' is defined as "
						"NOT NULL.",
						operation, create_name,
						key_text(fk).str(), col_name);

					return DB_CANNOT_ADD_CONSTRAINT;
				}
			}
		}
#if defined __GNUC__ && !defined __clang__ && __GNUC__ < 6
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wconversion"
#endif
		switch (fk->delete_opt) {
		case FK_OPTION_UNDEF:
		case FK_OPTION_RESTRICT:
			break;
		case FK_OPTION_CASCADE:
			foreign->type |= foreign->DELETE_CASCADE;
			break;
		case FK_OPTION_SET_NULL:
			foreign->type |= foreign->DELETE_SET_NULL;
			break;
		case FK_OPTION_NO_ACTION:
			foreign->type |= foreign->DELETE_NO_ACTION;
			break;
		case FK_OPTION_SET_DEFAULT:
			// TODO: MDEV-10393 Foreign keys SET DEFAULT action
			break;
		default:
			ut_ad(0);
			break;
		}

		switch (fk->update_opt) {
		case FK_OPTION_UNDEF:
		case FK_OPTION_RESTRICT:
			break;
		case FK_OPTION_CASCADE:
			foreign->type |= foreign->UPDATE_CASCADE;
			break;
		case FK_OPTION_SET_NULL:
			foreign->type |= foreign->UPDATE_SET_NULL;
			break;
		case FK_OPTION_NO_ACTION:
			foreign->type |= foreign->UPDATE_NO_ACTION;
			break;
		case FK_OPTION_SET_DEFAULT:
			// TODO: MDEV-10393 Foreign keys SET DEFAULT action
			break;
		default:
			ut_ad(0);
			break;
		}
#if defined __GNUC__ && !defined __clang__ && __GNUC__ < 6
# pragma GCC diagnostic pop
#endif
	}

	if (dict_foreigns_has_s_base_col(local_fk_set, table)) {
		return (DB_NO_FK_ON_S_BASE_COL);
	}

	/**********************************************************/
	/* The following call adds the foreign key constraints
	to the data dictionary system tables on disk */
	m_trx->op_info = "adding foreign keys";

	trx_start_if_not_started_xa(m_trx, true);

	m_trx->dict_operation = true;

	error = dict_create_add_foreigns_to_dictionary(local_fk_set, table,
						       m_trx);

	if (error == DB_SUCCESS) {

		table->foreign_set.insert(local_fk_set.begin(),
					  local_fk_set.end());
		std::for_each(local_fk_set.begin(), local_fk_set.end(),
			      dict_foreign_add_to_referenced_table());
		local_fk_set.clear();

		dict_mem_table_fill_foreign_vcol_set(table);
	}
	return (error);
}

/** Create the internal innodb table.
@param create_fk	whether to add FOREIGN KEY constraints
@param strict		whether to give warnings for too big rows */
int create_table_info_t::create_table(bool create_fk, bool strict)
{
	int		error;
	int		primary_key_no;
	uint		i;

	DBUG_ENTER("create_table");

	/* Look for a primary key */
	primary_key_no = (m_form->s->primary_key != MAX_KEY ?
			  (int) m_form->s->primary_key : -1);

	/* Our function innobase_get_mysql_key_number_for_index assumes
	the primary key is always number 0, if it exists */
	ut_a(primary_key_no == -1 || primary_key_no == 0);

	error = create_table_def();

	if (error) {
		DBUG_RETURN(error);
	}

	/* Create the keys */

	if (m_form->s->keys == 0 || primary_key_no == -1) {
		/* Create an index which is used as the clustered index;
		order the rows by their row id which is internally generated
		by InnoDB */
		ulint flags = m_table->flags;
		dict_index_t* index = dict_mem_index_create(
			m_table, GEN_CLUST_INDEX.str,
			DICT_CLUSTERED, 0);
		const ha_table_option_struct& o = *m_form->s->option_struct;
		error = convert_error_code_to_mysql(
			row_create_index_for_mysql(
				index, m_trx, NULL,
				fil_encryption_t(o.encryption),
				uint32_t(o.encryption_key_id)),
			flags, m_thd);
		if (error) {
			DBUG_RETURN(error);
		}
	}

	if (primary_key_no != -1) {
		/* In InnoDB the clustered index must always be created
		first */
		if ((error = create_index(m_trx, m_form, m_table,
					  (uint) primary_key_no))) {
			DBUG_RETURN(error);
		}
	}

	/* Create the ancillary tables that are common to all FTS indexes on
	this table. */
	if (m_flags2 & DICT_TF2_FTS) {
		fts_doc_id_index_enum	ret;

		/* Check whether there already exists FTS_DOC_ID_INDEX */
		ret = innobase_fts_check_doc_id_index_in_def(
			m_form->s->keys, m_form->key_info);

		switch (ret) {
		case FTS_INCORRECT_DOC_ID_INDEX:
			push_warning_printf(m_thd,
					    Sql_condition::WARN_LEVEL_WARN,
					    ER_WRONG_NAME_FOR_INDEX,
					    " InnoDB: Index name %s is reserved"
					    " for the unique index on"
					    " FTS_DOC_ID column for FTS"
					    " Document ID indexing"
					    " on table %s. Please check"
					    " the index definition to"
					    " make sure it is of correct"
					    " type",
					    FTS_DOC_ID_INDEX.str,
					    m_table->name.m_name);

			if (m_table->fts) {
				m_table->fts->~fts_t();
				m_table->fts = nullptr;
			}

			my_error(ER_WRONG_NAME_FOR_INDEX, MYF(0),
				 FTS_DOC_ID_INDEX.str);
			DBUG_RETURN(-1);
		case FTS_EXIST_DOC_ID_INDEX:
		case FTS_NOT_EXIST_DOC_ID_INDEX:
			break;
		}

		dberr_t	err = fts_create_common_tables(
			m_trx, m_table,
			(ret == FTS_EXIST_DOC_ID_INDEX));

		error = convert_error_code_to_mysql(err, 0, NULL);

		if (error) {
			DBUG_RETURN(error);
		}
	}

	for (i = 0; i < m_form->s->keys; i++) {
		if (i != uint(primary_key_no)
		    && (error = create_index(m_trx, m_form, m_table, i))) {
			DBUG_RETURN(error);
		}
	}

	/* Cache all the FTS indexes on this table in the FTS specific
	structure. They are used for FTS indexed column update handling. */
	if (m_flags2 & DICT_TF2_FTS) {
		fts_t*          fts = m_table->fts;

		ut_a(fts != NULL);

		dict_table_get_all_fts_indexes(m_table, fts->indexes);
	}

	create_fk&= !m_creating_stub;
	dberr_t err = create_fk ? create_foreign_keys() : DB_SUCCESS;

	if (err == DB_SUCCESS) {
		const dict_err_ignore_t ignore_err = m_trx->check_foreigns
			? DICT_ERR_IGNORE_NONE : DICT_ERR_IGNORE_FK_NOKEY;

		/* Check that also referencing constraints are ok */
		dict_names_t	fk_tables;
		err = dict_load_foreigns(m_table_name, nullptr,
					 m_trx->id, true,
					 ignore_err, fk_tables);
		while (err == DB_SUCCESS && !fk_tables.empty()) {
			dict_sys.load_table(
				{fk_tables.front(), strlen(fk_tables.front())},
				ignore_err);
			fk_tables.pop_front();
		}
	}

	switch (err) {
	case DB_PARENT_NO_INDEX:
		push_warning_printf(
			m_thd, Sql_condition::WARN_LEVEL_WARN,
			HA_ERR_CANNOT_ADD_FOREIGN,
			"Create table '%s' with foreign key constraint"
			" failed. There is no index in the referenced"
			" table where the referenced columns appear"
			" as the first columns.", m_table_name);
		break;

	case DB_CHILD_NO_INDEX:
		push_warning_printf(
			m_thd, Sql_condition::WARN_LEVEL_WARN,
			HA_ERR_CANNOT_ADD_FOREIGN,
			"Create table '%s' with foreign key constraint"
			" failed. There is no index in the referencing"
			" table where referencing columns appear"
			" as the first columns.", m_table_name);
		break;
	case DB_NO_FK_ON_S_BASE_COL:
		push_warning_printf(
			m_thd, Sql_condition::WARN_LEVEL_WARN,
			HA_ERR_CANNOT_ADD_FOREIGN,
			"Create table '%s' with foreign key constraint"
			" failed. Cannot add foreign key constraint"
			" placed on the base column of stored"
			" column. ",
			m_table_name);
	default:
		break;
	}

	if (err != DB_SUCCESS) {
		DBUG_RETURN(convert_error_code_to_mysql(
					err, m_flags, NULL));
	}

	/* In TRUNCATE TABLE, we will merely warn about the maximum
	row size being too large. */
	if (!row_size_is_acceptable(*m_table, create_fk && strict)) {
		DBUG_RETURN(convert_error_code_to_mysql(
			    DB_TOO_BIG_RECORD, m_flags, NULL));
	}

	DBUG_RETURN(0);
}

bool create_table_info_t::row_size_is_acceptable(
  const dict_table_t &table, bool strict) const
{
  for (dict_index_t *index= dict_table_get_first_index(&table); index;
       index= dict_table_get_next_index(index))
    if (!row_size_is_acceptable(*index, strict))
      return false;
  return true;
}

dict_index_t::record_size_info_t dict_index_t::record_size_info() const
{
  ut_ad(!(type & DICT_FTS));

  /* maximum allowed size of a node pointer record */
  ulint page_ptr_max;
  const bool comp= table->not_redundant();
  /* table->space == NULL after DISCARD TABLESPACE */
  const ulint zip_size= dict_tf_get_zip_size(table->flags);
  record_size_info_t result;

  if (zip_size && zip_size < srv_page_size)
  {
    /* On a ROW_FORMAT=COMPRESSED page, two records must fit in the
    uncompressed page modification log. On compressed pages
    with size.physical() == univ_page_size.physical(),
    this limit will never be reached. */
    ut_ad(comp);
    /* The maximum allowed record size is the size of
    an empty page, minus a byte for recoding the heap
    number in the page modification log.  The maximum
    allowed node pointer size is half that. */
    result.max_leaf_size= page_zip_empty_size(n_fields, zip_size);
    if (result.max_leaf_size)
    {
      result.max_leaf_size--;
    }
    page_ptr_max= result.max_leaf_size / 2;
    /* On a compressed page, there is a two-byte entry in
    the dense page directory for every record.  But there
    is no record header. */
    result.shortest_size= 2;
  }
  else
  {
    /* The maximum allowed record size is half a B-tree
    page(16k for 64k page size).  No additional sparse
    page directory entry will be generated for the first
    few user records. */
    result.max_leaf_size= (comp || srv_page_size < UNIV_PAGE_SIZE_MAX)
                              ? page_get_free_space_of_empty(comp) / 2
                              : REDUNDANT_REC_MAX_DATA_SIZE;

    page_ptr_max= result.max_leaf_size;
    /* Each record has a header. */
    result.shortest_size= comp ? REC_N_NEW_EXTRA_BYTES : REC_N_OLD_EXTRA_BYTES;
  }

  if (comp)
  {
    /* Include the "null" flags in the
    maximum possible record size. */
    result.shortest_size+= UT_BITS_IN_BYTES(n_nullable);
  }
  else
  {
    /* For each column, include a 2-byte offset and a
    "null" flag.  The 1-byte format is only used in short
    records that do not contain externally stored columns.
    Such records could never exceed the page limit, even
    when using the 2-byte format. */
    result.shortest_size+= 2 * n_fields;
  }

  const ulint max_local_len= table->get_overflow_field_local_len();

  /* Compute the maximum possible record size. */
  for (unsigned i= 0; i < n_fields; i++)
  {
    const dict_field_t &f= fields[i];
    const dict_col_t &col= *f.col;

    /* In dtuple_convert_big_rec(), variable-length columns
    that are longer than BTR_EXTERN_LOCAL_STORED_MAX_SIZE
    may be chosen for external storage.

    Fixed-length columns, and all columns of secondary
    index records are always stored inline. */

    /* Determine the maximum length of the index field.
    The field_ext_max_size should be computed as the worst
    case in rec_get_converted_size_comp() for
    REC_STATUS_ORDINARY records. */

    size_t field_max_size= dict_col_get_fixed_size(&col, comp);
    if (field_max_size && f.fixed_len != 0)
    {
      /* dict_index_add_col() should guarantee this */
      ut_ad(!f.prefix_len || f.fixed_len == f.prefix_len);
      if (f.prefix_len)
        field_max_size= f.prefix_len;
      /* Fixed lengths are not encoded
      in ROW_FORMAT=COMPACT. */
      goto add_field_size;
    }

    field_max_size= dict_col_get_max_size(&col);

    if (f.prefix_len)
    {
      if (f.prefix_len < field_max_size)
      {
        field_max_size= f.prefix_len;
      }

      /* those conditions were copied from dtuple_convert_big_rec()*/
    }
    else if (field_max_size > max_local_len &&
             field_max_size > BTR_EXTERN_LOCAL_STORED_MAX_SIZE &&
             DATA_BIG_COL(&col) && dict_index_is_clust(this))
    {

      /* In the worst case, we have a locally stored
      column of BTR_EXTERN_LOCAL_STORED_MAX_SIZE bytes.
      The length can be stored in one byte.  If the
      column were stored externally, the lengths in
      the clustered index page would be
      BTR_EXTERN_FIELD_REF_SIZE and 2. */
      field_max_size= max_local_len;
    }

    if (comp)
    {
      /* Add the extra size for ROW_FORMAT=COMPACT.
      For ROW_FORMAT=REDUNDANT, these bytes were
      added to result.shortest_size before this loop. */
      result.shortest_size+= field_max_size < 256 ? 1 : 2;
    }
  add_field_size:
    result.shortest_size+= field_max_size;

    /* Check the size limit on leaf pages. */
    if (result.shortest_size >= result.max_leaf_size)
    {
      result.set_too_big(i);
    }

    /* Check the size limit on non-leaf pages.  Records
    stored in non-leaf B-tree pages consist of the unique
    columns of the record (the key columns of the B-tree)
    and a node pointer field.  When we have processed the
    unique columns, result.shortest_size equals the size of the
    node pointer record minus the node pointer column. */
    if (i + 1 == dict_index_get_n_unique_in_tree(this) &&
        result.shortest_size + REC_NODE_PTR_SIZE + (comp ? 0 : 2) >=
        page_ptr_max)
    {
      result.set_too_big(i);
    }
  }

  return result;
}

/** Issue a warning that the row is too big. */
static void ib_warn_row_too_big(THD *thd, const dict_table_t *table)
{
  /* FIXME: this row size check should be improved */
  /* If prefix is true then a 768-byte prefix is stored
  locally for BLOB fields. Refer to dict_table_get_format() */
  const bool prefix= !dict_table_has_atomic_blobs(table);

  const ulint free_space=
      page_get_free_space_of_empty(table->flags & DICT_TF_COMPACT) / 2;

  push_warning_printf(
      thd, Sql_condition::WARN_LEVEL_WARN, HA_ERR_TO_BIG_ROW,
      "Row size too large (> " ULINTPF "). Changing some columns to TEXT"
      " or BLOB %smay help. In current row format, BLOB prefix of"
      " %d bytes is stored inline.",
      free_space,
      prefix ? "or using ROW_FORMAT=DYNAMIC or ROW_FORMAT=COMPRESSED " : "",
      prefix ? DICT_MAX_FIXED_COL_LEN : 0);
}

bool create_table_info_t::row_size_is_acceptable(
    const dict_index_t &index, bool strict) const
{
  if ((index.type & DICT_FTS) || index.table->is_system_db)
  {
    /* Ignore system tables check because innodb_table_stats
    maximum row size can not fit on 4k page. */
    return true;
  }

  const bool innodb_strict_mode= THDVAR(m_thd, strict_mode);
  dict_index_t::record_size_info_t info= index.record_size_info();

  if (info.row_is_too_big())
  {
    ut_ad(info.get_overrun_size() != 0);

    const size_t idx= info.get_first_overrun_field_index();
    const dict_field_t *field= dict_index_get_nth_field(&index, idx);

    ut_ad((!field->name) == field->col->is_dropped());
    if (innodb_strict_mode || global_system_variables.log_warnings > 2)
    {
      ib::error_or_warn eow(strict && innodb_strict_mode);
      if (field->name)
        eow << "Cannot add field " << field->name << " in table ";
      else
        eow << "Cannot add an instantly dropped column in table ";
      eow << "`" << m_form->s->db.str << "`.`" << m_form->s->table_name.str
	  << "`" " because after adding it, the row size is "
          << info.get_overrun_size()
          << " which is greater than maximum allowed size ("
          << info.max_leaf_size << " bytes) for a record on index leaf page.";
    }

    if (strict && innodb_strict_mode)
      return false;

    ib_warn_row_too_big(m_thd, index.table);
  }

  return true;
}

void create_table_info_t::create_table_update_dict(dict_table_t *table,
                                                   THD *thd,
                                                   const HA_CREATE_INFO &info,
                                                   const TABLE &t)
{
  ut_ad(dict_sys.locked());

  DBUG_ASSERT(table->get_ref_count());
  if (table->fts)
  {
    if (!table->fts_doc_id_index)
      table->fts_doc_id_index=
        dict_table_get_index_on_name(table, FTS_DOC_ID_INDEX.str);
    else
      DBUG_ASSERT(table->fts_doc_id_index ==
                  dict_table_get_index_on_name(table, FTS_DOC_ID_INDEX.str));
  }

  DBUG_ASSERT(!table->fts == !table->fts_doc_id_index);

  innobase_copy_frm_flags_from_create_info(table, &info);

  /* Load server stopword into FTS cache */
  if (table->flags2 & DICT_TF2_FTS &&
      innobase_fts_load_stopword(table, nullptr, thd))
    fts_optimize_add_table(table);

  if (const Field *ai = t.found_next_number_field)
  {
    ut_ad(ai->stored_in_db());
    ib_uint64_t autoinc= info.auto_increment_value;
    if (autoinc == 0)
      autoinc= 1;

    table->autoinc_mutex.wr_lock();
    dict_table_autoinc_initialize(table, autoinc);

    if (!table->is_temporary())
    {
      const unsigned col_no= innodb_col_no(ai);
      table->persistent_autoinc= static_cast<uint16_t>
        (dict_table_get_nth_col_pos(table, col_no, nullptr) + 1) &
        dict_index_t::MAX_N_FIELDS;
      /* Persist the "last used" value, which typically is AUTO_INCREMENT - 1.
      In btr_create(), the value 0 was already written. */
      if (--autoinc)
        btr_write_autoinc(dict_table_get_first_index(table), autoinc);
    }

    table->autoinc_mutex.wr_unlock();
  }

  innobase_parse_hint_from_comment(thd, table, t.s);
}

/** Allocate a new trx. */
void
create_table_info_t::allocate_trx()
{
	m_trx = innobase_trx_allocate(m_thd);
	m_trx->will_lock = true;
}

/** Create a new table to an InnoDB database.
@param[in]	name		Table name, format: "db/table_name".
@param[in]	form		Table format; columns and index information.
@param[in]	create_info	Create info (including create statement string).
@param[in]	file_per_table	whether to create .ibd file
@param[in,out]	trx		dictionary transaction, or NULL to create new
@return error code
@retval	0 on success */
int
ha_innobase::create(const char *name, TABLE *form, HA_CREATE_INFO *create_info,
                    bool file_per_table, trx_t *trx= nullptr)
{
  DBUG_ENTER("ha_innobase::create");
  DBUG_ASSERT(form->s == table_share);
  DBUG_ASSERT(table_share->table_type == TABLE_TYPE_SEQUENCE ||
              table_share->table_type == TABLE_TYPE_NORMAL);

  create_table_info_t info(ha_thd(), form, create_info, file_per_table, trx);

  int error= info.initialize();
  if (!error)
    error= info.prepare_create_table(name, !trx);
  if (error)
    DBUG_RETURN(error);

  const bool own_trx= !trx;
  if (own_trx)
  {
    info.allocate_trx();
    trx= info.trx();
    DBUG_ASSERT(trx_state_eq(trx, TRX_STATE_NOT_STARTED));

    if (!(info.flags2() & DICT_TF2_TEMPORARY))
    {
      trx_start_for_ddl(trx);
      if (dberr_t err= lock_sys_tables(trx))
        error= convert_error_code_to_mysql(err, 0, nullptr);
    }
    row_mysql_lock_data_dictionary(trx);
  }

  if (!error)
    /* We can't possibly have foreign key information when creating a
    stub table for importing .frm / .cfg / .ibd because it is not
    stored in any of these files. */
    error= info.create_table(own_trx, !create_info->recreate_identical_table);

  if (own_trx || (info.flags2() & DICT_TF2_TEMPORARY))
  {
    if (error)
      trx_rollback_for_mysql(trx);
    else
    {
      std::vector<pfs_os_file_t> deleted;
      trx->commit(deleted);
      ut_ad(deleted.empty());
      info.table()->acquire();
      info.create_table_update_dict(info.table(), info.thd(),
                                    *create_info, *form);
    }

    if (own_trx)
    {
      row_mysql_unlock_data_dictionary(trx);

      if (!error)
      {
        /* Skip stats update when creating a stub table for importing,
        as it is not needed and would report error due to the table
        not being readable yet. */
        if (!info.creating_stub())
          dict_stats_empty_table_and_save(info.table());
        if (!info.table()->is_temporary())
          log_write_up_to(trx->commit_lsn, true);
        info.table()->release();
      }
      trx->free();
    }
  }
  else if (!error && m_prebuilt)
    m_prebuilt->table= info.table();

  if (form->s->primary_key >= MAX_KEY)
    ref_length = DATA_ROW_ID_LEN;
  else
    ref_length = form->key_info[form->s->primary_key].key_length;

  DBUG_RETURN(error);
}

/** Create a new table to an InnoDB database.
@param[in]	name		Table name, format: "db/table_name".
@param[in]	form		Table format; columns and index information.
@param[in]	create_info	Create info (including create statement string).
@return	0 if success else error number. */
int ha_innobase::create(const char *name, TABLE *form,
                        HA_CREATE_INFO *create_info)
{
  return create(name, form, create_info, srv_file_per_table);
}

/*****************************************************************//**
Discards or imports an InnoDB tablespace.
@return 0 == success, -1 == error */

int
ha_innobase::discard_or_import_tablespace(
/*======================================*/
	my_bool		discard)	/*!< in: TRUE if discard, else import */
{

	DBUG_ENTER("ha_innobase::discard_or_import_tablespace");

	if (int err = is_valid_trx()) {
		DBUG_RETURN(err);
	}

	if (m_prebuilt->table->is_temporary()) {
		ib_senderrf(
			m_prebuilt->trx->mysql_thd, IB_LOG_LEVEL_ERROR,
			ER_CANNOT_DISCARD_TEMPORARY_TABLE);

		DBUG_RETURN(HA_ERR_TABLE_NEEDS_UPGRADE);
	}

	ut_ad(m_prebuilt->table->stat_initialized());

	if (m_prebuilt->table->space == fil_system.sys_space) {
		ib_senderrf(
			m_prebuilt->trx->mysql_thd, IB_LOG_LEVEL_ERROR,
			ER_TABLE_IN_SYSTEM_TABLESPACE,
			m_prebuilt->table->name.m_name);

		DBUG_RETURN(HA_ERR_TABLE_NEEDS_UPGRADE);
	}

	trx_start_if_not_started(m_prebuilt->trx, true);
	m_prebuilt->trx->dict_operation = true;

	/* Obtain an exclusive lock on the table. */
	dberr_t	err = lock_table_for_trx(m_prebuilt->table,
					 m_prebuilt->trx, LOCK_X);
	if (err == DB_SUCCESS) {
		err = lock_sys_tables(m_prebuilt->trx);
	}

	if (err != DB_SUCCESS) {
		/* unable to lock the table: do nothing */
		m_prebuilt->trx->commit();
	} else if (discard) {

		/* Discarding an already discarded tablespace should be an
		idempotent operation. Also, if the .ibd file is missing the
		user may want to set the DISCARD flag in order to IMPORT
		a new tablespace. */

		if (!m_prebuilt->table->is_readable()) {
			ib_senderrf(
				m_prebuilt->trx->mysql_thd,
				IB_LOG_LEVEL_WARN, ER_TABLESPACE_MISSING,
				m_prebuilt->table->name.m_name);
		}

		err = row_discard_tablespace_for_mysql(
			m_prebuilt->table, m_prebuilt->trx);
	} else if (m_prebuilt->table->is_readable()) {
		/* Commit the transaction in order to
		release the table lock. */
		trx_commit_for_mysql(m_prebuilt->trx);

		ib::error() << "Unable to import tablespace "
			<< m_prebuilt->table->name << " because it already"
			" exists.  Please DISCARD the tablespace"
			" before IMPORT.";
		ib_senderrf(
			m_prebuilt->trx->mysql_thd, IB_LOG_LEVEL_ERROR,
			ER_TABLESPACE_EXISTS, m_prebuilt->table->name.m_name);

		DBUG_RETURN(HA_ERR_TABLE_EXIST);
	} else {
		err = row_import_for_mysql(m_prebuilt->table, m_prebuilt);

		if (err == DB_SUCCESS) {

			info(HA_STATUS_TIME
			     | HA_STATUS_CONST
			     | HA_STATUS_VARIABLE
			     | HA_STATUS_AUTO);

			fil_crypt_add_imported_space(m_prebuilt->table->space);
		}
	}

	ut_ad(m_prebuilt->trx->state == TRX_STATE_NOT_STARTED);

	if (discard || err != DB_SUCCESS) {
		DBUG_RETURN(convert_error_code_to_mysql(
				    err, m_prebuilt->table->flags, NULL));
	}

	dict_table_t* t = m_prebuilt->table;

	if (dberr_t ret = dict_stats_update_persistent_try(t)) {
		push_warning_printf(
			ha_thd(),
			Sql_condition::WARN_LEVEL_WARN,
			ER_ALTER_INFO,
			"Error updating stats after"
			" ALTER TABLE %.*sQ.%sQ IMPORT TABLESPACE: %s",
			int(t->name.dblen()), t->name.m_name,
			t->name.basename(), ut_strerr(ret));
	}

	DBUG_RETURN(0);
}

/** Report a DROP TABLE failure due to a FOREIGN KEY constraint.
@param name     table name
@param foreign  constraint */
ATTRIBUTE_COLD
static void delete_table_cannot_drop_foreign(const table_name_t &name,
                                             const dict_foreign_t &foreign)
{
  mysql_mutex_lock(&dict_foreign_err_mutex);
  rewind(dict_foreign_err_file);
  ut_print_timestamp(dict_foreign_err_file);
  fputs("  Cannot drop table ", dict_foreign_err_file);
  ut_print_name(dict_foreign_err_file, nullptr, name.m_name);
  fputs("\nbecause it is referenced by ", dict_foreign_err_file);
  ut_print_name(dict_foreign_err_file, nullptr, foreign.foreign_table_name);
  putc('\n', dict_foreign_err_file);
  mysql_mutex_unlock(&dict_foreign_err_mutex);
}

/** Check if DROP TABLE would fail due to a FOREIGN KEY constraint.
@param table    table to be dropped
@param sqlcom   thd_sql_command(current_thd)
@return whether child tables that refer to this table exist */
static bool delete_table_check_foreigns(const dict_table_t &table,
                                        enum_sql_command sqlcom)
{
  const bool drop_db{sqlcom == SQLCOM_DROP_DB};
  for (const auto foreign : table.referenced_set)
  {
    /* We should allow dropping a referenced table if creating
    that referenced table has failed for some reason. For example
    if referenced table is created but it column types that are
    referenced do not match. */
    if (foreign->foreign_table == &table ||
        (drop_db &&
         dict_tables_have_same_db(table.name.m_name,
                                  foreign->foreign_table_name_lookup)))
      continue;
    delete_table_cannot_drop_foreign(table.name, *foreign);
    return true;
  }

  return false;
}

/** DROP TABLE (possibly as part of DROP DATABASE, CREATE/ALTER TABLE)
@param name   table name
@return error number */
int ha_innobase::delete_table(const char *name)
{
  DBUG_ENTER("ha_innobase::delete_table");
  if (high_level_read_only)
    DBUG_RETURN(HA_ERR_TABLE_READONLY);

  THD *thd= ha_thd();

  DBUG_EXECUTE_IF("test_normalize_table_name_low",
                  test_normalize_table_name_low(););

  const enum_sql_command sqlcom= enum_sql_command(thd_sql_command(thd));
  trx_t *parent_trx= check_trx_exists(thd);
  dict_table_t *table;

  {
    char norm_name[FN_REFLEN];
    size_t norm_len= normalize_table_name_c_low(norm_name, sizeof(norm_name),
    						name, IF_WIN(true,false));
    span<const char> n{norm_name, norm_len};

    dict_sys.lock(SRW_LOCK_CALL);
    table= dict_sys.load_table(n, DICT_ERR_IGNORE_DROP);
#ifdef WITH_PARTITION_STORAGE_ENGINE
    if (!table && lower_case_table_names == 1 && is_partition(norm_name))
    {
      norm_len= normalize_table_name_c_low(norm_name, sizeof(norm_name),
      					name, IF_WIN(false, true));
      n= {norm_name, norm_len};
      table= dict_sys.load_table(n, DICT_ERR_IGNORE_DROP);
    }
#endif
    if (!table)
    {
      dict_sys.unlock();
      DBUG_RETURN(HA_ERR_NO_SUCH_TABLE);
    }
  }

  if (table->is_temporary())
  {
    dict_sys.unlock();
    parent_trx->mod_tables.erase(table); /* CREATE...SELECT error handling */
    btr_drop_temporary_table(*table);
    dict_sys.lock(SRW_LOCK_CALL);
    dict_sys.remove(table);
    dict_sys.unlock();
    DBUG_RETURN(0);
  }

  if (parent_trx->check_foreigns &&
      delete_table_check_foreigns(*table, sqlcom))
  {
    dict_sys.unlock();
    DBUG_RETURN(HA_ERR_ROW_IS_REFERENCED);
  }

  table->acquire();
  dict_sys.unlock();

  trx_t *trx= parent_trx;
  dberr_t err= DB_SUCCESS;
  if (!trx->lock.table_locks.empty() &&
      thd_ddl_options(trx->mysql_thd)->is_create_select())
  {
    /* CREATE TABLE...PRIMARY KEY...SELECT ought to be dropping the
    table because a duplicate key was detected or a timeout occurred.

    We shall hijack the existing transaction to drop the table and
    commit the transaction.  If this is a partitioned table, one
    partition will use this hijacked transaction; others will use a
    separate transaction, one per partition. */
    ut_ad(!trx->dict_operation_lock_mode);
    ut_ad(trx->will_lock);
    ut_ad(trx->state == TRX_STATE_ACTIVE);
    trx->dict_operation= true;
  }
  else
  {
    trx= innobase_trx_allocate(thd);
    trx_start_for_ddl(trx);

    if (table->name.is_temporary())
      /* There is no need to lock any FOREIGN KEY child tables. */;
#ifdef WITH_PARTITION_STORAGE_ENGINE
    else if (table->name.part())
      /* FOREIGN KEY constraints cannot exist on partitioned tables. */;
#endif
    else
      err= lock_table_children(table, trx);
  }

  if (err == DB_SUCCESS)
    err= lock_table_for_trx(table, trx, LOCK_X);

  const bool fts= err == DB_SUCCESS &&
    (table->flags2 & (DICT_TF2_FTS_HAS_DOC_ID | DICT_TF2_FTS));

  if (fts)
  {
    fts_optimize_remove_table(table);
    purge_sys.stop_FTS(*table);
    err= fts_lock_tables(trx, *table);
  }

#ifdef WITH_PARTITION_STORAGE_ENGINE
  const bool rollback_add_partition=
    (sqlcom == SQLCOM_ALTER_TABLE && table->name.part());

  if (rollback_add_partition)
  {
    if (!fts)
      purge_sys.stop_FTS();
    /* This looks like the rollback of ALTER TABLE...ADD PARTITION
    that was caused by MDL timeout. We could have written undo log
    for inserting the data into the new partitions. */
    if (!(table->stat & dict_table_t::STATS_PERSISTENT_OFF))
    {
      /* We do not really know if we are holding MDL_EXCLUSIVE. Even
      though this code is handling the case that we are not holding
      it, we might actually hold it. We want to avoid a deadlock
      with dict_stats_process_entry_from_recalc_pool(). */
      dict_stats_recalc_pool_del(table->id, true);
      /* If statistics calculation is still using this table, we will
      catch it below while waiting for purge to stop using this table. */
    }
  }
#endif

  DEBUG_SYNC(thd, "before_delete_table_stats");
  dict_stats stats;
  bool stats_failed= true;

  if (err == DB_SUCCESS && table->stats_is_persistent() &&
      !table->is_stats_table())
  {
    stats_failed= stats.open(thd);
    const bool skip_wait{table->name.is_temporary()};

    if (!stats_failed &&
        !(err= lock_table_for_trx(stats.table(), trx, LOCK_X, skip_wait)))
      err= lock_table_for_trx(stats.index(), trx, LOCK_X, skip_wait);

    if (err != DB_SUCCESS && skip_wait)
    {
      /* We may skip deleting statistics if we cannot lock the tables,
      when the table carries a temporary name. */
      ut_ad(err == DB_LOCK_WAIT);
      ut_ad(trx->error_state == DB_SUCCESS);
      err= DB_SUCCESS;
      stats.close();
      stats_failed= true;
    }
  }

  if (err == DB_SUCCESS)
  {
    if (!table->space)
    {
      const char *data_dir_path= DICT_TF_HAS_DATA_DIR(table->flags)
        ? table->data_dir_path : nullptr;
      char *path= fil_make_filepath(data_dir_path, table->name, CFG,
                                    data_dir_path != nullptr);
      os_file_delete_if_exists(innodb_data_file_key, path, nullptr);
      ut_free(path);
      path= fil_make_filepath(data_dir_path, table->name, IBD,
                              data_dir_path != nullptr);
      os_file_delete_if_exists(innodb_data_file_key, path, nullptr);
      ut_free(path);
      if (data_dir_path)
      {
        path= fil_make_filepath(nullptr, table->name, ISL, false);
        os_file_delete_if_exists(innodb_data_file_key, path, nullptr);
        ut_free(path);
      }
    }
    err= lock_sys_tables(trx);
  }

  dict_sys.lock(SRW_LOCK_CALL);

  if (!table->release() && err == DB_SUCCESS)
  {
    /* Wait for purge threads to stop using the table. */
    for (uint n= 15;;)
    {
      dict_sys.unlock();
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      dict_sys.lock(SRW_LOCK_CALL);

      if (!--n)
      {
        err= DB_LOCK_WAIT_TIMEOUT;
        break;
      }
      if (!table->get_ref_count())
        break;
    }
  }

  trx->dict_operation_lock_mode= true;

  if (err != DB_SUCCESS)
  {
err_exit:
    trx->rollback();
    switch (err) {
    case DB_CANNOT_DROP_CONSTRAINT:
    case DB_LOCK_WAIT_TIMEOUT:
      break;
    default:
      ib::error() << "DROP TABLE " << table->name << ": " << err;
    }
    if (fts)
    {
      fts_optimize_add_table(table);
      purge_sys.resume_FTS();
    }
#ifdef WITH_PARTITION_STORAGE_ENGINE
    else if (rollback_add_partition)
      purge_sys.resume_FTS();
#endif
    row_mysql_unlock_data_dictionary(trx);
    if (trx != parent_trx)
      trx->free();
    if (!stats_failed)
      stats.close();
    DBUG_RETURN(convert_error_code_to_mysql(err, 0, NULL));
  }

  if (!table->no_rollback())
  {
    if (trx->check_foreigns && delete_table_check_foreigns(*table, sqlcom))
    {
      err= DB_CANNOT_DROP_CONSTRAINT;
      goto err_exit;
    }

    err= trx->drop_table_foreign(table->name);
  }

  if (err == DB_SUCCESS && !stats_failed)
    err= trx->drop_table_statistics(table->name);
  if (err != DB_SUCCESS)
    goto err_exit;

  err= trx->drop_table(*table);
  if (err != DB_SUCCESS)
    goto err_exit;

  std::vector<pfs_os_file_t> deleted;
  trx->commit(deleted);
  row_mysql_unlock_data_dictionary(trx);
  if (!stats_failed)
    stats.close();
  for (pfs_os_file_t d : deleted)
    os_file_close(d);
  log_write_up_to(trx->commit_lsn, true);
  if (trx != parent_trx)
    trx->free();
  if (!fts)
#ifdef WITH_PARTITION_STORAGE_ENGINE
  if (!rollback_add_partition)
#endif
    DBUG_RETURN(0);
  purge_sys.resume_FTS();
  DBUG_RETURN(0);
}

/** Rename an InnoDB table.
@param[in,out]	trx	InnoDB data dictionary transaction
@param[in]	from	old table name
@param[in]	to	new table name
@param[in]	fk	how to handle FOREIGN KEY
@return DB_SUCCESS or error code */
static dberr_t innobase_rename_table(trx_t *trx, const char *from,
                                     const char *to, rename_fk fk)
{
	dberr_t	error;
	char	norm_to[FN_REFLEN];
	char	norm_from[FN_REFLEN];

	DBUG_ENTER("innobase_rename_table");
	DBUG_ASSERT(trx->dict_operation);

	ut_ad(!srv_read_only_mode);

	normalize_table_name(norm_to, sizeof(norm_to), to);
	normalize_table_name(norm_from, sizeof(norm_from), from);

	DEBUG_SYNC_C("innodb_rename_table_ready");

	ut_ad(trx->will_lock);

	error = row_rename_table_for_mysql(norm_from, norm_to, trx, fk);

	if (error != DB_SUCCESS) {
		if (error == DB_TABLE_NOT_FOUND
		    && lower_case_table_names == 1) {
			char*	is_part = is_partition(norm_from);

			if (is_part) {
				char	par_case_name[FN_REFLEN];
#ifndef _WIN32
				/* Check for the table using lower
				case name, including the partition
				separator "P" */
				system_charset_info->casedn_z(
					norm_from, strlen(norm_from),
					par_case_name, sizeof(par_case_name));
#else
				/* On Windows platfrom, check
				whether there exists table name in
				system table whose name is
				not being normalized to lower case */
				normalize_table_name_c_low(
					par_case_name, sizeof(par_case_name),
					from, false);
#endif /* _WIN32 */
				trx_start_if_not_started(trx, true);
				error = row_rename_table_for_mysql(
					par_case_name, norm_to, trx,
					RENAME_IGNORE_FK);
			}
		}

		if (error == DB_SUCCESS) {
#ifndef _WIN32
			sql_print_warning("Rename partition table %s"
					  " succeeds after converting to lower"
					  " case. The table may have"
					  " been moved from a case"
					  " in-sensitive file system.\n",
					  norm_from);
#else
			sql_print_warning("Rename partition table %s"
					  " succeeds after skipping the step to"
					  " lower case the table name."
					  " The table may have been"
					  " moved from a case sensitive"
					  " file system.\n",
					  norm_from);
#endif /* _WIN32 */
		}
	}

	DBUG_RETURN(error);
}

/** TRUNCATE TABLE
@return	error code
@retval	0	on success */
int ha_innobase::truncate()
{
  mariadb_set_stats set_stats_temporary(handler_stats);
  DBUG_ENTER("ha_innobase::truncate");

  update_thd();

#ifdef UNIV_DEBUG
  if (!thd_test_options(m_user_thd, OPTION_NO_FOREIGN_KEY_CHECKS))
  {
    /* fk_truncate_illegal_if_parent() should have failed in
    Sql_cmd_truncate_table::handler_truncate() if foreign_key_checks=ON
    and child tables exist. */
    dict_sys.freeze(SRW_LOCK_CALL);
    for (const auto foreign : m_prebuilt->table->referenced_set)
      ut_ad(foreign->foreign_table == m_prebuilt->table);
    dict_sys.unfreeze();
  }
#endif

  if (int err= is_valid_trx())
    DBUG_RETURN(err);

  HA_CREATE_INFO info;
  dict_table_t *ib_table= m_prebuilt->table;
  info.init();
  update_create_info_from_table(&info, table);
  switch (dict_tf_get_rec_format(ib_table->flags)) {
  case REC_FORMAT_REDUNDANT:
    info.row_type= ROW_TYPE_REDUNDANT;
    break;
  case REC_FORMAT_COMPACT:
    info.row_type= ROW_TYPE_COMPACT;
    break;
  case REC_FORMAT_COMPRESSED:
    info.row_type= ROW_TYPE_COMPRESSED;
    break;
  case REC_FORMAT_DYNAMIC:
    info.row_type= ROW_TYPE_DYNAMIC;
    break;
  }

  const auto stored_lock= m_prebuilt->stored_select_lock_type;
  trx_t *trx= innobase_trx_allocate(m_user_thd);
  trx_start_for_ddl(trx);

  if (ib_table->is_temporary())
  {
    info.options|= HA_LEX_CREATE_TMP_TABLE;
    btr_drop_temporary_table(*ib_table);
    m_prebuilt->table= nullptr;
    row_prebuilt_free(m_prebuilt);
    m_prebuilt= nullptr;
    my_free(m_upd_buf);
    m_upd_buf= nullptr;
    m_upd_buf_size= 0;

    row_mysql_lock_data_dictionary(trx);
    ib_table->release();
    dict_sys.remove(ib_table, false, true);
    int err= create(ib_table->name.m_name, table, &info, true, trx);
    row_mysql_unlock_data_dictionary(trx);

    ut_ad(!err);
    if (!err)
    {
      err= open(ib_table->name.m_name, 0, 0);
      m_prebuilt->table->release();
      m_prebuilt->stored_select_lock_type= stored_lock;
    }

    trx->free();

#ifdef BTR_CUR_HASH_ADAPT
    if (UT_LIST_GET_LEN(ib_table->freed_indexes))
    {
      ib_table->vc_templ= nullptr;
      ib_table->id= 0;
    }
    else
#endif /* BTR_CUR_HASH_ADAPT */
    dict_mem_table_free(ib_table);

    DBUG_RETURN(err);
  }

  mem_heap_t *heap= mem_heap_create(1000);

  if (!ib_table->space)
    ib_senderrf(m_user_thd, IB_LOG_LEVEL_WARN, ER_TABLESPACE_DISCARDED,
                table->s->table_name.str);

  dict_get_and_save_data_dir_path(ib_table);
  info.data_file_name= ib_table->data_dir_path;
  const char *temp_name=
    dict_mem_create_temporary_tablename(heap,
                                        ib_table->name.m_name, ib_table->id);
  const char *name= mem_heap_strdup(heap, ib_table->name.m_name);

  dberr_t error= lock_table_children(ib_table, trx);

  if (error == DB_SUCCESS)
    error= lock_table_for_trx(ib_table, trx, LOCK_X);

  const bool fts= error == DB_SUCCESS &&
    ib_table->flags2 & (DICT_TF2_FTS_HAS_DOC_ID | DICT_TF2_FTS);
  const bool pause_purge= error == DB_SUCCESS && ib_table->get_ref_count() > 1;

  if (fts)
  {
    fts_optimize_remove_table(ib_table);
    purge_sys.stop_FTS(*ib_table);
    error= fts_lock_tables(trx, *ib_table);
  }
  else if (pause_purge)
    purge_sys.stop_FTS();

  if (error == DB_SUCCESS)
  {
    /* Wait for purge threads to stop using the table. */
    for (uint n = 15; ib_table->get_ref_count() > 1; )
    {
      if (!--n)
      {
        error= DB_LOCK_WAIT_TIMEOUT;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }

  dict_stats stats;
  bool stats_failed= true;

  if (error == DB_SUCCESS && ib_table->stats_is_persistent() &&
      !ib_table->is_stats_table())
  {
    stats_failed= stats.open(m_user_thd);
    if (!stats_failed &&
        !(error= lock_table_for_trx(stats.table(), trx, LOCK_X)))
      error= lock_table_for_trx(stats.index(), trx, LOCK_X);
  }

  if (error == DB_SUCCESS)
    error= lock_sys_tables(trx);

  std::vector<pfs_os_file_t> deleted;

  row_mysql_lock_data_dictionary(trx);

  if (error == DB_SUCCESS)
  {
    error= innobase_rename_table(trx, ib_table->name.m_name, temp_name,
                                 RENAME_REBUILD);
    if (error == DB_SUCCESS)
      error= trx->drop_table(*ib_table);
  }

  int err = convert_error_code_to_mysql(error, ib_table->flags, m_user_thd);
  const auto update_time = ib_table->update_time;

  if (err)
  {
    trx_rollback_for_mysql(trx);
    if (fts)
      fts_optimize_add_table(ib_table);
  }
  else
  {
    const auto def_trx_id= ib_table->def_trx_id;
    ib_table->release();
    m_prebuilt->table= nullptr;

    err= create(name, table, &info, dict_table_is_file_per_table(ib_table),
                trx);
    if (!err)
    {
      m_prebuilt->table->acquire();
      create_table_info_t::create_table_update_dict(m_prebuilt->table,
                                                    m_user_thd, info, *table);
      trx->commit(deleted);
    }
    else
    {
      trx_rollback_for_mysql(trx);
      m_prebuilt->table= dict_table_open_on_name(name, true,
                                                 DICT_ERR_IGNORE_FK_NOKEY);
      m_prebuilt->table->def_trx_id= def_trx_id;
    }
    dict_names_t fk_tables;
    dict_load_foreigns(m_prebuilt->table->name.m_name, nullptr, 1, true,
                       DICT_ERR_IGNORE_FK_NOKEY, fk_tables);
    for (const char *f : fk_tables)
      dict_sys.load_table({f, strlen(f)});
  }

  if (fts)
    purge_sys.resume_FTS();

  row_mysql_unlock_data_dictionary(trx);
  for (pfs_os_file_t d : deleted) os_file_close(d);

  if (!err)
  {
    dict_stats_empty_table_and_save(m_prebuilt->table);
    log_write_up_to(trx->commit_lsn, true);
    row_prebuilt_t *prebuilt= m_prebuilt;
    uchar *upd_buf= m_upd_buf;
    ulint upd_buf_size= m_upd_buf_size;
    /* Mimic ha_innobase::close(). */
    m_prebuilt= nullptr;
    m_upd_buf= nullptr;
    m_upd_buf_size= 0;

    err= open(name, 0, 0);
    if (!err)
    {
      m_prebuilt->stored_select_lock_type= stored_lock;
      m_prebuilt->table->update_time= update_time;
      row_prebuilt_free(prebuilt);
      my_free(upd_buf);
    }
    else
    {
      /* Revert to the old table. */
      m_prebuilt= prebuilt;
      m_upd_buf= upd_buf;
      m_upd_buf_size= upd_buf_size;
    }
  }

  trx->free();
  if (!stats_failed)
    stats.close();
  mem_heap_free(heap);
  DBUG_RETURN(err);
}

/** Deinitialize InnoDB persistent statistics, forcing them
to be reloaded on subsequent ha_innobase::open().
@param t  table for which the cached STATS_PERSISTENT are to be evicted */
static void stats_deinit(dict_table_t *t) noexcept
{
  ut_ad(dict_sys.frozen());
  ut_ad(t->get_ref_count() == 0);

  if (t->is_temporary() || t->no_rollback())
    return;

  t->stats_mutex_lock();
  t->stat= t->stat & ~dict_table_t::STATS_INITIALIZED;
  MEM_UNDEFINED(&t->stat_n_rows, sizeof t->stat_n_rows);
  MEM_UNDEFINED(&t->stat_clustered_index_size,
                sizeof t->stat_clustered_index_size);
  MEM_UNDEFINED(&t->stat_sum_of_other_index_sizes,
                sizeof t->stat_sum_of_other_index_sizes);
  MEM_UNDEFINED(&t->stat_modified_counter, sizeof t->stat_modified_counter);
#ifdef HAVE_valgrind
  for (dict_index_t *i= dict_table_get_first_index(t); i;
       i= dict_table_get_next_index(i))
  {
    MEM_UNDEFINED(i->stat_n_diff_key_vals,
                  i->n_uniq * sizeof *i->stat_n_diff_key_vals);
    MEM_UNDEFINED(i->stat_n_sample_sizes,
                  i->n_uniq * sizeof *i->stat_n_sample_sizes);
    MEM_UNDEFINED(i->stat_n_non_null_key_vals,
                  i->n_uniq * sizeof *i->stat_n_non_null_key_vals);
    MEM_UNDEFINED(&i->stat_index_size, sizeof i->stat_index_size);
    MEM_UNDEFINED(&i->stat_n_leaf_pages, sizeof i->stat_n_leaf_pages);
  }
#endif /* HAVE_valgrind */
  t->stats_mutex_unlock();
}

/*********************************************************************//**
Renames an InnoDB table.
@return 0 or error code */

int
ha_innobase::rename_table(
/*======================*/
	const char*	from,	/*!< in: old name of the table */
	const char*	to)	/*!< in: new name of the table */
{
	THD*	thd = ha_thd();

	DBUG_ENTER("ha_innobase::rename_table");

	if (high_level_read_only) {
		ib_senderrf(thd, IB_LOG_LEVEL_WARN, ER_READ_ONLY_MODE);
		DBUG_RETURN(HA_ERR_TABLE_READONLY);
	}

	trx_t*	trx = innobase_trx_allocate(thd);
	trx_start_for_ddl(trx);

	char norm_from[MAX_FULL_NAME_LEN];
	char norm_to[MAX_FULL_NAME_LEN];

	normalize_table_name(norm_from, sizeof(norm_from), from);
	normalize_table_name(norm_to, sizeof(norm_to), to);

	dberr_t error = DB_SUCCESS;
	const bool from_temp = dict_table_t::is_temporary_name(norm_from);

	dict_table_t* t;
	bool pause_purge = false, fts_exist = false;

	if (from_temp) {
		/* There is no need to lock any FOREIGN KEY child tables. */
		t = nullptr;
	} else {
		t = dict_table_open_on_name(
			norm_from, false, DICT_ERR_IGNORE_FK_NOKEY);
		if (t) {
			error = lock_table_children(t, trx);
			if (error == DB_SUCCESS) {
				error = lock_table_for_trx(t, trx, LOCK_X);
			}
			fts_exist = error == DB_SUCCESS && t->flags2
				& (DICT_TF2_FTS_HAS_DOC_ID | DICT_TF2_FTS);
			pause_purge = error == DB_SUCCESS
				&& t->get_ref_count() > 1;
			if (fts_exist) {
				fts_optimize_remove_table(t);
				purge_sys.stop_FTS(*t);
				if (error == DB_SUCCESS) {
					error = fts_lock_tables(trx, *t);
				}
			} else if (pause_purge) {
				purge_sys.stop_FTS();
			}
		}
	}

	dict_stats stats;
	bool stats_fail = true;

	if (strcmp(norm_from, TABLE_STATS_NAME)
	    && strcmp(norm_from, INDEX_STATS_NAME)
	    && strcmp(norm_to, TABLE_STATS_NAME)
	    && strcmp(norm_to, INDEX_STATS_NAME)) {
		stats_fail = stats.open(thd);
		if (!stats_fail && error == DB_SUCCESS) {
			error = lock_table_for_trx(stats.table(), trx,
						   LOCK_X, from_temp);
			if (error == DB_SUCCESS) {
				error = lock_table_for_trx(stats.index(), trx,
							   LOCK_X, from_temp);
			}
			if (error != DB_SUCCESS && from_temp) {
				ut_ad(error == DB_LOCK_WAIT);
				ut_ad(trx->error_state == DB_SUCCESS);
				error = DB_SUCCESS;
				/* We may skip renaming statistics if
				we cannot lock the tables, when the
				table is being renamed from from a
				temporary name. */
				stats.close();
				stats_fail = true;
			}
		}
	}

	if (error == DB_SUCCESS) {
		error = lock_table_for_trx(dict_sys.sys_tables, trx, LOCK_X);
		if (error == DB_SUCCESS) {
			error = lock_table_for_trx(dict_sys.sys_foreign, trx,
						   LOCK_X);
			if (error == DB_SUCCESS) {
				error = lock_table_for_trx(
					dict_sys.sys_foreign_cols,
					trx, LOCK_X);
			}
		}
	}

	row_mysql_lock_data_dictionary(trx);

	if (error == DB_SUCCESS) {
		error = innobase_rename_table(trx, from, to,
					      RENAME_ALTER_COPY);
	}

	DEBUG_SYNC(thd, "after_innobase_rename_table");

	if (error == DB_SUCCESS && !stats_fail) {
		error = dict_stats_rename_table(norm_from, norm_to, trx);
		if (error == DB_DUPLICATE_KEY) {
			/* The duplicate may also occur in
			mysql.innodb_index_stats.  */
			my_error(ER_DUP_KEY, MYF(0),
				 "mysql.innodb_table_stats");
			error = DB_ERROR;
		}
	}

	if (error == DB_SUCCESS) {
		trx->flush_log_later = true;
		if (t) {
			ut_ad(dict_sys.locked());
			if (fts_exist) {
		                fts_optimize_add_table(t);
                        }
			if (UNIV_LIKELY(t->release())) {
				stats_deinit(t);
			} else {
				ut_ad("unexpected references" == 0);
			}
		}
		innobase_commit_low(trx);
	} else {
		if (t) {
			if (fts_exist) {
		                fts_optimize_add_table(t);
                        }
			t->release();
		}
		trx->rollback();
	}

	row_mysql_unlock_data_dictionary(trx);

	if (fts_exist || pause_purge) {
		purge_sys.resume_FTS();
	}

	if (error == DB_SUCCESS) {
		log_write_up_to(trx->commit_lsn, true);
	}
	trx->flush_log_later = false;
	trx->free();
	if (!stats_fail) {
		stats.close();
	}

	switch (error) {
	case DB_SUCCESS:
		DBUG_RETURN(0);
	case DB_DUPLICATE_KEY:
		/* We are not able to deal with handler::get_dup_key()
		during DDL operations, because the duplicate key would
		exist in metadata tables, not in the user table. */
		my_error(ER_TABLE_EXISTS_ERROR, MYF(0), to);
		DBUG_RETURN(HA_ERR_GENERIC);
	case DB_LOCK_WAIT_TIMEOUT:
		my_error(ER_LOCK_WAIT_TIMEOUT, MYF(0));
		DBUG_RETURN(HA_ERR_GENERIC);
	case DB_FOREIGN_DUPLICATE_KEY:
		my_error(ER_DUP_CONSTRAINT_NAME, MYF(0), "FOREIGN KEY", "");
		DBUG_RETURN(HA_ERR_GENERIC);
	default:
		DBUG_RETURN(convert_error_code_to_mysql(error, 0, NULL));
	}
}

/*********************************************************************//**
Estimates the number of index records in a range.
@return estimated number of rows */

ha_rows
ha_innobase::records_in_range(
/*==========================*/
	uint			keynr,		/*!< in: index number */
	const key_range		*min_key,	/*!< in: start key value of the
						range, may also be 0 */
	const key_range		*max_key,	/*!< in: range end key val, may
						also be 0 */
        page_range              *pages)
{
	KEY*		key;
	dict_index_t*	index;
	dtuple_t*	range_start;
	dtuple_t*	range_end;
	ha_rows		n_rows = HA_POS_ERROR;
	page_cur_mode_t	mode1;
	page_cur_mode_t	mode2;
	mem_heap_t*	heap;

	DBUG_ENTER("records_in_range");

	ut_ad(m_prebuilt->trx == thd_to_trx(ha_thd()));

	m_prebuilt->trx->op_info = "estimating records in index range";

	active_index = keynr;

	key = table->key_info + active_index;

	index = innobase_get_index(keynr);

	/* There exists possibility of not being able to find requested
	index due to inconsistency between MySQL and InoDB dictionary info.
	Necessary message should have been printed in innobase_get_index() */
	if (!index || !m_prebuilt->table->space) {
		goto func_exit;
	}
	if (index->is_corrupted()) {
		n_rows = HA_ERR_INDEX_CORRUPT;
		goto func_exit;
	}
	if (!row_merge_is_index_usable(m_prebuilt->trx, index)) {
		n_rows = HA_ERR_TABLE_DEF_CHANGED;
		goto func_exit;
	}

	heap = mem_heap_create(2 * (key->ext_key_parts * sizeof(dfield_t)
				    + sizeof(dtuple_t)));

	range_start = dtuple_create(heap, key->ext_key_parts);

	range_end = dtuple_create(heap, key->ext_key_parts);

	if (!min_key) {
		mode1 = PAGE_CUR_GE;
		dtuple_set_n_fields(range_start, 0);
	} else if (convert_search_mode_to_innobase(min_key->flag, mode1)) {
		goto unsupported;
	} else {
		dict_index_copy_types(range_start, index, key->ext_key_parts);
		row_sel_convert_mysql_key_to_innobase(
			range_start,
			m_prebuilt->srch_key_val1,
			m_prebuilt->srch_key_val_len,
			index, min_key->key, min_key->length);
		DBUG_ASSERT(range_start->n_fields > 0);
	}

	if (!max_key) {
		mode2 = PAGE_CUR_GE;
		dtuple_set_n_fields(range_end, 0);
	} else if (convert_search_mode_to_innobase(max_key->flag, mode2)) {
		goto unsupported;
	} else {
		dict_index_copy_types(range_end, index, key->ext_key_parts);
		row_sel_convert_mysql_key_to_innobase(
			range_end,
			m_prebuilt->srch_key_val2,
			m_prebuilt->srch_key_val_len,
			index, max_key->key, max_key->length);
		DBUG_ASSERT(range_end->n_fields > 0);
	}

	if (dict_index_is_spatial(index)) {
		/*Only min_key used in spatial index. */
		n_rows = rtr_estimate_n_rows_in_range(
			index, range_start, mode1);
	} else {
		btr_pos_t tuple1(range_start, mode1, pages->first_page);
		btr_pos_t tuple2(range_end,   mode2, pages->last_page);
		n_rows = btr_estimate_n_rows_in_range(index, &tuple1, &tuple2);
		pages->first_page= tuple1.page_id.raw();
		pages->last_page=  tuple2.page_id.raw();
	}

	DBUG_EXECUTE_IF(
		"print_btr_estimate_n_rows_in_range_return_value",
		push_warning_printf(
			ha_thd(), Sql_condition::WARN_LEVEL_WARN,
			ER_NO_DEFAULT,
			"btr_estimate_n_rows_in_range(): %lld",
                        (longlong) n_rows);
	);

	/* The MariaDB optimizer seems to believe an estimate of 0 rows is
	always accurate and may return the result 'Empty set' based on that.
	The accuracy is not guaranteed, and even if it were, for a locking
	read we should anyway perform the search to set the next-key lock.
	Add 1 to the value to make sure MySQL does not make the assumption! */

	if (n_rows == 0) {
		n_rows = 1;
	}

unsupported:
	mem_heap_free(heap);
func_exit:
	m_prebuilt->trx->op_info = "";
	DBUG_RETURN((ha_rows) n_rows);
}

/*********************************************************************//**
Gives an UPPER BOUND to the number of rows in a table. This is used in
filesort.cc.
@return upper bound of rows */

ha_rows
ha_innobase::estimate_rows_upper_bound()
/*====================================*/
{
	const dict_index_t*	index;
	ulonglong		estimate;
	ulonglong		local_data_file_length;
	mariadb_set_stats set_stats_temporary(handler_stats);
	DBUG_ENTER("estimate_rows_upper_bound");

	/* We do not know if MySQL can call this function before calling
	external_lock(). To be safe, update the thd of the current table
	handle. */

	update_thd(ha_thd());

	m_prebuilt->trx->op_info = "calculating upper bound for table rows";

	index = dict_table_get_first_index(m_prebuilt->table);

	ulint	stat_n_leaf_pages = index->stat_n_leaf_pages;

	ut_a(stat_n_leaf_pages > 0);

	local_data_file_length = ulonglong(stat_n_leaf_pages)
		<< srv_page_size_shift;

	/* Calculate a minimum length for a clustered index record and from
	that an upper bound for the number of rows. Since we only calculate
	new statistics in row0mysql.cc when a table has grown by a threshold
	factor, we must add a safety factor 2 in front of the formula below. */

	estimate = 2 * local_data_file_length
		/ dict_index_calc_min_rec_len(index);

	m_prebuilt->trx->op_info = "";

        /* Set num_rows less than MERGEBUFF to simulate the case where we do
        not have enough space to merge the externally sorted file blocks. */
        DBUG_EXECUTE_IF("set_num_rows_lt_MERGEBUFF",
                        estimate = 2;
                        DBUG_SET("-d,set_num_rows_lt_MERGEBUFF");
                       );

	DBUG_RETURN((ha_rows) estimate);
}


/*********************************************************************//**
How many seeks it will take to read through the table. This is to be
comparable to the number returned by records_in_range so that we can
decide if we should scan the table or use keys.
@return estimated time measured in disk seeks */

#ifdef NOT_USED
IO_AND_CPU_COST
ha_innobase::scan_time()
/*====================*/
{
	/* Since MySQL seems to favor table scans too much over index
	searches, we pretend that a sequential read takes the same time
	as a random disk read, that is, we do not divide the following
	by 10, which would be physically realistic. */

	/* The locking below is disabled for performance reasons. Without
	it we could end up returning uninitialized value to the caller,
	which in the worst case could make some query plan go bogus or
	issue a Valgrind warning. */
	if (m_prebuilt == NULL) {
		/* In case of derived table, Optimizer will try to fetch stat
		for table even before table is create or open. In such
		cases return default value of 1.
		TODO: This will be further improved to return some approximate
		estimate but that would also needs pre-population of stats
		structure. As of now approach is in sync with MyISAM. */
          return { (ulonglong2double(stats.data_file_length) / IO_SIZE * DISK_READ_COST), 0.0 };
	}

	ulint	stat_clustered_index_size;
        IO_AND_CPU_COST cost;
	ut_ad(m_prebuilt->table->stat_initialized());

	stat_clustered_index_size =
		m_prebuilt->table->stat_clustered_index_size;

        cost.io= (double) stat_clustered_index_size * DISK_READ_COST;
        cost.cpu= 0;
	return(cost);
}
#endif

/******************************************************************//**
Calculate the time it takes to read a set of ranges through an index
This enables us to optimise reads for clustered indexes.
@return estimated time measured in disk seeks */

#ifdef NOT_USED
double
ha_innobase::read_time(
/*===================*/
	uint	index,	/*!< in: key number */
	uint	ranges,	/*!< in: how many ranges */
	ha_rows rows)	/*!< in: estimated number of rows in the ranges */
{
	ha_rows total_rows;

	if (index != table->s->primary_key) {
		/* Not clustered */
		return(handler::read_time(index, ranges, rows));
	}

	/* Assume that the read time is proportional to the scan time for all
	rows + at most one seek per range. */

	double	time_for_scan = scan_time();

	if ((total_rows = estimate_rows_upper_bound()) < rows) {

		return(time_for_scan);
	}

	return(ranges * KEY_LOOKUP_COST + (double) rows / (double) total_rows * time_for_scan);
}

/******************************************************************//**
Calculate the time it takes to read a set of rows with primary key.
*/

IO_AND_CPU_COST
ha_innobase::rnd_pos_time(ha_rows rows)
{
	ha_rows total_rows;

	/* Assume that the read time is proportional to the scan time for all
	rows + at most one seek per range. */

	IO_AND_CPU_COST	time_for_scan = scan_time();

	if ((total_rows = estimate_rows_upper_bound()) < rows) {

		return(time_for_scan);
	}
        double frac= (double) rows + (double) rows / (double) total_rows;
        time_for_scan.io*= frac;
        time_for_scan.cpu*= frac;
	return(time_for_scan);
}
#endif

/*********************************************************************//**
Calculates the key number used inside MySQL for an Innobase index.
@return the key number used inside MySQL */
static
unsigned
innobase_get_mysql_key_number_for_index(
/*====================================*/
	const TABLE*		table,	/*!< in: table in MySQL data
					dictionary */
	dict_table_t*		ib_table,/*!< in: table in InnoDB data
					dictionary */
	const dict_index_t*	index)	/*!< in: index */
{
	const dict_index_t*	ind;
	unsigned int		i;

	/* If index does not belong to the table object of share structure
	(ib_table comes from the share structure) search the index->table
	object instead */
	if (index->table != ib_table) {
		i = 0;
		ind = dict_table_get_first_index(index->table);
		const bool auto_gen_clust = dict_index_is_auto_gen_clust(ind);

		while (index != ind) {
			ind = dict_table_get_next_index(ind);
			i++;
		}

		if (auto_gen_clust) {
			ut_a(i > 0);
			i--;
		}

		return(i);
	}

	/* Directly find matching index with information from mysql TABLE
	structure and InnoDB dict_index_t list */
	for (i = 0; i < table->s->keys; i++) {
		ind = dict_table_get_index_on_name(
			ib_table, table->key_info[i].name.str);

		if (index == ind) {
			return(i);
		}
	}

	/* Loop through each index of the table and lock them */
	for (ind = dict_table_get_first_index(ib_table);
	     ind != NULL;
	     ind = dict_table_get_next_index(ind)) {
		if (index == ind) {
			/* Temp index is internal to InnoDB, that is
			not present in the MySQL index list, so no
			need to print such mismatch warning. */
			if (index->is_committed()) {
				sql_print_warning(
					"Found index %s in InnoDB index list"
					" but not its MariaDB index number."
					" It could be an InnoDB internal"
					" index.",
					index->name());
			}
			return(~0U);
		}
	}

	ut_error;

	return(~0U);
}

/*********************************************************************//**
Calculate Record Per Key value. Need to exclude the NULL value if
innodb_stats_method is set to "nulls_ignored"
@return estimated record per key value */
rec_per_key_t
innodb_rec_per_key(
/*===============*/
	dict_index_t*	index,		/*!< in: dict_index_t structure */
	ulint		i,		/*!< in: the column we are
					calculating rec per key */
	ha_rows		records)	/*!< in: estimated total records */
{
	rec_per_key_t	rec_per_key;
	ib_uint64_t	n_diff;

	ut_ad(index->table->stat_initialized());

	ut_ad(i < dict_index_get_n_unique(index));
	ut_ad(!dict_index_is_spatial(index));

	if (records == 0) {
		/* "Records per key" is meaningless for empty tables.
		Return 1.0 because that is most convenient to the Optimizer. */
		return(1.0);
	}

	n_diff = index->stat_n_diff_key_vals[i];

	if (n_diff == 0) {

		rec_per_key = static_cast<rec_per_key_t>(records);
	} else if (srv_innodb_stats_method == SRV_STATS_NULLS_IGNORED) {
		ib_uint64_t	n_null;
		ib_uint64_t	n_non_null;

		n_non_null = index->stat_n_non_null_key_vals[i];

		/* In theory, index->stat_n_non_null_key_vals[i]
		should always be less than the number of records.
		Since this is statistics value, the value could
		have slight discrepancy. But we will make sure
		the number of null values is not a negative number. */
		if (records < n_non_null) {
			n_null = 0;
		} else {
			n_null = records - n_non_null;
		}

		/* If the number of NULL values is the same as or
		larger than that of the distinct values, we could
		consider that the table consists mostly of NULL value.
		Set rec_per_key to 1. */
		if (n_diff <= n_null) {
			rec_per_key = 1.0;
		} else {
			/* Need to exclude rows with NULL values from
			rec_per_key calculation */
			rec_per_key
				= static_cast<rec_per_key_t>(records - n_null)
				/ static_cast<rec_per_key_t>(n_diff - n_null);
		}
	} else {
		DEBUG_SYNC_C("after_checking_for_0");
		rec_per_key = static_cast<rec_per_key_t>(records)
			/ static_cast<rec_per_key_t>(n_diff);
	}

	if (rec_per_key < 1.0) {
		/* Values below 1.0 are meaningless and must be due to the
		stats being imprecise. */
		rec_per_key = 1.0;
	}

	return(rec_per_key);
}

/** Calculate how many KiB of new data we will be able to insert to the
tablespace without running out of space. Start with a space object that has
been acquired by the caller who holds it for the calculation,
@param[in]	space		tablespace object from fil_space_acquire()
@return available space in KiB */
static uintmax_t
fsp_get_available_space_in_free_extents(const fil_space_t& space)
{
	ulint	size_in_header = space.size_in_header;
	if (size_in_header < FSP_EXTENT_SIZE) {
		return 0;		/* TODO: count free frag pages and
					return a value based on that */
	}

	/* Below we play safe when counting free extents above the free limit:
	some of them will contain extent descriptor pages, and therefore
	will not be free extents */
	ut_ad(size_in_header >= space.free_limit);
	ulint	n_free_up =
		(size_in_header - space.free_limit) / FSP_EXTENT_SIZE;

	const ulint size = space.physical_size();
	if (n_free_up > 0) {
		n_free_up--;
		n_free_up -= n_free_up / (size / FSP_EXTENT_SIZE);
	}

	/* We reserve 1 extent + 0.5 % of the space size to undo logs
	and 1 extent + 0.5 % to cleaning operations; NOTE: this source
	code is duplicated in the function above! */

	ulint	reserve = 2 + ((size_in_header / FSP_EXTENT_SIZE) * 2) / 200;
	ulint	n_free = space.free_len + n_free_up;

	if (reserve > n_free) {
		return(0);
	}

	return(static_cast<uintmax_t>(n_free - reserve)
	       * FSP_EXTENT_SIZE * (size / 1024));
}

/*********************************************************************//**
Returns statistics information of the table to the MySQL interpreter,
in various fields of the handle object.
@return HA_ERR_* error code or 0 */
TRANSACTIONAL_TARGET
int
ha_innobase::info_low(
/*==================*/
	uint	flag,	/*!< in: what information is requested */
	bool	is_analyze)
{
	dict_table_t*	ib_table;
	ib_uint64_t	n_rows;
	char		path[FN_REFLEN];
	os_file_stat_t	stat_info;

	DBUG_ENTER("info");

	DEBUG_SYNC_C("ha_innobase_info_low");

	/* If we are forcing recovery at a high level, we will suppress
	statistics calculation on tables, because that may crash the
	server if an index is badly corrupted. */

	/* We do not know if MySQL can call this function before calling
	external_lock(). To be safe, update the thd of the current table
	handle. */

	update_thd(ha_thd());

	m_prebuilt->trx->op_info = "returning various info to MariaDB";

	ib_table = m_prebuilt->table;
	DBUG_ASSERT(ib_table->get_ref_count() > 0);

	if (!ib_table->is_readable()
	    || srv_force_recovery >= SRV_FORCE_NO_UNDO_LOG_SCAN) {
		dict_stats_empty_table(ib_table);
        } else if (flag & HA_STATUS_TIME) {
		stats.update_time = ib_table->update_time;
		if (!is_analyze && !innobase_stats_on_metadata) {
			goto stats_fetch;
		}

		dberr_t ret;
		m_prebuilt->trx->op_info = "updating table statistics";

		if (ib_table->stats_is_persistent()
		    && !srv_read_only_mode
		    && dict_stats_persistent_storage_check(false)
		    == SCHEMA_OK) {
			if (is_analyze) {
				dict_stats_recalc_pool_del(ib_table->id,
							   false);
recalc:
				ret = statistics_init(ib_table, is_analyze);
			} else {
				/* This is e.g. 'SHOW INDEXES' */
				ret = statistics_init(ib_table, is_analyze);
				switch (ret) {
				case DB_SUCCESS:
				case DB_READ_ONLY:
					break;
				default:
					goto error;
				case DB_STATS_DO_NOT_EXIST:
					if (!ib_table
					    ->stats_is_auto_recalc()) {
						break;
					}

					if (opt_bootstrap) {
						break;
					}
#ifdef WITH_WSREP
					if (wsrep_thd_skip_locking(
						    m_user_thd)) {
						break;
					}
#endif
					is_analyze = true;
					goto recalc;
				}
			}
		} else {
			ret = dict_stats_update_transient(ib_table);
			if (ret != DB_SUCCESS) {
error:
				m_prebuilt->trx->op_info = "";
				DBUG_RETURN(HA_ERR_GENERIC);
			}
		}

		m_prebuilt->trx->op_info = "returning various info to MariaDB";
	} else {
stats_fetch:
		statistics_init(ib_table, false);
	}

	if (flag & HA_STATUS_VARIABLE) {

		ulint	stat_clustered_index_size;
		ulint	stat_sum_of_other_index_sizes;

#if !defined NO_ELISION && !defined SUX_LOCK_GENERIC
		if (xbegin()) {
			if (ib_table->stats_mutex_is_locked())
				xabort();

			ut_ad(ib_table->stat_initialized());

			n_rows = ib_table->stat_n_rows;

			stat_clustered_index_size
				= ib_table->stat_clustered_index_size;

			stat_sum_of_other_index_sizes
				= ib_table->stat_sum_of_other_index_sizes;

			xend();
		} else
#endif
		{
			ib_table->stats_shared_lock();

			ut_ad(ib_table->stat_initialized());

			n_rows = ib_table->stat_n_rows;

			stat_clustered_index_size
				= ib_table->stat_clustered_index_size;

			stat_sum_of_other_index_sizes
				= ib_table->stat_sum_of_other_index_sizes;

			ib_table->stats_shared_unlock();
		}

		/*
		The MySQL optimizer seems to assume in a left join that n_rows
		is an accurate estimate if it is zero. Of course, it is not,
		since we do not have any locks on the rows yet at this phase.
		Since SHOW TABLE STATUS seems to call this function with the
		HA_STATUS_TIME flag set, while the left join optimizer does not
		set that flag, we add one to a zero value if the flag is not
		set. That way SHOW TABLE STATUS will show the best estimate,
		while the optimizer never sees the table empty. */

		if (n_rows == 0 && !(flag & (HA_STATUS_TIME | HA_STATUS_OPEN))) {
			n_rows++;
		}

		/* Fix bug#40386: Not flushing query cache after truncate.
		n_rows can not be 0 unless the table is empty, set to 1
		instead. The original problem of bug#29507 is actually
		fixed in the server code. */
		if (thd_sql_command(m_user_thd) == SQLCOM_TRUNCATE) {

			n_rows = 1;

			/* We need to reset the m_prebuilt value too, otherwise
			checks for values greater than the last value written
			to the table will fail and the autoinc counter will
			not be updated. This will force write_row() into
			attempting an update of the table's AUTOINC counter. */

			m_prebuilt->autoinc_last_value = 0;
		}

		stats.records = (ha_rows) n_rows;
		stats.deleted = 0;
		if (fil_space_t* space = ib_table->space) {
			const ulint size = space->physical_size();
			stats.data_file_length
				= ulonglong(stat_clustered_index_size)
				* size;
			stats.index_file_length
				= ulonglong(stat_sum_of_other_index_sizes)
				* size;
			if (flag & HA_STATUS_VARIABLE_EXTRA) {
				space->s_lock();
				stats.delete_length = 1024
					* fsp_get_available_space_in_free_extents(
					*space);
				space->s_unlock();
			}
		}
		stats.check_time = 0;
		stats.mrr_length_per_rec= (uint)ref_length +  8; // 8 = max(sizeof(void *));

		if (stats.records == 0) {
			stats.mean_rec_length = 0;
		} else {
			stats.mean_rec_length = (ulong)
				(stats.data_file_length / stats.records);
		}
	}

	if (flag & HA_STATUS_CONST) {
		/* Verify the number of index in InnoDB and MySQL
		matches up. If m_prebuilt->clust_index_was_generated
		holds, InnoDB defines GEN_CLUST_INDEX internally */
		ulint	num_innodb_index = UT_LIST_GET_LEN(ib_table->indexes)
			- m_prebuilt->clust_index_was_generated;
		if (table->s->keys < num_innodb_index) {
			/* If there are too many indexes defined
			inside InnoDB, ignore those that are being
			created, because MySQL will only consider
			the fully built indexes here. */

			for (const dict_index_t* index
				     = UT_LIST_GET_FIRST(ib_table->indexes);
			     index != NULL;
			     index = UT_LIST_GET_NEXT(indexes, index)) {

				/* First, online index creation is
				completed inside InnoDB, and then
				MySQL attempts to upgrade the
				meta-data lock so that it can rebuild
				the .frm file. If we get here in that
				time frame, dict_index_is_online_ddl()
				would not hold and the index would
				still not be included in TABLE_SHARE. */
				if (!index->is_committed()) {
					num_innodb_index--;
				}
			}

			if (table->s->keys < num_innodb_index
			    && innobase_fts_check_doc_id_index(
				    ib_table, NULL, NULL)
			    == FTS_EXIST_DOC_ID_INDEX) {
				num_innodb_index--;
			}
		}

		if (table->s->keys != num_innodb_index) {
			ib_table->dict_frm_mismatch = DICT_FRM_INCONSISTENT_KEYS;
			ib_push_frm_error(m_user_thd, ib_table, table, num_innodb_index, true);
		}

		snprintf(path, sizeof(path), "%s/%s%s",
			 mysql_data_home, table->s->normalized_path.str,
			 reg_ext);

		unpack_filename(path,path);

		/* Note that we do not know the access time of the table,
		nor the CHECK TABLE time, nor the UPDATE or INSERT time. */

		if (os_file_get_status(
			    path, &stat_info, false,
			    srv_read_only_mode) == DB_SUCCESS) {
			stats.create_time = (ulong) stat_info.ctime;
		}

		ib_table->stats_shared_lock();
		auto _ = make_scope_exit([ib_table]() {
			ib_table->stats_shared_unlock(); });

		ut_ad(ib_table->stat_initialized());

		for (uint i = 0; i < table->s->keys; i++) {
			ulong	j;

			dict_index_t* index = innobase_get_index(i);

			if (index == NULL) {
				ib_table->dict_frm_mismatch = DICT_FRM_INCONSISTENT_KEYS;
				ib_push_frm_error(m_user_thd, ib_table, table, num_innodb_index, true);
				break;
			}

			KEY*	key = &table->key_info[i];

			for (j = 0; j < key->ext_key_parts; j++) {

				if ((key->algorithm == HA_KEY_ALG_FULLTEXT)
				    || (key->algorithm == HA_KEY_ALG_RTREE)) {

					/* The record per key does not apply to
					FTS or Spatial indexes. */
				/*
					key->rec_per_key[j] = 1;
					key->set_records_per_key(j, 1.0);
				*/
					continue;
				}

				if (j + 1 > index->n_uniq) {
					sql_print_error(
						"Index %s of %s has %u columns"
					        " unique inside InnoDB, but "
						"server is asking statistics for"
					        " %lu columns. Have you mixed "
						"up .frm files from different "
						" installations? %s",
						index->name(),
						ib_table->name.m_name,
						index->n_uniq, j + 1,
						TROUBLESHOOTING_MSG);
					break;
				}

				/* innodb_rec_per_key() will use
				index->stat_n_diff_key_vals[] and the value we
				pass index->table->stat_n_rows. Both are
				calculated by ANALYZE and by the background
				stats gathering thread (which kicks in when too
				much of the table has been changed). In
				addition table->stat_n_rows is adjusted with
				each DML (e.g. ++ on row insert). Those
				adjustments are not MVCC'ed and not even
				reversed on rollback. So,
				index->stat_n_diff_key_vals[] and
				index->table->stat_n_rows could have been
				calculated at different time. This is
				acceptable. */

				ulong	rec_per_key_int = static_cast<ulong>(
					innodb_rec_per_key(index, j,
							   stats.records));

				if (rec_per_key_int == 0) {
					rec_per_key_int = 1;
				}

				key->rec_per_key[j] = rec_per_key_int;
			}
		}
	}

	if (srv_force_recovery >= SRV_FORCE_NO_UNDO_LOG_SCAN) {

		goto func_exit;

	} else if (flag & HA_STATUS_ERRKEY) {
		const dict_index_t*	err_index;

		ut_a(m_prebuilt->trx);
		ut_a(m_prebuilt->trx->magic_n == TRX_MAGIC_N);

		err_index = trx_get_error_info(m_prebuilt->trx);

		if (err_index) {
			errkey = innobase_get_mysql_key_number_for_index(
					table, ib_table, err_index);
		} else {
			errkey = (unsigned int) (
				(m_prebuilt->trx->error_key_num
				 == ULINT_UNDEFINED)
					? ~0U
					: m_prebuilt->trx->error_key_num);
		}
	}

	if ((flag & HA_STATUS_AUTO) && table->found_next_number_field) {
		stats.auto_increment_value = innobase_peek_autoinc();
	}

func_exit:
	m_prebuilt->trx->op_info = (char*)"";

	DBUG_RETURN(0);
}

/*********************************************************************//**
Returns statistics information of the table to the MySQL interpreter,
in various fields of the handle object.
@return HA_ERR_* error code or 0 */

int
ha_innobase::info(
/*==============*/
	uint	flag)	/*!< in: what information is requested */
{
	return(info_low(flag, false /* not ANALYZE */));
}

/*
Updates index cardinalities of the table, based on random dives into
each index tree. This does NOT calculate exact statistics on the table.
@return HA_ADMIN_* error code or HA_ADMIN_OK */

int
ha_innobase::analyze(THD*, HA_CHECK_OPT*)
{
	/* Simply call info_low() with all the flags
	and request recalculation of the statistics */
	int	ret = info_low(
		HA_STATUS_TIME | HA_STATUS_CONST | HA_STATUS_VARIABLE,
		true /* this is ANALYZE */);

	if (ret != 0) {
		return(HA_ADMIN_FAILED);
	}

	return(HA_ADMIN_OK);
}

/**********************************************************************//**
This is mapped to "ALTER TABLE tablename ENGINE=InnoDB", which rebuilds
the table in MySQL. */

int
ha_innobase::optimize(
/*==================*/
	THD*		thd,		/*!< in: connection thread handle */
	HA_CHECK_OPT*)
{

	/* FTS-FIXME: Since MySQL doesn't support engine-specific commands,
	we have to hijack some existing command in order to be able to test
	the new admin commands added in InnoDB's FTS support. For now, we
	use MySQL's OPTIMIZE command, normally mapped to ALTER TABLE in
	InnoDB (so it recreates the table anew), and map it to OPTIMIZE.

	This works OK otherwise, but MySQL locks the entire table during
	calls to OPTIMIZE, which is undesirable. */
	bool try_alter = true;

	if (innodb_optimize_fulltext_only) {
		if (m_prebuilt->table->fts && m_prebuilt->table->fts->cache
		    && m_prebuilt->table->space) {
			fts_sync_table(m_prebuilt->table);
			fts_optimize_table(m_prebuilt->table);
		}
		try_alter = false;
	}

	return try_alter ? HA_ADMIN_TRY_ALTER : HA_ADMIN_OK;
}

/** Does the following validation check for the sequence table
1) Check whether the InnoDB table has no_rollback flags
2) Should have only one primary index
3) Root index page must be leaf page
4) There should be only one record in leaf page
5) There shouldn't be delete marked record in leaf page
6) DB_TRX_ID, DB_ROLL_PTR in the record should be 0 and 1U << 55
@param thd   Thread
@param table InnoDB table
@retval true if validation succeeds or false if validation fails */
static bool innobase_sequence_table_check(THD *thd, dict_table_t *table)
{
  fil_space_t *space= table->space;
  dict_index_t *clust_index= dict_table_get_first_index(table);
  mtr_t mtr;
  bool corruption= false;
  const rec_t *rec= nullptr;
  buf_block_t *root_block= nullptr;

  if (UT_LIST_GET_LEN(table->indexes) != 1)
  {
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN, ER_NOT_KEYFILE,
			"InnoDB: Sequence table %s does have more than one "
                        "indexes.", table->name.m_name);
    corruption= true;
    goto func_exit;
  }

  if (!clust_index->is_gen_clust())
  {
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN, ER_NOT_KEYFILE,
			"InnoDB: Sequence table %s does not have generated "
                        "clustered index.", table->name.m_name);
    corruption= true;
    goto func_exit;
  }

  mtr.start();
  mtr.set_named_space(space);
  root_block= buf_page_get_gen(page_id_t(space->id, clust_index->page),
                               space->zip_size(), RW_S_LATCH, nullptr, BUF_GET,
                               &mtr);
  DBUG_EXECUTE_IF("fail_root_page", root_block= nullptr;);
  if (!root_block)
  {
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN, ER_NOT_KEYFILE,
			"InnoDB: Sequence table %s is corrupted.",
                        table->name.m_name);
    corruption= true;
    goto err_exit;
  }

  if (!page_is_leaf(root_block->page.frame))
  {
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN, ER_NOT_KEYFILE,
			"InnoDB: Non leaf page exists for sequence table %s.",
                        table->name.m_name);
    corruption= true;
    goto err_exit;
  }

  if (page_get_n_recs(root_block->page.frame) != 1)
  {
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN, ER_NOT_KEYFILE,
			"InnoDB: Should have only one record in sequence "
                        "table %s. But it has %u records.", table->name.m_name,
                        page_get_n_recs(root_block->page.frame));
    corruption= true;
    goto err_exit;
  }

  rec= page_rec_get_next(page_get_infimum_rec(root_block->page.frame));

  if (rec_get_deleted_flag(rec, dict_table_is_comp(table)))
  {
    corruption= true;
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN, ER_NOT_KEYFILE,
			"InnoDB: Encountered delete marked record in sequence "
                        "table %s.", table->name.m_name);
     goto err_exit;
  }

  if (trx_read_trx_id(rec + clust_index->trx_id_offset) != 0 ||
      trx_read_roll_ptr(rec + clust_index->trx_id_offset + DATA_TRX_ID_LEN) !=
        roll_ptr_t{1} << ROLL_PTR_INSERT_FLAG_POS)
  {
    corruption= true;
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN, ER_NOT_KEYFILE,
			"InnoDB: Record in sequence table %s is corrupted.",
                        table->name.m_name);
    goto err_exit;
  }

  if (!table->no_rollback())
  {
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN, ER_NOT_KEYFILE,
			"InnoDB: Sequence table %s has ROLLBACK enabled.",
                        table->name.m_name);
    corruption= true;
  }
err_exit:
  mtr.commit();
func_exit:
  if (corruption)
  {
    dict_set_corrupted(clust_index, "Table corruption");
    return false;
  }
  return true;
}

/*******************************************************************//**
Tries to check that an InnoDB table is not corrupted. If corruption is
noticed, prints to stderr information about it. In case of corruption
may also assert a failure and crash the server.
@return HA_ADMIN_CORRUPT or HA_ADMIN_OK */

int
ha_innobase::check(
/*===============*/
	THD*		thd,		/*!< in: user thread handle */
	HA_CHECK_OPT*	check_opt)	/*!< in: check options */
{
	ulint		n_rows;
	ulint		n_rows_in_table	= ULINT_UNDEFINED;
	bool		is_ok		= true;
	dberr_t		ret;
        uint handler_flags= check_opt->handler_flags;

	DBUG_ENTER("ha_innobase::check");
	DBUG_ASSERT(thd == ha_thd());
	DBUG_ASSERT(thd == m_user_thd);
	ut_a(m_prebuilt->trx->magic_n == TRX_MAGIC_N);
	ut_a(m_prebuilt->trx == thd_to_trx(thd));
	ut_ad(m_prebuilt->trx->mysql_thd == thd);

	if (handler_flags || check_for_upgrade(check_opt)) {
		/* The file was already checked and fixed as part of open */
		print_check_msg(thd, table->s->db.str, table->s->table_name.str,
				"check", "note",
				(opt_readonly || high_level_read_only
				 || !(check_opt->sql_flags & TT_FOR_UPGRADE))
				? "Auto_increment will be"
				" checked on each open until"
				" CHECK TABLE FOR UPGRADE is executed"
				: "Auto_increment checked and"
				" .frm file version updated", 1);
		if (handler_flags && (check_opt->sql_flags & TT_FOR_UPGRADE)) {
			/*
			  No other issues found (as handler_flags was only
			  set if there as not other problems with the table
			  than auto_increment).
			*/
			DBUG_RETURN(HA_ADMIN_OK);
		}
	}

	if (m_prebuilt->mysql_template == NULL) {
		/* Build the template; we will use a dummy template
		in index scans done in checking */

		build_template(true);
	}

	if (!m_prebuilt->table->space) {
		ib_senderrf(
			thd,
			IB_LOG_LEVEL_ERROR,
			ER_TABLESPACE_DISCARDED,
			table->s->table_name.str);

		DBUG_RETURN(HA_ADMIN_CORRUPT);
	} else if (!m_prebuilt->table->is_readable()) {
		ib_senderrf(
			thd, IB_LOG_LEVEL_ERROR,
			ER_TABLESPACE_MISSING,
			table->s->table_name.str);

		DBUG_RETURN(HA_ADMIN_CORRUPT);
	} else if (table->s->table_type == TABLE_TYPE_SEQUENCE) {
		DBUG_RETURN(
		  innobase_sequence_table_check(thd, m_prebuilt->table)
		  ? HA_ADMIN_OK
		  : HA_ADMIN_CORRUPT);
	}

	m_prebuilt->trx->op_info = "checking table";

	uint old_isolation_level = m_prebuilt->trx->isolation_level;

	/* We must run the index record counts at an isolation level
	>= READ COMMITTED, because a dirty read can see a wrong number
	of records in some index; to play safe, we normally use
	REPEATABLE READ here */
	m_prebuilt->trx->isolation_level = high_level_read_only
		&& !m_prebuilt->table->is_temporary()
		? TRX_ISO_READ_UNCOMMITTED
		: TRX_ISO_REPEATABLE_READ;

	trx_start_if_not_started(m_prebuilt->trx, false);
	m_prebuilt->trx->read_view.open(m_prebuilt->trx);

	for (dict_index_t* index
	     = dict_table_get_first_index(m_prebuilt->table);
	     index;
	     index = dict_table_get_next_index(index)) {
		/* If this is an index being created or dropped, skip */
		if (!index->is_committed()) {
			continue;
		}
		if (index->type & DICT_FTS) {
			/* We do not check any FULLTEXT INDEX. */
			continue;
		}

		if ((check_opt->flags & T_QUICK) || index->is_corrupted()) {
		} else if (trx_id_t bulk_trx_id =
				m_prebuilt->table->bulk_trx_id) {
			if (!m_prebuilt->trx->read_view.changes_visible(
							bulk_trx_id)) {
				is_ok = true;
				goto func_exit;
			}

			if (btr_validate_index(index, m_prebuilt->trx)
			    != DB_SUCCESS) {
				is_ok = false;
				push_warning_printf(
					thd,
					Sql_condition::WARN_LEVEL_WARN,
					ER_NOT_KEYFILE,
					"InnoDB: The B-tree of"
					" index %s is corrupted.",
					index->name());
				continue;
			}
		}

		/* Instead of invoking change_active_index(), set up
		a dummy template for non-locking reads, disabling
		access to the clustered index. */
		m_prebuilt->index = index;

		m_prebuilt->index_usable = row_merge_is_index_usable(
			m_prebuilt->trx, m_prebuilt->index);

		DBUG_EXECUTE_IF(
			"dict_set_index_corrupted",
			if (!index->is_primary()) {
				m_prebuilt->index_usable = FALSE;
				dict_set_corrupted(index,
						   "dict_set_index_corrupted");
			});

		if (UNIV_UNLIKELY(!m_prebuilt->index_usable)) {
			if (index->is_corrupted()) {
				push_warning_printf(
					thd,
					Sql_condition::WARN_LEVEL_WARN,
					HA_ERR_INDEX_CORRUPT,
					"InnoDB: Index %s is marked as"
					" corrupted",
					index->name());
				is_ok = false;
			} else {
				push_warning_printf(
					thd,
					Sql_condition::WARN_LEVEL_WARN,
					HA_ERR_TABLE_DEF_CHANGED,
					"InnoDB: Insufficient history for"
					" index %s",
					index->name());
			}
			continue;
		}

		m_prebuilt->sql_stat_start = TRUE;
		m_prebuilt->template_type = ROW_MYSQL_DUMMY_TEMPLATE;
		m_prebuilt->n_template = 0;
		m_prebuilt->read_just_key = 0;
		m_prebuilt->autoinc_error = DB_SUCCESS;
		m_prebuilt->need_to_access_clustered =
			!!(check_opt->flags & T_EXTEND);

		dtuple_set_n_fields(m_prebuilt->search_tuple, 0);

		m_prebuilt->select_lock_type = LOCK_NONE;

		/* Scan this index. */
		if (index->is_spatial()) {
			ret = row_count_rtree_recs(m_prebuilt, &n_rows);
		} else if (index->type & DICT_FTS) {
			ret = DB_SUCCESS;
		} else {
			ret = row_check_index(m_prebuilt, &n_rows);
		}

		DBUG_EXECUTE_IF(
			"dict_set_index_corrupted",
			if (!index->is_primary()) {
				ret = DB_CORRUPTION;
			});

		if (ret == DB_INTERRUPTED || thd_killed(thd)) {
			/* Do not report error since this could happen
			during shutdown */
			break;
		}

		if (ret == DB_SUCCESS
		    && m_prebuilt->autoinc_error != DB_MISSING_HISTORY) {
			/* See if any non-fatal errors were reported. */
			ret = m_prebuilt->autoinc_error;
		}

		if (ret != DB_SUCCESS) {
			/* Assume some kind of corruption. */
			push_warning_printf(
				thd, Sql_condition::WARN_LEVEL_WARN,
				ER_NOT_KEYFILE,
				"InnoDB: The B-tree of"
				" index %s is corrupted.",
				index->name());
			is_ok = false;
			dict_set_corrupted(index, "CHECK TABLE-check index");
		}


		if (index == dict_table_get_first_index(m_prebuilt->table)) {
			n_rows_in_table = n_rows;
		} else if (!(index->type & DICT_FTS)
			   && (n_rows != n_rows_in_table)) {
			push_warning_printf(
				thd, Sql_condition::WARN_LEVEL_WARN,
				ER_NOT_KEYFILE,
				"InnoDB: Index '%-.200s' contains " ULINTPF
				" entries, should be " ULINTPF ".",
				index->name(), n_rows, n_rows_in_table);
			is_ok = false;
			dict_set_corrupted(index, "CHECK TABLE; Wrong count");
		}
	}

	/* Restore the original isolation level */
	m_prebuilt->trx->isolation_level = old_isolation_level & 3;
#ifdef BTR_CUR_HASH_ADAPT
# if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
	/* We validate the whole adaptive hash index for all tables
	at every CHECK TABLE only when QUICK flag is not present. */

	if (!(check_opt->flags & T_QUICK)
	    && !btr_search_validate(m_prebuilt->trx->mysql_thd)) {
		push_warning(thd, Sql_condition::WARN_LEVEL_WARN,
			     ER_NOT_KEYFILE,
			     "InnoDB: The adaptive hash index is corrupted.");
		is_ok = false;
	}
# endif /* defined UNIV_AHI_DEBUG || defined UNIV_DEBUG */
#endif /* BTR_CUR_HASH_ADAPT */
func_exit:
	m_prebuilt->trx->op_info = "";

	DBUG_RETURN(is_ok ? HA_ADMIN_OK : HA_ADMIN_CORRUPT);
}

/**
Check if we there is a problem with the InnoDB table.
@param check_opt     check options
@retval HA_ADMIN_OK           if Table is ok
@retval HA_ADMIN_NEEDS_ALTER  User should run ALTER TABLE FOR UPGRADE
@retval HA_ADMIN_NEEDS_CHECK  User should run CHECK TABLE FOR UPGRADE
@retval HA_ADMIN_FAILED       if InnoDB is in read-only mode */
int ha_innobase::check_for_upgrade(HA_CHECK_OPT *check_opt)
{
  /*
    Check if there is a possibility that the auto increment value
    stored in PAGE_ROOT_AUTO_INC could be corrupt.
  */
  if (table->s->mysql_version >= 100210);
  else if (const Field *auto_increment= table->found_next_number_field)
  {
    uint col_no= innodb_col_no(auto_increment);
    const dict_col_t *autoinc_col=
      dict_table_get_nth_col(m_prebuilt->table, col_no);
    if (m_prebuilt->table->get_index(*autoinc_col))
    {
      check_opt->handler_flags= 1;
      return (high_level_read_only && !opt_readonly)
        ? HA_ADMIN_FAILED : HA_ADMIN_NEEDS_CHECK;
    }
  }
  return HA_ADMIN_OK;
}

/*******************************************************************//**
Gets the foreign key create info for a table stored in InnoDB.
@return own: character string in the form which can be inserted to the
CREATE TABLE statement, MUST be freed with
ha_innobase::free_foreign_key_create_info */

char*
ha_innobase::get_foreign_key_create_info(void)
/*==========================================*/
{
	ut_a(m_prebuilt != NULL);

	/* We do not know if MySQL can call this function before calling
	external_lock(). To be safe, update the thd of the current table
	handle. */

	update_thd(ha_thd());

	m_prebuilt->trx->op_info = "getting info on foreign keys";

	/* Output the data to a temporary string */
	std::string str = dict_print_info_on_foreign_keys(
		TRUE, m_prebuilt->trx,
		m_prebuilt->table);

	m_prebuilt->trx->op_info = "";

	/* Allocate buffer for the string */
	char *fk_str = reinterpret_cast<char*>(
			my_malloc(PSI_INSTRUMENT_ME, str.length() + 1, MYF(0)));

	if (fk_str) {
		memcpy(fk_str, str.c_str(), str.length());
		fk_str[str.length()]='\0';
	}

	return(fk_str);
}


/***********************************************************************//**
Maps a InnoDB foreign key constraint to a equivalent MySQL foreign key info.
@return pointer to foreign key info */
static
FOREIGN_KEY_INFO*
get_foreign_key_info(
/*=================*/
	THD*		thd,	/*!< in: user thread handle */
	dict_foreign_t* foreign)/*!< in: foreign key constraint */
{
	FOREIGN_KEY_INFO	f_key_info;
	FOREIGN_KEY_INFO*	pf_key_info;
	uint			i = 0;
	size_t			len;
	char			tmp_buff[NAME_LEN+1];
	char			name_buff[NAME_LEN+1];
	const char*		ptr = foreign->sql_id();
	LEX_CSTRING*		name = NULL;

	if (dict_table_t::is_temporary_name(foreign->foreign_table_name)) {
		return NULL;
	}

	f_key_info.foreign_id = thd_make_lex_string(
		thd, 0, ptr, strlen(ptr), 1);

	/* Name format: database name, '/', table name, '\0' */

	/* Referenced (parent) database name */
	len = dict_get_db_name_len(foreign->referenced_table_name);
	ut_a(len < sizeof(tmp_buff));
	memcpy(tmp_buff, foreign->referenced_table_name, len);
	tmp_buff[len] = 0;

	len = filename_to_tablename(tmp_buff, name_buff, sizeof(name_buff));
	f_key_info.referenced_db = thd_make_lex_string(
		thd, 0, name_buff, len, 1);

	/* Referenced (parent) table name */
	ptr = dict_remove_db_name(foreign->referenced_table_name);
	len = filename_to_tablename(ptr, name_buff, sizeof(name_buff), 1);
	f_key_info.referenced_table = thd_make_lex_string(
		thd, 0, name_buff, len, 1);

	/* Dependent (child) database name */
	len = dict_get_db_name_len(foreign->foreign_table_name);
	ut_a(len < sizeof(tmp_buff));
	memcpy(tmp_buff, foreign->foreign_table_name, len);
	tmp_buff[len] = 0;

	len = filename_to_tablename(tmp_buff, name_buff, sizeof(name_buff));
	f_key_info.foreign_db = thd_make_lex_string(
		thd, 0, name_buff, len, 1);

	/* Dependent (child) table name */
	ptr = dict_remove_db_name(foreign->foreign_table_name);
	len = filename_to_tablename(ptr, name_buff, sizeof(name_buff), 1);
	f_key_info.foreign_table = thd_make_lex_string(
		thd, 0, name_buff, len, 1);

	do {
		ptr = foreign->foreign_col_names[i];
		name = thd_make_lex_string(thd, name, ptr,
					   strlen(ptr), 1);
		f_key_info.foreign_fields.push_back(name);

		if (dict_index_t* fidx = foreign->foreign_index) {
			if (fidx->fields[i].col->is_nullable()) {
				f_key_info.set_nullable(thd, false, i,
							foreign->n_fields);
			}
		}
		ptr = foreign->referenced_col_names[i];
		name = thd_make_lex_string(thd, name, ptr,
					   strlen(ptr), 1);
		f_key_info.referenced_fields.push_back(name);

		if (dict_index_t* ref_idx = foreign->referenced_index) {
			if (ref_idx->fields[i].col->is_nullable()) {
				f_key_info.set_nullable(thd, true, i,
							foreign->n_fields);
			}
		}

	} while (++i < foreign->n_fields);

	if (foreign->type & foreign->DELETE_CASCADE) {
		f_key_info.delete_method = FK_OPTION_CASCADE;
	} else if (foreign->type & foreign->DELETE_SET_NULL) {
		f_key_info.delete_method = FK_OPTION_SET_NULL;
	} else if (foreign->type & foreign->DELETE_NO_ACTION) {
		f_key_info.delete_method = FK_OPTION_NO_ACTION;
	} else {
		f_key_info.delete_method = FK_OPTION_RESTRICT;
	}


	if (foreign->type & foreign->UPDATE_CASCADE) {
		f_key_info.update_method = FK_OPTION_CASCADE;
	} else if (foreign->type & foreign->UPDATE_SET_NULL) {
		f_key_info.update_method = FK_OPTION_SET_NULL;
	} else if (foreign->type & foreign->UPDATE_NO_ACTION) {
		f_key_info.update_method = FK_OPTION_NO_ACTION;
	} else {
		f_key_info.update_method = FK_OPTION_RESTRICT;
	}

	/* Load referenced table to update FK referenced key name. */
	if (foreign->referenced_table == NULL) {

		dict_table_t*	ref_table = dict_table_open_on_name(
			foreign->referenced_table_name_lookup,
			true, DICT_ERR_IGNORE_NONE);

		if (ref_table == NULL) {

			if (!thd_test_options(
				thd, OPTION_NO_FOREIGN_KEY_CHECKS)) {
				ib::info()
					<< "Foreign Key referenced table "
					<< foreign->referenced_table_name
					<< " not found for foreign table "
					<< foreign->foreign_table_name;
 			}
		} else {
			ref_table->release();
		}
	}

	if (foreign->referenced_index
	    && foreign->referenced_index->name != NULL) {
		f_key_info.referenced_key_name = thd_make_lex_string(
			thd,
			nullptr,
			foreign->referenced_index->name,
			strlen(foreign->referenced_index->name),
			1);
	} else {
		f_key_info.referenced_key_name = NULL;
	}

	pf_key_info = (FOREIGN_KEY_INFO*) thd_memdup(thd, &f_key_info,
						      sizeof(FOREIGN_KEY_INFO));

	return(pf_key_info);
}

/*******************************************************************//**
Gets the list of foreign keys in this table.
@return always 0, that is, always succeeds */

int
ha_innobase::get_foreign_key_list(
/*==============================*/
	THD*			thd,		/*!< in: user thread handle */
	List<FOREIGN_KEY_INFO>*	f_key_list)	/*!< out: foreign key list */
{
	update_thd(ha_thd());

	m_prebuilt->trx->op_info = "getting list of foreign keys";

	dict_sys.lock(SRW_LOCK_CALL);

	for (dict_foreign_set::iterator it
		= m_prebuilt->table->foreign_set.begin();
	     it != m_prebuilt->table->foreign_set.end();
	     ++it) {

		FOREIGN_KEY_INFO*	pf_key_info;
		dict_foreign_t*		foreign = *it;

		pf_key_info = get_foreign_key_info(thd, foreign);

		if (pf_key_info != NULL) {
			f_key_list->push_back(pf_key_info);
		}
	}

	dict_sys.unlock();

	m_prebuilt->trx->op_info = "";

	return(0);
}

/*******************************************************************//**
Gets the set of foreign keys where this table is the referenced table.
@return always 0, that is, always succeeds */

int
ha_innobase::get_parent_foreign_key_list(
/*=====================================*/
	THD*			thd,		/*!< in: user thread handle */
	List<FOREIGN_KEY_INFO>*	f_key_list)	/*!< out: foreign key list */
{
	update_thd(ha_thd());

	m_prebuilt->trx->op_info = "getting list of referencing foreign keys";

	dict_sys.freeze(SRW_LOCK_CALL);

	for (dict_foreign_set::iterator it
		= m_prebuilt->table->referenced_set.begin();
	     it != m_prebuilt->table->referenced_set.end();
	     ++it) {

		FOREIGN_KEY_INFO*	pf_key_info;
		dict_foreign_t*		foreign = *it;

		pf_key_info = get_foreign_key_info(thd, foreign);

		if (pf_key_info != NULL) {
			f_key_list->push_back(pf_key_info);
		}
	}

	dict_sys.unfreeze();

	m_prebuilt->trx->op_info = "";

	return(0);
}

/** Table list item structure is used to store only the table
and name. It is used by get_cascade_foreign_key_table_list to store
the intermediate result for fetching the table set. */
struct table_list_item {
	/** InnoDB table object */
	const dict_table_t*	table;
	/** Table name */
	const char*		name;
};

/** @return whether ALTER TABLE may change the storage engine of the table */
bool ha_innobase::can_switch_engines()
{
  DBUG_ENTER("ha_innobase::can_switch_engines");
  update_thd();
  DBUG_RETURN(m_prebuilt->table->foreign_set.empty() &&
              m_prebuilt->table->referenced_set.empty());
}

/** Checks if a table is referenced by a foreign key. The MySQL manual states
that a REPLACE is either equivalent to an INSERT, or DELETE(s) + INSERT. Only a
delete is then allowed internally to resolve a duplicate key conflict in
REPLACE, not an update.
@return whether the table is referenced by a FOREIGN KEY */
bool ha_innobase::referenced_by_foreign_key() const noexcept
{
  dict_sys.freeze(SRW_LOCK_CALL);
  const bool empty= m_prebuilt->table->referenced_set.empty();
  dict_sys.unfreeze();
  return !empty;
}

/*******************************************************************//**
Tells something additional to the handler about how to do things.
@return 0 or error number */

int
ha_innobase::extra(
/*===============*/
	enum ha_extra_function operation)
			   /*!< in: HA_EXTRA_FLUSH or some other flag */
{
	/* Warning: since it is not sure that MariaDB calls external_lock()
	before calling this function, m_prebuilt->trx can be obsolete! */
	trx_t* trx;

	switch (operation) {
	case HA_EXTRA_FLUSH:
		(void)check_trx_exists(ha_thd());
		if (m_prebuilt->blob_heap) {
			row_mysql_prebuilt_free_blob_heap(m_prebuilt);
		}
		break;
	case HA_EXTRA_RESET_STATE:
		trx = check_trx_exists(ha_thd());
		reset_template();
		trx->duplicates = 0;
	stmt_boundary:
		trx->bulk_insert_apply();
		trx->end_bulk_insert(*m_prebuilt->table);
		trx->bulk_insert &= TRX_DDL_BULK;
		break;
	case HA_EXTRA_NO_KEYREAD:
		(void)check_trx_exists(ha_thd());
		m_prebuilt->read_just_key = 0;
		break;
	case HA_EXTRA_KEYREAD:
		(void)check_trx_exists(ha_thd());
		m_prebuilt->read_just_key = 1;
		break;
	case HA_EXTRA_KEYREAD_PRESERVE_FIELDS:
		(void)check_trx_exists(ha_thd());
		m_prebuilt->keep_other_fields_on_keyread = 1;
		break;
	case HA_EXTRA_INSERT_WITH_UPDATE:
		trx = check_trx_exists(ha_thd());
		trx->duplicates |= TRX_DUP_IGNORE;
		goto stmt_boundary;
	case HA_EXTRA_NO_IGNORE_DUP_KEY:
		trx = check_trx_exists(ha_thd());
		trx->duplicates &= ~TRX_DUP_IGNORE;
		if (trx->is_bulk_insert()) {
			/* Allow a subsequent INSERT into an empty table
			if !unique_checks && !foreign_key_checks. */
			if (dberr_t err = trx->bulk_insert_apply()) {
				return convert_error_code_to_mysql(
					 err, 0, trx->mysql_thd);
			}
			break;
		}
		goto stmt_boundary;
	case HA_EXTRA_WRITE_CAN_REPLACE:
		trx = check_trx_exists(ha_thd());
		trx->duplicates |= TRX_DUP_REPLACE;
		goto stmt_boundary;
	case HA_EXTRA_WRITE_CANNOT_REPLACE:
		trx = check_trx_exists(ha_thd());
		trx->duplicates &= ~TRX_DUP_REPLACE;
		if (trx->is_bulk_insert()) {
			/* Allow a subsequent INSERT into an empty table
			if !unique_checks && !foreign_key_checks. */
			break;
		}
		goto stmt_boundary;
	case HA_EXTRA_BEGIN_ALTER_COPY:
		trx = check_trx_exists(ha_thd());
		m_prebuilt->table->skip_alter_undo = 1;
		if (m_prebuilt->table->is_temporary()
		    || !m_prebuilt->table->versioned_by_id()) {
			break;
		}
		ut_ad(trx == m_prebuilt->trx);
		trx_start_if_not_started(trx, true);
		trx->mod_tables.emplace(
			const_cast<dict_table_t*>(m_prebuilt->table), 0)
			.first->second.set_versioned(0);
		break;
	case HA_EXTRA_END_ALTER_COPY:
		trx = check_trx_exists(ha_thd());
		if (!m_prebuilt->table->skip_alter_undo) {
			/* This could be invoked inside INSERT...SELECT.
			We do not want any extra log writes, because
			they could cause a severe performance regression. */
			break;
		}
		m_prebuilt->table->skip_alter_undo = 0;
		if (dberr_t err= trx->bulk_insert_apply<TRX_DDL_BULK>()) {
			trx->rollback();
			return convert_error_code_to_mysql(
				 err, m_prebuilt->table->flags,
				 trx->mysql_thd);
		}

		trx->end_bulk_insert(*m_prebuilt->table);
		trx->bulk_insert &= TRX_DML_BULK;
		if (!m_prebuilt->table->is_temporary()
		    && !high_level_read_only) {
			/* During copy_data_between_tables(), InnoDB only
			updates transient statistics. */
			if (!m_prebuilt->table->stats_is_persistent()) {
				dict_stats_update_if_needed(m_prebuilt->table,
							    *trx);
			}
			/* The extra log write is necessary for
			ALTER TABLE...ALGORITHM=COPY, because
			a normal transaction commit would be a no-op
			because no undo log records were generated.
			This log write will also be unnecessarily executed
			during CREATE...SELECT, which is the other caller of
			handler::extra(HA_EXTRA_BEGIN_ALTER_COPY). */
			log_buffer_flush_to_disk();
		}
		break;
	case HA_EXTRA_ABORT_ALTER_COPY:
		if (m_prebuilt->table->skip_alter_undo) {
			trx = check_trx_exists(ha_thd());
			m_prebuilt->table->skip_alter_undo = 0;
			trx->rollback();
		}
		break;
	default:/* Do nothing */
		;
	}

	return(0);
}

/**
MySQL calls this method at the end of each statement */
int
ha_innobase::reset()
{
	if (m_prebuilt->blob_heap) {
		row_mysql_prebuilt_free_blob_heap(m_prebuilt);
	}

	reset_template();

	m_ds_mrr.dsmrr_close();

	/* TODO: This should really be reset in reset_template() but for now
	it's safer to do it explicitly here. */

	/* This is a statement level counter. */
	m_prebuilt->autoinc_last_value = 0;

	m_prebuilt->skip_locked = false;
	return(0);
}

/******************************************************************//**
MySQL calls this function at the start of each SQL statement inside LOCK
TABLES. Inside LOCK TABLES the ::external_lock method does not work to
mark SQL statement borders. Note also a special case: if a temporary table
is created inside LOCK TABLES, MySQL has not called external_lock() at all
on that table.
MySQL-5.0 also calls this before each statement in an execution of a stored
procedure. To make the execution more deterministic for binlogging, MySQL-5.0
locks all tables involved in a stored procedure with full explicit table
locks (thd_in_lock_tables(thd) holds in store_lock()) before executing the
procedure.
@return 0 or error code */

int
ha_innobase::start_stmt(
/*====================*/
	THD*		thd,	/*!< in: handle to the user thread */
	thr_lock_type	lock_type)
{
	trx_t*		trx = m_prebuilt->trx;

	DBUG_ENTER("ha_innobase::start_stmt");

	update_thd(thd);

	ut_ad(m_prebuilt->table != NULL);

	trx = m_prebuilt->trx;

	switch (trx->state) {
	default:
		DBUG_RETURN(HA_ERR_ROLLBACK);
	case TRX_STATE_ACTIVE:
		break;
	case TRX_STATE_NOT_STARTED:
		trx->will_lock = true;
		break;
	}

	/* Reset the AUTOINC statement level counter for multi-row INSERTs. */
	trx->n_autoinc_rows = 0;

	const auto sql_command = thd_sql_command(thd);

	m_prebuilt->hint_need_to_fetch_extra_cols = 0;
	reset_template();

	switch (sql_command) {
	case SQLCOM_INSERT:
	case SQLCOM_INSERT_SELECT:
		if (trx->is_bulk_insert()) {
			/* Allow a subsequent INSERT into an empty table
			if !unique_checks && !foreign_key_checks. */
			ut_ad(!trx->duplicates);
			break;
		}
		/* fall through */
	default:
		trx->bulk_insert_apply();
		trx->end_bulk_insert();
		if (!trx->bulk_insert) {
			break;
		}

		ut_ad(trx->bulk_insert != TRX_DDL_BULK);
		trx->bulk_insert = TRX_NO_BULK;
		trx->last_stmt_start = trx->undo_no;
	}

	m_prebuilt->sql_stat_start = TRUE;

	if (m_prebuilt->table->is_temporary()
	    && m_mysql_has_locked
	    && m_prebuilt->select_lock_type == LOCK_NONE) {
		switch (sql_command) {
		case SQLCOM_INSERT:
		case SQLCOM_UPDATE:
		case SQLCOM_DELETE:
		case SQLCOM_REPLACE:
			init_table_handle_for_HANDLER();
			m_prebuilt->select_lock_type = LOCK_X;
			m_prebuilt->stored_select_lock_type = LOCK_X;
			if (dberr_t error = row_lock_table(m_prebuilt)) {
				DBUG_RETURN(convert_error_code_to_mysql(
						    error, 0, thd));
			}
			break;
		}
	}

	if (!m_mysql_has_locked) {
		/* This handle is for a temporary table created inside
		this same LOCK TABLES; since MySQL does NOT call external_lock
		in this case, we must use x-row locks inside InnoDB to be
		prepared for an update of a row */

		m_prebuilt->select_lock_type = LOCK_X;

	} else if (sql_command == SQLCOM_SELECT
		   && lock_type == TL_READ
		   && trx->isolation_level != TRX_ISO_SERIALIZABLE) {

		/* For other than temporary tables, we obtain
		no lock for consistent read (plain SELECT). */

		m_prebuilt->select_lock_type = LOCK_NONE;
	} else {
		/* Not a consistent read: restore the
		select_lock_type value. The value of
		stored_select_lock_type was decided in:
		1) ::store_lock(),
		2) ::external_lock(),
		3) ::init_table_handle_for_HANDLER(). */

		ut_a(m_prebuilt->stored_select_lock_type != LOCK_NONE_UNSET);

		m_prebuilt->select_lock_type =
			m_prebuilt->stored_select_lock_type;
	}

	*trx->detailed_error = 0;

	innobase_register_trx(ht, thd, trx);

	DBUG_RETURN(0);
}

/******************************************************************//**
Maps a MySQL trx isolation level code to the InnoDB isolation level code
@return InnoDB isolation level */
static inline
uint
innobase_map_isolation_level(
/*=========================*/
	enum_tx_isolation	iso)	/*!< in: MySQL isolation level code */
{
	if (UNIV_UNLIKELY(srv_force_recovery >= SRV_FORCE_NO_UNDO_LOG_SCAN)
	    || UNIV_UNLIKELY(srv_read_only_mode)) {
		return TRX_ISO_READ_UNCOMMITTED;
	}
	switch (iso) {
	case ISO_REPEATABLE_READ:	return(TRX_ISO_REPEATABLE_READ);
	case ISO_READ_COMMITTED:	return(TRX_ISO_READ_COMMITTED);
	case ISO_SERIALIZABLE:		return(TRX_ISO_SERIALIZABLE);
	case ISO_READ_UNCOMMITTED:	return(TRX_ISO_READ_UNCOMMITTED);
	}

	ut_error;

	return(0);
}

/******************************************************************//**
As MySQL will execute an external lock for every new table it uses when it
starts to process an SQL statement (an exception is when MySQL calls
start_stmt for the handle) we can use this function to store the pointer to
the THD in the handle. We will also use this function to communicate
to InnoDB that a new SQL statement has started and that we must store a
savepoint to our transaction handle, so that we are able to roll back
the SQL statement in case of an error.
@return 0 */

int
ha_innobase::external_lock(
/*=======================*/
	THD*	thd,		/*!< in: handle to the user thread */
	int	lock_type)	/*!< in: lock type */
{
	DBUG_ENTER("ha_innobase::external_lock");
	DBUG_PRINT("enter",("lock_type: %d", lock_type));

	update_thd(thd);
	trx_t* trx = m_prebuilt->trx;
	ut_ad(m_prebuilt->table);

	if (table->s->tmp_table == INTERNAL_TMP_TABLE)
		trx->check_unique_secondary = true;

	const bool not_autocommit = thd_test_options(thd, OPTION_NOT_AUTOCOMMIT
						     | OPTION_BEGIN);
	bool not_started = false;
	switch (trx->state) {
	default:
	case TRX_STATE_PREPARED:
		ut_ad("invalid state" == 0);
		DBUG_RETURN(HA_ERR_WRONG_COMMAND);
	case TRX_STATE_ABORTED:
		if (lock_type != F_UNLCK && not_autocommit) {
			DBUG_RETURN(HA_ERR_WRONG_COMMAND);
		}
		/* Reset the state if the transaction had been aborted. */
		trx->state = TRX_STATE_NOT_STARTED;
		/* fall through */
	case TRX_STATE_NOT_STARTED:
		not_started = true;
		break;
	case TRX_STATE_ACTIVE:
		break;
	}

	/* Statement based binlogging does not work in isolation level
	READ UNCOMMITTED and READ COMMITTED since the necessary
	locks cannot be taken. In this case, we print an
	informative error message and return with an error.
	Note: decide_logging_format would give the same error message,
	except it cannot give the extra details. */

	if (lock_type == F_WRLCK
	    && !(table_flags() & HA_BINLOG_STMT_CAPABLE)
	    && thd_binlog_format(thd) == BINLOG_FORMAT_STMT
	    && thd_binlog_filter_ok(thd)
	    && thd_sqlcom_can_generate_row_events(thd)) {
		bool skip = false;
#ifdef WITH_WSREP
		skip = trx->is_wsrep() && !wsrep_thd_is_local(thd);
#endif /* WITH_WSREP */
		/* used by test case */
		DBUG_EXECUTE_IF("no_innodb_binlog_errors", skip = true;);

		if (!skip) {
			my_error(ER_BINLOG_STMT_MODE_AND_ROW_ENGINE, MYF(0),
			         " InnoDB is limited to row-logging when"
			         " transaction isolation level is"
			         " READ COMMITTED or READ UNCOMMITTED.");

			DBUG_RETURN(HA_ERR_LOGGING_IMPOSSIBLE);
		}
	}

	const auto sql_command = thd_sql_command(thd);

	/* Check for UPDATEs in read-only mode. */
	if (srv_read_only_mode) {
		switch (sql_command) {
		case SQLCOM_CREATE_TABLE:
			if (lock_type != F_WRLCK) {
				break;
			}
			/* fall through */
		case SQLCOM_UPDATE:
		case SQLCOM_INSERT:
		case SQLCOM_REPLACE:
		case SQLCOM_DROP_TABLE:
		case SQLCOM_ALTER_TABLE:
		case SQLCOM_OPTIMIZE:
		case SQLCOM_CREATE_INDEX:
		case SQLCOM_DROP_INDEX:
		case SQLCOM_CREATE_SEQUENCE:
		case SQLCOM_DROP_SEQUENCE:
		case SQLCOM_DELETE:
			ib_senderrf(thd, IB_LOG_LEVEL_WARN,
				    ER_READ_ONLY_MODE);
			DBUG_RETURN(HA_ERR_TABLE_READONLY);
		}
	}

	m_prebuilt->sql_stat_start = TRUE;
	m_prebuilt->hint_need_to_fetch_extra_cols = 0;

	reset_template();
	switch (sql_command) {
	case SQLCOM_INSERT:
	case SQLCOM_INSERT_SELECT:
		if (trx->is_bulk_insert()) {
			/* Allow a subsequent INSERT into an empty table
			if !unique_checks && !foreign_key_checks. */
			ut_ad(!trx->duplicates);
			break;
		}
		/* fall through */
	default:
		trx->bulk_insert_apply();
		trx->end_bulk_insert();
		if (!trx->bulk_insert) {
			break;
		}
		trx->bulk_insert &= TRX_DDL_BULK;
		trx->last_stmt_start = trx->undo_no;
	}

	switch (m_prebuilt->table->quiesce) {
	case QUIESCE_START:
		/* Check for FLUSH TABLE t WITH READ LOCK; */
		if (!srv_read_only_mode
		    && sql_command == SQLCOM_FLUSH
		    && lock_type == F_RDLCK) {

			if (!m_prebuilt->table->space) {
				ib_senderrf(trx->mysql_thd, IB_LOG_LEVEL_ERROR,
					    ER_TABLESPACE_DISCARDED,
					    table->s->table_name.str);

				DBUG_RETURN(HA_ERR_TABLESPACE_MISSING);
			}

			row_quiesce_table_start(m_prebuilt->table, trx);

			/* Use the transaction instance to track UNLOCK
			TABLES. It can be done via START TRANSACTION; too
			implicitly. */

			++trx->flush_tables;
		}
		break;

	case QUIESCE_COMPLETE:
		/* Check for UNLOCK TABLES; implicit or explicit
		or trx interruption. */
		if (trx->flush_tables > 0
		    && (lock_type == F_UNLCK || trx_is_interrupted(trx))) {

			row_quiesce_table_complete(m_prebuilt->table, trx);

			ut_a(trx->flush_tables > 0);
			--trx->flush_tables;
		}

		break;

	case QUIESCE_NONE:
		break;
	}

	switch (lock_type) {
	case F_UNLCK:
		DEBUG_SYNC_C("ha_innobase_end_statement");
		m_mysql_has_locked = false;
		ut_a(trx->n_mysql_tables_in_use);

		if (--trx->n_mysql_tables_in_use) {
			break;
		}

		/* If the lock count drops to zero we know that the
		current SQL statement has ended */
		trx->mysql_n_tables_locked = 0;
		m_prebuilt->used_in_HANDLER = FALSE;

		if (!not_autocommit) {
			if (!not_started) {
				innobase_commit(thd, TRUE);
			}
		} else if (trx->isolation_level <= TRX_ISO_READ_COMMITTED) {
			trx->read_view.close();
		}
		break;
	case F_WRLCK:
		/* If this is a SELECT, then it is in UPDATE TABLE ...
		or SELECT ... FOR UPDATE */
		m_prebuilt->select_lock_type = LOCK_X;
		m_prebuilt->stored_select_lock_type = LOCK_X;
		goto set_lock;
	case F_RDLCK:
		/* Ensure that trx->lock.trx_locks is empty for read-only
		autocommit transactions */
		ut_ad(not_autocommit || trx->n_mysql_tables_in_use
		      || UT_LIST_GET_LEN(trx->lock.trx_locks) == 0);
set_lock:
		*trx->detailed_error = 0;

		innobase_register_trx(ht, thd, trx);

		if (not_autocommit
		    && trx->isolation_level == TRX_ISO_SERIALIZABLE
		    && m_prebuilt->select_lock_type == LOCK_NONE) {

			/* To get serializable execution, we let InnoDB
			conceptually add 'LOCK IN SHARE MODE' to all SELECTs
			which otherwise would have been consistent reads. An
			exception is consistent reads in the AUTOCOMMIT=1 mode:
			we know that they are read-only transactions, and they
			can be serialized also if performed as consistent
			reads. */

			m_prebuilt->select_lock_type = LOCK_S;
			m_prebuilt->stored_select_lock_type = LOCK_S;
		}

		/* Starting from 4.1.9, no InnoDB table lock is taken in LOCK
		TABLES if AUTOCOMMIT=1. It does not make much sense to acquire
		an InnoDB table lock if it is released immediately at the end
		of LOCK TABLES, and InnoDB's table locks in that case cause
		VERY easily deadlocks.

		We do not set InnoDB table locks if user has not explicitly
		requested a table lock. Note that thd_in_lock_tables(thd)
		can hold in some cases, e.g., at the start of a stored
		procedure call (SQLCOM_CALL). */

		if (m_prebuilt->select_lock_type != LOCK_NONE) {

			if (sql_command == SQLCOM_LOCK_TABLES
			    && THDVAR(thd, table_locks)
			    && thd_test_options(thd, OPTION_NOT_AUTOCOMMIT)
			    && thd_in_lock_tables(thd)) {

				dberr_t	error = row_lock_table(m_prebuilt);

				if (error != DB_SUCCESS) {

					DBUG_RETURN(
						convert_error_code_to_mysql(
							error, 0, thd));
				}
			}

			trx->mysql_n_tables_locked++;
		}

		trx->n_mysql_tables_in_use++;
		m_mysql_has_locked = true;

		if (not_started
		    && (m_prebuilt->select_lock_type != LOCK_NONE
			|| m_prebuilt->stored_select_lock_type != LOCK_NONE)) {

			trx->will_lock = true;
		}
	}

	DBUG_RETURN(0);
}

/************************************************************************//**
Here we export InnoDB status variables to MySQL. */
static
void
innodb_export_status()
/*==================*/
{
	if (srv_was_started) {
		srv_export_innodb_status();
	}
}

/************************************************************************//**
Implements the SHOW ENGINE INNODB STATUS command. Sends the output of the
InnoDB Monitor to the client.
@return 0 on success */
static
int
innodb_show_status(
/*===============*/
	handlerton*	hton,	/*!< in: the innodb handlerton */
	THD*		thd,	/*!< in: the MySQL query thread of the caller */
	stat_print_fn*	stat_print)
{
	static const char	truncated_msg[] = "... truncated...\n";
	const long		MAX_STATUS_SIZE = 1048576;
	ulint			trx_list_start = ULINT_UNDEFINED;
	ulint			trx_list_end = ULINT_UNDEFINED;
	bool			ret_val;

	DBUG_ENTER("innodb_show_status");
	DBUG_ASSERT(hton == innodb_hton_ptr);

	/* We don't create the temp files or associated
	mutexes in read-only-mode */

	if (srv_read_only_mode) {
		DBUG_RETURN(0);
	}

	purge_sys.wake_if_not_active();

	/* We let the InnoDB Monitor to output at most MAX_STATUS_SIZE
	bytes of text. */

	char*	str;
	size_t	flen;

	mysql_mutex_lock(&srv_monitor_file_mutex);
	rewind(srv_monitor_file);

	srv_printf_innodb_monitor(srv_monitor_file, FALSE,
				  &trx_list_start, &trx_list_end);

	os_file_set_eof(srv_monitor_file);

	flen = size_t(ftell(srv_monitor_file));
	if (ssize_t(flen) < 0) {
		flen = 0;
	}

	size_t	usable_len;

	if (flen > MAX_STATUS_SIZE) {
		usable_len = MAX_STATUS_SIZE;
		truncated_status_writes++;
	} else {
		usable_len = flen;
	}

	/* allocate buffer for the string, and
	read the contents of the temporary file */

	if (!(str = (char*) my_malloc(PSI_INSTRUMENT_ME,
		      usable_len + 1, MYF(0)))) {
		mysql_mutex_unlock(&srv_monitor_file_mutex);
		DBUG_RETURN(1);
	}

	rewind(srv_monitor_file);

	if (flen < MAX_STATUS_SIZE) {
		/* Display the entire output. */
		flen = fread(str, 1, flen, srv_monitor_file);
	} else if (trx_list_end < flen
		   && trx_list_start < trx_list_end
		   && trx_list_start + flen - trx_list_end
		   < MAX_STATUS_SIZE - sizeof truncated_msg - 1) {

		/* Omit the beginning of the list of active transactions. */
		size_t	len = fread(str, 1, trx_list_start, srv_monitor_file);

		memcpy(str + len, truncated_msg, sizeof truncated_msg - 1);
		len += sizeof truncated_msg - 1;
		usable_len = (MAX_STATUS_SIZE - 1) - len;
		fseek(srv_monitor_file, long(flen - usable_len), SEEK_SET);
		len += fread(str + len, 1, usable_len, srv_monitor_file);
		flen = len;
	} else {
		/* Omit the end of the output. */
		flen = fread(str, 1, MAX_STATUS_SIZE - 1, srv_monitor_file);
	}

	mysql_mutex_unlock(&srv_monitor_file_mutex);

	ret_val= stat_print(
		thd, innobase_hton_name,
		static_cast<uint>(strlen(innobase_hton_name)),
		STRING_WITH_LEN(""), str, static_cast<uint>(flen));

	my_free(str);

	DBUG_RETURN(ret_val);
}

/************************************************************************//**
Return 0 on success and non-zero on failure. Note: the bool return type
seems to be abused here, should be an int. */
static
bool
innobase_show_status(
/*=================*/
	handlerton*		hton,	/*!< in: the innodb handlerton */
	THD*			thd,	/*!< in: the MySQL query thread
					of the caller */
	stat_print_fn*		stat_print,
	enum ha_stat_type	stat_type)
{
	DBUG_ASSERT(hton == innodb_hton_ptr);

	switch (stat_type) {
	case HA_ENGINE_STATUS:
		/* Non-zero return value means there was an error. */
		return(innodb_show_status(hton, thd, stat_print) != 0);

	case HA_ENGINE_MUTEX:
	case HA_ENGINE_LOGS:
		/* Not handled */
		break;
	}

	/* Success */
	return(false);
}

/*********************************************************************//**
Returns number of THR_LOCK locks used for one instance of InnoDB table.
InnoDB no longer relies on THR_LOCK locks so 0 value is returned.
Instead of THR_LOCK locks InnoDB relies on combination of metadata locks
(e.g. for LOCK TABLES and DDL) and its own locking subsystem.
Note that even though this method returns 0, SQL-layer still calls
::store_lock(), ::start_stmt() and ::external_lock() methods for InnoDB
tables. */

uint
ha_innobase::lock_count(void) const
/*===============================*/
{
	return 0;
}

/*****************************************************************//**
Supposed to convert a MySQL table lock stored in the 'lock' field of the
handle to a proper type before storing pointer to the lock into an array
of pointers.
In practice, since InnoDB no longer relies on THR_LOCK locks and its
lock_count() method returns 0 it just informs storage engine about type
of THR_LOCK which SQL-layer would have acquired for this specific statement
on this specific table.
MySQL also calls this if it wants to reset some table locks to a not-locked
state during the processing of an SQL query. An example is that during a
SELECT the read lock is released early on the 'const' tables where we only
fetch one row. MySQL does not call this when it releases all locks at the
end of an SQL statement.
@return pointer to the current element in the 'to' array. */

THR_LOCK_DATA**
ha_innobase::store_lock(
/*====================*/
	THD*			thd,		/*!< in: user thread handle */
	THR_LOCK_DATA**		to,		/*!< in: pointer to the current
						element in an array of pointers
						to lock structs;
						only used as return value */
	thr_lock_type		lock_type)	/*!< in: lock type to store in
						'lock'; this may also be
						TL_IGNORE */
{
	/* Note that trx in this function is NOT necessarily m_prebuilt->trx
	because we call update_thd() later, in ::external_lock()! Failure to
	understand this caused a serious memory corruption bug in 5.1.11. */

	trx_t*	trx = check_trx_exists(thd);

	/* NOTE: MySQL can call this function with lock 'type' TL_IGNORE!
	Be careful to ignore TL_IGNORE if we are going to do something with
	only 'real' locks! */

	/* If no MySQL table is in use, we need to set the isolation level
	of the transaction. */

	if (lock_type != TL_IGNORE
	    && trx->n_mysql_tables_in_use == 0) {
		trx->isolation_level = innobase_map_isolation_level(
			(enum_tx_isolation) thd_tx_isolation(thd)) & 3;

		if (trx->isolation_level <= TRX_ISO_READ_COMMITTED) {

			/* At low transaction isolation levels we let
			each consistent read set its own snapshot */
			trx->read_view.close();
		}
	}

	DBUG_ASSERT(EQ_CURRENT_THD(thd));
	const bool in_lock_tables = thd_in_lock_tables(thd);
	const int sql_command = thd_sql_command(thd);

	if (srv_read_only_mode
	    && (sql_command == SQLCOM_UPDATE
		|| sql_command == SQLCOM_INSERT
		|| sql_command == SQLCOM_REPLACE
		|| sql_command == SQLCOM_DROP_TABLE
		|| sql_command == SQLCOM_ALTER_TABLE
		|| sql_command == SQLCOM_OPTIMIZE
		|| (sql_command == SQLCOM_CREATE_TABLE
		    && (lock_type >= TL_WRITE_CONCURRENT_INSERT
			 && lock_type <= TL_WRITE))
		|| sql_command == SQLCOM_CREATE_INDEX
		|| sql_command == SQLCOM_DROP_INDEX
		|| sql_command == SQLCOM_CREATE_SEQUENCE
		|| sql_command == SQLCOM_DROP_SEQUENCE
		|| sql_command == SQLCOM_DELETE)) {

		ib_senderrf(trx->mysql_thd,
			    IB_LOG_LEVEL_WARN, ER_READ_ONLY_MODE);

	} else if (sql_command == SQLCOM_FLUSH
		   && lock_type == TL_READ_NO_INSERT) {

		/* Check for FLUSH TABLES ... WITH READ LOCK */

		/* Note: This call can fail, but there is no way to return
		the error to the caller. We simply ignore it for now here
		and push the error code to the caller where the error is
		detected in the function. */

		dberr_t	err = row_quiesce_set_state(
			m_prebuilt->table, QUIESCE_START, trx);

		ut_a(err == DB_SUCCESS || err == DB_UNSUPPORTED);

		if (trx->isolation_level == TRX_ISO_SERIALIZABLE) {
			m_prebuilt->select_lock_type = LOCK_S;
			m_prebuilt->stored_select_lock_type = LOCK_S;
		} else {
			m_prebuilt->select_lock_type = LOCK_NONE;
			m_prebuilt->stored_select_lock_type = LOCK_NONE;
		}

	/* Check for DROP TABLE */
	} else if (sql_command == SQLCOM_DROP_TABLE ||
                   sql_command == SQLCOM_DROP_SEQUENCE) {

		/* MySQL calls this function in DROP TABLE though this table
		handle may belong to another thd that is running a query. Let
		us in that case skip any changes to the m_prebuilt struct. */

	/* Check for LOCK TABLE t1,...,tn WITH SHARED LOCKS */
	} else if ((lock_type == TL_READ && in_lock_tables)
		   || (lock_type == TL_READ_HIGH_PRIORITY && in_lock_tables)
		   || lock_type == TL_READ_WITH_SHARED_LOCKS
		   || lock_type == TL_READ_SKIP_LOCKED
		   || lock_type == TL_READ_NO_INSERT
		   || (lock_type != TL_IGNORE
		       && sql_command != SQLCOM_SELECT)) {

		/* The OR cases above are in this order:
		1) MySQL is doing LOCK TABLES ... READ LOCAL, or we
		are processing a stored procedure or function, or
		2) (we do not know when TL_READ_HIGH_PRIORITY is used), or
		3) this is a SELECT ... IN SHARE MODE, or
		4) this is a SELECT ... IN SHARE MODE SKIP LOCKED, or
		5) we are doing a complex SQL statement like
		INSERT INTO ... SELECT ... and the logical logging (MySQL
		binlog) requires the use of a locking read, or
		MySQL is doing LOCK TABLES ... READ.
		6) we let InnoDB do locking reads for all SQL statements that
		are not simple SELECTs; note that select_lock_type in this
		case may get strengthened in ::external_lock() to LOCK_X.
		Note that we MUST use a locking read in all data modifying
		SQL statements, because otherwise the execution would not be
		serializable, and also the results from the update could be
		unexpected if an obsolete consistent read view would be
		used. */

		/* Use consistent read for checksum table */

		if (sql_command == SQLCOM_CHECKSUM
		    || sql_command == SQLCOM_CREATE_SEQUENCE
		    || (sql_command == SQLCOM_ANALYZE && lock_type == TL_READ)
		    || (trx->isolation_level <= TRX_ISO_READ_COMMITTED
			&& (lock_type == TL_READ
			    || lock_type == TL_READ_NO_INSERT)
			&& (sql_command == SQLCOM_INSERT_SELECT
			    || sql_command == SQLCOM_REPLACE_SELECT
			    || sql_command == SQLCOM_UPDATE
			    || sql_command == SQLCOM_CREATE_SEQUENCE
			    || sql_command == SQLCOM_CREATE_TABLE))
		    || (trx->isolation_level == TRX_ISO_REPEATABLE_READ
		        && sql_command == SQLCOM_ALTER_TABLE
		        && lock_type == TL_READ)) {

			/* If the transaction isolation level is
			READ UNCOMMITTED or READ COMMITTED and we are executing
			INSERT INTO...SELECT or REPLACE INTO...SELECT
			or UPDATE ... = (SELECT ...) or CREATE  ...
			SELECT... without FOR UPDATE or IN SHARE
			MODE in select, then we use consistent read
			for select. */

			m_prebuilt->select_lock_type = LOCK_NONE;
			m_prebuilt->stored_select_lock_type = LOCK_NONE;
		} else {
			m_prebuilt->select_lock_type = LOCK_S;
			m_prebuilt->stored_select_lock_type = LOCK_S;
		}

	} else if (lock_type != TL_IGNORE) {

		/* We set possible LOCK_X value in external_lock, not yet
		here even if this would be SELECT ... FOR UPDATE */

		m_prebuilt->select_lock_type = LOCK_NONE;
		m_prebuilt->stored_select_lock_type = LOCK_NONE;
	}
	m_prebuilt->skip_locked= (lock_type == TL_WRITE_SKIP_LOCKED ||
				  lock_type == TL_READ_SKIP_LOCKED);
	return(to);
}

/*********************************************************************//**
Read the next autoinc value. Acquire the relevant locks before reading
the AUTOINC value. If SUCCESS then the table AUTOINC mutex will be locked
on return and all relevant locks acquired.
@return DB_SUCCESS or error code */

dberr_t
ha_innobase::innobase_get_autoinc(
/*==============================*/
	ulonglong*	value)		/*!< out: autoinc value */
{
	*value = 0;

	m_prebuilt->autoinc_error = innobase_lock_autoinc();

	if (m_prebuilt->autoinc_error == DB_SUCCESS) {

		/* Determine the first value of the interval */
		*value = dict_table_autoinc_read(m_prebuilt->table);

		/* It should have been initialized during open. */
		if (*value == 0) {
			m_prebuilt->autoinc_error = DB_UNSUPPORTED;
			m_prebuilt->table->autoinc_mutex.wr_unlock();
		}
	}

	return(m_prebuilt->autoinc_error);
}

/*******************************************************************//**
This function reads the global auto-inc counter. It doesn't use the
AUTOINC lock even if the lock mode is set to TRADITIONAL.
@return the autoinc value */

ulonglong
ha_innobase::innobase_peek_autoinc(void)
/*====================================*/
{
	ulonglong	auto_inc;
	dict_table_t*	innodb_table;

	ut_a(m_prebuilt != NULL);
	ut_a(m_prebuilt->table != NULL);

	innodb_table = m_prebuilt->table;

	innodb_table->autoinc_mutex.wr_lock();

	auto_inc = dict_table_autoinc_read(innodb_table);

	if (auto_inc == 0) {
		ib::info() << "AUTOINC next value generation is disabled for"
			" '" << innodb_table->name << "'";
	}

	innodb_table->autoinc_mutex.wr_unlock();

	return(auto_inc);
}

/*********************************************************************//**
Returns the value of the auto-inc counter in *first_value and ~0 on failure. */

void
ha_innobase::get_auto_increment(
/*============================*/
	ulonglong	offset,			/*!< in: table autoinc offset */
	ulonglong	increment,		/*!< in: table autoinc
						increment */
	ulonglong	nb_desired_values,	/*!< in: number of values
						reqd */
	ulonglong*	first_value,		/*!< out: the autoinc value */
	ulonglong*	nb_reserved_values)	/*!< out: count of reserved
						values */
{
	trx_t*		trx;
	dberr_t		error;
	ulonglong	autoinc = 0;
	mariadb_set_stats set_stats_temporary(handler_stats);

	/* Prepare m_prebuilt->trx in the table handle */
	update_thd(ha_thd());

	error = innobase_get_autoinc(&autoinc);

	if (error != DB_SUCCESS) {
		*first_value = (~(ulonglong) 0);
		/* This is an error case. We do the error handling by calling
		the error code conversion function. Specifically, we need to
		call thd_mark_transaction_to_rollback() to inform sql that we
		have rolled back innodb transaction after a deadlock error. We
		ignore the returned mysql error code here. */
		std::ignore = convert_error_code_to_mysql(
			error, m_prebuilt->table->flags, m_user_thd);
		return;
	}

	/* This is a hack, since nb_desired_values seems to be accurate only
	for the first call to get_auto_increment() for multi-row INSERT and
	meaningless for other statements e.g, LOAD etc. Subsequent calls to
	this method for the same statement results in different values which
	don't make sense. Therefore we store the value the first time we are
	called and count down from that as rows are written (see write_row()).
	*/

	trx = m_prebuilt->trx;

	/* Note: We can't rely on *first_value since some MySQL engines,
	in particular the partition engine, don't initialize it to 0 when
	invoking this method. So we are not sure if it's guaranteed to
	be 0 or not. */

	/* We need the upper limit of the col type to check for
	whether we update the table autoinc counter or not. */
	ulonglong col_max_value =
			table->next_number_field->get_max_int_value();

	/** The following logic is needed to avoid duplicate key error
	for autoincrement column.

	(1) InnoDB gives the current autoincrement value with respect
	to increment and offset value.

	(2) Basically it does compute_next_insert_id() logic inside InnoDB
	to avoid the current auto increment value changed by handler layer.

	(3) It is restricted only for insert operations. */

	if (increment > 1 && increment <= ~autoinc && autoinc < col_max_value
	    && thd_sql_command(m_user_thd) != SQLCOM_ALTER_TABLE) {

		ulonglong prev_auto_inc = autoinc;

		autoinc = ((autoinc - 1) + increment - offset)/ increment;

		autoinc = autoinc * increment + offset;

		/* If autoinc exceeds the col_max_value then reset
		to old autoinc value. Because in case of non-strict
		sql mode, boundary value is not considered as error. */

		if (autoinc >= col_max_value) {
			autoinc = prev_auto_inc;
		}

		ut_ad(autoinc > 0);
	}

	/* Called for the first time ? */
	if (trx->n_autoinc_rows == 0) {

		trx->n_autoinc_rows = (ulint) nb_desired_values;

		/* It's possible for nb_desired_values to be 0:
		e.g., INSERT INTO T1(C) SELECT C FROM T2; */
		if (nb_desired_values == 0) {

			trx->n_autoinc_rows = 1;
		}

		set_if_bigger(*first_value, autoinc);
	/* Not in the middle of a mult-row INSERT. */
	} else if (m_prebuilt->autoinc_last_value == 0) {
		set_if_bigger(*first_value, autoinc);
	}

	if (*first_value > col_max_value) {
		/* Out of range number. Let handler::update_auto_increment()
		take care of this */
		m_prebuilt->autoinc_last_value = 0;
		m_prebuilt->table->autoinc_mutex.wr_unlock();
		*nb_reserved_values= 0;
		return;
	}

	*nb_reserved_values = trx->n_autoinc_rows;

	/* With old style AUTOINC locking we only update the table's
	AUTOINC counter after attempting to insert the row. */
	if (innobase_autoinc_lock_mode != AUTOINC_OLD_STYLE_LOCKING) {
		ulonglong	current;
		ulonglong	next_value;

		current = *first_value;

		/* Compute the last value in the interval */
		next_value = innobase_next_autoinc(
			current, *nb_reserved_values, increment, offset,
			col_max_value);

		m_prebuilt->autoinc_last_value = next_value;

		if (m_prebuilt->autoinc_last_value < *first_value) {
			*first_value = (~(ulonglong) 0);
		} else {
			/* Update the table autoinc variable */
			dict_table_autoinc_update_if_greater(
				m_prebuilt->table,
				m_prebuilt->autoinc_last_value);
		}
	} else {
		/* This will force write_row() into attempting an update
		of the table's AUTOINC counter. */
		m_prebuilt->autoinc_last_value = 0;
	}

	/* The increment to be used to increase the AUTOINC value, we use
	this in write_row() and update_row() to increase the autoinc counter
	for columns that are filled by the user. We need the offset and
	the increment. */
	m_prebuilt->autoinc_offset = offset;
	m_prebuilt->autoinc_increment = increment;

	m_prebuilt->table->autoinc_mutex.wr_unlock();
}

/*******************************************************************//**
See comment in handler.cc */

bool
ha_innobase::get_error_message(
/*===========================*/
	int	error,
	String*	buf)
{
	trx_t*	trx = check_trx_exists(ha_thd());

	if (error == HA_ERR_DECRYPTION_FAILED) {
		const char *msg = "Table encrypted but decryption failed. This could be because correct encryption management plugin is not loaded, used encryption key is not available or encryption method does not match.";
		buf->copy(msg, (uint)strlen(msg), system_charset_info);
	} else {
		buf->copy(trx->detailed_error, (uint) strlen(trx->detailed_error),
			system_charset_info);
	}

	return(FALSE);
}

/** Retrieves the names of the table and the key for which there was a
duplicate entry in the case of HA_ERR_FOREIGN_DUPLICATE_KEY.

If any of the names is not available, then this method will return
false and will not change any of child_table_name or child_key_name.

@param[out] child_table_name Table name
@param[in] child_table_name_len Table name buffer size
@param[out] child_key_name Key name
@param[in] child_key_name_len Key name buffer size

@retval true table and key names were available and were written into the
corresponding out parameters.
@retval false table and key names were not available, the out parameters
were not touched. */
bool
ha_innobase::get_foreign_dup_key(
/*=============================*/
	char*	child_table_name,
	uint	child_table_name_len,
	char*	child_key_name,
	uint	child_key_name_len)
{
	const dict_index_t*	err_index;

	ut_a(m_prebuilt->trx != NULL);
	ut_a(m_prebuilt->trx->magic_n == TRX_MAGIC_N);

	err_index = trx_get_error_info(m_prebuilt->trx);

	if (err_index == NULL) {
		return(false);
	}
	/* else */

	/* copy table name (and convert from filename-safe encoding to
	system_charset_info) */
	char*	p = strchr(err_index->table->name.m_name, '/');

	/* strip ".../" prefix if any */
	if (p != NULL) {
		p++;
	} else {
		p = err_index->table->name.m_name;
	}

	size_t	len;

	len = filename_to_tablename(p, child_table_name, child_table_name_len);

	child_table_name[len] = '\0';

	/* copy index name */
	snprintf(child_key_name, child_key_name_len, "%s",
		    err_index->name());

	return(true);
}

/*******************************************************************//**
Compares two 'refs'. A 'ref' is the (internal) primary key value of the row.
If there is no explicitly declared non-null unique key or a primary key, then
InnoDB internally uses the row id as the primary key.
@return < 0 if ref1 < ref2, 0 if equal, else > 0 */

int
ha_innobase::cmp_ref(
/*=================*/
	const uchar*	ref1,	/*!< in: an (internal) primary key value in the
				MySQL key value format */
	const uchar*	ref2)	/*!< in: an (internal) primary key value in the
				MySQL key value format */
{
	enum_field_types mysql_type;
	Field*		field;
	KEY_PART_INFO*	key_part;
	KEY_PART_INFO*	key_part_end;
	uint		len1;
	uint		len2;
	int		result;

	if (m_prebuilt->clust_index_was_generated) {
		/* The 'ref' is an InnoDB row id */

		return(memcmp(ref1, ref2, DATA_ROW_ID_LEN));
	}

	/* Do a type-aware comparison of primary key fields. PK fields
	are always NOT NULL, so no checks for NULL are performed. */

	key_part = table->key_info[table->s->primary_key].key_part;

	key_part_end = key_part
		+ table->key_info[table->s->primary_key].user_defined_key_parts;

	for (; key_part != key_part_end; ++key_part) {
		field = key_part->field;
		mysql_type = field->type();

		if (mysql_type == MYSQL_TYPE_TINY_BLOB
			|| mysql_type == MYSQL_TYPE_MEDIUM_BLOB
			|| mysql_type == MYSQL_TYPE_BLOB
			|| mysql_type == MYSQL_TYPE_LONG_BLOB) {

			/* In the MySQL key value format, a column prefix of
			a BLOB is preceded by a 2-byte length field */

			len1 = innobase_read_from_2_little_endian(ref1);
			len2 = innobase_read_from_2_little_endian(ref2);

			result = ((Field_blob*) field)->cmp(
				ref1 + 2, len1, ref2 + 2, len2);
		} else {
			result = field->key_cmp(ref1, ref2);
		}

		if (result) {
			if (key_part->key_part_flag & HA_REVERSE_SORT)
				result = -result;
			return(result);
		}

		ref1 += key_part->store_length;
		ref2 += key_part->store_length;
	}

	return(0);
}

/*******************************************************************//**
Ask InnoDB if a query to a table can be cached.
@return TRUE if query caching of the table is permitted */

my_bool
ha_innobase::register_query_cache_table(
/*====================================*/
	THD*		thd,		/*!< in: user thread handle */
	const char*	table_key,	/*!< in: normalized path to the
					table */
	uint		key_length,	/*!< in: length of the normalized
					path to the table */
	qc_engine_callback*
			call_back,	/*!< out: pointer to function for
					checking if query caching
					is permitted */
	ulonglong	*engine_data)	/*!< in/out: data to call_back */
{
	*engine_data = 0;
	*call_back = innobase_query_caching_of_table_permitted;

	return(innobase_query_caching_of_table_permitted(
			thd, table_key,
			static_cast<uint>(key_length),
			engine_data));
}

/******************************************************************//**
This function is used to find the storage length in bytes of the first n
characters for prefix indexes using a multibyte character set. The function
finds charset information and returns length of prefix_len characters in the
index field in bytes.
@return number of bytes occupied by the first n characters */
ulint
innobase_get_at_most_n_mbchars(
/*===========================*/
	ulint charset_id,	/*!< in: character set id */
	ulint prefix_len,	/*!< in: prefix length in bytes of the index
				(this has to be divided by mbmaxlen to get the
				number of CHARACTERS n in the prefix) */
	ulint data_len,		/*!< in: length of the string in bytes */
	const char* str)	/*!< in: character string */
{
	ulint char_length;	/*!< character length in bytes */
	ulint n_chars;		/*!< number of characters in prefix */
	CHARSET_INFO* charset;	/*!< charset used in the field */

	charset = get_charset((uint) charset_id, MYF(MY_WME));

	ut_ad(charset);
	ut_ad(charset->mbmaxlen);

	/* Calculate how many characters at most the prefix index contains */

	n_chars = prefix_len / charset->mbmaxlen;

	/* If the charset is multi-byte, then we must find the length of the
	first at most n chars in the string. If the string contains less
	characters than n, then we return the length to the end of the last
	character. */

	if (charset->mbmaxlen > 1) {
		/* charpos() returns the byte length of the first n_chars
		characters, or a value bigger than the length of str, if
		there were not enough full characters in str.

		Why does the code below work:
		Suppose that we are looking for n UTF-8 characters.

		1) If the string is long enough, then the prefix contains at
		least n complete UTF-8 characters + maybe some extra
		characters + an incomplete UTF-8 character. No problem in
		this case. The function returns the pointer to the
		end of the nth character.

		2) If the string is not long enough, then the string contains
		the complete value of a column, that is, only complete UTF-8
		characters, and we can store in the column prefix index the
		whole string. */

		char_length= charset->charpos(str, str + data_len, n_chars);
		if (char_length > data_len) {
			char_length = data_len;
		}
	} else if (data_len < prefix_len) {

		char_length = data_len;

	} else {

		char_length = prefix_len;
	}

	return(char_length);
}

/*******************************************************************//**
This function is used to prepare an X/Open XA distributed transaction.
@return 0 or error number */
static
int
innobase_xa_prepare(
/*================*/
	THD*		thd,		/*!< in: handle to the MySQL thread of
					the user whose XA transaction should
					be prepared */
	bool		prepare_trx)	/*!< in: true - prepare transaction
					false - the current SQL statement
					ended */
{
  trx_t *trx= check_trx_exists(thd);
  ut_ad(trx_is_registered_for_2pc(trx));
  if (!prepare_trx)
    prepare_trx= !thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN);

  switch (UNIV_EXPECT(trx->state, TRX_STATE_ACTIVE)) {
  default:
    ut_ad("invalid state" == 0);
    return HA_ERR_GENERIC;
  case TRX_STATE_NOT_STARTED:
    if (prepare_trx)
      trx_start_if_not_started_xa(trx, false);;
    /* fall through */
  case TRX_STATE_ACTIVE:
    thd_get_xid(thd, &reinterpret_cast<MYSQL_XID&>(trx->xid));
    if (prepare_trx)
      trx_prepare_for_mysql(trx);
    else
    {
      lock_unlock_table_autoinc(trx);
      return end_of_statement(trx);
    }
    return 0;
  }
}

/*******************************************************************//**
This function is used to recover X/Open XA distributed transactions.
@return number of prepared transactions stored in xid_list */
static
int
innobase_xa_recover(
/*================*/
	XID*		xid_list,/*!< in/out: prepared transactions */
	uint		len)	/*!< in: number of slots in xid_list */
{
	if (len == 0 || xid_list == NULL) {

		return(0);
	}

	return(trx_recover_for_mysql(xid_list, len));
}

/*******************************************************************//**
This function is used to commit one X/Open XA distributed transaction
which is in the prepared state
@return 0 or error number */
static
int
innobase_commit_by_xid(
/*===================*/
	XID*		xid)	/*!< in: X/Open XA transaction identification */
{
	DBUG_EXECUTE_IF("innobase_xa_fail",
			return XAER_RMFAIL;);

	if (high_level_read_only) {
		return(XAER_RMFAIL);
	}

	if (trx_t* trx = trx_get_trx_by_xid(xid)) {
		/* use cases are: disconnected xa, slave xa, recovery */
		innobase_commit_low(trx);
		ut_ad(trx->mysql_thd == NULL);
		trx_deregister_from_2pc(trx);
		ut_ad(!trx->will_lock);    /* trx cache requirement */
		trx->free();

		return(XA_OK);
	} else {
		return(XAER_NOTA);
	}
}

#ifndef EMBEDDED_LIBRARY
/**
  This function is used to rollback one X/Open XA distributed transaction
  which is in the prepared state asynchronously.

  It only set the transaction's status to ACTIVE and persist the status.
  The transaction will be rolled back by background rollback thread.

  @param xid X/Open XA transaction identification

  @return 0 or error number
*/
static int innobase_recover_rollback_by_xid(const XID *xid)
{
  DBUG_EXECUTE_IF("innobase_xa_fail", return XAER_RMFAIL;);

  if (high_level_read_only)
    return XAER_RMFAIL;

  /*
    trx_get_trx_by_xid() sets trx's xid to null. Thus only one call for any
    given XID can find the transaction. Subsequent calls by other threads
    would return nullptr. That is what guarantees that no other thread can be
    modifying the state of the transaction at this point.
  */
  trx_t *trx= trx_get_trx_by_xid(xid);
  if (!trx)
    return XAER_RMFAIL;

  // ddl should not be rolled back through recovery
  ut_ad(!trx->dict_operation);
  ut_ad(trx->is_recovered);
  ut_ad(trx->state == TRX_STATE_PREPARED);

# ifdef WITH_WSREP
  // prepared transactions must not be rolled back asynchronously when wsrep is on
  ut_ad(!(WSREP_ON || wsrep_recovery));
# endif

  if (trx->rsegs.m_redo.undo)
  {
    ut_ad(trx->rsegs.m_redo.undo->rseg == trx->rsegs.m_redo.rseg);

    mtr_t mtr;
    mtr.start();
    trx_undo_set_state_at_prepare(trx, trx->rsegs.m_redo.undo, true, &mtr);
    mtr.commit();

    ut_ad(mtr.commit_lsn() > 0);
  }

  /* The above state change from XA PREPARE will be made durable in
  innobase_tc_log_recovery_done(), which will also initiate
  trx_rollback_recovered() to roll back this transaction. */
  trx->state= TRX_STATE_ACTIVE;
  return 0;
}

static void innobase_tc_log_recovery_done()
{
  if (high_level_read_only)
    return;

  /* Make durable any innobase_recover_rollback_by_xid(). */
  log_buffer_flush_to_disk(true);

  if (srv_force_recovery < SRV_FORCE_NO_TRX_UNDO)
  {
    /* Rollback incomplete non-DDL transactions */
    trx_rollback_is_active= true;
    srv_thread_pool->submit_task(&rollback_all_recovered_task);
  }
}
#endif // EMBEDDED_LIBRARY

bool
ha_innobase::check_if_incompatible_data(
/*====================================*/
	HA_CREATE_INFO*	info,
	uint		table_changes)
{
	ha_table_option_struct *param_old, *param_new;

	/* Cache engine specific options */
	param_new = info->option_struct;
	param_old = table->s->option_struct;

	m_prebuilt->table->stats_mutex_lock();
	if (!m_prebuilt->table->stat_initialized()) {
		innobase_copy_frm_flags_from_create_info(
			m_prebuilt->table, info);
	}
	m_prebuilt->table->stats_mutex_unlock();

	if (table_changes != IS_EQUAL_YES) {

		return(COMPATIBLE_DATA_NO);
	}

	/* Check that auto_increment value was not changed */
	if ((info->used_fields & HA_CREATE_USED_AUTO)
	    && info->auto_increment_value != 0) {

		return(COMPATIBLE_DATA_NO);
	}

	/* Check that row format didn't change */
	if ((info->used_fields & HA_CREATE_USED_ROW_FORMAT)
	    && info->row_type != get_row_type()) {

		return(COMPATIBLE_DATA_NO);
	}

	/* Specifying KEY_BLOCK_SIZE requests a rebuild of the table. */
	if (info->used_fields & HA_CREATE_USED_KEY_BLOCK_SIZE) {
		return(COMPATIBLE_DATA_NO);
	}

	/* Changes on engine specific table options requests a rebuild of the table. */
	if (param_new->page_compressed != param_old->page_compressed ||
	    param_new->page_compression_level != param_old->page_compression_level)
        {
		return(COMPATIBLE_DATA_NO);
	}

	return(COMPATIBLE_DATA_YES);
}

/****************************************************************//**
Update the system variable innodb_io_capacity_max using the "saved"
value. This function is registered as a callback with MySQL. */
static
void
innodb_io_capacity_max_update(
/*===========================*/
	THD*				thd,	/*!< in: thread handle */
	st_mysql_sys_var*, void*,
	const void*			save)	/*!< in: immediate result
						from check function */
{
	ulong	in_val = *static_cast<const ulong*>(save);

	if (in_val < srv_io_capacity) {
		push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
				    ER_WRONG_ARGUMENTS,
				    "Setting innodb_io_capacity_max %lu"
			" lower than innodb_io_capacity %lu.",
			in_val, srv_io_capacity);

		srv_io_capacity = in_val;

		push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
					    ER_WRONG_ARGUMENTS,
				    "Setting innodb_io_capacity to %lu",
				    srv_io_capacity);
	}

	srv_max_io_capacity = in_val;
}

/****************************************************************//**
Update the system variable innodb_io_capacity using the "saved"
value. This function is registered as a callback with MySQL. */
static
void
innodb_io_capacity_update(
/*======================*/
	THD*				thd,	/*!< in: thread handle */
	st_mysql_sys_var*, void*,
	const void*			save)	/*!< in: immediate result
						from check function */
{
	ulong	in_val = *static_cast<const ulong*>(save);

	if (in_val > srv_max_io_capacity) {
		push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
				    ER_WRONG_ARGUMENTS,
				    "Setting innodb_io_capacity to %lu"
				    " higher than innodb_io_capacity_max %lu",
				    in_val, srv_max_io_capacity);

		/* Avoid overflow. */
		srv_max_io_capacity = (in_val >= SRV_MAX_IO_CAPACITY_LIMIT / 2)
			? in_val : in_val * 2;

		push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
				    ER_WRONG_ARGUMENTS,
				    "Setting innodb_max_io_capacity to %lu",
				    srv_max_io_capacity);
	}

	srv_io_capacity = in_val;
}

/****************************************************************//**
Update the system variable innodb_max_dirty_pages_pct using the "saved"
value. This function is registered as a callback with MySQL. */
static
void
innodb_max_dirty_pages_pct_update(
/*==============================*/
	THD*				thd,	/*!< in: thread handle */
	st_mysql_sys_var*, void*,
	const void*			save)	/*!< in: immediate result
						from check function */
{
	double	in_val = *static_cast<const double*>(save);
	if (in_val < srv_max_dirty_pages_pct_lwm) {
		push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
				    ER_WRONG_ARGUMENTS,
				    "innodb_max_dirty_pages_pct cannot be"
				    " set lower than"
				    " innodb_max_dirty_pages_pct_lwm.");
		push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
				    ER_WRONG_ARGUMENTS,
				    "Lowering"
				    " innodb_max_dirty_page_pct_lwm to %lf",
				    in_val);

		srv_max_dirty_pages_pct_lwm = in_val;
	}

	srv_max_buf_pool_modified_pct = in_val;

	mysql_mutex_unlock(&LOCK_global_system_variables);
	mysql_mutex_lock(&buf_pool.flush_list_mutex);
	buf_pool.page_cleaner_wakeup();
	mysql_mutex_unlock(&buf_pool.flush_list_mutex);
	mysql_mutex_lock(&LOCK_global_system_variables);
}

/****************************************************************//**
Update the system variable innodb_max_dirty_pages_pct_lwm using the
"saved" value. This function is registered as a callback with MySQL. */
static
void
innodb_max_dirty_pages_pct_lwm_update(
/*==================================*/
	THD*				thd,	/*!< in: thread handle */
	st_mysql_sys_var*, void*,
	const void*			save)	/*!< in: immediate result
						from check function */
{
	double	in_val = *static_cast<const double*>(save);
	if (in_val > srv_max_buf_pool_modified_pct) {
		in_val = srv_max_buf_pool_modified_pct;
		push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
				    ER_WRONG_ARGUMENTS,
				    "innodb_max_dirty_pages_pct_lwm"
				    " cannot be set higher than"
				    " innodb_max_dirty_pages_pct.");
		push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
				    ER_WRONG_ARGUMENTS,
				    "Setting innodb_max_dirty_page_pct_lwm"
				    " to %lf",
				    in_val);
	}

	srv_max_dirty_pages_pct_lwm = in_val;

	mysql_mutex_unlock(&LOCK_global_system_variables);
	mysql_mutex_lock(&buf_pool.flush_list_mutex);
	buf_pool.page_cleaner_wakeup();
	mysql_mutex_unlock(&buf_pool.flush_list_mutex);
	mysql_mutex_lock(&LOCK_global_system_variables);
}

/*************************************************************//**
Don't allow to set innodb_fast_shutdown=0 if purge threads are
already down.
@return 0 if innodb_fast_shutdown can be set */
static
int
fast_shutdown_validate(
/*=============================*/
	THD*				thd,	/*!< in: thread handle */
	struct st_mysql_sys_var*	var,	/*!< in: pointer to system
						variable */
	void*				save,	/*!< out: immediate result
						for update function */
	struct st_mysql_value*		value)	/*!< in: incoming string */
{
	if (check_sysvar_int(thd, var, save, value)) {
		return(1);
	}

	uint new_val = *reinterpret_cast<uint*>(save);

	if (srv_fast_shutdown && !new_val
	    && !srv_read_only_mode && abort_loop) {
		return(1);
	}

	return(0);
}

/*************************************************************//**
Check whether valid argument given to innobase_*_stopword_table.
This function is registered as a callback with MySQL.
@return 0 for valid stopword table */
static
int
innodb_stopword_table_validate(
/*===========================*/
	THD*				thd,	/*!< in: thread handle */
	st_mysql_sys_var*,
	void*				save,	/*!< out: immediate result
						for update function */
	struct st_mysql_value*		value)	/*!< in: incoming string */
{
	const char*	stopword_table_name;
	char		buff[STRING_BUFFER_USUAL_SIZE];
	int		len = sizeof(buff);
	trx_t*		trx;

	ut_a(save != NULL);
	ut_a(value != NULL);

	stopword_table_name = value->val_str(value, buff, &len);

	trx = check_trx_exists(thd);

	row_mysql_lock_data_dictionary(trx);

	/* Validate the stopword table's (if supplied) existence and
	of the right format */
	int ret = stopword_table_name && !fts_valid_stopword_table(
		stopword_table_name, NULL);

	row_mysql_unlock_data_dictionary(trx);

	if (!ret) {
		if (stopword_table_name == buff) {
			ut_ad(static_cast<size_t>(len) < sizeof buff);
			stopword_table_name = thd_strmake(thd,
							  stopword_table_name,
							  len);
		}

		*static_cast<const char**>(save) = stopword_table_name;
	}

	return(ret);
}

/** The latest assigned innodb_ft_aux_table name */
static char* innodb_ft_aux_table;

/** Update innodb_ft_aux_table_id on SET GLOBAL innodb_ft_aux_table.
@param[in,out]	thd	connection
@param[out]	save	new value of innodb_ft_aux_table
@param[in]	value	user-specified value */
static int innodb_ft_aux_table_validate(THD *thd, st_mysql_sys_var*,
					void* save, st_mysql_value* value)
{
	char buf[STRING_BUFFER_USUAL_SIZE];
	int len = sizeof buf;

	if (const char* table_name = value->val_str(value, buf, &len)) {
		/* Because we are not acquiring MDL on the table name,
		we must contiguously hold dict_sys.latch while we are
		examining the table, to protect us against concurrent DDL. */
		dict_sys.lock(SRW_LOCK_CALL);
		if (dict_table_t* table = dict_table_open_on_name(
			    table_name, true, DICT_ERR_IGNORE_NONE)) {
			table->release();
			const table_id_t id = dict_table_has_fts_index(table)
				? table->id : 0;
			dict_sys.unlock();
			if (id) {
				innodb_ft_aux_table_id = id;
				if (table_name == buf) {
					ut_ad(static_cast<size_t>(len)
					      < sizeof buf);
					table_name = thd_strmake(thd,
								 table_name,
								 len);
				}

				*static_cast<const char**>(save) = table_name;
				return 0;
			}
		} else {
			dict_sys.unlock();
		}
		return 1;
	} else {
		*static_cast<char**>(save) = NULL;
		innodb_ft_aux_table_id = 0;
		return 0;
	}
}

#ifdef BTR_CUR_HASH_ADAPT
/****************************************************************//**
Update the system variable innodb_adaptive_hash_index using the "saved"
value. This function is registered as a callback with MySQL. */
static
void
innodb_adaptive_hash_index_update(THD*, st_mysql_sys_var*, void*,
				  const void* save)
{
	mysql_mutex_unlock(&LOCK_global_system_variables);
	if (*(my_bool*) save) {
		btr_search.enable();
	} else {
		btr_search.disable();
	}
	mysql_mutex_lock(&LOCK_global_system_variables);
}
#endif /* BTR_CUR_HASH_ADAPT */

/****************************************************************//**
Update the system variable innodb_cmp_per_index using the "saved"
value. This function is registered as a callback with MySQL. */
static
void
innodb_cmp_per_index_update(THD*, st_mysql_sys_var*, void*, const void* save)
{
	/* Reset the stats whenever we enable the table
	INFORMATION_SCHEMA.innodb_cmp_per_index. */
	if (!srv_cmp_per_index_enabled && *(my_bool*) save) {
		mysql_mutex_unlock(&LOCK_global_system_variables);
		page_zip_reset_stat_per_index();
		mysql_mutex_lock(&LOCK_global_system_variables);
	}

	srv_cmp_per_index_enabled = !!(*(my_bool*) save);
}

/****************************************************************//**
Update the system variable innodb_old_blocks_pct using the "saved"
value. This function is registered as a callback with MySQL. */
static
void
innodb_old_blocks_pct_update(THD*, st_mysql_sys_var*, void*, const void* save)
{
	mysql_mutex_unlock(&LOCK_global_system_variables);
	uint ratio = buf_LRU_old_ratio_update(*static_cast<const uint*>(save),
					      true);
	mysql_mutex_lock(&LOCK_global_system_variables);
	innobase_old_blocks_pct = ratio;
}

#ifdef UNIV_DEBUG
static uint srv_fil_make_page_dirty_debug = 0;
static uint srv_saved_page_number_debug;

/****************************************************************//**
Make the first page of given user tablespace dirty. */
static
void
innodb_make_page_dirty(THD*, st_mysql_sys_var*, void*, const void* save)
{
	mtr_t		mtr;
	uint		space_id = *static_cast<const uint*>(save);
	srv_fil_make_page_dirty_debug= space_id;
	mysql_mutex_unlock(&LOCK_global_system_variables);
	fil_space_t*	space = fil_space_t::get(space_id);

	if (space == NULL) {
func_exit_no_space:
		mysql_mutex_lock(&LOCK_global_system_variables);
		return;
	}

	if (srv_saved_page_number_debug >= space->size) {
func_exit:
		space->release();
		goto func_exit_no_space;
	}

	mtr.start();
	mtr.set_named_space(space);

	buf_block_t*	block = buf_page_get(
		page_id_t(space_id, srv_saved_page_number_debug),
		space->zip_size(), RW_X_LATCH, &mtr);

	if (block != NULL) {
		ib::info() << "Dirtying page: " << block->page.id();
		mtr.write<1,mtr_t::FORCED>(*block,
					   block->page.frame
					   + FIL_PAGE_SPACE_ID,
					   block->page.frame
					   [FIL_PAGE_SPACE_ID]);
	}
	mtr.commit();
	log_write_up_to(mtr.commit_lsn(), true);
	goto func_exit;
}
#endif // UNIV_DEBUG

/****************************************************************//**
Update the monitor counter according to the "set_option",  turn
on/off or reset specified monitor counter. */
static
void
innodb_monitor_set_option(
/*======================*/
	const monitor_info_t* monitor_info,/*!< in: monitor info for the monitor
					to set */
	mon_option_t	set_option)	/*!< in: Turn on/off reset the
					counter */
{
	monitor_id_t	monitor_id = monitor_info->monitor_id;

	/* If module type is MONITOR_GROUP_MODULE, it cannot be
	turned on/off individually. It should never use this
	function to set options */
	ut_a(!(monitor_info->monitor_type & MONITOR_GROUP_MODULE));

	switch (set_option) {
	case MONITOR_TURN_ON:
		MONITOR_ON(monitor_id);
		MONITOR_INIT(monitor_id);
		MONITOR_SET_START(monitor_id);

		/* If the monitor to be turned on uses
		exisitng monitor counter (status variable),
		make special processing to remember existing
		counter value. */
		if (monitor_info->monitor_type & MONITOR_EXISTING) {
			srv_mon_process_existing_counter(
				monitor_id, MONITOR_TURN_ON);
		}
		break;

	case MONITOR_TURN_OFF:
		if (monitor_info->monitor_type & MONITOR_EXISTING) {
			srv_mon_process_existing_counter(
				monitor_id, MONITOR_TURN_OFF);
		}

		MONITOR_OFF(monitor_id);
		MONITOR_SET_OFF(monitor_id);
		break;

	case MONITOR_RESET_VALUE:
		srv_mon_reset(monitor_id);
		break;

	case MONITOR_RESET_ALL_VALUE:
		srv_mon_reset_all(monitor_id);
		break;

	default:
		ut_error;
	}
}

/****************************************************************//**
Find matching InnoDB monitor counters and update their status
according to the "set_option",  turn on/off or reset specified
monitor counter. */
static
void
innodb_monitor_update_wildcard(
/*===========================*/
	const char*	name,		/*!< in: monitor name to match */
	mon_option_t	set_option)	/*!< in: the set option, whether
					to turn on/off or reset the counter */
{
	ut_a(name);

	for (ulint use = 0; use < NUM_MONITOR; use++) {
		ulint		type;
		monitor_id_t	monitor_id = static_cast<monitor_id_t>(use);
		monitor_info_t*	monitor_info;

		if (!innobase_wildcasecmp(
			srv_mon_get_name(monitor_id), name)) {
			monitor_info = srv_mon_get_info(monitor_id);

			type = monitor_info->monitor_type;

			/* If the monitor counter is of MONITOR_MODULE
			type, skip it. Except for those also marked with
			MONITOR_GROUP_MODULE flag, which can be turned
			on only as a module. */
			if (!(type & MONITOR_MODULE)
			     && !(type & MONITOR_GROUP_MODULE)) {
				innodb_monitor_set_option(monitor_info,
							  set_option);
			}

			/* Need to special handle counters marked with
			MONITOR_GROUP_MODULE, turn on the whole module if
			any one of it comes here. Currently, only
			"module_buf_page" is marked with MONITOR_GROUP_MODULE */
			if (type & MONITOR_GROUP_MODULE) {
				if ((monitor_id >= MONITOR_MODULE_BUF_PAGE)
				     && (monitor_id < MONITOR_MODULE_OS)) {
					if (set_option == MONITOR_TURN_ON
					    && MONITOR_IS_ON(
						MONITOR_MODULE_BUF_PAGE)) {
						continue;
					}

					srv_mon_set_module_control(
						MONITOR_MODULE_BUF_PAGE,
						set_option);
				} else {
					/* If new monitor is added with
					MONITOR_GROUP_MODULE, it needs
					to be added here. */
					ut_ad(0);
				}
			}
		}
	}
}

/*************************************************************//**
Given a configuration variable name, find corresponding monitor counter
and return its monitor ID if found.
@return monitor ID if found, MONITOR_NO_MATCH if there is no match */
static
ulint
innodb_monitor_id_by_name_get(
/*==========================*/
	const char*	name)	/*!< in: monitor counter namer */
{
	ut_a(name);
	const Lex_ident_column ident = Lex_cstring_strlen(name);

	/* Search for wild character '%' in the name, if
	found, we treat it as a wildcard match. We do not search for
	single character wildcard '_' since our monitor names already contain
	such character. To avoid confusion, we request user must include
	at least one '%' character to activate the wildcard search. */
	if (strchr(name, '%')) {
		return(MONITOR_WILDCARD_MATCH);
	}

	/* Not wildcard match, check for an exact match */
	for (ulint i = 0; i < NUM_MONITOR; i++) {
		if (ident.streq(Lex_cstring_strlen(
			srv_mon_get_name(static_cast<monitor_id_t>(i))))) {
			return(i);
		}
	}

	return(MONITOR_NO_MATCH);
}
/*************************************************************//**
Validate that the passed in monitor name matches at least one
monitor counter name with wildcard compare.
@return TRUE if at least one monitor name matches */
static
ibool
innodb_monitor_validate_wildcard_name(
/*==================================*/
	const char*	name)	/*!< in: monitor counter namer */
{
	for (ulint i = 0; i < NUM_MONITOR; i++) {
		if (!innobase_wildcasecmp(
			srv_mon_get_name(static_cast<monitor_id_t>(i)), name)) {
			return(TRUE);
		}
	}

	return(FALSE);
}
/*************************************************************//**
Validate the passed in monitor name, find and save the
corresponding monitor name in the function parameter "save".
@return 0 if monitor name is valid */
static int innodb_monitor_valid_byname(const char *name)
{
	ulint		use;
	monitor_info_t*	monitor_info;

	if (!name) {
		return(1);
	}

	use = innodb_monitor_id_by_name_get(name);

	/* No monitor name matches, nor it is wildcard match */
	if (use == MONITOR_NO_MATCH) {
		return(1);
	}

	if (use < NUM_MONITOR) {
		monitor_info = srv_mon_get_info((monitor_id_t) use);

		/* If the monitor counter is marked with
		MONITOR_GROUP_MODULE flag, then this counter
		cannot be turned on/off individually, instead
		it shall be turned on/off as a group using
		its module name */
		if ((monitor_info->monitor_type & MONITOR_GROUP_MODULE)
		    && (!(monitor_info->monitor_type & MONITOR_MODULE))) {
			sql_print_warning(
				"Monitor counter '%s' cannot"
				" be turned on/off individually."
				" Please use its module name"
				" to turn on/off the counters"
				" in the module as a group.\n",
				name);

			return(1);
		}

	} else {
		ut_a(use == MONITOR_WILDCARD_MATCH);

		/* For wildcard match, if there is not a single monitor
		counter name that matches, treat it as an invalid
		value for the system configuration variables */
		if (!innodb_monitor_validate_wildcard_name(name)) {
			return(1);
		}
	}

	return(0);
}
/*************************************************************//**
Validate passed-in "value" is a valid monitor counter name.
This function is registered as a callback with MySQL.
@return 0 for valid name */
static
int
innodb_monitor_validate(
/*====================*/
	THD*, st_mysql_sys_var*,
	void*				save,	/*!< out: immediate result
						for update function */
	struct st_mysql_value*		value)	/*!< in: incoming string */
{
  int ret= 0;

  if (const char *name= value->val_str(value, nullptr, &ret))
  {
    ret= innodb_monitor_valid_byname(name);
    if (!ret)
      *static_cast<const char**>(save)= name;
  }
  else
    ret= 1;

  return ret;
}

/****************************************************************//**
Update the system variable innodb_enable(disable/reset/reset_all)_monitor
according to the "set_option" and turn on/off or reset specified monitor
counter. */
static
void
innodb_monitor_update(
/*==================*/
	THD*			thd,		/*!< in: thread handle */
	void*			var_ptr,	/*!< out: where the
						formal string goes */
	const void*		save,		/*!< in: immediate result
						from check function */
	mon_option_t		set_option)	/*!< in: the set option,
						whether to turn on/off or
						reset the counter */
{
	monitor_info_t*	monitor_info;
	ulint		monitor_id;
	ulint		err_monitor = 0;
	const char*	name;

	ut_a(save != NULL);

	name = *static_cast<const char*const*>(save);

	if (!name) {
		monitor_id = MONITOR_DEFAULT_START;
	} else {
		monitor_id = innodb_monitor_id_by_name_get(name);

		/* Double check we have a valid monitor ID */
		if (monitor_id == MONITOR_NO_MATCH) {
			return;
		}
	}

	if (monitor_id == MONITOR_DEFAULT_START) {
		/* If user set the variable to "default", we will
		print a message and make this set operation a "noop".
		The check is being made here is because "set default"
		does not go through validation function */
		if (thd) {
			push_warning_printf(
				thd, Sql_condition::WARN_LEVEL_WARN,
				ER_NO_DEFAULT,
				"Default value is not defined for"
				" this set option. Please specify"
				" correct counter or module name.");
		} else {
			sql_print_error(
				"Default value is not defined for"
				" this set option. Please specify"
				" correct counter or module name.\n");
		}

		if (var_ptr) {
			*(const char**) var_ptr = NULL;
		}
	} else if (monitor_id == MONITOR_WILDCARD_MATCH) {
		innodb_monitor_update_wildcard(name, set_option);
	} else {
		monitor_info = srv_mon_get_info(
			static_cast<monitor_id_t>(monitor_id));

		ut_a(monitor_info);

		/* If monitor is already truned on, someone could already
		collect monitor data, exit and ask user to turn off the
		monitor before turn it on again. */
		if (set_option == MONITOR_TURN_ON
		    && MONITOR_IS_ON(monitor_id)) {
			err_monitor = monitor_id;
			goto exit;
		}

		if (var_ptr) {
			*(const char**) var_ptr = monitor_info->monitor_name;
		}

		/* Depending on the monitor name is for a module or
		a counter, process counters in the whole module or
		individual counter. */
		if (monitor_info->monitor_type & MONITOR_MODULE) {
			srv_mon_set_module_control(
				static_cast<monitor_id_t>(monitor_id),
				set_option);
		} else {
			innodb_monitor_set_option(monitor_info, set_option);
		}
	}
exit:
	/* Only if we are trying to turn on a monitor that already
	been turned on, we will set err_monitor. Print related
	information */
	if (err_monitor) {
		sql_print_warning("InnoDB: Monitor %s is already enabled.",
				  srv_mon_get_name((monitor_id_t) err_monitor));
	}
}

#ifdef UNIV_DEBUG
static char* srv_buffer_pool_evict;

/****************************************************************//**
Evict all uncompressed pages of compressed tables from the buffer pool.
Keep the compressed pages in the buffer pool.
@return whether all uncompressed pages were evicted */
static bool innodb_buffer_pool_evict_uncompressed()
{
	bool	all_evicted = true;

	mysql_mutex_lock(&buf_pool.mutex);

	for (buf_block_t* block = UT_LIST_GET_LAST(buf_pool.unzip_LRU);
	     block != NULL; ) {
		buf_block_t*	prev_block = UT_LIST_GET_PREV(unzip_LRU, block);
		ut_ad(block->page.in_file());
		ut_ad(block->page.belongs_to_unzip_LRU());
		ut_ad(block->in_unzip_LRU_list);
		ut_ad(block->page.in_LRU_list);

		if (!buf_LRU_free_page(&block->page, false)) {
			all_evicted = false;
			block = prev_block;
		} else {
			/* Because buf_LRU_free_page() may release
			and reacquire buf_pool.mutex, prev_block
			may be invalid. */
			block = UT_LIST_GET_LAST(buf_pool.unzip_LRU);
		}
	}

	mysql_mutex_unlock(&buf_pool.mutex);
	return(all_evicted);
}

/****************************************************************//**
Called on SET GLOBAL innodb_buffer_pool_evict=...
Handles some values specially, to evict pages from the buffer pool.
SET GLOBAL innodb_buffer_pool_evict='uncompressed'
evicts all uncompressed page frames of compressed tablespaces. */
static
void
innodb_buffer_pool_evict_update(THD*, st_mysql_sys_var*, void*,
				const void* save)
{
	if (const char* op = *static_cast<const char*const*>(save)) {
		if (!strcmp(op, "uncompressed")) {
			mysql_mutex_unlock(&LOCK_global_system_variables);
			for (uint tries = 0; tries < 10000; tries++) {
				if (innodb_buffer_pool_evict_uncompressed()) {
					mysql_mutex_lock(
						&LOCK_global_system_variables);
					return;
				}

				std::this_thread::sleep_for(
					std::chrono::milliseconds(10));
			}

			/* We failed to evict all uncompressed pages. */
			ut_ad(0);
		}
	}
}
#endif /* UNIV_DEBUG */

/****************************************************************//**
Update the system variable innodb_monitor_enable and enable
specified monitor counter.
This function is registered as a callback with MySQL. */
static
void
innodb_enable_monitor_update(
/*=========================*/
	THD*				thd,	/*!< in: thread handle */
	st_mysql_sys_var*,
	void*				var_ptr,/*!< out: where the
						formal string goes */
	const void*			save)	/*!< in: immediate result
						from check function */
{
	innodb_monitor_update(thd, var_ptr, save, MONITOR_TURN_ON);
}

/****************************************************************//**
Update the system variable innodb_monitor_disable and turn
off specified monitor counter. */
static
void
innodb_disable_monitor_update(
/*==========================*/
	THD*				thd,	/*!< in: thread handle */
	st_mysql_sys_var*,
	void*				var_ptr,/*!< out: where the
						formal string goes */
	const void*			save)	/*!< in: immediate result
						from check function */
{
	innodb_monitor_update(thd, var_ptr, save, MONITOR_TURN_OFF);
}

/****************************************************************//**
Update the system variable innodb_monitor_reset and reset
specified monitor counter(s).
This function is registered as a callback with MySQL. */
static
void
innodb_reset_monitor_update(
/*========================*/
	THD*				thd,	/*!< in: thread handle */
	st_mysql_sys_var*,
	void*				var_ptr,/*!< out: where the
						formal string goes */
	const void*			save)	/*!< in: immediate result
						from check function */
{
	innodb_monitor_update(thd, var_ptr, save, MONITOR_RESET_VALUE);
}

/****************************************************************//**
Update the system variable innodb_monitor_reset_all and reset
all value related monitor counter.
This function is registered as a callback with MySQL. */
static
void
innodb_reset_all_monitor_update(
/*============================*/
	THD*				thd,	/*!< in: thread handle */
	st_mysql_sys_var*,
	void*				var_ptr,/*!< out: where the
						formal string goes */
	const void*			save)	/*!< in: immediate result
						from check function */
{
	innodb_monitor_update(thd, var_ptr, save, MONITOR_RESET_ALL_VALUE);
}

static inline char *my_strtok_r(char *str, const char *delim, char **saveptr)
{
#if defined _WIN32
	return strtok_s(str, delim, saveptr);
#else
	return strtok_r(str, delim, saveptr);
#endif
}

/****************************************************************//**
Parse and enable InnoDB monitor counters during server startup.
User can list the monitor counters/groups to be enable by specifying
"loose-innodb_monitor_enable=monitor_name1;monitor_name2..."
in server configuration file or at the command line. The string
separate could be ";", "," or empty space. */
static
void
innodb_enable_monitor_at_startup(
/*=============================*/
	char*	str)	/*!< in/out: monitor counter enable list */
{
	static const char*	sep = " ;,";
	char*			last;

	ut_a(str);

	/* Walk through the string, and separate each monitor counter
	and/or counter group name, and calling innodb_monitor_update()
	if successfully updated. Please note that the "str" would be
	changed by strtok_r() as it walks through it. */
	for (char* option = my_strtok_r(str, sep, &last);
	     option;
	     option = my_strtok_r(NULL, sep, &last)) {
		if (!innodb_monitor_valid_byname(option)) {
			innodb_monitor_update(NULL, NULL, &option,
					      MONITOR_TURN_ON);
		} else {
			sql_print_warning("Invalid monitor counter"
					  " name: '%s'", option);
		}
	}
}

/****************************************************************//**
Callback function for accessing the InnoDB variables from MySQL:
SHOW VARIABLES. */
static int show_innodb_vars(THD*, SHOW_VAR* var, void *,
                            struct system_status_var *status_var,
                            enum enum_var_type var_type)
{
	innodb_export_status();
	var->type = SHOW_ARRAY;
	var->value = (char*) &innodb_status_variables;
	//var->scope = SHOW_SCOPE_GLOBAL;

	return(0);
}

/****************************************************************//**
This function checks each index name for a table against reserved
system default primary index name 'GEN_CLUST_INDEX'. If a name
matches, this function pushes an warning message to the client,
and returns true.
@return true if the index name matches the reserved name */
bool
innobase_index_name_is_reserved(
/*============================*/
	THD*		thd,		/*!< in/out: MySQL connection */
	const KEY*	key_info,	/*!< in: Indexes to be created */
	ulint		num_of_keys)	/*!< in: Number of indexes to
					be created. */
{
	const KEY*	key;
	uint		key_num;	/* index number */

	for (key_num = 0; key_num < num_of_keys; key_num++) {
		key = &key_info[key_num];

		if (key->name.streq(GEN_CLUST_INDEX)) {
			/* Push warning to mysql */
			push_warning_printf(thd,
					    Sql_condition::WARN_LEVEL_WARN,
					    ER_WRONG_NAME_FOR_INDEX,
					    "Cannot Create Index with name"
					    " '%s'. The name is reserved"
					    " for the system default primary"
					    " index.",
					    GEN_CLUST_INDEX.str);

			my_error(ER_WRONG_NAME_FOR_INDEX, MYF(0),
				 GEN_CLUST_INDEX.str);

			return(true);
		}
	}

	return(false);
}

/** Retrieve the FTS Relevance Ranking result for doc with doc_id
of m_prebuilt->fts_doc_id
@param[in,out]	fts_hdl	FTS handler
@return the relevance ranking value */
static
float
innobase_fts_retrieve_ranking(
	FT_INFO*	fts_hdl)
{
	fts_result_t*	result;
	row_prebuilt_t*	ft_prebuilt;

	result = reinterpret_cast<NEW_FT_INFO*>(fts_hdl)->ft_result;

	ft_prebuilt = reinterpret_cast<NEW_FT_INFO*>(fts_hdl)->ft_prebuilt;

	fts_ranking_t*  ranking = rbt_value(fts_ranking_t, result->current);
	ft_prebuilt->fts_doc_id= ranking->doc_id;

	return(ranking->rank);
}

/** Free the memory for the FTS handler
@param[in,out]	fts_hdl	FTS handler */
static
void
innobase_fts_close_ranking(
	FT_INFO*	fts_hdl)
{
	fts_result_t*	result;

	result = reinterpret_cast<NEW_FT_INFO*>(fts_hdl)->ft_result;

	fts_query_free_result(result);

	my_free((uchar*) fts_hdl);
}

/** Find and Retrieve the FTS Relevance Ranking result for doc with doc_id
of m_prebuilt->fts_doc_id
@param[in,out]	fts_hdl	FTS handler
@return the relevance ranking value */
static
float
innobase_fts_find_ranking(FT_INFO* fts_hdl, uchar*, uint)
{
	fts_result_t*	result;
	row_prebuilt_t*	ft_prebuilt;

	ft_prebuilt = reinterpret_cast<NEW_FT_INFO*>(fts_hdl)->ft_prebuilt;
	result = reinterpret_cast<NEW_FT_INFO*>(fts_hdl)->ft_result;

	/* Retrieve the ranking value for doc_id with value of
	m_prebuilt->fts_doc_id */
	return(fts_retrieve_ranking(result, ft_prebuilt->fts_doc_id));
}

/** Update a field that is protected by buf_pool.mutex */
template<typename T>
static void innodb_buf_pool_update(THD *thd, st_mysql_sys_var *,
                                   void *val, const void *save)
{
  mysql_mutex_lock(&buf_pool.mutex);
  *static_cast<T*>(val)= *static_cast<const T*>(save);
  mysql_mutex_unlock(&buf_pool.mutex);
}

static my_bool innodb_log_checkpoint_now;
#ifdef UNIV_DEBUG
static my_bool	innodb_buf_flush_list_now = TRUE;
static uint	innodb_merge_threshold_set_all_debug
	= DICT_INDEX_MERGE_THRESHOLD_DEFAULT;
#endif

/** Force an InnoDB log checkpoint. */
static
void
checkpoint_now_set(THD* thd, st_mysql_sys_var*, void*, const void *save)
{
  if (!*static_cast<const my_bool*>(save))
    return;

  if (srv_read_only_mode)
  {
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                        HA_ERR_UNSUPPORTED,
                        "InnoDB doesn't force checkpoint "
                        "when %s",
                        (srv_force_recovery
                         == SRV_FORCE_NO_LOG_REDO)
                        ? "innodb-force-recovery=6."
                        : "innodb-read-only=1.");
    return;
  }

  const auto size= log_sys.is_encrypted()
    ? SIZE_OF_FILE_CHECKPOINT + 8 : SIZE_OF_FILE_CHECKPOINT;
  mysql_mutex_unlock(&LOCK_global_system_variables);
  while (!thd_kill_level(thd))
  {
    log_sys.latch.wr_lock(SRW_LOCK_CALL);
    lsn_t cp= log_sys.last_checkpoint_lsn.load(std::memory_order_relaxed),
      lsn= log_sys.get_lsn();
    log_sys.latch.wr_unlock();
    if (cp + size >= lsn)
      break;
    log_make_checkpoint();
  }

  mysql_mutex_lock(&LOCK_global_system_variables);
}

#ifdef UNIV_DEBUG
/****************************************************************//**
Force a dirty pages flush now. */
static
void
buf_flush_list_now_set(THD*, st_mysql_sys_var*, void*, const void* save)
{
  if (!*(my_bool*) save)
    return;
  const uint s= srv_fil_make_page_dirty_debug;
  mysql_mutex_unlock(&LOCK_global_system_variables);
  if (s == 0 || srv_is_undo_tablespace(s))
  {
    fil_space_t *space= fil_system.sys_space;
    if (s) { space= fil_space_get(s); }
    while (buf_flush_list_space(space, nullptr));
    os_aio_wait_until_no_pending_writes(true);
  }
  else
    buf_flush_sync();
  mysql_mutex_lock(&LOCK_global_system_variables);
}

/** Override current MERGE_THRESHOLD setting for all indexes at dictionary
now.
@param[in]	save	immediate result from check function */
static
void
innodb_merge_threshold_set_all_debug_update(THD*, st_mysql_sys_var*, void*,
					    const void* save)
{
	innodb_merge_threshold_set_all_debug
		= (*static_cast<const uint*>(save));
	dict_set_merge_threshold_all_debug(
		innodb_merge_threshold_set_all_debug);
}
#endif /* UNIV_DEBUG */

/** Find and Retrieve the FTS doc_id for the current result row
@param[in,out]	fts_hdl	FTS handler
@return the document ID */
static
ulonglong
innobase_fts_retrieve_docid(
	FT_INFO_EXT*	fts_hdl)
{
	fts_result_t*	result;
	row_prebuilt_t* ft_prebuilt;

	ft_prebuilt = reinterpret_cast<NEW_FT_INFO *>(fts_hdl)->ft_prebuilt;
	result = reinterpret_cast<NEW_FT_INFO *>(fts_hdl)->ft_result;

	if (ft_prebuilt->read_just_key) {

		fts_ranking_t* ranking =
			rbt_value(fts_ranking_t, result->current);

		return(ranking->doc_id);
	}

	return(ft_prebuilt->fts_doc_id);
}

/* These variables are never read by InnoDB or changed. They are a kind of
dummies that are needed by the MySQL infrastructure to call
buffer_pool_dump_now(), buffer_pool_load_now() and buffer_pool_load_abort()
by the user by doing:
  SET GLOBAL innodb_buffer_pool_dump_now=ON;
  SET GLOBAL innodb_buffer_pool_load_now=ON;
  SET GLOBAL innodb_buffer_pool_load_abort=ON;
Their values are read by MySQL and displayed to the user when the variables
are queried, e.g.:
  SELECT @@innodb_buffer_pool_dump_now;
  SELECT @@innodb_buffer_pool_load_now;
  SELECT @@innodb_buffer_pool_load_abort; */
static my_bool	innodb_buffer_pool_dump_now = FALSE;
static my_bool	innodb_buffer_pool_load_now = FALSE;
static my_bool	innodb_buffer_pool_load_abort = FALSE;

/****************************************************************//**
Trigger a dump of the buffer pool if innodb_buffer_pool_dump_now is set
to ON. This function is registered as a callback with MySQL. */
static
void
buffer_pool_dump_now(
/*=================*/
	THD*				thd	/*!< in: thread handle */
					MY_ATTRIBUTE((unused)),
	struct st_mysql_sys_var*	var	/*!< in: pointer to system
						variable */
					MY_ATTRIBUTE((unused)),
	void*				var_ptr	/*!< out: where the formal
						string goes */
					MY_ATTRIBUTE((unused)),
	const void*			save)	/*!< in: immediate result from
						check function */
{
	if (*(my_bool*) save && !srv_read_only_mode) {
		mysql_mutex_unlock(&LOCK_global_system_variables);
		buf_dump_start();
		mysql_mutex_lock(&LOCK_global_system_variables);
	}
}

/****************************************************************//**
Trigger a load of the buffer pool if innodb_buffer_pool_load_now is set
to ON. This function is registered as a callback with MySQL. */
static
void
buffer_pool_load_now(
/*=================*/
	THD*				thd	/*!< in: thread handle */
					MY_ATTRIBUTE((unused)),
	struct st_mysql_sys_var*	var	/*!< in: pointer to system
						variable */
					MY_ATTRIBUTE((unused)),
	void*				var_ptr	/*!< out: where the formal
						string goes */
					MY_ATTRIBUTE((unused)),
	const void*			save)	/*!< in: immediate result from
						check function */
{
	if (*(my_bool*) save && !srv_read_only_mode) {
		mysql_mutex_unlock(&LOCK_global_system_variables);
		buf_load_start();
		mysql_mutex_lock(&LOCK_global_system_variables);
	}
}

/****************************************************************//**
Abort a load of the buffer pool if innodb_buffer_pool_load_abort
is set to ON. This function is registered as a callback with MySQL. */
static
void
buffer_pool_load_abort(
/*===================*/
	THD*				thd	/*!< in: thread handle */
					MY_ATTRIBUTE((unused)),
	struct st_mysql_sys_var*	var	/*!< in: pointer to system
						variable */
					MY_ATTRIBUTE((unused)),
	void*				var_ptr	/*!< out: where the formal
						string goes */
					MY_ATTRIBUTE((unused)),
	const void*			save)	/*!< in: immediate result from
						check function */
{
	if (*(my_bool*) save && !srv_read_only_mode) {
		mysql_mutex_unlock(&LOCK_global_system_variables);
		buf_load_abort();
		mysql_mutex_lock(&LOCK_global_system_variables);
	}
}

#if defined __linux__ || defined _WIN32
static void innodb_log_file_buffering_update(THD *, st_mysql_sys_var*,
                                             void *, const void *save)
{
  mysql_mutex_unlock(&LOCK_global_system_variables);
  log_sys.set_buffered(*static_cast<const my_bool*>(save));
  mysql_mutex_lock(&LOCK_global_system_variables);
}
#endif

static void innodb_log_file_write_through_update(THD *, st_mysql_sys_var*,
                                                 void *, const void *save)
{
  mysql_mutex_unlock(&LOCK_global_system_variables);
  log_sys.set_write_through(*static_cast<const my_bool*>(save));
  mysql_mutex_lock(&LOCK_global_system_variables);
}

static void innodb_data_file_buffering_update(THD *, st_mysql_sys_var*,
                                              void *, const void *save)
{
  mysql_mutex_unlock(&LOCK_global_system_variables);
  fil_system.set_buffered(*static_cast<const my_bool*>(save));
  mysql_mutex_lock(&LOCK_global_system_variables);
}

static void innodb_data_file_write_through_update(THD *, st_mysql_sys_var*,
                                                  void *, const void *save)
{
  mysql_mutex_unlock(&LOCK_global_system_variables);
  fil_system.set_write_through(*static_cast<const my_bool*>(save));
  mysql_mutex_lock(&LOCK_global_system_variables);
}

static void innodb_doublewrite_update(THD *, st_mysql_sys_var*,
                                      void *, const void *save)
{
  if (!srv_read_only_mode)
    fil_system.set_use_doublewrite(*static_cast<const ulong*>(save));
}

static void innodb_log_file_size_update(THD *thd, st_mysql_sys_var*,
                                        void *var, const void *save)
{
  ut_ad(var == &srv_log_file_size);
  mysql_mutex_unlock(&LOCK_global_system_variables);

  if (high_level_read_only)
    ib_senderrf(thd, IB_LOG_LEVEL_ERROR, ER_READ_ONLY_MODE);
  else if (
#ifdef HAVE_PMEM
          !log_sys.is_mmap() &&
#endif
           *static_cast<const ulonglong*>(save) < log_sys.buf_size)
    my_printf_error(ER_WRONG_ARGUMENTS,
                    "innodb_log_file_size must be at least"
                    " innodb_log_buffer_size=%u", MYF(0), log_sys.buf_size);
  else
  {
    switch (log_sys.resize_start(*static_cast<const ulonglong*>(save), thd)) {
    case log_t::RESIZE_NO_CHANGE:
      break;
    case log_t::RESIZE_IN_PROGRESS:
      my_printf_error(ER_WRONG_USAGE,
                      "innodb_log_file_size change is already in progress",
                      MYF(0));
      break;
    case log_t::RESIZE_FAILED:
      ib_senderrf(thd, IB_LOG_LEVEL_ERROR, ER_CANT_CREATE_HANDLER_FILE);
      break;
    case log_t::RESIZE_STARTED:
      for (timespec abstime;;)
      {
        if (thd_kill_level(thd))
        {
          log_sys.resize_abort(thd);
          break;
        }

        set_timespec(abstime, 5);
        mysql_mutex_lock(&buf_pool.flush_list_mutex);
        lsn_t resizing= log_sys.resize_in_progress();
        if (resizing > buf_pool.get_oldest_modification(0))
        {
          buf_pool.page_cleaner_wakeup(true);
          my_cond_timedwait(&buf_pool.done_flush_list,
                            &buf_pool.flush_list_mutex.m_mutex, &abstime);
          resizing= log_sys.resize_in_progress();
        }
        mysql_mutex_unlock(&buf_pool.flush_list_mutex);
        if (!resizing || !log_sys.resize_running(thd))
          break;
        log_sys.latch.wr_lock(SRW_LOCK_CALL);
        while (resizing > log_sys.get_lsn())
        {
          ut_ad(!log_sys.is_mmap());
          /* The server is almost idle. Write dummy FILE_CHECKPOINT records
          to ensure that the log resizing will complete. */
          mtr_t mtr;
          mtr.start();
          mtr.commit_files(log_sys.last_checkpoint_lsn);
        }
        log_sys.latch.wr_unlock();
      }
    }
  }
  mysql_mutex_lock(&LOCK_global_system_variables);
}

/** Update innodb_status_output or innodb_status_output_locks,
which control InnoDB "status monitor" output to the error log.
@param[out]	var	current value
@param[in]	save	to-be-assigned value */
static
void
innodb_status_output_update(THD*,st_mysql_sys_var*,void*var,const void*save)
{
  if (srv_monitor_timer)
  {
    *static_cast<my_bool*>(var)= *static_cast<const my_bool*>(save);
    mysql_mutex_unlock(&LOCK_global_system_variables);
    /* Wakeup server monitor. */
    srv_monitor_timer_schedule_now();
    mysql_mutex_lock(&LOCK_global_system_variables);
  }
}

/** Update the system variable innodb_encryption_threads.
@param[in]	save	to-be-assigned value */
static
void
innodb_encryption_threads_update(THD*,st_mysql_sys_var*,void*,const void*save)
{
	mysql_mutex_unlock(&LOCK_global_system_variables);
	fil_crypt_set_thread_cnt(*static_cast<const uint*>(save));
	mysql_mutex_lock(&LOCK_global_system_variables);
}

/** Update the system variable innodb_encryption_rotate_key_age.
@param[in]	save	to-be-assigned value */
static
void
innodb_encryption_rotate_key_age_update(THD*, st_mysql_sys_var*, void*,
					const void* save)
{
	mysql_mutex_unlock(&LOCK_global_system_variables);
	fil_crypt_set_rotate_key_age(*static_cast<const uint*>(save));
	mysql_mutex_lock(&LOCK_global_system_variables);
}

/** Update the system variable innodb_encryption_rotation_iops.
@param[in]	save	to-be-assigned value */
static
void
innodb_encryption_rotation_iops_update(THD*, st_mysql_sys_var*, void*,
				       const void* save)
{
	mysql_mutex_unlock(&LOCK_global_system_variables);
	fil_crypt_set_rotation_iops(*static_cast<const uint*>(save));
	mysql_mutex_lock(&LOCK_global_system_variables);
}

/** Update the system variable innodb_encrypt_tables.
@param[in]	save	to-be-assigned value */
static
void
innodb_encrypt_tables_update(THD*, st_mysql_sys_var*, void*, const void* save)
{
	mysql_mutex_unlock(&LOCK_global_system_variables);
	fil_crypt_set_encrypt_tables(*static_cast<const ulong*>(save));
	mysql_mutex_lock(&LOCK_global_system_variables);
}

/** Truncate the temporary tablespace if the
innodb_truncate_temporary_tablespace_now is enabled.
@param  save  to-be-assigned value */
static
void
innodb_trunc_temp_space_update(THD*, st_mysql_sys_var*, void*, const void* save)
{
  /* Temp tablespace is not initialized in read only mode. */
  if (!*static_cast<const my_bool*>(save) || srv_read_only_mode)
    return;
  mysql_mutex_unlock(&LOCK_global_system_variables);
  fsp_shrink_temp_space();
  mysql_mutex_lock(&LOCK_global_system_variables);
}

static SHOW_VAR innodb_status_variables_export[]= {
	SHOW_FUNC_ENTRY("Innodb", &show_innodb_vars),
	{NullS, NullS, SHOW_LONG}
};

static struct st_mysql_storage_engine innobase_storage_engine=
{ MYSQL_HANDLERTON_INTERFACE_VERSION };

#ifdef WITH_WSREP
/** Request a transaction to be killed that holds a conflicting lock.
@param bf_trx    brute force applier transaction
@param thd_id    thd_get_thread_id(victim_trx->mysql_htd)
@param trx_id    victim_trx->id */
void lock_wait_wsrep_kill(trx_t *bf_trx, ulong thd_id, trx_id_t trx_id)
{
  THD *bf_thd= bf_trx->mysql_thd;

  if (THD *vthd= find_thread_by_id(thd_id))
  {
    bool aborting= false;
    wsrep_thd_LOCK(vthd);
    trx_t *vtrx= thd_to_trx(vthd);
    if (vtrx)
    {
      /* Do not bother with lock elision using transactional memory here;
      this is rather complex code */
      LockMutexGuard g{SRW_LOCK_CALL};
      mysql_mutex_lock(&lock_sys.wait_mutex);
      vtrx->mutex_lock();
      /* victim transaction is either active or prepared, if it has already
	 proceeded to replication phase */
      if (vtrx->id == trx_id)
      {
        switch (vtrx->state) {
        default:
          break;
        case TRX_STATE_PREPARED:
          if (!wsrep_is_wsrep_xid(&vtrx->xid))
            break;
          /* fall through */
        case TRX_STATE_ACTIVE:
          WSREP_LOG_CONFLICT(bf_thd, vthd, TRUE);
          WSREP_DEBUG("Aborter BF trx_id: " TRX_ID_FMT " thread: %ld "
                      "seqno: %lld client_state: %s "
                      "client_mode: %s transaction_mode: %s query: %s",
                      bf_trx->id,
                      thd_get_thread_id(bf_thd),
                      wsrep_thd_trx_seqno(bf_thd),
                      wsrep_thd_client_state_str(bf_thd),
                      wsrep_thd_client_mode_str(bf_thd),
                      wsrep_thd_transaction_state_str(bf_thd),
                      wsrep_thd_query(bf_thd));
          WSREP_DEBUG("Victim %s trx_id: " TRX_ID_FMT " thread: %ld "
                      "seqno: %lld client_state: %s "
                      "client_mode: %s transaction_mode: %s query: %s",
                      wsrep_thd_is_BF(vthd, false) ? "BF" : "normal",
                      vtrx->id,
                      thd_get_thread_id(vthd),
                      wsrep_thd_trx_seqno(vthd),
                      wsrep_thd_client_state_str(vthd),
                      wsrep_thd_client_mode_str(vthd),
                      wsrep_thd_transaction_state_str(vthd),
                      wsrep_thd_query(vthd));
          aborting= true;
        }
      }
      mysql_mutex_unlock(&lock_sys.wait_mutex);
      vtrx->mutex_unlock();
    }

    DEBUG_SYNC(bf_thd, "before_wsrep_thd_abort");
    if (aborting && wsrep_thd_bf_abort(bf_thd, vthd, true))
    {
      /* Need to grab mutexes again to ensure that the trx is still in
         right state. */
      lock_sys.wr_lock(SRW_LOCK_CALL);
      mysql_mutex_lock(&lock_sys.wait_mutex);
      vtrx->mutex_lock();

      /* if victim is waiting for some other lock, we have to cancel
         that waiting
      */
      if (vtrx->id == trx_id)
      {
        switch (vtrx->state) {
        default:
          break;
        case TRX_STATE_ACTIVE:
        case TRX_STATE_PREPARED:
          lock_sys.cancel_lock_wait_for_wsrep_bf_abort(vtrx);
        }
      }
      lock_sys.wr_unlock();
      mysql_mutex_unlock(&lock_sys.wait_mutex);
      vtrx->mutex_unlock();
    }
    else
    {
      WSREP_DEBUG("wsrep_thd_bf_abort has failed, victim %lu will survive",
                  thd_get_thread_id(vthd));
    }
    wsrep_thd_UNLOCK(vthd);
    wsrep_thd_kill_UNLOCK(vthd);
  }

#ifdef ENABLED_DEBUG_SYNC
    DBUG_EXECUTE_IF(
        "wsrep_after_kill",
        {const char act[]=
             "now "
             "SIGNAL wsrep_after_kill_reached "
             "WAIT_FOR wsrep_after_kill_continue";
          DBUG_ASSERT(!debug_sync_set_action(bf_thd, STRING_WITH_LEN(act)));
        };);
    DBUG_EXECUTE_IF(
        "wsrep_after_kill_2",
        {const char act2[]=
             "now "
             "SIGNAL wsrep_after_kill_reached_2 "
             "WAIT_FOR wsrep_after_kill_continue_2";
          DBUG_ASSERT(!debug_sync_set_action(bf_thd, STRING_WITH_LEN(act2)));
        };);
#endif /* ENABLED_DEBUG_SYNC*/
}

/** This function forces the victim transaction to abort. Aborting the
  transaction does NOT end it, it still has to be rolled back.

  The caller must lock LOCK_thd_kill and LOCK_thd_data.

  @param bf_thd       brute force THD asking for the abort
  @param victim_thd   victim THD to be aborted
*/
static void wsrep_abort_transaction(handlerton *, THD *bf_thd, THD *victim_thd,
                                    my_bool signal)
{
  DBUG_ENTER("wsrep_abort_transaction");
  ut_ad(bf_thd);
  ut_ad(victim_thd);

  trx_t *victim_trx= thd_to_trx(victim_thd);

  WSREP_DEBUG("abort transaction: BF: %s victim: %s victim conf: %s",
              wsrep_thd_query(bf_thd), wsrep_thd_query(victim_thd),
              wsrep_thd_transaction_state_str(victim_thd));

  if (!victim_trx)
  {
    WSREP_DEBUG("abort transaction: victim did not exist");
    DBUG_VOID_RETURN;
  }

  lock_sys.wr_lock(SRW_LOCK_CALL);
  mysql_mutex_lock(&lock_sys.wait_mutex);
  victim_trx->mutex_lock();

  switch (victim_trx->state) {
  default:
    break;
  case TRX_STATE_ACTIVE:
  case TRX_STATE_PREPARED:
    /* Cancel lock wait if the victim is waiting for a lock in InnoDB.
       The transaction which is blocked somewhere else (e.g. waiting
       for next command or MDL) has been interrupted by THD::awake_no_mutex()
       on server level before calling this function. */
    lock_sys.cancel_lock_wait_for_wsrep_bf_abort(victim_trx);
  }
  lock_sys.wr_unlock();
  mysql_mutex_unlock(&lock_sys.wait_mutex);
  victim_trx->mutex_unlock();

  DBUG_VOID_RETURN;
}

static
int
innobase_wsrep_set_checkpoint(
/*==========================*/
	handlerton* hton,
	const XID* xid)
{
	DBUG_ASSERT(hton == innodb_hton_ptr);

	if (wsrep_is_wsrep_xid(xid)) {

		trx_rseg_update_wsrep_checkpoint(xid);
		log_buffer_flush_to_disk(srv_flush_log_at_trx_commit == 1);
		return 0;
	} else {
		return 1;
	}
}

static
int
innobase_wsrep_get_checkpoint(
/*==========================*/
	handlerton* hton,
	XID* xid)
{
	DBUG_ASSERT(hton == innodb_hton_ptr);
        trx_rseg_read_wsrep_checkpoint(*xid);
        return 0;
}
#endif /* WITH_WSREP */

/* plugin options */

static MYSQL_SYSVAR_ENUM(checksum_algorithm, srv_checksum_algorithm,
  PLUGIN_VAR_RQCMDARG,
  "The algorithm InnoDB uses for page checksumming. Possible values are"
  " FULL_CRC32"
    " for new files, always use CRC-32C; for old, see CRC32 below;"
  " STRICT_FULL_CRC32"
    " for new files, always use CRC-32C; for old, see STRICT_CRC32 below;"
  " CRC32"
    " write crc32, allow previously used algorithms to match when reading;"
  " STRICT_CRC32"
    " write crc32, do not allow other algorithms to match when reading;"
  " New files created with full_crc32 are readable by MariaDB 10.4.3+",
  NULL, NULL, SRV_CHECKSUM_ALGORITHM_FULL_CRC32,
  &innodb_checksum_algorithm_typelib);

static MYSQL_SYSVAR_STR(data_home_dir, innobase_data_home_dir,
  PLUGIN_VAR_READONLY,
  "The common part for InnoDB table spaces",
  NULL, NULL, NULL);

static MYSQL_SYSVAR_ENUM(doublewrite, buf_dblwr.use,
  PLUGIN_VAR_OPCMDARG,
  "Whether and how to use the doublewrite buffer. "
  "OFF=Assume that writes of innodb_page_size are atomic; "
  "ON=Prevent torn writes (the default); "
  "fast=Like ON, but do not synchronize writes to data files",
  nullptr, innodb_doublewrite_update, true,
  &innodb_doublewrite_typelib);

static MYSQL_SYSVAR_BOOL(use_atomic_writes, srv_use_atomic_writes,
  PLUGIN_VAR_NOCMDARG | PLUGIN_VAR_READONLY,
  "Enable atomic writes, instead of using the doublewrite buffer, for files "
  "on devices that supports atomic writes",
  NULL, NULL, TRUE);

static MYSQL_SYSVAR_BOOL(stats_include_delete_marked,
  srv_stats_include_delete_marked,
  PLUGIN_VAR_OPCMDARG,
  "Include delete marked records when calculating persistent statistics",
  NULL, NULL, FALSE);

static MYSQL_SYSVAR_ENUM(instant_alter_column_allowed,
			 innodb_instant_alter_column_allowed,
  PLUGIN_VAR_RQCMDARG,
  "File format constraint for ALTER TABLE", NULL, NULL, 2/*add_drop_reorder*/,
  &innodb_instant_alter_column_allowed_typelib);

static MYSQL_SYSVAR_ULONG(io_capacity, srv_io_capacity,
  PLUGIN_VAR_RQCMDARG,
  "Number of IOPs the server can do. Tunes the background IO rate",
  NULL, innodb_io_capacity_update, 200, 100, SRV_MAX_IO_CAPACITY_LIMIT, 0);

static MYSQL_SYSVAR_ULONG(io_capacity_max, srv_max_io_capacity,
  PLUGIN_VAR_RQCMDARG,
  "Limit to which innodb_io_capacity can be inflated",
  NULL, innodb_io_capacity_max_update,
  SRV_MAX_IO_CAPACITY_DUMMY_DEFAULT, 100,
  SRV_MAX_IO_CAPACITY_LIMIT, 0);

static MYSQL_SYSVAR_BOOL(log_checkpoint_now, innodb_log_checkpoint_now,
  PLUGIN_VAR_OPCMDARG,
  "Write back dirty pages from the buffer pool and update the log checkpoint",
  NULL, checkpoint_now_set, FALSE);

#ifdef UNIV_DEBUG
static MYSQL_SYSVAR_BOOL(buf_flush_list_now, innodb_buf_flush_list_now,
  PLUGIN_VAR_OPCMDARG,
  "Force dirty page flush now",
  NULL, buf_flush_list_now_set, FALSE);

static MYSQL_SYSVAR_UINT(merge_threshold_set_all_debug,
  innodb_merge_threshold_set_all_debug,
  PLUGIN_VAR_RQCMDARG,
  "Override current MERGE_THRESHOLD setting for all indexes at dictionary"
  " cache by the specified value dynamically, at the time",
  NULL, innodb_merge_threshold_set_all_debug_update,
  DICT_INDEX_MERGE_THRESHOLD_DEFAULT, 1, 50, 0);
#endif /* UNIV_DEBUG */

static MYSQL_SYSVAR_ULONG(purge_batch_size, srv_purge_batch_size,
  PLUGIN_VAR_OPCMDARG,
  "Number of UNDO log pages to purge in one batch from the history list",
  NULL, NULL,
  127,			/* Default setting */
  1,			/* Minimum value */
  innodb_purge_batch_size_MAX, 0);

extern void srv_update_purge_thread_count(uint n);

static
void
innodb_purge_threads_update(THD*, struct st_mysql_sys_var*, void*, const void*save )
{
  srv_update_purge_thread_count(*static_cast<const uint*>(save));
}

static MYSQL_SYSVAR_UINT(purge_threads, srv_n_purge_threads,
  PLUGIN_VAR_OPCMDARG,
  "Number of tasks for purging transaction history",
  NULL, innodb_purge_threads_update,
  4,			    /* Default setting */
  1,			    /* Minimum value */
  innodb_purge_threads_MAX, /* Maximum value */
  0);

static MYSQL_SYSVAR_UINT(fast_shutdown, srv_fast_shutdown,
  PLUGIN_VAR_OPCMDARG,
  "Speeds up the shutdown process of the InnoDB storage engine. Possible"
  " values are 0, 1 (faster), 2 (crash-like), 3 (fastest clean)",
  fast_shutdown_validate, NULL, 1, 0, 3, 0);

static MYSQL_SYSVAR_BOOL(file_per_table, srv_file_per_table,
  PLUGIN_VAR_NOCMDARG | PLUGIN_VAR_DEPRECATED,
  "Stores each InnoDB table to an .ibd file in the database dir",
  NULL, NULL, TRUE);

static MYSQL_SYSVAR_STR(ft_server_stopword_table, innobase_server_stopword_table,
  PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_MEMALLOC,
  "The user supplied stopword table name",
  innodb_stopword_table_validate,
  NULL,
  NULL);

static MYSQL_SYSVAR_UINT(flush_log_at_timeout, srv_flush_log_at_timeout,
  PLUGIN_VAR_OPCMDARG,
  "Write and flush logs every (n) second",
  NULL, NULL, 1, 0, 2700, 0);

static MYSQL_SYSVAR_ULONG(flush_log_at_trx_commit, srv_flush_log_at_trx_commit,
  PLUGIN_VAR_OPCMDARG,
  "Controls the durability/speed trade-off for commits."
  " Set to 0 (write and flush redo log to disk only once per second),"
  " 1 (flush to disk at each commit),"
  " 2 (write to log at commit but flush to disk only once per second)"
  " or 3 (flush to disk at prepare and at commit, slower and usually redundant)."
  " 1 and 3 guarantees that after a crash, committed transactions will"
  " not be lost and will be consistent with the binlog and other transactional"
  " engines. 2 can get inconsistent and lose transactions if there is a"
  " power failure or kernel crash but not if mysqld crashes. 0 has no"
  " guarantees in case of crash. 0 and 2 can be faster than 1 or 3",
  NULL, NULL, 1, 0, 3, 0);

static MYSQL_SYSVAR_ENUM(flush_method, innodb_flush_method,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY | PLUGIN_VAR_DEPRECATED,
  "With which method to flush data",
  NULL, NULL, innodb_flush_method_default, &innodb_flush_method_typelib);

static MYSQL_SYSVAR_STR(log_group_home_dir, srv_log_group_home_dir,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Path to ib_logfile0", NULL, NULL, NULL);

static MYSQL_SYSVAR_DOUBLE(max_dirty_pages_pct, srv_max_buf_pool_modified_pct,
  PLUGIN_VAR_RQCMDARG,
  "Percentage of dirty pages allowed in bufferpool",
  NULL, innodb_max_dirty_pages_pct_update, 90.0, 0, 99.999, 0);

static MYSQL_SYSVAR_DOUBLE(max_dirty_pages_pct_lwm,
  srv_max_dirty_pages_pct_lwm,
  PLUGIN_VAR_RQCMDARG,
  "Percentage of dirty pages at which flushing kicks in. "
  "The value 0 (default) means 'refer to innodb_max_dirty_pages_pct'",
  NULL, innodb_max_dirty_pages_pct_lwm_update, 0, 0, 99.999, 0);

static MYSQL_SYSVAR_DOUBLE(adaptive_flushing_lwm,
  srv_adaptive_flushing_lwm,
  PLUGIN_VAR_RQCMDARG,
  "Percentage of log capacity below which no adaptive flushing happens",
  NULL, NULL, 10.0, 0.0, 70.0, 0);

static MYSQL_SYSVAR_BOOL(adaptive_flushing, srv_adaptive_flushing,
  PLUGIN_VAR_NOCMDARG,
  "Attempt flushing dirty pages to avoid IO bursts at checkpoints",
  NULL, NULL, TRUE);

static MYSQL_SYSVAR_BOOL(flush_sync, srv_flush_sync,
  PLUGIN_VAR_NOCMDARG,
  "Allow IO bursts at the checkpoints ignoring io_capacity setting",
  NULL, NULL, TRUE);

static MYSQL_SYSVAR_ULONG(flushing_avg_loops,
  srv_flushing_avg_loops,
  PLUGIN_VAR_RQCMDARG,
  "Number of iterations over which the background flushing is averaged",
  NULL, NULL, 30, 1, 1000, 0);

static MYSQL_SYSVAR_ULONG(max_purge_lag, srv_max_purge_lag,
  PLUGIN_VAR_RQCMDARG,
  "Desired maximum length of the purge queue (0 = no limit)",
  NULL, NULL, 0, 0, ~0UL, 0);

static MYSQL_SYSVAR_ULONG(max_purge_lag_delay, srv_max_purge_lag_delay,
   PLUGIN_VAR_RQCMDARG,
   "Maximum delay of user threads in micro-seconds",
   NULL, NULL,
   0L,			/* Default seting */
   0L,			/* Minimum value */
   10000000UL, 0);	/* Maximum value */

static MYSQL_SYSVAR_UINT(max_purge_lag_wait, innodb_max_purge_lag_wait,
  PLUGIN_VAR_RQCMDARG,
  "Wait until History list length is below the specified limit",
  NULL, innodb_max_purge_lag_wait_update, UINT_MAX, 0, UINT_MAX, 0);

static MYSQL_SYSVAR_BOOL(rollback_on_timeout, innobase_rollback_on_timeout,
  PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_READONLY,
  "Roll back the complete transaction on lock wait timeout, for 4.x compatibility (disabled by default)",
  NULL, NULL, FALSE);

static MYSQL_SYSVAR_BOOL(status_file, innobase_create_status_file,
  PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_NOSYSVAR,
  "Enable SHOW ENGINE INNODB STATUS output in the innodb_status.<pid> file",
  NULL, NULL, FALSE);

static MYSQL_SYSVAR_BOOL(stats_on_metadata, innobase_stats_on_metadata,
  PLUGIN_VAR_OPCMDARG,
  "Enable statistics gathering for metadata commands such as"
  " SHOW TABLE STATUS for tables that use transient statistics (off by default)",
  NULL, NULL, FALSE);

static MYSQL_SYSVAR_UINT(stats_transient_sample_pages,
  srv_stats_transient_sample_pages,
  PLUGIN_VAR_RQCMDARG,
  "The number of leaf index pages to sample when calculating transient"
  " statistics (if persistent statistics are not used, default 8)",
  NULL, NULL, 8, 1, ~0U, 0);

static MYSQL_SYSVAR_BOOL(stats_persistent, srv_stats_persistent,
  PLUGIN_VAR_OPCMDARG,
  "InnoDB persistent statistics enabled for all tables unless overridden"
  " at table level",
  NULL, NULL, TRUE);

static MYSQL_SYSVAR_BOOL(stats_auto_recalc, srv_stats_auto_recalc,
  PLUGIN_VAR_OPCMDARG,
  "InnoDB automatic recalculation of persistent statistics enabled for all"
  " tables unless overridden at table level (automatic recalculation is only"
  " done when InnoDB decides that the table has changed too much and needs a"
  " new statistics)",
  NULL, NULL, TRUE);

static MYSQL_SYSVAR_UINT(stats_persistent_sample_pages,
  srv_stats_persistent_sample_pages,
  PLUGIN_VAR_RQCMDARG,
  "The number of leaf index pages to sample when calculating persistent"
  " statistics (by ANALYZE, default 20)",
  NULL, NULL, 20, 1, ~0U, 0);

static MYSQL_SYSVAR_ULONGLONG(stats_modified_counter, srv_stats_modified_counter,
  PLUGIN_VAR_RQCMDARG,
  "The number of rows modified before we calculate new statistics (default 0 = current limits)",
  NULL, NULL, 0, 0, ~0ULL, 0);

static MYSQL_SYSVAR_BOOL(stats_traditional, srv_stats_sample_traditional,
  PLUGIN_VAR_RQCMDARG,
  "Enable traditional statistic calculation based on number of configured pages (default true)",
  NULL, NULL, TRUE);

#ifdef BTR_CUR_HASH_ADAPT
static MYSQL_SYSVAR_BOOL(adaptive_hash_index, *(my_bool*) &btr_search.enabled,
  PLUGIN_VAR_OPCMDARG,
  "Enable InnoDB adaptive hash index (disabled by default)",
  NULL, innodb_adaptive_hash_index_update, false);

static MYSQL_SYSVAR_ULONG(adaptive_hash_index_parts, btr_search.n_parts,
  PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_READONLY,
  "Number of InnoDB Adaptive Hash Index Partitions (default 8)",
  NULL, NULL, 8, 1, array_elements(btr_search.parts), 0);
#endif /* BTR_CUR_HASH_ADAPT */

static MYSQL_SYSVAR_UINT(compression_level, page_zip_level,
  PLUGIN_VAR_RQCMDARG,
  "Compression level used for zlib compression.  0 is no compression"
  ", 1 is fastest, 9 is best compression and default is 6",
  NULL, NULL, DEFAULT_COMPRESSION_LEVEL, 0, 9, 0);

static MYSQL_SYSVAR_UINT(autoextend_increment,
  sys_tablespace_auto_extend_increment,
  PLUGIN_VAR_RQCMDARG,
  "Data file autoextend increment in megabytes",
  NULL, NULL, 64, 1, 1000, 0);

static size_t innodb_buffer_pool_chunk_size;

static MYSQL_SYSVAR_SIZE_T(buffer_pool_chunk_size,
                           innodb_buffer_pool_chunk_size,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY | PLUGIN_VAR_DEPRECATED,
  "Deprecated parameter with no effect",
  NULL, NULL,
  0, 0, SIZE_T_MAX, 1024 * 1024);

static MYSQL_SYSVAR_STR(buffer_pool_filename, srv_buf_dump_filename,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Filename to/from which to dump/load the InnoDB buffer pool",
  NULL, NULL, SRV_BUF_DUMP_FILENAME_DEFAULT);

static MYSQL_SYSVAR_BOOL(buffer_pool_dump_now, innodb_buffer_pool_dump_now,
  PLUGIN_VAR_RQCMDARG,
  "Trigger an immediate dump of the buffer pool into a file named @@innodb_buffer_pool_filename",
  NULL, buffer_pool_dump_now, FALSE);

static MYSQL_SYSVAR_BOOL(buffer_pool_dump_at_shutdown, srv_buffer_pool_dump_at_shutdown,
  PLUGIN_VAR_RQCMDARG,
  "Dump the buffer pool into a file named @@innodb_buffer_pool_filename",
  NULL, NULL, TRUE);

static MYSQL_SYSVAR_ULONG(buffer_pool_dump_pct, srv_buf_pool_dump_pct,
  PLUGIN_VAR_RQCMDARG,
  "Dump only the hottest N% of each buffer pool, defaults to 25",
  NULL, NULL, 25, 1, 100, 0);

#ifdef UNIV_DEBUG
/* Added to test the innodb_buffer_pool_load_incomplete status variable. */
static MYSQL_SYSVAR_ULONG(buffer_pool_load_pages_abort, srv_buf_pool_load_pages_abort,
  PLUGIN_VAR_RQCMDARG,
  "Number of pages during a buffer pool load to process before signaling innodb_buffer_pool_load_abort=1",
  NULL, NULL, LONG_MAX, 1, LONG_MAX, 0);

static MYSQL_SYSVAR_STR(buffer_pool_evict, srv_buffer_pool_evict,
  PLUGIN_VAR_RQCMDARG,
  "Evict pages from the buffer pool",
  NULL, innodb_buffer_pool_evict_update, "");
#endif /* UNIV_DEBUG */

static MYSQL_SYSVAR_BOOL(buffer_pool_load_now, innodb_buffer_pool_load_now,
  PLUGIN_VAR_RQCMDARG,
  "Trigger an immediate load of the buffer pool from a file named @@innodb_buffer_pool_filename",
  NULL, buffer_pool_load_now, FALSE);

static MYSQL_SYSVAR_BOOL(buffer_pool_load_abort, innodb_buffer_pool_load_abort,
  PLUGIN_VAR_RQCMDARG,
  "Abort a currently running load of the buffer pool",
  NULL, buffer_pool_load_abort, FALSE);

/* there is no point in changing this during runtime, thus readonly */
static MYSQL_SYSVAR_BOOL(buffer_pool_load_at_startup, srv_buffer_pool_load_at_startup,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Load the buffer pool from a file named @@innodb_buffer_pool_filename",
  NULL, NULL, TRUE);

static MYSQL_SYSVAR_ULONG(lru_scan_depth, buf_pool.LRU_scan_depth,
  PLUGIN_VAR_RQCMDARG,
  "How deep to scan LRU to keep it clean",
  NULL, innodb_buf_pool_update<ulong>, 1536, 100, ~0UL, 0);

static MYSQL_SYSVAR_ULONG(flush_neighbors, buf_pool.flush_neighbors,
  PLUGIN_VAR_OPCMDARG,
  "Set to 0 (don't flush neighbors from buffer pool),"
  " 1 (flush contiguous neighbors from buffer pool)"
  " or 2 (flush neighbors from buffer pool),"
  " when flushing a block",
  NULL, innodb_buf_pool_update<ulong>, 1, 0, 2, 0);

static MYSQL_SYSVAR_BOOL(deadlock_detect, innodb_deadlock_detect,
  PLUGIN_VAR_NOCMDARG,
  "Enable/disable InnoDB deadlock detector (default ON)."
  " if set to OFF, deadlock detection is skipped,"
  " and we rely on innodb_lock_wait_timeout in case of deadlock",
  NULL, NULL, TRUE);

static MYSQL_SYSVAR_ENUM(deadlock_report, innodb_deadlock_report,
  PLUGIN_VAR_RQCMDARG,
  "How to report deadlocks (if innodb_deadlock_detect=ON)",
  NULL, NULL, Deadlock::REPORT_FULL, &innodb_deadlock_report_typelib);

static MYSQL_SYSVAR_UINT(fill_factor, innobase_fill_factor,
  PLUGIN_VAR_RQCMDARG,
  "Percentage of B-tree page filled during bulk insert",
  NULL, NULL, 100, 10, 100, 0);

static MYSQL_SYSVAR_BOOL(ft_enable_diag_print, fts_enable_diag_print,
  PLUGIN_VAR_OPCMDARG,
  "Whether to enable additional FTS diagnostic printout",
  NULL, NULL, FALSE);

static MYSQL_SYSVAR_BOOL(disable_sort_file_cache, srv_disable_sort_file_cache,
  PLUGIN_VAR_OPCMDARG,
  "Whether to disable OS system file cache for sort I/O",
  NULL, NULL, FALSE);

static MYSQL_SYSVAR_STR(ft_aux_table, innodb_ft_aux_table,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC,
  "FTS internal auxiliary table to be checked",
  innodb_ft_aux_table_validate, NULL, NULL);

#if UNIV_WORD_SIZE == 4

static MYSQL_SYSVAR_SIZE_T(ft_cache_size,
  *reinterpret_cast<size_t*>(&fts_max_cache_size),
  PLUGIN_VAR_RQCMDARG,
  "InnoDB Fulltext search cache size in bytes",
  NULL, innodb_ft_cache_size_update, 8000000, 1600000, 1U << 29, 0);

static MYSQL_SYSVAR_SIZE_T(ft_total_cache_size,
  *reinterpret_cast<size_t*>(&fts_max_total_cache_size),
  PLUGIN_VAR_RQCMDARG,
  "Total memory allocated for InnoDB Fulltext Search cache",
  NULL, innodb_ft_total_cache_size_update, 640000000, 32000000, 1600000000, 0);

#else

static MYSQL_SYSVAR_SIZE_T(ft_cache_size,
  *reinterpret_cast<size_t*>(&fts_max_cache_size),
  PLUGIN_VAR_RQCMDARG,
  "InnoDB Fulltext search cache size in bytes",
  NULL, innodb_ft_cache_size_update, 8000000, 1600000, 1ULL << 40, 0);

static MYSQL_SYSVAR_SIZE_T(ft_total_cache_size,
  *reinterpret_cast<size_t*>(&fts_max_total_cache_size),
  PLUGIN_VAR_RQCMDARG,
  "Total memory allocated for InnoDB Fulltext Search cache",
  NULL, innodb_ft_total_cache_size_update, 640000000, 32000000, 1ULL << 40, 0);

#endif

static MYSQL_SYSVAR_SIZE_T(ft_result_cache_limit, fts_result_cache_limit,
  PLUGIN_VAR_RQCMDARG,
  "InnoDB Fulltext search query result cache limit in bytes",
  NULL, NULL, 2000000000L, 1000000L, SIZE_T_MAX, 0);

static MYSQL_SYSVAR_ULONG(ft_min_token_size, fts_min_token_size,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "InnoDB Fulltext search minimum token size in characters",
  NULL, NULL, 3, 0, 16, 0);

static MYSQL_SYSVAR_ULONG(ft_max_token_size, fts_max_token_size,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "InnoDB Fulltext search maximum token size in characters",
  NULL, NULL, FTS_MAX_WORD_LEN_IN_CHAR, 10, FTS_MAX_WORD_LEN_IN_CHAR, 0);

static MYSQL_SYSVAR_ULONG(ft_num_word_optimize, fts_num_word_optimize,
  PLUGIN_VAR_OPCMDARG,
  "InnoDB Fulltext search number of words to optimize for each optimize table call",
  NULL, NULL, 2000, 1000, 10000, 0);

static MYSQL_SYSVAR_ULONG(ft_sort_pll_degree, fts_sort_pll_degree,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "InnoDB Fulltext search parallel sort degree, will round up to nearest power of 2 number",
  NULL, NULL, 2, 1, 16, 0);

static MYSQL_SYSVAR_ULONG(sort_buffer_size, srv_sort_buf_size,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Memory buffer size for index creation",
  NULL, NULL, 1048576, 65536, 64<<20, 0);

static MYSQL_SYSVAR_ULONGLONG(online_alter_log_max_size, srv_online_max_size,
  PLUGIN_VAR_RQCMDARG,
  "Maximum modification log file size for online index creation",
  NULL, NULL, 128<<20, 65536, ~0ULL, 0);

static MYSQL_SYSVAR_BOOL(optimize_fulltext_only, innodb_optimize_fulltext_only,
  PLUGIN_VAR_NOCMDARG,
  "Only optimize the Fulltext index of the table",
  NULL, NULL, FALSE);

extern int os_aio_resize(ulint n_reader_threads, ulint n_writer_threads);
static void innodb_update_io_thread_count(THD *thd,ulint n_read, ulint n_write)
{
  int res = os_aio_resize(n_read, n_write);
  if (res)
  {
#ifndef __linux__
    ut_ad(0);
#else
    ut_a(srv_use_native_aio);
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
      ER_UNKNOWN_ERROR,
      "Could not reserve max. number of concurrent ios."
      "Increase the /proc/sys/fs/aio-max-nr to fix.");
#endif
  }
}

static void innodb_read_io_threads_update(THD* thd, struct st_mysql_sys_var*, void*, const void* save)
{
  srv_n_read_io_threads = *static_cast<const uint*>(save);
  innodb_update_io_thread_count(thd, srv_n_read_io_threads, srv_n_write_io_threads);
}
static void innodb_write_io_threads_update(THD* thd, struct st_mysql_sys_var*, void*, const void* save)
{
  srv_n_write_io_threads = *static_cast<const uint*>(save);
  innodb_update_io_thread_count(thd, srv_n_read_io_threads, srv_n_write_io_threads);
}

static MYSQL_SYSVAR_UINT(read_io_threads, srv_n_read_io_threads,
  PLUGIN_VAR_RQCMDARG,
  "Number of background read I/O threads in InnoDB",
  NULL, innodb_read_io_threads_update , 4, 1, 64, 0);

static MYSQL_SYSVAR_UINT(write_io_threads, srv_n_write_io_threads,
  PLUGIN_VAR_RQCMDARG,
  "Number of background write I/O threads in InnoDB",
  NULL, innodb_write_io_threads_update, 4, 2, 64, 0);

static MYSQL_SYSVAR_ULONG(force_recovery, srv_force_recovery,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Helps to save your data in case the disk image of the database becomes corrupt. Value 5 can return bogus data, and 6 can permanently corrupt data",
  NULL, NULL, 0, 0, 6, 0);

static MYSQL_SYSVAR_ULONG(page_size, srv_page_size,
  PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_READONLY,
  "Page size to use for all InnoDB tablespaces",
  NULL, NULL, UNIV_PAGE_SIZE_DEF,
  UNIV_PAGE_SIZE_MIN, UNIV_PAGE_SIZE_MAX, 0);

static MYSQL_SYSVAR_UINT(log_buffer_size, log_sys.buf_size,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Redo log buffer size in bytes",
  NULL, NULL, 16U << 20, 2U << 20, log_sys.buf_size_max, 4096);

  static constexpr const char *innodb_log_file_mmap_description=
    "Whether ib_logfile0"
    " resides in persistent memory (when supported) or"
    " should initially be memory-mapped";
static MYSQL_SYSVAR_BOOL(log_file_mmap, log_sys.log_mmap,
  PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_READONLY,
  innodb_log_file_mmap_description,
  nullptr, nullptr, log_sys.log_mmap_default);

#if defined __linux__ || defined _WIN32
static MYSQL_SYSVAR_BOOL(log_file_buffering, log_sys.log_buffered,
  PLUGIN_VAR_OPCMDARG,
  "Whether the file system cache for ib_logfile0 is enabled",
  nullptr, innodb_log_file_buffering_update, FALSE);
#endif

static MYSQL_SYSVAR_BOOL(log_file_write_through, log_sys.log_write_through,
  PLUGIN_VAR_OPCMDARG,
  "Whether each write to ib_logfile0 is write through",
  nullptr, innodb_log_file_write_through_update, FALSE);

static MYSQL_SYSVAR_BOOL(data_file_buffering, fil_system.buffered,
  PLUGIN_VAR_OPCMDARG,
  "Whether the file system cache for data files is enabled",
  nullptr, innodb_data_file_buffering_update, FALSE);

static MYSQL_SYSVAR_BOOL(data_file_write_through, fil_system.write_through,
  PLUGIN_VAR_OPCMDARG,
  "Whether each write to data files writes through",
  nullptr, innodb_data_file_write_through_update, FALSE);

static MYSQL_SYSVAR_ULONGLONG(log_file_size, srv_log_file_size,
  PLUGIN_VAR_RQCMDARG,
  "Redo log size in bytes",
  nullptr, innodb_log_file_size_update,
  96 << 20, 4 << 20, std::numeric_limits<ulonglong>::max(), 4096);

static uint innodb_log_spin_wait_delay;

static MYSQL_SYSVAR_UINT(log_spin_wait_delay, innodb_log_spin_wait_delay,
  PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_DEPRECATED,
  "Deprecated parameter with no effect",
  nullptr, nullptr, 0, 0, 6000, 0);

static MYSQL_SYSVAR_UINT(old_blocks_pct, innobase_old_blocks_pct,
  PLUGIN_VAR_RQCMDARG,
  "Percentage of the buffer pool to reserve for 'old' blocks",
  NULL, innodb_old_blocks_pct_update, 100 * 3 / 8, 5, 95, 0);

static MYSQL_SYSVAR_UINT(old_blocks_time, buf_LRU_old_threshold_ms,
  PLUGIN_VAR_RQCMDARG,
  "Move blocks to the 'new' end of the buffer pool if the first access"
  " was at least this many milliseconds ago."
  " The timeout is disabled if 0",
  NULL, NULL, 1000, 0, UINT_MAX32, 0);

static MYSQL_SYSVAR_ULONG(open_files, innobase_open_files,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "How many files at the maximum InnoDB keeps open at the same time",
  NULL, NULL, 0, 0, LONG_MAX, 0);

static MYSQL_SYSVAR_ULONG(sync_spin_loops, srv_n_spin_wait_rounds,
  PLUGIN_VAR_RQCMDARG,
  "Count of spin-loop rounds in InnoDB mutexes (30 by default)",
  NULL, NULL, 30L, 0L, ~0UL, 0);

static MYSQL_SYSVAR_UINT(spin_wait_delay, srv_spin_wait_delay,
  PLUGIN_VAR_OPCMDARG,
  "Maximum delay between polling for a spin lock (4 by default)",
  NULL, NULL, 4, 0, 6000, 0);

static my_bool innodb_prefix_index_cluster_optimization;

static MYSQL_SYSVAR_BOOL(prefix_index_cluster_optimization,
  innodb_prefix_index_cluster_optimization,
  PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_DEPRECATED,
  "Unused", nullptr, nullptr, FALSE);

static MYSQL_SYSVAR_STR(data_file_path, innobase_data_file_path,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Path to individual files and their sizes",
  NULL, NULL, "ibdata1:12M:autoextend");

static MYSQL_SYSVAR_STR(temp_data_file_path, innobase_temp_data_file_path,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Path to files and their sizes making temp-tablespace",
  NULL, NULL, "ibtmp1:12M:autoextend");

static MYSQL_SYSVAR_STR(undo_directory, srv_undo_dir,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Directory where undo tablespace files live, this path can be absolute",
  NULL, NULL, NULL);

static MYSQL_SYSVAR_UINT(undo_tablespaces, srv_undo_tablespaces,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Number of undo tablespaces to use",
  NULL, NULL,
  3L,			/* Default seting */
  0L,			/* Minimum value */
  TRX_SYS_MAX_UNDO_SPACES, 0); /* Maximum value */

static MYSQL_SYSVAR_ULONGLONG(max_undo_log_size, srv_max_undo_log_size,
  PLUGIN_VAR_OPCMDARG,
  "Desired maximum UNDO tablespace size in bytes",
  NULL, NULL,
  10 << 20, 10 << 20,
  1ULL << (32 + UNIV_PAGE_SIZE_SHIFT_MAX), 0);

static ulong innodb_purge_rseg_truncate_frequency= 128;

static MYSQL_SYSVAR_ULONG(purge_rseg_truncate_frequency,
  innodb_purge_rseg_truncate_frequency,
  PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_DEPRECATED | PLUGIN_VAR_NOCMDOPT,
  "Unused",
  NULL, NULL, 128, 1, 128, 0);

static size_t innodb_lru_flush_size;

static MYSQL_SYSVAR_SIZE_T(lru_flush_size, innodb_lru_flush_size,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_DEPRECATED | PLUGIN_VAR_NOCMDOPT,
  "Unused",
  NULL, NULL, 32, 1, SIZE_T_MAX, 0);

static void innodb_undo_log_truncate_update(THD *thd, struct st_mysql_sys_var*,
                                            void*, const void *save)
{
  if ((srv_undo_log_truncate= *static_cast<const my_bool*>(save)))
    purge_sys.wake_if_not_active();
}

static MYSQL_SYSVAR_BOOL(undo_log_truncate, srv_undo_log_truncate,
  PLUGIN_VAR_OPCMDARG,
  "Enable or Disable Truncate of UNDO tablespace",
  NULL, innodb_undo_log_truncate_update, FALSE);

static MYSQL_SYSVAR_LONG(autoinc_lock_mode, innobase_autoinc_lock_mode,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "The AUTOINC lock modes supported by InnoDB:"
  " 0 => Old style AUTOINC locking (for backward compatibility);"
  " 1 => New style AUTOINC locking;"
  " 2 => No AUTOINC locking (unsafe for SBR)",
  NULL, NULL,
  AUTOINC_NEW_STYLE_LOCKING,	/* Default setting */
  AUTOINC_OLD_STYLE_LOCKING,	/* Minimum value */
  AUTOINC_NO_LOCKING, 0);	/* Maximum value */

static MYSQL_SYSVAR_BOOL(use_native_aio, srv_use_native_aio,
  PLUGIN_VAR_NOCMDARG | PLUGIN_VAR_READONLY,
  "Use native AIO if supported on this platform",
  NULL, NULL, TRUE);

#ifdef HAVE_LIBNUMA
static MYSQL_SYSVAR_BOOL(numa_interleave, srv_numa_interleave,
  PLUGIN_VAR_NOCMDARG | PLUGIN_VAR_READONLY,
  "Use NUMA interleave memory policy to allocate InnoDB buffer pool",
  NULL, NULL, FALSE);
#endif /* HAVE_LIBNUMA */

static MYSQL_SYSVAR_ENUM(stats_method, srv_innodb_stats_method,
   PLUGIN_VAR_RQCMDARG,
  "Specifies how InnoDB index statistics collection code should"
  " treat NULLs. Possible values are NULLS_EQUAL (default),"
  " NULLS_UNEQUAL and NULLS_IGNORED",
   NULL, NULL, SRV_STATS_NULLS_EQUAL, &innodb_stats_method_typelib);

static MYSQL_SYSVAR_ULONG(buf_dump_status_frequency, srv_buf_dump_status_frequency,
  PLUGIN_VAR_RQCMDARG,
  "A number that tells how often buffer pool dump status "
  "in percentages should be printed. E.g. 10 means that buffer pool dump "
  "status is printed when every 10% of number of buffer pool pages are "
  "dumped. Default is 0 (only start and end status is printed)",
  NULL, NULL, 0, 0, 100, 0);

static MYSQL_SYSVAR_BOOL(random_read_ahead, srv_random_read_ahead,
  PLUGIN_VAR_NOCMDARG,
  "Whether to use read ahead for random access within an extent",
  NULL, NULL, FALSE);

static MYSQL_SYSVAR_ULONG(read_ahead_threshold, srv_read_ahead_threshold,
  PLUGIN_VAR_RQCMDARG,
  "Number of pages that must be accessed sequentially for InnoDB to"
  " trigger a readahead",
  NULL, NULL, 56, 0, 64, 0);

static MYSQL_SYSVAR_STR(monitor_enable, innobase_enable_monitor_counter,
  PLUGIN_VAR_RQCMDARG,
  "Turn on a monitor counter",
  innodb_monitor_validate,
  innodb_enable_monitor_update, NULL);

static MYSQL_SYSVAR_STR(monitor_disable, innobase_disable_monitor_counter,
  PLUGIN_VAR_RQCMDARG,
  "Turn off a monitor counter",
  innodb_monitor_validate,
  innodb_disable_monitor_update, NULL);

static MYSQL_SYSVAR_STR(monitor_reset, innobase_reset_monitor_counter,
  PLUGIN_VAR_RQCMDARG,
  "Reset a monitor counter",
  innodb_monitor_validate,
  innodb_reset_monitor_update, NULL);

static MYSQL_SYSVAR_STR(monitor_reset_all, innobase_reset_all_monitor_counter,
  PLUGIN_VAR_RQCMDARG,
  "Reset all values for a monitor counter",
  innodb_monitor_validate,
  innodb_reset_all_monitor_update, NULL);

static MYSQL_SYSVAR_BOOL(status_output, srv_print_innodb_monitor,
  PLUGIN_VAR_OPCMDARG, "Enable InnoDB monitor output to the error log",
  NULL, innodb_status_output_update, FALSE);

static MYSQL_SYSVAR_BOOL(status_output_locks, srv_print_innodb_lock_monitor,
  PLUGIN_VAR_OPCMDARG, "Enable InnoDB lock monitor output to the error log."
  " Requires innodb_status_output=ON",
  NULL, innodb_status_output_update, FALSE);

static MYSQL_SYSVAR_BOOL(print_all_deadlocks, srv_print_all_deadlocks,
  PLUGIN_VAR_OPCMDARG,
  "Print all deadlocks to MariaDB error log (off by default)",
  NULL, NULL, FALSE);

static MYSQL_SYSVAR_ULONG(compression_failure_threshold_pct,
  zip_failure_threshold_pct, PLUGIN_VAR_OPCMDARG,
  "If the compression failure rate of a table is greater than this number"
  " more padding is added to the pages to reduce the failures. A value of"
  " zero implies no padding",
  NULL, NULL, 5, 0, 100, 0);

static MYSQL_SYSVAR_ULONG(compression_pad_pct_max,
  zip_pad_max, PLUGIN_VAR_OPCMDARG,
  "Percentage of empty space on a data page that can be reserved"
  " to make the page compressible",
  NULL, NULL, 50, 0, 75, 0);

static MYSQL_SYSVAR_BOOL(read_only, srv_read_only_mode,
  PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_READONLY,
  "Start InnoDB in read only mode (off by default)",
  NULL, NULL, FALSE);

static MYSQL_SYSVAR_BOOL(read_only_compressed, innodb_read_only_compressed,
  PLUGIN_VAR_OPCMDARG,
  "Make ROW_FORMAT=COMPRESSED tables read-only",
  NULL, NULL, FALSE);

static MYSQL_SYSVAR_BOOL(cmp_per_index_enabled, srv_cmp_per_index_enabled,
  PLUGIN_VAR_OPCMDARG,
  "Enable INFORMATION_SCHEMA.innodb_cmp_per_index,"
  " may have negative impact on performance (off by default)",
  NULL, innodb_cmp_per_index_update, FALSE);

static MYSQL_SYSVAR_ENUM(default_row_format, innodb_default_row_format,
  PLUGIN_VAR_RQCMDARG,
  "The default ROW FORMAT for all innodb tables created without explicit"
  " ROW_FORMAT. Possible values are REDUNDANT, COMPACT, and DYNAMIC."
  " The ROW_FORMAT value COMPRESSED is not allowed",
  NULL, NULL, DEFAULT_ROW_FORMAT_DYNAMIC,
  &innodb_default_row_format_typelib);

#ifdef UNIV_DEBUG
static MYSQL_SYSVAR_UINT(trx_rseg_n_slots_debug, trx_rseg_n_slots_debug,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_NOCMDOPT,
  "Debug flags for InnoDB to limit TRX_RSEG_N_SLOTS for trx_rsegf_undo_find_free()",
  NULL, NULL, 0, 0, 1024, 0);

static MYSQL_SYSVAR_UINT(limit_optimistic_insert_debug,
  btr_cur_limit_optimistic_insert_debug, PLUGIN_VAR_RQCMDARG,
  "Artificially limit the number of records per B-tree page (0=unlimited)",
  NULL, NULL, 0, 0, UINT_MAX32, 0);

static MYSQL_SYSVAR_BOOL(trx_purge_view_update_only_debug,
  srv_purge_view_update_only_debug, PLUGIN_VAR_NOCMDOPT,
  "Pause actual purging any delete-marked records, but merely update the purge view."
  " It is to create artificially the situation the purge view have been updated"
  " but the each purges were not done yet",
  NULL, NULL, FALSE);

static MYSQL_SYSVAR_BOOL(evict_tables_on_commit_debug,
  innodb_evict_tables_on_commit_debug, PLUGIN_VAR_OPCMDARG,
  "On transaction commit, try to evict tables from the data dictionary cache",
  NULL, NULL, FALSE);

static MYSQL_SYSVAR_UINT(data_file_size_debug,
  srv_sys_space_size_debug,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "InnoDB system tablespace size to be set in recovery",
  NULL, NULL, 0, 0, 256U << 20, 0);

static MYSQL_SYSVAR_UINT(fil_make_page_dirty_debug,
  srv_fil_make_page_dirty_debug, PLUGIN_VAR_OPCMDARG,
  "Make the first page of the given tablespace dirty",
  NULL, innodb_make_page_dirty, 0, 0, UINT_MAX32, 0);

static MYSQL_SYSVAR_UINT(saved_page_number_debug,
  srv_saved_page_number_debug, PLUGIN_VAR_OPCMDARG,
  "An InnoDB page number",
  NULL, NULL, 0, 0, UINT_MAX32, 0);
#endif /* UNIV_DEBUG */

#if defined(UNIV_DEBUG) || \
    defined(INNODB_ENABLE_XAP_UNLOCK_UNMODIFIED_FOR_PRIMARY)
static MYSQL_SYSVAR_BOOL(enable_xap_unlock_unmodified_for_primary_debug,
  innodb_enable_xap_unlock_unmodified_for_primary_debug, PLUGIN_VAR_NOCMDARG,
  "Unlock unmodified records on XA PREPARE for primary",
  NULL, NULL, FALSE);
#endif /* defined(UNIV_DEBUG) ||
          defined(INNODB_ENABLE_XAP_UNLOCK_UNMODIFIED_FOR_PRIMARY) */

static MYSQL_SYSVAR_BOOL(force_primary_key,
  srv_force_primary_key,
  PLUGIN_VAR_OPCMDARG,
  "Do not allow creating a table without primary key (off by default)",
  NULL, NULL, FALSE);

static MYSQL_SYSVAR_BOOL(alter_copy_bulk, innodb_alter_copy_bulk,
  PLUGIN_VAR_NOCMDARG,
  "Allow bulk insert operation for copy alter operation", NULL, NULL, TRUE);

const char *page_compression_algorithms[]= { "none", "zlib", "lz4", "lzo", "lzma", "bzip2", "snappy", 0 };
static TYPELIB page_compression_algorithms_typelib=
		CREATE_TYPELIB_FOR(page_compression_algorithms);
static MYSQL_SYSVAR_ENUM(compression_algorithm, innodb_compression_algorithm,
  PLUGIN_VAR_OPCMDARG,
  "Compression algorithm used on page compression. One of: none, zlib, lz4, lzo, lzma, bzip2, or snappy",
  innodb_compression_algorithm_validate, NULL,
  /* We use here the largest number of supported compression method to
  enable all those methods that are available. Availability of compression
  method is verified on innodb_compression_algorithm_validate function. */
  PAGE_ZLIB_ALGORITHM,
  &page_compression_algorithms_typelib);

static MYSQL_SYSVAR_ULONG(fatal_semaphore_wait_threshold, srv_fatal_semaphore_wait_threshold,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Maximum number of seconds that semaphore times out in InnoDB",
  NULL, NULL,
  DEFAULT_SRV_FATAL_SEMAPHORE_TIMEOUT, /* Default setting */
  1, /* Minimum setting */
  UINT_MAX32, /* Maximum setting */
  0);

static const char* srv_encrypt_tables_names[] = { "OFF", "ON", "FORCE", 0 };
static TYPELIB srv_encrypt_tables_typelib =
		CREATE_TYPELIB_FOR(srv_encrypt_tables_names);
static MYSQL_SYSVAR_ENUM(encrypt_tables, srv_encrypt_tables,
			 PLUGIN_VAR_OPCMDARG,
			 "Enable encryption for tables. "
			 "Don't forget to enable --innodb-encrypt-log too",
			 innodb_encrypt_tables_validate,
			 innodb_encrypt_tables_update,
			 0,
			 &srv_encrypt_tables_typelib);

static MYSQL_SYSVAR_UINT(encryption_threads, srv_n_fil_crypt_threads,
			 PLUGIN_VAR_RQCMDARG,
			 "Number of threads performing background key rotation",
			 NULL,
			 innodb_encryption_threads_update,
			 0, 0, 255, 0);

static MYSQL_SYSVAR_UINT(encryption_rotate_key_age,
			 srv_fil_crypt_rotate_key_age,
			 PLUGIN_VAR_RQCMDARG,
			 "Key rotation - re-encrypt in background "
                         "all pages that were encrypted with a key that "
                         "many (or more) versions behind. Value 0 indicates "
			 "that key rotation is disabled",
			 NULL,
			 innodb_encryption_rotate_key_age_update,
			 1, 0, UINT_MAX32, 0);

static MYSQL_SYSVAR_UINT(encryption_rotation_iops, srv_n_fil_crypt_iops,
			 PLUGIN_VAR_RQCMDARG,
			 "Use this many iops for background key rotation",
			 NULL,
			 innodb_encryption_rotation_iops_update,
			 100, 0, UINT_MAX32, 0);

static MYSQL_SYSVAR_BOOL(encrypt_log, srv_encrypt_log,
  PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_READONLY,
  "Enable redo log encryption",
  NULL, NULL, FALSE);

static MYSQL_SYSVAR_BOOL(immediate_scrub_data_uncompressed,
			 srv_immediate_scrub_data_uncompressed,
			 0,
			 "Enable scrubbing of data",
			 NULL, NULL, FALSE);

static MYSQL_SYSVAR_BOOL(encrypt_temporary_tables, innodb_encrypt_temporary_tables,
  PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_READONLY,
  "Encrypt the temporary table data",
  NULL, NULL, false);

static MYSQL_SYSVAR_BOOL(truncate_temporary_tablespace_now,
  innodb_truncate_temporary_tablespace_now,
  PLUGIN_VAR_OPCMDARG,
  "Shrink the temporary tablespace",
  NULL, innodb_trunc_temp_space_update, false);

static struct st_mysql_sys_var* innobase_system_variables[]= {
  MYSQL_SYSVAR(autoextend_increment),
  MYSQL_SYSVAR(buffer_pool_size),
#if defined __linux__ || !defined DBUG_OFF
  MYSQL_SYSVAR(buffer_pool_size_auto_min),
#endif
  MYSQL_SYSVAR(buffer_pool_size_max),
  MYSQL_SYSVAR(buffer_pool_chunk_size),
  MYSQL_SYSVAR(buffer_pool_filename),
  MYSQL_SYSVAR(buffer_pool_dump_now),
  MYSQL_SYSVAR(buffer_pool_dump_at_shutdown),
  MYSQL_SYSVAR(buffer_pool_dump_pct),
#ifdef UNIV_DEBUG
  MYSQL_SYSVAR(buffer_pool_evict),
#endif /* UNIV_DEBUG */
  MYSQL_SYSVAR(buffer_pool_load_now),
  MYSQL_SYSVAR(buffer_pool_load_abort),
#ifdef UNIV_DEBUG
  MYSQL_SYSVAR(buffer_pool_load_pages_abort),
#endif /* UNIV_DEBUG */
  MYSQL_SYSVAR(buffer_pool_load_at_startup),
  MYSQL_SYSVAR(lru_scan_depth),
  MYSQL_SYSVAR(lru_flush_size),
  MYSQL_SYSVAR(flush_neighbors),
  MYSQL_SYSVAR(checksum_algorithm),
  MYSQL_SYSVAR(compression_level),
  MYSQL_SYSVAR(data_file_path),
  MYSQL_SYSVAR(temp_data_file_path),
  MYSQL_SYSVAR(data_home_dir),
  MYSQL_SYSVAR(doublewrite),
  MYSQL_SYSVAR(stats_include_delete_marked),
  MYSQL_SYSVAR(use_atomic_writes),
  MYSQL_SYSVAR(fast_shutdown),
  MYSQL_SYSVAR(read_io_threads),
  MYSQL_SYSVAR(write_io_threads),
  MYSQL_SYSVAR(file_per_table),
  MYSQL_SYSVAR(flush_log_at_timeout),
  MYSQL_SYSVAR(flush_log_at_trx_commit),
  MYSQL_SYSVAR(flush_method),
  MYSQL_SYSVAR(force_recovery),
  MYSQL_SYSVAR(fill_factor),
  MYSQL_SYSVAR(ft_cache_size),
  MYSQL_SYSVAR(ft_total_cache_size),
  MYSQL_SYSVAR(ft_result_cache_limit),
  MYSQL_SYSVAR(ft_enable_stopword),
  MYSQL_SYSVAR(ft_max_token_size),
  MYSQL_SYSVAR(ft_min_token_size),
  MYSQL_SYSVAR(ft_num_word_optimize),
  MYSQL_SYSVAR(ft_sort_pll_degree),
  MYSQL_SYSVAR(lock_wait_timeout),
  MYSQL_SYSVAR(deadlock_detect),
  MYSQL_SYSVAR(deadlock_report),
  MYSQL_SYSVAR(page_size),
  MYSQL_SYSVAR(log_buffer_size),
  MYSQL_SYSVAR(log_file_mmap),
#if defined __linux__ || defined _WIN32
  MYSQL_SYSVAR(log_file_buffering),
#endif
  MYSQL_SYSVAR(log_file_write_through),
  MYSQL_SYSVAR(data_file_buffering),
  MYSQL_SYSVAR(data_file_write_through),
  MYSQL_SYSVAR(log_file_size),
  MYSQL_SYSVAR(log_write_ahead_size),
  MYSQL_SYSVAR(log_spin_wait_delay),
  MYSQL_SYSVAR(log_group_home_dir),
  MYSQL_SYSVAR(max_dirty_pages_pct),
  MYSQL_SYSVAR(max_dirty_pages_pct_lwm),
  MYSQL_SYSVAR(adaptive_flushing_lwm),
  MYSQL_SYSVAR(adaptive_flushing),
  MYSQL_SYSVAR(flush_sync),
  MYSQL_SYSVAR(flushing_avg_loops),
  MYSQL_SYSVAR(max_purge_lag),
  MYSQL_SYSVAR(max_purge_lag_delay),
  MYSQL_SYSVAR(max_purge_lag_wait),
  MYSQL_SYSVAR(old_blocks_pct),
  MYSQL_SYSVAR(old_blocks_time),
  MYSQL_SYSVAR(open_files),
  MYSQL_SYSVAR(optimize_fulltext_only),
  MYSQL_SYSVAR(rollback_on_timeout),
  MYSQL_SYSVAR(ft_aux_table),
  MYSQL_SYSVAR(ft_enable_diag_print),
  MYSQL_SYSVAR(ft_server_stopword_table),
  MYSQL_SYSVAR(ft_user_stopword_table),
  MYSQL_SYSVAR(disable_sort_file_cache),
  MYSQL_SYSVAR(snapshot_isolation),
  MYSQL_SYSVAR(stats_on_metadata),
  MYSQL_SYSVAR(stats_transient_sample_pages),
  MYSQL_SYSVAR(stats_persistent),
  MYSQL_SYSVAR(stats_persistent_sample_pages),
  MYSQL_SYSVAR(stats_auto_recalc),
  MYSQL_SYSVAR(stats_modified_counter),
  MYSQL_SYSVAR(stats_traditional),
#ifdef BTR_CUR_HASH_ADAPT
  MYSQL_SYSVAR(adaptive_hash_index),
  MYSQL_SYSVAR(adaptive_hash_index_parts),
#endif /* BTR_CUR_HASH_ADAPT */
  MYSQL_SYSVAR(stats_method),
  MYSQL_SYSVAR(status_file),
  MYSQL_SYSVAR(strict_mode),
  MYSQL_SYSVAR(sort_buffer_size),
  MYSQL_SYSVAR(online_alter_log_max_size),
  MYSQL_SYSVAR(sync_spin_loops),
  MYSQL_SYSVAR(spin_wait_delay),
  MYSQL_SYSVAR(table_locks),
  MYSQL_SYSVAR(prefix_index_cluster_optimization),
  MYSQL_SYSVAR(tmpdir),
  MYSQL_SYSVAR(autoinc_lock_mode),
  MYSQL_SYSVAR(use_native_aio),
#ifdef HAVE_LIBNUMA
  MYSQL_SYSVAR(numa_interleave),
#endif /* HAVE_LIBNUMA */
  MYSQL_SYSVAR(random_read_ahead),
  MYSQL_SYSVAR(read_ahead_threshold),
  MYSQL_SYSVAR(read_only),
  MYSQL_SYSVAR(read_only_compressed),
  MYSQL_SYSVAR(instant_alter_column_allowed),
  MYSQL_SYSVAR(io_capacity),
  MYSQL_SYSVAR(io_capacity_max),
  MYSQL_SYSVAR(monitor_enable),
  MYSQL_SYSVAR(monitor_disable),
  MYSQL_SYSVAR(monitor_reset),
  MYSQL_SYSVAR(monitor_reset_all),
  MYSQL_SYSVAR(purge_threads),
  MYSQL_SYSVAR(purge_batch_size),
  MYSQL_SYSVAR(log_checkpoint_now),
#ifdef UNIV_DEBUG
  MYSQL_SYSVAR(buf_flush_list_now),
  MYSQL_SYSVAR(merge_threshold_set_all_debug),
#endif /* UNIV_DEBUG */
  MYSQL_SYSVAR(status_output),
  MYSQL_SYSVAR(status_output_locks),
  MYSQL_SYSVAR(print_all_deadlocks),
  MYSQL_SYSVAR(cmp_per_index_enabled),
  MYSQL_SYSVAR(max_undo_log_size),
  MYSQL_SYSVAR(purge_rseg_truncate_frequency),
  MYSQL_SYSVAR(undo_log_truncate),
  MYSQL_SYSVAR(undo_directory),
  MYSQL_SYSVAR(undo_tablespaces),
  MYSQL_SYSVAR(compression_failure_threshold_pct),
  MYSQL_SYSVAR(compression_pad_pct_max),
  MYSQL_SYSVAR(default_row_format),
#ifdef UNIV_DEBUG
  MYSQL_SYSVAR(trx_rseg_n_slots_debug),
  MYSQL_SYSVAR(limit_optimistic_insert_debug),
  MYSQL_SYSVAR(trx_purge_view_update_only_debug),
  MYSQL_SYSVAR(evict_tables_on_commit_debug),
  MYSQL_SYSVAR(data_file_size_debug),
  MYSQL_SYSVAR(fil_make_page_dirty_debug),
  MYSQL_SYSVAR(saved_page_number_debug),
#endif /* UNIV_DEBUG */
#if defined(UNIV_DEBUG) || \
    defined(INNODB_ENABLE_XAP_UNLOCK_UNMODIFIED_FOR_PRIMARY)
  MYSQL_SYSVAR(enable_xap_unlock_unmodified_for_primary_debug),
#endif /* defined(UNIV_DEBUG) ||
          defined(INNODB_ENABLE_XAP_UNLOCK_UNMODIFIED_FOR_PRIMARY) */
  MYSQL_SYSVAR(force_primary_key),
  MYSQL_SYSVAR(alter_copy_bulk),
  MYSQL_SYSVAR(fatal_semaphore_wait_threshold),
  /* Table page compression feature */
  MYSQL_SYSVAR(compression_default),
  MYSQL_SYSVAR(compression_algorithm),
  /* Encryption feature */
  MYSQL_SYSVAR(encrypt_tables),
  MYSQL_SYSVAR(encryption_threads),
  MYSQL_SYSVAR(encryption_rotate_key_age),
  MYSQL_SYSVAR(encryption_rotation_iops),
  MYSQL_SYSVAR(encrypt_log),
  MYSQL_SYSVAR(default_encryption_key_id),
  MYSQL_SYSVAR(immediate_scrub_data_uncompressed),
  MYSQL_SYSVAR(buf_dump_status_frequency),
  MYSQL_SYSVAR(background_thread),
  MYSQL_SYSVAR(encrypt_temporary_tables),
  MYSQL_SYSVAR(truncate_temporary_tablespace_now),

  NULL
};

maria_declare_plugin(innobase)
{
  MYSQL_STORAGE_ENGINE_PLUGIN,
  &innobase_storage_engine,
  innobase_hton_name,
  plugin_author,
  "Supports transactions, row-level locking, foreign keys and encryption for tables",
  PLUGIN_LICENSE_GPL,
  innodb_init, /* Plugin Init */
  NULL, /* Plugin Deinit */
  MYSQL_VERSION_MAJOR << 8 | MYSQL_VERSION_MINOR,
  innodb_status_variables_export,/* status variables             */
  innobase_system_variables, /* system variables */
  PACKAGE_VERSION,
  MariaDB_PLUGIN_MATURITY_STABLE /* maturity */
},
i_s_innodb_trx,
i_s_innodb_locks,
i_s_innodb_lock_waits,
i_s_innodb_cmp,
i_s_innodb_cmp_reset,
i_s_innodb_cmpmem,
i_s_innodb_cmpmem_reset,
i_s_innodb_cmp_per_index,
i_s_innodb_cmp_per_index_reset,
i_s_innodb_buffer_page,
i_s_innodb_buffer_page_lru,
i_s_innodb_buffer_stats,
i_s_innodb_metrics,
i_s_innodb_ft_default_stopword,
i_s_innodb_ft_deleted,
i_s_innodb_ft_being_deleted,
i_s_innodb_ft_config,
i_s_innodb_ft_index_cache,
i_s_innodb_ft_index_table,
i_s_innodb_sys_tables,
i_s_innodb_sys_tablestats,
i_s_innodb_sys_indexes,
i_s_innodb_sys_columns,
i_s_innodb_sys_fields,
i_s_innodb_sys_foreign,
i_s_innodb_sys_foreign_cols,
i_s_innodb_sys_tablespaces,
i_s_innodb_sys_virtual,
i_s_innodb_tablespaces_encryption
maria_declare_plugin_end;

/** Adjust some InnoDB startup parameters based on the data directory */
static void innodb_params_adjust()
{
  MYSQL_SYSVAR_NAME(max_undo_log_size).max_val=
    1ULL << (32U + srv_page_size_shift);
  MYSQL_SYSVAR_NAME(max_undo_log_size).min_val=
    MYSQL_SYSVAR_NAME(max_undo_log_size).def_val=
    ulonglong{SRV_UNDO_TABLESPACE_SIZE_IN_PAGES} << srv_page_size_shift;
  MYSQL_SYSVAR_NAME(max_undo_log_size).max_val=
    1ULL << (32U + srv_page_size_shift);
#if 0 /* FIXME: INFORMATION_SCHEMA.SYSTEM_VARIABLES won't reflect this. */
  /* plugin_opt_set_limits() would have copied all MYSQL_SYSVAR
  before innodb_init() was invoked. Therefore, changing the
  min_val, def_val, max_val will have no observable effect. */
# if defined __linux__ || defined _WIN32
  uint &min_val= MYSQL_SYSVAR_NAME(log_write_ahead_size).min_val;
  if (min_val < log_sys.write_size)
  {
    min_val= log_sys.write_size;
    MYSQL_SYSVAR_NAME(log_write_ahead_size).def_val= log_sys.write_size;
  }
# endif
  ut_ad(MYSQL_SYSVAR_NAME(log_write_ahead_size).min_val <=
        log_sys.write_size);
#endif
  ut_ad(MYSQL_SYSVAR_NAME(log_write_ahead_size).max_val == 4096);
}

/****************************************************************************
 * DS-MRR implementation
 ***************************************************************************/

/**
Multi Range Read interface, DS-MRR calls */
int
ha_innobase::multi_range_read_init(
	RANGE_SEQ_IF*	seq,
	void*		seq_init_param,
	uint		n_ranges,
	uint		mode,
	HANDLER_BUFFER*	buf)
{
	return(m_ds_mrr.dsmrr_init(this, seq, seq_init_param,
				 n_ranges, mode, buf));
}

int
ha_innobase::multi_range_read_next(
	range_id_t*		range_info)
{
	return(m_ds_mrr.dsmrr_next(range_info));
}

ha_rows
ha_innobase::multi_range_read_info_const(
	uint		keyno,
	RANGE_SEQ_IF*	seq,
	void*		seq_init_param,
	uint		n_ranges,
	uint*		bufsz,
	uint*		flags,
        ha_rows         limit,
	Cost_estimate*	cost)
{
	/* See comments in ha_myisam::multi_range_read_info_const */
	m_ds_mrr.init(this, table);

	if (m_prebuilt->select_lock_type != LOCK_NONE) {
		*flags |= HA_MRR_USE_DEFAULT_IMPL;
	}

	ha_rows res= m_ds_mrr.dsmrr_info_const(keyno, seq, seq_init_param,
                                               n_ranges,
                                               bufsz, flags, limit, cost);
	return res;
}

ha_rows
ha_innobase::multi_range_read_info(
	uint		keyno,
	uint		n_ranges,
	uint		keys,
	uint		key_parts,
	uint*		bufsz,
	uint*		flags,
	Cost_estimate*	cost)
{
	m_ds_mrr.init(this, table);
	ha_rows res= m_ds_mrr.dsmrr_info(keyno, n_ranges, keys, key_parts, bufsz,
					flags, cost);
	return res;
}

int
ha_innobase::multi_range_read_explain_info(
	uint mrr_mode,
	char *str,
	size_t size)
{
	return m_ds_mrr.dsmrr_explain_info(mrr_mode, str, size);
}

/** Find or open a table handle for the virtual column template
@param[in]	thd	thread handle
@param[in,out]	table	InnoDB table whose virtual column template
			is to be updated
@return table handle
@retval NULL if the table is dropped, unaccessible or corrupted
for purge thread */
static TABLE* innodb_find_table_for_vc(THD* thd, dict_table_t* table)
{
	TABLE *mysql_table;
	const bool  bg_thread = THDVAR(thd, background_thread);

	if (bg_thread) {
		if ((mysql_table = get_purge_table(thd))) {
			return mysql_table;
		}
	} else {
		if (table->vc_templ->mysql_table_query_id
		    == thd_get_query_id(thd)) {
			return table->vc_templ->mysql_table;
		}
	}

	char	db_buf[NAME_LEN + 1];
	char	tbl_buf[NAME_LEN + 1];
	ulint	db_buf_len, tbl_buf_len;

	if (!table->parse_name(db_buf, tbl_buf, &db_buf_len, &tbl_buf_len)) {
		return NULL;
	}

	if (bg_thread) {
		return open_purge_table(thd, db_buf, db_buf_len,
					tbl_buf, tbl_buf_len);
	}

	mysql_table = find_fk_open_table(thd, db_buf, db_buf_len,
					 tbl_buf, tbl_buf_len);
	table->vc_templ->mysql_table = mysql_table;
	table->vc_templ->mysql_table_query_id = thd_get_query_id(thd);
	return mysql_table;
}

/** Change dbname and table name in table->vc_templ.
@param[in,out]	table	the table whose virtual column template
dbname and tbname to be renamed. */
void
innobase_rename_vc_templ(
	dict_table_t*	table)
{
	char	dbname[MAX_DATABASE_NAME_LEN + 1];
	char	tbname[MAX_DATABASE_NAME_LEN + 1];
	char*	name = table->name.m_name;
	ulint	dbnamelen = dict_get_db_name_len(name);
	ulint	tbnamelen = strlen(name) - dbnamelen - 1;
	char	t_dbname[MAX_DATABASE_NAME_LEN + 1];
	char	t_tbname[MAX_TABLE_NAME_LEN + 1];

	strncpy(dbname, name, dbnamelen);
	dbname[dbnamelen] = 0;
	strncpy(tbname, name + dbnamelen + 1, tbnamelen);
	tbname[tbnamelen] =0;

	/* For partition table, remove the partition name and use the
	"main" table name to build the template */

	if (char *is_part = is_partition(tbname)) {
		*is_part = '\0';
		tbnamelen = ulint(is_part - tbname);
	}
	else if (char *is_hli = is_hlindex(tbname)) {
		*is_hli = '\0';
		tbnamelen = ulint(is_hli - tbname);
	}

	dbnamelen = filename_to_tablename(dbname, t_dbname,
					  MAX_DATABASE_NAME_LEN + 1);
	tbnamelen = filename_to_tablename(tbname, t_tbname,
					  MAX_TABLE_NAME_LEN + 1);

	table->vc_templ->db_name = t_dbname;
	table->vc_templ->tb_name = t_tbname;
}


/**
   Allocate a heap and record for calculating virtual fields
   Used mainly for virtual fields in indexes

@param[in]      thd             MariaDB THD
@param[in]      index           Index in use
@param[out]     heap            Heap that holds temporary row
@param[in,out]  table           MariaDB table
@param[out]     record	        Pointer to allocated MariaDB record
@param[out]     storage	        Internal storage for blobs etc

@retval		true on success
@retval		false on malloc failure or failed to open the maria table
		for purge thread.
*/

bool innobase_allocate_row_for_vcol(THD *thd, const dict_index_t *index,
                                    mem_heap_t **heap, TABLE **table,
                                    VCOL_STORAGE *storage)
{
  TABLE *maria_table;
  String *blob_value_storage;
  if (!*table)
    *table = innodb_find_table_for_vc(thd, index->table);

  /* For purge thread, there is a possiblity that table could have
     dropped, corrupted or unaccessible. */
  if (!*table)
    return false;
  maria_table = *table;
  if (!*heap && !(*heap = mem_heap_create(srv_page_size)))
    return false;

  uchar *record = static_cast<byte *>(mem_heap_alloc(*heap,
                                                    maria_table->s->reclength));

  size_t len = maria_table->s->virtual_not_stored_blob_fields * sizeof(String);
  blob_value_storage = static_cast<String *>(mem_heap_alloc(*heap, len));

  if (!record || !blob_value_storage)
    return false;

  storage->maria_table = maria_table;
  storage->innobase_record = record;
  storage->maria_record = maria_table->field[0]->record_ptr();
  storage->blob_value_storage = blob_value_storage;

  maria_table->move_fields(maria_table->field, record, storage->maria_record);
  maria_table->remember_blob_values(blob_value_storage);

  return true;
}


/** Free memory allocated by innobase_allocate_row_for_vcol() */

void innobase_free_row_for_vcol(VCOL_STORAGE *storage)
{
	TABLE *maria_table= storage->maria_table;
	maria_table->move_fields(maria_table->field, storage->maria_record,
                                 storage->innobase_record);
        maria_table->restore_blob_values(storage->blob_value_storage);
}


void innobase_report_computed_value_failed(dtuple_t *row)
{
  ib::error() << "Compute virtual column values failed for "
              << rec_printer(row).str();
}


/** Get the computed value by supplying the base column values.
@param[in,out]	row		the data row
@param[in]	col		virtual column
@param[in]	index		index
@param[in,out]	local_heap	heap memory for processing large data etc.
@param[in,out]	heap		memory heap that copies the actual index row
@param[in]	ifield		index field
@param[in]	thd		MySQL thread handle
@param[in,out]	mysql_table	mysql table object
@param[in,out]	mysql_rec	MariaDB record buffer
@param[in]	old_table	during ALTER TABLE, this is the old table
				or NULL.
@param[in]	update		update vector for the row, if any
@param[in]	foreign		foreign key information
@return the field filled with computed value, or NULL if just want
to store the value in passed in "my_rec" */
dfield_t*
innobase_get_computed_value(
	dtuple_t*		row,
	const dict_v_col_t*	col,
	const dict_index_t*	index,
	mem_heap_t**		local_heap,
	mem_heap_t*		heap,
	const dict_field_t*	ifield,
	THD*			thd,
	TABLE*			mysql_table,
	byte*			mysql_rec,
	const dict_table_t*	old_table,
	const upd_t*		update,
	bool			ignore_warnings)
{
	byte		rec_buf2[REC_VERSION_56_MAX_INDEX_COL_LEN];
	byte*		buf;
	dfield_t*	field;
	ulint		len;

	const ulint zip_size = old_table
		? old_table->space->zip_size()
		: dict_tf_get_zip_size(index->table->flags);

	ulint		ret = 0;

	dict_index_t *clust_index= dict_table_get_first_index(index->table);

	ut_ad(index->table->vc_templ);
	ut_ad(thd != NULL);
	ut_ad(mysql_table);

	DBUG_ENTER("innobase_get_computed_value");
	const mysql_row_templ_t*
			vctempl =  index->table->vc_templ->vtempl[
				index->table->vc_templ->n_col + col->v_pos];

	if (!heap || index->table->vc_templ->rec_len
		     >= REC_VERSION_56_MAX_INDEX_COL_LEN) {
		if (*local_heap == NULL) {
			*local_heap = mem_heap_create(srv_page_size);
		}

		buf = static_cast<byte*>(mem_heap_alloc(
				*local_heap, index->table->vc_templ->rec_len));
	} else {
		buf = rec_buf2;
	}

	for (ulint i = 0; i < unsigned{col->num_base}; i++) {
		dict_col_t*			base_col = col->base_col[i];
		const dfield_t*			row_field = NULL;
		ulint				col_no = base_col->ind;
		const mysql_row_templ_t*	templ
			= index->table->vc_templ->vtempl[col_no];
		const byte*			data;

		if (update) {
			ulint clust_no = dict_col_get_clust_pos(base_col,
								clust_index);
			ut_ad(clust_no != ULINT_UNDEFINED);
			if (const upd_field_t *uf = upd_get_field_by_field_no(
				    update, uint16_t(clust_no), false)) {
				row_field = &uf->new_val;
			}
		}

		if (!row_field) {
			row_field = dtuple_get_nth_field(row, col_no);
		}

		data = static_cast<const byte*>(row_field->data);
		len = row_field->len;

		if (row_field->ext) {
			if (*local_heap == NULL) {
				*local_heap = mem_heap_create(srv_page_size);
			}

			data = btr_copy_externally_stored_field(
				&len, data, zip_size,
				dfield_get_len(row_field), *local_heap);
		}

		if (len == UNIV_SQL_NULL) {
#if defined __GNUC__ && !defined __clang__ && __GNUC__ < 6
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wconversion" /* GCC 5 may need this here */
#endif
                        mysql_rec[templ->mysql_null_byte_offset]
                                |= (byte) templ->mysql_null_bit_mask;
#if defined __GNUC__ && !defined __clang__ && __GNUC__ < 6
# pragma GCC diagnostic pop
#endif
                        memcpy(mysql_rec + templ->mysql_col_offset,
                               static_cast<const byte*>(
					index->table->vc_templ->default_rec
					+ templ->mysql_col_offset),
                               templ->mysql_col_len);
                } else {

			row_sel_field_store_in_mysql_format(
				mysql_rec + templ->mysql_col_offset,
				templ, index, templ->clust_rec_field_no,
				(const byte*)data, len);

			if (templ->mysql_null_bit_mask) {
				/* It is a nullable column with a
				non-NULL value */
				mysql_rec[templ->mysql_null_byte_offset]
					&= static_cast<byte>(
						~templ->mysql_null_bit_mask);
			}
		}
	}

	field = dtuple_get_nth_v_field(row, col->v_pos);

	MY_BITMAP *old_write_set = dbug_tmp_use_all_columns(mysql_table, &mysql_table->write_set);
	MY_BITMAP *old_read_set = dbug_tmp_use_all_columns(mysql_table, &mysql_table->read_set);
	ret = mysql_table->update_virtual_field(
		mysql_table->field[col->m_col.ind],
		ignore_warnings);
	dbug_tmp_restore_column_map(&mysql_table->read_set, old_read_set);
	dbug_tmp_restore_column_map(&mysql_table->write_set, old_write_set);

	if (ret != 0) {
		DBUG_RETURN(NULL);
	}

	if (vctempl->mysql_null_bit_mask
	    && (mysql_rec[vctempl->mysql_null_byte_offset]
	        & vctempl->mysql_null_bit_mask)) {
		dfield_set_null(field);
		field->type.prtype |= DATA_VIRTUAL;
		DBUG_RETURN(field);
	}

	row_mysql_store_col_in_innobase_format(
		field, buf,
		TRUE, mysql_rec + vctempl->mysql_col_offset,
		vctempl->mysql_col_len, dict_table_is_comp(index->table));
	field->type.prtype |= DATA_VIRTUAL;

	ulint	max_prefix = col->m_col.max_prefix;

	if (max_prefix && ifield
	    && (ifield->prefix_len == 0
	        || ifield->prefix_len > col->m_col.max_prefix)) {
		max_prefix = ifield->prefix_len;
	}

	/* If this is a prefix index, we only need a portion of the field */
	if (max_prefix) {
		len = dtype_get_at_most_n_mbchars(
			col->m_col.prtype,
			col->m_col.mbminlen, col->m_col.mbmaxlen,
			max_prefix,
			field->len,
			static_cast<char*>(dfield_get_data(field)));
		dfield_set_len(field, len);
	}

	if (heap) {
		dfield_dup(field, heap);
	}

	DBUG_RETURN(field);
}


/** Attempt to push down an index condition.
@param[in] keyno MySQL key number
@param[in] idx_cond Index condition to be checked
@return Part of idx_cond which the handler will not evaluate */

class Item*
ha_innobase::idx_cond_push(
	uint		keyno,
	class Item*	idx_cond)
{
	DBUG_ENTER("ha_innobase::idx_cond_push");
	DBUG_ASSERT(keyno != MAX_KEY);
	DBUG_ASSERT(idx_cond != NULL);

	/* We can only evaluate the condition if all columns are stored.*/
	dict_index_t* idx  = innobase_get_index(keyno);
	if (idx && dict_index_has_virtual(idx)) {
		DBUG_RETURN(idx_cond);
	}

	pushed_idx_cond = idx_cond;
	pushed_idx_cond_keyno = keyno;
	in_range_check_pushed_down = TRUE;
	/* We will evaluate the condition entirely */
	DBUG_RETURN(NULL);
}


/** Push a primary key filter.
@param[in]	pk_filter	filter against which primary keys
				are to be checked
@retval	false if pushed (always) */
bool ha_innobase::rowid_filter_push(Rowid_filter* pk_filter)
{
	DBUG_ENTER("ha_innobase::rowid_filter_push");
	DBUG_ASSERT(pk_filter != NULL);
	pushed_rowid_filter= pk_filter;
	DBUG_RETURN(false);
}

static bool is_part_of_a_key_prefix(const Field_longstr *field)
{
  const TABLE_SHARE *s= field->table->s;

  for (uint i= 0; i < s->keys; i++)
  {
    const KEY &key= s->key_info[i];
    for (uint j= 0; j < key.user_defined_key_parts; j++)
    {
      const KEY_PART_INFO &info= key.key_part[j];
      // When field is a part of some key, a key part and field will have the
      // same length. And their length will be different when only some prefix
      // of a field is used as a key part. That's what we're looking for here.
      if (info.field->field_index == field->field_index &&
          info.length != field->field_length)
      {
        DBUG_ASSERT(info.length < field->field_length);
        return true;
      }
    }
  }

  return false;
}

static bool
is_part_of_a_primary_key(const Field* field)
{
	const TABLE_SHARE* s = field->table->s;

	return s->primary_key != MAX_KEY
	       && field->part_of_key.is_set(s->primary_key);
}

bool ha_innobase::can_convert_string(const Field_string *field,
                                     const Column_definition &new_type) const
{
  DBUG_ASSERT(!field->compression_method());
  if (new_type.type_handler() != field->type_handler())
    return false;

  if (new_type.char_length != field->char_length())
    return false;

  const Charset field_cs(field->charset());

  if (new_type.length != field->max_display_length() &&
      (!m_prebuilt->table->not_redundant() ||
       field_cs.mbminlen() == field_cs.mbmaxlen()))
    return false;

  if (new_type.charset != field->charset())
  {
    if (!field_cs.encoding_allows_reinterpret_as(new_type.charset))
      return false;

    if (!field_cs.eq_collation_specific_names(new_type.charset))
      return !is_part_of_a_primary_key(field);

    // Fully indexed case works instantly like
    // Compare_keys::EqualButKeyPartLength. But prefix case isn't implemented.
    if (is_part_of_a_key_prefix(field))
	    return false;

    return true;
  }

  return true;
}

static bool
supports_enlarging(const dict_table_t* table, const Field_varstring* field,
		   const Column_definition& new_type)
{
	return field->field_length <= 127 || new_type.length <= 255
	       || field->field_length > 255 || !table->not_redundant();
}

bool ha_innobase::can_convert_varstring(
    const Field_varstring *field, const Column_definition &new_type) const
{
  if (new_type.length < field->field_length)
    return false;

  if (new_type.char_length < field->char_length())
    return false;

  if (!new_type.compression_method() != !field->compression_method())
    return false;

  if (new_type.type_handler() != field->type_handler())
    return false;

  if (new_type.charset != field->charset())
  {
    if (!supports_enlarging(m_prebuilt->table, field, new_type))
      return false;

    Charset field_cs(field->charset());
    if (!field_cs.encoding_allows_reinterpret_as(new_type.charset))
      return false;

    if (!field_cs.eq_collation_specific_names(new_type.charset))
      return !is_part_of_a_primary_key(field);

    // Fully indexed case works instantly like
    // Compare_keys::EqualButKeyPartLength. But prefix case isn't implemented.
    if (is_part_of_a_key_prefix(field))
      return false;

    return true;
  }

  if (new_type.length != field->field_length)
  {
    if (!supports_enlarging(m_prebuilt->table, field, new_type))
      return false;

    return true;
  }

  return true;
}

static bool is_part_of_a_key(const Field_blob *field)
{
  const TABLE_SHARE *s= field->table->s;

  for (uint i= 0; i < s->keys; i++)
  {
    const KEY &key= s->key_info[i];
    for (uint j= 0; j < key.user_defined_key_parts; j++)
    {
      const KEY_PART_INFO &info= key.key_part[j];
      if (info.field->field_index == field->field_index)
        return true;
    }
  }

  return false;
}

bool ha_innobase::can_convert_blob(const Field_blob *field,
                                   const Column_definition &new_type) const
{
  if (new_type.type_handler() != field->type_handler())
    return false;

  if (!new_type.compression_method() != !field->compression_method())
    return false;

  if (new_type.pack_length != field->pack_length())
    return false;

  if (new_type.charset != field->charset())
  {
    Charset field_cs(field->charset());
    if (!field_cs.encoding_allows_reinterpret_as(new_type.charset))
      return false;

    if (!field_cs.eq_collation_specific_names(new_type.charset))
      return !is_part_of_a_key(field);

    // Fully indexed case works instantly like
    // Compare_keys::EqualButKeyPartLength. But prefix case isn't implemented.
    if (is_part_of_a_key_prefix(field))
      return false;

    return true;
  }

  return true;
}


bool ha_innobase::can_convert_nocopy(const Field &field,
                                     const Column_definition &new_type) const
{
  if (dynamic_cast<const Field_vector *>(&field))
    return false;

  if (const Field_string *tf= dynamic_cast<const Field_string *>(&field))
    return can_convert_string(tf, new_type);

  if (const Field_varstring *tf= dynamic_cast<const Field_varstring *>(&field))
    return can_convert_varstring(tf, new_type);

  if (dynamic_cast<const Field_geom *>(&field))
    return false;

  if (const Field_blob *tf= dynamic_cast<const Field_blob *>(&field))
    return can_convert_blob(tf, new_type);

  return false;
}


Compare_keys ha_innobase::compare_key_parts(
    const Field &old_field, const Column_definition &new_field,
    const KEY_PART_INFO &old_part, const KEY_PART_INFO &new_part) const
{
  const bool is_equal= old_field.is_equal(new_field);
  const CHARSET_INFO *old_cs= old_field.charset();
  const CHARSET_INFO *new_cs= new_field.charset;

  if (!is_equal)
  {
    if (!old_field.table->file->can_convert_nocopy(old_field, new_field))
      return Compare_keys::NotEqual;

    if (!Charset(old_cs).eq_collation_specific_names(new_cs))
      return Compare_keys::NotEqual;
  }

  if (old_part.length / old_cs->mbmaxlen != new_part.length / new_cs->mbmaxlen)
  {
    if (old_part.length != old_field.field_length)
      return Compare_keys::NotEqual;

    if (old_part.length >= new_part.length)
      return Compare_keys::NotEqual;

    if (old_part.length == old_field.key_length() &&
        new_part.length != new_field.length)
      return Compare_keys::NotEqual;

    return Compare_keys::EqualButKeyPartLength;
  }

  return Compare_keys::Equal;
}

/******************************************************************//**
Use this when the args are passed to the format string from
errmsg-utf8.txt directly as is.

Push a warning message to the client, it is a wrapper around:

void push_warning_printf(
	THD *thd, Sql_condition::enum_condition_level level,
	uint code, const char *format, ...);
*/
void
ib_senderrf(
/*========*/
	THD*		thd,		/*!< in/out: session */
	ib_log_level_t	level,		/*!< in: warning level */
	ib_uint32_t	code,		/*!< MySQL error code */
	...)				/*!< Args */
{
	va_list		args;
	const char*	format = my_get_err_msg(code);

	/* If the caller wants to push a message to the client then
	the caller must pass a valid session handle. */

	ut_a(thd != 0);

	/* The error code must exist in the errmsg-utf8.txt file. */
	ut_a(format != 0);

	va_start(args, code);

	myf l;

	switch (level) {
	case IB_LOG_LEVEL_INFO:
		l = ME_NOTE;
		break;
	case IB_LOG_LEVEL_WARN:
		l = ME_WARNING;
		break;
	default:
		l = 0;
		break;
	}

	my_printv_error(code, format, MYF(l), args);

	va_end(args);

	if (level == IB_LOG_LEVEL_FATAL) {
		ut_error;
	}
}

/******************************************************************//**
Use this when the args are first converted to a formatted string and then
passed to the format string from errmsg-utf8.txt. The error message format
must be: "Some string ... %s".

Push a warning message to the client, it is a wrapper around:

void push_warning_printf(
	THD *thd, Sql_condition::enum_condition_level level,
	uint code, const char *format, ...);
*/
void
ib_errf(
/*====*/
	THD*		thd,		/*!< in/out: session */
	ib_log_level_t	level,		/*!< in: warning level */
	ib_uint32_t	code,		/*!< MySQL error code */
	const char*	format,		/*!< printf format */
	...)				/*!< Args */
{
	char*		str = NULL;
	va_list         args;

	/* If the caller wants to push a message to the client then
	the caller must pass a valid session handle. */

	ut_a(thd != 0);
	ut_a(format != 0);

	va_start(args, format);

#ifdef _WIN32
	int		size = _vscprintf(format, args) + 1;
	if (size > 0) {
		str = static_cast<char*>(malloc(size));
	}
	if (str == NULL) {
		va_end(args);
		return;	/* Watch for Out-Of-Memory */
	}
	str[size - 1] = 0x0;
	vsnprintf(str, size, format, args);
#elif HAVE_VASPRINTF
	if (vasprintf(&str, format, args) == -1) {
		/* In case of failure use a fixed length string */
		str = static_cast<char*>(malloc(BUFSIZ));
		vsnprintf(str, BUFSIZ, format, args);
	}
#else
	/* Use a fixed length string. */
	str = static_cast<char*>(malloc(BUFSIZ));
	if (str == NULL) {
		va_end(args);
		return;	/* Watch for Out-Of-Memory */
	}
	vsnprintf(str, BUFSIZ, format, args);
#endif /* _WIN32 */

	ib_senderrf(thd, level, code, str);

	va_end(args);
	free(str);
}

/* Keep the first 16 characters as-is, since the url is sometimes used
as an offset from this.*/
const char*	TROUBLESHOOTING_MSG =
	"Please refer to https://mariadb.com/kb/en/innodb-troubleshooting/"
	" for how to resolve the issue.";

const char*	TROUBLESHOOT_DATADICT_MSG =
	"Please refer to https://mariadb.com/kb/en/innodb-data-dictionary-troubleshooting/"
	" for how to resolve the issue.";

const char*	BUG_REPORT_MSG =
	"Submit a detailed bug report to https://jira.mariadb.org/";

const char*	FORCE_RECOVERY_MSG =
	"Please refer to "
	"https://mariadb.com/kb/en/library/innodb-recovery-modes/"
	" for information about forcing recovery.";

const char*	OPERATING_SYSTEM_ERROR_MSG =
	"Some operating system error numbers are described at"
	" https://mariadb.com/kb/en/library/operating-system-error-codes/";

const char*	FOREIGN_KEY_CONSTRAINTS_MSG =
	"Please refer to https://mariadb.com/kb/en/library/foreign-keys/"
	" for correct foreign key definition.";

const char*	SET_TRANSACTION_MSG =
	"Please refer to https://mariadb.com/kb/en/library/set-transaction/";

const char*	INNODB_PARAMETERS_MSG =
	"Please refer to https://mariadb.com/kb/en/library/innodb-system-variables/";

/**********************************************************************
Converts an identifier from my_charset_filename to UTF-8 charset.
@return result string length, as returned by strconvert() */
uint
innobase_convert_to_system_charset(
/*===============================*/
	char*		to,	/* out: converted identifier */
	const char*	from,	/* in: identifier to convert */
	ulint		len,	/* in: length of 'to', in bytes */
	uint*		errors)	/* out: error return */
{
	CHARSET_INFO*	cs1 = &my_charset_filename;
	CHARSET_INFO*	cs2 = system_charset_info;

	return(static_cast<uint>(strconvert(
				cs1, from, static_cast<uint>(strlen(from)),
				cs2, to, static_cast<uint>(len), errors)));
}

/*************************************************************//**
Check for a valid value of innobase_compression_algorithm.
@return	0 for valid innodb_compression_algorithm. */
static
int
innodb_compression_algorithm_validate(
/*==================================*/
	THD*				thd,	/*!< in: thread handle */
	struct st_mysql_sys_var*	var,	/*!< in: pointer to system
						variable */
	void*				save,	/*!< out: immediate result
						for update function */
	struct st_mysql_value*		value)	/*!< in: incoming string */
{
	DBUG_ENTER("innobase_compression_algorithm_validate");

	if (check_sysvar_enum(thd, var, save, value)) {
		DBUG_RETURN(1);
	}

        if (compression_algorithm_is_not_loaded(*(ulong*)save, ME_WARNING))
          DBUG_RETURN(1);
	DBUG_RETURN(0);
}

static
int
innodb_encrypt_tables_validate(
/*=================================*/
	THD*				thd,	/*!< in: thread handle */
	struct st_mysql_sys_var*	var,	/*!< in: pointer to system
						variable */
	void*				save,	/*!< out: immediate result
						for update function */
	struct st_mysql_value*		value)	/*!< in: incoming string */
{
	if (check_sysvar_enum(thd, var, save, value)) {
		return 1;
	}

	ulong encrypt_tables = *(ulong*)save;

	if (encrypt_tables
	    && !encryption_key_id_exists(FIL_DEFAULT_ENCRYPTION_KEY)) {
		push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
				    HA_ERR_UNSUPPORTED,
				    "InnoDB: cannot enable encryption, "
		                    "encryption plugin is not available");
		return 1;
	}

	return 0;
}

static void innodb_remember_check_sysvar_funcs()
{
	/* remember build-in sysvar check functions */
	ut_ad((MYSQL_SYSVAR_NAME(checksum_algorithm).flags & 0x1FF) == PLUGIN_VAR_ENUM);
	check_sysvar_enum = MYSQL_SYSVAR_NAME(checksum_algorithm).check;

	ut_ad((MYSQL_SYSVAR_NAME(flush_log_at_timeout).flags & 15) == PLUGIN_VAR_INT);
	check_sysvar_int = MYSQL_SYSVAR_NAME(flush_log_at_timeout).check;
}

/** Report that a table cannot be decrypted.
@param thd    connection context
@param table  table that cannot be decrypted
@retval DB_DECRYPTION_FAILED (always) */
ATTRIBUTE_COLD
dberr_t innodb_decryption_failed(THD *thd, dict_table_t *table)
{
  table->file_unreadable= true;
  if (!thd)
    thd= current_thd;
  const int dblen= int(table->name.dblen());
  push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                      HA_ERR_DECRYPTION_FAILED,
                      "Table %.*sQ.%sQ in tablespace " UINT32PF
                      " (file %s) cannot be decrypted.",
                      dblen, table->name.m_name,
                      table->name.m_name + dblen + 1,
                      uint32_t(table->space_id),
                      UT_LIST_GET_FIRST(table->space->chain)->name);
  return DB_DECRYPTION_FAILED;
}

/** Report a foreign key error.
@param error    error to report
@param name     table name
@param foreign  constraint */
ATTRIBUTE_COLD
void innodb_fk_error(const trx_t *trx, dberr_t err, const char *name,
                     const dict_foreign_t& foreign)
{
  const int dblen= int(table_name_t(const_cast<char*>(name)).dblen());
  std::string fk= dict_print_info_on_foreign_key_in_create_format
    (trx, &foreign, false);
  push_warning_printf(trx->mysql_thd, Sql_condition::WARN_LEVEL_WARN,
                      convert_error_code_to_mysql(err, 0, nullptr),
                      "CREATE or ALTER TABLE %.*sQ.%sQ failed%s%.*s",
                      dblen, name, name + dblen + 1,
                      err == DB_DUPLICATE_KEY
                      ? ": duplicate name" : "",
                      int(fk.length()), fk.data());
}

/** Helper function to push warnings from InnoDB internals to SQL-layer.
@param[in]	trx
@param[in]	error		Error code to push as warning
@param[in]	table_name	Table name
@param[in]	format		Warning message
@param[in]	...		Message arguments */
void
ib_foreign_warn(trx_t*	    trx,   /*!< in: trx */
		dberr_t	    error, /*!< in: error code to push as warning */
		const char* table_name,
		const char* format, /*!< in: warning message */
		...)
{
	va_list		    args;
	char*		    buf;
	static FILE*	    ef		 = dict_foreign_err_file;
	static const size_t MAX_BUF_SIZE = 4 * 1024;
	buf = (char*)my_malloc(PSI_INSTRUMENT_ME, MAX_BUF_SIZE, MYF(MY_WME));
	if (!buf) {
		return;
	}

	va_start(args, format);
	vsprintf(buf, format, args);
	va_end(args);

	mysql_mutex_lock(&dict_foreign_err_mutex);
	rewind(ef);
	ut_print_timestamp(ef);
	fprintf(ef, " Error in foreign key constraint of table %s:\n",
		table_name);
	fputs(buf, ef);
	mysql_mutex_unlock(&dict_foreign_err_mutex);

	if (trx && trx->mysql_thd) {
		THD* thd = (THD*)trx->mysql_thd;

		push_warning(
			thd, Sql_condition::WARN_LEVEL_WARN,
			uint(convert_error_code_to_mysql(error, 0, thd)), buf);
	}

	my_free(buf);
}

/********************************************************************//**
Helper function to push frm mismatch error to error log and
if needed to sql-layer. */
void
ib_push_frm_error(
	THD*		thd,		/*!< in: MySQL thd */
	dict_table_t*	ib_table,	/*!< in: InnoDB table */
	TABLE*		table,		/*!< in: MySQL table */
	ulint		n_keys,		/*!< in: InnoDB #keys */
	bool		push_warning)	/*!< in: print warning ? */
{
	switch (ib_table->dict_frm_mismatch) {
	case DICT_FRM_NO_PK:
		sql_print_error("Table %s has a primary key in "
			"InnoDB data dictionary, but not "
			"in MariaDB!"
			" Have you mixed up "
			".frm files from different "
			"installations? See "
			"https://mariadb.com/kb/en/innodb-troubleshooting/\n",
			ib_table->name.m_name);

		if (push_warning) {
			push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
				ER_NO_SUCH_INDEX,
				"InnoDB: Table %s has a "
				"primary key in InnoDB data "
				"dictionary, but not in "
				"MariaDB!", ib_table->name.m_name);
		}
		break;
	case DICT_NO_PK_FRM_HAS:
		sql_print_error(
				"Table %s has no primary key in InnoDB data "
				"dictionary, but has one in MariaDB! If you "
				"created the table with a MariaDB version < "
				"3.23.54 and did not define a primary key, "
				"but defined a unique key with all non-NULL "
				"columns, then MariaDB internally treats that "
				"key as the primary key. You can fix this "
				"error by dump + DROP + CREATE + reimport "
				"of the table.", ib_table->name.m_name);

		if (push_warning) {
			push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
				ER_NO_SUCH_INDEX,
				"InnoDB: Table %s has no "
				"primary key in InnoDB data "
				"dictionary, but has one in "
				"MariaDB!",
				ib_table->name.m_name);
		}
		break;

	case DICT_FRM_INCONSISTENT_KEYS:
		sql_print_error("InnoDB: Table %s contains " ULINTPF " "
			"indexes inside InnoDB, which "
			"is different from the number of "
			"indexes %u defined in the .frm file. See "
			"https://mariadb.com/kb/en/innodb-troubleshooting/\n",
			ib_table->name.m_name, n_keys,
			table->s->keys);

		if (push_warning) {
			push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
				ER_NO_SUCH_INDEX,
				"InnoDB: Table %s contains " ULINTPF " "
				"indexes inside InnoDB, which "
				"is different from the number of "
				"indexes %u defined in the MariaDB ",
                                ib_table->name.m_name, n_keys,
				table->s->keys);
		}
		break;

	case DICT_FRM_CONSISTENT:
	default:
		sql_print_error("InnoDB: Table %s is consistent "
			"on InnoDB data dictionary and MariaDB "
			" FRM file.",
			ib_table->name.m_name);
		ut_error;
		break;
	}
}

/** Writes 8 bytes to nth tuple field
@param[in]	tuple	where to write
@param[in]	nth	index in tuple
@param[in]	data	what to write
@param[in]	buf	field data buffer */
static void set_tuple_col_8(dtuple_t *tuple, int col, uint64_t data, byte *buf)
{
  dfield_t *dfield= dtuple_get_nth_field(tuple, col);
  ut_ad(dfield->type.len == 8);
  if (dfield->len == UNIV_SQL_NULL)
  {
    dfield_set_data(dfield, buf, 8);
  }
  ut_ad(dfield->len == dfield->type.len && dfield->data);
  mach_write_to_8(dfield->data, data);
}

void ins_node_t::vers_update_end(row_prebuilt_t *prebuilt, bool history_row)
{
  ut_ad(prebuilt->ins_node == this);
  trx_t *trx= prebuilt->trx;
#ifndef DBUG_OFF
  ut_ad(table->vers_start != table->vers_end);
  const mysql_row_templ_t *t= prebuilt->get_template_by_col(table->vers_end);
  ut_ad(t);
  ut_ad(t->mysql_col_len == 8);
#endif

  if (history_row)
  {
    set_tuple_col_8(row, table->vers_end, trx->id, vers_end_buf);
  }
  else /* ROW_INS_VERSIONED */
  {
    set_tuple_col_8(row, table->vers_end, TRX_ID_MAX, vers_end_buf);
#ifndef DBUG_OFF
    t= prebuilt->get_template_by_col(table->vers_start);
    ut_ad(t);
    ut_ad(t->mysql_col_len == 8);
#endif
    set_tuple_col_8(row, table->vers_start, trx->id, vers_start_buf);
  }
  dict_index_t *clust_index= dict_table_get_first_index(table);
  THD *thd= trx->mysql_thd;
  TABLE *mysql_table= prebuilt->m_mysql_table;
  mem_heap_t *local_heap= NULL;
  for (ulint col_no= 0; col_no < dict_table_get_n_v_cols(table); col_no++)
  {
    const dict_v_col_t *v_col= dict_table_get_nth_v_col(table, col_no);
    for (ulint i= 0; i < unsigned(v_col->num_base); i++)
      if (v_col->base_col[i]->ind == table->vers_end)
        innobase_get_computed_value(row, v_col, clust_index, &local_heap,
                                    table->heap, NULL, thd, mysql_table,
                                    mysql_table->record[0], NULL, NULL);
  }
  if (UNIV_LIKELY_NULL(local_heap))
    mem_heap_free(local_heap);
}
