/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */

#include "config.h"

#include "scanner_config.h"
#include "feature_recorder_set.h"
#include "feature_recorder_file.h"
#include "feature_recorder_sql.h"

#include "dfxml/src/dfxml_writer.h"
#include "dfxml/src/hash_t.h"

/**
 * feature_recorder_set:
 * Manage the set of feature recorders.
 * Handles both file-based feature recorders and the SQLite3 feature recorder.
 */

const std::string feature_recorder_set::ALERT_RECORDER_NAME = "alerts";
//const std::string feature_recorder_set::DISABLED_RECORDER_NAME = "disabled";

/* be_hash. Currently this just returns the MD5 of the sbuf,
 * but eventually it will allow the use of different hashes.
 */
std::string feature_recorder_set::hash_def::md5_hasher(const uint8_t *buf,size_t bufsize)
{
    return dfxml::md5_generator::hash_buf(buf,bufsize).hexdigest();
}

std::string feature_recorder_set::hash_def::sha1_hasher(const uint8_t *buf,size_t bufsize)
{
    return dfxml::sha1_generator::hash_buf(buf,bufsize).hexdigest();
}

std::string feature_recorder_set::hash_def::sha256_hasher(const uint8_t *buf,size_t bufsize)
{
    return dfxml::sha256_generator::hash_buf(buf,bufsize).hexdigest();
}

feature_recorder_set::hash_func_t feature_recorder_set::hash_def::hash_func_for_name(const std::string &name)
{
    if(name=="md5" || name=="MD5"){
        return md5_hasher;
    }
    if(name=="sha1" || name=="SHA1" || name=="sha-1" || name=="SHA-1"){
        return sha1_hasher;
    }
    if(name=="sha256" || name=="SHA256" || name=="sha-256" || name=="SHA-256"){
        return sha256_hasher;
    }
    throw std::invalid_argument("invalid hasher name: " + name);
}



/**
 * Constructor.
 * Create an empty recorder with no outdir.
 */
feature_recorder_set::feature_recorder_set( const flags_t &flags_,
                                            const std::string &hash_algorithm,
                                            const std::string &input_fname_,
                                            const std::string &outdir_):
    input_fname(input_fname_),outdir(outdir_),flags(flags_),
    hasher( hash_def(hash_algorithm, hash_def::hash_func_for_name(hash_algorithm)))
{
    namespace fs = std::filesystem;
    if (outdir.size() == 0){
        throw std::invalid_argument("feature_recorder_set::feature_recorder_set(): output directory not provided");
    }

    /* Make sure we can write to the outdir if one is provided */
    if (outdir == scanner_config::NO_OUTDIR){
        flags.disabled = true;
    } else {
        /* Create the output directory if it doesn't exist */
        if (!fs::is_directory(outdir)) {
            fs::create_directory(outdir);
        }

        /* Make sure it is writable */
        if (!fs::is_directory(outdir)){
            throw std::runtime_error("Could not create directory " + outdir);
        }

        if (access(outdir.c_str(),W_OK)!=0) {
            throw std::invalid_argument("output directory " + outdir + " not writable");
        }
    }

#if 0
    /* Create a disabled feature recorder if necessary */
    if ( flags.disabled ){
        named_feature_recorder(DISABLED_RECORDER_NAME).flags.disabled=true;
    }
#endif

    //message_enabled_scanners(scanner_params::PHASE_INIT); // tell all enabled scanners to init

#if 0
#if defined(HAVE_SQLITE3_H) and defined(HAVE_LIBSQLITE3)
    if (flag_set(ENABLE_SQLITE3_RECORDERS)) {
        db_create();
    }
#endif
#endif

#if 0
    /* Create the requested feature files */
    for( auto it:feature_files){
        create_name(it, flags & CREATE_STOP_LIST_RECORDERS);
    }
#endif
}


/*
 * deallocator
 */
feature_recorder_set::~feature_recorder_set()
{
    frm.delete_all();
#if 0
#if defined(HAVE_SQLITE3_H) and defined(HAVE_LIBSQLITE3)
    db_close();
#endif
#endif
}



/**
 *
 * Initialize a properly functioning feature recorder set.
 * If disabled, create a disabled feature_recorder that can respond to functions as requested.
 */



/****************************************************************
 *** adding and removing feature recorders
 ****************************************************************/

void feature_recorder_set::create_alert_recorder()
{
    /* Create an alert recorder if necessary */
    if (!flags.no_alert) {
        create_feature_recorder(feature_recorder_set::ALERT_RECORDER_NAME); // make the alert recorder
    }
}


/**
 * Create a requested feature recorder. Do not create it if it already exists.
 */

feature_recorder &feature_recorder_set::create_feature_recorder(const feature_recorder_def def)
{
    if (flags.record_files and flags.record_sql){
        throw std::runtime_error("currently can only record to files or SQL, not both");
    }
    if (!flags.record_files and !flags.record_sql){
        throw std::runtime_error("Must record to either files or SQL");
    }
    auto it = frm.find(def.name);
    if ( it != frm.end() ) {
        throw FeatureRecorderAlreadyExists {std::string("feature recorder already exists: ")+def.name};
    }

    feature_recorder *fr = nullptr;
    if (flags.record_files) {
        fr = new feature_recorder_file(*this, def);
    }
    if (flags.record_sql) {
        fr = new feature_recorder_sql(*this, def);
    }
    frm.insert(def.name, fr);
    return *fr;                          // as a courtesy
}

/*
 * Get the named feature recorder.
 * If the feature recorder doesn't exist and create is true, create it.
 * Otherwise raise an exception.
 */
feature_recorder &feature_recorder_set::named_feature_recorder(const std::string name) const
{
    auto it = frm.find(name);
    if ( it == frm.end() ) {
        throw NoSuchFeatureRecorder {std::string("No such feature recorder: ")+name};
    }
    return *it->second;
}

/*
 * The alert recorder is special. It's provided for all of the feature recorders.
 * If one doesn't exist, create it.
 */
feature_recorder &feature_recorder_set::get_alert_recorder() const
{
    return named_feature_recorder(feature_recorder_set::ALERT_RECORDER_NAME);
}


// send every enabled scanner the phase message
void feature_recorder_set::feature_recorders_shutdown()
{
    for(auto const &it : frm){
        it.second->shutdown();
    }
}


/****************************************************************
 *** Data handling
 ****************************************************************/


/*
 * uses md5 to determine if a block was prevously seen.
 * Hopefully sbuf.buf() is zero-copy.
 */
bool feature_recorder_set::check_previously_processed(const sbuf_t &sbuf)
{
    std::string sha1 = dfxml::sha1_generator::hash_buf(sbuf.buf,sbuf.bufsize).hexdigest();
    return seen_set.check_for_presence_and_insert(sha1);
}

/****************************************************************
 *** Stats
 ****************************************************************/

void feature_recorder_set::dump_name_count_stats(dfxml_writer *writer) const
{
    if (writer) {
        writer->push("feature_files");
        for (auto ij: frm) {
            writer->set_oneline(true);
            writer->push("feature_file");
            writer->xmlout("name",ij.second->name);
            writer->xmlout("count",ij.second->features_written);
            writer->pop();
            writer->set_oneline(false);
        }
        writer->pop();
    }
}



/****************************************************************
 *** Histogram Support - Called during shutdown of scanner_set.
 ****************************************************************/

/**
 * We now have three kinds of histograms:
 * 1 - Traditional post-processing histograms specified by the histogram library
     1a - feature-file based traditional ones
     1b - SQL-based traditional ones.
 * 2 - In-memory histograms (used primarily by beapi)
 */


void feature_recorder_set::histogram_add(const histogram_def &def)
{
    named_feature_recorder(def.feature).histogram_add(def);
}

size_t feature_recorder_set::histogram_count() const
{
    /* Ask each feature recorder to count the number of histograms it can produce */
    size_t count = 0;
    for ( auto it: frm ){
        count += it.second->histogram_count();
    }
    return count;
}


/**
 * Have every feature recorder generate all of its histograms.
 */
void feature_recorder_set::histograms_generate()
{
    for (auto it : frm ){
        it.second->histogram_flush_all();
    }
}


std::vector<std::string> feature_recorder_set::feature_file_list() const
{
    std::vector<std::string> ret;
    for( auto it: frm ){
        ret.push_back(it.first);
    }
    return ret;
}


#if 0
//#if defined(HAVE_SQLITE3_H) and defined(HAVE_LIBSQLITE3)

/*** SQL Support ***/

/*
 * Time results with ubnist1 on R4:
 * no SQL - 79 seconds
 * no pragmas - 651 seconds
 * "PRAGMA synchronous =  OFF", - 146 second
 * "PRAGMA synchronous =  OFF", "PRAGMA journal_mode=MEMORY", - 79 seconds
 *
 * Time with domexusers:
 * no SQL -
 */


#define SQLITE_EXTENSION ".sqlite"
#ifndef SQLITE_DETERMINISTIC
#define SQLITE_DETERMINISTIC 0
#endif

static int debug  = 0;

static const char *schema_db[] = {
    "PRAGMA synchronous =  OFF",
    "PRAGMA journal_mode=MEMORY",
    //"PRAGMA temp_store=MEMORY",  // did not improve performance
    "PRAGMA cache_size = 200000",
    "CREATE TABLE IF NOT EXISTS db_info (schema_ver INTEGER, bulk_extractor_ver INTEGER)",
    "INSERT INTO  db_info (schema_ver, bulk_extractor_ver) VALUES (1,1)",
    "CREATE TABLE IF NOT EXISTS be_features (tablename VARCHAR,comment TEXT)",
    "CREATE TABLE IF NOT EXISTS be_config (name VARCHAR,value VARCHAR)",
    0};

/* Create a feature table and note that it has been created in be_features */
static const char *schema_tbl[] = {
    "CREATE TABLE IF NOT EXISTS f_%s (offset INTEGER(12), path VARCHAR, feature_eutf8 TEXT, feature_utf8 TEXT, context_eutf8 TEXT)",
    "CREATE INDEX IF NOT EXISTS f_%s_idx1 ON f_%s(offset)",
    "CREATE INDEX IF NOT EXISTS f_%s_idx2 ON f_%s(feature_eutf8)",
    "CREATE INDEX IF NOT EXISTS f_%s_idx3 ON f_%s(feature_utf8)",
    "INSERT INTO be_features (tablename,comment) VALUES ('f_%s','')",
    0};

static const char *begin_transaction[] = {"BEGIN TRANSACTION",0};
static const char *commit_transaction[] = {"COMMIT TRANSACTION",0};
void feature_recorder_set::db_send_sql(sqlite3 *db,const char **stmts, ...)
{
    assert(db!=0);
    for(int i=0;stmts[i];i++){
        char *errmsg = 0;
        char buf[65536];

        va_list ap;
        va_start(ap,stmts);
        vsnprintf(buf,sizeof(buf),stmts[i],ap);
        va_end(ap);
        if(debug) std::cerr << "SQL: " << buf << "\n";
        // Don't error on a PRAGMA
        if((sqlite3_exec(db,buf,NULL,NULL,&errmsg) != SQLITE_OK)  && (strncmp(buf,"PRAGMA",6)!=0)) {
            fprintf(stderr,"Error executing '%s' : %s\n",buf,errmsg);
            exit(1);
        }
    }
}

void feature_recorder_set::db_create_table(const std::string &name)
{
    std::cerr << "db_create_table\n";
    assert(name.size()>0);
    assert(db3!=NULL);
    db_send_sql(db3,schema_tbl,name.c_str(),name.c_str());
}

sqlite3 *feature_recorder_set::db_create_empty(const std::string &name)
{
    assert(name.size()>0);
    std::string dbfname  = outdir + "/" + name +  SQLITE_EXTENSION;
    if(debug) std::cerr << "create_feature_database " << dbfname << "\n";
    sqlite3 *db=0;
    if (sqlite3_open_v2(dbfname.c_str(), &db,
                        SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|SQLITE_OPEN_FULLMUTEX,
                        0)!=SQLITE_OK) {
        std::cerr << "Cannot create database '" << dbfname << "': " << sqlite3_errmsg(db) << "\n";
        sqlite3_close(db);
        exit(1);
    }
    return db;
}

void feature_recorder_set::db_create()
{
    assert(db3==0);
    db3 = db_create_empty("report");
    db_send_sql(db3,schema_db);
    std::cout << "in db_create called\n";
}

void feature_recorder_set::db_close()
{
    if(db3){
        if(debug) std::cerr << "db_close()\n";
        sqlite3_close(db3);
        db3 = 0;
    }
}

void feature_recorder_set::db_transaction_begin()
{
    const std::lock_guard<std::mutex> lock(Min_transaction);
    if(!in_transaction){
        db_send_sql(db3,begin_transaction);
        in_transaction = true;
    }
}

void feature_recorder_set::db_transaction_commit()
{
    const std::lock_guard<std::mutex> lock(Min_transaction);
    if(in_transaction){
        db_send_sql(db3,commit_transaction);
        in_transaction = false;
    } else {
        std::cerr << "No transaction to commit\n";
    }
}

#endif
