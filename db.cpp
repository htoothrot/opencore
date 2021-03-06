
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef OSX
#include <mach/clock.h>
#include <mach/mach.h>
#endif

#include "mysql/mysql.h"
#include "mysql/errmsg.h"
#include "bsd/queue.h"

#include "opencore.hpp"

#include "config.hpp"
#include "lib.hpp"
#include "util.hpp"


static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_key_t g_db_tls_key;
static pthread_t g_dbthread = -1;
static pthread_cond_t g_db_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t g_db_cond_mtx = PTHREAD_MUTEX_INITIALIZER;

static char username[128] = { '\0' };
static char password[128] = { '\0' };
static char server[128] = { '\0' };
static int  port = 3306;
static char dbname[128] = { '\0' };
static bool g_running = 0;

typedef struct db_result DB_RESULT;
struct db_result {
	TAILQ_ENTRY(db_result) entry;

	unsigned int nrows;
	unsigned int ncols;
	char ***rs;
	bool success;
	uintptr_t user_data;
	char name[24];
	int type;
	char *query;
	char libname[64];
};
TAILQ_HEAD(result_head, db_result);

typedef struct db_context DB_CONTEXT;
struct db_context {
	bool running;
	size_t npending;
	result_head result_list_head;

	pthread_mutex_t mtx;
};

typedef struct db_query DB_QUERY;
struct db_query {
	TAILQ_ENTRY(db_query) entry;

	char *query;
	uintptr_t user_data;
	DB_CONTEXT *dbc;
	char name[24];
	char libname[64];
	int type;
};
TAILQ_HEAD(query_head, db_query);
static query_head query_list_head = TAILQ_HEAD_INITIALIZER(query_list_head);
static pthread_mutex_t query_list_mtx = PTHREAD_MUTEX_INITIALIZER;



DB_CONTEXT*
dbc_alloc()
{
	DB_CONTEXT *ctx = (DB_CONTEXT*)xmalloc(sizeof(DB_CONTEXT));
	if (!ctx) return NULL;
	memset(ctx, 0, sizeof(DB_CONTEXT));

	TAILQ_INIT(&ctx->result_list_head);
	pthread_mutex_init(&ctx->mtx, NULL);
	ctx->running = true;
	ctx->npending = 0;

	return ctx;
}


void
dbc_free(DB_CONTEXT *ctx)
{
	if (!ctx) return;

	pthread_mutex_destroy(&ctx->mtx);

	free(ctx);
}


static
DB_CONTEXT*
dbc_get()
{
	return (DB_CONTEXT*)pthread_getspecific(g_db_tls_key);
}


static
char***
resultset_alloc(unsigned int nrows, unsigned int ncols)
{
	if (nrows < 0 || ncols < 0) return NULL;

	char ***rs = (char***)calloc(nrows, sizeof(char**));
	if (!rs) return NULL;

	for (unsigned int row = 0; row < nrows; ++row) {
		rs[row] = (char**)calloc(ncols, sizeof(char*));
		if (!rs[row]) {
			// allocation failed
			while (row > 0) {
				free(rs[--row]);
			}
			free(rs);
			return NULL;
		}
	}

	return rs;
}


static
void
resultset_free(char ***rs, int nrows, int ncols)
{
	if (!rs) return;

	for (int row = 0; row < nrows; ++row) {
		for (int col = 0; col < ncols; ++col) {
			free(rs[row][col]);	
		}
		free(rs[row]);
	}
	free(rs);
}


void*
db_entrypoint(void *unused)
{
	MYSQL lmysql;
	mysql_init(&lmysql);

	// set connection options
	const bool opt_reconnect = true;
	mysql_options(&lmysql, MYSQL_OPT_RECONNECT, &opt_reconnect);

	const ticks_ms_t reconnect_interval = 60 * 1000;
	ticks_ms_t last_connect_ticks = get_ticks_ms() - reconnect_interval;
	MYSQL *mysql = NULL;
	
	bool running = true;
	while (running) {
		// wait for an event
		struct timespec abstimeout;
#ifdef OSX
		clock_serv_t cclock;
		mach_timespec_t mts;
		host_get_clock_service(mach_host_self(), CALENDAR_CLOCK, &cclock);
		clock_get_time(cclock, &mts);
		mach_port_deallocate(mach_task_self(), cclock);
		abstimeout->tv_sec = mts.tv_sec;
		abstimeout->tv_nsec = mts.tv_nsec;
#else
		clock_gettime(CLOCK_REALTIME, &abstimeout);
#endif
		abstimeout.tv_sec += 30;
		bool keepalive = false;
		pthread_mutex_lock(&g_db_cond_mtx);
		int rv = pthread_cond_timedwait(&g_db_cond, &g_db_cond_mtx, &abstimeout);
		pthread_mutex_unlock(&g_db_cond_mtx);
		if (rv == ETIMEDOUT) {
			// if the wait timed out, just submit a keep alive to the db
			keepalive = true;
		}

		// acquire the mysql connection if its not connected
		if (get_ticks_ms() - last_connect_ticks >= reconnect_interval) {
			MYSQL *m = mysql_real_connect(&lmysql, server, username, password, dbname, port, 0, CLIENT_COMPRESS | CLIENT_MULTI_RESULTS | CLIENT_INTERACTIVE);
			unsigned int last_error = mysql_errno(&lmysql);	

			if (m) {
				mysql = m;
				Log(OP_SMOD, "Connected to database");
			} else if (last_error == CR_ALREADY_CONNECTED) {
				// db was already connected
				mysql = &lmysql;
			} else {
				mysql = NULL;
				Log(OP_SMOD, "Database reconnection failure");
			}
			
			last_connect_ticks = get_ticks_ms();
		}

		/* no db connection */
		if (!mysql) continue;

		/* only submit a keepalive */
		if (keepalive) {
			// keep alive
			keepalive = true;
			const char *keepalive_query = "SELECT 1;";
			if (mysql_real_query(mysql, keepalive_query, strlen(keepalive_query)) == 0) {
				do {
					MYSQL_RES *result = mysql_store_result(mysql);
					if (result) {
						while (mysql_fetch_row(result))
							;
						mysql_free_result(result);
					}
				} while (mysql_next_result(mysql) == 0);
			}
			continue;
		}

		pthread_mutex_lock(&g_mtx);
		if (g_running && mysql) {
			pthread_mutex_unlock(&g_mtx);

			while (1) {
				// pull the first entry from the queue
				pthread_mutex_lock(&query_list_mtx);
				DB_QUERY *dbq = TAILQ_FIRST(&query_list_head);
				if (dbq) TAILQ_REMOVE(&query_list_head, dbq, entry);
				pthread_mutex_unlock(&query_list_mtx);
				if (!dbq) break;

				unsigned int nrows = 0;
				unsigned int ncols = 0;

				// execute the query
				DB_RESULT *dbr = (DB_RESULT*)malloc(sizeof(DB_RESULT));
				if (dbr) {
					memset(dbr, 0, sizeof(DB_RESULT));

					strlcpy(dbr->libname, dbq->libname, sizeof(dbr->libname));
					dbr->type = dbq->type;
					strncpy(dbr->name, dbq->name, sizeof(dbr->name));
					dbr->user_data = dbq->user_data;
					
					// only store the first result from call statements, change this later
					bool stored_result = false;
					if (mysql_real_query(mysql, dbq->query, strlen(dbq->query)) == 0) {
						do {
							MYSQL_RES *result = mysql_store_result(mysql);
							if (result) {
								// result set returned
								nrows = mysql_num_rows(result);
								ncols = mysql_num_fields(result);

								char ***rs = resultset_alloc(nrows, ncols);
								if (rs) {
									MYSQL_ROW row;
									unsigned int current_row = 0;
									while ((row = mysql_fetch_row(result))) {
										for (unsigned int i = 0; i < ncols; ++i) {
											rs[current_row][i] = row[i] ? strdup(row[i]) : NULL;
										}
										++current_row;
									}
								} else {
									Log(OP_SMOD, "Error allocating DB resultset");
								}

								mysql_free_result(result);

								if (!stored_result) {
									dbr->nrows = nrows;
									dbr->ncols = ncols;
									dbr->rs = rs;
									dbr->success = true;

									stored_result = true;
								}
							} else {
								// check for error on query with no results
								if (mysql_errno(mysql) == 0) {
									dbr->success = true;
									dbr->nrows = 0;
									dbr->ncols = 0;
								} else {
									// error
								}
							}
						} while (mysql_next_result(mysql) == 0);
					}

					// pass the query pointer over
					dbr->query = dbq->query;
					dbq->query = NULL;

					// pass the db results back to the appropriate context or free it
					DB_CONTEXT *dbc = dbq->dbc;
					pthread_mutex_lock(&dbc->mtx);
					if (dbc->running) {
						TAILQ_INSERT_TAIL(&dbc->result_list_head, dbr, entry);
						pthread_mutex_unlock(&dbc->mtx);
					} else {
						if (dbc->npending == 0) {
							pthread_mutex_unlock(&dbc->mtx);
							// db isnt running and no more queries are pending or will be created
							if (dbr->rs) resultset_free(dbr->rs, nrows, ncols);
							if (dbr->query) free(dbr->query);
							free(dbr);
						} else {
							pthread_mutex_unlock(&dbc->mtx);
						}
						pthread_mutex_destroy(&dbc->mtx);
						dbc_free(dbc);
					}
				} else {
					free(dbq->query);
				}

				free(dbq);
			}
			pthread_mutex_lock(&g_mtx);
		}
		if (!g_running) running = false;
		pthread_mutex_unlock(&g_mtx);
	}

	if (mysql) mysql_close(mysql);

	return NULL;
}


/*
 * Set the thread's database context data.
 */
void
db_instance_init()
{
	DB_CONTEXT *dbc = dbc_alloc();
	if (!dbc) {
		Log(OP_SMOD, "Unabled to allocate DB context");
		exit(-1);
	}
	pthread_setspecific(g_db_tls_key, dbc);
}


/*
 * This shuts down a bot's instance but does not free it, since pending queries
 * will still need the context data when they are executed.
 */
void
db_instance_shutdown()
{
	DB_CONTEXT *dbc = dbc_get();
	pthread_mutex_lock(&dbc->mtx);
	dbc->running = false;
	//TODO: This needs to clear out the dbc list and export events for queries still in queue
	pthread_mutex_unlock(&dbc->mtx);
}


void
db_instance_export_events(ticks_ms_t max_time)
{
	DB_CONTEXT *dbc = dbc_get();
	ticks_ms_t base = get_ticks_ms();
	DB_RESULT *dbr = NULL;
	do {
		pthread_mutex_lock(&dbc->mtx);
		dbr = TAILQ_FIRST(&dbc->result_list_head);
		if (dbr) TAILQ_REMOVE(&dbc->result_list_head, dbr, entry);
		dbc->npending -= 1;
		pthread_mutex_unlock(&dbc->mtx);

		if (dbr) {
			THREAD_DATA *td = get_thread_data();

			CORE_DATA *cd = libman_get_core_data(td);

			cd->query_success = dbr->success;
			cd->query_resultset = dbr->rs;
			cd->query_user_data = dbr->user_data;
			cd->query_nrows = dbr->nrows;
			cd->query_ncols = dbr->ncols;
			strncpy(cd->query_name, dbr->name, sizeof(cd->query_name));
			cd->query_type = dbr->type;

			LIB_ENTRY *le = libman_find_lib(dbr->libname);
			if (le) {
				libman_export_event(td, EVENT_QUERY_RESULT, cd, le);
			}

			if (dbr->rs) resultset_free(dbr->rs, dbr->nrows, dbr->ncols);
			free(dbr);
		}

	} while (dbr && get_ticks_ms() - base <= max_time);
}


static
void
db_init_once()
{
	if (pthread_key_create(&g_db_tls_key, NULL) != 0) {
		Log(OP_SMOD, "Error creating DB thread-specific storage");
		exit(-1);
	}
}


/*
 * Init the core-wide database engine.
 */
void
db_init(const char *configfile)
{
	static pthread_once_t init_once = PTHREAD_ONCE_INIT;
	pthread_once(&init_once, db_init_once);

	pthread_mutex_lock(&g_mtx);

	// read the config
	g_running = config_get_int("database.enabled", 0, configfile) != 0;
	config_get_string("database.username", username, sizeof(username), "user", configfile);
	config_get_string("database.password", password, sizeof(password), "password", configfile);
	config_get_string("database.server", server, sizeof(server), "localhost", configfile);
	config_get_string("database.dbname", dbname, sizeof(dbname), "db", configfile);
	port = config_get_int("database.port", 3306, configfile);

	// spawn the db thread
	if (g_running) {
		if (pthread_create(&g_dbthread, NULL, db_entrypoint, NULL) != 0) {
			Log(OP_SMOD, "Unable to create DB thread");
			exit(-1);
		}
		Log(OP_MOD, "Database thread created");
	} else {
		Log(OP_SMOD, "Database configured to be disabled");
	}

	pthread_mutex_unlock(&g_mtx);
}


void
db_shutdown()
{
	pthread_mutex_lock(&g_mtx);
	bool running = g_running;
	pthread_mutex_unlock(&g_mtx);

	if (running) {
		Log(OP_SMOD, "Waiting on DB thread to exit");	

		pthread_mutex_lock(&g_mtx);
		g_running = false;
		pthread_mutex_unlock(&g_mtx);
		
		pthread_cond_signal(&g_db_cond);
		void *retval = NULL;
		if (pthread_join(g_dbthread, &retval) == 0) {
			pthread_mutex_destroy(&g_mtx);
			Log(OP_SMOD, "DB thread exited");
		} else {
			Log(OP_SMOD, "Unable to join with DB thread");	
		}
	}
}


int
QueryFmt(int query_type, uintptr_t user_data, const char *name, const char *fmt, ...)
{
	va_list ap;
	va_list ap2;
	va_start(ap, fmt);
	va_copy(ap2, ap);

	int rv = -1;

	int size = vsnprintf(NULL, 0, fmt, ap);
	if (size >= 0) {
		char *query = (char*)malloc(size + 1);
		if (query) {
			if (vsnprintf(query, size + 1, fmt, ap2) < size + 1) {
				rv = Query(query_type, user_data, name, query);
			}
			
			free(query);
		}
	}

	va_end(ap);
	va_end(ap2);
	
	return rv;
}


void
QueryEscape(char *result, size_t result_sz, const char *str)
{
	if (result_sz < strlen(str)*2 + 1) {
		if (result_sz > 0) *result = '\0';
		Log(OP_SMOD, "Invalid length passed to QueryEscape()");
		return;
	}

	while (*str) {
		switch (*str) {
		case '\\':
		case '\'':
		case '"':
		case '\0':
		case '\n':
		case '\r':
		case 0x1A:
		*result++ = '\\';
		/* FALLTRHOUGH */
		default:
			*result++ = *str++;
			break;
		};
	}
	*result = '\0';
}


int
Query(int query_type, uintptr_t user_data, const char *name, const char *query)
{
	int rv = -1;

	pthread_mutex_lock(&g_mtx);
	if (g_running) {
		DB_QUERY *dbq = (DB_QUERY*)malloc(sizeof(DB_QUERY));
		if (dbq) {
			memset(dbq, 0, sizeof(DB_QUERY));
			libman_get_current_libname(dbq->libname, sizeof(dbq->libname));
			dbq->query = strdup(query);
			dbq->user_data = user_data;
			strncpy(dbq->name, name ? name : "", sizeof(dbq->name));
			dbq->type = query_type;
			if (dbq->query) {
				dbq->dbc = dbc_get();

				if (dbq->dbc->running) {

					pthread_mutex_lock(&dbq->dbc->mtx);
					dbq->dbc->npending += 1;
					pthread_mutex_unlock(&dbq->dbc->mtx);

					pthread_mutex_lock(&query_list_mtx);
					TAILQ_INSERT_TAIL(&query_list_head, dbq, entry);
					pthread_mutex_unlock(&query_list_mtx);

					pthread_cond_signal(&g_db_cond);

					rv = 0;
				} else {
					free(dbq);
				}
			} else {
				free(dbq);
			}
		}
	}
	pthread_mutex_unlock(&g_mtx);

	return rv;
}

