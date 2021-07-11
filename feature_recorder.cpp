/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */

#include "config.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>
#include <unistd.h>

#include <ctime>
#include <cstdarg>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>

#include "feature_recorder.h"
#include "feature_recorder_set.h"
#include "formatter.h"
#include "unicode_escape.h"
#include "utils.h"
#include "word_and_context_list.h"

#if 0
static inline bool isodigit(char c) { return c >= '0' && c <= '7'; }

/* Feature recorder functions that don't have anything to do with files  or SQL databases */
static inline int hexval(char ch) {
    switch (ch) {
    case '0': return 0;
    case '1': return 1;
    case '2': return 2;
    case '3': return 3;
    case '4': return 4;
    case '5': return 5;
    case '6': return 6;
    case '7': return 7;
    case '8': return 8;
    case '9': return 9;
    case 'a':
    case 'A': return 10;
    case 'b':
    case 'B': return 11;
    case 'c':
    case 'C': return 12;
    case 'd':
    case 'D': return 13;
    case 'e':
    case 'E': return 14;
    case 'f':
    case 'F': return 15;
    }
    return 0;
}
#endif

feature_recorder::feature_recorder(class feature_recorder_set& fs_, const struct feature_recorder_def def_)
    : fs(fs_), name(def_.name), def(def_) {}

feature_recorder::~feature_recorder() {}

void feature_recorder::flush()    {}


#if 0
/**
 * Unquote Python or octal-style quoting of a string
 */
std::string feature_recorder::unquote_string(const std::string& s) {
    size_t len = s.size();
    if (len < 4) return s; // too small for a quote

    std::string out;
    for (size_t i = 0; i < len; i++) {
        /* Look for octal coding */
        if (i + 3 < len && s[i] == '\\' && isodigit(s[i + 1]) && isodigit(s[i + 2]) && isodigit(s[i + 3])) {
            uint8_t code = (s[i + 1] - '0') * 64 + (s[i + 2] - '0') * 8 + (s[i + 3] - '0');
            out.push_back(code);
            i += 3; // skip over the digits
            continue;
        }
        /* Look for hex coding */
        if (i + 3 < len && s[i] == '\\' && s[i + 1] == 'x' && isxdigit(s[i + 2]) && isxdigit(s[i + 3])) {
            uint8_t code = (hexval(s[i + 2]) * 16) | hexval(s[i + 3]);
            out.push_back(code);
            i += 3; // skip over the digits
            continue;
        }
        out.push_back(s[i]);
    }
    return out;
}
#endif

/**
 * Get the feature which is defined as being between a \t and [\t\n]
 */

#if 0
/*static*/ std::string feature_recorder::extract_feature(const std::string& line) {
    size_t tab1 = line.find('\t');
    if (tab1 == std::string::npos) return ""; // no feature
    size_t feature_start = tab1 + 1;
    size_t tab2 = line.find('\t', feature_start);
    if (tab2 != std::string::npos) return line.substr(feature_start, tab2 - feature_start);
    return line.substr(feature_start); // no context to remove
}
#endif

const std::filesystem::path feature_recorder::get_outdir() const // cannot be inline becuase it accesses fs
{
    return fs.get_outdir();
}

/*
 * Returns a filename for this feature recorder with a specific suffix.
 */
const std::filesystem::path feature_recorder::fname_in_outdir(std::string suffix, int count) const {
    if (fs.get_outdir() == scanner_config::NO_OUTDIR) {
        throw std::runtime_error("fname_in_outdir called, but outdir==NO_OUTDIR");
    }

    std::filesystem::path base_path = fs.get_outdir() / this->name;
    if (suffix.size() > 0) { base_path = base_path.string() + "_" + suffix; }
    if (count == NO_COUNT) return base_path.string() + ".txt";
    if (count != NEXT_COUNT) { return base_path.string() + "_" + std::to_string(count) + ".txt"; }
    /* Probe for a file that we can create. When we create it, return the name.
     * Yes, this created a TOCTOU error. We should return the open file descriptor.
     */
    for (int i = 0; i < 1000000; i++) {
        std::filesystem::path fname = base_path.string() + (i > 0 ? "_" + std::to_string(i) : "") + ".txt";
        int fd = open(fname.c_str(), O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
        if (fd >= 0) {
            /* Created the file. close it. and return.*/
            close(fd);
            return fname;
        }
    }
    throw std::runtime_error("it is unlikely that there are a million files, so this is probably a logic error.");
}

/**
 * the main entry point of writing a feature and its context to the feature file.
 * processes the stop list
 */

void feature_recorder::quote_if_necessary(std::string& feature, std::string& context) const {
    /* By default quote string that is not UTF-8, and quote backslashes. */
    bool escape_bad_utf8 = true;
    bool escape_backslash = true;

    if (def.flags.no_quote) { // don't quote either
        escape_bad_utf8 = false;
        escape_backslash = false;
    }

    if (def.flags.xml) { // only quote bad utf8
        escape_bad_utf8 = true;
        escape_backslash = false;
    }

    feature = validateOrEscapeUTF8(feature, escape_bad_utf8, escape_backslash, validateOrEscapeUTF8_validate);

    if (feature.size() > def.max_feature_size) { feature.resize(def.max_feature_size); }

    if (def.flags.no_context == false) {
        context = validateOrEscapeUTF8(context, escape_bad_utf8, escape_backslash, validateOrEscapeUTF8_validate);
        if (context.size() > def.max_context_size) { context.resize(def.max_context_size); }
    }
}

/*
 * write0:
 */
void feature_recorder::write0(const std::string& str) {}

/*
 * Write: keep track of count of features written.
 */
void feature_recorder::write0(const pos0_t& pos0, const std::string& feature, const std::string& context) {
    features_written += 1;
}

/**
 * write() is the main entry point for writing a feature at a given position with context.
 * write() checks the stoplist and escapes non-UTF8 characters, then calls write0().
 */
void feature_recorder::write(const pos0_t& pos0, const std::string& feature_, const std::string& context_) {
    if (fs.flags.disabled) return; // disabled

    if (fs.flags.pedantic) {
        if (feature_.size() > def.max_feature_size) {
            throw std::runtime_error(std::string("feature_recorder::write : feature_.size()=") +
                                     std::to_string(feature_.size()));
        }
        if (context_.size() > def.max_context_size) {
            throw std::runtime_error(std::string("feature_recorder::write : context_.size()=") +
                                     std::to_string(context_.size()));
        }
    }

    /* TODO: This needs to be change to do all processing in utf32 and not utf8 */

    std::string feature = feature_;
    std::string context = def.flags.no_context ? "" : context_;
    std::string feature_utf8 = make_utf8(feature);

    quote_if_necessary(feature, context);

    if (feature.size() == 0) {
        std::cerr << name << ": zero length feature at " << pos0 << "\n";
        if (fs.flags.pedantic) assert(0);
        return;
    }
    if (fs.flags.pedantic) {
        /* Check for tabs or newlines in feature and and context */
        for (size_t i = 0; i < feature.size(); i++) {
            if (feature[i] == '\t') throw std::runtime_error("feature contains \\t");
            if (feature[i] == '\n') throw std::runtime_error("feature contains \\n");
            if (feature[i] == '\r') throw std::runtime_error("feature contains \\r");
        }
        for (size_t i = 0; i < context.size(); i++) {
            if (context[i] == '\t') throw std::runtime_error("context contains \\t");
            if (context[i] == '\n') throw std::runtime_error("context contains \\n");
            if (context[i] == '\r') throw std::runtime_error("context contains \\r");
        }
    }

    /* First check to see if the feature is on the stop list.
     * Only do this if we have a stop_list_recorder (the stop list recorder itself
     * does not have a stop list recorder. If it did we would infinitely recurse.
     */
    if (def.flags.no_stoplist == false && fs.stop_list && fs.stop_list_recorder &&
        fs.stop_list->check_feature_context(feature_utf8, context)) {
        fs.stop_list_recorder->write(pos0, feature, context);
        return;
    }

    /* The alert list is a special features that are called out.
     * If we have one of those, write it to the redlist.
     */
#if 0
    if (flags.no_alertlist==false
        && fs.alert_list
        && fs.alert_list->check_feature_context(*feature_utf8,context)) {
        std::filesystem::path alert_fn = fs.get_outdir() + "/ALERTS_found.txt";
        const std::lock_guard<std::mutex> lock(Mr);                // notice we are locking the alert list
        std::ofstream rf(alert_fn.c_str(),std::ios_base::app);
        if(rf.is_open()){
            rf << pos0.shift(fs.offset_add).str() << '\t' << feature << '\t' << "\n";
        }
    }
#endif

    /* add the feature to any histograms; the regex is applied in the histogram */
    this->histograms_add_feature(feature);

    /* Finally write out the feature and the context */
    this->write0(pos0, feature, context);
}

/**
 * Given a buffer, an offset into that buffer of the feature, and the length
 * of the feature, make the context and write it out. This is mostly used
 * for writing from within the lexical analyzers.
 */

/* for debugging, halt at this position */
const pos0_t debug_halt_pos0("", 9999999);
const size_t debug_halt_pos(9999999);
void feature_recorder::write_buf(const sbuf_t& sbuf, size_t pos, size_t len) {
    if (fs.flags.debug) {
        std::cerr << "*** write_buf " << name << " sbuf=" << sbuf << " pos=" << pos << " len=" << len << "\n";
        /*
         * for debugging, you can set a debug at Breakpoint Reached below.
         * This lets you fast-forward to the error condition without having to set a watch point.
         */
        if (sbuf.pos0 == debug_halt_pos0 || pos == debug_halt_pos) { std::cerr << "Breakpoint Reached.\n"; }
    }

    /* If we are in the margin, ignore; it will be processed again */
    if (pos >= sbuf.pagesize && pos < sbuf.bufsize) { return; }

    if (pos >= sbuf.bufsize) { /* Sanity checks */
        throw std::runtime_error(std::string("*** write_buf: WRITE OUTSIDE BUFFER. pos=") + std::to_string(pos) +
                                 " sbuf=");
    }

    /* Asked to write beyond bufsize; bring it in */
    if (pos + len > sbuf.bufsize) { len = sbuf.bufsize - pos; }

    std::string feature = sbuf.substr(pos, len);
    std::string context;

    if (def.flags.no_context == false) {
        /* Context write; create a clean context */
        size_t p0 = context_window < pos ? pos - context_window : 0;
        size_t p1 = pos + len + context_window;

        if (p1 > sbuf.bufsize) p1 = sbuf.bufsize;
        assert(p0 <= p1);
        context = sbuf.substr(p0, p1 - p0);
    }
    this->write(sbuf.pos0 + pos, feature, context);
}

/**
 * replace a character in a string with another
 */
std::string replace(const std::string& src, char f, char t) {
    std::string ret;
    for (size_t i = 0; i < src.size(); i++) {
        if (src[i] == f)
            ret.push_back(t);
        else
            ret.push_back(src[i]);
    }
    return ret;
}

/****************************************************************
 *** carving support
 ****************************************************************
 *
 * carving support.
 * 2021-07-10 - we no longer carve to forensic path or actual path. Now we carve sequential numbers.
 * 2014-04-24 - $ is no longer valid either
 * 2013-08-29 - replace invalid characters in filenames
 * 2013-07-30 - automatically bin directories
 * 2013-06-08 - filenames are the forensic path.
 */

#if 0
std::string valid_dosname(std::string in) {
    std::string out;
    for (size_t i = 0; i < in.size(); i++) {
        uint8_t ch = in.at(i);
        if (ch <= 32 || ch >= 128 || ch == '"' || ch == '*' || ch == '+' || ch == ',' || ch == '/' || ch == ':' ||
            ch == ';' || ch == '<' || ch == '=' || ch == '>' || ch == '?' || ch == '\\' || ch == '[' || ch == ']' ||
            ch == '|' || ch == '$') {
            out.push_back('_');
        } else {
            out.push_back(ch);
        }
    }
    return out;
}
#endif

/*
 * write buffer to specified dirname/filename for writing data.
 * The name is {outdir}/{scanner}/{seq}/{pos0}.{ext}
 * Where: {outdir} is the output directory of the feature recorder.
 *        {scanner} is the name of the scanner
 *        {seq} is 000 through 999.  (1000 files per directory)
 *        {pos0} is where the feature was found.
 *        {ext} is the provided extension.

 * @param sbuf   - the buffer to carve
 * @param pos    - offset in the buffer to carve
 * @param len    - how many bytes to carve
 * @return - the path relative to the outdir
 */
/* Carving to a file depends on the carving mode.
 *
 * The purpose of CARVE_ENCODED is to allow us to carve JPEGs when
 * they are embedded in, say, GZIP files, but not carve JPEGs that
 * are bare.  The difficulty arises when you have a tool that can
 * go into, say, ZIP files. In this case, we don't want to carve
 * every ZIP file, just the (for example) XORed ZIP files. So the
 * ZIP carver doesn't carve every ZIP file, just the ZIP files
 * that are in HIBER files.  That is, we want to not carve a path
 * of ZIP-234234 but we do want to carve a path of
 * 1000-HIBER-33423-ZIP-2343.  This is implemented by having an
 * do_not_carve_encoding. the ZIP carver sets it to ZIP so it
 * won't carve things that are just found in a ZIP file. This
 * means that it won't carve disembodied ZIP files found in
 * unallocated space. You might want to do that.  If so, set ZIP's
 * carve mode to CARVE_ALL.
 */

#include <iomanip>
std::string feature_recorder::carve(const sbuf_t& header, const sbuf_t& data, std::string ext, time_t mtime) {
    switch (carve_mode) {
    case feature_recorder_def::CARVE_NONE: return NO_CARVED_FILE; // carve nothing
    case feature_recorder_def::CARVE_ENCODED:
        if (data.pos0.path.size() == 0) return NO_CARVED_FILE; // not encoded
        if (data.pos0.alphaPart() == do_not_carve_encoding)
            return std::string(); // ignore if it is just encoded with this
        break;                    // otherwise carve
    case feature_recorder_def::CARVE_ALL: break;
    }

    /* See if we have previously carved this object, in which case do not carve it again */
    std::string carved_hash_hexvalue = hash(data);
    bool in_cache = carve_cache.check_for_presence_and_insert(carved_hash_hexvalue);
    std::string carved_relative_path; // carved path reported in feature file, relative to outdir
    std::filesystem::path carved_absolute_path;
    // std::string fname_feature;          // what goes into the feature file
    if (in_cache) {
        carved_relative_path = "<CACHED>";
    } else {
        /* Determine the directory and filename */
        int64_t myfileNumber = carved_file_count++; // atomic operation
        std::ostringstream seq;
        seq << std::setw(3) << std::setfill('0') << int(myfileNumber / 1000);
        const std::string thousands{seq.str()};

        std::ostringstream num;
        num << std::setw(8) << std::setfill('0') << int(myfileNumber);
        const std::string units{num.str()};

        /* Create the directory.
         * If it exists, the call fails.
         * (That's faster than checking to see if it exists and then creating the directory)
         */
        std::filesystem::create_directory(fs.get_outdir() / name);
        std::filesystem::create_directory(fs.get_outdir() / name / thousands);

        // const std::filesystem::path scannerDir { fs.get_outdir() / name};
        carved_relative_path = name + "/" + thousands + "/" + units + ext;
        carved_absolute_path = fs.get_outdir() / name / thousands / (units + ext);

        // fname  = seqDir / (sbuf.pos0.str() + std::string(".") + ext);
        // fname_feature = fname.string().substr(fs.get_outdir().string().size()+1);
    }

    // write to the feature file
    std::stringstream xml;
    xml.str(std::string()); // clear the stringstream
    xml << "<fileobject>";
    if (!in_cache) xml << "<filename>" << carved_relative_path << "</filename>";
    xml << "<filesize>" << header.bufsize + data.bufsize << "</filesize>";
    xml << "<hashdigest type='" << fs.hasher.name << "'>" << carved_hash_hexvalue << "</hashdigest>";
    xml << "</fileobject>";
    this->write(data.pos0, carved_relative_path, xml.str());

    if (!in_cache) {
        /* Write the data */
        std::ofstream os;
        os.open(carved_absolute_path, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!os.is_open()) {
            perror(carved_absolute_path.c_str());
            throw std::runtime_error(Formatter() << "cannot open file for writing:" << carved_absolute_path);
        }
        header.write(os);
        data.write(os);
        os.close();
        if (os.bad()) { throw std::runtime_error(Formatter() << "error writing file " << carved_absolute_path); }

        /* Set timestamp if necessary. Note that we do not use std::filesystem::last_write_time()
         * as there seems to be no portable way to use it.
         */
        if (mtime > 0) {
            const struct timeval times[2] = {{mtime, 0}, {mtime, 0}};
            utimes(carved_absolute_path.c_str(), times);
        }
    }
    return carved_relative_path;
}

const std::string feature_recorder::hash(const sbuf_t& sbuf) const {
    return sbuf.hash(fs.hasher.func);
}

void feature_recorder::shutdown() {}


/****************************************************************
 ** Histogram Support
 ****************************************************************/

/**
 * add a new histogram to this feature recorder.
 * @param def - the histogram definition
 */
void feature_recorder::histogram_add(const struct histogram_def& hdef) {
    if (features_written != 0) {
        throw std::runtime_error("Cannot add histograms after features have been written.");
    }
    histograms.push_back(std::make_unique<AtomicUnicodeHistogram>(hdef));
}

/**
 * add a feature to all of the feature recorder's histograms
 * @param feature - the feature to add.
 */
void feature_recorder::histograms_add_feature(const std::string& feature) {
    for (auto& h : histograms) {
        h->add(feature); // add the original feature
    }
}

/**
 * flush the largest histogram to the disk. This is a way to release
 * allocated memory.
 *
 * In BE2.0, the file recorder's histograms are built in memory. If
 * they are too big for memory, file-histograms they are flushed and
 * need to be recombined in post-processing. SQL feature recorder uses the
 * SQLite3 to create the histograms.
 */

void feature_recorder::histogram_flush(AtomicUnicodeHistogram& h) {
    std::cerr << "feature_recorder::histogram_flush should not be called yet.\n";
}

bool feature_recorder::histogram_flush_largest() {
    /* TODO - implement */
    return false;
}

void feature_recorder::histogram_flush_all() {
    for (auto& h : histograms) { this->histogram_flush(*h); }
}

#if 0
/*
 * histogram_merge:
 * If possible, read all histogram files into memory and write them
 * out as a single histogram file. If we run out of memory during the process, fail.
 *
 */
void feature_recorder::histogram_merge(const struct histogram_def &def)
{
    throw std::runtime_error("Implement histogram_merge");
}

void feature_recorder::histogram_merge_all()
{
    throw std::runtime_error("Implement histogram_merge_all");
}
#endif
