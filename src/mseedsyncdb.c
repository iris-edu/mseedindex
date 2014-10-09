/***************************************************************************
 * mseedsyncdb.c - Synchronize Mini-SEED with database schema
 *
 * Opens user specified file(s), parses the Mini-SEED records and
 * synchronizes time series summary with PostgreSQL database schema.
 *
 * The time series are grouped by NSLCQ records that are conterminous
 * in a given file.  Each resulting row in the database represents the
 * earliest and latest time, the number of contiguous segments and the
 * total gap (non-coverage) in seconds from each group.
 *
 * The logic to determine contiguous segments and gap (non-coverage)
 * is expecting the data to be organized into segments ordered by
 * increasing start time.
 * 
 * Expected database schema:
 *
 * Field Name   Type
 * ------------ ------------
 * id           serial (auto incrementing)
 * nslcq	ltree
 * timerange	tstzrange   [timestamps with time zone]
 * samplerate	numeric(10,6)
 * filename	character varying(256)
 * offset	numeric(15,0)
 * bytes	numeric(15,0)
 * hash		character varying(64)
 * segments     integer
 * gapseconds   real
 * updated	timestamp with time zone
 * scanned	timestamp with time zone
 *
 * In general critical error messages are prefixed with "ERROR:" and
 * the return code will be 1.  On successfull operation the return
 * code will be 0.
 *
 * Written by Chad Trabant, IRIS Data Management Center.
 *
 * modified 2014.281
 ***************************************************************************/

// Enforce increasing version number for data files?

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <ctype.h>
#include <regex.h>

#include <libpq-fe.h>

#include <libmseed.h>

#include "md5.h"

#define VERSION "0.4"
#define PACKAGE "mseedsyncdb"

static int     retval       = 0;
static flag    verbose      = 0;
static double  timetol      = -1.0; /* Time tolerance for continuous traces */
static double  sampratetol  = -1.0; /* Sample rate tolerance for continuous traces */
static flag    nosync       = 0;    /* Control synchronization with database, 1 = no database */

static PGconn *dbconn       = NULL; /* Database connection */
static flag    dbconntrace  = 0;    /* Trace database interactions, for debugging */
static char   *dbhost       = "dbserv3";
static char   *dbport       = "5444";
static char   *dbname       = "iris";
static char   *dbuser       = "timeseries";
static char   *dbpass       = "timeseries";
static char   *dbtable      = "timeseries.tsextents"; /* With schema */

struct segdetails {
  int64_t startoffset;
  int64_t endoffset;
  hptime_t earliest;
  hptime_t latest;
  int32_t segments;
  double gapseconds;
  md5_state_t digeststate;
};

struct filelink {
  char *filename;
  MSTraceGroup *mstg;
  struct filelink *next;
};

struct filelink *filelist = 0;
struct filelink *filelisttail = 0;

static int syncfileseries (struct filelink *flp, time_t scantime);
static PGresult *pquery (PGconn *pgdb, const char *format, ...);
void local_mst_printtracelist ( MSTraceGroup *mstg, flag timeformat );
static int processparam (int argcount, char **argvec);
static char *getoptval (int argcount, char **argvec, int argopt);
static int addfile (char *filename);
static int addlistfile (char *filename);
static void usage (void);

int
main (int argc, char **argv)
{
  PGresult *result = NULL;
  struct segdetails *sd = NULL;
  struct filelink *flp = NULL;
  MSRecord *msr = NULL;
  MSTrace *mst = NULL;
  MSTrace *cmst = NULL;
  hptime_t endtime = HPTERROR;
  int retcode = MS_NOERROR;
  time_t scantime;
  
  off_t filepos = 0;
  off_t prevfilepos = 0;
  
  flag whence = 0;
  
  /* Set default error message prefix */
  ms_loginit (NULL, NULL, NULL, "ERROR: ");
  
  /* Process given parameters (command line and parameter file) */
  if ( processparam (argc, argv) < 0 )
    return 1;
  
  if ( ! nosync )
    {
      const char *keywords[7];
      const char *values[7];
      
      keywords[0] = "host";
      values[0] = dbhost;
      keywords[1] = "port";
      values[1] = dbport;
      keywords[2] = "user";
      values[2] = dbuser;
      keywords[3] = "password";
      values[3] = dbpass;
      keywords[4] = "dbname";
      values[4] = dbname;
      keywords[5] = "fallback_application_name";
      values[5] = PACKAGE;
      keywords[6] = NULL;
      values[6] = NULL;
      
      dbconn = PQconnectdbParams(keywords, values, 1);
      
      if ( ! dbconn )
	{
	  ms_log (2, "PQconnectdb returned NULL, connection failed");
	  exit (1);
	}

      if ( dbconntrace )
	{
          PQtrace (dbconn, stderr);
        }
      
      if ( PQstatus(dbconn) != CONNECTION_OK )
	{
	  ms_log (2, "Connection to database failed: %s", PQerrorMessage(dbconn));
	  PQfinish (dbconn);
	  exit (1);
	}
      
      if ( verbose )
	{
	  int sver = PQserverVersion(dbconn);
	  int major, minor, less;
	  major = sver/10000;
	  minor = sver/100 - major*100;
	  less = sver - major*10000 - minor*100;
	  
	  ms_log (1, "Connected to database %s on host %s (server %d.%d.%d)\n",
		  PQdb(dbconn), PQhost(dbconn),
		  major, minor, less);
	}
      
      /* Set session timezone to 'UTC' */
      result = PQexec (dbconn, "SET SESSION timezone TO 'UTC'");
      if ( PQresultStatus(result) != PGRES_COMMAND_OK )
	{
	  fprintf (stderr, "SET timezone command failed: %s", PQerrorMessage(dbconn));
	  PQclear (result);
	  exit(1);
	}
      PQclear (result);
      
      if ( verbose )
	ms_log (1, "Set database session timezone to UTC\n");
    } /* End of ! nosync */
  
  flp = filelist;
  while ( flp )
    {
      if ( verbose >= 1 )
	ms_log (1, "Processing: %s\n", flp->filename);
      
      flp->mstg = mst_initgroup (flp->mstg);
      cmst = NULL;
      
      scantime = time (NULL);
      
      /* Read records from the input file */
      while ( (retcode = ms_readmsr (&msr, flp->filename, -1, &filepos,
				     NULL, 1, 0, verbose-1)) == MS_NOERROR )
	{
	  mst = NULL;
	  whence = 0;
	  endtime = msr_endtime(msr);
	  
	  if ( cmst )
	    {
	      mst = mst_findadjacent (flp->mstg, &whence, msr->dataquality,
				      msr->network, msr->station, msr->location,
				      msr->channel, msr->samprate, sampratetol,
				      msr->starttime, endtime, timetol);
	    }
	  
	  /* Exception: check for matching channel records regardless of time */
	  if ( mst != cmst )
	    {
	      mst = mst_findmatch (cmst, msr->dataquality, msr->network, msr->station,
				   msr->location, msr->channel);

	      /* If a match was found it must be a time tear relative to the current MSTrace */
	      if ( mst == cmst )
		{
		  whence = 1;
		  
		  sd = (struct segdetails *)cmst->prvtptr;
		  sd->segments++;
		  
		  /* Determine gap between the latest sample and this record if any */
		  if ( msr->starttime > sd->latest )
		    sd->gapseconds += (double) MS_HPTIME2EPOCH((msr->starttime - sd->latest));
		}
	    }
	  
	  /* Update details of current MSTrace if current record is an extension */
	  if ( mst == cmst && whence == 1 && filepos == (prevfilepos + msr->reclen) )
	    {
	      if ( msr->samplecnt > 0 )
		mst_addmsr (cmst, msr, 1);
	      
	      sd = (struct segdetails *)cmst->prvtptr;
	      sd->endoffset = filepos + msr->reclen - 1;
	      
	      /* Maintain earliest and latest time stamps */
	      if ( msr->starttime < sd->earliest )
		sd->earliest = msr->starttime;
	      if ( endtime > sd->latest )
		sd->latest = endtime;
	      
	      md5_append (&(sd->digeststate), (const md5_byte_t *)msr->record, msr->reclen);
	    }
	  /* Otherwise create a new MSTrace */
	  else
	    {
	      /* Create & populate new current MSTrace and add it to the file MSTraceGroup */
	      cmst = mst_init (NULL);
	      mst_addtracetogroup (flp->mstg, cmst);
	      
	      strncpy (cmst->network, msr->network, sizeof(cmst->network));
	      strncpy (cmst->station, msr->station, sizeof(cmst->station));
	      strncpy (cmst->location, msr->location, sizeof(cmst->location));
	      strncpy (cmst->channel, msr->channel, sizeof(cmst->channel));
	      cmst->dataquality = msr->dataquality;
	      cmst->starttime = msr->starttime;
	      cmst->endtime = endtime;
	      cmst->samprate = msr->samprate;
	      cmst->samplecnt = msr->samplecnt;
	      
	      if ( ! (sd = calloc (1, sizeof (struct segdetails))) )
		{
		  ms_log (2, "Cannot allocate segment details\n");
		  return 1;
		}
	      
	      cmst->prvtptr = sd;
	      
	      sd->startoffset = filepos;
	      sd->endoffset = filepos + msr->reclen - 1;
	      sd->earliest = msr->starttime;
	      sd->latest = endtime;
	      sd->segments = 1;
	      sd->gapseconds = 0.0;
	      
	      memset (&(sd->digeststate), 0, sizeof(md5_state_t));
 	      md5_init (&(sd->digeststate));
	      md5_append (&(sd->digeststate), (const md5_byte_t *)msr->record, msr->reclen);
	    }
	  
	  prevfilepos = filepos;
	}
      
      /* Print error if not EOF */
      if ( retcode != MS_ENDOFFILE )
	{
	  ms_log (2, "Cannot read %s: %s\n", flp->filename, ms_errorstr(retcode));
	  ms_readmsr (&msr, NULL, 0, NULL, NULL, 0, 0, 0);
	  exit (1);
	}
      
      /* Make sure everything is cleaned up */
      ms_readmsr (&msr, NULL, 0, NULL, NULL, 0, 0, 0);

      /* Print segments for verbose output */
      if ( verbose >= 2 )
        {
          ms_log (1, "Segment list to synchronize for %s\n", flp->filename);
	  local_mst_printtracelist (flp->mstg, 1);
	}
      
      /* Sync time series listing */
      if ( syncfileseries (flp, scantime) )
	{
	  ms_log (2, "Error synchronizing time series for %s with database\n", flp->filename);
	  exit (1);
	}
      
      flp = flp->next;
    } /* End of looping over file list */
  
  if ( dbconn )
    {
      if ( verbose >= 2 )
	ms_log (1, "Closing database connection to %s\n", PQhost(dbconn));

      PQfinish (dbconn);
    }
  
  return retval;
}  /* End of main() */


/***************************************************************************
 * syncfileseries():
 *
 * Synchronize the time series list associated with a file entry to
 * the database.
 *
 * Expected database schema is documented in the initial comment block.
 *
 * Returns 0 on success, and -1 on failure
 ***************************************************************************/
static int
syncfileseries (struct filelink *flp, time_t scantime)
{
  PGresult *result = NULL;
  PGresult *matchresult = NULL;
  struct segdetails *sd;
  MSTrace *mst = NULL;
  int64_t bytecount;
  char nslcq[64];
  char earliest[64];
  char latest[64];
  int64_t updated;
  char *filewhere = NULL;
  int baselength = 0;
  
  char *vp;
  char *ep = NULL;
  double version = -1.0;
  
  md5_byte_t digest[16];
  char digeststr[33];
  int idx;
  
  if ( ! flp )
    return -1;
  
  if ( verbose )
    ms_log (0, "Synchronizing segments for %s\n", flp->filename);
  
  /* Check and parse version from file */
  if ( (vp = strrchr (flp->filename, '#')) )
    {
      baselength = vp - flp->filename;
      
      version = strtod (++vp, &ep);
      
      if ( ! version && vp == ep )
	{
	  ms_log (2, "Error parsing version from %s\n", flp->filename);
	  return -1;
	}
      
      if ( verbose >= 2 )
	ms_log (1, "Parsed version %g from %s\n", version, flp->filename);
    }
  
  /* Search database for rows matching the filename or a version of the filename */
  if ( dbconn )
    {
      if ( baselength > 0 )
	asprintf (&filewhere, "filename like '%.*s%%'", baselength, flp->filename);
      else
	asprintf (&filewhere, "filename='%s'", flp->filename);
      
      if ( ! filewhere )
	{
	  ms_log (2, "Cannot allocate memory for WHERE filename clause\n", flp->filename);
	  return -1;
	}
      
      if ( verbose >= 2 )
	ms_log (1, "Searching for rows matching '%s'\n", flp->filename);
      
      matchresult = pquery (dbconn,
			    "SELECT nslcq,hash,extract (epoch from updated) "
			    "FROM %s "
			    "WHERE %s", dbtable, filewhere);
      
      if ( PQresultStatus(matchresult) != PGRES_TUPLES_OK )
	{
	  ms_log (2, "SELECT failed: %s\n", PQresultErrorMessage(matchresult));
	  PQclear (matchresult);
	  if ( filewhere )
	    free (filewhere);
	  return -1;
	}
      
      if ( verbose >= 2 )
	ms_log (1, "Found %d matching rows\n", PQntuples(matchresult));
      
      if ( PQntuples(matchresult) <= 0 )
	{
	  PQclear (matchresult);
	  matchresult = NULL;
	}
      
      /* Start a transaction block */
      result = PQexec (dbconn, "BEGIN");
      if ( PQresultStatus(result) != PGRES_COMMAND_OK )
	{
	  fprintf (stderr, "BEGIN command failed: %s", PQerrorMessage(dbconn));
	  PQclear (result);
	  if ( filewhere )
	    free (filewhere);
	  return -1;
	}
      PQclear (result);
      
      /* Delete existing rows for filename or previous version of filename */
      if ( PQntuples(matchresult) > 0 )
	{
	  result = pquery (dbconn, "DELETE FROM %s WHERE %s", dbtable, filewhere);
	  if ( PQresultStatus(result) != PGRES_COMMAND_OK )
	    {
	      fprintf (stderr, "DELETE failed: %s", PQerrorMessage(dbconn));
	      PQclear (result);
	      if ( filewhere )
		free (filewhere);
	      return -1;
	    }
	  PQclear (result);
	}
      
      free (filewhere);
    }
  
  /* Loop through trace list */
  mst = flp->mstg->traces;
  while ( mst )
    {
      sd = (struct segdetails *)mst->prvtptr;
      
      /* Calculate MD5 digest and string representation */
      md5_finish(&(sd->digeststate), digest);
      for (idx=0; idx < 16; idx++)
	sprintf (digeststr+(idx*2), "%02x", digest[idx]);
      
      bytecount = sd->endoffset - sd->startoffset + 1;
      
      /* Generate label: network.station.location.channel.quality
       * Labels can only include A-Za-z0-9_ (e.g. alphanumeric and underscore).
       * Use underscore for any empty fields. */
      snprintf (nslcq, sizeof(nslcq), "%s.%s.%s.%s.%c",
		(*(mst->network)) ? mst->network : "_",
		(*(mst->station)) ? mst->station : "_",
		(*(mst->location)) ? mst->location : "_",
		(*(mst->channel)) ? mst->channel : "_",
		(mst->dataquality) ? mst->dataquality : '_');
      
      /* Create earliest and latest time strings with UTC (-00) timezone indicators */
      ms_hptime2isotimestr (sd->earliest, earliest, 1);
      strcat (earliest, "-00");
      ms_hptime2isotimestr (sd->latest, latest, 1);
      strcat (latest, "-00");
      
      updated = (int64_t) scantime;
      
      if ( dbconn )
	{
	  int64_t updated = (int64_t) scantime;
	  
	  /* Search for matching trace entry to retain updated time if hash has not changed */
	  if ( matchresult )
	    {
	      /* Fields: 0=nslc,1=hash,2=updated */
	      for ( idx=0; idx < PQntuples(matchresult); idx++ )
		{
		  if ( ! strcmp (digeststr, PQgetvalue(matchresult, idx, 1)) )
		    {
		      if ( ! strcmp (nslcq, PQgetvalue(matchresult, idx, 0)) )
			{
			  updated = strtoll (PQgetvalue(matchresult, idx, 2), NULL, 10);
			}
		    }
		}
	    }
	  
	  /* Insert new row */
	  result = pquery (dbconn,
			   "INSERT INTO %s "
			   "(nslcq,timerange,samplerate,filename,byteoffset,bytes,hash,segments,gapseconds,updated,scanned) "
			   "VALUES "
			   "('%s','[%s,%s]',"
			   "%.6g,'%s',%lld,%lld,'%s',%lld,%.6g,"
			   "to_timestamp(%lld),to_timestamp(%lld))",
			   dbtable,
			   nslcq,
			   earliest, latest,
			   mst->samprate, flp->filename, sd->startoffset, bytecount, digeststr,
			   sd->segments, sd->gapseconds,
			   (long long int) updated, (long long int) scantime
			   );
	  
	  if ( PQresultStatus(result) != PGRES_COMMAND_OK )
	    {
	      fprintf (stderr, "INSERT failed: %s\n", PQresultErrorMessage(result));
	      PQclear (result);
	      return -1;
	    }
	  PQclear (result);
	}
      
      /* Print trace line when verbose >=2 or when verbose and nosync */
      if ( verbose >= 2 || (verbose && nosync) )
	{
	  ms_log (0, "%s|%s|%s|%.10g|%s|%lld|%lld|%s|%lld|%.6g|%lld|%lld\n",
		  nslcq, earliest, latest, mst->samprate, flp->filename,
		  sd->startoffset, bytecount, digeststr,
		  sd->segments, sd->gapseconds,
		  (long long int) updated, (long long int) scantime);
	}
      
      mst = mst->next;
    }
  
  /* End the transaction */
  if ( dbconn )
    {
      result = PQexec (dbconn, "END");
      PQclear(result);
      
      if ( matchresult )
	PQclear (matchresult);
    }
  
  return 0;
}  /* End of syncfileseries() */


/***************************************************************************
 * pquery():
 *
 * Execute a query to the Postgres DB connection at 'pgdb'.
 *
 * Returns PGresult on success and NULL on failure
 ***************************************************************************/
static PGresult *
pquery (PGconn *pgdb, const char *format, ...)
{
  va_list argv;
  char *query = NULL;
  PGresult *result = NULL;
  int length;
  
  va_start (argv, format);
  length = vasprintf (&query, format, argv);
  va_end (argv);
  
  if ( ! query || length <= 0 )
    {
      ms_log (2, "Cannot create query string\n");
      return NULL;
    }
  
  if ( verbose >= 2 )
    fprintf (stderr, "QUERY(%d): '%s'\n", length, query);
  
  result = PQexec (pgdb, query);
  
  if ( query )
    free (query);
  
  return result;
}  /* End of pquery() */


/***************************************************************************
 * local_mst_printtracelist:
 *
 * Print trace list summary information for the specified MSTraceGroup.
 *
 * The timeformat flag can either be:
 * 0 : SEED time format (year, day-of-year, hour, min, sec)
 * 1 : ISO time format (year, month, day, hour, min, sec)
 * 2 : Epoch time, seconds since the epoch
 ***************************************************************************/
void
local_mst_printtracelist ( MSTraceGroup *mstg, flag timeformat )
{
  struct segdetails *sd;
  MSTrace *mst = 0;
  char srcname[50];
  char stime[30];
  char etime[30];
  
  if ( ! mstg )
    {
      return;
    }
  
  mst = mstg->traces;
  
  /* Print out header */
  ms_log (0, "   Source                Earliest sample          Latest sample          Hz  Segments  GapSeconds\n");
  
  while ( mst )
    {
      mst_srcname (mst, srcname, 1);
      sd = (struct segdetails *)mst->prvtptr;
      
      /* Create formatted time strings */
      if ( timeformat == 2 )
	{
	  snprintf (stime, sizeof(stime), "%.6f", (double) MS_HPTIME2EPOCH(sd->earliest) );
	  snprintf (etime, sizeof(etime), "%.6f", (double) MS_HPTIME2EPOCH(sd->latest) );
	}
      else if ( timeformat == 1 )
	{
	  if ( ms_hptime2isotimestr (sd->earliest, stime, 1) == NULL )
	    ms_log (2, "Cannot convert earliest time for %s\n", srcname);
	  
	  if ( ms_hptime2isotimestr (sd->latest, etime, 1) == NULL )
	    ms_log (2, "Cannot convert latest time for %s\n", srcname);
	}
      else
	{
	  if ( ms_hptime2seedtimestr (sd->earliest, stime, 1) == NULL )
	    ms_log (2, "Cannot convert earliest time for %s\n", srcname);
	  
	  if ( ms_hptime2seedtimestr (sd->latest, etime, 1) == NULL )
	    ms_log (2, "Cannot convert latest time for %s\n", srcname);
	}
      
      /* Print trace info */
      ms_log (0, "%-17s %-24s %-24s  %-3.3g %-9lld %-g\n",
	      srcname, stime, etime, mst->samprate, sd->segments, sd->gapseconds);
      
      mst = mst->next;
    }
}  /* End of local_mst_printtracelist() */


/***************************************************************************
 * parameter_proc():
 * Process the command line parameters.
 *
 * Returns 0 on success, and -1 on failure
 ***************************************************************************/
static int
processparam (int argcount, char **argvec)
{
  int optind;
  char *tptr;
  
  /* Process all command line arguments */
  for (optind = 1; optind < argcount; optind++)
    {
      if (strcmp (argvec[optind], "-V") == 0)
	{
	  ms_log (1, "%s version: %s\n", PACKAGE, VERSION);
	  exit (0);
	}
      else if (strcmp (argvec[optind], "-h") == 0)
	{
	  usage();
	  exit (0);
	}
      else if (strncmp (argvec[optind], "-v", 2) == 0)
	{
	  verbose += strspn (&argvec[optind][1], "v");
	}
      else if (strncmp (argvec[optind], "-ns", 3) == 0)
	{
	  nosync = 1;
	}
      else if (strncmp (argvec[optind], "-table", 6) == 0)
	{
	  dbtable = strdup (getoptval(argcount, argvec, optind++));
	}
      else if (strncmp (argvec[optind], "-dbhost", 7) == 0)
	{
	  dbhost = strdup (getoptval(argcount, argvec, optind++));
	}
      else if (strncmp (argvec[optind], "-dbport", 7) == 0)
	{
	  dbport = strdup (getoptval(argcount, argvec, optind++));
	}
      else if (strncmp (argvec[optind], "-dbuser", 7) == 0)
	{
	  dbuser = strdup (getoptval(argcount, argvec, optind++));
	}
      else if (strncmp (argvec[optind], "-dbpass", 7) == 0)
	{
	  dbpass = strdup (getoptval(argcount, argvec, optind++));
	}
      else if (strncmp (argvec[optind], "-TRACE", 6) == 0)
        {
          dbconntrace = 1;
        }
      else if (strcmp (argvec[optind], "-tt") == 0)
	{
	  timetol = strtod (getoptval(argcount, argvec, optind++), NULL);
	}
      else if (strcmp (argvec[optind], "-rt") == 0)
	{
	  sampratetol = strtod (getoptval(argcount, argvec, optind++), NULL);
	}
      else if (strncmp (argvec[optind], "-", 1) == 0 &&
	       strlen (argvec[optind]) > 1 )
	{
	  ms_log (2, "Unknown option: %s\n", argvec[optind]);
	  exit (1);
	}
      else
	{
	  tptr = argvec[optind];
	  
          /* Check for an input file list */
          if ( tptr[0] == '@' )
            {
              if ( addlistfile (tptr+1) < 0 )
                {
                  ms_log (2, "Error adding list file %s", tptr+1);
                  exit (1);
                }
            }
          /* Otherwise this is an input file */
          else
            {
              /* Add file to global file list */
              if ( addfile (tptr) )
                {
                  ms_log (2, "Error adding file to input list %s", tptr);
                  exit (1);
                }
            }
	}
    }
  
  /* Make sure input files were specified */
  if ( filelist == 0 )
    {
      ms_log (2, "No input files were specified\n\n");
      ms_log (1, "%s version %s\n\n", PACKAGE, VERSION);
      ms_log (1, "Try %s -h for usage\n", PACKAGE);
      exit (1);
    }
  
  /* Report the program version */
  if ( verbose )
    ms_log (1, "%s version: %s\n", PACKAGE, VERSION);
  
  return 0;
}  /* End of parameter_proc() */


/***************************************************************************
 * getoptval:
 * Return the value to a command line option; checking that the value is 
 * itself not an option (starting with '-') and is not past the end of
 * the argument list.
 *
 * argcount: total arguments in argvec
 * argvec: argument list
 * argopt: index of option to process, value is expected to be at argopt+1
 *
 * Returns value on success and exits with error message on failure
 ***************************************************************************/
static char *
getoptval (int argcount, char **argvec, int argopt)
{
  if ( argvec == NULL || argvec[argopt] == NULL ) {
    ms_log (2, "getoptval(): NULL option requested\n");
    exit (1);
    return 0;
  }
  
  /* Special case of '-o -' usage */
  if ( (argopt+1) < argcount && strcmp (argvec[argopt], "-o") == 0 )
    if ( strcmp (argvec[argopt+1], "-") == 0 )
      return argvec[argopt+1];
    
  if ( (argopt+1) < argcount && *argvec[argopt+1] != '-' )
    return argvec[argopt+1];
  
  ms_log (2, "Option %s requires a value, try -h for usage\n", argvec[argopt]);
  exit (1);
  return 0;
}  /* End of getoptval() */


/***************************************************************************
 * addfile:
 *
 * Add file to end of the global file list (filelist).
 *
 * Returns 0 on success and -1 on error.
 ***************************************************************************/
static int
addfile (char *filename)
{
  struct filelink *newlp;
  
  if ( ! filename )
    {
      ms_log (2, "addfile(): No file name specified\n");
      return -1;
    }
  
  if ( ! (newlp = malloc (sizeof (struct filelink))) )
    {
      ms_log (2, "addfile(): Cannot allocate memory\n");
      return -1;
    }

  if ( ! (newlp->filename = strdup(filename)) )
    {
      ms_log (2, "addfile(): Cannot duplicate filename string\n");
      return -1;
    }
  
  newlp->mstg = NULL;
  newlp->next = NULL;
  
  /* Add new file to the end of the list */
  if ( filelisttail == 0 )
    {
      filelist = newlp;
      filelisttail = newlp;
    }
  else
    {
      filelisttail->next = newlp;
      filelisttail = newlp;
    }
  
  return 0;
}  /* End of addfile() */


/***************************************************************************
 * addlistfile:
 *
 * Add files listed in the specified file to the global input file list.
 *
 * Returns count of files added on success and -1 on error.
 ***************************************************************************/
static int
addlistfile (char *filename) 
{
  FILE *fp;
  char filelistent[1024];
  int filecount = 0;
  
  if ( verbose >= 1 )
    ms_log (1, "Reading list file '%s'\n", filename);
  
  if ( ! (fp = fopen(filename, "rb")) )
    {
      ms_log (2, "Cannot open list file %s: %s\n", filename, strerror(errno));
      return -1;
    }
  
  while ( fgets (filelistent, sizeof(filelistent), fp) )
    {
      char *cp;
      
      /* End string at first newline character */
      if ( (cp = strchr(filelistent, '\n')) )
        *cp = '\0';
      
      /* Skip empty lines */
      if ( ! strlen (filelistent) )
        continue;
      
      /* Skip comment lines */
      if ( *filelistent == '#' )
        continue;
      
      if ( verbose > 1 )
        ms_log (1, "Adding '%s' from list file\n", filelistent);
      
      if ( addfile (filelistent) )
        return -1;
      
      filecount++;
    }
  
  fclose (fp);
  
  return filecount;
}  /* End of addlistfile() */


/***************************************************************************
 * usage():
 * Print the usage message.
 ***************************************************************************/
static void
usage (void)
{
  fprintf (stderr, "%s - Synchronize Mini-SEED to database schema version: %s\n\n", PACKAGE, VERSION);
  fprintf (stderr, "Usage: %s [options] file1 [file2] [file3] ...\n\n", PACKAGE);
  fprintf (stderr,
	   " ## General options ##\n"
	   " -V             Report program version\n"
	   " -h             Show this usage message\n"
	   " -v             Be more verbose, multiple flags can be used\n"
	   " -ns            No sync, perform data parsing but do not connect to database\n"
	   "\n"
           " -table  table  Use specified table name, currently: %s\n"
	   " -dbhost  host  Specify database host, currently: %s\n"
	   " -dbport  port  Specify database port, currently: %s\n"
	   " -dbuser  user  Specify database user name, currently: %s\n"
	   " -dbpass  pass  Specify database user password, currently: %s\n"
	   " -dbname  name  Specify database name or full connection info, currently: %s\n"
           " -TRACE         Enable libpq tracing facility and direct output to stderr\n"
	   "\n"
	   " -tt secs       Specify a time tolerance for continuous traces\n"
	   " -rt diff       Specify a sample rate tolerance for continuous traces\n"
	   "\n"
	   " files          File(s) of Mini-SEED records, list files prefixed with '@'\n"
	   "\n", dbtable, dbhost, dbport, dbuser, dbpass, dbname);
}  /* End of usage() */
