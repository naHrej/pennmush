/**
 * \file game.c
 *
 * \brief The main game driver.
 *
 */

#include "copyrite.h"
#include "game.h"

#include <ctype.h>
#include <fcntl.h>
#include <string.h>
#include <signal.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#ifdef TIME_WITH_SYS_TIME
#include <time.h>
#endif
#else
#include <time.h>
#endif
#ifdef WIN32
#include <process.h>
#include <windows.h>
#undef OPAQUE /* Clashes with flags.h */
void Win32MUSH_setup(void);
#endif
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_SYS_RESOURCE_H
#include <sys/resource.h>
#endif
#include <stdlib.h>
#include <stdarg.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <errno.h>

#include "access.h"
#include "ansi.h"
#include "attrib.h"
#include "case.h"
#include "command.h"
#include "conf.h"
#include "dbdefs.h"
#include "extchat.h"
#include "externs.h"
#include "extmail.h"
#include "flags.h"
#include "function.h"
#include "help.h"
#include "htab.h"
#include "intmap.h"
#include "lock.h"
#include "log.h"
#include "match.h"
#include "mushdb.h"
#include "mymalloc.h"
#include "mypcre.h"
#include "parse.h"
#include "ptab.h"
#include "sig.h"
#include "strtree.h"
#include "strutil.h"
#include "version.h"
#include "mushsql.h"

#ifdef HAVE_SSL
#include "myssl.h"
#endif

#ifndef WIN32
#include "wait.h"
#endif

#ifdef hpux
#include <sys/syscall.h>
#define getrusage(x, p) syscall(SYS_GETRUSAGE, x, p)
#endif /* fix to HP-UX getrusage() braindamage */

/* declarations */
GLOBALTAB globals = {0, "", 0, 0, 0, 0, 0, 0, 0, 0};

static int epoch = 0;
#ifndef WIN32
static int reserved = -1; /**< Reserved file descriptor */
#endif
static dbref *errdblist = NULL; /**< List of dbrefs to return errors from */
static dbref *errdbtail = NULL; /**< Pointer to end of errdblist */
#define ERRDB_INITIAL_SIZE 5
static int errdbsize =
  ERRDB_INITIAL_SIZE; /**< Current size of errdblist array */
static void errdb_grow(void);

extern const unsigned char *tables;
extern void conf_default_set(void);
static bool dump_database_internal(void);
static PENNFILE *db_open(const char *);
static PENNFILE *db_open_write(const char *);
static int fail_commands(dbref player);
void do_readcache(dbref player);
int check_alias(const char *command, const char *list);
static int list_check(dbref thing, dbref player, char type, char end, char *str,
                      int just_match, MQUE *queue_entry, int queue_flags);
int alias_list_check(dbref thing, const char *command, const char *type);
int loc_alias_check(dbref loc, const char *command, const char *type);
void do_poor(dbref player, char *arg1);
void do_writelog(dbref player, char *str, int ltype);
void bind_and_queue(dbref executor, dbref enactor, char *action,
                    const char *arg, int num, MQUE *queue_entry,
                    int queue_type);
void do_uptime(dbref player, int mortal);
static char *make_new_epoch_file(const char *basename, int the_epoch);
#ifdef HAVE_GETRUSAGE
void rusage_stats(void);
#endif

void st_stats_header(dbref player);
void st_stats(dbref player, StrTree *root, const char *name);
void do_timestring(char *buff, char **bp, const char *format,
                   unsigned long secs);

extern void create_minimal_db(void); /* From db.c */

dbref orator =
  NOTHING; /**< Last dbref to issue a speech command. DEPRECATED. DO NOT USE. */

#ifdef COMP_STATS
extern void compress_stats(long *entries, long *mem_used,
                           long *total_uncompressed, long *total_compressed);
#endif

/* Set in do_entry(), used in report() */
char report_cmd[BUFFER_LEN];
dbref report_dbref = NOTHING;

pid_t forked_dump_pid = -1;

/** Open /dev/null to reserve a file descriptor that can be reused later. */
void
reserve_fd(void)
{
#ifndef WIN32
  reserved = open("/dev/null", O_RDWR | O_CLOEXEC);
#endif
}

/** Release the reserved file descriptor for other use. */
void
release_fd(void)
{
#ifndef WIN32
  if (reserved >= 0) {
    close(reserved);
    reserved = -1;
  }
#endif
}

/** User command to dump the database.
 * \verbatim
 * This implements the @dump command.
 * \endverbatim
 * \param player the enactor, for permission checking.
 * \param num checkpoint interval, as a string.
 * \param flag type of dump.
 */
void
do_dump(dbref player, char *num, enum dump_type flag)
{
  if (Wizard(player)) {
#ifdef ALWAYS_PARANOID
    if (1) {
#else
    if (flag != DUMP_NORMAL && flag != DUMP_NOFORK) {
#endif
      /* want to do a scan before dumping each object */
      globals.paranoid_dump = (flag == DUMP_DEBUG ? 2 : 1);
      if (num && *num) {
        /* checkpoint interval given */
        globals.paranoid_checkpt = atoi(num);
        if ((globals.paranoid_checkpt < 1) ||
            (globals.paranoid_checkpt >= db_top)) {
          notify(player, T("Permission denied. Invalid checkpoint interval."));
          globals.paranoid_dump = 0;
          return;
        }
      } else {
        /* use a default interval */
        globals.paranoid_checkpt = db_top / 5;
        if (globals.paranoid_checkpt < 1)
          globals.paranoid_checkpt = 1;
      }
      if (flag == DUMP_PARANOID) {
        notify_format(player, T("Paranoid dumping, checkpoint interval %d."),
                      globals.paranoid_checkpt);
        do_rawlog(LT_CHECK, "*** PARANOID DUMP *** done by %s(#%d),\n",
                  Name(player), player);
      } else {
        notify_format(player, T("Debug dumping, checkpoint interval %d."),
                      globals.paranoid_checkpt);
        do_rawlog(LT_CHECK, "*** DEBUG DUMP *** done by %s(#%d),\n",
                  Name(player), player);
      }
      do_rawlog(LT_CHECK, "\tcheckpoint interval %d, at %s",
                globals.paranoid_checkpt, show_time(mudtime, 0));
    } else {
      /* normal dump */
      globals.paranoid_dump = 0; /* just to be safe */
      notify(player, T("Dumping..."));
      do_rawlog(LT_CHECK, "** DUMP ** done by %s(#%d) at %s", Name(player),
                player, show_time(mudtime, 0));
    }
    fork_and_dump(flag == DUMP_NORMAL);
    globals.paranoid_dump = 0;
    notify(player, T("Dump complete."));
  } else {
    notify(player, T("Sorry, you are in a no dumping zone."));
  }
}

/** Print global variables to the trace log.
 * This function is used for error reporting.
 */
void
report(void)
{
  if (GoodObject(report_dbref))
    do_rawlog(LT_TRACE, "TRACE: Cmd:%s\tby #%d at #%d", (char *) report_cmd,
              report_dbref, Location(report_dbref));
  else
    do_rawlog(LT_TRACE, "TRACE: Cmd:%s\tby #%d", report_cmd, report_dbref);
  notify_activity(NOTHING, 0, 1);
}

#ifdef HAVE_GETRUSAGE
/** Log process statistics to the error log.
 */
void
rusage_stats(void)
{
  struct rusage usage;
  int psize;

  psize = mush_getpagesize();
  getrusage(RUSAGE_SELF, &usage);

  do_rawlog(LT_ERR, "Process statistics:");
  do_rawlog(LT_ERR, "Time used:   %10ld user   %10ld sys",
            (long) usage.ru_utime.tv_sec, (long) usage.ru_stime.tv_sec);
  do_rawlog(LT_ERR, "Max res mem: %10ld pages  %10ld bytes", usage.ru_maxrss,
            (usage.ru_maxrss * psize));
  do_rawlog(LT_ERR, "Integral mem:%10ld shared %10ld private %10ld stack",
            usage.ru_ixrss, usage.ru_idrss, usage.ru_isrss);
  do_rawlog(LT_ERR, "Page faults: %10ld hard   %10ld soft    %10ld swapouts",
            usage.ru_majflt, usage.ru_minflt, usage.ru_nswap);
  do_rawlog(LT_ERR, "Disk I/O:    %10ld reads  %10ld writes", usage.ru_inblock,
            usage.ru_oublock);
  do_rawlog(LT_ERR, "Network I/O: %10ld in     %10ld out", usage.ru_msgrcv,
            usage.ru_msgsnd);
  do_rawlog(LT_ERR, "Context swi: %10ld vol    %10ld forced", usage.ru_nvcsw,
            usage.ru_nivcsw);
  do_rawlog(LT_ERR, "Signals:     %10ld", usage.ru_nsignals);
}

#endif /* HAVE_GETRUSAGE */

/** User interface to shut down the MUSH.
 * \verbatim
 * This implements the @shutdown command.
 * \endverbatim
 * \param player the enactor, for permission checking.
 * \param flag type of shutdown to perform.
 */
void
do_shutdown(dbref player, enum shutdown_type flag)
{
  if (flag == SHUT_PANIC && !God(player)) {
    notify(player, T("It takes a God to make me panic."));
    return;
  }
  flag_broadcast(0, 0, T("GAME: Shutdown by %s"),
                 AName(player, AN_ANNOUNCE, NULL));
  do_log(LT_ERR, player, NOTHING, "SHUTDOWN by %s(%s)\n", Name(player),
         unparse_dbref(player));

  if (flag == SHUT_PANIC) {
    mush_panic("@shutdown/panic");
  } else {
    if (flag == SHUT_PARANOID) {
      globals.paranoid_checkpt = db_top / 5;
      if (globals.paranoid_checkpt < 1)
        globals.paranoid_checkpt = 1;
      globals.paranoid_dump = 1;
    }
    shutdown_flag = 1;
  }
}

jmp_buf db_err;

static bool
dump_database_internal(void)
{
  PENNFILE *volatile f = NULL;

#ifndef PROFILING
#ifndef WIN32
#ifdef __CYGWIN__
  ignore_signal(SIGALRM);
#else
  ignore_signal(SIGPROF);
#endif /* __CYGWIN__ */
#endif /* WIN32 */
#endif /* PROFILING */

  if (setjmp(db_err)) {
    /* The dump failed. Disk might be full or something went bad with the
       compression slave. Boo! */
    const char *errmsg = NULL;

    if (f) {
      switch (f->type) {
      case PFT_FILE:
      case PFT_PIPE:
        errmsg = strerror(errno);
        break;
      case PFT_GZFILE:
#ifdef HAVE_LIBZ
      {
        int errnum = 0;
        errmsg = gzerror(f->handle.g, &errnum);
        if (errnum == Z_ERRNO)
          errmsg = strerror(errno);
      }
#endif
      break;
      }
    } else {
      errmsg = strerror(errno);
    }

    do_rawlog(LT_ERR, "ERROR! Database save failed: %s", errmsg);
    queue_event(SYSEVENT, "DUMP`ERROR", "%s,%d,PERROR %s",
                T("GAME: ERROR! Database save failed!"), 0, errmsg);
    flag_broadcast("WIZARD ROYALTY", 0,
                   T("GAME: ERROR! Database save failed!"));
    if (f) {
      penn_fclose(f);
    }
#ifndef PROFILING
#ifdef HAVE_SETITIMER
#ifdef __CYGWIN__
    install_sig_handler(SIGALRM, signal_cpu_limit);
#else
    install_sig_handler(SIGPROF, signal_cpu_limit);
#endif /* __CYGWIN__ */
#endif /* HAVE_SETITIMER */
#endif /* PROFILING */
    return false;
  } else {
    char realdumpfile[2048];
    char realtmpfl[2304];
    char tmpfl[2048];

    local_dump_database();

#ifdef ALWAYS_PARANOID
    globals.paranoid_checkpt = db_top / 5;
    if (globals.paranoid_checkpt < 1) {
      globals.paranoid_checkpt = 1;
    }
#endif

    snprintf(realdumpfile, sizeof realdumpfile, "%s%s", globals.dumpfile,
             options.compresssuff);
    mush_strncpy(tmpfl, make_new_epoch_file(globals.dumpfile, epoch),
                 sizeof tmpfl);
    snprintf(realtmpfl, sizeof realtmpfl, "%s%s", tmpfl, options.compresssuff);

    if ((f = db_open_write(tmpfl)) != NULL) {
      switch (globals.paranoid_dump) {
      case 0:
#ifdef ALWAYS_PARANOID
        db_paranoid_write(f, 0);
#else
        db_write(f, 0);
#endif
        break;
      case 1:
        db_paranoid_write(f, 0);
        break;
      case 2:
        db_paranoid_write(f, 1);
        break;
      }
      penn_fclose(f);
      if (rename_file(realtmpfl, realdumpfile) < 0) {
        penn_perror(realtmpfl);
        longjmp(db_err, 1);
      }
    } else {
      penn_perror(realtmpfl);
      longjmp(db_err, 1);
    }
    snprintf(realdumpfile, sizeof realdumpfile, "%s%s", options.mail_db,
             options.compresssuff);
    strcpy(tmpfl, make_new_epoch_file(options.mail_db, epoch));
    snprintf(realtmpfl, sizeof realtmpfl, "%s%s", tmpfl, options.compresssuff);
    if (mdb_top >= 0) {
      if ((f = db_open_write(tmpfl)) != NULL) {
        dump_mail(f);
        penn_fclose(f);
        if (rename_file(realtmpfl, realdumpfile) < 0) {
          penn_perror(realtmpfl);
          longjmp(db_err, 1);
        }
      } else {
        penn_perror(realtmpfl);
        longjmp(db_err, 1);
      }
    }
    snprintf(realdumpfile, sizeof realdumpfile, "%s%s", options.chatdb,
             options.compresssuff);
    strcpy(tmpfl, make_new_epoch_file(options.chatdb, epoch));
    snprintf(realtmpfl, sizeof realtmpfl, "%s%s", tmpfl, options.compresssuff);
    if ((f = db_open_write(tmpfl)) != NULL) {
      save_chatdb(f);
      penn_fclose(f);
      if (rename_file(realtmpfl, realdumpfile) < 0) {
        penn_perror(realtmpfl);
        longjmp(db_err, 1);
      }
    } else {
      penn_perror(realtmpfl);
      longjmp(db_err, 1);
    }
    time(&globals.last_dump_time);
  }

#ifndef PROFILING
#ifdef HAVE_SETITIMER
#ifdef __CYGWIN__
  install_sig_handler(SIGALRM, signal_cpu_limit);
#else
  install_sig_handler(SIGPROF, signal_cpu_limit);
#endif
#endif
#endif

  return true;
}

/** Crash gracefully.
 * This function is called when something disastrous happens - typically
 * a failure to malloc memory or a signal like segfault.
 * It logs the fault, does its best to dump a panic database, and
 * exits abruptly. This function does not return.
 * \param message message to log to the error log.
 */
void
mush_panic(const char *message)
{
  const char *panicfile = options.crash_db;
  PENNFILE *f = NULL;
  static int already_panicking = 0;

  if (already_panicking) {
    do_rawlog_lvl(LT_ERR, MLOG_CRIT,
                  "PANIC: Attempted to panic because of '%s' while already "
                  "panicking. Run in circles, scream and shout!",
                  message);
    abort();
  }

  already_panicking = 1;
  do_rawlog_lvl(LT_ERR, MLOG_EMERG, "PANIC: %s", message);
  report();
  flag_broadcast(0, 0, T("EMERGENCY SHUTDOWN: %s"), message);

  /* turn off signals */
  block_signals();

  /* shut down interface */
  emergency_shutdown();

  /* dump panic file if we have a database read. */
  if (globals.database_loaded) {
    if (setjmp(db_err)) {
      /* Dump failed. We're in deep doo-doo */
      do_rawlog_lvl(LT_ERR, MLOG_EMERG, "CANNOT DUMP PANIC DB. OOPS.");
      abort();
    } else {
      if ((f = penn_fopen(panicfile, FOPEN_WRITE)) == NULL) {
        do_rawlog_lvl(LT_ERR, MLOG_EMERG, "CANNOT OPEN PANIC FILE, YOU LOSE");
        _exit(135);
      } else {
        do_rawlog(LT_ERR, "DUMPING: %s", panicfile);
        db_write(f, DBF_PANIC);
        dump_mail(f);
        save_chatdb(f);
        penn_fclose(f);
        do_rawlog(LT_ERR, "DUMPING: %s (done)", panicfile);
      }
    }
  } else {
    do_rawlog_lvl(LT_ERR, MLOG_CRIT,
                  "Skipping panic dump because database isn't loaded.");
  }
  abort();
}

/** Crash gracefully.
 * Calls mush_panic() with its arguments formatted.
 * \param msg printf()-style format string.
 */
void
mush_panicf(const char *msg, ...)
{
  char c[BUFFER_LEN];
  va_list args;

  va_start(args, msg);

  mush_vsnprintf(c, sizeof c, msg, args);
  va_end(args);

  mush_panic(c);
  _exit(136); /* Not reached but kills warnings */
}

/** Dump the database.
 * This function is a wrapper for dump_database_internal() that does
 * a little logging before and after the dump.
 */
void
dump_database(void)
{
  epoch++;

  do_rawlog_lvl(LT_ERR, MLOG_INFO, "DUMPING: %s.#%d#", globals.dumpfile, epoch);
  if (dump_database_internal()) {
    do_rawlog_lvl(LT_ERR, MLOG_INFO, "DUMPING: %s.#%d# (done)",
                  globals.dumpfile, epoch);
  }
}

/** Dump a database, possibly by forking the process.
 * This function calls dump_database_internal() to dump the MUSH
 * databases. If we're configured to do so, it forks first, so that
 * the child process can perform the dump while the parent continues
 * to run the MUSH for the players. If we can't fork, this function
 * warns players online that a dump is taking place and the game
 * may pause.
 * \param forking if 1, attempt a forking dump.
 */
bool
fork_and_dump(int forking)
{
  pid_t child;
  bool nofork, status = true;
#ifndef WIN32
  bool split = false;
#endif

  epoch++;

#ifdef LOG_CHUNK_STATS
  chunk_stats(NOTHING, 0);
  chunk_stats(NOTHING, 1);
#endif
  do_rawlog_lvl(LT_CHECK, MLOG_INFO, "CHECKPOINTING: %s.#%d#", globals.dumpfile,
                epoch);
  if (NO_FORK)
    nofork = 1;
  else
    nofork =
      !forking || (globals.paranoid_dump == 2); /* Don't fork for dump/debug */
#if defined(WIN32) || !defined(HAVE_FORK)
  nofork = 1;
#endif

  if (!nofork && chunk_num_swapped()) {
#ifndef WIN32
    /* Try to clone the chunk swapfile. */
    if (chunk_fork_file()) {
      split = 1;
    } else {
      /* Ack, can't fork, 'cause we have stuff on disk... */
      do_rawlog_lvl(LT_ERR, MLOG_INFO,
                    "fork_and_dump: Data are swapped to disk, so "
                    "nonforking dumps will be used.");
      flag_broadcast(
        "WIZARD", 0,
        T("DUMP: Data are swapped to disk, so nonforking dumps will be used."));
      nofork = 1;
    }
#endif
  }
  if (!nofork) {
#ifndef WIN32
#ifdef HAVE_FORK
    child = fork();
#else
    /* Never actually reached, since nofork is set to 1 if HAVE_FORK is
     *  not defined. */
    child = -1;
#endif
    if (child < 0) {
      /* Oops, fork failed. Let's do a nofork dump */
      do_rawlog_lvl(LT_ERR, MLOG_WARNING,
                    "fork_and_dump: fork() failed! Dumping nofork instead.");
      if (DUMP_NOFORK_MESSAGE && *DUMP_NOFORK_MESSAGE)
        flag_broadcast(0, 0, "%s", DUMP_NOFORK_MESSAGE);
      child = 0;
      nofork = 1;
      if (split) {
        split = 0;
        chunk_fork_done();
      }
    } else if (child > 0) {
      forked_dump_pid = child;
      lower_priority_by(child, 8);
      chunk_fork_parent();
    } else {
      chunk_fork_child();
    }
#endif /* WIN32 */
  } else {
    if (DUMP_NOFORK_MESSAGE && *DUMP_NOFORK_MESSAGE)
      flag_broadcast(0, 0, "%s", DUMP_NOFORK_MESSAGE);
    child = 0;
  }
  if (nofork || (!nofork && child == 0)) {
    /* in the child */
    release_fd();
    status = dump_database_internal();
#ifndef WIN32
    if (split)
      chunk_fork_done();
#endif
    if (!nofork) {
      _exit(status ? 0 : 1); /* d_d_i() returns true on success but exit code
                                should be 0 on success */
    } else {
      reserve_fd();
      if (status) {
        queue_event(SYSEVENT, "DUMP`COMPLETE", "%s,%d", DUMP_NOFORK_COMPLETE,
                    0);
        if (status && DUMP_NOFORK_COMPLETE && *DUMP_NOFORK_COMPLETE) {
          flag_broadcast(0, 0, "%s", DUMP_NOFORK_COMPLETE);
        }
      }
    }
  }
#ifdef LOG_CHUNK_STATS
  chunk_stats(NOTHING, 5);
#endif
  return status;
}

/** Start up the MUSH.
 * This function does all of the work that's necessary to start up
 * MUSH objects and code for the game. It sets up player aliases,
 * fixes null object names, and triggers all object startups.
 */
void
do_restart(void)
{
  dbref thing;
  ATTR *s;
  char buf[BUFFER_LEN];
  char *bp;
  sqlite3 *sqldb = get_shared_db();

  /* Do stuff that needs to be done for players only: add stuff to the
   * alias table, and refund money from queued commands at shutdown.
   */

  sqlite3_exec(sqldb, "BEGIN TRANSACTION", NULL, NULL, NULL);
  for (thing = 0; thing < db_top; thing++) {
    if (IsPlayer(thing)) {
      if ((s = atr_get_noparent(thing, "ALIAS")) != NULL) {
        bp = buf;
        safe_str(atr_value(s), buf, &bp);
        *bp = '\0';
        add_player_alias(thing, buf, 1);
      }
    }
  }
  sqlite3_exec(sqldb, "COMMIT TRANSACTION", NULL, NULL, NULL);

  /* Once we load all that, then we can trigger the startups and
   * begin queueing commands. Also, let's make sure that we get
   * rid of null names.
   */

  for (thing = 0; thing < db_top; thing++) {
    if (Name(thing) == NULL) {
      if (IsGarbage(thing))
        set_name(thing, "Garbage");
      else {
        do_rawlog_lvl(LT_ERR, MLOG_ERR, "Null name on object #%d", thing);
        set_name(thing, "XXXX");
      }
    }
    if (STARTUPS && !IsGarbage(thing) && !(Halted(thing))) {
      queue_attribute_base(thing, "STARTUP", thing, 1, NULL, QUEUE_PRIORITY);
      do_top(5);
    }
  }
}

void init_names(void);
extern struct db_stat_info current_state;
void init_queue(void);
void build_rgb_map(void);
void init_telnet_opts(void);

/** Initialize game structures and read the most of the configuration file.
 * This function runs before we read in the databases. It is responsible
 * for recording the MUSH start time, setting up all the hash and
 * prefix tables and similar structures, and reading the portions of the
 * config file that don't require database load.
 * \param conf file name of the configuration file.
 */
void
init_game_config(const char *conf)
{
  pid_t mypid = -1;

  memset(report_cmd, 0, sizeof(report_cmd));

  init_queue();

  /* set MUSH start time */
  globals.start_time = time((time_t *) 0);
  if (!globals.first_start_time)
    globals.first_start_time = globals.start_time;

  /* initialize all the hash and prefix tables */
  init_flagspaces();
  init_flag_table("FLAG");
  init_flag_table("POWER");
  init_func_hashtab();
  init_ansi_codes();
  init_aname_table();
  init_atr_name_tree();
  init_pe_regs_trees();
  init_locks();
  init_names();
  init_pronouns();
  memset(&current_state, 0, sizeof current_state);

  /* Load all the config file stuff except restrict_* */
  local_configs();
  conf_default_set();
  config_file_startup(conf, 0);
  start_all_logs();
  config_file_checks();

  /* Initialize the attribute chunk storage */
  chunk_init();

  /* Set up telnet stuff */
  init_telnet_opts();

#ifdef HAVE_GETPID
  mypid = getpid();
#endif

  do_rawlog(LT_ERR, "%s", VERSION);
  do_rawlog(LT_ERR, "MUSH restarted, PID %d, at %s", (int) mypid,
            show_time(globals.start_time, 0));
}

void build_linked_table(void);

/** Post-db-load configuration.
 * This function contains code that should be run after dbs are loaded
 * (usually because we need to have the flag table loaded, or because they
 * run last). It reads in the portions of the config file that rely
 * on flags being defined.
 * \param conf file name of the configuration file.
 */
void
init_game_postdb(const char *conf)
{
  /* access file stuff */
  read_access_file();
  /* set up signal handlers for the timer */
  init_timer();
  /* Commands and functions require the flag table for restrictions */
  command_init_preconfig();
  command_init_postconfig();
  function_init_postconfig();
  attr_init_postconfig();
  /* Load further restrictions from config file */
  config_file_startup(conf, 1);
  validate_config();

  build_linked_table();

  /* Build color/RGB mappings */
  build_rgb_map();

  add_dict_words();

/* Set up ssl */
#ifndef SSL_SLAVE
  if (!ssl_init(options.ssl_private_key_file, options.ssl_certificate_file,
                options.ssl_ca_file, options.ssl_ca_dir,
                options.ssl_require_client_cert)) {
    do_rawlog(LT_ERR, "SSL initialization failure");
    options.ssl_port = 0; /* Disable ssl */
  }
#endif
  /* Load hash algorithms */
  OpenSSL_add_all_digests();
}

extern int dbline;

/** Read the game databases.
 * This function reads in the object, mail, and chat databases.
 * \retval -1 error.
 * \retval 0 success.
 */
int
init_game_dbs(void)
{
  PENNFILE *volatile f = NULL;
  const char *volatile infile;
  const char *outfile;
  const char *mailfile;
  volatile int panicdb;

#ifdef WIN32
  Win32MUSH_setup(); /* create index files, copy databases etc. */
#endif

  infile = restarting ? options.output_db : options.input_db;
  outfile = options.output_db;
  mailfile = options.mail_db;
  strcpy(globals.dumpfile, outfile);

  /* read small text files into cache */
  fcache_init();
  

  if (setjmp(db_err) == 1) {
    do_rawlog(LT_ERR, "Couldn't open %s! Creating minimal world.", infile);
    if (f) {
      penn_fclose(f);
    }
    init_compress(NULL);
    create_minimal_db();
    return 0;
  } else {
    int c;
    f = db_open(infile);
    c = penn_fgetc(f);
    if (c == EOF) {
      do_rawlog(LT_ERR, "Couldn't read %s! Creating minimal world.", infile);
      if (f) {
        penn_fclose(f);
      }
      init_compress(NULL);
      create_minimal_db();
      return 0;
    }

    penn_ungetc(c, f);
  }

  if (setjmp(db_err) == 1) {
    do_rawlog(LT_ERR, "ERROR: Unable to read %s. Giving up.\n", infile);
    if (f) {
      penn_fclose(f);
    }
    return -1;
  } else {
    /* ok, read it in */
    do_rawlog(LT_ERR, "ANALYZING: %s", infile);
    if (init_compress(f) != 1) {
      do_rawlog(LT_ERR, "ERROR LOADING %s", infile);
      return -1;
    }
    do_rawlog(LT_ERR, "ANALYZING: %s (done)", infile);

    /* everything ok */
    penn_fclose(f);

    f = db_open(infile);
    if (!f) {
      return -1;
    }

    /* ok, read it in */
    do_rawlog(LT_ERR, "LOADING: %s", infile);
    dbline = 0;
    if (db_read(f) < 0) {
      do_rawlog(LT_ERR, "ERROR LOADING %s", infile);
      penn_fclose(f);
      return -1;
    }
    do_rawlog(LT_ERR, "LOADING: %s (done)", infile);

    if (globals.new_indb_version < 6) {
      do_flag_delete("POWER", GOD, "Cemit");
    }

    /* If there's stuff at the end of the db, we may have a panic
     * format db, with everything shoved together. In that case,
     * don't close the file
     */
    panicdb = ((globals.indb_flags & DBF_PANIC) && !penn_feof(f));

    if (!panicdb) {
      penn_fclose(f);
    }

    /* complain about bad config options */
    if (!GoodObject(PLAYER_START) || (!IsRoom(PLAYER_START))) {
      do_rawlog(LT_ERR, "WARNING: Player_start (#%d) is NOT a room.",
                PLAYER_START);
    }
    if (!GoodObject(MASTER_ROOM) || (!IsRoom(MASTER_ROOM))) {
      do_rawlog(LT_ERR, "WARNING: Master room (#%d) is NOT a room.",
                MASTER_ROOM);
    }
    if (!GoodObject(BASE_ROOM) || (!IsRoom(BASE_ROOM))) {
      do_rawlog(LT_ERR, "WARNING: Base room (#%d) is NOT a room.", BASE_ROOM);
    }
    if (!GoodObject(DEFAULT_HOME) || (!IsRoom(DEFAULT_HOME))) {
      do_rawlog(LT_ERR, "WARNING: Default home (#%d) is NOT a room.",
                DEFAULT_HOME);
    }
    if (!GoodObject(GOD) || (!IsPlayer(GOD))) {
      do_rawlog(LT_ERR, "WARNING: God (#%d) is NOT a player.", GOD);
    }
  }

  /* read mail database */
  if (setjmp(db_err) == 1) {
    do_rawlog(LT_ERR,
              "ERROR: Unable to read mail database! Continuing with startup.");
  } else {
    mail_init();

    if (panicdb) {
      do_rawlog(LT_ERR, "LOADING: Trying to get mail from %s", infile);
      if (load_mail(f) <= 0) {
        do_rawlog(LT_ERR, "FAILED: Reverting to normal maildb");
        penn_fclose(f);
        panicdb = 0;
      }
    }

    if (!panicdb) {
      f = db_open(mailfile);
      /* okay, read it in */
      if (f) {
        do_rawlog(LT_ERR, "LOADING: %s", mailfile);
        dbline = 0;
        load_mail(f);
        do_rawlog(LT_ERR, "LOADING: %s (done)", mailfile);
        penn_fclose(f);
      }
    }
  }

  /* read chat database */
  if (setjmp(db_err) == 1) {
    do_rawlog(LT_ERR,
              "ERROR: Unable to read chat database! Continuing with startup.");
  } else {
    init_chatdb();

    if (panicdb) {
      do_rawlog(LT_ERR, "LOADING: Trying to get chat from %s", infile);
      if (load_chatdb(f, restarting) <= 0) {
        do_rawlog(LT_ERR, "FAILED: Reverting to normal chatdb");
        penn_fclose(f);
        panicdb = 0;
      }
    }

    if (!panicdb) {
      f = db_open(options.chatdb);
      if (f) {
        do_rawlog(LT_ERR, "LOADING: %s", options.chatdb);
        dbline = 0;
        if (load_chatdb(f, restarting)) {
          do_rawlog(LT_ERR, "LOADING: %s (done)", options.chatdb);
        } else {
          do_rawlog(LT_ERR, "ERROR LOADING %s", options.chatdb);
        }
        penn_fclose(f);
      }
    } else /* Close the panicdb file handle */
      penn_fclose(f);
  }

  return 0;
}

/* From bsd.c */
int file_watch_init(void);

/** Read cached text files.
 * \verbatim
 * This implements the @readcache function.
 * \endverbatim
 * \param player the enactor, for permission checking.
 */
void
do_readcache(dbref player)
{
  if (!Wizard(player)) {
    notify(player, T("Permission denied."));
    return;
  }
  fcache_load(player);
  help_rebuild(player);
  file_watch_init();
}

/** Check each attribute on each object in x for a $command matching cptr */
#define list_match(x, q, qflags)                                               \
  list_check(x, executor, '$', ':', cptr, 0, q, qflags)
/** Check each attribute on x for a $command matching cptr */
#define cmd_match(x, q, qflags)                                                \
  atr_comm_match(x, executor, '$', ':', cptr, 0, 1, NULL, NULL, 0, &errdb, q,  \
                 qflags, NULL)
#define MAYBE_ADD_ERRDB(errdb)                                                 \
  do {                                                                         \
    if (GoodObject(errdb) && errdblist) {                                      \
      if ((errdbtail - errdblist) >= errdbsize)                                \
        errdb_grow();                                                          \
      if ((errdbtail - errdblist) < errdbsize) {                               \
        *errdbtail = errdb;                                                    \
        errdbtail++;                                                           \
      }                                                                        \
      errdb = NOTHING;                                                         \
    }                                                                          \
  } while (0)

/** Attempt to tell if the command is a @password or @newpassword, so
 * that the password isn't logged by Suspect or log_commands
 * \param cmd The command to check
 * \return A sanitized version of the command suitable for logging.
 */
static char *
passwd_filter(const char *cmd)
{
  static bool initialized = 0;
  static pcre2_code *pass_ptn = NULL, *newpass_ptn = NULL;
  static pcre2_match_data *pass_md = NULL, *newpass_md = NULL;
  static char buff[BUFFER_LEN];
  char *bp = buff;
  size_t cmdlen;
  int matched;

  if (!initialized) {
    int errcode;
    PCRE2_SIZE eo;

    pass_ptn = pcre2_compile(
      (const PCRE2_UCHAR *) "^(@pass.*?)\\s([^=]*)=(.*)", PCRE2_ZERO_TERMINATED,
      re_compile_flags | PCRE2_CASELESS | PCRE2_NO_UTF_CHECK, &errcode, &eo,
      re_compile_ctx);
    if (!pass_ptn) {
      char errstr[120];
      pcre2_get_error_message(errcode, (PCRE2_UCHAR *) errstr, sizeof errstr);
      do_rawlog_lvl(LT_ERR, MLOG_ERR, "pcre_compile: %s", errstr);
      return "";
    }
    pcre2_jit_compile(pass_ptn, PCRE2_JIT_COMPLETE);
    pass_md = pcre2_match_data_create_from_pattern(pass_ptn, NULL);
    newpass_ptn =
      pcre2_compile((const PCRE2_UCHAR *) "^(@(?:newp|pcreate)[^=]*)=(.*)",
                    PCRE2_ZERO_TERMINATED,
                    re_compile_flags | PCRE2_CASELESS | PCRE2_NO_UTF_CHECK,
                    &errcode, &eo, re_compile_ctx);
    if (!newpass_ptn) {
      char errstr[120];
      pcre2_get_error_message(errcode, (PCRE2_UCHAR *) errstr, sizeof errstr);
      do_rawlog_lvl(LT_ERR, MLOG_ERR, "pcre_compile: %s", errstr);
      return "";
    }
    pcre2_jit_compile(newpass_ptn, PCRE2_JIT_COMPLETE);
    newpass_md = pcre2_match_data_create_from_pattern(newpass_ptn, NULL);
    initialized = 1;
  }

  cmdlen = strlen(cmd);
  buff[0] = '\0';

  if ((matched = pcre2_match(pass_ptn, (const PCRE2_UCHAR *) cmd, cmdlen, 0,
                             re_match_flags, pass_md, re_match_ctx)) > 0) {
    /* It's a password */
    PCRE2_SIZE bufflen = BUFFER_LEN;
    PCRE2_SIZE *ovec = pcre2_get_ovector_pointer(pass_md);
    pcre2_substring_copy_bynumber(pass_md, 1, (PCRE2_UCHAR *) buff, &bufflen);
    bp = buff + bufflen;
    safe_chr(' ', buff, &bp);
    safe_fill('*', ovec[5] - ovec[4], buff, &bp);
    safe_chr('=', buff, &bp);
    safe_fill('*', ovec[7] - ovec[6], buff, &bp);
  } else if ((matched =
                pcre2_match(newpass_ptn, (const PCRE2_UCHAR *) cmd, cmdlen, 0,
                            re_match_flags, newpass_md, re_match_ctx)) > 0) {
    PCRE2_SIZE bufflen = BUFFER_LEN;
    PCRE2_SIZE *ovec = pcre2_get_ovector_pointer(newpass_md);
    pcre2_substring_copy_bynumber(newpass_md, 1, (PCRE2_UCHAR *) buff,
                                  &bufflen);
    bp = buff + bufflen;
    safe_chr('=', buff, &bp);
    safe_fill('*', ovec[5] - ovec[4], buff, &bp);
  } else {
    safe_strl(cmd, cmdlen, buff, &bp);
  }
  *bp = '\0';
  return buff;
}

/** Attempt to match and execute a command.
 * This function performs some sanity checks and then attempts to
 * run a command. It checks, in order: built-in commands,
 * enter aliases, leave aliases, $commands on neighboring objects or
 * the player, $commands on the container, $commands on inventory,
 * exits in the zone master room, $commands on objects in the ZMR,
 * $commands on the ZMO, $commands on the player's zone, exits in the
 * master room, and $commands on objects in the master room.
 *
 * When a command is directly input from a socket, we don't parse
 * the value in attribute sets.
 *
 * \param executor the object running the command.
 * \param command command to match and execute.
 * \param queue_entry the queue entry the command is being run for
 */
void
process_command(dbref executor, char *command, MQUE *queue_entry)
{
  int a;
  char *p; /* utility */

  char unp[BUFFER_LEN]; /* unparsed command */
  /* general form command arg0=arg1,arg2...arg10 */
  char temp[BUFFER_LEN]; /* utility */
  int i;                 /* utility */
  char *cptr;
  dbref errdb;
  dbref check_loc;
  COMMAND_INFO *cmd;
  int queue_flags = QUEUE_DEFAULT;
  int temp_debug_privs = 0;

  if (!errdblist)
    if (!(errdblist = mush_calloc(errdbsize, sizeof(dbref), "errdblist")))
      mush_panic("Unable to allocate memory in process_command()!");

  errdbtail = errdblist;
  errdb = NOTHING;
  if (!command) {
    do_rawlog_lvl(LT_ERR, MLOG_ERR, "ERROR: No command!!!");
    return;
  }
  /* robustify executor */
  if (!GoodObject(executor)) {
    do_rawlog_lvl(LT_ERR, MLOG_ERR, "process_command bad player #%d", executor);
    return;
  }

  /* Destroyed objects shouldn't execute commands */
  if (IsGarbage(executor))
    /* No message - nobody to tell, and it's too easy to do to log. */
    return;
  /* Halted objects can't execute commands */
  /* And neither can halted players if the command isn't from_port */
  if (Halted(executor) &&
      (!IsPlayer(executor) || !(queue_entry->queue_type & QUEUE_SOCKET))) {
    notify_format(Owner(executor),
                  T("Attempt to execute command by halted object #%d"),
                  executor);
    return;
  }
  /* Players, things, and exits should not have invalid locations. This check
   * must be done _after_ the destroyed-object check.
   */
  check_loc = speech_loc(executor);
  if (!GoodObject(check_loc) || IsGarbage(check_loc)) {
    notify_format(Owner(executor),
                  T("Invalid location on command execution: %s(#%d)"),
                  Name(executor), executor);
    do_rawlog_lvl(LT_ERR, MLOG_ERR,
                  "Command attempted by %s(#%d) in invalid location #%d.",
                  Name(executor), executor, Location(executor));
    if (Mobile(executor)) {
      moveto(executor, PLAYER_START, SYSEVENT,
             "dbck"); /* move it someplace valid */
    }
  }
  orator = executor;

  /* eat leading whitespace */
  while (*command && isspace(*command))
    command++;

  /* eat trailing whitespace */
  p = command + strlen(command) - 1;
  while (isspace(*p) && (p >= command))
    p--;
  *++p = '\0';

  if (*command == DEBUG_TOKEN) {
    command++;
    temp_debug_privs = ((queue_entry->queue_type & QUEUE_DEBUG_PRIVS) == 0);
    queue_entry->queue_type |= QUEUE_DEBUG_PRIVS;
  }
  if (queue_entry->queue_type & QUEUE_DEBUG_PRIVS)
    queue_flags |= QUEUE_DEBUG_PRIVS;

  if (queue_entry->queue_type & QUEUE_SOCKET)
    queue_flags |= QUEUE_INPLACE;

  /* ignore null commands that aren't from players */
  if ((!command || !*command) && !(queue_entry->queue_type & QUEUE_SOCKET))
    return;

  {
    char *msg = passwd_filter(command);

    log_activity(LA_CMD, executor, msg);
    if (!(queue_entry->queue_type & QUEUE_EVENT) &&
        (options.log_commands || Suspect(executor))) {
      do_log(LT_CMD, executor, NOTHING, "%s", msg);
    }
    if (Verbose(executor)) {
      char tmp[BUFFER_LEN + 20];
      snprintf(tmp, sizeof tmp, "#%d] %s", executor, msg);
      raw_notify(Owner(executor), tmp);
    }
  }

  strcpy(unp, command);

  /* Check all our hardcoded commands, chat tokens, etc. Returns if no match. */
  cptr = command_parse(executor, command, queue_entry);

  if (cptr) {
    if (queue_entry->pe_info->cmd_evaled) {
      mush_free(queue_entry->pe_info->cmd_evaled, "string");
    }
    queue_entry->pe_info->cmd_evaled = mush_strdup(cptr, "string");
    a = 0;
    if (!Gagged(executor)) {

      if (Mobile(executor)) {
        /* if the executor is an exit or room, no need to do these checks */
        /* try matching enter aliases */
        if (check_loc != NOTHING && (cmd = command_find("ENTER")) &&
            !(cmd->type & CMD_T_DISABLED) &&
            (i = alias_list_check(Contents(check_loc), cptr, "EALIAS")) !=
              NOTHING) {
          if (command_check_with(executor, cmd, 1, queue_entry->pe_info)) {
            char upd[SBUF_LEN];
            snprintf(temp, sizeof temp, "ENTER #%d", i);
            snprintf(upd, sizeof upd, "#%d", i);
            run_command(cmd, executor, queue_entry->enactor, temp, NULL, NULL,
                        temp, NULL, NULL, upd, NULL, NULL, NULL, queue_entry);
          }
          goto done;
        }
        /* if that didn't work, try matching leave aliases */
        if (!IsRoom(check_loc) && (cmd = command_find("LEAVE")) &&
            !(cmd->type & CMD_T_DISABLED) &&
            (loc_alias_check(check_loc, cptr, "LALIAS"))) {
          if (command_check_with(executor, cmd, 1, queue_entry->pe_info))
            run_command(cmd, executor, queue_entry->enactor, "LEAVE", NULL,
                        NULL, "LEAVE", NULL, NULL, NULL, NULL, NULL, NULL,
                        queue_entry);
          goto done;
        }
      }

      /* try matching user defined commands before chopping */

      /* try objects in the executor's location, the location itself,
       * and objects in the executor's inventory.
       */
      if (GoodObject(check_loc)) {
        a += list_match(Contents(check_loc), queue_entry, queue_flags);
        if (check_loc != executor) {
          a += cmd_match(check_loc, queue_entry, queue_flags);
          MAYBE_ADD_ERRDB(errdb);
        }
      }
      if (check_loc != executor)
        a += list_match(Contents(executor), queue_entry, queue_flags);

      /* now do check on zones */
      if ((!a) && (Zone(check_loc) != NOTHING)) {
        if (IsRoom(Zone(check_loc))) {
          /* zone of executor's location is a zone master room,
           * so we check for exits and commands
           */
          /* check zone master room exits */
          if (remote_exit(executor, cptr) && (cmd = command_find("GOTO")) &&
              !(cmd->type & CMD_T_DISABLED)) {
            if (!Mobile(executor) ||
                !command_check_with(executor, cmd, 1, queue_entry->pe_info)) {
              goto done;
            } else {
              snprintf(temp, sizeof temp, "GOTO %s", cptr);
              run_command(cmd, executor, queue_entry->enactor, temp, NULL, NULL,
                          temp, NULL, NULL, cptr, NULL, NULL, NULL,
                          queue_entry);
              goto done;
            }
          } else
            a +=
              list_match(Contents(Zone(check_loc)), queue_entry, queue_flags);
        } else {
          a += cmd_match(Zone(check_loc), queue_entry, queue_flags);
          MAYBE_ADD_ERRDB(errdb);
        }
      }
      /* if nothing matched with zone master room/zone object, try
       * matching zone commands on the executor's personal zone
       */
      if ((!a) && (Zone(executor) != NOTHING) &&
          (Zone(check_loc) != Zone(executor))) {
        if (IsRoom(Zone(executor)))
          /* Player's personal zone is a zone master room, so we
           * also check commands on objects in that room
           */
          a += list_match(Contents(Zone(executor)), queue_entry, queue_flags);
        else {
          a += cmd_match(Zone(executor), queue_entry, queue_flags);
          MAYBE_ADD_ERRDB(errdb);
        }
      }
      /* end of zone stuff */
      /* check global exits only if no other commands are matched */
      if ((!a) && (check_loc != MASTER_ROOM)) {
        if (global_exit(executor, cptr) && (cmd = command_find("GOTO")) &&
            !(cmd->type & CMD_T_DISABLED)) {
          if (!Mobile(executor) ||
              !command_check_with(executor, cmd, 1, queue_entry->pe_info))
            goto done;
          else {
            snprintf(temp, sizeof temp, "GOTO %s", cptr);
            run_command(cmd, executor, queue_entry->enactor, temp, NULL, NULL,
                        temp, NULL, NULL, cptr, NULL, NULL, NULL, queue_entry);
            goto done;
          }
        } else
          /* global user-defined commands checked if all else fails.
           * May match more than one command in the master room.
           */
          a += list_match(Contents(MASTER_ROOM), queue_entry, queue_flags);
      }
      /* end of master room check */
    } /* end of special checks */
    if (!a) {
      /* Do we have any error dbs queued up, and if so, do any
       * have associated failure messages?
       */
      if ((errdblist == errdbtail) || (!fail_commands(executor)))
        /* Nope. This is totally unmatched, run generic failure */
        generic_command_failure(executor, queue_entry->enactor, cptr,
                                queue_entry);
    }
  }

  /* command has been executed. Free up memory. */

done:
  if (errdblist) {
    mush_free(errdblist, "errdblist");
    errdblist = errdbtail = NULL;
    errdbsize = ERRDB_INITIAL_SIZE;
  }
  if (temp_debug_privs)
    queue_entry->queue_type &= ~QUEUE_DEBUG_PRIVS;
}

COMMAND(cmd_with)
{
  dbref what;
  char *cptr = arg_right;
  dbref errdb;

  what = noisy_match_result(executor, arg_left, NOTYPE, MAT_EVERYTHING);
  if (!GoodObject(what))
    return;
  if (!(nearby(executor, what) || Long_Fingers(executor) ||
        controls(executor, what))) {
    if (SW_ISSET(sw, SWITCH_ROOM)) {
      if (what != MASTER_ROOM && what != Zone(executor)) {
        notify(executor, T("I don't see that here."));
        return;
      } else if (what == Zone(executor) && !IsRoom(what)) {
        notify(executor, T("Make room! Make room!"));
        return;
      }
    } else if (what != Zone(executor) || IsRoom(what)) {
      notify(executor, T("I don't see that here."));
      return;
    }
  }

  errdbtail = errdblist;
  errdb = NOTHING;
  if (!SW_ISSET(sw, SWITCH_ROOM)) {
    /* Run commands on a single object */
    /* Should this be passing on QUEUE_DEBUG_PRIVS? */
    if (!cmd_match(what, NULL, QUEUE_DEFAULT)) {
      MAYBE_ADD_ERRDB(errdb);
      notify(executor, T("No matching command."));
    }
  } else {
    /* Run commands on objects in a masterish room */

    if (!IsRoom(what) && what != Location(executor)) {
      notify(executor, T("Make room! Make room!"));
      return;
    }

    /* Should this be passing on QUEUE_DEBUG_PRIVS? */
    if (!list_match(Contents(what), NULL, QUEUE_DEFAULT))
      notify(executor, T("No matching command."));
  }
}

/* now undef everything that needs to be */
#undef list_match
#undef cmd_match

/** Check to see if a string matches part of a semicolon-separated list.
 * \param command string to match.
 * \param list semicolon-separated list of aliases to match against.
 * \retval 1 string matched an alias.
 * \retval 0 string failed to match an alias.
 */
int
check_alias(const char *command, const char *list)
{
  /* check if a string matches part of a semi-colon separated list */
  const char *p;
  while (*list) {
    for (p = command;
         (*p && DOWNCASE(*p) == DOWNCASE(*list) && *list != ALIAS_DELIMITER);
         p++, list++)
      ;
    if (*p == '\0') {
      while (isspace(*list))
        list++;
      if (*list == '\0' || *list == ALIAS_DELIMITER)
        return 1; /* word matched */
    }
    /* didn't match. check next word in list */
    while (*list && *list++ != ALIAS_DELIMITER)
      ;
    while (isspace(*list))
      list++;
  }
  /* reached the end of the list without matching anything */
  return 0;
}

/** Match a command or listen pattern against a list of things.
 * This function iterates through a list of things (using the object
 * Next pointer, so typically this is a list of contents of an object),
 * and checks each for an attribute matching a command/listen pattern.
 * \param thing first object on list.
 * \param player the enactor.
 * \param type type of attribute to match ('$' or '^')
 * \param end character that signals the end of the matchable portion (':')
 * \param str string to match against the attributes.
 * \param just_match if 1, don't execute the command on match.
 * \param queue_flags QUEUE_* flags to pass to atr_comm_match
 * \retval 1 a match was made.
 * \retval 0 no match was made.
 */
static int
list_check(dbref thing, dbref player, char type, char end, char *str,
           int just_match, MQUE *queue_entry, int queue_flags)
{
  int match = 0;
  dbref errdb = NOTHING;

  while (thing != NOTHING) {
    if (atr_comm_match(thing, player, type, end, str, just_match, 1, NULL, NULL,
                       0, &errdb, queue_entry, queue_flags, NULL))
      match = 1;
    else {
      MAYBE_ADD_ERRDB(errdb);
    }
    thing = Next(thing);
  }
  return (match);
}

/** Match a command against an attribute of aliases.
 * This function iterates through a list of things (using the object
 * Next pointer, so typically this is a list of contents of an object),
 * and checks each for an attribute of aliases that might be matched
 * (as in EALIAS).
 * \param thing first object on list.
 * \param command command to attempt to match.
 * \param type name of attribute of aliases to match against.
 * \return dbref of first matching object, or NOTHING if none.
 */
int
alias_list_check(dbref thing, const char *command, const char *type)
{
  ATTR *a;
  char alias[BUFFER_LEN];

  while (thing != NOTHING) {
    a = atr_get_noparent(thing, type);
    if (a) {
      strcpy(alias, atr_value(a));
      if (check_alias(command, alias) != 0)
        return thing; /* matched an alias */
    }
    thing = Next(thing);
  }
  return NOTHING;
}

/** Check a command against a list of aliases on a location
 * (as for LALIAS).
 * \param loc location with attribute of aliases.
 * \param command command to attempt to match.
 * \param type name of attribute of aliases to match against.
 * \retval 1 successful match.
 * \retval 0 failure.
 */
int
loc_alias_check(dbref loc, const char *command, const char *type)
{
  ATTR *a;
  char alias[BUFFER_LEN];
  a = atr_get_noparent(loc, type);
  if (a) {
    strcpy(alias, atr_value(a));
    return (check_alias(command, alias));
  } else
    return 0;
}

/** Can an object hear?
 * This function determines if a given object can hear. A Hearer is:
 * a connected player, a puppet, an AUDIBLE object with a FORWARDLIST
 * attribute, or an object with a LISTEN attribute.
 * \param thing object to check.
 * \retval 1 object can hear.
 * \retval 0 object can't hear.
 */
int
Hearer(dbref thing)
{
  ALIST *ptr;
  int cmp;

  if (Connected(thing) || Puppet(thing))
    return 1;
  ATTR_FOR_EACH (thing, ptr) {
    if (Audible(thing) && (strcmp(AL_NAME(ptr), "FORWARDLIST") == 0))
      return 1;
    cmp = strcoll(AL_NAME(ptr), "LISTEN");
    if (cmp == 0)
      return 1;
    if (cmp > 0)
      break;
  }
  return 0;
}

/** Might an object be responsive to commands?
 * This function determines if a given object might pick up a $command.
 * That is, if it has any attributes with $commands on them that are
 * not set no_command.
 * \param thing object to check.
 * \retval 1 object responds to commands.
 * \retval 0 object doesn't respond to commands.
 */
int
Commer(dbref thing)
{
  ALIST *ptr;

  ATTR_FOR_EACH (thing, ptr) {
    if (AF_Command(ptr) && !AF_Noprog(ptr))
      return 1;
  }
  return 0;
}

/** Is an object listening?
 * This function determines if a given object is a Listener. A Listener
 * is a thing or room that has the MONITOR flag set.
 * \param thing object to check.
 * \retval 1 object is a Listener.
 * \retval 0 object isn't listening with ^patterns.
 */
int
Listener(dbref thing)
{
  /* If a monitor flag is set on a room or thing, it's a listener.
   * Otherwise not (even if ^patterns are present)
   */
  return has_flag_by_name(thing, "MONITOR", NOTYPE);
}

/** Reset all players' money.
 * \verbatim
 * This function implements the @poor command. It probably belongs in
 * rob.c, though.
 * \endverbatim
 * \param player the enactor, for permission checking.
 * \param arg1 the amount of money to reset all players to.
 */
void
do_poor(dbref player, char *arg1)
{
  int amt = atoi(arg1);
  dbref a;
  if (!God(player)) {
    notify(player, T("Only God can cause financial ruin."));
    return;
  }
  for (a = 0; a < db_top; a++)
    if (IsPlayer(a))
      s_Pennies(a, amt);
  notify_format(player,
                T("The money supply of all players has been reset to %d %s."),
                amt, MONIES);
  do_log(LT_WIZ, player, NOTHING,
         "** POOR done ** Money supply reset to %d %s.", amt, MONIES);
}

/** User interface to write a message to a log.
 * \verbatim
 * This function implements @log.
 * \endverbatim
 * \param player the enactor.
 * \param str message to write to the log.
 * \param ltype type of log to write to.
 */
void
do_writelog(dbref player, char *str, int ltype)
{
  if (!Wizard(player)) {
    notify(player, T("Permission denied."));
    return;
  }
  do_rawlog(ltype, "LOG: %s(#%d%s): %s", Name(player), player,
            unparse_flags(player, GOD), str);

  notify(player, T("Logged."));
}

#define queue_dolist(al, pe_regs)                                              \
  if (queue_type & QUEUE_INPLACE) {                                            \
    new_queue_actionlist(executor, enactor, enactor, al, queue_entry,          \
                         PE_INFO_SHARE, queue_type, pe_regs);                  \
  } else {                                                                     \
    new_queue_actionlist(executor, enactor, enactor, al, queue_entry,          \
                         PE_INFO_CLONE, queue_type, pe_regs);                  \
    if (pe_regs)                                                               \
      pe_regs_free(pe_regs);                                                   \
  }

/** Bind occurences of '##' in "action" to "arg", then run "action".
 * \param executor the executor.
 * \param enactor object that caused command to run.
 * \param action command string which may contain tokens.
 * \param arg value for ## token.
 * \param num value for #@ token.
 * \param queue_entry the queue entry this is being run from
 * \param queue_type QUEUE_* values
 */
void
bind_and_queue(dbref executor, dbref enactor, char *action, const char *arg,
               int num, MQUE *queue_entry, int queue_type)
{
  char *command;
  const char *replace[2];
  char placestr[10];
  PE_REGS *pe_regs;

  replace[0] = arg;
  snprintf(placestr, 10, "%d", num);
  replace[1] = placestr;

  command = replace_string2(standard_tokens, replace, action);

  /* Add the new iter context to the parent queue entry's pe_info... */
  pe_regs = pe_regs_create(PE_REGS_ITER, "bind_and_queue");
  pe_regs_set(pe_regs, PE_REGS_ITER, "t0", arg);
  pe_regs_set_int(pe_regs, PE_REGS_ITER, "n0", num);
  /* Then queue the new command, using a cloned pe_info... */
  queue_dolist(command, pe_regs);

  mush_free(command, "replace_string.buff");
}

/** Would the \@scan command find attributes with $-commands matching c on
 * victim for looker? executor must be able to examine victim */
#define ScanFind(executor, looker, victim, c)                                  \
  (Can_Examine(executor, victim) &&                                            \
   ((num = atr_comm_match(victim, looker, '$', ':', command, 1, 1, atrname,    \
                          &ptr, c, NULL, NULL, QUEUE_DEFAULT, NULL)) != 0))

/** Scan for matches of $commands.
 * This function scans for possible matches of user-def'd commands from the
 * viewpoint of player, and return as a string.
 * It assumes that atr_comm_match() returns atrname with a leading space.
 * \param executor object to use for permission checking.
 * \param looker the object from whose viewpoint to scan.
 * \param command the command to scan for matches to.
 * \param flag CHECK_* flags to limit objects searched
 * \return string of obj/attrib pairs with matching $commands.
 */
char *
scan_list(dbref executor, dbref looker, char *command, int flag)
{
  static char tbuf[BUFFER_LEN];
  char *tp;
  dbref thing;
  char atrname[BUFFER_LEN];
  char *ptr;
  int num;
  int matches = 0;
  dbref loc = speech_loc(looker);

  if (!GoodObject(loc)) {
    strcpy(tbuf, T("#-1 INVALID LOCATION"));
    return tbuf;
  }
  if (!command || !*command) {
    strcpy(tbuf, T("#-1 NO COMMAND"));
    return tbuf;
  }
  tp = tbuf;
  ptr = atrname;

  if (flag & CHECK_HERE) {
    if (ScanFind(executor, looker, loc, 1)) {
      *ptr = '\0';
      safe_str(atrname, tbuf, &tp);
      ptr = atrname;
      matches++;
    }
    if (looker == loc)
      flag &= ~CHECK_SELF;
  }

  if (flag & CHECK_NEIGHBORS) {
    flag &= ~CHECK_SELF;
    DOLIST (thing, Contents(loc)) {
      if (ScanFind(executor, looker, thing, 1)) {
        *ptr = '\0';
        safe_str(atrname, tbuf, &tp);
        ptr = atrname;
        matches++;
      }
    }
    if (looker == loc)
      flag &= ~CHECK_INVENTORY;
  }

  if ((flag & CHECK_SELF)) {
    if (ScanFind(executor, looker, looker, 1)) {
      *ptr = '\0';
      safe_str(atrname, tbuf, &tp);
      ptr = atrname;
      matches++;
    }
  }

  if (flag & CHECK_INVENTORY) {
    DOLIST (thing, Contents(looker)) {
      if (ScanFind(executor, looker, thing, 1)) {
        *ptr = '\0';
        safe_str(atrname, tbuf, &tp);
        ptr = atrname;
        matches++;
      }
    }
  }

  /* zone checks */
  if ((flag & CHECK_ZONE)) {
    if (Zone(loc) != NOTHING && !(matches && (flag & CHECK_BREAK))) {
      if (IsRoom(Zone(Location(looker)))) {
        /* zone of looker's location is a zone master room */
        if (Location(looker) != Zone(looker)) {
          DOLIST (thing, Contents(Zone(Location(looker)))) {
            if (ScanFind(executor, looker, thing, 1)) {
              *ptr = '\0';
              safe_str(atrname, tbuf, &tp);
              ptr = atrname;
              matches++;
            }
          }
        }
      } else {
        /* regular zone object */
        if (ScanFind(executor, looker, Zone(loc), 1)) {
          *ptr = '\0';
          safe_str(atrname, tbuf, &tp);
          ptr = atrname;
          matches++;
        }
      }
    }
    if ((Zone(looker) != NOTHING) && !(matches && (flag & CHECK_BREAK)) &&
        (Zone(looker) != Zone(loc))) {
      /* check the looker's personal zone */
      if (IsRoom(Zone(looker))) {
        if (Location(looker) != Zone(looker)) {
          DOLIST (thing, Contents(Zone(looker))) {
            if (ScanFind(executor, looker, thing, 1)) {
              *ptr = '\0';
              safe_str(atrname, tbuf, &tp);
              ptr = atrname;
              matches++;
            }
          }
        }
      } else if (ScanFind(executor, looker, Zone(looker), 1)) {
        *ptr = '\0';
        safe_str(atrname, tbuf, &tp);
        ptr = atrname;
        matches++;
      }
    }
  }

  if ((flag & CHECK_GLOBAL) && !(matches && (flag & CHECK_BREAK)) &&
      (loc != MASTER_ROOM) && (Zone(loc) != MASTER_ROOM) &&
      (Zone(looker) != MASTER_ROOM)) {
    /* try Master Room stuff */
    DOLIST (thing, Contents(MASTER_ROOM)) {
      if (ScanFind(executor, looker, thing, 1)) {
        *ptr = '\0';
        safe_str(atrname, tbuf, &tp);
        ptr = atrname;
        matches++;
      }
    }
  }
  *tp = '\0';
  if (*tbuf && *tbuf == ' ')
    return tbuf + 1; /* atrname comes with leading spaces */
  return tbuf;
}

/** User interface to scan for $command matches.
 * \verbatim
 * This function implements @scan.
 * \endverbatim
 * \param player the enactor.
 * \param command command to scan for matches to.
 * \param flag bitflags for where to scan.
 */
void
do_scan(dbref player, char *command, int flag)
{
  /* scan for possible matches of user-def'ed commands */
  char atrname[BUFFER_LEN];
  char *ptr;
  dbref thing;
  int num;

  ptr = atrname;
  if (!GoodObject(Location(player))) {
    notify(player, T("Sorry, you are in an invalid location."));
    return;
  }
  if (!command || !*command) {
    notify(player, T("What command do you want to scan for?"));
    return;
  }
  if (flag & CHECK_NEIGHBORS) {
    notify(player, T("Matches on contents of this room:"));
    DOLIST (thing, Contents(Location(player))) {
      if (ScanFind(player, player, thing, 0)) {
        *ptr = '\0';
        notify_format(player, "%s  [%d:%s]",
                      unparse_object(player, thing, AN_UNPARSE), num, atrname);
        ptr = atrname;
      }
    }
  }
  ptr = atrname;
  if (flag & CHECK_HERE) {
    if (ScanFind(player, player, Location(player), 0)) {
      *ptr = '\0';
      notify_format(player, T("Matched here: %s  [%d:%s]"),
                    unparse_object(player, Location(player), AN_UNPARSE), num,
                    atrname);
    }
  }
  ptr = atrname;
  if (flag & CHECK_INVENTORY) {
    notify(player, T("Matches on carried objects:"));
    DOLIST (thing, Contents(player)) {
      if (ScanFind(player, player, thing, 0)) {
        *ptr = '\0';
        notify_format(player, "%s  [%d:%s]",
                      unparse_object(player, thing, AN_UNPARSE), num, atrname);
        ptr = atrname;
      }
    }
  }
  ptr = atrname;
  if (flag & CHECK_SELF) {
    if (ScanFind(player, player, player, 0)) {
      *ptr = '\0';
      notify_format(player, T("Matched self: %s  [%d:%s]"),
                    unparse_object(player, player, AN_UNPARSE), num, atrname);
    }
  }
  ptr = atrname;
  if (flag & CHECK_ZONE) {
    /* zone checks */
    if (Zone(Location(player)) != NOTHING) {
      if (IsRoom(Zone(Location(player)))) {
        /* zone of player's location is a zone master room */
        if (Location(player) != Zone(player)) {
          notify(player, T("Matches on zone master room of location:"));
          DOLIST (thing, Contents(Zone(Location(player)))) {
            if (ScanFind(player, player, thing, 0)) {
              *ptr = '\0';
              notify_format(player, "%s  [%d:%s]",
                            unparse_object(player, thing, AN_UNPARSE), num,
                            atrname);
              ptr = atrname;
            }
          }
        }
      } else {
        /* regular zone object */
        if (ScanFind(player, player, Zone(Location(player)), 0)) {
          *ptr = '\0';
          notify_format(
            player, T("Matched zone of location: %s  [%d:%s]"),
            unparse_object(player, Zone(Location(player)), AN_UNPARSE), num,
            atrname);
        }
      }
    }
    ptr = atrname;
    if ((Zone(player) != NOTHING) && (Zone(player) != Zone(Location(player)))) {
      /* check the player's personal zone */
      if (IsRoom(Zone(player))) {
        if (Location(player) != Zone(player)) {
          notify(player, T("Matches on personal zone master room:"));
          DOLIST (thing, Contents(Zone(player))) {
            if (ScanFind(player, player, thing, 0)) {
              *ptr = '\0';
              notify_format(player, "%s  [%d:%s]",
                            unparse_object(player, thing, AN_UNPARSE), num,
                            atrname);
              ptr = atrname;
            }
          }
        }
      } else if (ScanFind(player, player, Zone(player), 0)) {
        *ptr = '\0';
        notify_format(player, T("Matched personal zone: %s  [%d:%s]"),
                      unparse_object(player, Zone(player), AN_UNPARSE), num,
                      atrname);
      }
    }
  }
  ptr = atrname;
  if ((flag & CHECK_GLOBAL) && (Location(player) != MASTER_ROOM) &&
      (Zone(Location(player)) != MASTER_ROOM) &&
      (Zone(player) != MASTER_ROOM)) {
    /* try Master Room stuff */
    notify(player, T("Matches on objects in the Master Room:"));
    DOLIST (thing, Contents(MASTER_ROOM)) {
      if (ScanFind(player, player, thing, 0)) {
        *ptr = '\0';
        notify_format(player, "%s  [%d:%s]",
                      unparse_object(player, thing, AN_UNPARSE), num, atrname);
        ptr = atrname;
      }
    }
  }
}

#define DOL_NOTIFY 2 /**< Add a notify after a dolist */
#define DOL_DELIM 4  /**< Specify a delimiter to a dolist */

/** Execute a command for each element of a list.
 * \verbatim
 * This function implements @dolist.
 * \endverbatim
 * \param executor the executor.
 * \param list string containing the list to iterate over.
 * \param command command to run for each list element.
 * \param enactor the enactor
 * \param flags command switch flags.
 * \param queue_entry the queue entry \@dolist is being run in
 * \param queue_type QUEUE_* values
 */
void
do_dolist(dbref executor, char *list, char *command, dbref enactor,
          unsigned int flags, MQUE *queue_entry, int queue_type)
{
  char *curr, *objstring;
  char outbuf[BUFFER_LEN];
  char *bp;
  int place;
  char delim = ' ';
  if (!command || !*command) {
    notify(executor, T("What do you want to do with the list?"));
    if (flags & DOL_NOTIFY) {
      queue_dolist("@notify me", NULL);
    }
    return;
  }

  if (flags & DOL_DELIM) {
    if (list[1] != ' ') {
      notify(executor, T("Separator must be one character."));
      if (flags & DOL_NOTIFY) {
        queue_dolist("@notify me", NULL);
      }
      return;
    }
    delim = list[0];
  }

  bp = outbuf;
  if (flags & DOL_DELIM)
    list += 2;
  place = 0;
  objstring = trim_space_sep(list, delim);
  if (objstring && !*objstring) {
    /* Blank list */
    if (flags & DOL_NOTIFY) {
      queue_dolist("@notify me", NULL);
    }
    return;
  }

  while (objstring) {
    curr = split_token(&objstring, delim);
    place++;
    bind_and_queue(executor, enactor, command, curr, place, queue_entry,
                   queue_type);
  }

  *bp = '\0';
  if (flags & DOL_NOTIFY) {
    /*  Execute a '@notify me' so the object knows we're done
     *  with the list execution. We don't execute dequeue_semaphores()
     *  directly, since we want the command to be queued
     *  _after_ the list has executed.
     */
    queue_dolist("@notify me", NULL);
  }
}

static void linux_uptime(dbref player) __attribute__((__unused__));
static void unix_uptime(dbref player) __attribute__((__unused__));
static void win32_uptime(dbref player) __attribute__((__unused__));

static void
linux_uptime(dbref player __attribute__((__unused__)))
{
#ifdef linux
  /* Use /proc files instead of calling the external uptime program on linux */
  char tbuf1[BUFFER_LEN];
  FILE *fp;
  char line[128]; /* Overkill */
  char *nl;
  pid_t pid;
  int psize;
#ifdef HAVE_GETRUSAGE
  struct rusage usage;
#endif

  /* Current time */
  {
    struct tm *t;
    t = localtime(&mudtime);
    strftime(tbuf1, sizeof tbuf1, "Server uptime: %I:%M%p ", t);
    nl = tbuf1 + strlen(tbuf1);
  }
  /* System uptime */
  fp = fopen("/proc/uptime", "r");
  if (fp) {
    time_t uptime;
    const char *fmt;
    if (fgets(line, sizeof line, fp)) {
      /* First part of this line is uptime in seconds.milliseconds. We
         only care about seconds. */
      uptime = strtol(line, NULL, 10);
      if (uptime > 86400)
        fmt = "up $d days, $2h:$2M,";
      else
        fmt = "up $2h:$2M,";
      do_timestring(tbuf1, &nl, fmt, uptime);
    } else {
      safe_str("Unknown uptime,", tbuf1, &nl);
    }
    fclose(fp);
  } else {
    safe_str("Unknown uptime,", tbuf1, &nl);
  }

  /* Now load averages */
  fp = fopen("/proc/loadavg", "r");
  if (fp) {
    if (fgets(line, sizeof line, fp)) {
      double load[3];
      char *x, *l = line;
      load[0] = strtod(l, &x);
      l = x;
      load[1] = strtod(l, &x);
      l = x;
      load[2] = strtod(l, NULL);
      safe_format(tbuf1, &nl, " load average: %.2f, %.2f, %.2f", load[0],
                  load[1], load[2]);
    } else {
      safe_str("Unknown load", tbuf1, &nl);
    }
    fclose(fp);
  } else {
    safe_str("Unknown load", tbuf1, &nl);
  }

  *nl = '\0';
  notify(player, tbuf1);

  /* do process stats */
  pid = getpid();
  psize = mush_getpagesize();
  notify_format(player, "\nProcess ID:  %10u        %10d bytes per page", pid,
                psize);

/* Linux's getrusage() is mostly unimplemented. Just has times, page faults
   and swapouts. We use /proc/self/status */
#ifdef HAVE_GETRUSAGE
  getrusage(RUSAGE_SELF, &usage);
  notify_format(player, "Time used:   %10ld user   %10ld sys",
                usage.ru_utime.tv_sec, usage.ru_stime.tv_sec);
  notify_format(player,
                "Page faults: %10ld hard   %10ld soft    %10ld swapouts",
                usage.ru_majflt, usage.ru_minflt, usage.ru_nswap);
#endif
  fp = fopen("/proc/self/status", "r");
  if (!fp)
    return;
  /* Skip lines we don't care about. */
  while (fgets(line, sizeof line, fp) != NULL) {
    static const char *fields[] = {
      "VmSize:", "VmRSS:",  "VmData:", "VmStk:",  "VmExe:", "VmLib:",
      "SigPnd:", "SigBlk:", "SigIgn:", "SigCgt:", NULL};
    int n;
    for (n = 0; fields[n]; n++) {
      size_t len = strlen(fields[n]);
      if (strncmp(line, fields[n], len) == 0) {
        if ((nl = strchr(line, '\n')) != NULL)
          *nl = '\0';
        notify(player, line);
      }
    }
  }

  fclose(fp);

#endif
}

static void
unix_uptime(dbref player __attribute__((__unused__)))
{
#ifndef WIN32
#ifdef HAVE_UPTIME
  FILE *fp;
  int c;
  int i;
  char tbuf1[BUFFER_LEN];
#endif
#ifdef HAVE_GETRUSAGE
  struct rusage usage;
#endif
  pid_t pid;
  int psize;

#ifdef HAVE_UPTIME
  fp = popen(UPTIME, "r");

  /* just in case the system is screwy */
  if (fp == NULL) {
    notify(player, T("Error -- cannot execute uptime."));
    do_rawlog(LT_ERR, "** ERROR ** popen for @uptime returned NULL.");
    return;
  }
  /* print system uptime */
  for (i = 0; (c = getc(fp)) != '\n' && c != EOF; i++)
    tbuf1[i] = (char) c;
  tbuf1[i] = '\0';
  pclose(fp);

  notify(player, tbuf1);
#endif /* HAVE_UPTIME */

  /* do process stats */

  pid = getpid();
  psize = mush_getpagesize();
  notify_format(player, "\nProcess ID:  %10u        %10d bytes per page", pid,
                psize);

#ifdef HAVE_GETRUSAGE
  getrusage(RUSAGE_SELF, &usage);
  notify_format(player, "Time used:   %10ld user   %10ld sys",
                (long) usage.ru_utime.tv_sec, (long) usage.ru_stime.tv_sec);
  notify_format(player, "Max res mem: %10ld pages  %10ld bytes",
                usage.ru_maxrss, (usage.ru_maxrss * psize));
  notify_format(player, "Integral mem:%10ld shared %10ld private %10ld stack",
                usage.ru_ixrss, usage.ru_idrss, usage.ru_isrss);
  notify_format(player,
                "Page faults: %10ld hard   %10ld soft    %10ld swapouts",
                usage.ru_majflt, usage.ru_minflt, usage.ru_nswap);
  notify_format(player, "Disk I/O:    %10ld reads  %10ld writes",
                usage.ru_inblock, usage.ru_oublock);
  notify_format(player, "Network I/O: %10ld in     %10ld out", usage.ru_msgrcv,
                usage.ru_msgsnd);
  notify_format(player, "Context swi: %10ld vol    %10ld forced",
                usage.ru_nvcsw, usage.ru_nivcsw);
  notify_format(player, "Signals:     %10ld", usage.ru_nsignals);
#endif /* HAVE_GETRUSAGE */
#endif
}

static void
win32_uptime(dbref player __attribute__((__unused__)))
{ /* written by NJG */
#ifdef WIN32
  MEMORYSTATUS memstat;
  double mem;
  memstat.dwLength = sizeof(memstat);
  GlobalMemoryStatus(&memstat);
  notify(player, "---------- Windows memory usage ------------");
  notify_format(player, "%10ld %% memory in use", memstat.dwMemoryLoad);
  mem = memstat.dwAvailPhys / 1024.0 / 1024.0;
  notify_format(player, "%10.3f Mb free physical memory", mem);
  mem = memstat.dwTotalPhys / 1024.0 / 1024.0;
  notify_format(player, "%10.3f Mb total physical memory", mem);
  mem = memstat.dwAvailPageFile / 1024.0 / 1024.0;
  notify_format(player, "%10.3f Mb available in the paging file ", mem);
  mem = memstat.dwTotalPageFile / 1024.0 / 1024.0;
  notify_format(player, "%10.3f Mb total paging file size", mem);
#endif
}

enum uptime_type {
  UPTIME_UPSINCE,
  UPTIME_REBOOT,
  UPTIME_LAST_SAVE,
  UPTIME_NEXT_SAVE,
  UPTIME_DBCK,
  UPTIME_PURGE,
  UPTIME_WARNING
};
/* ARGSUSED */
FUNCTION(fun_uptime)
{
  enum uptime_type which = UPTIME_UPSINCE;

  if (args[0] && *args[0]) {
    if (strcasecmp("upsince", args[0]) == 0)
      which = UPTIME_UPSINCE;
    else if (strcasecmp("reboot", args[0]) == 0)
      which = UPTIME_REBOOT;
    else if (strcasecmp("save", args[0]) == 0)
      which = UPTIME_LAST_SAVE;
    else if (strcasecmp("nextsave", args[0]) == 0)
      which = UPTIME_NEXT_SAVE;
    else if (strcasecmp("dbck", args[0]) == 0)
      which = UPTIME_DBCK;
    else if (strcasecmp("purge", args[0]) == 0)
      which = UPTIME_PURGE;
    else if (strcasecmp("warnings", args[0]) == 0)
      which = UPTIME_WARNING;
    else {
      safe_str("#-1", buff, bp);
      return;
    }
  }

  switch (which) {
  case UPTIME_UPSINCE:
    safe_integer(globals.first_start_time, buff, bp);
    break;
  case UPTIME_REBOOT:
    safe_integer(globals.start_time, buff, bp);
    break;
  case UPTIME_LAST_SAVE:
    if (globals.last_dump_time > 0)
      safe_integer(globals.last_dump_time, buff, bp);
    else
      safe_str("-1", buff, bp);
    break;
  case UPTIME_NEXT_SAVE:
    safe_integer(options.dump_counter, buff, bp);
    break;
  case UPTIME_DBCK:
    safe_integer(options.dbck_counter, buff, bp);
    break;
  case UPTIME_PURGE:
    safe_integer(options.purge_counter, buff, bp);
    break;
  case UPTIME_WARNING:
    if (options.warn_interval)
      safe_integer(options.warn_counter, buff, bp);
    else
      safe_str("-1", buff, bp);
    break;
  }
}

/** Report on server uptime.
 * \verbatim
 * This command implements @uptime.
 * \endverbatim
 * \param player the enactor.
 * \param mortal if 1, show mortal display, even if player is privileged.
 */
void
do_uptime(dbref player, int mortal)
{
  char tbuf1[BUFFER_LEN];
  struct tm *when;
  ldiv_t secs;

  when = localtime(&globals.first_start_time);
  strftime(tbuf1, sizeof tbuf1, "%a %b %d %X %Z %Y", when);
  notify_format(player, "%13s: %s", T("Up since"), tbuf1);

  when = localtime(&globals.start_time);
  strftime(tbuf1, sizeof tbuf1, "%a %b %d %X %Z %Y", when);
  notify_format(player, "%13s: %s", T("Last reboot"), tbuf1);

  notify_format(player, "%13s: %d", T("Total reboots"), globals.reboot_count);

  when = localtime(&mudtime);
  strftime(tbuf1, sizeof tbuf1, "%a %b %d %X %Z %Y", when);
  notify_format(player, "%13s: %s", T("Time now"), tbuf1);

  if (globals.last_dump_time > 0) {
    when = localtime(&globals.last_dump_time);
    strftime(tbuf1, sizeof tbuf1, "%a %b %d %X %Z %Y", when);
    notify_format(player, "%29s: %s", T("Time of last database save"), tbuf1);
  }

  /* calculate times until various events */
  when = localtime(&options.dump_counter);
  strftime(tbuf1, sizeof tbuf1, "%X", when);
  secs = ldiv((long) difftime(options.dump_counter, mudtime), 60);
  notify_format(player, T("%29s: %ld minutes %ld seconds, at %s."),
                T("Time until next database save"), secs.quot, secs.rem, tbuf1);

  when = localtime(&options.dbck_counter);
  strftime(tbuf1, sizeof tbuf1, "%X", when);
  secs = ldiv((long) difftime(options.dbck_counter, mudtime), 60);
  notify_format(player, T("%29s: %ld minutes %ld seconds, at %s."),
                T("Time until next dbck check"), secs.quot, secs.rem, tbuf1);

  when = localtime(&options.purge_counter);
  strftime(tbuf1, sizeof tbuf1, "%X", when);
  secs = ldiv((long) difftime(options.purge_counter, mudtime), 60);
  notify_format(player, T("%29s: %ld minutes %ld seconds, at %s."),
                T("Time until next purge"), secs.quot, secs.rem, tbuf1);

  if (options.warn_interval) {
    when = localtime(&options.warn_counter);
    strftime(tbuf1, sizeof tbuf1, "%X", when);
    secs = ldiv((long) difftime(options.warn_counter, mudtime), 60);
    notify_format(player, T("%29s: %ld minutes %ld seconds, at %s."),
                  T("Time until next @warnings"), secs.quot, secs.rem, tbuf1);
  }

  {
    /* 86400 == seconds in 1 day. 3600 == seconds in 1 hour */
    long days;
    ldiv_t hours, mins;

    secs = ldiv((long) difftime(mudtime, globals.first_start_time), 86400);
    days = secs.quot;
    hours = ldiv(secs.rem, 3600);
    mins = ldiv(hours.rem, 60);

    notify_format(
      player, T("PennMUSH Uptime: %ld days %ld hours %ld minutes %ld seconds"),
      days, hours.quot, mins.quot, mins.rem);
  }
  /* Mortals, go no further! */
  if (!Wizard(player) || mortal)
    return;

#if defined(linux)
  linux_uptime(player);
#elif defined(WIN32)
  win32_uptime(player);
#else
  unix_uptime(player);
#endif

  if (God(player))
    notify_activity(player, 0, 0);
}

/* Open a db file, which may be compressed, and return a file pointer. These
 probably should be moved into db.c or
 a new dbio.c */
static PENNFILE *
db_open(const char *fname)
{
  PENNFILE *pf;
  sqlite3_str *fstr;
  char *filename;

  fstr = sqlite3_str_new(NULL);
  sqlite3_str_appendall(fstr, fname);
  sqlite3_str_appendall(fstr, options.compresssuff);
  filename = sqlite3_str_finish(fstr);

  pf = mush_malloc(sizeof *pf, "pennfile");

#ifdef HAVE_LIBZ
  if (*options.uncompressprog &&
      strcmp(options.uncompressprog, "gunzip") == 0) {
    pf->type = PFT_GZFILE;
    pf->handle.g = gzopen(filename, "rb");
    if (!pf->handle.g) {
      do_rawlog(LT_ERR, "Unable to open %s with libz: %s\n", filename,
                strerror(errno));
      sqlite3_free(filename);
      mush_free(pf, "pennfile");
      longjmp(db_err, 1);
    }
#ifdef HAVE_GZBUFFER
    gzbuffer(pf->handle.g,
             1024 * 64); /* Large buffer to speed up decompression */
#endif
    sqlite3_free(filename);
    return pf;
  }
#endif

#ifndef WIN32
  if (*options.uncompressprog) {
    pf->type = PFT_PIPE;
    /* We do this because on some machines (SGI Irix, for example),
     * the popen will not return NULL if the mailfile isn't there.
     */

    if (access(filename, R_OK) == 0) {
      char *prog;
      fstr = sqlite3_str_new(NULL);
      sqlite3_str_appendf(fstr, "%s < '%s'", options.uncompressprog, filename);
      prog = sqlite3_str_finish(fstr);
      pf->handle.f = popen(prog, "r");
      sqlite3_free(prog);
      /* Force the pipe to be fully buffered */
      if (pf->handle.f) {
        setvbuf(pf->handle.f, NULL, _IOFBF, 1024 * 32);
      } else {
        do_rawlog(LT_ERR, "Unable to run '%s < %s': %s", options.uncompressprog,
                  filename, strerror(errno));
      }
      sqlite3_free(filename);
    } else {
      sqlite3_free(filename);
      mush_free(pf, "pennfile");
      longjmp(db_err, 1);
    }
  } else
#endif /* WIN32 */
  {
    pf->type = PFT_FILE;
    pf->handle.f = fopen(filename, FOPEN_READ);
    if (!pf->handle.f) {
      do_rawlog(LT_ERR, "Unable to open %s: %s\n", filename, strerror(errno));
    } else {
      setvbuf(pf->handle.f, NULL, _IOFBF, 1024 * 32);
#ifdef HAVE_POSIX_FADVISE
      posix_fadvise(fileno(pf->handle.f), 0, 0, POSIX_FADV_SEQUENTIAL);
#endif
    }
    sqlite3_free(filename);
  }
  if (!pf->handle.f) {
    mush_free(pf, "pennfile");
    longjmp(db_err, 1);
  }
  return pf;
}

/* Open a file or pipe (if compressing) for writing */
static PENNFILE *
db_open_write(const char *fname)
{
  PENNFILE *pf;
  sqlite3_str *fstr;
  char workdir[BUFFER_LEN], *filename;

  fstr = sqlite3_str_new(NULL);
  sqlite3_str_appendall(fstr, fname);
  sqlite3_str_appendall(fstr, options.compresssuff);
  filename = sqlite3_str_finish(fstr);

/* Be safe in case our game directory was removed and restored,
 * in which case our inode is screwy
 */
#ifdef WIN32
  if (GetCurrentDirectory(BUFFER_LEN, workdir)) {
    if (SetCurrentDirectory(workdir) < 0)
#else
  if (getcwd(workdir, BUFFER_LEN)) {
    if (chdir(workdir) < 0)
#endif
      do_rawlog(LT_ERR, "chdir to %s failed in db_open_write, errno %d (%s)",
                workdir, errno, strerror(errno));
  } else {
    /* If this fails, we probably can't write to a log, either, though */
    fprintf(stderr, "getcwd failed during db_open_write, errno %d (%s)\n",
            errno, strerror(errno));
  }

  pf = mush_malloc(sizeof *pf, "pennfile");

#ifdef HAVE_LIBZ
  if (*options.compressprog && strcmp(options.compressprog, "gzip") == 0) {
    pf->type = PFT_GZFILE;
    pf->handle.g = gzopen(filename, "wb");
    if (!pf->handle.g) {
      do_rawlog(LT_ERR, "Unable to open %s with libz: %s\n", filename,
                strerror(errno));
      sqlite3_free(filename);
      mush_free(pf, "pennfile");
      longjmp(db_err, 1);
    }
#ifdef HAVE_GZBUFFER
    gzbuffer(pf->handle.g, 1024 * 64);
#endif
    sqlite3_free(filename);
    return pf;
  }
#endif

#ifndef WIN32
  if (*options.compressprog) {
    char *prog;
    pf->type = PFT_PIPE;
    fstr = sqlite3_str_new(NULL);
    sqlite3_str_appendf(fstr, "%s > '%s'", options.compressprog, filename);
    prog = sqlite3_str_finish(fstr);
    pf->handle.f = popen(prog, "w");
    sqlite3_free(prog);
    /* Force the pipe to be fully buffered */
    if (pf->handle.f) {
      setvbuf(pf->handle.f, NULL, _IOFBF, 1024 * 32);
    } else {
      do_rawlog(LT_ERR, "Unable to run '%s > %s': %s", options.compressprog,
                filename, strerror(errno));
    }
    sqlite3_free(filename);
  } else
#endif /* WIN32 */
  {
    pf->type = PFT_FILE;
    pf->handle.f = fopen(filename, "wb");
    if (!pf->handle.f) {
      do_rawlog(LT_ERR, "Unable to open %s: %s\n", filename, strerror(errno));
    }
    sqlite3_free(filename);
  }
  if (!pf->handle.f) {
    mush_free(pf, "pennfile");
    longjmp(db_err, 1);
  } else {
    setvbuf(pf->handle.f, NULL, _IOFBF, 1024 * 32);
  }
  return pf;
}

extern HASHTAB htab_function;
extern HASHTAB htab_user_function;
extern HASHTAB htab_reserved_aliases;
extern HASHTAB help_files;
extern HASHTAB htab_locks;
extern HASHTAB local_options;
extern StrTree atr_names;
extern StrTree lock_names;
extern StrTree object_names;
extern PTAB ptab_command;
extern PTAB ptab_attrib;
extern PTAB ptab_flag;
extern intmap *queue_map, *descs_by_fd;
#ifdef HAVE_INOTIFY_INIT1
extern intmap *watchtable;
#endif

static void
list_sqlite3_stats(dbref player, const char *name, sqlite3 *db)
{
#ifdef SQLITE_ENABLE_STMTVTAB
  sqlite3_stmt *statter;
  statter = prepare_statement(
    db,
    "SELECT sql, nscan, nsort, naidx, nstep, reprep, run, mem FROM sqlite_stmt",
    "list.memstats");
  if (statter) {
    int status;
    notify_format(player, "Prepared query stats for %s database", name);
    notify_format(player, "%-30s %6s %5s %5s %9s %6s %7s %6s", "SQL", "nscan",
                  "nsort", "naidx", "nstep", "reprep", "run", "memory");
    do {
      status = sqlite3_step(statter);
      if (status == SQLITE_ROW) {
        int nscan, nsort, naidx, nstep, reprep, run, mem;
        const char *query;
        query = (const char *) sqlite3_column_text(statter, 0);
        if (strstr(query, "sqlite_stmt")) {
          continue;
        }
        nscan = sqlite3_column_int(statter, 1);
        nsort = sqlite3_column_int(statter, 2);
        naidx = sqlite3_column_int(statter, 3);
        nstep = sqlite3_column_int(statter, 4);
        reprep = sqlite3_column_int(statter, 5);
        run = sqlite3_column_int(statter, 6);
        mem = sqlite3_column_int(statter, 7);

        notify_format(player, "%-30.30s %6d %5d %5d %9d %6d %7d %6d", query,
                      nscan, nsort, naidx, nstep, reprep, run, mem);
      }
    } while (status == SQLITE_ROW || is_busy_status(status));
    sqlite3_reset(statter);
  }
#endif
}

/** Reports stats on various in-memory data structures.
 * \param player the enactor.
 */
void
do_list_memstats(dbref player)
{
  static const struct {
    const HASHTAB *const table;
    const char *name;
  } hash_tables[] = {
    {&htab_function, "Functions"},
    {&htab_user_function, "@Functions"},
    {&htab_reserved_aliases, "Aliases"},
    {&help_files, "HelpFiles"},
    {&htab_locks, "@locks"},
    {&local_options, "ConfigOpts"},
  };
  unsigned int i;
  int64_t sqlmem;

  notify(player, "Hash Tables:");
  notify(player,
         "Table       Buckets Entries 1Lookup 2Lookup 3Lookup ~Memory KeySize");
  for (i = 0; i < sizeof(hash_tables) / sizeof(hash_tables[0]); ++i) {
    const HASHTAB *htab = hash_tables[i].table;
    struct hashstats stats;

    hash_stats(htab, &stats);
    notify_format(player, "%-11s %7d %7d %7d %7d %7d %7d %7.1f",
                  hash_tables[i].name, htab->hashsize, htab->entries,
                  stats.lookups[0], stats.lookups[1], stats.lookups[2],
                  stats.bytes, stats.key_length);
    if (stats.entries != htab->entries) {
      notify_format(player, "Mismatch in size: %d expected, %d found!",
                    htab->entries, stats.entries);
    }
  }

  notify(player, "Prefix Trees:");
  ptab_stats_header(player);
  ptab_stats(player, &ptab_attrib, "AttrPerms");
  ptab_stats(player, &ptab_command, "Commands");
  ptab_stats(player, &ptab_flag, "Flags");
  notify(player, "String Trees:");
  st_stats_header(player);
  st_stats(player, &atr_names, "AttrNames");
  st_stats(player, &object_names, "ObjNames");
  st_stats(player, &lock_names, "LockNames");
  notify(player, "Integer Maps:");
  im_stats_header(player);
  im_stats(player, queue_map, "Queue IDs");
  im_stats(player, descs_by_fd, "Connections");
#ifdef HAVE_INOTIFY_INIT1
  im_stats(player, watchtable, "Inotify");
#endif

  notify(player, "Sqlite3 Databases:");
  sqlmem = sqlite3_memory_used();
  notify_format(player, " Using %ld megabytes and %ld kilobytes of memory.",
                (long) (sqlmem / (1024 * 1024)),
                (long) ((sqlmem % (1024 * 1024)) / 1024));
  if (Wizard(player)) {
    extern sqlite3 *help_db, *connlog_db;
    list_sqlite3_stats(player, "temporary", get_shared_db());
    list_sqlite3_stats(player, "help", help_db);
    if (options.use_connlog) {
      list_sqlite3_stats(player, "connlog", connlog_db);
    }
  }

#ifdef COMP_STATS
  if (Wizard(player) && strcmp(options.attr_compression, "word") == 0) {
    long items, used, total_comp, total_uncomp;
    float percent;
    compress_stats(&items, &used, &total_uncomp, &total_comp);
    notify(player, "---------- Internal attribute compression  ----------");
    notify_format(player,
                  "%10ld compression table items used, "
                  "taking %ld bytes.",
                  items, used);
    notify_format(player, "%10ld bytes in text before compression. ",
                  total_uncomp);
    notify_format(player, "%10ld bytes in text AFTER  compression. ",
                  total_comp);
    percent = ((float) (total_comp)) / ((float) total_uncomp) * 100.0;
    notify_format(player,
                  "%10.0f %% text    compression ratio (lower is better). ",
                  percent);
    percent = ((float) (total_comp + used + (32768L * sizeof(char *)))) /
              ((float) total_uncomp) * 100.0;
    notify_format(player,
                  "%10.0f %% OVERALL compression ratio (lower is better). ",
                  percent);
    notify_format(player,
                  "          (Includes table items, and table of words "
                  "pointers of %ld bytes)",
                  (long) (32768 * sizeof(char *)));
    if (percent >= 100.0)
      notify(player, "          "
                     "(Compression ratio improves with larger database)");
  }
#endif
}

static char *
make_new_epoch_file(const char *basename, int the_epoch)
{
  static char result[BUFFER_LEN]; /* STATIC! */
  /* Unlink the last the_epoch and create a new one */
  snprintf(result, sizeof result, "%s.#%d#", basename, the_epoch - 1);
  unlink(result);
  snprintf(result, sizeof result, "%s.#%d#", basename, the_epoch);
  return result;
}

/* Given a list of dbrefs on which a command has matched but been
 * denied by a lock, queue up the COMMAND`*FAILURE attributes, if
 * any.
 */
static int
fail_commands(dbref player)
{
  dbref *obj = errdblist;
  int size = errdbtail - errdblist;
  int matched = 0;
  while (size--) {
    matched += fail_lock(player, *obj, Command_Lock, NULL, NOTHING);
    obj++;
  }
  errdbtail = errdblist;
  return (matched > 0);
}

/* Increase the size of the errdblist - up to some maximum */
static void
errdb_grow(void)
{
  dbref *newerrdb;
  if (errdbsize >= 50)
    return; /* That's it, no more, forget it */
  newerrdb =
    mush_realloc(errdblist, (errdbsize + 1) * sizeof(dbref), "errdblist");
  if (newerrdb) {
    errdblist = newerrdb;
    errdbtail = errdblist + errdbsize;
    errdbsize += 1;
  }
}
