/*
 *	check.c
 *
 *	server checks and output routines
 *
 *	Copyright (c) 2010-2022, PostgreSQL Global Development Group
 *	src/bin/pg_upgrade/check.c
 */

#include "postgres_fe.h"

#include "catalog/pg_authid_d.h"
#include "catalog/pg_collation.h"
#include "fe_utils/string_utils.h"
#include "mb/pg_wchar.h"
#include "pg_upgrade.h"

/* YB includes */
#include "lib/stringinfo.h"

static void check_new_cluster_is_empty(void);
static void check_databases_are_compatible(void);
static void check_locale_and_encoding(DbInfo *olddb, DbInfo *newdb);
static bool equivalent_locale(int category, const char *loca, const char *locb);
static void check_is_install_user(ClusterInfo *cluster);
static void check_proper_datallowconn(ClusterInfo *cluster);
static void check_for_prepared_transactions(ClusterInfo *cluster);
static void check_for_isn_and_int8_passing_mismatch(ClusterInfo *cluster);
static void check_for_user_defined_postfix_ops(ClusterInfo *cluster);
static void check_for_incompatible_polymorphics(ClusterInfo *cluster);
static void check_for_tables_with_oids(ClusterInfo *cluster);
static void check_for_composite_data_type_usage(ClusterInfo *cluster);
static void check_for_reg_data_type_usage(ClusterInfo *cluster);
static void check_for_removed_data_type_usage(ClusterInfo *cluster,
											  const char *version,
											  const char *datatype);
static void check_for_jsonb_9_4_usage(ClusterInfo *cluster);
static void check_for_pg_role_prefix(ClusterInfo *cluster);
static void check_for_new_tablespace_dir(ClusterInfo *new_cluster);
static void check_for_user_defined_encoding_conversions(ClusterInfo *cluster);
static char *get_canonical_locale_name(int category, const char *locale);

/* YB functions */
static void yb_check_upgrade_compatibility_guc(PGconn *old_cluster_conn);
static void yb_check_system_databases_exist(PGconn *old_cluster_conn);
static void yb_check_user_attributes(PGconn *old_cluster_conn,
									 const char *user_name,
									 const char **role_attrs);
static void yb_check_yugabyte_user(PGconn *old_cluster_conn);
static void yb_check_old_cluster_user(PGconn *old_cluster_conn);
static void yb_check_installed_extensions();

/*
 * fix_path_separator
 * For non-Windows, just return the argument.
 * For Windows convert any forward slash to a backslash
 * such as is suitable for arguments to builtin commands
 * like RMDIR and DEL.
 */
static char *
fix_path_separator(char *path)
{
#ifdef WIN32

	char	   *result;
	char	   *c;

	result = pg_strdup(path);

	for (c = result; *c != '\0'; c++)
		if (*c == '/')
			*c = '\\';

	return result;
#else

	return path;
#endif
}

void
output_check_banner(bool live_check)
{
	if (user_opts.check && live_check)
	{
		pg_log(PG_REPORT,
			   "Performing Consistency Checks on Old Live Server\n"
			   "------------------------------------------------\n");
	}
	else
	{
		pg_log(PG_REPORT,
			   "Performing Consistency Checks\n"
			   "-----------------------------\n");
	}
}


void
check_and_dump_old_cluster(bool live_check)
{
	/* -- OLD -- */

	if (!is_yugabyte_enabled() && !live_check)
		start_postmaster(&old_cluster, true);

	if (is_yugabyte_enabled())
	{
		PGconn	   *old_cluster_conn = connectToServer(&old_cluster, "template1");

		/*
		 * --check can be run any time before the upgrade. Upgrade Compatibility
		 * GUC is only required to be set during the actual upgrade.
		 */
		if (!user_opts.check)
			yb_check_upgrade_compatibility_guc(old_cluster_conn);

		yb_check_old_cluster_user(old_cluster_conn);
		yb_check_yugabyte_user(old_cluster_conn);
		yb_check_system_databases_exist(old_cluster_conn);

		PQfinish(old_cluster_conn);
	}

	/* Extract a list of databases and tables from the old cluster */
	get_db_and_rel_infos(&old_cluster);

	/* YB: Enable these checks and other functions to initialize new node */
	if (!is_yugabyte_enabled())
		/* Yugabyte does not support tablespace directories */
		init_tablespaces();

	get_loadable_libraries();


	/*
	 * Check for various failure cases
	 */
	if (!is_yugabyte_enabled())
		/* In Yugabyte this is handled by yb_check_yugabyte_user */
		check_is_install_user(&old_cluster);
	check_proper_datallowconn(&old_cluster);
	if (!is_yugabyte_enabled())
		/* Yugabyte does not support prepared transactions, see #1125 */
		check_for_prepared_transactions(&old_cluster);
	check_for_composite_data_type_usage(&old_cluster);
	check_for_reg_data_type_usage(&old_cluster);

	if (!is_yugabyte_enabled())
		/* YB: See comment above call to check_cluster_compatibility */
		check_for_isn_and_int8_passing_mismatch(&old_cluster);

	/*
	 * PG 12 removed types abstime, reltime, tinterval.
	 */
	if (GET_MAJOR_VERSION(old_cluster.major_version) <= 1100)
	{
		check_for_removed_data_type_usage(&old_cluster, "12", "abstime");
		check_for_removed_data_type_usage(&old_cluster, "12", "reltime");
		check_for_removed_data_type_usage(&old_cluster, "12", "tinterval");
	}

	/*
	 * PG 14 changed the function signature of encoding conversion functions.
	 * Conversions from older versions cannot be upgraded automatically
	 * because the user-defined functions used by the encoding conversions
	 * need to be changed to match the new signature.
	 */
	if (!is_yugabyte_enabled() && GET_MAJOR_VERSION(old_cluster.major_version) <= 1300)
		/*
		 * CREATE CONVERSION is not supported by YB,
		 * cannot have user-defined encoding conversions.
		 */
		check_for_user_defined_encoding_conversions(&old_cluster);

	/*
	 * Pre-PG 14 allowed user defined postfix operators, which are not
	 * supported anymore.  Verify there are none, iff applicable.
	 */
	if (GET_MAJOR_VERSION(old_cluster.major_version) <= 1300)
		check_for_user_defined_postfix_ops(&old_cluster);

	/*
	 * PG 14 changed polymorphic functions from anyarray to
	 * anycompatiblearray.
	 */
	if (GET_MAJOR_VERSION(old_cluster.major_version) <= 1300)
		check_for_incompatible_polymorphics(&old_cluster);

	/*
	 * Pre-PG 12 allowed tables to be declared WITH OIDS, which is not
	 * supported anymore. Verify there are none, iff applicable.
	 * In Yugabyte, this was not allowed.
	 */
	if (!is_yugabyte_enabled() && GET_MAJOR_VERSION(old_cluster.major_version) <= 1100)
		check_for_tables_with_oids(&old_cluster);

	/*
	 * PG 12 changed the 'sql_identifier' type storage to be based on name,
	 * not varchar, which breaks on-disk format for existing data. So we need
	 * to prevent upgrade when used in user objects (tables, indexes, ...).
	 */
	if (GET_MAJOR_VERSION(old_cluster.major_version) <= 1100)
		old_11_check_for_sql_identifier_data_type_usage(&old_cluster);

	/*
	 * Pre-PG 10 allowed tables with 'unknown' type columns and non WAL logged
	 * hash indexes
	 */
	/* These checks are irrelevant for Yugabyte (Pre-PG11 checks) */
	if (!is_yugabyte_enabled() && GET_MAJOR_VERSION(old_cluster.major_version) <= 906)
	{
		old_9_6_check_for_unknown_data_type_usage(&old_cluster);
		if (user_opts.check)
			old_9_6_invalidate_hash_indexes(&old_cluster, true);
	}

	/* 9.5 and below should not have roles starting with pg_ */
	if (!is_yugabyte_enabled() && GET_MAJOR_VERSION(old_cluster.major_version) <= 905)
		check_for_pg_role_prefix(&old_cluster);

	if (!is_yugabyte_enabled() && GET_MAJOR_VERSION(old_cluster.major_version) == 904 &&
		old_cluster.controldata.cat_ver < JSONB_FORMAT_CHANGE_CAT_VER)
		check_for_jsonb_9_4_usage(&old_cluster);

	/* Pre-PG 9.4 had a different 'line' data type internal format */
	if (!is_yugabyte_enabled() && GET_MAJOR_VERSION(old_cluster.major_version) <= 903)
		old_9_3_check_for_line_data_type_usage(&old_cluster);

	yb_check_installed_extensions();

	if (yb_has_check_fatal)
		pg_fatal("\n");

	/*
	 * While not a check option, we do this now because this is the only time
	 * the old server is running.
	 */
	if (!user_opts.check)
		generate_old_dump();

	if (!is_yugabyte_enabled() && !live_check)
		stop_postmaster(false);
}


void
check_new_cluster(void)
{
	get_db_and_rel_infos(&new_cluster);

	check_new_cluster_is_empty();
	check_databases_are_compatible();

	check_loadable_libraries();

	switch (user_opts.transfer_mode)
	{
		case TRANSFER_MODE_CLONE:
			check_file_clone();
			break;
		case TRANSFER_MODE_COPY:
			break;
		case TRANSFER_MODE_LINK:
			check_hard_link();
			break;
	}

	check_is_install_user(&new_cluster);

	check_for_prepared_transactions(&new_cluster);

	check_for_new_tablespace_dir(&new_cluster);
}


void
report_clusters_compatible(void)
{
	if (user_opts.check)
	{
		pg_log(PG_REPORT, "\n*Clusters are compatible*\n");
		/* stops new cluster */
		stop_postmaster(false);

		cleanup_output_dirs();
		exit(0);
	}

	pg_log(PG_REPORT, "\n"
		   "If pg_upgrade fails after this point, you must re-initdb the\n"
		   "new cluster before continuing.\n");
}


void
issue_warnings_and_set_wal_level(void)
{
	/*
	 * We unconditionally start/stop the new server because pg_resetwal -o set
	 * wal_level to 'minimum'.  If the user is upgrading standby servers using
	 * the rsync instructions, they will need pg_upgrade to write its final
	 * WAL record showing wal_level as 'replica'.
	 */
	start_postmaster(&new_cluster, true);

	/* Reindex hash indexes for old < 10.0 */
	if (GET_MAJOR_VERSION(old_cluster.major_version) <= 906)
		old_9_6_invalidate_hash_indexes(&new_cluster, false);

	report_extension_updates(&new_cluster);

	stop_postmaster(false);
}


void
output_completion_banner(char *deletion_script_file_name)
{
	PQExpBufferData user_specification;

	initPQExpBuffer(&user_specification);
	if (os_info.user_specified)
	{
		appendPQExpBufferStr(&user_specification, "-U ");
		appendShellString(&user_specification, os_info.user);
		appendPQExpBufferChar(&user_specification, ' ');
	}

	pg_log(PG_REPORT,
		   "Optimizer statistics are not transferred by pg_upgrade.\n"
		   "Once you start the new server, consider running:\n"
		   "    %s/vacuumdb %s--all --analyze-in-stages\n\n", new_cluster.bindir, user_specification.data);

	if (deletion_script_file_name)
		pg_log(PG_REPORT,
			   "Running this script will delete the old cluster's data files:\n"
			   "    %s\n",
			   deletion_script_file_name);
	else
		pg_log(PG_REPORT,
			   "Could not create a script to delete the old cluster's data files\n"
			   "because user-defined tablespaces or the new cluster's data directory\n"
			   "exist in the old cluster directory.  The old cluster's contents must\n"
			   "be deleted manually.\n");

	termPQExpBuffer(&user_specification);
}


void
check_cluster_versions(void)
{
	prep_status("Checking cluster versions");

	/* cluster versions should already have been obtained */
	Assert(old_cluster.major_version != 0);
	Assert(new_cluster.major_version != 0);

	/*
	 * We allow upgrades from/to the same major version for alpha/beta
	 * upgrades
	 */

	if (GET_MAJOR_VERSION(old_cluster.major_version) < 902)
		pg_fatal("This utility can only upgrade from PostgreSQL version %s and later.\n",
				 "9.2");

	/* Only current PG version is supported as a target */
	if (GET_MAJOR_VERSION(new_cluster.major_version) != GET_MAJOR_VERSION(PG_VERSION_NUM))
		pg_fatal("This utility can only upgrade to PostgreSQL version %s.\n",
				 PG_MAJORVERSION);

	/*
	 * We can't allow downgrading because we use the target pg_dump, and
	 * pg_dump cannot operate on newer database versions, only current and
	 * older versions.
	 */
	if (old_cluster.major_version > new_cluster.major_version)
		pg_fatal("This utility cannot be used to downgrade to older major PostgreSQL versions.\n");

	/* Ensure binaries match the designated data directories */
	if (GET_MAJOR_VERSION(old_cluster.major_version) !=
		GET_MAJOR_VERSION(old_cluster.bin_version))
		pg_fatal("Old cluster data and binary directories are from different major versions.\n");
	if (GET_MAJOR_VERSION(new_cluster.major_version) !=
		GET_MAJOR_VERSION(new_cluster.bin_version))
		pg_fatal("New cluster data and binary directories are from different major versions.\n");

	check_ok();
}

void
yb_check_cluster_versions(void)
{
	if (!user_opts.check)
		return;

	prep_status("Checking cluster versions");

	/* cluster versions should already have been obtained */
	Assert(old_cluster.major_version != 0);

	if (GET_MAJOR_VERSION(old_cluster.major_version) > 1100)
		pg_fatal("This version of the utility can only be used for checking "
				 "YSQL version 11. The cluster is currently on YSQL version %s\n",
				 old_cluster.major_version_str);

	check_ok();
}

void
check_cluster_compatibility(bool live_check)
{
	/* get/check pg_control data of servers */
	get_control_data(&old_cluster, live_check);
	get_control_data(&new_cluster, false);
	check_control_data(&old_cluster.controldata, &new_cluster.controldata);

	if (live_check && old_cluster.port == new_cluster.port)
		pg_fatal("When checking a live server, "
				 "the old and new port numbers must be different.\n");
}


/*
 * check_locale_and_encoding()
 *
 * Check that locale and encoding of a database in the old and new clusters
 * are compatible.
 */
static void
check_locale_and_encoding(DbInfo *olddb, DbInfo *newdb)
{
	if (olddb->db_encoding != newdb->db_encoding)
		pg_fatal("encodings for database \"%s\" do not match:  old \"%s\", new \"%s\"\n",
				 olddb->db_name,
				 pg_encoding_to_char(olddb->db_encoding),
				 pg_encoding_to_char(newdb->db_encoding));
	if (!equivalent_locale(LC_COLLATE, olddb->db_collate, newdb->db_collate))
		pg_fatal("lc_collate values for database \"%s\" do not match:  old \"%s\", new \"%s\"\n",
				 olddb->db_name, olddb->db_collate, newdb->db_collate);
	if (!equivalent_locale(LC_CTYPE, olddb->db_ctype, newdb->db_ctype))
		pg_fatal("lc_ctype values for database \"%s\" do not match:  old \"%s\", new \"%s\"\n",
				 olddb->db_name, olddb->db_ctype, newdb->db_ctype);
	if (olddb->db_collprovider != newdb->db_collprovider)
		pg_fatal("locale providers for database \"%s\" do not match:  old \"%s\", new \"%s\"\n",
				 olddb->db_name,
				 collprovider_name(olddb->db_collprovider),
				 collprovider_name(newdb->db_collprovider));
	if ((olddb->db_iculocale == NULL && newdb->db_iculocale != NULL) ||
		(olddb->db_iculocale != NULL && newdb->db_iculocale == NULL) ||
		(olddb->db_iculocale != NULL && newdb->db_iculocale != NULL && strcmp(olddb->db_iculocale, newdb->db_iculocale) != 0))
		pg_fatal("ICU locale values for database \"%s\" do not match:  old \"%s\", new \"%s\"\n",
				 olddb->db_name,
				 olddb->db_iculocale ? olddb->db_iculocale : "(null)",
				 newdb->db_iculocale ? newdb->db_iculocale : "(null)");
}

/*
 * equivalent_locale()
 *
 * Best effort locale-name comparison.  Return false if we are not 100% sure
 * the locales are equivalent.
 *
 * Note: The encoding parts of the names are ignored. This function is
 * currently used to compare locale names stored in pg_database, and
 * pg_database contains a separate encoding field. That's compared directly
 * in check_locale_and_encoding().
 */
static bool
equivalent_locale(int category, const char *loca, const char *locb)
{
	const char *chara;
	const char *charb;
	char	   *canona;
	char	   *canonb;
	int			lena;
	int			lenb;

	/*
	 * If the names are equal, the locales are equivalent. Checking this first
	 * avoids calling setlocale() in the common case that the names are equal.
	 * That's a good thing, if setlocale() is buggy, for example.
	 */
	if (pg_strcasecmp(loca, locb) == 0)
		return true;

	/*
	 * Not identical. Canonicalize both names, remove the encoding parts, and
	 * try again.
	 */
	canona = get_canonical_locale_name(category, loca);
	chara = strrchr(canona, '.');
	lena = chara ? (chara - canona) : strlen(canona);

	canonb = get_canonical_locale_name(category, locb);
	charb = strrchr(canonb, '.');
	lenb = charb ? (charb - canonb) : strlen(canonb);

	if (lena == lenb && pg_strncasecmp(canona, canonb, lena) == 0)
	{
		pg_free(canona);
		pg_free(canonb);
		return true;
	}

	pg_free(canona);
	pg_free(canonb);
	return false;
}


static void
check_new_cluster_is_empty(void)
{
	int			dbnum;

	for (dbnum = 0; dbnum < new_cluster.dbarr.ndbs; dbnum++)
	{
		int			relnum;
		RelInfoArr *rel_arr = &new_cluster.dbarr.dbs[dbnum].rel_arr;

		for (relnum = 0; relnum < rel_arr->nrels;
			 relnum++)
		{
			/* pg_largeobject and its index should be skipped */
			if (strcmp(rel_arr->rels[relnum].nspname, "pg_catalog") != 0)
				pg_fatal("New cluster database \"%s\" is not empty: found relation \"%s.%s\"\n",
						 new_cluster.dbarr.dbs[dbnum].db_name,
						 rel_arr->rels[relnum].nspname,
						 rel_arr->rels[relnum].relname);
		}
	}
}

/*
 * Check that every database that already exists in the new cluster is
 * compatible with the corresponding database in the old one.
 */
static void
check_databases_are_compatible(void)
{
	int			newdbnum;
	int			olddbnum;
	DbInfo	   *newdbinfo;
	DbInfo	   *olddbinfo;

	for (newdbnum = 0; newdbnum < new_cluster.dbarr.ndbs; newdbnum++)
	{
		newdbinfo = &new_cluster.dbarr.dbs[newdbnum];

		/* Find the corresponding database in the old cluster */
		for (olddbnum = 0; olddbnum < old_cluster.dbarr.ndbs; olddbnum++)
		{
			olddbinfo = &old_cluster.dbarr.dbs[olddbnum];
			if (strcmp(newdbinfo->db_name, olddbinfo->db_name) == 0)
			{
				check_locale_and_encoding(olddbinfo, newdbinfo);
				break;
			}
		}
	}
}

/*
 * A previous run of pg_upgrade might have failed and the new cluster
 * directory recreated, but they might have forgotten to remove
 * the new cluster's tablespace directories.  Therefore, check that
 * new cluster tablespace directories do not already exist.  If
 * they do, it would cause an error while restoring global objects.
 * This allows the failure to be detected at check time, rather than
 * during schema restore.
 */
static void
check_for_new_tablespace_dir(ClusterInfo *new_cluster)
{
	int			tblnum;
	char		new_tablespace_dir[MAXPGPATH];

	prep_status("Checking for new cluster tablespace directories");

	for (tblnum = 0; tblnum < os_info.num_old_tablespaces; tblnum++)
	{
		struct stat statbuf;

		snprintf(new_tablespace_dir, MAXPGPATH, "%s%s",
				 os_info.old_tablespaces[tblnum],
				 new_cluster->tablespace_suffix);

		if (stat(new_tablespace_dir, &statbuf) == 0 || errno != ENOENT)
			pg_fatal("new cluster tablespace directory already exists: \"%s\"\n",
					 new_tablespace_dir);
	}

	check_ok();
}

/*
 * create_script_for_old_cluster_deletion()
 *
 *	This is particularly useful for tablespace deletion.
 */
void
create_script_for_old_cluster_deletion(char **deletion_script_file_name)
{
	FILE	   *script = NULL;
	int			tblnum;
	char		old_cluster_pgdata[MAXPGPATH],
				new_cluster_pgdata[MAXPGPATH];

	*deletion_script_file_name = psprintf("%sdelete_old_cluster.%s",
										  SCRIPT_PREFIX, SCRIPT_EXT);

	strlcpy(old_cluster_pgdata, old_cluster.pgdata, MAXPGPATH);
	canonicalize_path(old_cluster_pgdata);

	strlcpy(new_cluster_pgdata, new_cluster.pgdata, MAXPGPATH);
	canonicalize_path(new_cluster_pgdata);

	/* Some people put the new data directory inside the old one. */
	if (path_is_prefix_of_path(old_cluster_pgdata, new_cluster_pgdata))
	{
		pg_log(PG_WARNING,
			   "\nWARNING:  new data directory should not be inside the old data directory, i.e. %s\n", old_cluster_pgdata);

		/* Unlink file in case it is left over from a previous run. */
		unlink(*deletion_script_file_name);
		pg_free(*deletion_script_file_name);
		*deletion_script_file_name = NULL;
		return;
	}

	/*
	 * Some users (oddly) create tablespaces inside the cluster data
	 * directory.  We can't create a proper old cluster delete script in that
	 * case.
	 */
	for (tblnum = 0; tblnum < os_info.num_old_tablespaces; tblnum++)
	{
		char		old_tablespace_dir[MAXPGPATH];

		strlcpy(old_tablespace_dir, os_info.old_tablespaces[tblnum], MAXPGPATH);
		canonicalize_path(old_tablespace_dir);
		if (path_is_prefix_of_path(old_cluster_pgdata, old_tablespace_dir))
		{
			/* reproduce warning from CREATE TABLESPACE that is in the log */
			pg_log(PG_WARNING,
				   "\nWARNING:  user-defined tablespace locations should not be inside the data directory, i.e. %s\n", old_tablespace_dir);

			/* Unlink file in case it is left over from a previous run. */
			unlink(*deletion_script_file_name);
			pg_free(*deletion_script_file_name);
			*deletion_script_file_name = NULL;
			return;
		}
	}

	prep_status("Creating script to delete old cluster");

	if ((script = fopen_priv(*deletion_script_file_name, "w")) == NULL)
		pg_fatal("could not open file \"%s\": %s\n",
				 *deletion_script_file_name, strerror(errno));

#ifndef WIN32
	/* add shebang header */
	fprintf(script, "#!/bin/sh\n\n");
#endif

	/* delete old cluster's default tablespace */
	fprintf(script, RMDIR_CMD " %c%s%c\n", PATH_QUOTE,
			fix_path_separator(old_cluster.pgdata), PATH_QUOTE);

	/* delete old cluster's alternate tablespaces */
	for (tblnum = 0; tblnum < os_info.num_old_tablespaces; tblnum++)
	{
		/*
		 * Do the old cluster's per-database directories share a directory
		 * with a new version-specific tablespace?
		 */
		if (strlen(old_cluster.tablespace_suffix) == 0)
		{
			/* delete per-database directories */
			int			dbnum;

			fprintf(script, "\n");

			for (dbnum = 0; dbnum < old_cluster.dbarr.ndbs; dbnum++)
				fprintf(script, RMDIR_CMD " %c%s%c%u%c\n", PATH_QUOTE,
						fix_path_separator(os_info.old_tablespaces[tblnum]),
						PATH_SEPARATOR, old_cluster.dbarr.dbs[dbnum].db_oid,
						PATH_QUOTE);
		}
		else
		{
			char	   *suffix_path = pg_strdup(old_cluster.tablespace_suffix);

			/*
			 * Simply delete the tablespace directory, which might be ".old"
			 * or a version-specific subdirectory.
			 */
			fprintf(script, RMDIR_CMD " %c%s%s%c\n", PATH_QUOTE,
					fix_path_separator(os_info.old_tablespaces[tblnum]),
					fix_path_separator(suffix_path), PATH_QUOTE);
			pfree(suffix_path);
		}
	}

	fclose(script);

#ifndef WIN32
	if (chmod(*deletion_script_file_name, S_IRWXU) != 0)
		pg_fatal("could not add execute permission to file \"%s\": %s\n",
				 *deletion_script_file_name, strerror(errno));
#endif

	check_ok();
}


/*
 *	check_is_install_user()
 *
 *	Check we are the install user, and that the new cluster
 *	has no other users.
 */
static void
check_is_install_user(ClusterInfo *cluster)
{
	PGresult   *res;
	PGconn	   *conn = connectToServer(cluster, "template1");

	prep_status("Checking database user is the install user");

	/* Can't use pg_authid because only superusers can view it. */
	res = executeQueryOrDie(conn,
							"SELECT rolsuper, oid "
							"FROM pg_catalog.pg_roles "
							"WHERE rolname = current_user "
							"AND rolname !~ '^pg_'");

	/*
	 * We only allow the install user in the new cluster (see comment below)
	 * and we preserve pg_authid.oid, so this must be the install user in the
	 * old cluster too.
	 */
	if (PQntuples(res) != 1 ||
		atooid(PQgetvalue(res, 0, 1)) != BOOTSTRAP_SUPERUSERID)
		pg_fatal("database user \"%s\" is not the install user\n",
				 os_info.user);

	PQclear(res);

	res = executeQueryOrDie(conn,
							"SELECT COUNT(*) "
							"FROM pg_catalog.pg_roles "
							"WHERE rolname !~ '^pg_'");

	if (PQntuples(res) != 1)
		pg_fatal("could not determine the number of users\n");

	/*
	 * We only allow the install user in the new cluster because other defined
	 * users might match users defined in the old cluster and generate an
	 * error during pg_dump restore.
	 */
	if (cluster == &new_cluster && atooid(PQgetvalue(res, 0, 0)) != 1)
		pg_fatal("Only the install user can be defined in the new cluster.\n");

	PQclear(res);

	PQfinish(conn);

	check_ok();
}


/*
 *	check_proper_datallowconn
 *
 *	Ensure that all non-template0 databases allow connections since they
 *	otherwise won't be restored; and that template0 explicitly doesn't allow
 *	connections since it would make pg_dumpall --globals restore fail.
 */
static void
check_proper_datallowconn(ClusterInfo *cluster)
{
	int			dbnum;
	PGconn	   *conn_template1;
	PGresult   *dbres;
	int			ntups;
	int			i_datname;
	int			i_datallowconn;
	FILE	   *script = NULL;
	char		output_path[MAXPGPATH];
	bool		found = false;

	prep_status("Checking database connection settings");

	snprintf(output_path, sizeof(output_path), "%s/%s",
			 log_opts.basedir,
			 "databases_with_datallowconn_false.txt");

	conn_template1 = connectToServer(cluster, "template1");

	/* get database names */
	dbres = executeQueryOrDie(conn_template1,
							  "SELECT	datname, datallowconn "
							  "FROM	pg_catalog.pg_database");

	i_datname = PQfnumber(dbres, "datname");
	i_datallowconn = PQfnumber(dbres, "datallowconn");

	ntups = PQntuples(dbres);
	for (dbnum = 0; dbnum < ntups; dbnum++)
	{
		char	   *datname = PQgetvalue(dbres, dbnum, i_datname);
		char	   *datallowconn = PQgetvalue(dbres, dbnum, i_datallowconn);

		if (strcmp(datname, "template0") == 0)
		{
			/* avoid restore failure when pg_dumpall tries to create template0 */
			if (strcmp(datallowconn, "t") == 0)
				pg_fatal("template0 must not allow connections, "
						 "i.e. its pg_database.datallowconn must be false\n");
		}
		else
		{
			/*
			 * avoid datallowconn == false databases from being skipped on
			 * restore
			 */
			if (strcmp(datallowconn, "f") == 0)
			{
				found = true;
				if (script == NULL && (script = fopen_priv(output_path, "w")) == NULL)
					pg_fatal("could not open file \"%s\": %s\n",
							 output_path, strerror(errno));

				yb_fprintf_and_log(script, "%s\n", datname);
			}
		}
	}

	PQclear(dbres);

	PQfinish(conn_template1);

	if (script)
		fclose(script);

	if (found)
	{
		if (!is_yugabyte_enabled())
			pg_log(PG_REPORT, "fatal\n");
		yb_fatal("All non-template0 databases must allow connections, i.e. their\n"
				 "pg_database.datallowconn must be true.  Your installation contains\n"
				 "non-template0 databases with their pg_database.datallowconn set to\n"
				 "false.  Consider allowing connection for all non-template0 databases\n"
				 "or drop the databases which do not allow connections.  A list of\n"
				 "databases with the problem is printed above and in the file:\n"
				 "    %s\n\n", output_path);
	}
	else
		check_ok();
}


/*
 *	check_for_prepared_transactions()
 *
 *	Make sure there are no prepared transactions because the storage format
 *	might have changed.
 */
static void
check_for_prepared_transactions(ClusterInfo *cluster)
{
	PGresult   *res;
	PGconn	   *conn = connectToServer(cluster, "template1");

	prep_status("Checking for prepared transactions");

	res = executeQueryOrDie(conn,
							"SELECT * "
							"FROM pg_catalog.pg_prepared_xacts");

	if (PQntuples(res) != 0)
	{
		if (cluster == &old_cluster)
			pg_fatal("The source cluster contains prepared transactions\n");
		else
			pg_fatal("The target cluster contains prepared transactions\n");
	}

	PQclear(res);

	PQfinish(conn);

	check_ok();
}


/*
 *	check_for_isn_and_int8_passing_mismatch()
 *
 *	contrib/isn relies on data type int8, and in 8.4 int8 can now be passed
 *	by value.  The schema dumps the CREATE TYPE PASSEDBYVALUE setting so
 *	it must match for the old and new servers.
 */
static void
check_for_isn_and_int8_passing_mismatch(ClusterInfo *cluster)
{
	int			dbnum;
	FILE	   *script = NULL;
	bool		found = false;
	char		output_path[MAXPGPATH];

	prep_status("Checking for contrib/isn with bigint-passing mismatch");

	if (old_cluster.controldata.float8_pass_by_value ==
		new_cluster.controldata.float8_pass_by_value)
	{
		/* no mismatch */
		check_ok();
		return;
	}

	snprintf(output_path, sizeof(output_path), "%s/%s",
			 log_opts.basedir,
			 "contrib_isn_and_int8_pass_by_value.txt");

	for (dbnum = 0; dbnum < cluster->dbarr.ndbs; dbnum++)
	{
		PGresult   *res;
		bool		db_used = false;
		int			ntups;
		int			rowno;
		int			i_nspname,
					i_proname;
		DbInfo	   *active_db = &cluster->dbarr.dbs[dbnum];
		PGconn	   *conn = connectToServer(cluster, active_db->db_name);

		/* Find any functions coming from contrib/isn */
		res = executeQueryOrDie(conn,
								"SELECT n.nspname, p.proname "
								"FROM	pg_catalog.pg_proc p, "
								"		pg_catalog.pg_namespace n "
								"WHERE	p.pronamespace = n.oid AND "
								"		p.probin = '$libdir/isn'");

		ntups = PQntuples(res);
		i_nspname = PQfnumber(res, "nspname");
		i_proname = PQfnumber(res, "proname");
		for (rowno = 0; rowno < ntups; rowno++)
		{
			found = true;
			if (script == NULL && (script = fopen_priv(output_path, "w")) == NULL)
				pg_fatal("could not open file \"%s\": %s\n",
						 output_path, strerror(errno));
			if (!db_used)
			{
				fprintf(script, "In database: %s\n", active_db->db_name);
				db_used = true;
			}
			fprintf(script, "  %s.%s\n",
					PQgetvalue(res, rowno, i_nspname),
					PQgetvalue(res, rowno, i_proname));
		}

		PQclear(res);

		PQfinish(conn);
	}

	if (script)
		fclose(script);

	if (found)
	{
		pg_log(PG_REPORT, "fatal\n");
		pg_fatal("Your installation contains \"contrib/isn\" functions which rely on the\n"
				 "bigint data type.  Your old and new clusters pass bigint values\n"
				 "differently so this cluster cannot currently be upgraded.  You can\n"
				 "manually dump databases in the old cluster that use \"contrib/isn\"\n"
				 "facilities, drop them, perform the upgrade, and then restore them.  A\n"
				 "list of the problem functions is in the file:\n"
				 "    %s\n\n", output_path);
	}
	else
		check_ok();
}

/*
 * Verify that no user defined postfix operators exist.
 */
static void
check_for_user_defined_postfix_ops(ClusterInfo *cluster)
{
	int			dbnum;
	FILE	   *script = NULL;
	bool		found = false;
	char		output_path[MAXPGPATH];

	prep_status("Checking for user-defined postfix operators");

	snprintf(output_path, sizeof(output_path), "%s/%s",
			 log_opts.basedir,
			 "postfix_ops.txt");

	/* Find any user defined postfix operators */
	for (dbnum = 0; dbnum < cluster->dbarr.ndbs; dbnum++)
	{
		PGresult   *res;
		bool		db_used = false;
		int			ntups;
		int			rowno;
		int			i_oproid,
					i_oprnsp,
					i_oprname,
					i_typnsp,
					i_typname;
		DbInfo	   *active_db = &cluster->dbarr.dbs[dbnum];
		PGconn	   *conn = connectToServer(cluster, active_db->db_name);

		/*
		 * The query below hardcodes FirstNormalObjectId as 16384 rather than
		 * interpolating that C #define into the query because, if that
		 * #define is ever changed, the cutoff we want to use is the value
		 * used by pre-version 14 servers, not that of some future version.
		 */
		res = executeQueryOrDie(conn,
								"SELECT o.oid AS oproid, "
								"       n.nspname AS oprnsp, "
								"       o.oprname, "
								"       tn.nspname AS typnsp, "
								"       t.typname "
								"FROM pg_catalog.pg_operator o, "
								"     pg_catalog.pg_namespace n, "
								"     pg_catalog.pg_type t, "
								"     pg_catalog.pg_namespace tn "
								"WHERE o.oprnamespace = n.oid AND "
								"      o.oprleft = t.oid AND "
								"      t.typnamespace = tn.oid AND "
								"      o.oprright = 0 AND "
								"      o.oid >= 16384");
		ntups = PQntuples(res);
		i_oproid = PQfnumber(res, "oproid");
		i_oprnsp = PQfnumber(res, "oprnsp");
		i_oprname = PQfnumber(res, "oprname");
		i_typnsp = PQfnumber(res, "typnsp");
		i_typname = PQfnumber(res, "typname");
		for (rowno = 0; rowno < ntups; rowno++)
		{
			found = true;
			if (script == NULL &&
				(script = fopen_priv(output_path, "w")) == NULL)
				pg_fatal("could not open file \"%s\": %s\n",
						 output_path, strerror(errno));
			if (!db_used)
			{
				yb_fprintf_and_log(script, "In database: %s\n", active_db->db_name);
				db_used = true;
			}
			yb_fprintf_and_log(script, "  (oid=%s) %s.%s (%s.%s, NONE)\n",
							   PQgetvalue(res, rowno, i_oproid),
							   PQgetvalue(res, rowno, i_oprnsp),
							   PQgetvalue(res, rowno, i_oprname),
							   PQgetvalue(res, rowno, i_typnsp),
							   PQgetvalue(res, rowno, i_typname));
		}

		PQclear(res);

		PQfinish(conn);
	}

	if (script)
		fclose(script);

	if (found)
	{
		if (!is_yugabyte_enabled())
			pg_log(PG_REPORT, "fatal\n");
		yb_fatal("Your installation contains user-defined postfix operators, which are not\n"
				 "supported anymore.  Consider dropping the postfix operators and replacing\n"
				 "them with prefix operators or function calls.\n"
				 "A list of user-defined postfix operators is printed above and in the file:\n"
				 "    %s\n\n", output_path);
	}
	else
		check_ok();
}

/*
 *	check_for_incompatible_polymorphics()
 *
 *	Make sure nothing is using old polymorphic functions with
 *	anyarray/anyelement rather than the new anycompatible variants.
 */
static void
check_for_incompatible_polymorphics(ClusterInfo *cluster)
{
	PGresult   *res;
	FILE	   *script = NULL;
	char		output_path[MAXPGPATH];
	PQExpBufferData old_polymorphics;

	prep_status("Checking for incompatible polymorphic functions");

	snprintf(output_path, sizeof(output_path), "%s/%s",
			 log_opts.basedir,
			 "incompatible_polymorphics.txt");

	/* The set of problematic functions varies a bit in different versions */
	initPQExpBuffer(&old_polymorphics);

	appendPQExpBufferStr(&old_polymorphics,
						 "'array_append(anyarray,anyelement)'"
						 ", 'array_cat(anyarray,anyarray)'"
						 ", 'array_prepend(anyelement,anyarray)'");

	if (GET_MAJOR_VERSION(cluster->major_version) >= 903)
		appendPQExpBufferStr(&old_polymorphics,
							 ", 'array_remove(anyarray,anyelement)'"
							 ", 'array_replace(anyarray,anyelement,anyelement)'");

	if (GET_MAJOR_VERSION(cluster->major_version) >= 905)
		appendPQExpBufferStr(&old_polymorphics,
							 ", 'array_position(anyarray,anyelement)'"
							 ", 'array_position(anyarray,anyelement,integer)'"
							 ", 'array_positions(anyarray,anyelement)'"
							 ", 'width_bucket(anyelement,anyarray)'");

	for (int dbnum = 0; dbnum < cluster->dbarr.ndbs; dbnum++)
	{
		bool		db_used = false;
		DbInfo	   *active_db = &cluster->dbarr.dbs[dbnum];
		PGconn	   *conn = connectToServer(cluster, active_db->db_name);
		int			ntups;
		int			i_objkind,
					i_objname;

		/*
		 * The query below hardcodes FirstNormalObjectId as 16384 rather than
		 * interpolating that C #define into the query because, if that
		 * #define is ever changed, the cutoff we want to use is the value
		 * used by pre-version 14 servers, not that of some future version.
		 */
		res = executeQueryOrDie(conn,
		/* Aggregate transition functions */
								"SELECT 'aggregate' AS objkind, p.oid::regprocedure::text AS objname "
								"FROM pg_proc AS p "
								"JOIN pg_aggregate AS a ON a.aggfnoid=p.oid "
								"JOIN pg_proc AS transfn ON transfn.oid=a.aggtransfn "
								"WHERE p.oid >= 16384 "
								"AND a.aggtransfn = ANY(ARRAY[%s]::regprocedure[]) "
								"AND a.aggtranstype = ANY(ARRAY['anyarray', 'anyelement']::regtype[]) "

		/* Aggregate final functions */
								"UNION ALL "
								"SELECT 'aggregate' AS objkind, p.oid::regprocedure::text AS objname "
								"FROM pg_proc AS p "
								"JOIN pg_aggregate AS a ON a.aggfnoid=p.oid "
								"JOIN pg_proc AS finalfn ON finalfn.oid=a.aggfinalfn "
								"WHERE p.oid >= 16384 "
								"AND a.aggfinalfn = ANY(ARRAY[%s]::regprocedure[]) "
								"AND a.aggtranstype = ANY(ARRAY['anyarray', 'anyelement']::regtype[]) "

		/* Operators */
								"UNION ALL "
								"SELECT 'operator' AS objkind, op.oid::regoperator::text AS objname "
								"FROM pg_operator AS op "
								"WHERE op.oid >= 16384 "
								"AND oprcode = ANY(ARRAY[%s]::regprocedure[]) "
								"AND oprleft = ANY(ARRAY['anyarray', 'anyelement']::regtype[]);",
								old_polymorphics.data,
								old_polymorphics.data,
								old_polymorphics.data);

		ntups = PQntuples(res);

		i_objkind = PQfnumber(res, "objkind");
		i_objname = PQfnumber(res, "objname");

		for (int rowno = 0; rowno < ntups; rowno++)
		{
			if (script == NULL &&
				(script = fopen_priv(output_path, "w")) == NULL)
				pg_fatal("could not open file \"%s\": %s\n",
						 output_path, strerror(errno));
			if (!db_used)
			{
				yb_fprintf_and_log(script, "In database: %s\n", active_db->db_name);
				db_used = true;
			}

			yb_fprintf_and_log(script, "  %s: %s\n",
							   PQgetvalue(res, rowno, i_objkind),
							   PQgetvalue(res, rowno, i_objname));
		}

		PQclear(res);
		PQfinish(conn);
	}

	if (script)
	{
		fclose(script);
		if (!is_yugabyte_enabled())
			pg_log(PG_REPORT, "fatal\n");
		yb_fatal("Your installation contains user-defined objects that refer to internal\n"
				 "polymorphic functions with arguments of type \"anyarray\" or \"anyelement\".\n"
				 "These user-defined objects must be dropped before upgrading and restored\n"
				 "afterwards, changing them to refer to the new corresponding functions with\n"
				 "arguments of type \"anycompatiblearray\" and \"anycompatible\".\n"
				 "A list of the problematic objects is printed above and in the file:\n"
				 "    %s\n\n", output_path);
	}
	else
		check_ok();

	termPQExpBuffer(&old_polymorphics);
}

/*
 * Verify that no tables are declared WITH OIDS.
 */
static void
check_for_tables_with_oids(ClusterInfo *cluster)
{
	int			dbnum;
	FILE	   *script = NULL;
	bool		found = false;
	char		output_path[MAXPGPATH];

	prep_status("Checking for tables WITH OIDS");

	snprintf(output_path, sizeof(output_path), "%s/%s",
			 log_opts.basedir,
			 "tables_with_oids.txt");

	/* Find any tables declared WITH OIDS */
	for (dbnum = 0; dbnum < cluster->dbarr.ndbs; dbnum++)
	{
		PGresult   *res;
		bool		db_used = false;
		int			ntups;
		int			rowno;
		int			i_nspname,
					i_relname;
		DbInfo	   *active_db = &cluster->dbarr.dbs[dbnum];
		PGconn	   *conn = connectToServer(cluster, active_db->db_name);

		res = executeQueryOrDie(conn,
								"SELECT n.nspname, c.relname "
								"FROM	pg_catalog.pg_class c, "
								"		pg_catalog.pg_namespace n "
								"WHERE	c.relnamespace = n.oid AND "
								"		c.relhasoids AND"
								"       n.nspname NOT IN ('pg_catalog')");

		ntups = PQntuples(res);
		i_nspname = PQfnumber(res, "nspname");
		i_relname = PQfnumber(res, "relname");
		for (rowno = 0; rowno < ntups; rowno++)
		{
			found = true;
			if (script == NULL && (script = fopen_priv(output_path, "w")) == NULL)
				pg_fatal("could not open file \"%s\": %s\n",
						 output_path, strerror(errno));
			if (!db_used)
			{
				fprintf(script, "In database: %s\n", active_db->db_name);
				db_used = true;
			}
			fprintf(script, "  %s.%s\n",
					PQgetvalue(res, rowno, i_nspname),
					PQgetvalue(res, rowno, i_relname));
		}

		PQclear(res);

		PQfinish(conn);
	}

	if (script)
		fclose(script);

	if (found)
	{
		pg_log(PG_REPORT, "fatal\n");
		pg_fatal("Your installation contains tables declared WITH OIDS, which is not\n"
				 "supported anymore.  Consider removing the oid column using\n"
				 "    ALTER TABLE ... SET WITHOUT OIDS;\n"
				 "A list of tables with the problem is in the file:\n"
				 "    %s\n\n", output_path);
	}
	else
		check_ok();
}


/*
 * check_for_composite_data_type_usage()
 *	Check for system-defined composite types used in user tables.
 *
 *	The OIDs of rowtypes of system catalogs and information_schema views
 *	can change across major versions; unlike user-defined types, we have
 *	no mechanism for forcing them to be the same in the new cluster.
 *	Hence, if any user table uses one, that's problematic for pg_upgrade.
 */
static void
check_for_composite_data_type_usage(ClusterInfo *cluster)
{
	bool		found;
	Oid			firstUserOid;
	char		output_path[MAXPGPATH];
	char	   *base_query;

	prep_status("Checking for system-defined composite types in user tables");

	snprintf(output_path, sizeof(output_path), "%s/%s",
			 log_opts.basedir,
			 "tables_using_composite.txt");

	/*
	 * Look for composite types that were made during initdb *or* belong to
	 * information_schema; that's important in case information_schema was
	 * dropped and reloaded.
	 *
	 * The cutoff OID here should match the source cluster's value of
	 * FirstNormalObjectId.  We hardcode it rather than using that C #define
	 * because, if that #define is ever changed, our own version's value is
	 * NOT what to use.  Eventually we may need a test on the source cluster's
	 * version to select the correct value.
	 */
	firstUserOid = 16384;

	base_query = psprintf("SELECT t.oid FROM pg_catalog.pg_type t "
						  "LEFT JOIN pg_catalog.pg_namespace n ON t.typnamespace = n.oid "
						  " WHERE typtype = 'c' AND (t.oid < %u OR nspname = 'information_schema')",
						  firstUserOid);

	found = check_for_data_types_usage(cluster, base_query, output_path);

	free(base_query);

	if (found)
	{
		if (!is_yugabyte_enabled())
			pg_log(PG_REPORT, "fatal\n");
		yb_fatal("Your installation contains system-defined composite type(s) in user tables.\n"
				 "These type OIDs are not stable across PostgreSQL versions,\n"
				 "so this cluster cannot currently be upgraded.  You can\n"
				 "drop the problem columns and restart the upgrade.\n"
				 "A list of the problem columns is printed above and in the file:\n"
				 "    %s\n\n", output_path);
	}
	else
		check_ok();
}

/*
 * check_for_reg_data_type_usage()
 *	pg_upgrade only preserves these system values:
 *		pg_class.oid
 *		pg_type.oid
 *		pg_enum.oid
 *
 *	Many of the reg* data types reference system catalog info that is
 *	not preserved, and hence these data types cannot be used in user
 *	tables upgraded by pg_upgrade.
 */
static void
check_for_reg_data_type_usage(ClusterInfo *cluster)
{
	bool		found;
	char		output_path[MAXPGPATH];

	prep_status("Checking for reg* data types in user tables");

	snprintf(output_path, sizeof(output_path), "%s/%s",
			 log_opts.basedir,
			 "tables_using_reg.txt");

	/*
	 * Note: older servers will not have all of these reg* types, so we have
	 * to write the query like this rather than depending on casts to regtype.
	 */
	found = check_for_data_types_usage(cluster,
									   "SELECT oid FROM pg_catalog.pg_type t "
									   "WHERE t.typnamespace = "
									   "        (SELECT oid FROM pg_catalog.pg_namespace "
									   "         WHERE nspname = 'pg_catalog') "
									   "  AND t.typname IN ( "
	/* pg_class.oid is preserved, so 'regclass' is OK */
									   "           'regcollation', "
									   "           'regconfig', "
									   "           'regdictionary', "
									   "           'regnamespace', "
									   "           'regoper', "
									   "           'regoperator', "
									   "           'regproc', "
									   "           'regprocedure' "
	/* pg_authid.oid is preserved, so 'regrole' is OK */
	/* pg_type.oid is (mostly) preserved, so 'regtype' is OK */
									   "         )",
									   output_path);

	if (found)
	{
		if (!is_yugabyte_enabled())
			pg_log(PG_REPORT, "fatal\n");
		yb_fatal("Your installation contains one of the reg* data types in user tables.\n"
				 "These data types reference system OIDs that are not preserved by\n"
				 "pg_upgrade, so this cluster cannot currently be upgraded.  You can\n"
				 "drop the problem columns and restart the upgrade.\n"
				 "A list of the problem columns is printed above and in the file:\n"
				 "    %s\n\n", output_path);
	}
	else
		check_ok();
}

/*
 * check_for_removed_data_type_usage
 *
 *	Check for in-core data types that have been removed.  Callers know
 *	the exact list.
 */
static void
check_for_removed_data_type_usage(ClusterInfo *cluster, const char *version,
								  const char *datatype)
{
	char		output_path[MAXPGPATH];
	char		typename[NAMEDATALEN];

	prep_status("Checking for removed \"%s\" data type in user tables",
				datatype);

	snprintf(output_path, sizeof(output_path), "tables_using_%s.txt",
			 datatype);
	snprintf(typename, sizeof(typename), "pg_catalog.%s", datatype);

	if (check_for_data_type_usage(cluster, typename, output_path))
	{
		pg_log(PG_REPORT, "fatal\n");
		pg_fatal("Your installation contains the \"%s\" data type in user tables.\n"
				 "The \"%s\" type has been removed in PostgreSQL version %s,\n"
				 "so this cluster cannot currently be upgraded.  You can drop the\n"
				 "problem columns, or change them to another data type, and restart\n"
				 "the upgrade.  A list of the problem columns is in the file:\n"
				 "    %s\n\n", datatype, datatype, version, output_path);
	}
	else
		check_ok();
}


/*
 * check_for_jsonb_9_4_usage()
 *
 *	JSONB changed its storage format during 9.4 beta, so check for it.
 */
static void
check_for_jsonb_9_4_usage(ClusterInfo *cluster)
{
	char		output_path[MAXPGPATH];

	prep_status("Checking for incompatible \"jsonb\" data type");

	snprintf(output_path, sizeof(output_path), "%s/%s",
			 log_opts.basedir,
			 "tables_using_jsonb.txt");

	if (check_for_data_type_usage(cluster, "pg_catalog.jsonb", output_path))
	{
		pg_log(PG_REPORT, "fatal\n");
		pg_fatal("Your installation contains the \"jsonb\" data type in user tables.\n"
				 "The internal format of \"jsonb\" changed during 9.4 beta so this\n"
				 "cluster cannot currently be upgraded.  You can\n"
				 "drop the problem columns and restart the upgrade.\n"
				 "A list of the problem columns is in the file:\n"
				 "    %s\n\n", output_path);
	}
	else
		check_ok();
}

/*
 * check_for_pg_role_prefix()
 *
 *	Versions older than 9.6 should not have any pg_* roles
 */
static void
check_for_pg_role_prefix(ClusterInfo *cluster)
{
	PGresult   *res;
	PGconn	   *conn = connectToServer(cluster, "template1");

	prep_status("Checking for roles starting with \"pg_\"");

	res = executeQueryOrDie(conn,
							"SELECT * "
							"FROM pg_catalog.pg_roles "
							"WHERE rolname ~ '^pg_'");

	if (PQntuples(res) != 0)
	{
		if (cluster == &old_cluster)
			pg_fatal("The source cluster contains roles starting with \"pg_\"\n");
		else
			pg_fatal("The target cluster contains roles starting with \"pg_\"\n");
	}

	PQclear(res);

	PQfinish(conn);

	check_ok();
}

/*
 * Verify that no user-defined encoding conversions exist.
 */
static void
check_for_user_defined_encoding_conversions(ClusterInfo *cluster)
{
	int			dbnum;
	FILE	   *script = NULL;
	bool		found = false;
	char		output_path[MAXPGPATH];

	prep_status("Checking for user-defined encoding conversions");

	snprintf(output_path, sizeof(output_path), "%s/%s",
			 log_opts.basedir,
			 "encoding_conversions.txt");

	/* Find any user defined encoding conversions */
	for (dbnum = 0; dbnum < cluster->dbarr.ndbs; dbnum++)
	{
		PGresult   *res;
		bool		db_used = false;
		int			ntups;
		int			rowno;
		int			i_conoid,
					i_conname,
					i_nspname;
		DbInfo	   *active_db = &cluster->dbarr.dbs[dbnum];
		PGconn	   *conn = connectToServer(cluster, active_db->db_name);

		/*
		 * The query below hardcodes FirstNormalObjectId as 16384 rather than
		 * interpolating that C #define into the query because, if that
		 * #define is ever changed, the cutoff we want to use is the value
		 * used by pre-version 14 servers, not that of some future version.
		 */
		res = executeQueryOrDie(conn,
								"SELECT c.oid as conoid, c.conname, n.nspname "
								"FROM pg_catalog.pg_conversion c, "
								"     pg_catalog.pg_namespace n "
								"WHERE c.connamespace = n.oid AND "
								"      c.oid >= 16384");
		ntups = PQntuples(res);
		i_conoid = PQfnumber(res, "conoid");
		i_conname = PQfnumber(res, "conname");
		i_nspname = PQfnumber(res, "nspname");
		for (rowno = 0; rowno < ntups; rowno++)
		{
			found = true;
			if (script == NULL &&
				(script = fopen_priv(output_path, "w")) == NULL)
				pg_fatal("could not open file \"%s\": %s\n",
						 output_path, strerror(errno));
			if (!db_used)
			{
				fprintf(script, "In database: %s\n", active_db->db_name);
				db_used = true;
			}
			fprintf(script, "  (oid=%s) %s.%s\n",
					PQgetvalue(res, rowno, i_conoid),
					PQgetvalue(res, rowno, i_nspname),
					PQgetvalue(res, rowno, i_conname));
		}

		PQclear(res);

		PQfinish(conn);
	}

	if (script)
		fclose(script);

	if (found)
	{
		pg_log(PG_REPORT, "fatal\n");
		pg_fatal("Your installation contains user-defined encoding conversions.\n"
				 "The conversion function parameters changed in PostgreSQL version 14\n"
				 "so this cluster cannot currently be upgraded.  You can remove the\n"
				 "encoding conversions in the old cluster and restart the upgrade.\n"
				 "A list of user-defined encoding conversions is in the file:\n"
				 "    %s\n\n", output_path);
	}
	else
		check_ok();
}


/*
 * get_canonical_locale_name
 *
 * Send the locale name to the system, and hope we get back a canonical
 * version.  This should match the backend's check_locale() function.
 */
static char *
get_canonical_locale_name(int category, const char *locale)
{
	char	   *save;
	char	   *res;

	/* get the current setting, so we can restore it. */
	save = setlocale(category, NULL);
	if (!save)
		pg_fatal("failed to get the current locale\n");

	/* 'save' may be pointing at a modifiable scratch variable, so copy it. */
	save = pg_strdup(save);

	/* set the locale with setlocale, to see if it accepts it. */
	res = setlocale(category, locale);

	if (!res)
		pg_fatal("failed to get system locale name for \"%s\"\n", locale);

	res = pg_strdup(res);

	/* restore old value. */
	if (!setlocale(category, save))
		pg_fatal("failed to restore old locale \"%s\"\n", save);

	pg_free(save);

	return res;
}

/*
 * yb_check_upgrade_compatibility_guc()
 *
 * Make sure the yb_major_version_upgrade_compatibility GUC is set to the
 * correct value.
 */
static void
yb_check_upgrade_compatibility_guc(PGconn *old_cluster_conn)
{
	PGresult   *res;

	prep_status("Checking expression pushdown is disabled");

	res = executeQueryOrDie(old_cluster_conn, "SHOW yb_major_version_upgrade_compatibility");

	if (strncmp(PQgetvalue(res, 0, 0), "11", 2))
		pg_fatal("yb_major_version_upgrade_compatibility must be set to 11\n");

	PQclear(res);

	check_ok();
}

/*
 * yb_check_system_databases_exist()
 *
 *	All the 3 system database should exist before upgrading
 */
static void
yb_check_system_databases_exist(PGconn *old_cluster_conn)
{
	PGresult   *res;

	prep_status("Checking for all 3 system databases");

	res = executeQueryOrDie(old_cluster_conn,
							"VALUES ('template0'), ('template1'), ('yugabyte') EXCEPT SELECT datname FROM pg_database;");

	if (PQntuples(res) != 0)
		pg_fatal("Missing system database '%s'\n", PQgetvalue(res, 0, 0));

	PQclear(res);

	check_ok();
}

/*
 * yb_check_user_attributes()
 *
 *	Make sure the user have the required attributes.
 */
static void
yb_check_user_attributes(PGconn *old_cluster_conn, const char *user_name,
						 const char **role_attrs)
{
	PQExpBufferData buf;
	PGresult   *res;
	const char **role_attr;
	bool		first_attribute = true;

	initPQExpBuffer(&buf);
	for (role_attr = role_attrs; *role_attr != NULL; role_attr++)
	{
		if (!first_attribute)
			appendPQExpBufferStr(&buf, ", ");
		first_attribute = false;
		appendPQExpBufferStr(&buf, *role_attr);
	}

	res = executeQueryOrDie(old_cluster_conn,
							"SELECT %s FROM pg_catalog.pg_roles WHERE rolname = '%s'",
							buf.data, user_name);

	if (PQntuples(res) != 1)
		pg_fatal("The '%s' user is missing\n", user_name);

	for (role_attr = role_attrs; *role_attr != NULL; role_attr++)
	{
		if (strcmp(PQgetvalue(res, 0, PQfnumber(res, *role_attr)), "f") == 0)
			pg_fatal("The '%s' user is missing the '%s' attribute\n", user_name,
					 *role_attr);
	}

	termPQExpBuffer(&buf);
	PQclear(res);
}

/*
 * yb_check_yugabyte_user()
 *
 *	Make sure yugabyte user has the required attributes.
 */
static void
yb_check_yugabyte_user(PGconn *old_cluster_conn)
{
	static const char *role_attrs[] = {
		"rolsuper",
		"rolinherit",
		"rolcreaterole",
		"rolcreatedb",
		"rolcanlogin",
		"rolreplication",
		"rolbypassrls",
		NULL,
	};

	prep_status("Checking attributes of the 'yugabyte' user");
	yb_check_user_attributes(old_cluster_conn, "yugabyte", role_attrs);
	check_ok();
}

/*
 * yb_check_old_cluster_user()
 *
 *	Make sure user used to dump the old cluster has required attributes.
 */
static void
yb_check_old_cluster_user(PGconn *old_cluster_conn)
{
	static const char *role_attributes[] = {"rolsuper", "rolcanlogin", NULL};

	/* yugabyte user is checked in yb_check_yugabyte_user */
	if (strcmp(old_cluster.yb_user, "yugabyte") == 0)
		return;

	prep_status("Checking attributes of the '%s' user", old_cluster.yb_user);
	yb_check_user_attributes(old_cluster_conn, old_cluster.yb_user,
							 role_attributes);
	check_ok();
}

void
yb_check_installed_extensions()
{
	int			dbnum;
	bool		found = false;
	StringInfoData error_msg;
	char		output_path[MAXPGPATH];
	FILE	   *script = NULL;

	initStringInfo(&error_msg);
	snprintf(output_path, sizeof(output_path), "%s/%s",
			 log_opts.basedir,
			 "unsupported_extensions.txt");

	prep_status("Checking installed extensions");

	for (dbnum = 0; dbnum < old_cluster.dbarr.ndbs; dbnum++)
	{
		PGresult   *res;
		int			ntups;
		int			rowno;
		int			i_name;
		bool		db_used = false;
		DbInfo	   *active_db = &old_cluster.dbarr.dbs[dbnum];
		PGconn	   *conn = connectToServer(&old_cluster, active_db->db_name);

		/* Find installed extensions not in the allowed list */
		res = executeQueryOrDie(conn,
								"SELECT extname FROM pg_extension WHERE extname NOT IN "
								"('auto_explain',"
								"'file_fdw',"
								"'fuzzystrmatch',"
								"'hstore',"
								"'passwordcheck',"
								"'pgcrypto',"
								"'pg_stat_statements',"
								"'pg_trgm',"
								"'postgres_fdw',"
								"'autoinc',"
								"'insert_username',"
								"'moddatetime',"
								"'refint',"
								"'sslinfo',"
								"'tablefunc',"
								"'uuid-ossp',"
								"'hypopg',"
								"'orafce',"
								"'pgaudit',"
								"'pg_hint_plan',"
								"'hll',"
								"'pg_cron',"
								"'pg_partman',"
								"'plpgsql',"
								"'anon',"
								"'cube',"
								"'earthdistance')");
		ntups = PQntuples(res);
		i_name = PQfnumber(res, "extname");

		for (rowno = 0; rowno < ntups; rowno++)
		{
			const char *extension_name = PQgetvalue(res, rowno, i_name);

			found = true;
			if (script == NULL && (script = fopen_priv(output_path, "w")) == NULL)
				pg_fatal("could not open file \"%s\": %s\n", output_path,
						 strerror(errno));
			if (!db_used)
			{
				yb_fprintf_and_log(script, "In database: %s\n",
								   active_db->db_name);
				db_used = true;
			}
			yb_fprintf_and_log(script, "  %s\n", extension_name);
		}

		PQclear(res);
		PQfinish(conn);
	}

	if (found)
	{
		yb_fatal("Your installation contains extensions that are not compatible\n"
				 "with YSQL major version upgrade. Please uninstall the extensions\n"
				 "using DROP EXTENSION, and reinstall them after the upgrade. A list of\n"
				 "extensions with problems is printed above and in the file:\n"
				 "    %s\n\n", output_path);
	}
	else
	{
		check_ok();
	}
}
